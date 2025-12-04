/**
 * @file test_SegmentTracker.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/3
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <vector>

#include "graph/core/Node.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/SegmentTypeRegistry.hpp"
#include "graph/storage/StorageHeaders.hpp"

using namespace graph::storage;
using namespace graph;

namespace {
	// Helper to calculate robust file offsets based on header constants
	uint64_t getSegmentOffset(uint32_t index) {
		return FILE_HEADER_SIZE + (static_cast<uint64_t>(index) * TOTAL_SEGMENT_SIZE);
	}
} // namespace

class SegmentTrackerTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> fileStream;
	std::shared_ptr<SegmentTracker> tracker;
	std::shared_ptr<SegmentIndexManager> indexManager;

	void SetUp() override {
		// 1. Generate unique temporary file path
		auto tempDir = std::filesystem::temp_directory_path();
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dis(0, 9999999);
		testFilePath = tempDir / ("segment_tracker_test_" + std::to_string(dis(gen)) + ".dat");

		// 2. Initialize file with a clean FileHeader and empty space
		{
			std::ofstream out(testFilePath, std::ios::binary);
			FileHeader header;
			header.version = 1;
			out.write(reinterpret_cast<const char *>(&header), sizeof(FileHeader));

			// Allocate space for 10 segments to prevent EOF errors during random access
			std::vector<char> emptySegment(TOTAL_SEGMENT_SIZE, 0);
			for (int i = 0; i < 10; ++i) {
				out.write(emptySegment.data(), TOTAL_SEGMENT_SIZE);
			}
		}

		// 3. Open file stream
		fileStream = std::make_shared<std::fstream>(testFilePath, std::ios::binary | std::ios::in | std::ios::out);
		ASSERT_TRUE(fileStream->is_open()) << "Failed to open test file";

		// 4. Initialize Tracker
		tracker = std::make_shared<SegmentTracker>(fileStream);

		// Read header to pass to initialize
		fileStream->seekg(0);
		FileHeader header;
		fileStream->read(reinterpret_cast<char *>(&header), sizeof(FileHeader));
		tracker->initialize(header);

		// 5. Initialize and Link IndexManager
		// We need a real IndexManager to test the integration (e.g., updating indexes when tracker updates)
		indexManager = std::make_shared<SegmentIndexManager>(tracker);

		// Mock chain heads for the index manager initialization
		uint64_t nodeHead = 0, edgeHead = 0, propHead = 0, blobHead = 0, idxHead = 0, stateHead = 0;
		indexManager->initialize(nodeHead, edgeHead, propHead, blobHead, idxHead, stateHead);

		// Link them
		tracker->setSegmentIndexManager(indexManager);
	}

	void TearDown() override {
		// Break circular references if any
		tracker.reset();
		indexManager.reset();

		if (fileStream) {
			fileStream->close();
		}
		// Cleanup file
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	// Helper: Read raw bytes to verify disk persistence
	template<typename T>
	T readRawAt(uint64_t offset) {
		T data;
		fileStream->seekg(static_cast<std::streamoff>(offset));
		fileStream->read(reinterpret_cast<char *>(&data), sizeof(T));
		return data;
	}
};

// ============================================================================
// 1. Initialization & Registry Tests
// ============================================================================

TEST_F(SegmentTrackerTest, InitializationRegistersTypes) {
	// Verify that initializeRegistry() was called internally
	auto types = SegmentTypeRegistry::getAllTypes();
	EXPECT_FALSE(types.empty());

	// Check for standard types
	bool foundNode = false;
	for (auto t: types) {
		if (t == EntityType::Node)
			foundNode = true;
	}
	EXPECT_TRUE(foundNode);
}

TEST_F(SegmentTrackerTest, ChainHeadsInitializedToZero) {
	EXPECT_EQ(tracker->getChainHead(static_cast<uint32_t>(EntityType::Node)), 0);
	EXPECT_EQ(tracker->getChainHead(static_cast<uint32_t>(EntityType::Edge)), 0);
}

// ============================================================================
// 2. Segment Management (Register, Get, Error)
// ============================================================================

TEST_F(SegmentTrackerTest, RegisterAndRetrieveSegment) {
	uint64_t offset = getSegmentOffset(0);
	auto type = static_cast<uint32_t>(EntityType::Node);
	uint32_t capacity = NODES_PER_SEGMENT;

	tracker->registerSegment(offset, type, capacity);

	SegmentHeader &header = tracker->getSegmentHeader(offset);
	EXPECT_EQ(header.file_offset, offset);
	EXPECT_EQ(header.data_type, type);
	EXPECT_EQ(header.capacity, capacity);
	EXPECT_EQ(header.used, 0);
	EXPECT_EQ(header.is_dirty, 0); // Newly registered but not modified via tracker's update methods yet
}

TEST_F(SegmentTrackerTest, GetNonExistentSegmentThrows) {
	uint64_t invalidOffset = getSegmentOffset(999); // Way out of bounds of our setup
	// Since file is small, reading this might fail or tracker throws if not in map
	// The tracker reads from file if not in map.
	EXPECT_THROW(tracker->getSegmentHeader(invalidOffset), std::runtime_error);
}

// ============================================================================
// 3. Metadata Updates & Integration with IndexManager
// ============================================================================

TEST_F(SegmentTrackerTest, UpdateUsageUpdatesIndex) {
	uint64_t offset = getSegmentOffset(0);
	auto type = static_cast<uint32_t>(EntityType::Node);
	tracker->registerSegment(offset, type, 100);

	// Initial state: Start ID 0 (default)
	SegmentHeader &header = tracker->getSegmentHeader(offset);
	header.start_id = 1000; // Manually set start ID for test
	tracker->writeSegmentHeader(offset, header); // Commit to update internal cache properly

	// Action: Update usage
	// This should trigger IndexManager update because tracker calls updateSegmentIndex
	tracker->updateSegmentUsage(offset, 50, 0);

	// Verification
	SegmentHeader &updatedHeader = tracker->getSegmentHeader(offset);
	EXPECT_EQ(updatedHeader.used, 50);
	EXPECT_EQ(updatedHeader.is_dirty, 1);

	// check index manager to see if it knows about this segment now
	uint64_t foundOffset = indexManager->findSegmentForId(type, 1025); // 1000 + 25
	EXPECT_EQ(foundOffset, offset);
}

TEST_F(SegmentTrackerTest, MarkForCompaction) {
	uint64_t offset = getSegmentOffset(0);
	tracker->registerSegment(offset, static_cast<uint32_t>(EntityType::Node), 100);

	tracker->markForCompaction(offset, true);
	EXPECT_EQ(tracker->getSegmentHeader(offset).needs_compaction, 1);
	EXPECT_EQ(tracker->getSegmentHeader(offset).is_dirty, 1);

	tracker->markForCompaction(offset, false);
	EXPECT_EQ(tracker->getSegmentHeader(offset).needs_compaction, 0);
}

// ============================================================================
// 4. Persistence & Flushing
// ============================================================================

TEST_F(SegmentTrackerTest, FlushDirtySegments) {
	uint64_t offset = getSegmentOffset(0);
	tracker->registerSegment(offset, static_cast<uint32_t>(EntityType::Node), 100);

	// Modify in memory
	tracker->updateSegmentUsage(offset, 10, 0);

	// Verify disk is NOT yet updated (raw read)
	auto diskHeader = readRawAt<SegmentHeader>(offset);
	// registerSegment writes 0 used. updateSegmentUsage marks dirty but doesn't flush immediately.
	// Note: implementation details: registerSegment writes via memory map or just map?
	// Looking at code: registerSegment just updates `segments_` map. It doesn't write to file immediately unless
	// `writeSegmentHeader` is called or flushed. However, the test SetUp initialized file with zeros.
	EXPECT_EQ(diskHeader.used, 0);

	// Action: Flush
	tracker->flushDirtySegments();

	// Verify disk IS updated
	diskHeader = readRawAt<SegmentHeader>(offset);
	EXPECT_EQ(diskHeader.used, 10);

	// Verify internal dirty flag cleared
	EXPECT_EQ(tracker->getSegmentHeader(offset).is_dirty, 0);
}

// ============================================================================
// 5. Linked List Management
// ============================================================================

TEST_F(SegmentTrackerTest, LinkSegments) {
	uint64_t head = getSegmentOffset(0);
	uint64_t next = getSegmentOffset(1);

	tracker->registerSegment(head, static_cast<uint32_t>(EntityType::Node), 100);
	tracker->registerSegment(next, static_cast<uint32_t>(EntityType::Node), 100);

	// Link
	tracker->updateSegmentLinks(head, 0, next);
	tracker->updateSegmentLinks(next, head, 0);

	EXPECT_EQ(tracker->getSegmentHeader(head).next_segment_offset, next);
	EXPECT_EQ(tracker->getSegmentHeader(next).prev_segment_offset, head);

	// Self loop detection
	EXPECT_THROW(tracker->updateSegmentLinks(head, head, 0), std::runtime_error);
	EXPECT_THROW(tracker->updateSegmentLinks(head, 0, head), std::runtime_error);
}

// ============================================================================
// 6. Bitmap Operations
// ============================================================================

TEST_F(SegmentTrackerTest, SingleBitOperations) {
	uint64_t offset = getSegmentOffset(0);
	// Capacity 16 -> 2 bytes bitmap
	tracker->registerSegment(offset, static_cast<uint32_t>(EntityType::Node), 16);

	// Pretend usage is 16
	tracker->updateSegmentUsage(offset, 16, 0);

	// Set bit 5 to inactive (assuming setEntityActive(..., false) means inactive/deleted?)
	// In code: setBit(..., value).
	// Logic: if value=true (active), inactive_count decr. if value=false (inactive), inactive_count incr.

	// Initial: all 0s? registerSegment memsets to 0.
	// But usually active entities should have bit 1.
	// Let's assume we initialize all to active (1) manually or via bulk update first.

	std::vector<bool> allActive(16, true);
	tracker->updateActivityBitmap(offset, allActive);
	EXPECT_EQ(tracker->getSegmentHeader(offset).inactive_count, 0);

	// Mark index 5 as inactive (false)
	tracker->setEntityActive(offset, 5, false);
	EXPECT_FALSE(tracker->isEntityActive(offset, 5));
	EXPECT_EQ(tracker->getSegmentHeader(offset).inactive_count, 1);

	// Mark index 5 as active again
	tracker->setEntityActive(offset, 5, true);
	EXPECT_TRUE(tracker->isEntityActive(offset, 5));
	EXPECT_EQ(tracker->getSegmentHeader(offset).inactive_count, 0);
}

TEST_F(SegmentTrackerTest, BulkBitmapUpdate) {
	uint64_t offset = getSegmentOffset(0);
	tracker->registerSegment(offset, static_cast<uint32_t>(EntityType::Node), 8);

	std::vector<bool> pattern = {true, false, true, false, true, false, true, false}; // 4 inactive
	tracker->updateActivityBitmap(offset, pattern);

	SegmentHeader &header = tracker->getSegmentHeader(offset);
	EXPECT_EQ(header.inactive_count, 4);
	EXPECT_TRUE(tracker->isEntityActive(offset, 0));
	EXPECT_FALSE(tracker->isEntityActive(offset, 1));
}

// ============================================================================
// 7. Free List Management
// ============================================================================

TEST_F(SegmentTrackerTest, FreeListOperations) {
	uint64_t offset = getSegmentOffset(0);
	tracker->registerSegment(offset, static_cast<uint32_t>(EntityType::Node), 100);

	// Verify empty initially
	EXPECT_TRUE(tracker->getFreeSegments().empty());

	// Mark free
	tracker->markSegmentFree(offset);

	// Verify in list
	auto freeList = tracker->getFreeSegments();
	ASSERT_EQ(freeList.size(), 1);
	EXPECT_EQ(freeList[0], offset);

	// Verify header on disk is wiped/marked (Code writes data_type = 0xFF)
	auto diskHeader = readRawAt<SegmentHeader>(offset);
	EXPECT_EQ(diskHeader.data_type, 0xFF);

	// Remove from free list (reallocation scenario)
	tracker->removeFromFreeList(offset);
	EXPECT_TRUE(tracker->getFreeSegments().empty());
}

TEST_F(SegmentTrackerTest, MarkFreeAlignCheck) {
	// Should throw if offset is not aligned with SEGMENT_SIZE
	EXPECT_THROW(tracker->markSegmentFree(FILE_HEADER_SIZE + 10), std::runtime_error);
}

// ============================================================================
// 8. ID Lookup Strategy (Fast vs Slow Path)
// ============================================================================

TEST_F(SegmentTrackerTest, LookupFastPathViaIndex) {
	uint64_t offset = getSegmentOffset(0);
	tracker->registerSegment(offset, static_cast<uint32_t>(EntityType::Node), 100);

	// Setup Header
	tracker->updateSegmentHeader(offset, [](SegmentHeader &h) {
		h.start_id = 500;
		h.used = 100;
	});

	// Verify IndexManager caught it
	// Note: updateSegmentHeader calls indexMgr->updateSegmentIndex

	// Perform lookup
	uint64_t result = tracker->getSegmentOffsetForNodeId(550);
	EXPECT_EQ(result, offset);

	// Lookup out of range
	EXPECT_EQ(tracker->getSegmentOffsetForNodeId(800), 0);
}

TEST_F(SegmentTrackerTest, LookupSlowPathLinearScan) {
	// Simulate a scenario where index is not yet built or failed,
	// or manually disconnect index manager to force linear scan (if possible)
	// The code tries indexMgr lock first.

	// Let's create a scenario: We set the chain head, but we clear the IndexManager's memory
	// (though IndexManager monitors tracker, so it's hard to desync them in this test structure).

	// Alternative: Reset the indexManager pointer in tracker to null to force fallback
	tracker->setSegmentIndexManager(std::weak_ptr<SegmentIndexManager>());

	uint64_t offset1 = getSegmentOffset(0);
	uint64_t offset2 = getSegmentOffset(1);

	tracker->registerSegment(offset1, static_cast<uint32_t>(EntityType::Node), 10);
	tracker->registerSegment(offset2, static_cast<uint32_t>(EntityType::Node), 10);

	// Link them
	tracker->updateSegmentLinks(offset1, 0, offset2);
	tracker->updateSegmentLinks(offset2, offset1, 0);

	// Set headers
	tracker->updateSegmentHeader(offset1, [](SegmentHeader &h) {
		h.start_id = 0;
		h.capacity = 10;
	});
	tracker->updateSegmentHeader(offset2, [](SegmentHeader &h) {
		h.start_id = 10;
		h.capacity = 10;
	});

	// Set Chain Head
	tracker->updateChainHead(static_cast<uint32_t>(EntityType::Node), offset1);

	// Linear scan for ID 15 (should be in offset2)
	uint64_t result = tracker->getSegmentOffsetForNodeId(15);
	EXPECT_EQ(result, offset2);

	// Restore Manager for TearDown cleanliness
	tracker->setSegmentIndexManager(indexManager);
}

// ============================================================================
// 9. Fragmentation Calculation
// ============================================================================

TEST_F(SegmentTrackerTest, FragmentationCalculation) {
	auto type = static_cast<uint32_t>(EntityType::Node);
	uint64_t offset = getSegmentOffset(0);
	tracker->registerSegment(offset, type, 100);

	// Case 1: 100% used -> 0 fragmentation
	tracker->updateSegmentUsage(offset, 100, 0);
	EXPECT_NEAR(tracker->calculateFragmentationRatio(type), 0.0, 0.001);

	// Case 2: 50 used, 0 inactive -> 50% free space -> 0.5 fragmentation
	tracker->updateSegmentUsage(offset, 50, 0);
	EXPECT_NEAR(tracker->calculateFragmentationRatio(type), 0.5, 0.001);

	// Case 3: 100 used, 50 inactive -> 50 active -> 0.5 fragmentation
	tracker->updateSegmentUsage(offset, 100, 50);
	EXPECT_NEAR(tracker->calculateFragmentationRatio(type), 0.5, 0.001);
}

TEST_F(SegmentTrackerTest, GetSegmentsNeedingCompaction) {
	auto type = static_cast<uint32_t>(EntityType::Node);
	uint64_t off1 = getSegmentOffset(0);
	uint64_t off2 = getSegmentOffset(1);

	tracker->registerSegment(off1, type, 100);
	tracker->registerSegment(off2, type, 100);

	// Off1: High fragmentation (1 used)
	tracker->updateSegmentUsage(off1, 1, 0);

	// Off2: Low fragmentation (100 used)
	tracker->updateSegmentUsage(off2, 100, 0);

	// Threshold 0.8
	auto segments = tracker->getSegmentsNeedingCompaction(type, 0.8);

	EXPECT_EQ(segments.size(), 1);
	EXPECT_EQ(segments[0].file_offset, off1);
}

// ============================================================================
// 10. Entity I/O (Mocked/Basic)
// ============================================================================

TEST_F(SegmentTrackerTest, WriteAndReadEntity) {
	// This test assumes Node has a valid FixedSizeSerializer specialization
	// or is trivially copyable.

	uint64_t segOffset = getSegmentOffset(0);
	tracker->registerSegment(segOffset, static_cast<uint32_t>(EntityType::Node), 10);

	Node testNode(123, "TestNode");
	// Calculate size (using Node::getTotalSize() as defined in StorageHeaders constants)
	size_t nodeSize = Node::getTotalSize();

	// Write at index 0
	tracker->writeEntity<Node>(segOffset, 0, testNode, nodeSize);

	// Verify dirty
	EXPECT_EQ(tracker->getSegmentHeader(segOffset).is_dirty, 1);

	// Read back
	// Note: If FixedSizeSerializer is not linked, this might fail linking.
	// Assuming the environment matches the user's codebase.
	try {
		Node result = tracker->readEntity<Node>(segOffset, 0, nodeSize);
		EXPECT_EQ(result.getId(), 123);
		// String label comparison depends on Node implementation (fixed buffer vs pointer)
		// Assuming fixed buffer for FixedSizeSerializer compatibility.
		EXPECT_EQ(result.getLabel(), "TestNode");
	} catch (const std::exception &e) {
		// Fallback if Node/Serializer implementation prevents direct testing here
		FAIL() << "Entity I/O failed: " << e.what();
	}
}
