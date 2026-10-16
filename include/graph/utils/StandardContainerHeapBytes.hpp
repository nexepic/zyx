#pragma once

#include <cstddef>
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>
#include <string>
#include <unordered_map>

namespace graph::utils::detail {

// MSVC STL's iterator-debug proxy contains two pointers. Default-allocator
// strings/vectors own one; its hash table owns separate list/vector proxies.
[[nodiscard]] inline constexpr size_t standardContainerProxyBytes() noexcept {
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0
    return 2U * sizeof(void *);
#else
    return 0U;
#endif
}

[[nodiscard]] inline size_t addStandardHeapBytes(size_t left, size_t right) {
    if (right > (std::numeric_limits<size_t>::max)() - left)
        throw std::overflow_error("standard container heap size overflowed");
    return left + right;
}

// Account default std::allocator requests, including MSVC's manually aligned
// large-allocation prefix. This is separate from malloc metadata and RSS.
[[nodiscard]] inline size_t standardAllocatorRequestBytes(size_t bytes, size_t alignment) {
#if defined(_MSVC_STL_VERSION) && (defined(_M_IX86) || defined(_M_X64))
#ifdef __STDCPP_DEFAULT_NEW_ALIGNMENT__
    constexpr size_t defaultAlignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
#else
    constexpr size_t defaultAlignment = alignof(std::max_align_t);
#endif
    if (bytes >= 4096U && alignment <= defaultAlignment) {
#ifdef _DEBUG
        constexpr size_t prefix = 2U * sizeof(void *) + 31U;
#else
        constexpr size_t prefix = sizeof(void *) + 31U;
#endif
        return addStandardHeapBytes(bytes, prefix);
    }
#else
    static_cast<void>(alignment);
#endif
    return bytes;
}

// Fresh count/copy construction, using the same audited policies as property
// decoding. This is a request bound, distinct from observed retained capacity.
[[nodiscard]] inline size_t standardFreshStringHeapUpper(size_t count) {
    const auto proxy = standardContainerProxyBytes();
#if defined(_MSVC_STL_VERSION)
    if (count <= 15U) return proxy;
    const auto bytes = addStandardHeapBytes(count | size_t{15U}, 1U);
#elif defined(__GLIBCXX__) && _GLIBCXX_USE_CXX11_ABI
    if (count <= 15U) return proxy;
    const auto bytes = addStandardHeapBytes(count, 1U);
#elif defined(_LIBCPP_VERSION)
    if (count <= sizeof(std::string) - 2U) return proxy;
    const auto bytes = addStandardHeapBytes(addStandardHeapBytes(count, count), 1U);
#else
    const size_t bytes = 0U;
    throw std::logic_error("admitted string construction requires a supported standard string ABI");
#endif
    return addStandardHeapBytes(proxy, standardAllocatorRequestBytes(bytes, alignof(char)));
}

template<class T>
[[nodiscard]] inline size_t standardVectorHeapBufferBytes(const std::vector<T> &values) {
    if (values.capacity() > (std::numeric_limits<size_t>::max)() / sizeof(T))
        throw std::overflow_error("standard vector heap size overflowed");
    return addStandardHeapBytes(standardContainerProxyBytes(),
        standardAllocatorRequestBytes(values.capacity() * sizeof(T), alignof(T)));
}

// Fresh default map followed by reserve(count), before any insertion. Supported
// STLs use prime/power-of-two buckets below 2*count+1 (MSVC minimum eight).
// This is a bound for the visible fresh table, not an opaque adopted capacity.
template<class Key, class Value>
[[nodiscard]] inline size_t standardFreshUnorderedMapHeapUpper(size_t count) {
    // Keep default-load-factor arithmetic exact even in float-based STLs.
    if (count > (size_t{1U} << 24U)) throw std::length_error("fresh map admission exceeds its element bound");
    using Entry = std::pair<const Key, Value>;
    constexpr size_t nodeBytes = sizeof(Entry) + 2U * sizeof(void *) + alignof(Entry) - 1U;
    const auto bucketCount = (std::max)(size_t{8U}, addStandardHeapBytes(addStandardHeapBytes(count, count), 1U));
#if defined(_MSVC_STL_VERSION)
    const auto nodes = addStandardHeapBytes(count, 1U);
    constexpr size_t bucketWidth = 2U * sizeof(void *);
    constexpr size_t proxies = 2U * standardContainerProxyBytes();
#elif defined(__GLIBCXX__) || defined(_LIBCPP_VERSION)
    const auto nodes = count;
    constexpr size_t bucketWidth = sizeof(void *);
    constexpr size_t proxies = 0U;
#else
    throw std::logic_error("fresh map admission requires a supported standard container ABI");
    const auto nodes = count;
    constexpr size_t bucketWidth = sizeof(void *), proxies = 0U;
#endif
    const auto nodeRequest = standardAllocatorRequestBytes(nodeBytes, alignof(Entry));
    if (nodes > (std::numeric_limits<size_t>::max)() / nodeRequest ||
        bucketCount > (std::numeric_limits<size_t>::max)() / bucketWidth)
        throw std::overflow_error("fresh map request overflowed");
    return addStandardHeapBytes(proxies, addStandardHeapBytes(
        nodes * nodeRequest,
        standardAllocatorRequestBytes(bucketCount * bucketWidth, alignof(void *))));
}

// Structural storage only; callers account key/value heap payloads separately.
// The default allocator is intentional: PMR containers account their requests
// through their owning resource instead of this observed-capacity model.
template<class Key, class Value, class Hash, class Equal>
[[nodiscard]] inline size_t standardUnorderedMapStructuralHeapBytes(
        const std::unordered_map<Key, Value, Hash, Equal> &map) {
    using Entry = typename std::unordered_map<Key, Value, Hash, Equal>::value_type;
    constexpr size_t nodeBytes = sizeof(Entry) + 2U * sizeof(void *) + alignof(Entry) - 1U;
#if defined(_MSVC_STL_VERSION)
    const size_t nodes = addStandardHeapBytes(map.size(), 1U);
    constexpr size_t bucketWidth = 2U * sizeof(void *);
    constexpr size_t proxyBytes = 2U * standardContainerProxyBytes();
#else
    const size_t nodes = map.size();
    constexpr size_t bucketWidth = sizeof(void *);
    constexpr size_t proxyBytes = 0U;
#endif
    if (map.bucket_count() > (std::numeric_limits<size_t>::max)() / bucketWidth)
        throw std::overflow_error("standard map bucket heap size overflowed");
    const size_t buckets = standardAllocatorRequestBytes(map.bucket_count() * bucketWidth, alignof(void *));
    const size_t nodeRequest = standardAllocatorRequestBytes(nodeBytes, alignof(Entry));
    if (nodes > (std::numeric_limits<size_t>::max)() / nodeRequest)
        throw std::overflow_error("standard map node heap size overflowed");
    return addStandardHeapBytes(proxyBytes, addStandardHeapBytes(buckets, nodes * nodeRequest));
}

} // namespace graph::utils::detail
