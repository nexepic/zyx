/**
 * @file OwnedLazyUnorderedMap.hpp
 * @brief Cold maps with admitted state, buckets and nodes and captured mutation authority.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <unordered_map>
#include "graph/utils/OwnedSharedPtr.hpp"

namespace graph::utils {
// Keys and values must govern their own nested payloads. A cold map allocates
// nothing, including MSVC debug proxies. clear() destroys the entire state and
// invalidates every iterator, including the end iterator. Callers synchronize.
template<class Key, class Value, class Hash = std::hash<Key>, class Equal = std::equal_to<Key>>
class OwnedLazyUnorderedMap {
    using Entry = std::pair<const Key, Value>;
    using Map = std::unordered_map<Key, Value, Hash, Equal, OwnedPmrAllocator<Entry>>;
    struct State {
        std::shared_ptr<std::pmr::memory_resource> owner;
        Map values;
        explicit State(std::shared_ptr<std::pmr::memory_resource> resource)
            : owner(std::move(resource)), values(0U, Hash{}, Equal{}, OwnedPmrAllocator<Entry>(owner)) {}
    };
    std::shared_ptr<State> state_;
    static std::shared_ptr<State> makeState() {
        auto owner = sharedAllocationOwner();
        OwnedAllocationScope scope(owner);
        return makeSharedOwned<State>(owner);
    }
public:
    using iterator = typename Map::iterator;
    using const_iterator = typename Map::const_iterator;
    OwnedLazyUnorderedMap() noexcept = default;
    OwnedLazyUnorderedMap(const OwnedLazyUnorderedMap &) = delete;
    OwnedLazyUnorderedMap &operator=(const OwnedLazyUnorderedMap &) = delete;
    OwnedLazyUnorderedMap(OwnedLazyUnorderedMap &&) noexcept = default;
    OwnedLazyUnorderedMap &operator=(OwnedLazyUnorderedMap &&) noexcept = default;
    [[nodiscard]] size_t size() const noexcept { return state_ ? state_->values.size() : 0U; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0U; }
    iterator begin() noexcept { return state_ ? state_->values.begin() : iterator{}; }
    iterator end() noexcept { return state_ ? state_->values.end() : iterator{}; }
    const_iterator begin() const noexcept { return state_ ? state_->values.cbegin() : const_iterator{}; }
    const_iterator end() const noexcept { return state_ ? state_->values.cend() : const_iterator{}; }
    template<class Query> iterator find(const Query &key) {
        return state_ ? state_->values.find(key) : iterator{};
    }
    template<class Query> const_iterator find(const Query &key) const {
        return state_ ? state_->values.find(key) : const_iterator{};
    }
    template<class Query> bool contains(const Query &key) const { return find(key) != end(); }
    template<class... Args> std::pair<iterator, bool> emplace(Args &&...args) {
        if (state_) {
            OwnedAllocationScope scope(state_->owner);
            return state_->values.emplace(std::forward<Args>(args)...);
        }
        auto next = makeState();
        OwnedAllocationScope scope(next->owner);
        auto result = next->values.emplace(std::forward<Args>(args)...);
        state_ = std::move(next);
        return result;
    }
    void reserve(size_t count) {
        if (state_) {
            OwnedAllocationScope scope(state_->owner);
            state_->values.reserve(count);
        } else if (count != 0U) {
            auto next = makeState();
            OwnedAllocationScope scope(next->owner);
            next->values.reserve(count);
            state_ = std::move(next);
        }
    }
    size_t erase(const Key &key) {
        if (!state_) return 0U;
        return state_->values.erase(key);
    }
    void erase(iterator position) { state_->values.erase(position); }
    void erase(const_iterator position) { state_->values.erase(position); }
    void clear() noexcept { state_.reset(); }
};
} // namespace graph::utils
