/**
 * @file ParallelExecutionTypes.hpp
 * @brief Shared types for parallel execution decisions.
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace graph::concurrent {

	enum class ParallelWorkloadKind {
		PWK_GENERAL,
		PWK_CPU_BOUND,
		PWK_MEMORY_SCAN,
		PWK_MEMORY_INTENSIVE,
		PWK_STORAGE_SCAN,
		PWK_ADJACENCY_TRAVERSAL
	};

	enum class ParallelDecisionReason {
		PDR_PARALLEL,
		PDR_NO_WORKERS,
		PDR_INSUFFICIENT_PARTITIONS,
		PDR_INSUFFICIENT_ITEMS,
		PDR_INSUFFICIENT_GRANULARITY
	};

	struct ParallelWorkEstimate {
		ParallelWorkloadKind workloadKind = ParallelWorkloadKind::PWK_GENERAL;
		size_t partitions = 0;
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

	struct ParallelExecutionDecision {
		bool useParallel = false;
		bool usedAdaptiveRecommendation = false;
		size_t workerCount = 1;
		size_t baselineWorkerCount = 1;
		size_t hardWorkerLimit = 1;
		ParallelDecisionReason reason = ParallelDecisionReason::PDR_NO_WORKERS;
	};

	struct ParallelExecutionTelemetry {
		ParallelWorkEstimate estimate;
		size_t workerCount = 1;
		uint64_t elapsedNs = 0;
		uint64_t taskNs = 0;
		uint64_t mergeNs = 0;
		bool completed = true;
	};

	inline std::string_view parallelDecisionReasonName(ParallelDecisionReason reason) {
		switch (reason) {
			case ParallelDecisionReason::PDR_PARALLEL:
				return "parallel";
			case ParallelDecisionReason::PDR_NO_WORKERS:
				return "no_workers";
			case ParallelDecisionReason::PDR_INSUFFICIENT_PARTITIONS:
				return "insufficient_partitions";
			case ParallelDecisionReason::PDR_INSUFFICIENT_ITEMS:
				return "insufficient_items";
			case ParallelDecisionReason::PDR_INSUFFICIENT_GRANULARITY:
				return "insufficient_granularity";
		}
		return "unknown";
	}

	inline int64_t parallelDecisionReasonCode(ParallelDecisionReason reason) {
		switch (reason) {
			case ParallelDecisionReason::PDR_PARALLEL:
				return 0;
			case ParallelDecisionReason::PDR_NO_WORKERS:
				return 1;
			case ParallelDecisionReason::PDR_INSUFFICIENT_PARTITIONS:
				return 2;
			case ParallelDecisionReason::PDR_INSUFFICIENT_ITEMS:
				return 3;
			case ParallelDecisionReason::PDR_INSUFFICIENT_GRANULARITY:
				return 4;
		}
		return -1;
	}

} // namespace graph::concurrent
