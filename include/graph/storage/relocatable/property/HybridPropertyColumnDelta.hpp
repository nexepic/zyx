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
        uint64_t set_live = 0;
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
        uint64_t requested_slots = 0, changed_slots = 0;
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
        bool eraseOwner, size_t nativeRecordLimit, bool pendingOwner = false) {
        const auto slot = hybridPropertySlotFor(owner);
        const auto bit = UINT64_C(1) << slot;
        const auto value = incoming(replacement);
        if (!eraseOwner && !pendingOwner && same(cellAt(state.source, slot), value)) return;
        state.changed_slots = bit;
        state.effects.memberships = int32_t(bool(replacement)) - int32_t(bool(state.source.presence & bit));
        state.effects.clear_live = eraseOwner ? bit : 0;
        state.effects.set_live = pendingOwner ? bit : 0;
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
            state->requested_slots = UINT64_C(1) << hybridPropertySlotFor(owner);
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
    // Missing memberships may join an existing group column. Authenticate the
    // absence in the owner map and the actual source column independently; a
    // disengaged result means there is no column for this token, not permission
    // to bypass a competing extension or dead owner.
    [[nodiscard]] static std::optional<HybridPropertyColumnDelta> planMissingColumnMember(
        const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot, EntityPropertyKey owner,
        uint32_t token, Generation nextGeneration, std::shared_ptr<std::pmr::memory_resource> memory,
        const PropertyValue &replacement) {
        return planAbsentColumnMember(context, snapshot, owner, token, nextGeneration, std::move(memory), replacement, false);
    }
    // Private images only: a pending logical owner has no Entity Map authority.
    // Publication additionally needs its leased Entity/field/liveness coordinator.
    [[nodiscard]] static std::optional<HybridPropertyColumnDelta> planPendingOwnerColumnMember(
        const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot, EntityPropertyKey owner,
        uint32_t token, Generation nextGeneration, std::shared_ptr<std::pmr::memory_resource> memory,
        const PropertyValue &replacement) {
        return planAbsentColumnMember(context, snapshot, owner, token, nextGeneration, std::move(memory), replacement, true);
    }
private:
    [[nodiscard]] static std::optional<HybridPropertyColumnDelta> planAbsentColumnMember(
        const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot, EntityPropertyKey owner,
        uint32_t token, Generation nextGeneration, std::shared_ptr<std::pmr::memory_resource> memory,
        const PropertyValue &replacement, bool pendingOwner) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native column insertion is disabled");
        const auto require = [](bool valid, const char *message) { hybrid_property_detail::require(valid, message); };
        require(context.usesSnapshot(snapshot) && memory && nextGeneration.value() > snapshot.superblock().generation.value() &&
            nextGeneration.value() - snapshot.superblock().generation.value() == 1, "native insertion has an invalid fence");
        utils::OwnedAllocationScope scope(memory);
        utils::OwnedHeapReservation boundaryCredit(memory), referenceCredit(memory);
        const auto boundary = [&](size_t bytes) { boundaryCredit.set(bytes); };
        const auto reference = [&](size_t bytes) { referenceCredit.set(bytes); };
        require(context.visitField(owner, token, [](const auto &) {}, [](const auto &) {}, std::ref(boundary),
            std::ref(reference)) == HybridPropertyReadContext::FieldRoute::HFR_MISSING,
            "native insertion requires an absent owner membership");
        boundaryCredit.set(0); referenceCredit.set(0);
        SlottedSlotReference groupRef;
        const auto group = context.findGroup(owner, std::ref(boundary), &groupRef);
        require(group && context.isLive(owner, *group) != pendingOwner,
            "native insertion has inconsistent existing/pending owner liveness");
        if (context.hasRoot(owner.owner_kind, 2)) {
            const auto extensionBinding = context.bind(owner.owner_kind, 2);
            HybridPropertyFieldDirectoryMap extensions(extensionBinding.root.index_id, context.trees_, context.metadata_, extensionBinding);
            require(!extensions.find(snapshot, HybridPropertyDirectoryCodec::ownerKey(owner, token), std::ref(boundary), context.cancellation_),
                "native insertion has a competing extension locator"); boundaryCredit.set(0);
        }
        if (!context.hasRoot(owner.owner_kind, 1)) return {};
        const auto binding = context.bind(owner.owner_kind, 1);
        HybridPropertyFieldDirectoryMap map(binding.root.index_id, context.trees_, context.metadata_, binding);
        const auto locator = map.find(snapshot, HybridPropertyDirectoryCodec::columnKey(group->group_id, token),
            std::ref(boundary), context.cancellation_); boundaryCredit.set(0);
        if (!locator) return {};
        const auto ref = HybridPropertyDirectoryCodec::decodeLocator(*locator);
        const auto page = context.fields_.read(snapshot, ref.page_id);
        const auto column = page.view().readColumn(ref, *group, token, context.descriptor_->semantic_generation);
        const auto bit = UINT64_C(1) << hybridPropertySlotFor(owner);
        require(!(column.presence & bit) && (!pendingOwner || !(column.live & bit)),
            "native insertion conflicts with source column presence or prior owner lifetime");
        Storage state(nullptr, Deleter{memory});
        state.reset(std::construct_at(static_cast<State *>(memory->allocate(sizeof(State), alignof(State)))));
        state->database_id = snapshot.superblock().database_id; state->source_generation = snapshot.superblock().generation;
        state->next_generation = nextGeneration; state->owner = owner; state->group_reference = groupRef;
        state->column_reference = ref; state->requested_slots = bit; state->group = *group;
        state->source = state->column = column;
        state->column.live |= bit;
        change(*state, owner, &replacement, false, RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, pendingOwner);
        context.check(); return HybridPropertyColumnDelta(std::move(state));
    }
public:
    // Coalesce authenticated changes to ONE physical column before applying
    // the 48-slot threshold. Individual plans alone cannot decide the final
    // layout of several removals. Replacement values retain their original
    // borrowed/accounted lifetime; callers must keep them alive through use.
    [[nodiscard]] static HybridPropertyColumnDelta mergeExistingColumnChanges(
        std::span<const HybridPropertyColumnDelta *const> changes,
        std::shared_ptr<std::pmr::memory_resource> memory,
        size_t nativeRecordLimit = RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES,
        const CancellationCheckpoint *cancellation = nullptr) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native column merge is disabled");
        hybrid_property_detail::require(bool(memory) && !changes.empty() && changes.size() <= RSS_HYBRID_GROUP_CAPACITY &&
            changes.front() && changes.front()->state_, "native column merge requires bounded authenticated plans and an account");
        utils::OwnedAllocationScope scope(memory);
        const auto check = [&] { if (cancellation) cancellation->check(); };
        check();
        const auto &first = *changes.front()->state_;
        Storage merged(nullptr, Deleter{memory});
        merged.reset(std::construct_at(static_cast<State *>(memory->allocate(sizeof(State), alignof(State)))));
        auto &state = *merged;
        state.database_id = first.database_id; state.source_generation = first.source_generation;
        state.next_generation = first.next_generation; state.owner = first.owner;
        state.group_reference = first.group_reference; state.column_reference = first.column_reference;
        state.group = first.group; state.source = state.column = first.source;
        for (unsigned i = 0; i < RSS_HYBRID_GROUP_CAPACITY; ++i) state.cells[i] = cellAt(state.source, i);
        for (const auto *change : changes) {
            check();
            hybrid_property_detail::require(change && change->state_, "native column merge has a null or moved plan");
            const auto &part = *change->state_;
            hybrid_property_detail::require(part.database_id == state.database_id &&
                part.source_generation == state.source_generation && part.next_generation == state.next_generation &&
                part.group_reference == state.group_reference && part.column_reference == state.column_reference &&
                part.group == state.group && part.source == state.source && part.requested_slots &&
                !(state.requested_slots & part.requested_slots),
                "native column merge has stale, foreign, inconsistent or overlapping plans");
            state.requested_slots |= part.requested_slots; state.changed_slots |= part.changed_slots;
            state.effects.clear_live |= part.effects.clear_live;
            state.effects.set_live |= part.effects.set_live;
            state.column.live |= part.column.live;
            for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot) {
                if (!(part.changed_slots & (UINT64_C(1) << slot))) continue;
                state.cells[slot] = part.action == Action::HCD_DEMOTE_FIELD ? part.cells[slot] : cellAt(part.column, slot);
            }
        }
        if (state.changed_slots) {
            auto &column = state.column;
            column.identity.semantic_generation = state.next_generation;
            hybrid_property_detail::require(!(state.effects.set_live & state.effects.clear_live),
                "native merged owner cannot be inserted and deleted together");
            column.live = (column.live | state.effects.set_live) & ~state.effects.clear_live;
            column.presence = column.nulls = 0; column.words.fill(0);
            bool compatible = true;
            for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot) {
                const auto bit = UINT64_C(1) << slot; const auto &cell = state.cells[slot];
                if (cell.kind == CellKind::HCC_MISSING) continue;
                hybrid_property_detail::require(bool(column.live & bit), "native merged field belongs to a deleted owner");
                column.presence |= bit;
                if (cell.kind == CellKind::HCC_NULL) column.nulls |= bit;
                else if (cell.kind == kind(column.identity.scalar_kind)) column.words[slot] = cell.bits;
                else compatible = false;
            }
            state.effects.memberships = int32_t(std::popcount(column.presence)) - int32_t(std::popcount(state.source.presence));
            if (compatible && std::popcount(column.presence) >= 48) {
                hybrid_property_detail::require(column.wireSize() <= nativeRecordLimit,
                    "native merged column exceeds its destination record limit");
                column.validate(); state.action = Action::HCD_REPLACE_COLUMN;
            } else { state.action = Action::HCD_DEMOTE_FIELD; state.effects.columns = -1; }
        }
        check();
        return HybridPropertyColumnDelta(std::move(merged));
    }
    [[nodiscard]] bool isValid() const noexcept { return bool(state_); }
    [[nodiscard]] Action action() const noexcept { return state_->action; }
    [[nodiscard]] GroupEffects effects() const noexcept { return state_->effects; }
    [[nodiscard]] DatabaseId databaseId() const noexcept { return state_->database_id; }
    [[nodiscard]] Generation sourceGeneration() const noexcept { return state_->source_generation; }
    [[nodiscard]] Generation nextGeneration() const noexcept { return state_->next_generation; }
    [[nodiscard]] EntityPropertyKey owner() const noexcept { return state_->owner; }
    [[nodiscard]] uint64_t requestedSlots() const noexcept { return state_->requested_slots; }
    [[nodiscard]] uint64_t changedSlots() const noexcept { return state_->changed_slots; }
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
