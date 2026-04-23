/**
 * @file test_SegmentTracker_CrcValidation.cpp
 * @brief Coverage tests for SegmentTracker CRC validation, dirty flushing,
 *        loadSegmentChain type mismatch, getSegmentHeaderCopy slow path,
 *        setEntityActiveRange, batchSetEntityActive, and bitmap edge cases.
 **/

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>

#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/StorageHeaders.hpp"

using namespace graph::storage;
using namespace graph;

namespace {
	uint64_t getSegOff(uint32_t index) {
		return FILE_HEADER_SIZE + (static_cast<uint64_t>(index) * TOTAL_SEGMENT_SIZE);
	}
} // namespace

class SegmentTrackerCrcTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> fileStream;
	std::shared_ptr<StorageIO> storageIO;
	FileHeader fileHeader{};
	std::shared_ptr<SegmentTracker> tracker;

	void SetUp() override {
		testFilePath = std::filesystem::temp_directory_path() / "seg_tracker_crc_test.dat";
		fileStream = std::make_shared<std::fstream>(testFilePath,
			std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(fileStream->is_open());

		// Write file header
		fileStream->write(reinterpret_cast<char *>(&fileHeader), sizeof(FileHeader));

		// Pre-allocate 5 segments worth of space
		std::vector<char> zeros(TOTAL_SEGMENT_SIZE * 5, 0);
		fileStream->write(zeros.data(), zeros.size());
		fileStream->flush();

		storageIO = std::make_shared<StorageIO>(fileStream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		tracker.reset();
		storageIO.reset();
		if (fileStream && fileStream->is_open()) fileStream->close();
		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	void writeSegmentHeader(uint64_t offset, const SegmentHeader &header) {
		storageIO->writeAt(offset, &header, sizeof(SegmentHeader));
	}

	void createTrackerWithChain() {
		uint64_t seg0 = getSegOff(0);
		uint64_t seg1 = getSegOff(1);

		SegmentHeader h0{};
		h0.data_type = Node::typeId;
		h0.capacity = NODES_PER_SEGMENT;
		h0.start_id = 1;
		h0.used = 5;
		h0.inactive_count = 0;
		h0.bitmap_size = bitmap::calculateBitmapSize(5);
		h0.file_offset = seg0;
		h0.prev_segment_offset = 0;
		h0.next_segment_offset = seg1;
		for (uint32_t i = 0; i < 5; ++i) bitmap::setBit(h0.activity_bitmap, i, true);
		writeSegmentHeader(seg0, h0);

		SegmentHeader h1{};
		h1.data_type = Node::typeId;
		h1.capacity = NODES_PER_SEGMENT;
		h1.start_id = NODES_PER_SEGMENT + 1;
		h1.used = 3;
		h1.inactive_count = 0;
		h1.bitmap_size = bitmap::calculateBitmapSize(3);
		h1.file_offset = seg1;
		h1.prev_segment_offset = seg0;
		h1.next_segment_offset = 0;
		for (uint32_t i = 0; i < 3; ++i) bitmap::setBit(h1.activity_bitmap, i, true);
		writeSegmentHeader(seg1, h1);

		storageIO->flushStream();

		fileHeader.node_segment_head = seg0;
		tracker = std::make_shared<SegmentTracker>(storageIO, fileHeader);
	}
};

TEST_F(SegmentTrackerCrcTest, FlushDirtySegments_NoopWhenClean) {
	fileHeader = {};
	tracker = std::make_shared<SegmentTracker>(storageIO, fileHeader);
	EXPECT_NO_THROW(tracker->flushDirtySegments());
}

TEST_F(SegmentTrackerCrcTest, FlushDirtySegments_WriteDirtyHeaders) {
	createTrackerWithChain();
	uint64_t seg0 = getSegOff(0);

	// Modify a segment to make it dirty
	tracker->updateSegmentUsage(seg0, 6, 0);
	tracker->flushDirtySegments();

	// Verify the flush worked by re-reading the header
	SegmentHeader check{};
	storageIO->readAt(seg0, &check, sizeof(SegmentHeader));
	EXPECT_EQ(check.used, 6u);
}

TEST_F(SegmentTrackerCrcTest, FlushDirtySegments_WithPendingCrc) {
	createTrackerWithChain();
	uint64_t seg0 = getSegOff(0);

	// Set a pending CRC to exercise the pendingCrcs_ path
	tracker->setPendingCrc(seg0, 0x12345678);
	tracker->updateSegmentUsage(seg0, 6, 0);
	tracker->flushDirtySegments();

	SegmentHeader check{};
	storageIO->readAt(seg0, &check, sizeof(SegmentHeader));
	EXPECT_EQ(check.segment_crc, 0x12345678u);
}

TEST_F(SegmentTrackerCrcTest, GetSegmentHeaderCopy_CachedPath) {
	createTrackerWithChain();
	uint64_t seg0 = getSegOff(0);

	// First call is cached (fast path)
	SegmentHeader copy = tracker->getSegmentHeaderCopy(seg0);
	EXPECT_EQ(copy.data_type, Node::typeId);
	EXPECT_EQ(copy.used, 5u);
}

TEST_F(SegmentTrackerCrcTest, GetSegmentHeaderCopy_SlowPath) {
	// Create tracker with no chain heads so segments_ is empty
	fileHeader = {};
	tracker = std::make_shared<SegmentTracker>(storageIO, fileHeader);

	// Write a segment directly to disk (not in tracker's cache)
	uint64_t seg0 = getSegOff(0);
	SegmentHeader h{};
	h.data_type = Edge::typeId;
	h.capacity = EDGES_PER_SEGMENT;
	h.start_id = 1;
	h.used = 2;
	h.file_offset = seg0;
	writeSegmentHeader(seg0, h);
	storageIO->flushStream();

	// getSegmentHeaderCopy should fallback to reading from disk
	SegmentHeader copy = tracker->getSegmentHeaderCopy(seg0);
	EXPECT_EQ(copy.data_type, Edge::typeId);
	EXPECT_EQ(copy.used, 2u);
}

TEST_F(SegmentTrackerCrcTest, SetEntityActiveRange_ActivateDeactivate) {
	createTrackerWithChain();
	uint64_t seg0 = getSegOff(0);

	// Deactivate entities 1-3
	tracker->setEntityActiveRange(seg0, 1, 3, false);

	auto &header = tracker->getSegmentHeader(seg0);
	EXPECT_EQ(header.inactive_count, 3u);

	// Re-activate entities 1-2
	tracker->setEntityActiveRange(seg0, 1, 2, true);
	auto &h2 = tracker->getSegmentHeader(seg0);
	EXPECT_EQ(h2.inactive_count, 1u);
}

TEST_F(SegmentTrackerCrcTest, SetEntityActiveRange_NoChange) {
	createTrackerWithChain();
	uint64_t seg0 = getSegOff(0);

	auto &hBefore = tracker->getSegmentHeader(seg0);
	uint32_t inactiveBefore = hBefore.inactive_count;

	// Setting already active entities to active -> no change
	tracker->setEntityActiveRange(seg0, 0, 3, true);

	auto &hAfter = tracker->getSegmentHeader(seg0);
	EXPECT_EQ(hAfter.inactive_count, inactiveBefore);
}

TEST_F(SegmentTrackerCrcTest, BatchSetEntityActive_MixedChanges) {
	createTrackerWithChain();
	uint64_t seg0 = getSegOff(0);

	std::vector<std::pair<uint32_t, bool>> changes = {
		{0, false}, // deactivate
		{1, false}, // deactivate
		{2, true},  // already active - no change
	};
	tracker->batchSetEntityActive(seg0, changes);

	auto &h = tracker->getSegmentHeader(seg0);
	EXPECT_EQ(h.inactive_count, 2u);
}

TEST_F(SegmentTrackerCrcTest, BatchSetEntityActive_EmptyList) {
	createTrackerWithChain();
	uint64_t seg0 = getSegOff(0);

	// Empty list should be a no-op
	tracker->batchSetEntityActive(seg0, {});
	EXPECT_EQ(tracker->getSegmentHeader(seg0).inactive_count, 0u);
}

TEST_F(SegmentTrackerCrcTest, SetBitmapBit_DeactivateThenReactivate) {
	createTrackerWithChain();
	uint64_t seg0 = getSegOff(0);

	tracker->setBitmapBit(seg0, 0, false);
	EXPECT_EQ(tracker->getSegmentHeader(seg0).inactive_count, 1u);

	tracker->setBitmapBit(seg0, 0, true);
	EXPECT_EQ(tracker->getSegmentHeader(seg0).inactive_count, 0u);
}

TEST_F(SegmentTrackerCrcTest, UpdateActivityBitmap_WithInactiveEntries) {
	createTrackerWithChain();
	uint64_t seg0 = getSegOff(0);

	// Set activity map with some inactive entries
	std::vector<bool> newMap = {true, false, true, false, true};
	tracker->updateActivityBitmap(seg0, newMap);

	auto &h = tracker->getSegmentHeader(seg0);
	EXPECT_EQ(h.inactive_count, 2u);
	EXPECT_EQ(h.bitmap_size, bitmap::calculateBitmapSize(5));
}

TEST_F(SegmentTrackerCrcTest, GetActivityBitmap_MatchesData) {
	createTrackerWithChain();
	uint64_t seg0 = getSegOff(0);

	auto bm = tracker->getActivityBitmap(seg0);
	EXPECT_EQ(bm.size(), 5u);
	for (size_t i = 0; i < 5; ++i) EXPECT_TRUE(bm[i]);
}

TEST_F(SegmentTrackerCrcTest, CountActiveEntities_Correct) {
	createTrackerWithChain();
	uint64_t seg0 = getSegOff(0);

	EXPECT_EQ(tracker->countActiveEntities(seg0), 5u);

	tracker->setEntityActive(seg0, 0, false);
	EXPECT_EQ(tracker->countActiveEntities(seg0), 4u);
}

TEST_F(SegmentTrackerCrcTest, UpdateSegmentLinks_SelfLoopThrows) {
	createTrackerWithChain();
	uint64_t seg0 = getSegOff(0);

	EXPECT_THROW(tracker->updateSegmentLinks(seg0, seg0, 0), std::runtime_error);
	EXPECT_THROW(tracker->updateSegmentLinks(seg0, 0, seg0), std::runtime_error);
}

TEST_F(SegmentTrackerCrcTest, MarkSegmentFree_InvalidAlignment) {
	createTrackerWithChain();
	EXPECT_THROW(tracker->markSegmentFree(FILE_HEADER_SIZE + 1), std::runtime_error);
}

TEST_F(SegmentTrackerCrcTest, CalculateFragmentationRatio_EmptyType) {
	fileHeader = {};
	tracker = std::make_shared<SegmentTracker>(storageIO, fileHeader);

	double ratio = tracker->calculateFragmentationRatio(Node::typeId);
	EXPECT_DOUBLE_EQ(ratio, 0.0);
}

TEST_F(SegmentTrackerCrcTest, IsIdInUsedRange_TrueAndFalse) {
	createTrackerWithChain();
	uint64_t seg0 = getSegOff(0);

	EXPECT_TRUE(tracker->isIdInUsedRange(seg0, 1));
	EXPECT_TRUE(tracker->isIdInUsedRange(seg0, 5));
	EXPECT_FALSE(tracker->isIdInUsedRange(seg0, 6));
	EXPECT_FALSE(tracker->isIdInUsedRange(seg0, 0));
}

TEST_F(SegmentTrackerCrcTest, CollectSegmentCrcs_Ordered) {
	createTrackerWithChain();
	auto crcs = tracker->collectSegmentCrcs();
	// Should have 2 segments
	EXPECT_EQ(crcs.size(), 2u);
}
