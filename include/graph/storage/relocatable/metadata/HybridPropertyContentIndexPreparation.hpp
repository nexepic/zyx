/**
 * @file HybridPropertyContentIndexPreparation.hpp
 * @brief Private content-index images shared by bounded native conversions.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/storage/relocatable/property/HybridPropertyLargeFieldPreparation.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include <algorithm>

namespace graph::storage {
class HybridPropertyContentIndexPreparation final {
    friend class RelocatableMetadataStore;
    struct Image { CatalogEntry pool; PageLocation location; BTreePage page; };
    utils::OwnedVector<Image> images_;
    utils::OwnedVector<uint64_t> superseded_, erased_;
    std::optional<IndexRootLocation> root_;
    template<class AllocatePool, class Checkpoint>
    HybridPropertyContentIndexPreparation(MetadataWriteBatch &batch, const MetadataSnapshotPin &source,
        const NativeGraphHybridPropertyConversion &conversion, RelocatableMetadataStore &metadata,
        BTreePageStore &trees, LargeValueStore &values,
        std::span<const HybridPropertyLargeFieldPreparation> largeFields,
        AllocatePool &&allocatePool, Checkpoint &&checkpoint,
        std::span<const NativeGraphLargeValueMutation> jointValues = {})
        : images_(utils::OwnedPmrAllocator<Image>(conversion.memory)),
          superseded_(utils::OwnedPmrAllocator<uint64_t>(conversion.memory)),
          erased_(utils::OwnedPmrAllocator<uint64_t>(conversion.memory)) {
        const auto &current = batch.current();
        hybrid_property_detail::require(RSS_HYBRID_PROPERTY_FORMAT_ENABLED && conversion.memory &&
            metadata.ownsSnapshot(source) && source.superblock() == current && trees.usesMetadataStore(metadata) &&
            values.usesMetadataStore(metadata) && largeFields.size() <= 4096U && jointValues.size() <= 64U,
            "native content preparation requires one metadata-owned source and bounded proposals");
        utils::OwnedAllocationScope scope(conversion.memory);
        size_t newLargeValues = jointValues.size();
        for (const auto &value : largeFields) {
            checkpoint();
            hybrid_property_detail::require(value.databaseId() == current.database_id && value.sourceGeneration() == current.generation,
                "native content preparation has a stale immutable proposal");
            newLargeValues += !value.reused();
        }
        auto &contentRoot = root_; auto &supersededContentPages = superseded_; auto &erasedContentPages = erased_;
        auto *indexImages = &images_; auto *conversionIndexes = &trees; auto *conversionLargeValues = &values;
        const auto collectIndexImages = [&](const CatalogEntry &pool, std::span<BTreePage> images,
            std::span<PageLocation> locations, LogSequenceNumber lsn) {
            hybrid_property_detail::require(images.size() == locations.size() && !images.empty() && pool.committed_lsn == lsn,
                "native content images lost their assigned placement");
            for (size_t i = 0; i < images.size(); ++i) {
                checkpoint(); const auto header = images[i].header();
                hybrid_property_detail::require(header.index_id == RSS_LARGE_VALUE_CONTENT_INDEX_ID && pool.owner_id == header.index_id &&
                    header.page_id == locations[i].page_id && header.generation == pool.generation &&
                    locations[i].segment_id == pool.segment_id && locations[i].page_index == 0 &&
                    images[i].slottedPage().ownerCommittedLsn() == lsn,
                    "native content image changed its assigned identity");
                locations[i].page_index = uint16_t(RSS_PAGE_POOL_FIRST_DATA_FRAME + i);
                locations[i].free_bytes = images[i].slottedPage().stats().contiguous_free_bytes;
                images_.push_back({pool, locations[i], std::move(images[i])});
            }
        };
            if (newLargeValues) {
                // The content index is part of this conversion's ONE publication.
                // Existing trees use dirty-path COW, never a full index rebuild.
                utils::OwnedHeapReservation keyCredit(conversion.memory, newLargeValues * 4096U);
                utils::OwnedVector<BTreeScalarIndexOperation> operations{utils::OwnedPmrAllocator<BTreeScalarIndexOperation>(conversion.memory)};
                operations.reserve(newLargeValues);
                for (const auto &value : largeFields) if (!value.reused()) {
                    checkpoint(); operations.push_back({BTreeScalarIndexOperationKind::BSIOP_UPSERT,
                        conversionLargeValues->contentIndexKey(value.reference()), value.reference().value_id.value()});
                }
                for (const auto &value : jointValues) {
                    checkpoint(); operations.push_back({BTreeScalarIndexOperationKind::BSIOP_UPSERT,
                        conversionLargeValues->contentIndexKey(value.reference), value.reference.value_id.value()});
                }
                std::sort(operations.begin(), operations.end(), [](const auto &a, const auto &b) { return a.key < b.key; });
                const auto existing = metadata.findIndexRootOptional(source, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
                if (!existing) {
                    OrderedBTreeEntrySource entries = [&operations]() -> OrderedBTreeEntryCursor {
                        return [&operations, position = size_t(0)]() mutable -> std::optional<BTreePageEntry> {
                            if (position == operations.size()) return {};
                            const auto &operation = operations[position++];
                            return BTreePageEntry{utils::OwnedStdVector<std::byte>(operation.key.begin(), operation.key.end()), operation.value};
                        };
                    };
                    const BulkBTreeBuildOptions build{conversion.first_slot, conversion.last_slot, RSS_LARGE_VALUE_CONTENT_INDEX_ID,
                        operations.size(), operations.size(), BTreeLeafValueLayout::BLVL_U64};
                    auto plan = BulkBTreeBuilder::planNativeTreeWithImageSink(*conversionIndexes, metadata, source,
                        RSS_LARGE_VALUE_CONTENT_INDEX_ID, entries, build, collectIndexImages);
                    utils::OwnedHeapReservation assignmentCredit(conversion.memory, 16384U + plan.new_pool_mutations.size() * 8192U);
                    std::vector<IndexTreeMutationPoolBatch> assignments(plan.new_pool_mutations.size());
                    for (size_t i = 0; i < assignments.size(); ++i) {
                        auto &assignment = assignments[i]; assignment.newly_reserved = true;
                        assignment.pool = allocatePool(SegmentKind::SGK_INDEX_PAGE_POOL, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
                        assignment.replacement_pool = assignment.pool;
                        for (size_t n = 0; n < plan.new_pool_mutations[i].appended_page_count; ++n) {
                            const auto id = batch.allocateLogicalPageId(); assignment.appended_page_ids.push_back(id);
                            assignment.appended_locations.push_back({id, assignment.pool.segment_id, 0, assignment.pool.generation, 0});
                        }
                    }
                    plan.preparer(assignments, batch.checkpointLsn());
                    const auto &root = assignments.at(plan.root_page.pool_ordinal);
                    contentRoot = IndexRootLocation{RSS_LARGE_VALUE_CONTENT_INDEX_ID,
                        root.appended_page_ids.at(plan.root_page.appended_page_ordinal), root.pool.generation};
                } else {
                    utils::OwnedVector<BTreePage> dirty{utils::OwnedPmrAllocator<BTreePage>(conversion.memory)};
                    auto plan = conversionIndexes->planHybridMapImages(source, RSS_LARGE_VALUE_CONTENT_INDEX_ID,
                        operations, conversion.memory, [&](BTreePage page) { checkpoint(); dirty.push_back(std::move(page)); }, conversion.cancellation);
                    utils::OwnedHeapReservation assignmentCredit(conversion.memory, 16384U +
                        (plan.pool_requests.size() + plan.additional_new_pool_mutations.size() + size_t(bool(plan.new_pool_mutation))) * 8192U);
                    std::vector<IndexTreeMutationPoolBatch> assignments;
                    for (const auto &request : plan.pool_requests) {
                        IndexTreeMutationPoolBatch assignment; assignment.pool = metadata.findCatalog(source, request.segment_id);
                        assignment.replacement_pool = assignment.pool; assignment.expected_replacements = request.expected_replacements;
                        assignment.expected_retirements = request.expected_retirements;
                        if (request.appended_page_count || request.replacement_segment_id.isValid())
                            throw FormatException(FormatError::FERR_INVALID_STATE, "private content index selected source physical capacity");
                        for (const auto &page : request.expected_replacements) supersededContentPages.push_back(page.page_id.value());
                        for (const auto &page : request.expected_retirements) {
                            supersededContentPages.push_back(page.location.page_id.value()); erasedContentPages.push_back(page.location.page_id.value());
                        }
                        assignments.push_back(std::move(assignment));
                    }
                    const auto addVirtualPool = [&](const BTreeNewIndexTreePoolMutation &request) {
                        IndexTreeMutationPoolBatch assignment; assignment.newly_reserved = true;
                        assignment.pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL; assignment.pool.owner_id = RSS_LARGE_VALUE_CONTENT_INDEX_ID;
                        assignment.pool.generation = Generation(1); assignment.pool.state = SegmentState::SGS_SEALED;
                        assignment.pool.committed_lsn = batch.checkpointLsn(); assignment.replacement_pool = assignment.pool;
                        for (size_t n = 0; n < request.appended_page_count; ++n) {
                            const auto id = batch.allocateLogicalPageId(); assignment.appended_page_ids.push_back(id);
                            assignment.appended_locations.push_back({id, {}, 0, Generation(1), 0});
                        }
                        assignments.push_back(std::move(assignment));
                    };
                    if (plan.new_pool_mutation) addVirtualPool(*plan.new_pool_mutation);
                    for (const auto &pool : plan.additional_new_pool_mutations) addVirtualPool(pool);
                    plan.preparer(assignments, batch.checkpointLsn());
                    if (plan.final_tree_empty || dirty.empty())
                        throw FormatException(FormatError::FERR_INVALID_STATE, "new immutable values lost their content-index insertion");
                    auto rootId = plan.stable_replacement_root.value_or(plan.expected_root.root_page_id);
                    if (plan.replacement_root) rootId = assignments.at(plan.replacement_root->pool_ordinal).appended_page_ids.at(plan.replacement_root->appended_page_ordinal);
                    Generation rootGeneration = existing->root_generation;
                    CatalogEntry pool; size_t used = RSS_PAGE_POOL_DATA_FRAME_COUNT;
                    for (auto &page : dirty) {
                        checkpoint(); if (used == RSS_PAGE_POOL_DATA_FRAME_COUNT) {
                            pool = allocatePool(SegmentKind::SGK_INDEX_PAGE_POOL, RSS_LARGE_VALUE_CONTENT_INDEX_ID, std::min(size_t(RSS_PAGE_POOL_DATA_FRAME_COUNT), dirty.size() - images_.size())); used = 0;
                        }
                        const PageOwnerHeader owner{page.header().page_id, pool.owner_id, PageOwnerKind::POK_INDEX, 0,
                            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
                        auto rebound = page.rebindOwner(owner);
                        const PageLocation location{owner.page_id, pool.segment_id, uint16_t(RSS_PAGE_POOL_FIRST_DATA_FRAME + used++),
                            pool.generation, rebound.slottedPage().stats().contiguous_free_bytes};
                        if (owner.page_id == rootId) rootGeneration = pool.generation;
                        indexImages->push_back({pool, location, std::move(rebound)});
                    }
                    contentRoot = IndexRootLocation{RSS_LARGE_VALUE_CONTENT_INDEX_ID, rootId, rootGeneration};
                }
            }
    }
};
} // namespace graph::storage
