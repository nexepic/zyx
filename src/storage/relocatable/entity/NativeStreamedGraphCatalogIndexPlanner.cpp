/**
 * @file NativeStreamedGraphCatalogIndexPlanner.cpp
 * @brief Deferred System Catalog tree planning for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphCatalogIndexPlanner.hpp"

#include <utility>

#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] NativeGraphDeferredIndexPlan makeFreshPlan(
				const BTreeInitialTreeMutationPlan &plan,
				const NativeStreamedGraphCatalogIndexPlanningOptions &options, LogSequenceNumber checkpointLsn) {
			// BulkBTreeBuilder returns a complete plan after validating its fresh root.
			NativeGraphDeferredIndexPlan result;
			result.mutation.index_id = RSS_SYSTEM_CATALOG_INDEX_ID;
			for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
				result.mutation.new_pool_requests.push_back(
						{options.first_slot, options.last_slot, pool.owner_id, pool.appended_page_count});
			}
			result.mutation.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal, true};
			result.preparer = [preparer = plan.preparer, checkpointLsn](std::vector<IndexTreeMutationPoolBatch> &batches) {
				preparer(batches, checkpointLsn);
			};
			return result;
		}

	} // namespace

	std::optional<NativeGraphDeferredIndexPlan> NativeStreamedGraphCatalogIndexPlanner::plan(
			SystemCatalog &catalog, RelocatableMetadataStore &metadata, OrderedBTreeEntrySource finalEntries,
			bool catalogChanged, const NativeStreamedGraphCatalogIndexPlanningOptions &options,
			const MetadataSnapshotPin &snapshot, LogSequenceNumber checkpointLsn) {
		if (!catalog.usesMetadataStore(metadata) || !snapshot.isValid() || !options.first_slot.isValid() ||
			!options.last_slot.isValid() || options.first_slot > options.last_slot || options.maximum_entries == 0U ||
			options.maximum_pages == 0U || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge Catalog planner requires a bounded writer-fenced source");
		}
		if (!catalogChanged) {
			return std::nullopt;
		}
		if (!finalEntries) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge Catalog planner has no final ordered Catalog source");
		}

		BulkBTreeBuildOptions build;
		build.first_slot = options.first_slot;
		build.last_slot = options.last_slot;
		build.pool_owner_id = RSS_SYSTEM_CATALOG_INDEX_ID;
		build.maximum_entries = options.maximum_entries;
		build.maximum_pages = options.maximum_pages;
		build.leaf_value_layout = BTreeLeafValueLayout::BLVL_U64;

		const std::optional<IndexRootLocation> existingRoot =
				metadata.findIndexRootOptional(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID);
		if (!existingRoot.has_value()) {
			return makeFreshPlan(BulkBTreeBuilder::planNativeTree(catalog.pageStore(), metadata, snapshot,
																			 RSS_SYSTEM_CATALOG_INDEX_ID, finalEntries, build),
							 options, checkpointLsn);
		}

		const BTreeInitialTreeMutationPlan replacement = BulkBTreeBuilder::planNativeReplacementTree(
				catalog.pageStore(), metadata, snapshot, RSS_SYSTEM_CATALOG_INDEX_ID, finalEntries, build);
		const BTreeTreeRetirementPlan retirement = catalog.pageStore().planTreeRetirement(
				snapshot, RSS_SYSTEM_CATALOG_INDEX_ID, options.maximum_pages);
		if (replacement.entry_count == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge Catalog planner produced an empty replacement tree");
		}
		NativeGraphDeferredIndexPlan result = makeFreshPlan(replacement, options, checkpointLsn);
		result.mutation.expected_root = retirement.expected_root;
		result.mutation.pool_requests = retirement.pool_requests;
		const size_t retirementPoolCount = retirement.pool_requests.size();
		const IndexTreeMultiPoolMutationPreparer replacementPreparer = std::move(result.preparer);
		result.preparer = [retirementPreparer = retirement.preparer, replacementPreparer, retirementPoolCount,
						   checkpointLsn](std::vector<IndexTreeMutationPoolBatch> &batches) {
			if (batches.size() < retirementPoolCount) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge Catalog replacement changed its pool partition");
			}
			std::vector<IndexTreeMutationPoolBatch> retired(
					batches.begin(), batches.begin() + static_cast<ptrdiff_t>(retirementPoolCount));
			std::vector<IndexTreeMutationPoolBatch> fresh(batches.begin() + static_cast<ptrdiff_t>(retirementPoolCount),
																				batches.end());
			retirementPreparer(retired, checkpointLsn);
			replacementPreparer(fresh);
			std::move(retired.begin(), retired.end(), batches.begin());
			std::move(fresh.begin(), fresh.end(), batches.begin() + static_cast<ptrdiff_t>(retirementPoolCount));
		};
		return result;
	}

} // namespace graph::storage
