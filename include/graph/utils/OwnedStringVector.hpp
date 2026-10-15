/**
 * @file OwnedStringVector.hpp
 * @brief Admission for standard string-vector backing and nested buffers.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <functional>
#include <string_view>
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/utils/StringBufferBytes.hpp"

namespace graph::utils {
// Internal value type; mutation through a mutable std::vector base alias is not
// supported. Ordinary const std::vector<string> reference getters stay intact.
class OwnedStringVector final : private OwnedHeapReservation, public std::vector<std::string> {
    using Credit = OwnedHeapReservation;
    using Vector = std::vector<std::string>;
public:
    OwnedStringVector() : Credit(currentOwner(), detail::standardContainerProxyBytes()), Vector() {}
    OwnedStringVector(const Vector &source) : OwnedStringVector(currentOwner(), source) {}
    OwnedStringVector(std::initializer_list<std::string> source)
        : Credit(currentOwner(), copiedBytes(source)), Vector(source) {
        const auto retained = observedBytes(*this);
        if (retained > Credit::bytes()) throw std::logic_error("string-vector list copy exceeded its audited request bound");
        Credit::set(retained);
    }
    OwnedStringVector(Vector &&source) : Credit(currentOwner(), observedBytes(source)), Vector(std::move(source)) {}
    OwnedStringVector(const OwnedStringVector &source)
        : OwnedStringVector(currentOwner(source.Credit::owner()), static_cast<const Vector &>(source)) {}
    OwnedStringVector(OwnedStringVector &&source)
        : Credit(source.Credit::owner(), detail::standardContainerProxyBytes()),
          Vector(std::move(static_cast<Vector &>(source))) { Credit::swap(source); }
    OwnedStringVector &operator=(const OwnedStringVector &source) {
        if (this != &source) { OwnedStringVector copy(source); swap(copy); } return *this;
    }
    OwnedStringVector &operator=(OwnedStringVector &&source) {
        if (this != &source) { OwnedStringVector moved(std::move(source)); swap(moved); } return *this;
    }
    OwnedStringVector &operator=(const Vector &source) {
        OwnedStringVector copy(currentOwner(Credit::owner()), source); swap(copy); return *this;
    }
    OwnedStringVector &operator=(Vector &&source) {
        OwnedAllocationScope scope(currentOwner(Credit::owner()));
        OwnedStringVector moved(std::move(source)); swap(moved); return *this;
    }
    OwnedStringVector &operator=(std::initializer_list<std::string> source) {
        OwnedAllocationScope scope(currentOwner(Credit::owner()));
        OwnedStringVector copy(source); swap(copy); return *this;
    }
    void assign(size_t count, const std::string &value) {
        OwnedAllocationScope scope(currentOwner(Credit::owner()));
        OwnedStringVector copy; copy.reserve(count);
        for (size_t index = 0U; index < count; ++index) copy.push_back(value);
        swap(copy);
    }
    void reserveString(size_t index, size_t count) {
        auto &value = Vector::at(index);
        if (count <= value.capacity()) return;
        if (count > (std::numeric_limits<size_t>::max)()/2U) throw std::length_error("owned string capacity overflowed");
        const auto old = detail::dynamicStringBufferBytes(value);
        const auto previous = Credit::bytes();
        Credit next(Credit::owner(), detail::standardFreshStringHeapUpper(2U*count));
        value.reserve(count);
        const auto retained = detail::dynamicStringBufferBytes(value);
        constexpr auto proxy = detail::standardContainerProxyBytes();
        Credit::set(previous - old + proxy);
        next.set(retained - proxy); Credit::absorb(next);
    }
    void swap(OwnedStringVector &other) noexcept { Vector::swap(other); Credit::swap(other); }
    friend void swap(OwnedStringVector &left, OwnedStringVector &right) noexcept { left.swap(right); }
    void reserve(size_t count) {
        if (count <= Vector::capacity()) return;
        const auto nextBacking = backingBytes(count);
        const auto oldBacking = backingBytes(Vector::capacity());
        const auto extraProxies = proxyBytes(Vector::size());
        Credit next(Credit::owner(), detail::addStandardHeapBytes(nextBacking, extraProxies));
        Vector::reserve(count);
        if (Vector::capacity() != count) throw std::logic_error("string-vector reserve exceeded its audited capacity");
        next.set(nextBacking); // moved-from element proxies have been freed
        Credit::set(Credit::bytes() - oldBacking);
        Credit::absorb(next);
    }
    void push_back(const std::string &value) {
        const auto index = aliasIndex(&value);
        Credit element(Credit::owner(), detail::standardFreshStringHeapUpper(value.size()));
        ensureRoom();
        Vector::push_back(index < Vector::size() ? Vector::operator[](index) : value);
        const auto retained = detail::dynamicStringBufferBytes(Vector::back());
        if (retained > element.bytes()) throw std::logic_error("string copy exceeded its audited request bound");
        element.set(retained); Credit::absorb(element);
    }
    template<class Value> const std::string &emplace_back(Value &&value) {
        // Borrow the input; do not build an unadmitted temporary std::string.
        const std::string_view view(value);
        Credit element(Credit::owner(), detail::standardFreshStringHeapUpper(view.size()));
        // A view into an element's character buffer survives string moves on
        // these STLs for long strings; SSO views require stable index rebinding.
        // Normalize into a separately admitted candidate before any growth.
        Credit movedProxy(Credit::owner(), detail::standardContainerProxyBytes());
        std::string candidate(view.empty() ? "" : view.data(), view.size());
        element.set(detail::dynamicStringBufferBytes(candidate));
        ensureRoom();
        Vector::push_back(std::move(candidate));
        Credit::absorb(element);
        return Vector::back();
    }
    void clear() noexcept {
        Vector::clear();
        Credit::set(detail::addStandardHeapBytes(detail::standardContainerProxyBytes(), backingBytes(Vector::capacity())));
    }
    void pop_back() {
        const auto released = detail::dynamicStringBufferBytes(Vector::back());
        Vector::pop_back(); Credit::set(Credit::bytes() - released);
    }
    const std::string &operator[](size_t index) const noexcept { return Vector::operator[](index); }
    const std::string &front() const { return Vector::front(); }
    const std::string &back() const { return Vector::back(); }
    const std::string &at(size_t index) const { return Vector::at(index); }
    auto begin() const noexcept { return Vector::cbegin(); }
    auto end() const noexcept { return Vector::cend(); }
    const std::string *data() const noexcept { return Vector::data(); }
private:
    OwnedStringVector(std::shared_ptr<std::pmr::memory_resource> owner, const Vector &source)
        : Credit(std::move(owner), copiedBytes(source)), Vector(source) {
        const auto retained = observedBytes(*this);
        if (retained > Credit::bytes()) throw std::logic_error("string-vector copy exceeded its audited request bound");
        Credit::set(retained);
    }
    static std::shared_ptr<std::pmr::memory_resource> currentOwner(
            const std::shared_ptr<std::pmr::memory_resource> &fallback = {}) {
        return OwnedAllocationScope::active() ? OwnedAllocationScope::owner() : fallback;
    }
    static size_t proxyBytes(size_t count) {
        constexpr auto proxy = detail::standardContainerProxyBytes();
        if constexpr (proxy != 0U) {
            if (count > (std::numeric_limits<size_t>::max)()/proxy)
                throw std::overflow_error("string-vector proxy request overflowed");
        }
        return count * proxy;
    }
    static size_t backingBytes(size_t count) {
        if (count > (std::numeric_limits<size_t>::max)()/sizeof(std::string)) throw std::length_error("string-vector backing overflowed");
        return detail::standardAllocatorRequestBytes(count * sizeof(std::string), alignof(std::string));
    }
    template<class Range> static size_t copiedBytes(const Range &source) {
        auto total = detail::addStandardHeapBytes(detail::standardContainerProxyBytes(), backingBytes(source.size()));
        for (const auto &value : source) total = detail::addStandardHeapBytes(total, detail::standardFreshStringHeapUpper(value.size()));
        return total;
    }
    static size_t observedBytes(const Vector &source) {
        auto total = detail::standardVectorHeapBufferBytes(source);
        for (const auto &value : source) total = detail::addStandardHeapBytes(total, detail::dynamicStringBufferBytes(value));
        return total;
    }
    size_t aliasIndex(const std::string *value) const noexcept {
        if (Vector::empty()) return Vector::size();
        const auto *first = Vector::data(); const auto *last = first + Vector::size();
        const std::less<const std::string *> less;
        return !less(value, first) && less(value, last) ? static_cast<size_t>(value - first) : Vector::size();
    }
    void ensureRoom() {
        if (Vector::size() < Vector::capacity()) return;
        const auto old = Vector::capacity(), maximum = Vector::max_size();
        if (old == maximum) throw std::length_error("string-vector capacity exhausted");
        reserve(old == 0U ? 1U : old + (std::min)(old, maximum - old));
    }
    using Vector::erase;
    using Vector::insert;
    using Vector::resize;
    using Vector::shrink_to_fit;
};
} // namespace graph::utils
