#pragma once
#include <algorithm>
#include <functional>
#include "PropertySubblockFieldTransition.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/storage/relocatable/property/HybridPropertyRecords.hpp"
#include "graph/storage/relocatable/property/HybridPropertyGroupConversion.hpp"

namespace zyx::benchmark::subblock {
// Laboratory view adapter over the single storage-owned conversion planner.
// Classification and reverse projection are implemented only in storage.
// Inputs are immutable, caller-owned, authenticated ROW roots. This is not a persisted native layout: extension values remain borrowed
// canonical PropertyValues, never invented LargeValue references. The caller
// must keep input roots and budget alive and separately charge their storage.
// No source value is copied or mutated while classifying the group.
class ReservedGroupConversion final {
public:
    static constexpr size_t MAX_MEMBERSHIPS = graph::storage::HybridPropertyGroupConversion::MAX_MEMBERSHIPS;
    static constexpr size_t MAX_COLUMNS = graph::storage::HybridPropertyGroupConversion::MAX_COLUMNS;
    struct Member {
        uint64_t owner = 0;
        uint32_t token = 0;
        const graph::PropertyValue *value = nullptr;
        Source source = Source::HPS_EXTENSION;
        uint16_t column = 0;
    };
private:
    graph::query::execution::MaterializedByteReservation reservation_;
    graph::storage::HybridPropertyGroupConversion native_;
    GroupHeader group_;
    std::array<Member, MAX_MEMBERSHIPS> members_{};
    std::array<Column, MAX_COLUMNS> columns_{};
    size_t member_count_ = 0, column_count_ = 0;

    static graph::storage::EntityPropertyOwnerKind nativeKind(OwnerKind kind) {
        require(valid(kind));
        return kind == OwnerKind::HPK_NODE ? graph::storage::EntityPropertyOwnerKind::EPOK_NODE
            : graph::storage::EntityPropertyOwnerKind::EPOK_EDGE;
    }
    static auto reserve(graph::query::execution::MaterializedByteBudget &budget) {
        graph::query::execution::MaterializedByteReservation result(&budget);
        // Fixed holder plus column construction and bounded sorting stack
        // allowance. Payload bodies, native publication buffers and roots are
        // separate owners; this reservation does not pretend to cover them.
        constexpr size_t charge = sizeof(ReservedGroupConversion) + sizeof(Column) + 4096;
        static_assert(charge <= 1024 * 1024);
        result.set(charge); return result;
    }
public:
    ReservedGroupConversion(graph::query::execution::MaterializedByteBudget &budget,
        std::span<const graph::storage::PropertyCollectionRoot> roots, OwnerKind kind,
        uint64_t group, uint64_t generation, size_t nativeRecordLimit,
        const std::function<void()> &checkpoint = {}) : reservation_(reserve(budget)),
        // Adapter lifetime remains strictly inside the caller's budget lifetime;
        // production uses a genuinely owned resource, never this borrowed alias.
        native_(std::shared_ptr<std::pmr::memory_resource>(&budget, [](auto *) {}), roots,
            nativeKind(kind), group, graph::storage::Generation(generation), nativeRecordLimit, checkpoint) {
        const auto &groupRecord = native_.group();
        group_ = {kind, GroupMode::HPG_HYBRID, group, generation, groupRecord.live, groupRecord.memberships, groupRecord.columns};
        member_count_ = native_.members().size(); column_count_ = native_.columns().size();
        for (size_t i = 0; i < member_count_; ++i) {
            const auto &member = native_.members()[i];
            members_[i] = {member.owner, member.token, member.value,
                member.source == graph::storage::HybridPropertySource::HPS_COLUMN ? Source::HPS_COLUMN : Source::HPS_EXTENSION,
                member.column};
        }
        for (size_t i = 0; i < column_count_; ++i) {
            const auto &source = native_.columns()[i]; auto &column = columns_[i];
            column.identity = {kind, static_cast<ScalarKind>(source.identity.scalar_kind), group,
                source.identity.token, generation};
            column.presence = source.presence; column.nulls = source.nulls; column.live = source.live;
            column.words = source.words; column.validate();
        }
        group_.validate();
    }
    ReservedGroupConversion(const ReservedGroupConversion &) = delete;
    ReservedGroupConversion &operator=(const ReservedGroupConversion &) = delete;
    [[nodiscard]] const GroupHeader &group() const & noexcept { return group_; }
    const GroupHeader &group() const && = delete;
    [[nodiscard]] std::span<const Member> members() const & noexcept { return {members_.data(), member_count_}; }
    std::span<const Member> members() const && = delete;
    [[nodiscard]] std::span<const Column> columns() const & noexcept { return {columns_.data(), column_count_}; }
    std::span<const Column> columns() const && = delete;
    [[nodiscard]] size_t retainedCharge() const noexcept { return reservation_.bytes() + native_.retainedCharge(); }

    // Emit the native typed payload, never the laboratory magic. The output
    // buffer remains caller-owned/admitted. No PageId is allocated here and no
    // root is published; that belongs to the single metadata coordinator.
    [[nodiscard]] graph::storage::HybridPropertyGroupRecord nativeGroup() const {
        return native_.group();
    }
    void encodeNativeColumn(size_t ordinal, std::span<std::byte> output) const {
        native_.encodeNativeColumn(ordinal, output);
    }
    [[nodiscard]] std::pmr::vector<std::byte> encodeNativeInlineExtension(uint64_t owner, uint32_t token,
        size_t nativeRecordLimit, std::pmr::memory_resource &outputMemory) const {
        return native_.encodeNativeInlineExtension(owner, token, nativeRecordLimit, outputMemory);
    }

    // Reverse projection visits only actual owner fields in token order. Heap
    // values are borrowed without copying; primitive cells are reconstructed
    // from column bits. The synchronous visitor owns any copy/encoding charge.
    // Property-less live owners are valid, dead/out-of-group owners are not.
    void visitOwner(uint64_t owner, const std::function<void(uint32_t, const graph::PropertyValue &)> &visitor) const {
        native_.visitOwner(owner, visitor);
    }
};
} // namespace zyx::benchmark::subblock
