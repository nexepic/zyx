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
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/metadata/NativeGraphDeferredIndexImages.hpp"
#include "graph/debug/PerfTrace.hpp"

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
        const auto &sourceSuperblock = planning.current_snapshot.superblock();
        const auto sourceDataRoot = sourceSuperblock.usesPageDirectoryBootstrap() ?
            metadata.pages_.readPageDirectoryBootstrap(sourceSuperblock.page_directory_root).data_page_directory_root :
            sourceSuperblock.page_directory_root;
        const auto memory = utils::OwnedAllocationScope::owner();
        const auto receipt = [&] {
            return std::shared_ptr<NativeGraphDeferredIndexImages>(
                new (memory) NativeGraphDeferredIndexImages(metadata, planning.current_snapshot.superblock().generation, RSS_GLOBAL_ADJACENCY_INDEX_ID,
                    existingRoot, memory, options.first_slot, options.last_slot, planning.private_segments, planning.private_segment_reader, sourceDataRoot),
                std::default_delete<NativeGraphDeferredIndexImages>(), utils::OwnedPmrAllocator<std::byte>(memory));
        };
        const auto attachFreshShape = [&](NativeGraphDeferredIndexPlan &result, const BTreeInitialTreeMutationPlan &fresh) {
            if (!memory) return;
            auto proof = receipt(); proof->entry_count_ = fresh.entry_count;
            for (const auto &pool : fresh.new_pool_mutations) proof->page_count_ += pool.appended_page_count;
            proof->fresh_mutation_ = std::move(result.mutation); proof->fresh_preparer_ = std::move(result.preparer);
            result.mutation = {}; result.mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
            result.images = std::move(proof);
        };
        const auto hint = [&] {
            std::lock_guard lock(metadata.streamed_adjacency_shape_mutex_);
            return metadata.streamed_adjacency_shape_;
        }();
        constexpr size_t threshold = NativeGraphWritePlannerOptions::DEFAULT_GLOBAL_ADJACENCY_BULK_REBUILD_ENTRY_THRESHOLD;
        if (memory && existingRoot && hint.data_root == sourceDataRoot && hint.root == existingRoot &&
            hint.pages && hint.pages <= options.maximum_pages && hint.entries <= options.maximum_entries) {
            // Probe only the bounded new run. Unknown/stale shape never causes
            // an index census on the incremental path.
            utils::OwnedHeapReservation cursorCredit(memory, 65536U);
            size_t count = 0, added = 0; bool insertionsOnly = true;
            BTreeIndex existing(RSS_GLOBAL_ADJACENCY_INDEX_ID, pages, metadata, BTreeLeafValueLayout::BLVL_EMPTY);
            auto cursor = operations();
            while (auto next = cursor()) {
                if (++count == threshold) break;
                if (next->value_or_child != 1U) { insertionsOnly = false; break; }
                if (!existing.find(planning.current_snapshot, next->key)) ++added;
            }
            if (count && count < threshold && insertionsOnly) {
                if (added > options.maximum_entries - hint.entries) {
                    debug::PerfTrace::addValue("native_graph.write.streamed_adjacency.bounds_refused", 1);
                    fail(FormatError::FERR_INVALID_STATE, "streamed adjacency delta exceeds its final entry bound");
                }
                auto proof = receipt(); proof->incremental_ = true;
                const auto source = [&](const std::function<void(const BTreeScalarIndexOperation &)> &emit) {
                    auto input = operations();
                    while (auto next = input()) {
                        if (next->value_or_child != 1U)
                            fail(FormatError::FERR_INVALID_STATE, "streamed adjacency delta changed its insertion source");
                        emit({BTreeScalarIndexOperationKind::BSIOP_UPSERT, std::move(next->key), 0});
                    }
                };
                proof->plan_ = pages.planGlobalAdjacencyImagesFromSource(planning.current_snapshot, count, source, memory,
                    [target = proof.get()](BTreePage page) { target->images_.push_back(std::move(page)); });
                size_t appended = 0, erased = 0;
                for (const auto &request : proof->plan_.pool_requests) {
                    appended += request.appended_page_count; erased += request.expected_retirements.size();
                }
                if (proof->plan_.new_pool_mutation) appended += proof->plan_.new_pool_mutation->appended_page_count;
                for (const auto &pool : proof->plan_.additional_new_pool_mutations) appended += pool.appended_page_count;
                if (erased > hint.pages || appended > options.maximum_pages - (hint.pages - erased))
                    fail(FormatError::FERR_INVALID_STATE, "streamed adjacency delta exceeds its final page bound");
                proof->entry_count_ = hint.entries + added; proof->page_count_ = hint.pages - erased + appended;
                proof->assignments_credit_.set(16384U +
                    (proof->plan_.pool_requests.size() + proof->plan_.additional_new_pool_mutations.size() +
                        size_t(bool(proof->plan_.new_pool_mutation))) * 8192U);
                NativeGraphDeferredIndexPlan result; result.mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
                result.images = std::move(proof);
                debug::PerfTrace::addValue("native_graph.write.streamed_adjacency.incremental_plans", 1);
                debug::PerfTrace::addValue("native_graph.write.streamed_adjacency.incremental_keys", int64_t(count));
                return result;
            }
        }
        debug::PerfTrace::addValue("native_graph.write.streamed_adjacency.full_plans", 1);

		if (!existingRoot.has_value()) {
            const auto fresh = BulkBTreeBuilder::planNativeTree(pages, metadata, planning.current_snapshot,
                RSS_GLOBAL_ADJACENCY_INDEX_ID, insertionEntries(operations), build);
            auto result = makeFreshPlan(fresh, options, checkpointLsn); attachFreshShape(result, fresh); return result;
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
        attachFreshShape(result, replacement);
		return result;
	}

} // namespace graph::storage
