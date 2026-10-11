/**
 * @file NativeBulkEdgePublicationPreflight.hpp
 * @brief Bounded preflight validation for one streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#include "graph/storage/relocatable/entity/NativeBulkEdgeRunStore.hpp"

namespace graph::storage {

	struct NativeBulkEdgePublicationPreflightOptions {
		size_t maximum_records = 0U;
		size_t sort_run_bytes = 0U;
		size_t maximum_sort_runs = 0U;
		size_t maximum_candidate_fragments = 0U;
	};

	struct NativeBulkEdgePublicationPreflightResult {
		size_t record_count = 0U;
		// The sealed source is strictly Edge-ID ordered. Persisting its exact span
		// in the count-only plan lets later page-directory sessions accept sparse
		// IDs across more than one durable lease without retaining every ID.
		uint64_t first_edge_id = 0U;
		uint64_t last_edge_id = 0U;
		// A Property root stores a fixed-width token for every key. Therefore its
		// inline/overflow shape is independent of the eventual Catalog token IDs
		// and can be determined before private candidate allocation.
		size_t property_root_count = 0U;
		size_t large_value_count = 0U;
		// Counted while the sealed source is already being validated. This is a
		// scalar resource bound, not a retained token-name collection.
		size_t token_entry_count = 0U;
		// Count of source-sized runs observed during validation. The durable source
		// is consumed directly, so this is diagnostic rather than candidate work.
		size_t source_run_count = 0U;
		size_t token_fragments = 0U;
		size_t outgoing_adjacency_fragments = 0U;
		size_t incoming_adjacency_fragments = 0U;

		[[nodiscard]] size_t derivedCandidateFragments() const;
	};

	using NativeBulkEdgePersistedNodeValidator = std::function<bool(uint64_t)>;

	class NativeBulkEdgePublicationPreflight final {
	public:
		// This phase allocates no Catalog, Entity, Property, or private candidate
		// state. It deliberately repeats endpoint and lease validation against the
		// writer-pinned snapshot so staging-time observations cannot become stale.
		[[nodiscard]] static NativeBulkEdgePublicationPreflightResult
		validate(const NativeBulkEdgeRecordSource &source, std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
				 const NativeBulkEdgePublicationPreflightOptions &options,
				 const NativeBulkEdgePersistedNodeValidator &persistedNodeExists);
	};

} // namespace graph::storage
