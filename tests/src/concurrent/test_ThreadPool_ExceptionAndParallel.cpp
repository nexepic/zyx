/**
 * @file test_ThreadPool_ExceptionAndParallel.cpp
 * @brief Additional branch coverage tests for ThreadPool.hpp.
 *        Covers: single-thread void-returning exception path,
 *        parallelFor begin > end, and multi-thread total <= 1 sequential fallback.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <thread>

#include "graph/concurrent/ParallelExecutionPolicy.hpp"
#include "graph/concurrent/ThreadPool.hpp"

using graph::concurrent::ceilDiv;
using graph::concurrent::decideParallelExecution;
using graph::concurrent::ParallelAdaptivePolicyConfig;
using graph::concurrent::ParallelDecisionReason;
using graph::concurrent::ParallelExecutionTelemetry;
using graph::concurrent::ParallelExecutionPolicyConfig;
using graph::concurrent::ParallelPolicyMode;
using graph::concurrent::ScopedParallelExecutionTelemetry;
using graph::concurrent::ParallelWorkEstimate;
using graph::concurrent::ParallelWorkloadKind;
using graph::concurrent::shouldParallelize;
using graph::concurrent::ThreadPool;
using graph::concurrent::ThreadPoolSizeSource;
using graph::concurrent::AdaptiveParallelPolicyState;

// Single-thread void submit that throws — covers the catch path for void returns.
TEST(ThreadPoolExceptionAndParallelTest, SingleThreadVoidSubmitExceptionPropagates) {
	ThreadPool pool(1);
	auto future = pool.submit([]() -> void { throw std::logic_error("void-boom"); });
	EXPECT_THROW(future.get(), std::logic_error);
}

// parallelFor with begin > end — immediate return.
TEST(ThreadPoolExceptionAndParallelTest, ParallelForBeginGreaterThanEnd) {
	ThreadPool pool(2);
	std::atomic<size_t> hits{0};
	pool.parallelFor(10, 5, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 0UL);
}

// Multi-thread parallelFor with exactly 1 element — total <= 1 sequential fallback.
TEST(ThreadPoolExceptionAndParallelTest, MultiThreadParallelForTotalOneSequentialFallback) {
	ThreadPool pool(4);
	EXPECT_FALSE(pool.isSingleThreaded());
	std::atomic<size_t> hits{0};
	pool.parallelFor(7, 8, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 1UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ExplicitZeroMaxWorkersUsesSafeSingleWorkerFallback) {
	ThreadPool pool(4);
	std::atomic<size_t> hits{0};
	pool.parallelFor(0, 3, 0, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 3UL);
}

// Multi-thread parallelFor with remainder > 0 (covers c < remainder branch in chunk loop).
TEST(ThreadPoolExceptionAndParallelTest, MultiThreadParallelForWithRemainder) {
	ThreadPool pool(3);
	std::atomic<size_t> hits{0};
	// 10 items / 3 threads = 3 chunks + 1 remainder
	pool.parallelFor(0, 10, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 10UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelForKeepsFirstExceptionWhenMultipleChunksThrow) {
	ThreadPool pool(4);
	std::atomic<size_t> readyChunks{0};

	EXPECT_THROW(pool.parallelFor(0, 8, 4, [&](size_t i) {
		if (i % 2 != 0) {
			return;
		}
		readyChunks.fetch_add(1, std::memory_order_acq_rel);
		while (readyChunks.load(std::memory_order_acquire) < 4) {
			std::this_thread::yield();
		}
		throw std::runtime_error("chunk failed");
	}), std::runtime_error);
	EXPECT_EQ(readyChunks.load(std::memory_order_acquire), 4UL);
}

// Single-thread non-void submit that throws — covers the catch path for non-void returns.
TEST(ThreadPoolExceptionAndParallelTest, SingleThreadNonVoidSubmitExceptionPropagates) {
	ThreadPool pool(1);
	auto future = pool.submit([]() -> int { throw std::runtime_error("nonvoid-boom"); });
	EXPECT_THROW(future.get(), std::runtime_error);
}

// Single-thread non-void submit returning a value — covers the non-void inline path.
TEST(ThreadPoolExceptionAndParallelTest, SingleThreadNonVoidSubmitReturnsValue) {
	ThreadPool pool(1);
	auto future = pool.submit([]() -> int { return 42; });
	EXPECT_EQ(future.get(), 42);
}

// Single-thread parallelFor (sequential path, threadCount_ <= 1).
TEST(ThreadPoolExceptionAndParallelTest, SingleThreadParallelForSequential) {
	ThreadPool pool(1);
	std::atomic<size_t> hits{0};
	pool.parallelFor(0, 5, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 5UL);
}

// parallelFor with begin == end — immediate return.
TEST(ThreadPoolExceptionAndParallelTest, ParallelForBeginEqualsEnd) {
	ThreadPool pool(2);
	std::atomic<size_t> hits{0};
	pool.parallelFor(5, 5, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 0UL);
}

// Verify resolveThreadCount: request 0 when hardware_concurrency would be 0 → fallback to 2.
// This is tested indirectly but let's test the isSingleThreaded / getThreadCount accessors.
TEST(ThreadPoolExceptionAndParallelTest, SingleThreadedAccessors) {
	ThreadPool pool(1);
	EXPECT_TRUE(pool.isSingleThreaded());
	EXPECT_EQ(pool.getThreadCount(), 1UL);
}

// Multi-thread submit of non-void task — covers multi-thread packaged_task path.
TEST(ThreadPoolExceptionAndParallelTest, MultiThreadNonVoidSubmit) {
	ThreadPool pool(2);
	auto future = pool.submit([]() -> int { return 99; });
	EXPECT_EQ(future.get(), 99);
}

// Multi-thread submit of void task — covers multi-thread packaged_task void path.
TEST(ThreadPoolExceptionAndParallelTest, MultiThreadVoidSubmit) {
	ThreadPool pool(2);
	std::atomic<bool> done{false};
	auto future = pool.submit([&done]() { done.store(true); });
	future.get();
	EXPECT_TRUE(done.load());
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionPolicyRequiresUsefulWork) {
	ThreadPool singleThread(1);
	ThreadPool multiThread(4);

	EXPECT_FALSE(shouldParallelize(nullptr, 4, 100));
	EXPECT_FALSE(shouldParallelize(&singleThread, 4, 100));
	EXPECT_FALSE(shouldParallelize(&multiThread, 1, 100));
	EXPECT_FALSE(shouldParallelize(&multiThread, 4, 1, 2, 2));
	EXPECT_TRUE(shouldParallelize(&multiThread, 4, 100, 2, 2));
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionReportsNoWorkers) {
	ThreadPool singleThread(1);

	const auto nullDecision = decideParallelExecution(nullptr, {.partitions = 4, .estimatedItems = 100});
	EXPECT_FALSE(nullDecision.useParallel);
	EXPECT_EQ(nullDecision.workerCount, 1UL);
	EXPECT_EQ(nullDecision.reason, ParallelDecisionReason::PDR_NO_WORKERS);

	const auto singleThreadDecision = decideParallelExecution(&singleThread, {.partitions = 4, .estimatedItems = 100});
	EXPECT_FALSE(singleThreadDecision.useParallel);
	EXPECT_EQ(singleThreadDecision.reason, ParallelDecisionReason::PDR_NO_WORKERS);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionExplainsRejectedWork) {
	ThreadPool pool(4);

	const auto partitionDecision =
			decideParallelExecution(&pool, {.partitions = 1, .estimatedItems = 100, .minPartitions = 2, .minItems = 2});
	EXPECT_FALSE(partitionDecision.useParallel);
	EXPECT_EQ(partitionDecision.reason, ParallelDecisionReason::PDR_INSUFFICIENT_PARTITIONS);

	const auto itemDecision =
			decideParallelExecution(&pool, {.partitions = 4, .estimatedItems = 1, .minPartitions = 2, .minItems = 2});
	EXPECT_FALSE(itemDecision.useParallel);
	EXPECT_EQ(itemDecision.reason, ParallelDecisionReason::PDR_INSUFFICIENT_ITEMS);

	const auto granularityDecision = decideParallelExecution(
			&pool,
			{.partitions = 4, .estimatedItems = 100, .minPartitions = 2, .minItems = 2, .minItemsPerWorker = 80});
	EXPECT_FALSE(granularityDecision.useParallel);
	EXPECT_EQ(granularityDecision.reason, ParallelDecisionReason::PDR_INSUFFICIENT_GRANULARITY);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionPolicyCoversDefaultWorkloadAndTelemetryGuards) {
	ThreadPool pool(4);
	ParallelExecutionPolicyConfig config;
	config.generalItemsPerWorker = 10;

	auto estimate = ParallelWorkEstimate{
			.workloadKind = static_cast<ParallelWorkloadKind>(999),
			.partitions = 8,
			.estimatedItems = 45,
			.minPartitions = 2,
			.minItems = 2};
	const auto decision = decideParallelExecution(&pool, estimate, config);
	EXPECT_TRUE(decision.useParallel);
	EXPECT_EQ(decision.workerCount, 4UL);
	EXPECT_EQ(decision.reason, ParallelDecisionReason::PDR_PARALLEL);

	recordParallelExecution(nullptr, estimate, decision, 10);
	recordParallelExecution(&pool, estimate, decision, 0);

	{
		ScopedParallelExecutionTelemetry telemetry(&pool, estimate, decision);
		telemetry.dismiss();
	}
	{
		ScopedParallelExecutionTelemetry telemetry(&pool, estimate, decision);
		telemetry.setTaskNs(3);
		telemetry.setMergeNs(2);
		telemetry.markCompleted();
	}
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionUsesAllWorkersForGeneralWork) {
	ThreadPool pool(8);

	const auto decision = decideParallelExecution(&pool, {.workloadKind = ParallelWorkloadKind::PWK_CPU_BOUND,
														  .partitions = 16,
														  .estimatedItems = 10000,
														  .minPartitions = 2,
														  .minItems = 2});

	EXPECT_TRUE(decision.useParallel);
	EXPECT_EQ(decision.workerCount, 8UL);
	EXPECT_EQ(decision.reason, ParallelDecisionReason::PDR_PARALLEL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionCapsMemoryScanByBytes) {
	ThreadPool pool(8);

	const auto decision = decideParallelExecution(&pool, {.workloadKind = ParallelWorkloadKind::PWK_MEMORY_SCAN,
														  .partitions = 16,
														  .estimatedItems = 10000,
														  .estimatedBytes = size_t{64} * 1024 * 1024,
														  .minPartitions = 2,
														  .minItems = 2});

	EXPECT_TRUE(decision.useParallel);
	EXPECT_EQ(decision.workerCount, 4UL);
	EXPECT_EQ(decision.reason, ParallelDecisionReason::PDR_PARALLEL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionRejectsTinyMemoryScan) {
	ThreadPool pool(8);

	const auto decision = decideParallelExecution(&pool, {.workloadKind = ParallelWorkloadKind::PWK_MEMORY_SCAN,
														  .partitions = 16,
														  .estimatedItems = 10000,
														  .estimatedBytes = size_t{2} * 1024 * 1024,
														  .minPartitions = 2,
														  .minItems = 2});

	EXPECT_FALSE(decision.useParallel);
	EXPECT_EQ(decision.workerCount, 1UL);
	EXPECT_EQ(decision.reason, ParallelDecisionReason::PDR_INSUFFICIENT_GRANULARITY);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionHonorsExplicitMaxWorkers) {
	ThreadPool pool(8);

	const auto decision = decideParallelExecution(&pool, {.workloadKind = ParallelWorkloadKind::PWK_CPU_BOUND,
														  .partitions = 16,
														  .estimatedItems = 10000,
														  .minPartitions = 2,
														  .minItems = 2,
														  .maxWorkers = 3});

	EXPECT_TRUE(decision.useParallel);
	EXPECT_EQ(decision.workerCount, 3UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionCeilDivHandlesExactRemainderAndZeroDivisor) {
	EXPECT_EQ(ceilDiv(32, 16), 2UL);
	EXPECT_EQ(ceilDiv(33, 16), 3UL);
	EXPECT_EQ(ceilDiv(7, 0), 7UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionUsesExplicitByteGranularity) {
	ThreadPool pool(8);

	const auto decision = decideParallelExecution(&pool, {.workloadKind = ParallelWorkloadKind::PWK_STORAGE_SCAN,
														  .partitions = 16,
														  .estimatedItems = 10000,
														  .estimatedBytes = size_t{32} * 1024 * 1024,
														  .minPartitions = 2,
														  .minItems = 2,
														  .minBytesPerWorker = size_t{16} * 1024 * 1024});

	EXPECT_TRUE(decision.useParallel);
	EXPECT_EQ(decision.workerCount, 2UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionCapsStorageScanByDefaultBytes) {
	ThreadPool pool(8);

	const auto decision = decideParallelExecution(
			&pool,
			{.workloadKind = ParallelWorkloadKind::PWK_STORAGE_SCAN,
			 .partitions = 16,
			 .estimatedItems = 128,
			 .estimatedBytes = size_t{7} * 1024 * 1024,
			 .minPartitions = 2,
			 .minItems = 2});

	EXPECT_TRUE(decision.useParallel);
	EXPECT_EQ(decision.workerCount, 2UL);
	EXPECT_EQ(decision.hardWorkerLimit, 2UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionRejectsTinyStorageScanByDefaultBytes) {
	ThreadPool pool(8);

	const auto decision = decideParallelExecution(
			&pool,
			{.workloadKind = ParallelWorkloadKind::PWK_STORAGE_SCAN,
			 .partitions = 16,
			 .estimatedItems = 128,
			 .estimatedBytes = size_t{2} * 1024 * 1024,
			 .minPartitions = 2,
			 .minItems = 2});

	EXPECT_FALSE(decision.useParallel);
	EXPECT_EQ(decision.workerCount, 1UL);
	EXPECT_EQ(decision.reason, ParallelDecisionReason::PDR_INSUFFICIENT_GRANULARITY);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionUsesItemsPerWorkerCap) {
	ThreadPool pool(8);

	const auto decision = decideParallelExecution(&pool, {.workloadKind = ParallelWorkloadKind::PWK_CPU_BOUND,
														  .partitions = 16,
														  .estimatedItems = 550,
														  .minPartitions = 2,
														  .minItems = 2,
														  .minItemsPerWorker = 100});

	EXPECT_TRUE(decision.useParallel);
	EXPECT_EQ(decision.workerCount, 5UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionAppliesMemoryWorkerCapWithoutByteEstimate) {
	ThreadPool pool(8);

	const auto decision = decideParallelExecution(&pool, {.workloadKind = ParallelWorkloadKind::PWK_MEMORY_SCAN,
														  .partitions = 16,
														  .estimatedItems = 10000,
														  .estimatedBytes = 0,
														  .minPartitions = 2,
														  .minItems = 2});

	EXPECT_TRUE(decision.useParallel);
	EXPECT_EQ(decision.workerCount, 4UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionCapsMemoryIntensiveWork) {
	ThreadPool pool(8);

	const auto decision = decideParallelExecution(&pool, {.workloadKind = ParallelWorkloadKind::PWK_MEMORY_INTENSIVE,
														  .partitions = 16,
														  .estimatedItems = 100000,
														  .minPartitions = 2,
														  .minItems = 2});

	EXPECT_TRUE(decision.useParallel);
	EXPECT_EQ(decision.workerCount, 4UL);
	EXPECT_EQ(decision.reason, ParallelDecisionReason::PDR_PARALLEL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionCapsMemoryIntensiveWorkByBytes) {
	ThreadPool pool(8);

	const auto decision = decideParallelExecution(
			&pool,
			{.workloadKind = ParallelWorkloadKind::PWK_MEMORY_INTENSIVE,
			 .partitions = 16,
			 .estimatedItems = 100000,
			 .estimatedBytes = size_t{6} * 1024 * 1024,
			 .minPartitions = 2,
			 .minItems = 2});

	EXPECT_TRUE(decision.useParallel);
	EXPECT_EQ(decision.workerCount, 2UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionCapsAdjacencyTraversalByEstimatedEdges) {
	ThreadPool pool(8);

	const auto smallDecision = decideParallelExecution(
			&pool,
			{.workloadKind = ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
			 .partitions = 16,
			 .estimatedItems = 1024,
			 .minPartitions = 2,
			 .minItems = 2});
	EXPECT_FALSE(smallDecision.useParallel);
	EXPECT_EQ(smallDecision.reason, ParallelDecisionReason::PDR_INSUFFICIENT_GRANULARITY);

	const auto largeDecision = decideParallelExecution(
			&pool,
			{.workloadKind = ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
			 .partitions = 16,
			 .estimatedItems = 10'000,
			 .minPartitions = 2,
			 .minItems = 2});
	EXPECT_TRUE(largeDecision.useParallel);
	EXPECT_EQ(largeDecision.workerCount, 4UL);
	EXPECT_EQ(largeDecision.hardWorkerLimit, 4UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionLimitsDeepFrontierStateBaseline) {
	ThreadPool pool(8);

	const auto decision = decideParallelExecution(
			&pool,
			{.workloadKind = ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
			 .partitions = 16,
			 .estimatedItems = 100'000,
			 .estimatedBytes = 100'000 * size_t{32},
			 .minPartitions = 2,
			 .minItems = 2,
			 .estimatedStateBytesPerItem = 32,
			 .frontierWidth = 4096,
			 .traversalDepth = 2});

	EXPECT_TRUE(decision.useParallel);
	EXPECT_EQ(decision.baselineWorkerCount, 2UL);
	EXPECT_LE(decision.workerCount, 2UL);
	EXPECT_EQ(decision.reason, ParallelDecisionReason::PDR_PARALLEL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionLimitsBroadFrontierStateBaseline) {
	ThreadPool pool(8);

	const auto decision = decideParallelExecution(
			&pool,
			{.workloadKind = ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
			 .partitions = 16,
			 .estimatedItems = 100'000,
			 .estimatedBytes = 100'000 * size_t{32},
			 .minPartitions = 2,
			 .minItems = 2,
			 .estimatedStateBytesPerItem = 32,
			 .frontierWidth = 4096,
			 .traversalDepth = 0});

	EXPECT_TRUE(decision.useParallel);
	EXPECT_EQ(decision.baselineWorkerCount, 2UL);
	EXPECT_LE(decision.workerCount, 2UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionUsesPolicyConfigDefaults) {
	ThreadPool pool(8);
	ParallelExecutionPolicyConfig config;
	config.generalItemsPerWorker = 100;
	config.memoryScanBytesPerWorker = size_t{8} * 1024 * 1024;
	config.memoryScanMaxWorkers = 3;
	config.memoryIntensiveBytesPerWorker = size_t{16} * 1024 * 1024;
	config.memoryIntensiveMaxWorkers = 2;

	const auto generalDecision = decideParallelExecution(
			&pool,
			{.workloadKind = ParallelWorkloadKind::PWK_GENERAL,
			 .partitions = 16,
			 .estimatedItems = 450,
			 .minPartitions = 2,
			 .minItems = 2},
			config);
	EXPECT_TRUE(generalDecision.useParallel);
	EXPECT_EQ(generalDecision.workerCount, 4UL);

	const auto memoryDecision = decideParallelExecution(
			&pool,
			{.workloadKind = ParallelWorkloadKind::PWK_MEMORY_SCAN,
			 .partitions = 16,
			 .estimatedItems = 4096,
			 .estimatedBytes = size_t{64} * 1024 * 1024,
			 .minPartitions = 2,
			 .minItems = 2},
			config);
	EXPECT_TRUE(memoryDecision.useParallel);
	EXPECT_EQ(memoryDecision.workerCount, 3UL);

	const auto memoryIntensiveDecision = decideParallelExecution(
			&pool,
			{.workloadKind = ParallelWorkloadKind::PWK_MEMORY_INTENSIVE,
			 .partitions = 16,
			 .estimatedItems = 4096,
			 .estimatedBytes = size_t{64} * 1024 * 1024,
			 .minPartitions = 2,
			 .minItems = 2},
			config);
	EXPECT_TRUE(memoryIntensiveDecision.useParallel);
	EXPECT_EQ(memoryIntensiveDecision.workerCount, 2UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ParallelExecutionDecisionEstimateOverridesPolicyConfigDefaults) {
	ThreadPool pool(8);
	ParallelExecutionPolicyConfig config;
	config.cpuBoundItemsPerWorker = 1000;
	config.storageScanBytesPerWorker = size_t{64} * 1024 * 1024;
	config.storageScanMaxWorkers = 2;

	const auto cpuDecision = decideParallelExecution(
			&pool,
			{.workloadKind = ParallelWorkloadKind::PWK_CPU_BOUND,
			 .partitions = 16,
			 .estimatedItems = 450,
			 .minPartitions = 2,
			 .minItems = 2,
			 .minItemsPerWorker = 100},
			config);
	EXPECT_TRUE(cpuDecision.useParallel);
	EXPECT_EQ(cpuDecision.workerCount, 4UL);

	const auto storageDecision = decideParallelExecution(
			&pool,
			{.workloadKind = ParallelWorkloadKind::PWK_STORAGE_SCAN,
			 .partitions = 16,
			 .estimatedItems = 4096,
			 .estimatedBytes = size_t{96} * 1024 * 1024,
			 .minPartitions = 2,
			 .minItems = 2,
			 .minBytesPerWorker = size_t{32} * 1024 * 1024,
			 .maxWorkers = 5},
			config);
	EXPECT_TRUE(storageDecision.useParallel);
	EXPECT_EQ(storageDecision.workerCount, 3UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ThreadPoolHardwareInfoTracksManualAutoAndFallbackSources) {
	ThreadPool manualPool(4);
	EXPECT_EQ(manualPool.getHardwareInfo().requestedThreadCount, 4UL);
	EXPECT_EQ(manualPool.getHardwareInfo().resolvedThreadCount, 4UL);
	EXPECT_EQ(manualPool.getHardwareInfo().source, ThreadPoolSizeSource::TPSS_MANUAL);

	ThreadPool singleThreadPool(1);
	EXPECT_EQ(singleThreadPool.getHardwareInfo().resolvedThreadCount, 1UL);
	EXPECT_EQ(singleThreadPool.getHardwareInfo().source, ThreadPoolSizeSource::TPSS_FORCED_SINGLE_THREADED);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptiveParallelPolicyCanBeDisabled) {
	ThreadPool pool(8);
	pool.setParallelPolicyMode(ParallelPolicyMode::PPM_FIXED_HEURISTIC);

	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_MEMORY_SCAN,
			.partitions = 64,
			.estimatedItems = 100000,
			.estimatedBytes = size_t{256} * 1024 * 1024,
			.minPartitions = 2,
			.minItems = 2};

	for (int i = 0; i < 3; ++i) {
		pool.adaptivePolicyState().record(
				ParallelExecutionTelemetry{.estimate = estimate,
										   .workerCount = 8,
										   .elapsedNs = 10,
										   .completed = true});
	}

	const auto decision = decideParallelExecution(&pool, estimate);
	EXPECT_TRUE(decision.useParallel);
	EXPECT_FALSE(decision.usedAdaptiveRecommendation);
	EXPECT_EQ(decision.workerCount, 4UL);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptiveParallelPolicyRecommendsObservedBetterWorkerCount) {
	ThreadPool pool(8);

	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_MEMORY_SCAN,
			.partitions = 64,
			.estimatedItems = 100000,
			.estimatedBytes = size_t{256} * 1024 * 1024,
			.minPartitions = 2,
			.minItems = 2};

	for (int i = 0; i < 2; ++i) {
		pool.adaptivePolicyState().record(
				ParallelExecutionTelemetry{.estimate = estimate,
										   .workerCount = 4,
										   .elapsedNs = 200,
										   .completed = true});
		pool.adaptivePolicyState().record(
				ParallelExecutionTelemetry{.estimate = estimate,
										   .workerCount = 8,
										   .elapsedNs = 100,
										   .completed = true});
	}

	const auto decision = decideParallelExecution(&pool, estimate);
	EXPECT_TRUE(decision.useParallel);
	EXPECT_TRUE(decision.usedAdaptiveRecommendation);
	EXPECT_EQ(decision.baselineWorkerCount, 4UL);
	EXPECT_EQ(decision.workerCount, 8UL);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptiveParallelPolicyRequiresReliableSamples) {
	ThreadPool pool(8);

	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_MEMORY_SCAN,
			.partitions = 64,
			.estimatedItems = 100000,
			.estimatedBytes = size_t{256} * 1024 * 1024,
			.minPartitions = 2,
			.minItems = 2};

	pool.adaptivePolicyState().record(
			ParallelExecutionTelemetry{.estimate = estimate,
									   .workerCount = 8,
									   .elapsedNs = 100,
									   .completed = true});

	const auto decision = decideParallelExecution(&pool, estimate);
	EXPECT_TRUE(decision.useParallel);
	EXPECT_FALSE(decision.usedAdaptiveRecommendation);
	EXPECT_EQ(decision.workerCount, 4UL);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptiveParallelPolicyRespectsExplicitMaxWorkersAsHardLimit) {
	ThreadPool pool(8);

	const auto observedEstimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_MEMORY_SCAN,
			.partitions = 64,
			.estimatedItems = 100000,
			.estimatedBytes = size_t{256} * 1024 * 1024,
			.minPartitions = 2,
			.minItems = 2};
	const auto limitedEstimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_MEMORY_SCAN,
			.partitions = 64,
			.estimatedItems = 100000,
			.estimatedBytes = size_t{256} * 1024 * 1024,
			.minPartitions = 2,
			.minItems = 2,
			.maxWorkers = 4};

	for (int i = 0; i < 2; ++i) {
		pool.adaptivePolicyState().record(
				ParallelExecutionTelemetry{.estimate = observedEstimate,
										   .workerCount = 8,
										   .elapsedNs = 100,
										   .completed = true});
	}

	const auto decision = decideParallelExecution(&pool, limitedEstimate);
	EXPECT_TRUE(decision.useParallel);
	EXPECT_FALSE(decision.usedAdaptiveRecommendation);
	EXPECT_EQ(decision.workerCount, 4UL);
	EXPECT_EQ(decision.hardWorkerLimit, 4UL);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptiveParallelPolicyExploresNextWorkerAfterBaselineSamples) {
	ThreadPool pool(8);

	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_MEMORY_SCAN,
			.partitions = 64,
			.estimatedItems = 100000,
			.estimatedBytes = size_t{256} * 1024 * 1024,
			.minPartitions = 2,
			.minItems = 2};

	for (int i = 0; i < 2; ++i) {
		pool.adaptivePolicyState().record(
				ParallelExecutionTelemetry{.estimate = estimate,
										   .workerCount = 4,
										   .elapsedNs = 200,
										   .completed = true});
	}

	const auto decision = decideParallelExecution(&pool, estimate);
	EXPECT_TRUE(decision.useParallel);
	EXPECT_TRUE(decision.usedAdaptiveRecommendation);
	EXPECT_EQ(decision.workerCount, 8UL);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptiveParallelPolicyConfigValidationKeepsSafeValues) {
	ThreadPool pool(4);
	ParallelAdaptivePolicyConfig config;
	config.ewmaAlpha = 2.0;
	config.minImprovementRatio = 0.5;
	config.maxMergeRatioForRecommendation = 0.0;
	config.minSamplesBeforeRecommend = 1;

	pool.adaptivePolicyState().setConfig(config);
	const auto applied = pool.adaptivePolicyState().config();

	EXPECT_EQ(applied.minSamplesBeforeRecommend, 1U);
	EXPECT_DOUBLE_EQ(applied.ewmaAlpha, 0.25);
	EXPECT_DOUBLE_EQ(applied.minImprovementRatio, 1.0);
	EXPECT_DOUBLE_EQ(applied.maxMergeRatioForRecommendation, 0.45);

	config.maxMergeRatioForRecommendation = 2.0;
	pool.adaptivePolicyState().setConfig(config);
	EXPECT_DOUBLE_EQ(pool.adaptivePolicyState().config().maxMergeRatioForRecommendation, 0.45);
}

TEST(ThreadPoolExceptionAndParallelTest, ScopedTelemetryRecordsOnlyCompletedWork) {
	ThreadPool pool(4);
	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_CPU_BOUND,
			.partitions = 4,
			.estimatedItems = 10000,
			.minPartitions = 2,
			.minItems = 2};
	const auto decision = decideParallelExecution(&pool, estimate);
	ASSERT_TRUE(decision.useParallel);

	{
		ScopedParallelExecutionTelemetry telemetry(&pool, estimate, decision);
	}
	EXPECT_EQ(pool.adaptivePolicyState().statsFor(estimate, decision.workerCount).samples, 0U);

	{
		ScopedParallelExecutionTelemetry telemetry(&pool, estimate, decision);
		telemetry.setTaskNs(1);
		telemetry.setMergeNs(1);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		telemetry.markCompleted();
	}
	const auto stats = pool.adaptivePolicyState().statsFor(estimate, decision.workerCount);
	EXPECT_EQ(stats.samples, 1U);
	EXPECT_GT(stats.throughputEwma, 0.0);
	EXPECT_GT(stats.mergeRatioEwma, 0.0);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptivePolicyStateExposesModeResetAndSafeConfig) {
	AdaptiveParallelPolicyState state;
	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_GENERAL,
			.partitions = 4,
			.estimatedItems = 2048,
			.minPartitions = 2,
			.minItems = 2};

	EXPECT_EQ(state.mode(), ParallelPolicyMode::PPM_ADAPTIVE);
	state.setMode(ParallelPolicyMode::PPM_FIXED_HEURISTIC);
	EXPECT_EQ(state.mode(), ParallelPolicyMode::PPM_FIXED_HEURISTIC);

	state.record(ParallelExecutionTelemetry{
			.estimate = estimate,
			.workerCount = 2,
			.elapsedNs = 100,
			.completed = true});
	EXPECT_EQ(state.statsFor(estimate, 2).samples, 1U);
	state.reset();
	EXPECT_EQ(state.statsFor(estimate, 2).samples, 0U);

	ParallelAdaptivePolicyConfig config;
	config.ewmaAlpha = 0.0;
	config.minImprovementRatio = 1.25;
	config.explorationEnabled = false;
	state.setConfig(config);
	const auto applied = state.config();
	EXPECT_DOUBLE_EQ(applied.ewmaAlpha, 0.25);
	EXPECT_DOUBLE_EQ(applied.minImprovementRatio, 1.25);
	EXPECT_FALSE(applied.explorationEnabled);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptivePolicyIgnoresUnsafeTelemetryAndClampsMergeRatio) {
	AdaptiveParallelPolicyState state;
	const auto emptyEstimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_GENERAL,
			.partitions = 0,
			.estimatedItems = 0,
			.estimatedBytes = 0};
	state.record(ParallelExecutionTelemetry{
			.estimate = emptyEstimate,
			.workerCount = 1,
			.elapsedNs = 100,
			.completed = true});
	state.record(ParallelExecutionTelemetry{
			.estimate = emptyEstimate,
			.workerCount = 1,
			.elapsedNs = 0,
			.completed = true});
	state.record(ParallelExecutionTelemetry{
			.estimate = emptyEstimate,
			.workerCount = 0,
			.elapsedNs = 100,
			.completed = true});
	EXPECT_EQ(state.statsFor(emptyEstimate, 1).samples, 0U);

	const auto hugeEstimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_STORAGE_SCAN,
			.partitions = 4096,
			.estimatedBytes = std::numeric_limits<size_t>::max()};
	state.record(ParallelExecutionTelemetry{
			.estimate = hugeEstimate,
			.workerCount = std::numeric_limits<size_t>::max(),
			.elapsedNs = 100,
			.mergeNs = 1000,
			.completed = true});
	const auto stats = state.statsFor(hugeEstimate, std::numeric_limits<size_t>::max());
	EXPECT_EQ(stats.samples, 1U);
	EXPECT_DOUBLE_EQ(stats.mergeRatioEwma, 1.0);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptivePolicyUsesCandidateWithoutBaselineAndStopsExplorationWhenDisabled) {
	AdaptiveParallelPolicyState state;
	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_MEMORY_SCAN,
			.partitions = 64,
			.estimatedItems = 100000,
			.estimatedBytes = size_t{256} * 1024 * 1024,
			.minPartitions = 2,
			.minItems = 2};

	for (int i = 0; i < 2; ++i) {
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 8,
				.elapsedNs = 100,
				.completed = true});
	}
	EXPECT_EQ(state.recommendWorkerCount(estimate, 4, 8), 8UL);

	ParallelAdaptivePolicyConfig config = state.config();
	config.explorationEnabled = false;
	state.setConfig(config);
	state.reset();
	for (int i = 0; i < 2; ++i) {
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 4,
				.elapsedNs = 100,
				.completed = true});
	}
	EXPECT_EQ(state.recommendWorkerCount(estimate, 4, 8), 4UL);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptivePolicyAvoidsWorkersWithHighMergeCost) {
	AdaptiveParallelPolicyState state;
	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_CPU_BOUND,
			.partitions = 64,
			.estimatedItems = 100000,
			.minPartitions = 2,
			.minItems = 2};

	for (int i = 0; i < 2; ++i) {
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 4,
				.elapsedNs = 100,
				.mergeNs = 10,
				.completed = true});
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 8,
				.elapsedNs = 90,
				.mergeNs = 60,
				.completed = true});
	}

	EXPECT_EQ(state.recommendWorkerCount(estimate, 4, 8), 4UL);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptivePolicyPrefersLowerWorkerOnMarginalThroughputGain) {
	AdaptiveParallelPolicyState state;
	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_CPU_BOUND,
			.partitions = 64,
			.estimatedItems = 100000,
			.minPartitions = 2,
			.minItems = 2};

	for (int i = 0; i < 2; ++i) {
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 4,
				.elapsedNs = 100,
				.completed = true});
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 8,
				.elapsedNs = 95,
				.completed = true});
	}

	EXPECT_EQ(state.recommendWorkerCount(estimate, 8, 8), 4UL);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptivePolicyExploresLowerWhenBaselineSaturatesHardLimit) {
	AdaptiveParallelPolicyState state;
	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_CPU_BOUND,
			.partitions = 64,
			.estimatedItems = 100000,
			.minPartitions = 2,
			.minItems = 2};

	for (int i = 0; i < 2; ++i) {
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 8,
				.elapsedNs = 100,
				.completed = true});
	}

	EXPECT_EQ(state.recommendWorkerCount(estimate, 8, 8), 4UL);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptivePolicyRespectsGranularityWhenExploring) {
	AdaptiveParallelPolicyState state;
	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_CPU_BOUND,
			.partitions = 64,
			.estimatedItems = 800,
			.minPartitions = 2,
			.minItems = 2,
			.minItemsPerWorker = 101};

	for (int i = 0; i < 2; ++i) {
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 4,
				.elapsedNs = 100,
				.completed = true});
	}

	EXPECT_EQ(state.recommendWorkerCount(estimate, 4, 8), 4UL);
	EXPECT_EQ(state.recommendWorkerCount(estimate, 4, 0), 1UL);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptivePolicySeparatesAdjacencyFrontierShapes) {
	AdaptiveParallelPolicyState state;
	const auto broadFrontier = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
			.partitions = 64,
			.estimatedItems = 100000,
			.estimatedBytes = 100000 * size_t{32},
			.minPartitions = 2,
			.minItems = 2,
			.estimatedStateBytesPerItem = 32,
			.frontierWidth = 4096,
			.traversalDepth = 2};
	const auto narrowFrontier = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
			.partitions = 64,
			.estimatedItems = 100000,
			.estimatedBytes = 100000 * size_t{32},
			.minPartitions = 2,
			.minItems = 2,
			.estimatedStateBytesPerItem = 32,
			.frontierWidth = 8,
			.traversalDepth = 1};

	for (int i = 0; i < 2; ++i) {
		state.record(ParallelExecutionTelemetry{
				.estimate = broadFrontier,
				.workerCount = 8,
				.elapsedNs = 100,
				.completed = true});
	}

	EXPECT_EQ(state.recommendWorkerCount(broadFrontier, 2, 8), 8UL);
	EXPECT_EQ(state.recommendWorkerCount(narrowFrontier, 2, 8), 2UL);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptivePolicyDetailHelpersClassifyBoundaryEstimates) {
	using namespace graph::concurrent;

	EXPECT_EQ(detail::workloadIndex(ParallelWorkloadKind::PWK_GENERAL), 0UL);
	EXPECT_EQ(detail::workloadIndex(ParallelWorkloadKind::PWK_CPU_BOUND), 1UL);
	EXPECT_EQ(detail::workloadIndex(ParallelWorkloadKind::PWK_MEMORY_SCAN), 2UL);
	EXPECT_EQ(detail::workloadIndex(ParallelWorkloadKind::PWK_MEMORY_INTENSIVE), 3UL);
	EXPECT_EQ(detail::workloadIndex(ParallelWorkloadKind::PWK_STORAGE_SCAN), 4UL);
	EXPECT_EQ(detail::workloadIndex(ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL), 5UL);
	EXPECT_EQ(detail::workloadIndex(static_cast<ParallelWorkloadKind>(999)), 0UL);

	const auto hugeEstimate = ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_STORAGE_SCAN,
			.partitions = 8,
			.estimatedBytes = std::numeric_limits<size_t>::max()};
	EXPECT_EQ(detail::sizeBucket(hugeEstimate), detail::kAdaptiveSizeBucketCount - 1);

	const auto broadOnlyFrontier = ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
			.partitions = 8,
			.estimatedItems = 4096,
			.estimatedStateBytesPerItem = 32,
			.frontierWidth = 2048,
			.traversalDepth = 0};
	const auto shallowPathFrontier = ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
			.partitions = 8,
			.estimatedItems = 4096,
			.estimatedStateBytesPerItem = 32,
			.frontierWidth = 8,
			.traversalDepth = 1};
	const auto noPathStateFrontier = ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
			.partitions = 8,
			.estimatedItems = 4096,
			.estimatedStateBytesPerItem = 8,
			.frontierWidth = 2048,
			.traversalDepth = 3};
	const auto deepOnlyFrontier = ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL,
			.partitions = 8,
			.estimatedItems = 4096,
			.estimatedStateBytesPerItem = 32,
			.frontierWidth = 8,
			.traversalDepth = 3};
	EXPECT_EQ(detail::shapeBucket(broadOnlyFrontier), 3UL);
	EXPECT_EQ(detail::shapeBucket(deepOnlyFrontier), 4UL);
	EXPECT_EQ(detail::shapeBucket(shallowPathFrontier), 2UL);
	EXPECT_EQ(detail::shapeBucket(noPathStateFrontier), 0UL);

	const auto itemLimited = ParallelWorkEstimate{
			.partitions = 8,
			.estimatedItems = 15,
			.minItemsPerWorker = 8};
	const auto byteLimited = ParallelWorkEstimate{
			.partitions = 8,
			.estimatedBytes = 15,
			.minBytesPerWorker = 8};
	EXPECT_FALSE(detail::hasEnoughGranularity(itemLimited, 2));
	EXPECT_FALSE(detail::hasEnoughGranularity(byteLimited, 2));
	EXPECT_TRUE(detail::hasEnoughGranularity(itemLimited, 1));

	const auto openPartitionEstimate = ParallelWorkEstimate{.partitions = 0, .estimatedItems = 0, .minItemsPerWorker = 8};
	const auto openByteEstimate = ParallelWorkEstimate{.partitions = 0, .estimatedBytes = 0, .minBytesPerWorker = 8};
	EXPECT_TRUE(detail::hasEnoughGranularity(openPartitionEstimate, 8));
	EXPECT_TRUE(detail::hasEnoughGranularity(openByteEstimate, 8));

	EXPECT_DOUBLE_EQ(detail::telemetryThroughput(ParallelExecutionTelemetry{.estimate = itemLimited, .elapsedNs = 0}), 0.0);
	EXPECT_DOUBLE_EQ(detail::mergeRatio(ParallelExecutionTelemetry{.elapsedNs = 0, .mergeNs = 1}), 0.0);
	EXPECT_DOUBLE_EQ(detail::mergeRatio(ParallelExecutionTelemetry{.elapsedNs = 10, .mergeNs = 0}), 0.0);
	EXPECT_FALSE(detail::hasReliableThroughput(ParallelAdaptiveStats{.samples = 2, .throughputEwma = 0.0},
									  ParallelAdaptivePolicyConfig{}));
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptivePolicyReturnsClampedBaselineForSingleWorkerLimits) {
	AdaptiveParallelPolicyState state;
	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_CPU_BOUND,
			.partitions = 8,
			.estimatedItems = 10000,
			.minPartitions = 2,
			.minItems = 2};

	EXPECT_EQ(state.recommendWorkerCount(estimate, 1, 8), 1UL);
	EXPECT_EQ(state.recommendWorkerCount(estimate, 4, 1), 1UL);
}

TEST(ThreadPoolExceptionAndParallelTest, ThreadPoolPolicyAccessorsExposeAndResetAdaptiveState) {
	ThreadPool pool(4);
	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_GENERAL,
			.partitions = 4,
			.estimatedItems = 4096,
			.minPartitions = 2,
			.minItems = 2};

	EXPECT_EQ(pool.getParallelPolicyMode(), ParallelPolicyMode::PPM_ADAPTIVE);
	pool.setParallelPolicyMode(ParallelPolicyMode::PPM_FIXED_HEURISTIC);
	EXPECT_EQ(pool.getParallelPolicyMode(), ParallelPolicyMode::PPM_FIXED_HEURISTIC);
	pool.adaptivePolicyState().record(
			ParallelExecutionTelemetry{.estimate = estimate,
									   .workerCount = 2,
									   .elapsedNs = 100,
									   .completed = true});
	ASSERT_EQ(pool.adaptivePolicyState().statsFor(estimate, 2).samples, 1U);
	pool.resetAdaptiveParallelPolicy();
	EXPECT_EQ(pool.adaptivePolicyState().statsFor(estimate, 2).samples, 0U);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptivePolicyExploresLowerAfterExpensiveBaselineMerge) {
	AdaptiveParallelPolicyState state;
	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_CPU_BOUND,
			.partitions = 64,
			.estimatedItems = 100000,
			.minPartitions = 2,
			.minItems = 2};

	for (int i = 0; i < 2; ++i) {
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 4,
				.elapsedNs = 100,
				.mergeNs = 80,
				.completed = true});
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 2,
				.elapsedNs = 120,
				.completed = true});
	}

	EXPECT_EQ(state.recommendWorkerCount(estimate, 4, 8), 2UL);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptivePolicyExploresHigherWhenLowerWorkersAreKnown) {
	AdaptiveParallelPolicyState state;
	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_CPU_BOUND,
			.partitions = 64,
			.estimatedItems = 100000,
			.minPartitions = 2,
			.minItems = 2};

	for (int i = 0; i < 2; ++i) {
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 1,
				.elapsedNs = 240,
				.mergeNs = 240,
				.completed = true});
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 2,
				.elapsedNs = 130,
				.mergeNs = 130,
				.completed = true});
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 4,
				.elapsedNs = 100,
				.mergeNs = 80,
				.completed = true});
	}

	EXPECT_EQ(state.recommendWorkerCount(estimate, 4, 8), 8UL);
}

TEST(ThreadPoolExceptionAndParallelTest, AdaptivePolicyReturnsBestReliableWorkerWhenExplorationHasNoCandidate) {
	ParallelAdaptivePolicyConfig config;
	config.minImprovementRatio = 1.0;
	AdaptiveParallelPolicyState state;
	state.setConfig(config);
	const auto estimate = graph::concurrent::ParallelWorkEstimate{
			.workloadKind = ParallelWorkloadKind::PWK_CPU_BOUND,
			.partitions = 64,
			.estimatedItems = 100000,
			.minPartitions = 2,
			.minItems = 2};

	for (int i = 0; i < 2; ++i) {
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 1,
				.elapsedNs = 500,
				.completed = true});
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 2,
				.elapsedNs = 250,
				.completed = true});
		state.record(ParallelExecutionTelemetry{
				.estimate = estimate,
				.workerCount = 4,
				.elapsedNs = 100,
				.completed = true});
	}

	EXPECT_EQ(state.recommendWorkerCount(estimate, 4, 4), 4UL);
}
