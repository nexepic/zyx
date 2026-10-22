/**
 * @file MetadataPagePoolAvailability.cpp
 * @brief Intrusive index of allocatable metadata capacity.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include <bit>

namespace graph::storage {
void MetadataPagePoolAllocator::unlinkPoolAvailabilityLocked(PhysicalSlotId slot) {
    auto &pool = pools_.at(slot.value());
    if (pool.available_frames == 0U) return;
    if (pool.available_previous != 0U)
        pools_.at(pool.available_previous).available_next = pool.available_next;
    else available_pool_head_ = pool.available_next;
    if (pool.available_next != 0U)
        pools_.at(pool.available_next).available_previous = pool.available_previous;
    available_metadata_frames_ -= pool.available_frames;
    pool.available_frames = 0U; pool.available_previous = 0U; pool.available_next = 0U;
}
void MetadataPagePoolAllocator::refreshPoolAvailabilityLocked(PhysicalSlotId slot) {
    auto &pool = pools_.at(slot.value());
    const bool excluded = snapshot_protected_pool_slots_.contains(slot.value()) ||
        (emergency_reserve_.isValid() && emergency_reserve_.physical_slot == slot);
    const auto free = static_cast<uint8_t>(excluded ? 0U :
        RSS_PAGE_POOL_DATA_FRAME_COUNT - std::popcount(pool.header.allocated_frames));
    if (free == 0U) { unlinkPoolAvailabilityLocked(slot); return; }
    if (pool.available_frames == 0U) {
        pool.available_next = available_pool_head_; pool.available_previous = 0U;
        if (available_pool_head_ != 0U) pools_.at(available_pool_head_).available_previous = slot.value();
        available_pool_head_ = slot.value();
    }
    available_metadata_frames_ -= pool.available_frames;
    pool.available_frames = free;
    available_metadata_frames_ += free;
}
void MetadataPagePoolAllocator::rebuildPoolAvailabilityLocked() {
    available_pool_head_ = 0U; available_metadata_frames_ = 0U;
    for (auto &[slot, pool] : pools_) {
        static_cast<void>(slot);
        pool.available_frames = 0U; pool.available_previous = 0U; pool.available_next = 0U;
    }
    // Reverse insertion preserves physical order after recovery.
    for (auto it = pools_.rbegin(); it != pools_.rend(); ++it)
        refreshPoolAvailabilityLocked(PhysicalSlotId(it->first));
}
} // namespace graph::storage
