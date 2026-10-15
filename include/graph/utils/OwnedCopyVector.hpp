/**
 * @file OwnedCopyVector.hpp
 * @brief Immutable vector copies admitted before backing and nested allocations.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <vector>
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/utils/StandardContainerHeapBytes.hpp"
namespace graph::utils {
// Policy::copyBytes describes each element's nested standard-allocator requests.
// Raw inputs are copied, including rvalues: opaque nested container capacities
// cannot safely be adopted using a fresh-copy bound. Mutation is whole-value only.
template<class T, class Policy>
class OwnedCopyVector final : private OwnedHeapReservation, public std::vector<T> {
    using Credit = OwnedHeapReservation;
    using Vector = std::vector<T>;
public:
    OwnedCopyVector() : Credit(currentOwner(), detail::standardContainerProxyBytes()), Vector() {}
    OwnedCopyVector(const Vector &source) : OwnedCopyVector(currentOwner(), source) {}
    OwnedCopyVector(Vector &&source) : OwnedCopyVector(static_cast<const Vector &>(source)) {}
    OwnedCopyVector(std::initializer_list<T> source) : Credit(currentOwner(), copiedBytes(source)), Vector(source) {}
    OwnedCopyVector(const OwnedCopyVector &source)
        : OwnedCopyVector(currentOwner(source.Credit::owner()), static_cast<const Vector &>(source)) {}
    OwnedCopyVector(OwnedCopyVector &&source)
        : Credit(source.Credit::owner(), detail::standardContainerProxyBytes()),
          Vector(std::move(static_cast<Vector &>(source))) { Credit::swap(source); }
    OwnedCopyVector &operator=(const OwnedCopyVector &source) {
        if (this != &source) { OwnedCopyVector copy(source); swap(copy); } return *this;
    }
    OwnedCopyVector &operator=(OwnedCopyVector &&source) {
        if (this != &source) { OwnedCopyVector moved(std::move(source)); swap(moved); } return *this;
    }
    OwnedCopyVector &operator=(const Vector &source) {
        OwnedCopyVector copy(currentOwner(Credit::owner()), source); swap(copy); return *this;
    }
    OwnedCopyVector &operator=(Vector &&source) { return operator=(static_cast<const Vector &>(source)); }
    void swap(OwnedCopyVector &other) noexcept { Vector::swap(other); Credit::swap(other); }
    friend void swap(OwnedCopyVector &a, OwnedCopyVector &b) noexcept { a.swap(b); }
    const T &operator[](size_t i) const noexcept { return Vector::operator[](i); }
    const T &at(size_t i) const { return Vector::at(i); }
    const T &front() const { return Vector::front(); }
    const T &back() const { return Vector::back(); }
    const T *data() const noexcept { return Vector::data(); }
    auto begin() const noexcept { return Vector::cbegin(); }
    auto end() const noexcept { return Vector::cend(); }
    auto rbegin() const noexcept { return Vector::crbegin(); }
    auto rend() const noexcept { return Vector::crend(); }
private:
    OwnedCopyVector(std::shared_ptr<std::pmr::memory_resource> owner, const Vector &source)
        : Credit(std::move(owner), copiedBytes(source)), Vector(source) {}
    static std::shared_ptr<std::pmr::memory_resource> currentOwner(
            const std::shared_ptr<std::pmr::memory_resource> &fallback = {}) {
        return OwnedAllocationScope::active() ? OwnedAllocationScope::owner() : fallback;
    }
    template<class Range> static size_t copiedBytes(const Range &source) {
        if (source.size() > (std::numeric_limits<size_t>::max)()/sizeof(T))
            throw std::overflow_error("owned copy vector backing overflowed");
        auto bytes = detail::addStandardHeapBytes(detail::standardContainerProxyBytes(),
            detail::standardAllocatorRequestBytes(source.size()*sizeof(T), alignof(T)));
        for (const auto &value : source) bytes = detail::addStandardHeapBytes(bytes, Policy::copyBytes(value));
        return bytes;
    }
    using Vector::assign;
    using Vector::clear;
    using Vector::emplace;
    using Vector::emplace_back;
    using Vector::erase;
    using Vector::insert;
    using Vector::pop_back;
    using Vector::push_back;
    using Vector::reserve;
    using Vector::resize;
    using Vector::shrink_to_fit;
};
} // namespace graph::utils
