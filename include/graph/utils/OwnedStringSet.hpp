/**
 * @file OwnedStringSet.hpp
 * @brief Admitted immutable string-set metadata and key copies.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <unordered_set>
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/utils/StringBufferBytes.hpp"
namespace graph::utils {
class OwnedStringSet final : private OwnedHeapReservation, public std::unordered_set<std::string> {
    using Credit = OwnedHeapReservation;
    using Set = std::unordered_set<std::string>;
public:
    OwnedStringSet() : Credit(currentOwner(), emptyBytes()), Set() {}
    OwnedStringSet(const Set &source) : OwnedStringSet(currentOwner(), source) {}
    // Normalize raw inputs; an opaque MSVC bucket vector can retain invisible
    // capacity. Its copy uses the visible bucket-vector size instead.
    OwnedStringSet(Set &&source) : OwnedStringSet(currentOwner(), static_cast<const Set &>(source)) {}
    OwnedStringSet(const OwnedStringSet &source)
        : OwnedStringSet(currentOwner(source.Credit::owner()), static_cast<const Set &>(source)) {}
    OwnedStringSet(OwnedStringSet &&source)
        : Credit(source.Credit::owner(), emptyBytes()), Set(std::move(static_cast<Set &>(source))) { Credit::swap(source); }
    OwnedStringSet &operator=(const OwnedStringSet &source) {
        if (this != &source) { OwnedStringSet copy(source); swap(copy); } return *this;
    }
    OwnedStringSet &operator=(OwnedStringSet &&source) {
        if (this != &source) { OwnedStringSet moved(std::move(source)); swap(moved); } return *this;
    }
    OwnedStringSet &operator=(const Set &source) {
        OwnedStringSet copy(currentOwner(Credit::owner()), source); swap(copy); return *this;
    }
    OwnedStringSet &operator=(Set &&source) { return operator=(static_cast<const Set &>(source)); }
    void swap(OwnedStringSet &other) noexcept { Set::swap(other); Credit::swap(other); }
    friend void swap(OwnedStringSet &left, OwnedStringSet &right) noexcept { left.swap(right); }
private:
    OwnedStringSet(std::shared_ptr<std::pmr::memory_resource> owner, const Set &source)
        : Credit(std::move(owner), copiedBytes(source)), Set(source) {}
    static std::shared_ptr<std::pmr::memory_resource> currentOwner(
            const std::shared_ptr<std::pmr::memory_resource> &fallback = {}) {
        return OwnedAllocationScope::active() ? OwnedAllocationScope::owner() : fallback;
    }
    static size_t structuralBytes(size_t entries, size_t buckets) {
        using namespace detail;
        constexpr auto node = sizeof(std::string) + 2U * sizeof(void *) + alignof(std::string) - 1U;
#if defined(_MSVC_STL_VERSION)
        entries = addStandardHeapBytes(entries, 1U);
        constexpr size_t width = 2U * sizeof(void *);
        constexpr size_t proxies = 2U * standardContainerProxyBytes();
#else
        constexpr size_t width = sizeof(void *);
        constexpr size_t proxies = 0U;
#endif
        if (buckets > (std::numeric_limits<size_t>::max)()/width) throw std::overflow_error("string-set buckets overflowed");
        const auto nodeRequest = standardAllocatorRequestBytes(node, alignof(std::string));
        if (entries > (std::numeric_limits<size_t>::max)()/nodeRequest) throw std::overflow_error("string-set nodes overflowed");
        return addStandardHeapBytes(proxies, addStandardHeapBytes(entries * nodeRequest,
            standardAllocatorRequestBytes(buckets * width, alignof(void *))));
    }
    static size_t emptyBytes() {
#if defined(_MSVC_STL_VERSION)
        return structuralBytes(0U, 8U);
#else
        return 0U;
#endif
    }
    static size_t copiedBytes(const Set &source) {
        auto bytes = structuralBytes(source.size(), source.bucket_count());
        for (const auto &key : source) bytes = detail::addStandardHeapBytes(bytes, detail::standardFreshStringHeapUpper(key.size()));
        return bytes;
    }
    using Set::clear;
    using Set::emplace;
    using Set::emplace_hint;
    using Set::erase;
    using Set::extract;
    using Set::insert;
    using Set::max_load_factor;
    using Set::merge;
    using Set::rehash;
    using Set::reserve;
};
} // namespace graph::utils
