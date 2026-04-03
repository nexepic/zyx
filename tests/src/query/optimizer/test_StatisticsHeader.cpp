/**
 * @file test_StatisticsHeader.cpp
 * @brief Focused unit tests for inline helpers in Statistics.hpp.
 */

#include <gtest/gtest.h>

#include "graph/query/optimizer/Statistics.hpp"

using namespace graph::query::optimizer;

TEST(StatisticsHeaderTest, RangeSelectivityUsesConstantFallback) {
	EXPECT_DOUBLE_EQ(PropertyStatistics::rangeSelectivity(), 0.33);
}

TEST(StatisticsHeaderTest, EstimateLabelCountFallsBackToTotalWhenLabelMissing) {
	Statistics stats;
	stats.totalNodeCount = 42;
	EXPECT_EQ(stats.estimateLabelCount("Unknown"), 42);
}

