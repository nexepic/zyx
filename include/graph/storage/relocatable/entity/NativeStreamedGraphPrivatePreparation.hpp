/**
 * @file NativeStreamedGraphPrivatePreparation.hpp
 * @brief Writer-scoped private preparation for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateFamilyMaterializer.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateTokenResolver.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphCandidateLayout.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphDeferredPublication.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValuePrivateMaterializer.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationShape.hpp"

namespace graph::storage {

	struct NativeStreamedGraphPrivatePreparationOptions {
		NativeBulkEdgePrivateFamilyMaterializationOptions families;
		size_t maximum_catalog_entries = 0U;
		// The store is required only when the preflight found Property roots that
		// overflow their slotted-page representation. The private content run is
		// still unreachable until the final graph publication publishes its index
		// root and LargeValue manifest routes in the same generation.
		LargeValueStore *large_value_store = nullptr;
		NativeStreamedGraphLargeValuePlanningOptions large_value_options;
		NativeStreamedGraphLargeValuePrivateMaterializationOptions large_value_content_options;
	};

	struct NativeStreamedGraphLargeValuePrivatePreparation {
		LargeValueId first_value_id{};
		NativeStreamedGraphLargeValuePlan value_plan;
		NativeStreamedGraphLargeValueContentRunPlan content_run_plan;
		NativeBulkRunFragmentLocation content_run;
		NativeStreamedGraphLargeValueReferenceMapRunPlan reference_map_plan;
		NativeBulkRunFragmentLocation reference_map_run;
	};

	// This object is valid only while the preparation visitor is executing. It is
	// deliberately not a publication result: the final coordinator must still
	// materialize public Entity, Property/LargeValue, Catalog, and adjacency roots
	// in one generation.
	struct NativeStreamedGraphPrivatePreparation {
		std::optional<NativeBulkRunFragmentLocation> token_run;
		NativeBulkEdgeCatalogTokenMaterialization catalog_tokens;
		NativeBulkEdgePrivateTokenResolver token_resolver = NativeBulkEdgePrivateTokenResolver::makeEmpty();
		NativeBulkEdgePrivateAdjacencyRoots adjacency_roots;
		std::optional<NativeStreamedGraphLargeValuePrivatePreparation> large_values;
		// The final coordinator consumes this scalar shape before allocating any
		// public PageId. It is deliberately independent of the private candidates.
		std::optional<NativeStreamedGraphPublicationShape> publication_shape;
	};
	using NativeStreamedGraphPrivatePreparationVisitor =
			std::function<void(const NativeStreamedGraphPrivatePreparation &)>;
	// Both arguments are valid only during this callback. In particular, the
	// deferred source's token resolver borrows the private resolver owned by
	// `preparation`; callers must consume page-local batches before returning.
	using NativeStreamedGraphPreparedPublicationVisitor =
			std::function<void(const NativeStreamedGraphPrivatePreparation &preparation,
							   const NativeStreamedGraphDeferredPublication &publication)>;

	class NativeStreamedGraphPrivatePreparationCoordinator final {
	public:
		static void
		prepare(const NativeBulkEdgeRecordSource &source, std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
				const NativeStreamedGraphCandidateLayout &layout, const NativeGraphPrivateSegmentSet &identities,
				SystemCatalog &catalog, const MetadataSnapshotPin &snapshot,
				const NativeStreamedGraphPrivatePreparationOptions &options,
				const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
				const NativeGraphPrivateSegmentFence &fence, const NativeStreamedGraphPrivatePreparationVisitor &visit,
				uint64_t edgeCountIncrement = 0U);
		// Produces the only supported final graph-page source for a streamed Edge
		// publication. It cannot outlive the writer-scoped private Token resolver
		// and does not expose Catalog, allocator, PageId, or physical-slot control.
		static void prepareDeferredPublication(const NativeBulkEdgeRecordSource &source,
											   std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
											   const NativeStreamedGraphPublicationPlan &publicationPlan,
											   const NativeStreamedGraphCandidateLayout &layout,
											   const NativeGraphPrivateSegmentSet &identities, SystemCatalog &catalog,
											   const MetadataSnapshotPin &snapshot,
											   const NativeStreamedGraphPrivatePreparationOptions &options,
											   const NativeGraphPrivateSegmentReader &reader,
											   const NativeGraphPrivateSegmentWriter &writer,
											   const NativeGraphPrivateSegmentFence &fence,
											   const NativeStreamedGraphPreparedPublicationVisitor &visit);
	};

} // namespace graph::storage
