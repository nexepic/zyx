/**
 * @file HybridPropertyConversionPreparation.hpp
 * @brief Metadata-owned private field images and final logical slot locators.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/debug/PerfTrace.hpp"
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
    enum class PlacementRole : uint8_t { HPPR_METADATA, HPPR_COLUMN, HPPR_COLD, HPPR_EXTENSION, HPPR_SPILL };
    struct PlacementLane { PlacementRole role; uint32_t token; size_t page; };
    // Private physical packing only: logical owner groups remain 64 entities.
    // This bounded state never survives the cohort's atomic publication.
    utils::OwnedVector<PlacementLane> lanes_;
    bool field_major_ = false;
    utils::OwnedVector<HybridPropertyPageImage> pages_;
    utils::OwnedVector<HybridPropertyOwnerMap::Entry> owners_;
    utils::OwnedVector<HybridPropertyFieldDirectoryMap::Entry> columns_, extensions_;
    HybridPropertyGroupDirectoryMap::Entry group_{};
    utils::OwnedVector<HybridPropertyGroupDirectoryMap::Entry> groups_;
    bool compact_ = false;

    using WholeRootResolver = std::function<std::pmr::vector<std::byte>(EntityPropertyKey,
        std::span<const BorrowedPropertyCollectionRootEntry>)>;
    using LargeFieldResolver = std::function<std::pmr::vector<std::byte>(EntityPropertyKey, uint32_t, const PropertyValue &)>;
    HybridPropertyConversionPreparation(MetadataWriteBatch &batch, const HybridPropertyGroupConversion &plan,
        const std::function<void()> &checkpoint = {}, const LargeFieldResolver &largeField = {}, bool compactColumns = false,
        bool coldRows = false, const WholeRootResolver &wholeRoot = {},
        bool fieldMajor = false, HybridPropertyConversionPreparation *cohort = nullptr)
        : lanes_(utils::OwnedPmrAllocator<PlacementLane>(plan.memoryOwner())), field_major_(fieldMajor),
          pages_(utils::OwnedPmrAllocator<HybridPropertyPageImage>(plan.memoryOwner())),
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
        compact_ = coldRows || (compactColumns && !plan.columns().empty());
        const auto extensionCount = std::count_if(plan.members().begin(), plan.members().end(), [](const auto &member) {
            return member.source == HybridPropertySource::HPS_EXTENSION;
        });
        owners_.reserve(coldRows ? 0U : compact_ ? size_t(extensionCount) : plan.members().size()); columns_.reserve(plan.columns().size());
        extensions_.reserve(coldRows ? 0U : size_t(extensionCount));
        if (cohort) {
            hybrid_property_detail::require(field_major_ && cohort->field_major_ &&
                pages_.get_allocator() == cohort->pages_.get_allocator(),
                "field-major cohort lost its original staging account");
            pages_.swap(cohort->pages_); lanes_.swap(cohort->lanes_);
        }
        const auto destination = [&](size_t bytes, PlacementRole role = PlacementRole::HPPR_METADATA,
                uint32_t token = 0U) -> HybridPropertyPageImage & {
            hybrid_property_detail::require(bytes <= RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES,
                "native field record exceeds a fresh slotted frame");
            constexpr size_t absent = (std::numeric_limits<size_t>::max)();
            size_t lane = absent;
            if (field_major_) {
                const auto locate = [&] {
                    for (size_t i = 0; i < lanes_.size(); ++i)
                        if (lanes_[i].role == role && lanes_[i].token == token) return i;
                    return absent;
                };
                lane = locate();
                if (lane == absent && role == PlacementRole::HPPR_COLUMN &&
                    std::count_if(lanes_.begin(), lanes_.end(), [](const auto &entry) {
                        return entry.role == PlacementRole::HPPR_COLUMN;
                    }) >= 64) {
                    // Wide heterogeneous schemas remain bounded and correct;
                    // only the overflow lane gives up same-token clustering.
                    role = PlacementRole::HPPR_SPILL; token = 0; lane = locate();
                }
                if (lane == absent) { lane = lanes_.size(); lanes_.push_back({role, token, absent}); }
            }
            size_t page = field_major_ ? lanes_[lane].page : pages_.empty() ? absent : pages_.size() - 1U;
            if (page == absent || pages_[page].stats().contiguous_free_bytes < bytes + RSS_SLOTTED_PAGE_SLOT_BYTES) {
                if (pages_.size() == pages_.capacity()) pages_.reserve(pages_.empty() ? 1U : pages_.size() * 2U);
                const auto id = batch.allocateLogicalPageId();
                pages_.push_back(HybridPropertyPageImage::create(id, Generation(1), batch.generation(), plan.memoryOwner()));
                page = pages_.size() - 1U;
                if (field_major_) {
                    lanes_[lane].page = page;
                    debug::PerfTrace::addValue("native_graph.hybrid.field_major.private_pages", 1);
                }
            } else if (field_major_) {
                debug::PerfTrace::addValue("native_graph.hybrid.field_major.destination_reuses", 1);
                if (role == PlacementRole::HPPR_COLUMN)
                    debug::PerfTrace::addValue("native_graph.hybrid.field_major.column_destination_reuses", 1);
            }
            return pages_[page];
        };
        std::optional<HybridPropertyColumnMembership> matrix;
        if (compact_ && !plan.columns().empty()) matrix = HybridPropertyColumnMembership::fromColumns(group, plan.columns());
        if (coldRows) {
            if (!matrix) {
                matrix.emplace(); matrix->owner_kind = group.owner_kind; matrix->group_id = group.group_id;
                matrix->semantic_generation = group.semantic_generation; matrix->live = group.live;
            }
            matrix->row_remainder = true;
        }
        const auto appendGroup = [&] {
            const auto slot = destination(64).appendGroup(group);
            group_ = {HybridPropertyDirectoryCodec::groupKey(group.group_id), HybridPropertyDirectoryCodec::locatorValue(slot)};
            groups_.push_back(group_);
        };
        // Preserve the existing noncompact preparation/cancellation ordering;
        // only compact membership needs its column locations first.
        if (!matrix) appendGroup();
        size_t columnOrdinal = 0;
        for (const auto &column : plan.columns()) {
            if (checkpoint) checkpoint();
            const auto slot = destination(column.wireSize(), PlacementRole::HPPR_COLUMN, column.identity.token).appendColumn(column, group, batch.generation());
            if (matrix) matrix->locators[columnOrdinal++] = slot;
            else columns_.push_back({HybridPropertyDirectoryCodec::columnKey(group.group_id, column.identity.token),
                    HybridPropertyDirectoryCodec::locatorValue(slot)});
        }
        if (coldRows) {
            // Source values remain borrowed. Only canonical bytes and existing
            // whole-root references are staged in the original account.
            using Entry = BorrowedPropertyCollectionRootEntry;
            utils::OwnedHeapReservation scratch(plan.memoryOwner(), sizeof(std::array<Entry, HybridPropertyGroupConversion::MAX_MEMBERSHIPS>));
            std::array<Entry, HybridPropertyGroupConversion::MAX_MEMBERSHIPS> selected{};
            utils::OwnedVector<std::pmr::vector<std::byte>> roots{utils::OwnedPmrAllocator<std::pmr::vector<std::byte>>(plan.memoryOwner())};
            utils::OwnedVector<HybridColdRowInput> rows{utils::OwnedPmrAllocator<HybridColdRowInput>(plan.memoryOwner())};
            roots.reserve(64); rows.reserve(64);
            const auto members = plan.members(); size_t first = 0;
            const auto kindType = group.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
            while (first < members.size()) {
                if (checkpoint) checkpoint();
                const auto ownerId = members[first].owner; size_t end = first, count = 0;
                while (end < members.size() && members[end].owner == ownerId) {
                    const auto &member = members[end++];
                    if (member.source == HybridPropertySource::HPS_EXTENSION) selected[count++] = {member.token, member.value};
                }
                first = end; if (!count) continue;
                const EntityPropertyKey owner{group.owner_kind, ownerId};
                const auto entries = std::span(selected).first(count);
                const auto size = EntityPropertyCollectionRootCodec::encodedSizeBorrowed(ownerId, kindType, entries, checkpoint);
                bool large = size > HybridPropertyColdRowBlock::MAX_UNCOMPRESSED_BYTES - HybridPropertyColdRowBlock::DIRECTORY_ENTRY_BYTES;
                std::pmr::vector<std::byte> bytes(plan.memoryOwner().get());
                if (!large) {
                    bytes = EntityPropertyCollectionRootCodec::encodeBorrowed(ownerId, kindType, entries, *plan.memoryOwner(), checkpoint);
                    if (size > HybridPropertyColdRowBlock::MAX_STORED_BYTES - HybridPropertyColdRowBlock::DIRECTORY_ENTRY_BYTES) {
                        const std::array<HybridColdRowInput, 1> probe{{{uint8_t(hybridPropertySlotFor(owner)), uint32_t(count), false, bytes}}};
                        try { static_cast<void>(HybridPropertyColdRowBlock::encode(group.owner_kind, group.group_id,
                            group.semantic_generation, probe, plan.memoryOwner())); }
                        catch (const std::length_error &) { large = true; }
                    }
                }
                if (large) {
                    if (!wholeRoot) throw std::length_error("cold ROW requires the same-batch whole-root LargeValue resolver");
                    bytes = wholeRoot(owner, entries);
                    utils::OwnedHeapReservation credit(plan.memoryOwner(), bytes.size());
                    const auto reference = EntityPropertyLargeValueReferenceCodec::decode(bytes);
                    hybrid_property_detail::require(reference.owner_entity_id == ownerId && reference.owner_entity_type == kindType &&
                        reference.value.kind == LargeValueKind::LVK_BYTES && reference.value.media_type == RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE &&
                        reference.value.generation <= group.semantic_generation &&
                        reference.value.logical_bytes >= RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES,
                        "cold ROW resolver returned a foreign, mistyped or future whole-root reference");
                }
                roots.push_back(std::move(bytes));
                rows.push_back({uint8_t(hybridPropertySlotFor(owner)), uint32_t(count), large, roots.back()});
            }
            first = 0;
            const auto rowBytes = [&](size_t i) { return HybridPropertyColdRowBlock::DIRECTORY_ENTRY_BYTES +
                (rows[i].large_value_reference ? 4U : 0U) + rows[i].root.size(); };
            while (first < rows.size()) {
                if (checkpoint) checkpoint();
                size_t end = first, size = 0;
                while (end < rows.size() && rowBytes(end) <= HybridPropertyColdRowBlock::MAX_UNCOMPRESSED_BYTES - size)
                    size += rowBytes(end++);
                hybrid_property_detail::require(end > first, "cold ROW reference exceeds its existing block bound");
                utils::OwnedVector<std::byte> encoded{utils::OwnedPmrAllocator<std::byte>(plan.memoryOwner())};
                const auto encode = [&] { return HybridPropertyColdRowBlock::encode(group.owner_kind, group.group_id,
                    group.semantic_generation, std::span(rows).subspan(first, end - first), plan.memoryOwner()); };
                try { encoded = encode(); }
                catch (const std::length_error &) {
                    // One retry at the RAW-safe prefix. A single larger row
                    // has already proved that its existing LZ4 form fits.
                    end = first + 1; size = rowBytes(first);
                    while (end < rows.size() && size <= HybridPropertyColdRowBlock::MAX_STORED_BYTES &&
                        rowBytes(end) <= HybridPropertyColdRowBlock::MAX_STORED_BYTES - size) size += rowBytes(end++);
                    encoded = encode();
                }
                const auto slot = destination(encoded.size(), PlacementRole::HPPR_COLD).appendColdRows(encoded, group.owner_kind, group.group_id);
                for (size_t i = first; i < end; ++i) {
                    matrix->cold_rows.owners |= UINT64_C(1) << rows[i].owner_slot;
                    matrix->cold_rows.roots[rows[i].owner_slot] = slot;
                    matrix->cold_fields += rows[i].field_count;
                }
                first = end;
            }
        }
        if (matrix) {
            // Reserve both records and both slot entries before choosing a
            // frame; group/membership must remain co-located even at page tails.
            group.column_membership = destination(matrix->wireSize() + 64U + RSS_SLOTTED_PAGE_SLOT_BYTES)
                .appendColumnMembership(*matrix, group, batch.generation());
            group.mode = coldRows ? HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER :
                HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP;
            appendGroup();
        }
        if (!coldRows) for (const auto &member : plan.members()) {
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
            const auto slot = destination(bytes.size(), PlacementRole::HPPR_EXTENSION).appendExtension(bytes, owner, member.token, group, batch.generation(),
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
