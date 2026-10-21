/** Bounded type membership maintenance inside streamed graph publication. */
#pragma once
#include "graph/storage/relocatable/entity/NativeStreamedGraphCatalogIndexPlanner.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphDeferredPublication.hpp"
namespace graph::storage {
	struct NativeStreamedGraphMembershipIndexPlanningOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		size_t maximum_entries = 0U;
		size_t maximum_pages = 0U;
		size_t maximum_definitions = 65536U;
		size_t sort_run_bytes = 0U;
		size_t maximum_open_runs = 0U;
	};
	class NativeStreamedGraphMembershipIndexPlanner final {
	public:
		[[nodiscard]] static NativeGraphDeferredIndexPlans
		plan(SystemCatalog &catalog, BTreePageStore &pages, RelocatableMetadataStore &metadata,
			 const NativeStreamedGraphDeferredPublication *publication,
			 const NativeStreamedGraphMembershipIndexPlanningOptions &options, const MetadataSnapshotPin &snapshot,
			 LogSequenceNumber checkpointLsn, OrderedBTreeEntrySource *tokenCountUpdates = nullptr,
             const NativeGraphDeferredIndexPlanningContext *planning = nullptr);
	};
} // namespace graph::storage
