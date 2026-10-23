#include "graph/storage/relocatable/property/HybridPropertyColdRowBlock.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/utils/StandardContainerHeapBytes.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#if ZYX_HAS_LZ4
#include <lz4.h>
#endif

namespace graph::storage {
namespace {
using Block = HybridPropertyColdRowBlock;
using hybrid_property_detail::require;
void check(const CancellationCheckpoint *c) { if (c) c->check(); }
uint32_t crc(std::span<const std::byte> bytes) { return utils::calculateCrc(bytes.data(), bytes.size()); }
uint32_t validateRow(EntityPropertyOwnerKind kind, uint64_t group, const HybridColdRowInput &row,
    const std::shared_ptr<std::pmr::memory_resource> &memory, bool deriveInlineCount = false) {
    require(row.owner_slot < 64 && (row.field_count || (deriveInlineCount && !row.large_value_reference)) && !row.root.empty(), "cold ROW has an invalid slot, count or payload");
    const uint64_t owner = (group - 1U) * 64U + row.owner_slot + 1U;
    require(owner <= RSS_HYBRID_MAX_ENTITY_ID, "cold ROW owner exceeds the native domain");
    const uint32_t type = kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
    // Covers temporary empty-container proxies on debug STLs and the bounded
    // media string decoded by the existing whole-root reference codec.
    utils::OwnedHeapReservation scratch(memory, 3U * utils::detail::standardContainerProxyBytes() +
        (row.large_value_reference ? utils::detail::standardAllocatorRequestBytes(row.root.size(), alignof(char)) : 0U));
    if (row.large_value_reference) {
        const auto root = EntityPropertyLargeValueReferenceCodec::decode(row.root);
        require(root.owner_entity_id == owner && root.owner_entity_type == type, "cold ROW reference has a foreign owner");
        // The actual directory's field count is checked with its manifest by
        // the storage reader/integrity phase, not inferred from reference bytes.
        return row.field_count;
    } else {
        const auto root = EntityPropertyCollectionRootCodec::decodeOwner(row.root);
        require(root.owner_entity_id == owner && root.owner_entity_type == type, "cold ROW root has a foreign owner");
        const auto actual = EntityPropertyCollectionRootCodec::entryCount(row.root);
        require(actual && actual <= UINT32_MAX && (deriveInlineCount || actual == row.field_count),
            "cold ROW field count disagrees with its canonical directory");
        return uint32_t(actual);
    }
}
}
utils::OwnedVector<std::byte> HybridPropertyColdRowBlock::encode(EntityPropertyOwnerKind kind, uint64_t group,
    Generation generation, std::span<const HybridColdRowInput> rows,
    std::shared_ptr<std::pmr::memory_resource> memory, bool allowCompression, const CancellationCheckpoint *cancellation) {
    require(memory && hybrid_property_detail::validKind(kind) && generation.isValid() && !rows.empty() && rows.size() <= MAX_ROWS,
        "cold ROW encoding requires bounded rows, identity and account");
    check(cancellation);
    uint64_t owners = 0, largeOwners = 0; uint32_t fields = 0; size_t size = rows.size() * DIRECTORY_ENTRY_BYTES;
    for (const auto &row : rows) {
        check(cancellation);
        require(row.owner_slot < 64 && (!owners || row.owner_slot > unsigned(63U - std::countl_zero(owners))),
            "cold ROW owners are not strictly increasing");
        hybrid_property_detail::validateGroup(group, UINT64_C(1) << row.owner_slot);
        const size_t prefix = row.large_value_reference ? 4U : 0U;
        if (prefix > MAX_UNCOMPRESSED_BYTES - size || row.root.size() > MAX_UNCOMPRESSED_BYTES - size - prefix)
            throw std::length_error("cold ROW body requires splitting");
        (void)validateRow(kind, group, row, memory);
        require(row.field_count <= UINT32_MAX - fields, "cold ROW field count overflowed"); fields += row.field_count;
        size += row.root.size() + prefix; owners |= UINT64_C(1) << row.owner_slot;
        if (row.large_value_reference) largeOwners |= UINT64_C(1) << row.owner_slot;
    }
    utils::OwnedVector<std::byte> body{utils::OwnedPmrAllocator<std::byte>(memory)}; body.resize(size);
    size_t offset = rows.size() * DIRECTORY_ENTRY_BYTES;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto &row = rows[i]; const size_t at = i * DIRECTORY_ENTRY_BYTES;
        if (row.large_value_reference) { LittleEndianCodec::writeU32(body, offset, row.field_count); offset += 4U; }
        std::copy(row.root.begin(), row.root.end(), body.begin() + offset); offset += row.root.size();
        LittleEndianCodec::writeU32(body, at, uint32_t(offset));
    }
    HybridColdRowBlockCodec codec = HybridColdRowBlockCodec::HCRBC_RAW;
    utils::OwnedVector<std::byte> compressed{utils::OwnedPmrAllocator<std::byte>(memory)};
    std::span<const std::byte> stored = body;
#if ZYX_HAS_LZ4
    if (allowCompression) {
        check(cancellation); compressed.resize(size_t(LZ4_compressBound(int(body.size()))));
        const int written = LZ4_compress_default(reinterpret_cast<const char *>(body.data()),
            reinterpret_cast<char *>(compressed.data()), int(body.size()), int(compressed.size()));
        if (written > 0 && size_t(written) < body.size()) {
            compressed.resize(size_t(written)); stored = compressed; codec = HybridColdRowBlockCodec::HCRBC_LZ4;
        }
    }
#else
    static_cast<void>(allowCompression);
#endif
    if (stored.size() > MAX_STORED_BYTES) throw std::length_error("cold ROW record requires splitting or an existing LargeValue root");
    utils::OwnedVector<std::byte> bytes{utils::OwnedPmrAllocator<std::byte>(memory)}; bytes.resize(HEADER_BYTES + stored.size());
    LittleEndianCodec::writeU32(bytes, 0, MAGIC); LittleEndianCodec::writeU16(bytes, 4, VERSION);
    bytes[6] = std::byte(uint8_t(codec)); bytes[7] = std::byte(uint8_t(kind));
    LittleEndianCodec::writeU64(bytes, 8, group); LittleEndianCodec::writeU64(bytes, 16, generation.value());
    LittleEndianCodec::writeU64(bytes, 24, owners); LittleEndianCodec::writeU32(bytes, 32, uint32_t(body.size()));
    LittleEndianCodec::writeU32(bytes, 36, uint32_t(stored.size())); LittleEndianCodec::writeU16(bytes, 40, uint16_t(rows.size()));
    LittleEndianCodec::writeU16(bytes, 42, uint16_t(rows.size() * DIRECTORY_ENTRY_BYTES));
    LittleEndianCodec::writeU32(bytes, 44, fields); LittleEndianCodec::writeU32(bytes, 48, crc(body));
    LittleEndianCodec::writeU32(bytes, 52, crc(stored)); LittleEndianCodec::writeU64(bytes, 56, largeOwners); std::copy(stored.begin(), stored.end(), bytes.begin() + HEADER_BYTES);
    check(cancellation); return bytes;
}
HybridPropertyColdRowBlock::Decoded HybridPropertyColdRowBlock::decode(std::span<const std::byte> bytes,
    EntityPropertyOwnerKind expectedKind, uint64_t expectedGroup, Generation visibleGeneration,
    std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation) {
    require(memory && hybrid_property_detail::validKind(expectedKind) && visibleGeneration.isValid(),
        "cold ROW decoding requires identity, visibility and account");
    check(cancellation); require(bytes.size() >= HEADER_BYTES && bytes.size() <= HEADER_BYTES + MAX_STORED_BYTES,
        "cold ROW record length is invalid");
    require(LittleEndianCodec::readU32(bytes, 0) == MAGIC, "cold ROW magic mismatch");
    if (LittleEndianCodec::readU16(bytes, 4) != VERSION)
        throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "unsupported cold ROW version");
    const auto codec = HybridColdRowBlockCodec(std::to_integer<uint8_t>(bytes[6]));
    require(codec == HybridColdRowBlockCodec::HCRBC_RAW || codec == HybridColdRowBlockCodec::HCRBC_LZ4, "unknown cold ROW codec");
    const auto kind = EntityPropertyOwnerKind(std::to_integer<uint8_t>(bytes[7]));
    const auto group = LittleEndianCodec::readU64(bytes, 8); const Generation generation(LittleEndianCodec::readU64(bytes, 16));
    const auto owners = LittleEndianCodec::readU64(bytes, 24); const size_t size = LittleEndianCodec::readU32(bytes, 32);
    const size_t storedBytes = LittleEndianCodec::readU32(bytes, 36), count = LittleEndianCodec::readU16(bytes, 40);
    const auto fields = LittleEndianCodec::readU32(bytes, 44);
    require(kind == expectedKind && group == expectedGroup && generation.isValid() && generation <= visibleGeneration,
        "cold ROW identity or generation mismatch");
    hybrid_property_detail::validateGroup(group, owners);
    require(owners && count == size_t(std::popcount(owners)) && count <= MAX_ROWS && fields >= count &&
        size && size <= MAX_UNCOMPRESSED_BYTES && storedBytes && storedBytes == bytes.size() - HEADER_BYTES &&
        LittleEndianCodec::readU16(bytes, 42) == count * DIRECTORY_ENTRY_BYTES && count * DIRECTORY_ENTRY_BYTES < size,
        "cold ROW declared bounds are invalid");
    const auto largeOwners = LittleEndianCodec::readU64(bytes, 56);
    require(!(largeOwners & ~owners), "cold ROW reference mask is outside its owners");
    const auto stored = bytes.subspan(HEADER_BYTES);
    if (crc(stored) != LittleEndianCodec::readU32(bytes, 52))
        throw FormatException(FormatError::FERR_CHECKSUM_MISMATCH, "cold ROW stored CRC mismatch");
    require(codec != HybridColdRowBlockCodec::HCRBC_RAW || size == storedBytes, "cold ROW RAW size mismatch");
    require(codec != HybridColdRowBlockCodec::HCRBC_LZ4 || storedBytes < size,
        "cold ROW compressed form does not save bytes");
#if !ZYX_HAS_LZ4
    if (codec == HybridColdRowBlockCodec::HCRBC_LZ4)
        throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "cold ROW LZ4 is unavailable");
#endif
    Decoded result(memory); result.owner_kind = kind; result.group_id = group; result.owners = owners;
    result.semantic_generation = generation; result.fields = fields; result.codec = codec; result.body.resize(size);
    if (codec == HybridColdRowBlockCodec::HCRBC_RAW) std::copy(stored.begin(), stored.end(), result.body.begin());
#if ZYX_HAS_LZ4
    else require(LZ4_decompress_safe(reinterpret_cast<const char *>(stored.data()), reinterpret_cast<char *>(result.body.data()),
        int(stored.size()), int(result.body.size())) == int(size), "cold ROW decompression size mismatch");
#endif
    if (crc(result.body) != LittleEndianCodec::readU32(bytes, 48))
        throw FormatException(FormatError::FERR_CHECKSUM_MISMATCH, "cold ROW uncompressed CRC mismatch");
    size_t start = count * DIRECTORY_ENTRY_BYTES; uint64_t remaining = owners; uint32_t actualFields = 0;
    for (size_t i = 0; i < count; ++i) {
        check(cancellation); const size_t at = i * DIRECTORY_ENTRY_BYTES;
        const unsigned slot = unsigned(std::countr_zero(remaining));
        const bool large = bool(largeOwners & (UINT64_C(1) << slot));
        const size_t end = LittleEndianCodec::readU32(result.body, at);
        const size_t prefix = large ? 4U : 0U;
        require(end > start && end <= size && end - start > prefix, "cold ROW directory is noncanonical");
        const auto declared = large ? LittleEndianCodec::readU32(result.body, start) : 0U;
        const auto root = std::span<const std::byte>(result.body).subspan(start + prefix, end - start - prefix);
        const auto n = validateRow(kind, group, HybridColdRowInput{uint8_t(slot), declared, large, root}, memory, true);
        require(n <= UINT32_MAX - actualFields, "cold ROW field count overflowed");
        result.slices[slot] = {uint32_t(start + prefix), uint32_t(root.size()), n, large};
        remaining &= remaining - 1U; actualFields += n; start = end;
    }
    require(start == size && !remaining && actualFields == fields, "cold ROW body census differs from its header");
    check(cancellation); return result;
}
namespace {
template<class Consumer>
auto repackRows(std::span<const std::byte> source,
    EntityPropertyOwnerKind kind, uint64_t group, Generation visibleGeneration, Generation targetGeneration,
    std::span<const HybridColdRowInput> changes, std::shared_ptr<std::pmr::memory_resource> memory,
    const CancellationCheckpoint *cancellation, Consumer &&consume) {
    require(memory && hybrid_property_detail::validKind(kind) && visibleGeneration.isValid() &&
        targetGeneration.isValid() && targetGeneration > visibleGeneration &&
        !changes.empty() && changes.size() <= Block::MAX_ROWS, "cold repack requires a bounded change set and next generation");
    hybrid_property_detail::validateGroup(group, 1);
    auto decoded = source.empty() ? Block::Decoded(memory) : Block::decode(source, kind, group, visibleGeneration, memory, cancellation);
    if (source.empty()) {
        decoded.owner_kind = kind; decoded.group_id = group; decoded.semantic_generation = visibleGeneration;
    }
    // Authenticate reference identity against the ORIGINAL semantic generation,
    // including replaced/deleted rows. Re-encoding under the new generation
    // must never make an originally future reference appear legitimate.
    const auto validateReference = [&](const HybridColdRowInput &row, Generation fence) {
        if (!row.large_value_reference) return;
        utils::OwnedHeapReservation scratch(memory, row.root.size() +
            3U * utils::detail::standardContainerProxyBytes());
        const auto reference = EntityPropertyLargeValueReferenceCodec::decode(row.root);
        require(reference.value.generation <= fence && reference.value.kind == LargeValueKind::LVK_BYTES &&
            reference.value.media_type == RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE &&
            reference.value.logical_bytes >= RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES,
            "cold repack reference has invalid media, length or original generation");
    };
    std::array<HybridColdRowInput, Block::MAX_ROWS> rows{};
    uint64_t owners = decoded.owners, changed = 0;
    for (unsigned slot = 0; slot < Block::MAX_ROWS; ++slot) {
        check(cancellation);
        if (const auto row = decoded.row(slot)) {
            validateReference(*row, decoded.semantic_generation); rows[slot] = *row;
        }
    }
    for (const auto &row : changes) {
        check(cancellation);
        require(row.owner_slot < Block::MAX_ROWS, "cold repack change slot is outside the group");
        const auto bit = UINT64_C(1) << row.owner_slot;
        require(!(changed & bit), "cold repack changes duplicate an owner"); changed |= bit;
        if (row.root.empty()) {
            require(!row.field_count && !row.large_value_reference && (owners & bit),
                "cold repack removal must name an existing row"); owners &= ~bit;
        } else {
            (void)validateRow(kind, group, row, memory);
            validateReference(row, targetGeneration); rows[row.owner_slot] = row; owners |= bit;
        }
    }
    check(cancellation);
    std::array<HybridColdRowInput, Block::MAX_ROWS> ordered{}; size_t count = 0;
    for (unsigned slot = 0; slot < Block::MAX_ROWS; ++slot)
        if (owners & (UINT64_C(1) << slot)) ordered[count++] = rows[slot];
    return consume(std::span(ordered).first(count));
}
}

utils::OwnedVector<std::byte> HybridPropertyColdRowBlock::repack(std::span<const std::byte> source,
    EntityPropertyOwnerKind kind, uint64_t group, Generation visibleGeneration, Generation targetGeneration,
    std::span<const HybridColdRowInput> changes, std::shared_ptr<std::pmr::memory_resource> memory,
    const CancellationCheckpoint *cancellation) {
    return repackRows(source, kind, group, visibleGeneration, targetGeneration, changes, memory, cancellation,
        [&](auto rows) {
            if (rows.empty()) return utils::OwnedVector<std::byte>{utils::OwnedPmrAllocator<std::byte>(memory)};
            return encode(kind, group, targetGeneration, rows, memory, true, cancellation);
        });
}
HybridPropertyColdRowBlock::EncodedBlocks HybridPropertyColdRowBlock::repackBlocks(std::span<const std::byte> source,
    EntityPropertyOwnerKind kind, uint64_t group, Generation visibleGeneration, Generation targetGeneration,
    std::span<const HybridColdRowInput> changes, std::shared_ptr<std::pmr::memory_resource> memory,
    const CancellationCheckpoint *cancellation) {
    return repackRows(source, kind, group, visibleGeneration, targetGeneration, changes, memory, cancellation,
        [&](std::span<const HybridColdRowInput> rows) {
            EncodedBlocks result{utils::OwnedPmrAllocator<utils::OwnedVector<std::byte>>(memory)};
            const auto rowBytes = [&](size_t i) { return DIRECTORY_ENTRY_BYTES +
                (rows[i].large_value_reference ? 4U : 0U) + rows[i].root.size(); };
            size_t first = 0;
            while (first < rows.size()) {
                check(cancellation); size_t end = first, size = 0;
                while (end < rows.size() && rowBytes(end) <= MAX_UNCOMPRESSED_BYTES - size)
                    size += rowBytes(end++);
                if (end == first) throw std::length_error("cold ROW requires an existing whole-root LargeValue");
                const auto encodePrefix = [&] { return encode(kind, group, targetGeneration,
                    rows.subspan(first, end - first), memory, true, cancellation); };
                utils::OwnedVector<std::byte> encoded{utils::OwnedPmrAllocator<std::byte>(memory)};
                try { encoded = encodePrefix(); }
                catch (const std::length_error &) {
                    // Original conversion's RAW-safe prefix fallback. A larger
                    // single row must fit compressed or refuse as a whole.
                    end = first + 1; size = rowBytes(first);
                    while (end < rows.size() && size <= MAX_STORED_BYTES && rowBytes(end) <= MAX_STORED_BYTES - size)
                        size += rowBytes(end++);
                    encoded = encodePrefix();
                }
                result.push_back(std::move(encoded)); first = end;
            }
            check(cancellation); return result;
        });
}
}
