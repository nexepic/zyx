/**
 * @file HybridPropertyDirectory.hpp
 * @brief Typed fixed directory fields for private hybrid-property preparation.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/storage/relocatable/allocation/SlottedPage.hpp"
#include "graph/storage/relocatable/index/FixedBTreeMap.hpp"
#include "graph/storage/relocatable/property/HybridPropertyRecords.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDescriptor.hpp"

namespace graph::storage {
// These maps install no root or reserved IndexId. The enclosing coordinator
// must bind each to the descriptor's correct Node/Edge namespace and authenticate
// the selected data page/record, not reinterpret an arbitrary generic tree.
using HybridPropertyOwnerMap = FixedBTreeMap<12, 4>;
using HybridPropertyFieldDirectoryMap = FixedBTreeMap<12, 16>;
using HybridPropertyGroupDirectoryMap = FixedBTreeMap<8, 16>;

class HybridPropertyDirectoryCodec final {
public:
    // Input descriptor must have been decoded from the pinned physical metadata
    // root. This constructs no authority from CRC alone: BTreeIndex subsequently
    // matches the exact namespace/root in the pinned Index Root Directory and
    // authenticates the actual typed root page through its normal single descent.
    [[nodiscard]] static BTreeRootReadBinding bindRoot(const HybridPropertyDescriptor &descriptor,
        const MetadataSnapshotPin &snapshot, HybridPropertyRootOrdinal ordinal) {
        const auto index = hybridPropertyIndexId(ordinal);
        hybrid_property_detail::require(snapshot.isValid() && descriptor.database_id == snapshot.superblock().database_id &&
            descriptor.semantic_generation.isValid() && descriptor.semantic_generation <= snapshot.superblock().generation,
            "hybrid directory descriptor has a foreign or future snapshot identity");
        const auto &root = descriptor.roots[static_cast<size_t>(ordinal)];
        hybrid_property_detail::require(root.page_id.isValid() && root.generation.isValid(),
            "hybrid directory cannot bind an absent descriptor namespace");
        return {descriptor.database_id, snapshot.superblock().generation, {index, root.page_id, root.generation}};
    }
    struct OwnerField {
        EntityPropertyKey owner;
        uint32_t token = 0;
        constexpr bool operator==(const OwnerField &) const = default;
    };
    struct GroupField {
        uint64_t group = 0;
        uint32_t token = 0;
        constexpr bool operator==(const GroupField &) const = default;
    };
    [[nodiscard]] static HybridPropertyOwnerMap::Key ownerKey(EntityPropertyKey owner, uint32_t token) {
        hybrid_property_detail::require(owner.isValid() && token, "hybrid owner directory key is invalid");
        HybridPropertyOwnerMap::Key result{};
        writeBig(std::span(result).first(8), owner.entity_id); writeBig(std::span(result).subspan(8), token); return result;
    }
    [[nodiscard]] static OwnerField decodeOwnerKey(const HybridPropertyOwnerMap::Key &key, EntityPropertyOwnerKind kind) {
        OwnerField result{{kind, readBig(std::span(key).first(8))}, uint32_t(readBig(std::span(key).subspan(8)))};
        hybrid_property_detail::require(result.owner.isValid() && result.token, "hybrid owner directory contains an invalid key");
        return result;
    }
    [[nodiscard]] static HybridPropertyFieldDirectoryMap::Key columnKey(uint64_t group, uint32_t token) {
        hybrid_property_detail::validateGroup(group, 0);
        hybrid_property_detail::require(token, "hybrid column directory token is zero");
        HybridPropertyFieldDirectoryMap::Key result{};
        writeBig(std::span(result).first(8), group); writeBig(std::span(result).subspan(8), token); return result;
    }
    [[nodiscard]] static GroupField decodeColumnKey(const HybridPropertyFieldDirectoryMap::Key &key) {
        GroupField result{readBig(std::span(key).first(8)), uint32_t(readBig(std::span(key).subspan(8)))};
        static_cast<void>(columnKey(result.group, result.token)); return result;
    }
    [[nodiscard]] static HybridPropertyGroupDirectoryMap::Key groupKey(uint64_t group) {
        hybrid_property_detail::validateGroup(group, 0);
        HybridPropertyGroupDirectoryMap::Key result{}; writeBig(result, group); return result;
    }
    [[nodiscard]] static uint64_t decodeGroupKey(const HybridPropertyGroupDirectoryMap::Key &key) {
        const auto result = readBig(key); hybrid_property_detail::validateGroup(result, 0); return result;
    }
    // The caller binds map to the authenticated Node/Edge owner root in the
    // same pin. No property values or token dictionary are materialized here.
    // Validate each membership before it can enter an owning result or a direct
    // selected-field reader. A false result is incomplete, never a full count.
    [[nodiscard]] static bool visitOwner(const HybridPropertyOwnerMap &map,
        const MetadataSnapshotPin &snapshot, EntityPropertyKey owner, size_t maximumEntries,
        const std::function<bool(uint32_t, HybridPropertySource)> &visitor,
        const std::function<void(size_t)> &boundaryAdmission = {},
        const CancellationCheckpoint *cancellation = nullptr) {
        hybrid_property_detail::require(owner.isValid() && bool(visitor),
            "hybrid owner enumeration requires an owner and visitor");
        const auto first = ownerKey(owner, 1);
        return map.visitPrefix(snapshot, std::span(first).first(8), maximumEntries,
            [&](const HybridPropertyOwnerMap::Entry &entry) {
                const auto field = decodeOwnerKey(entry.key, owner.owner_kind);
                hybrid_property_detail::require(field.owner == owner,
                    "hybrid owner enumeration escaped its owner");
                const auto source = decodeSource(entry.value);
                return visitor(field.token, source);
            }, boundaryAdmission, cancellation);
    }
    [[nodiscard]] static HybridPropertyOwnerMap::Value sourceValue(HybridPropertySource source) {
        hybrid_property_detail::require(source == HybridPropertySource::HPS_COLUMN ||
            source == HybridPropertySource::HPS_EXTENSION, "unknown hybrid field source");
        return {std::byte(uint8_t(source)), std::byte{}, std::byte{}, std::byte{}};
    }
    [[nodiscard]] static HybridPropertySource decodeSource(const HybridPropertyOwnerMap::Value &bytes) {
        hybrid_property_detail::zero(bytes, 1, 4);
        const auto result = HybridPropertySource(std::to_integer<uint8_t>(bytes[0]));
        static_cast<void>(sourceValue(result)); return result;
    }
    [[nodiscard]] static HybridPropertyFieldDirectoryMap::Value locatorValue(const SlottedSlotReference &reference) {
        hybrid_property_detail::require(reference.isValid(), "hybrid field locator is invalid");
        HybridPropertyFieldDirectoryMap::Value result{};
        LittleEndianCodec::writeU64(result, 0, reference.page_id.value());
        LittleEndianCodec::writeU16(result, 8, reference.slot_id);
        LittleEndianCodec::writeU32(result, 12, reference.generation); return result;
    }
    [[nodiscard]] static SlottedSlotReference decodeLocator(const HybridPropertyFieldDirectoryMap::Value &bytes) {
        hybrid_property_detail::zero(bytes, 10, 12);
        SlottedSlotReference result{PageId(LittleEndianCodec::readU64(bytes, 0)), LittleEndianCodec::readU16(bytes, 8),
            LittleEndianCodec::readU32(bytes, 12)};
        hybrid_property_detail::require(result.isValid(), "hybrid field locator contains an invalid slot identity"); return result;
    }
private:
    static void writeBig(std::span<std::byte> bytes, uint64_t value) {
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = std::byte((value >> ((bytes.size() - 1 - i) * 8)) & 255U);
    }
    static uint64_t readBig(std::span<const std::byte> bytes) {
        uint64_t result = 0; for (const auto byte : bytes) result = (result << 8U) | std::to_integer<uint8_t>(byte); return result;
    }
};
} // namespace graph::storage
