/**
 * @file HybridPropertyDemotionPreparation.hpp
 * @brief Same-batch private images and directory edits for mixed group mutations.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include <algorithm>
#include <array>
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/storage/relocatable/property/HybridPropertyColumnDelta.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPageImage.hpp"
#include "graph/storage/relocatable/property/HybridPropertyLargeFieldPreparation.hpp"

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
    utils::OwnedVector<HybridPropertyGroupDirectoryMap::Entry> groups_;
    struct NamespaceEdits {
        utils::OwnedVector<HybridPropertyOwnerMap::Entry> owners;
        utils::OwnedVector<HybridPropertyOwnerMap::Key> erased_owners;
        utils::OwnedVector<HybridPropertyFieldDirectoryMap::Entry> extensions;
        utils::OwnedVector<HybridPropertyFieldDirectoryMap::Key> erased_columns, erased_extensions;
        utils::OwnedVector<HybridPropertyGroupDirectoryMap::Key> erased_groups;
        utils::OwnedVector<HybridPropertyGroupDirectoryMap::Entry> groups;
        explicit NamespaceEdits(const std::shared_ptr<std::pmr::memory_resource> &memory)
            : owners(utils::OwnedPmrAllocator<HybridPropertyOwnerMap::Entry>(memory)),
              erased_owners(utils::OwnedPmrAllocator<HybridPropertyOwnerMap::Key>(memory)),
              extensions(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Entry>(memory)),
              erased_columns(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Key>(memory)),
              erased_extensions(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Key>(memory)),
              erased_groups(utils::OwnedPmrAllocator<HybridPropertyGroupDirectoryMap::Key>(memory)),
              groups(utils::OwnedPmrAllocator<HybridPropertyGroupDirectoryMap::Entry>(memory)) {}
    };
    NamespaceEdits other_;
    HybridPropertyGroupRecord group_;
    bool group_erased_ = false;
    template<class Image>
    static void synchronizeMembershipLiveness(const HybridPropertyGroupRecord &previous,
        const HybridPropertyGroupRecord &replacement, Generation generation, Image &&image) {
        if (!previous.usesColumnMembership() || previous.live == replacement.live) return;
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
          erased_groups_(utils::OwnedPmrAllocator<HybridPropertyGroupDirectoryMap::Key>(memory)),
          groups_(utils::OwnedPmrAllocator<HybridPropertyGroupDirectoryMap::Entry>(memory)), other_(memory) {}
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
    auto &groupsFor(EntityPropertyOwnerKind kind) {
        return kind == group_.owner_kind ? groups_ : other_.groups;
    }
    auto &erasedGroupsFor(EntityPropertyOwnerKind kind) {
        return kind == group_.owner_kind ? erased_groups_ : other_.erased_groups;
    }
public:
    struct ColdBlockChange {
        EntityPropertyKey anchor;
        SlottedSlotReference reference;
        std::span<const HybridColdRowInput> rows;
    };
    [[nodiscard]] static HybridPropertyDemotionPreparation prepareColdBlocks(MetadataWriteBatch &batch,
        const HybridPropertyReadContext &context, std::span<const ColdBlockChange> blocks,
        std::shared_ptr<std::pmr::memory_resource> memory,
        std::span<const HybridPropertyColumnDelta *const> hotChanges = {},
        const LargeValueStore *values = nullptr, const CancellationCheckpoint *cancellation = nullptr,
        std::span<const HybridPropertyLargeFieldPreparation> newRoots = {},
        std::span<const HybridPropertyColumnDelta *const> demotions = {},
        std::span<const EntityPropertyKey> deletedOwners = {},
        std::span<const EntityPropertyKey> pendingOwners = {}) {
        return prepareColdBlocksImpl(batch, context, blocks, memory, hotChanges, values, cancellation,
            newRoots, demotions, deletedOwners, pendingOwners, HybridPropertyDemotionPreparation(memory));
    }
private:
    [[nodiscard]] static HybridPropertyDemotionPreparation prepareColdBlocksImpl(MetadataWriteBatch &batch,
        const HybridPropertyReadContext &context, std::span<const ColdBlockChange> blocks,
        std::shared_ptr<std::pmr::memory_resource> memory,
        std::span<const HybridPropertyColumnDelta *const> hotChanges,
        const LargeValueStore *values, const CancellationCheckpoint *cancellation,
        std::span<const HybridPropertyLargeFieldPreparation> newRoots,
        std::span<const HybridPropertyColumnDelta *const> demotions,
        std::span<const EntityPropertyKey> deletedOwners,
        std::span<const EntityPropertyKey> pendingOwners, HybridPropertyDemotionPreparation result) {
        using hybrid_property_detail::require;
        require(memory && context.snapshot_.superblock() == batch.current() && deletedOwners.size() <= 64 && pendingOwners.size() <= 64 &&
            (!blocks.empty() || !demotions.empty() || !deletedOwners.empty() || !pendingOwners.empty()) &&
            blocks.size() <= 64 && demotions.size() <= 64 && hotChanges.size() <= 64U - demotions.size(),
            "cold batch requires bounded blocks and hot plans");
        // Whole-column demotion expands a bounded user write to at most64
        // actual owners. Structural column plans are not additional owners.
        // Existing ordinary cold/hot callers retain their original count limit.
        size_t count = demotions.empty() && deletedOwners.empty() && pendingOwners.empty() ? hotChanges.size() : 0; uint64_t changed = 0;
        for (size_t i = 0; i < blocks.size(); ++i) {
            const auto &block = blocks[i];
            require(block.anchor.isValid() && (block.reference.isValid() || block.reference == SlottedSlotReference{}) && !block.rows.empty() &&
                block.rows.size() <= 64U - count && block.anchor.owner_kind == blocks.front().anchor.owner_kind &&
                hybridPropertyGroupFor(block.anchor) == hybridPropertyGroupFor(blocks.front().anchor),
                "cold batch changes exceed one group or original64change bound"); count += block.rows.size();
            for (size_t j = 0; j < i; ++j)
                require(block.reference != blocks[j].reference, "cold batch repeats a source block");
            for (const auto &row : block.rows) {
                require(row.owner_slot < 64, "cold batch owner slot exceeds its group");
                const auto bit = UINT64_C(1) << row.owner_slot;
                require(!(changed & bit), "cold batch repeats an owner root"); changed |= bit;
            }
        }
        const bool prepared = result.group_.group_id != 0;
        uint64_t pendingMask = 0;
        if (!pendingOwners.empty()) {
            if (!prepared) result = preparePendingOwners(batch, context, context.snapshot_, context.fields_, hotChanges,
                pendingOwners, memory, cancellation, {}, true);
            for (const auto owner : pendingOwners) {
                require(owner.owner_kind == result.group_.owner_kind && hybridPropertyGroupFor(owner) == result.group_.group_id &&
                    std::ranges::find(deletedOwners, owner) == deletedOwners.end(),
                    "cold pending roots crossed groups or repeat a deleted owner");
                pendingMask |= UINT64_C(1) << hybridPropertySlotFor(owner);
            }
        } else if (!prepared && !hotChanges.empty()) result = HybridPropertyDemotionPreparation(batch, context.snapshot_, context.fields_,
            hotChanges, memory, cancellation);
        utils::OwnedVector<uint32_t> removedTokens{utils::OwnedPmrAllocator<uint32_t>(memory)};
        if (!demotions.empty()) {
            const auto check = [&] { context.check(); if (cancellation) cancellation->check(); };
            require(demotions.front() && demotions.front()->isValid(), "cold demotion has no authenticated first plan");
            const auto &source = demotions.front()->sourceGroup();
            const auto groupReference = demotions.front()->groupReference();
            require(source.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
                context.snapshot_.superblock() == batch.current(), "cold demotion requires its original source group and batch");
            utils::OwnedHeapReservation boundary(memory);
            const SelectedPropertyReadAdmission admit = [&](size_t bytes) { boundary.set(bytes); };
            require(context.findGroup(demotions.front()->owner(), admit, nullptr, true, true) == source,
                "cold demotion source Entity/ROW authority changed"); boundary.set(0);
            if (!result.group_.group_id) result.group_ = source;
            require(result.group_.owner_kind == source.owner_kind && result.group_.group_id == source.group_id &&
                result.group_.columns == source.columns && result.group_.live == (source.live | pendingMask),
                "cold demotion compatible plans change source group identity or liveness");
            result.pages_.reserve(result.pages_.size() + demotions.size() + 1U);
            const auto image = [&](PageId id) -> HybridPropertyPageImage & {
                for (auto &page : result.pages_) if (page.pageId() == id) return page;
                result.pages_.push_back(context.fields_.cloneForMutation(context.snapshot_, id, batch.generation(), memory));
                return result.pages_.back();
            };
            const auto previousGroup = result.group_;
            auto matrix = image(groupReference.page_id).readColumnMembership(previousGroup.column_membership,
                previousGroup, batch.generation());
            // Removing global hot authority changes what every existing cold
            // directory is allowed to contain. Authenticate all original cold
            // roots in this one64-owner group, even roots not being repacked,
            // so an old forbidden hot token cannot become valid by demotion.
            const auto originalMatrix = context.columnMembership(source);
            uint64_t inspectedOwners = 0;
            for (unsigned slot = 0; slot < 64; ++slot) {
                const auto bit = UINT64_C(1) << slot;
                if (!(originalMatrix.cold_rows.owners & bit) || (inspectedOwners & bit)) continue;
                context.withColdBlock(source, originalMatrix, originalMatrix.cold_rows.roots[slot],
                    [&](const auto &block, const auto &) {
                        for (unsigned member = 0; member < 64; ++member) if (const auto row = block.row(member)) {
                            check(); const EntityPropertyKey owner{source.owner_kind, (source.group_id - 1U) * 64U + member + 1U};
                            require(context.findGroup(owner, admit, nullptr, true, true) == source,
                                "cold demotion original cold owner Entity/ROW authority changed");
                            context.requireNoLegacySelector(owner, admit); boundary.set(0);
                            HybridPropertyColdRowInspection original(context.snapshot_, *row, owner, block.semantic_generation,
                                std::span(originalMatrix.tokens).first(originalMatrix.column_count), memory, values, cancellation);
                            static_cast<void>(original.count());
                        }
                        inspectedOwners |= block.owners;
                    });
            }
            uint64_t requiredOwners = 0; size_t emitted = 0;
            for (const auto *plan : demotions) {
                check(); require(plan && plan->isValid() && plan->action() == HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD &&
                    plan->databaseId() == batch.current().database_id && plan->sourceGeneration() == batch.current().generation &&
                    plan->nextGeneration() == batch.generation() && plan->sourceGroup() == source &&
                    plan->groupReference() == groupReference && !plan->effects().clear_live && !(plan->effects().set_live & ~pendingMask) &&
                    plan->effects().columns == -1, "cold demotion has a foreign source or liveness plan");
                const auto token = plan->sourceColumn().identity.token;
                require(matrix.presence(token) == plan->sourceColumn().presence && matrix.columnLocator(token) == plan->columnReference(),
                    "cold demotion column contradicts its source membership");
                require(image(plan->columnReference().page_id).readColumn(plan->columnReference(),
                    plan->sourceColumn().identity, source, batch.generation()) == plan->sourceColumn(),
                    "cold demotion source column bytes changed");
                for (unsigned slot = 0; slot < 64; ++slot)
                    if (plan->demotedCells()[slot].kind != HybridPropertyColumnDelta::CellKind::HCC_MISSING) {
                        require(emitted < 4096U, "cold demotion exceeds4096actual emitted memberships"); ++emitted;
                        requiredOwners |= UINT64_C(1) << slot;
                    }
                removedTokens.push_back(token); matrix.eraseColumn(token);
                image(plan->columnReference().page_id).erase(plan->columnReference());
            }
            require(!(requiredOwners & ~changed), "cold demotion omitted an actual surviving column owner");
            std::sort(removedTokens.begin(), removedTokens.end());
            // Prove every surviving/removed demoted membership against complete
            // target roots, including original same-batch unpublished carriers.
            for (const auto &block : blocks) for (const auto &row : block.rows) {
                check(); const EntityPropertyKey owner{source.owner_kind, (source.group_id - 1U) * 64U + row.owner_slot + 1U};
                const uint32_t type = source.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
                const auto verifyValue = [&](size_t ordinal, std::optional<std::span<const std::byte>> bytes) {
                    check(); const auto token = removedTokens[ordinal];
                    const auto found = std::find_if(demotions.begin(), demotions.end(), [&](const auto *plan) {
                        return plan->sourceColumn().identity.token == token;
                    });
                    require(found != demotions.end(), "cold demotion lost its exact column plan");
                    const auto &cell = (*found)->demotedCells()[row.owner_slot];
                    require(bool(bytes) == (cell.kind != HybridPropertyColumnDelta::CellKind::HCC_MISSING),
                        "cold demotion target contradicts an actual final column membership");
                    if (!bytes) return;
                    utils::OwnedHeapReservation scalarCredit(memory, 3U * utils::detail::standardContainerProxyBytes() +
                        utils::detail::standardAllocatorRequestBytes(8U, alignof(char)));
                    PropertyValue scalar; const PropertyValue *expected = &scalar;
                    switch (cell.kind) {
                        case HybridPropertyColumnDelta::CellKind::HCC_NULL: break;
                        case HybridPropertyColumnDelta::CellKind::HCC_INT64: scalar = PropertyValue(std::bit_cast<int64_t>(cell.bits)); break;
                        case HybridPropertyColumnDelta::CellKind::HCC_DOUBLE: scalar = PropertyValue(std::bit_cast<double>(cell.bits)); break;
                        case HybridPropertyColumnDelta::CellKind::HCC_BOOL: scalar = PropertyValue(bool(cell.bits)); break;
                        case HybridPropertyColumnDelta::CellKind::HCC_SHORT_TEXT7: scalar = PropertyValue(HybridPropertyShortText::decode(cell.bits)); break;
                        case HybridPropertyColumnDelta::CellKind::HCC_EXTENSION:
                            require(cell.extension, "cold demotion lost its borrowed replacement value"); expected = cell.extension; break;
                        default: require(false, "cold demotion has an invalid final cell");
                    }
                    std::pmr::vector<std::byte> encoded(memory.get());
                    encoded.reserve(CanonicalPropertyValueCodec::encodedSize(*expected));
                    CanonicalPropertyValueCodec::append(encoded, *expected);
                    require(std::ranges::equal(encoded, *bytes), "cold demotion target changes an authenticated final column value");
                };
                if (row.root.empty()) {
                    for (size_t i = 0; i < removedTokens.size(); ++i) verifyValue(i, {});
                } else if (!row.large_value_reference)
                    EntityPropertyCollectionRootCodec::visitSelectedEncoded(row.root, owner.entity_id, type, removedTokens, verifyValue);
                else {
                    utils::OwnedHeapReservation referenceCredit(memory, row.root.size() + 4U * utils::detail::standardContainerProxyBytes());
                    const auto reference = EntityPropertyLargeValueReferenceCodec::decode(row.root);
                    if (reference.value.value_id.value() >= batch.current().next_large_value_id) {
                        require(admitPreparedColdRoot(batch, row, owner, std::span(matrix.tokens).first(matrix.column_count),
                            newRoots, memory, cancellation), "cold demotion target has no original reserved carrier proposal");
                        const auto found = std::find_if(newRoots.begin(), newRoots.end(), [&](const auto &proposal) {
                            return proposal.reference() == reference.value;
                        });
                        require(found != newRoots.end(), "cold demotion lost its immutable carrier holder");
                        const auto directory = EntityPropertyLargeValuePayloadCodec::decodeDirectory(found->payload(), &admit, cancellation);
                        const auto data = found->payload().subspan(RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES);
                        for (size_t i = 0; i < removedTokens.size(); ++i) {
                            const auto entry = std::lower_bound(directory.begin(), directory.end(), removedTokens[i],
                                [](const auto &location, uint32_t token) { return location.key_token < token; });
                            if (entry == directory.end() || entry->key_token != removedTokens[i]) verifyValue(i, {});
                            else verifyValue(i, data.subspan(entry->offset + sizeof(uint32_t), entry->length - sizeof(uint32_t)));
                        }
                        boundary.set(0);
                    } else {
                        HybridPropertyColdRowInspection inspected(context.snapshot_, row, owner, batch.current().generation,
                            std::span(matrix.tokens).first(matrix.column_count), memory, values, cancellation);
                        inspected.visitSelected(removedTokens, verifyValue);
                    }
                }
            }
            matrix.semantic_generation = batch.generation();
            result.group_.columns = matrix.column_count; result.group_.memberships = matrix.totalMemberships();
            result.group_.semantic_generation = batch.generation();
            image(groupReference.page_id).replaceColumnMembership(previousGroup.column_membership, matrix,
                previousGroup, result.group_, batch.generation());
            image(groupReference.page_id).replaceGroup(groupReference, result.group_, batch.generation());
        }
        for (const auto &block : blocks)
            result = prepareColdBlockImpl(batch, context, block.anchor, block.reference, block.rows, memory, values,
                cancellation, std::move(result), newRoots, removedTokens, pendingMask);
        if (!deletedOwners.empty()) {
            // Remove liveness only after every hot/cold membership has been
            // removed in these same private images. No implicit field clearing
            // may hide an omitted removal or a stale source Entity/ROW witness.
            utils::OwnedAllocationScope scope(memory);
            utils::OwnedHeapReservation boundary(memory);
            const SelectedPropertyReadAdmission admit = [&](size_t bytes) { boundary.set(bytes); };
            const auto check = [&] { context.check(); if (cancellation) cancellation->check(); };
            uint64_t deleted = 0; SlottedSlotReference sourceReference;
            const auto source = context.findGroup(deletedOwners.front(), admit, &sourceReference, true, true);
            boundary.set(0);
            require(source && source->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER,
                "cold deletion requires its actual original source group");
            for (const auto owner : deletedOwners) {
                check(); require(owner.isValid() && owner.owner_kind == source->owner_kind &&
                    hybridPropertyGroupFor(owner) == source->group_id,
                    "cold deletion crossed its authenticated source group");
                const auto bit = UINT64_C(1) << hybridPropertySlotFor(owner);
                require((source->live & bit) && !(deleted & bit) &&
                    context.findGroup(owner, admit, nullptr, true, true) == source,
                    "cold deletion has a dead, repeated or foreign source owner");
                context.requireNoLegacySelector(owner, admit); boundary.set(0); deleted |= bit;
            }
            if (!result.group_.group_id) result.group_ = *source;
            require(result.group_.owner_kind == source->owner_kind && result.group_.group_id == source->group_id &&
                result.group_.live == (source->live | pendingMask), "cold deletion preparation changed liveness prematurely");
            result.pages_.reserve(result.pages_.size() + 2U);
            const auto image = [&](PageId id) -> HybridPropertyPageImage & {
                for (auto &page : result.pages_) if (page.pageId() == id) return page;
                result.pages_.push_back(context.fields_.cloneForMutation(context.snapshot_, id, batch.generation(), memory));
                return result.pages_.back();
            };
            auto reference = sourceReference;
            if (!result.groups_.empty()) {
                require(result.groups_.size() == 1 && result.groups_.front().key ==
                    HybridPropertyDirectoryCodec::groupKey(source->group_id), "cold deletion has unrelated group relocation");
                reference = HybridPropertyDirectoryCodec::decodeLocator(result.groups_.front().value);
            }
            const auto previous = result.group_;
            require(image(reference.page_id).readGroup(reference, previous.owner_kind, previous.group_id,
                batch.generation()) == previous, "cold deletion private group bytes changed");
            auto matrix = image(previous.column_membership.page_id).readColumnMembership(previous.column_membership,
                previous, batch.generation());
            require(!(matrix.cold_rows.owners & deleted), "cold deletion omitted an actual cold root removal");
            for (unsigned i = 0; i < matrix.column_count; ++i)
                require(!(matrix.presence(matrix.tokens[i]) & deleted), "cold deletion omitted an actual hot membership removal");
            result.group_.live &= ~deleted; result.group_.semantic_generation = batch.generation();
            if (result.group_.live) {
                matrix.live = result.group_.live; matrix.semantic_generation = batch.generation();
                image(previous.column_membership.page_id).replaceColumnMembership(previous.column_membership, matrix,
                    previous, result.group_, batch.generation());
                image(reference.page_id).replaceGroup(reference, result.group_, batch.generation());
            } else {
                require(!result.group_.columns && !result.group_.memberships && !matrix.totalMemberships(),
                    "final cold group deletion retains fields or columns");
                image(previous.column_membership.page_id).erase(previous.column_membership);
                image(reference.page_id).erase(reference);
                result.groups_.clear(); result.group_erased_ = true;
                result.erased_groups_.push_back(HybridPropertyDirectoryCodec::groupKey(source->group_id));
            }
            check();
        }
        return result;
    }
public:
    // Restricted same-batch preparation of one affected existing cold block.
    // This does not open mode3 publication or authorize Entity Map changes.
    // Growth uses existing bounded private images and emits group-directory
    // edits for the owning coordinator to publish in the same atomic batch.
    [[nodiscard]] static HybridPropertyDemotionPreparation prepareColdBlock(MetadataWriteBatch &batch,
        const HybridPropertyReadContext &context, EntityPropertyKey anchor, SlottedSlotReference coldReference,
        std::span<const HybridColdRowInput> changes, std::shared_ptr<std::pmr::memory_resource> memory,
        const LargeValueStore *values = nullptr, const CancellationCheckpoint *cancellation = nullptr,
        std::span<const HybridPropertyLargeFieldPreparation> newRoots = {}) {
        return prepareColdBlockImpl(batch, context, anchor, coldReference, changes, memory, values,
            cancellation, HybridPropertyDemotionPreparation(memory), newRoots);
    }
    [[nodiscard]] static HybridPropertyDemotionPreparation prepareColdBlockWithHot(MetadataWriteBatch &batch,
        const HybridPropertyReadContext &context, std::span<const HybridPropertyColumnDelta *const> hotChanges,
        EntityPropertyKey anchor, SlottedSlotReference coldReference, std::span<const HybridColdRowInput> changes,
        std::shared_ptr<std::pmr::memory_resource> memory, const LargeValueStore *values = nullptr,
        const CancellationCheckpoint *cancellation = nullptr,
        std::span<const HybridPropertyLargeFieldPreparation> newRoots = {}) {
        hybrid_property_detail::require(hotChanges.size() <= 64U && changes.size() <= 64U - hotChanges.size(),
            "combined cold/hot preparation exceeds its original64change bound");
        // The original constructor authenticates every hot plan's database,
        // source/target generation, group and actual source column bytes.
        HybridPropertyDemotionPreparation prepared(batch, context.snapshot_, context.fields_, hotChanges, memory, cancellation);
        return prepareColdBlockImpl(batch, context, anchor, coldReference, changes, memory, values,
            cancellation, std::move(prepared), newRoots);
    }
private:
    // Only immutable original proposal objects can authenticate not-yet-visible
    // roots. Copied SOURCE rows never use this path: their original block
    // generation/manifests must still be checked against the source pin.
    static bool admitPreparedColdRoot(const MetadataWriteBatch &batch, const HybridColdRowInput &row,
        EntityPropertyKey owner, std::span<const uint32_t> hotTokens,
        std::span<const HybridPropertyLargeFieldPreparation> proposals,
        const std::shared_ptr<std::pmr::memory_resource> &memory, const CancellationCheckpoint *cancellation) {
        using hybrid_property_detail::require;
        if (!row.large_value_reference || proposals.empty()) return false;
        require(proposals.size() <= 4096U, "cold root proposals exceed the original membership frontier");
        utils::OwnedHeapReservation referenceCredit(memory, row.root.size() +
            3U * utils::detail::standardContainerProxyBytes());
        const auto reference = EntityPropertyLargeValueReferenceCodec::decode(row.root);
        if (reference.value.value_id.value() < batch.current().next_large_value_id) return false;
        require(reference.owner_entity_id == owner.entity_id && reference.owner_entity_type ==
            (owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId) &&
            reference.value.value_id.value() < batch.candidate().next_large_value_id,
            "new cold root has a foreign owner or unreserved batch identity");
        for (const auto &proposal : proposals) {
            if (cancellation) cancellation->check();
            if (proposal.reference().value_id != reference.value.value_id) continue;
            require(!proposal.reused() && proposal.databaseId() == batch.current().database_id &&
                proposal.sourceGeneration() == batch.current().generation && proposal.reference() == reference.value &&
                reference.value.kind == LargeValueKind::LVK_BYTES &&
                reference.value.media_type == RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE &&
                reference.value.generation <= batch.generation(), "new cold root lost its original immutable carrier proof");
            const auto payload = proposal.payload();
            require(payload.size() >= RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES &&
                payload.size() == reference.value.logical_bytes &&
                LittleEndianCodec::readU32(payload, 12) == payload.size() - RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES,
                "new cold root proposal has an invalid payload length");
            utils::OwnedHeapReservation directoryCredit(memory);
            const SelectedPropertyReadAdmission admit = [&](size_t bytes) {
                directoryCredit.set(bytes + utils::detail::standardContainerProxyBytes());
            };
            const auto directory = EntityPropertyLargeValuePayloadCodec::decodeDirectory(payload, &admit, cancellation);
            require(directory.size() == row.field_count, "new cold root proposal has a different field count");
            for (const auto &entry : directory) {
                if (cancellation) cancellation->check();
                require(!std::binary_search(hotTokens.begin(), hotTokens.end(), entry.key_token),
                    "new cold root proposal contains a globally hot token");
                const auto data = payload.subspan(RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES);
                require(entry.offset <= data.size() && entry.length <= data.size() - entry.offset && entry.length >= 4,
                    "new cold root proposal entry exceeds its actual payload");
                const auto bytes = data.subspan(entry.offset, entry.length);
                require(LittleEndianCodec::readU32(bytes, 0) == entry.key_token, "new cold root proposal entry has a foreign token");
                size_t offset = 0; const auto value = bytes.subspan(4);
                static_cast<void>(CanonicalPropertyValueCodec::inspectNext(value, offset));
                require(offset == value.size(), "new cold root proposal value has trailing bytes");
            }
            return true;
        }
        return false;
    }
    [[nodiscard]] static HybridPropertyDemotionPreparation prepareColdBlockImpl(MetadataWriteBatch &batch,
        const HybridPropertyReadContext &context, EntityPropertyKey anchor, SlottedSlotReference coldReference,
        std::span<const HybridColdRowInput> changes, const std::shared_ptr<std::pmr::memory_resource> &memory,
        const LargeValueStore *values, const CancellationCheckpoint *cancellation, HybridPropertyDemotionPreparation result,
        std::span<const HybridPropertyLargeFieldPreparation> newRoots,
        std::span<const uint32_t> removedTokens = {}, uint64_t pendingMask = 0) {
        using hybrid_property_detail::require;
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "cold block preparation is disabled");
        require(memory && !changes.empty() && changes.size() <= 64 &&
            (coldReference.isValid() || coldReference == SlottedSlotReference{}) && context.snapshot_.superblock() == batch.current(),
            "cold block preparation requires one source batch and bounded changes");
        utils::OwnedAllocationScope scope(memory);
        const auto check = [&] { context.check(); if (cancellation) cancellation->check(); };
        check(); utils::OwnedHeapReservation boundary(memory);
        const auto admit = [&](size_t bytes) { boundary.set(bytes); };
        SlottedSlotReference sourceGroupReference;
        const auto source = context.findGroup(anchor, admit, &sourceGroupReference, true, true);
        boundary.set(0);
        require(source && source->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
            (context.isLive(anchor, *source) || (pendingMask & (UINT64_C(1) << hybridPropertySlotFor(anchor)))),
            "cold block preparation requires an actual live or declared pending remainder owner");
        require(!(source->live & pendingMask), "cold block pending mask repeats a source live owner");
        auto groupReference = sourceGroupReference;
        if (!result.groups_.empty()) {
            require(result.groups_.size() == 1 && result.groups_.front().key == HybridPropertyDirectoryCodec::groupKey(source->group_id),
                "cold batch private group relocation has a foreign key");
            groupReference = HybridPropertyDirectoryCodec::decodeLocator(result.groups_.front().value);
        }
        if (result.group_.group_id) {
            require(result.group_.owner_kind == source->owner_kind && result.group_.group_id == source->group_id &&
                result.group_.mode == source->mode && result.group_.live == (source->live | pendingMask) &&
                removedTokens.size() <= source->columns && result.group_.columns == source->columns - removedTokens.size() &&
                result.group_.column_membership.page_id == groupReference.page_id,
                "cold/hot merge changes source group identity, liveness or hot ownership");
        } else result.group_ = *source;
        const auto previousPrivateGroup = result.group_;
        // Own the source matrix before another read can reuse snapshot caches.
        const auto matrixSource = [&] {
            const auto page = context.fields_.read(context.snapshot_, sourceGroupReference.page_id);
            require(page.view().readGroup(sourceGroupReference, source->owner_kind, source->group_id,
                context.descriptor_->semantic_generation) == *source, "cold block source group changed");
            return page.view().readColumnMembership(source->column_membership, *source,
                context.descriptor_->semantic_generation);
        }();
        // Reuse existing hot COW images. Never create a second independent
        // source copy that could overwrite hot values/presence on publication.
        result.pages_.reserve(result.pages_.size() + 2);
        const auto imageFor = [&](PageId id) -> HybridPropertyPageImage & {
            for (auto &page : result.pages_) if (page.pageId() == id) return page;
            result.pages_.push_back(context.fields_.cloneForMutation(context.snapshot_, id, batch.generation(), memory));
            return result.pages_.back();
        };
        (void)imageFor(groupReference.page_id);
        if (coldReference.isValid()) (void)imageFor(coldReference.page_id);
        require(imageFor(groupReference.page_id).readGroup(groupReference, source->owner_kind, source->group_id,
            batch.generation()) == previousPrivateGroup, "cold batch lost its current private group");
        auto matrix = imageFor(groupReference.page_id).readColumnMembership(previousPrivateGroup.column_membership,
            previousPrivateGroup, batch.generation());
        auto expectedHot = matrixSource;
        for (const auto token : removedTokens) expectedHot.eraseColumn(token);
        require(matrix.tokens == expectedHot.tokens && matrix.locators == expectedHot.locators,
            "compatible hot preparation changed cold binding or global hot identity");
        uint64_t affected = 0;
        for (unsigned slot = 0; slot < 64; ++slot)
            if ((matrixSource.cold_rows.owners & (UINT64_C(1) << slot)) && matrixSource.cold_rows.roots[slot] == coldReference) {
                require((matrix.cold_rows.owners & (UINT64_C(1) << slot)) && matrix.cold_rows.roots[slot] == coldReference,
                    "cold batch already modified this source owner"); affected |= UINT64_C(1) << slot;
            }
        require(coldReference.isValid() ? affected != 0 : affected == 0, "cold block source reference has no actual members");
        uint64_t touched = affected;
        for (const auto &row : changes) {
            require(row.owner_slot < 64, "cold change owner exceeds its group");
            const auto bit = UINT64_C(1) << row.owner_slot;
            require(((source->live | pendingMask) & bit) && (!(matrix.cold_rows.owners & bit) || (affected & bit)),
                "cold change targets a dead owner or another cold block"); touched |= bit;
        }
        for (unsigned slot = 0; slot < 64; ++slot) if (touched & (UINT64_C(1) << slot)) {
            check(); const EntityPropertyKey owner{source->owner_kind, (source->group_id - 1U) * 64U + slot + 1U};
            require(context.findGroup(owner, admit, nullptr, true, true) == source,
                "cold block owner Entity/ROW source changed");
            context.requireNoLegacySelector(owner, admit); boundary.set(0);
        }
        // Authenticate actual manifests and global hot-token exclusion before
        // minting replacement images. Unrelated blocks are never decoded.
        const auto inspect = [&](HybridColdRowInput row, Generation generation, std::span<const uint32_t> hotTokens) {
            const EntityPropertyKey owner{source->owner_kind, (source->group_id - 1U) * 64U + row.owner_slot + 1U};
            HybridPropertyColdRowInspection inspected(context.snapshot_, row, owner, generation,
                hotTokens, memory, values, cancellation);
            if (!row.large_value_reference) for (size_t i = 0; i < inspected.count(); ++i) {
                const std::array token{inspected.token(i)};
                inspected.visitSelected(token, [](size_t, auto) {});
            }
        };
        uint32_t previousFields = 0;
        if (coldReference.isValid()) context.withColdBlock(*source, matrixSource, coldReference, [&](const auto &block, const auto &) {
            previousFields = block.fields;
            for (unsigned slot = 0; slot < 64; ++slot)
                if (const auto row = block.row(slot)) { check(); inspect(*row, block.semantic_generation, std::span(matrixSource.tokens).first(matrixSource.column_count)); }
        });
        for (const auto &row : changes) if (!row.root.empty()) {
            check(); const EntityPropertyKey owner{source->owner_kind, (source->group_id - 1U) * 64U + row.owner_slot + 1U};
            if (!admitPreparedColdRoot(batch, row, owner, std::span(matrix.tokens).first(matrix.column_count),
                newRoots, memory, cancellation)) inspect(row, batch.current().generation, std::span(matrix.tokens).first(matrix.column_count));
        }
        utils::OwnedVector<std::byte> sourceBytes{utils::OwnedPmrAllocator<std::byte>(memory)};
        if (coldReference.isValid()) sourceBytes = imageFor(coldReference.page_id).readRecord(coldReference);
        const auto blocks = HybridPropertyColdRowBlock::repackBlocks(sourceBytes, source->owner_kind,
            source->group_id, batch.current().generation, batch.generation(), changes, memory, cancellation);
        // Reserve holders before allocating private IDs. No references into
        // pages_ survive growth. A failure discards this whole metadata batch.
        result.pages_.reserve(result.pages_.size() + blocks.size() + 1U); result.groups_.reserve(1);
        std::array<SlottedSlotReference, 64> replacements{};
        uint64_t replacementOwners = 0; uint32_t replacementFields = 0;
        const auto newPage = [&]() -> HybridPropertyPageImage & {
            const auto id = batch.allocateLogicalPageId();
            result.pages_.push_back(HybridPropertyPageImage::create(id, Generation(1), batch.generation(), memory));
            return result.pages_.back();
        };
        const auto capacityForOverwrite = [](const HybridPropertyPageImage &page, size_t oldBytes) {
            const auto stats = page.stats();
            return size_t(stats.contiguous_free_bytes) + stats.reclaimable_bytes + oldBytes;
        };
        const bool keepSlot = coldReference.isValid() && blocks.size() == 1 && blocks.front().size() <=
            capacityForOverwrite(imageFor(coldReference.page_id), sourceBytes.size());
        if (!keepSlot && coldReference.isValid()) imageFor(coldReference.page_id).erase(coldReference);
        std::optional<PageId> destination;
        for (const auto &bytes : blocks) {
            check(); const auto replacement = HybridPropertyColdRowBlock::decode(bytes, source->owner_kind, source->group_id,
                batch.generation(), memory, cancellation);
            require(!(replacementOwners & replacement.owners) && replacement.fields <= UINT32_MAX - replacementFields,
                "cold split duplicates owners or overflows field totals");
            replacementOwners |= replacement.owners; replacementFields += replacement.fields;
            SlottedSlotReference locator;
            if (keepSlot) {
                imageFor(coldReference.page_id).replaceColdRows(coldReference, bytes, source->owner_kind, source->group_id, cancellation);
                locator = coldReference;
            } else {
                // Keep growth records off original source pages, leaving room
                // for membership growth and avoiding unrelated source edits.
                if (!destination || imageFor(*destination).stats().contiguous_free_bytes < bytes.size() + RSS_SLOTTED_PAGE_SLOT_BYTES)
                    destination = newPage().pageId();
                locator = imageFor(*destination).appendColdRows(bytes, source->owner_kind, source->group_id, cancellation);
            }
            for (unsigned slot = 0; slot < 64; ++slot)
                if (replacement.owners & (UINT64_C(1) << slot)) replacements[slot] = locator;
        }
        require(previousFields <= matrix.cold_fields &&
            replacementFields <= UINT32_MAX - (matrix.cold_fields - previousFields), "cold block field totals overflow");
        matrix.cold_fields = matrix.cold_fields - previousFields + replacementFields;
        matrix.cold_rows.owners = (matrix.cold_rows.owners & ~affected) | replacementOwners;
        for (unsigned slot = 0; slot < 64; ++slot) {
            const auto bit = UINT64_C(1) << slot;
            if (replacementOwners & bit) matrix.cold_rows.roots[slot] = replacements[slot];
            else if (affected & bit) matrix.cold_rows.roots[slot] = {};
        }
        matrix.semantic_generation = batch.generation();
        result.group_.semantic_generation = batch.generation(); result.group_.memberships = matrix.totalMemberships();
        matrix.validateInGroup(result.group_, batch.generation(), batch.generation());
        const auto oldMatrixBytes = imageFor(groupReference.page_id).readRecord(previousPrivateGroup.column_membership).size();
        if (matrix.wireSize() <= capacityForOverwrite(imageFor(groupReference.page_id), oldMatrixBytes)) {
            auto &groupImage = imageFor(groupReference.page_id);
            groupImage.replaceColumnMembership(previousPrivateGroup.column_membership, matrix, previousPrivateGroup, result.group_, batch.generation());
            groupImage.replaceGroup(groupReference, result.group_, batch.generation());
        } else {
            // Keep the compact group and membership co-located. Existing hot
            // column locators still point to their original COW source pages.
            imageFor(groupReference.page_id).erase(groupReference);
            imageFor(groupReference.page_id).erase(previousPrivateGroup.column_membership);
            auto &page = newPage();
            result.group_.column_membership = page.appendColumnMembership(matrix, result.group_, batch.generation());
            const auto locator = page.appendGroup(result.group_);
            const HybridPropertyGroupDirectoryMap::Entry entry{HybridPropertyDirectoryCodec::groupKey(source->group_id),
                HybridPropertyDirectoryCodec::locatorValue(locator)};
            if (result.groups_.empty()) result.groups_.push_back(entry); else result.groups_.front() = entry;
        }
        check(); return result;
    }
public:
    using LargeFieldResolver = std::function<std::pmr::vector<std::byte>(EntityPropertyKey, uint32_t, const PropertyValue &)>;
    HybridPropertyDemotionPreparation(MetadataWriteBatch &batch, const MetadataSnapshotPin &snapshot,
        const HybridPropertyFieldPageStore &fields, const HybridPropertyColumnDelta &change,
        std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation = nullptr, const LargeFieldResolver &largeField = {})
        : HybridPropertyDemotionPreparation(batch, snapshot, fields,
            std::array<const HybridPropertyColumnDelta *, 1>{&change},
            std::move(memory), cancellation, largeField) {}
    HybridPropertyDemotionPreparation(MetadataWriteBatch &batch, const MetadataSnapshotPin &snapshot,
        const HybridPropertyFieldPageStore &fields, std::span<const HybridPropertyColumnDelta *const> changes,
        std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation = nullptr, const LargeFieldResolver &largeField = {})
        : HybridPropertyDemotionPreparation(batch, snapshot, fields, changes,
            std::move(memory), cancellation, largeField, false) {}
private:
    HybridPropertyDemotionPreparation(MetadataWriteBatch &batch, const MetadataSnapshotPin &snapshot,
        const HybridPropertyFieldPageStore &fields, std::span<const HybridPropertyColumnDelta *const> changes,
        std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation, const LargeFieldResolver &largeField, bool wholeOwner,
        std::span<const EntityPropertyKey> pendingOwners = {}, bool allowMultipleGroups = false, bool prepareColdPending = false)
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
            if (group->source.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER)
                require(plan->action() != HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD &&
                    !effects.clear_live && !(effects.set_live & ~pendingMask(group->source)) &&
                    (prepareColdPending || (!effects.set_live && !pendingMask(group->source))),
                    "cold ROW private preparation only admits compatible hot changes");
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
            if (group->source.usesColumnMembership())
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
            if (memberships < group.source.memberships && !group.source.usesColumnMembership()) erasedByKind[kindIndex(group.source.owner_kind)] += size_t(group.source.memberships - memberships);
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
                if (change.sourceGroup().usesColumnMembership()) continue;
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
            if (change.sourceGroup().mode != HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP)
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
                    case HybridPropertyColumnDelta::CellKind::HCC_SHORT_TEXT7: scalar = PropertyValue(HybridPropertyShortText::decode(cell.bits)); break;
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
        const HybridPropertyFieldPageStore &fields, std::span<const HybridPropertyColumnDelta *const> changes,
        std::span<const EntityPropertyKey> owners, std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation = nullptr, const LargeFieldResolver &largeField = {}, bool prepareColdPending = false) {
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
            const auto source = context.findGroup(owner, std::ref(admit), &reference, true, prepareColdPending); boundary.set(0);
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
            memory, cancellation, largeField, false, owners, true, prepareColdPending);
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
    [[nodiscard]] std::span<const HybridPropertyGroupDirectoryMap::Entry> groups(EntityPropertyOwnerKind kind) const & noexcept {
        return kind == group_.owner_kind ? std::span<const HybridPropertyGroupDirectoryMap::Entry>(groups_) :
            std::span<const HybridPropertyGroupDirectoryMap::Entry>(other_.groups);
    }
    std::span<const HybridPropertyGroupDirectoryMap::Entry> groups(EntityPropertyOwnerKind) const && = delete;
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
