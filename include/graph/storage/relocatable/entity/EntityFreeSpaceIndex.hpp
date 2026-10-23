/**
 * @brief Derived, snapshot-bound free-slot lookup over the existing B+Tree wire format.
 * Published entity directory roots have a unique generation per record kind
 * and database. Physical evacuation invalidates this fast-path proof; explicit
 * rebuild can rebind it. The graph publisher checks each summary fence.
 */
#pragma once
#include <array>
#include <algorithm>
#include <limits>
#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage {
constexpr uint64_t RSS_NODE_FREE_SPACE_INDEX_ID = UINT64_C(0x8000000000000040);
constexpr uint64_t RSS_EDGE_FREE_SPACE_INDEX_ID = UINT64_C(0x8000000000000041);
[[nodiscard]] constexpr bool isEntityFreeSpaceIndexId(uint64_t id) {
    return id == RSS_NODE_FREE_SPACE_INDEX_ID || id == RSS_EDGE_FREE_SPACE_INDEX_ID;
}
class EntityFreeSpaceIndex final {
public:
    struct Proof { uint64_t directory_generation = 0, free_pages = 0; bool fresh = false; };
    struct Entry { PageId page; uint16_t free_slots; };
    static uint64_t indexId(EntityPageRecordKind kind) {
        if (kind == EntityPageRecordKind::EPRK_NODE) return RSS_NODE_FREE_SPACE_INDEX_ID;
        if (kind == EntityPageRecordKind::EPRK_EDGE) return RSS_EDGE_FREE_SPACE_INDEX_ID;
        throw std::invalid_argument("free-space summary requires Node or Edge records");
    }
    static PhysicalPageRef entityRoot(const Superblock &source, EntityPageRecordKind kind) {
        return indexId(kind) == RSS_NODE_FREE_SPACE_INDEX_ID ? source.node_entity_directory_root : source.edge_entity_directory_root;
    }
    static constexpr std::array<std::byte, 5> generationKey() {
        return {std::byte{0}, std::byte{'F'}, std::byte{'S'}, std::byte{1}, std::byte{0}};
    }
    static constexpr std::array<std::byte, 5> countKey() {
        auto key = generationKey(); key.back() = std::byte{1}; return key;
    }
    static std::array<std::byte, 11> entryKey(PageId page, uint16_t freeSlots, EntityPageRecordKind kind) {
        if (!page.isValid() || !freeSlots || freeSlots >= EntityPage::recordCapacity(kind))
            throw FormatException(FormatError::FERR_INVALID_HEADER, "invalid entity free-slot summary entry");
        std::array<std::byte, 11> key{}; key[0] = std::byte{1};
        const uint16_t ordered = uint16_t(UINT16_MAX - freeSlots);
        key[1] = std::byte(ordered >> 8U); key[2] = std::byte(ordered & 255U);
        for (unsigned i = 0; i < 8; ++i) key[3 + i] = std::byte((page.value() >> (56U - 8U * i)) & 255U);
        return key;
    }
    static Entry decodeEntry(const BTreePageEntry &entry, EntityPageRecordKind kind) {
        const auto &key = entry.key;
        if (key.size() != 11U || key[0] != std::byte{1} || entry.value_or_child != 1U)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "invalid entity free-slot summary key/value");
        const auto ordered = uint16_t((uint16_t(std::to_integer<uint8_t>(key[1])) << 8U) | std::to_integer<uint8_t>(key[2]));
        const uint16_t free = uint16_t(UINT16_MAX - ordered); uint64_t id = 0U;
        for (unsigned i = 0; i < 8; ++i) id = (id << 8U) | std::to_integer<uint8_t>(key[3 + i]);
        const Entry result{PageId(id), free};
        static_cast<void>(entryKey(result.page, result.free_slots, kind));
        return result;
    }
    static std::optional<Proof> readProof(RelocatableMetadataStore &metadata, BTreePageStore &pages,
        const MetadataSnapshotPin &snapshot, EntityPageRecordKind kind) {
        const auto id = indexId(kind);
        if (!metadata.findIndexRootOptional(snapshot, id)) return std::nullopt;
        BTreeIndex index(id, pages, metadata);
        const auto generation = index.find(snapshot, generationKey()), count = index.find(snapshot, countKey());
        if (!generation || !count)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "reserved free-space index lacks its versioned markers");
        if (!*generation || !*count || *generation - 1U > snapshot.superblock().generation.value() || *count - 1U >= (std::numeric_limits<size_t>::max)() ||
            (snapshot.superblock().next_logical_page_id && *count - 1U > snapshot.superblock().next_logical_page_id))
            throw FormatException(FormatError::FERR_INVALID_STATE, "free-space index has impossible source/count markers");
        const auto root = entityRoot(snapshot.superblock(), kind);
        // The canonical empty directory itself proves no live entity pages.
        return Proof{*generation - 1U, *count - 1U, root.isValid() ? *generation - 1U == root.generation.value() : *count == 1U};
    }
    // true means the complete candidate set was exhausted. At most `needed`
    // candidates can be consumed because every candidate has a positive count.
    template<class Visitor>
    static bool visitCandidates(RelocatableMetadataStore &metadata, BTreePageStore &pages,
        const MetadataSnapshotPin &snapshot, EntityPageRecordKind kind, const Proof &proof,
        size_t needed, Visitor visitor, const CancellationCheckpoint *cancellation = nullptr) {
        if (!proof.fresh || !needed) throw std::invalid_argument("free-slot lookup requires fresh proof and positive work");
        BTreeIndex index(indexId(kind), pages, metadata); const std::array lower{std::byte{1}};
        auto cursor = index.openRangeCursor(snapshot, std::span<const std::byte>(lower), size_t(proof.free_pages) + 1U,
            BufferPoolReadHint::BPRH_NORMAL, false, cancellation);
        size_t visited = 0U, capacity = 0U;
        while (visited < proof.free_pages) {
            if (cancellation) cancellation->check();
            const auto encoded = cursor.next();
            if (!encoded) throw FormatException(FormatError::FERR_INVALID_STATE, "free-space index count exceeds its entries");
            const auto entry = decodeEntry(*encoded, kind); visitor(entry); ++visited;
            capacity += (std::min)(size_t(entry.free_slots), needed - capacity);
            if (capacity == needed && visited < proof.free_pages) return false;
        }
        if (cursor.next()) throw FormatException(FormatError::FERR_INVALID_STATE, "free-space index contains uncounted entries");
        return true;
    }
};
} // namespace graph::storage
