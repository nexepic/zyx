/**
 * @file HybridPropertyColdRowInspection.hpp
 * @brief Pinned projection of an authenticated cold canonical ROW root.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/storage/relocatable/property/HybridPropertyColdRowBlock.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"

namespace graph::storage {
// A synchronous source, not a cache or publication capability. The decoded
// block, pin, store and account must outlive this object. blockGeneration
// must be the decoded cold block generation, not a later group generation.
// Inline roots borrow the caller's one ROW body. Large roots retain only the
// existing authenticated manifest/directory and one existing range window.
class HybridPropertyColdRowInspection final {
    const MetadataSnapshotPin &snapshot_;
    Generation generation_;
    DatabaseId database_;
    HybridColdRowInput row_;
    EntityPropertyKey owner_;
    const LargeValueStore *values_;
    const CancellationCheckpoint *cancellation_;
    utils::OwnedHeapReservation proxy_credit_, reference_credit_, manifest_credit_, range_credit_, header_credit_, directory_credit_;
    std::optional<EntityPropertyLargeValueReference> reference_;
    LargeValueManifest manifest_;
    LargeValueStore::RangeReadCache range_;
    std::vector<std::byte> header_;
    std::vector<uint32_t> inline_tokens_;
    std::vector<EntityPropertyLargeValueEntryLocation> locations_;
public:
    HybridPropertyColdRowInspection(const MetadataSnapshotPin &snapshot, HybridColdRowInput row,
        EntityPropertyKey owner, Generation blockGeneration, std::span<const uint32_t> hotTokens,
        const std::shared_ptr<std::pmr::memory_resource> &memory, const LargeValueStore *values = nullptr,
        const CancellationCheckpoint *cancellation = nullptr);
    HybridPropertyColdRowInspection(const HybridPropertyColdRowInspection &) = delete;
    HybridPropertyColdRowInspection &operator=(const HybridPropertyColdRowInspection &) = delete;
    [[nodiscard]] size_t count() const { return row_.large_value_reference ? locations_.size() : inline_tokens_.size(); }
    [[nodiscard]] uint32_t token(size_t ordinal) const;
    [[nodiscard]] const std::optional<EntityPropertyLargeValueReference> &reference() const & { return reference_; }
    const std::optional<EntityPropertyLargeValueReference> &reference() const && = delete;
    // Selected canonical value bytes exclude the key token, exactly as the
    // ordinary ROW codec's encoded-selection sink. No borrowed range escapes
    // the callback; exceptions/cancellation never replay already emitted cells.
    void visitSelected(std::span<const uint32_t> tokens, const EntityPropertyCollectionRootCodec::EncodedSelectionSink &sink,
        const SelectedPropertyReadAdmission *admission = nullptr);
private:
    void check() const {
        hybrid_property_detail::require(snapshot_.isValid() && snapshot_.superblock().generation == generation_ &&
            snapshot_.superblock().database_id == database_, "cold ROW inspection lost its original live snapshot");
        if (cancellation_) cancellation_->check();
    }
    [[nodiscard]] std::vector<std::byte> readRange(uint64_t offset, uint64_t size);
};
}
