/**
 * @file NativeBulkEdgePrivateFamilyMaterializer.hpp
 * @brief Private bounded token and adjacency family materialization.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <optional>
#include <span>

#include "graph/storage/relocatable/entity/NativeBulkEdgeAdjacencyRunBuilder.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunPipeline.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeTokenRunBuilder.hpp"

namespace graph::storage {

	struct NativeBulkEdgePrivateFamilyMaterializationOptions {
		size_t maximum_records = 0U;
		// A record can contribute a relationship type and multiple Property names,
		// so token reduction has a distinct bound from adjacency reduction.
		size_t maximum_token_entries = 0U;
		size_t maximum_sort_run_bytes = 0U;
		NativeBulkEdgePrivateRunPipelineOptions merge;
	};

	struct NativeBulkEdgePrivateAdjacencyRoots {
		NativeBulkRunFragmentLocation outgoing;
		NativeBulkRunFragmentLocation incoming;
	};

	// Builds source-specific initial runs directly into the common private merge
	// pipeline. It does not retain token names, adjacency keys, or all root
	// manifests; all callback state is bounded by the configured source builder.
	class NativeBulkEdgePrivateFamilyMaterializer final {
	public:
		[[nodiscard]] static std::optional<NativeBulkRunFragmentLocation> materializeTokens(
				const NativeBulkEdgeRecordSource &source, const NativeGraphPrivateSegmentSet &identities,
				NativeBulkEdgePrivateCandidateRange candidateRange,
				const NativeBulkEdgePrivateFamilyMaterializationOptions &options,
				const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
				const NativeGraphPrivateSegmentFence &fence);

		[[nodiscard]] static NativeBulkEdgePrivateAdjacencyRoots materializeAdjacency(
				const NativeBulkEdgeRecordSource &source, std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
				const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange outgoingRange,
				NativeBulkEdgePrivateCandidateRange incomingRange,
				const NativeBulkEdgePrivateFamilyMaterializationOptions &options,
				const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
				const NativeGraphPrivateSegmentFence &fence);
	};

} // namespace graph::storage
