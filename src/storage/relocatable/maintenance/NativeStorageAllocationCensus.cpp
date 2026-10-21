/**
 * @file NativeStorageAllocationCensus.cpp
 * @brief Attribute every persistent allocation bit without payload-size guesses.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#include "graph/storage/relocatable/maintenance/NativeStorageAllocationCensus.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/storage/relocatable/property/HybridPropertyIndexIds.hpp"
#include "graph/storage/relocatable/property/LargeValue.hpp"
#include "graph/utils/OwnedVector.hpp"
#include <algorithm>
#include <limits>

namespace graph::storage {
namespace {
struct CensusLimitReached {};
uint64_t add(uint64_t left, uint64_t right) {
    if (left > UINT64_MAX - right) throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "native allocation census byte sum overflows");
    return left + right;
}
void require(bool valid, const char *message) {
    if (!valid) throw FormatException(FormatError::FERR_INVALID_STATE, message);
}
NativeStorageAllocationKind classify(const CatalogEntry &entry) {
    using enum NativeStorageAllocationKind;
    switch (entry.kind) {
        case SegmentKind::SGK_ENTITY_PAGE_POOL: return NSAK_ENTITY;
        case SegmentKind::SGK_PROPERTY_PAGE_POOL: return NSAK_PROPERTY;
        case SegmentKind::SGK_INDEX_PAGE_POOL: return isHybridPropertyIndexId(entry.owner_id) ? NSAK_HYBRID_MAP :
            entry.owner_id == RSS_LARGE_VALUE_CONTENT_INDEX_ID ? NSAK_LARGE_VALUE : NSAK_QUERY_INDEX;
        case SegmentKind::SGK_LARGE_VALUE_DATA_POOL: case SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL: return NSAK_LARGE_VALUE;
        case SegmentKind::SGK_METADATA_PAGE_POOL: return NSAK_METADATA;
        case SegmentKind::SGK_STATE_PAGE_POOL: return NSAK_STATE;
        case SegmentKind::SGK_VECTOR_RAW_POOL: case SegmentKind::SGK_VECTOR_PQ_POOL:
        case SegmentKind::SGK_VECTOR_ADJACENCY_POOL: case SegmentKind::SGK_VECTOR_MANIFEST_POOL:
        case SegmentKind::SGK_CODEBOOK_EXTENT: return NSAK_VECTOR;
        default: return NSAK_OTHER;
    }
}
}
uint64_t NativeStorageAllocationFootprint::steadyAllocatedBytes() const {
    uint64_t total = add(bootstrap_bytes, emergency_reserve_bytes);
    for (const auto bytes : sealed_pool_bytes) total = add(total, bytes);
    return total;
}
uint64_t NativeStorageAllocationFootprint::totalAllocatedBytes() const { return add(bootstrap_bytes, allocated_slot_bytes); }
NativeStorageAllocationCensus::NativeStorageAllocationCensus(std::shared_ptr<StorageIO> io,
    RelocatableMetadataStore &metadata, std::shared_ptr<std::pmr::memory_resource> workspaceOwner)
    : io_(std::move(io)), metadata_(metadata), workspace_owner_(std::move(workspaceOwner)) {
    require(io_ && io_ == metadata_.io_ && workspace_owner_, "native allocation census requires its exact database IO and admitted workspace");
}
std::optional<NativeStorageAllocationFootprint> NativeStorageAllocationCensus::inspect(const MetadataSnapshotPin &snapshot,
    const NativeStorageAllocationCensusOptions &options) const {
    require(metadata_.ownsSnapshot(snapshot), "native allocation census requires this database's registered snapshot");
    const auto &current = snapshot.superblock();
    require(metadata_.openSnapshot().superblock() == current, "native allocation census requires a current stable generation");
    return inspectImpl(current, current, options);
}
std::optional<NativeStorageAllocationFootprint> NativeStorageAllocationCensus::inspectCurrentUnderWrite(
    const MetadataWriteGuard &writer, const MetadataSnapshotPin &snapshot, const NativeStorageAllocationCensusOptions &options) const {
    require(metadata_.ownsSnapshot(snapshot), "native allocation census requires its registered writer source");
    const auto proof = metadata_.snapshots_.pinCurrentUnderWrite(writer, snapshot.superblock());
    return inspectImpl(snapshot.superblock(), snapshot.superblock(), options);
}
std::optional<NativeStorageAllocationFootprint> NativeStorageAllocationCensus::inspectPreparedCandidate(
    const MetadataWriteGuard &writer, const MetadataWriteBatch &batch, const NativeStorageAllocationCensusOptions &options) const {
    require(batch.publication_prepared_ && batch.usesAllocator(metadata_.metadata_page_pool_),
        "native allocation preview requires this database's completely prepared publication");
    const auto proof = metadata_.snapshots_.pinCurrentUnderWrite(writer, batch.current());
    return inspectImpl(batch.candidate(), batch.current(), options);
}
std::optional<NativeStorageAllocationFootprint> NativeStorageAllocationCensus::inspectImpl(const Superblock &current,
    const Superblock &durable, const NativeStorageAllocationCensusOptions &options) const {
    require(metadata_.bootstrap_.recover() == durable, "native allocation census lost its durable source");
    const auto epoch = io_->dataMutationEpoch();
    if (epoch == UINT64_MAX) return {}; // a saturated epoch cannot prove physical stability
    utils::OwnedAllocationScope scope(workspace_owner_);
    NativeStorageAllocationFootprint result; result.database_id = current.database_id; result.generation = current.generation;
    result.bootstrap_bytes = current.data_region_offset; result.file_bytes = io_->fileSize();
    utils::OwnedVector<PhysicalSlotId> attributed{utils::OwnedPmrAllocator<PhysicalSlotId>(workspace_owner_)};
    const auto check = [&] { if (options.cancellation) options.cancellation->check(); };
    const auto own = [&](PhysicalSlotId slot) {
        require(slot.isValid() && metadata_.allocator_.isAllocated(current, slot), "native allocation census found an unallocated owned slot");
        attributed.push_back(slot);
    };
    try {
        metadata_.catalog_.visit(current, [&](const CatalogEntry &entry) {
            check(); if (result.catalog_entries++ == options.maximum_catalog_entries) throw CensusLimitReached{};
            const bool allocated = metadata_.allocator_.isAllocated(current, entry.physical_slot);
            if (entry.state == SegmentState::SGS_SEALED || entry.state == SegmentState::SGS_RETIRED) {
                require(allocated, "native allocation census found an unallocated sealed or retired pool");
                if (classify(entry) == NativeStorageAllocationKind::NSAK_OTHER) return;
                own(entry.physical_slot);
                if (entry.state == SegmentState::SGS_RETIRED) result.retired_catalog_bytes = add(result.retired_catalog_bytes, RSS_SEGMENT_SIZE);
                else result.sealed_pool_bytes[size_t(classify(entry))] = add(result.sealed_pool_bytes[size_t(classify(entry))], RSS_SEGMENT_SIZE);
            } else if (allocated) {
                // BUILDING/OBSOLETE/scratch state is not silently promoted to
                // steady graph storage; its bit remains unattributed below.
            }
        });
        metadata_.locations_.visitRetireQueue(current, [&](const RetireQueueEntry &entry) {
            check(); if (result.retirement_records++ == options.maximum_retirement_records) throw CensusLimitReached{};
            if (entry.disposition == RetireQueueDisposition::RQD_RETIRED_SEGMENT) {
                const auto pool = metadata_.catalog_.find(current, entry.segment_id);
                require(pool.physical_slot == entry.physical_slot && pool.state == SegmentState::SGS_RETIRED,
                    "native allocation census found an inconsistent retired Catalog slot");
            } else {
                require(entry.disposition == RetireQueueDisposition::RQD_RELOCATED_SOURCE,
                    "native allocation census found an unknown retirement disposition");
                own(entry.physical_slot); result.relocated_source_bytes = add(result.relocated_source_bytes, RSS_SEGMENT_SIZE);
            }
        });
        if (current.metadata_emergency_reserve.isValid()) {
            check(); own(current.metadata_emergency_reserve.physical_slot); result.emergency_reserve_bytes = RSS_SEGMENT_SIZE;
        }
        std::sort(attributed.begin(), attributed.end());
        require(std::adjacent_find(attributed.begin(), attributed.end()) == attributed.end(),
            "native allocation census found duplicate physical slot ownership");
        uint64_t slots = 0, matched = 0, previous = 0; size_t visited = 0;
        const bool exhausted = metadata_.allocator_.visitAllocatedSlotsUntil(current, options.maximum_allocator_metadata_pages,
            [&](PhysicalSlotId slot) {
                check(); if (slots == options.maximum_allocated_slots) return false;
                require(slot.value() > previous, "native allocation census found duplicate or unordered allocator slots"); previous = slot.value(); ++slots;
                if (std::binary_search(attributed.begin(), attributed.end(), slot)) ++matched;
                else result.unattributed_allocated_bytes = add(result.unattributed_allocated_bytes, RSS_SEGMENT_SIZE);
                result.allocated_slot_bytes = add(result.allocated_slot_bytes, RSS_SEGMENT_SIZE); return true;
            }, &visited);
        check(); if (!exhausted) return {};
        result.allocator_metadata_pages = visited;
        require(matched == attributed.size(), "native allocation census did not find every owned slot in its allocator walk");
        const auto accounted = add(add(result.steadyAllocatedBytes(), result.retired_catalog_bytes),
            add(result.relocated_source_bytes, result.unattributed_allocated_bytes));
        require(accounted == result.totalAllocatedBytes(), "native allocation census categories disagree with allocated slots");
        require(metadata_.bootstrap_.recover() == durable && io_->dataMutationEpoch() == epoch && io_->fileSize() == result.file_bytes,
            "native allocation census source changed during read-only observation");
        return result;
    } catch (const CensusLimitReached &) { return {}; }
}
} // namespace graph::storage
