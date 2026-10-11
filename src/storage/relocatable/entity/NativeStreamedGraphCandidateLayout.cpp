/**
 * @file NativeStreamedGraphCandidateLayout.cpp
 * @brief Private-candidate range derivation for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphCandidateLayout.hpp"

#include <limits>

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] NativeStreamedGraphCandidateRange appendRange(size_t &next, size_t count) {
			if (count > std::numeric_limits<size_t>::max() - next) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native streamed Edge candidate layout ordinal count overflowed");
			}
			const NativeStreamedGraphCandidateRange result{next, count};
			next += count;
			return result;
		}

	} // namespace

	NativeStreamedGraphCandidateLayout
	NativeStreamedGraphCandidateLayout::derive(const NativeStreamedGraphPublicationPlan &plan,
																		   size_t largeValueContentCandidateSegments,
																		   size_t largeValueReferenceMapCandidateSegments) {
		if (plan.record_count == 0U || plan.source_run_count == 0U ||
			plan.maximumCandidateSegmentCount() == 0U ||
			(plan.large_value_count == 0U) != (largeValueContentCandidateSegments == 0U) ||
			(plan.large_value_count == 0U) != (largeValueReferenceMapCandidateSegments == 0U)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge candidate layout requires complete bounded private families");
		}
		size_t next = 0U;
		NativeStreamedGraphCandidateLayout result;
		result.tokens = appendRange(next, plan.token_candidate_segments);
		result.token_resolver = appendRange(next, plan.token_resolver_candidate_segments);
		result.outgoing_adjacency = appendRange(next, plan.outgoing_adjacency_candidate_segments);
		result.incoming_adjacency = appendRange(next, plan.incoming_adjacency_candidate_segments);
		result.large_value_content = appendRange(next, largeValueContentCandidateSegments);
		result.large_value_reference_map = appendRange(next, largeValueReferenceMapCandidateSegments);
		result.candidate_count = next;
		if (result.candidate_count - largeValueContentCandidateSegments - largeValueReferenceMapCandidateSegments !=
			plan.maximumCandidateSegmentCount()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge candidate layout disagrees with its immutable publication plan");
		}
		if (plan.maximum_candidate_segments != 0U &&
			result.candidate_count > plan.maximum_candidate_segments) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge candidate layout exceeds its transaction-wide candidate bound");
		}
		return result;
	}

} // namespace graph::storage
