/**
 * @file ParallelScanExecutor.hpp
 * @brief Shared partition-local execution helper for scan-style workloads.
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "graph/concurrent/ParallelExecutionPolicy.hpp"
#include "graph/debug/PerfTrace.hpp"

namespace graph::concurrent {

	struct ParallelScanOptions {
		std::string_view phase;
		ParallelWorkloadKind workloadKind = ParallelWorkloadKind::PWK_GENERAL;
		size_t estimatedItems = 0;
		size_t estimatedBytes = 0;
		size_t minPartitions = 2;
		size_t minItems = 1;
		size_t minItemsPerWorker = 0;
		size_t minBytesPerWorker = 0;
		size_t maxWorkers = 0;
	};

	namespace detail {
		template<typename State>
		struct ParallelScanStateStorage {
			explicit ParallelScanStateStorage(size_t partitionCount) : states(partitionCount) {}
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
		bool invokeWorker(Worker &&worker, size_t partition, State &state) {
			if constexpr (std::is_same_v<std::invoke_result_t<Worker, size_t, State &>, bool>) {
				return std::invoke(worker, partition, state);
			} else {
				std::invoke(worker, partition, state);
				return true;
			}
		}
	} // namespace detail

	template<typename State, typename Worker, typename Merger>
	bool runIndexedPartitions(size_t partitionCount, ThreadPool *pool, const ParallelScanOptions &options,
							  Worker &&worker, Merger &&merger) {
		if (partitionCount == 0) {
			return true;
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled() && !options.phase.empty();
		const auto totalStart = std::chrono::steady_clock::now();
		const std::string taskPhase = traceEnabled ? detail::childPhase(options.phase, ".task") : std::string{};
		const std::string mergePhase = traceEnabled ? detail::childPhase(options.phase, ".merge") : std::string{};

		detail::ParallelScanStateStorage<State> storage(partitionCount);
		std::atomic<bool> failed{false};
		auto runOne = [&](size_t partition) {
			if (failed.load(std::memory_order_relaxed)) {
				return;
			}
			const auto taskStart = std::chrono::steady_clock::now();
			const bool ok = detail::invokeWorker(worker, partition, storage.states[partition]);
			if (traceEnabled) {
				debug::PerfTrace::addDuration(taskPhase, detail::elapsedNs(taskStart));
			}
			if (!ok) {
				failed.store(true, std::memory_order_relaxed);
			}
		};

		const ParallelWorkEstimate estimate{.workloadKind = options.workloadKind,
											.partitions = partitionCount,
											.estimatedItems = options.estimatedItems,
											.estimatedBytes = options.estimatedBytes,
											.minPartitions = options.minPartitions,
											.minItems = options.minItems,
											.minItemsPerWorker = options.minItemsPerWorker,
											.minBytesPerWorker = options.minBytesPerWorker,
											.maxWorkers = options.maxWorkers};
		const auto decision = decideParallelExecution(pool, estimate);
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

		if (failed.load(std::memory_order_relaxed)) {
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
		recordParallelExecution(pool, estimate, decision, totalNs, 0, mergeNs);
		return true;
	}

} // namespace graph::concurrent
