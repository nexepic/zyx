/**
 * @file NativeStreamedGraphPublicationIndexPlanner.hpp
 * @brief One ordered deferred-index bundle for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include "graph/storage/relocatable/entity/NativeStreamedGraphAdjacencyIndexPlanner.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphCatalogIndexPlanner.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueContentIndexPlanner.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPrivatePreparation.hpp"

namespace graph::storage {

	struct NativeStreamedGraphPublicationIndexPlanningOptions {
		NativeStreamedGraphCatalogIndexPlanningOptions catalog;
		NativeStreamedGraphAdjacencyIndexPlanningOptions adjacency;
		NativeStreamedGraphLargeValueContentIndexPlanningOptions large_value_content;
	};

	// Combines the three independently typed roots that a streamed Edge commit
	// may change. This is deliberately the only composition point: callers
	// receive one deterministic, duplicate-free deferred-plan bundle for the
	// enclosing metadata generation instead of publishing Catalog, adjacency, or
	// LargeValue state through separate transactions.
	class NativeStreamedGraphPublicationIndexPlanner final {
	public:
		[[nodiscard]] static NativeGraphDeferredIndexPlans
		plan(SystemCatalog &catalog, BTreePageStore &adjacencyPages, LargeValueStore *largeValueStore,
			 RelocatableMetadataStore &metadata, const NativeStreamedGraphPrivatePreparation &prepared,
			 const NativeStreamedGraphPublicationIndexPlanningOptions &options,
			 const NativeGraphDeferredIndexPlanningContext &planning, LogSequenceNumber checkpointLsn);
	};

} // namespace graph::storage
