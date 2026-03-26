/**
 * @file test_SegmentReadUtils.cpp
 * @brief Unit tests for coalesced segment read grouping logic
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <gtest/gtest.h>
#include "graph/storage/SegmentReadUtils.hpp"

using namespace graph::storage;
using SegIndex = SegmentIndexManager::SegmentIndex;

class SegmentReadUtilsTest : public ::testing::Test {
protected:
	// Helper: build a SegmentIndex at a given offset
	static SegIndex makeSeg(int64_t startId, int64_t endId, uint64_t offset) {
		return {startId, endId, offset};
	}
};

TEST_F(SegmentReadUtilsTest, EmptyInput) {
	std::vector<SegIndex> segIndex;
	std::vector<size_t> segIndices;
	auto groups = buildCoalescedGroups(segIndices, segIndex);
	EXPECT_TRUE(groups.empty());
}

TEST_F(SegmentReadUtilsTest, SingleSegment) {
	std::vector<SegIndex> segIndex = {makeSeg(1, 10, FILE_HEADER_SIZE)};
	std::vector<size_t> segIndices = {0};
	auto groups = buildCoalescedGroups(segIndices, segIndex);
	ASSERT_EQ(groups.size(), 1u);
	EXPECT_EQ(groups[0].startOffset, FILE_HEADER_SIZE);
	EXPECT_EQ(groups[0].segCount, 1u);
	EXPECT_EQ(groups[0].memberIndices.size(), 1u);
	EXPECT_EQ(groups[0].memberIndices[0], 0u);
}

TEST_F(SegmentReadUtilsTest, TwoConsecutiveSegments) {
	uint64_t off0 = FILE_HEADER_SIZE;
	uint64_t off1 = FILE_HEADER_SIZE + TOTAL_SEGMENT_SIZE;
	std::vector<SegIndex> segIndex = {
		makeSeg(1, 10, off0),
		makeSeg(11, 20, off1),
	};
	std::vector<size_t> segIndices = {0, 1};
	auto groups = buildCoalescedGroups(segIndices, segIndex);
	ASSERT_EQ(groups.size(), 1u);
	EXPECT_EQ(groups[0].startOffset, off0);
	EXPECT_EQ(groups[0].segCount, 2u);
	EXPECT_EQ(groups[0].memberIndices.size(), 2u);
}

TEST_F(SegmentReadUtilsTest, TwoNonConsecutiveSegments) {
	uint64_t off0 = FILE_HEADER_SIZE;
	uint64_t off1 = FILE_HEADER_SIZE + 3 * TOTAL_SEGMENT_SIZE; // gap
	std::vector<SegIndex> segIndex = {
		makeSeg(1, 10, off0),
		makeSeg(11, 20, off1),
	};
	std::vector<size_t> segIndices = {0, 1};
	auto groups = buildCoalescedGroups(segIndices, segIndex);
	ASSERT_EQ(groups.size(), 2u);
	EXPECT_EQ(groups[0].segCount, 1u);
	EXPECT_EQ(groups[1].segCount, 1u);
}

TEST_F(SegmentReadUtilsTest, MixedConsecutiveAndGap) {
	uint64_t base = FILE_HEADER_SIZE;
	std::vector<SegIndex> segIndex = {
		makeSeg(1, 10, base),
		makeSeg(11, 20, base + TOTAL_SEGMENT_SIZE),
		makeSeg(21, 30, base + 2 * TOTAL_SEGMENT_SIZE),
		makeSeg(31, 40, base + 5 * TOTAL_SEGMENT_SIZE), // gap
		makeSeg(41, 50, base + 6 * TOTAL_SEGMENT_SIZE),
	};
	std::vector<size_t> segIndices = {0, 1, 2, 3, 4};
	auto groups = buildCoalescedGroups(segIndices, segIndex);
	ASSERT_EQ(groups.size(), 2u);
	EXPECT_EQ(groups[0].segCount, 3u);
	EXPECT_EQ(groups[0].startOffset, base);
	EXPECT_EQ(groups[1].segCount, 2u);
	EXPECT_EQ(groups[1].startOffset, base + 5 * TOTAL_SEGMENT_SIZE);
}

TEST_F(SegmentReadUtilsTest, UnsortedInputIsSorted) {
	uint64_t base = FILE_HEADER_SIZE;
	std::vector<SegIndex> segIndex = {
		makeSeg(11, 20, base + TOTAL_SEGMENT_SIZE),
		makeSeg(1, 10, base),
	};
	// Pass in reverse order
	std::vector<size_t> segIndices = {0, 1};
	auto groups = buildCoalescedGroups(segIndices, segIndex);
	ASSERT_EQ(groups.size(), 1u);
	EXPECT_EQ(groups[0].startOffset, base);
	EXPECT_EQ(groups[0].segCount, 2u);
	// memberIndices should reflect original indices: index 1 (offset base) first, index 0 second
	EXPECT_EQ(groups[0].memberIndices[0], 1u);
	EXPECT_EQ(groups[0].memberIndices[1], 0u);
}

TEST_F(SegmentReadUtilsTest, SubsetOfSegments) {
	uint64_t base = FILE_HEADER_SIZE;
	std::vector<SegIndex> segIndex = {
		makeSeg(1, 10, base),
		makeSeg(11, 20, base + TOTAL_SEGMENT_SIZE),
		makeSeg(21, 30, base + 2 * TOTAL_SEGMENT_SIZE),
		makeSeg(31, 40, base + 3 * TOTAL_SEGMENT_SIZE),
	};
	// Only select segments 0 and 2 (not consecutive)
	std::vector<size_t> segIndices = {0, 2};
	auto groups = buildCoalescedGroups(segIndices, segIndex);
	ASSERT_EQ(groups.size(), 2u);
	EXPECT_EQ(groups[0].segCount, 1u);
	EXPECT_EQ(groups[1].segCount, 1u);
}
