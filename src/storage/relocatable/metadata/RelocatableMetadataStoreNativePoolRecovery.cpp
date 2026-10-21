/**
 * @file RelocatableMetadataStoreNativePoolRecovery.cpp
 * @brief Recover unpublished native COW frames before a subsequent publication.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include <array>
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
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
    MetadataPageReadContext::Scope recoveryReads(pin.readContext(), *io_);
    const auto fileBytes = io_->fileSize();
    SharedPagePoolAllocator pools(io_, *this, pin);
    std::optional<utils::OwnedHeapReservation> frontier;
    const auto admitFrontier = [&] {
        if (!frontier) if (const auto memory = utils::OwnedAllocationScope::owner()) frontier.emplace(memory, 32U * 1024U);
    };
    const auto findPage = [&](PageId id) { return locations_.findPageOptional(current, id); };
    const auto findCatalog = [&](SegmentId id) { return catalog_.find(current, id); };
    // Only the two read-only validation passes use this bounded cache. Their
    // Catalog and control records cannot change under this writer. Cleanup below
    // rereads each pool's actual control record before any physical mutation.
    struct PoolControl { SegmentId id; bool shared = false, header_loaded = false; SharedPagePoolHeader header; };
    std::array<PoolControl, 16> controls{}; size_t nextControl = 0;
    const auto control = [&](const CatalogEntry &pool, bool withHeader = true) {
        for (auto &entry : controls) if (entry.id == pool.segment_id) {
            if (withHeader && entry.shared && !entry.header_loaded) { entry.header = pools.readPoolHeader(pool); entry.header_loaded = true; }
            return entry;
        }
        const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(current.data_region_offset, pool.physical_slot);
        std::array<std::byte, RSS_ENVELOPE_BYTES> bytes{};
        if (io_->readAtSequential(offset, bytes.data(), bytes.size()) != bytes.size())
            throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL, "native recovery found a truncated segment envelope");
        const auto envelope = SegmentEnvelopeCodec::decode(bytes);
        require(envelope.state == pool.state && envelope.kind == pool.kind && envelope.segment_id == pool.segment_id &&
            envelope.owner_id == pool.owner_id && envelope.generation == pool.generation && envelope.committed_lsn == pool.committed_lsn,
            "native recovery segment envelope disagrees with its Catalog identity");
        // The physical allocator also supports opaque whole-segment objects
        // with these kinds. Their64-byte envelope is not a shared-frame bitmap.
        require(envelope.header_bytes == RSS_ENVELOPE_BYTES || envelope.header_bytes == RSS_PAGE_SIZE,
            "native recovery found an unregistered shared-kind segment layout");
        const bool shared = envelope.header_bytes == RSS_PAGE_SIZE;
        PoolControl result{pool.segment_id, shared, !shared || withHeader,
            shared && withHeader ? pools.readPoolHeader(pool) : SharedPagePoolHeader{}};
        controls[nextControl] = result; nextControl = (nextControl + 1U) % controls.size(); return result;
    };
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
        require(pool.state == SegmentState::SGS_SEALED && allocator_.isAllocated(current, pool.physical_slot) &&
            pool.generation == route.generation, "native recovery found an invalid current pool route");
        const auto info = control(pool);
        if (!info.shared) { require(route.page_index == 0, "native recovery found a framed route into an opaque segment"); return; }
        admitFrontier();
        require(route.page_index >= RSS_PAGE_POOL_FIRST_DATA_FRAME && route.page_index <= RSS_PAGE_POOL_DATA_FRAME_COUNT,
            "native recovery found an invalid current frame index");
        const auto header = info.header;
        require(SharedPagePoolFrameMap::isAllocated(header, uint8_t(route.page_index)),
            "native recovery found a current route hidden by the frame bitmap");
        const auto owner = pools.readRecoveryPageOwner(pool, uint8_t(route.page_index), fileBytes);
        require(owner.page_id == route.page_id && owner.generation == route.generation,
            "native recovery found a mismatched current page identity");
    });
    // Validate free positions too: they may contain stale bytes, but no active
    // pin can select one. Finish this read-only pass before physical cleanup.
    catalog_.visit(current, [&](const CatalogEntry &pool) {
        if (!isSharedPagePoolSegmentKind(pool.kind) || pool.state != SegmentState::SGS_SEALED) return;
        const auto info = control(pool); if (!info.shared) return;
        admitFrontier();
        require(allocator_.isAllocated(current, pool.physical_slot), "native recovery found an unallocated sealed pool");
        const auto header = info.header;
        for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
            if (SharedPagePoolFrameMap::isAllocated(header, frame)) continue;
            std::optional<PageOwnerHeader> owner;
            try { owner = pools.readRecoveryPageOwner(pool, frame, fileBytes); } catch (const FormatException &) { continue; }
            require(!visible(owner->page_id, pool.segment_id, frame, pool.generation),
                "native recovery found a pinned frame hidden by the pool bitmap");
        }
    });
    catalog_.visit(current, [&](const CatalogEntry &pool) {
        if (isSharedPagePoolSegmentKind(pool.kind) && pool.state == SegmentState::SGS_SEALED && control(pool, false).shared)
            static_cast<void>(pools.reconcilePoolFrames(pool, findPage, findCatalog, visible, true));
    });
    pages_.rebindSuperblock(current);
    native_pool_recovery_required_ = false;
}
} // namespace graph::storage
