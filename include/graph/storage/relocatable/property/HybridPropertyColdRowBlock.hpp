/**
 * @file HybridPropertyColdRowBlock.hpp
 * @brief Bounded cold ROW block byte codec; not a publication capability.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <array>
#include <span>
#include "graph/storage/relocatable/property/HybridPropertyRecords.hpp"
#include "graph/storage/relocatable/property/HybridPropertyExtension.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"

namespace graph::storage {
enum class HybridColdRowBlockCodec : uint8_t { HCRBC_RAW = 0, HCRBC_LZ4 = 1 };
struct HybridColdRowInput {
    uint8_t owner_slot = 0;
    uint32_t field_count = 0;
    bool large_value_reference = false;
    std::span<const std::byte> root;
};
// Decoded rows borrow this object's accounted buffer. Keeping a returned span
// requires retaining the decoded block; production sinks must consume it
// synchronously. LargeValue bytes are only a structurally validated reference,
// never proof of its actual manifest/media/content or publication ownership.
class HybridPropertyColdRowBlock final {
public:
    static constexpr uint32_t MAGIC = 0x5243595aU;
    static constexpr uint16_t VERSION = 1;
    static constexpr size_t HEADER_BYTES = 64;
    static constexpr size_t DIRECTORY_ENTRY_BYTES = 4;
    static constexpr size_t MAX_UNCOMPRESSED_BYTES = 4096;
    static constexpr size_t MAX_ROWS = 64;
    static constexpr size_t MAX_STORED_BYTES = RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES - HEADER_BYTES;
    struct Decoded {
        EntityPropertyOwnerKind owner_kind;
        uint64_t group_id = 0, owners = 0;
        Generation semantic_generation{};
        uint32_t fields = 0;
        HybridColdRowBlockCodec codec = HybridColdRowBlockCodec::HCRBC_RAW;
        utils::OwnedVector<std::byte> body;
        struct Slice { uint32_t start = 0, length = 0, fields = 0; bool large = false; };
        std::array<Slice, MAX_ROWS> slices{};
        explicit Decoded(std::shared_ptr<std::pmr::memory_resource> memory)
            : owner_kind(EntityPropertyOwnerKind::EPOK_NODE), body(utils::OwnedPmrAllocator<std::byte>(std::move(memory))) {}
        [[nodiscard]] std::optional<HybridColdRowInput> row(unsigned slot) const {
            hybrid_property_detail::require(slot < MAX_ROWS, "cold ROW slot is outside its group");
            if (!(owners & (UINT64_C(1) << slot))) return {};
            const auto &s = slices[slot];
            return HybridColdRowInput{uint8_t(slot), s.fields, s.large, std::span<const std::byte>(body).subspan(s.start, s.length)};
        }
    };
    // Source roots are canonical existing ROW roots or owner-keyed whole-root
    // references. A length_error means split the source rows (or prepare the
    // existing LargeValue form), not permission to increase a bound.
    [[nodiscard]] static utils::OwnedVector<std::byte> encode(EntityPropertyOwnerKind kind, uint64_t group,
        Generation generation, std::span<const HybridColdRowInput> rows,
        std::shared_ptr<std::pmr::memory_resource> memory, bool allowCompression = true,
        const CancellationCheckpoint *cancellation = nullptr);
    // Private byte preparation only: caller authenticates source storage and
    // manifests, and atomically installs bindings/retirement. Changes replace
    // complete owner roots; empty source prepares a fresh block without granting
    // storage authority. Empty root/count removes an existing owner. Empty output
    // means the last row was removed. Oversize output refuses without a prefix.
    [[nodiscard]] static utils::OwnedVector<std::byte> repack(std::span<const std::byte> source,
        EntityPropertyOwnerKind kind, uint64_t group, Generation visibleGeneration, Generation targetGeneration,
        std::span<const HybridColdRowInput> changes, std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation = nullptr);
    // Complete private result: all blocks share the original 4KiB bound.
    // A failure destroys every prepared block; no successful prefix escapes.
    using EncodedBlocks = utils::OwnedVector<utils::OwnedVector<std::byte>>;
    [[nodiscard]] static EncodedBlocks repackBlocks(std::span<const std::byte> source,
        EntityPropertyOwnerKind kind, uint64_t group, Generation visibleGeneration, Generation targetGeneration,
        std::span<const HybridColdRowInput> changes, std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation = nullptr);
    // All declared bounds/checksums/row identities are authenticated before a
    // decoded buffer becomes visible. No consumer callback can precede this.
    [[nodiscard]] static Decoded decode(std::span<const std::byte> bytes,
        EntityPropertyOwnerKind expectedKind, uint64_t expectedGroup, Generation visibleGeneration,
        std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation = nullptr);
};
static_assert(HybridPropertyColdRowBlock::MAX_UNCOMPRESSED_BYTES == RSS_PAGE_SIZE);
}
