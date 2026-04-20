/**
 * @file test_SegmentAllocator.cpp
 * @author Nexepic
 * @date 2025/12/4
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
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
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/SegmentAllocator.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/StorageIO.hpp"

using namespace graph::storage;
using namespace graph;

class SegmentAllocatorTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	FileHeader header;
	std::shared_ptr<SegmentTracker> segmentTracker;
	std::shared_ptr<FileHeaderManager> fileHeaderManager;
	std::shared_ptr<SegmentAllocator> allocator;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("seg_alloc_test_" + to_string(uuid) + ".dat");

		file = std::make_shared<std::fstream>(testFilePath,
			std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());

		file->write(reinterpret_cast<char *>(&header), sizeof(FileHeader));
		file->flush();

		auto storageIO = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);

		segmentTracker = std::make_shared<SegmentTracker>(storageIO, header);
		fileHeaderManager = std::make_shared<FileHeaderManager>(file, header);
		allocator = std::make_shared<SegmentAllocator>(storageIO, segmentTracker, fileHeaderManager);
	}

	void TearDown() override {
		allocator.reset();
		fileHeaderManager.reset();
		segmentTracker.reset();
		if (file && file->is_open()) file->close();
		file.reset();
		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	// Helper to verify chain integrity
	void verifyChainLinks(uint32_t type, const std::vector<uint64_t> &expectedOffsets) const {
		if (expectedOffsets.empty()) {
			EXPECT_EQ(segmentTracker->getChainHead(type), 0ULL);
			return;
		}
		EXPECT_EQ(segmentTracker->getChainHead(type), expectedOffsets[0]);
		for (size_t i = 0; i < expectedOffsets.size(); ++i) {
			SegmentHeader h = segmentTracker->getSegmentHeader(expectedOffsets[i]);
			EXPECT_EQ(h.data_type, type);
			EXPECT_EQ(h.prev_segment_offset, i == 0 ? 0ULL : expectedOffsets[i - 1]);
			EXPECT_EQ(h.next_segment_offset, i == expectedOffsets.size() - 1 ? 0ULL : expectedOffsets[i + 1]);
		}
	}
};

// =========================================================================
// GROUP 1: Segment Allocation
// =========================================================================

TEST_F(SegmentAllocatorTest, AllocateSegment_FirstSegment) {
	auto type = toUnderlying(EntityType::Node);
	uint64_t offset = allocator->allocateSegment(type, NODES_PER_SEGMENT);

	// First segment must be immediately after the file header
	EXPECT_EQ(offset, sizeof(FileHeader));

	// The chain head for nodes must point to this segment
	EXPECT_EQ(segmentTracker->getChainHead(type), offset);

	// Verify the segment header stored by the tracker
	SegmentHeader h = segmentTracker->getSegmentHeader(offset);
	EXPECT_EQ(h.data_type, type);
	EXPECT_EQ(h.capacity, NODES_PER_SEGMENT);
	EXPECT_EQ(h.file_offset, offset);
	EXPECT_EQ(h.start_id, 1);
	EXPECT_EQ(h.used, 0u);
	EXPECT_EQ(h.prev_segment_offset, 0ULL);
	EXPECT_EQ(h.next_segment_offset, 0ULL);
}

TEST_F(SegmentAllocatorTest, AllocateSegment_MultipleTypes) {
	auto nodeType = toUnderlying(EntityType::Node);
	auto edgeType = toUnderlying(EntityType::Edge);
	auto propType = toUnderlying(EntityType::Property);

	uint64_t nodeOffset = allocator->allocateSegment(nodeType, NODES_PER_SEGMENT);
	uint64_t edgeOffset = allocator->allocateSegment(edgeType, EDGES_PER_SEGMENT);
	uint64_t propOffset = allocator->allocateSegment(propType, PROPERTIES_PER_SEGMENT);

	// Each segment has the correct type
	EXPECT_EQ(segmentTracker->getSegmentHeader(nodeOffset).data_type, nodeType);
	EXPECT_EQ(segmentTracker->getSegmentHeader(edgeOffset).data_type, edgeType);
	EXPECT_EQ(segmentTracker->getSegmentHeader(propOffset).data_type, propType);

	// Each type has its own independent chain head
	EXPECT_EQ(segmentTracker->getChainHead(nodeType), nodeOffset);
	EXPECT_EQ(segmentTracker->getChainHead(edgeType), edgeOffset);
	EXPECT_EQ(segmentTracker->getChainHead(propType), propOffset);

	// Correct capacities
	EXPECT_EQ(segmentTracker->getSegmentHeader(nodeOffset).capacity, NODES_PER_SEGMENT);
	EXPECT_EQ(segmentTracker->getSegmentHeader(edgeOffset).capacity, EDGES_PER_SEGMENT);
	EXPECT_EQ(segmentTracker->getSegmentHeader(propOffset).capacity, PROPERTIES_PER_SEGMENT);
}

TEST_F(SegmentAllocatorTest, AllocateSegment_ChainLinking) {
	auto type = toUnderlying(EntityType::Node);

	uint64_t off1 = allocator->allocateSegment(type, 10);
	uint64_t off2 = allocator->allocateSegment(type, 10);
	uint64_t off3 = allocator->allocateSegment(type, 10);

	// Verify the doubly-linked chain: off1 <-> off2 <-> off3
	verifyChainLinks(type, {off1, off2, off3});

	// Verify start_id progression: each segment starts after the previous capacity
	SegmentHeader h1 = segmentTracker->getSegmentHeader(off1);
	SegmentHeader h2 = segmentTracker->getSegmentHeader(off2);
	SegmentHeader h3 = segmentTracker->getSegmentHeader(off3);

	EXPECT_EQ(h1.start_id, 1);
	EXPECT_EQ(h2.start_id, static_cast<int64_t>(h1.start_id + h1.capacity + 1));
	EXPECT_EQ(h3.start_id, static_cast<int64_t>(h2.start_id + h2.capacity + 1));
}

TEST_F(SegmentAllocatorTest, AllocateSegment_SequentialFileOffsets) {
	auto type = toUnderlying(EntityType::Edge);

	uint64_t off1 = allocator->allocateSegment(type, 10);
	uint64_t off2 = allocator->allocateSegment(type, 10);
	uint64_t off3 = allocator->allocateSegment(type, 10);

	// Segments are laid out sequentially after the file header
	EXPECT_EQ(off1, sizeof(FileHeader));
	EXPECT_EQ(off2, sizeof(FileHeader) + TOTAL_SEGMENT_SIZE);
	EXPECT_EQ(off3, sizeof(FileHeader) + 2 * TOTAL_SEGMENT_SIZE);
}

// =========================================================================
// GROUP 2: Segment Deallocation and Chain Repair
// =========================================================================

TEST_F(SegmentAllocatorTest, DeallocateSegment_Middle) {
	auto type = toUnderlying(EntityType::Node);

	uint64_t off1 = allocator->allocateSegment(type, 10);
	uint64_t off2 = allocator->allocateSegment(type, 10);
	uint64_t off3 = allocator->allocateSegment(type, 10);

	// Remove the middle segment
	allocator->deallocateSegment(off2);

	// Chain should now be: off1 <-> off3
	verifyChainLinks(type, {off1, off3});

	// The deallocated segment should be in the free list
	auto freeSegs = segmentTracker->getFreeSegments();
	EXPECT_EQ(freeSegs.size(), 1u);
	EXPECT_EQ(freeSegs[0], off2);
}

TEST_F(SegmentAllocatorTest, DeallocateSegment_Head) {
	auto type = toUnderlying(EntityType::Node);

	uint64_t off1 = allocator->allocateSegment(type, 10);
	uint64_t off2 = allocator->allocateSegment(type, 10);
	uint64_t off3 = allocator->allocateSegment(type, 10);

	// Remove the head segment
	allocator->deallocateSegment(off1);

	// New chain: off2 <-> off3 (off2 is the new head)
	verifyChainLinks(type, {off2, off3});

	// Verify the new head has no previous
	SegmentHeader h2 = segmentTracker->getSegmentHeader(off2);
	EXPECT_EQ(h2.prev_segment_offset, 0ULL);
}

TEST_F(SegmentAllocatorTest, DeallocateSegment_Tail) {
	auto type = toUnderlying(EntityType::Node);

	uint64_t off1 = allocator->allocateSegment(type, 10);
	uint64_t off2 = allocator->allocateSegment(type, 10);
	uint64_t off3 = allocator->allocateSegment(type, 10);

	// Remove the tail segment
	allocator->deallocateSegment(off3);

	// Chain should be: off1 <-> off2 (off2 is now the tail)
	verifyChainLinks(type, {off1, off2});

	// Verify the new tail has no next
	SegmentHeader h2 = segmentTracker->getSegmentHeader(off2);
	EXPECT_EQ(h2.next_segment_offset, 0ULL);
}

// =========================================================================
// GROUP 3: Free Segment Reuse
// =========================================================================

TEST_F(SegmentAllocatorTest, AllocateSegment_ReusesFreeSegment) {
	auto type = toUnderlying(EntityType::Node);

	uint64_t off1 = allocator->allocateSegment(type, 10);
	uint64_t off2 = allocator->allocateSegment(type, 10);

	// Deallocate the first segment to put it on the free list
	allocator->deallocateSegment(off1);

	// Allocating a new segment should reuse the freed offset
	uint64_t off3 = allocator->allocateSegment(type, 10);
	EXPECT_EQ(off3, off1);

	// Free list should now be empty
	auto freeSegs = segmentTracker->getFreeSegments();
	EXPECT_TRUE(freeSegs.empty());
}

// =========================================================================
// GROUP 4: findMaxId
// =========================================================================

TEST_F(SegmentAllocatorTest, FindMaxId_EmptyChain) {
	auto type = toUnderlying(EntityType::Node);

	// No segments allocated for this type, so maxId should be 0
	uint64_t maxId = SegmentAllocator::findMaxId(type, segmentTracker);
	EXPECT_EQ(maxId, 0ULL);
}

TEST_F(SegmentAllocatorTest, FindMaxId_SingleSegment) {
	auto type = toUnderlying(EntityType::Node);

	allocator->allocateSegment(type, 10);

	// maxId = start_id + capacity = 1 + 10 = 11
	uint64_t maxId = SegmentAllocator::findMaxId(type, segmentTracker);
	EXPECT_EQ(maxId, 11ULL);
}

TEST_F(SegmentAllocatorTest, FindMaxId_MultipleSegments) {
	auto type = toUnderlying(EntityType::Edge);

	allocator->allocateSegment(type, 10);
	allocator->allocateSegment(type, 20);

	// The second segment gets start_id = findMaxId(first) + 1 = 12
	// Its contribution to findMaxId = 12 + 20 = 32
	uint64_t maxId = SegmentAllocator::findMaxId(type, segmentTracker);
	EXPECT_GT(maxId, 0ULL);

	// Verify it accounts for the last segment's range
	auto segments = segmentTracker->getSegmentsByType(type);
	ASSERT_EQ(segments.size(), 2u);
	uint64_t expectedMax = 0;
	for (const auto &seg : segments) {
		uint64_t segMax = static_cast<uint64_t>(seg.start_id + seg.capacity);
		if (segMax > expectedMax) expectedMax = segMax;
	}
	EXPECT_EQ(maxId, expectedMax);
}

// =========================================================================
// GROUP 5: updateFileHeaderChainHeads
// =========================================================================

TEST_F(SegmentAllocatorTest, UpdateFileHeaderChainHeads) {
	auto nodeType = toUnderlying(EntityType::Node);
	auto edgeType = toUnderlying(EntityType::Edge);
	auto propType = toUnderlying(EntityType::Property);

	uint64_t nodeOff = allocator->allocateSegment(nodeType, 10);
	uint64_t edgeOff = allocator->allocateSegment(edgeType, 10);
	uint64_t propOff = allocator->allocateSegment(propType, 10);

	// Call the method under test
	allocator->updateFileHeaderChainHeads();

	// Verify the file header matches the tracker chain heads
	FileHeader &fh = fileHeaderManager->getFileHeader();
	EXPECT_EQ(fh.node_segment_head, nodeOff);
	EXPECT_EQ(fh.edge_segment_head, edgeOff);
	EXPECT_EQ(fh.property_segment_head, propOff);

	// Types with no segments should have head = 0
	EXPECT_EQ(fh.blob_segment_head, 0ULL);
	EXPECT_EQ(fh.index_segment_head, 0ULL);
	EXPECT_EQ(fh.state_segment_head, 0ULL);
}

TEST_F(SegmentAllocatorTest, UpdateFileHeaderChainHeads_AfterDeallocation) {
	auto type = toUnderlying(EntityType::Node);

	uint64_t off1 = allocator->allocateSegment(type, 10);
	allocator->allocateSegment(type, 10);

	// Deallocate the head; this internally calls updateFileHeaderChainHeads
	allocator->deallocateSegment(off1);

	// Verify the file header was updated to the new chain head
	FileHeader &fh = fileHeaderManager->getFileHeader();
	EXPECT_EQ(fh.node_segment_head, segmentTracker->getChainHead(type));
	EXPECT_NE(fh.node_segment_head, off1);
}

// =========================================================================
// GROUP 6: Accessor
// =========================================================================

TEST_F(SegmentAllocatorTest, GetTracker_ReturnsSameInstance) {
	auto tracker = allocator->getTracker();
	EXPECT_EQ(tracker.get(), segmentTracker.get());
}

// =========================================================================
// GROUP 7: Invalid Type and Edge Cases
// =========================================================================

TEST_F(SegmentAllocatorTest, AllocateSegment_InvalidTypeThrows) {
	// Type greater than getMaxEntityType() should throw std::invalid_argument
	uint32_t invalidType = getMaxEntityType() + 1;
	EXPECT_THROW(allocator->allocateSegment(invalidType, 10), std::invalid_argument);
}

TEST_F(SegmentAllocatorTest, AllocateSegment_InvalidTypeLargeValueThrows) {
	// A much larger invalid type value
	uint32_t invalidType = getMaxEntityType() + 100;
	EXPECT_THROW(allocator->allocateSegment(invalidType, 10), std::invalid_argument);
}

TEST_F(SegmentAllocatorTest, AllocateSegment_TailZeroFallback) {
	auto type = toUnderlying(EntityType::Node);

	// Allocate a first segment so chainHead != 0
	uint64_t off1 = allocator->allocateSegment(type, 10);
	EXPECT_NE(off1, 0ULL);

	// Force the chain tail to 0 to trigger the fallback traversal path
	segmentTracker->updateChainTail(type, 0);

	// Allocate a second segment; the fallback path will walk the chain to find the tail
	uint64_t off2 = allocator->allocateSegment(type, 10);
	EXPECT_NE(off2, 0ULL);
	EXPECT_NE(off2, off1);

	// Verify chain is still properly linked: off1 <-> off2
	SegmentHeader h1 = segmentTracker->getSegmentHeader(off1);
	SegmentHeader h2 = segmentTracker->getSegmentHeader(off2);
	EXPECT_EQ(h1.next_segment_offset, off2);
	EXPECT_EQ(h2.prev_segment_offset, off1);
	EXPECT_EQ(h2.next_segment_offset, 0ULL);
}

TEST_F(SegmentAllocatorTest, AllocateSegment_TailZeroFallbackMultipleSegments) {
	auto type = toUnderlying(EntityType::Edge);

	// Build a chain of 3 segments
	(void)allocator->allocateSegment(type, 10);
	(void)allocator->allocateSegment(type, 10);
	uint64_t off3 = allocator->allocateSegment(type, 10);

	// Force the chain tail to 0 to trigger the fallback traversal
	segmentTracker->updateChainTail(type, 0);

	// Allocate a 4th segment; fallback walks off1 -> off2 -> off3 to find the end
	uint64_t off4 = allocator->allocateSegment(type, 10);
	EXPECT_NE(off4, 0ULL);

	// Verify off3 -> off4 link
	SegmentHeader h3 = segmentTracker->getSegmentHeader(off3);
	SegmentHeader h4 = segmentTracker->getSegmentHeader(off4);
	EXPECT_EQ(h3.next_segment_offset, off4);
	EXPECT_EQ(h4.prev_segment_offset, off3);
	EXPECT_EQ(h4.next_segment_offset, 0ULL);
}

TEST_F(SegmentAllocatorTest, DeallocateSegment_OnlySegmentInChain) {
	auto type = toUnderlying(EntityType::Node);

	// Allocate a single segment — it is both the head and tail
	uint64_t off1 = allocator->allocateSegment(type, 10);

	// Deallocate the only segment (isChainHead == true, nextOffset == 0)
	allocator->deallocateSegment(off1);

	// Chain should now be empty
	EXPECT_EQ(segmentTracker->getChainHead(type), 0ULL);

	// The segment should be on the free list
	auto freeSegs = segmentTracker->getFreeSegments();
	EXPECT_EQ(freeSegs.size(), 1u);
	EXPECT_EQ(freeSegs[0], off1);

	// File header should reflect the empty chain
	FileHeader &fh = fileHeaderManager->getFileHeader();
	EXPECT_EQ(fh.node_segment_head, 0ULL);
}

TEST_F(SegmentAllocatorTest, DeallocateSegment_HeadWithNext) {
	auto type = toUnderlying(EntityType::Property);

	uint64_t off1 = allocator->allocateSegment(type, 10);
	uint64_t off2 = allocator->allocateSegment(type, 10);

	// Deallocate head (isChainHead == true, nextOffset != 0)
	allocator->deallocateSegment(off1);

	// off2 should be the new head with no previous
	EXPECT_EQ(segmentTracker->getChainHead(type), off2);
	SegmentHeader h2 = segmentTracker->getSegmentHeader(off2);
	EXPECT_EQ(h2.prev_segment_offset, 0ULL);

	// File header should be updated
	FileHeader &fh = fileHeaderManager->getFileHeader();
	EXPECT_EQ(fh.property_segment_head, off2);
}

TEST_F(SegmentAllocatorTest, DeallocateSegment_MiddleWithNextAndPrev) {
	auto type = toUnderlying(EntityType::Node);

	uint64_t off1 = allocator->allocateSegment(type, 10);
	uint64_t off2 = allocator->allocateSegment(type, 10);
	uint64_t off3 = allocator->allocateSegment(type, 10);
	uint64_t off4 = allocator->allocateSegment(type, 10);

	// Remove off2 (middle segment, prevOffset != 0, nextOffset != 0)
	allocator->deallocateSegment(off2);

	// Chain should be: off1 <-> off3 <-> off4
	verifyChainLinks(type, {off1, off3, off4});

	// off2 should be free
	auto freeSegs = segmentTracker->getFreeSegments();
	EXPECT_EQ(freeSegs.size(), 1u);
	EXPECT_EQ(freeSegs[0], off2);
}

TEST_F(SegmentAllocatorTest, AllocateSegment_ReusesFreeSegmentFromDeallocatedHead) {
	auto type = toUnderlying(EntityType::Node);

	uint64_t off1 = allocator->allocateSegment(type, 10);
	(void)allocator->allocateSegment(type, 10);

	// Deallocate the head segment, putting it on the free list
	allocator->deallocateSegment(off1);

	// Allocate a new segment — should reuse the freed offset
	uint64_t off3 = allocator->allocateSegment(type, 10);
	EXPECT_EQ(off3, off1);

	// Free list should now be empty
	auto freeSegs = segmentTracker->getFreeSegments();
	EXPECT_TRUE(freeSegs.empty());
}

TEST_F(SegmentAllocatorTest, AllocateSegment_AllEntityTypes) {
	// Verify allocation works for all valid entity types including Blob, Index, State
	auto blobType = toUnderlying(EntityType::Blob);
	auto indexType = toUnderlying(EntityType::Index);
	auto stateType = toUnderlying(EntityType::State);

	uint64_t blobOff = allocator->allocateSegment(blobType, 10);
	uint64_t indexOff = allocator->allocateSegment(indexType, 10);
	uint64_t stateOff = allocator->allocateSegment(stateType, 10);

	EXPECT_NE(blobOff, 0ULL);
	EXPECT_NE(indexOff, 0ULL);
	EXPECT_NE(stateOff, 0ULL);

	EXPECT_EQ(segmentTracker->getSegmentHeader(blobOff).data_type, blobType);
	EXPECT_EQ(segmentTracker->getSegmentHeader(indexOff).data_type, indexType);
	EXPECT_EQ(segmentTracker->getSegmentHeader(stateOff).data_type, stateType);
}
