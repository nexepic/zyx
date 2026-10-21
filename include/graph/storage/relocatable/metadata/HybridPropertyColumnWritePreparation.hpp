/**
 * @file HybridPropertyColumnWritePreparation.hpp
 * @brief Coalesced private COW images for compatible native column updates.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <algorithm>
#include "graph/storage/relocatable/property/HybridPropertyColumnDelta.hpp"

namespace graph::storage {
// Private preparation only. Stable logical locators are preserved for these
// compatible replacements. The metadata coordinator must still reserve physical
// COW destinations and atomically update Page Directory, pool accounting,
// descriptor generation and retirement. It must fence the original database
// and metadata generation; no image here is a separately publishable ROW page.
class HybridPropertyColumnWritePreparation final {
    friend class RelocatableMetadataStore;
    struct Group {
        SlottedSlotReference reference;
        HybridPropertyGroupRecord source, replacement;
    };
    struct Column {
        SlottedSlotReference reference, group_reference;
        HybridPropertyColumnRecord source, replacement;
        uint64_t requested_slots = 0;
    };
    utils::OwnedVector<HybridPropertyPageImage> pages_;
    Generation source_generation_{}, next_generation_{};
public:
    HybridPropertyColumnWritePreparation(const MetadataSnapshotPin &snapshot,
        HybridPropertyFieldPageStore &fields, std::span<const HybridPropertyColumnDelta *const> changes,
        std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation = nullptr)
        : pages_(utils::OwnedPmrAllocator<HybridPropertyPageImage>(memory)),
          source_generation_(snapshot.superblock().generation) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native column COW preparation is disabled");
        hybrid_property_detail::require(bool(memory) && source_generation_.value() < (std::numeric_limits<uint64_t>::max)(),
            "native column COW preparation requires a bounded owner and next generation");
        next_generation_ = Generation(source_generation_.value() + 1);
        utils::OwnedAllocationScope scope(memory);
        const auto checkpoint = [&] { if (cancellation) cancellation->check(); };
        const auto require = [](bool value, const char *message) { hybrid_property_detail::require(value, message); };
        utils::OwnedVector<const HybridPropertyColumnDelta *> ordered{
            utils::OwnedPmrAllocator<const HybridPropertyColumnDelta *>(memory)};
        ordered.reserve(changes.size());
        for (const auto *change : changes) {
            checkpoint();
            require(change && change->isValid() && change->databaseId() == snapshot.superblock().database_id &&
                change->sourceGeneration() == source_generation_ && change->nextGeneration() == next_generation_,
                "native column COW plan has a foreign or stale source fence");
            if (change->action() == HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD)
                throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                    "native field demotion requires its atomic owner/extension directory coordinator");
            ordered.push_back(change);
        }
        std::sort(ordered.begin(), ordered.end(), [&](auto *left, auto *right) {
            checkpoint();
            if (left->owner().directoryId() != right->owner().directoryId())
                return left->owner().directoryId() < right->owner().directoryId();
            return left->sourceColumn().identity.token < right->sourceColumn().identity.token;
        });
        for (size_t i = 1; i < ordered.size(); ++i)
            require(ordered[i - 1]->owner() != ordered[i]->owner() ||
                ordered[i - 1]->sourceColumn().identity.token != ordered[i]->sourceColumn().identity.token,
                "native column COW requires one final change per owner/token");
        ordered.erase(std::remove_if(ordered.begin(), ordered.end(), [](auto *change) {
            return change->action() == HybridPropertyColumnDelta::Action::HCD_NO_CHANGE;
        }), ordered.end());
        if (ordered.empty()) return;

        std::sort(ordered.begin(), ordered.end(), [&](auto *left, auto *right) {
            checkpoint(); return left->groupReference() < right->groupReference();
        });
        size_t groupCount = 1;
        for (size_t i = 1; i < ordered.size(); ++i)
            groupCount += ordered[i - 1]->groupReference() != ordered[i]->groupReference();
        utils::OwnedVector<Group> groups{utils::OwnedPmrAllocator<Group>(memory)};
        groups.reserve(groupCount);
        for (const auto *change : ordered) {
            checkpoint();
            const auto effects = change->effects();
            require(!effects.memberships && !effects.columns && !effects.clear_live && !effects.set_live,
                "compatible column preparation cannot change group membership or liveness");
            if (groups.empty() || groups.back().reference != change->groupReference()) {
                auto group = change->sourceGroup(); group.semantic_generation = next_generation_;
                groups.push_back({change->groupReference(), change->sourceGroup(), group});
            } else require(groups.back().source == change->sourceGroup(), "native group plans disagree about source bytes");
        }
        std::sort(ordered.begin(), ordered.end(), [&](auto *left, auto *right) {
            checkpoint(); return left->columnReference() < right->columnReference();
        });
        size_t columnCount = 1;
        for (size_t i = 1; i < ordered.size(); ++i)
            columnCount += ordered[i - 1]->columnReference() != ordered[i]->columnReference();
        utils::OwnedVector<Column> columns{utils::OwnedPmrAllocator<Column>(memory)};
        columns.reserve(columnCount);
        for (const auto *change : ordered) {
            checkpoint();
            if (columns.empty() || columns.back().reference != change->columnReference()) {
                auto column = change->sourceColumn(); column.identity.semantic_generation = next_generation_;
                columns.push_back({change->columnReference(), change->groupReference(), change->sourceColumn(), column});
            } else require(columns.back().source == change->sourceColumn() &&
                columns.back().group_reference == change->groupReference(), "native column plans disagree about source bytes");
            auto &target = columns.back().replacement;
            require(!(columns.back().requested_slots & change->requestedSlots()),
                "native column plans overlap an owner/token");
            columns.back().requested_slots |= change->requestedSlots();
            const auto mask = change->changedSlots();
            const auto &replacement = change->replacementColumn();
            require(target.presence == replacement.presence && target.live == replacement.live,
                "compatible native column change modifies membership or liveness");
            target.nulls = (target.nulls & ~mask) | (replacement.nulls & mask);
            for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot)
                if (mask & (UINT64_C(1) << slot)) target.words[slot] = replacement.words[slot];
        }
        const auto groupFor = [&](const SlottedSlotReference &reference) -> const Group & {
            const auto found = std::lower_bound(groups.begin(), groups.end(), reference,
                [](const Group &group, const auto &key) { return group.reference < key; });
            require(found != groups.end() && found->reference == reference, "native COW column has no group plan");
            return *found;
        };
        for (const auto &column : columns)
            column.replacement.validateInGroup(groupFor(column.group_reference).replacement, next_generation_, next_generation_);
        if (groups.size() > (std::numeric_limits<size_t>::max)() - columns.size())
            throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "native COW source-page count overflowed");
        utils::OwnedVector<PageId> ids{utils::OwnedPmrAllocator<PageId>(memory)};
        ids.reserve(groups.size() + columns.size());
        for (const auto &group : groups) ids.push_back(group.reference.page_id);
        for (const auto &column : columns) ids.push_back(column.reference.page_id);
        std::sort(ids.begin(), ids.end()); ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        pages_.reserve(ids.size());
        for (const auto id : ids) {
            checkpoint(); pages_.push_back(fields.cloneForMutation(snapshot, id, next_generation_, memory)); checkpoint();
        }
        const auto image = [&](PageId id) -> HybridPropertyPageImage & {
            const auto found = std::lower_bound(ids.begin(), ids.end(), id);
            require(found != ids.end() && *found == id, "native COW source image is missing");
            return pages_[size_t(found - ids.begin())];
        };
        // Authenticate every original record before changing any private image.
        for (const auto &group : groups) {
            checkpoint();
            require(image(group.reference.page_id).readGroup(group.reference, group.source.owner_kind,
                group.source.group_id, source_generation_) == group.source, "native COW group bytes changed since planning");
        }
        for (const auto &column : columns) {
            checkpoint();
            require(image(column.reference.page_id).readColumn(column.reference, column.source.identity,
                groupFor(column.group_reference).source, source_generation_) == column.source,
                "native COW column bytes changed since planning");
        }
        for (const auto &group : groups) {
            checkpoint(); image(group.reference.page_id).replaceGroup(group.reference, group.replacement, next_generation_);
        }
        for (const auto &column : columns) {
            checkpoint(); image(column.reference.page_id).replaceColumn(column.reference, column.source.identity,
                column.replacement, groupFor(column.group_reference).replacement, next_generation_);
        }
        checkpoint();
    }
    HybridPropertyColumnWritePreparation(const HybridPropertyColumnWritePreparation &) = delete;
    HybridPropertyColumnWritePreparation &operator=(const HybridPropertyColumnWritePreparation &) = delete;
    HybridPropertyColumnWritePreparation(HybridPropertyColumnWritePreparation &&) noexcept = default;
    HybridPropertyColumnWritePreparation &operator=(HybridPropertyColumnWritePreparation &&) noexcept = default;
    [[nodiscard]] std::span<const HybridPropertyPageImage> pages() const & { return pages_; }
    std::span<const HybridPropertyPageImage> pages() const && = delete;
    [[nodiscard]] Generation sourceGeneration() const noexcept { return source_generation_; }
    [[nodiscard]] Generation nextGeneration() const noexcept { return next_generation_; }
};
} // namespace graph::storage
