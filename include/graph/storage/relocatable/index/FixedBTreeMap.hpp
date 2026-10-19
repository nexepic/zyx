/**
 * @file FixedBTreeMap.hpp
 * @brief Fixed prefix/value maps over existing authenticated key-only B+Trees.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include "graph/storage/relocatable/index/BTreeIndex.hpp"

namespace graph::storage {

// Logical (key,value) is physically a key-only (ordered-prefix,value-suffix)
// entry. This keeps generic page/entry layouts unchanged. It is NOT an ordinary
// multimap: each prefix has exactly one authoritative value. The typed owner
// must validate payload semantics and full integrity, and atomically erase the
// exact old record before inserting a changed value. Canonicalize final changes
// to one replacement per prefix per transaction; repeated unmerged replacements
// from the same snapshot could otherwise introduce two suffixes.
//
// Ordered prefixes must be order preserving (e.g. unsigned big endian). Value
// bytes may use their own canonical little-endian codec. On-disk keys include
// every suffix byte in existing packed framing/compression and space costs.
// No new file/page kind or graph semantic fence is defined by this component.
template<size_t KeyBytes, size_t ValueBytes>
class FixedBTreeMap final {
    static_assert(KeyBytes > 0 && ValueBytes > 0 && KeyBytes <= 4096 && ValueBytes <= 4096 - KeyBytes);
public:
    using Key = std::array<std::byte, KeyBytes>;
    using Value = std::array<std::byte, ValueBytes>;
    using Record = std::array<std::byte, KeyBytes + ValueBytes>;
    struct Entry { Key key; Value value; bool operator==(const Entry &) const = default; };

    FixedBTreeMap(uint64_t indexId, BTreePageStore &pages, RelocatableMetadataStore &metadata)
        : index_(indexId, pages, metadata, BTreeLeafValueLayout::BLVL_EMPTY) {}

    [[nodiscard]] static Record encode(const Key &key, const Value &value) {
        Record result{};
        std::copy(key.begin(), key.end(), result.begin());
        std::copy(value.begin(), value.end(), result.begin() + KeyBytes);
        return result;
    }
    [[nodiscard]] static Entry decode(const BTreePageEntry &record) {
        if (record.key.size() != KeyBytes + ValueBytes || record.value_or_child != 0)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "fixed B+Tree map record has wrong width or leaf value");
        Entry result{};
        std::copy_n(record.key.begin(), KeyBytes, result.key.begin());
        std::copy_n(record.key.begin() + KeyBytes, ValueBytes, result.value.begin());
        return result;
    }
    // Read only the visited prefix and one following record. Two suffixes for
    // the same prefix are corruption, never last-wins or missing-value fallback.
    // The callback is passed to the existing authenticated cursor for boundary
    // key admission. Cursor/page/source memory follows existing native accounting;
    // returned key/value arrays require no dynamic allocation.
    [[nodiscard]] std::optional<Value> find(const MetadataSnapshotPin &snapshot, const Key &key,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        auto cursor = index_.openRangeCursor(snapshot, std::span<const std::byte>(key), 2,
            BufferPoolReadHint::BPRH_NORMAL);
        const auto first = cursor.next(boundaryAdmission);
        if (!first) return {};
        const auto entry = decode(*first);
        if (entry.key != key) return {};
        if (const auto second = cursor.next(boundaryAdmission); second && decode(*second).key == key)
            throw FormatException(FormatError::FERR_INVALID_STATE, "fixed B+Tree map has duplicate logical ownership");
        return entry.value;
    }
private:
    BTreeIndex index_;
};
} // namespace graph::storage
