/**
 * @file NativeStreamedGraphLargeValueContentRunBuilder.hpp
 * @brief Bounded content-index runs for streamed LargeValue publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>

#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValuePlanner.hpp"
#include "graph/storage/relocatable/index/NativeBulkRun.hpp"

namespace graph::storage {

	using NativeStreamedGraphLargeValueContentRunVisitor = std::function<void(NativeBulkRunFragment &&)>;

	struct NativeStreamedGraphLargeValueContentRunPlan {
		size_t initial_fragment_count = 0U;
		size_t candidate_segment_count = 0U;
	};

	// Creates independently sorted private runs of the reserved LargeValue
	// content-index entries. The final coordinator performs the bounded merge
	// with the current tree and publishes its replacement root in the same
	// generation as the corresponding manifest directory entries.
	class NativeStreamedGraphLargeValueContentRunBuilder final {
	public:
		// Content-index keys have a fixed wire length for Entity Property roots.
		// This permits the writer to reserve the exact private-run family from a
		// preflight overflow count, before it has materialized tokenized roots.
		[[nodiscard]] static NativeStreamedGraphLargeValueContentRunPlan
		derivePlanForValueCount(const LargeValueStore &store, size_t valueCount, size_t maximumRunBytes,
								size_t maximumOpenRuns);
		// Measures private scratch shape without retaining transaction-wide index
		// entries, allowing a later metadata batch to reserve exact candidates.
		[[nodiscard]] static NativeStreamedGraphLargeValueContentRunPlan
		derivePlan(const NativeStreamedGraphDeferredPublication &publication, const LargeValueStore &store,
				   LargeValueId firstValueId, const NativeStreamedGraphLargeValuePlan &plan,
				   const NativeStreamedGraphLargeValuePlanningOptions &options, size_t maximumRunBytes,
				   size_t maximumOpenRuns);
		static void buildRuns(const NativeStreamedGraphDeferredPublication &publication, const LargeValueStore &store,
							  LargeValueId firstValueId, const NativeStreamedGraphLargeValuePlan &plan,
							  const NativeStreamedGraphLargeValuePlanningOptions &options, size_t maximumRunBytes,
							  size_t maximumFragments, const NativeStreamedGraphLargeValueContentRunVisitor &visitor);
	};

} // namespace graph::storage
