/** Bounded type membership maintenance inside streamed graph publication. */
#include "graph/storage/relocatable/entity/NativeStreamedGraphMembershipIndexPlanner.hpp"
#include <array>
#include "graph/storage/relocatable/metadata/NativeGraphDeferredIndexImages.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <limits>
#include <utility>
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/index/ExternalBTreeEntrySorter.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/index/NativeBulkRun.hpp"
namespace graph::storage {
	namespace {
		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }
		[[nodiscard]] NativeGraphDeferredIndexPlan
		makeFreshPlan(uint64_t indexId, const BTreeInitialTreeMutationPlan &plan,
					  const NativeStreamedGraphMembershipIndexPlanningOptions &options,
					  LogSequenceNumber checkpointLsn) {
			// BulkBTreeBuilder returns a complete plan after validating its fresh root.
			NativeGraphDeferredIndexPlan result;
			result.mutation.index_id = indexId;
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
	NativeGraphDeferredIndexPlans NativeStreamedGraphMembershipIndexPlanner::plan(
			SystemCatalog &catalog, BTreePageStore &pages, RelocatableMetadataStore &metadata,
			const NativeStreamedGraphDeferredPublication *publication,
			const NativeStreamedGraphMembershipIndexPlanningOptions &options, const MetadataSnapshotPin &snapshot,
			LogSequenceNumber checkpointLsn, OrderedBTreeEntrySource *tokenCountUpdates, const NativeGraphDeferredIndexPlanningContext *planning) {
		NativeGraphDeferredIndexPlans plans;
		const auto definitions = catalog.readIndexDefinitions(snapshot, options.maximum_definitions);
		bool hasMembers = false;
		for (const auto &definition: definitions) {
			if (definition.kind == IndexDefinitionKind::IDK_LABEL &&
				definition.entity_kind == IndexDefinitionEntityKind::IDEK_EDGE)
				hasMembers = true;
		}
		const bool collectCounts =
				tokenCountUpdates != nullptr &&
				catalog.hasCompleteTokenCardinality(snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE);
		if (tokenCountUpdates)
			*tokenCountUpdates = {};
		if (!hasMembers && !collectCounts)
			return plans;
		if (!catalog.usesMetadataStore(metadata) || !pages.usesMetadataStore(metadata) || publication == nullptr ||
			!options.first_slot.isValid() || !options.last_slot.isValid() || options.first_slot > options.last_slot ||
			options.maximum_entries == 0U || options.maximum_pages == 0U || options.sort_run_bytes == 0U ||
			options.maximum_open_runs < 2U || !checkpointLsn.isValid()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "streamed membership planning requires a bounded authenticated source");
		}
		// One page-sized source pass and one bounded sorter are shared by every
		// existing EDGE membership definition. The returned source owns its runs;
		// no deferred preparer borrows the callback-scoped Token resolver.
		ExternalBTreeEntrySorter sorter({options.maximum_entries, options.sort_run_bytes, options.maximum_open_runs});
		size_t added = 0U;
		size_t namedAdded = 0U;
		publication->visitEntityPages([&](NativeBulkEdgeEntityPageBatch &&batch) {
			for (const auto &record: batch.records) {
				const uint64_t token = record.entity.type_id;
				if (token == 0U && !collectCounts)
					continue;
				if (token > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
					fail(FormatError::FERR_INVALID_STATE, "streamed membership type exceeds scalar representation");
				}
				sorter.add({IndexKeyCodec::encodeEntryKey(PropertyValue(static_cast<int64_t>(token)), record.edge_id),
							record.edge_id});
				++added;
				if (token != 0U)
					++namedAdded;
			}
		});
		if (added == 0U)
			return plans;
		const auto allInserted = sorter.finish();
		const OrderedBTreeEntrySource inserted = [allInserted] {
			auto cursor = allInserted();
			return [cursor = std::move(cursor)]() mutable -> std::optional<BTreePageEntry> {
				while (auto entry = cursor()) {
					if (IndexKeyCodec::decodeIntegerEntryValue(entry->key) != 0)
						return entry;
				}
				return std::nullopt;
			};
		};
		if (collectCounts) {
			const auto *pin = &snapshot;
			*tokenCountUpdates = [allInserted, &catalog, pin] {
				auto cursor = allInserted();
				return [cursor = std::move(cursor), &catalog, pin,
						pending = std::optional<BTreePageEntry>{}]() mutable -> std::optional<BTreePageEntry> {
					if (!pending)
						pending = cursor();
					if (!pending)
						return std::nullopt;
					const uint64_t token = static_cast<uint64_t>(IndexKeyCodec::decodeIntegerEntryValue(pending->key));
					uint64_t increment = 0U;
					do {
						++increment;
						pending = cursor();
					} while (pending &&
							 static_cast<uint64_t>(IndexKeyCodec::decodeIntegerEntryValue(pending->key)) == token);
					const auto current = catalog.readTokenCardinality(
							*pin, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, token);
					if (!current || increment > std::numeric_limits<uint64_t>::max() - *current) {
						fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
							 "streamed type cardinality lost completeness or overflowed");
					}
					return BTreePageEntry{SystemCatalogTokenCardinalityCodec::countKeyOwned(
												  SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, token),
										  SystemCatalogGraphCardinalityCodec::encodeCount(*current + increment)};
				};
			};
		}

		if (namedAdded == 0U)
			return plans;
		for (const auto &definition: definitions) {
			if (definition.kind != IndexDefinitionKind::IDK_LABEL ||
				definition.entity_kind != IndexDefinitionEntityKind::IDEK_EDGE)
				continue;
            const auto existingRoot = metadata.findIndexRootOptional(snapshot, definition.index_id);
            if (!existingRoot) fail(FormatError::FERR_INVALID_STATE, "streamed membership definition has no published root");
            const auto memory = utils::OwnedAllocationScope::owner();
            // Direct legacy planner callers retain the complete builder. Only
            // the writer's callback-scoped source can mint an image receipt.
            const bool scoped = memory && planning && planning->current_snapshot.superblock() == snapshot.superblock() &&
                planning->private_segments.size() && planning->private_segment_reader;
            const auto sourceDataRoot = snapshot.superblock().usesPageDirectoryBootstrap() ?
                metadata.pages_.readPageDirectoryBootstrap(snapshot.superblock().page_directory_root).data_page_directory_root :
                snapshot.superblock().page_directory_root;
            const auto receipt = [&] {
                return std::shared_ptr<NativeGraphDeferredIndexImages>(new (memory) NativeGraphDeferredIndexImages(
                    metadata, snapshot.superblock().generation, definition.index_id, existingRoot, memory,
                    options.first_slot, options.last_slot, planning->private_segments, planning->private_segment_reader, sourceDataRoot),
                    std::default_delete<NativeGraphDeferredIndexImages>(), utils::OwnedPmrAllocator<std::byte>(memory));
            };
            const auto hint = [&] {
                std::lock_guard lock(metadata.streamed_adjacency_shape_mutex_); return metadata.streamed_membership_shape_;
            }();
            constexpr auto threshold = NativeGraphWritePlannerOptions::DEFAULT_GLOBAL_ADJACENCY_BULK_REBUILD_ENTRY_THRESHOLD;
            if (scoped && namedAdded < threshold && hint.root == existingRoot && hint.data_root == sourceDataRoot &&
                hint.pages && hint.pages <= options.maximum_pages && hint.entries <= options.maximum_entries) {
                utils::OwnedHeapReservation cursorCredit(memory, 65536U);
                size_t count = 0, newKeys = 0;
                BTreeIndex current(definition.index_id, pages, metadata); auto cursor = inserted();
                while (auto entry = cursor()) {
                    if (++count > namedAdded) fail(FormatError::FERR_INVALID_STATE, "streamed membership insertion count changed");
                    if (!current.find(snapshot, entry->key)) ++newKeys;
                }
                if (count != namedAdded) fail(FormatError::FERR_INVALID_STATE, "streamed membership insertion source incomplete");
                cursor = {}; // Release the sorter lease before opening the overlay pass.
                if (newKeys > options.maximum_entries - hint.entries)
                    fail(FormatError::FERR_INVALID_STATE, "streamed membership delta exceeds final entry bound");
                auto proof = receipt(); proof->incremental_ = true;
                const auto source = [&](const std::function<void(const BTreeScalarIndexOperation &)> &emit) {
                    auto input = inserted(); while (auto entry = input())
                        emit({BTreeScalarIndexOperationKind::BSIOP_UPSERT, std::move(entry->key), entry->value_or_child});
                };
                proof->plan_ = pages.planStreamedMembershipImagesFromSource(snapshot, definition, count, source, memory,
                    [target = proof.get()](BTreePage page) { target->images_.push_back(std::move(page)); });
                size_t appended = 0, erased = 0;
                for (const auto &request : proof->plan_.pool_requests) {
                    appended += request.appended_page_count; erased += request.expected_retirements.size();
                }
                if (proof->plan_.new_pool_mutation) appended += proof->plan_.new_pool_mutation->appended_page_count;
                for (const auto &pool : proof->plan_.additional_new_pool_mutations) appended += pool.appended_page_count;
                if (erased > hint.pages || appended > options.maximum_pages - (hint.pages - erased))
                    fail(FormatError::FERR_INVALID_STATE, "streamed membership delta exceeds final page bound");
                proof->entry_count_ = hint.entries + newKeys; proof->page_count_ = hint.pages - erased + appended;
                proof->assignments_credit_.set(16384U +
                    (proof->plan_.pool_requests.size() + proof->plan_.additional_new_pool_mutations.size() +
                        size_t(bool(proof->plan_.new_pool_mutation))) * 8192U);
                NativeGraphDeferredIndexPlan result; result.mutation.index_id = definition.index_id;
                result.images = std::move(proof); plans.plans.push_back(std::move(result));
                debug::PerfTrace::addValue("native_graph.write.streamed_membership.incremental_plans", 1);
                debug::PerfTrace::addValue("native_graph.write.streamed_membership.incremental_keys", int64_t(count));
                continue;
            }
            debug::PerfTrace::addValue("native_graph.write.streamed_membership.full_plans", 1);
			BTreeIndex existing(definition.index_id, pages, metadata);
			const MetadataSnapshotPin *pin = &snapshot;
			const OrderedBTreeEntrySource old = [existing = std::move(existing), pin,
												 maximum = options.maximum_entries]() mutable {
				return existing.orderedSource(*pin, maximum)();
			};
			const std::array sources{old, inserted};
			const auto merged = NativeBulkRunMerger::orderedSource(sources, 2U, options.maximum_entries);
			const BulkBTreeBuildOptions build{options.first_slot, options.last_slot, definition.index_id,
											  options.maximum_entries, options.maximum_pages};
			const auto replacement = BulkBTreeBuilder::planNativeReplacementTree(pages, metadata, snapshot,
																				 definition.index_id, merged, build);
			const auto retirement = pages.planTreeRetirement(snapshot, definition.index_id, options.maximum_pages);
			auto result = makeFreshPlan(definition.index_id, replacement, options, checkpointLsn);
			result.mutation.expected_root = retirement.expected_root;
			result.mutation.pool_requests = retirement.pool_requests;
			const size_t retirementPoolCount = retirement.pool_requests.size();
			const IndexTreeMultiPoolMutationPreparer replacementPreparer = std::move(result.preparer);
			result.preparer = [retirementPreparer = retirement.preparer, replacementPreparer, retirementPoolCount,
							   checkpointLsn](std::vector<IndexTreeMutationPoolBatch> &batches) {
				if (batches.size() < retirementPoolCount) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed Edge membership replacement changed its pool partition");
				}
				std::vector<IndexTreeMutationPoolBatch> retired(
						batches.begin(), batches.begin() + static_cast<ptrdiff_t>(retirementPoolCount));
				std::vector<IndexTreeMutationPoolBatch> fresh(
						batches.begin() + static_cast<ptrdiff_t>(retirementPoolCount), batches.end());
				retirementPreparer(retired, checkpointLsn);
				replacementPreparer(fresh);
				std::move(retired.begin(), retired.end(), batches.begin());
				std::move(fresh.begin(), fresh.end(), batches.begin() + static_cast<ptrdiff_t>(retirementPoolCount));
			};
            if (scoped) {
                auto proof = receipt(); proof->entry_count_ = replacement.entry_count;
                for (const auto &pool : replacement.new_pool_mutations) proof->page_count_ += pool.appended_page_count;
                proof->fresh_mutation_ = std::move(result.mutation); proof->fresh_preparer_ = std::move(result.preparer);
                result.mutation = {}; result.mutation.index_id = definition.index_id; result.images = std::move(proof);
            }
			plans.plans.push_back(std::move(result));
		}
		return plans;
	}
} // namespace graph::storage
