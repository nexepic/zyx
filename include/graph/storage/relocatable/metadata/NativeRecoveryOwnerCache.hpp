/** Bounded, per-recovery reuse of already authenticated page owner headers. */
#pragma once
#include <optional>
#include <new>
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/utils/OwnedStdVector.hpp"
#include "graph/query/execution/ResourceCoordinator.hpp"

namespace graph::storage::detail {
class NativeRecoveryOwnerCache final {
    struct Entry { SegmentId segment; Generation generation; PhysicalSlotId slot; uint8_t frame; PageOwnerHeader owner; };
    std::optional<utils::OwnedStdVector<Entry>> entries_;
public:
    static constexpr size_t capacity = 1024U;
    NativeRecoveryOwnerCache() {
        try { entries_.emplace(); entries_->reserve(capacity); }
        catch (const query::execution::detail::ResourceAdmissionDenied &) { entries_.reset(); }
        catch (const std::bad_alloc &) { entries_.reset(); }
    }
    void remember(SegmentId segment, Generation generation, PhysicalSlotId slot, uint8_t frame, PageOwnerHeader owner) {
        if (entries_ && entries_->size() < capacity) entries_->push_back({segment, generation, slot, frame, owner});
    }
    [[nodiscard]] std::optional<PageOwnerHeader> find(SegmentId segment, Generation generation, PhysicalSlotId slot, uint8_t frame) const {
        if (!entries_) return std::nullopt;
        for (const auto &entry : *entries_)
            if (entry.segment == segment && entry.generation == generation && entry.slot == slot && entry.frame == frame)
                return entry.owner;
        return std::nullopt;
    }
};
} // namespace graph::storage::detail
