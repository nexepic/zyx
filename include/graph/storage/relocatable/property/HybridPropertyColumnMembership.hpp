#pragma once
#include <bit>
#include <optional>
#include "graph/storage/relocatable/property/HybridPropertyRecords.hpp"
#include "graph/storage/relocatable/property/HybridPropertyColdRowBindings.hpp"

namespace graph::storage {
// Byte/value codec only. A decoded record is not a publication capability.
// Physical framing, source authentication and the group-owned locator belong
// to the existing private lifecycle coordinator before any persisted use.
struct HybridPropertyColumnMembership {
    static constexpr uint32_t MAGIC = 0x4d48595aU; // ZYHM
    static constexpr uint16_t VERSION = 3;
    static constexpr size_t MAX_COLUMNS = 4096U / 48U;
    static constexpr size_t MAX_WORDS = (MAX_COLUMNS + 63U) / 64U;
    static constexpr size_t MAX_BYTES = 64U + 20U * MAX_COLUMNS + 64U * MAX_WORDS * 8U + HybridPropertyColdRowBindings::MAX_BYTES;
    EntityPropertyOwnerKind owner_kind = EntityPropertyOwnerKind::EPOK_NODE;
    uint64_t group_id = 0;
    Generation semantic_generation{};
    uint64_t live = 0;
    uint16_t column_count = 0;
    std::array<uint32_t, MAX_COLUMNS> tokens{};
    std::array<std::array<uint64_t, MAX_WORDS>, 64> owners{};
    std::array<SlottedSlotReference, MAX_COLUMNS> locators{};
    bool row_remainder = false;
    uint32_t cold_fields = 0;
    HybridPropertyColdRowBindings cold_rows;
    constexpr bool operator==(const HybridPropertyColumnMembership &) const = default;

    [[nodiscard]] size_t wordsPerOwner() const {
        hybrid_property_detail::require((column_count || row_remainder) && column_count <= MAX_COLUMNS,
            "column membership has an invalid column count");
        return (column_count + 63U) / 64U;
    }
    [[nodiscard]] size_t wireSize() const { return 64U + 20U * column_count + 64U * wordsPerOwner() * 8U + cold_rows.layout().bytes; }
    [[nodiscard]] uint32_t memberships() const {
        uint32_t result = 0;
        for (const auto &row : owners) for (auto word : row) result += std::popcount(word);
        return result;
    }
    [[nodiscard]] std::optional<size_t> ordinal(uint32_t token) const {
        static_cast<void>(wordsPerOwner());
        hybrid_property_detail::require(token != 0, "column membership token is zero");
        const auto end = tokens.begin() + column_count;
        const auto found = std::lower_bound(tokens.begin(), end, token);
        if (found == end || *found != token) return std::nullopt;
        return size_t(found - tokens.begin());
    }
    [[nodiscard]] bool contains(EntityPropertyKey owner, uint32_t token) const {
        hybrid_property_detail::require(owner.owner_kind == owner_kind && hybridPropertyGroupFor(owner) == group_id,
            "column membership has a foreign owner");
        const auto field = ordinal(token);
        return field && (owners[hybridPropertySlotFor(owner)][*field / 64U] & (UINT64_C(1) << (*field % 64U)));
    }
    [[nodiscard]] uint64_t presence(uint32_t token) const {
        const auto field = ordinal(token);
        hybrid_property_detail::require(bool(field), "column membership token is absent");
        uint64_t result = 0;
        for (unsigned slot = 0; slot < 64; ++slot)
            if (owners[slot][*field / 64U] & (UINT64_C(1) << (*field % 64U))) result |= UINT64_C(1) << slot;
        return result;
    }
    // Private builders may have zero columns after removing the last one;
    // ordinary column-only groups must drop their matrix locator. ROW-remainder
    // groups retain it as the authority for cold-only or propertyless owners.
    void eraseColumn(uint32_t token) {
        const auto selected = ordinal(token);
        hybrid_property_detail::require(bool(selected), "compact column removal has no source ordinal");
        for (auto &row : owners) {
            for (size_t field = *selected; field + 1U < column_count; ++field) {
                const auto bit = (row[(field + 1U) / 64U] >> ((field + 1U) % 64U)) & 1U;
                const auto mask = UINT64_C(1) << (field % 64U);
                row[field / 64U] = (row[field / 64U] & ~mask) | (bit << (field % 64U));
            }
            const auto last = size_t(column_count - 1U);
            row[last / 64U] &= ~(UINT64_C(1) << (last % 64U));
        }
        std::move(tokens.begin() + *selected + 1U, tokens.begin() + column_count, tokens.begin() + *selected);
        std::move(locators.begin() + *selected + 1U, locators.begin() + column_count, locators.begin() + *selected);
        tokens[--column_count] = 0;
        locators[column_count] = {};
    }
    void setPresence(uint32_t token, uint64_t presence) {
        const auto field = ordinal(token);
        hybrid_property_detail::require(field && !(presence & ~live) && std::popcount(presence) >= 48,
            "compact column replacement has no ordinal or requires demotion");
        const auto mask = UINT64_C(1) << (*field % 64U);
        for (unsigned slot = 0; slot < 64; ++slot)
            owners[slot][*field / 64U] = (owners[slot][*field / 64U] & ~mask) |
                ((presence & (UINT64_C(1) << slot)) ? mask : 0U);
    }
    void validate() const {
        using hybrid_property_detail::require;
        require(hybrid_property_detail::validKind(owner_kind) && semantic_generation.isValid() && live,
            "column membership has an invalid identity");
        hybrid_property_detail::validateGroup(group_id, live);
        const auto width = wordsPerOwner();
        static_cast<void>(cold_rows.layout());
        require(!(cold_rows.owners & ~live) && ((!cold_rows.owners && !cold_fields) ||
            (cold_rows.owners && cold_fields >= unsigned(std::popcount(cold_rows.owners)))),
            "cold ROW mask/count contradicts group liveness");
        require(row_remainder || (!cold_rows.owners && !cold_fields), "ordinary column membership contains cold ROW state");
        require(cold_fields <= UINT32_MAX - memberships(), "combined hot/cold membership count overflowed");
        for (size_t i = 0; i < tokens.size(); ++i)
            require(i < column_count ? tokens[i] && (!i || tokens[i - 1] < tokens[i]) : !tokens[i],
                "column membership tokens are unsorted, repeated or noncanonical");
        for (size_t i = column_count; i < locators.size(); ++i)
            require(locators[i] == SlottedSlotReference{}, "unused column locator is nonzero");
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
        unsigned completeOwners = 0;
        for (unsigned slot = 0; slot < 64; ++slot) {
            unsigned fields = 0;
#else
        for (unsigned slot = 0; slot < 64; ++slot)
#endif
        for (size_t word = 0; word < MAX_WORDS; ++word) {
            const auto bits = owners[slot][word];
            require((live & (UINT64_C(1) << slot)) || !bits, "dead owner has column memberships");
            if (word >= width) require(!bits, "column membership unused word is nonzero");
            else if (word + 1U == width && column_count % 64U)
                require(!(bits >> (column_count % 64U)), "column membership unused bits are nonzero");
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
            fields += unsigned(std::popcount(bits));
#endif
        }
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
            if (fields == column_count) ++completeOwners;
        }
        // Every column contains these same owners. The row/domain checks above
        // still cover every bit; 48 complete owners prove every column's
        // eligibility without reconstructing each 64-owner presence mask.
        if (completeOwners >= 48) return;
#endif
        for (size_t field = 0; field < column_count; ++field)
            require(std::popcount(presence(tokens[field])) >= 48, "column membership lost column eligibility");
    }
    [[nodiscard]] uint32_t totalMemberships() const {
        const auto hot = memberships();
        hybrid_property_detail::require(cold_fields <= UINT32_MAX - hot, "combined membership count overflowed");
        return hot + cold_fields;
    }
    void validateBound() const {
        validate();
        for (size_t i = 0; i < column_count; ++i)
            hybrid_property_detail::require(locators[i].isValid(), "column membership has an unbound locator");
    }
    [[nodiscard]] SlottedSlotReference columnLocator(uint32_t token) const {
        const auto field = ordinal(token);
        hybrid_property_detail::require(field && locators[*field].isValid(), "column membership has no bound token locator");
        return locators[*field];
    }
    void validateInGroup(const HybridPropertyGroupRecord &group, Generation descriptor, Generation visible) const {
        validate(); group.validate();
        hybrid_property_detail::require(owner_kind == group.owner_kind && group_id == group.group_id && live == group.live &&
            column_count == group.columns && (row_remainder ? totalMemberships() == group.memberships : memberships() <= group.memberships) &&
            (group.mode != HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER || row_remainder) &&
            (group.mode != HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP || !row_remainder) &&
            semantic_generation <= group.semantic_generation &&
            descriptor.isValid() && group.semantic_generation <= descriptor && descriptor <= visible,
            "column membership contradicts its authenticated group or snapshot");
    }
    [[nodiscard]] static HybridPropertyColumnMembership fromColumns(const HybridPropertyGroupRecord &group,
        std::span<const HybridPropertyColumnRecord> columns) {
        hybrid_property_detail::require(!columns.empty() && columns.size() <= MAX_COLUMNS && columns.size() == group.columns,
            "column membership construction exceeds its bounded column set");
        HybridPropertyColumnMembership result;
        result.owner_kind = group.owner_kind; result.group_id = group.group_id;
        result.semantic_generation = group.semantic_generation; result.live = group.live;
        result.column_count = uint16_t(columns.size());
        for (size_t i = 0; i < columns.size(); ++i) {
            const auto &column = columns[i];
            column.validateInGroup(group, group.semantic_generation, group.semantic_generation);
            result.tokens[i] = column.identity.token;
            for (unsigned slot = 0; slot < 64; ++slot)
                if (column.presence & (UINT64_C(1) << slot)) result.owners[slot][i / 64U] |= UINT64_C(1) << (i % 64U);
        }
        result.validateInGroup(group, group.semantic_generation, group.semantic_generation); return result;
    }
    template<class Visitor>
    bool visitOwner(EntityPropertyKey owner, size_t maximumEntries, Visitor &&visitor) const {
        hybrid_property_detail::require(maximumEntries && owner.owner_kind == owner_kind && hybridPropertyGroupFor(owner) == group_id,
            "column membership enumeration requires its owner and a positive bound");
        const auto &row = owners[hybridPropertySlotFor(owner)]; size_t visited = 0;
        for (size_t word = 0; word < wordsPerOwner(); ++word) {
            auto bits = row[word];
            while (bits) {
                const auto field = word * 64U + size_t(std::countr_zero(bits));
                hybrid_property_detail::require(field < column_count, "column membership enumeration found an unused bit");
                if (visited == maximumEntries) return false;
                ++visited; if (!visitor(tokens[field])) return false;
                bits &= bits - 1U;
            }
        }
        return true;
    }
    void encode(std::span<std::byte> bytes) const {
        validateBound(); hybrid_property_detail::require(bytes.size() == wireSize(), "column membership output width mismatch");
        std::fill(bytes.begin(), bytes.end(), std::byte{});
        LittleEndianCodec::writeU32(bytes, 0, MAGIC); LittleEndianCodec::writeU16(bytes, 4, VERSION);
        bytes[6] = std::byte(uint8_t(owner_kind)); bytes[7] = std::byte(wordsPerOwner());
        LittleEndianCodec::writeU64(bytes, 8, group_id); LittleEndianCodec::writeU64(bytes, 16, semantic_generation.value());
        LittleEndianCodec::writeU64(bytes, 24, live); LittleEndianCodec::writeU16(bytes, 32, column_count);
        LittleEndianCodec::writeU32(bytes, 36, memberships());
        const auto coldLayout = cold_rows.layout();
        LittleEndianCodec::writeU64(bytes, 40, cold_rows.owners);
        LittleEndianCodec::writeU32(bytes, 48, cold_fields);
        bytes[52] = std::byte((row_remainder ? 0x80U : 0U) |
            (coldLayout.encoding == HybridColdRowLocationEncoding::HCRLE_GROUPED ? 1U : 0U));
        LittleEndianCodec::writeU16(bytes, 54, coldLayout.groups);
        for (size_t i = 0; i < column_count; ++i) LittleEndianCodec::writeU32(bytes, 64U + i * 4U, tokens[i]);
        size_t offset = 64U + 4U * column_count;
        for (const auto &row : owners) for (size_t word = 0; word < wordsPerOwner(); ++word, offset += 8U)
            LittleEndianCodec::writeU64(bytes, offset, row[word]);
        for (size_t i = 0; i < column_count; ++i, offset += 16U) {
            LittleEndianCodec::writeU64(bytes, offset, locators[i].page_id.value());
            LittleEndianCodec::writeU16(bytes, offset + 8U, locators[i].slot_id);
            LittleEndianCodec::writeU32(bytes, offset + 12U, locators[i].generation);
        }
        cold_rows.encode(bytes.subspan(offset));
    }
    [[nodiscard]] static HybridPropertyColumnMembership decodePayload(std::span<const std::byte> bytes,
        EntityPropertyOwnerKind kind, uint64_t group, Generation visible) {
        using hybrid_property_detail::require;
        require(bytes.size() >= 64U && bytes.size() <= MAX_BYTES, "column membership header is truncated or oversized");
        require(LittleEndianCodec::readU32(bytes, 0) == MAGIC, "column membership magic mismatch");
        if (LittleEndianCodec::readU16(bytes, 4) != VERSION)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "unsupported column membership version");
        HybridPropertyColumnMembership result;
        result.owner_kind = EntityPropertyOwnerKind(std::to_integer<uint8_t>(bytes[6]));
        result.group_id = LittleEndianCodec::readU64(bytes, 8); result.semantic_generation = Generation(LittleEndianCodec::readU64(bytes, 16));
        result.live = LittleEndianCodec::readU64(bytes, 24); result.column_count = LittleEndianCodec::readU16(bytes, 32);
        const auto flags = std::to_integer<uint8_t>(bytes[52]);
        require(!(flags & ~0x81U), "column membership has unknown row flags");
        result.row_remainder = bool(flags & 0x80U);
        const auto coldMask = LittleEndianCodec::readU64(bytes, 40);
        result.cold_fields = LittleEndianCodec::readU32(bytes, 48);
        const auto coldGroups = LittleEndianCodec::readU16(bytes, 54);
        const auto encoding = (flags & 1U) ? HybridColdRowLocationEncoding::HCRLE_GROUPED : HybridColdRowLocationEncoding::HCRLE_DENSE;
        const size_t coldBytes = (flags & 1U) ? size_t(coldGroups) * 24U : size_t(std::popcount(coldMask)) * 16U;
        const size_t hotBytes = 64U + 20U * result.column_count + 64U * result.wordsPerOwner() * 8U;
        require(bytes.size() == hotBytes + coldBytes && std::to_integer<uint8_t>(bytes[7]) == result.wordsPerOwner(),
            "column membership record has an unexpected width");
        hybrid_property_detail::zero(bytes, 34, 36); hybrid_property_detail::zero(bytes, 53, 54);
        hybrid_property_detail::zero(bytes, 56, 64);
        for (size_t i = 0; i < result.column_count; ++i) result.tokens[i] = LittleEndianCodec::readU32(bytes, 64U + i * 4U);
        size_t offset = 64U + 4U * result.column_count;
        for (auto &row : result.owners) for (size_t word = 0; word < result.wordsPerOwner(); ++word, offset += 8U)
            row[word] = LittleEndianCodec::readU64(bytes, offset);
        for (size_t i = 0; i < result.column_count; ++i, offset += 16U) {
            hybrid_property_detail::zero(bytes, offset + 10U, offset + 12U);
            result.locators[i] = {PageId(LittleEndianCodec::readU64(bytes, offset)), LittleEndianCodec::readU16(bytes, offset + 8U),
                LittleEndianCodec::readU32(bytes, offset + 12U)};
        }
        require(result.memberships() == LittleEndianCodec::readU32(bytes, 36), "column membership count mismatch");
        result.cold_rows = HybridPropertyColdRowBindings::decode(coldMask, encoding, coldGroups, bytes.subspan(offset));
        require(result.owner_kind == kind && result.group_id == group && result.semantic_generation <= visible,
            "column membership belongs to a foreign source");
        result.validateBound(); return result;
    }
    [[nodiscard]] static HybridPropertyColumnMembership decode(std::span<const std::byte> bytes,
        const HybridPropertyGroupRecord &group, Generation descriptor, Generation visible) {
        auto result = decodePayload(bytes, group.owner_kind, group.group_id, visible);
        result.validateInGroup(group, descriptor, visible); return result;
    }
};
static_assert(HybridPropertyColumnMembership::MAX_BYTES < 4096);
}
