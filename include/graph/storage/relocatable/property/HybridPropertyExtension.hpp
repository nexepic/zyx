/**
 * @file HybridPropertyExtension.hpp
 * @brief Single-field extension envelopes for the isolated hybrid candidate.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <functional>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>
#include "graph/storage/relocatable/allocation/SlottedPage.hpp"
#include "graph/storage/relocatable/property/HybridPropertyRecords.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"

namespace graph::storage {
constexpr uint32_t RSS_HYBRID_EXTENSION_MAGIC = 0x4548595AU; // ZYHE
constexpr size_t RSS_HYBRID_EXTENSION_HEADER_BYTES = 48;
constexpr size_t RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES =
    RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - RSS_SLOTTED_PAGE_SLOT_BYTES;
enum class HybridPropertyExtensionMode : uint8_t { HPEM_INLINE = 1, HPEM_LARGE_REFERENCE = 2 };

// This is an uninstalled candidate envelope, not a file writer or graph API.
// Inline bodies contain exactly one existing canonical PropertyValue. Large
// bodies reuse the existing owner-neutral Property payload/media, constrained
// to exactly one token/value by decodeLargePayload. Never accept a whole row
// merely because it has the same LargeValue media type. Actual LargeValueStore
// reads must authenticate reference/manifest/content before calling that method.
class HybridPropertyExtensionView final {
public:
    HybridPropertyExtensionView(const HybridPropertyExtensionView &) = delete;
    HybridPropertyExtensionView &operator=(const HybridPropertyExtensionView &) = delete;
    HybridPropertyExtensionView(HybridPropertyExtensionView &&) noexcept = default;
    HybridPropertyExtensionView &operator=(HybridPropertyExtensionView &&) noexcept = default;
    // Reference decode constructs a bounded media string. Admission is mandatory
    // for that branch and reports absolute requested then observed capacity bytes;
    // the caller keeps its reservation until the returned view is destroyed.
    using ReferenceAdmission = std::function<void(size_t)>;

    [[nodiscard]] static HybridPropertyExtensionView open(std::span<const std::byte> bytes,
        EntityPropertyKey expectedOwner, uint32_t expectedToken, Generation groupGeneration,
        Generation descriptorGeneration, Generation visibleGeneration,
        const ReferenceAdmission &referenceAdmission = {}) {
        using namespace hybrid_property_detail;
        require(bytes.size() >= RSS_HYBRID_EXTENSION_HEADER_BYTES &&
            bytes.size() <= RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, "hybrid extension record width is invalid");
        require(LittleEndianCodec::readU32(bytes, 0) == RSS_HYBRID_EXTENSION_MAGIC &&
            LittleEndianCodec::readU16(bytes, 4) == RSS_HYBRID_RECORD_VERSION,
            "hybrid extension magic or version mismatch");
        const EntityPropertyKey owner{EntityPropertyOwnerKind(std::to_integer<uint8_t>(bytes[6])),
            LittleEndianCodec::readU64(bytes, 8)};
        const auto token = LittleEndianCodec::readU32(bytes, 16);
        const auto generation = Generation(LittleEndianCodec::readU64(bytes, 24));
        const auto mode = HybridPropertyExtensionMode(std::to_integer<uint8_t>(bytes[7]));
        require(owner.isValid() && owner == expectedOwner && token && token == expectedToken &&
            generation.isValid() && generation <= groupGeneration && groupGeneration <= descriptorGeneration &&
            descriptorGeneration <= visibleGeneration && LittleEndianCodec::readU32(bytes, 20) ==
                bytes.size() - RSS_HYBRID_EXTENSION_HEADER_BYTES,
            "hybrid extension owner, token, generation or payload length mismatch");
        zero(bytes, 32, RSS_HYBRID_EXTENSION_HEADER_BYTES);
        HybridPropertyExtensionView result(bytes.subspan(RSS_HYBRID_EXTENSION_HEADER_BYTES), owner, token, generation, mode);
        if (mode == HybridPropertyExtensionMode::HPEM_INLINE) {
            size_t offset = 0;
            result.shape_ = CanonicalPropertyValueCodec::inspectNext(result.payload_, offset);
            require(offset == result.payload_.size(), "hybrid extension contains multiple canonical values");
        } else if (mode == HybridPropertyExtensionMode::HPEM_LARGE_REFERENCE) {
            require(bool(referenceAdmission), "hybrid large reference requires retained-buffer admission");
            require(result.payload_.size() >= RSS_LARGE_VALUE_REF_HEADER_BYTES &&
                result.payload_.size() <= RSS_LARGE_VALUE_REF_HEADER_BYTES + RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES,
                "hybrid large reference width is invalid");
            // This charges the bounded media allocation before the existing
            // reference decoder can allocate. The decoded reference is retained
            // in the result; the callback's account remains caller-owned.
            referenceAdmission(sizeof(LargeValueRef) + RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES + 1);
            result.reference_ = LargeValueRefCodec::decode(result.payload_);
            const auto &reference = *result.reference_;
            require(reference.kind == LargeValueKind::LVK_BYTES &&
                reference.media_type == RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE &&
                reference.generation <= generation && reference.logical_bytes >=
                    RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + 8U + 4U + CanonicalPropertyValueCodec::HEADER_BYTES,
                "hybrid extension references an incompatible or future LargeValue");
            referenceAdmission(sizeof(LargeValueRef) + reference.media_type.capacity() + 1);
        } else require(false, "unknown hybrid extension mode");
        return result;
    }
    // Input is borrowed, immutable and pinned for this view's full lifetime.
    // Explicitly prohibit temporary-to-view accessor calls at escape boundaries.
    [[nodiscard]] std::span<const std::byte> payload() const & { return payload_; }
    std::span<const std::byte> payload() const && = delete;
    [[nodiscard]] const std::optional<LargeValueRef> &reference() const & { return reference_; }
    const std::optional<LargeValueRef> &reference() const && = delete;
    [[nodiscard]] HybridPropertyExtensionMode mode() const { return mode_; }
    [[nodiscard]] EntityPropertyKey owner() const { return owner_; }
    [[nodiscard]] uint32_t token() const { return token_; }
    [[nodiscard]] Generation semanticGeneration() const { return generation_; }
    [[nodiscard]] const CanonicalPropertyValueCodec::DecodeShape &shape() const & { return shape_; }
    const CanonicalPropertyValueCodec::DecodeShape &shape() const && = delete;

    [[nodiscard]] PropertyValue decodeInline(
        const std::function<void(const CanonicalPropertyValueCodec::DecodeShape &)> &admission,
        const std::function<void()> &checkpoint = {}) const {
        hybrid_property_detail::require(mode_ == HybridPropertyExtensionMode::HPEM_INLINE && bool(admission),
            "hybrid inline decode requires inline mode and value admission");
        size_t offset = 0;
        auto result = CanonicalPropertyValueCodec::decodeNextWithAdmission(payload_, offset, admission, checkpoint);
        hybrid_property_detail::require(offset == payload_.size(), "hybrid inline value has trailing bytes");
        return result;
    }
    [[nodiscard]] PropertyCollectionRootEntry decodeLargePayload(std::span<const std::byte> authenticatedPayload,
        const std::function<void(const CanonicalPropertyValueCodec::DecodeShape &)> &valueAdmission,
        const SelectedPropertyReadAdmission &slotsAdmission,
        const std::function<void()> &checkpoint = {}) const {
        hybrid_property_detail::require(bool(valueAdmission), "hybrid LargeValue decode requires value admission");
        const auto entry = checkedLargeValueBytes(authenticatedPayload, slotsAdmission);
        size_t offset = 0;
        auto value = CanonicalPropertyValueCodec::decodeNextWithAdmission(entry, offset, valueAdmission, checkpoint);
        hybrid_property_detail::require(offset == entry.size(), "hybrid LargeValue field has trailing bytes");
        return {token_, std::move(value)};
    }

    [[nodiscard]] CanonicalPropertyValueCodec::DecodeShape inspectLargePayload(
        std::span<const std::byte> authenticatedPayload, const SelectedPropertyReadAdmission &slotsAdmission,
        const std::function<void()> &checkpoint = {}) const {
        const auto bytes = checkedLargeValueBytes(authenticatedPayload, slotsAdmission);
        size_t offset = 0;
        auto shape = CanonicalPropertyValueCodec::inspectNext(bytes, offset, checkpoint);
        hybrid_property_detail::require(offset == bytes.size(), "hybrid LargeValue field has trailing bytes");
        return shape;
    }

    // Membership/presence consumers inspect the authenticated single-field
    // canonical header without allocating a PropertyValue tree.
    [[nodiscard]] bool largePayloadIsNull(std::span<const std::byte> authenticatedPayload,
        const SelectedPropertyReadAdmission &slotsAdmission) const {
        const auto bytes = checkedLargeValueBytes(authenticatedPayload, slotsAdmission);
        return CanonicalPropertyValueCodec::isNullHeader(bytes, bytes.size());
    }

    // Destination allocation and canonical map-ordering workspace use the
    // caller's actual PMR resource. No encoded whole-row temporary is created.
    [[nodiscard]] static std::pmr::vector<std::byte> encodeInline(EntityPropertyKey owner, uint32_t token,
        Generation generation, const PropertyValue &value, size_t nativeRecordLimit, std::pmr::memory_resource &memory) {
        const auto size = CanonicalPropertyValueCodec::encodedSize(value);
        validateOutput(owner, token, generation, size, nativeRecordLimit);
        std::pmr::vector<std::byte> result(&memory);
        result.reserve(RSS_HYBRID_EXTENSION_HEADER_BYTES + size);
        result.resize(RSS_HYBRID_EXTENSION_HEADER_BYTES);
        writeHeader(result, owner, token, generation, HybridPropertyExtensionMode::HPEM_INLINE, size);
        CanonicalPropertyValueCodec::append(result, value);
        return result;
    }
    // encodedReference comes from the existing LargeValueRefCodec. Its bytes
    // are copied once into the charged destination; open() validates the full
    // existing checksum/identity envelope, with the caller retaining admission.
    [[nodiscard]] static std::pmr::vector<std::byte> encodeReference(EntityPropertyKey owner, uint32_t token,
        Generation generation, std::span<const std::byte> encodedReference, size_t nativeRecordLimit,
        std::pmr::memory_resource &memory, const ReferenceAdmission &referenceAdmission) {
        validateOutput(owner, token, generation, encodedReference.size(), nativeRecordLimit);
        std::pmr::vector<std::byte> result(&memory);
        result.resize(RSS_HYBRID_EXTENSION_HEADER_BYTES + encodedReference.size());
        writeHeader(result, owner, token, generation, HybridPropertyExtensionMode::HPEM_LARGE_REFERENCE, encodedReference.size());
        std::copy(encodedReference.begin(), encodedReference.end(), result.begin() + RSS_HYBRID_EXTENSION_HEADER_BYTES);
        static_cast<void>(open(result, owner, token, generation, generation, generation, referenceAdmission));
        return result;
    }
private:
    [[nodiscard]] std::span<const std::byte> checkedLargeValueBytes(std::span<const std::byte> authenticatedPayload,
        const SelectedPropertyReadAdmission &slotsAdmission) const {
        hybrid_property_detail::require(mode_ == HybridPropertyExtensionMode::HPEM_LARGE_REFERENCE &&
            reference_ && authenticatedPayload.size() == reference_->logical_bytes &&
            bool(slotsAdmission), "hybrid LargeValue decode lacks identity or admission");
        // Reject whole-row counts before the directory decoder can allocate a
        // count-sized vector. Then reuse its canonical directory validation.
        hybrid_property_detail::require(authenticatedPayload.size() >=
            RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES &&
            LittleEndianCodec::readU32(authenticatedPayload, 8) == 1,
            "hybrid LargeValue must contain exactly one field");
        const auto locations = EntityPropertyLargeValuePayloadCodec::decodeDirectory(authenticatedPayload, &slotsAdmission);
        hybrid_property_detail::require(locations.size() == 1 && locations[0].key_token == token_ &&
            authenticatedPayload.size() - RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES ==
                LittleEndianCodec::readU32(authenticatedPayload, 12),
            "hybrid LargeValue token does not match its extension");
        const auto entry = authenticatedPayload.subspan(RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES +
            locations[0].offset, locations[0].length);
        hybrid_property_detail::require(entry.size() >= 4 && LittleEndianCodec::readU32(entry, 0) == token_,
            "hybrid LargeValue entry token does not match its directory");
        return entry.subspan(4);
    }
    HybridPropertyExtensionView(std::span<const std::byte> payload, EntityPropertyKey owner, uint32_t token,
        Generation generation, HybridPropertyExtensionMode mode)
        : payload_(payload), owner_(owner), token_(token), generation_(generation), mode_(mode) {}
    static void validateOutput(EntityPropertyKey owner, uint32_t token, Generation generation,
        size_t payloadBytes, size_t nativeRecordLimit) {
        hybrid_property_detail::require(owner.isValid() && token && generation.isValid() &&
            nativeRecordLimit <= RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES &&
            nativeRecordLimit >= RSS_HYBRID_EXTENSION_HEADER_BYTES &&
            payloadBytes <= nativeRecordLimit - RSS_HYBRID_EXTENSION_HEADER_BYTES,
            "hybrid extension exceeds native record capacity or has invalid identity");
    }
    static void writeHeader(std::span<std::byte> bytes, EntityPropertyKey owner, uint32_t token,
        Generation generation, HybridPropertyExtensionMode mode, size_t payloadBytes) {
        LittleEndianCodec::writeU32(bytes, 0, RSS_HYBRID_EXTENSION_MAGIC);
        LittleEndianCodec::writeU16(bytes, 4, RSS_HYBRID_RECORD_VERSION);
        bytes[6] = std::byte(uint8_t(owner.owner_kind)); bytes[7] = std::byte(uint8_t(mode));
        LittleEndianCodec::writeU64(bytes, 8, owner.entity_id); LittleEndianCodec::writeU32(bytes, 16, token);
        LittleEndianCodec::writeU32(bytes, 20, uint32_t(payloadBytes));
        LittleEndianCodec::writeU64(bytes, 24, generation.value());
    }
    std::span<const std::byte> payload_;
    EntityPropertyKey owner_;
    uint32_t token_;
    Generation generation_;
    HybridPropertyExtensionMode mode_;
    CanonicalPropertyValueCodec::DecodeShape shape_{};
    std::optional<LargeValueRef> reference_;
};
} // namespace graph::storage
