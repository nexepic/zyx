/**
 * @file HybridPropertyPageImage.hpp
 * @brief Private shared slotted-page preparation for bounded hybrid fields.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <bitset>
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/property/HybridPropertyColumnMembership.hpp"
#include "graph/storage/relocatable/property/HybridPropertyColdRowBlock.hpp"
#include "graph/storage/relocatable/property/HybridPropertyExtension.hpp"
#include "graph/storage/relocatable/metadata/Catalog.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"

namespace graph::storage {
class HybridPropertyFieldPage;

// Prepublication images ONLY. Logical PageIds must ultimately come from the
// metadata-owned early coordinator in the SAME publication as their locators.
// This class allocates no IDs or file/pool/directory/descriptor. Its private
// owner binding consumes only a metadata-supplied header and writes no file.
// Its neutral SlottedPage must never be written as an ordinary ROW page.
// Full authenticated pool binding/routing/retirement is still required before
// nonempty descriptor guards can be removed. Fixed temporary codec/compaction
// workspace remains part of the enclosing bounded writer's scratch admission.
class HybridPropertyPageImage final {
public:
    [[nodiscard]] static HybridPropertyPageImage create(PageId id, Generation physicalPoolGeneration,
        Generation visibleGeneration, std::shared_ptr<std::pmr::memory_resource> memory) {
        require(bool(memory) && visibleGeneration.isValid(), "private hybrid image requires an account and visible identity");
        utils::OwnedAllocationScope scope(memory);
        return HybridPropertyPageImage(SlottedPage::create(id, physicalPoolGeneration), visibleGeneration, std::move(memory));
    }
    [[nodiscard]] static HybridPropertyPageImage open(std::span<const std::byte> bytes, PageId expectedPage,
        Generation expectedPhysicalPoolGeneration, Generation visibleGeneration,
        std::shared_ptr<std::pmr::memory_resource> memory) {
        require(bool(memory) && visibleGeneration.isValid() && expectedPage.isValid() &&
            expectedPhysicalPoolGeneration.isValid() && bytes.size() == RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
            "private hybrid image context is invalid");
        require(LittleEndianCodec::readU64(bytes, 8) == expectedPage.value() &&
            LittleEndianCodec::readU64(bytes, 16) == expectedPhysicalPoolGeneration.value(),
            "private hybrid image has a foreign page or physical generation");
        auto page = SlottedPage::open(bytes, memory); // authenticates CRC/layout/slot generations
        return HybridPropertyPageImage(std::move(page), visibleGeneration, std::move(memory));
    }
    HybridPropertyPageImage(const HybridPropertyPageImage &) = delete;
    HybridPropertyPageImage &operator=(const HybridPropertyPageImage &) = delete;
    HybridPropertyPageImage(HybridPropertyPageImage &&) noexcept = default;
    HybridPropertyPageImage &operator=(HybridPropertyPageImage &&) noexcept = default;

    [[nodiscard]] HybridPropertyPageImage clone(Generation visibleGeneration,
        std::shared_ptr<std::pmr::memory_resource> memory) const {
        require(bool(memory) && visibleGeneration >= visible_generation_, "private hybrid clone has invalid admission or visibility");
        utils::OwnedAllocationScope scope(memory);
        auto result = HybridPropertyPageImage(page_.clone(), visibleGeneration, std::move(memory));
        result.source_slots_ = source_slots_;
        return result;
    }
    [[nodiscard]] PageId pageId() const { return page_.pageId(); }
    [[nodiscard]] uint16_t liveSlotCount() const { return page_.stats().live_slot_count; }
    [[nodiscard]] Generation physicalPoolGeneration() const { return page_.pageGeneration(); }
    [[nodiscard]] SlottedPageStats stats() const { return page_.stats(); }
    [[nodiscard]] utils::OwnedVector<std::byte> serialize() const { return page_.serializeOwned(memory_); }
    [[nodiscard]] utils::OwnedVector<std::byte> readRecord(const SlottedSlotReference &reference) const {
        return page_.readOwned(reference, memory_);
    }
    [[nodiscard]] SlottedSlotReference appendGroup(const HybridPropertyGroupRecord &group) {
        group.validate(); require(group.semantic_generation <= visible_generation_, "hybrid group is not visible");
        if (group.usesColumnMembership()) {
            require(group.column_membership.page_id == page_.pageId(), "compact group membership must share its page");
            static_cast<void>(readColumnMembership(group.column_membership, group, visible_generation_));
        }
        return appendRecord(group.encode());
    }
    [[nodiscard]] SlottedSlotReference appendColumn(const HybridPropertyColumnRecord &column,
        const HybridPropertyGroupRecord &group, Generation descriptorGeneration) {
        column.validateInGroup(group, descriptorGeneration, visible_generation_);
        std::array<std::byte, 576> encoded{}; auto bytes = std::span(encoded).first(column.wireSize());
        column.encode(bytes); return appendRecord(bytes);
    }
    // Local byte/identity admission only. The coordinator must bind the result
    // in the same group membership and authenticate actual LargeValue content.
    [[nodiscard]] SlottedSlotReference appendColdRows(std::span<const std::byte> bytes,
        EntityPropertyOwnerKind kind, uint64_t group, const CancellationCheckpoint *cancellation = nullptr) {
        static_cast<void>(HybridPropertyColdRowBlock::decode(bytes, kind, group,
            visible_generation_, memory_, cancellation));
        return appendRecord(bytes);
    }
    void replaceColdRows(const SlottedSlotReference &reference, std::span<const std::byte> bytes,
        EntityPropertyOwnerKind kind, uint64_t group, const CancellationCheckpoint *cancellation = nullptr) {
        const auto previous = HybridPropertyColdRowBlock::decode(readRecord(reference), kind, group,
            visible_generation_, memory_, cancellation);
        const auto replacement = HybridPropertyColdRowBlock::decode(bytes, kind, group,
            visible_generation_, memory_, cancellation);
        require(replacement.semantic_generation >= previous.semantic_generation,
            "cold ROW replacement moves semantic identity backwards");
        overwriteRecord(reference, bytes);
    }
    [[nodiscard]] SlottedSlotReference appendExtension(std::span<const std::byte> bytes, EntityPropertyKey owner,
        uint32_t token, const HybridPropertyGroupRecord &group, Generation descriptorGeneration,
        const HybridPropertyExtensionView::ReferenceAdmission &referenceAdmission = {}) {
        validateExtension(bytes, owner, token, group, descriptorGeneration, referenceAdmission);
        return appendRecord(bytes);
    }
    [[nodiscard]] HybridPropertyGroupRecord readGroup(const SlottedSlotReference &reference,
        EntityPropertyOwnerKind kind, uint64_t group, Generation descriptorGeneration) const {
        const auto bytes = readRecord(reference);
        const auto result = HybridPropertyGroupRecord::decode(bytes, kind, group, descriptorGeneration, visible_generation_);
        hybrid_property_detail::require(!result.usesColumnMembership() ||
            (result.column_membership.page_id == reference.page_id && result.column_membership.slot_id != reference.slot_id),
            "compact group and membership must share one page with distinct slots");
        return result;
    }
    [[nodiscard]] HybridPropertyColumnRecord readColumn(const SlottedSlotReference &reference,
        const HybridPropertyColumnIdentity &expected, const HybridPropertyGroupRecord &group,
        Generation descriptorGeneration) const {
        const auto bytes = readRecord(reference);
        return HybridPropertyColumnRecord::decode(bytes, expected, group, descriptorGeneration, visible_generation_);
    }
    [[nodiscard]] SlottedSlotReference appendColumnMembership(const HybridPropertyColumnMembership &value,
        const HybridPropertyGroupRecord &group, Generation descriptorGeneration) {
        value.validateInGroup(group, descriptorGeneration, visible_generation_);
        std::array<std::byte, HybridPropertyColumnMembership::MAX_BYTES> encoded{};
        const auto bytes = std::span(encoded).first(value.wireSize()); value.encode(bytes);
        return appendRecord(bytes);
    }
    [[nodiscard]] HybridPropertyColumnMembership readColumnMembership(const SlottedSlotReference &reference,
        const HybridPropertyGroupRecord &group, Generation descriptorGeneration) const {
        hybrid_property_detail::require(group.usesColumnMembership() &&
            group.column_membership == reference, "column membership slot does not belong to its group");
        return HybridPropertyColumnMembership::decode(readRecord(reference), group, descriptorGeneration, visible_generation_);
    }
    void replaceColumnMembership(const SlottedSlotReference &reference, const HybridPropertyColumnMembership &replacement,
        const HybridPropertyGroupRecord &previousGroup, const HybridPropertyGroupRecord &group, Generation descriptorGeneration) {
        const auto previous = readColumnMembership(reference, previousGroup, descriptorGeneration);
        require(replacement.semantic_generation >= previous.semantic_generation,
            "column membership replacement moves its semantic identity backwards");
        replacement.validateInGroup(group, descriptorGeneration, visible_generation_);
        std::array<std::byte, HybridPropertyColumnMembership::MAX_BYTES> encoded{};
        const auto bytes = std::span(encoded).first(replacement.wireSize()); replacement.encode(bytes); overwriteRecord(reference, bytes);
    }
    void replaceGroup(const SlottedSlotReference &reference, const HybridPropertyGroupRecord &replacement,
        Generation descriptorGeneration) {
        const auto previous = readGroup(reference, replacement.owner_kind, replacement.group_id, descriptorGeneration);
        replacement.validate();
        if (replacement.usesColumnMembership())
            static_cast<void>(readColumnMembership(replacement.column_membership, replacement, descriptorGeneration));
        require(replacement.semantic_generation >= previous.semantic_generation &&
            replacement.semantic_generation <= descriptorGeneration && descriptorGeneration <= visible_generation_,
            "hybrid group replacement moves semantic identity backwards or into the future");
        overwriteRecord(reference, replacement.encode());
    }
    void replaceColumn(const SlottedSlotReference &reference, const HybridPropertyColumnIdentity &expectedPrevious,
        const HybridPropertyColumnRecord &replacement, const HybridPropertyGroupRecord &group,
        Generation descriptorGeneration) {
        const auto previousBytes = readRecord(reference);
        static_cast<void>(HybridPropertyColumnRecord::decodePayload(previousBytes, expectedPrevious));
        const auto &identity = replacement.identity;
        require(identity.owner_kind == expectedPrevious.owner_kind && identity.scalar_kind == expectedPrevious.scalar_kind &&
            identity.group_id == expectedPrevious.group_id && identity.token == expectedPrevious.token &&
            identity.semantic_generation >= expectedPrevious.semantic_generation,
            "hybrid column replacement changes field identity or requires demotion");
        replacement.validateInGroup(group, descriptorGeneration, visible_generation_);
        std::array<std::byte, 576> encoded{}; auto bytes = std::span(encoded).first(replacement.wireSize());
        replacement.encode(bytes); overwriteRecord(reference, bytes);
    }
    void replaceExtension(const SlottedSlotReference &reference, std::span<const std::byte> replacement,
        EntityPropertyKey owner, uint32_t token, const HybridPropertyGroupRecord &group, Generation descriptorGeneration,
        const HybridPropertyExtensionView::ReferenceAdmission &referenceAdmission = {}) {
        const auto previous = readRecord(reference);
        validateExtension(previous, owner, token, group, descriptorGeneration, referenceAdmission);
        validateExtension(replacement, owner, token, group, descriptorGeneration, referenceAdmission);
        require(LittleEndianCodec::readU64(replacement, 24) >= LittleEndianCodec::readU64(previous, 24),
            "hybrid extension replacement moves semantic identity backwards");
        overwriteRecord(reference, replacement);
    }
    // Consume a private image into its metadata-assigned physical frame owner.
    // This binds bytes only: ordinary pool publication/read matching still
    // rejects this kind until the complete atomic field coordinator is installed.
    [[nodiscard]] SlottedPage bindPrivateOwner(const PageOwnerHeader &owner, const CatalogEntry &pool,
        LogSequenceNumber expectedLsn,
        const HybridPropertyExtensionView::ReferenceAdmission &referenceAdmission = {}) && {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "hybrid private owner binding is disabled");
        require(pool.segment_id.isValid() && pool.physical_slot.isValid() &&
            pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && pool.state == SegmentState::SGS_SEALED &&
            pool.owner_id && owner.owner_id == pool.owner_id && pool.generation.isValid() &&
            owner.generation == pool.generation && pool.committed_lsn.isValid() &&
            expectedLsn.isValid() && pool.committed_lsn <= expectedLsn && owner.committed_lsn == expectedLsn &&
            owner.kind == PageOwnerKind::POK_HYBRID_PROPERTY && owner.flags == 0 &&
            owner.page_id == page_.pageId(), "hybrid private frame owner does not match its assigned pool/publication");
        SlottedPage::validateOwner(owner);
        // Validate new/changed and noncold records before rebinding physical
        // ownership. Authenticated unchanged cold source bytes need no ROW
        // decoding. This is local shape/visibility, not global graph authority.
        const auto references = page_.liveSlotsOwned(memory_);
        for (const auto &reference : references) {
            const auto bytes = readRecord(reference);
            if (reference.slot_id < source_slots_.size() && source_slots_.test(reference.slot_id) &&
                bytes.size() >= sizeof(uint32_t) && LittleEndianCodec::readU32(bytes, 0) == HybridPropertyColdRowBlock::MAGIC) {
                // Page CRC/layout/owner were authenticated by FieldPageStore.
                // This untouched source record is copied, not read as a ROW.
                // New/changed records and ordinary open() never have this proof.
                debug::PerfTrace::addValue("native_graph.hybrid.unchanged_cold_bindings", 1);
                continue;
            }
            validatePrivateRecord(bytes, referenceAdmission);
        }
        page_.rebindOwner(owner); // no allocation after the successful checks
        return std::move(page_);
    }
    void erase(const SlottedSlotReference &reference) { forgetSourceSlot(reference); page_.erase(reference); }
    void compact() { page_.compact(); }
private:
    friend class HybridPropertyFieldPage;
    // Only a real snapshot-authenticated FieldPage can mint this transient
    // proof. Future slots are cleared by appendRecord before they can bind.
    void trustSnapshotClone() { source_slots_.set(); }
    void forgetSourceSlot(const SlottedSlotReference &reference) {
        if (reference.slot_id < source_slots_.size()) source_slots_.reset(reference.slot_id);
    }
    [[nodiscard]] SlottedSlotReference appendRecord(std::span<const std::byte> bytes) {
        const auto original = source_slots_;
        source_slots_.reset(); // an insertion exception cannot leave stale source proof
        const auto reference = page_.insert(bytes);
        source_slots_ = original; forgetSourceSlot(reference); return reference;
    }
    void overwriteRecord(const SlottedSlotReference &reference, std::span<const std::byte> bytes) {
        forgetSourceSlot(reference); // fail closed even if a private overwrite refuses
        page_.overwrite(reference, bytes);
    }
    static void require(bool value, const char *message) { hybrid_property_detail::require(value, message); }
    HybridPropertyPageImage(SlottedPage page, Generation visible, std::shared_ptr<std::pmr::memory_resource> memory)
        : page_(std::move(page)), visible_generation_(visible), memory_(std::move(memory)) {}
    void validateExtension(std::span<const std::byte> bytes, EntityPropertyKey owner, uint32_t token,
        const HybridPropertyGroupRecord &group, Generation descriptorGeneration,
        const HybridPropertyExtensionView::ReferenceAdmission &referenceAdmission) const {
        group.validate();
        require(group.owner_kind == owner.owner_kind && group.group_id == hybridPropertyGroupFor(owner) &&
            group.memberships && (group.live & (UINT64_C(1) << hybridPropertySlotFor(owner))),
            "hybrid extension has no live owner in its expected group");
        static_cast<void>(HybridPropertyExtensionView::open(bytes, owner, token, group.semantic_generation,
            descriptorGeneration, visible_generation_, referenceAdmission));
    }
    void validatePrivateRecord(std::span<const std::byte> bytes,
        const HybridPropertyExtensionView::ReferenceAdmission &referenceAdmission) const {
        require(bytes.size() >= sizeof(uint32_t), "hybrid private slot has no typed record envelope");
        const auto magic = LittleEndianCodec::readU32(bytes, 0);
        if (magic == RSS_HYBRID_GROUP_MAGIC) {
            require(bytes.size() >= 64, "hybrid private group record is truncated");
            const auto group = HybridPropertyGroupRecord::decode(bytes,
                EntityPropertyOwnerKind(std::to_integer<uint8_t>(bytes[6])), LittleEndianCodec::readU64(bytes, 8),
                visible_generation_, visible_generation_);
            if (group.usesColumnMembership()) {
                require(group.column_membership.page_id == page_.pageId(), "compact group membership is on another page");
                static_cast<void>(readColumnMembership(group.column_membership, group, visible_generation_));
            }
        } else if (magic == HybridPropertyColumnMembership::MAGIC) {
            require(bytes.size() >= 64, "hybrid private membership record is truncated");
            static_cast<void>(HybridPropertyColumnMembership::decodePayload(bytes,
                EntityPropertyOwnerKind(std::to_integer<uint8_t>(bytes[6])), LittleEndianCodec::readU64(bytes, 8), visible_generation_));
        } else if (magic == HybridPropertyColdRowBlock::MAGIC) {
            require(bytes.size() >= HybridPropertyColdRowBlock::HEADER_BYTES, "hybrid private cold ROW record is truncated");
            static_cast<void>(HybridPropertyColdRowBlock::decode(bytes,
                EntityPropertyOwnerKind(std::to_integer<uint8_t>(bytes[7])), LittleEndianCodec::readU64(bytes, 8),
                visible_generation_, memory_));
        } else if (magic == RSS_HYBRID_COLUMN_MAGIC) {
            require(bytes.size() >= 64, "hybrid private column record is truncated");
            const auto record = HybridPropertyColumnRecord::decodePayload(bytes,
                {EntityPropertyOwnerKind(std::to_integer<uint8_t>(bytes[6])),
                    HybridPropertyScalarKind(std::to_integer<uint8_t>(bytes[7])), LittleEndianCodec::readU64(bytes, 8),
                    LittleEndianCodec::readU32(bytes, 16), Generation(LittleEndianCodec::readU64(bytes, 24))});
            require(record.identity.semantic_generation <= visible_generation_, "hybrid private column is not visible");
        } else if (magic == RSS_HYBRID_EXTENSION_MAGIC) {
            require(bytes.size() >= RSS_HYBRID_EXTENSION_HEADER_BYTES, "hybrid private extension record is truncated");
            static_cast<void>(HybridPropertyExtensionView::open(bytes,
                {EntityPropertyOwnerKind(std::to_integer<uint8_t>(bytes[6])), LittleEndianCodec::readU64(bytes, 8)},
                LittleEndianCodec::readU32(bytes, 16), visible_generation_, visible_generation_, visible_generation_,
                referenceAdmission));
        } else require(false, "hybrid private page contains an unknown record kind");
    }
    SlottedPage page_;
    // Fixed per-page COW provenance, included in the existing holder charge.
    // Not serialized, not a read cache, and never a publication capability.
    std::bitset<RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES / RSS_SLOTTED_PAGE_SLOT_BYTES> source_slots_{};
    // Semantic visibility is independent of the shared pool's physical reuse
    // generation. A normal later commit need not change that pool generation.
    Generation visible_generation_;
    std::shared_ptr<std::pmr::memory_resource> memory_;
};
} // namespace graph::storage
