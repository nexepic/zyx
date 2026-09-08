/**
 * @file NativeStreamedGraphLargeValueContentIndexPlanner.cpp
 * @brief Deferred B+Tree planning for streamed LargeValue content identities.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueContentIndexPlanner.hpp"

#include <array>
#include <memory>
#include <utility>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] NativeBulkRunPayloadReader
		privateReader(const NativeGraphDeferredIndexPlanningContext &planning) {
			// BulkBTreeBuilder retains its source for the second materialization pass.
			// Keep the writer-scoped capabilities by value so that pass never retains
			// the temporary planning-context wrapper itself.
			return [privateSegments = planning.private_segments,
					privateSegmentReader =
							planning.private_segment_reader](const NativeBulkRunFragmentLocation &location) {
				const NativeGraphPrivateSegmentMaterialization identity{location.segment_id, location.generation,
																		location.transaction_id};
				try {
					static_cast<void>(privateSegments.ordinalOf(identity));
				} catch (const FormatException &) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed Edge LargeValue content-index planner received a foreign private run");
				}
				return privateSegmentReader(identity);
			};
		}

		[[nodiscard]] NativeGraphDeferredIndexPlan
		makeFreshPlan(const BTreeInitialTreeMutationPlan &plan,
					  const NativeStreamedGraphLargeValueContentIndexPlanningOptions &options,
					  LogSequenceNumber checkpointLsn) {
			NativeGraphDeferredIndexPlan result;
			result.mutation.index_id = RSS_LARGE_VALUE_CONTENT_INDEX_ID;
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

		[[nodiscard]] OrderedBTreeEntrySource canonicalContentEntries(OrderedBTreeEntrySource entries,
																 size_t keyBytes) {
			const size_t identityBytes = keyBytes - sizeof(uint64_t);
			return [entries = std::move(entries), keyBytes, identityBytes] {
				OrderedBTreeEntryCursor cursor = entries();
				auto previousIdentity = std::make_shared<std::vector<std::byte>>();
				return [cursor = std::move(cursor), previousIdentity, keyBytes, identityBytes]() mutable
						   -> std::optional<BTreePageEntry> {
					for (std::optional<BTreePageEntry> entry = cursor(); entry.has_value(); entry = cursor()) {
						if (entry->key.size() != keyBytes || entry->value_or_child == 0U) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native streamed Edge LargeValue content run has an invalid entry shape");
						}
						const std::vector<std::byte> identity(entry->key.begin(),
																	entry->key.begin() + static_cast<ptrdiff_t>(identityBytes));
						if (*previousIdentity == identity) {
							continue;
						}
						*previousIdentity = identity;
						return entry;
					}
					return std::nullopt;
				};
			};
		}

	} // namespace

	NativeGraphDeferredIndexPlan NativeStreamedGraphLargeValueContentIndexPlanner::plan(
			LargeValueStore &store, RelocatableMetadataStore &metadata, NativeBulkRunFragmentLocation privateRunRoot,
			const NativeStreamedGraphLargeValueContentIndexPlanningOptions &options,
			const NativeGraphDeferredIndexPlanningContext &planning, LogSequenceNumber checkpointLsn) {
		if (!store.usesMetadataStore(metadata) || !privateRunRoot.isValid() || !planning.current_snapshot.isValid() ||
			!options.first_slot.isValid() || !options.last_slot.isValid() || options.first_slot > options.last_slot ||
			options.maximum_entries == 0U || options.maximum_pages == 0U || options.maximum_manifest_depth == 0U ||
			!planning.private_segment_reader || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge LargeValue content-index planner requires a fenced private source and bounds");
		}
		LargeValueRef representative;
		representative.value_id = LargeValueId(1U);
		representative.kind = LargeValueKind::LVK_BYTES;
		representative.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
		const OrderedBTreeEntrySource additions = canonicalContentEntries(
				NativeBulkRunStore::orderedSourceFromPayloadReader(
						privateRunRoot, options.maximum_manifest_depth, privateReader(planning)),
				store.contentIndexKey(representative).size());
		BulkBTreeBuildOptions build;
		build.first_slot = options.first_slot;
		build.last_slot = options.last_slot;
		build.pool_owner_id = RSS_LARGE_VALUE_CONTENT_INDEX_ID;
		build.maximum_entries = options.maximum_entries;
		build.maximum_pages = options.maximum_pages;
		build.leaf_value_layout = BTreeLeafValueLayout::BLVL_U64;
		const std::optional<IndexRootLocation> existingRoot =
				metadata.findIndexRootOptional(planning.current_snapshot, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
		if (!existingRoot.has_value()) {
			return makeFreshPlan(BulkBTreeBuilder::planNativeTree(store.contentIndex(), metadata,
																  planning.current_snapshot,
																  RSS_LARGE_VALUE_CONTENT_INDEX_ID, additions, build),
								 options, checkpointLsn);
		}

		BTreeIndex existing(RSS_LARGE_VALUE_CONTENT_INDEX_ID, store.contentIndex(), metadata);
		const MetadataSnapshotPin *snapshot = &planning.current_snapshot;
		const OrderedBTreeEntrySource oldEntries = [existing = std::move(existing), snapshot,
													maximumEntries = options.maximum_entries]() mutable {
			return existing.orderedSource(*snapshot, maximumEntries)();
		};
		const std::array<OrderedBTreeEntrySource, 2U> sources = {oldEntries, additions};
		const OrderedBTreeEntrySource merged =
				NativeBulkRunMerger::orderedSource(sources, sources.size(), options.maximum_entries);
		const BTreeInitialTreeMutationPlan replacement =
				BulkBTreeBuilder::planNativeReplacementTree(store.contentIndex(), metadata, planning.current_snapshot,
															RSS_LARGE_VALUE_CONTENT_INDEX_ID, merged, build);
		const BTreeTreeRetirementPlan retirement = store.contentIndex().planTreeRetirement(
				planning.current_snapshot, RSS_LARGE_VALUE_CONTENT_INDEX_ID, options.maximum_pages);
		NativeGraphDeferredIndexPlan result = makeFreshPlan(replacement, options, checkpointLsn);
		result.mutation.expected_root = retirement.expected_root;
		result.mutation.pool_requests = retirement.pool_requests;
		const size_t retirementPoolCount = retirement.pool_requests.size();
		const IndexTreeMultiPoolMutationPreparer replacementPreparer = std::move(result.preparer);
		result.preparer = [retirementPreparer = retirement.preparer, replacementPreparer, retirementPoolCount,
						   checkpointLsn](std::vector<IndexTreeMutationPoolBatch> &batches) {
			if (batches.size() < retirementPoolCount) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge LargeValue content-index replacement changed its pool partition");
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
