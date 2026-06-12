#include "graph/concurrent/ParallelScanExecutor.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/debug/PerfTrace.hpp"

namespace {

	struct SumState {
		int64_t sum = 0;
	};

} // namespace

TEST(ParallelScanExecutorTest, RunsPartitionLocalStateAndMergesInOrder) {
	graph::concurrent::ThreadPool pool(4);
	std::vector<int64_t> merged;
	merged.reserve(8);

	const bool ok = graph::concurrent::runIndexedPartitions<SumState>(
			8, &pool, {.phase = "test.parallel_scan", .estimatedItems = 1024, .minPartitions = 2, .minItems = 2},
			[](size_t partition, SumState &state) { state.sum = static_cast<int64_t>(partition * 10); },
			[&](size_t, SumState &state) { merged.push_back(state.sum); });

	EXPECT_TRUE(ok);
	EXPECT_EQ(merged, (std::vector<int64_t>{0, 10, 20, 30, 40, 50, 60, 70}));
}

TEST(ParallelScanExecutorTest, ReturnsSuccessForEmptyPartitionRange) {
	graph::concurrent::ThreadPool pool(4);
	bool workerCalled = false;
	bool mergerCalled = false;

	const bool ok = graph::concurrent::runIndexedPartitions<SumState>(
			0, &pool, {.phase = "test.empty_parallel_scan", .estimatedItems = 1024, .minPartitions = 2, .minItems = 2},
			[&](size_t, SumState &) { workerCalled = true; }, [&](size_t, SumState &) { mergerCalled = true; });

	EXPECT_TRUE(ok);
	EXPECT_FALSE(workerCalled);
	EXPECT_FALSE(mergerCalled);
}

TEST(ParallelScanExecutorTest, SerialFallbackStopsAfterFailure) {
	graph::concurrent::ThreadPool pool(1);
	std::vector<size_t> visited;
	bool merged = false;

	const bool ok = graph::concurrent::runIndexedPartitions<SumState>(
			5, &pool,
			{.phase = "test.serial_parallel_scan_failure", .estimatedItems = 1, .minPartitions = 2, .minItems = 10},
			[&](size_t partition, SumState &) {
				visited.push_back(partition);
				return partition != 2;
			},
			[&](size_t, SumState &) { merged = true; });

	EXPECT_FALSE(ok);
	EXPECT_EQ(visited, (std::vector<size_t>{0U, 1U, 2U}));
	EXPECT_FALSE(merged);
}

TEST(ParallelScanExecutorTest, PropagatesPartitionFailureWithoutMerging) {
	graph::concurrent::ThreadPool pool(4);
	std::atomic<int> visited{0};
	bool merged = false;

	const bool ok = graph::concurrent::runIndexedPartitions<SumState>(
			4, &pool,
			{.phase = "test.parallel_scan_failure", .estimatedItems = 1024, .minPartitions = 2, .minItems = 2},
			[&](size_t partition, SumState &) {
				visited.fetch_add(1, std::memory_order_relaxed);
				return partition != 2;
			},
			[&](size_t, SumState &) { merged = true; });

	EXPECT_FALSE(ok);
	EXPECT_GT(visited.load(std::memory_order_relaxed), 0);
	EXPECT_FALSE(merged);
}

TEST(ParallelScanExecutorTest, ParallelFailureSkipsRemainingPartitionInChunk) {
	graph::concurrent::ThreadPool pool(2);
	std::atomic<int> visited{0};
	bool merged = false;

	const bool ok = graph::concurrent::runIndexedPartitions<SumState>(
			4, &pool, {.phase = "test.parallel_scan_skip_after_failure", .estimatedItems = 1024, .maxWorkers = 2},
			[&](size_t partition, SumState &) {
				visited.fetch_add(1, std::memory_order_relaxed);
				return partition != 0;
			},
			[&](size_t, SumState &) { merged = true; });

	EXPECT_FALSE(ok);
	EXPECT_LT(visited.load(std::memory_order_relaxed), 4);
	EXPECT_FALSE(merged);
}

TEST(ParallelScanExecutorTest, EmptyPhaseDoesNotEmitProfilePhases) {
	graph::concurrent::ThreadPool pool(2);
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();

	const bool ok = graph::concurrent::runIndexedPartitions<SumState>(
			2, &pool, {.phase = "", .estimatedItems = 128, .minPartitions = 2, .minItems = 2},
			[](size_t partition, SumState &state) { state.sum = static_cast<int64_t>(partition); },
			[](size_t, SumState &) {});

	auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_TRUE(ok);
	EXPECT_TRUE(snapshot.empty());
}

TEST(ParallelScanExecutorTest, EmitsTaskMergeAndTotalProfilePhases) {
	graph::concurrent::ThreadPool pool(2);
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();

	const bool ok = graph::concurrent::runIndexedPartitions<SumState>(
			3, &pool,
			{.phase = "test.profiled_parallel_scan", .estimatedItems = 256, .minPartitions = 2, .minItems = 2},
			[](size_t partition, SumState &state) { state.sum = static_cast<int64_t>(partition); },
			[](size_t, SumState &) {});

	auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_TRUE(ok);
	EXPECT_TRUE(snapshot.contains("test.profiled_parallel_scan"));
	EXPECT_TRUE(snapshot.contains("test.profiled_parallel_scan.task"));
	EXPECT_TRUE(snapshot.contains("test.profiled_parallel_scan.merge"));
	EXPECT_EQ(snapshot.at("test.profiled_parallel_scan.task").calls, 3U);
	EXPECT_EQ(snapshot.at("test.profiled_parallel_scan.merge").calls, 1U);
}

TEST(ParallelScanExecutorTest, HonorsMemoryScanWorkerCap) {
	graph::concurrent::ThreadPool pool(8);
	std::atomic<int> active{0};
	std::atomic<int> maxActive{0};

	const bool ok = graph::concurrent::runIndexedPartitions<SumState>(
			8, &pool,
			{.phase = "test.capped_parallel_scan",
			 .workloadKind = graph::concurrent::ParallelWorkloadKind::PWK_MEMORY_SCAN,
			 .estimatedItems = 1024,
			 .estimatedBytes = size_t{32} * 1024 * 1024,
			 .minPartitions = 2,
			 .minItems = 2,
			 .minBytesPerWorker = size_t{16} * 1024 * 1024},
			[&](size_t, SumState &) {
				const int current = active.fetch_add(1, std::memory_order_relaxed) + 1;
				int observed = maxActive.load(std::memory_order_relaxed);
				while (current > observed &&
					   !maxActive.compare_exchange_weak(observed, current, std::memory_order_relaxed,
														std::memory_order_relaxed)) {
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				active.fetch_sub(1, std::memory_order_relaxed);
			},
			[](size_t, SumState &) {});

	EXPECT_TRUE(ok);
	EXPECT_LE(maxActive.load(std::memory_order_relaxed), 2);
}

TEST(ParallelScanExecutorTest, RecordsAdaptiveTelemetryForSuccessfulRuns) {
	graph::concurrent::ThreadPool pool(4);
	const graph::concurrent::ParallelScanOptions options{
			.phase = "test.adaptive_parallel_scan",
			.workloadKind = graph::concurrent::ParallelWorkloadKind::PWK_STORAGE_SCAN,
			.estimatedItems = 4096,
			.estimatedBytes = size_t{8} * 1024 * 1024,
			.minPartitions = 2,
			.minItems = 2,
			.maxWorkers = 2};

	const bool ok = graph::concurrent::runIndexedPartitions<SumState>(
			4, &pool, options,
			[](size_t partition, SumState &state) { state.sum = static_cast<int64_t>(partition); },
			[](size_t, SumState &) {});

	const graph::concurrent::ParallelWorkEstimate estimate{
			.workloadKind = options.workloadKind,
			.partitions = 4,
			.estimatedItems = options.estimatedItems,
			.estimatedBytes = options.estimatedBytes,
			.minPartitions = options.minPartitions,
			.minItems = options.minItems,
			.minItemsPerWorker = options.minItemsPerWorker,
			.minBytesPerWorker = options.minBytesPerWorker,
			.maxWorkers = options.maxWorkers};
	const auto stats = pool.adaptivePolicyState().statsFor(estimate, 2);

	EXPECT_TRUE(ok);
	EXPECT_EQ(stats.samples, 1U);
	EXPECT_GT(stats.throughputEwma, 0.0);
	EXPECT_GE(stats.elapsedNsEwma, 1.0);
}
