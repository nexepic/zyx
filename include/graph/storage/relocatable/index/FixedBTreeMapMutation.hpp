/**
 * @file FixedBTreeMapMutation.hpp
 * @brief Canonical logical-prefix replacements for a shared native map.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <map>
#include <memory_resource>
#include "graph/storage/relocatable/index/FixedBTreeMap.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"

namespace graph::storage {

// One instance owns all changes to an existing index in an enclosing graph transaction.
// A repeated SET/REMOVE changes final state, never replays two suffix inserts
// against the same base prefix. Call stage exactly once, after all final values
// and locator identities have been assigned. The transaction must outlive this
// synchronous planner. Its PMR resource admits map nodes before allocation.
template<size_t KeyBytes, size_t ValueBytes>
class FixedBTreeMapMutation final {
    using Map = FixedBTreeMap<KeyBytes, ValueBytes>;
public:
    using Key = typename Map::Key;
    using Value = typename Map::Value;
private:
    struct Change { std::optional<Value> original, final; };
    NativeGraphWriteTransaction &transaction_;
    BTreePageStore &pages_;
    Map map_;
    uint64_t index_id_;
    size_t maximum_keys_;
    std::pmr::map<Key, Change> changes_;
    bool staged_ = false;

    void change(const Key &key, std::optional<Value> value,
        const std::function<void(size_t)> &boundaryAdmission) {
        if (staged_ || !transaction_.isActive()) throw std::logic_error("fixed map mutation is no longer writable");
        if (auto existing = changes_.find(key); existing != changes_.end()) {
            existing->second.final = value;
            return;
        }
        if (changes_.size() == maximum_keys_)
            throw FormatException(FormatError::FERR_INVALID_STATE, "fixed map mutation exceeds its logical-key bound");
        auto original = map_.find(transaction_.baseSnapshot(), key, boundaryAdmission);
        changes_.emplace(key, Change{original, value});
    }
public:
    FixedBTreeMapMutation(NativeGraphWriteTransaction &transaction, BTreePageStore &pages,
        RelocatableMetadataStore &metadata, uint64_t indexId, size_t maximumKeys,
        std::pmr::memory_resource &resource)
        : transaction_(transaction), pages_(pages), map_(indexId, pages, metadata),
          index_id_(indexId), maximum_keys_(maximumKeys), changes_(&resource) {
        if (!transaction.isActive() || !indexId || !maximumKeys)
            throw std::invalid_argument("fixed map mutation requires an active transaction and positive bounds");
        if (!metadata.ownsSnapshot(transaction.baseSnapshot()) ||
            !metadata.findIndexRootOptional(transaction.baseSnapshot(), indexId))
            throw std::invalid_argument("fixed map mutation requires an existing root in its transaction domain");
    }
    FixedBTreeMapMutation(const FixedBTreeMapMutation &) = delete;
    FixedBTreeMapMutation &operator=(const FixedBTreeMapMutation &) = delete;
    // Native cursor/source buffers and staged transaction buffers remain
    // separate owners. Pass boundary admission for the selected route; the PMR
    // resource above accounts this coalescer's retained map nodes only.
    void set(const Key &key, const Value &value,
        const std::function<void(size_t)> &boundaryAdmission = {}) { change(key, value, boundaryAdmission); }
    void erase(const Key &key,
        const std::function<void(size_t)> &boundaryAdmission = {}) { change(key, {}, boundaryAdmission); }
    [[nodiscard]] size_t changedPrefixCount() const noexcept {
        size_t count = 0;
        for (const auto &[key, value] : changes_) count += value.original != value.final;
        return count;
    }
    // Native staging can allocate. On any failure poison the enclosing graph
    // transaction by rollback; a caller must never commit a half-staged map.
    void stage() {
        if (staged_) throw std::logic_error("fixed map mutation is already staged");
        staged_ = true;
        try {
            for (const auto &[key, value] : changes_) {
                if (value.original == value.final) continue;
                if (value.original) {
                    const auto record = Map::encode(key, *value.original);
                    transaction_.stageScalarIndexErase(pages_, index_id_, record, BTreeLeafValueLayout::BLVL_EMPTY);
                }
                if (value.final) {
                    const auto record = Map::encode(key, *value.final);
                    transaction_.stageScalarIndexUpsert(pages_, index_id_, record, 0, BTreeLeafValueLayout::BLVL_EMPTY);
                }
            }
        } catch (...) {
            transaction_.rollback();
            throw;
        }
    }
};
} // namespace graph::storage
