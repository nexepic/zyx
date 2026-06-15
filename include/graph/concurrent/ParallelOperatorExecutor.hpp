/**
 * @file ParallelOperatorExecutor.hpp
 * @brief Shared partition-local executor for query/storage operators.
 *
 * The executor centralizes worker selection, worker-local state, ordered merge,
 * and profile/adaptive telemetry so operators do not grow bespoke parallelFor
 * policies over time.
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "graph/concurrent/ParallelExecutionPolicy.hpp"
#include "graph/debug/PerfTrace.hpp"

namespace graph::concurrent {

	struct ParallelOperatorOptions {
		std::string_view phase;
		ParallelWorkloadKind workloadKind = ParallelWorkloadKind::PWK_GENERAL;
		size_t estimatedItems = 0;
		size_t estimatedBytes = 0;
		size_t minPartitions = 2;
		size_t minItems = 1;
		size_t minItemsPerWorker = 0;
		size_t minBytesPerWorker = 0;
		size_t maxWorkers = 0;
		size_t estimatedStateBytesPerItem = 0;
		size_t frontierWidth = 0;
		size_t traversalDepth = 0;
	};

	struct ParallelRangePartition {
		size_t begin = 0;
		size_t end = 0;

		[[nodiscard]] size_t size() const { return end > begin ? end - begin : 0; }
		[[nodiscard]] bool empty() const { return begin >= end; }
	};

	namespace detail {
		template<typename State>
		struct ParallelOperatorStateStorage {
			explicit ParallelOperatorStateStorage(size_t partitionCount) : states(partitionCount) {}
			std::vector<State> states;
		};

		inline uint64_t elapsedNs(std::chrono::steady_clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
							.count());
		}

		inline std::string childPhase(std::string_view phase, std::string_view suffix) {
			std::string name;
			name.reserve(phase.size() + suffix.size());
			name.append(phase.data(), phase.size());
			name.append(suffix.data(), suffix.size());
			return name;
		}

		template<typename Worker, typename State>
		bool invokeIndexedWorker(Worker &&worker, size_t partition, State &state) {
			if constexpr (std::is_same_v<std::invoke_result_t<Worker, size_t, State &>, bool>) {
				return std::invoke(worker, partition, state);
			} else {
				std::invoke(worker, partition, state);
				return true;
			}
		}

		template<typename Worker, typename State>
		bool invokeRangeWorker(Worker &&worker, const ParallelRangePartition &range, State &state) {
			if constexpr (std::is_same_v<std::invoke_result_t<Worker, const ParallelRangePartition &, State &>, bool>) {
				return std::invoke(worker, range, state);
			} else {
				std::invoke(worker, range, state);
				return true;
			}
		}

		inline ParallelWorkEstimate makeEstimate(size_t partitionCount, const ParallelOperatorOptions &options) {
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

		inline int64_t clampProfileValue(size_t value) {
			return static_cast<int64_t>(
					std::min<size_t>(value, static_cast<size_t>(std::numeric_limits<int64_t>::max())));
		}

		inline void emitEstimateValue(std::string_view phase, std::string_view suffix, size_t value) {
			if (value == 0) {
				return;
			}
			const std::string valuePhase = childPhase(phase, suffix);
			debug::PerfTrace::addValue(valuePhase, clampProfileValue(value));
		}

		inline void emitDecisionProfile(
				std::string_view phase,
				const ParallelWorkEstimate &estimate,
				const ParallelExecutionDecision &decision) {
			if (!debug::PerfTrace::isEnabled() || phase.empty()) {
				return;
			}
			const std::string workersPhase = childPhase(phase, ".workers");
			const std::string baselinePhase = childPhase(phase, ".baseline_workers");
			const std::string hardLimitPhase = childPhase(phase, ".hard_worker_limit");
			const std::string adaptivePhase = childPhase(phase, ".adaptive");
			const std::string reasonCodePhase = childPhase(phase, ".decision_reason_code");
			const std::string reasonPhase = childPhase(
					childPhase(phase, ".decision."), parallelDecisionReasonName(decision.reason));

			emitEstimateValue(phase, ".estimated_items", estimate.estimatedItems);
			emitEstimateValue(phase, ".estimated_bytes", estimate.estimatedBytes);
			emitEstimateValue(phase, ".state_bytes_per_item", estimate.estimatedStateBytesPerItem);
			emitEstimateValue(phase, ".frontier_width", estimate.frontierWidth);
			emitEstimateValue(phase, ".traversal_depth", estimate.traversalDepth);
			debug::PerfTrace::addValue(workersPhase, static_cast<int64_t>(decision.useParallel ? decision.workerCount : 1));
			debug::PerfTrace::addValue(baselinePhase, static_cast<int64_t>(decision.baselineWorkerCount));
			debug::PerfTrace::addValue(hardLimitPhase, static_cast<int64_t>(decision.hardWorkerLimit));
			debug::PerfTrace::addValue(adaptivePhase, decision.usedAdaptiveRecommendation ? 1 : 0);
			debug::PerfTrace::addValue(reasonCodePhase, parallelDecisionReasonCode(decision.reason));
			debug::PerfTrace::addValue(reasonPhase, 1);
		}

		inline size_t chooseRangePartitionCount(size_t totalItems,
												ThreadPool *pool,
												const ParallelOperatorOptions &options) {
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
	} // namespace detail

	inline std::vector<ParallelRangePartition>
	buildRangePartitions(size_t begin, size_t end, size_t partitionCount) {
		std::vector<ParallelRangePartition> partitions;
		if (begin >= end || partitionCount == 0) {
			return partitions;
		}

		const size_t total = end - begin;
		partitionCount = std::min(partitionCount, total);
		partitions.reserve(partitionCount);

		const size_t baseSize = total / partitionCount;
		const size_t remainder = total % partitionCount;
		size_t cursor = begin;
		for (size_t partition = 0; partition < partitionCount; ++partition) {
			const size_t size = baseSize + static_cast<size_t>(partition < remainder);
			partitions.push_back({cursor, cursor + size});
			cursor += size;
		}
		return partitions;
	}

	template<typename State, typename Worker, typename Merger>
	bool runOperatorIndexedPartitionsWithDecision(size_t partitionCount,
												  ThreadPool *pool,
												  const ParallelOperatorOptions &options,
												  const ParallelWorkEstimate &estimate,
												  const ParallelExecutionDecision &decision,
												  Worker &&worker,
												  Merger &&merger) {
		if (partitionCount == 0) {
			return true;
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled() && !options.phase.empty();
		const auto totalStart = std::chrono::steady_clock::now();
		const std::string taskPhase = traceEnabled ? detail::childPhase(options.phase, ".task") : std::string{};
		const std::string mergePhase = traceEnabled ? detail::childPhase(options.phase, ".merge") : std::string{};

		detail::ParallelOperatorStateStorage<State> storage(partitionCount);
		std::atomic<bool> failed{false};
		std::atomic<uint64_t> taskNs{0};
		std::atomic<uint64_t> taskCalls{0};
		auto runOne = [&](size_t partition) {
			if (failed.load(std::memory_order_relaxed)) {
				return;
			}
			const auto taskStart = std::chrono::steady_clock::now();
			const bool ok = detail::invokeIndexedWorker(worker, partition, storage.states[partition]);
			const uint64_t elapsed = detail::elapsedNs(taskStart);
			taskNs.fetch_add(elapsed, std::memory_order_relaxed);
			taskCalls.fetch_add(1, std::memory_order_relaxed);
			if (!ok) {
				failed.store(true, std::memory_order_relaxed);
			}
		};

		detail::emitDecisionProfile(options.phase, estimate, decision);
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
			recordParallelExecution(pool, estimate, decision, detail::elapsedNs(totalStart), taskNs.load(), 0, false);
			return false;
		}

		const auto mergeStart = std::chrono::steady_clock::now();
		for (size_t partition = 0; partition < storage.states.size(); ++partition) {
			std::invoke(merger, partition, storage.states[partition]);
		}
		const uint64_t mergeNs = detail::elapsedNs(mergeStart);
		const uint64_t totalNs = detail::elapsedNs(totalStart);
		if (traceEnabled) {
			debug::PerfTrace::addDuration(mergePhase, mergeNs);
			debug::PerfTrace::addDuration(options.phase, totalNs);
		}
		recordParallelExecution(pool, estimate, decision, totalNs, taskNs.load(std::memory_order_relaxed), mergeNs);
		return true;
	}

	template<typename State, typename Worker, typename Merger>
	bool runOperatorIndexedPartitions(size_t partitionCount, ThreadPool *pool, const ParallelOperatorOptions &options,
									  Worker &&worker, Merger &&merger) {
		const ParallelWorkEstimate estimate = detail::makeEstimate(partitionCount, options);
		const auto decision = decideParallelExecution(pool, estimate);
		return runOperatorIndexedPartitionsWithDecision<State>(
				partitionCount,
				pool,
				options,
				estimate,
				decision,
				std::forward<Worker>(worker),
				std::forward<Merger>(merger));
	}

	template<typename State, typename Worker, typename Merger>
	bool runOperatorRangePartitions(size_t begin, size_t end, size_t partitionCount, ThreadPool *pool,
									const ParallelOperatorOptions &options, Worker &&worker, Merger &&merger) {
		const auto partitions = buildRangePartitions(begin, end, partitionCount);
		auto indexedWorker = [&](size_t partitionIndex, State &state) {
			if constexpr (std::is_same_v<std::invoke_result_t<Worker, const ParallelRangePartition &, State &>, bool>) {
				return std::invoke(worker, partitions[partitionIndex], state);
			} else {
				std::invoke(worker, partitions[partitionIndex], state);
				return true;
			}
		};
		return runOperatorIndexedPartitions<State>(
				partitions.size(), pool, options, indexedWorker, std::forward<Merger>(merger));
	}

	template<typename State, typename Worker, typename Merger>
	bool runOperatorRangePartitions(size_t begin, size_t end, ThreadPool *pool,
									const ParallelOperatorOptions &options, Worker &&worker, Merger &&merger) {
		const size_t totalItems = end > begin ? end - begin : 0;
		if (totalItems == 0) {
			return true;
		}

		const size_t partitionCount = detail::chooseRangePartitionCount(totalItems, pool, options);
		const ParallelWorkEstimate estimate = detail::makeEstimate(partitionCount, options);
		const auto decision = decideParallelExecution(pool, estimate);

		if (decision.useParallel) {
			const auto partitions = buildRangePartitions(begin, end, partitionCount);
			auto indexedWorker = [&](size_t partitionIndex, State &state) {
				if constexpr (std::is_same_v<std::invoke_result_t<Worker, const ParallelRangePartition &, State &>, bool>) {
					return std::invoke(worker, partitions[partitionIndex], state);
				} else {
					std::invoke(worker, partitions[partitionIndex], state);
					return true;
				}
			};
			return runOperatorIndexedPartitionsWithDecision<State>(
					partitions.size(),
					pool,
					options,
					estimate,
					decision,
					indexedWorker,
					std::forward<Merger>(merger));
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled() && !options.phase.empty();
		const auto totalStart = std::chrono::steady_clock::now();
		const std::string taskPhase = traceEnabled ? detail::childPhase(options.phase, ".task") : std::string{};
		const std::string mergePhase = traceEnabled ? detail::childPhase(options.phase, ".merge") : std::string{};
		detail::emitDecisionProfile(options.phase, estimate, decision);

		State state;
		const auto taskStart = std::chrono::steady_clock::now();
		const bool ok = detail::invokeRangeWorker(worker, ParallelRangePartition{begin, end}, state);
		const uint64_t taskNs = detail::elapsedNs(taskStart);
		if (traceEnabled) {
			debug::PerfTrace::addDuration(taskPhase, taskNs);
		}
		if (!ok) {
			const uint64_t totalNs = detail::elapsedNs(totalStart);
			recordParallelExecution(pool, estimate, decision, totalNs, taskNs, 0, false);
			return false;
		}

		const auto mergeStart = std::chrono::steady_clock::now();
		std::invoke(merger, size_t{0}, state);
		const uint64_t mergeNs = detail::elapsedNs(mergeStart);
		const uint64_t totalNs = detail::elapsedNs(totalStart);
		if (traceEnabled) {
			debug::PerfTrace::addDuration(mergePhase, mergeNs);
			debug::PerfTrace::addDuration(options.phase, totalNs);
		}
		recordParallelExecution(pool, estimate, decision, totalNs, taskNs, mergeNs);
		return true;
	}

	class ParallelOperatorExecutor {
	public:
		template<typename State, typename Worker, typename Merger>
		static bool runIndexedPartitions(size_t partitionCount, ThreadPool *pool,
										 const ParallelOperatorOptions &options, Worker &&worker, Merger &&merger) {
			return runOperatorIndexedPartitions<State>(
					partitionCount, pool, options, std::forward<Worker>(worker), std::forward<Merger>(merger));
		}

		template<typename State, typename Worker, typename Merger>
		static bool runRangePartitions(size_t begin, size_t end, size_t partitionCount, ThreadPool *pool,
									   const ParallelOperatorOptions &options, Worker &&worker, Merger &&merger) {
			return runOperatorRangePartitions<State>(
					begin, end, partitionCount, pool, options, std::forward<Worker>(worker), std::forward<Merger>(merger));
		}

		template<typename State, typename Worker, typename Merger>
		static bool runRangePartitions(size_t begin, size_t end, ThreadPool *pool,
									   const ParallelOperatorOptions &options, Worker &&worker, Merger &&merger) {
			return runOperatorRangePartitions<State>(
					begin, end, pool, options, std::forward<Worker>(worker), std::forward<Merger>(merger));
		}
	};

} // namespace graph::concurrent
