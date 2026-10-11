/**
 * @file NativeStreamedGraphPrivatePreparation.cpp
 * @brief Writer-scoped private streamed Edge preparation implementation.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphPrivatePreparation.hpp"

#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/index/NativeBulkRun.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] NativeBulkRunPayloadReader payloadReader(const NativeGraphPrivateSegmentSet &identities,
															   const NativeGraphPrivateSegmentReader &reader) {
			return [&identities, &reader](const NativeBulkRunFragmentLocation &location) {
				return reader(identities[identities.ordinalOf(
						{location.segment_id, location.generation, location.transaction_id})]);
			};
		}

		void validate(const NativeBulkEdgeRecordSource &source,
					  std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
					  const NativeStreamedGraphCandidateLayout &layout, const NativeGraphPrivateSegmentSet &identities,
					  const MetadataSnapshotPin &snapshot, const NativeStreamedGraphPrivatePreparationOptions &options,
					  const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
					  const NativeGraphPrivateSegmentFence &fence) {
			if (!source || edgeIdRanges.empty() || !snapshot.isValid() || options.maximum_catalog_entries == 0U ||
				!reader || !writer || !fence || layout.candidate_count != identities.size() ||
				layout.outgoing_adjacency.count == 0U || layout.incoming_adjacency.count == 0U ||
				options.families.maximum_token_entries == 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native streamed Edge private preparation requires one complete writer-scoped candidate layout");
			}
		}

	} // namespace

	void NativeStreamedGraphPrivatePreparationCoordinator::prepare(
			const NativeBulkEdgeRecordSource &source, std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
			const NativeStreamedGraphCandidateLayout &layout, const NativeGraphPrivateSegmentSet &identities,
			SystemCatalog &catalog, const MetadataSnapshotPin &snapshot,
			const NativeStreamedGraphPrivatePreparationOptions &options, const NativeGraphPrivateSegmentReader &reader,
			const NativeGraphPrivateSegmentWriter &writer, const NativeGraphPrivateSegmentFence &fence,
			const NativeStreamedGraphPrivatePreparationVisitor &visit, uint64_t edgeCountIncrement) {
		validate(source, edgeIdRanges, layout, identities, snapshot, options, reader, writer, fence);
		if (!visit) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge private preparation requires a scoped result consumer");
		}
		NativeStreamedGraphPrivatePreparation result;
		{
			debug::ScopedPerfTimer timer("native_graph.streamed.private_prepare.tokens");
			result.token_run = NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
					source, identities, {layout.tokens.first_ordinal, layout.tokens.count}, options.families, reader,
					writer, fence);
		}
		const OrderedBTreeEntrySource tokenEntries =
				result.token_run.has_value()
						? NativeBulkRunStore::orderedSourceFromPayloadReader(
								  *result.token_run, options.families.merge.maximum_manifest_depth,
								  payloadReader(identities, reader))
						: [] { return []() -> std::optional<BTreePageEntry> { return std::nullopt; }; };
		{
			debug::ScopedPerfTimer timer("native_graph.streamed.private_prepare.catalog_tokens");
			result.catalog_tokens = NativeBulkEdgeCatalogTokenMaterializer::prepare(
					catalog, snapshot, tokenEntries,
					{options.families.maximum_token_entries, options.maximum_catalog_entries}, edgeCountIncrement);
		}
		if (result.token_run.has_value()) {
			if (layout.token_resolver.count == 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge private preparation omitted Token resolver candidates");
			}
			{
				debug::ScopedPerfTimer timer("native_graph.streamed.private_prepare.token_resolver");
				result.token_resolver = NativeBulkEdgePrivateTokenResolver::materialize(
						identities, {layout.token_resolver.first_ordinal, layout.token_resolver.count},
						result.catalog_tokens.resolutions, options.families.maximum_token_entries, reader, writer,
						fence);
			}
		} else if (layout.token_resolver.count != 0U) {
			fail(FormatError::FERR_INVALID_STATE, "native streamed Edge private preparation reserved Token resolver "
												  "candidates for an empty token stream");
		}
		{
			debug::ScopedPerfTimer timer("native_graph.streamed.private_prepare.adjacency");
			result.adjacency_roots = NativeBulkEdgePrivateFamilyMaterializer::materializeAdjacency(
					source, edgeIdRanges, identities,
					{layout.outgoing_adjacency.first_ordinal, layout.outgoing_adjacency.count},
					{layout.incoming_adjacency.first_ordinal, layout.incoming_adjacency.count}, options.families,
					reader, writer, fence);
		}
		visit(result);
	}

	void NativeStreamedGraphPrivatePreparationCoordinator::prepareDeferredPublication(
			const NativeBulkEdgeRecordSource &source, std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
			const NativeStreamedGraphPublicationPlan &publicationPlan, const NativeStreamedGraphCandidateLayout &layout,
			const NativeGraphPrivateSegmentSet &identities, SystemCatalog &catalog, const MetadataSnapshotPin &snapshot,
			const NativeStreamedGraphPrivatePreparationOptions &options, const NativeGraphPrivateSegmentReader &reader,
			const NativeGraphPrivateSegmentWriter &writer, const NativeGraphPrivateSegmentFence &fence,
			const NativeStreamedGraphPreparedPublicationVisitor &visit) {
		if (!visit || publicationPlan.record_count == 0U || publicationPlan.edge_entity_page_count == 0U ||
			layout.candidate_count != identities.size()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge deferred publication requires one complete scoped callback");
		}
		debug::ScopedPerfTimer timer("native_graph.streamed.private_prepare");
		prepare(
				source, edgeIdRanges, layout, identities, catalog, snapshot, options, reader, writer, fence,
				[&source, edgeIdRanges, &publicationPlan, &layout, &catalog, &snapshot, &options, &identities, &reader,
				 &writer, &fence, &visit](const NativeStreamedGraphPrivatePreparation &prepared) {
					const NativeStreamedGraphDeferredPublication publication =
							NativeStreamedGraphDeferredPublication::create(
									source, edgeIdRanges, publicationPlan,
									prepared.token_resolver.materializerResolvers());
					NativeStreamedGraphPrivatePreparation complete = prepared;
					NativeStreamedGraphLargeValuePlan valuePlan;
					if (publicationPlan.large_value_count == 0U) {
						if (layout.large_value_content.count != 0U || layout.large_value_reference_map.count != 0U) {
							fail(FormatError::FERR_INVALID_STATE, "native streamed Edge private preparation reserved "
																  "LargeValue candidates for no overflow roots");
						}
						if (publicationPlan.property_root_count != 0U) {
							debug::ScopedPerfTimer propertyTimer(
									"native_graph.streamed.private_prepare.property_measure");
							valuePlan.property_pages =
									NativeStreamedGraphPropertyPageMaterializer::measureInlinePages(publication);
						}
					} else {
						if (options.large_value_store == nullptr ||
							!options.large_value_store->usesMetadataStore(catalog.metadataStore()) ||
							layout.large_value_content.count == 0U || layout.large_value_reference_map.count == 0U ||
							!snapshot.superblock().next_large_value_id) {
							fail(FormatError::FERR_INVALID_STATE, "native streamed Edge private preparation has no "
																  "writer-scoped LargeValue content path");
						}
						const LargeValueId firstValueId(snapshot.superblock().next_large_value_id);
						{
							debug::ScopedPerfTimer valueTimer(
									"native_graph.streamed.private_prepare.large_value_measure");
							valuePlan = NativeStreamedGraphLargeValuePlanner::measure(
									publication, *options.large_value_store, options.large_value_options);
						}
						if (valuePlan.reference_count != publicationPlan.large_value_count ||
							valuePlan.value_count != valuePlan.reference_count) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native streamed Edge Property overflow preflight diverged from its tokenized source");
						}
						const NativeStreamedGraphLargeValueContentRunPlan contentPlan =
								NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
								*options.large_value_store, valuePlan.reference_count,
										options.large_value_content_options.maximum_run_bytes,
										options.large_value_content_options.maximum_open_runs);
						if (contentPlan.candidate_segment_count != layout.large_value_content.count) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native streamed Edge LargeValue content candidate layout is not exact");
						}
						NativeBulkRunFragmentLocation contentRun;
						{
							debug::ScopedPerfTimer contentTimer(
									"native_graph.streamed.private_prepare.large_value_content");
							contentRun = NativeStreamedGraphLargeValuePrivateMaterializer::materialize(
									publication, *options.large_value_store, firstValueId, valuePlan,
									options.large_value_options, contentPlan, identities,
									{layout.large_value_content.first_ordinal, layout.large_value_content.count},
									options.large_value_content_options, reader, writer, fence);
						}
						const NativeStreamedGraphLargeValueReferenceMapRunPlan referenceMapPlan =
								NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(
										valuePlan.reference_count, options.large_value_content_options.maximum_run_bytes,
										options.large_value_content_options.maximum_open_runs);
						if (referenceMapPlan.candidate_segment_count != layout.large_value_reference_map.count) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native streamed Edge LargeValue reference-map candidate layout is not exact");
						}
						const OrderedBTreeEntrySource contentEntries = NativeBulkRunStore::orderedSourceFromPayloadReader(
								contentRun, options.large_value_content_options.maximum_manifest_depth,
								payloadReader(identities, reader));
						NativeBulkRunFragmentLocation referenceMapRun;
						{
							debug::ScopedPerfTimer referenceMapTimer(
									"native_graph.streamed.private_prepare.large_value_reference_map");
							referenceMapRun = NativeStreamedGraphLargeValuePrivateMaterializer::materializeReferenceMap(
										contentEntries, *options.large_value_store, firstValueId, valuePlan.reference_count,
										referenceMapPlan, identities,
										{layout.large_value_reference_map.first_ordinal, layout.large_value_reference_map.count},
										options.large_value_content_options, reader, writer, fence);
						}
						const OrderedBTreeEntrySource canonicalReferences = NativeBulkRunStore::orderedSourceFromPayloadReader(
								referenceMapRun, options.large_value_content_options.maximum_manifest_depth,
								payloadReader(identities, reader));
						valuePlan = NativeStreamedGraphLargeValuePlanner::deriveCanonicalPlan(
								publication, *options.large_value_store, firstValueId, valuePlan,
								options.large_value_options, canonicalReferences);
						complete.large_values = {firstValueId, valuePlan, contentPlan, contentRun, referenceMapPlan,
													 referenceMapRun};
					}
					complete.publication_shape =
							NativeStreamedGraphPublicationShape::derive(publicationPlan, valuePlan);
					visit(complete, publication);
				},
				publicationPlan.record_count);
	}

} // namespace graph::storage
