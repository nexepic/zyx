/**
 * @file HybridPropertyDescriptor.hpp
 * @brief Authenticated metadata frame for the bounded hybrid-property candidate.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

// Isolated metadata prototype only: ordinary format builds reject this root.
// MetadataPageFile currently admits empty descriptors and rejects nonempty
// routing until the complete native property lifecycle is installed. This codec
// can represent nonempty roots for byte/identity checks, not production admission.
// This is not a generic State record or a routed radix root.
constexpr uint32_t RSS_HYBRID_PROPERTY_DESCRIPTOR_FRAME_MAGIC = 0x4858595AU; // ZYXH
constexpr uint32_t RSS_HYBRID_PROPERTY_DESCRIPTOR_MAGIC = 0x4458595AU; // ZYXD
constexpr uint16_t RSS_HYBRID_PROPERTY_DESCRIPTOR_VERSION = 1;
constexpr size_t RSS_HYBRID_PROPERTY_DESCRIPTOR_FRAME_HEADER_BYTES = 32;
constexpr size_t RSS_HYBRID_PROPERTY_DESCRIPTOR_BYTES = 192;
constexpr size_t RSS_HYBRID_PROPERTY_DESCRIPTOR_ROOT_COUNT = 8;

enum class HybridPropertyRootOrdinal : uint8_t {
    HPRO_NODE_OWNER = 0, HPRO_NODE_COLUMN, HPRO_NODE_EXTENSION, HPRO_NODE_GROUP,
    HPRO_EDGE_OWNER, HPRO_EDGE_COLUMN, HPRO_EDGE_EXTENSION, HPRO_EDGE_GROUP,
};
struct HybridPropertyTreeRoot {
    PageId page_id{};
    Generation generation{};
    constexpr bool operator==(const HybridPropertyTreeRoot &) const = default;
};
struct HybridPropertyDescriptor {
    DatabaseId database_id{};
    Generation semantic_generation{};
    // Owner/column/extension/group roots for Node, followed by Edge. Existing
    // ROW routing remains solely in entity_property_directory_root.
    std::array<HybridPropertyTreeRoot, RSS_HYBRID_PROPERTY_DESCRIPTOR_ROOT_COUNT> roots{};
    constexpr bool operator==(const HybridPropertyDescriptor &) const = default;
};
struct HybridPropertyDescriptorPage {
    Generation generation{}; // physical COW frame identity, including relocation
    LogSequenceNumber committed_lsn{};
    HybridPropertyDescriptor descriptor;
    constexpr bool operator==(const HybridPropertyDescriptorPage &) const = default;
};

class HybridPropertyDescriptorCodec final {
    [[noreturn]] static void fail(FormatError error, const char *message) { throw FormatException(error, message); }
    static void require(bool valid, const char *message) {
        if (!valid) fail(FormatError::FERR_INVALID_HEADER, message);
    }
    static uint32_t crc(std::span<const std::byte> bytes) {
        return utils::calculateCrc(bytes.data(), bytes.size());
    }
    static void validate(const HybridPropertyDescriptorPage &page) {
        require(page.generation.isValid() && page.generation.value() <= (UINT64_MAX >> 5U) &&
            page.committed_lsn.isValid(), "hybrid descriptor frame has no representable publication identity");
        const auto &descriptor = page.descriptor;
        require(descriptor.database_id.isValid() && descriptor.semantic_generation.isValid() &&
            descriptor.semantic_generation <= page.generation, "hybrid descriptor has an invalid semantic identity");
        for (const auto &root : descriptor.roots)
            require(root.page_id.isValid() == root.generation.isValid() &&
                root.generation <= descriptor.semantic_generation, "hybrid descriptor has a partial or future tree root");
        for (size_t i = 0; i < descriptor.roots.size(); ++i)
            for (size_t j = 0; j < i; ++j)
                require(!descriptor.roots[i].page_id.isValid() || descriptor.roots[i].page_id != descriptor.roots[j].page_id,
                    "hybrid descriptor aliases distinct namespace roots");
    }
public:
    using Bytes = std::array<std::byte, RSS_PAGE_SIZE>;
    [[nodiscard]] static Bytes encode(const HybridPropertyDescriptorPage &page) {
        validate(page); Bytes result{};
        LittleEndianCodec::writeU32(result, 0, RSS_HYBRID_PROPERTY_DESCRIPTOR_FRAME_MAGIC);
        LittleEndianCodec::writeU16(result, 4, RSS_HYBRID_PROPERTY_DESCRIPTOR_VERSION);
        LittleEndianCodec::writeU16(result, 6, RSS_HYBRID_PROPERTY_DESCRIPTOR_FRAME_HEADER_BYTES);
        LittleEndianCodec::writeU64(result, 8, page.generation.value());
        LittleEndianCodec::writeU64(result, 16, page.committed_lsn.value());
        auto body = std::span<std::byte>(result).subspan(RSS_HYBRID_PROPERTY_DESCRIPTOR_FRAME_HEADER_BYTES,
            RSS_HYBRID_PROPERTY_DESCRIPTOR_BYTES);
        LittleEndianCodec::writeU32(body, 0, RSS_HYBRID_PROPERTY_DESCRIPTOR_MAGIC);
        LittleEndianCodec::writeU16(body, 4, RSS_HYBRID_PROPERTY_DESCRIPTOR_VERSION);
        LittleEndianCodec::writeU64(body, 8, page.descriptor.semantic_generation.value());
        LittleEndianCodec::writeU64(body, 16, page.descriptor.database_id.value());
        for (size_t i = 0; i < page.descriptor.roots.size(); ++i) {
            LittleEndianCodec::writeU64(body, 64 + i * 16, page.descriptor.roots[i].page_id.value());
            LittleEndianCodec::writeU64(body, 72 + i * 16, page.descriptor.roots[i].generation.value());
        }
        LittleEndianCodec::writeU32(result, 24,
            crc(std::span<const std::byte>(result).subspan(RSS_HYBRID_PROPERTY_DESCRIPTOR_FRAME_HEADER_BYTES)));
        LittleEndianCodec::writeU32(result, 28,
            crc(std::span<const std::byte>(result).first(RSS_HYBRID_PROPERTY_DESCRIPTOR_FRAME_HEADER_BYTES)));
        return result;
    }
    // The containing native metadata reader supplies its pinned database/root
    // and visible LSN/generation. A valid checksum is insufficient identity.
    [[nodiscard]] static HybridPropertyDescriptorPage decode(std::span<const std::byte> bytes,
        DatabaseId expectedDatabase, Generation expectedPhysicalGeneration,
        Generation visibleGeneration, LogSequenceNumber visibleLsn) {
        if (bytes.size() < RSS_PAGE_SIZE) fail(FormatError::FERR_BUFFER_TOO_SMALL, "hybrid descriptor frame is truncated");
        require(bytes.size() == RSS_PAGE_SIZE, "hybrid descriptor frame has unexpected width");
        std::array<std::byte, RSS_HYBRID_PROPERTY_DESCRIPTOR_FRAME_HEADER_BYTES> header{};
        std::copy_n(bytes.begin(), header.size(), header.begin());
        const auto expectedHeaderCrc = LittleEndianCodec::readU32(header, 28);
        LittleEndianCodec::writeU32(header, 28, 0);
        if (crc(header) != expectedHeaderCrc) fail(FormatError::FERR_CHECKSUM_MISMATCH, "hybrid descriptor header checksum mismatch");
        if (LittleEndianCodec::readU32(bytes, 0) != RSS_HYBRID_PROPERTY_DESCRIPTOR_FRAME_MAGIC)
            fail(FormatError::FERR_INVALID_MAGIC, "hybrid descriptor frame magic mismatch");
        if (LittleEndianCodec::readU16(bytes, 4) != RSS_HYBRID_PROPERTY_DESCRIPTOR_VERSION)
            fail(FormatError::FERR_UNSUPPORTED_VERSION, "unsupported hybrid descriptor frame version");
        require(LittleEndianCodec::readU16(bytes, 6) == header.size(), "hybrid descriptor frame header width mismatch");
        if (crc(bytes.subspan(header.size())) != LittleEndianCodec::readU32(bytes, 24))
            fail(FormatError::FERR_CHECKSUM_MISMATCH, "hybrid descriptor payload checksum mismatch");
        const auto body = bytes.subspan(header.size(), RSS_HYBRID_PROPERTY_DESCRIPTOR_BYTES);
        if (LittleEndianCodec::readU32(body, 0) != RSS_HYBRID_PROPERTY_DESCRIPTOR_MAGIC)
            fail(FormatError::FERR_INVALID_MAGIC, "hybrid descriptor payload magic mismatch");
        if (LittleEndianCodec::readU16(body, 4) != RSS_HYBRID_PROPERTY_DESCRIPTOR_VERSION)
            fail(FormatError::FERR_UNSUPPORTED_VERSION, "unsupported hybrid descriptor payload version");
        if (LittleEndianCodec::readU16(body, 6) != 0 ||
            !std::all_of(body.begin() + 24, body.begin() + 64, [](std::byte value) { return value == std::byte{}; }) ||
            !std::all_of(bytes.begin() + header.size() + body.size(), bytes.end(),
                [](std::byte value) { return value == std::byte{}; }))
            fail(FormatError::FERR_RESERVED_BITS, "hybrid descriptor has nonzero reserved bytes");
        HybridPropertyDescriptorPage result;
        result.generation = Generation(LittleEndianCodec::readU64(bytes, 8));
        result.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, 16));
        result.descriptor.semantic_generation = Generation(LittleEndianCodec::readU64(body, 8));
        result.descriptor.database_id = DatabaseId(LittleEndianCodec::readU64(body, 16));
        for (size_t i = 0; i < result.descriptor.roots.size(); ++i)
            result.descriptor.roots[i] = {PageId(LittleEndianCodec::readU64(body, 64 + i * 16)),
                Generation(LittleEndianCodec::readU64(body, 72 + i * 16))};
        validate(result);
        require(expectedDatabase.isValid() && expectedPhysicalGeneration.isValid() && visibleGeneration.isValid() &&
            visibleLsn.isValid() && result.descriptor.database_id == expectedDatabase &&
            result.generation == expectedPhysicalGeneration && result.generation <= visibleGeneration &&
            result.committed_lsn <= visibleLsn, "hybrid descriptor does not belong to its pinned root and database");
        return result;
    }
};
} // namespace graph::storage
