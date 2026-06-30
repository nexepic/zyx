/**
 * @file test_ParallelExecutionTypes.cpp
 * @brief Tests shared parallel execution enum helpers.
 */

#include <gtest/gtest.h>

#include "graph/concurrent/ParallelExecutionTypes.hpp"

using graph::concurrent::ParallelDecisionReason;
using graph::concurrent::parallelDecisionReasonCode;
using graph::concurrent::parallelDecisionReasonName;

TEST(ParallelExecutionTypesTest, DecisionReasonNamesAndCodesCoverKnownReasons) {
	EXPECT_EQ(parallelDecisionReasonName(ParallelDecisionReason::PDR_PARALLEL), "parallel");
	EXPECT_EQ(parallelDecisionReasonCode(ParallelDecisionReason::PDR_PARALLEL), 0);

	EXPECT_EQ(parallelDecisionReasonName(ParallelDecisionReason::PDR_NO_WORKERS), "no_workers");
	EXPECT_EQ(parallelDecisionReasonCode(ParallelDecisionReason::PDR_NO_WORKERS), 1);

	EXPECT_EQ(
			parallelDecisionReasonName(ParallelDecisionReason::PDR_INSUFFICIENT_PARTITIONS),
			"insufficient_partitions");
	EXPECT_EQ(parallelDecisionReasonCode(ParallelDecisionReason::PDR_INSUFFICIENT_PARTITIONS), 2);

	EXPECT_EQ(parallelDecisionReasonName(ParallelDecisionReason::PDR_INSUFFICIENT_ITEMS), "insufficient_items");
	EXPECT_EQ(parallelDecisionReasonCode(ParallelDecisionReason::PDR_INSUFFICIENT_ITEMS), 3);

	EXPECT_EQ(
			parallelDecisionReasonName(ParallelDecisionReason::PDR_INSUFFICIENT_GRANULARITY),
			"insufficient_granularity");
	EXPECT_EQ(parallelDecisionReasonCode(ParallelDecisionReason::PDR_INSUFFICIENT_GRANULARITY), 4);
}

TEST(ParallelExecutionTypesTest, DecisionReasonHelpersHandleUnknownValues) {
	const auto unknown = static_cast<ParallelDecisionReason>(127);
	EXPECT_EQ(parallelDecisionReasonName(unknown), "unknown");
	EXPECT_EQ(parallelDecisionReasonCode(unknown), -1);
}
