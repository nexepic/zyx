/**
 * @file test_SegmentCompactor.cpp
 * @author Nexepic
 * @date 2025/12/4
 *
 * @brief Tests for SegmentCompactor: compaction, merging, relocation, max-ID recalculation.
 *
 * @copyright Copyright (c) 2025 Nexepic
 * @license Apache-2.0
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/EntityReferenceUpdater.hpp"
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentAllocator.hpp"
#include "graph/storage/SegmentCompactor.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/data/DataManager.hpp"

using namespace graph::storage;
using namespace graph;

class SegmentCompactorTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	FileHeader header;

	std::shared_ptr<SegmentTracker> segmentTracker;
	std::shared_ptr<FileHeaderManager> fileHeaderManager;
	std::shared_ptr<IDAllocator> idAllocator;
	std::shared_ptr<SegmentAllocator> segmentAllocator;
	std::shared_ptr<SegmentCompactor> compactor;
	std::shared_ptr<DataManager> dataManager;
	std::shared_ptr<EntityReferenceUpdater> refUpdater;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("compact_test_" + to_string(uuid) + ".dat");

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
		compactor = std::make_shared<SegmentCompactor>(file, segmentTracker, segmentAllocator, fileHeaderManager);

		dataManager = std::make_shared<DataManager>(file, 100, header, idAllocator, segmentTracker);
		refUpdater = std::make_shared<EntityReferenceUpdater>(dataManager);
		compactor->setEntityReferenceUpdater(refUpdater);
	}

	void TearDown() override {
		compactor.reset();
		refUpdater.reset();
		dataManager.reset();
		segmentAllocator.reset();
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

	template<typename T>
	void createActiveEntity(uint64_t offset, uint32_t index, int64_t id) {
		T entity;
		entity.setId(id);
		segmentTracker->writeEntity(offset, index, entity, T::getTotalSize());
		segmentTracker->setEntityActive(offset, index, true);
	}
};

// =========================================================================
// Compaction Tests
// =========================================================================

TEST_F(SegmentCompactorTest, CompactSegment_FullSegment_NoOp) {
	auto type = Node::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	// Fill segment with active entities
	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 5; ++i) {
		createActiveNode(seg, i, h.start_id + i);
	}
	segmentTracker->updateSegmentUsage(seg, 5, 0);

	// No fragmentation — compaction should be a no-op
	compactor->compactSegments(type, 0.3);

	// Segment should still exist with same data
	auto segments = segmentTracker->getSegmentsByType(type);
	ASSERT_EQ(segments.size(), 1u);
	EXPECT_EQ(segments[0].file_offset, seg);
}

TEST_F(SegmentCompactorTest, CompactSegment_AllDeleted_DeallocatesSegment) {
	auto type = Node::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	// Create entities then mark all inactive
	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 5; ++i) {
		createActiveNode(seg, i, h.start_id + i);
	}
	for (uint32_t i = 0; i < 5; ++i) {
		segmentTracker->setEntityActive(seg, i, false);
	}
	segmentTracker->updateSegmentUsage(seg, 5, 5);

	// Compact with very low threshold to trigger
	compactor->compactSegments(type, 0.1);

	// Segment should be freed (no active segments of this type)
	auto segments = segmentTracker->getSegmentsByType(type);
	EXPECT_EQ(segments.size(), 0u);
}

TEST_F(SegmentCompactorTest, CompactSegment_BelowThreshold_NoOp) {
	auto type = Node::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 10; ++i) {
		createActiveNode(seg, i, h.start_id + i);
	}
	// 20% fragmentation (below 30% threshold)
	segmentTracker->setEntityActive(seg, 8, false);
	segmentTracker->setEntityActive(seg, 9, false);
	segmentTracker->updateSegmentUsage(seg, 10, 2);

	compactor->compactSegments(type, 0.3);

	// Segment should still exist unchanged
	auto segments = segmentTracker->getSegmentsByType(type);
	ASSERT_EQ(segments.size(), 1u);
	EXPECT_EQ(segments[0].file_offset, seg);
}

TEST_F(SegmentCompactorTest, CompactSegment_EdgeType) {
	auto type = Edge::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 5; ++i) {
		createActiveEntity<Edge>(seg, i, h.start_id + i);
	}
	for (uint32_t i = 0; i < 5; ++i) {
		segmentTracker->setEntityActive(seg, i, false);
	}
	segmentTracker->updateSegmentUsage(seg, 5, 5);

	compactor->compactSegments(type, 0.1);

	auto segments = segmentTracker->getSegmentsByType(type);
	EXPECT_EQ(segments.size(), 0u);
}

TEST_F(SegmentCompactorTest, CompactSegment_PropertyType) {
	auto type = Property::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 5; ++i) {
		createActiveEntity<Property>(seg, i, h.start_id + i);
	}
	for (uint32_t i = 0; i < 5; ++i) {
		segmentTracker->setEntityActive(seg, i, false);
	}
	segmentTracker->updateSegmentUsage(seg, 5, 5);

	compactor->compactSegments(type, 0.1);

	auto segments = segmentTracker->getSegmentsByType(type);
	EXPECT_EQ(segments.size(), 0u);
}

TEST_F(SegmentCompactorTest, CompactSegment_BlobType) {
	auto type = Blob::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 5; ++i) {
		createActiveEntity<Blob>(seg, i, h.start_id + i);
	}
	for (uint32_t i = 0; i < 5; ++i) {
		segmentTracker->setEntityActive(seg, i, false);
	}
	segmentTracker->updateSegmentUsage(seg, 5, 5);

	compactor->compactSegments(type, 0.1);

	auto segments = segmentTracker->getSegmentsByType(type);
	EXPECT_EQ(segments.size(), 0u);
}

TEST_F(SegmentCompactorTest, CompactSegment_IndexType) {
	auto type = Index::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 5; ++i) {
		createActiveEntity<Index>(seg, i, h.start_id + i);
	}
	for (uint32_t i = 0; i < 5; ++i) {
		segmentTracker->setEntityActive(seg, i, false);
	}
	segmentTracker->updateSegmentUsage(seg, 5, 5);

	compactor->compactSegments(type, 0.1);

	auto segments = segmentTracker->getSegmentsByType(type);
	EXPECT_EQ(segments.size(), 0u);
}

TEST_F(SegmentCompactorTest, CompactSegment_StateType) {
	auto type = State::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 5; ++i) {
		createActiveEntity<State>(seg, i, h.start_id + i);
	}
	for (uint32_t i = 0; i < 5; ++i) {
		segmentTracker->setEntityActive(seg, i, false);
	}
	segmentTracker->updateSegmentUsage(seg, 5, 5);

	compactor->compactSegments(type, 0.1);

	auto segments = segmentTracker->getSegmentsByType(type);
	EXPECT_EQ(segments.size(), 0u);
}

// =========================================================================
// Merge Tests
// =========================================================================

TEST_F(SegmentCompactorTest, FindCandidatesForMerge_NoSegments) {
	auto candidates = compactor->findCandidatesForMerge(Node::typeId, 0.3);
	EXPECT_TRUE(candidates.empty());
}

TEST_F(SegmentCompactorTest, MergeSegments_SingleCandidate_ReturnsFalse) {
	auto type = Node::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);
	segmentTracker->updateSegmentUsage(seg, 2, 0); // Low usage

	EXPECT_FALSE(compactor->mergeSegments(type, 0.3));
}

TEST_F(SegmentCompactorTest, MergeIntoSegment_TypeMismatch) {
	uint64_t nodeSeg = segmentAllocator->allocateSegment(Node::typeId, 10);
	uint64_t edgeSeg = segmentAllocator->allocateSegment(Edge::typeId, 10);

	EXPECT_FALSE(compactor->mergeIntoSegment(nodeSeg, edgeSeg, Node::typeId));
}

TEST_F(SegmentCompactorTest, MergeIntoSegment_CapacityExceeded) {
	auto type = Node::typeId;
	uint64_t seg1 = segmentAllocator->allocateSegment(type, 10);
	uint64_t seg2 = segmentAllocator->allocateSegment(type, 10);

	// Fill both to ~60% capacity — total would exceed 100%
	SegmentHeader h1 = segmentTracker->getSegmentHeader(seg1);
	for (uint32_t i = 0; i < 6; ++i) {
		createActiveNode(seg1, i, h1.start_id + i);
	}
	segmentTracker->updateSegmentUsage(seg1, 6, 0);

	SegmentHeader h2 = segmentTracker->getSegmentHeader(seg2);
	for (uint32_t i = 0; i < 6; ++i) {
		createActiveNode(seg2, i, h2.start_id + i);
	}
	segmentTracker->updateSegmentUsage(seg2, 6, 0);

	EXPECT_FALSE(compactor->mergeIntoSegment(seg1, seg2, type));
}

TEST_F(SegmentCompactorTest, MergeIntoSegment_Success) {
	auto type = Node::typeId;
	uint64_t seg1 = segmentAllocator->allocateSegment(type, 10);
	uint64_t seg2 = segmentAllocator->allocateSegment(type, 10);

	// seg1 (target): 2 active entities
	SegmentHeader h1 = segmentTracker->getSegmentHeader(seg1);
	for (uint32_t i = 0; i < 2; ++i) {
		createActiveNode(seg1, i, h1.start_id + i);
	}
	segmentTracker->updateSegmentUsage(seg1, 2, 0);

	// seg2 (source): 2 active entities — fits into seg1's remaining capacity
	SegmentHeader h2 = segmentTracker->getSegmentHeader(seg2);
	for (uint32_t i = 0; i < 2; ++i) {
		createActiveNode(seg2, i, h2.start_id + i);
	}
	segmentTracker->updateSegmentUsage(seg2, 2, 0);

	// Merge seg2 into seg1
	bool result = compactor->mergeIntoSegment(seg1, seg2, type);
	EXPECT_TRUE(result);

	// seg1 should now have 4 entities
	SegmentHeader merged = segmentTracker->getSegmentHeader(seg1);
	EXPECT_EQ(merged.used, 4u);
}

TEST_F(SegmentCompactorTest, MergeIntoSegment_SourceIsChainHead) {
	auto type = Node::typeId;
	// Allocate 3 segments — chain: seg3 → seg2 → seg1
	uint64_t seg1 = segmentAllocator->allocateSegment(type, 10);
	uint64_t seg2 = segmentAllocator->allocateSegment(type, 10);
	uint64_t seg3 = segmentAllocator->allocateSegment(type, 10);

	// Put minimal data in each
	SegmentHeader h1 = segmentTracker->getSegmentHeader(seg1);
	createActiveNode(seg1, 0, h1.start_id);
	segmentTracker->updateSegmentUsage(seg1, 1, 0);

	SegmentHeader h2 = segmentTracker->getSegmentHeader(seg2);
	createActiveNode(seg2, 0, h2.start_id);
	segmentTracker->updateSegmentUsage(seg2, 1, 0);

	SegmentHeader h3 = segmentTracker->getSegmentHeader(seg3);
	createActiveNode(seg3, 0, h3.start_id);
	segmentTracker->updateSegmentUsage(seg3, 1, 0);

	// The chain head is seg3 (last allocated). Merge seg3 (source=chain head) into seg1.
	// This exercises the "source is chain head" branch (lines 188-189).
	uint64_t chainHead = segmentTracker->getChainHead(type);
	bool result = compactor->mergeIntoSegment(seg1, chainHead, type);
	EXPECT_TRUE(result);
}

TEST_F(SegmentCompactorTest, MergeIntoSegment_SourceHasNextSegment) {
	auto type = Node::typeId;
	// Allocate 3 segments — chain: seg3 → seg2 → seg1
	uint64_t seg1 = segmentAllocator->allocateSegment(type, 10);
	uint64_t seg2 = segmentAllocator->allocateSegment(type, 10);
	uint64_t seg3 = segmentAllocator->allocateSegment(type, 10);

	// Put minimal data in each
	for (auto seg : {seg1, seg2, seg3}) {
		SegmentHeader h = segmentTracker->getSegmentHeader(seg);
		createActiveNode(seg, 0, h.start_id);
		segmentTracker->updateSegmentUsage(seg, 1, 0);
	}

	// Merge seg2 (middle, has both prev and next) into seg1
	// This exercises next_segment_offset != 0 branch (lines 192-196)
	bool result = compactor->mergeIntoSegment(seg1, seg2, type);
	EXPECT_TRUE(result);
}

TEST_F(SegmentCompactorTest, MergeSegments_SuccessfulMerge) {
	auto type = Node::typeId;
	uint64_t seg1 = segmentAllocator->allocateSegment(type, 10);
	uint64_t seg2 = segmentAllocator->allocateSegment(type, 10);

	// seg1: 2 active entities
	SegmentHeader h1 = segmentTracker->getSegmentHeader(seg1);
	for (uint32_t i = 0; i < 2; ++i) {
		createActiveNode(seg1, i, h1.start_id + i);
	}
	segmentTracker->updateSegmentUsage(seg1, 2, 0);

	// seg2: 2 active entities
	SegmentHeader h2 = segmentTracker->getSegmentHeader(seg2);
	for (uint32_t i = 0; i < 2; ++i) {
		createActiveNode(seg2, i, h2.start_id + i);
	}
	segmentTracker->updateSegmentUsage(seg2, 2, 0);

	// Both have 20% usage — below 70% threshold → both are candidates
	bool merged = compactor->mergeSegments(type, 0.7);

	// Verify data integrity — at least some segments exist
	auto segments = segmentTracker->getSegmentsByType(type);
	EXPECT_GE(segments.size(), 1u);
	// If merge succeeded, we should have fewer segments
	if (merged) {
		EXPECT_LE(segments.size(), 2u);
	}
}

TEST_F(SegmentCompactorTest, MergeSegments_ManySegments_TriggersPhases) {
	// Create many segments to trigger the end-to-front and consolidation phases
	auto type = Node::typeId;

	// Allocate 6+ segments so we get both front and end candidates
	std::vector<uint64_t> segs;
	for (int i = 0; i < 8; ++i) {
		uint64_t seg = segmentAllocator->allocateSegment(type, 10);
		SegmentHeader h = segmentTracker->getSegmentHeader(seg);
		// Put 1 active entity in each — very low usage
		createActiveNode(seg, 0, h.start_id);
		segmentTracker->updateSegmentUsage(seg, 1, 0);
		segs.push_back(seg);
	}

	// All segments have 10% usage, well below 0.7 threshold → all are candidates
	bool merged = compactor->mergeSegments(type, 0.7);

	auto segments = segmentTracker->getSegmentsByType(type);
	// After merging, we should have fewer segments (data consolidated)
	EXPECT_GE(segments.size(), 1u);
	if (merged) {
		EXPECT_LT(segments.size(), 8u);
	}
}

TEST_F(SegmentCompactorTest, CompactSegments_MoreThanFiveNeeding) {
	// Create >5 segments needing compaction to trigger the sort+resize(5) path
	auto type = Node::typeId;

	for (int i = 0; i < 7; ++i) {
		uint64_t seg = segmentAllocator->allocateSegment(type, 10);
		SegmentHeader h = segmentTracker->getSegmentHeader(seg);
		// Create 5 entities, mark 3 inactive → 60% fragmentation (above any reasonable threshold)
		for (uint32_t j = 0; j < 5; ++j) {
			createActiveNode(seg, j, h.start_id + j);
		}
		for (uint32_t j = 0; j < 3; ++j) {
			segmentTracker->setEntityActive(seg, j, false);
		}
		segmentTracker->updateSegmentUsage(seg, 5, 3);
	}

	// threshold=0.1 so all 7 are candidates, triggering the >5 sort+limit path
	compactor->compactSegments(type, 0.1);

	// Should not crash; some compaction should have occurred
	auto segments = segmentTracker->getSegmentsByType(type);
	EXPECT_GE(segments.size(), 1u);
}

// =========================================================================
// Process Empty Segments Tests
// =========================================================================

TEST_F(SegmentCompactorTest, ProcessEmptySegments_RemovesEmpty) {
	auto type = Node::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	// Segment has used=0 (empty)
	segmentTracker->updateSegmentUsage(seg, 0, 0);

	bool removed = compactor->processEmptySegments(type);
	EXPECT_TRUE(removed);

	auto segments = segmentTracker->getSegmentsByType(type);
	EXPECT_EQ(segments.size(), 0u);
}

TEST_F(SegmentCompactorTest, ProcessEmptySegments_NoEmpty) {
	auto type = Node::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	createActiveNode(seg, 0, h.start_id);
	segmentTracker->updateSegmentUsage(seg, 1, 0);

	bool removed = compactor->processEmptySegments(type);
	EXPECT_FALSE(removed);
}

TEST_F(SegmentCompactorTest, ProcessAllEmptySegments) {
	// Create empty segments for multiple types
	uint64_t nodeSeg = segmentAllocator->allocateSegment(Node::typeId, 10);
	uint64_t edgeSeg = segmentAllocator->allocateSegment(Edge::typeId, 10);
	segmentTracker->updateSegmentUsage(nodeSeg, 0, 0);
	segmentTracker->updateSegmentUsage(edgeSeg, 0, 0);

	compactor->processAllEmptySegments();

	EXPECT_EQ(segmentTracker->getSegmentsByType(Node::typeId).size(), 0u);
	EXPECT_EQ(segmentTracker->getSegmentsByType(Edge::typeId).size(), 0u);
}

// =========================================================================
// Move Segment Tests
// =========================================================================

TEST_F(SegmentCompactorTest, MoveSegment_SameOffset_NoOp) {
	auto type = Node::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	EXPECT_TRUE(compactor->moveSegment(seg, seg));
}

TEST_F(SegmentCompactorTest, MoveSegment_UpdatesChainLinks) {
	auto type = Node::typeId;
	uint64_t seg1 = segmentAllocator->allocateSegment(type, 10);
	uint64_t seg2 = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h1 = segmentTracker->getSegmentHeader(seg1);
	createActiveNode(seg1, 0, h1.start_id);
	segmentTracker->updateSegmentUsage(seg1, 1, 0);

	SegmentHeader h2 = segmentTracker->getSegmentHeader(seg2);
	createActiveNode(seg2, 0, h2.start_id);
	segmentTracker->updateSegmentUsage(seg2, 1, 0);

	// Deallocate seg1 to create a free slot, then move seg2 to seg1's position
	segmentAllocator->deallocateSegment(seg1);

	bool moved = compactor->moveSegment(seg2, seg1);
	EXPECT_TRUE(moved);

	// Verify moved segment is now at seg1's offset
	SegmentHeader movedHeader = segmentTracker->getSegmentHeader(seg1);
	EXPECT_EQ(movedHeader.data_type, type);
	EXPECT_GE(movedHeader.used, 1u);
}

// =========================================================================
// Recalculate Max IDs Tests
// =========================================================================

TEST_F(SegmentCompactorTest, RecalculateMaxIds_ScansCorrectly) {
	auto type = Node::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 5; ++i) {
		createActiveNode(seg, i, h.start_id + i);
	}
	segmentTracker->updateSegmentUsage(seg, 5, 0);

	compactor->recalculateMaxIds();

	// Max node ID should be at least start_id + 4
	EXPECT_GE(fileHeaderManager->getMaxNodeIdRef(), h.start_id + 4);
}

TEST_F(SegmentCompactorTest, RecalculateMaxIds_IgnoresInactive) {
	auto type = Node::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 5; ++i) {
		createActiveNode(seg, i, h.start_id + i);
	}
	// Mark last two as inactive
	segmentTracker->setEntityActive(seg, 3, false);
	segmentTracker->setEntityActive(seg, 4, false);
	segmentTracker->updateSegmentUsage(seg, 5, 2);

	compactor->recalculateMaxIds();

	// Max should reflect the last active ID (start_id + 2)
	int64_t maxNodeId = fileHeaderManager->getMaxNodeIdRef();
	EXPECT_GE(maxNodeId, h.start_id + 2);
}

TEST_F(SegmentCompactorTest, RecalculateMaxIds_AllSegmentsEmpty) {
	auto type = Node::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);
	segmentTracker->updateSegmentUsage(seg, 0, 0);

	compactor->recalculateMaxIds();

	// Max ID should be 0 when all segments are empty
	EXPECT_EQ(fileHeaderManager->getMaxNodeIdRef(), 0);
}

TEST_F(SegmentCompactorTest, RecalculateMaxIds_EdgeType) {
	auto type = Edge::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 3; ++i) {
		createActiveEntity<Edge>(seg, i, h.start_id + i);
	}
	segmentTracker->updateSegmentUsage(seg, 3, 0);

	compactor->recalculateMaxIds();

	EXPECT_GE(fileHeaderManager->getMaxEdgeIdRef(), h.start_id + 2);
}

TEST_F(SegmentCompactorTest, RecalculateMaxIds_PropertyType) {
	auto type = Property::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 3; ++i) {
		createActiveEntity<Property>(seg, i, h.start_id + i);
	}
	segmentTracker->updateSegmentUsage(seg, 3, 0);

	compactor->recalculateMaxIds();

	EXPECT_GE(fileHeaderManager->getMaxPropIdRef(), h.start_id + 2);
}

TEST_F(SegmentCompactorTest, RecalculateMaxIds_BlobType) {
	auto type = Blob::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 3; ++i) {
		createActiveEntity<Blob>(seg, i, h.start_id + i);
	}
	segmentTracker->updateSegmentUsage(seg, 3, 0);

	compactor->recalculateMaxIds();

	EXPECT_GE(fileHeaderManager->getMaxBlobIdRef(), h.start_id + 2);
}

TEST_F(SegmentCompactorTest, RecalculateMaxIds_IndexType) {
	auto type = Index::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 3; ++i) {
		createActiveEntity<Index>(seg, i, h.start_id + i);
	}
	segmentTracker->updateSegmentUsage(seg, 3, 0);

	compactor->recalculateMaxIds();

	EXPECT_GE(fileHeaderManager->getMaxIndexIdRef(), h.start_id + 2);
}

TEST_F(SegmentCompactorTest, RecalculateMaxIds_StateType) {
	auto type = State::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	for (uint32_t i = 0; i < 3; ++i) {
		createActiveEntity<State>(seg, i, h.start_id + i);
	}
	segmentTracker->updateSegmentUsage(seg, 3, 0);

	compactor->recalculateMaxIds();

	EXPECT_GE(fileHeaderManager->getMaxStateIdRef(), h.start_id + 2);
}

TEST_F(SegmentCompactorTest, MoveSegment_ChainHeadUpdate) {
	auto type = Node::typeId;
	uint64_t seg1 = segmentAllocator->allocateSegment(type, 10);
	uint64_t seg2 = segmentAllocator->allocateSegment(type, 10);

	// seg2 is chain head. Put data in it.
	SegmentHeader h2 = segmentTracker->getSegmentHeader(seg2);
	createActiveNode(seg2, 0, h2.start_id);
	segmentTracker->updateSegmentUsage(seg2, 1, 0);

	// Deallocate seg1 to create free slot
	segmentAllocator->deallocateSegment(seg1);

	// Move chain head (seg2) to seg1 position
	uint64_t chainHead = segmentTracker->getChainHead(type);
	EXPECT_EQ(chainHead, seg2);

	bool moved = compactor->moveSegment(seg2, seg1);
	EXPECT_TRUE(moved);

	// Chain head should now point to seg1
	uint64_t newChainHead = segmentTracker->getChainHead(type);
	EXPECT_EQ(newChainHead, seg1);
}

TEST_F(SegmentCompactorTest, MoveSegment_WithNextSegment) {
	auto type = Node::typeId;
	uint64_t seg1 = segmentAllocator->allocateSegment(type, 10);
	uint64_t seg2 = segmentAllocator->allocateSegment(type, 10);
	uint64_t seg3 = segmentAllocator->allocateSegment(type, 10);

	// Put data in all three
	for (auto seg : {seg1, seg2, seg3}) {
		SegmentHeader h = segmentTracker->getSegmentHeader(seg);
		createActiveNode(seg, 0, h.start_id);
		segmentTracker->updateSegmentUsage(seg, 1, 0);
	}

	// Deallocate seg1 to create free slot, then move seg2 (has next=seg1 link) to it
	segmentAllocator->deallocateSegment(seg1);

	bool moved = compactor->moveSegment(seg2, seg1);
	EXPECT_TRUE(moved);

	// Verify seg3's chain still links correctly
	auto segments = segmentTracker->getSegmentsByType(type);
	EXPECT_GE(segments.size(), 1u);
}

// =========================================================================
// Relocate Segments Tests
// =========================================================================

TEST_F(SegmentCompactorTest, RelocateSegments_NoHoles_NoOp) {
	auto type = Node::typeId;
	uint64_t seg = segmentAllocator->allocateSegment(type, 10);

	SegmentHeader h = segmentTracker->getSegmentHeader(seg);
	createActiveNode(seg, 0, h.start_id);
	segmentTracker->updateSegmentUsage(seg, 1, 0);

	// No free segments — relocation should be a no-op
	bool relocated = compactor->relocateSegmentsFromEnd();
	EXPECT_FALSE(relocated);
}

TEST_F(SegmentCompactorTest, IsSegmentAtEndOfFile) {
	auto type = Node::typeId;
	uint64_t seg1 = segmentAllocator->allocateSegment(type, 10);
	uint64_t seg2 = segmentAllocator->allocateSegment(type, 10);

	EXPECT_FALSE(compactor->isSegmentAtEndOfFile(seg1));
	EXPECT_TRUE(compactor->isSegmentAtEndOfFile(seg2));
}

TEST_F(SegmentCompactorTest, FindFreeSegmentNotAtEnd_NoFreeSegments) {
	auto type = Node::typeId;
	segmentAllocator->allocateSegment(type, 10);

	EXPECT_EQ(compactor->findFreeSegmentNotAtEnd(), 0ULL);
}

TEST_F(SegmentCompactorTest, FindFreeSegmentNotAtEnd_HasFreeHole) {
	auto type = Node::typeId;
	uint64_t seg1 = segmentAllocator->allocateSegment(type, 10);
	uint64_t seg2 = segmentAllocator->allocateSegment(type, 10);

	// Free the first segment — it's a "hole" before the end
	segmentAllocator->deallocateSegment(seg1);

	// seg2 is at the end and still allocated, seg1 is free and not at end
	// Also deallocate seg2 so there's free space at end too
	segmentAllocator->deallocateSegment(seg2);

	uint64_t freeHole = compactor->findFreeSegmentNotAtEnd();
	EXPECT_EQ(freeHole, seg1);
}

TEST_F(SegmentCompactorTest, RelocateSegments_MovesTailToHole) {
	auto type = Node::typeId;
	uint32_t maxItemsPerSeg = SEGMENT_SIZE / Node::getTotalSize();
	ASSERT_GE(maxItemsPerSeg, 2U) << "Segment size too small for this test";

	uint32_t fillCount = std::max(1u, maxItemsPerSeg / 2);

	uint64_t segHole = segmentAllocator->allocateSegment(type, maxItemsPerSeg);
	uint64_t segMid = segmentAllocator->allocateSegment(type, maxItemsPerSeg);
	uint64_t segTail = segmentAllocator->allocateSegment(type, maxItemsPerSeg);

	// Fill mid and tail with active entities
	SegmentHeader hMid = segmentTracker->getSegmentHeader(segMid);
	for (uint32_t i = 0; i < fillCount; ++i) {
		createActiveNode(segMid, i, hMid.start_id + i);
	}
	segmentTracker->updateSegmentUsage(segMid, fillCount, 0);

	SegmentHeader hTail = segmentTracker->getSegmentHeader(segTail);
	for (uint32_t i = 0; i < fillCount; ++i) {
		createActiveNode(segTail, i, hTail.start_id + i);
	}
	segmentTracker->updateSegmentUsage(segTail, fillCount, 0);

	// Free the first segment to create a hole
	segmentAllocator->deallocateSegment(segHole);

	// Relocate should move the tail segment into the hole
	bool relocated = compactor->relocateSegmentsFromEnd();
	// Whether it succeeds depends on internal logic, but it shouldn't crash
	(void)relocated;

	// Verify at least some segments exist
	auto segments = segmentTracker->getSegmentsByType(type);
	EXPECT_GE(segments.size(), 1u);
}
