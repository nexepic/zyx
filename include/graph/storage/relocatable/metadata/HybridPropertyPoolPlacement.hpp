/**
 * @file HybridPropertyPoolPlacement.hpp
 * @brief Private bounded placement and pin-safe reuse for native COW images.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPoolRoutes.hpp"
#include <algorithm>
#include "graph/debug/PerfTrace.hpp"

namespace graph::storage {
class HybridPropertyPoolPlacement final {
    friend class RelocatableMetadataStore;
    friend class HybridPropertyNativeWriteTestAccess;
    struct Reservation {
        CatalogEntry pool;
        size_t capacity = 0, assigned = 0, prepared = 0;
        bool fresh = false, native = false, reconciled = false, registered = false;
    };
    RelocatableMetadataStore &metadata_;
    MetadataWriteBatch &batch_;
    const MetadataWriteGuard &writer_;
    SharedPagePoolAllocator source_;
    utils::OwnedVector<Reservation> reservations_;
    std::optional<uint64_t> next_slot_;
    PhysicalSlotId last_slot_;
    bool skip_mixed_index_pools_ = false;
    std::function<void()> checkpoint_;

    HybridPropertyPoolPlacement(RelocatableMetadataStore &metadata, MetadataWriteBatch &batch,
        const MetadataWriteGuard &writer, const MetadataSnapshotPin &snapshot,
        PhysicalSlotId first, PhysicalSlotId last, std::shared_ptr<std::pmr::memory_resource> memory,
        std::function<void()> checkpoint, bool skipMixedIndexPools = false)
        : metadata_(metadata), batch_(batch), writer_(writer), source_(metadata.io_, metadata, snapshot),
          reservations_(utils::OwnedPmrAllocator<Reservation>(memory)), next_slot_(first.value()),
          last_slot_(last), skip_mixed_index_pools_(skipMixedIndexPools), checkpoint_(std::move(checkpoint)) {}
    bool visible(PageId id, SegmentId segment, uint16_t frame, Generation generation) const {
        return metadata_.snapshots_.anyActiveSnapshot(writer_, [&](const Superblock &snapshot) {
            const auto route = metadata_.locations_.findPageOptional(snapshot, id);
            return route && route->segment_id == segment && route->page_index == frame && route->generation == generation;
        });
    }
    bool contains(SegmentId id) const {
        return std::any_of(reservations_.begin(), reservations_.end(), [&](const auto &r) { return r.pool.segment_id == id; });
    }
    bool destination(SegmentId id) const {
        return std::any_of(reservations_.begin(), reservations_.end(), [&](const auto &r) { return r.pool.segment_id == id && r.assigned; });
    }
    void candidate(SegmentId id, SegmentKind kind, uint64_t owner, bool native) {
        using hybrid_property_detail::require;
        if (!id.isValid() || contains(id)) return;
        checkpoint_(); const auto &current = batch_.current();
        const auto entry = metadata_.catalog_.findOptional(current, id);
        if (!entry || entry->state != SegmentState::SGS_SEALED || entry->kind != kind || entry->owner_id != owner ||
            !metadata_.allocator_.isAllocated(current, entry->physical_slot)) return; // stale advisory hint
        const auto header = source_.readPoolHeader(*entry);
        size_t capacity = 0; bool mixedIndexPool = false;
        for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
            checkpoint_(); const bool allocated = SharedPagePoolFrameMap::isAllocated(header, frame);
            std::optional<PageOwnerHeader> page;
            try { page = source_.readPageOwner(*entry, frame); }
            catch (const FormatException &) { if (allocated) throw; ++capacity; continue; }
            const auto route = metadata_.locations_.findPageOptional(current, page->page_id);
            const bool currentFrame = route && route->segment_id == id && route->page_index == frame && route->generation == entry->generation;
            const bool pinned = visible(page->page_id, id, frame, entry->generation);
            require(allocated || (!currentFrame && !pinned), "native placement found a bitmap-hidden current or pinned frame");
            if (allocated) {
                // Do not mix ROW and native frames in a Property pool, even if
                // both happen to use the same numeric owner ID.
                if (kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && page->kind !=
                    (native ? PageOwnerKind::POK_HYBRID_PROPERTY : PageOwnerKind::POK_PROPERTY)) return;
                const bool mixed = kind == SegmentKind::SGK_INDEX_PAGE_POOL && page->owner_id != owner;
                mixedIndexPool = mixedIndexPool || mixed;
                require(page->generation == entry->generation && page->owner_id != 0 &&
                    (page->owner_id == owner || (mixed && skip_mixed_index_pools_)) &&
                    (kind != SegmentKind::SGK_INDEX_PAGE_POOL || page->kind == PageOwnerKind::POK_INDEX) &&
                    (kind != SegmentKind::SGK_ENTITY_PAGE_POOL || page->kind == PageOwnerKind::POK_ENTITY || page->kind == PageOwnerKind::POK_ENTITY_LABEL),
                    "native placement found a foreign page owner");
                if (currentFrame) require(page->flags == 0, "native placement found a retired current frame");
            }
            if (!allocated || (!currentFrame && !pinned)) ++capacity;
        }
        if (mixedIndexPool) {
            debug::PerfTrace::addValue("native_graph.write.index_placement.mixed_pool_skipped", 1);
            // Cache this rejection in the owned per-batch reservations: no
            // capacity/assignment means no frame is reused or remembered.
            reservations_.push_back({*entry, 0, 0, 0, false, native, false});
            return; // Ordinary format permits shared Index owners; do not reuse this target.
        }
        reservations_.push_back({*entry, capacity, 0, 0, false, native, false});
    }
    CatalogEntry allocate(SegmentKind kind, uint64_t owner, size_t count, bool native = false, bool forceFresh = false) {
        using hybrid_property_detail::require;
        require(count && count <= RSS_PAGE_POOL_DATA_FRAME_COUNT, "native placement exceeds one bounded physical pool");
        if (!forceFresh) {
            if (kind == SegmentKind::SGK_INDEX_PAGE_POOL) {
                const auto root = metadata_.locations_.findIndexRootOptional(batch_.current(), owner);
                if (root) candidate(metadata_.locations_.findPage(batch_.current(), root->root_page_id).segment_id, kind, owner, false);
            }
            for (const auto &hint : metadata_.native_pool_placement_hints_)
                if (hint.kind == kind && hint.owner_id == owner) candidate(hint.segment_id, kind, owner, native);
            for (auto &r : reservations_) if (r.pool.kind == kind && r.pool.owner_id == owner && r.native == native &&
                r.capacity - r.assigned >= count) { r.assigned += count; return r.pool; }
        }
        while (next_slot_) {
            checkpoint_(); const auto free = metadata_.allocator_.findFirstFree(batch_.current(), PhysicalSlotId(*next_slot_), last_slot_);
            if (!free) break;
            next_slot_ = *free == last_slot_ ? std::nullopt : std::optional(free->value() + 1U);
            if (batch_.isDataSlotReserved(*free) || batch_.isMetadataPoolSlot(*free)) continue;
            CatalogEntry pool{batch_.allocateLogicalSegmentId(), *free, kind, owner, Generation(1), SegmentState::SGS_SEALED, batch_.checkpointLsn()};
            batch_.reserveDataSlot(*free);
            reservations_.push_back({pool, RSS_PAGE_POOL_DATA_FRAME_COUNT, count, 0, true, native, false}); return pool;
        }
        throw FormatException(FormatError::FERR_INVALID_STATE, "native placement has no free slot or reusable bounded destination capacity");
    }
    void write(const CatalogEntry &pool, std::span<const SharedPagePoolPreparedAppend> pages, std::span<PageLocation> locations, bool native) {
        const auto found = std::find_if(reservations_.begin(), reservations_.end(), [&](const auto &r) { return r.pool.segment_id == pool.segment_id; });
        hybrid_property_detail::require(found != reservations_.end() && found->native == native &&
            found->prepared + pages.size() <= found->assigned, "native placement writes exceed their private reservation");
        if (found->fresh) SharedPagePoolAllocator::prepareNewPoolPagesUnderWrite(*metadata_.io_, batch_.current().data_region_offset,
            pool, pages, locations, batch_.checkpointLsn(), found->prepared, native);
        else {
            // Exactly once, before the first append: later appends in this same
            // batch are still unpublished and must not be mistaken for orphans.
            if (!found->reconciled) {
                static_cast<void>(source_.reconcilePoolFrames(pool,
                    [&](PageId id) { return metadata_.locations_.findPageOptional(batch_.current(), id); },
                    [&](SegmentId id) { return metadata_.catalog_.find(batch_.current(), id); },
                    [&](PageId id, SegmentId segment, uint16_t frame, Generation generation) { return visible(id, segment, frame, generation); }, native));
                found->reconciled = true;
            }
            SharedPagePoolAllocator::prepareAppendPagesUnderWrite(*metadata_.io_, batch_.current().data_region_offset,
                pool, pages, locations, batch_.checkpointLsn(), native);
        }
        found->prepared += pages.size();
        debug::PerfTrace::addValue("native_graph.hybrid.pool_frames_prepared", int64_t(pages.size()));
    }
    void remember() {
        for (const auto &r : reservations_) if (r.assigned &&
            (r.pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL || r.pool.kind == SegmentKind::SGK_INDEX_PAGE_POOL || r.pool.kind == SegmentKind::SGK_ENTITY_PAGE_POOL)) {
            auto &hints = metadata_.native_pool_placement_hints_;
            // Keep distinct destinations in the existing fixed-size hint ring;
            // one hint per owner forgets reusable tails in its earlier pools.
            auto target = std::find_if(hints.begin(), hints.end(), [&](const auto &h) { return h.segment_id == r.pool.segment_id; });
            if (target == hints.end()) target = hints.begin() + (metadata_.native_pool_hint_cursor_++ % hints.size());
            *target = r.pool;
        }
    }
};
} // namespace graph::storage
