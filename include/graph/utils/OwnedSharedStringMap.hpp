/**
 * @file OwnedSharedStringMap.hpp
 * @brief Admitted immutable string maps of independently owned shared values.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <unordered_map>
#include "graph/utils/StandardContainerHeapBytes.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
namespace graph::utils {
template<class Value>
class OwnedSharedStringMap final : private OwnedHeapReservation, public std::unordered_map<std::string, std::shared_ptr<Value>> {
    using Credit = utils::OwnedHeapReservation;
    using Map = std::unordered_map<std::string, std::shared_ptr<Value>>;
public:
    OwnedSharedStringMap() : Credit(currentOwner(), emptyBytes()), Map() {}
    OwnedSharedStringMap(const Map &source) : OwnedSharedStringMap(currentOwner(), source) {}
    // Normalize hidden bucket capacity rather than adopting an opaque raw map.
    OwnedSharedStringMap(Map &&source) : OwnedSharedStringMap(static_cast<const Map &>(source)) {}
    OwnedSharedStringMap(const OwnedSharedStringMap &source)
        : OwnedSharedStringMap(currentOwner(source.Credit::owner()), static_cast<const Map &>(source)) {}
    OwnedSharedStringMap(OwnedSharedStringMap &&source)
        : Credit(source.Credit::owner(), emptyBytes()), Map() {
        Map::swap(source); Credit::swap(source);
    }
    OwnedSharedStringMap &operator=(const OwnedSharedStringMap &source) {
        if (this != &source) { OwnedSharedStringMap copy(source); swap(copy); } return *this;
    }
    OwnedSharedStringMap &operator=(OwnedSharedStringMap &&source) {
        if (this != &source) { OwnedSharedStringMap moved(std::move(source)); swap(moved); } return *this;
    }
    OwnedSharedStringMap &operator=(const Map &source) {
        OwnedSharedStringMap copy(currentOwner(Credit::owner()), source); swap(copy); return *this;
    }
    OwnedSharedStringMap &operator=(Map &&source) { return operator=(static_cast<const Map &>(source)); }
    void swap(OwnedSharedStringMap &other) noexcept { Map::swap(other); Credit::swap(other); }
    friend void swap(OwnedSharedStringMap &a, OwnedSharedStringMap &b) noexcept { a.swap(b); }
    const std::shared_ptr<Value> &at(const std::string &key) const { return Map::at(key); }
    auto begin() const noexcept { return Map::cbegin(); }
    auto end() const noexcept { return Map::cend(); }
    auto find(const std::string &key) const { return static_cast<const Map &>(*this).find(key); }
    auto equal_range(const std::string &key) const { return static_cast<const Map &>(*this).equal_range(key); }
    template<class Transform>
    [[nodiscard]] static OwnedSharedStringMap transformValues(const Map &source, Transform transform) {
        OwnedSharedStringMap result(source);
        OwnedAllocationScope scope(result.Credit::owner());
        for (auto &[key, value] : static_cast<Map &>(result)) {
            static_cast<void>(key);
            value = transform(value);
        }
        return result;
    }
private:
    OwnedSharedStringMap(std::shared_ptr<std::pmr::memory_resource> owner, const Map &source)
        : Credit(std::move(owner), copiedBytes(source)), Map(source) {}
    static std::shared_ptr<std::pmr::memory_resource> currentOwner(
            const std::shared_ptr<std::pmr::memory_resource> &fallback = {}) {
        return utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() : fallback;
    }
    static size_t emptyBytes() {
#if defined(_MSVC_STL_VERSION)
        using namespace utils::detail;
        constexpr auto node = sizeof(typename Map::value_type) + 2U*sizeof(void *) + alignof(typename Map::value_type) - 1U;
        return addStandardHeapBytes(2U*standardContainerProxyBytes(),
            addStandardHeapBytes(standardAllocatorRequestBytes(node, alignof(typename Map::value_type)),
                standardAllocatorRequestBytes(16U*sizeof(void *), alignof(void *))));
#else
        return 0U;
#endif
    }
    static size_t copiedBytes(const Map &source) {
        auto bytes = utils::detail::standardUnorderedMapStructuralHeapBytes(source);
        for (const auto &[key, value] : source) {
            bytes = utils::detail::addStandardHeapBytes(bytes, utils::detail::standardFreshStringHeapUpper(key.size()));
            static_cast<void>(value); // Pointees and control blocks retain their own admission.
        }
        return bytes;
    }
    using Map::operator[];
    using Map::clear;
    using Map::emplace;
    using Map::emplace_hint;
    using Map::erase;
    using Map::extract;
    using Map::insert;
    using Map::insert_or_assign;
    using Map::max_load_factor;
    using Map::merge;
    using Map::rehash;
    using Map::reserve;
    using Map::try_emplace;
};
} // namespace graph::utils
