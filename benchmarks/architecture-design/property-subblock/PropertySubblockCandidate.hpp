#pragma once

// Isolated R3.2 payload experiment. These tags are NOT native page kinds.
// No file writer or production caller may use this before the atomic native
// format/root/fence/reader/writer/integrity/retirement update in R3.2.
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace zyx::benchmark::subblock {
using Bytes = std::span<const std::byte>;
enum class OwnerKind : uint8_t { HPK_NODE = 1, HPK_EDGE = 2 };
enum class ScalarKind : uint8_t { HPS_INT64 = 1, HPS_DOUBLE = 2, HPS_BOOL = 3 };
enum class Source : uint8_t { HPS_COLUMN = 1, HPS_EXTENSION = 2 };
enum class GroupMode : uint8_t { HPG_HYBRID = 1 };

inline void require(bool ok) {
    if (!ok) throw std::invalid_argument("Invalid property subblock candidate");
}
inline uint64_t read(Bytes bytes, size_t offset, size_t width) {
    require(width <= 8 && offset <= bytes.size() && width <= bytes.size() - offset);
    uint64_t result = 0;
    for (size_t i = 0; i < width; ++i)
        result |= uint64_t(std::to_integer<uint8_t>(bytes[offset + i])) << (i * 8);
    return result;
}
inline void write(std::span<std::byte> bytes, size_t offset, size_t width, uint64_t value) {
    require(width <= 8 && offset <= bytes.size() && width <= bytes.size() - offset);
    require(width == 8 || value < (uint64_t{1} << (width * 8)));
    for (size_t i = 0; i < width; ++i) bytes[offset + i] = std::byte((value >> (i * 8)) & 255);
}
inline void zeros(Bytes bytes, size_t first, size_t last) {
    require(first <= last && last <= bytes.size());
    for (size_t i = first; i < last; ++i) require(bytes[i] == std::byte{0});
}
inline bool valid(OwnerKind kind) { return kind == OwnerKind::HPK_NODE || kind == OwnerKind::HPK_EDGE; }
inline bool valid(ScalarKind kind) {
    return kind == ScalarKind::HPS_INT64 || kind == ScalarKind::HPS_DOUBLE || kind == ScalarKind::HPS_BOOL;
}
inline uint64_t groupFor(uint64_t owner) { require(owner != 0); return 1 + (owner - 1) / 64; }
inline unsigned slotFor(uint64_t owner) { require(owner != 0); return unsigned((owner - 1) % 64); }
inline void validGroup(uint64_t group) { require(group && group <= 1 + (UINT64_MAX - 1) / 64); }
inline void validLive(uint64_t group, uint64_t live) {
    validGroup(group);
    // The final uint64 ID group has 63 usable IDs: ID zero cannot wrap in.
    if (group == 1 + (UINT64_MAX - 1) / 64) require((live >> 63) == 0);
}

struct ColumnIdentity {
    OwnerKind owner_kind;
    ScalarKind scalar_kind;
    uint64_t group;
    uint32_t token;
    uint64_t generation;
    bool operator==(const ColumnIdentity &) const = default;
};
struct Column {
    static constexpr uint32_t MAGIC = 0x43425048; // HPBC, laboratory payload tag
    ColumnIdentity identity{OwnerKind::HPK_NODE, ScalarKind::HPS_INT64, 1, 1, 1};
    uint64_t presence = 0, nulls = 0, live = 0;
    // Raw IEEE/integer bits preserve signed zero, NaN payload and int/double identity.
    // No dynamic allocations; absent and NULL words must be zero.
    std::array<uint64_t, 64> words{};
    [[nodiscard]] size_t wireSize() const { return identity.scalar_kind == ScalarKind::HPS_BOOL ? 72 : 576; }
    void validate() const {
        require(valid(identity.owner_kind) && valid(identity.scalar_kind));
        validLive(identity.group, live);
        require(identity.token && identity.generation && !(presence & ~live) && !(nulls & ~presence));
        require(std::popcount(presence) >= 48);
        for (unsigned i = 0; i < 64; ++i) {
            const bool value = ((presence & ~nulls) & (uint64_t{1} << i)) != 0;
            require(value || words[i] == 0);
            if (identity.scalar_kind == ScalarKind::HPS_BOOL) require(words[i] <= 1);
        }
    }
    // Caller provides already admitted storage; encode never allocates.
    void encode(std::span<std::byte> out) const {
        validate(); require(out.size() == wireSize());
        for (auto &b : out) b = std::byte{0};
        write(out, 0, 4, MAGIC); write(out, 4, 2, 1);
        write(out, 6, 1, uint8_t(identity.owner_kind)); write(out, 7, 1, uint8_t(identity.scalar_kind));
        write(out, 8, 8, identity.group); write(out, 16, 4, identity.token);
        write(out, 20, 2, 64); write(out, 22, 2, wireSize() - 64);
        write(out, 24, 8, identity.generation); write(out, 32, 8, presence);
        write(out, 40, 8, nulls); write(out, 48, 8, live);
        uint64_t bits = 0;
        for (unsigned i = 0; i < 64; ++i) {
            if (identity.scalar_kind == ScalarKind::HPS_BOOL) bits |= words[i] << i;
            else write(out, 64 + i * 8, 8, words[i]);
        }
        if (identity.scalar_kind == ScalarKind::HPS_BOOL) write(out, 64, 8, bits);
    }
    static Column decode(Bytes bytes, const ColumnIdentity &expected) {
        require(bytes.size() >= 64 && read(bytes, 0, 4) == MAGIC && read(bytes, 4, 2) == 1);
        Column result;
        result.identity = {OwnerKind(read(bytes, 6, 1)), ScalarKind(read(bytes, 7, 1)),
            read(bytes, 8, 8), uint32_t(read(bytes, 16, 4)), read(bytes, 24, 8)};
        require(result.identity == expected && bytes.size() == result.wireSize());
        require(read(bytes, 20, 2) == 64 && read(bytes, 22, 2) == bytes.size() - 64);
        zeros(bytes, 56, 64);
        result.presence = read(bytes, 32, 8); result.nulls = read(bytes, 40, 8); result.live = read(bytes, 48, 8);
        for (unsigned i = 0; i < 64; ++i)
            result.words[i] = result.identity.scalar_kind == ScalarKind::HPS_BOOL ?
                (read(bytes, 64, 8) >> i) & 1 : read(bytes, 64 + i * 8, 8);
        result.validate(); return result;
    }
};

struct GroupHeader {
    static constexpr uint32_t MAGIC = 0x47425048;
    OwnerKind kind = OwnerKind::HPK_NODE;
    GroupMode mode = GroupMode::HPG_HYBRID;
    uint64_t group = 1, generation = 1, live = 0;
    uint32_t memberships = 0, columns = 0;
    void validate() const {
        require(valid(kind) && mode == GroupMode::HPG_HYBRID);
        validLive(group, live);
        // 4096 bounds an explicit conversion, not subsequent dynamic writes.
        // The persisted count uses its declared u32 range.
        require(generation && live && columns <= memberships / 48);
        // Every column has >=48 present live slots. Empty field sets are legal
        // during conversion of property-less live owners, not empty groups.
        require(columns == 0 || std::popcount(live) >= 48);
    }
    std::array<std::byte, 64> encode() const {
        validate(); std::array<std::byte, 64> out{};
        write(out, 0, 4, MAGIC); write(out, 4, 2, 1); write(out, 6, 1, uint8_t(kind));
        write(out, 7, 1, uint8_t(mode)); write(out, 8, 8, group); write(out, 16, 8, generation);
        write(out, 24, 8, live); write(out, 32, 4, memberships); write(out, 36, 4, columns); return out;
    }
    static GroupHeader decode(Bytes bytes, OwnerKind expectedKind, uint64_t expectedGroup, uint64_t expectedGeneration) {
        require(bytes.size() == 64 && read(bytes, 0, 4) == MAGIC && read(bytes, 4, 2) == 1);
        GroupHeader result{OwnerKind(read(bytes, 6, 1)), GroupMode(read(bytes, 7, 1)),
            read(bytes, 8, 8), read(bytes, 16, 8), read(bytes, 24, 8),
            uint32_t(read(bytes, 32, 4)), uint32_t(read(bytes, 36, 4))};
        zeros(bytes, 40, 64);
        require(result.kind == expectedKind && result.group == expectedGroup && result.generation == expectedGeneration);
        result.validate(); return result;
    }
};

struct RootReference {
    uint64_t page = 0, generation = 0;
    bool operator==(const RootReference &) const = default;
};
struct Descriptor {
    static constexpr uint32_t MAGIC = 0x44425048;
    uint64_t generation = 1, database = 1;
    // Node owner/column/extension/group, then Edge equivalents. No ROW root.
    std::array<RootReference, 8> roots{};
    void validate(uint64_t visibleGeneration) const {
        require(generation && generation <= visibleGeneration && database);
        for (const auto &root : roots)
            require((root.page == 0) == (root.generation == 0) && root.generation <= generation);
    }
    std::array<std::byte, 192> encode(uint64_t visibleGeneration) const {
        validate(visibleGeneration); std::array<std::byte, 192> out{};
        write(out, 0, 4, MAGIC); write(out, 4, 2, 1); write(out, 8, 8, generation); write(out, 16, 8, database);
        for (size_t i = 0; i < roots.size(); ++i) {
            write(out, 64 + i * 16, 8, roots[i].page); write(out, 72 + i * 16, 8, roots[i].generation);
        }
        return out;
    }
    static Descriptor decode(Bytes bytes, uint64_t expectedDatabase, uint64_t expectedGeneration, uint64_t visibleGeneration) {
        require(bytes.size() == 192 && read(bytes, 0, 4) == MAGIC && read(bytes, 4, 2) == 1);
        zeros(bytes, 6, 8); zeros(bytes, 24, 64);
        Descriptor result; result.generation = read(bytes, 8, 8); result.database = read(bytes, 16, 8);
        require(result.database == expectedDatabase && result.generation == expectedGeneration);
        for (size_t i = 0; i < result.roots.size(); ++i)
            result.roots[i] = {read(bytes, 64 + i * 16, 8), read(bytes, 72 + i * 16, 8)};
        result.validate(visibleGeneration); return result;
    }
};

struct Membership {
    uint64_t owner = 1;
    uint32_t token = 1;
    Source source = Source::HPS_COLUMN;
    std::array<std::byte, 16> encode() const {
        require(owner && token && (source == Source::HPS_COLUMN || source == Source::HPS_EXTENSION));
        std::array<std::byte, 16> out{};
        write(out, 0, 8, owner); write(out, 8, 4, token); write(out, 12, 1, uint8_t(source)); return out;
    }
    static Membership decode(Bytes bytes, uint64_t expectedOwner, uint32_t expectedToken) {
        require(bytes.size() == 16); zeros(bytes, 13, 16);
        Membership result{read(bytes, 0, 8), uint32_t(read(bytes, 8, 4)), Source(read(bytes, 12, 1))};
        require(result.owner == expectedOwner && result.token == expectedToken);
        (void)result.encode(); return result;
    }
};
struct Locator {
    uint64_t page = 1;
    uint16_t slot = 0;
    uint32_t generation = 1;
    bool operator==(const Locator &) const = default;
    std::array<std::byte, 16> encode() const {
        require(page && generation); std::array<std::byte, 16> out{};
        write(out, 0, 8, page); write(out, 8, 2, slot); write(out, 12, 4, generation); return out;
    }
    static Locator decode(Bytes bytes) {
        require(bytes.size() == 16); zeros(bytes, 10, 12);
        Locator result{read(bytes, 0, 8), uint16_t(read(bytes, 8, 2)), uint32_t(read(bytes, 12, 4))};
        (void)result.encode(); return result;
    }
    void authenticate(const Locator &actual) const { require(*this == actual); }
};

// These are payload records, not a claim about B+Tree allocated bytes.
struct ColumnDirectoryEntry {
    uint64_t group = 1;
    uint32_t token = 1;
    Locator locator;
    std::array<std::byte, 28> encode() const {
        validGroup(group); require(token);
        std::array<std::byte, 28> out{};
        write(out, 0, 8, group); write(out, 8, 4, token);
        const auto value = locator.encode();
        for (size_t i = 0; i < value.size(); ++i) out[12 + i] = value[i];
        return out;
    }
    static ColumnDirectoryEntry decode(Bytes bytes, uint64_t expectedGroup, uint32_t expectedToken) {
        require(bytes.size() == 28);
        ColumnDirectoryEntry result{read(bytes, 0, 8), uint32_t(read(bytes, 8, 4)), Locator::decode(bytes.subspan(12))};
        require(result.group == expectedGroup && result.token == expectedToken);
        (void)result.encode(); return result;
    }
};
struct GroupDirectoryEntry {
    uint64_t group = 1;
    Locator locator;
    std::array<std::byte, 24> encode() const {
        validGroup(group); std::array<std::byte, 24> out{};
        write(out, 0, 8, group); const auto value = locator.encode();
        for (size_t i = 0; i < value.size(); ++i) out[8 + i] = value[i];
        return out;
    }
    static GroupDirectoryEntry decode(Bytes bytes, uint64_t expectedGroup) {
        require(bytes.size() == 24);
        GroupDirectoryEntry result{read(bytes, 0, 8), Locator::decode(bytes.subspan(8))};
        require(result.group == expectedGroup); (void)result.encode(); return result;
    }
};

// Supplied by a future native estimator, NOT a payload-size estimate. Each
// category must be measured/bounded (including explicit zero) before admission.
// Separate candidate upper bound and ROW lower bound avoid optimistic rounding.
enum class CostPart : size_t {
    HPC_DATA_POOLS, HPC_MAP_POOLS, HPC_DESCRIPTOR, HPC_CATALOG_ALLOCATOR,
    HPC_PAGE_DIRECTORY_ROUTES, HPC_LARGE_MANIFEST_CONTENT, HPC_QUERY_INDEXES,
    HPC_RETIRE_QUEUE, HPC_RETAINED_PINS, HPC_COUNT
};
constexpr size_t COST_PARTS = size_t(CostPart::HPC_COUNT);
struct CostEvidence {
    std::array<uint64_t, COST_PARTS> row_lower{}, candidate_upper{};
    std::array<bool, COST_PARTS> known{};
    uint64_t peak_allocated_upper = 0;
    bool peak_known = false;
};
enum class GateResult {
    HPG_ADMIT, HPG_UNKNOWN_COST, HPG_OVERFLOW, HPG_WORK_LIMIT,
    HPG_RESOURCE_LIMIT, HPG_INSUFFICIENT_SAVING, HPG_INVALID_COST
};
struct ConversionLimits {
    uint64_t owners = 0, memberships = 0, staging_bytes = 0;
    uint64_t account_remaining = 0, file_cap = 0;
};
inline GateResult conversionGate(const CostEvidence &cost, const ConversionLimits &limits) {
    if (limits.owners == 0 || limits.owners > 64 || limits.memberships > 4096 || limits.staging_bytes > 1048576)
        return GateResult::HPG_WORK_LIMIT;
    if (limits.staging_bytes > limits.account_remaining) return GateResult::HPG_RESOURCE_LIMIT;
    uint64_t row = 0, candidate = 0;
    for (size_t i = 0; i < COST_PARTS; ++i) {
        if (!cost.known[i]) return GateResult::HPG_UNKNOWN_COST;
        if (cost.row_lower[i] > UINT64_MAX - row || cost.candidate_upper[i] > UINT64_MAX - candidate)
            return GateResult::HPG_OVERFLOW;
        row += cost.row_lower[i]; candidate += cost.candidate_upper[i];
    }
    if (!cost.peak_known) return GateResult::HPG_UNKNOWN_COST;
    // Peak must include all currently allocated old+new/retired/pinned pools,
    // not just steady candidate bytes. Do not silently repair inconsistent evidence.
    if (!row || !candidate || cost.peak_allocated_upper < candidate || cost.peak_allocated_upper < row) return GateResult::HPG_INVALID_COST;
    if (cost.peak_allocated_upper > limits.file_cap) return GateResult::HPG_RESOURCE_LIMIT;
    // candidate <= floor(9*ROW/10) without multiplying a possibly huge total.
    const uint64_t threshold = (row / 10) * 9 + ((row % 10) * 9) / 10;
    return candidate <= threshold ? GateResult::HPG_ADMIT : GateResult::HPG_INSUFFICIENT_SAVING;
}
} // namespace zyx::benchmark::subblock
