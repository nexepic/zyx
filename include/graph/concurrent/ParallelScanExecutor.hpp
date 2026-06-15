/**
 * @file ParallelScanExecutor.hpp
 * @brief Backward-compatible scan facade over ParallelOperatorExecutor.
 *
 * New operator code should include ParallelOperatorExecutor.hpp directly. This
 * facade keeps existing scan/storage call sites source-compatible while all
 * policy, telemetry, worker-local state, and merge behavior lives in one place.
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <utility>

#include "graph/concurrent/ParallelOperatorExecutor.hpp"

namespace graph::concurrent {

	using ParallelScanOptions = ParallelOperatorOptions;

	template<typename State, typename Worker, typename Merger>
	bool runIndexedPartitions(size_t partitionCount, ThreadPool *pool, const ParallelScanOptions &options,
							  Worker &&worker, Merger &&merger) {
		return runOperatorIndexedPartitions<State>(
				partitionCount, pool, options, std::forward<Worker>(worker), std::forward<Merger>(merger));
	}

} // namespace graph::concurrent
