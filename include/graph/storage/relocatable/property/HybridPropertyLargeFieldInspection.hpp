/**
 * @file HybridPropertyLargeFieldInspection.hpp
 * @brief Pinned bounded authentication and inspection of one native large field.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/core/PropertyDecodeAllocationScope.hpp"
#include "graph/storage/relocatable/property/HybridPropertyReadContext.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"

namespace graph::storage {
// The context, pin, field view and store must outlive this synchronous helper.
// The manifest and at most one authenticated chunk remain between range calls.
// Credits precede their backing objects so destruction cannot refund buffers
// still held by another member.
class HybridPropertyLargeFieldInspection final {
    const HybridPropertyReadContext &context_;
    const MetadataSnapshotPin &snapshot_;
    const HybridPropertyExtensionView &field_;
    const LargeValueStore &values_;
    utils::OwnedHeapReservation manifest_credit_, range_credit_, directory_credit_;
    LargeValueStore::RangeReadCache cache_;
    LargeValueManifest manifest_;
public:
    HybridPropertyLargeFieldInspection(const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot,
        const HybridPropertyExtensionView &field, const LargeValueStore &values,
        const std::shared_ptr<std::pmr::memory_resource> &memory)
        : context_(context), snapshot_(snapshot), field_(field), values_(values),
          manifest_credit_(memory), range_credit_(memory), directory_credit_(memory) {
        hybrid_property_detail::require(context.usesSnapshot(snapshot) &&
            field.mode() == HybridPropertyExtensionMode::HPEM_LARGE_REFERENCE && field.reference(),
            "hybrid large inspection requires its pinned field and store");
        const auto admit = [&](size_t bytes) { manifest_credit_.set(bytes); };
        const auto checkpoint = [&] { check(); };
        manifest_ = values_.verifyContent(snapshot_, *field_.reference(), std::ref(admit), std::ref(checkpoint));
    }
    HybridPropertyLargeFieldInspection(const HybridPropertyLargeFieldInspection &) = delete;
    HybridPropertyLargeFieldInspection &operator=(const HybridPropertyLargeFieldInspection &) = delete;
    [[nodiscard]] CanonicalPropertyValueCodec::DecodeShape inspect() {
        const auto read = [&](size_t offset, std::span<std::byte> bytes) { readRange(offset, bytes); };
        const auto admit = [&](size_t bytes) { directory_credit_.set(bytes); };
        const auto checkpoint = [&] { check(); };
        return field_.inspectLargePayloadRange(std::ref(read), std::ref(admit), std::ref(checkpoint));
    }
    [[nodiscard]] bool isNull() {
        const auto read = [&](size_t offset, std::span<std::byte> bytes) { readRange(offset, bytes); };
        const auto admit = [&](size_t bytes) { directory_credit_.set(bytes); };
        const auto checkpoint = [&] { check(); };
        return field_.largePayloadRangeIsNull(std::ref(read), std::ref(admit), std::ref(checkpoint));
    }
private:
    void check() const {
        context_.checkpoint();
        if (auto *scope = property_decode::AllocationScope::current()) scope->checkpoint();
    }
    void readRange(size_t offset, std::span<std::byte> target) {
        check();
        const auto admit = [&](size_t bytes) { range_credit_.set(bytes); };
        {
            const auto bytes = values_.readRangeFromManifestCached(snapshot_, manifest_, offset,
                target.size(), cache_, std::ref(admit));
            std::copy(bytes.begin(), bytes.end(), target.begin());
        }
        range_credit_.set(cache_.retainedBytes());
    }
};
} // namespace graph::storage
