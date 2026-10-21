/**
 * @file HybridPropertyPoolRoutes.hpp
 * @brief Bounded reverse-route checks for controlled native pool maintenance.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <optional>
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/metadata/Catalog.hpp"
#include "graph/storage/relocatable/property/HybridPropertyRecords.hpp"

namespace graph::storage::hybrid_property_detail {
// Probe every physical frame, including free-bitmap positions: a valid owner
// that still has a current route must not disappear merely because its pool
// allocation bit was corrupted. Unallocated zero/truncated/stale bytes are not
// owners. This grants no global integrity or publication authority.
template<class ReadOwner, class FindCurrent>
[[nodiscard]] std::optional<PageOwnerHeader> currentPoolFrameOwner(const CatalogEntry &pool,
    const SharedPagePoolHeader &header, uint8_t frame, ReadOwner &&readOwner, FindCurrent &&findCurrent) {
    const bool allocated = SharedPagePoolFrameMap::isAllocated(header, frame);
    std::optional<PageOwnerHeader> owner;
    try { owner = readOwner(frame); }
    catch (const FormatException &) { if (allocated) throw; return {}; }
    const auto routed = findCurrent(owner->page_id);
    if (!routed || routed->segment_id != pool.segment_id) return {};
    require(routed->generation == pool.generation, "native pool has a current route with a foreign physical epoch");
    if (routed->page_index != frame) return {}; // old version of the same PageId in this pool
    require(allocated && owner->generation == pool.generation && owner->flags == 0,
        "native pool has a current unallocated, retired or foreign frame");
    return owner;
}
} // namespace graph::storage::hybrid_property_detail
