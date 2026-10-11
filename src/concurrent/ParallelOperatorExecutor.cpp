/**
 * @file ParallelOperatorExecutor.cpp
 * @brief Non-template scheduling core for partition-local operators.
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "graph/concurrent/ParallelOperatorExecutor.hpp"

#include <atomic>
#include <chrono>
#include <limits>
#include <string>

#include "graph/debug/PerfTrace.hpp"

namespace graph::concurrent::detail {

	namespace {

		[[nodiscard]] uint64_t elapsedNs(std::chrono::steady_clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
							.count());
		}

		[[nodiscard]] std::string childPhase(std::string_view phase, std::string_view suffix) {
			std::string name;
			name.reserve(phase.size() + suffix.size());
			name.append(phase.data(), phase.size());
			name.append(suffix.data(), suffix.size());
			return name;
		}

		[[nodiscard]] int64_t clampProfileValue(size_t value) {
			return static_cast<int64_t>(
					std::min<size_t>(value, static_cast<size_t>(std::numeric_limits<int64_t>::max())));
		}

		void emitEstimateValue(std::string_view phase, std::string_view suffix, size_t value) {
			if (value == 0) {
				return;
			}
			debug::PerfTrace::addValue(childPhase(phase, suffix), clampProfileValue(value));
		}

		void emitDecisionProfile(std::string_view phase,
								 const ParallelWorkEstimate &estimate,
								 const ParallelExecutionDecision &decision) {
			if (!debug::PerfTrace::isEnabled() || phase.empty()) {
				return;
			}

			emitEstimateValue(phase, ".estimated_items", estimate.estimatedItems);
			emitEstimateValue(phase, ".estimated_bytes", estimate.estimatedBytes);
			emitEstimateValue(phase, ".state_bytes_per_item", estimate.estimatedStateBytesPerItem);
			emitEstimateValue(phase, ".frontier_width", estimate.frontierWidth);
			emitEstimateValue(phase, ".traversal_depth", estimate.traversalDepth);
			debug::PerfTrace::addValue(
					childPhase(phase, ".workers"), static_cast<int64_t>(decision.useParallel ? decision.workerCount : 1));
			debug::PerfTrace::addValue(
					childPhase(phase, ".baseline_workers"), static_cast<int64_t>(decision.baselineWorkerCount));
			debug::PerfTrace::addValue(
					childPhase(phase, ".hard_worker_limit"), static_cast<int64_t>(decision.hardWorkerLimit));
			debug::PerfTrace::addValue(childPhase(phase, ".adaptive"), decision.usedAdaptiveRecommendation ? 1 : 0);
			debug::PerfTrace::addValue(childPhase(phase, ".decision_reason_code"), parallelDecisionReasonCode(decision.reason));
			debug::PerfTrace::addValue(
					childPhase(childPhase(phase, ".decision."), parallelDecisionReasonName(decision.reason)), 1);
		}

	} // namespace

	ParallelWorkEstimate makeEstimate(size_t partitionCount, const ParallelOperatorOptions &options) {
		return {.workloadKind = options.workloadKind,
				.partitions = partitionCount,
				.estimatedItems = options.estimatedItems,
				.estimatedBytes = options.estimatedBytes,
				.minPartitions = options.minPartitions,
				.minItems = options.minItems,
				.minItemsPerWorker = options.minItemsPerWorker,
				.minBytesPerWorker = options.minBytesPerWorker,
				.maxWorkers = options.maxWorkers,
				.estimatedStateBytesPerItem = options.estimatedStateBytesPerItem,
				.frontierWidth = options.frontierWidth,
				.traversalDepth = options.traversalDepth};
	}

	size_t chooseRangePartitionCount(size_t totalItems, ThreadPool *pool, const ParallelOperatorOptions &options) {
		if (totalItems == 0) {
			return 0;
		}
		const size_t poolWorkers = pool ? std::max<size_t>(1, pool->getThreadCount()) : size_t{1};
		const size_t workerLimit = options.maxWorkers == 0 ? poolWorkers : std::min(poolWorkers, options.maxWorkers);
		const size_t targetPartitions = std::max<size_t>(1, workerLimit) * 4;
		size_t byGranularity = totalItems;
		if (options.minItemsPerWorker != 0) {
			byGranularity = std::max<size_t>(1, ceilDiv(totalItems, options.minItemsPerWorker));
		}
		return std::max<size_t>(1, std::min({totalItems, targetPartitions, byGranularity}));
	}

	bool runOperatorIndexedPartitionsCore(size_t partitionCount,
														ThreadPool *pool,
														const ParallelOperatorOptions &options,
														const ParallelWorkEstimate &estimate,
														const ParallelExecutionDecision &decision,
														const ParallelOperatorCallbacks &callbacks) {
		if (partitionCount == 0) {
			return true;
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled() && !options.phase.empty();
		const auto totalStart = std::chrono::steady_clock::now();
		const std::string taskPhase = traceEnabled ? childPhase(options.phase, ".task") : std::string{};
		const std::string mergePhase = traceEnabled ? childPhase(options.phase, ".merge") : std::string{};
		std::atomic<bool> failed{false};
		std::atomic<uint64_t> taskNs{0};
		std::atomic<uint64_t> taskCalls{0};
		auto runOne = [&](size_t partition) {
			if (failed.load(std::memory_order_relaxed)) {
				return;
			}
			const auto taskStart = std::chrono::steady_clock::now();
			const bool ok = callbacks.run(callbacks.context, partition);
			const uint64_t elapsed = elapsedNs(taskStart);
			taskNs.fetch_add(elapsed, std::memory_order_relaxed);
			taskCalls.fetch_add(1, std::memory_order_relaxed);
			if (!ok) {
				failed.store(true, std::memory_order_relaxed);
			}
		};

		emitDecisionProfile(options.phase, estimate, decision);
		if (decision.useParallel) {
			pool->parallelFor(0, partitionCount, decision.workerCount, runOne);
		} else {
			for (size_t partition = 0; partition < partitionCount; ++partition) {
				runOne(partition);
				if (failed.load(std::memory_order_relaxed)) {
					break;
				}
			}
		}
		if (traceEnabled) {
			debug::PerfTrace::addDurationBatch(
					taskPhase, taskNs.load(std::memory_order_relaxed), taskCalls.load(std::memory_order_relaxed));
		}

		if (failed.load(std::memory_order_relaxed)) {
			recordParallelExecution(pool, estimate, decision, elapsedNs(totalStart), taskNs.load(), 0, false);
			return false;
		}

		const auto mergeStart = std::chrono::steady_clock::now();
		for (size_t partition = 0; partition < partitionCount; ++partition) {
			callbacks.merge(callbacks.context, partition);
		}
		const uint64_t mergeNs = elapsedNs(mergeStart);
		const uint64_t totalNs = elapsedNs(totalStart);
		if (traceEnabled) {
			debug::PerfTrace::addDuration(mergePhase, mergeNs);
			debug::PerfTrace::addDuration(options.phase, totalNs);
		}
		recordParallelExecution(pool, estimate, decision, totalNs, taskNs.load(std::memory_order_relaxed), mergeNs);
		return true;
	}

} // namespace graph::concurrent::detail
