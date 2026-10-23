/**
 * @file HybridPropertyColdRowBindings.hpp
 * @brief Canonical bounded owner-to-cold-block locations, without storage authority.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include "graph/storage/relocatable/property/HybridPropertyRecords.hpp"

namespace graph::storage {
enum class HybridColdRowLocationEncoding : uint8_t { HCRLE_DENSE = 0, HCRLE_GROUPED = 1 };
class HybridPropertyColdRowBindings final {
    struct Group { SlottedSlotReference reference{}; uint64_t owners = 0; };
    [[nodiscard]] size_t collect(std::array<Group, 64> &groups) const {
        size_t count = 0;
        for (unsigned slot = 0; slot < 64; ++slot) {
            if (!(owners & (UINT64_C(1) << slot))) {
                hybrid_property_detail::require(roots[slot] == SlottedSlotReference{}, "unused cold ROW location is nonzero");
                continue;
            }
            hybrid_property_detail::require(roots[slot].isValid(), "cold ROW location is unbound");
            size_t selected = 0;
            while (selected < count && groups[selected].reference != roots[slot]) ++selected;
            if (selected == count) groups[count++].reference = roots[slot];
            groups[selected].owners |= UINT64_C(1) << slot;
        }
        return count;
    }
    static void writeReference(std::span<std::byte> bytes, size_t at, const SlottedSlotReference &ref) {
        LittleEndianCodec::writeU64(bytes, at, ref.page_id.value());
        LittleEndianCodec::writeU16(bytes, at + 8, ref.slot_id);
        LittleEndianCodec::writeU32(bytes, at + 12, ref.generation);
    }
    [[nodiscard]] static SlottedSlotReference readReference(std::span<const std::byte> bytes, size_t at) {
        hybrid_property_detail::zero(bytes, at + 10, at + 12);
        const SlottedSlotReference ref{PageId(LittleEndianCodec::readU64(bytes, at)), LittleEndianCodec::readU16(bytes, at + 8),
            LittleEndianCodec::readU32(bytes, at + 12)};
        hybrid_property_detail::require(ref.isValid(), "encoded cold ROW location is invalid"); return ref;
    }
public:
    static constexpr size_t MAX_BYTES = 64U * 16U;
    uint64_t owners = 0;
    std::array<SlottedSlotReference, 64> roots{};
    constexpr bool operator==(const HybridPropertyColdRowBindings &) const = default;
    struct Layout {
        HybridColdRowLocationEncoding encoding = HybridColdRowLocationEncoding::HCRLE_DENSE;
        uint16_t groups = 0;
        size_t bytes = 0;
    };
    [[nodiscard]] Layout layout() const {
        std::array<Group, 64> groups{}; const auto count = collect(groups);
        const auto denseBytes = size_t(std::popcount(owners)) * 16U;
        if (count * 24U < denseBytes) return {HybridColdRowLocationEncoding::HCRLE_GROUPED, uint16_t(count), count * 24U};
        return {HybridColdRowLocationEncoding::HCRLE_DENSE, 0, denseBytes};
    }
    void encode(std::span<std::byte> bytes) const {
        const auto selected = layout();
        hybrid_property_detail::require(bytes.size() == selected.bytes, "cold ROW location buffer has the wrong width");
        std::fill(bytes.begin(), bytes.end(), std::byte{});
        size_t at = 0;
        if (selected.encoding == HybridColdRowLocationEncoding::HCRLE_DENSE) {
            for (unsigned slot = 0; slot < 64; ++slot) if (owners & (UINT64_C(1) << slot)) {
                writeReference(bytes, at, roots[slot]); at += 16U;
            }
        } else {
            std::array<Group, 64> groups{}; const auto count = collect(groups);
            for (size_t i = 0; i < count; ++i, at += 24U) {
                LittleEndianCodec::writeU64(bytes, at, groups[i].owners);
                writeReference(bytes, at + 8U, groups[i].reference);
            }
        }
    }
    [[nodiscard]] static HybridPropertyColdRowBindings decode(uint64_t owners,
        HybridColdRowLocationEncoding encoding, uint16_t groups, std::span<const std::byte> bytes) {
        using hybrid_property_detail::require;
        require(encoding == HybridColdRowLocationEncoding::HCRLE_DENSE || encoding == HybridColdRowLocationEncoding::HCRLE_GROUPED,
            "unknown cold ROW location encoding");
        HybridPropertyColdRowBindings result; result.owners = owners; size_t at = 0;
        if (encoding == HybridColdRowLocationEncoding::HCRLE_DENSE) {
            require(!groups && bytes.size() == size_t(std::popcount(owners)) * 16U, "dense cold ROW location width is invalid");
            for (unsigned slot = 0; slot < 64; ++slot) if (owners & (UINT64_C(1) << slot)) {
                result.roots[slot] = readReference(bytes, at); at += 16U;
            }
        } else {
            require(groups && groups <= 64U && groups <= std::popcount(owners) && bytes.size() == size_t(groups) * 24U,
                "grouped cold ROW location width is invalid");
            uint64_t seen = 0; unsigned previous = 0;
            for (unsigned i = 0; i < groups; ++i, at += 24U) {
                const auto mask = LittleEndianCodec::readU64(bytes, at);
                require(mask && !(mask & ~owners) && !(mask & seen), "cold ROW location masks overlap or escape their owners");
                const unsigned first = unsigned(std::countr_zero(mask));
                require(!i || first > previous, "cold ROW location groups are not canonical"); previous = first;
                const auto ref = readReference(bytes, at + 8U);
                for (uint64_t pending = mask; pending; pending &= pending - 1U)
                    result.roots[unsigned(std::countr_zero(pending))] = ref;
                seen |= mask;
            }
            require(seen == owners, "cold ROW locations do not cover their owner mask");
        }
        const auto canonical = result.layout();
        require(canonical.encoding == encoding && canonical.groups == groups && canonical.bytes == bytes.size(),
            "cold ROW locations do not use the unique minimum-width representation");
        return result;
    }
};
}
