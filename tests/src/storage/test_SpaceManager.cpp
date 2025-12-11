/**
 * @file test_SpaceManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/4
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <algorithm>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "graph/core/Node.hpp"
#include "graph/storage/EntityReferenceUpdater.hpp"
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/SpaceManager.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/data/DataManager.hpp"

using namespace graph::storage;
using namespace graph;

/**
 * @brief Test Fixture for SpaceManager
 */
class SpaceManagerTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	FileHeader header; // Member variable to prevent dangling reference

	std::shared_ptr<DataManager> dataManager;
	std::shared_ptr<SegmentTracker> segmentTracker;
	std::shared_ptr<FileHeaderManager> fileHeaderManager;
	std::shared_ptr<IDAllocator> idAllocator;
	std::shared_ptr<EntityReferenceUpdater> refUpdater;

	std::shared_ptr<SpaceManager> spaceManager;

	void SetUp() override {
		// Generate random file path
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("spacemgr_full_" + to_string(uuid) + ".dat");

		file = std::make_shared<std::fstream>(testFilePath,
											  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open()) << "Failed to open temporary test file.";

		// Write initial header
		file->write(reinterpret_cast<char *>(&header), sizeof(FileHeader));
		file->flush();

		// Initialize Components
		segmentTracker = std::make_shared<SegmentTracker>(file, header);
		fileHeaderManager = std::make_shared<FileHeaderManager>(file, header);

		idAllocator = std::make_shared<IDAllocator>(
				file, segmentTracker, fileHeaderManager->getMaxNodeIdRef(), fileHeaderManager->getMaxEdgeIdRef(),
				fileHeaderManager->getMaxPropIdRef(), fileHeaderManager->getMaxBlobIdRef(),
				fileHeaderManager->getMaxIndexIdRef(), fileHeaderManager->getMaxStateIdRef());

		spaceManager = std::make_shared<SpaceManager>(file, testFilePath.string(), segmentTracker, fileHeaderManager,
													  idAllocator);

		dataManager = std::make_shared<DataManager>(file,
													100, // Cache size
													header, idAllocator, segmentTracker, spaceManager);

		refUpdater = std::make_shared<EntityReferenceUpdater>(dataManager);

		spaceManager->setEntityReferenceUpdater(refUpdater);
	}

	void TearDown() override {
		spaceManager.reset();
		refUpdater.reset();
		idAllocator.reset();
		fileHeaderManager.reset();
		segmentTracker.reset();
		if (file && file->is_open())
			file->close();
		std::filesystem::remove(testFilePath);
	}

	[[nodiscard]] uint64_t getFileSize() const { return std::filesystem::file_size(testFilePath); }

	// Helper: Write a mock node
	void writeNode(uint64_t segmentOffset, uint32_t index, int64_t id, const std::string &label) const {
		Node node(id, label);
		segmentTracker->writeEntity(segmentOffset, index, node, Node::getTotalSize());
		segmentTracker->setEntityActive(segmentOffset, index, true);
	}

	// Helper: Verify chain links locally
	void verifyChainLinks(uint32_t type, const std::vector<uint64_t> &expectedOffsets) const {
		if (expectedOffsets.empty()) {
			EXPECT_EQ(segmentTracker->getChainHead(type), 0);
			return;
		}

		// Check Head
		EXPECT_EQ(segmentTracker->getChainHead(type), expectedOffsets[0]);

		for (size_t i = 0; i < expectedOffsets.size(); ++i) {
			uint64_t curr = expectedOffsets[i];
			SegmentHeader h = segmentTracker->getSegmentHeader(curr);

			// Check Data Type
			EXPECT_EQ(h.data_type, type);

			// Check Prev
			if (i == 0) {
				EXPECT_EQ(h.prev_segment_offset, 0);
			} else {
				EXPECT_EQ(h.prev_segment_offset, expectedOffsets[i - 1]);
			}

			// Check Next
			if (i == expectedOffsets.size() - 1) {
				EXPECT_EQ(h.next_segment_offset, 0);
			} else {
				EXPECT_EQ(h.next_segment_offset, expectedOffsets[i + 1]);
			}
		}
	}
};

// =========================================================================
// GROUP 1: Allocation & Chain Topology (CRITICAL FOR DATA INTEGRITY)
// =========================================================================

TEST_F(SpaceManagerTest, AllocateSegment_CorrectOffsets) {
	auto type = static_cast<uint32_t>(EntityType::Node);
	// The first segment must be immediately after FILE_HEADER_SIZE
	uint64_t offset1 = spaceManager->allocateSegment(type, 10);
	ASSERT_EQ(offset1, FILE_HEADER_SIZE);

	// The second segment must be offset1 + TOTAL_SEGMENT_SIZE
	uint64_t offset2 = spaceManager->allocateSegment(type, 10);
	ASSERT_EQ(offset2, FILE_HEADER_SIZE + TOTAL_SEGMENT_SIZE);
}

TEST_F(SpaceManagerTest, ComplexChainManipulation_DeleteOrder) {
	auto type = static_cast<uint32_t>(EntityType::Edge);

	// Allocate 5 segments: A -> B -> C -> D -> E
	std::vector<uint64_t> offsets;
	offsets.reserve(5);
	for (int i = 0; i < 5; i++)
		offsets.push_back(spaceManager->allocateSegment(type, 10));

	// Verify initial state
	verifyChainLinks(type, offsets);

	// 1. Delete Head (A)
	spaceManager->deallocateSegment(offsets[0]);
	// Expect: B -> C -> D -> E
	verifyChainLinks(type, {offsets[1], offsets[2], offsets[3], offsets[4]});

	// 2. Delete Tail (E)
	spaceManager->deallocateSegment(offsets[4]);
	// Expect: B -> C -> D
	verifyChainLinks(type, {offsets[1], offsets[2], offsets[3]});

	// 3. Delete Middle (C)
	spaceManager->deallocateSegment(offsets[2]);
	// Expect: B -> D
	verifyChainLinks(type, {offsets[1], offsets[3]});

	// 4. Delete Remaining Head (B)
	spaceManager->deallocateSegment(offsets[1]);
	// Expect: D
	verifyChainLinks(type, {offsets[3]});

	// 5. Delete Last One (D)
	spaceManager->deallocateSegment(offsets[3]);
	// Expect: Empty
	verifyChainLinks(type, {});
}

TEST_F(SpaceManagerTest, ReuseFreeSegmentsStrategy) {
	auto type = static_cast<uint32_t>(EntityType::Node);
	uint64_t seg1 = spaceManager->allocateSegment(type, 10);
	uint64_t seg2 = spaceManager->allocateSegment(type, 10);
	uint64_t seg3 = spaceManager->allocateSegment(type, 10);

	// Current file size should cover 3 segments
	uint64_t sizeAfterAlloc = getFileSize();

	// Free the middle segment (Seg2)
	spaceManager->deallocateSegment(seg2);

	// Allocate a NEW segment. It MUST reuse Seg2's space, not append to file.
	uint64_t seg4 = spaceManager->allocateSegment(type, 10);

	EXPECT_EQ(seg4, seg2) << "SpaceManager failed to reuse the free segment gap.";
	EXPECT_EQ(getFileSize(), sizeAfterAlloc) << "File size grew but should have reused space.";

	// Check Chain: Seg1 -> Seg3 -> Seg4 (Order depends on implementation, usually append to tail)
	// Actually implementation appends to tail of linked list, but physically located at seg2 offset.
	// Chain should be: Seg1 -> Seg3 -> Seg4(physical seg2)

	SegmentHeader h1 = segmentTracker->getSegmentHeader(seg1);
	SegmentHeader h3 = segmentTracker->getSegmentHeader(seg3);
	SegmentHeader h4 = segmentTracker->getSegmentHeader(seg4);

	EXPECT_EQ(h1.next_segment_offset, seg3);
	EXPECT_EQ(h3.next_segment_offset, seg4);
	EXPECT_EQ(h4.next_segment_offset, 0);
}

// =========================================================================
// GROUP 2: Compaction & ID Remapping (CRITICAL FOR DATA CORRECTNESS)
// =========================================================================

TEST_F(SpaceManagerTest, Compaction_SparseToDense_IDRemapping) {
	auto type = static_cast<uint32_t>(EntityType::Node);
	uint64_t offset = spaceManager->allocateSegment(type, 10);
	int64_t startId = segmentTracker->getSegmentHeader(offset).start_id;

	// Simulate State: [A, dead, B, dead, C]
	// Indices: 0, 1, 2, 3, 4
	writeNode(offset, 0, startId + 0, "A");
	writeNode(offset, 1, startId + 1, "dead");
	writeNode(offset, 2, startId + 2, "B");
	writeNode(offset, 3, startId + 3, "dead");
	writeNode(offset, 4, startId + 4, "C");

	// Mark 1 and 3 as inactive
	segmentTracker->setEntityActive(offset, 1, false);
	segmentTracker->setEntityActive(offset, 3, false);
	segmentTracker->updateSegmentUsage(offset, 5, 2); // 5 used, 2 inactive

	// Trigger Compaction
	spaceManager->compactSegments(type, 0.0);

	// Verify Result: [A, B, C] packed at 0, 1, 2
	SegmentHeader h = segmentTracker->getSegmentHeader(offset);
	EXPECT_EQ(h.used, 3);
	EXPECT_EQ(h.inactive_count, 0);

	// Check content and IDs
	// Slot 0: "A", ID unchanged (startId + 0)
	Node n0 = segmentTracker->readEntity<Node>(offset, 0, Node::getTotalSize());
	EXPECT_EQ(n0.getLabel(), "A");
	EXPECT_EQ(n0.getId(), startId + 0);

	// Slot 1: "B", ID CHANGED from (startId + 2) -> (startId + 1)
	Node n1 = segmentTracker->readEntity<Node>(offset, 1, Node::getTotalSize());
	EXPECT_EQ(n1.getLabel(), "B");
	EXPECT_EQ(n1.getId(), startId + 1);

	// Slot 2: "C", ID CHANGED from (startId + 4) -> (startId + 2)
	Node n2 = segmentTracker->readEntity<Node>(offset, 2, Node::getTotalSize());
	EXPECT_EQ(n2.getLabel(), "C");
	EXPECT_EQ(n2.getId(), startId + 2);
}

TEST_F(SpaceManagerTest, Compaction_FullSegment_NoOp) {
	auto type = static_cast<uint32_t>(EntityType::Node);
	uint64_t offset = spaceManager->allocateSegment(type, 5);

	// Fill completely
	for (int i = 0; i < 5; i++) {
		writeNode(offset, i, 100 + i, "data");
	}
	segmentTracker->updateSegmentUsage(offset, 5, 0);

	// Compact
	spaceManager->compactSegments(type, 0.0);

	// Should be untouched
	SegmentHeader h = segmentTracker->getSegmentHeader(offset);
	EXPECT_EQ(h.used, 5);
	EXPECT_EQ(h.inactive_count, 0);
}

TEST_F(SpaceManagerTest, Compaction_AllDeleted_SegmentFreed) {
	auto type = static_cast<uint32_t>(EntityType::Node);
	uint64_t offset = spaceManager->allocateSegment(type, 5);

	// Write 2 items, delete 2 items
	writeNode(offset, 0, 100, "del");
	writeNode(offset, 1, 101, "del");
	segmentTracker->setEntityActive(offset, 0, false);
	segmentTracker->setEntityActive(offset, 1, false);
	segmentTracker->updateSegmentUsage(offset, 2, 2); // 2 used, 2 inactive (100% garbage)

	// Compact
	spaceManager->compactSegments(type, 0.0);

	// Segment should be deallocated and in free list
	auto freeList = segmentTracker->getFreeSegments();
	EXPECT_NE(std::ranges::find(freeList, offset), std::ranges::end(freeList));

	// Chain head should be 0 (if it was the only segment)
	EXPECT_EQ(segmentTracker->getChainHead(type), 0);
}

// =========================================================================
// GROUP 3: Segment Merging (CRITICAL FOR SPACE EFFICIENCY)
// =========================================================================

TEST_F(SpaceManagerTest, Merge_CapacityCheck_ShouldNotMerge) {
	auto type = static_cast<uint32_t>(EntityType::Node);
	uint32_t capacity = 10;

	uint64_t segA = spaceManager->allocateSegment(type, capacity);
	uint64_t segB = spaceManager->allocateSegment(type, capacity);

	// Fill A with 9 items (1 slot left)
	for (int i = 0; i < 9; i++)
		writeNode(segA, i, 100 + i, "A");
	segmentTracker->updateSegmentUsage(segA, 9, 0);

	// Fill B with 2 items
	for (int i = 0; i < 2; i++)
		writeNode(segB, i, 200 + i, "B");
	segmentTracker->updateSegmentUsage(segB, 2, 0);

	// Attempt Merge (B needs 2 slots, A only has 1)
	// Threshold 0.9 (very high) to force attempt
	bool result = spaceManager->mergeSegments(type, 0.9);

	// Should FAIL
	EXPECT_FALSE(result);
	EXPECT_EQ(segmentTracker->getSegmentHeader(segA).used, 9);
	EXPECT_EQ(segmentTracker->getSegmentHeader(segB).used, 2);
}

TEST_F(SpaceManagerTest, Merge_EndSegmentIntoFrontSegment) {
	auto type = static_cast<uint32_t>(EntityType::Node);
	uint32_t capacity = 10;

	// Layout: [SegA (Front)] ... [SegB (End)]
	// We create a gap to simulate distance
	uint64_t segA = spaceManager->allocateSegment(type, capacity);
	uint64_t segGap = spaceManager->allocateSegment(type, capacity); // Filler
	uint64_t segB = spaceManager->allocateSegment(type, capacity);

	// SegA: 1 item
	writeNode(segA, 0, 100, "Front");
	segmentTracker->updateSegmentUsage(segA, 1, 0);

	// SegB: 1 item
	writeNode(segB, 0, 200, "End");
	segmentTracker->updateSegmentUsage(segB, 1, 0);

	// Gap is full/busy so it's not a merge candidate
	segmentTracker->updateSegmentUsage(segGap, 10, 0);

	// Merge
	bool result = spaceManager->mergeSegments(type, 0.5);
	ASSERT_TRUE(result);

	// Expectation: SegB merged into SegA. SegB freed.
	SegmentHeader hA = segmentTracker->getSegmentHeader(segA);
	EXPECT_EQ(hA.used, 2);

	// Verify Content in A
	// Slot 0: Front
	// Slot 1: End (Moved from B)
	Node n1 = segmentTracker->readEntity<Node>(segA, 1, Node::getTotalSize());
	EXPECT_EQ(n1.getLabel(), "End");

	// Verify B is freed
	auto freeList = segmentTracker->getFreeSegments();
	EXPECT_NE(std::ranges::find(freeList, segB), std::ranges::end(freeList));
}

TEST_F(SpaceManagerTest, Merge_DifferentTypes_ShouldNotMix) {
	auto typeNode = static_cast<uint32_t>(EntityType::Node);
	auto typeEdge = static_cast<uint32_t>(EntityType::Edge);

	uint64_t segNode = spaceManager->allocateSegment(typeNode, 10);
	uint64_t segEdge = spaceManager->allocateSegment(typeEdge, 10);

	writeNode(segNode, 0, 1, "N");
	segmentTracker->updateSegmentUsage(segNode, 1, 0);

	// Mock writing edge (using node write helper but tracking it as edge segment)
	// Note: In real engine types are distinct, here we rely on data_type check.
	segmentTracker->updateSegmentUsage(segEdge, 1, 0);

	// Try merging Nodes. Edge segment should NOT be involved.
	bool result = spaceManager->mergeSegments(typeNode, 0.5);

	// Should be false because only 1 node segment exists.
	EXPECT_FALSE(result);
}

// =========================================================================
// GROUP 4: Move Segment (CRITICAL FOR DEFRAGMENTATION)
// =========================================================================

TEST_F(SpaceManagerTest, MoveSegment_UpdatesNeighborsLinks) {
	auto type = static_cast<uint32_t>(EntityType::Node);

	// Chain: A -> B -> C
	uint64_t segA = spaceManager->allocateSegment(type, 10);
	uint64_t segB = spaceManager->allocateSegment(type, 10);
	uint64_t segC = spaceManager->allocateSegment(type, 10);

	// Alloc a destination slot (e.g. by freeing an earlier segment)
	// We force a move to a specific arbitrary free offset for testing manually
	// But `moveSegment` requires the dest to be valid free space or handled safely.
	// Easier way: Allocate D, Free D, use D as target.
	uint64_t segD = spaceManager->allocateSegment(type, 10); // Offset 4
	spaceManager->deallocateSegment(segD); // D is free now

	// MOVE Middle Segment (B) to D's location
	// Old chain: A -> B -> C
	// New chain: A -> D(content of B) -> C
	bool moved = spaceManager->moveSegment(segB, segD);
	ASSERT_TRUE(moved);

	// Check A's next
	SegmentHeader hA = segmentTracker->getSegmentHeader(segA);
	EXPECT_EQ(hA.next_segment_offset, segD);

	// Check C's prev
	SegmentHeader hC = segmentTracker->getSegmentHeader(segC);
	EXPECT_EQ(hC.prev_segment_offset, segD);

	// Check D's links
	SegmentHeader hD = segmentTracker->getSegmentHeader(segD);
	EXPECT_EQ(hD.prev_segment_offset, segA);
	EXPECT_EQ(hD.next_segment_offset, segC);
	EXPECT_EQ(hD.data_type, type);

	// Check B is freed
	auto freeList = segmentTracker->getFreeSegments();
	EXPECT_NE(std::ranges::find(freeList, segB), std::ranges::end(freeList));
}

TEST_F(SpaceManagerTest, MoveSegment_UpdatesHeadReference) {
	auto type = static_cast<uint32_t>(EntityType::Node);

	// Chain: A -> B
	uint64_t segA = spaceManager->allocateSegment(type, 10);
	uint64_t segB = spaceManager->allocateSegment(type, 10);

	// Target: C (Free)
	uint64_t segC = spaceManager->allocateSegment(type, 10);
	spaceManager->deallocateSegment(segC);

	// Move Head (A) to C
	bool moved = spaceManager->moveSegment(segA, segC);
	ASSERT_TRUE(moved);

	// Registry should point to C
	EXPECT_EQ(segmentTracker->getChainHead(type), segC);

	// B should point back to C
	SegmentHeader hB = segmentTracker->getSegmentHeader(segB);
	EXPECT_EQ(hB.prev_segment_offset, segC);
}

// =========================================================================
// GROUP 5: File Truncation (CRITICAL FOR DISK USAGE)
// =========================================================================

TEST_F(SpaceManagerTest, Truncate_PartialTail) {
	auto type = static_cast<uint32_t>(EntityType::Node);

	uint64_t initialSize = getFileSize();

	// Alloc A, B, C
	uint64_t segA = spaceManager->allocateSegment(type, 10);
	uint64_t segB = spaceManager->allocateSegment(type, 10);
	uint64_t segC = spaceManager->allocateSegment(type, 10);

	// Free C and B (Tail segments)
	spaceManager->deallocateSegment(segC);
	spaceManager->deallocateSegment(segB);

	// Truncate
	(void) spaceManager->truncateFile();

	// Should align to end of A
	// A starts at FILE_HEADER_SIZE.
	// Size = Header + SegA size.
	uint64_t expectedSize = FILE_HEADER_SIZE + TOTAL_SEGMENT_SIZE;

	// Allow slight OS alignment variance, but usually exact in this mock
	EXPECT_EQ(getFileSize(), expectedSize);

	// SegA should still be valid
	SegmentHeader hA = segmentTracker->getSegmentHeader(segA);
	EXPECT_EQ(hA.data_type, type);
}

TEST_F(SpaceManagerTest, Truncate_BlockedByActiveSegment) {
	auto type = static_cast<uint32_t>(EntityType::Node);

	// Alloc A (Active), B (Active), C (Free)
	uint64_t segA = spaceManager->allocateSegment(type, 10);
	uint64_t segB = spaceManager->allocateSegment(type, 10);
	uint64_t segC = spaceManager->allocateSegment(type, 10);

	spaceManager->deallocateSegment(segC); // Free last one

	// Truncate
	(void) spaceManager->truncateFile();

	// Should stop at B
	uint64_t expectedSize = FILE_HEADER_SIZE + (TOTAL_SEGMENT_SIZE * 2);
	EXPECT_EQ(getFileSize(), expectedSize);
}
