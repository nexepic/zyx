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
#include <limits>
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
// A zero-byte suffix is the corresponding authenticated key-only set.
// Ordered prefixes must be order preserving (e.g. unsigned big endian). Value
// bytes may use their own canonical little-endian codec. On-disk keys include
// every suffix byte in existing packed framing/compression and space costs.
// No new file/page kind or graph semantic fence is defined by this component.
template<size_t KeyBytes, size_t ValueBytes>
class FixedBTreeMap final {
    static_assert(KeyBytes > 0 && KeyBytes <= 4096 && ValueBytes <= 4096 - KeyBytes);
public:
    using Key = std::array<std::byte, KeyBytes>;
    using Value = std::array<std::byte, ValueBytes>;
    using Record = std::array<std::byte, KeyBytes + ValueBytes>;
    struct Entry { Key key; Value value; bool operator==(const Entry &) const = default; };

    FixedBTreeMap(uint64_t indexId, BTreePageStore &pages, RelocatableMetadataStore &metadata,
        std::optional<BTreeRootReadBinding> binding = {})
        : index_(indexId, pages, metadata, BTreeLeafValueLayout::BLVL_EMPTY, std::move(binding)) {}

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
    [[nodiscard]] static Entry decode(const BTreeFixedKey &record) {
        if (record.width != KeyBytes + ValueBytes || record.width > BTreeFixedKey::MAX_BYTES)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "fixed B+Tree inline record has wrong width");
        Entry result{};
        std::copy_n(record.bytes.begin(), KeyBytes, result.key.begin());
        std::copy_n(record.bytes.begin() + KeyBytes, ValueBytes, result.value.begin());
        return result;
    }
    // Read only the visited prefix and one following record. Two suffixes for
    // the same prefix are corruption, never last-wins or missing-value fallback.
    // The callback is passed to the existing authenticated cursor for boundary
    // key admission. Cursor/page/source memory follows existing native accounting;
    // returned key/value arrays require no dynamic allocation.
    [[nodiscard]] std::optional<Value> find(const MetadataSnapshotPin &snapshot, const Key &key,
        const std::function<void(size_t)> &boundaryAdmission = {},
        const CancellationCheckpoint *cancellation = nullptr) const {
        if (cancellation) cancellation->check();
        auto cursor = index_.openRangeCursor(snapshot, std::span<const std::byte>(key), 2,
            BufferPoolReadHint::BPRH_NORMAL, false, cancellation);
        const auto first = cursor.next(boundaryAdmission);
        if (!first) return {};
        const auto entry = decode(*first);
        if (entry.key != key) return {};
        if (const auto second = cursor.next(boundaryAdmission); second && decode(*second).key == key)
            throw FormatException(FormatError::FERR_INVALID_STATE, "fixed B+Tree map has duplicate logical ownership");
        return entry.value;
    }
    // Enumerate only one ordered logical-key prefix, with one native-record
    // lookahead. This is the owner/token path: never enumerate every group or
    // token just to reconstruct one owner. Authenticate duplicate ownership
    // before delivering that entry, including duplicates across leaf pages.
    // False means the explicit entry bound or visitor stopped enumeration;
    // corruption/cancellation/admission exceptions are never missing results.
    // Returned Entry arrays are fixed storage; cursor/page/output admission
    // remains with existing owners. A caller must discard private output on error.
    [[nodiscard]] bool visitPrefix(const MetadataSnapshotPin &snapshot, std::span<const std::byte> prefix,
        size_t maximumEntries, const std::function<bool(const Entry &)> &visitor,
        const std::function<void(size_t)> &boundaryAdmission = {},
        const CancellationCheckpoint *cancellation = nullptr) const {
        if (prefix.empty() || prefix.size() > KeyBytes || !maximumEntries || !visitor)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "fixed map prefix enumeration requires a prefix, bound and visitor");
        return visitRange(snapshot, prefix, maximumEntries, visitor, boundaryAdmission, cancellation);
    }
    // Whole-map streaming is for explicit integrity/reclamation work, never an
    // owner reconstruction fallback. It keeps the same bound, lookahead and
    // duplicate-prefix authentication as the selected prefix path.
    [[nodiscard]] bool visitAll(const MetadataSnapshotPin &snapshot, size_t maximumEntries,
        const std::function<bool(const Entry &)> &visitor,
        const std::function<void(size_t)> &boundaryAdmission = {},
        const CancellationCheckpoint *cancellation = nullptr) const {
        if (!maximumEntries || !visitor)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "fixed map scan requires a bound and visitor");
        return visitRange(snapshot, {}, maximumEntries, visitor, boundaryAdmission, cancellation);
    }
    // Inclusive logical-key seek inside one prefix. Resumable users advance
    // their ordered key before calling; no prefix-sized skip/rescan is needed.
    [[nodiscard]] bool visitPrefixFrom(const MetadataSnapshotPin &snapshot, std::span<const std::byte> prefix,
        const Key &first, size_t maximumEntries, const std::function<bool(const Entry &)> &visitor,
        const std::function<void(size_t)> &boundaryAdmission = {},
        const CancellationCheckpoint *cancellation = nullptr) const {
        if (prefix.empty() || prefix.size() > KeyBytes || !maximumEntries || !visitor ||
            !std::equal(prefix.begin(), prefix.end(), first.begin()))
            throw FormatException(FormatError::FERR_INVALID_HEADER, "fixed map seek must remain within its bounded prefix");
        return visitRange(snapshot, prefix, maximumEntries, visitor, boundaryAdmission, cancellation, first);
    }
    // Inclusive seek across the complete logical-key namespace. Cursor owners
    // retain the last completed logical identity, never a physical leaf/slot.
    [[nodiscard]] bool visitFrom(const MetadataSnapshotPin &snapshot, const Key &first, size_t maximumEntries,
        const std::function<bool(const Entry &)> &visitor, const std::function<void(size_t)> &boundaryAdmission = {},
        const CancellationCheckpoint *cancellation = nullptr) const {
        if (!maximumEntries || !visitor)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "fixed map seek requires a bound and visitor");
        return visitRange(snapshot, {}, maximumEntries, visitor, boundaryAdmission, cancellation, first);
    }
private:
    [[nodiscard]] bool visitRange(const MetadataSnapshotPin &snapshot, std::span<const std::byte> prefix,
        size_t maximumEntries, const std::function<bool(const Entry &)> &visitor,
        const std::function<void(size_t)> &boundaryAdmission, const CancellationCheckpoint *cancellation,
        std::span<const std::byte> firstKey = {}) const {
        if (maximumEntries == (std::numeric_limits<size_t>::max)())
            throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "fixed map prefix lookahead bound overflowed");
        auto cursor = [&] {
            if constexpr (KeyBytes + ValueBytes <= BTreeFixedKey::MAX_BYTES) {
                // Point/prefix probes retain their existing representation;
                // a long stream amortizes the initial optional conversion.
                if (prefix.empty() && maximumEntries >= 64U)
                    return index_.openFixedKeyRangeCursor(snapshot, firstKey.empty() ? prefix : firstKey,
                        maximumEntries + 1U, KeyBytes + ValueBytes, BufferPoolReadHint::BPRH_NORMAL, false, cancellation);
            }
            return index_.openRangeCursor(snapshot, firstKey.empty() ? prefix : firstKey, maximumEntries + 1U,
                BufferPoolReadHint::BPRH_NORMAL, false, cancellation);
        }();
        const auto nextRecord = [&]() -> std::optional<Entry> {
            if (cursor.fixedKeyWidth()) {
                const auto raw = cursor.nextFixedKey(boundaryAdmission);
                return raw ? std::optional(decode(*raw)) : std::nullopt;
            }
            const auto raw = cursor.next(boundaryAdmission);
            return raw ? std::optional(decode(*raw)) : std::nullopt;
        };
        const auto matches = [prefix](const Entry &entry) {
            return std::equal(prefix.begin(), prefix.end(), entry.key.begin());
        };
        auto first = nextRecord();
        if (!first) return true;
        Entry pending = *first;
        if (!matches(pending)) return true;
        size_t emitted = 0;
        for (;;) {
            if (cancellation) cancellation->check();
            const auto next = nextRecord();
            if (next && next->key == pending.key)
                throw FormatException(FormatError::FERR_INVALID_STATE, "fixed B+Tree map has duplicate logical ownership");
            if (!visitor(pending)) return false;
            ++emitted;
            if (!next || !matches(*next)) return true;
            if (emitted == maximumEntries) return false;
            pending = *next;
        }
    }
private:
    BTreeIndex index_;
};
} // namespace graph::storage
