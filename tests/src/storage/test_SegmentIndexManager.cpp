/**
 * @file test_SegmentIndexManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/4
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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
