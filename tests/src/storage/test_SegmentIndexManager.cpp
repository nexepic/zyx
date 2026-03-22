/**
 * @file test_SegmentIndexManager.cpp
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

#include <algorithm>
#include <cstring> // For memset
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <vector>

#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/SegmentTypeRegistry.hpp"
#include "graph/storage/StorageHeaders.hpp"

using namespace graph::storage;

namespace {
	// Helper to calculate offset similar to previous tests
	uint64_t getSegOffset(uint32_t index) {
		return FILE_HEADER_SIZE + (static_cast<uint64_t>(index) * TOTAL_SEGMENT_SIZE);
	}
} // namespace

class SegmentIndexManagerTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> fileStream;
	std::shared_ptr<SegmentTracker> tracker;
	std::shared_ptr<SegmentIndexManager> indexManager;

	// Head offsets (mocked for initialization)
	uint64_t nodeHead = 0;
	uint64_t edgeHead = 0;
	uint64_t propHead = 0;
	uint64_t blobHead = 0;
	uint64_t idxHead = 0;
	uint64_t stateHead = 0;

	void SetUp() override {
		// 1. Setup temp file
		auto tempDir = std::filesystem::temp_directory_path();
		std::random_device rd;
		std::mt19937 gen(rd());
		testFilePath = tempDir / ("segment_index_test_" + std::to_string(gen()) + ".dat");

		{
			std::ofstream out(testFilePath, std::ios::binary);
			FileHeader header;
			out.write(reinterpret_cast<const char *>(&header), sizeof(FileHeader));
			// Pre-allocate space for 10 segments
			std::vector<char> zeros(TOTAL_SEGMENT_SIZE, 0);
			for (int i = 0; i < 10; ++i)
				out.write(zeros.data(), TOTAL_SEGMENT_SIZE);
		}

		fileStream = std::make_shared<std::fstream>(testFilePath, std::ios::binary | std::ios::in | std::ios::out);

		FileHeader header; // Empty header
		// 2. Init Tracker
		tracker = std::make_shared<SegmentTracker>(fileStream, header);

		// 3. Init IndexManager
		indexManager = std::make_shared<SegmentIndexManager>(tracker);
		// We pass references to our member variables so we can modify them if needed
		// (though Manager only reads them during re-init/build)
		indexManager->initialize(nodeHead, edgeHead, propHead, blobHead, idxHead, stateHead);

		// Link tracker back to manager to simulate real usage
		tracker->setSegmentIndexManager(indexManager);
	}

	void TearDown() override {
		tracker.reset();
		indexManager.reset();
		fileStream->close();
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	// Helper to create a dummy header for direct API testing
	// MANUAL INITIALIZATION as requested
	static SegmentHeader createHeader(uint64_t offset, uint32_t type, int64_t startId, uint32_t used) {
		SegmentHeader h;
		// Basic fields
		h.file_offset = offset;
		h.data_type = type;
		h.start_id = startId;
		h.used = used;
		h.capacity = 100; // Mock capacity

		// Default initializations to ensure clean state
		h.inactive_count = 0;
		h.next_segment_offset = 0;
		h.prev_segment_offset = 0;
		h.needs_compaction = 0;
		h.is_dirty = 0;
		h.bitmap_size = bitmap::calculateBitmapSize(h.capacity);
		std::memset(h.activity_bitmap, 0, sizeof(h.activity_bitmap));

		return h;
	}
};

// ============================================================================
// 1. Initialization & Empty State
// ============================================================================

TEST_F(SegmentIndexManagerTest, StartsEmpty) {
	// Verify internal vectors are empty (indirectly via public getters)
	EXPECT_TRUE(indexManager->getNodeSegmentIndex().empty());
	EXPECT_TRUE(indexManager->getEdgeSegmentIndex().empty());

	// Verify lookup returns 0
	EXPECT_EQ(indexManager->findSegmentForId(static_cast<uint32_t>(graph::EntityType::Node), 100), 0ULL);
}

// ============================================================================
// 2. Direct Index Manipulation (Unit Testing internal logic)
// ============================================================================

TEST_F(SegmentIndexManagerTest, InsertAndFindSingleSegment) {
	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	uint64_t offset = getSegOffset(0);

	// Create a header: IDs [100, 109] (used=10)
	SegmentHeader header = createHeader(offset, type, 100, 10);

	// Action: Update Index (Insert)
	// -1 indicates no old start ID (new insertion)
	indexManager->updateSegmentIndex(header, -1);

	// Verify internal state
	const auto &idx = indexManager->getNodeSegmentIndex();
	ASSERT_EQ(idx.size(), 1UL);
	EXPECT_EQ(idx[0].startId, 100);
	EXPECT_EQ(idx[0].endId, 109);
	EXPECT_EQ(idx[0].segmentOffset, offset);

	// Verify Lookup
	EXPECT_EQ(indexManager->findSegmentForId(type, 100), offset); // Start
	EXPECT_EQ(indexManager->findSegmentForId(type, 105), offset); // Middle
	EXPECT_EQ(indexManager->findSegmentForId(type, 109), offset); // End
	EXPECT_EQ(indexManager->findSegmentForId(type, 99), 0ULL); // Before
	EXPECT_EQ(indexManager->findSegmentForId(type, 110), 0ULL); // After
}

TEST_F(SegmentIndexManagerTest, MaintainSortedOrder) {
	auto type = static_cast<uint32_t>(graph::EntityType::Node);

	// Insert [200, 209] first
	indexManager->updateSegmentIndex(createHeader(getSegOffset(1), type, 200, 10), -1);

	// Insert [0, 9] (Should go to front)
	indexManager->updateSegmentIndex(createHeader(getSegOffset(0), type, 0, 10), -1);

	// Insert [100, 109] (Should go to middle)
	indexManager->updateSegmentIndex(createHeader(getSegOffset(2), type, 100, 10), -1);

	const auto &idx = indexManager->getNodeSegmentIndex();
	ASSERT_EQ(idx.size(), 3UL);

	// Check ordering
	EXPECT_EQ(idx[0].startId, 0);
	EXPECT_EQ(idx[1].startId, 100);
	EXPECT_EQ(idx[2].startId, 200);

	// Verify lookup still works
	EXPECT_EQ(indexManager->findSegmentForId(type, 5), getSegOffset(0));
	EXPECT_EQ(indexManager->findSegmentForId(type, 105), getSegOffset(2));
	EXPECT_EQ(indexManager->findSegmentForId(type, 205), getSegOffset(1));
}

TEST_F(SegmentIndexManagerTest, UpdateExistingSegmentExtent) {
	auto type = static_cast<uint32_t>(graph::EntityType::Edge);
	uint64_t offset = getSegOffset(0);

	// 1. Initial Insert [100, 100] (Used = 1)
	SegmentHeader h = createHeader(offset, type, 100, 1);
	indexManager->updateSegmentIndex(h, -1);

	EXPECT_EQ(indexManager->findSegmentForId(type, 100), offset);
	EXPECT_EQ(indexManager->findSegmentForId(type, 101), 0ULL); // Not used yet

	// 2. Update Used count (Simulate adding data) -> [100, 109]
	h.used = 10;
	// Calling update with same startId
	indexManager->updateSegmentIndex(h, 100);

	// Verify new range
	EXPECT_EQ(indexManager->findSegmentForId(type, 109), offset);

	// Internal Check
	const auto &idx = indexManager->getEdgeSegmentIndex();
	ASSERT_EQ(idx.size(), 1UL);
	EXPECT_EQ(idx[0].endId, 109);
}

// ============================================================================
// 3. Complex Updates: Key Change (Move)
// ============================================================================

TEST_F(SegmentIndexManagerTest, UpdateStartIdMovesIndex) {
	// This tests the case where a segment's start_id is modified.
	// The Manager must remove the old entry and insert the new one to maintain sort order.

	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	uint64_t off1 = getSegOffset(0); // Fixed ID [0, 9]
	uint64_t off2 = getSegOffset(1); // Moving ID [200, 209] -> [100, 109]

	// Setup initial state
	indexManager->updateSegmentIndex(createHeader(off1, type, 0, 10), -1);
	indexManager->updateSegmentIndex(createHeader(off2, type, 200, 10), -1);

	// Verify Initial Order
	ASSERT_EQ(indexManager->getNodeSegmentIndex().size(), 2UL);
	EXPECT_EQ(indexManager->getNodeSegmentIndex()[1].startId, 200);

	// Action: Move off2 from 200 to 100
	SegmentHeader newH = createHeader(off2, type, 100, 10);
	// Crucial: Pass oldStartId = 200 so manager knows what to remove
	indexManager->updateSegmentIndex(newH, 200);

	// Verify
	const auto &list = indexManager->getNodeSegmentIndex();
	ASSERT_EQ(list.size(), 2UL);

	// Check Sort Order: [0,9], [100,109]
	EXPECT_EQ(list[0].segmentOffset, off1);
	EXPECT_EQ(list[0].startId, 0);

	EXPECT_EQ(list[1].segmentOffset, off2);
	EXPECT_EQ(list[1].startId, 100); // Changed
	EXPECT_EQ(list[1].endId, 109);

	// Verify Lookups
	EXPECT_EQ(indexManager->findSegmentForId(type, 205), 0ULL); // Old location empty
	EXPECT_EQ(indexManager->findSegmentForId(type, 105), off2); // New location found
}

// ============================================================================
// 4. Removal Logic
// ============================================================================

TEST_F(SegmentIndexManagerTest, RemoveSegmentIndex) {
	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	uint64_t offset = getSegOffset(0);

	// Insert
	SegmentHeader h = createHeader(offset, type, 500, 10);
	indexManager->updateSegmentIndex(h, -1);
	EXPECT_EQ(indexManager->findSegmentForId(type, 505), offset);

	// Remove
	indexManager->removeSegmentIndex(h);

	// Verify
	EXPECT_EQ(indexManager->getNodeSegmentIndex().size(), 0UL);
	EXPECT_EQ(indexManager->findSegmentForId(type, 505), 0ULL);
}

// ============================================================================
// 5. Full Rebuild (Integration with Tracker)
// ============================================================================

TEST_F(SegmentIndexManagerTest, BuildSegmentIndexesFromChain) {
	// This tests `buildSegmentIndexes` which walks the linked list in the tracker.

	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	uint64_t head = getSegOffset(0);
	uint64_t next = getSegOffset(1);

	// --- FIX: Manual SegmentHeader Initialization ---
	// Instead of using convenience constructor, we build and register

	// Register Head Segment
	{
		SegmentHeader h;
		h.file_offset = head;
		h.data_type = type;
		h.capacity = 100;
		h.start_id = 0; // [0, 49]
		h.used = 50;
		h.next_segment_offset = next;
		h.prev_segment_offset = 0;
		h.inactive_count = 0;
		h.needs_compaction = 0;
		h.is_dirty = 0;
		h.bitmap_size = bitmap::calculateBitmapSize(h.capacity);
		std::memset(h.activity_bitmap, 0, sizeof(h.activity_bitmap));

		// Register to tracker (tracker will copy it)
		tracker->registerSegment(h);
	}

	// Register Next Segment
	{
		SegmentHeader h;
		h.file_offset = next;
		h.data_type = type;
		h.capacity = 100;
		h.start_id = 100; // [100, 119]
		h.used = 20;
		h.next_segment_offset = 0;
		h.prev_segment_offset = head;
		h.inactive_count = 0;
		h.needs_compaction = 0;
		h.is_dirty = 0;
		h.bitmap_size = bitmap::calculateBitmapSize(h.capacity);
		std::memset(h.activity_bitmap, 0, sizeof(h.activity_bitmap));

		// Register to tracker
		tracker->registerSegment(h);
	}

	// 3. Update the tracked head pointer in our test fixture
	// Note: indexManager->initialize captured the pointers to these variables.
	nodeHead = head;

	// 4. Force Rebuild
	// The previous updates might have triggered updateSegmentIndex incrementally if wired,
	// but here we want to test the full scan logic.
	// Calling buildSegmentIndexes() clears vectors first internally.
	indexManager->buildSegmentIndexes();

	// 5. Verify Results
	const auto &list = indexManager->getNodeSegmentIndex();
	ASSERT_EQ(list.size(), 2UL);

	// Check Segment 1
	EXPECT_EQ(list[0].startId, 0);
	EXPECT_EQ(list[0].endId, 49);
	EXPECT_EQ(list[0].segmentOffset, head);

	// Check Segment 2
	EXPECT_EQ(list[1].startId, 100);
	EXPECT_EQ(list[1].endId, 119);
	EXPECT_EQ(list[1].segmentOffset, next);
}

// ============================================================================
// 6. Edge Cases & Gaps
// ============================================================================

TEST_F(SegmentIndexManagerTest, LookupWithGaps) {
	auto type = static_cast<uint32_t>(graph::EntityType::Node);

	// Seg 1: [0, 9]
	indexManager->updateSegmentIndex(createHeader(getSegOffset(0), type, 0, 10), -1);

	// Seg 2: [20, 29] (Gap 10-19)
	indexManager->updateSegmentIndex(createHeader(getSegOffset(1), type, 20, 10), -1);

	EXPECT_EQ(indexManager->findSegmentForId(type, 5), getSegOffset(0));
	EXPECT_EQ(indexManager->findSegmentForId(type, 25), getSegOffset(1));

	// Gap check
	EXPECT_EQ(indexManager->findSegmentForId(type, 15), 0ULL);

	// End check
	EXPECT_EQ(indexManager->findSegmentForId(type, 30), 0ULL);
}

TEST_F(SegmentIndexManagerTest, InvalidTypeThrows) {
	// Calling internal helpers with invalid type should ideally throw or handle gracefully.
	// findSegmentForId verifies type implicitly via switch case.

	// Assuming toUnderlying is working, casting an arbitrary int to type might hit default case
	EXPECT_THROW({ indexManager->findSegmentForId(999, 100); }, std::invalid_argument);
}

// ============================================================================
// 7. Multi-Type Isolation
// ============================================================================

TEST_F(SegmentIndexManagerTest, TypesAreIsolated) {
	uint64_t offNode = getSegOffset(0);
	uint64_t offEdge = getSegOffset(1);

	// Node ID 100
	indexManager->updateSegmentIndex(createHeader(offNode, static_cast<uint32_t>(graph::EntityType::Node), 100, 10),
									 -1);

	// Edge ID 100
	indexManager->updateSegmentIndex(createHeader(offEdge, static_cast<uint32_t>(graph::EntityType::Edge), 100, 10),
									 -1);

	// Finding Node 100 should return Node Segment
	EXPECT_EQ(indexManager->findSegmentForId(static_cast<uint32_t>(graph::EntityType::Node), 100), offNode);

	// Finding Edge 100 should return Edge Segment
	EXPECT_EQ(indexManager->findSegmentForId(static_cast<uint32_t>(graph::EntityType::Edge), 100), offEdge);
}

// ============================================================================
// 8. Performance & Optimization Logic (Append Strategy)
// ============================================================================

TEST_F(SegmentIndexManagerTest, SequentialAppendWorksCorrectly) {
	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	size_t count = 1000;

	// 1. Simulate bulk creation of sequential segments
	// Pattern:
	// Segment i starts at ID (i * 100)
	// Segment i has 'used' count of 50
	// Valid Range for Segment i: [i*100, i*100 + 49]
	// Gap (Unallocated) for Segment i: [i*100 + 50, i*100 + 99]
	for (size_t i = 0; i < count; ++i) {
		uint64_t offset = getSegOffset(static_cast<uint32_t>(i));
		int64_t startId = static_cast<int64_t>(i * 100);

		// Note: passing -1 indicates a NEW insertion, triggering the O(1) optimization path
		indexManager->updateSegmentIndex(createHeader(offset, type, startId, 50), -1);
	}

	const auto &idx = indexManager->getNodeSegmentIndex();
	ASSERT_EQ(idx.size(), count);

	// 2. Verify Boundaries
	EXPECT_EQ(idx.front().startId, 0);
	EXPECT_EQ(idx.back().startId, 99900);

	// 3. Verify Logic: Valid Lookup in the Middle
	// We check Segment 500.
	// Start ID: 50000. Used: 50. Valid IDs: [50000, 50049].
	// Target ID: 50025.
	EXPECT_EQ(indexManager->findSegmentForId(type, 50025), getSegOffset(500));

	// 4. Verify Logic: Boundary Lookup
	// The last valid ID of Segment 500 is 50049.
	EXPECT_EQ(indexManager->findSegmentForId(type, 50049), getSegOffset(500));

	// 5. Verify Logic: Gap Lookup
	// ID 50050 falls into the unallocated gap between Segment 500 and Segment 501.
	// It should NOT find a segment.
	EXPECT_EQ(indexManager->findSegmentForId(type, 50050), 0ULL);
}

// ============================================================================
// 9. Robustness & Edge Cases
// ============================================================================

TEST_F(SegmentIndexManagerTest, IdempotencyOnDuplicateInsert) {
	// BUG PREVENTER: Verify that adding the exact same segment twice doesn't create duplicates
	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	uint64_t offset = getSegOffset(0);
	SegmentHeader h = createHeader(offset, type, 100, 10);

	// First Insert
	indexManager->updateSegmentIndex(h, -1);
	ASSERT_EQ(indexManager->getNodeSegmentIndex().size(), 1UL);

	// Second Insert (Simulate accidental double-call or recovery logic)
	// Even with -1 (new), it should detect existing StartID and update/ignore instead of duplicate
	indexManager->updateSegmentIndex(h, -1);

	// Should still be 1
	const auto &idx = indexManager->getNodeSegmentIndex();
	ASSERT_EQ(idx.size(), 1UL) << "Index manager should prevent duplicate entries for same Start ID";
	EXPECT_EQ(idx[0].segmentOffset, offset);
}

TEST_F(SegmentIndexManagerTest, UpdateReplacesOldEntryWaitSameStartID) {
	// Case: offset changes but StartID stays same (e.g. file defragmentation/move without ID change)
	// Though rare, if StartID is same, we treat it as the "Same Logical Segment"

	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	int64_t startId = 100;

	// Old location
	indexManager->updateSegmentIndex(createHeader(getSegOffset(0), type, startId, 10), -1);

	// New location, same Start ID
	// Note: passing -1 means we might treat it as new, but since StartID exists, it should update
	indexManager->updateSegmentIndex(createHeader(getSegOffset(1), type, startId, 20), -1);

	const auto &idx = indexManager->getNodeSegmentIndex();
	ASSERT_EQ(idx.size(), 1UL);
	EXPECT_EQ(idx[0].segmentOffset, getSegOffset(1)); // Should point to new offset
	EXPECT_EQ(idx[0].endId, 119); // Should reflect new usage
}

TEST_F(SegmentIndexManagerTest, ZeroUsageHandling) {
	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	uint64_t offset = getSegOffset(0);

	// Create segment with USED = 0
	// Logic: endId = start + (0 > 0 ? -1 : 0) = start
	indexManager->updateSegmentIndex(createHeader(offset, type, 100, 0), -1);

	// It should effectively cover the range [100, 100] in the current logic
	// (or just be a placeholder)
	EXPECT_EQ(indexManager->findSegmentForId(type, 100), offset);

	// Update to Used = 1
	SegmentHeader h = createHeader(offset, type, 100, 1);
	indexManager->updateSegmentIndex(h, 100);
	EXPECT_EQ(indexManager->findSegmentForId(type, 100), offset);
}

// ============================================================================
// 10. Supplementary Coverage (Edge Logic & Switch Cases)
// ============================================================================

TEST_F(SegmentIndexManagerTest, LookupInEmptyManagerReturnsZero) {
	// Explicitly test find on an empty vector
	// (SetUp leaves vectors empty initially)
	EXPECT_EQ(indexManager->findSegmentForId(static_cast<uint32_t>(graph::EntityType::Node), 0), 0ULL);
	EXPECT_EQ(indexManager->findSegmentForId(static_cast<uint32_t>(graph::EntityType::Node), 9999), 0ULL);
}

TEST_F(SegmentIndexManagerTest, UpdateWithStaleOldIdTriggersFullScanRemoval) {
	// Covers the `erase_if` path in updateSegmentIndex.
	// Scenario: The tracker thinks the segment was at 'oldStartId', but the IndexManager
	// doesn't have a record of 'oldStartId' pointing to that offset (state mismatch).
	// The manager should fall back to scanning by offset to ensure the old entry is removed.

	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	uint64_t offset = getSegOffset(0);

	// 1. Initial State: Segment at ID 100
	indexManager->updateSegmentIndex(createHeader(offset, type, 100, 10), -1);
	ASSERT_EQ(indexManager->findSegmentForId(type, 100), offset);

	// 2. Update: Change ID to 200.
	// BUT pass a wrong 'oldStartId' (e.g., 999) that doesn't exist.
	// Logic expected:
	// - Look for 999 -> Not found.
	// - Fallback: Search entire vector for 'offset', remove it (removes 100).
	// - Insert 200.
	SegmentHeader newHeader = createHeader(offset, type, 200, 10);
	indexManager->updateSegmentIndex(newHeader, 999);

	const auto &idx = indexManager->getNodeSegmentIndex();
	ASSERT_EQ(idx.size(), 1UL);
	EXPECT_EQ(idx[0].startId, 200); // 100 should be gone, 200 added
	EXPECT_EQ(idx[0].segmentOffset, offset);
}

TEST_F(SegmentIndexManagerTest, RemoveSegmentIgnoredIfOffsetMismatch) {
	// Covers `removeSegmentIndex` safety check.
	// Scenario: Trying to remove ID 100, but the provided header has a different file offset
	// than what is stored in the index. Should NOT remove the entry.

	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	uint64_t correctOffset = getSegOffset(0);
	uint64_t wrongOffset = getSegOffset(1);

	// Insert [100, 109] at correctOffset
	indexManager->updateSegmentIndex(createHeader(correctOffset, type, 100, 10), -1);

	// Attempt remove [100, 109] but claiming it is at wrongOffset
	indexManager->removeSegmentIndex(createHeader(wrongOffset, type, 100, 10));

	// Verify entry still exists
	EXPECT_EQ(indexManager->getNodeSegmentIndex().size(), 1UL);
	EXPECT_EQ(indexManager->findSegmentForId(type, 100), correctOffset);
}

TEST_F(SegmentIndexManagerTest, RemoveNonExistentSegmentIsSafe) {
	// Covers `removeSegmentIndex` when ID doesn't exist.
	auto type = static_cast<uint32_t>(graph::EntityType::Node);

	// Attempt remove something from empty index
	indexManager->removeSegmentIndex(createHeader(getSegOffset(0), type, 100, 10));
	EXPECT_TRUE(indexManager->getNodeSegmentIndex().empty());

	// Insert valid one
	indexManager->updateSegmentIndex(createHeader(getSegOffset(0), type, 100, 10), -1);

	// Attempt remove non-existent ID
	indexManager->removeSegmentIndex(createHeader(getSegOffset(1), type, 500, 10));
	EXPECT_EQ(indexManager->getNodeSegmentIndex().size(), 1UL);
}

TEST_F(SegmentIndexManagerTest, AllSegmentTypesSupported) {
	// Covers the switch cases in getSegmentIndexForType for types not covered by previous tests
	// (Blob, Index, State).
	uint64_t offset = getSegOffset(0);

	// Blob
	auto typeBlob = static_cast<uint32_t>(graph::EntityType::Blob);
	indexManager->updateSegmentIndex(createHeader(offset, typeBlob, 10, 1), -1);
	EXPECT_EQ(indexManager->getBlobSegmentIndex().size(), 1UL);
	EXPECT_EQ(indexManager->findSegmentForId(typeBlob, 10), offset);

	// Index
	auto typeIndex = static_cast<uint32_t>(graph::EntityType::Index);
	indexManager->updateSegmentIndex(createHeader(offset, typeIndex, 20, 1), -1);
	EXPECT_EQ(indexManager->getIndexSegmentIndex().size(), 1UL);
	EXPECT_EQ(indexManager->findSegmentForId(typeIndex, 20), offset);

	// State
	auto typeState = static_cast<uint32_t>(graph::EntityType::State);
	indexManager->updateSegmentIndex(createHeader(offset, typeState, 30, 1), -1);
	EXPECT_EQ(indexManager->getStateSegmentIndex().size(), 1UL);
	EXPECT_EQ(indexManager->findSegmentForId(typeState, 30), offset);
}

// ============================================================================
// 11. Additional Branch Coverage Tests
// ============================================================================

TEST_F(SegmentIndexManagerTest, UpdateSegmentIndex_NewInsertionNotAppending) {
	// Test new insertion (oldStartId == -1) that is NOT appending to end
	// (line 108 false branch: header.start_id <= segmentIndex.back().startId)
	auto type = static_cast<uint32_t>(graph::EntityType::Node);

	// Insert a segment with high start ID first
	indexManager->updateSegmentIndex(createHeader(getSegOffset(0), type, 1000, 10), -1);

	// Now insert with lower start ID (oldStartId == -1 but not appending)
	// This falls through to the generic insertion path
	indexManager->updateSegmentIndex(createHeader(getSegOffset(1), type, 500, 10), -1);

	const auto &idx = indexManager->getNodeSegmentIndex();
	ASSERT_EQ(idx.size(), 2UL);
	// Should be sorted: 500 first, then 1000
	EXPECT_EQ(idx[0].startId, 500);
	EXPECT_EQ(idx[1].startId, 1000);
}

TEST_F(SegmentIndexManagerTest, UpdateSegmentIndex_KeyChangedWithCorrectOldEntry) {
	// Test key change where old entry is found via binary search (line 131-133)
	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	uint64_t offset = getSegOffset(0);

	// Insert at start ID 100
	indexManager->updateSegmentIndex(createHeader(offset, type, 100, 10), -1);
	EXPECT_EQ(indexManager->findSegmentForId(type, 105), offset);

	// Change key from 100 to 200 - old entry should be found and removed
	SegmentHeader newH = createHeader(offset, type, 200, 10);
	indexManager->updateSegmentIndex(newH, 100);

	const auto &idx = indexManager->getNodeSegmentIndex();
	ASSERT_EQ(idx.size(), 1UL);
	EXPECT_EQ(idx[0].startId, 200);
	EXPECT_EQ(indexManager->findSegmentForId(type, 100), 0ULL); // Old gone
	EXPECT_EQ(indexManager->findSegmentForId(type, 205), offset); // New found
}

TEST_F(SegmentIndexManagerTest, FindSegmentForId_NotInRange) {
	// Test findSegmentForId where lower_bound finds an entry but ID is NOT in its range
	// (line 89 false branch: id < it->startId)
	auto type = static_cast<uint32_t>(graph::EntityType::Node);

	// Insert segment [100, 109]
	indexManager->updateSegmentIndex(createHeader(getSegOffset(0), type, 100, 10), -1);

	// Search for ID 50 - lower_bound finds [100,109] but 50 < 100
	EXPECT_EQ(indexManager->findSegmentForId(type, 50), 0ULL);
}

TEST_F(SegmentIndexManagerTest, UpdateSegmentIndex_InPlaceUpdate) {
	// Test in-place update (scenario 3) when startId matches existing entry
	auto type = static_cast<uint32_t>(graph::EntityType::Edge);
	uint64_t offset = getSegOffset(0);

	// Insert [100, 100] (used=1)
	SegmentHeader h = createHeader(offset, type, 100, 1);
	indexManager->updateSegmentIndex(h, -1);

	// Update same startId but different used count
	// This should hit the in-place update path (line 153)
	h.used = 50;
	indexManager->updateSegmentIndex(h, 100); // oldStartId == current startId

	const auto &idx = indexManager->getEdgeSegmentIndex();
	ASSERT_EQ(idx.size(), 1UL);
	EXPECT_EQ(idx[0].endId, 149); // Updated
}

TEST_F(SegmentIndexManagerTest, BuildSegmentIndex_WithZeroUsed) {
	// Test buildSegmentIndex with a segment where used == 0 (line 71)
	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	uint64_t offset = getSegOffset(0);

	// Register a segment with used = 0
	SegmentHeader h;
	h.file_offset = offset;
	h.data_type = type;
	h.capacity = 100;
	h.start_id = 100;
	h.used = 0; // Zero used
	h.next_segment_offset = 0;
	h.prev_segment_offset = 0;
	h.inactive_count = 0;
	h.needs_compaction = 0;
	h.is_dirty = 0;
	h.bitmap_size = bitmap::calculateBitmapSize(h.capacity);
	std::memset(h.activity_bitmap, 0, sizeof(h.activity_bitmap));
	tracker->registerSegment(h);

	// Set chain head and rebuild
	nodeHead = offset;
	indexManager->buildSegmentIndexes();

	const auto &idx = indexManager->getNodeSegmentIndex();
	ASSERT_EQ(idx.size(), 1UL);
	// endId should be start_id + 0 = 100 (since used == 0, endId = startId + 0)
	EXPECT_EQ(idx[0].startId, 100);
	EXPECT_EQ(idx[0].endId, 100);
}

TEST_F(SegmentIndexManagerTest, BuildSegmentIndexes_NullHeadPointers) {
	// Test buildSegmentIndexes when head pointers are null (line 54-59 ternary)
	// Reset the index manager without calling initialize (head pointers are null)
	auto newIndexManager = std::make_shared<SegmentIndexManager>(tracker);

	// buildSegmentIndexes should use 0 for null head pointers
	EXPECT_NO_THROW(newIndexManager->buildSegmentIndexes());

	// All indexes should be empty
	EXPECT_TRUE(newIndexManager->getNodeSegmentIndex().empty());
	EXPECT_TRUE(newIndexManager->getEdgeSegmentIndex().empty());
}

TEST_F(SegmentIndexManagerTest, PropertySegmentIndex) {
	// Test Property type specifically for getSegmentIndexForType switch case
	auto type = static_cast<uint32_t>(graph::EntityType::Property);
	uint64_t offset = getSegOffset(0);

	indexManager->updateSegmentIndex(createHeader(offset, type, 50, 5), -1);
	EXPECT_EQ(indexManager->getPropertySegmentIndex().size(), 1UL);
	EXPECT_EQ(indexManager->findSegmentForId(type, 52), offset);
}

TEST_F(SegmentIndexManagerTest, RemoveSegmentIndex_MatchesByStartIdButNotOffset) {
	// Test removeSegmentIndex when startId matches but offset doesn't (line 183)
	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	uint64_t correctOffset = getSegOffset(0);
	uint64_t wrongOffset = getSegOffset(1);

	// Insert at correctOffset
	indexManager->updateSegmentIndex(createHeader(correctOffset, type, 100, 10), -1);

	// Try to remove with matching startId but wrong offset
	SegmentHeader remHeader = createHeader(wrongOffset, type, 100, 10);
	indexManager->removeSegmentIndex(remHeader);

	// Should NOT have removed
	EXPECT_EQ(indexManager->getNodeSegmentIndex().size(), 1UL);
}

// ============================================================================
// 12. Additional Branch Coverage Tests
// ============================================================================

TEST_F(SegmentIndexManagerTest, FindSegmentForId_LowerBoundFindsButIdBeyondEndId) {
	// Covers: line 89 - id <= it->endId False branch
	// lower_bound finds a segment whose endId >= id (by design), but then
	// id < it->startId makes the id NOT in range.
	// Actually the condition is: id >= it->startId && id <= it->endId
	// We need lower_bound to find an entry but id > endId or id < startId.
	//
	// Scenario: two segments [0,9] and [100,109] with gap [10,99].
	// Search for id=50: lower_bound on endId finds [100,109] (since 109 >= 50).
	// Then: 50 >= 100 is false -> return 0. This covers id < startId.
	//
	// For id > endId: Segment [0,9], search for id=15.
	// lower_bound finds end (no entry with endId >= 15 if only [0,9]).
	// Actually lower_bound(15) on [endId=9] -> 9 < 15 so it skips, returns end.
	// That hits it == segmentIndex.end() -> return 0.
	//
	// For the exact branch "id <= it->endId" False:
	// We need lower_bound to return a valid iterator but id > it->endId.
	// lower_bound finds first entry where endId >= id. So endId >= id is always true.
	// Unless... segments have overlapping ranges or adjacent. Let me think.
	//
	// Actually: lower_bound uses endId < value, so it finds first entry where
	// !(endId < id), i.e., endId >= id. So if found, endId >= id is guaranteed.
	// Then the condition id >= startId && id <= endId checks both bounds.
	// Since endId >= id is guaranteed by lower_bound, the only way "id <= endId"
	// can be false is if it == end(). But that's a different branch.
	//
	// Wait - looking more carefully at the code, let me re-read:
	// lower_bound with comparator: index.endId < value
	// This finds first entry where !(endId < id), i.e., endId >= id.
	// So when found: endId >= id.
	// Then: id >= startId && id <= endId.
	// Since endId >= id (from lower_bound), the check "id <= endId" is always true
	// when iterator is valid. So the False branch for "id <= endId" only occurs
	// when it == end(), which is already the other branch.
	//
	// The uncovered branch must be from the combined condition.
	// Actually the coverage shows:
	// Branch (89:7): [True: 10.0k, False: 180k] - it != end()
	// Branch (89:35): [True: 10.0k, False: 21] - id >= startId
	// Branch (89:56): [True: 10.0k, False: 0] - id <= endId
	//
	// So id >= startId has been False 21 times, but id <= endId has never been False.
	// As analyzed above, when lower_bound returns valid iterator, endId >= id,
	// so id <= endId is always true. This branch is UNREACHABLE.
	//
	// However, we can still try to create a scenario and verify correctness.
	auto type = static_cast<uint32_t>(graph::EntityType::Node);

	// Single segment [100, 109]
	indexManager->updateSegmentIndex(createHeader(getSegOffset(0), type, 100, 10), -1);

	// Search for id=50: lower_bound finds [100,109] because 109 >= 50
	// But id=50 < startId=100, so returns 0 (covers startId branch False)
	EXPECT_EQ(indexManager->findSegmentForId(type, 50), 0ULL);

	// Search for id=110: lower_bound finds end() because 109 < 110
	// it == end(), returns 0
	EXPECT_EQ(indexManager->findSegmentForId(type, 110), 0ULL);
}

TEST_F(SegmentIndexManagerTest, UpdateSegmentIndex_KeyChangedOldStartIdFoundButOffsetMismatch) {
	// Covers: line 131-132 - itOld found by startId but offset doesn't match
	// This triggers the erase_if fallback at line 135
	auto type = static_cast<uint32_t>(graph::EntityType::Node);
	uint64_t offset1 = getSegOffset(0);
	uint64_t offset2 = getSegOffset(1);

	// Insert two segments at different offsets but nearby IDs
	// Segment A: [100, 109] at offset1
	indexManager->updateSegmentIndex(createHeader(offset1, type, 100, 10), -1);
	// Segment B: [200, 209] at offset2
	indexManager->updateSegmentIndex(createHeader(offset2, type, 200, 10), -1);

	ASSERT_EQ(indexManager->getNodeSegmentIndex().size(), 2UL);

	// Now update segment B (offset2) claiming old start was 100 (which is segment A's start)
	// This means:
	// - binary search for oldStartId=100 finds itOld pointing to segment A at offset1
	// - itOld->segmentOffset (offset1) != header.file_offset (offset2) -> offset mismatch!
	// - Falls through to erase_if, which removes by offset2
	SegmentHeader newH = createHeader(offset2, type, 300, 10);
	indexManager->updateSegmentIndex(newH, 100);

	// Segment A ([100,109]) should still exist (not erased by startId match since offset didn't match)
	// But erase_if removes all entries with offset == offset2, so segment B is gone
	// And new entry [300,309] is added at offset2
	const auto &idx = indexManager->getNodeSegmentIndex();
	ASSERT_EQ(idx.size(), 2UL);
	// Should have [100,109] and [300,309]
	EXPECT_EQ(indexManager->findSegmentForId(type, 105), offset1);
	EXPECT_EQ(indexManager->findSegmentForId(type, 305), offset2);
	EXPECT_EQ(indexManager->findSegmentForId(type, 205), 0ULL); // Old B range gone
}

TEST_F(SegmentIndexManagerTest, InvalidTypeThrowsOnNonConstPath) {
	// Covers: line 203 default branch in non-const getSegmentIndexForType
	// updateSegmentIndex calls non-const getSegmentIndexForType
	auto invalidType = 255u;
	SegmentHeader h = createHeader(getSegOffset(0), invalidType, 100, 10);

	EXPECT_THROW(indexManager->updateSegmentIndex(h, -1), std::invalid_argument);
}

TEST_F(SegmentIndexManagerTest, RemoveSegmentIndex_FoundByStartIdButDifferentOffset) {
	// Covers: line 183 - it->segmentOffset == header.file_offset False branch
	// (This is similar to RemoveSegmentIgnoredIfOffsetMismatch but explicitly for this path)
	auto type = static_cast<uint32_t>(graph::EntityType::Edge);
	uint64_t offset1 = getSegOffset(0);
	uint64_t offset2 = getSegOffset(1);

	// Insert at offset1
	indexManager->updateSegmentIndex(createHeader(offset1, type, 100, 10), -1);

	// Try to remove with same startId but different offset
	SegmentHeader remH = createHeader(offset2, type, 100, 10);
	indexManager->removeSegmentIndex(remH);

	// Should not have removed since offsets don't match
	EXPECT_EQ(indexManager->getEdgeSegmentIndex().size(), 1UL);
	EXPECT_EQ(indexManager->findSegmentForId(type, 105), offset1);
}
