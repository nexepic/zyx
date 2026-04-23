/**
 * @file test_SegmentCompactor_MergeEdgeCases.cpp
 * @brief Branch coverage tests for SegmentCompactor merge, relocate, and
 *        max-ID recalculation paths not covered by the main test file.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>

#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentAllocator.hpp"
#include "graph/storage/SegmentCompactor.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

using namespace graph::storage;
using namespace graph;

class SegmentCompactorMergeTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	FileHeader header;
	std::shared_ptr<StorageIO> storageIO;
	std::shared_ptr<SegmentTracker> segmentTracker;
	std::shared_ptr<FileHeaderManager> fileHeaderManager;
	IDAllocators allocators;
	std::shared_ptr<SegmentAllocator> segmentAllocator;
	std::shared_ptr<SegmentCompactor> compactor;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() /
			("compact_merge_test_" + to_string(uuid) + ".dat");

		file = std::make_shared<std::fstream>(testFilePath,
			std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());

		file->write(reinterpret_cast<char *>(&header), sizeof(FileHeader));

		// Pre-allocate space for many segments
		std::vector<char> zeros(TOTAL_SEGMENT_SIZE, 0);
		for (int i = 0; i < 20; ++i)
			file->write(zeros.data(), TOTAL_SEGMENT_SIZE);
		file->flush();

		storageIO = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		segmentTracker = std::make_shared<SegmentTracker>(storageIO, header);
		fileHeaderManager = std::make_shared<FileHeaderManager>(file, header);
		std::pair<EntityType, int64_t *> typeMaxPairs[] = {
			{EntityType::Node, &fileHeaderManager->getMaxNodeIdRef()},
			{EntityType::Edge, &fileHeaderManager->getMaxEdgeIdRef()},
			{EntityType::Property, &fileHeaderManager->getMaxPropIdRef()},
			{EntityType::Blob, &fileHeaderManager->getMaxBlobIdRef()},
			{EntityType::Index, &fileHeaderManager->getMaxIndexIdRef()},
			{EntityType::State, &fileHeaderManager->getMaxStateIdRef()},
		};
		for (auto &[type, maxIdPtr] : typeMaxPairs) {
			allocators[static_cast<size_t>(type)] =
				std::make_shared<IDAllocator>(type, segmentTracker, *maxIdPtr);
		}
		segmentAllocator = std::make_shared<SegmentAllocator>(storageIO, segmentTracker, fileHeaderManager);
		compactor = std::make_shared<SegmentCompactor>(storageIO, segmentTracker, segmentAllocator, fileHeaderManager);
	}

	void TearDown() override {
		compactor.reset();
		segmentAllocator.reset();
		for (auto &a : allocators) a.reset();
		fileHeaderManager.reset();
		segmentTracker.reset();
		if (file && file->is_open()) file->close();
		file.reset();
		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	uint64_t allocateNodeSegment(uint32_t capacity = NODES_PER_SEGMENT) {
		return segmentAllocator->allocateSegment(Node::typeId, capacity);
	}

	void writeNodeToSegment(uint64_t segOffset, uint32_t slot, int64_t nodeId) {
		Node node(nodeId, 100);
		auto buf = utils::FixedSizeSerializer::serializeToBuffer(node, Node::getTotalSize());
		uint64_t offset = segOffset + sizeof(SegmentHeader) + slot * Node::getTotalSize();
		storageIO->writeAt(offset, buf.data(), buf.size());
		segmentTracker->setEntityActive(segOffset, slot, true);
	}
};

TEST_F(SegmentCompactorMergeTest, MergeSegments_LessThanTwoCandidates) {
	uint64_t seg = allocateNodeSegment();
	writeNodeToSegment(seg, 0, 1);
	segmentTracker->updateSegmentUsage(seg, 1, 0);

	bool result = compactor->mergeSegments(Node::typeId, 0.9);
	EXPECT_FALSE(result);
}

TEST_F(SegmentCompactorMergeTest, MergeIntoSegment_TypeMismatch) {
	uint64_t seg1 = allocateNodeSegment();
	uint64_t seg2 = segmentAllocator->allocateSegment(Edge::typeId, EDGES_PER_SEGMENT);

	bool result = compactor->mergeIntoSegment(seg1, seg2, Node::typeId);
	EXPECT_FALSE(result);
}

TEST_F(SegmentCompactorMergeTest, MergeIntoSegment_CapacityExceeded) {
	uint64_t seg1 = allocateNodeSegment();
	uint64_t seg2 = allocateNodeSegment();

	auto &h1 = segmentTracker->getSegmentHeader(seg1);
	// Fill both segments near capacity
	segmentTracker->updateSegmentUsage(seg1, h1.capacity, 0);
	for (uint32_t i = 0; i < h1.capacity; ++i)
		segmentTracker->setEntityActive(seg1, i, true);

	segmentTracker->updateSegmentUsage(seg2, 10, 0);
	for (uint32_t i = 0; i < 10; ++i)
		segmentTracker->setEntityActive(seg2, i, true);

	bool result = compactor->mergeIntoSegment(seg1, seg2, Node::typeId);
	EXPECT_FALSE(result);
}

TEST_F(SegmentCompactorMergeTest, CompactSegments_MoreThanFiveSegments) {
	// Create more than 5 segments needing compaction (covers resize to 5)
	std::vector<uint64_t> offsets;
	for (int i = 0; i < 7; ++i) {
		uint64_t seg = allocateNodeSegment();
		offsets.push_back(seg);
		segmentTracker->updateSegmentUsage(seg, 10, 8);
		for (uint32_t j = 0; j < 10; ++j)
			segmentTracker->setEntityActive(seg, j, j < 2);
	}

	EXPECT_NO_THROW(compactor->compactSegments(Node::typeId, 0.3));
}

TEST_F(SegmentCompactorMergeTest, CompactSegmentImpl_AllInactive) {
	uint64_t seg = allocateNodeSegment();
	writeNodeToSegment(seg, 0, 1);
	writeNodeToSegment(seg, 1, 2);
	segmentTracker->updateSegmentUsage(seg, 2, 2);
	segmentTracker->setEntityActive(seg, 0, false);
	segmentTracker->setEntityActive(seg, 1, false);

	// Calling compactSegments exercises the compactSegmentImpl<Node> path
	// where used == inactive_count (all-inactive segment)
	EXPECT_NO_THROW(compactor->compactSegments(Node::typeId, 0.1));
}

TEST_F(SegmentCompactorMergeTest, CompactSegmentImpl_LowFragmentation) {
	uint64_t seg = allocateNodeSegment();
	for (uint32_t i = 0; i < 10; ++i) {
		writeNodeToSegment(seg, i, static_cast<int64_t>(i + 1));
	}
	segmentTracker->updateSegmentUsage(seg, 10, 0);
	segmentTracker->markForCompaction(seg, true);

	compactor->compactSegments(Node::typeId, 0.3);

	auto &h = segmentTracker->getSegmentHeader(seg);
	EXPECT_EQ(h.used, 10u);
	EXPECT_EQ(h.inactive_count, 0u);
}

TEST_F(SegmentCompactorMergeTest, ProcessEmptySegments_NoEmpty) {
	uint64_t seg = allocateNodeSegment();
	writeNodeToSegment(seg, 0, 1);
	segmentTracker->updateSegmentUsage(seg, 1, 0);

	bool result = compactor->processEmptySegments(Node::typeId);
	EXPECT_FALSE(result);
}

TEST_F(SegmentCompactorMergeTest, ProcessAllEmptySegments) {
	uint64_t nodeSeg = allocateNodeSegment();
	segmentTracker->updateSegmentUsage(nodeSeg, 0, 0);

	compactor->processAllEmptySegments();

	auto freeSegs = segmentTracker->getFreeSegments();
	bool found = std::find(freeSegs.begin(), freeSegs.end(), nodeSeg) != freeSegs.end();
	EXPECT_TRUE(found);
}

TEST_F(SegmentCompactorMergeTest, MoveSegment_SameOffset) {
	uint64_t seg = allocateNodeSegment();
	bool result = compactor->moveSegment(seg, seg);
	EXPECT_TRUE(result);
}

TEST_F(SegmentCompactorMergeTest, RelocateSegmentsFromEnd_NoFreeSegments) {
	uint64_t seg = allocateNodeSegment();
	writeNodeToSegment(seg, 0, 1);
	segmentTracker->updateSegmentUsage(seg, 1, 0);

	bool result = compactor->relocateSegmentsFromEnd();
	EXPECT_FALSE(result);
}

TEST_F(SegmentCompactorMergeTest, FindFreeSegmentNotAtEnd_EmptyFreeList) {
	uint64_t result = compactor->findFreeSegmentNotAtEnd();
	EXPECT_EQ(result, 0u);
}

TEST_F(SegmentCompactorMergeTest, FindFreeSegmentNotAtEnd_AllAtEnd) {
	uint64_t seg1 = allocateNodeSegment();
	segmentTracker->markSegmentFree(seg1);

	uint64_t result = compactor->findFreeSegmentNotAtEnd();
	EXPECT_EQ(result, 0u);
}

TEST_F(SegmentCompactorMergeTest, RecalculateMaxIds_WithActiveNodes) {
	uint64_t nodeSeg = allocateNodeSegment();
	// Start_id is already set by allocation. Write nodes to slot 0 and 1.
	writeNodeToSegment(nodeSeg, 0, 1);
	writeNodeToSegment(nodeSeg, 1, 2);
	segmentTracker->updateSegmentUsage(nodeSeg, 2, 0);

	compactor->recalculateMaxIds();

	// Max node ID should be based on start_id + highest active slot
	auto &h = segmentTracker->getSegmentHeader(nodeSeg);
	// calculateLastUsedIdInSegment returns start_id + lastActiveSlot
	EXPECT_GE(fileHeaderManager->getMaxNodeIdRef(), h.start_id + 1);
}

TEST_F(SegmentCompactorMergeTest, RecalculateMaxIds_AllInactiveSegment) {
	// Test recalculateMaxIds when a segment has all inactive entities
	uint64_t seg = allocateNodeSegment();
	writeNodeToSegment(seg, 0, 1);
	writeNodeToSegment(seg, 1, 2);
	writeNodeToSegment(seg, 2, 3);
	segmentTracker->updateSegmentUsage(seg, 3, 3);
	segmentTracker->setEntityActive(seg, 0, false);
	segmentTracker->setEntityActive(seg, 1, false);
	segmentTracker->setEntityActive(seg, 2, false);

	compactor->recalculateMaxIds();
	// All entities are inactive, so max node ID should remain 0
	EXPECT_EQ(fileHeaderManager->getMaxNodeIdRef(), 0);
}
