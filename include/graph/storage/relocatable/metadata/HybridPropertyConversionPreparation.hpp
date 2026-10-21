/**
 * @file HybridPropertyConversionPreparation.hpp
 * @brief Metadata-owned private field images and final logical slot locators.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectory.hpp"
#include "graph/storage/relocatable/property/HybridPropertyGroupConversion.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPageImage.hpp"

namespace graph::storage {
class RelocatableMetadataStore;
class HybridPropertyConversionPreparationTestAccess;

// Private early preparation only. The native metadata writer owns the batch;
// ordinary graph callers cannot reserve IDs through this type. Physical pools,
// map trees and final descriptor still need the complete atomic coordinator.
// On failure discard the enclosing batch: no ID or field is separately
// published, and the old graph remains authoritative. Oversized fields require
// the enclosing writer's accounted immutable-value resolver; no value is
// independently published and missing resolvers refuse without truncation.
// The eventual writer must supply the plan's shared conversion account with
// the documented <=1 MiB staging cap as well as its parent QUERY/WRITE limit.
// This private helper does not infer a quota from an arbitrary PMR resource.
class HybridPropertyConversionPreparation final {
    friend class RelocatableMetadataStore;
    friend class HybridPropertyConversionPreparationTestAccess;
    utils::OwnedVector<HybridPropertyPageImage> pages_;
    utils::OwnedVector<HybridPropertyOwnerMap::Entry> owners_;
    utils::OwnedVector<HybridPropertyFieldDirectoryMap::Entry> columns_, extensions_;
    HybridPropertyGroupDirectoryMap::Entry group_{};
    utils::OwnedVector<HybridPropertyGroupDirectoryMap::Entry> groups_;
    bool compact_ = false;

    using LargeFieldResolver = std::function<std::pmr::vector<std::byte>(EntityPropertyKey, uint32_t, const PropertyValue &)>;
    HybridPropertyConversionPreparation(MetadataWriteBatch &batch, const HybridPropertyGroupConversion &plan,
        const std::function<void()> &checkpoint = {}, const LargeFieldResolver &largeField = {}, bool compactColumns = false)
        : pages_(utils::OwnedPmrAllocator<HybridPropertyPageImage>(plan.memoryOwner())),
          owners_(utils::OwnedPmrAllocator<HybridPropertyOwnerMap::Entry>(plan.memoryOwner())),
          columns_(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Entry>(plan.memoryOwner())),
          extensions_(utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Entry>(plan.memoryOwner())),
          groups_(utils::OwnedPmrAllocator<HybridPropertyGroupDirectoryMap::Entry>(plan.memoryOwner())) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native field preparation is disabled");
        auto group = plan.group();
        hybrid_property_detail::require(group.semantic_generation == batch.generation() &&
            batch.candidate().database_id.isValid(), "native field preparation has a stale publication generation");
        if (checkpoint) checkpoint();
        compact_ = compactColumns && !plan.columns().empty();
        const auto extensionCount = std::count_if(plan.members().begin(), plan.members().end(), [](const auto &member) {
            return member.source == HybridPropertySource::HPS_EXTENSION;
        });
        owners_.reserve(compact_ ? size_t(extensionCount) : plan.members().size()); columns_.reserve(plan.columns().size());
        extensions_.reserve(size_t(extensionCount));
        const auto destination = [&](size_t bytes) -> HybridPropertyPageImage & {
            hybrid_property_detail::require(bytes <= RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES,
                "native field record exceeds a fresh slotted frame");
            if (pages_.empty() || pages_.back().stats().contiguous_free_bytes < bytes + RSS_SLOTTED_PAGE_SLOT_BYTES) {
                // Grow the charged holder before consuming an identity. These
                // pages are append-only, so contiguous free is exact here.
                if (pages_.size() == pages_.capacity()) pages_.reserve(pages_.empty() ? 1U : pages_.size() * 2U);
                const auto id = batch.allocateLogicalPageId();
                // Neutral physical epoch is never persisted. Later private
                // owner binding installs the actual assigned pool epoch.
                pages_.push_back(HybridPropertyPageImage::create(id, Generation(1), batch.generation(), plan.memoryOwner()));
            }
            return pages_.back();
        };
        if (compact_) {
            const auto matrix = HybridPropertyColumnMembership::fromColumns(group, plan.columns());
            group.column_membership = destination(matrix.wireSize()).appendColumnMembership(matrix, group, batch.generation());
            group.mode = HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP;
        }
        const auto groupSlot = destination(64).appendGroup(group);
        group_ = {HybridPropertyDirectoryCodec::groupKey(group.group_id), HybridPropertyDirectoryCodec::locatorValue(groupSlot)};
        groups_.push_back(group_);
        for (const auto &column : plan.columns()) {
            if (checkpoint) checkpoint();
            const auto slot = destination(column.wireSize()).appendColumn(column, group, batch.generation());
            columns_.push_back({HybridPropertyDirectoryCodec::columnKey(group.group_id, column.identity.token),
                HybridPropertyDirectoryCodec::locatorValue(slot)});
        }
        for (const auto &member : plan.members()) {
            if (checkpoint) checkpoint();
            const EntityPropertyKey owner{group.owner_kind, member.owner};
            if (!compact_ || member.source == HybridPropertySource::HPS_EXTENSION)
                owners_.push_back({HybridPropertyDirectoryCodec::ownerKey(owner, member.token),
                    HybridPropertyDirectoryCodec::sourceValue(member.source)});
            if (member.source != HybridPropertySource::HPS_EXTENSION) continue;
            const auto canonicalBytes = CanonicalPropertyValueCodec::encodedSize(*member.value);
            const bool oversized = canonicalBytes > RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES - RSS_HYBRID_EXTENSION_HEADER_BYTES;
            const auto bytes = oversized && largeField ? largeField(owner, member.token, *member.value) :
                plan.encodeNativeInlineExtension(member.owner, member.token,
                    RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, *plan.memoryOwner());
            utils::OwnedHeapReservation referenceCredit(plan.memoryOwner());
            const auto admitReference = [&](size_t retained) { referenceCredit.set(retained); };
            const auto slot = destination(bytes.size()).appendExtension(bytes, owner, member.token, group, batch.generation(),
                std::ref(admitReference));
            extensions_.push_back({HybridPropertyDirectoryCodec::ownerKey(owner, member.token),
                HybridPropertyDirectoryCodec::locatorValue(slot)});
        }
    }
public:
    HybridPropertyConversionPreparation(const HybridPropertyConversionPreparation &) = delete;
    HybridPropertyConversionPreparation &operator=(const HybridPropertyConversionPreparation &) = delete;
    [[nodiscard]] std::span<const HybridPropertyPageImage> pages() const & noexcept { return pages_; }
    std::span<const HybridPropertyPageImage> pages() const && = delete;
    [[nodiscard]] std::span<const HybridPropertyOwnerMap::Entry> owners() const & noexcept { return owners_; }
    std::span<const HybridPropertyOwnerMap::Entry> owners() const && = delete;
    [[nodiscard]] std::span<const HybridPropertyFieldDirectoryMap::Entry> columns() const & noexcept { return columns_; }
    std::span<const HybridPropertyFieldDirectoryMap::Entry> columns() const && = delete;
    [[nodiscard]] std::span<const HybridPropertyFieldDirectoryMap::Entry> extensions() const & noexcept { return extensions_; }
    std::span<const HybridPropertyFieldDirectoryMap::Entry> extensions() const && = delete;
    [[nodiscard]] const HybridPropertyGroupDirectoryMap::Entry &group() const & noexcept { return group_; }
    const HybridPropertyGroupDirectoryMap::Entry &group() const && = delete;
};
} // namespace graph::storage
