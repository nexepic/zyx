/**
 * @file OwnedStdString.hpp
 * @brief Pre-admitted standard strings retained by private plan fields.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <string>
#include <string_view>
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/utils/StandardContainerHeapBytes.hpp"
#include "graph/utils/StringBufferBytes.hpp"

namespace graph::utils {
// Internal value type: standard-string reference APIs remain compatible. Do not
// delete through a std::string pointer or mutate through a mutable base alias.
// Credit precedes the string base and is refunded after its heap is destroyed.
class OwnedStdString final : private OwnedHeapReservation, public std::string {
    using Credit = OwnedHeapReservation;
    using String = std::string;
public:
    using OwnedHeapElement = void;
    OwnedStdString() : OwnedStdString(currentOwner(), std::string_view{}) {}
    OwnedStdString(const char *source) : OwnedStdString(currentOwner(), std::string_view(source)) {}
    OwnedStdString(std::string_view source) : OwnedStdString(currentOwner(), source) {}
    OwnedStdString(const String &source) : OwnedStdString(currentOwner(), std::string_view(source)) {}
    OwnedStdString(const OwnedStdString &source)
        : OwnedStdString(currentOwner(source.Credit::owner()), std::string_view(source)) {}
    OwnedStdString(OwnedStdString &&source)
        : Credit(source.Credit::owner(), detail::standardContainerProxyBytes()),
          String(std::move(static_cast<String &>(source))) {
        // MSVC move construction creates one proxy for the emptied source.
        // Its credit was admitted above, before entering the STL constructor.
        Credit::swap(source);
    }
    OwnedStdString &operator=(const OwnedStdString &source) {
        if (this != &source) { OwnedStdString copy(source); swap(copy); }
        return *this;
    }
    OwnedStdString &operator=(OwnedStdString &&source) {
        if (this != &source) { OwnedStdString moved(std::move(source)); swap(moved); }
        return *this;
    }
    OwnedStdString &operator=(const String &source) {
        if (static_cast<const String *>(this) == &source) return *this;
        OwnedStdString copy(currentOwner(Credit::owner()), std::string_view(source));
        swap(copy); return *this;
    }
    OwnedStdString &operator=(const char *source) {
        OwnedStdString copy(currentOwner(Credit::owner()), std::string_view(source));
        swap(copy); return *this;
    }
    void clear() noexcept { String::clear(); } // Capacity remains owned.
    OwnedStdString &assign(size_t count, char value) {
        OwnedStdString copy(Credit::owner(), count, value); swap(copy); return *this;
    }
    void reserve(size_t count) {
        if (count <= String::capacity()) return;
        if (count > (std::numeric_limits<size_t>::max)()/2U) throw std::length_error("owned string reserve overflowed");
        Credit next(Credit::owner(), detail::standardFreshStringHeapUpper(2U*count));
        String::reserve(count);
        const auto retained = detail::dynamicStringBufferBytes(*this);
        constexpr auto proxy = detail::standardContainerProxyBytes();
        Credit::set(proxy); // reserve freed the former character buffer.
        next.set(retained - proxy); Credit::absorb(next);
    }
    OwnedStdString &append(std::string_view suffix) {
        if (suffix.empty()) return *this;
        if (suffix.size() > String::max_size() - String::size())
            throw std::length_error("owned string append overflowed");
        const auto total = String::size() + suffix.size();
        if (total <= String::capacity()) {
            // Audited STLs do not allocate within retained capacity; standard
            // append also preserves overlapping views into this string.
            String::append(suffix.data(), suffix.size());
            return *this;
        }
        // The old buffer stays alive while reading an aliased suffix. Admit a
        // complete candidate before any character copy or observable mutation.
        OwnedStdString next(Credit::owner(), total, '\0');
        String::copy(next.String::data(), String::size());
        std::char_traits<char>::copy(next.String::data() + String::size(), suffix.data(), suffix.size());
        swap(next);
        return *this;
    }
    OwnedStdString &operator+=(std::string_view suffix) { return append(suffix); }
    OwnedStdString &operator+=(char suffix) { return append(std::string_view(&suffix, 1U)); }
    void swap(OwnedStdString &other) noexcept {
        String::swap(other);
        Credit::swap(other);
    }
    friend void swap(OwnedStdString &left, OwnedStdString &right) noexcept { left.swap(right); }
    [[nodiscard]] size_t retainedHeapRequestBytes() const noexcept { return Credit::bytes(); }
private:
    OwnedStdString(std::shared_ptr<std::pmr::memory_resource> owner, size_t count, char value)
        : Credit(std::move(owner), detail::standardFreshStringHeapUpper(count)), String(count, value) {
        const auto retained = detail::dynamicStringBufferBytes(*this);
        if (retained > Credit::bytes()) throw std::logic_error("count string construction exceeded its audited request bound");
        Credit::set(retained);
    }
    OwnedStdString(std::shared_ptr<std::pmr::memory_resource> owner, std::string_view source)
        : Credit(std::move(owner), detail::standardFreshStringHeapUpper(source.size())),
          String(source.empty() ? "" : source.data(), source.size()) {
        const auto retained = detail::dynamicStringBufferBytes(*this);
        if (retained > Credit::bytes())
            throw std::logic_error("standard string construction exceeded its audited request bound");
        Credit::set(retained);
    }
    static std::shared_ptr<std::pmr::memory_resource> currentOwner(
            const std::shared_ptr<std::pmr::memory_resource> &fallback = {}) {
        return OwnedAllocationScope::active() ? OwnedAllocationScope::owner() : fallback;
    }
    // Heap-changing operations must go through admitted replacement above.
    using String::erase;
    using String::insert;
    using String::pop_back;
    using String::push_back;
    using String::replace;
    using String::resize;
    using String::shrink_to_fit;
};
} // namespace graph::utils
