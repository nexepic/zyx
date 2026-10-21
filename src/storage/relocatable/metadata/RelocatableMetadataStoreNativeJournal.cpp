/**
 * @file RelocatableMetadataStoreNativeJournal.cpp
 * @brief Restricted snapshot-safe State COW for the native maintenance journal.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueMaintenanceJournal.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPage.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPoolRoutes.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDescriptor.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace graph::storage {
std::optional<StateLocation> RelocatableMetadataStore::upsertNativeMaintenanceJournal(
    const LargeValueMaintenanceJournal &journal, const LargeValueMaintenanceJournalStoreOptions &placement,
    const MetadataPublishOptions &options, const NativeJournalProgressEvidence *evidence) {
    if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED) return {};
    {
        const auto probe = openSnapshot();
        if (!probe.superblock().hybrid_property_descriptor_root.isValid()) return {};
        const auto descriptor = pages_.readHybridPropertyDescriptor(
            probe.superblock().hybrid_property_descriptor_root, probe.superblock()).descriptor;
        if (std::ranges::none_of(descriptor.roots, [](const auto &root) { return root.page_id.isValid(); })) return {};
    }
    const auto require = [](bool valid, const char *message) { hybrid_property_detail::require(valid, message); };
    const auto memory = utils::OwnedAllocationScope::owner();
    require(bool(memory) && placement.state_id && placement.state_record_options.page_owner_id &&
        placement.state_record_options.first_slot.isValid() &&
        placement.state_record_options.last_slot >= placement.state_record_options.first_slot,
        "native maintenance journal requires its admitted owner and bounded State placement");
    require(evidence || (journal.phase == LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS &&
        journal.candidate_value_ids.empty() && journal.reachable_value_ids.empty() &&
        !journal.entity_property_after_directory_id && !journal.state_after_id),
        "native owner progress requires an unmodified reclaimer advancement");
    // Standard codec, reconciliation (at most31 frames), prepared-payload and
    // callback frontiers coexist here; retained page/metadata bytes are separate.
    utils::OwnedHeapReservation frontier(memory, 32U * 1024U);
    StateRecord record; record.state_id = placement.state_id;
    record.key = std::string(RSS_NATIVE_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY);
    record.storage_kind = StateRecordStorageKind::SRS_INLINE;
    record.inline_bytes = LargeValueMaintenanceJournalCodec::encode(journal);
    const auto encoded = StateRecordCodec::encode(record);
    require(encoded.size() <= RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - RSS_SLOTTED_PAGE_SLOT_BYTES,
        "native maintenance journal exceeds its fixed slotted record capacity");
    SharedPagePoolAllocator sourcePools(io_, *this, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    auto writer = beginWrite(true); const auto current = bootstrap_.recover();
    require(current.hybrid_property_descriptor_root.isValid() &&
        current.generation.value() < (std::numeric_limits<uint64_t>::max)() &&
        journal.scan_generation.value() == current.generation.value() + 1U,
        "native maintenance journal evidence is stale or not bound to its next metadata generation");
    if (evidence) {
        require(evidence->source_.database_id == current.database_id && evidence->source_.generation == current.generation &&
            evidence->source_.hybrid_property_descriptor_root == current.hybrid_property_descriptor_root,
            "native journal advancement no longer matches its source database/generation/descriptor");
        auto expected = evidence->result_; expected.scan_generation = journal.scan_generation;
        require(expected == journal && evidence->result_.scan_generation == current.generation,
            "native journal advancement payload was modified after scanning");
    }
    require(current.checkpoint_lsn.value() < (std::numeric_limits<uint64_t>::max)(),
        "native maintenance journal cannot advance its checkpoint LSN");
    const auto snapshot = snapshots_.pinCurrentUnderWrite(writer, current);
    MetadataWriteBatch batch(metadata_page_pool_, current, LogSequenceNumber(current.checkpoint_lsn.value() + 1U), memory);
    const auto existing = locations_.findStateOptional(current, placement.state_id);
    std::optional<CatalogEntry> sourcePool;
    std::optional<PageLocation> sourceLocation;
    StateLocation state;
    auto page = [&] {
        if (existing) {
            sourceLocation = locations_.findPage(current, existing->page_id);
            sourcePool = catalog_.find(current, sourceLocation->segment_id);
            require(sourcePool->kind == SegmentKind::SGK_STATE_PAGE_POOL &&
                sourcePool->owner_id == placement.state_record_options.page_owner_id &&
                sourcePool->state == SegmentState::SGS_SEALED && allocator_.isAllocated(current, sourcePool->physical_slot),
                "native maintenance journal refuses a foreign State pool owner");
            const auto contents = sourcePools.read(snapshot, existing->page_id);
            require(contents.owner.kind == PageOwnerKind::POK_STATE,
                "native maintenance journal requires a typed State page");
            auto image = SlottedPage::open(contents.owner, contents.payload, memory);
            require(image.stats().contiguous_free_bytes == sourceLocation->free_bytes,
                "native maintenance journal found a stale State free-byte hint");
            const SlottedSlotReference slot{existing->page_id, existing->slot_id, existing->slot_generation};
            const auto bytes = image.readOwned(slot, memory);
            const auto prior = StateRecordCodec::decode(bytes);
            require(prior.state_id == record.state_id && (prior.key == record.key || prior.key == RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY) &&
                prior.storage_kind == StateRecordStorageKind::SRS_INLINE && !prior.large_value_id.isValid(),
                "native maintenance journal refuses an unrelated State identity");
            static_cast<void>(LargeValueMaintenanceJournalCodec::decode(prior.inline_bytes));
            require(encoded.size() <= bytes.size(), "native maintenance journal cannot grow its stable slot");
            image.overwrite(slot, encoded); state = *existing; return image;
        }
        const auto id = batch.allocateLogicalPageId();
        const PageOwnerHeader owner{id, placement.state_record_options.page_owner_id, PageOwnerKind::POK_STATE, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1), batch.checkpointLsn()};
        auto image = SlottedPage::create(owner); const auto slot = image.insert(encoded);
        state = {placement.state_id, id, slot.slot_id, slot.generation}; return image;
    }();
    const auto findPage = [&](PageId id) { return locations_.findPageOptional(current, id); };
    const auto findCatalog = [&](SegmentId id) { return catalog_.find(current, id); };
    const auto visible = [&](PageId id, SegmentId segment, uint16_t frame, Generation generation) {
        return snapshots_.anyActiveSnapshot(writer, [&](const Superblock &pin) {
            const auto location = locations_.findPageOptional(pin, id);
            return location && location->segment_id == segment && location->page_index == frame && location->generation == generation;
        });
    };
    if (sourcePool) {
        const auto header = SharedPagePoolAllocator::readPoolHeader(*io_, current.data_region_offset, *sourcePool);
        // Check even bitmap-free positions: a correctly checksummed control
        // record must never hide a current or old-pin-visible physical frame.
        for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
            const bool allocated = SharedPagePoolFrameMap::isAllocated(header, frame);
            std::optional<PageOwnerHeader> owner;
            try { owner = sourcePools.readPageOwner(*sourcePool, frame); }
            catch (const FormatException &) { if (allocated) throw; continue; }
            if (allocated) require(owner->kind == PageOwnerKind::POK_STATE && owner->owner_id == sourcePool->owner_id &&
                owner->generation == sourcePool->generation, "native journal reconciliation found a foreign frame");
            const auto route = findPage(owner->page_id);
            const bool currentFrame = route && route->segment_id == sourcePool->segment_id && route->page_index == frame &&
                route->generation == sourcePool->generation;
            require(allocated || (!currentFrame && !visible(owner->page_id, sourcePool->segment_id, frame, sourcePool->generation)),
                "native journal pool bitmap hides a current or pinned frame");
        }
        // Physical-only recovery is restricted to the already authenticated
        // journal pool, under this writer and every active physical route.
        static_cast<void>(sourcePools.reconcilePoolFrames(*sourcePool, findPage, findCatalog, visible));
    }
    bool newlyReserved = !sourcePool || !SharedPagePoolAllocator::availableFrameCountUnderWrite(*io_, current.data_region_offset, *sourcePool);
    CatalogEntry destination;
    if (!newlyReserved) destination = *sourcePool;
    else {
        std::optional<uint64_t> next = placement.state_record_options.first_slot.value();
        while (next) {
            const auto slot = allocator_.findFirstFree(current, PhysicalSlotId(*next), placement.state_record_options.last_slot);
            if (!slot) break;
            next = *slot == placement.state_record_options.last_slot ? std::nullopt : std::optional(slot->value() + 1U);
            if (batch.isDataSlotReserved(*slot) || batch.isMetadataPoolSlot(*slot)) continue;
            destination = {batch.allocateLogicalSegmentId(), *slot, SegmentKind::SGK_STATE_PAGE_POOL,
                placement.state_record_options.page_owner_id, Generation(1), SegmentState::SGS_SEALED, batch.checkpointLsn()};
            batch.reserveDataSlot(*slot); break;
        }
        require(destination.segment_id.isValid(), "native maintenance journal has no free destination pool");
    }
    const PageOwnerHeader owner{state.page_id, destination.owner_id, PageOwnerKind::POK_STATE, 0,
        RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, destination.generation, batch.checkpointLsn()};
    page.rebindOwner(owner); const auto bytes = page.serializeOwned(memory);
    const std::array prepared{SharedPagePoolPreparedAppend{owner,
        std::vector<std::byte>(bytes.begin(), bytes.end()), page.stats().contiguous_free_bytes}};
    std::array locations{PageLocation{state.page_id, destination.segment_id, 0, destination.generation, page.stats().contiguous_free_bytes}};
    if (newlyReserved) {
        SharedPagePoolAllocator::prepareNewPoolPagesUnderWrite(*io_, current.data_region_offset, destination, prepared, locations, batch.checkpointLsn());
        allocator_.prepareSetAllocated(batch, destination.physical_slot, true);
        catalog_.prepareUpsert(batch, std::span(&destination, 1));
    } else SharedPagePoolAllocator::prepareAppendPagesUnderWrite(*io_, current.data_region_offset, destination, prepared, locations, batch.checkpointLsn());
    locations_.prepareReplacePages(batch, locations, {});
    locations_.prepareReplaceStates(batch, std::span(&state, 1), {});
    if (sourcePool && sourcePool->segment_id != destination.segment_id) {
        bool detached = true;
        const auto header = SharedPagePoolAllocator::readPoolHeader(*io_, current.data_region_offset, *sourcePool);
        for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
            const auto currentOwner = hybrid_property_detail::currentPoolFrameOwner(*sourcePool, header, frame,
                [&](uint8_t index) { return sourcePools.readPageOwner(*sourcePool, index); }, findPage);
            if (currentOwner) detached = detached && currentOwner->page_id == state.page_id;
        }
        if (detached) {
            auto retired = *sourcePool; retired.state = SegmentState::SGS_RETIRED;
            const RetireQueueEntry retirement{retired.physical_slot, retired.segment_id, batch.generation(), RetireQueueDisposition::RQD_RETIRED_SEGMENT};
            catalog_.prepareUpsert(batch, std::span(&retired, 1)); retirement_.prepareQueueInsertions(batch, std::span(&retirement, 1));
        }
    }
    // Preserve the descriptor/semantic fence and all graph/owner namespaces.
    batch.hybrid_property_publication_ = true; publishBatch(batch, writer, options); return state;
}
} // namespace graph::storage
