/**
 * @file NativeStreamedGraphCatalogIndexPlanner.hpp
 * @brief Deferred System Catalog tree planning for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <optional>

#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

namespace graph::storage {

	struct NativeStreamedGraphCatalogIndexPlanningOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		size_t maximum_entries = 0U;
		size_t maximum_pages = 0U;
	};

	// Converts the complete, ordered token Catalog source into one native tree
	// transition. The caller supplies the already-merged final source produced by
	// NativeBulkEdgeCatalogTokenMaterializer; this class never makes an
	// independently visible Catalog publication.
	class NativeStreamedGraphCatalogIndexPlanner final {
	public:
		[[nodiscard]] static std::optional<NativeGraphDeferredIndexPlan>
		plan(SystemCatalog &catalog, RelocatableMetadataStore &metadata, OrderedBTreeEntrySource finalEntries,
			 bool catalogChanged, const NativeStreamedGraphCatalogIndexPlanningOptions &options,
			 const MetadataSnapshotPin &snapshot, LogSequenceNumber checkpointLsn);
	};

} // namespace graph::storage
