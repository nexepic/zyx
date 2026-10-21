/**
 * @file HybridPropertyDemotionPreparation.hpp
 * @brief Same-batch private images and directory edits for mixed group mutations.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <algorithm>
#include <array>
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/storage/relocatable/property/HybridPropertyColumnDelta.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPageImage.hpp"

namespace graph::storage {
// This is bounded private group lifecycle preparation, not a publication entry point.
// The owning writer must merge these edits with every other final mutation,
// register COW locations, retire superseded frames and publish one descriptor.
// A failure discards the enclosing batch, including its private logical IDs.
class HybridPropertyDemotionPreparation final {
    friend class RelocatableMetadataStore;
    utils::OwnedVector<HybridPropertyPageImage> pages_;
    utils::OwnedVector<HybridPropertyOwnerMap::Entry> owners_;
    utils::OwnedVector<HybridPropertyOwnerMap::Key> erased_owners_;
    utils::OwnedVector<HybridPropertyFieldDirectoryMap::Entry> extensions_;
    utils::OwnedVector<HybridPropertyFieldDirectoryMap::Key> erased_columns_;
    utils::OwnedVector<HybridPropertyFieldDirectoryMap::Key> erased_extensions_;
    utils::OwnedVector<HybridPropertyGroupDirectoryMap::Key> erased_groups_;
    struct NamespaceEdits {
        utils::OwnedVector<HybridPropertyOwnerMap::Entry> owners;
        utils::OwnedVector<HybridPropertyOwnerMap::Key> erased_owners;
        utils::OwnedVector<HybridPropertyFieldDirectoryMap::Entry> extensions;
        utils::OwnedVector<HybridPropertyFieldDirectoryMap::Key> erased_columns, erased_extensions;
        utils::OwnedVector<HybridPropertyGroupDirectoryMap::Key> erased_groups;
        explicit NamespaceEdits(const std::shared_ptr<std::pmr::memory_resource> &memory)
            : owners(utils::OwnedPmrAllocator<HybridPropertyOwnerMap::Entry>(memory)),
              erased_owners(utils::OwnedPmrAllocator<HybridPropertyOwnerMap::Key>(memory)),
              extensions(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Entry>(memory)),
              erased_columns(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Key>(memory)),
              erased_extensions(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Key>(memory)),
              erased_groups(utils::OwnedPmrAllocator<HybridPropertyGroupDirectoryMap::Key>(memory)) {}
    };
    NamespaceEdits other_;
    HybridPropertyGroupRecord group_;
    bool group_erased_ = false;
    template<class Image>
    static void synchronizeMembershipLiveness(const HybridPropertyGroupRecord &previous,
        const HybridPropertyGroupRecord &replacement, Generation generation, Image &&image) {
        if (previous.mode != HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP || previous.live == replacement.live) return;
        hybrid_property_detail::require(replacement.mode == previous.mode && replacement.column_membership == previous.column_membership &&
            replacement.columns == previous.columns, "native liveness update changes compact membership ownership");
        const auto reference = previous.column_membership;
        auto matrix = image(reference.page_id).readColumnMembership(reference, previous, generation);
        matrix.live = replacement.live; matrix.semantic_generation = generation;
        // validateInGroup refuses a deleted row whose actual field removals
        // were omitted; this is not permission to silently clear memberships.
        image(reference.page_id).replaceColumnMembership(reference, matrix, previous, replacement, generation);
    }
    explicit HybridPropertyDemotionPreparation(std::shared_ptr<std::pmr::memory_resource> memory)
        : pages_(utils::OwnedPmrAllocator<HybridPropertyPageImage>(memory)),
          owners_(utils::OwnedPmrAllocator<HybridPropertyOwnerMap::Entry>(memory)),
          erased_owners_(utils::OwnedPmrAllocator<HybridPropertyOwnerMap::Key>(memory)),
          extensions_(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Entry>(memory)),
          erased_columns_(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Key>(memory)),
          erased_extensions_(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Key>(memory)),
          erased_groups_(utils::OwnedPmrAllocator<HybridPropertyGroupDirectoryMap::Key>(memory)), other_(memory) {}
    auto &ownersFor(EntityPropertyOwnerKind kind) {
        return kind == group_.owner_kind ? owners_ : other_.owners;
    }
    auto &erasedOwnersFor(EntityPropertyOwnerKind kind) {
        return kind == group_.owner_kind ? erased_owners_ : other_.erased_owners;
    }
    auto &extensionsFor(EntityPropertyOwnerKind kind) {
        return kind == group_.owner_kind ? extensions_ : other_.extensions;
    }
    auto &erasedColumnsFor(EntityPropertyOwnerKind kind) {
        return kind == group_.owner_kind ? erased_columns_ : other_.erased_columns;
    }
    auto &erasedExtensionsFor(EntityPropertyOwnerKind kind) {
        return kind == group_.owner_kind ? erased_extensions_ : other_.erased_extensions;
    }
    auto &erasedGroupsFor(EntityPropertyOwnerKind kind) {
        return kind == group_.owner_kind ? erased_groups_ : other_.erased_groups;
    }
public:
    using LargeFieldResolver = std::function<std::pmr::vector<std::byte>(EntityPropertyKey, uint32_t, const PropertyValue &)>;
    HybridPropertyDemotionPreparation(MetadataWriteBatch &batch, const MetadataSnapshotPin &snapshot,
        HybridPropertyFieldPageStore &fields, const HybridPropertyColumnDelta &change,
        std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation = nullptr, const LargeFieldResolver &largeField = {})
        : HybridPropertyDemotionPreparation(batch, snapshot, fields,
            std::array<const HybridPropertyColumnDelta *, 1>{&change},
            std::move(memory), cancellation, largeField) {}
    HybridPropertyDemotionPreparation(MetadataWriteBatch &batch, const MetadataSnapshotPin &snapshot,
        HybridPropertyFieldPageStore &fields, std::span<const HybridPropertyColumnDelta *const> changes,
        std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation = nullptr, const LargeFieldResolver &largeField = {})
        : HybridPropertyDemotionPreparation(batch, snapshot, fields, changes,
            std::move(memory), cancellation, largeField, false) {}
private:
    HybridPropertyDemotionPreparation(MetadataWriteBatch &batch, const MetadataSnapshotPin &snapshot,
        HybridPropertyFieldPageStore &fields, std::span<const HybridPropertyColumnDelta *const> changes,
        std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation, const LargeFieldResolver &largeField, bool wholeOwner,
        std::span<const EntityPropertyKey> pendingOwners = {}, bool allowMultipleGroups = false)
        : HybridPropertyDemotionPreparation(memory) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native field demotion is disabled");
        const auto require = [](bool valid, const char *message) { hybrid_property_detail::require(valid, message); };
        require(bool(memory) && snapshot.superblock() == batch.current() && !changes.empty() && changes.size() <= (wholeOwner ? 4096U / 48U : 64U),
            "native mixed group preparation exceeds its exact batch or final column work bound");
        utils::OwnedAllocationScope scope(memory);
        const auto check = [&] { if (cancellation) cancellation->check(); };
        check();
        const auto *first = changes.front();
        require(first && first->isValid(), "native mixed group preparation received a null plan");
        group_ = first->sourceGroup();
        require(pendingOwners.size() <= 64U, "native pending owner set exceeds its operation bound");
        const auto pendingMask = [&](const HybridPropertyGroupRecord &group) {
            uint64_t mask = 0;
            for (const auto owner : pendingOwners)
                if (owner.owner_kind == group.owner_kind && hybridPropertyGroupFor(owner) == group.group_id)
                    mask |= UINT64_C(1) << hybridPropertySlotFor(owner);
            require(!(group.live & mask), "native pending owner is already live in its source group"); return mask;
        };
        struct GroupWork {
            SlottedSlotReference reference;
            HybridPropertyGroupRecord source, replacement;
            int64_t memberships = 0, columns = 0;
            bool dirty = false, matrix_changed = false;
        };
        utils::OwnedVector<GroupWork> groups{utils::OwnedPmrAllocator<GroupWork>(memory)};
        utils::OwnedVector<const HybridPropertyColumnDelta *> ordered{
            utils::OwnedPmrAllocator<const HybridPropertyColumnDelta *>(memory)};
        ordered.reserve(changes.size());
        size_t emittedMembers = 0; std::array<size_t, 2> emittedByKind{}, erasedByKind{};
        const auto kindIndex = [](EntityPropertyOwnerKind kind) { return kind == EntityPropertyOwnerKind::EPOK_NODE ? 0U : 1U; };
        for (const auto *plan : changes) {
            check();
            require(plan && plan->isValid() && plan->databaseId() == batch.current().database_id &&
                plan->sourceGeneration() == batch.current().generation && plan->nextGeneration() == batch.generation() &&
                (allowMultipleGroups || (plan->groupReference() == first->groupReference() && plan->sourceGroup() == group_)),
                "native mixed group plans have inconsistent source fences or group bytes");
            auto group = std::find_if(groups.begin(), groups.end(), [&](const auto &candidate) { return candidate.reference == plan->groupReference(); });
            if (group == groups.end()) {
                require(std::none_of(groups.begin(), groups.end(), [&](const auto &candidate) { return candidate.source.owner_kind == plan->sourceGroup().owner_kind && candidate.source.group_id == plan->sourceGroup().group_id; }),
                    "native mixed group has conflicting source locators");
                groups.push_back({plan->groupReference(), plan->sourceGroup(), plan->sourceGroup()}); group = std::prev(groups.end());
            } else require(group->source == plan->sourceGroup(), "native mixed group source records disagree");
            const auto effects = plan->effects();
            require(wholeOwner || !effects.clear_live, "native owner deletion requires whole-owner coordination");
            require(!(effects.set_live & ~pendingMask(group->source)), "pending owner insertion requires its declared Entity/field/liveness coordinator");
            if (plan->action() == HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD) {
                const auto members = int64_t(std::popcount(plan->sourceColumn().presence)) + effects.memberships;
                require(effects.columns == -1 && members >= 0 && members <= 64,
                    "invalid native demotion effects");
                emittedMembers += size_t(members); emittedByKind[kindIndex(group->source.owner_kind)] += size_t(members);
                require(emittedMembers <= 4096U, "native demotion exceeds4096actual emitted members");
            } else require(!effects.columns && effects.memberships >= -64 && effects.memberships <= 64,
                "invalid compatible native column effects");
            if (group->source.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP)
                group->matrix_changed |= bool(pendingMask(group->source)) || plan->action() == HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD ||
                    (plan->action() != HybridPropertyColumnDelta::Action::HCD_NO_CHANGE &&
                        plan->action() != HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD &&
                        plan->sourceColumn().presence != plan->replacementColumn().presence);
            group->memberships += effects.memberships; group->columns += effects.columns;
            group->dirty |= plan->action() != HybridPropertyColumnDelta::Action::HCD_NO_CHANGE || pendingMask(group->source);
            ordered.push_back(plan);
        }
        std::sort(ordered.begin(), ordered.end(), [](auto *left, auto *right) {
            if (left->groupReference() != right->groupReference()) return left->groupReference() < right->groupReference();
            return left->sourceColumn().identity.token < right->sourceColumn().identity.token;
        });
        for (size_t i = 1; i < ordered.size(); ++i)
            require(ordered[i - 1]->groupReference() != ordered[i]->groupReference() ||
                ordered[i - 1]->sourceColumn().identity.token != ordered[i]->sourceColumn().identity.token,
                "native mixed group requires one merged final plan per column");
        ordered.erase(std::remove_if(ordered.begin(), ordered.end(), [](auto *plan) {
            return plan->action() == HybridPropertyColumnDelta::Action::HCD_NO_CHANGE;
        }), ordered.end());
        if (ordered.empty() && pendingOwners.empty()) return;
        for (auto &group : groups) {
            const auto memberships = int64_t(group.source.memberships) + group.memberships;
            const auto columns = int64_t(group.source.columns) + group.columns;
            require(memberships >= 0 && memberships <= UINT32_MAX && columns >= 0 && columns <= UINT32_MAX,
                "native mixed group effects underflow or overflow");
            if (!group.dirty) continue;
            group.replacement.memberships = uint32_t(memberships); group.replacement.columns = uint32_t(columns);
            group.replacement.live |= pendingMask(group.source); group.replacement.semantic_generation = batch.generation();
            if (group.source.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP && !columns) {
                group.replacement.mode = HybridPropertyGroupMode::HPGM_HYBRID;
                group.replacement.column_membership = {};
            }
            group.replacement.validate();
            if (memberships < group.source.memberships && group.source.mode != HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) erasedByKind[kindIndex(group.source.owner_kind)] += size_t(group.source.memberships - memberships);
        }
        const auto groupFor = [&](const SlottedSlotReference &reference) -> const GroupWork & {
            const auto group = std::find_if(groups.begin(), groups.end(), [&](const auto &candidate) { return candidate.reference == reference; });
            require(group != groups.end(), "native column has no authenticated source group"); return *group;
        };
        group_ = groupFor(first->groupReference()).replacement;
        utils::OwnedVector<PageId> sourceIds{utils::OwnedPmrAllocator<PageId>(memory)};
        sourceIds.reserve(groups.size() + ordered.size());
        for (const auto &group : groups) if (group.dirty) sourceIds.push_back(group.reference.page_id);
        for (const auto &group : groups) if (group.matrix_changed) sourceIds.push_back(group.source.column_membership.page_id);
        for (const auto *plan : ordered) sourceIds.push_back(plan->columnReference().page_id);
        std::sort(sourceIds.begin(), sourceIds.end());
        sourceIds.erase(std::unique(sourceIds.begin(), sourceIds.end()), sourceIds.end());
        const auto sourcePages = sourceIds.size(); pages_.reserve(sourcePages);
        for (const auto id : sourceIds) { check(); pages_.push_back(fields.cloneForMutation(snapshot, id, batch.generation(), memory)); }
        const auto image = [&](PageId id) -> HybridPropertyPageImage & {
            const auto found = std::lower_bound(sourceIds.begin(), sourceIds.end(), id);
            require(found != sourceIds.end() && *found == id, "native mixed source page missing"); return pages_[size_t(found - sourceIds.begin())];
        };
        for (const auto &group : groups) if (group.dirty)
            require(image(group.reference.page_id).readGroup(group.reference, group.source.owner_kind, group.source.group_id,
                batch.generation()) == group.source, "native mixed group source changed");
        for (const auto *plan : ordered) {
            check(); const auto &group = groupFor(plan->groupReference());
            require(image(plan->columnReference().page_id).readColumn(plan->columnReference(),
                plan->sourceColumn().identity, group.source, batch.generation()) == plan->sourceColumn(), "native mixed column source changed");
        }
        // Derive the final bitmap from authenticated private source images;
        // no database-sized selector list or second source copy is retained.
        for (const auto &group : groups) if (group.matrix_changed) {
            const auto reference = group.source.column_membership;
            auto matrix = image(reference.page_id).readColumnMembership(reference, group.source, batch.generation());
            matrix.live = group.replacement.live;
            for (const auto *plan : ordered) if (plan->groupReference() == group.reference) {
                require(matrix.presence(plan->sourceColumn().identity.token) == plan->sourceColumn().presence,
                    "compact mutation source presence differs from its actual column");
                if (plan->action() == HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD)
                    matrix.eraseColumn(plan->sourceColumn().identity.token);
                else matrix.setPresence(plan->sourceColumn().identity.token, plan->replacementColumn().presence);
            }
            require(matrix.column_count == group.replacement.columns, "compact mutation lost its final column count");
            if (matrix.column_count) {
                matrix.semantic_generation = batch.generation();
                image(reference.page_id).replaceColumnMembership(reference, matrix, group.source, group.replacement, batch.generation());
            } else image(reference.page_id).erase(reference);
        }
        // Authenticate all groups and columns before changing each shared page once.
        for (const auto &group : groups) if (group.dirty)
            image(group.reference.page_id).replaceGroup(group.reference, group.replacement, batch.generation());
        for (const auto kind : {EntityPropertyOwnerKind::EPOK_NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
            ownersFor(kind).reserve(emittedByKind[kindIndex(kind)]); extensionsFor(kind).reserve(emittedByKind[kindIndex(kind)]);
            erasedOwnersFor(kind).reserve(erasedByKind[kindIndex(kind)]);
        }
        for (const auto *plan : ordered) {
            check();
            const auto &change = *plan;
            const auto &targetGroup = groupFor(plan->groupReference()).replacement;
            if (change.action() == HybridPropertyColumnDelta::Action::HCD_NO_CHANGE) continue;
            if (change.action() != HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD) {
                const auto &replacement = change.replacementColumn();
                image(change.columnReference().page_id).replaceColumn(change.columnReference(),
                    change.sourceColumn().identity, replacement, targetGroup, batch.generation());
                if (change.sourceGroup().mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) continue;
                const auto added = replacement.presence & ~change.sourceColumn().presence;
                for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot)
                    if (added & (UINT64_C(1) << slot)) {
                        const EntityPropertyKey owner{targetGroup.owner_kind,
                            (targetGroup.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + slot + 1U};
                        ownersFor(targetGroup.owner_kind).push_back({HybridPropertyDirectoryCodec::ownerKey(owner, replacement.identity.token),
                            HybridPropertyDirectoryCodec::sourceValue(HybridPropertySource::HPS_COLUMN)});
                    }
                const auto removed = change.sourceColumn().presence & ~replacement.presence;
                for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot)
                    if (removed & (UINT64_C(1) << slot)) {
                        const EntityPropertyKey owner{targetGroup.owner_kind,
                            (targetGroup.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + slot + 1U};
                        erasedOwnersFor(targetGroup.owner_kind).push_back(HybridPropertyDirectoryCodec::ownerKey(owner, replacement.identity.token));
                    }
                continue;
            }
            image(change.columnReference().page_id).erase(change.columnReference());
            const auto token = change.sourceColumn().identity.token;
            erasedColumnsFor(targetGroup.owner_kind).push_back(HybridPropertyDirectoryCodec::columnKey(targetGroup.group_id, token));
            const auto cells = change.demotedCells();
            for (unsigned slot = 0; slot < cells.size(); ++slot) {
                check();
                const EntityPropertyKey owner{targetGroup.owner_kind, (targetGroup.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + slot + 1U};
                const auto &cell = cells[slot];
                if (cell.kind == HybridPropertyColumnDelta::CellKind::HCC_MISSING) {
                    if (change.sourceGroup().mode != HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP &&
                        (change.sourceColumn().presence & (UINT64_C(1) << slot)))
                        erasedOwnersFor(targetGroup.owner_kind).push_back(HybridPropertyDirectoryCodec::ownerKey(owner, token));
                    continue;
                }
                PropertyValue scalar;
                const PropertyValue *value = &scalar;
                switch (cell.kind) {
                    case HybridPropertyColumnDelta::CellKind::HCC_NULL: break;
                    case HybridPropertyColumnDelta::CellKind::HCC_INT64: scalar = PropertyValue(std::bit_cast<int64_t>(cell.bits)); break;
                    case HybridPropertyColumnDelta::CellKind::HCC_DOUBLE: scalar = PropertyValue(std::bit_cast<double>(cell.bits)); break;
                    case HybridPropertyColumnDelta::CellKind::HCC_BOOL: scalar = PropertyValue(bool(cell.bits)); break;
                    case HybridPropertyColumnDelta::CellKind::HCC_EXTENSION:
                        require(cell.extension != nullptr, "native demotion lost its borrowed replacement"); value = cell.extension; break;
                    default: require(false, "unknown native demotion cell");
                }
                const auto size = CanonicalPropertyValueCodec::encodedSize(*value);
                const bool oversized = size > RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES - RSS_HYBRID_EXTENSION_HEADER_BYTES;
                auto bytes = oversized && largeField ? largeField(owner, token, *value) :
                    HybridPropertyExtensionView::encodeInline(owner, token, batch.generation(), *value,
                        RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, *memory);
                // New extension pages are shared across every demoted field, never
                // one tail per owner; existing unrelated records stay byte-identical.
                if (pages_.size() == sourcePages || pages_.back().stats().contiguous_free_bytes < bytes.size() + RSS_SLOTTED_PAGE_SLOT_BYTES) {
                    if (pages_.size() == pages_.capacity()) pages_.reserve(pages_.capacity() * 2U);
                    const auto id = batch.allocateLogicalPageId();
                    pages_.push_back(HybridPropertyPageImage::create(id, Generation(1), batch.generation(), memory));
                }
                utils::OwnedHeapReservation referenceCredit(memory);
                const auto admitReference = [&](size_t retained) { referenceCredit.set(retained); };
                const auto locator = pages_.back().appendExtension(bytes, owner, token, targetGroup, batch.generation(), std::ref(admitReference));
                const auto key = HybridPropertyDirectoryCodec::ownerKey(owner, token);
                ownersFor(targetGroup.owner_kind).push_back({key, HybridPropertyDirectoryCodec::sourceValue(HybridPropertySource::HPS_EXTENSION)});
                extensionsFor(targetGroup.owner_kind).push_back({key, HybridPropertyDirectoryCodec::locatorValue(locator)});
            }
        }
        // Node and Edge keys can have the same bytes; each directory namespace
        // keeps its own sorted edits while all kinds share one private page set.
        const auto entryLess = [&](const auto &left, const auto &right) { check(); return left.key < right.key; };
        for (const auto kind : {EntityPropertyOwnerKind::EPOK_NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
            auto &owners = ownersFor(kind); auto &extensions = extensionsFor(kind);
            auto &erasedOwners = erasedOwnersFor(kind); auto &erasedColumns = erasedColumnsFor(kind);
            std::sort(owners.begin(), owners.end(), entryLess); std::sort(extensions.begin(), extensions.end(), entryLess);
            std::sort(erasedOwners.begin(), erasedOwners.end()); std::sort(erasedColumns.begin(), erasedColumns.end());
        }
        check();
    }
public:
    // A single private page set for multiple source groups and owner namespaces.
    // Entity liveness and all derived/directory authority remain with metadata.
    [[nodiscard]] static HybridPropertyDemotionPreparation prepareGroups(
        MetadataWriteBatch &batch, const MetadataSnapshotPin &snapshot, HybridPropertyFieldPageStore &fields,
        std::span<const HybridPropertyColumnDelta *const> changes, std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation = nullptr, const LargeFieldResolver &largeField = {}) {
        return HybridPropertyDemotionPreparation(batch, snapshot, fields, changes, std::move(memory),
            cancellation, largeField, false, {}, true);
    }
    // Prepares only group/field images. An enclosing metadata-owned graph
    // publication must authenticate logical-ID leases and install Entity Map,
    // labels, indexes/counts, placement and retirement with these exact images.
    [[nodiscard]] static HybridPropertyDemotionPreparation preparePendingOwners(
        MetadataWriteBatch &batch, const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot,
        HybridPropertyFieldPageStore &fields, std::span<const HybridPropertyColumnDelta *const> changes,
        std::span<const EntityPropertyKey> owners, std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation = nullptr, const LargeFieldResolver &largeField = {}) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "pending native owner preparation is disabled");
        const auto require = [](bool value, const char *message) { hybrid_property_detail::require(value, message); };
        require(memory && context.usesSnapshot(snapshot) && snapshot.superblock() == batch.current() &&
            !owners.empty() && owners.size() <= 64U && changes.size() <= 64U,
            "pending native owners need one bounded source batch");
        utils::OwnedAllocationScope scope(memory);
        const auto check = [&] { context.check(); if (cancellation) cancellation->check(); };
        utils::OwnedHeapReservation boundary(memory); const auto admit = [&](size_t bytes) { boundary.set(bytes); };
        struct PendingGroup { SlottedSlotReference reference; HybridPropertyGroupRecord source; uint64_t mask = 0; };
        std::array<PendingGroup, 64> pendingGroups{}; size_t groupCount = 0;
        for (size_t i = 0; i < owners.size(); ++i) {
            check(); const auto owner = owners[i]; SlottedSlotReference reference;
            require(owner.isValid() && std::ranges::find(owners.first(i), owner) == owners.first(i).end(),
                "pending native owner has an invalid or duplicate identity");
            const auto source = context.findGroup(owner, std::ref(admit), &reference); boundary.set(0);
            const auto bit = UINT64_C(1) << hybridPropertySlotFor(owner);
            require(source && !(source->live & bit), "pending native owner requires an installed group and absent live bit");
            size_t group = 0;
            while (group < groupCount && pendingGroups[group].reference != reference) ++group;
            if (group == groupCount) pendingGroups[groupCount++] = {reference, *source};
            require(pendingGroups[group].source == *source, "pending native group source records disagree");
            pendingGroups[group].mask |= bit;
        }
        HybridPropertyDemotionPreparation result(memory);
        if (!changes.empty()) result = HybridPropertyDemotionPreparation(batch, snapshot, fields, changes,
            memory, cancellation, largeField, false, owners, true);
        else result.group_ = pendingGroups[0].source;
        const auto imageFor = [&](PageId id) -> HybridPropertyPageImage & {
            const auto found = std::find_if(result.pages_.begin(), result.pages_.end(), [&](const auto &page) { return page.pageId() == id; });
            if (found != result.pages_.end()) return *found;
            result.pages_.push_back(fields.cloneForMutation(snapshot, id, batch.generation(), memory)); return result.pages_.back();
        };
        for (size_t i = 0; i < groupCount; ++i) {
            check(); const auto &group = pendingGroups[i];
            auto current = imageFor(group.reference.page_id).readGroup(group.reference, group.source.owner_kind, group.source.group_id, batch.generation());
            int64_t memberships = group.source.memberships, columns = group.source.columns;
            for (const auto *change : changes) if (change->groupReference() == group.reference) {
                memberships += change->effects().memberships; columns += change->effects().columns;
            }
            const bool droppedMatrix = group.source.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP && !columns;
            require(current.mode == (droppedMatrix ? HybridPropertyGroupMode::HPGM_HYBRID : group.source.mode) &&
                current.column_membership == (droppedMatrix ? SlottedSlotReference{} : group.source.column_membership),
                "pending native group changed its column membership ownership");
            require(memberships >= 0 && memberships <= UINT32_MAX && columns >= 0 && columns <= UINT32_MAX &&
                current.memberships == uint32_t(memberships) && current.columns == uint32_t(columns) &&
                (current.live | group.mask) == (group.source.live | group.mask) &&
                (current.semantic_generation == group.source.semantic_generation || current.semantic_generation == batch.generation()),
                "pending native group has inconsistent private field effects");
            auto replacement = current; replacement.live |= group.mask; replacement.semantic_generation = batch.generation(); replacement.validate();
            synchronizeMembershipLiveness(current, replacement, batch.generation(), imageFor);
            imageFor(group.reference.page_id).replaceGroup(group.reference, replacement, batch.generation());
            if (result.group_.owner_kind == replacement.owner_kind && result.group_.group_id == replacement.group_id) result.group_ = replacement;
        }
        check(); return result;
    }

    // A complete private owner deletion. Entity Map removal, root/map updates,
    // physical COW placement and global retirement still belong to one enclosing
    // publication. Never retire a LargeValue solely because this owner drops it.
    [[nodiscard]] static HybridPropertyDemotionPreparation prepareOwnerDeletion(
        MetadataWriteBatch &batch, const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot,
        HybridPropertyFieldPageStore &fields, std::span<const EntityPropertyKey> owners,
        std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation = nullptr,
        std::span<const HybridPropertyColumnDelta *const> additionalChanges = {}, const LargeFieldResolver &largeField = {}, std::span<const EntityPropertyKey> pendingOwners = {}) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native owner deletion preparation is disabled");
        const auto require = [](bool valid, const char *message) { hybrid_property_detail::require(valid, message); };
        require(memory && context.usesSnapshot(snapshot) && snapshot.superblock() == batch.current() &&
            !owners.empty() && owners.size() <= RSS_HYBRID_GROUP_CAPACITY && additionalChanges.size() <= 64U && pendingOwners.size() <= 64U,
            "native owner deletion requires one exact source batch and at most 64 owners");
        utils::OwnedAllocationScope scope(memory);
        const auto check = [&] { context.check(); if (cancellation) cancellation->check(); };
        utils::OwnedHeapReservation boundary(memory), referenceCredit(memory);
        const auto admit = [&](size_t bytes) { boundary.set(bytes); };
        const auto admitReference = [&](size_t bytes) { referenceCredit.set(bytes); };
        check();
        struct GroupRemoval {
            SlottedSlotReference reference;
            HybridPropertyGroupRecord source;
            uint64_t deleted = 0;
            size_t seen = 0, extensions = 0;
        };
        struct Extension { EntityPropertyKey owner; SlottedSlotReference reference; uint32_t token; uint8_t group; };
        utils::OwnedVector<GroupRemoval> groups{utils::OwnedPmrAllocator<GroupRemoval>(memory)};
        utils::OwnedVector<Extension> erased{utils::OwnedPmrAllocator<Extension>(memory)};
        utils::OwnedVector<HybridPropertyColumnDelta> plans{utils::OwnedPmrAllocator<HybridPropertyColumnDelta>(memory)};
        size_t fieldsSeen = 0;
        for (const auto owner : owners) {
            check(); SlottedSlotReference reference;
            const auto source = context.findGroup(owner, std::ref(admit), &reference); boundary.set(0);
            require(bool(source), "native owner deletion requires an installed group");
            auto group = std::find_if(groups.begin(), groups.end(), [&](const auto &value) {
                return value.source.owner_kind == source->owner_kind && value.source.group_id == source->group_id;
            });
            if (group == groups.end()) {
                groups.push_back({reference, *source}); group = std::prev(groups.end());
            } else require(group->source == *source && group->reference == reference,
                "native owner deletion source group identity disagrees");
            const size_t groupIndex = size_t(group - groups.begin());
            const auto bit = UINT64_C(1) << hybridPropertySlotFor(owner);
            require((source->live & bit) && !(group->deleted & bit), "native owner deletion has a dead or duplicate owner");
            group->deleted |= bit;
            const auto visit = [&](uint32_t token, HybridPropertySource kind) {
                check(); auto &state = groups[groupIndex];
                require(state.seen < source->memberships && fieldsSeen < 4096,
                    "native owner deletion exceeded its authenticated membership bound");
                ++state.seen; ++fieldsSeen;
                if (kind == HybridPropertySource::HPS_COLUMN) {
                    auto plan = HybridPropertyColumnDelta::planExistingColumn(context, snapshot, owner, token,
                        batch.generation(), memory, nullptr, true);
                    require(bool(plan), "native owner deletion lost its column source");
                    const auto previous = std::find_if(plans.begin(), plans.end(), [&](const auto &value) {
                        return value.columnReference() == plan->columnReference();
                    });
                    if (previous == plans.end()) {
                        if (plans.size() == plans.capacity()) plans.reserve(plans.empty() ? 1U : plans.size() * 2U);
                        plans.push_back(std::move(*plan));
                    } else {
                        const std::array<const HybridPropertyColumnDelta *, 2> merge{&*previous, &*plan};
                        *previous = HybridPropertyColumnDelta::mergeExistingColumnChanges(merge, memory,
                            RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, cancellation);
                    }
                } else {
                    const auto binding = context.bind(owner.owner_kind, 2);
                    HybridPropertyFieldDirectoryMap map(binding.root.index_id, context.trees_, context.metadata_, binding);
                    const auto locator = map.find(snapshot, HybridPropertyDirectoryCodec::ownerKey(owner, token),
                        std::ref(admit), context.cancellation_); boundary.set(0);
                    require(bool(locator), "native owner deletion lost its extension directory entry");
                    const auto ref = HybridPropertyDirectoryCodec::decodeLocator(*locator);
                    const auto page = context.fields_.read(snapshot, ref.page_id);
                    static_cast<void>(page.view().readExtension(ref, owner, token, *source,
                        context.descriptor_->semantic_generation, std::ref(admitReference)));
                    referenceCredit.set(0);
                    if (erased.size() == erased.capacity()) erased.reserve(erased.empty() ? 1U : erased.size() * 2U);
                    erased.push_back({owner, ref, token, uint8_t(groupIndex)}); ++state.extensions;
                }
                return true;
            };
            const auto summary = context.visitOwner(owner, 4097, std::ref(visit), std::ref(admit)); boundary.set(0);
            require(summary.routed && summary.owner_live && summary.complete,
                "native owner deletion did not enumerate every final source field");
        }
        const auto pendingMask = [&](const HybridPropertyGroupRecord &source) {
            uint64_t mask = 0;
            for (const auto owner : pendingOwners)
                if (owner.owner_kind == source.owner_kind && hybridPropertyGroupFor(owner) == source.group_id)
                    mask |= UINT64_C(1) << hybridPropertySlotFor(owner);
            return mask;
        };
        for (size_t i = 0; i < pendingOwners.size(); ++i) {
            check(); const auto owner = pendingOwners[i]; SlottedSlotReference reference;
            require(owner.isValid() && std::ranges::find(pendingOwners.first(i), owner) == pendingOwners.first(i).end() &&
                std::ranges::find(owners, owner) == owners.end(), "native birth/deletion repeats an owner");
            const auto source = context.findGroup(owner, std::ref(admit), &reference); boundary.set(0);
            require(source && !(source->live & (UINT64_C(1) << hybridPropertySlotFor(owner))), "native birth/deletion has no absent source owner");
            auto group = std::find_if(groups.begin(), groups.end(), [&](const auto &entry) { return entry.reference == reference; });
            if (group == groups.end()) { groups.push_back({reference, *source}); }
            else require(group->source == *source, "native birth/deletion source group records disagree");
        }
        // Fold surviving-owner changes into the same final column plans as
        // removals. Preparing two source-based images would lose one mutation.
        for (const auto *change : additionalChanges) {
            check(); require(change && change->isValid(), "native deletion received an invalid surviving column change");
            const auto previous = std::find_if(plans.begin(), plans.end(), [&](const auto &plan) {
                return plan.columnReference() == change->columnReference();
            });
            if (previous == plans.end()) {
                const std::array<const HybridPropertyColumnDelta *, 1> input{change};
                plans.push_back(HybridPropertyColumnDelta::mergeExistingColumnChanges(input, memory,
                    RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, cancellation));
            } else {
                const std::array<const HybridPropertyColumnDelta *, 2> input{&*previous, change};
                *previous = HybridPropertyColumnDelta::mergeExistingColumnChanges(input, memory,
                    RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, cancellation);
            }
        }
        HybridPropertyDemotionPreparation result(memory);
        if (!plans.empty()) {
            utils::OwnedVector<const HybridPropertyColumnDelta *> pointers{
                utils::OwnedPmrAllocator<const HybridPropertyColumnDelta *>(memory)};
            pointers.reserve(plans.size()); for (const auto &plan : plans) pointers.push_back(&plan);
            result = HybridPropertyDemotionPreparation(batch, snapshot, fields, pointers, memory, cancellation, largeField, true, pendingOwners, true);
        } else result.group_ = groups.front().source;
        const auto image = [&](PageId id) -> HybridPropertyPageImage & {
            const auto found = std::find_if(result.pages_.begin(), result.pages_.end(), [&](const auto &page) { return page.pageId() == id; });
            if (found != result.pages_.end()) return *found;
            if (result.pages_.size() == result.pages_.capacity()) result.pages_.reserve(result.pages_.empty() ? 1U : result.pages_.size() * 2U);
            check(); result.pages_.push_back(fields.cloneForMutation(snapshot, id, batch.generation(), memory)); return result.pages_.back();
        };
        // Verify every group against its own column effects before applying
        // extension removals or live-bit changes to any shared private page.
        for (const auto &group : groups) {
            auto expected = group.source; bool changed = false, hasColumnPlan = false;
            int64_t memberships = expected.memberships, columns = expected.columns;
            for (const auto &plan : plans) if (plan.groupReference() == group.reference) {
                hasColumnPlan = true;
                memberships += plan.effects().memberships; columns += plan.effects().columns;
                changed |= plan.action() != HybridPropertyColumnDelta::Action::HCD_NO_CHANGE;
            }
            require(memberships >= 0 && memberships <= UINT32_MAX && columns >= 0 && columns <= UINT32_MAX,
                "native deletion group effects underflow or overflow");
            expected.memberships = uint32_t(memberships); expected.columns = uint32_t(columns);
            if (expected.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP && !columns) {
                expected.mode = HybridPropertyGroupMode::HPGM_HYBRID; expected.column_membership = {};
            }
            if (hasColumnPlan) { expected.live |= pendingMask(group.source); changed |= bool(pendingMask(group.source)); }
            if (changed) expected.semantic_generation = batch.generation();
            require(image(group.reference.page_id).readGroup(group.reference, expected.owner_kind, expected.group_id,
                batch.generation()) == expected, "native owner deletion group source changed");
        }
        for (const auto &extension : erased) {
            check(); const auto bytes = image(extension.reference.page_id).readRecord(extension.reference);
            static_cast<void>(HybridPropertyExtensionView::open(bytes, extension.owner, extension.token,
                groups[extension.group].source.semantic_generation, batch.generation(), batch.generation(), std::ref(admitReference)));
            referenceCredit.set(0);
        }
        for (const auto &extension : erased) {
            check(); image(extension.reference.page_id).erase(extension.reference);
            const auto key = HybridPropertyDirectoryCodec::ownerKey(extension.owner, extension.token);
            result.erasedExtensionsFor(extension.owner.owner_kind).push_back(key);
            result.erasedOwnersFor(extension.owner.owner_kind).push_back(key);
        }
        for (const auto &group : groups) {
            check(); const auto currentGroup = image(group.reference.page_id).readGroup(group.reference, group.source.owner_kind,
                group.source.group_id, batch.generation());
            auto finalGroup = currentGroup;
            require(finalGroup.memberships >= group.extensions, "native owner deletion membership count underflow");
            finalGroup.memberships -= uint32_t(group.extensions); finalGroup.live &= ~group.deleted;
            finalGroup.live |= pendingMask(group.source);
            finalGroup.semantic_generation = batch.generation();
            if (finalGroup.live) {
                finalGroup.validate(); synchronizeMembershipLiveness(currentGroup, finalGroup, batch.generation(), image);
                image(group.reference.page_id).replaceGroup(group.reference, finalGroup, batch.generation());
            } else {
                require(!finalGroup.memberships && !finalGroup.columns, "empty native owner group retains fields or columns");
                image(group.reference.page_id).erase(group.reference);
                result.erasedGroupsFor(finalGroup.owner_kind).push_back(HybridPropertyDirectoryCodec::groupKey(finalGroup.group_id));
            }
            if (result.group_.owner_kind == finalGroup.owner_kind && result.group_.group_id == finalGroup.group_id) {
                result.group_ = finalGroup; result.group_erased_ = !finalGroup.live;
            }
        }
        for (const auto kind : {EntityPropertyOwnerKind::EPOK_NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
            auto &owners = result.erasedOwnersFor(kind); auto &extensions = result.erasedExtensionsFor(kind); auto &erasedGroups = result.erasedGroupsFor(kind);
            std::sort(owners.begin(), owners.end()); std::sort(extensions.begin(), extensions.end()); std::sort(erasedGroups.begin(), erasedGroups.end());
        }
        check(); return result;
    }
    HybridPropertyDemotionPreparation(const HybridPropertyDemotionPreparation &) = delete;
    HybridPropertyDemotionPreparation &operator=(const HybridPropertyDemotionPreparation &) = delete;
    HybridPropertyDemotionPreparation(HybridPropertyDemotionPreparation &&) noexcept = default;
    HybridPropertyDemotionPreparation &operator=(HybridPropertyDemotionPreparation &&) noexcept = default;
    [[nodiscard]] std::span<const HybridPropertyPageImage> pages() const & noexcept { return pages_; }
    std::span<const HybridPropertyPageImage> pages() const && = delete;
    // Explicit namespace access is required when preparing Node and Edge together.
    [[nodiscard]] std::span<const HybridPropertyOwnerMap::Entry> owners(EntityPropertyOwnerKind kind) const & noexcept {
        return kind == group_.owner_kind ? owners_ : other_.owners;
    }
    std::span<const HybridPropertyOwnerMap::Entry> owners(EntityPropertyOwnerKind) const && = delete;
    [[nodiscard]] std::span<const HybridPropertyOwnerMap::Key> erasedOwners(EntityPropertyOwnerKind kind) const & noexcept {
        return kind == group_.owner_kind ? erased_owners_ : other_.erased_owners;
    }
    std::span<const HybridPropertyOwnerMap::Key> erasedOwners(EntityPropertyOwnerKind) const && = delete;
    [[nodiscard]] std::span<const HybridPropertyFieldDirectoryMap::Entry> extensions(EntityPropertyOwnerKind kind) const & noexcept {
        return kind == group_.owner_kind ? extensions_ : other_.extensions;
    }
    std::span<const HybridPropertyFieldDirectoryMap::Entry> extensions(EntityPropertyOwnerKind) const && = delete;
    [[nodiscard]] std::span<const HybridPropertyFieldDirectoryMap::Key> erasedColumns(EntityPropertyOwnerKind kind) const & noexcept {
        return kind == group_.owner_kind ? erased_columns_ : other_.erased_columns;
    }
    std::span<const HybridPropertyFieldDirectoryMap::Key> erasedColumns(EntityPropertyOwnerKind) const && = delete;
    [[nodiscard]] std::span<const HybridPropertyFieldDirectoryMap::Key> erasedExtensions(EntityPropertyOwnerKind kind) const & noexcept {
        return kind == group_.owner_kind ? erased_extensions_ : other_.erased_extensions;
    }
    std::span<const HybridPropertyFieldDirectoryMap::Key> erasedExtensions(EntityPropertyOwnerKind) const && = delete;
    // Legacy accessors address the first source namespace only.
    [[nodiscard]] std::span<const HybridPropertyOwnerMap::Entry> owners() const & noexcept { return owners_; }
    std::span<const HybridPropertyOwnerMap::Entry> owners() const && = delete;
    [[nodiscard]] std::span<const HybridPropertyOwnerMap::Key> erasedOwners() const & noexcept { return erased_owners_; }
    std::span<const HybridPropertyOwnerMap::Key> erasedOwners() const && = delete;
    [[nodiscard]] std::span<const HybridPropertyFieldDirectoryMap::Entry> extensions() const & noexcept { return extensions_; }
    std::span<const HybridPropertyFieldDirectoryMap::Entry> extensions() const && = delete;
    [[nodiscard]] HybridPropertyFieldDirectoryMap::Key erasedColumn() const {
        hybrid_property_detail::require(erased_columns_.size() == 1, "single erased column accessor requires exactly one demotion");
        return erased_columns_.front();
    }
    [[nodiscard]] std::span<const HybridPropertyFieldDirectoryMap::Key> erasedColumns() const & noexcept { return erased_columns_; }
    std::span<const HybridPropertyFieldDirectoryMap::Key> erasedColumns() const && = delete;
    [[nodiscard]] std::span<const HybridPropertyFieldDirectoryMap::Key> erasedExtensions() const & noexcept { return erased_extensions_; }
    std::span<const HybridPropertyFieldDirectoryMap::Key> erasedExtensions() const && = delete;
    [[nodiscard]] std::span<const HybridPropertyGroupDirectoryMap::Key> erasedGroups(EntityPropertyOwnerKind kind) const & noexcept {
        return kind == group_.owner_kind ? erased_groups_ : other_.erased_groups;
    }
    std::span<const HybridPropertyGroupDirectoryMap::Key> erasedGroups(EntityPropertyOwnerKind) const && = delete;
    [[nodiscard]] std::optional<HybridPropertyGroupDirectoryMap::Key> erasedGroup() const noexcept {
        if (group_erased_) return HybridPropertyDirectoryCodec::groupKey(group_.group_id);
        return {};
    }
    [[nodiscard]] HybridPropertyGroupRecord group() const noexcept { return group_; }
};
} // namespace graph::storage
