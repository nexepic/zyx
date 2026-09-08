/**
 * @file NativeStreamedGraphAdjacencyIndexPlanner.cpp
 * @brief Deferred global adjacency-tree planning for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphAdjacencyIndexPlanner.hpp"

#include <array>
#include <utility>

#include "graph/storage/relocatable/adjacency/GlobalAdjacencyMutationMerger.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/index/NativeBulkRun.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] NativeBulkRunPayloadReader
		privateReader(const NativeGraphDeferredIndexPlanningContext &planning) {
			// The deferred tree preparer performs a second source pass after the
			// planner callback returns, so it must own the scoped capabilities rather
			// than retain the callback's stack wrapper.
			return [privateSegments = planning.private_segments,
					privateSegmentReader =
							planning.private_segment_reader](const NativeBulkRunFragmentLocation &location) {
				const NativeGraphPrivateSegmentMaterialization identity{location.segment_id, location.generation,
																		location.transaction_id};
				try {
					static_cast<void>(privateSegments.ordinalOf(identity));
				} catch (const FormatException &) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed Edge adjacency planner received a foreign private run");
				}
				return privateSegmentReader(identity);
			};
		}

		[[nodiscard]] OrderedBTreeEntrySource insertionEntries(const OrderedBTreeEntrySource &operations) {
			// The two native run sources are merged into a rewindable operation source.
			return [operations] {
				OrderedBTreeEntryCursor cursor = operations();
				// Native run readers reject malformed payloads by exception, not by a null cursor.
				return [cursor = std::move(cursor)]() mutable -> std::optional<BTreePageEntry> {
					std::optional<BTreePageEntry> operation = cursor();
					if (!operation.has_value()) {
						return std::nullopt;
					}
					if (operation->value_or_child != 1U) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native streamed Edge adjacency source cannot erase from an absent tree");
					}
					return BTreePageEntry{std::move(operation->key), 0U};
				};
			};
		}

		[[nodiscard]] NativeGraphDeferredIndexPlan
		makeFreshPlan(const BTreeInitialTreeMutationPlan &plan,
					  const NativeStreamedGraphAdjacencyIndexPlanningOptions &options,
					  LogSequenceNumber checkpointLsn) {
			// BulkBTreeBuilder returns a complete plan after validating its fresh root.
			NativeGraphDeferredIndexPlan result;
			result.mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
			for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
				result.mutation.new_pool_requests.push_back(
						{options.first_slot, options.last_slot, pool.owner_id, pool.appended_page_count});
			}
			result.mutation.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal,
												true};
			result.preparer = [preparer = plan.preparer,
							   checkpointLsn](std::vector<IndexTreeMutationPoolBatch> &batches) {
				preparer(batches, checkpointLsn);
			};
			return result;
		}

	} // namespace

	NativeGraphDeferredIndexPlan
	NativeStreamedGraphAdjacencyIndexPlanner::plan(BTreePageStore &pages, RelocatableMetadataStore &metadata,
												   const NativeBulkEdgePrivateAdjacencyRoots &privateRoots,
												   const NativeStreamedGraphAdjacencyIndexPlanningOptions &options,
												   const NativeGraphDeferredIndexPlanningContext &planning,
												   LogSequenceNumber checkpointLsn) {
		if (!pages.usesMetadataStore(metadata) || !privateRoots.outgoing.isValid() ||
			!privateRoots.incoming.isValid() || !planning.current_snapshot.isValid() ||
			!planning.private_segment_reader || !options.first_slot.isValid() || !options.last_slot.isValid() ||
			options.first_slot > options.last_slot || options.maximum_entries == 0U || options.maximum_pages == 0U ||
			options.maximum_manifest_depth == 0U || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge adjacency planner requires bounded writer-fenced private runs");
		}
		const NativeBulkRunPayloadReader reader = privateReader(planning);
		const std::array<OrderedBTreeEntrySource, 2U> directionSources = {
				NativeBulkRunStore::orderedSourceFromPayloadReader(privateRoots.outgoing,
																   options.maximum_manifest_depth, reader),
				NativeBulkRunStore::orderedSourceFromPayloadReader(privateRoots.incoming,
																   options.maximum_manifest_depth, reader)};
		const OrderedBTreeEntrySource operations =
				NativeBulkRunMerger::orderedSource(directionSources, directionSources.size(), options.maximum_entries);

		BulkBTreeBuildOptions build;
		build.first_slot = options.first_slot;
		build.last_slot = options.last_slot;
		build.pool_owner_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
		build.maximum_entries = options.maximum_entries;
		build.maximum_pages = options.maximum_pages;
		build.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
		const std::optional<IndexRootLocation> existingRoot =
				metadata.findIndexRootOptional(planning.current_snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		if (!existingRoot.has_value()) {
			return makeFreshPlan(BulkBTreeBuilder::planNativeTree(pages, metadata, planning.current_snapshot,
																  RSS_GLOBAL_ADJACENCY_INDEX_ID,
																  insertionEntries(operations), build),
								 options, checkpointLsn);
		}

		BTreeIndex existing(RSS_GLOBAL_ADJACENCY_INDEX_ID, pages, metadata, BTreeLeafValueLayout::BLVL_EMPTY);
		const MetadataSnapshotPin *snapshot = &planning.current_snapshot;
		const OrderedBTreeEntrySource oldEntries = [existing = std::move(existing), snapshot,
													maximumEntries = options.maximum_entries]() mutable {
			return existing.orderedSource(*snapshot, maximumEntries)();
		};
		const OrderedBTreeEntrySource merged =
				GlobalAdjacencyMutationMerger::merge(oldEntries, operations, options.maximum_entries);
		const BTreeInitialTreeMutationPlan replacement = BulkBTreeBuilder::planNativeReplacementTree(
				pages, metadata, planning.current_snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID, merged, build);
		const BTreeTreeRetirementPlan retirement = pages.planTreeRetirement(
				planning.current_snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID, options.maximum_pages);
		// Existing trees are nonempty and streamed Edge runs contain only UPSERTs.
		NativeGraphDeferredIndexPlan result = makeFreshPlan(replacement, options, checkpointLsn);
		result.mutation.expected_root = retirement.expected_root;
		result.mutation.pool_requests = retirement.pool_requests;
		const size_t retirementPoolCount = retirement.pool_requests.size();
		const IndexTreeMultiPoolMutationPreparer replacementPreparer = std::move(result.preparer);
		result.preparer = [retirementPreparer = retirement.preparer, replacementPreparer, retirementPoolCount,
						   checkpointLsn](std::vector<IndexTreeMutationPoolBatch> &batches) {
			if (batches.size() < retirementPoolCount) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge adjacency replacement changed its pool partition");
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
