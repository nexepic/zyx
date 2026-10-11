/**
 * @file NativeStreamedGraphAdjacencyIndexPlanner.hpp
 * @brief Deferred global adjacency-tree planning for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstdint>

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateFamilyMaterializer.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"

namespace graph::storage {

	struct NativeStreamedGraphAdjacencyIndexPlanningOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		size_t maximum_entries = 0U;
		size_t maximum_pages = 0U;
		uint16_t maximum_manifest_depth = 0U;
	};

	// Plans the one global ordinary-graph adjacency B+Tree transition from the
	// two direction-partitioned private runs. It never creates per-Node roots or
	// independently publishes either direction.
	class NativeStreamedGraphAdjacencyIndexPlanner final {
	public:
		[[nodiscard]] static NativeGraphDeferredIndexPlan
		plan(BTreePageStore &pages, RelocatableMetadataStore &metadata,
			 const NativeBulkEdgePrivateAdjacencyRoots &privateRoots,
			 const NativeStreamedGraphAdjacencyIndexPlanningOptions &options,
			 const NativeGraphDeferredIndexPlanningContext &planning, LogSequenceNumber checkpointLsn);
	};

} // namespace graph::storage
