#pragma once
#include "graph/storage/relocatable/metadata/HybridPropertyPoolPlacement.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
namespace graph::storage {
// Laboratory-only access to the original coordinator. A supplied candidate
// source is unconditionally refused before its first physical write.
class HybridPropertyNativeWriteTestAccess {
public:
    // The routing fixture advances Bootstrap directly to set its compact flag.
    // Consume that external-handle rebase before measuring field preparation;
    // it can legitimately repair allocator bookkeeping during writer admission.
    static void synchronizeFixture(RelocatableMetadataStore &metadata) {
        auto writer = metadata.beginWrite(true);
    }
    static void prepare(RelocatableMetadataStore &metadata, const MetadataSnapshotPin &source,
        HybridPropertyReadContext &context,
        const NativeHybridPropertyColumnWrite &request) {
        const auto check = [&] { if (request.cancellation) request.cancellation->check(); };
        utils::OwnedHeapReservation sourceStoreCredit(request.memory, 16U * 1024U);
        SlottedPagePool pages(metadata.io_, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore trees(pages, metadata);
        LargeValueStore values(metadata.io_, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        EntityPageStore entities(metadata.io_, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        EntityLabelSetStore labels(metadata.io_, metadata, pages, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        auto writer = metadata.beginWrite(true);
        MetadataWriteBatch batch(metadata.metadata_page_pool_, source.superblock(),
            LogSequenceNumber(source.superblock().checkpoint_lsn.value() + 1), request.memory);
        HybridPropertyPoolPlacement placement(metadata, batch, writer, source, request.first_slot,
            request.last_slot, request.memory, check);
        static_cast<void>(metadata.prepareHybridPropertyColumns(batch, writer, source, request, request.replacements,
            pages, trees, values, entities, labels, placement, nullptr, nullptr, {}, &context));
    }
    static void prepareInverse(RelocatableMetadataStore &metadata, const MetadataSnapshotPin &source,
        HybridPropertyReadContext &context, const NativeGraphHybridPropertyConversion &conversion) {
        utils::OwnedHeapReservation sourceStoreCredit(conversion.memory, 16U * 1024U);
        SlottedPagePool pages(metadata.io_, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore trees(pages, metadata);
        LargeValueStore values(metadata.io_, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        EntityPageStore entities(metadata.io_, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        auto writer = metadata.beginWrite(true);
        MetadataWriteBatch batch(metadata.metadata_page_pool_, source.superblock(),
            LogSequenceNumber(source.superblock().checkpoint_lsn.value() + 1), conversion.memory);
        static_cast<void>(metadata.publishHybridPropertyRows(batch, writer, source, conversion, entities,
            pages, trees, values, {}, &context));
    }
};
}
