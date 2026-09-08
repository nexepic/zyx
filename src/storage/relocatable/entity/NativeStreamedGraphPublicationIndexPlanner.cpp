/**
 * @file NativeStreamedGraphPublicationIndexPlanner.cpp
 * @brief One ordered deferred-index bundle for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationIndexPlanner.hpp"

#include <algorithm>
#include <utility>

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

	} // namespace

	NativeGraphDeferredIndexPlans NativeStreamedGraphPublicationIndexPlanner::plan(
			SystemCatalog &catalog, BTreePageStore &adjacencyPages, LargeValueStore *largeValueStore,
			RelocatableMetadataStore &metadata, const NativeStreamedGraphPrivatePreparation &prepared,
			const NativeStreamedGraphPublicationIndexPlanningOptions &options,
			const NativeGraphDeferredIndexPlanningContext &planning, LogSequenceNumber checkpointLsn) {
		if (!catalog.usesMetadataStore(metadata) || !adjacencyPages.usesMetadataStore(metadata) ||
			!planning.current_snapshot.isValid() || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge index bundle requires one writer-fenced metadata domain");
		}

		NativeGraphDeferredIndexPlans result;
		if (std::optional<NativeGraphDeferredIndexPlan> catalogPlan = NativeStreamedGraphCatalogIndexPlanner::plan(
					catalog, metadata, prepared.catalog_tokens.final_catalog_entries,
					prepared.catalog_tokens.catalog_changed, options.catalog, planning.current_snapshot, checkpointLsn);
			catalogPlan.has_value()) {
			result.plans.push_back(std::move(*catalogPlan));
		}
		result.plans.push_back(NativeStreamedGraphAdjacencyIndexPlanner::plan(
				adjacencyPages, metadata, prepared.adjacency_roots, options.adjacency, planning, checkpointLsn));
		if (prepared.large_values.has_value()) {
			if (largeValueStore == nullptr || !largeValueStore->usesMetadataStore(metadata)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge index bundle has LargeValue content without its native store");
			}
			result.plans.push_back(NativeStreamedGraphLargeValueContentIndexPlanner::plan(
					*largeValueStore, metadata, prepared.large_values->content_run, options.large_value_content,
					planning, checkpointLsn));
		} else if (largeValueStore != nullptr && !largeValueStore->usesMetadataStore(metadata)) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge index bundle has a foreign optional LargeValue store");
		}

		std::ranges::sort(result.plans, {},
						  [](const NativeGraphDeferredIndexPlan &plan) { return plan.mutation.index_id; });
		// Catalog, global-adjacency, and LargeValue planners each own a distinct fixed Index ID.
		return result;
	}

} // namespace graph::storage
