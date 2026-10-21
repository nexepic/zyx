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
// This is bounded single-group lifecycle preparation, not a publication entry point.
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
    HybridPropertyGroupRecord group_;
    bool group_erased_ = false;
    explicit HybridPropertyDemotionPreparation(std::shared_ptr<std::pmr::memory_resource> memory)
        : pages_(utils::OwnedPmrAllocator<HybridPropertyPageImage>(memory)),
          owners_(utils::OwnedPmrAllocator<HybridPropertyOwnerMap::Entry>(memory)),
          erased_owners_(utils::OwnedPmrAllocator<HybridPropertyOwnerMap::Key>(memory)),
          extensions_(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Entry>(memory)),
          erased_columns_(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Key>(memory)),
          erased_extensions_(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Key>(memory)) {}
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
        const CancellationCheckpoint *cancellation, const LargeFieldResolver &largeField, bool wholeOwner)
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
        const auto sourceGroup = group_;
        const auto groupRef = first->groupReference();
        utils::OwnedVector<const HybridPropertyColumnDelta *> ordered{
            utils::OwnedPmrAllocator<const HybridPropertyColumnDelta *>(memory)};
        ordered.reserve(changes.size());
        int64_t memberships = group_.memberships, columns = group_.columns;
        size_t emittedMembers = 0;
        for (const auto *plan : changes) {
            check();
            require(plan && plan->isValid() && plan->databaseId() == batch.current().database_id &&
                plan->sourceGeneration() == batch.current().generation && plan->nextGeneration() == batch.generation() &&
                plan->groupReference() == groupRef && plan->sourceGroup() == sourceGroup,
                "native mixed group plans have inconsistent source fences or group bytes");
            require(wholeOwner || !plan->effects().clear_live, "native owner deletion requires whole-owner coordination");
            const auto effects = plan->effects();
            if (plan->action() == HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD) {
                require(effects.columns == -1 && effects.memberships >= -64 && effects.memberships <= 64,
                    "invalid native demotion effects");
                emittedMembers += size_t(std::popcount(plan->sourceColumn().presence) + effects.memberships);
            } else require(!effects.columns && effects.memberships >= -64 && effects.memberships <= 64,
                "invalid compatible native column effects");
            memberships += effects.memberships; columns += effects.columns;
            ordered.push_back(plan);
        }
        std::sort(ordered.begin(), ordered.end(), [](auto *left, auto *right) {
            return left->sourceColumn().identity.token < right->sourceColumn().identity.token;
        });
        for (size_t i = 1; i < ordered.size(); ++i)
            require(ordered[i - 1]->sourceColumn().identity.token != ordered[i]->sourceColumn().identity.token,
                "native mixed group requires one merged final plan per column");
        ordered.erase(std::remove_if(ordered.begin(), ordered.end(), [](auto *plan) {
            return plan->action() == HybridPropertyColumnDelta::Action::HCD_NO_CHANGE;
        }), ordered.end());
        if (ordered.empty()) return;
        require(memberships >= 0 && memberships <= UINT32_MAX && columns >= 0, "native mixed group effects underflow");
        group_.memberships = uint32_t(memberships); group_.columns = uint32_t(columns);
        group_.semantic_generation = batch.generation(); group_.validate();
        utils::OwnedVector<PageId> sourceIds{utils::OwnedPmrAllocator<PageId>(memory)};
        sourceIds.reserve(1 + ordered.size()); sourceIds.push_back(groupRef.page_id);
        for (const auto *plan : ordered) sourceIds.push_back(plan->columnReference().page_id);
        std::sort(sourceIds.begin(), sourceIds.end());
        sourceIds.erase(std::unique(sourceIds.begin(), sourceIds.end()), sourceIds.end());
        const auto sourcePages = sourceIds.size();
        pages_.reserve(sourcePages);
        for (const auto id : sourceIds) {
            check(); pages_.push_back(fields.cloneForMutation(snapshot, id, batch.generation(), memory));
        }
        const auto image = [&](PageId id) -> HybridPropertyPageImage & {
            const auto found = std::lower_bound(sourceIds.begin(), sourceIds.end(), id);
            require(found != sourceIds.end() && *found == id, "native mixed source page missing");
            return pages_[size_t(found - sourceIds.begin())];
        };
        require(image(groupRef.page_id).readGroup(groupRef, group_.owner_kind, group_.group_id,
            batch.generation()) == sourceGroup, "native mixed group source changed");
        for (const auto *plan : ordered) {
            check();
            require(image(plan->columnReference().page_id).readColumn(plan->columnReference(),
                plan->sourceColumn().identity, sourceGroup, batch.generation()) == plan->sourceColumn(),
                "native mixed column source changed");
        }
        // All source bytes are authenticated before modifying any private image.
        image(groupRef.page_id).replaceGroup(groupRef, group_, batch.generation());
        erased_columns_.reserve(ordered.size());
        owners_.reserve(emittedMembers); extensions_.reserve(emittedMembers);
        erased_owners_.reserve(sourceGroup.memberships > group_.memberships ? size_t(sourceGroup.memberships - group_.memberships) : 0U);
        for (const auto *plan : ordered) {
            check();
            const auto &change = *plan;
            if (change.action() == HybridPropertyColumnDelta::Action::HCD_NO_CHANGE) continue;
            if (change.action() != HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD) {
                const auto &replacement = change.replacementColumn();
                image(change.columnReference().page_id).replaceColumn(change.columnReference(),
                    change.sourceColumn().identity, replacement, group_, batch.generation());
                const auto added = replacement.presence & ~change.sourceColumn().presence;
                for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot)
                    if (added & (UINT64_C(1) << slot)) {
                        const EntityPropertyKey owner{group_.owner_kind,
                            (group_.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + slot + 1U};
                        owners_.push_back({HybridPropertyDirectoryCodec::ownerKey(owner, replacement.identity.token),
                            HybridPropertyDirectoryCodec::sourceValue(HybridPropertySource::HPS_COLUMN)});
                    }
                const auto removed = change.sourceColumn().presence & ~replacement.presence;
                for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot)
                    if (removed & (UINT64_C(1) << slot)) {
                        const EntityPropertyKey owner{group_.owner_kind,
                            (group_.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + slot + 1U};
                        erased_owners_.push_back(HybridPropertyDirectoryCodec::ownerKey(owner, replacement.identity.token));
                    }
                continue;
            }
            image(change.columnReference().page_id).erase(change.columnReference());
            const auto token = change.sourceColumn().identity.token;
            erased_columns_.push_back(HybridPropertyDirectoryCodec::columnKey(group_.group_id, token));
            const auto cells = change.demotedCells();
            for (unsigned slot = 0; slot < cells.size(); ++slot) {
                check();
                const EntityPropertyKey owner{group_.owner_kind, (group_.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + slot + 1U};
                const auto &cell = cells[slot];
                if (cell.kind == HybridPropertyColumnDelta::CellKind::HCC_MISSING) {
                    if (change.sourceColumn().presence & (UINT64_C(1) << slot))
                        erased_owners_.push_back(HybridPropertyDirectoryCodec::ownerKey(owner, token));
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
                const auto locator = pages_.back().appendExtension(bytes, owner, token, group_, batch.generation(), std::ref(admitReference));
                const auto key = HybridPropertyDirectoryCodec::ownerKey(owner, token);
                owners_.push_back({key, HybridPropertyDirectoryCodec::sourceValue(HybridPropertySource::HPS_EXTENSION)});
                extensions_.push_back({key, HybridPropertyDirectoryCodec::locatorValue(locator)});
            }
        }
        // Directory mutations are consumed by key-ordered map coordinators.
        // Token-major preparation must not leak an unsorted owner-major batch.
        const auto entryLess = [&](const auto &left, const auto &right) { check(); return left.key < right.key; };
        std::sort(owners_.begin(), owners_.end(), entryLess);
        std::sort(extensions_.begin(), extensions_.end(), entryLess);
        std::sort(erased_owners_.begin(), erased_owners_.end(), [&](const auto &left, const auto &right) {
            check(); return left < right;
        });
        check();
    }
public:
    // A complete private owner deletion. Entity Map removal, root/map updates,
    // physical COW placement and global retirement still belong to one enclosing
    // publication. Never retire a LargeValue solely because this owner drops it.
    [[nodiscard]] static HybridPropertyDemotionPreparation prepareOwnerDeletion(
        MetadataWriteBatch &batch, const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot,
        HybridPropertyFieldPageStore &fields, std::span<const EntityPropertyKey> owners,
        std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation = nullptr) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native owner deletion preparation is disabled");
        const auto require = [](bool valid, const char *message) { hybrid_property_detail::require(valid, message); };
        require(memory && context.usesSnapshot(snapshot) && snapshot.superblock() == batch.current() &&
            !owners.empty() && owners.size() <= RSS_HYBRID_GROUP_CAPACITY,
            "native owner deletion requires one exact source batch and at most 64 owners");
        utils::OwnedAllocationScope scope(memory);
        const auto check = [&] { context.check(); if (cancellation) cancellation->check(); };
        utils::OwnedHeapReservation boundary(memory), referenceCredit(memory);
        const auto admit = [&](size_t bytes) { boundary.set(bytes); };
        const auto admitReference = [&](size_t bytes) { referenceCredit.set(bytes); };
        check();
        SlottedSlotReference groupRef;
        const auto source = context.findGroup(owners.front(), std::ref(admit), &groupRef);
        require(bool(source), "native owner deletion requires an installed group");
        struct Extension { EntityPropertyKey owner; uint32_t token; SlottedSlotReference reference; };
        utils::OwnedVector<Extension> erased{utils::OwnedPmrAllocator<Extension>(memory)};
        utils::OwnedVector<HybridPropertyColumnDelta> plans{utils::OwnedPmrAllocator<HybridPropertyColumnDelta>(memory)};
        uint64_t deleted = 0; size_t fieldsSeen = 0;
        for (const auto owner : owners) {
            check(); SlottedSlotReference actualRef;
            const auto group = context.findGroup(owner, std::ref(admit), &actualRef);
            require(group && *group == *source && actualRef == groupRef,
                "native owner deletion crosses a group or namespace");
            const auto bit = UINT64_C(1) << hybridPropertySlotFor(owner);
            require((source->live & bit) && !(deleted & bit), "native owner deletion has a dead or duplicate owner");
            deleted |= bit;
            const auto visit = [&](uint32_t token, HybridPropertySource kind) {
                check(); require(fieldsSeen < source->memberships && fieldsSeen < 4096,
                    "native owner deletion exceeded its authenticated membership bound");
                ++fieldsSeen;
                if (kind == HybridPropertySource::HPS_COLUMN) {
                    auto plan = HybridPropertyColumnDelta::planExistingColumn(context, snapshot, owner, token,
                        batch.generation(), memory, nullptr, true);
                    require(bool(plan), "native owner deletion lost its column source");
                    const auto previous = std::find_if(plans.begin(), plans.end(), [&](const auto &value) {
                        return value.sourceColumn().identity.token == token;
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
                        std::ref(admit), context.cancellation_);
                    require(bool(locator), "native owner deletion lost its extension directory entry");
                    const auto ref = HybridPropertyDirectoryCodec::decodeLocator(*locator);
                    const auto page = context.fields_.read(snapshot, ref.page_id);
                    static_cast<void>(page.view().readExtension(ref, owner, token, *source,
                        context.descriptor_->semantic_generation, std::ref(admitReference)));
                    referenceCredit.set(0);
                    if (erased.size() == erased.capacity()) erased.reserve(erased.empty() ? 1U : erased.size() * 2U);
                    erased.push_back({owner, token, ref});
                }
                return true;
            };
            const auto summary = context.visitOwner(owner, 4097, std::ref(visit), std::ref(admit));
            require(summary.routed && summary.owner_live && summary.complete,
                "native owner deletion did not enumerate every final source field");
        }
        boundary.set(0);
        HybridPropertyDemotionPreparation result(memory);
        if (!plans.empty()) {
            utils::OwnedVector<const HybridPropertyColumnDelta *> pointers{
                utils::OwnedPmrAllocator<const HybridPropertyColumnDelta *>(memory)};
            pointers.reserve(plans.size()); for (const auto &plan : plans) pointers.push_back(&plan);
            result = HybridPropertyDemotionPreparation(batch, snapshot, fields, pointers, memory, cancellation, {}, true);
        } else result.group_ = *source;
        const auto image = [&](PageId id) -> HybridPropertyPageImage & {
            const auto found = std::find_if(result.pages_.begin(), result.pages_.end(),
                [&](const auto &page) { return page.pageId() == id; });
            if (found != result.pages_.end()) return *found;
            if (result.pages_.size() == result.pages_.capacity())
                result.pages_.reserve(result.pages_.empty() ? 1U : result.pages_.size() * 2U);
            check(); result.pages_.push_back(fields.cloneForMutation(snapshot, id, batch.generation(), memory));
            return result.pages_.back();
        };
        // With no affected columns the group was not cloned/authenticated by
        // the shared column coordinator. Check its exact source bytes as well.
        require(image(groupRef.page_id).readGroup(groupRef, source->owner_kind, source->group_id,
            batch.generation()) == result.group_, "native owner deletion group source changed");
        // Authenticate all extra source records before erasing any of them.
        for (const auto &extension : erased) {
            check(); const auto bytes = image(extension.reference.page_id).readRecord(extension.reference);
            static_cast<void>(HybridPropertyExtensionView::open(bytes, extension.owner, extension.token,
                source->semantic_generation, batch.generation(), batch.generation(), std::ref(admitReference)));
            referenceCredit.set(0);
        }
        auto finalGroup = result.group_;
        require(finalGroup.memberships >= erased.size(), "native owner deletion membership count underflow");
        finalGroup.memberships -= uint32_t(erased.size()); finalGroup.live &= ~deleted;
        finalGroup.semantic_generation = batch.generation();
        if (finalGroup.live) finalGroup.validate();
        else require(!finalGroup.memberships && !finalGroup.columns,
            "empty native owner group retains fields or columns");
        result.erased_extensions_.reserve(erased.size());
        result.erased_owners_.reserve(result.erased_owners_.size() + erased.size());
        for (const auto &extension : erased) {
            check(); image(extension.reference.page_id).erase(extension.reference);
            const auto key = HybridPropertyDirectoryCodec::ownerKey(extension.owner, extension.token);
            result.erased_extensions_.push_back(key); result.erased_owners_.push_back(key);
        }
        if (finalGroup.live) image(groupRef.page_id).replaceGroup(groupRef, finalGroup, batch.generation());
        else { image(groupRef.page_id).erase(groupRef); result.group_erased_ = true; }
        result.group_ = finalGroup;
        const auto less = [&](const auto &left, const auto &right) { check(); return left < right; };
        std::sort(result.erased_owners_.begin(), result.erased_owners_.end(), less);
        std::sort(result.erased_extensions_.begin(), result.erased_extensions_.end(), less);
        check(); return result;
    }
    HybridPropertyDemotionPreparation(const HybridPropertyDemotionPreparation &) = delete;
    HybridPropertyDemotionPreparation &operator=(const HybridPropertyDemotionPreparation &) = delete;
    HybridPropertyDemotionPreparation(HybridPropertyDemotionPreparation &&) noexcept = default;
    HybridPropertyDemotionPreparation &operator=(HybridPropertyDemotionPreparation &&) noexcept = default;
    [[nodiscard]] std::span<const HybridPropertyPageImage> pages() const & noexcept { return pages_; }
    std::span<const HybridPropertyPageImage> pages() const && = delete;
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
    [[nodiscard]] std::optional<HybridPropertyGroupDirectoryMap::Key> erasedGroup() const noexcept {
        if (group_erased_) return HybridPropertyDirectoryCodec::groupKey(group_.group_id);
        return {};
    }
    [[nodiscard]] HybridPropertyGroupRecord group() const noexcept { return group_; }
};
} // namespace graph::storage
