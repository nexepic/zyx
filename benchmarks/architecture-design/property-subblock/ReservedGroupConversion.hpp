#pragma once
#include <algorithm>
#include <functional>
#include "PropertySubblockFieldTransition.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/storage/relocatable/property/HybridPropertyRecords.hpp"

namespace zyx::benchmark::subblock {
// Bounded conversion planning over immutable, caller-owned, authenticated ROW
// roots. This is not a persisted native layout: extension values remain borrowed
// canonical PropertyValues, never invented LargeValue references. The caller
// must keep input roots and budget alive and separately charge their storage.
// No source value is copied or mutated while classifying the group.
class ReservedGroupConversion final {
public:
    static constexpr size_t MAX_MEMBERSHIPS = 4096;
    static constexpr size_t MAX_COLUMNS = MAX_MEMBERSHIPS / 48;
    struct Member {
        uint64_t owner = 0;
        uint32_t token = 0;
        const graph::PropertyValue *value = nullptr;
        Source source = Source::HPS_EXTENSION;
        uint16_t column = 0;
    };
private:
    graph::query::execution::MaterializedByteReservation reservation_;
    GroupHeader group_;
    std::array<Member, MAX_MEMBERSHIPS> members_{};
    std::array<Column, MAX_COLUMNS> columns_{};
    size_t member_count_ = 0, column_count_ = 0;

    static auto reserve(graph::query::execution::MaterializedByteBudget &budget) {
        graph::query::execution::MaterializedByteReservation result(&budget);
        // Fixed holder plus column construction and bounded sorting stack
        // allowance. Payload bodies, native publication buffers and roots are
        // separate owners; this reservation does not pretend to cover them.
        constexpr size_t charge = sizeof(ReservedGroupConversion) + sizeof(Column) + 4096;
        static_assert(charge <= 1024 * 1024);
        result.set(charge); return result;
    }
    static std::optional<ScalarKind> primitive(const graph::PropertyValue &value) {
        if (std::holds_alternative<int64_t>(value.getVariant())) return ScalarKind::HPS_INT64;
        if (std::holds_alternative<double>(value.getVariant())) return ScalarKind::HPS_DOUBLE;
        if (std::holds_alternative<bool>(value.getVariant())) return ScalarKind::HPS_BOOL;
        return {};
    }
    static bool isNull(const graph::PropertyValue &value) {
        return std::holds_alternative<std::monostate>(value.getVariant());
    }
    static uint64_t bits(const graph::PropertyValue &value, ScalarKind kind) {
        if (kind == ScalarKind::HPS_INT64) return std::bit_cast<uint64_t>(std::get<int64_t>(value.getVariant()));
        if (kind == ScalarKind::HPS_DOUBLE) return std::bit_cast<uint64_t>(std::get<double>(value.getVariant()));
        return std::get<bool>(value.getVariant());
    }
public:
    ReservedGroupConversion(graph::query::execution::MaterializedByteBudget &budget,
        std::span<const graph::storage::PropertyCollectionRoot> roots, OwnerKind kind,
        uint64_t group, uint64_t generation, size_t nativeRecordLimit,
        const std::function<void()> &checkpoint = {}) : reservation_(reserve(budget)) {
        require(valid(kind) && !roots.empty() && roots.size() <= 64 && generation);
        validGroup(group);
        if (nativeRecordLimit < 64) throw std::length_error("property conversion native group-header bound");
        group_.kind = kind; group_.group = group; group_.generation = generation;
        const uint32_t nativeKind = kind == OwnerKind::HPK_NODE ? graph::Node::typeId : graph::Edge::typeId;
        for (const auto &root : roots) {
            if (checkpoint) checkpoint();
            require(root.owner_entity_type == nativeKind && root.owner_entity_id &&
                root.owner_entity_id <= uint64_t(INT64_MAX) && groupFor(root.owner_entity_id) == group);
            const uint64_t bit = uint64_t{1} << slotFor(root.owner_entity_id);
            require(!(group_.live & bit)); group_.live |= bit;
            if (root.entries.size() > MAX_MEMBERSHIPS - member_count_)
                throw std::length_error("property conversion membership bound");
            uint32_t previous = 0;
            for (const auto &entry : root.entries) {
                require(entry.key_token > previous); previous = entry.key_token;
                members_[member_count_++] = {root.owner_entity_id, entry.key_token, &entry.value};
            }
        }
        auto end = members_.begin() + member_count_;
        std::sort(members_.begin(), end, [](const Member &a, const Member &b) {
            return a.token != b.token ? a.token < b.token : a.owner < b.owner;
        });
        for (size_t first = 0; first < member_count_;) {
            if (checkpoint) checkpoint();
            size_t last = first + 1;
            while (last < member_count_ && members_[last].token == members_[first].token) ++last;
            std::optional<ScalarKind> scalar;
            bool compatible = last - first >= 48;
            for (size_t i = first; compatible && i < last; ++i) {
                const auto &value = *members_[i].value;
                if (isNull(value)) continue;
                const auto current = primitive(value);
                if (!current || (scalar && current != scalar)) compatible = false;
                else scalar = current;
            }
            // An all-NULL new field has no inferred scalar type.
            if (compatible && scalar) {
                require(column_count_ < MAX_COLUMNS);
                Column &column = columns_[column_count_];
                column.identity = {kind, *scalar, group, members_[first].token, generation};
                column.live = group_.live;
                if (column.wireSize() > nativeRecordLimit || column.wireSize() > 4096)
                    throw std::length_error("property conversion native column bound");
                for (size_t i = first; i < last; ++i) {
                    auto &member = members_[i];
                    const unsigned slot = slotFor(member.owner);
                    const uint64_t bit = uint64_t{1} << slot;
                    column.presence |= bit;
                    if (isNull(*member.value)) column.nulls |= bit;
                    else column.words[slot] = bits(*member.value, *scalar);
                    member.source = Source::HPS_COLUMN; member.column = uint16_t(column_count_);
                }
                column.validate(); ++column_count_;
            }
            first = last;
        }
        std::sort(members_.begin(), end, [](const Member &a, const Member &b) {
            return a.owner != b.owner ? a.owner < b.owner : a.token < b.token;
        });
        group_.memberships = uint32_t(member_count_); group_.columns = uint32_t(column_count_);
        group_.validate(); if (checkpoint) checkpoint();
    }
    ReservedGroupConversion(const ReservedGroupConversion &) = delete;
    ReservedGroupConversion &operator=(const ReservedGroupConversion &) = delete;
    [[nodiscard]] const GroupHeader &group() const & noexcept { return group_; }
    const GroupHeader &group() const && = delete;
    [[nodiscard]] std::span<const Member> members() const & noexcept { return {members_.data(), member_count_}; }
    std::span<const Member> members() const && = delete;
    [[nodiscard]] std::span<const Column> columns() const & noexcept { return {columns_.data(), column_count_}; }
    std::span<const Column> columns() const && = delete;
    [[nodiscard]] size_t retainedCharge() const noexcept { return reservation_.bytes(); }

    // Emit the native typed payload, never the laboratory magic. The output
    // buffer remains caller-owned/admitted. No PageId is allocated here and no
    // root is published; that belongs to the single metadata coordinator.
    [[nodiscard]] graph::storage::HybridPropertyGroupRecord nativeGroup() const {
        using namespace graph::storage;
        HybridPropertyGroupRecord result{
            group_.kind == OwnerKind::HPK_NODE ? EntityPropertyOwnerKind::EPOK_NODE : EntityPropertyOwnerKind::EPOK_EDGE,
            HybridPropertyGroupMode::HPGM_HYBRID, group_.group, Generation(group_.generation),
            group_.live, group_.memberships, group_.columns};
        result.validate(); return result;
    }
    void encodeNativeColumn(size_t ordinal, std::span<std::byte> output) const {
        using namespace graph::storage;
        require(ordinal < column_count_);
        const auto &source = columns_[ordinal];
        HybridPropertyColumnRecord record;
        // Existing conversion reservation includes one temporary Column and
        // 4096 stack bytes. Native encoding must fit that existing allowance.
        static_assert(sizeof(record) <= sizeof(Column));
        record.identity = {nativeGroup().owner_kind,
            source.identity.scalar_kind == ScalarKind::HPS_INT64 ? HybridPropertyScalarKind::HPSK_INT64 :
                source.identity.scalar_kind == ScalarKind::HPS_DOUBLE ? HybridPropertyScalarKind::HPSK_DOUBLE :
                    HybridPropertyScalarKind::HPSK_BOOL,
            source.identity.group, source.identity.token, Generation(source.identity.generation)};
        record.presence = source.presence; record.nulls = source.nulls; record.live = source.live;
        record.words = source.words; record.encode(output);
    }

    // Reverse projection visits only actual owner fields in token order. Heap
    // values are borrowed without copying; primitive cells are reconstructed
    // from column bits. The synchronous visitor owns any copy/encoding charge.
    // Property-less live owners are valid, dead/out-of-group owners are not.
    void visitOwner(uint64_t owner, const std::function<void(uint32_t, const graph::PropertyValue &)> &visitor) const {
        require(owner && owner <= uint64_t(INT64_MAX) && groupFor(owner) == group_.group && visitor &&
            (group_.live & (uint64_t{1} << slotFor(owner))));
        auto found = std::lower_bound(members_.begin(), members_.begin() + member_count_, owner,
            [](const Member &member, uint64_t id) { return member.owner < id; });
        for (; found != members_.begin() + member_count_ && found->owner == owner; ++found) {
            if (found->source == Source::HPS_EXTENSION) { visitor(found->token, *found->value); continue; }
            const auto cell = cellAt(columns_[found->column], slotFor(owner));
            graph::PropertyValue value;
            switch (cell.kind) {
            case CellKind::HFC_NULL: break;
            case CellKind::HFC_INT64: value = graph::PropertyValue(std::bit_cast<int64_t>(cell.bits)); break;
            case CellKind::HFC_DOUBLE: value = graph::PropertyValue(std::bit_cast<double>(cell.bits)); break;
            case CellKind::HFC_BOOL: value = graph::PropertyValue(bool(cell.bits)); break;
            default: require(false);
            }
            visitor(found->token, value);
        }
    }
};
} // namespace zyx::benchmark::subblock
