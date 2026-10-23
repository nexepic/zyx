/**
 * @file HybridPropertyGroupConversion.hpp
 * @brief Accounted, bounded conversion planning over authenticated ROW inputs.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/relocatable/property/HybridPropertyRecords.hpp"
#include "graph/storage/relocatable/property/HybridPropertyExtension.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"

namespace graph::storage {

// The enclosing metadata coordinator owns/authenticates the ROW roots and keeps
// them immutable and alive until this plan is destroyed. Extension values are
// borrowed, never fabricated LargeValue references. This plan allocates no IDs,
// installs no roots and grants no conversion/space/performance admission.
class HybridPropertyGroupConversion final {
public:
    static constexpr size_t MAX_MEMBERSHIPS = 4096;
    static constexpr size_t MAX_COLUMNS = MAX_MEMBERSHIPS / 48;
    struct Member {
        uint64_t owner = 0;
        uint32_t token = 0;
        const PropertyValue *value = nullptr;
        HybridPropertySource source = HybridPropertySource::HPS_EXTENSION;
        uint16_t column = 0;
    };
private:
    struct State {
        HybridPropertyGroupRecord group;
        std::array<Member, MAX_MEMBERSHIPS> members{};
        std::array<HybridPropertyColumnRecord, MAX_COLUMNS> columns{};
        size_t member_count = 0, column_count = 0;
        // Charge the bounded sorting/temporary column allowance while the plan
        // lives. Source roots, output encoding and publication have other owners.
        std::array<std::byte, 4096 + sizeof(HybridPropertyColumnRecord)> workspace{};
    };
    static_assert(sizeof(State) <= 1024 * 1024);
    struct Deleter {
        std::shared_ptr<std::pmr::memory_resource> memory;
        void operator()(State *state) const noexcept {
            if (state) { std::destroy_at(state); memory->deallocate(state, sizeof(State), alignof(State)); }
        }
    };
    std::unique_ptr<State, Deleter> state_;
    static void require(bool valid) {
        if (!valid) throw std::invalid_argument("invalid native hybrid group conversion input");
    }
    static std::optional<HybridPropertyScalarKind> primitive(const PropertyValue &value) {
        if (std::holds_alternative<int64_t>(value.getVariant())) return HybridPropertyScalarKind::HPSK_INT64;
        if (std::holds_alternative<double>(value.getVariant())) return HybridPropertyScalarKind::HPSK_DOUBLE;
        if (std::holds_alternative<bool>(value.getVariant())) return HybridPropertyScalarKind::HPSK_BOOL;
        if(const auto *text=std::get_if<std::string>(&value.getVariant());text && text->size()<=HybridPropertyShortText::MAX_BYTES)
            return HybridPropertyScalarKind::HPSK_SHORT_TEXT7;
        return {};
    }
    static bool isNull(const PropertyValue &value) {
        return std::holds_alternative<std::monostate>(value.getVariant());
    }
    static uint64_t bits(const PropertyValue &value, HybridPropertyScalarKind kind) {
        if (kind == HybridPropertyScalarKind::HPSK_INT64) return std::bit_cast<uint64_t>(std::get<int64_t>(value.getVariant()));
        if (kind == HybridPropertyScalarKind::HPSK_DOUBLE) return std::bit_cast<uint64_t>(std::get<double>(value.getVariant()));
        if(kind == HybridPropertyScalarKind::HPSK_SHORT_TEXT7)
            return *HybridPropertyShortText::encode(std::get<std::string>(value.getVariant()));
        return std::get<bool>(value.getVariant());
    }
public:
    HybridPropertyGroupConversion(std::shared_ptr<std::pmr::memory_resource> memory,
        std::span<const PropertyCollectionRoot> roots, EntityPropertyOwnerKind kind,
        uint64_t group, Generation generation, size_t nativeRecordLimit,
        const std::function<void()> &checkpoint = {}) : state_(nullptr, Deleter{std::move(memory)}) {
        require(bool(state_.get_deleter().memory));
        // Retain the exact allocation owner; allocation/refusal is exception safe
        // before any borrowed source is inspected or transformed.
        static_assert(std::is_nothrow_default_constructible_v<State>);
        state_.reset(std::construct_at(static_cast<State *>(state_.get_deleter().memory->allocate(sizeof(State), alignof(State)))));
        require(hybrid_property_detail::validKind(kind) && !roots.empty() && roots.size() <= 64 &&
            generation.isValid() && group && group <= RSS_HYBRID_MAX_GROUP_ID);
        if (nativeRecordLimit < 64) throw std::length_error("property conversion native group-header bound");
        auto &s = *state_; s.group = {kind, HybridPropertyGroupMode::HPGM_HYBRID, group, generation, 0, 0, 0};
        const uint32_t nativeKind = kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
        for (const auto &root : roots) {
            if (checkpoint) checkpoint();
            const EntityPropertyKey owner{kind, root.owner_entity_id};
            require(root.owner_entity_type == nativeKind && owner.isValid() && hybridPropertyGroupFor(owner) == group);
            const uint64_t bit = UINT64_C(1) << hybridPropertySlotFor(owner);
            require(!(s.group.live & bit)); s.group.live |= bit;
            if (root.entries.size() > MAX_MEMBERSHIPS - s.member_count)
                throw std::length_error("property conversion membership bound");
            uint32_t previous = 0;
            for (const auto &entry : root.entries) {
                require(entry.key_token > previous); previous = entry.key_token;
                s.members[s.member_count++] = {root.owner_entity_id, entry.key_token, &entry.value};
            }
        }
        const auto end = s.members.begin() + s.member_count;
        std::sort(s.members.begin(), end, [](const Member &a, const Member &b) {
            return a.token != b.token ? a.token < b.token : a.owner < b.owner;
        });
        for (size_t first = 0; first < s.member_count;) {
            if (checkpoint) checkpoint();
            size_t last = first + 1;
            while (last < s.member_count && s.members[last].token == s.members[first].token) ++last;
            std::optional<HybridPropertyScalarKind> scalar;
            bool compatible = last - first >= 48;
            for (size_t i = first; compatible && i < last; ++i) {
                const auto &value = *s.members[i].value;
                if (isNull(value)) continue;
                const auto current = primitive(value);
                if (!current || (scalar && current != scalar)) compatible = false;
                else scalar = current;
            }
            if (compatible && scalar) { // all-NULL new fields have no inferred type
                require(s.column_count < MAX_COLUMNS);
                auto &column = s.columns[s.column_count];
                column.identity = {kind, *scalar, group, s.members[first].token, generation};
                column.live = s.group.live;
                if (column.wireSize() > nativeRecordLimit || column.wireSize() > 4096)
                    throw std::length_error("property conversion native column bound");
                for (size_t i = first; i < last; ++i) {
                    auto &member = s.members[i];
                    const auto slot = hybridPropertySlotFor({kind, member.owner});
                    const uint64_t bit = UINT64_C(1) << slot;
                    column.presence |= bit;
                    if (isNull(*member.value)) column.nulls |= bit;
                    else column.words[slot] = bits(*member.value, *scalar);
                    member.source = HybridPropertySource::HPS_COLUMN; member.column = uint16_t(s.column_count);
                }
                column.validate(); ++s.column_count;
            }
            first = last;
        }
        std::sort(s.members.begin(), end, [](const Member &a, const Member &b) {
            return a.owner != b.owner ? a.owner < b.owner : a.token < b.token;
        });
        s.group.memberships = uint32_t(s.member_count); s.group.columns = uint32_t(s.column_count);
        s.group.validate(); if (checkpoint) checkpoint();
    }
    HybridPropertyGroupConversion(const HybridPropertyGroupConversion &) = delete;
    HybridPropertyGroupConversion &operator=(const HybridPropertyGroupConversion &) = delete;
    [[nodiscard]] const HybridPropertyGroupRecord &group() const & noexcept { return state_->group; }
    const HybridPropertyGroupRecord &group() const && = delete;
    [[nodiscard]] std::span<const Member> members() const & noexcept { return {state_->members.data(), state_->member_count}; }
    std::span<const Member> members() const && = delete;
    [[nodiscard]] std::span<const HybridPropertyColumnRecord> columns() const & noexcept {
        return {state_->columns.data(), state_->column_count};
    }
    std::span<const HybridPropertyColumnRecord> columns() const && = delete;
    [[nodiscard]] size_t retainedCharge() const noexcept { return sizeof(State); }
    [[nodiscard]] const std::shared_ptr<std::pmr::memory_resource> &memoryOwner() const & noexcept {
        return state_.get_deleter().memory;
    }
    const std::shared_ptr<std::pmr::memory_resource> &memoryOwner() const && = delete;
    void encodeNativeColumn(size_t ordinal, std::span<std::byte> output) const {
        require(ordinal < state_->column_count); state_->columns[ordinal].encode(output);
    }
    [[nodiscard]] std::pmr::vector<std::byte> encodeNativeInlineExtension(uint64_t owner, uint32_t token,
        size_t nativeRecordLimit, std::pmr::memory_resource &outputMemory) const {
        const auto members = this->members();
        const auto found = std::lower_bound(members.begin(), members.end(), std::pair(owner, token),
            [](const Member &member, const auto &key) { return std::pair(member.owner, member.token) < key; });
        require(found != members.end() && found->owner == owner && found->token == token &&
            found->source == HybridPropertySource::HPS_EXTENSION && found->value);
        return HybridPropertyExtensionView::encodeInline({group().owner_kind, owner}, token,
            group().semantic_generation, *found->value, nativeRecordLimit, outputMemory);
    }
    void visitOwner(uint64_t owner, const std::function<void(uint32_t, const PropertyValue &)> &visitor) const {
        const EntityPropertyKey key{group().owner_kind, owner};
        require(key.isValid() && hybridPropertyGroupFor(key) == group().group_id && visitor &&
            (group().live & (UINT64_C(1) << hybridPropertySlotFor(key))));
        const auto members = this->members();
        auto found = std::lower_bound(members.begin(), members.end(), owner,
            [](const Member &member, uint64_t id) { return member.owner < id; });
        for (; found != members.end() && found->owner == owner; ++found) {
            if (found->source == HybridPropertySource::HPS_EXTENSION) { visitor(found->token, *found->value); continue; }
            const auto &column = state_->columns[found->column]; const auto slot = hybridPropertySlotFor(key);
            const uint64_t bit = UINT64_C(1) << slot; PropertyValue value;
            require(column.presence & bit);
            if (!(column.nulls & bit)) {
                if (column.identity.scalar_kind == HybridPropertyScalarKind::HPSK_INT64)
                    value = PropertyValue(std::bit_cast<int64_t>(column.words[slot]));
                else if (column.identity.scalar_kind == HybridPropertyScalarKind::HPSK_DOUBLE)
                    value = PropertyValue(std::bit_cast<double>(column.words[slot]));
                else if(column.identity.scalar_kind == HybridPropertyScalarKind::HPSK_SHORT_TEXT7)
                    value=PropertyValue(HybridPropertyShortText::decode(column.words[slot]));
                else value = PropertyValue(bool(column.words[slot]));
            }
            visitor(found->token, value);
        }
    }
};
} // namespace graph::storage
