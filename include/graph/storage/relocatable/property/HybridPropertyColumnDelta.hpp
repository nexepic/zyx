/**
 * @file HybridPropertyColumnDelta.hpp
 * @brief Accounted single-column changes from authenticated native fields.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <array>
#include <bit>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include "graph/storage/relocatable/property/HybridPropertyReadContext.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"

namespace graph::storage {
// A private write plan, not publication authority. The enclosing coordinator
// must fence database/source generation, merge group effects, update all map
// selectors/locators and reclaim old storage in the same metadata batch.
// Incoming complex values remain borrowed from that coordinator's immutable,
// accounted replacement root, which must outlive this plan. No source bytes,
// IDs, allocator entries or durable roots change while planning.
class HybridPropertyColumnDelta final {
public:
    enum class Action : uint8_t { HCD_NO_CHANGE = 1, HCD_REPLACE_COLUMN, HCD_DEMOTE_FIELD };
    enum class CellKind : uint8_t { HCC_MISSING = 1, HCC_NULL, HCC_INT64, HCC_DOUBLE, HCC_BOOL, HCC_EXTENSION };
    struct Cell {
        CellKind kind = CellKind::HCC_MISSING;
        uint64_t bits = 0;
        const PropertyValue *extension = nullptr;
    };
    struct GroupEffects {
        int32_t memberships = 0;
        int32_t columns = 0;
        uint64_t clear_live = 0;
    };
private:
    struct State {
        DatabaseId database_id{};
        Generation source_generation{}, next_generation{};
        EntityPropertyKey owner;
        SlottedSlotReference group_reference, column_reference;
        HybridPropertyGroupRecord group;
        HybridPropertyColumnRecord source, column;
        std::array<Cell, RSS_HYBRID_GROUP_CAPACITY> cells{};
        Action action = Action::HCD_NO_CHANGE;
        GroupEffects effects;
    };
    struct Deleter {
        std::shared_ptr<std::pmr::memory_resource> memory;
        void operator()(State *state) const noexcept {
            if (state) { std::destroy_at(state); memory->deallocate(state, sizeof(State), alignof(State)); }
        }
    };
    using Storage = std::unique_ptr<State, Deleter>;
    explicit HybridPropertyColumnDelta(Storage state) : state_(std::move(state)) {}
    static CellKind kind(HybridPropertyScalarKind value) {
        switch (value) {
            case HybridPropertyScalarKind::HPSK_INT64: return CellKind::HCC_INT64;
            case HybridPropertyScalarKind::HPSK_DOUBLE: return CellKind::HCC_DOUBLE;
            case HybridPropertyScalarKind::HPSK_BOOL: return CellKind::HCC_BOOL;
            default: throw FormatException(FormatError::FERR_INVALID_ENUM, "unknown native column type");
        }
    }
    static Cell cellAt(const HybridPropertyColumnRecord &source, unsigned slot) {
        const auto bit = UINT64_C(1) << slot;
        if (!(source.presence & bit)) return {};
        if (source.nulls & bit) return {CellKind::HCC_NULL};
        return {kind(source.identity.scalar_kind), source.words[slot]};
    }
    static Cell incoming(const PropertyValue *value) {
        if (!value) return {};
        const auto &data = value->getVariant();
        if (std::holds_alternative<std::monostate>(data)) return {CellKind::HCC_NULL};
        if (const auto *v = std::get_if<int64_t>(&data)) return {CellKind::HCC_INT64, std::bit_cast<uint64_t>(*v)};
        if (const auto *v = std::get_if<double>(&data)) return {CellKind::HCC_DOUBLE, std::bit_cast<uint64_t>(*v)};
        if (const auto *v = std::get_if<bool>(&data)) return {CellKind::HCC_BOOL, uint64_t(*v)};
        return {CellKind::HCC_EXTENSION, 0, value};
    }
    static bool same(Cell left, Cell right) {
        if (left.kind != right.kind) return false;
        // Match existing ROW PropertyValue equality: +0 == -0, NaN != NaN.
        if (left.kind == CellKind::HCC_DOUBLE)
            return std::bit_cast<double>(left.bits) == std::bit_cast<double>(right.bits);
        return left.kind != CellKind::HCC_EXTENSION && left.bits == right.bits;
    }
    static void change(State &state, EntityPropertyKey owner, const PropertyValue *replacement,
        bool eraseOwner, size_t nativeRecordLimit) {
        const auto slot = hybridPropertySlotFor(owner);
        const auto bit = UINT64_C(1) << slot;
        const auto value = incoming(replacement);
        if (!eraseOwner && same(cellAt(state.source, slot), value)) return;
        state.effects.memberships = replacement ? 0 : -1;
        state.effects.clear_live = eraseOwner ? bit : 0;
        auto &column = state.column;
        column.identity.semantic_generation = state.next_generation;
        if (eraseOwner) column.live &= ~bit;
        column.presence &= ~bit; column.nulls &= ~bit; column.words[slot] = 0;
        if (replacement) column.presence |= bit;
        if (value.kind == CellKind::HCC_NULL) column.nulls |= bit;
        const bool compatible = value.kind == CellKind::HCC_MISSING || value.kind == CellKind::HCC_NULL ||
            value.kind == kind(column.identity.scalar_kind);
        if (compatible && std::popcount(column.presence) >= 48) {
            column.words[slot] = value.bits;
            if (column.wireSize() > nativeRecordLimit)
                throw FormatException(FormatError::FERR_INVALID_STATE, "native column replacement exceeds its destination record limit");
            column.validate(); state.action = Action::HCD_REPLACE_COLUMN;
        } else {
            state.action = Action::HCD_DEMOTE_FIELD; state.effects.columns = -1;
            for (unsigned i = 0; i < RSS_HYBRID_GROUP_CAPACITY; ++i) state.cells[i] = cellAt(state.source, i);
            state.cells[slot] = value;
        }
    }
public:
    HybridPropertyColumnDelta(const HybridPropertyColumnDelta &) = delete;
    HybridPropertyColumnDelta &operator=(const HybridPropertyColumnDelta &) = delete;
    HybridPropertyColumnDelta(HybridPropertyColumnDelta &&) noexcept = default;
    HybridPropertyColumnDelta &operator=(HybridPropertyColumnDelta &&) noexcept = default;

    // Disengaged means ROW, missing membership or an extension, for which the
    // coordinator must retain its separate write route. A NULL pointer removes
    // the field; a pointer to PropertyValue(NULL) stores NULL. There is no
    // implicit promotion or whole-group reconstruction in this operation.
    [[nodiscard]] static std::optional<HybridPropertyColumnDelta> planExistingColumn(
        const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot, EntityPropertyKey owner,
        uint32_t token, Generation nextGeneration, std::shared_ptr<std::pmr::memory_resource> memory,
        const PropertyValue *replacement, bool eraseOwner = false,
        size_t nativeRecordLimit = RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native column mutation is disabled");
        hybrid_property_detail::require(context.usesSnapshot(snapshot) && bool(memory) &&
            snapshot.superblock().generation.value() < (std::numeric_limits<uint64_t>::max)() &&
            nextGeneration.value() == snapshot.superblock().generation.value() + 1 &&
            (!eraseOwner || !replacement), "native column mutation requires one fenced next generation and account");
        utils::OwnedAllocationScope scope(memory);
        utils::OwnedHeapReservation boundaryCredit(memory), referenceCredit(memory);
        const auto boundary = [&](size_t bytes) { boundaryCredit.set(bytes); };
        const auto reference = [&](size_t bytes) { boundaryCredit.set(0); referenceCredit.set(bytes); };
        std::optional<HybridPropertyColumnDelta> result;
        const auto column = [&](const HybridPropertyGroupRecord &group, const HybridPropertyColumnRecord &source,
            const SlottedSlotReference &groupReference, const SlottedSlotReference &columnReference) {
            boundaryCredit.set(0);
            static_assert(std::is_nothrow_default_constructible_v<State>);
            Storage state(nullptr, Deleter{memory});
            state.reset(std::construct_at(static_cast<State *>(memory->allocate(sizeof(State), alignof(State)))));
            state->database_id = snapshot.superblock().database_id;
            state->source_generation = snapshot.superblock().generation; state->next_generation = nextGeneration;
            state->owner = owner; state->group_reference = groupReference; state->column_reference = columnReference;
            state->group = group; state->source = state->column = source;
            change(*state, owner, replacement, eraseOwner, nativeRecordLimit);
            result = HybridPropertyColumnDelta(std::move(state));
        };
        const auto ignoreScalar = [](const HybridPropertyFieldView::ScalarCell &) {};
        const auto ignoreExtension = [](const HybridPropertyExtensionView &) {};
        static_cast<void>(context.visitField(owner, token, std::ref(ignoreScalar), std::ref(ignoreExtension),
            std::ref(boundary), std::ref(reference), std::ref(column)));
        return result;
    }
    [[nodiscard]] Action action() const noexcept { return state_->action; }
    [[nodiscard]] GroupEffects effects() const noexcept { return state_->effects; }
    [[nodiscard]] DatabaseId databaseId() const noexcept { return state_->database_id; }
    [[nodiscard]] Generation sourceGeneration() const noexcept { return state_->source_generation; }
    [[nodiscard]] Generation nextGeneration() const noexcept { return state_->next_generation; }
    [[nodiscard]] EntityPropertyKey owner() const noexcept { return state_->owner; }
    [[nodiscard]] SlottedSlotReference groupReference() const noexcept { return state_->group_reference; }
    [[nodiscard]] SlottedSlotReference columnReference() const noexcept { return state_->column_reference; }
    [[nodiscard]] const HybridPropertyGroupRecord &sourceGroup() const & { return state_->group; }
    const HybridPropertyGroupRecord &sourceGroup() const && = delete;
    [[nodiscard]] const HybridPropertyColumnRecord &sourceColumn() const & { return state_->source; }
    const HybridPropertyColumnRecord &sourceColumn() const && = delete;
    [[nodiscard]] const HybridPropertyColumnRecord &replacementColumn() const & {
        hybrid_property_detail::require(state_->action == Action::HCD_REPLACE_COLUMN, "native delta has no replacement column");
        return state_->column;
    }
    const HybridPropertyColumnRecord &replacementColumn() const && = delete;
    [[nodiscard]] std::span<const Cell> demotedCells() const & {
        hybrid_property_detail::require(state_->action == Action::HCD_DEMOTE_FIELD, "native delta has no demotion cells");
        return state_->cells;
    }
    std::span<const Cell> demotedCells() const && = delete;
private:
    Storage state_;
};
} // namespace graph::storage
