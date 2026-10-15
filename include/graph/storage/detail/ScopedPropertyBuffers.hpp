/**
 * @file ScopedPropertyBuffers.hpp
 * @brief Pre-admission of unmanaged property-reader backing buffers.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <limits>
#include <stdexcept>
#include <vector>
#include <string>
#include "graph/core/PropertyDecodeAllocationScope.hpp"
#include "graph/query/execution/StandardContainerHeapBytes.hpp"

namespace graph::storage::detail {
// Standard-vector backing storage only. PMR/OwnedVector requests have their own
// allocator authority; callers must not charge them through this helper too.
// A fresh std::string count construction, including supported ABI rounding.
inline void admitScopedPropertyString(size_t count) {
    auto *scope = property_decode::AllocationScope::current();
    if (!scope) return;
    using namespace query::execution::detail;
    size_t bytes = standardContainerProxyBytes();
#if defined(_MSVC_STL_VERSION)
    if (count > 15U) bytes = addStandardHeapBytes(bytes,
        standardAllocatorRequestBytes(addStandardHeapBytes(count | size_t{15U}, 1U), alignof(char)));
#elif defined(__GLIBCXX__) && _GLIBCXX_USE_CXX11_ABI
    if (count > 15U) bytes = addStandardHeapBytes(bytes,
        standardAllocatorRequestBytes(addStandardHeapBytes(count, 1U), alignof(char)));
#elif defined(_LIBCPP_VERSION)
    if (count > sizeof(std::string) - 2U) {
        const auto doubled = addStandardHeapBytes(count, count);
        bytes = addStandardHeapBytes(bytes,
            standardAllocatorRequestBytes(addStandardHeapBytes(doubled, 1U), alignof(char)));
    }
#else
    throw std::logic_error("admitted property names require a supported standard string ABI");
#endif
    if (bytes) scope->admit(bytes);
}

template<class T>
void admitScopedPropertyVectorStorage(size_t count, size_t capacity = 0U) {
    auto *scope = property_decode::AllocationScope::current();
    if (!scope || count <= capacity) return;
    if (count > (std::numeric_limits<size_t>::max)()/sizeof(T))
        throw std::length_error("property reader vector exceeds addressable backing storage");
    scope->admit(query::execution::detail::standardAllocatorRequestBytes(count * sizeof(T), alignof(T)));
}

template<class T>
void admitScopedPropertyVectorGrowth(const std::vector<T> &values, size_t count) {
    admitScopedPropertyVectorStorage<T>(count, values.capacity());
}
} // namespace graph::storage::detail
