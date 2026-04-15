/**
 * @file test_SpaceManager.cpp
 * @author Nexepic
 * @date 2025/12/4
 *
 * @brief Tests for SpaceManager coordination layer.
 *
 * SpaceManager orchestrates SegmentAllocator, SegmentCompactor, and
 * FileTruncator. These tests verify the coordination logic, not the
 * individual component logic (tested in their own files).
 *
 * @copyright Copyright (c) 2025 Nexepic
 * @license Apache-2.0
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <thread>

#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/EntityReferenceUpdater.hpp"
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/FileTruncator.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentAllocator.hpp"
#include "graph/storage/SegmentCompactor.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/SpaceManager.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/data/DataManager.hpp"

using namespace graph::storage;
using namespace graph;

class SpaceManagerTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	FileHeader header;

	std::shared_ptr<SegmentTracker> segmentTracker;
	std::shared_ptr<FileHeaderManager> fileHeaderManager;
	std::shared_ptr<IDAllocator> idAllocator;
	std::shared_ptr<SegmentAllocator> segmentAllocator;
	std::shared_ptr<SegmentCompactor> segmentCompactor;
	std::shared_ptr<FileTruncator> fileTruncator;
	std::shared_ptr<SpaceManager> spaceManager;

	std::shared_ptr<DataManager> dataManager;
	std::shared_ptr<EntityReferenceUpdater> refUpdater;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("spacemgr_test_" + to_string(uuid) + ".dat");

		file = std::make_shared<std::fstream>(testFilePath,
			std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());

		file->write(reinterpret_cast<char *>(&header), sizeof(FileHeader));
		file->flush();

		segmentTracker = std::make_shared<SegmentTracker>(file, header);
		fileHeaderManager = std::make_shared<FileHeaderManager>(file, header);
		idAllocator = std::make_shared<IDAllocator>(
			file, segmentTracker, fileHeaderManager->getMaxNodeIdRef(), fileHeaderManager->getMaxEdgeIdRef(),
			fileHeaderManager->getMaxPropIdRef(), fileHeaderManager->getMaxBlobIdRef(),
			fileHeaderManager->getMaxIndexIdRef(), fileHeaderManager->getMaxStateIdRef());

		segmentAllocator = std::make_shared<SegmentAllocator>(file, segmentTracker, fileHeaderManager, idAllocator);
		segmentCompactor = std::make_shared<SegmentCompactor>(file, segmentTracker, segmentAllocator, fileHeaderManager);
		fileTruncator = std::make_shared<FileTruncator>(file, testFilePath.string(), segmentTracker);
		spaceManager = std::make_shared<SpaceManager>(segmentAllocator, segmentCompactor, fileTruncator, segmentTracker);

		dataManager = std::make_shared<DataManager>(file, 100, header, idAllocator, segmentTracker);
		refUpdater = std::make_shared<EntityReferenceUpdater>(dataManager);
		segmentCompactor->setEntityReferenceUpdater(refUpdater);
	}

	void TearDown() override {
		spaceManager.reset();
		segmentCompactor.reset();
		fileTruncator.reset();
		segmentAllocator.reset();
		refUpdater.reset();
		dataManager.reset();
		idAllocator.reset();
		fileHeaderManager.reset();
		segmentTracker.reset();
		if (file && file->is_open())
			file->close();
		file.reset();
		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	void createActiveNode(uint64_t offset, uint32_t index, int64_t id) const {
		Node n(id, 100);
		segmentTracker->writeEntity(offset, index, n, Node::getTotalSize());
		segmentTracker->setEntityActive(offset, index, true);
	}
};

// =========================================================================
// Accessor Tests
// =========================================================================

TEST_F(SpaceManagerTest, Accessors_ReturnCorrectComponents) {
	EXPECT_EQ(spaceManager->getAllocator(), segmentAllocator);
	EXPECT_EQ(spaceManager->getCompactor(), segmentCompactor);
	EXPECT_EQ(spaceManager->getTruncator(), fileTruncator);
	EXPECT_EQ(spaceManager->getTracker(), segmentTracker);
}

// =========================================================================
// Fragmentation Ratio Tests
// =========================================================================

TEST_F(SpaceManagerTest, GetTotalFragmentationRatio_EmptyDatabase) {
	EXPECT_DOUBLE_EQ(spaceManager->getTotalFragmentationRatio(), 0.0);
}

TEST_F(SpaceManagerTest, GetTotalFragmentationRatio_SingleType) {
	auto type = Node::typeId;

	// Segment A: 100% fragmented
	uint64_t segA = segmentAllocator->allocateSegment(type, 10);
	segmentTracker->updateSegmentUsage(segA, 10, 10);

	// Segment B: 0% fragmented
	uint64_t segB = segmentAllocator->allocateSegment(type, 10);
	segmentTracker->updateSegmentUsage(segB, 10, 0);

	// Weighted average: (1.0 * 1 + 0.0 * 1) / 2 = 0.5
	EXPECT_DOUBLE_EQ(spaceManager->getTotalFragmentationRatio(), 0.5);
}

TEST_F(SpaceManagerTest, GetTotalFragmentationRatio_MultipleTypes) {
	// Node: 1 segment, 100% fragmented
	uint64_t nodeSeg = segmentAllocator->allocateSegment(Node::typeId, 10);
	segmentTracker->updateSegmentUsage(nodeSeg, 10, 10);

	// Edge: 1 segment, 0% fragmented
	uint64_t edgeSeg = segmentAllocator->allocateSegment(Edge::typeId, 10);
	segmentTracker->updateSegmentUsage(edgeSeg, 10, 0);

	// Weighted: Node contributes 1.0 (weight 1), Edge contributes 0.0 (weight 1)
	// Total = (1.0 + 0.0) / 2 = 0.5
	double ratio = spaceManager->getTotalFragmentationRatio();
	EXPECT_DOUBLE_EQ(ratio, 0.5);
}

// =========================================================================
// ShouldCompact Tests
// =========================================================================

TEST_F(SpaceManagerTest, ShouldCompact_NoSegments_ReturnsFalse) {
	EXPECT_FALSE(spaceManager->shouldCompact());
}

TEST_F(SpaceManagerTest, ShouldCompact_LowFragmentation_ReturnsFalse) {
	uint64_t seg = segmentAllocator->allocateSegment(Node::typeId, 10);
	segmentTracker->updateSegmentUsage(seg, 10, 2); // 20% < 30% threshold
	EXPECT_FALSE(spaceManager->shouldCompact());
}

TEST_F(SpaceManagerTest, ShouldCompact_HighFragmentation_ReturnsTrue) {
	uint64_t seg = segmentAllocator->allocateSegment(Node::typeId, 10);
	segmentTracker->updateSegmentUsage(seg, 10, 5); // 50% > 30% threshold
	EXPECT_TRUE(spaceManager->shouldCompact());
}

// =========================================================================
// SafeCompactSegments Tests
// =========================================================================

TEST_F(SpaceManagerTest, SafeCompactSegments_RunsSuccessfully) {
	// Empty database — just verify it runs without error
	EXPECT_TRUE(spaceManager->safeCompactSegments());
}

TEST_F(SpaceManagerTest, SafeCompactSegments_WithFragmentedData) {
	auto type = Node::typeId;

	// Create a segment with some fragmentation
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);
	for (uint32_t i = 0; i < 5; ++i) {
		createActiveNode(seg, i, static_cast<int64_t>(i + 1));
	}
	segmentTracker->updateSegmentUsage(seg, 5, 0);

	// Mark some as inactive to create fragmentation
	segmentTracker->setEntityActive(seg, 1, false);
	segmentTracker->setEntityActive(seg, 3, false);
	segmentTracker->updateSegmentUsage(seg, 5, 2);

	EXPECT_TRUE(spaceManager->safeCompactSegments());
}

TEST_F(SpaceManagerTest, SafeCompactSegments_SingleThreaded) {
	// Verify two sequential calls both succeed (no stuck lock)
	EXPECT_TRUE(spaceManager->safeCompactSegments());
	EXPECT_TRUE(spaceManager->safeCompactSegments());
}

// =========================================================================
// CompactSegments Orchestration Tests
// =========================================================================

TEST_F(SpaceManagerTest, CompactSegments_OrchestratesAllPhases) {
	auto type = Node::typeId;

	// Phase setup: create segments with varying fragmentation
	uint64_t seg1 = segmentAllocator->allocateSegment(type, 10);
	segmentTracker->updateSegmentUsage(seg1, 10, 10); // 100% fragmented (empty)

	uint64_t seg2 = segmentAllocator->allocateSegment(type, 10);
	for (uint32_t i = 0; i < 3; ++i) {
		createActiveNode(seg2, i, static_cast<int64_t>(i + 1));
	}
	segmentTracker->updateSegmentUsage(seg2, 3, 0);

	// Run full orchestration
	spaceManager->compactSegments();

	// Verify: active data should survive compaction (may be relocated)
	auto segments = segmentTracker->getSegmentsByType(type);
	ASSERT_EQ(segments.size(), 1u) << "One segment with active data should survive";
	EXPECT_GE(segments[0].used, 3u) << "Active entities should be preserved";
}

TEST_F(SpaceManagerTest, CompactSegments_EmptyDatabase_NoOp) {
	// No segments allocated — verify compactSegments runs without crash
	spaceManager->compactSegments();
	EXPECT_DOUBLE_EQ(spaceManager->getTotalFragmentationRatio(), 0.0);
}

// =========================================================================
// Mutex Accessor Test
// =========================================================================

TEST_F(SpaceManagerTest, GetMutex_IsLockable) {
	auto &mtx = spaceManager->getMutex();
	mtx.lock();
	mtx.unlock();
	// Just verify it doesn't deadlock
	SUCCEED();
}
