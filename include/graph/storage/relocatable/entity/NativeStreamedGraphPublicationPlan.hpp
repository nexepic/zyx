/**
 * @file NativeStreamedGraphPublicationPlan.hpp
 * @brief Immutable bounded-work shape for one streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "graph/storage/relocatable/entity/NativeBulkEdgePublicationPreflight.hpp"

namespace graph::storage {

	// This is deliberately a count-only plan. It owns no record, token name,
	// candidate payload, physical slot, or public metadata handle. The eventual
	// coordinator recreates each bounded source pass from its callback-scoped
	// private candidate reader and consumes this shape in one publication.
	struct NativeStreamedGraphPublicationPlan {
		uint64_t record_count = 0U;
		size_t edge_entity_page_count = 0U;
		// This shape is independent of the eventual numeric Catalog tokens because
		// Property roots encode every token in a fixed-width field. The final
		// coordinator uses it to reserve a contiguous LargeValue identity range.
		size_t property_root_count = 0U;
		size_t large_value_count = 0U;
		// Diagnostic cardinality of the already durable source. These fragments
		// are consumed in place and are not charged again as derived candidates.
		size_t source_run_count = 0U;
		size_t token_entry_count = 0U;
		// Each family includes sorted fragment data plus its immutable manifest
		// hierarchy for every retained reduction level. A merge output can contain
		// as many fragments as its inputs, so counting only fan-in groups would
		// under-reserve the candidate suffix.
		size_t token_candidate_segments = 0U;
		// The Catalog token stream is ordered by token name whereas Edge records
		// are streamed by Edge ID. This private immutable lookup tree prevents an
		// O(records * tokens) scan without retaining a transaction-wide name map.
		size_t token_resolver_candidate_segments = 0U;
		size_t outgoing_adjacency_candidate_segments = 0U;
		size_t incoming_adjacency_candidate_segments = 0U;
		// This is the transaction-wide ceiling inherited from preflight. The
		// LargeValue content run is derived only after this core plan, so the
		// candidate layout must apply this cap again when it appends that family.
		size_t maximum_candidate_segments = 0U;
		size_t merge_passes = 0U;
		// Keep this span after the original shape fields so existing aggregate
		// initializers remain count-only test fixtures. Production preflight fills
		// it before any metadata-owned directory session is opened.
		uint64_t first_edge_id = 0U;
		uint64_t last_edge_id = 0U;

		// This is an admission and placement upper bound, not an exact allocation
		// count. The final bounded merger may compact entries, but it must never
		// require more private candidate segments than this result.
		[[nodiscard]] size_t maximumCandidateSegmentCount() const;

		// Validates the sealed source against the writer-fenced endpoint view and
		// derives every temporary candidate level required by a bounded fan-in
		// merge. A configured fan-in therefore limits actual future work, not just
		// an admission check.
		[[nodiscard]] static NativeStreamedGraphPublicationPlan
		derive(const NativeBulkEdgeRecordSource &source, std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
			   const NativeBulkEdgePublicationPreflightOptions &options,
			   const NativeBulkEdgePersistedNodeValidator &persistedNodeExists);
	};

} // namespace graph::storage
