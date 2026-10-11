/**
 * @file NativeStreamedGraphLargeValueContentIndexPlanner.hpp
 * @brief Deferred B+Tree planning for streamed LargeValue content identities.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValuePrivateMaterializer.hpp"

namespace graph::storage {

	struct NativeStreamedGraphLargeValueContentIndexPlanningOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		size_t maximum_entries = 0U;
		size_t maximum_pages = 0U;
		uint16_t maximum_manifest_depth = 0U;
	};

	// Plans, but does not publish, the reserved LargeValue content-index root.
	// Both old and private-new entries are rewindable ordered sources, so the
	// dense builder remains bounded across its sizing and frame passes.
	class NativeStreamedGraphLargeValueContentIndexPlanner final {
	public:
		[[nodiscard]] static NativeGraphDeferredIndexPlan
		plan(LargeValueStore &store, RelocatableMetadataStore &metadata, NativeBulkRunFragmentLocation privateRunRoot,
			 const NativeStreamedGraphLargeValueContentIndexPlanningOptions &options,
			 const NativeGraphDeferredIndexPlanningContext &planning, LogSequenceNumber checkpointLsn);
	};

} // namespace graph::storage
