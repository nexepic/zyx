#include "graph/concurrent/ParallelOperatorExecutor.hpp"

#include <atomic>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/debug/PerfTrace.hpp"

namespace {
	struct SumState {
		int64_t sum = 0;
		std::vector<size_t> visited;
	};
} // namespace

TEST(ParallelOperatorExecutorTest, RangePartitionsPreserveOrderedMerge) {
	graph::concurrent::ThreadPool pool(4);
	std::vector<int64_t> values(1024);
	std::iota(values.begin(), values.end(), int64_t{1});
	int64_t total = 0;

	const bool ok = graph::concurrent::ParallelOperatorExecutor::runRangePartitions<SumState>(
			0,
			values.size(),
			&pool,
			{.phase = "test.range_operator",
			 .workloadKind = graph::concurrent::ParallelWorkloadKind::PWK_CPU_BOUND,
			 .estimatedItems = values.size(),
			 .minPartitions = 2,
			 .minItems = 2,
			 .minItemsPerWorker = 64},
			[&](const graph::concurrent::ParallelRangePartition &range, SumState &state) {
				for (size_t i = range.begin; i < range.end; ++i) {
					state.sum += values[i];
				}
			},
			[&](size_t, SumState &state) { total += state.sum; });

	EXPECT_TRUE(ok);
	EXPECT_EQ(total, (1024LL * 1025LL) / 2LL);
}

TEST(ParallelOperatorExecutorTest, EmitsDecisionTelemetryValues) {
	graph::concurrent::ThreadPool pool(4);
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();

	const bool ok = graph::concurrent::ParallelOperatorExecutor::runIndexedPartitions<SumState>(
			4,
			&pool,
			{.phase = "test.operator_profile", .estimatedItems = 4096, .minPartitions = 2, .minItems = 2},
			[](size_t partition, SumState &state) {
				state.sum = static_cast<int64_t>(partition);
			},
			[](size_t, SumState &) {});

	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_TRUE(ok);
	ASSERT_TRUE(snapshot.contains("test.operator_profile.workers"));
	ASSERT_TRUE(snapshot.contains("test.operator_profile.estimated_items"));
	ASSERT_TRUE(snapshot.contains("test.operator_profile.decision.parallel"));
	EXPECT_EQ(snapshot.at("test.operator_profile.task").calls, 4u);
	EXPECT_EQ(snapshot.at("test.operator_profile.workers").valueCalls, 1u);
	EXPECT_GE(snapshot.at("test.operator_profile.workers").totalValue, 2);
	EXPECT_EQ(snapshot.at("test.operator_profile.estimated_items").totalValue, 4096);
}

TEST(ParallelOperatorExecutorTest, SerialDecisionIsProfiledWithoutWorkers) {
	graph::concurrent::ThreadPool pool(1);
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();

	const bool ok = graph::concurrent::ParallelOperatorExecutor::runIndexedPartitions<SumState>(
			3,
			&pool,
			{.phase = "test.operator_serial", .estimatedItems = 3, .minPartitions = 2, .minItems = 2},
			[](size_t partition, SumState &state) {
				state.visited.push_back(partition);
			},
			[](size_t, SumState &) {});

	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_TRUE(ok);
	ASSERT_TRUE(snapshot.contains("test.operator_serial.decision.no_workers"));
	EXPECT_EQ(snapshot.at("test.operator_serial.workers").totalValue, 1);
}

TEST(ParallelOperatorExecutorTest, AutomaticRangeSerialFallbackUsesSinglePartition) {
	graph::concurrent::ThreadPool pool(4);
	size_t workerCalls = 0;
	size_t mergeCalls = 0;
	graph::concurrent::ParallelRangePartition observedRange;

	const bool ok = graph::concurrent::ParallelOperatorExecutor::runRangePartitions<SumState>(
			10,
			20,
			&pool,
			{.phase = "test.operator_auto_serial",
			 .estimatedItems = 10,
			 .minPartitions = 2,
			 .minItems = 1000},
			[&](const graph::concurrent::ParallelRangePartition &range, SumState &state) {
				++workerCalls;
				observedRange = range;
				state.sum = static_cast<int64_t>(range.size());
			},
			[&](size_t partition, SumState &state) {
				++mergeCalls;
				EXPECT_EQ(partition, 0U);
				EXPECT_EQ(state.sum, 10);
			});

	EXPECT_TRUE(ok);
	EXPECT_EQ(workerCalls, 1U);
	EXPECT_EQ(mergeCalls, 1U);
	EXPECT_EQ(observedRange.begin, 10U);
	EXPECT_EQ(observedRange.end, 20U);
}

TEST(ParallelOperatorExecutorTest, FailedSerialRunProfilesOnlyExecutedTasks) {
	graph::concurrent::ThreadPool pool(1);
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();

	const bool ok = graph::concurrent::ParallelOperatorExecutor::runIndexedPartitions<SumState>(
			5,
			&pool,
			{.phase = "test.operator_failed", .estimatedItems = 5, .minPartitions = 2, .minItems = 1},
			[](size_t partition, SumState &) {
				return partition < 1;
			},
			[](size_t, SumState &) {});

	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_FALSE(ok);
	ASSERT_TRUE(snapshot.contains("test.operator_failed.task"));
	EXPECT_EQ(snapshot.at("test.operator_failed.task").calls, 2U);
	EXPECT_FALSE(snapshot.contains("test.operator_failed.merge"));
}
