/**
 * @file NativeStreamedGraphLargeValuePrivateMaterializer.cpp
 * @brief Writer-scoped private materialization of streamed LargeValue index runs.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValuePrivateMaterializer.hpp"

#include <utility>

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

	} // namespace

	NativeBulkRunFragmentLocation NativeStreamedGraphLargeValuePrivateMaterializer::materialize(
			const NativeStreamedGraphDeferredPublication &publication, const LargeValueStore &store,
			LargeValueId firstValueId, const NativeStreamedGraphLargeValuePlan &valuePlan,
			const NativeStreamedGraphLargeValuePlanningOptions &valueOptions,
			const NativeStreamedGraphLargeValueContentRunPlan &contentPlan,
			const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange candidateRange,
			const NativeStreamedGraphLargeValuePrivateMaterializationOptions &options,
			const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
			const NativeGraphPrivateSegmentFence &fence) {
		if (!firstValueId.isValid() || valuePlan.value_count == 0U || contentPlan.initial_fragment_count == 0U ||
			contentPlan.candidate_segment_count == 0U || candidateRange.count != contentPlan.candidate_segment_count ||
			options.maximum_run_bytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES || options.maximum_open_runs < 2U ||
			options.maximum_entries < valuePlan.value_count || options.maximum_manifest_depth == 0U || !reader ||
			!writer || !fence) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge LargeValue private materialization requires its exact bounded candidate plan");
		}
		const NativeBulkEdgePrivateRunPipelineOptions pipelineOptions{
				options.maximum_open_runs, options.maximum_run_bytes, options.maximum_entries,
				options.maximum_manifest_depth, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT};
		return NativeBulkEdgePrivateRunPipeline::materializeInitialFragments(
				identities, candidateRange,
				[&](const NativeBulkEdgePrivateRunFragmentVisitor &visit) {
					NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
							publication, store, firstValueId, valuePlan, valueOptions, options.maximum_run_bytes,
							contentPlan.initial_fragment_count,
							[&visit](NativeBulkRunFragment &&fragment) { visit(std::move(fragment)); });
				},
				pipelineOptions, reader, writer, fence);
	}

	NativeBulkRunFragmentLocation NativeStreamedGraphLargeValuePrivateMaterializer::materializeReferenceMap(
			const OrderedBTreeEntrySource &contentEntries, const LargeValueStore &store, LargeValueId firstValueId,
			size_t referenceCount, const NativeStreamedGraphLargeValueReferenceMapRunPlan &mapPlan,
			const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange candidateRange,
			const NativeStreamedGraphLargeValuePrivateMaterializationOptions &options,
			const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
			const NativeGraphPrivateSegmentFence &fence) {
		if (!contentEntries || !firstValueId.isValid() || referenceCount == 0U ||
			mapPlan.initial_fragment_count == 0U || mapPlan.candidate_segment_count == 0U ||
			candidateRange.count != mapPlan.candidate_segment_count ||
			options.maximum_run_bytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES || options.maximum_open_runs < 2U ||
			options.maximum_entries < referenceCount || options.maximum_manifest_depth == 0U || !reader || !writer ||
			!fence) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge LargeValue reference-map materialization requires its exact bounded candidate plan");
		}
		const NativeBulkEdgePrivateRunPipelineOptions pipelineOptions{
				options.maximum_open_runs, options.maximum_run_bytes, options.maximum_entries,
				options.maximum_manifest_depth, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT};
		return NativeBulkEdgePrivateRunPipeline::materializeInitialFragments(
				identities, candidateRange,
				[&](const NativeBulkEdgePrivateRunFragmentVisitor &visit) {
					NativeStreamedGraphLargeValueReferenceMapBuilder::buildRuns(
							contentEntries, store, firstValueId, referenceCount, options.maximum_run_bytes,
							mapPlan.initial_fragment_count,
							[&visit](NativeBulkRunFragment &&fragment) { visit(std::move(fragment)); });
				},
				pipelineOptions, reader, writer, fence);
	}

} // namespace graph::storage
