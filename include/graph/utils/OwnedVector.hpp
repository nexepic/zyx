#pragma once
#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0
#include <boost/container/vector.hpp>
#endif
#include "graph/utils/OwnedPmrAllocator.hpp"

namespace graph::utils {
// Vector-compatible storage with an owning resource allocator. Conversions
// from ordinary input vectors preserve this container's allocation domain.
// A rejecting allocator cannot be used in MSVC's allocating noexcept vector
// constructors. Debug storage therefore uses the same proxy-free vector policy
// as AdmissionPmrVector. Its moves transfer buffers without allocating, keeping
// page/transaction value types' existing nonthrowing ownership handoff valid.
// Other platforms and MSVC release retain their standard-vector storage.
template<class T> using OwnedVectorStorage =
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0
    boost::container::vector<T, OwnedPmrAllocator<T>>;
#else
    std::vector<T, OwnedPmrAllocator<T>>;
#endif

template<class T> class OwnedVector : public OwnedVectorStorage<T> {
    using Base = OwnedVectorStorage<T>;
public:
    using Base::Base;
    using Base::operator=;
    OwnedVector() = default;
    OwnedVector(const OwnedVector &) = default;
    OwnedVector(OwnedVector &&) noexcept = default;
    OwnedVector &operator=(const OwnedVector &) = default;
    OwnedVector &operator=(OwnedVector &&) noexcept = default;
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0
    T &at(typename Base::size_type index) {
        if (index >= this->size()) throw std::out_of_range("owned vector index out of range");
        return (*this)[index];
    }
    const T &at(typename Base::size_type index) const {
        if (index >= this->size()) throw std::out_of_range("owned vector index out of range");
        return (*this)[index];
    }
    friend bool operator==(const OwnedVector &left, const OwnedVector &right) {
        return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
    }
    template<class Allocator>
    friend bool operator==(const OwnedVector &left, const std::vector<T, Allocator> &right) {
        return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
    }
#endif
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
