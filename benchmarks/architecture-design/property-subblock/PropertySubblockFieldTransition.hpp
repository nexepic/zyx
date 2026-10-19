#pragma once
#include "PropertySubblockCandidate.hpp"

namespace zyx::benchmark::subblock {
// Pure, bounded field transition only. No promotion, graph mutation, file IO,
// allocator reservation or invented large-value reference is performed here.
// EXTENSION marks the incoming caller-owned canonical value; its body remains
// with the caller and must be independently admitted/authenticated by the future
// native extension writer. These cells are not a disk encoding.
enum class CellKind : uint8_t {
    HFC_MISSING, HFC_NULL, HFC_INT64, HFC_DOUBLE, HFC_BOOL, HFC_EXTENSION
};
struct FieldCell {
    CellKind kind = CellKind::HFC_MISSING;
    uint64_t bits = 0;
    bool operator==(const FieldCell &) const = default;
    void validate() const {
        switch (kind) {
        case CellKind::HFC_MISSING: case CellKind::HFC_NULL: case CellKind::HFC_EXTENSION:
            require(bits == 0); break;
        case CellKind::HFC_BOOL: require(bits <= 1); break;
        case CellKind::HFC_INT64: case CellKind::HFC_DOUBLE: break;
        default: require(false);
        }
    }
};
enum class FieldAction : uint8_t { HFT_NO_CHANGE, HFT_REPLACE_COLUMN, HFT_DEMOTE_FIELD };
struct FieldTransition {
    FieldAction action = FieldAction::HFT_NO_CHANGE;
    Column column;
    // Used only for demotion. At most the one changed field's 64 cells, never
    // another field/whole-group container. Missing cells have no selector.
    std::array<FieldCell,64> extension_cells{};
};
inline CellKind cellKind(ScalarKind kind) {
    switch (kind) {
    case ScalarKind::HPS_INT64: return CellKind::HFC_INT64;
    case ScalarKind::HPS_DOUBLE: return CellKind::HFC_DOUBLE;
    case ScalarKind::HPS_BOOL: return CellKind::HFC_BOOL;
    default: require(false); return CellKind::HFC_MISSING;
    }
}
inline FieldCell cellAt(const Column &source, unsigned slot) {
    require(slot < 64);
    const uint64_t bit = uint64_t{1} << slot;
    if (!(source.presence & bit)) return {};
    if (source.nulls & bit) return {CellKind::HFC_NULL,0};
    return {cellKind(source.identity.scalar_kind),source.words[slot]};
}
// Match existing PropertyValue equality used by ROW write no-op detection:
// types are distinct, double +0 == -0, and NaN != NaN even with equal bits.
// Encoded replacement payloads still preserve raw IEEE bits.
inline bool sameCellValue(FieldCell left,FieldCell right) {
    if (left.kind != right.kind) return false;
    if (left.kind == CellKind::HFC_DOUBLE) {
        static_assert(sizeof(double) == sizeof(uint64_t) && std::numeric_limits<double>::is_iec559);
        return std::bit_cast<double>(left.bits) == std::bit_cast<double>(right.bits);
    }
    // An opaque extension body cannot be compared through a marker.
    return left.kind != CellKind::HFC_EXTENSION && left.bits == right.bits;
}
// Caller must reserve the fixed staging/output footprint before entering and
// retain it while the result lives. Input and caller-owned extension body stay
// unchanged on every exception. Limits are those of the actual account/native
// slotted-page usable payload, never a larger independent laboratory allowance.
// The encoded column live mask may differ from current group live only on
// absent cells. This argument changes at most the target slot; current group
// presence/liveness is separately checked by validateColumnSnapshot.
inline FieldTransition planFieldChange(const Column &source, unsigned slot,
    FieldCell value, uint64_t newColumnLive, uint64_t nextGeneration,
    uint64_t stagingAvailable, size_t nativeRecordLimit) {
    if (stagingAvailable < sizeof(FieldTransition)) throw std::length_error("field transition staging bound");
    source.validate(); require(slot < 64); value.validate(); validLive(source.identity.group,newColumnLive);
    const uint64_t bit = uint64_t{1} << slot;
    require(((source.live ^ newColumnLive) & ~bit) == 0);
    require((newColumnLive & bit) || value.kind == CellKind::HFC_MISSING);
    FieldTransition result; result.column = source;
    if (newColumnLive == source.live && sameCellValue(cellAt(source,slot),value)) return result;
    require(nextGeneration > source.identity.generation);
    result.column.identity.generation = nextGeneration;
    result.column.live = newColumnLive;
    result.column.presence &= ~bit; result.column.nulls &= ~bit; result.column.words[slot] = 0;
    if (value.kind != CellKind::HFC_MISSING) result.column.presence |= bit;
    if (value.kind == CellKind::HFC_NULL) result.column.nulls |= bit;
    const bool compatible = value.kind == CellKind::HFC_MISSING || value.kind == CellKind::HFC_NULL ||
        value.kind == cellKind(source.identity.scalar_kind);
    if (compatible && std::popcount(result.column.presence) >= 48) {
        if (value.kind != CellKind::HFC_MISSING && value.kind != CellKind::HFC_NULL)
            result.column.words[slot] = value.bits;
        if (result.column.wireSize() > 4096 || result.column.wireSize() > nativeRecordLimit)
            throw std::length_error("field transition native record bound");
        result.column.validate(); result.action = FieldAction::HFT_REPLACE_COLUMN;
    } else {
        result.action = FieldAction::HFT_DEMOTE_FIELD;
        for (unsigned i=0;i<64;++i) result.extension_cells[i] = cellAt(source,i);
        result.extension_cells[slot] = value;
        // Incompatible new value is deliberately not encoded into Column; only
        // extension_cells is authoritative for a demotion result. Consumers
        // must branch on action and remove the old column before publishing.
        result.column = Column{}; // invalid empty column cannot accidentally encode as a replacement
    }
    return result;
}

// Group live is current authority. A retained column can have an older encoded
// live mask only where it has no present value. That permits birth/death of an
// owner absent from this field without rewriting every unrelated column.
inline void validateColumnSnapshot(const Column &column,const GroupHeader &group,
    const Descriptor &descriptor,uint64_t expectedDatabase,uint64_t visibleGeneration) {
    column.validate(); group.validate(); descriptor.validate(visibleGeneration);
    require(descriptor.database == expectedDatabase && group.kind == column.identity.owner_kind &&
        group.group == column.identity.group && column.identity.generation <= group.generation &&
        group.generation <= descriptor.generation && !(column.presence & ~group.live) && group.columns > 0);
    const size_t base = group.kind == OwnerKind::HPK_NODE ? 0 : 4;
    require(descriptor.roots[base].page && descriptor.roots[base+1].page && descriptor.roots[base+3].page);
}
} // namespace zyx::benchmark::subblock
