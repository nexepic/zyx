/**
 * @file NativeStorageAllocationCensus.hpp
 * @brief Read-only whole-pool allocation evidence for explicit layout experiments.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <array>
#include <optional>
#include "graph/storage/relocatable/metadata/MetadataSnapshotRegistry.hpp"
#include "graph/storage/CancellationCheckpoint.hpp"

namespace graph::storage {
class RelocatableMetadataStore;
class StorageIO;
class MetadataWriteBatch;
enum class NativeStorageAllocationKind : uint8_t {
    NSAK_ENTITY, NSAK_PROPERTY, NSAK_HYBRID_MAP, NSAK_QUERY_INDEX,
    NSAK_LARGE_VALUE, NSAK_VECTOR, NSAK_STATE, NSAK_METADATA, NSAK_OTHER, NSAK_COUNT
};
struct NativeStorageAllocationCensusOptions {
    size_t maximum_catalog_entries = 4096, maximum_retirement_records = 4096;
    size_t maximum_allocated_slots = 8192, maximum_allocator_metadata_pages = 256;
    const CancellationCheckpoint *cancellation = nullptr;
};
struct NativeStorageAllocationFootprint {
    DatabaseId database_id{}; Generation generation{};
    std::array<uint64_t, size_t(NativeStorageAllocationKind::NSAK_COUNT)> sealed_pool_bytes{};
    uint64_t retired_catalog_bytes = 0, relocated_source_bytes = 0, emergency_reserve_bytes = 0;
    uint64_t unattributed_allocated_bytes = 0, bootstrap_bytes = 0, file_bytes = 0;
    uint64_t allocated_slot_bytes = 0, catalog_entries = 0, retirement_records = 0, allocator_metadata_pages = 0;
    // Whole sealed pools include unused frames, old versions within a live
    // pool, all metadata framing/routes and descriptor frames exactly once.
    [[nodiscard]] uint64_t steadyAllocatedBytes() const;
    [[nodiscard]] uint64_t totalAllocatedBytes() const;
    [[nodiscard]] bool fullyAttributed() const { return !unattributed_allocated_bytes; }
};
// An explicit bounded census, not a foreground small-write search or a future
// allocation estimator. No payload integrity/production-admission capability
// is granted. Unknown allocations or incomplete scans cannot authorize a layout.
class NativeStorageAllocationCensus final {
public:
    NativeStorageAllocationCensus(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
        std::shared_ptr<std::pmr::memory_resource> workspaceOwner);
    [[nodiscard]] std::optional<NativeStorageAllocationFootprint> inspect(const MetadataSnapshotPin &snapshot,
        const NativeStorageAllocationCensusOptions &options = {}) const;
private:
    friend class RelocatableMetadataStore;
    [[nodiscard]] std::optional<NativeStorageAllocationFootprint> inspectCurrentUnderWrite(
        const MetadataWriteGuard &writer, const MetadataSnapshotPin &snapshot, const NativeStorageAllocationCensusOptions &options) const;
    [[nodiscard]] std::optional<NativeStorageAllocationFootprint> inspectPreparedCandidate(
        const MetadataWriteGuard &writer, const MetadataWriteBatch &batch, const NativeStorageAllocationCensusOptions &options) const;
    [[nodiscard]] std::optional<NativeStorageAllocationFootprint> inspectImpl(const Superblock &observed,
        const Superblock &durable, const NativeStorageAllocationCensusOptions &options) const;
    std::shared_ptr<StorageIO> io_;
    RelocatableMetadataStore &metadata_;
    std::shared_ptr<std::pmr::memory_resource> workspace_owner_;
};
} // namespace graph::storage
