/**
 * @file HybridPropertyDirectoryPreparation.hpp
 * @brief Private dirty-path directory images for bounded hybrid group changes.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectory.hpp"

namespace graph::storage {
class HybridPropertyDirectoryPreparation final {
    friend class RelocatableMetadataStore;
    struct Image { CatalogEntry pool; PageLocation location; BTreePage page; };
    utils::OwnedVector<Image> images_;
    utils::OwnedVector<uint64_t> superseded_, erased_;
    std::optional<IndexRootLocation> root_;
    template<class Map, class AllocatePool, class Checkpoint>
    HybridPropertyDirectoryPreparation(MetadataWriteBatch &batch, const MetadataSnapshotPin &source,
        RelocatableMetadataStore &metadata, BTreePageStore &trees, const BTreeRootReadBinding &binding,
        std::span<const typename Map::Entry> upserts, std::span<const typename Map::Key> erases,
        const std::shared_ptr<std::pmr::memory_resource> &memory, const CancellationCheckpoint *cancellation,
        AllocatePool &&allocatePool, Checkpoint &&checkpoint, std::type_identity<Map>)
        : images_(utils::OwnedPmrAllocator<Image>(memory)), superseded_(utils::OwnedPmrAllocator<uint64_t>(memory)),
          erased_(utils::OwnedPmrAllocator<uint64_t>(memory)), root_(binding.root) {
        const auto require = [](bool valid, const char *message) { hybrid_property_detail::require(valid, message); };
        const auto index = binding.root.index_id;
        require(metadata.ownsSnapshot(source) && source.superblock() == batch.current() && memory &&
            binding.database_id == source.superblock().database_id && binding.snapshot_generation == source.superblock().generation &&
            isHybridPropertyIndexId(index) && upserts.size() <= 4096U && erases.size() <= 4096U, "native shared-directory preparation has a foreign namespace or source");
        Map map(index, trees, metadata, binding);
        // A complete bounded prefix census can prove that the selected group
        // owns the entire tree. Retire it directly instead of retaining an
        // overlay containing thousands of individual deletions.
        if (upserts.empty() && !erases.empty()) {
            utils::OwnedHeapReservation censusBoundary(memory);
            size_t position = 0; bool exclusive = true;
            const bool exhausted = map.visitAll(source, erases.size() + 1U, [&](const auto &entry) {
                checkpoint();
                if (position == erases.size() || entry.key != erases[position]) { exclusive = false; return false; }
                ++position; return true;
            }, [&](size_t bytes) { censusBoundary.set(bytes); }, cancellation);
            censusBoundary.set(0);
            if (exclusive && exhausted && position == erases.size()) {
                utils::OwnedHeapReservation frontier(memory, 256U * 1024U);
                const auto plan = trees.planTreeRetirement(source, index, 128U, cancellation);
                require(plan.expected_root == binding.root, "native exact directory retirement lost its source root");
                for (const auto &pool : plan.pool_requests) for (const auto &page : pool.expected_retirements) {
                    superseded_.push_back(page.location.page_id.value()); erased_.push_back(page.location.page_id.value());
                }
                root_.reset(); return;
            }
        }
        // Retain only the bounded selected keys' values, not the whole tree.
        // A new group's upserts are mostly absent: reserving two operations per
        // key can exceed the operation account before actual work is known.
        utils::OwnedVector<typename Map::Value> erasedValues{utils::OwnedPmrAllocator<typename Map::Value>(memory)};
        utils::OwnedVector<std::optional<typename Map::Value>> oldValues{
            utils::OwnedPmrAllocator<std::optional<typename Map::Value>>(memory)};
        erasedValues.reserve(erases.size()); oldValues.reserve(upserts.size());
        utils::OwnedHeapReservation boundary(memory);
        const auto admit = [&](size_t bytes) { boundary.set(bytes); };
        size_t maximumOperations = erases.size();
        for (const auto &key : erases) {
            checkpoint(); const auto old = map.find(source, key, std::ref(admit), cancellation); boundary.set(0);
            require(bool(old), "native shared-directory erasure has no exact source prefix");
            erasedValues.push_back(*old);
        }
        for (const auto &entry : upserts) {
            checkpoint(); const auto old = map.find(source, entry.key, std::ref(admit), cancellation); boundary.set(0);
            oldValues.push_back(old);
            if (old != std::optional(entry.value)) maximumOperations += old ? 2U : 1U;
        }
        if (!maximumOperations) return;
        debug::PerfTrace::addValue("native_graph.hybrid.directory_operations_prepared", int64_t(maximumOperations));
        // Feed one admitted wire operation at a time into the same dirty-path
        // overlay. Keeping a second complete operation vector would make a
        // valid 4096-member group exceed its account when the tree exists.
        utils::OwnedHeapReservation operationsCredit(memory, 16384U);
        const auto sourceOperations = [&](const std::function<void(const BTreeScalarIndexOperation &)> &emit) {
            const auto operation = [&](BTreeScalarIndexOperationKind action, const typename Map::Key &key, const typename Map::Value &value) {
                checkpoint(); const auto record = Map::encode(key, value);
                const BTreeScalarIndexOperation next{action, std::vector<std::byte>(record.begin(), record.end()), 0};
                emit(next);
            };
            for (size_t i = 0; i < erases.size(); ++i)
                operation(BTreeScalarIndexOperationKind::BSIOP_ERASE, erases[i], erasedValues[i]);
            for (size_t i = 0; i < upserts.size(); ++i) {
                checkpoint(); const auto &entry = upserts[i]; const auto &old = oldValues[i];
                if (old == std::optional(entry.value)) continue;
                if (old) operation(BTreeScalarIndexOperationKind::BSIOP_ERASE, entry.key, *old);
                operation(BTreeScalarIndexOperationKind::BSIOP_UPSERT, entry.key, entry.value);
            }
        };
        utils::OwnedVector<BTreePage> dirty{utils::OwnedPmrAllocator<BTreePage>(memory)};
        debug::PerfTrace::addValue("native_graph.hybrid.directory_dirty_path_plans", 1);
        auto plan = trees.planHybridMapImagesFromSource(source, index, maximumOperations, sourceOperations, memory,
            [&](BTreePage image) { checkpoint(); dirty.push_back(std::move(image)); }, cancellation);
        utils::OwnedHeapReservation assignmentsCredit(memory, 16384U +
            (plan.pool_requests.size() + plan.additional_new_pool_mutations.size() + size_t(bool(plan.new_pool_mutation))) * 8192U);
        std::vector<IndexTreeMutationPoolBatch> assignments;
        assignments.reserve(plan.pool_requests.size() + plan.additional_new_pool_mutations.size() + size_t(bool(plan.new_pool_mutation)));
        for (const auto &request : plan.pool_requests) {
            IndexTreeMutationPoolBatch assignment; assignment.pool = metadata.findCatalog(source, request.segment_id);
            assignment.replacement_pool = assignment.pool; assignment.expected_replacements = request.expected_replacements;
            assignment.expected_retirements = request.expected_retirements;
            require(!request.appended_page_count && !request.replacement_segment_id.isValid(),
                "private shared-directory plan selected source physical capacity");
            for (const auto &page : request.expected_replacements) superseded_.push_back(page.page_id.value());
            for (const auto &page : request.expected_retirements) {
                superseded_.push_back(page.location.page_id.value()); erased_.push_back(page.location.page_id.value());
            }
            assignments.push_back(std::move(assignment));
        }
        const auto appendPool = [&](const BTreeNewIndexTreePoolMutation &request) {
            IndexTreeMutationPoolBatch assignment; assignment.newly_reserved = true;
            assignment.pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL; assignment.pool.owner_id = index;
            assignment.pool.generation = Generation(1); assignment.pool.state = SegmentState::SGS_SEALED;
            assignment.pool.committed_lsn = batch.checkpointLsn(); assignment.replacement_pool = assignment.pool;
            assignment.appended_page_ids.reserve(request.appended_page_count);
            assignment.appended_locations.reserve(request.appended_page_count);
            for (size_t i = 0; i < request.appended_page_count; ++i) {
                const auto id = batch.allocateLogicalPageId(); assignment.appended_page_ids.push_back(id);
                assignment.appended_locations.push_back({id, {}, 0, Generation(1), 0});
            }
            assignments.push_back(std::move(assignment));
        };
        if (plan.new_pool_mutation) appendPool(*plan.new_pool_mutation);
        for (const auto &pool : plan.additional_new_pool_mutations) appendPool(pool);
        plan.preparer(assignments, batch.checkpointLsn());
        if (plan.final_tree_empty) {
            for (const auto &pool : plan.pool_requests)
                for (const auto &page : pool.expected_replacements) erased_.push_back(page.page_id.value());
            root_.reset(); return;
        }
        auto root = plan.stable_replacement_root.value_or(plan.expected_root.root_page_id);
        if (plan.replacement_root) root = assignments.at(plan.replacement_root->pool_ordinal).appended_page_ids.at(plan.replacement_root->appended_page_ordinal);
        auto generation = binding.root.root_generation;
        CatalogEntry pool; size_t used = RSS_PAGE_POOL_DATA_FRAME_COUNT;
        for (auto &page : dirty) {
            checkpoint(); if (used == RSS_PAGE_POOL_DATA_FRAME_COUNT) { pool = allocatePool(SegmentKind::SGK_INDEX_PAGE_POOL, index, std::min(size_t(RSS_PAGE_POOL_DATA_FRAME_COUNT), dirty.size() - images_.size())); used = 0; }
            const PageOwnerHeader owner{page.header().page_id, pool.owner_id, PageOwnerKind::POK_INDEX, 0,
                RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
            auto rebound = page.rebindOwner(owner);
            const PageLocation location{owner.page_id, pool.segment_id, uint16_t(RSS_PAGE_POOL_FIRST_DATA_FRAME + used++),
                pool.generation, rebound.slottedPage().stats().contiguous_free_bytes};
            if (owner.page_id == root) generation = pool.generation;
            images_.push_back({pool, location, std::move(rebound)});
        }
        root_ = IndexRootLocation{index, root, generation};
    }
};
} // namespace graph::storage
