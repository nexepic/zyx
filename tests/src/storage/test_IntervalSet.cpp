/**
 * @file test_IntervalSet.cpp
 * @brief Direct tests for compact free-id interval storage.
 */

#include <gtest/gtest.h>

#include "graph/storage/IntervalSet.hpp"

using graph::storage::IntervalSet;

TEST(IntervalSetTest, InvalidRangeIsIgnoredAndEmptyPopReturnsZero) {
	IntervalSet intervals;

	intervals.addRange(9, 3);
	EXPECT_TRUE(intervals.empty());
	EXPECT_EQ(intervals.intervalCount(), 0UL);
	EXPECT_EQ(intervals.pop(), 0);
}

TEST(IntervalSetTest, AdjacentAndOverlappingRangesAreMerged) {
	IntervalSet intervals;

	intervals.addRange(5, 6);
	intervals.addRange(1, 2);
	intervals.addRange(3, 4);
	intervals.add(7);

	EXPECT_EQ(intervals.intervalCount(), 1UL);
	for (int64_t expected = 1; expected <= 7; ++expected) {
		EXPECT_EQ(intervals.pop(), expected);
	}
	EXPECT_TRUE(intervals.empty());
}
