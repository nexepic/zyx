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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "graph/concurrent/ParallelExecutionPolicy.hpp"

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
		struct ParallelOperatorCallbacks {
			void *context = nullptr;
			bool (*run)(void *context, size_t partition) = nullptr;
			void (*merge)(void *context, size_t partition) = nullptr;
		};

		// The scheduling/telemetry loop is deliberately non-template so every
		// query operator shares one compact, consistently tested implementation.
		[[nodiscard]] bool runOperatorIndexedPartitionsCore(size_t partitionCount,
															ThreadPool *pool,
															const ParallelOperatorOptions &options,
															const ParallelWorkEstimate &estimate,
															const ParallelExecutionDecision &decision,
															const ParallelOperatorCallbacks &callbacks);

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

		[[nodiscard]] ParallelWorkEstimate makeEstimate(size_t partitionCount, const ParallelOperatorOptions &options);
		[[nodiscard]] size_t chooseRangePartitionCount(size_t totalItems,
																ThreadPool *pool,
																const ParallelOperatorOptions &options);

		template<typename State, typename Worker, typename Merger>
		struct IndexedCallbackContext {
			explicit IndexedCallbackContext(size_t partitionCount, Worker &worker, Merger &merger) :
					states(partitionCount), worker(worker), merger(merger) {}

			std::vector<State> states;
			Worker &worker;
			Merger &merger;
		};

		template<typename State, typename Worker, typename Merger>
		bool runIndexedCallback(void *context, size_t partition) {
			auto &typed = *static_cast<IndexedCallbackContext<State, Worker, Merger> *>(context);
			return invokeIndexedWorker(typed.worker, partition, typed.states[partition]);
		}

		template<typename State, typename Worker, typename Merger>
		void mergeIndexedCallback(void *context, size_t partition) {
			auto &typed = *static_cast<IndexedCallbackContext<State, Worker, Merger> *>(context);
			std::invoke(typed.merger, partition, typed.states[partition]);
		}

		template<typename State, typename Worker, typename Merger>
		struct RangeCallbackContext {
			RangeCallbackContext(const std::vector<ParallelRangePartition> &partitions, Worker &worker, Merger &merger) :
					partitions(partitions), states(partitions.size()), worker(worker), merger(merger) {}

			const std::vector<ParallelRangePartition> &partitions;
			std::vector<State> states;
			Worker &worker;
			Merger &merger;
		};

		template<typename State, typename Worker, typename Merger>
		bool runRangeCallback(void *context, size_t partition) {
			auto &typed = *static_cast<RangeCallbackContext<State, Worker, Merger> *>(context);
			return invokeRangeWorker(typed.worker, typed.partitions[partition], typed.states[partition]);
		}

		template<typename State, typename Worker, typename Merger>
		void mergeRangeCallback(void *context, size_t partition) {
			auto &typed = *static_cast<RangeCallbackContext<State, Worker, Merger> *>(context);
			std::invoke(typed.merger, partition, typed.states[partition]);
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
		detail::IndexedCallbackContext<State, Worker, Merger> context(partitionCount, worker, merger);
		return detail::runOperatorIndexedPartitionsCore(
				partitionCount,
				pool,
				options,
				estimate,
				decision,
				{.context = &context,
				 .run = &detail::runIndexedCallback<State, Worker, Merger>,
				 .merge = &detail::mergeIndexedCallback<State, Worker, Merger>});
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
		const ParallelWorkEstimate estimate = detail::makeEstimate(partitions.size(), options);
		const auto decision = decideParallelExecution(pool, estimate);
		detail::RangeCallbackContext<State, Worker, Merger> context(partitions, worker, merger);
		return detail::runOperatorIndexedPartitionsCore(
				partitions.size(),
				pool,
				options,
				estimate,
				decision,
				{.context = &context,
				 .run = &detail::runRangeCallback<State, Worker, Merger>,
				 .merge = &detail::mergeRangeCallback<State, Worker, Merger>});
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

		const std::vector<ParallelRangePartition> partitions = decision.useParallel
															 ? buildRangePartitions(begin, end, partitionCount)
															 : std::vector<ParallelRangePartition>{{begin, end}};
		detail::RangeCallbackContext<State, Worker, Merger> context(partitions, worker, merger);
		return detail::runOperatorIndexedPartitionsCore(
				partitions.size(),
				pool,
				options,
				estimate,
				decision,
				{.context = &context,
				 .run = &detail::runRangeCallback<State, Worker, Merger>,
				 .merge = &detail::mergeRangeCallback<State, Worker, Merger>});
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
