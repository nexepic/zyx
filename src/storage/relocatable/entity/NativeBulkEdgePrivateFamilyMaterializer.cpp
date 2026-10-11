/**
 * @file NativeBulkEdgePrivateFamilyMaterializer.cpp
 * @brief Private bounded token and adjacency family materialization.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateFamilyMaterializer.hpp"

#include "graph/debug/PerfTrace.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validate(const NativeBulkEdgeRecordSource &source,
					  const NativeBulkEdgePrivateFamilyMaterializationOptions &options,
					  const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
					  const NativeGraphPrivateSegmentFence &fence) {
			if (!source || options.maximum_records == 0U || options.maximum_token_entries == 0U ||
				options.maximum_sort_run_bytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES || !reader || !writer || !fence) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge private family materializer requires bounded source and private callbacks");
			}
		}

	} // namespace

	std::optional<NativeBulkRunFragmentLocation> NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
			const NativeBulkEdgeRecordSource &source, const NativeGraphPrivateSegmentSet &identities,
			NativeBulkEdgePrivateCandidateRange candidateRange,
			const NativeBulkEdgePrivateFamilyMaterializationOptions &options,
			const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
			const NativeGraphPrivateSegmentFence &fence) {
		validate(source, options, reader, writer, fence);
		if (candidateRange.count == 0U) {
			bool sawToken = false;
			NativeBulkEdgeTokenRunBuilder::buildRuns(
					source, options.maximum_records, options.maximum_sort_run_bytes, 1U,
					[&sawToken](NativeBulkRunFragment &&) { sawToken = true; });
			if (sawToken) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge token family omitted candidates for nonempty input");
			}
			return std::nullopt;
		}
		NativeBulkEdgePrivateRunPipelineOptions tokenMerge = options.merge;
		tokenMerge.maximum_entries = options.maximum_token_entries;
		tokenMerge.duplicate_policy = NativeBulkEdgeRunDuplicatePolicy::NBERDP_KEEP_ONE;
		return NativeBulkEdgePrivateRunPipeline::materializeInitialFragments(
				identities, candidateRange,
				[&](const NativeBulkEdgePrivateRunFragmentVisitor &visit) {
					NativeBulkEdgeTokenRunBuilder::buildRuns(
							source, options.maximum_records, options.maximum_sort_run_bytes, candidateRange.count,
								[&visit](NativeBulkRunFragment &&fragment) { visit(std::move(fragment)); });
				},
				tokenMerge, reader, writer, fence);
	}

	NativeBulkEdgePrivateAdjacencyRoots NativeBulkEdgePrivateFamilyMaterializer::materializeAdjacency(
			const NativeBulkEdgeRecordSource &source, std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
			const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange outgoingRange,
			NativeBulkEdgePrivateCandidateRange incomingRange,
			const NativeBulkEdgePrivateFamilyMaterializationOptions &options,
			const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
			const NativeGraphPrivateSegmentFence &fence) {
		validate(source, options, reader, writer, fence);
		if (edgeIdRanges.empty() || outgoingRange.count == 0U || incomingRange.count == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native bulk Edge private adjacency family has invalid leases or candidate ranges");
		}
		NativeBulkEdgePrivateRunPipelineOptions adjacencyMerge = options.merge;
		adjacencyMerge.maximum_entries = options.maximum_records;
		adjacencyMerge.duplicate_policy = NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT;
		const auto roots = NativeBulkEdgePrivateRunPipeline::materializeInitialFragmentPair(
				identities, outgoingRange, incomingRange,
				[&](const NativeBulkEdgePrivateRunFragmentVisitor &outgoing,
						const NativeBulkEdgePrivateRunFragmentVisitor &incoming) {
					debug::ScopedPerfTimer timer("native_graph.streamed.private_prepare.adjacency_initial_runs");
					NativeBulkEdgeAdjacencyRunBuilder::buildRunsPair(
							source, edgeIdRanges, options.maximum_records, options.maximum_sort_run_bytes, outgoingRange.count,
							incomingRange.count,
							[&outgoing](NativeBulkRunFragment &&fragment) { outgoing(std::move(fragment)); },
							[&incoming](NativeBulkRunFragment &&fragment) { incoming(std::move(fragment)); });
				},
				adjacencyMerge, adjacencyMerge, reader, writer, fence);
		return {roots.first, roots.second};
	}

} // namespace graph::storage
