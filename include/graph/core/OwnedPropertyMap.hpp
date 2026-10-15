/**
 * @file OwnedPropertyMap.hpp
 * @brief Immutable property-map copies admitted before nested allocation.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "graph/core/PropertyHeapRequestBytes.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
namespace graph {
class OwnedPropertyMap final : private utils::OwnedHeapReservation, public PropertyValue::MapType {
    using Credit = utils::OwnedHeapReservation;
    using Map = PropertyValue::MapType;
public:
    OwnedPropertyMap() : Credit(currentOwner(), emptyBytes()), Map() {}
    OwnedPropertyMap(const Map &source) : OwnedPropertyMap(currentOwner(), source) {}
    // Normalize hidden bucket capacity rather than adopting an opaque raw map.
    OwnedPropertyMap(Map &&source) : OwnedPropertyMap(static_cast<const Map &>(source)) {}
    OwnedPropertyMap(const OwnedPropertyMap &source)
        : OwnedPropertyMap(currentOwner(source.Credit::owner()), static_cast<const Map &>(source)) {}
    OwnedPropertyMap(OwnedPropertyMap &&source)
        : Credit(source.Credit::owner(), emptyBytes()), Map() {
        Map::swap(source); Credit::swap(source);
    }
    OwnedPropertyMap &operator=(const OwnedPropertyMap &source) {
        if (this != &source) { OwnedPropertyMap copy(source); swap(copy); } return *this;
    }
    OwnedPropertyMap &operator=(OwnedPropertyMap &&source) {
        if (this != &source) { OwnedPropertyMap moved(std::move(source)); swap(moved); } return *this;
    }
    OwnedPropertyMap &operator=(const Map &source) {
        OwnedPropertyMap copy(currentOwner(Credit::owner()), source); swap(copy); return *this;
    }
    OwnedPropertyMap &operator=(Map &&source) { return operator=(static_cast<const Map &>(source)); }
    void swap(OwnedPropertyMap &other) noexcept { Map::swap(other); Credit::swap(other); }
    friend void swap(OwnedPropertyMap &a, OwnedPropertyMap &b) noexcept { a.swap(b); }
    const PropertyValue &at(const std::string &key) const { return Map::at(key); }
    auto begin() const noexcept { return Map::cbegin(); }
    auto end() const noexcept { return Map::cend(); }
    auto find(const std::string &key) const { return static_cast<const Map &>(*this).find(key); }
    auto equal_range(const std::string &key) const { return static_cast<const Map &>(*this).equal_range(key); }
private:
    OwnedPropertyMap(std::shared_ptr<std::pmr::memory_resource> owner, const Map &source)
        : Credit(std::move(owner), copiedBytes(source)), Map(source) {}
    static std::shared_ptr<std::pmr::memory_resource> currentOwner(
            const std::shared_ptr<std::pmr::memory_resource> &fallback = {}) {
        return utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() : fallback;
    }
    static size_t emptyBytes() {
#if defined(_MSVC_STL_VERSION)
        using namespace utils::detail;
        constexpr auto node = sizeof(Map::value_type) + 2U*sizeof(void *) + alignof(Map::value_type) - 1U;
        return addStandardHeapBytes(2U*standardContainerProxyBytes(),
            addStandardHeapBytes(standardAllocatorRequestBytes(node, alignof(Map::value_type)),
                standardAllocatorRequestBytes(16U*sizeof(void *), alignof(void *))));
#else
        return 0U;
#endif
    }
    static size_t copiedBytes(const Map &source) {
        auto bytes = utils::detail::standardUnorderedMapStructuralHeapBytes(source);
        for (const auto &[key, value] : source) {
            bytes = utils::detail::addStandardHeapBytes(bytes, utils::detail::standardFreshStringHeapUpper(key.size()));
            bytes = utils::detail::addStandardHeapBytes(bytes, property_heap::copyRequestUpper(value));
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
} // namespace graph
