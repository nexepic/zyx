/**
 * @file ParallelExecutionPolicy.hpp
 * @brief Shared heuristics for deciding when a thread pool should be used.
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "graph/concurrent/ThreadPool.hpp"

namespace graph::concurrent {

	inline constexpr size_t kDefaultMemoryScanBytesPerWorker = size_t{4} * 1024 * 1024;
	inline constexpr size_t kDefaultMemoryScanMaxWorkers = 4;
	inline constexpr size_t kDefaultMemoryIntensiveBytesPerWorker = kDefaultMemoryScanBytesPerWorker;
	inline constexpr size_t kDefaultMemoryIntensiveMaxWorkers = kDefaultMemoryScanMaxWorkers;
	inline constexpr size_t kDefaultStorageScanBytesPerWorker = size_t{4} * 1024 * 1024;
	inline constexpr size_t kDefaultStorageScanMaxWorkers = 8;
	inline constexpr size_t kDefaultAdjacencyTraversalItemsPerWorker = 2048;
	inline constexpr size_t kDefaultAdjacencyTraversalMaxWorkers = 0;

	struct ParallelExecutionPolicyConfig {
		size_t generalItemsPerWorker = 0;
		size_t cpuBoundItemsPerWorker = 0;
		size_t memoryScanBytesPerWorker = kDefaultMemoryScanBytesPerWorker;
		size_t memoryScanMaxWorkers = kDefaultMemoryScanMaxWorkers;
		size_t memoryIntensiveBytesPerWorker = kDefaultMemoryIntensiveBytesPerWorker;
		size_t memoryIntensiveMaxWorkers = kDefaultMemoryIntensiveMaxWorkers;
		size_t storageScanBytesPerWorker = kDefaultStorageScanBytesPerWorker;
		size_t storageScanMaxWorkers = kDefaultStorageScanMaxWorkers;
		size_t adjacencyTraversalItemsPerWorker = kDefaultAdjacencyTraversalItemsPerWorker;
		size_t adjacencyTraversalMaxWorkers = kDefaultAdjacencyTraversalMaxWorkers;
	};

	struct ParallelWorkloadDefaults {
		size_t minItemsPerWorker = 0;
		size_t minBytesPerWorker = 0;
		size_t maxWorkers = 0;
	};

	inline ParallelWorkloadDefaults workloadDefaults(ParallelWorkloadKind workloadKind,
													 const ParallelExecutionPolicyConfig &config) {
		switch (workloadKind) {
			case ParallelWorkloadKind::PWK_CPU_BOUND:
				return {.minItemsPerWorker = config.cpuBoundItemsPerWorker};
			case ParallelWorkloadKind::PWK_MEMORY_SCAN:
				return {.minBytesPerWorker = config.memoryScanBytesPerWorker,
						.maxWorkers = config.memoryScanMaxWorkers};
			case ParallelWorkloadKind::PWK_MEMORY_INTENSIVE:
				return {.minBytesPerWorker = config.memoryIntensiveBytesPerWorker,
						.maxWorkers = config.memoryIntensiveMaxWorkers};
			case ParallelWorkloadKind::PWK_STORAGE_SCAN:
				return {.minBytesPerWorker = config.storageScanBytesPerWorker,
						.maxWorkers = config.storageScanMaxWorkers};
			case ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL:
				return {.minItemsPerWorker = config.adjacencyTraversalItemsPerWorker,
						.maxWorkers = config.adjacencyTraversalMaxWorkers};
			case ParallelWorkloadKind::PWK_GENERAL:
			default:
				return {.minItemsPerWorker = config.generalItemsPerWorker};
		}
	}

	inline bool hasParallelWorkers(const ThreadPool *pool) {
		return pool != nullptr && !pool->isSingleThreaded() && pool->getThreadCount() > 1;
	}

	inline size_t ceilDiv(size_t value, size_t divisor) {
		return divisor == 0 ? value : (value / divisor) + static_cast<size_t>((value % divisor) != 0);
	}

	inline size_t applyWorkerGranularityLimit(size_t workerCount,
											  const ParallelWorkEstimate &estimate,
											  const ParallelWorkloadDefaults &defaults) {
		const size_t itemsPerWorker =
				estimate.minItemsPerWorker != 0 ? estimate.minItemsPerWorker : defaults.minItemsPerWorker;
		if (itemsPerWorker != 0) {
			workerCount = std::min(workerCount, estimate.estimatedItems / itemsPerWorker);
		}

		const size_t bytesPerWorker =
				estimate.minBytesPerWorker != 0 ? estimate.minBytesPerWorker : defaults.minBytesPerWorker;
		if (bytesPerWorker != 0 && estimate.estimatedBytes != 0) {
			workerCount = std::min(workerCount, ceilDiv(estimate.estimatedBytes, bytesPerWorker));
		}
		return workerCount;
	}

	inline size_t applyAdjacencyTraversalStateLimit(size_t workerCount,
													const ParallelWorkEstimate &estimate) {
		if (estimate.workloadKind != ParallelWorkloadKind::PWK_ADJACENCY_TRAVERSAL || workerCount <= 1) {
			return workerCount;
		}

		// Frontier traversal is usually memory-bandwidth bound once each output carries
		// path state. Prefer a conservative baseline and let the adaptive policy explore
		// higher counts only after local telemetry proves that they help.
		const bool carriesPathState = estimate.estimatedStateBytesPerItem >= 32;
		const bool broadFrontier = estimate.frontierWidth >= 1024;
		if (carriesPathState && (estimate.traversalDepth >= 1 || broadFrontier)) {
			workerCount = std::min(workerCount, size_t{2});
		}
		return workerCount;
	}

	inline ParallelExecutionDecision decideParallelExecution(const ThreadPool *pool,
															 const ParallelWorkEstimate &estimate,
															 const ParallelExecutionPolicyConfig &config) {
		ParallelExecutionDecision decision;
		if (!hasParallelWorkers(pool)) {
			decision.reason = ParallelDecisionReason::PDR_NO_WORKERS;
			return decision;
		}
		if (estimate.partitions < estimate.minPartitions) {
			decision.reason = ParallelDecisionReason::PDR_INSUFFICIENT_PARTITIONS;
			return decision;
		}
		if (estimate.estimatedItems < estimate.minItems) {
			decision.reason = ParallelDecisionReason::PDR_INSUFFICIENT_ITEMS;
			return decision;
		}

		const auto defaults = workloadDefaults(estimate.workloadKind, config);
		size_t hardWorkerLimit = std::min(pool->getThreadCount(), estimate.partitions);
		if (estimate.maxWorkers != 0) {
			hardWorkerLimit = std::min(hardWorkerLimit, estimate.maxWorkers);
		}
		hardWorkerLimit = applyWorkerGranularityLimit(hardWorkerLimit, estimate, defaults);
		decision.hardWorkerLimit = std::max<size_t>(1, hardWorkerLimit);

		size_t workerCount = hardWorkerLimit;
		if (estimate.maxWorkers == 0 && defaults.maxWorkers != 0) {
			workerCount = std::min(workerCount, defaults.maxWorkers);
		}
		workerCount = applyAdjacencyTraversalStateLimit(workerCount, estimate);
		decision.baselineWorkerCount = std::max<size_t>(1, workerCount);

		if (workerCount > 1 && estimate.maxWorkers == 0) {
			const size_t adaptiveWorkerCount =
					pool->adaptivePolicyState().recommendWorkerCount(estimate, workerCount, hardWorkerLimit);
			if (adaptiveWorkerCount != workerCount) {
				workerCount = adaptiveWorkerCount;
				decision.usedAdaptiveRecommendation = true;
			}
		}

		if (workerCount <= 1) {
			decision.reason = ParallelDecisionReason::PDR_INSUFFICIENT_GRANULARITY;
			return decision;
		}

		decision.useParallel = true;
		decision.workerCount = workerCount;
		decision.reason = ParallelDecisionReason::PDR_PARALLEL;
		return decision;
	}

	inline void recordParallelExecution(ThreadPool *pool,
										const ParallelWorkEstimate &estimate,
										const ParallelExecutionDecision &decision,
										uint64_t elapsedNs,
										uint64_t taskNs = 0,
										uint64_t mergeNs = 0,
										bool completed = true) {
		if (!pool || elapsedNs == 0) {
			return;
		}
		pool->adaptivePolicyState().record({.estimate = estimate,
											 .workerCount = decision.useParallel ? decision.workerCount : size_t{1},
											 .elapsedNs = elapsedNs,
											 .taskNs = taskNs,
											 .mergeNs = mergeNs,
											 .completed = completed});
	}

	class ScopedParallelExecutionTelemetry {
	public:
		ScopedParallelExecutionTelemetry(ThreadPool *pool,
										 ParallelWorkEstimate estimate,
										 ParallelExecutionDecision decision) :
			pool_(pool),
			estimate_(estimate),
			decision_(decision),
			start_(Clock::now()) {}

		~ScopedParallelExecutionTelemetry() {
			if (!active_) {
				return;
			}
			recordParallelExecution(pool_, estimate_, decision_, elapsedNs(), taskNs_, mergeNs_, completed_);
		}

		ScopedParallelExecutionTelemetry(const ScopedParallelExecutionTelemetry &) = delete;
		ScopedParallelExecutionTelemetry &operator=(const ScopedParallelExecutionTelemetry &) = delete;

		ScopedParallelExecutionTelemetry(ScopedParallelExecutionTelemetry &&other) noexcept :
			pool_(other.pool_),
			estimate_(other.estimate_),
			decision_(other.decision_),
			start_(other.start_),
			taskNs_(other.taskNs_),
			mergeNs_(other.mergeNs_),
			completed_(other.completed_),
			active_(other.active_) {
			other.active_ = false;
		}

		ScopedParallelExecutionTelemetry &operator=(ScopedParallelExecutionTelemetry &&) = delete;

		void markCompleted() { completed_ = true; }

		void setTaskNs(uint64_t taskNs) { taskNs_ = taskNs; }

		void setMergeNs(uint64_t mergeNs) { mergeNs_ = mergeNs; }

		void dismiss() { active_ = false; }

	private:
		using Clock = std::chrono::steady_clock;

		[[nodiscard]] uint64_t elapsedNs() const {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count());
		}

		ThreadPool *pool_ = nullptr;
		ParallelWorkEstimate estimate_;
		ParallelExecutionDecision decision_;
		Clock::time_point start_;
		uint64_t taskNs_ = 0;
		uint64_t mergeNs_ = 0;
		bool completed_ = false;
		bool active_ = true;
	};

	inline ParallelExecutionDecision decideParallelExecution(const ThreadPool *pool,
															 const ParallelWorkEstimate &estimate) {
		return decideParallelExecution(pool, estimate, {});
	}

	inline bool shouldParallelize(const ThreadPool *pool, size_t partitions, size_t estimatedItems,
								  size_t minPartitions = 2, size_t minItems = 1) {
		return decideParallelExecution(pool, {.partitions = partitions,
											  .estimatedItems = estimatedItems,
											  .minPartitions = minPartitions,
											  .minItems = minItems})
				.useParallel;
	}

} // namespace graph::concurrent
