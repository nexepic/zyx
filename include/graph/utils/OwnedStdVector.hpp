/**
 * @file OwnedStdVector.hpp
 * @brief Pre-admitted standard-vector backing for self-governed elements.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <functional>
#include <initializer_list>
#include <type_traits>
#include <vector>
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/utils/StandardContainerHeapBytes.hpp"

namespace graph::utils {
namespace detail {
template<class T> struct OwnedPointerElement : std::false_type {};
template<class T, class D> struct OwnedPointerElement<std::unique_ptr<T, D>> : std::true_type {};
template<class T> struct OwnedPointerElement<std::shared_ptr<T>> : std::true_type {};
}
// Only backing storage is counted here. Native pointees and marked value fields
// must retain their own heap admission; plain strings/PropertyValues need a
// different aggregate policy. Public const std::vector reference APIs survive.
template<class T>
class OwnedStdVector final : private OwnedHeapReservation, public std::vector<T> {
    static_assert(std::is_trivially_copyable_v<T> || detail::OwnedPointerElement<T>::value ||
        requires { typename T::OwnedHeapElement; }, "vector element must govern its own heap payload");
    using Credit = OwnedHeapReservation;
    using Vector = std::vector<T>;
public:
    OwnedStdVector() : Credit(currentOwner(), detail::standardContainerProxyBytes()), Vector() {}
    OwnedStdVector(std::initializer_list<T> source) requires std::is_copy_constructible_v<T> : OwnedStdVector() {
        reserve(source.size());
        for (const auto &value : source) push_back(value);
    }
    OwnedStdVector(Vector &&source)
        : Credit(currentOwner(), detail::standardVectorHeapBufferBytes(source)), Vector(std::move(source)) {}
    OwnedStdVector(const Vector &source) requires (std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>)
        : OwnedStdVector(currentOwner(), source) {}
    OwnedStdVector(const OwnedStdVector &source) requires (std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>)
        : OwnedStdVector(currentOwner(source.Credit::owner()), static_cast<const Vector &>(source)) {}
    OwnedStdVector(const OwnedStdVector &) requires (!std::is_copy_constructible_v<T> || !std::is_copy_assignable_v<T>) = delete;
    OwnedStdVector(OwnedStdVector &&source)
        : Credit(source.Credit::owner(), detail::standardContainerProxyBytes()),
          Vector(std::move(static_cast<Vector &>(source))) { Credit::swap(source); }
    OwnedStdVector &operator=(const OwnedStdVector &source) requires (std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>) {
        if (this != &source) { OwnedStdVector copy(source); swap(copy); } return *this;
    }
    OwnedStdVector &operator=(OwnedStdVector &&source) {
        if (this != &source) { OwnedStdVector moved(std::move(source)); swap(moved); } return *this;
    }
    OwnedStdVector &operator=(std::initializer_list<T> source) requires std::is_copy_constructible_v<T> {
        OwnedAllocationScope scope(currentOwner(Credit::owner()));
        OwnedStdVector copy(source); swap(copy); return *this;
    }
    OwnedStdVector &operator=(Vector &&source) {
        OwnedAllocationScope scope(currentOwner(Credit::owner()));
        OwnedStdVector moved(std::move(source)); swap(moved); return *this;
    }
    OwnedStdVector &operator=(const Vector &source) requires (std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>) {
        OwnedStdVector copy(currentOwner(Credit::owner()), source); swap(copy); return *this;
    }
    void swap(OwnedStdVector &other) noexcept { Vector::swap(other); Credit::swap(other); }
    friend void swap(OwnedStdVector &left, OwnedStdVector &right) noexcept { left.swap(right); }
    void reserve(size_t count) {
        if (count <= Vector::capacity()) return;
        const size_t nextBytes = backingBytes(count);
        Credit next(Credit::owner(), nextBytes);
        OwnedAllocationScope scope(Credit::owner());
        Vector::reserve(count);
        // Supported C++20 default STL allocators reserve exactly count slots.
        if (Vector::capacity() != count)
            throw std::logic_error("standard vector reserve exceeded its audited capacity");
        // The old backing was freed by reserve; its proxy remains in place.
        Credit::set(detail::standardContainerProxyBytes());
        Credit::absorb(next);
    }
    void push_back(const T &value) requires std::is_copy_constructible_v<T> {
        const auto index = aliasIndex(&value);
        ensureRoom();
        OwnedAllocationScope scope(Credit::owner());
        Vector::push_back(index < Vector::size() ? Vector::operator[](index) : value);
    }
    void push_back(T &&value) {
        const auto index = aliasIndex(&value);
        ensureRoom();
        OwnedAllocationScope scope(Credit::owner());
        Vector::push_back(std::move(index < Vector::size() ? Vector::operator[](index) : value));
    }
    template<class... Args> T &emplace_back(Args &&...args) {
        OwnedAllocationScope scope(Credit::owner());
        T candidate(std::forward<Args>(args)...);
        push_back(std::move(candidate)); return Vector::back();
    }
    void resize(size_t count) requires std::is_nothrow_default_constructible_v<T> {
        reserve(count); Vector::resize(count);
    }
    [[nodiscard]] size_t retainedBackingRequestBytes() const noexcept { return Credit::bytes(); }
private:
    OwnedStdVector(std::shared_ptr<std::pmr::memory_resource> owner, const Vector &source)
        : Credit(std::move(owner), detail::addStandardHeapBytes(detail::standardContainerProxyBytes(), backingBytes(source.size()))), Vector() {
        OwnedAllocationScope scope(Credit::owner());
        Vector::assign(source.begin(), source.end());
    }
    static std::shared_ptr<std::pmr::memory_resource> currentOwner(
            const std::shared_ptr<std::pmr::memory_resource> &fallback = {}) {
        return OwnedAllocationScope::active() ? OwnedAllocationScope::owner() : fallback;
    }
    static size_t backingBytes(size_t count) {
        if (count > (std::numeric_limits<size_t>::max)()/sizeof(T)) throw std::length_error("vector backing request overflowed");
        return detail::standardAllocatorRequestBytes(count * sizeof(T), alignof(T));
    }
    size_t aliasIndex(const T *value) const noexcept {
        if (Vector::empty()) return Vector::size();
        const auto *first = Vector::data(); const auto *last = first + Vector::size();
        const std::less<const T *> less;
        return !less(value, first) && less(value, last) ? static_cast<size_t>(value - first) : Vector::size();
    }
    void ensureRoom() {
        if (Vector::size() < Vector::capacity()) return;
        const size_t old = Vector::capacity();
        const size_t maximum = Vector::max_size();
        if (old == maximum) throw std::length_error("vector capacity exhausted");
        reserve(old == 0U ? 1U : old + (std::min)(old, maximum - old));
    }
    using Vector::assign;
    using Vector::erase;
    using Vector::insert;
    using Vector::shrink_to_fit;
};
} // namespace graph::utils
