/**
 * @file HybridPropertyRecords.hpp
 * @brief Bounded typed records for the isolated hybrid-property format.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/storage/relocatable/allocation/SlottedPage.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/storage/relocatable/property/HybridPropertyShortText.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"

namespace graph::storage {

// Payload codecs only. No native pool kind or graph routing is admitted by
// these definitions. Nonempty descriptor guards remain until atomic lifecycle
// support is complete. Existing ROW records never pass through these codecs.
constexpr uint32_t RSS_HYBRID_COLUMN_MAGIC = 0x4348595AU; // ZYHC
constexpr uint32_t RSS_HYBRID_GROUP_MAGIC = 0x4748595AU; // ZYHG
constexpr uint16_t RSS_HYBRID_RECORD_VERSION = 1;
constexpr size_t RSS_HYBRID_GROUP_CAPACITY = 64;
constexpr size_t RSS_HYBRID_RECORD_HEADER_BYTES = 64;
constexpr uint64_t RSS_HYBRID_MAX_ENTITY_ID = (UINT64_C(1) << 63U) - 1U;
constexpr uint64_t RSS_HYBRID_MAX_GROUP_ID = 1 + (RSS_HYBRID_MAX_ENTITY_ID - 1) / 64;

enum class HybridPropertyScalarKind : uint8_t { HPSK_INT64 = 1, HPSK_DOUBLE = 2, HPSK_BOOL = 3, HPSK_SHORT_TEXT7 = 4 };
enum class HybridPropertyGroupMode : uint8_t { HPGM_HYBRID = 1, HPGM_COLUMN_MEMBERSHIP_BITMAP = 2, HPGM_COLUMN_ROW_REMAINDER = 3 };
enum class HybridPropertySource : uint8_t { HPS_COLUMN = 1, HPS_EXTENSION = 2, HPS_COLD_ROW = 3 };

namespace hybrid_property_detail {
inline void require(bool value, const char *message) {
    if (!value) throw FormatException(FormatError::FERR_INVALID_HEADER, message);
}
inline bool validKind(EntityPropertyOwnerKind kind) {
    return kind == EntityPropertyOwnerKind::EPOK_NODE || kind == EntityPropertyOwnerKind::EPOK_EDGE;
}
inline void validateGroup(uint64_t group, uint64_t live) {
    require(group && group <= RSS_HYBRID_MAX_GROUP_ID, "hybrid group exceeds native entity ID domain");
    // The final native group has 63 IDs, not 64: 2^63 is not a valid entity ID.
    require(group != RSS_HYBRID_MAX_GROUP_ID || !(live & (UINT64_C(1) << 63U)),
        "hybrid final group contains an invalid native entity ID");
}
inline void zero(std::span<const std::byte> bytes, size_t begin, size_t end) {
    require(std::all_of(bytes.begin() + begin, bytes.begin() + end,
        [](std::byte value) { return value == std::byte{}; }), "hybrid record reserved bytes are nonzero");
}
inline void header(std::span<const std::byte> bytes, uint32_t magic) {
    require(bytes.size() >= RSS_HYBRID_RECORD_HEADER_BYTES, "truncated hybrid record header");
    if (LittleEndianCodec::readU32(bytes, 0) != magic)
        throw FormatException(FormatError::FERR_INVALID_MAGIC, "hybrid record magic mismatch");
    if (LittleEndianCodec::readU16(bytes, 4) != RSS_HYBRID_RECORD_VERSION)
        throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "unsupported hybrid record version");
}
} // namespace hybrid_property_detail

[[nodiscard]] inline uint64_t hybridPropertyGroupFor(EntityPropertyKey owner) {
    hybrid_property_detail::require(owner.isValid(), "invalid hybrid property owner");
    return 1 + (owner.entity_id - 1) / RSS_HYBRID_GROUP_CAPACITY;
}
[[nodiscard]] inline unsigned hybridPropertySlotFor(EntityPropertyKey owner) {
    hybrid_property_detail::require(owner.isValid(), "invalid hybrid property owner");
    return unsigned((owner.entity_id - 1) % RSS_HYBRID_GROUP_CAPACITY);
}

struct HybridPropertyGroupRecord {
    EntityPropertyOwnerKind owner_kind = EntityPropertyOwnerKind::EPOK_NODE;
    HybridPropertyGroupMode mode = HybridPropertyGroupMode::HPGM_HYBRID;
    uint64_t group_id = 0;
    Generation semantic_generation{};
    uint64_t live = 0;
    uint32_t memberships = 0, columns = 0;
    SlottedSlotReference column_membership{};
    constexpr bool operator==(const HybridPropertyGroupRecord &) const = default;

    [[nodiscard]] bool usesColumnMembership() const {
        return mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP ||
            mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER;
    }
    void validate() const {
        using namespace hybrid_property_detail;
        require(validKind(owner_kind) && (mode == HybridPropertyGroupMode::HPGM_HYBRID ||
            mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP ||
            mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER), "invalid hybrid group namespace or mode");
        if (usesColumnMembership())
            require((columns || mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) &&
                columns <= 4096U / 48U && column_membership.isValid(), "compact group has no bounded column membership record");
        else require(column_membership == SlottedSlotReference{}, "legacy group has a column membership locator");
        validateGroup(group_id, live);
        require(semantic_generation.isValid() && live && columns <= memberships / 48U &&
            (!columns || std::popcount(live) >= 48), "invalid hybrid group identity or counts");
        // The 4096 conversion bound is not a lifetime membership limit.
    }
    [[nodiscard]] std::array<std::byte, 64> encode() const {
        validate(); std::array<std::byte, 64> bytes{};
        LittleEndianCodec::writeU32(bytes, 0, RSS_HYBRID_GROUP_MAGIC);
        LittleEndianCodec::writeU16(bytes, 4, RSS_HYBRID_RECORD_VERSION);
        bytes[6] = std::byte(uint8_t(owner_kind)); bytes[7] = std::byte(uint8_t(mode));
        LittleEndianCodec::writeU64(bytes, 8, group_id);
        LittleEndianCodec::writeU64(bytes, 16, semantic_generation.value());
        LittleEndianCodec::writeU64(bytes, 24, live);
        LittleEndianCodec::writeU32(bytes, 32, memberships);
        LittleEndianCodec::writeU32(bytes, 36, columns);
        if (usesColumnMembership()) {
            LittleEndianCodec::writeU64(bytes, 40, column_membership.page_id.value());
            LittleEndianCodec::writeU16(bytes, 48, column_membership.slot_id);
            LittleEndianCodec::writeU32(bytes, 52, column_membership.generation);
        }
        return bytes;
    }
    [[nodiscard]] static HybridPropertyGroupRecord decode(std::span<const std::byte> bytes,
        EntityPropertyOwnerKind expectedKind, uint64_t expectedGroup, Generation descriptorGeneration,
        Generation visibleGeneration) {
        using namespace hybrid_property_detail;
        header(bytes, RSS_HYBRID_GROUP_MAGIC);
        require(bytes.size() == 64, "hybrid group record has unexpected width");
        HybridPropertyGroupRecord result{EntityPropertyOwnerKind(std::to_integer<uint8_t>(bytes[6])),
            HybridPropertyGroupMode(std::to_integer<uint8_t>(bytes[7])), LittleEndianCodec::readU64(bytes, 8),
            Generation(LittleEndianCodec::readU64(bytes, 16)), LittleEndianCodec::readU64(bytes, 24),
            LittleEndianCodec::readU32(bytes, 32), LittleEndianCodec::readU32(bytes, 36)};
        if (result.usesColumnMembership()) {
            zero(bytes, 50, 52); zero(bytes, 56, 64);
            result.column_membership = {PageId(LittleEndianCodec::readU64(bytes, 40)), LittleEndianCodec::readU16(bytes, 48),
                LittleEndianCodec::readU32(bytes, 52)};
        } else zero(bytes, 40, 64);
        result.validate();
        require(result.owner_kind == expectedKind && result.group_id == expectedGroup &&
            descriptorGeneration.isValid() && descriptorGeneration <= visibleGeneration &&
            result.semantic_generation <= descriptorGeneration, "hybrid group does not belong to its snapshot");
        return result;
    }
};

struct HybridPropertyColumnIdentity {
    EntityPropertyOwnerKind owner_kind = EntityPropertyOwnerKind::EPOK_NODE;
    HybridPropertyScalarKind scalar_kind = HybridPropertyScalarKind::HPSK_INT64;
    uint64_t group_id = 0;
    uint32_t token = 0;
    Generation semantic_generation{};
    constexpr bool operator==(const HybridPropertyColumnIdentity &) const = default;
};
struct HybridPropertyColumnRecord {
    HybridPropertyColumnIdentity identity;
    uint64_t presence = 0, nulls = 0, live = 0;
    // IEEE/integer bits, not native scalar serialization. +0/-0 and NaN
    // encoding are lossless; write no-op equality remains PropertyValue's job.
    std::array<uint64_t, 64> words{};
    constexpr bool operator==(const HybridPropertyColumnRecord &) const = default;
    [[nodiscard]] size_t wireSize() const {
        return identity.scalar_kind == HybridPropertyScalarKind::HPSK_BOOL ? 72U : 576U;
    }
    void validate() const {
        using namespace hybrid_property_detail;
        require(validKind(identity.owner_kind) && (identity.scalar_kind == HybridPropertyScalarKind::HPSK_INT64 ||
            identity.scalar_kind == HybridPropertyScalarKind::HPSK_DOUBLE ||
            identity.scalar_kind == HybridPropertyScalarKind::HPSK_BOOL ||
            identity.scalar_kind == HybridPropertyScalarKind::HPSK_SHORT_TEXT7), "invalid hybrid column namespace or scalar");
        validateGroup(identity.group_id, live);
        require(identity.token && identity.semantic_generation.isValid() && !(presence & ~live) &&
            !(nulls & ~presence) && std::popcount(presence) >= 48, "invalid hybrid column identity or bitmaps");
        for (unsigned slot = 0; slot < 64; ++slot) {
            require(((presence & ~nulls) & (UINT64_C(1) << slot)) || !words[slot],
                "absent or NULL hybrid column payload is nonzero");
            require(identity.scalar_kind != HybridPropertyScalarKind::HPSK_BOOL || words[slot] <= 1U,
                "hybrid bool column value is not canonical");
            if(identity.scalar_kind == HybridPropertyScalarKind::HPSK_SHORT_TEXT7)
                HybridPropertyShortText::validate(words[slot]);
        }
    }
    void encode(std::span<std::byte> bytes) const {
        validate(); hybrid_property_detail::require(bytes.size() == wireSize(), "hybrid column output width mismatch");
        std::fill(bytes.begin(), bytes.end(), std::byte{});
        LittleEndianCodec::writeU32(bytes, 0, RSS_HYBRID_COLUMN_MAGIC);
        LittleEndianCodec::writeU16(bytes, 4, RSS_HYBRID_RECORD_VERSION);
        bytes[6] = std::byte(uint8_t(identity.owner_kind)); bytes[7] = std::byte(uint8_t(identity.scalar_kind));
        LittleEndianCodec::writeU64(bytes, 8, identity.group_id); LittleEndianCodec::writeU32(bytes, 16, identity.token);
        LittleEndianCodec::writeU16(bytes, 20, 64); LittleEndianCodec::writeU16(bytes, 22, uint16_t(wireSize() - 64));
        LittleEndianCodec::writeU64(bytes, 24, identity.semantic_generation.value());
        LittleEndianCodec::writeU64(bytes, 32, presence); LittleEndianCodec::writeU64(bytes, 40, nulls);
        LittleEndianCodec::writeU64(bytes, 48, live);
        if (identity.scalar_kind == HybridPropertyScalarKind::HPSK_BOOL) {
            uint64_t bits = 0; for (unsigned slot = 0; slot < 64; ++slot) bits |= words[slot] << slot;
            LittleEndianCodec::writeU64(bytes, 64, bits);
        } else for (unsigned slot = 0; slot < 64; ++slot) LittleEndianCodec::writeU64(bytes, 64 + slot * 8, words[slot]);
    }
    // Payload validation for a private page builder. Reading graph authority
    // still requires decode() with its authenticated group/descriptor context.
    [[nodiscard]] static HybridPropertyColumnRecord decodePayload(std::span<const std::byte> bytes,
        const HybridPropertyColumnIdentity &expected) {
        using namespace hybrid_property_detail;
        header(bytes, RSS_HYBRID_COLUMN_MAGIC);
        HybridPropertyColumnRecord result;
        result.identity = {EntityPropertyOwnerKind(std::to_integer<uint8_t>(bytes[6])),
            HybridPropertyScalarKind(std::to_integer<uint8_t>(bytes[7])), LittleEndianCodec::readU64(bytes, 8),
            LittleEndianCodec::readU32(bytes, 16), Generation(LittleEndianCodec::readU64(bytes, 24))};
        require(result.identity == expected && bytes.size() == result.wireSize() &&
            LittleEndianCodec::readU16(bytes, 20) == 64 &&
            LittleEndianCodec::readU16(bytes, 22) == result.wireSize() - 64, "hybrid column identity or width mismatch");
        zero(bytes, 56, 64);
        result.presence = LittleEndianCodec::readU64(bytes, 32); result.nulls = LittleEndianCodec::readU64(bytes, 40);
        result.live = LittleEndianCodec::readU64(bytes, 48);
        for (unsigned slot = 0; slot < 64; ++slot)
            result.words[slot] = result.identity.scalar_kind == HybridPropertyScalarKind::HPSK_BOOL ?
                (LittleEndianCodec::readU64(bytes, 64) >> slot) & 1U : LittleEndianCodec::readU64(bytes, 64 + slot * 8);
        result.validate(); return result;
    }
    void validateInGroup(const HybridPropertyGroupRecord &group, Generation descriptorGeneration,
        Generation visibleGeneration) const {
        validate(); validateGroupContext(group, descriptorGeneration, visibleGeneration);
    }
private:
    void validateGroupContext(const HybridPropertyGroupRecord &group, Generation descriptorGeneration,
        Generation visibleGeneration) const {
        using namespace hybrid_property_detail;
        group.validate();
        require(group.owner_kind == identity.owner_kind && group.group_id == identity.group_id &&
            group.columns && group.memberships >= uint32_t(std::popcount(presence)) &&
            !(presence & ~group.live) && identity.semantic_generation <= group.semantic_generation &&
            group.semantic_generation <= descriptorGeneration && descriptorGeneration.isValid() &&
            descriptorGeneration <= visibleGeneration, "hybrid column does not belong to its visible group");
    }
public:
    [[nodiscard]] static HybridPropertyColumnRecord decode(std::span<const std::byte> bytes,
        const HybridPropertyColumnIdentity &expected, const HybridPropertyGroupRecord &group,
        Generation descriptorGeneration, Generation visibleGeneration) {
        auto result = decodePayload(bytes, expected);
        result.validateGroupContext(group, descriptorGeneration, visibleGeneration);
        return result;
    }
};
} // namespace graph::storage
