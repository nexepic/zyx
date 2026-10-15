#pragma once
#include <iterator>
#include <utility>
#include <vector>
#include "graph/utils/OwnedPmrAllocator.hpp"

namespace graph::utils {
// Vector-compatible storage with an owning resource allocator. Conversions
// from ordinary input vectors preserve this container's allocation domain.
template<class T> class OwnedVector : public std::vector<T, OwnedPmrAllocator<T>> {
    using Base = std::vector<T, OwnedPmrAllocator<T>>;
public:
    using Base::Base;
    using Base::operator=;
    OwnedVector() = default;
    OwnedVector(const OwnedVector &) = default;
    OwnedVector(OwnedVector &&) noexcept = default;
    OwnedVector &operator=(const OwnedVector &) = default;
    OwnedVector &operator=(OwnedVector &&) noexcept = default;
    template<class Allocator>
    OwnedVector(const std::vector<T, Allocator> &source, OwnedPmrAllocator<T> allocator = {})
        : Base(source.begin(), source.end(), std::move(allocator)) {}
    template<class Allocator>
    OwnedVector(std::vector<T, Allocator> &&source, OwnedPmrAllocator<T> allocator = {})
        : Base(std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()), std::move(allocator)) {}
    template<class Allocator>
    OwnedVector &operator=(const std::vector<T, Allocator> &source) {
        this->assign(source.begin(), source.end()); return *this;
    }
    template<class Allocator>
    OwnedVector &operator=(std::vector<T, Allocator> &&source) {
        this->assign(std::make_move_iterator(source.begin()), std::make_move_iterator(source.end())); return *this;
    }
};
} // namespace graph::utils
