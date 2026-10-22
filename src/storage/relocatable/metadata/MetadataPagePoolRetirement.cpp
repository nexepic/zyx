/**
 * @file MetadataPagePoolRetirement.cpp
 * @brief Snapshot-delayed, allocation-free handoff of changed metadata frames.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <algorithm>
#include <limits>

namespace graph::storage {
namespace {
uint64_t frameToken(PhysicalSlotId slot, uint8_t frame) {
    if (!slot.isValid() || frame < RSS_PAGE_POOL_FIRST_DATA_FRAME || frame > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
        slot.value() > (std::numeric_limits<uint64_t>::max() >> 5U))
        throw FormatException(FormatError::FERR_INVALID_STATE, "metadata retirement has an invalid physical token");
    return (slot.value() << 5U) | frame;
}
uint64_t frameToken(PhysicalPageRef reference) {
    if (!reference.isValid())
        throw FormatException(FormatError::FERR_INVALID_STATE, "metadata retirement has an invalid frame reference");
    return frameToken(reference.physical_slot, reference.frame_index);
}
uint64_t tokenSlot(uint64_t token) { return token >> 5U; }
size_t tokenIndex(uint64_t token) { return static_cast<size_t>((token & 31U) - RSS_PAGE_POOL_FIRST_DATA_FRAME); }
}

void MetadataPagePoolAllocator::commitCatalogPoolOwnership(PhysicalSlotId slot) noexcept {
    std::unique_lock lock(mutex_, std::defer_lock);
    try {
        lock.lock(); requireHealthyLocked();
        unlinkUnconfirmedOwnershipLocked(slot);
    } catch (...) {
        if (lock.owns_lock()) poisoned_ = true;
    }
}

void MetadataPagePoolAllocator::validateMetadataRetirement(const Superblock &current, const Superblock &candidate,
    std::span<const PhysicalPageRef> retired, bool complete) const {
    if (!complete) {
        debug::PerfTrace::addValue("native_metadata.retirement.incomplete_publications", 1);
        return;
    }
    std::lock_guard lock(mutex_); requireHealthyLocked();
    if (!retirement_base_ || *retirement_base_ != current)
        debug::PerfTrace::addValue("native_metadata.retirement.unproven_publications", 1);
    if (current.database_id != candidate.database_id || candidate.generation.value() != current.generation.value() + 1U)
        throw FormatException(FormatError::FERR_INVALID_STATE, "metadata retirement requires the next database generation");
    uint64_t previous = 0U;
    for (const auto reference : retired) {
        const auto token = frameToken(reference);
        const auto pool = pools_.find(reference.physical_slot.value());
        const auto index = tokenIndex(token);
        if (token <= previous || reference.generation > candidate.generation || pool == pools_.end() ||
            index >= RSS_PAGE_POOL_DATA_FRAME_COUNT ||
            (pool->second.header.allocated_frames & (1U << index)) == 0U ||
            pool->second.retired_frames[index].generation != 0U)
            throw FormatException(FormatError::FERR_INVALID_STATE, "metadata retirement references duplicate, free, or queued frames");
        previous = token;
    }
}

void MetadataPagePoolAllocator::queueMetadataRetirementLocked(PhysicalSlotId slot, uint8_t frame, Generation after) {
    const auto token = frameToken(slot, frame);
    auto &node = pools_.at(slot.value()).retired_frames[tokenIndex(token)];
    if (node.generation != 0U)
        throw FormatException(FormatError::FERR_INVALID_STATE, "metadata frame was retired twice");
    node.generation = after.value(); node.next = 0U;
    if (retired_metadata_tail_ != 0U)
        pools_.at(tokenSlot(retired_metadata_tail_)).retired_frames[tokenIndex(retired_metadata_tail_)].next = token;
    else retired_metadata_head_ = token;
    retired_metadata_tail_ = token; ++retired_metadata_count_;
}

void MetadataPagePoolAllocator::commitMetadataRetirement(const Superblock &current, const Superblock &candidate,
    std::span<const PhysicalPageRef> retired, bool complete) noexcept {
    std::unique_lock lock(mutex_, std::defer_lock);
    try {
        lock.lock(); requireHealthyLocked();
        if (!complete || !retirement_base_ || *retirement_base_ != current) {
            retirement_base_.reset();
            return;
        }
        // Nodes are embedded in already-existing pool state. Everything that can
        // allocate or reject a valid plan ran before the durable publication.
        for (const auto reference : retired)
            queueMetadataRetirementLocked(reference.physical_slot, reference.frame_index, candidate.generation);
        retirement_base_ = candidate;
    } catch (...) {
        // Never free on an uncertain handoff. Recovery reconstructs reachability.
        if (lock.owns_lock()) { poisoned_ = true; retirement_base_.reset(); }
    }
}

void MetadataPagePoolAllocator::resetMetadataRetirementLocked() {
    retired_metadata_head_ = 0U; retired_metadata_tail_ = 0U; retired_metadata_count_ = 0U;
    retirement_base_.reset();
    for (auto &[slot, pool] : pools_) {
        static_cast<void>(slot); pool.retired_frames.fill({});
        pool.retirement_dirty = false; pool.retirement_dirty_next = 0U;
    }
}

void MetadataPagePoolAllocator::reclaimRetiredMetadataLocked(const Superblock &current,
    std::span<const Superblock> snapshots) {
    uint64_t oldest = current.generation.value();
    for (const auto &snapshot : snapshots) oldest = std::min(oldest, snapshot.generation.value());
    uint64_t dirtyHead = 0U;
    size_t reclaimed = 0U;
    while (retired_metadata_head_ != 0U) {
        const auto token = retired_metadata_head_;
        auto &pool = pools_.at(tokenSlot(token));
        const auto index = tokenIndex(token);
        if (index >= RSS_PAGE_POOL_DATA_FRAME_COUNT)
            throw FormatException(FormatError::FERR_INVALID_STATE, "metadata retirement queue has an invalid frame index");
        auto &node = pool.retired_frames[index];
        if (node.generation == 0U ||
            (pool.header.allocated_frames & (1U << index)) == 0U)
            throw FormatException(FormatError::FERR_INVALID_STATE, "metadata retirement queue disagrees with allocation state");
        if (node.generation > oldest) break;
        retired_metadata_head_ = node.next;
        if (retired_metadata_head_ == 0U) retired_metadata_tail_ = 0U;
        node = {}; --retired_metadata_count_;
        pool.header.allocated_frames &= ~(1U << index);
        if (!pool.retirement_dirty) {
            pool.retirement_dirty = true; pool.retirement_dirty_next = dirtyHead;
            dirtyHead = tokenSlot(token);
        }
        ++reclaimed;
    }
    while (dirtyHead != 0U) {
        auto &pool = pools_.at(dirtyHead);
        const auto next = pool.retirement_dirty_next;
        // As with the previous reclaimer, the next mandatory data fence persists
        // these control records. A crash rebuilds the bitmap from published roots.
        writePoolLocked(pool);
        if (pool.header.allocated_frames == 0U) snapshot_protected_pool_slots_.erase(dirtyHead);
        pool.retirement_dirty = false; pool.retirement_dirty_next = 0U;
        refreshPoolAvailabilityLocked(PhysicalSlotId(dirtyHead));
        dirtyHead = next;
    }
    if (reclaimed != 0U) allocation_cursor_slot_.reset();
    allocations_since_reconciliation_ = 0U;
    debug::PerfTrace::addValue("native_metadata.reclaim.incremental_frames", static_cast<int64_t>(reclaimed));
    debug::PerfTrace::addValue("native_metadata.reclaim.retained_frames", static_cast<int64_t>(retired_metadata_count_));
}
} // namespace graph::storage
