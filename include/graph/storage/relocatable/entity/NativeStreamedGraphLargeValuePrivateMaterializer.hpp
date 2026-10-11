/**
 * @file NativeStreamedGraphLargeValuePrivateMaterializer.hpp
 * @brief Writer-scoped private materialization of streamed LargeValue index runs.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunPipeline.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueContentRunBuilder.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueReferenceMapBuilder.hpp"

namespace graph::storage {

	struct NativeStreamedGraphLargeValuePrivateMaterializationOptions {
		size_t maximum_run_bytes = 0U;
		size_t maximum_open_runs = 0U;
		size_t maximum_entries = 0U;
		uint16_t maximum_manifest_depth = 0U;
	};

	// Keeps the content-index sorting/merge intermediate state inside the
	// enclosing metadata writer's unpublished candidate suffix. Its result is a
	// rewindable ordered source root, not a public Catalog or Page Directory
	// route; a final coordinator is still responsible for publishing the B+Tree.
	class NativeStreamedGraphLargeValuePrivateMaterializer final {
	public:
		[[nodiscard]] static NativeBulkRunFragmentLocation
		materialize(const NativeStreamedGraphDeferredPublication &publication, const LargeValueStore &store,
					LargeValueId firstValueId, const NativeStreamedGraphLargeValuePlan &valuePlan,
					const NativeStreamedGraphLargeValuePlanningOptions &valueOptions,
					const NativeStreamedGraphLargeValueContentRunPlan &contentPlan,
					const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange candidateRange,
					const NativeStreamedGraphLargeValuePrivateMaterializationOptions &options,
					const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
					const NativeGraphPrivateSegmentFence &fence);
		[[nodiscard]] static NativeBulkRunFragmentLocation materializeReferenceMap(
				const OrderedBTreeEntrySource &contentEntries, const LargeValueStore &store, LargeValueId firstValueId,
				size_t referenceCount, const NativeStreamedGraphLargeValueReferenceMapRunPlan &mapPlan,
				const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange candidateRange,
				const NativeStreamedGraphLargeValuePrivateMaterializationOptions &options,
				const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
				const NativeGraphPrivateSegmentFence &fence);
	};

} // namespace graph::storage
