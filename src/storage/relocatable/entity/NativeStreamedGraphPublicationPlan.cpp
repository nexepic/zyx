/**
 * @file NativeStreamedGraphPublicationPlan.cpp
 * @brief Bounded-work planning for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationPlan.hpp"

#include <algorithm>
#include <limits>

#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunPipeline.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateTokenResolver.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] size_t checkedAdd(size_t left, size_t right, const char *message) {
			if (right > std::numeric_limits<size_t>::max() - left) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
			return left + right;
		}

		[[nodiscard]] size_t checkedCeilingDivide(size_t numerator, size_t denominator, const char *message) {
			if (numerator == 0U || denominator == 0U) {
				fail(FormatError::FERR_INVALID_HEADER, message);
			}
			return (numerator - 1U) / denominator + 1U;
		}

		struct ReductionShape {
			size_t retained_candidate_segments = 0U;
			size_t passes = 0U;
		};

		[[nodiscard]] ReductionShape deriveReductionShape(size_t initialRuns, size_t maximumFanIn) {
			if (maximumFanIn < 2U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native streamed Edge publication requires a usable merge fan-in");
			}
			if (initialRuns == 0U) {
				return {};
			}
			ReductionShape result{NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(initialRuns, maximumFanIn),
								  0U};
			for (size_t levelRuns = initialRuns; levelRuns > 1U;) {
				const size_t nextLevelRuns = checkedCeilingDivide(
						levelRuns, maximumFanIn, "native streamed Edge merge level has an invalid fan-in");
				result.passes = checkedAdd(result.passes, 1U, "native streamed Edge merge pass count overflowed");
				levelRuns = nextLevelRuns;
			}
			return result;
		}

	} // namespace

	size_t NativeStreamedGraphPublicationPlan::maximumCandidateSegmentCount() const {
		size_t total = token_candidate_segments;
		total = checkedAdd(total, token_resolver_candidate_segments, "native streamed Edge candidate count overflowed");
		total = checkedAdd(total, outgoing_adjacency_candidate_segments,
						   "native streamed Edge candidate count overflowed");
		return checkedAdd(total, incoming_adjacency_candidate_segments,
						  "native streamed Edge candidate count overflowed");
	}

	NativeStreamedGraphPublicationPlan
	NativeStreamedGraphPublicationPlan::derive(const NativeBulkEdgeRecordSource &source,
											   std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
											   const NativeBulkEdgePublicationPreflightOptions &options,
											   const NativeBulkEdgePersistedNodeValidator &persistedNodeExists) {
		const NativeBulkEdgePublicationPreflightResult preflight =
				NativeBulkEdgePublicationPreflight::validate(source, edgeIdRanges, options, persistedNodeExists);
		const size_t entityCapacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_EDGE);
		if (entityCapacity == 0U || preflight.record_count == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge publication cannot derive an Entity-page batch shape");
		}

		const ReductionShape token = deriveReductionShape(preflight.token_fragments, options.maximum_sort_runs);
		const ReductionShape outgoing =
				deriveReductionShape(preflight.outgoing_adjacency_fragments, options.maximum_sort_runs);
		const ReductionShape incoming =
				deriveReductionShape(preflight.incoming_adjacency_fragments, options.maximum_sort_runs);
		NativeStreamedGraphPublicationPlan result;
		result.record_count = preflight.record_count;
		result.first_edge_id = preflight.first_edge_id;
		result.last_edge_id = preflight.last_edge_id;
		result.edge_entity_page_count = checkedCeilingDivide(preflight.record_count, entityCapacity,
														 "native streamed Edge Entity-page count is invalid");
		result.property_root_count = preflight.property_root_count;
		result.large_value_count = preflight.large_value_count;
		result.source_run_count = preflight.source_run_count;
		result.token_entry_count = preflight.token_entry_count;
		result.token_candidate_segments = token.retained_candidate_segments;
		result.token_resolver_candidate_segments =
				NativeBulkEdgePrivateTokenResolver::maximumCandidateSegments(result.token_entry_count);
		result.outgoing_adjacency_candidate_segments = outgoing.retained_candidate_segments;
		result.incoming_adjacency_candidate_segments = incoming.retained_candidate_segments;
		result.maximum_candidate_segments = options.maximum_candidate_fragments;
		result.merge_passes = std::max({token.passes, outgoing.passes, incoming.passes});
		if (result.maximumCandidateSegmentCount() > options.maximum_candidate_fragments) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge publication exceeds its full multi-pass candidate bound");
		}
		return result;
	}

} // namespace graph::storage
