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

namespace graph::concurrent {

	enum class ParallelWorkloadKind {
		PWK_GENERAL,
		PWK_CPU_BOUND,
		PWK_MEMORY_SCAN,
		PWK_MEMORY_INTENSIVE,
		PWK_STORAGE_SCAN
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

} // namespace graph::concurrent
