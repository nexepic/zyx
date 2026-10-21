/**
 * @file RelocatableMetadataStoreNativePoolRecovery.cpp
 * @brief Recover unpublished native COW frames before a subsequent publication.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDescriptor.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPoolRoutes.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"

namespace graph::storage {
void RelocatableMetadataStore::recoverNativePoolFrames(const MetadataWriteGuard &writer, const Superblock &current) {
    const auto require = [](bool valid, const char *message) {
        if (!valid) throw FormatException(FormatError::FERR_INVALID_STATE, message);
    };
    // This streaming Catalog walk is recovery work, never placement discovery
    // on an ordinary successful write. No payloads or whole-database maps live here.
    const auto pin = snapshots_.pinCurrentUnderWrite(writer, current);
    SharedPagePoolAllocator pools(io_, *this, pin);
    std::optional<utils::OwnedHeapReservation> frontier;
    const auto admitFrontier = [&] {
        if (!frontier) if (const auto memory = utils::OwnedAllocationScope::owner()) frontier.emplace(memory, 32U * 1024U);
    };
    const auto findPage = [&](PageId id) { return locations_.findPageOptional(current, id); };
    const auto findCatalog = [&](SegmentId id) { return catalog_.find(current, id); };
    const auto visible = [&](PageId id, SegmentId segment, uint16_t frame, Generation generation) {
        return snapshots_.anyActiveSnapshot(writer, [&](const Superblock &snapshot) {
            const auto route = locations_.findPageOptional(snapshot, id);
            return route && route->segment_id == segment && route->page_index == frame && route->generation == generation;
        });
    };
    // Validate current routes, including allocation bits, before changing any
    // pool. Recovery may clear abandoned frames; it must never conceal a lost
    // committed frame behind an otherwise valid control-record checksum.
    locations_.visitPages(current, [&](const PageLocation &route) {
        const auto pool = findCatalog(route.segment_id);
        if (!isSharedPagePoolSegmentKind(pool.kind)) return;
        admitFrontier();
        require(pool.state == SegmentState::SGS_SEALED && allocator_.isAllocated(current, pool.physical_slot) &&
            pool.generation == route.generation && route.page_index >= RSS_PAGE_POOL_FIRST_DATA_FRAME &&
            route.page_index <= RSS_PAGE_POOL_DATA_FRAME_COUNT, "native recovery found an invalid current pool route");
        const auto header = pools.readPoolHeader(pool);
        require(SharedPagePoolFrameMap::isAllocated(header, uint8_t(route.page_index)),
            "native recovery found a current route hidden by the frame bitmap");
        const auto owner = pools.readPageOwner(pool, uint8_t(route.page_index));
        require(owner.page_id == route.page_id && owner.generation == route.generation,
            "native recovery found a mismatched current page identity");
    });
    // Validate free positions too: they may contain stale bytes, but no active
    // pin can select one. Finish this read-only pass before physical cleanup.
    catalog_.visit(current, [&](const CatalogEntry &pool) {
        if (!isSharedPagePoolSegmentKind(pool.kind) || pool.state != SegmentState::SGS_SEALED) return;
        admitFrontier();
        require(allocator_.isAllocated(current, pool.physical_slot), "native recovery found an unallocated sealed pool");
        const auto header = pools.readPoolHeader(pool);
        for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
            if (SharedPagePoolFrameMap::isAllocated(header, frame)) continue;
            std::optional<PageOwnerHeader> owner;
            try { owner = pools.readPageOwner(pool, frame); } catch (const FormatException &) { continue; }
            require(!visible(owner->page_id, pool.segment_id, frame, pool.generation),
                "native recovery found a pinned frame hidden by the pool bitmap");
        }
    });
    catalog_.visit(current, [&](const CatalogEntry &pool) {
        if (isSharedPagePoolSegmentKind(pool.kind) && pool.state == SegmentState::SGS_SEALED)
            static_cast<void>(pools.reconcilePoolFrames(pool, findPage, findCatalog, visible, true));
    });
    pages_.rebindSuperblock(current);
    native_pool_recovery_required_ = false;
}
} // namespace graph::storage
