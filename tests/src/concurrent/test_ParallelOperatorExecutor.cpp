#include "graph/concurrent/ParallelOperatorExecutor.hpp"

#include <atomic>
#include <limits>
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

	struct RangeBoolWorker {
		bool ok = true;

		bool operator()(const graph::concurrent::ParallelRangePartition &range, SumState &state) const {
			state.sum = static_cast<int64_t>(range.size());
			return ok;
		}
	};

	struct IndexedBoolWorker {
		size_t failingPartition = std::numeric_limits<size_t>::max();
		std::atomic<size_t> *calls = nullptr;

		bool operator()(size_t partition, SumState &state) const {
			if (calls) {
				calls->fetch_add(1, std::memory_order_relaxed);
			}
			state.sum = static_cast<int64_t>(partition + 1);
			return partition != failingPartition;
		}
	};

	struct SummingMerger {
		int64_t *sum = nullptr;
		size_t *calls = nullptr;

		void operator()(size_t, SumState &state) const {
			++*calls;
			*sum += state.sum;
		}
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

TEST(ParallelOperatorExecutorTest, RangePartitionBuilderHandlesEmptyAndClampedCounts) {
	EXPECT_TRUE(graph::concurrent::buildRangePartitions(5, 5, 4).empty());
	EXPECT_TRUE(graph::concurrent::buildRangePartitions(9, 3, 4).empty());
	EXPECT_TRUE(graph::concurrent::buildRangePartitions(0, 8, 0).empty());

	const auto partitions = graph::concurrent::buildRangePartitions(3, 8, 20);
	ASSERT_EQ(partitions.size(), 5UL);
	EXPECT_EQ(partitions.front().begin, 3UL);
	EXPECT_EQ(partitions.front().end, 4UL);
	EXPECT_EQ(partitions.back().begin, 7UL);
	EXPECT_EQ(partitions.back().end, 8UL);
	for (const auto &partition : partitions) {
		EXPECT_FALSE(partition.empty());
		EXPECT_EQ(partition.size(), 1UL);
	}
}

TEST(ParallelOperatorExecutorTest, RangePartitionHelpersHandleReversedBoundsAndGranularity) {
	const graph::concurrent::ParallelRangePartition reversed{.begin = 9, .end = 3};
	EXPECT_TRUE(reversed.empty());
	EXPECT_EQ(reversed.size(), 0U);

	graph::concurrent::ThreadPool pool(4);
	EXPECT_EQ(graph::concurrent::detail::chooseRangePartitionCount(0, &pool, {}), 0U);
	EXPECT_EQ(graph::concurrent::detail::chooseRangePartitionCount(
				 17, &pool, {.minItemsPerWorker = 9, .maxWorkers = 2}),
			 2U);
	EXPECT_EQ(graph::concurrent::detail::chooseRangePartitionCount(17, nullptr, {.minItemsPerWorker = 5}), 4U);
}

TEST(ParallelOperatorExecutorTest, EmptyPartitionRunsReturnSuccessWithoutWorkerOrMerge) {
	graph::concurrent::ThreadPool pool(4);
	size_t workerCalls = 0;
	size_t mergeCalls = 0;

	const bool indexedOk = graph::concurrent::ParallelOperatorExecutor::runIndexedPartitions<SumState>(
			0,
			&pool,
			{.phase = "test.operator_empty_indexed", .estimatedItems = 0},
			[&](size_t, SumState &) {
				++workerCalls;
				return true;
			},
			[&](size_t, SumState &) { ++mergeCalls; });
	const bool rangeOk = graph::concurrent::ParallelOperatorExecutor::runRangePartitions<SumState>(
			4,
			4,
			&pool,
			{.phase = "test.operator_empty_range", .estimatedItems = 0},
			[&](const graph::concurrent::ParallelRangePartition &, SumState &) {
				++workerCalls;
				return true;
			},
			[&](size_t, SumState &) { ++mergeCalls; });

	EXPECT_TRUE(indexedOk);
	EXPECT_TRUE(rangeOk);
	EXPECT_EQ(workerCalls, 0UL);
	EXPECT_EQ(mergeCalls, 0UL);
}

TEST(ParallelOperatorExecutorTest, ParallelWorkerFailureReturnsFalseWithoutMerging) {
	graph::concurrent::ThreadPool pool(4);
	std::atomic<size_t> workerCalls{0};
	size_t mergeCalls = 0;

	const bool ok = graph::concurrent::ParallelOperatorExecutor::runIndexedPartitions<SumState>(
			16,
			&pool,
			{.phase = "test.operator_parallel_failure",
			 .workloadKind = graph::concurrent::ParallelWorkloadKind::PWK_CPU_BOUND,
			 .estimatedItems = 16000,
			 .minPartitions = 2,
			 .minItems = 2,
			 .minItemsPerWorker = 256},
			[&](size_t partition, SumState &state) {
				workerCalls.fetch_add(1, std::memory_order_relaxed);
				state.sum = static_cast<int64_t>(partition);
				return partition != 3;
			},
			[&](size_t, SumState &) { ++mergeCalls; });

	EXPECT_FALSE(ok);
	EXPECT_GT(workerCalls.load(), 0UL);
	EXPECT_EQ(mergeCalls, 0UL);
}

TEST(ParallelOperatorExecutorTest, AutomaticRangeParallelPathAcceptsBoolWorkers) {
	graph::concurrent::ThreadPool pool(4);
	std::vector<int> values(2048, 1);
	int64_t total = 0;

	const bool ok = graph::concurrent::ParallelOperatorExecutor::runRangePartitions<SumState>(
			0,
			values.size(),
			&pool,
			{.phase = "test.operator_auto_parallel_bool",
			 .workloadKind = graph::concurrent::ParallelWorkloadKind::PWK_CPU_BOUND,
			 .estimatedItems = values.size(),
			 .minPartitions = 2,
			 .minItems = 2,
			 .minItemsPerWorker = 128},
			[&](const graph::concurrent::ParallelRangePartition &range, SumState &state) {
				for (size_t i = range.begin; i < range.end; ++i) {
					state.sum += values[i];
				}
				return true;
			},
			[&](size_t, SumState &state) { total += state.sum; });

	EXPECT_TRUE(ok);
	EXPECT_EQ(total, static_cast<int64_t>(values.size()));
}

TEST(ParallelOperatorExecutorTest, AutomaticRangeSerialBoolWorkerCanFailBeforeMerge) {
	graph::concurrent::ThreadPool pool(4);
	size_t mergeCalls = 0;

	const auto options = graph::concurrent::ParallelOperatorOptions{
			.phase = "test.operator_serial_bool",
			.estimatedItems = 8,
			.minPartitions = 2,
			.minItems = 1000};
	const bool ok = graph::concurrent::ParallelOperatorExecutor::runRangePartitions<SumState>(
			0,
			8,
			&pool,
			options,
			RangeBoolWorker{true},
			[&](size_t partition, SumState &state) {
				++mergeCalls;
				EXPECT_EQ(partition, 0U);
				EXPECT_EQ(state.sum, 8);
			});
	const bool failed = graph::concurrent::ParallelOperatorExecutor::runRangePartitions<SumState>(
			0,
			8,
			&pool,
			options,
			RangeBoolWorker{false},
			[&](size_t, SumState &) { ++mergeCalls; });

	EXPECT_TRUE(ok);
	EXPECT_FALSE(failed);
	EXPECT_EQ(mergeCalls, 1U);
}

TEST(ParallelOperatorExecutorTest, AutomaticRangeNoPoolHonorsMaxWorkersAndProfilesLargeEstimates) {
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();

	size_t workerCalls = 0;
	size_t mergeCalls = 0;
	const bool ok = graph::concurrent::ParallelOperatorExecutor::runRangePartitions<SumState>(
			2,
			7,
			nullptr,
			{.phase = "test.operator_no_pool",
			 .estimatedItems = std::numeric_limits<size_t>::max(),
			 .estimatedBytes = std::numeric_limits<size_t>::max(),
			 .minPartitions = 2,
			 .minItems = 2,
			 .maxWorkers = 3},
			[&](const graph::concurrent::ParallelRangePartition &range, SumState &state) {
				++workerCalls;
				state.sum = static_cast<int64_t>(range.end - range.begin);
			},
			[&](size_t partition, SumState &state) {
				++mergeCalls;
				EXPECT_EQ(partition, 0U);
				EXPECT_EQ(state.sum, 5);
			});

	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_TRUE(ok);
	EXPECT_EQ(workerCalls, 1U);
	EXPECT_EQ(mergeCalls, 1U);
	ASSERT_TRUE(snapshot.contains("test.operator_no_pool.workers"));
	EXPECT_EQ(snapshot.at("test.operator_no_pool.workers").totalValue, 1);
	ASSERT_TRUE(snapshot.contains("test.operator_no_pool.estimated_items"));
	EXPECT_EQ(snapshot.at("test.operator_no_pool.estimated_items").totalValue,
			  std::numeric_limits<int64_t>::max());
}

TEST(ParallelOperatorExecutorTest, ExplicitDecisionPreservesSerialAndParallelFailureContracts) {
	graph::concurrent::ThreadPool pool(4);
	const graph::concurrent::ParallelOperatorOptions options{
			.phase = "test.operator_explicit_decision", .estimatedItems = 4, .minPartitions = 2, .minItems = 1};
	const graph::concurrent::ParallelWorkEstimate estimate{
			.partitions = 4, .estimatedItems = 4, .minPartitions = 2, .minItems = 1};
	int64_t total = 0;
	size_t mergeCalls = 0;
	std::atomic<size_t> workerCalls{0};
	SummingMerger merger{.sum = &total, .calls = &mergeCalls};

	const graph::concurrent::ParallelExecutionDecision serialDecision{
		.useParallel = false,
		.workerCount = 0,
		.baselineWorkerCount = 1,
		.hardWorkerLimit = 1,
		.reason = graph::concurrent::ParallelDecisionReason::PDR_INSUFFICIENT_GRANULARITY};
	const bool serialOk = graph::concurrent::runOperatorIndexedPartitionsWithDecision<SumState>(
			4, &pool, options, estimate, serialDecision, IndexedBoolWorker{.calls = &workerCalls}, merger);

	EXPECT_TRUE(serialOk);
	EXPECT_EQ(workerCalls.load(std::memory_order_relaxed), 4U);
	EXPECT_EQ(mergeCalls, 4U);
	EXPECT_EQ(total, 10);

	total = 0;
	mergeCalls = 0;
	workerCalls.store(0, std::memory_order_relaxed);
	const bool serialFailed = graph::concurrent::runOperatorIndexedPartitionsWithDecision<SumState>(
			4, &pool, options, estimate, serialDecision,
			IndexedBoolWorker{.failingPartition = 1, .calls = &workerCalls}, merger);

	EXPECT_FALSE(serialFailed);
	EXPECT_EQ(workerCalls.load(std::memory_order_relaxed), 2U);
	EXPECT_EQ(mergeCalls, 0U);

	workerCalls.store(0, std::memory_order_relaxed);
	const graph::concurrent::ParallelExecutionDecision parallelDecision{
		.useParallel = true,
		.workerCount = 2,
		.baselineWorkerCount = 2,
		.hardWorkerLimit = 2,
		.reason = graph::concurrent::ParallelDecisionReason::PDR_PARALLEL};
	const bool parallelFailed = graph::concurrent::runOperatorIndexedPartitionsWithDecision<SumState>(
			8, &pool, options, {.partitions = 8, .estimatedItems = 8, .minPartitions = 2, .minItems = 1}, parallelDecision,
			IndexedBoolWorker{.failingPartition = 0, .calls = &workerCalls}, merger);

	EXPECT_FALSE(parallelFailed);
	EXPECT_GT(workerCalls.load(std::memory_order_relaxed), 0U);
	EXPECT_EQ(mergeCalls, 0U);
}

TEST(ParallelOperatorExecutorTest, ExplicitRangeBoolFailureAndAutomaticEmptyRangeDoNotMerge) {
	graph::concurrent::ThreadPool pool(4);
	int64_t total = 0;
	size_t mergeCalls = 0;
	SummingMerger merger{.sum = &total, .calls = &mergeCalls};
	const graph::concurrent::ParallelOperatorOptions options{
			.phase = "test.operator_range_bool",
			.estimatedItems = 8,
			.minPartitions = 2,
			.minItems = 1,
			.minItemsPerWorker = 2};

	const bool explicitFailed = graph::concurrent::ParallelOperatorExecutor::runRangePartitions<SumState>(
			10, 18, 2, &pool, options, RangeBoolWorker{false}, merger);
	const bool explicitSucceeded = graph::concurrent::ParallelOperatorExecutor::runRangePartitions<SumState>(
			10, 18, 2, &pool, options, RangeBoolWorker{true}, merger);
	const bool automaticSucceeded = graph::concurrent::ParallelOperatorExecutor::runRangePartitions<SumState>(
			10, 18, &pool, options, RangeBoolWorker{true}, merger);
	const bool automaticEmpty = graph::concurrent::ParallelOperatorExecutor::runRangePartitions<SumState>(
			18, 10, &pool, options, RangeBoolWorker{true}, merger);

	EXPECT_FALSE(explicitFailed);
	EXPECT_TRUE(explicitSucceeded);
	EXPECT_TRUE(automaticSucceeded);
	EXPECT_TRUE(automaticEmpty);
	EXPECT_EQ(mergeCalls, 6U);
	EXPECT_EQ(total, 16);
}
