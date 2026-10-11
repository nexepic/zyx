/**
 * @file NativeStreamedGraphCandidateLayout.hpp
 * @brief Immutable private-candidate ranges for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>

#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationPlan.hpp"

namespace graph::storage {

	struct NativeStreamedGraphCandidateRange {
		size_t first_ordinal = 0U;
		size_t count = 0U;

		[[nodiscard]] bool contains(size_t ordinal) const {
			return ordinal >= first_ordinal && ordinal - first_ordinal < count;
		}
	};

	// This layout reserves every candidate identity before any private payload is
	// written. It owns only scalar ordinals; actual payload construction remains
	// callback-scoped under the metadata writer.
	struct NativeStreamedGraphCandidateLayout {
		NativeStreamedGraphCandidateRange tokens;
		NativeStreamedGraphCandidateRange token_resolver;
		NativeStreamedGraphCandidateRange outgoing_adjacency;
		NativeStreamedGraphCandidateRange incoming_adjacency;
		// Materialized only when the sealed source contains overflowing Property
		// roots. Its precise size is derived from the preflight count and the
		// fixed content-index entry codec before the metadata writer reserves its
		// private candidate tail.
		NativeStreamedGraphCandidateRange large_value_content;
		// Sorted by provisional value ID and consumed during each final source
		// replay to translate an overflow reference to its canonical payload.
		NativeStreamedGraphCandidateRange large_value_reference_map;
		size_t candidate_count = 0U;

		[[nodiscard]] static NativeStreamedGraphCandidateLayout derive(const NativeStreamedGraphPublicationPlan &plan,
																		   size_t largeValueContentCandidateSegments = 0U,
																		   size_t largeValueReferenceMapCandidateSegments = 0U);
	};

} // namespace graph::storage
