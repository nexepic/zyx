/**
 * @file HybridPropertyFieldView.hpp
 * @brief Borrowed typed access to authenticated hybrid field frames.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/storage/relocatable/entity/ScanBlockMembership.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/relocatable/property/HybridPropertyColumnMembership.hpp"
#include "graph/storage/relocatable/property/HybridPropertyColdRowBlock.hpp"
#include "graph/storage/relocatable/property/HybridPropertyExtension.hpp"

namespace graph::storage {
// The enclosing reader must authenticate Catalog, Page Directory, descriptor
// namespace and root under one pin before opening this payload. This view owns
// no pin and must not outlive or mutate its source. It grants no publication or
// generic ROW-page authority. Physical pool epochs are independent of semantic
// generations. Retired frames remain readable through a still-valid pinned
// Page Directory route; this view grants no permission to reuse them. Only the committed LSN is compared with the pinned visible LSN.
class HybridPropertyFieldView final {
public:
    struct ScalarCell {
        HybridPropertyScalarKind kind = HybridPropertyScalarKind::HPSK_INT64;
        bool present = false;
        bool is_null = false;
        uint64_t word = 0;
    };
    [[nodiscard]] static HybridPropertyFieldView open(const PageOwnerHeader &owner,
        std::span<const std::byte> payload, Generation visibleGeneration, LogSequenceNumber visibleLsn) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "hybrid field reading is disabled");
        hybrid_property_detail::require(owner.kind == PageOwnerKind::POK_HYBRID_PROPERTY &&
            (owner.flags & ~RSS_PAGE_OWNER_KNOWN_FLAGS) == 0 && owner.owner_id && visibleGeneration.isValid() && visibleLsn.isValid() &&
            owner.committed_lsn.isValid() && owner.committed_lsn <= visibleLsn,
            "hybrid field frame has an invalid owner or visibility");
        return HybridPropertyFieldView(SlottedPageReadView::open(owner, payload), visibleGeneration);
    }
    [[nodiscard]] ScanBlockMembership readScanMembership(const SlottedSlotReference &reference,
        DatabaseId database, uint64_t group, Generation descriptorGeneration) const {
        hybrid_property_detail::require(RSS_SCAN_PATH_FORMAT_ENABLED && descriptorGeneration.isValid() &&
            descriptorGeneration <= visible_generation_, "scan membership has an unsupported or future descriptor");
        return ScanBlockMembership::decode(page_.read(reference), database, descriptorGeneration, group);
    }
    [[nodiscard]] HybridPropertyColumnMembership readColumnMembership(const SlottedSlotReference &reference,
        const HybridPropertyGroupRecord &group, Generation descriptorGeneration) const {
        hybrid_property_detail::require(group.usesColumnMembership() &&
            group.column_membership == reference, "column membership slot does not belong to its group");
        return HybridPropertyColumnMembership::decode(page_.read(reference), group, descriptorGeneration, visible_generation_);
    }
    // The returned body is the caller's one accounted ROW representation, not
    // a cache. Whole-root LargeValue references still require the existing
    // storage layer's actual manifest/media/content admission before use.
    [[nodiscard]] HybridPropertyColdRowBlock::Decoded readColdRows(const SlottedSlotReference &reference,
        const HybridPropertyGroupRecord &group, const HybridPropertyColumnMembership &matrix,
        Generation descriptorGeneration, std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation = nullptr) const {
        return decodeColdRows(page_.read(reference), reference, group, matrix, descriptorGeneration,
            visible_generation_, std::move(memory), cancellation);
    }
    // Physical auditing already owns the exact authenticated slot bytes. Share
    // the same binding/owner/count checks without opening a second ROW body.
    [[nodiscard]] static HybridPropertyColdRowBlock::Decoded decodeColdRows(std::span<const std::byte> bytes,
        const SlottedSlotReference &reference, const HybridPropertyGroupRecord &group,
        const HybridPropertyColumnMembership &matrix, Generation descriptorGeneration, Generation visibleGeneration,
        std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation = nullptr) {
        matrix.validateInGroup(group, descriptorGeneration, visibleGeneration);
        hybrid_property_detail::require(group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER,
            "cold ROW block has no ROW-remainder group");
        uint64_t expected = 0;
        for (unsigned slot = 0; slot < 64; ++slot)
            if ((matrix.cold_rows.owners & (UINT64_C(1) << slot)) && matrix.cold_rows.roots[slot] == reference)
                expected |= UINT64_C(1) << slot;
        hybrid_property_detail::require(expected != 0, "cold ROW slot has no group-owned locator");
        auto block = HybridPropertyColdRowBlock::decode(bytes, group.owner_kind, group.group_id,
            group.semantic_generation, std::move(memory), cancellation);
        const auto remainingOwners = unsigned(std::popcount(matrix.cold_rows.owners & ~expected));
        hybrid_property_detail::require(block.owners == expected && block.fields <= matrix.cold_fields &&
            (remainingOwners ? matrix.cold_fields - block.fields >= remainingOwners : block.fields == matrix.cold_fields),
            "cold ROW block disagrees with its membership mask or field count");
        if (matrix.column_count) for (unsigned slot = 0; slot < 64; ++slot) {
            const auto row = block.row(slot);
            if (!row || row->large_value_reference) continue;
            if (cancellation) cancellation->check();
            EntityPropertyCollectionRootCodec::visitSelectedEncoded(row->root,
                (group.group_id - 1U) * 64U + slot + 1U,
                group.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId,
                std::span(matrix.tokens).first(matrix.column_count),
                [](size_t, std::optional<std::span<const std::byte>> value) {
                    hybrid_property_detail::require(!value, "cold ROW contains a group-owned hot token");
                }, nullptr, cancellation);
        }
        return block;
    }
    [[nodiscard]] HybridPropertyGroupRecord readGroup(const SlottedSlotReference &reference,
        EntityPropertyOwnerKind kind, uint64_t group, Generation descriptorGeneration) const {
        const auto result = HybridPropertyGroupRecord::decode(page_.read(reference), kind, group, descriptorGeneration, visible_generation_);
        hybrid_property_detail::require(!result.usesColumnMembership() ||
            (result.column_membership.page_id == reference.page_id && result.column_membership.slot_id != reference.slot_id),
            "compact group and membership must share one page with distinct slots");
        return result;
    }
    [[nodiscard]] HybridPropertyColumnRecord readColumn(const SlottedSlotReference &reference,
        const HybridPropertyColumnIdentity &identity, const HybridPropertyGroupRecord &group,
        Generation descriptorGeneration) const {
        return HybridPropertyColumnRecord::decode(page_.read(reference), identity, group,
            descriptorGeneration, visible_generation_);
    }
    // Directory locators do not carry a scalar type or field generation. Read
    // those untrusted fields from the bounded record, then validate them against
    // the expected token, namespace and authenticated group context.
    [[nodiscard]] HybridPropertyColumnRecord readColumn(const SlottedSlotReference &reference,
        const HybridPropertyGroupRecord &group, uint32_t token, Generation descriptorGeneration) const {
        const auto bytes = page_.read(reference);
        hybrid_property_detail::header(bytes, RSS_HYBRID_COLUMN_MAGIC);
        const HybridPropertyColumnIdentity expected{group.owner_kind,
            HybridPropertyScalarKind(std::to_integer<uint8_t>(bytes[7])), group.group_id, token,
            Generation(LittleEndianCodec::readU64(bytes, 24))};
        return HybridPropertyColumnRecord::decode(bytes, expected, group, descriptorGeneration, visible_generation_);
    }
    // Return exact persisted scalar bits, preserving NULL, signed integers,
    // NaN payloads and signed zero without a PropertyValue conversion layer.
    [[nodiscard]] ScalarCell readScalar(const SlottedSlotReference &reference,
        const HybridPropertyColumnIdentity &identity, const HybridPropertyGroupRecord &group,
        Generation descriptorGeneration, EntityPropertyKey owner) const {
        requireLiveOwner(owner, group);
        const auto column = readColumn(reference, identity, group, descriptorGeneration);
        const auto slot = hybridPropertySlotFor(owner);
        const auto bit = UINT64_C(1) << slot;
        return {column.identity.scalar_kind, bool(column.presence & bit), bool(column.nulls & bit), column.words[slot]};
    }
    [[nodiscard]] HybridPropertyExtensionView readExtension(const SlottedSlotReference &reference,
        EntityPropertyKey owner, uint32_t token, const HybridPropertyGroupRecord &group,
        Generation descriptorGeneration,
        const HybridPropertyExtensionView::ReferenceAdmission &referenceAdmission = {}) const & {
        requireLiveOwner(owner, group);
        return HybridPropertyExtensionView::open(page_.read(reference), owner, token,
            group.semantic_generation, descriptorGeneration, visible_generation_, referenceAdmission);
    }
    HybridPropertyExtensionView readExtension(const SlottedSlotReference &, EntityPropertyKey, uint32_t,
        const HybridPropertyGroupRecord &, Generation,
        const HybridPropertyExtensionView::ReferenceAdmission & = {}) const && = delete;
private:
    HybridPropertyFieldView(SlottedPageReadView page, Generation visible)
        : page_(std::move(page)), visible_generation_(visible) {}
    static void requireLiveOwner(EntityPropertyKey owner, const HybridPropertyGroupRecord &group) {
        group.validate();
        hybrid_property_detail::require(owner.isValid() && owner.owner_kind == group.owner_kind &&
            hybridPropertyGroupFor(owner) == group.group_id &&
            (group.live & (UINT64_C(1) << hybridPropertySlotFor(owner))),
            "hybrid field owner is absent or belongs to another group");
    }
    SlottedPageReadView page_;
    Generation visible_generation_;
};
} // namespace graph::storage
