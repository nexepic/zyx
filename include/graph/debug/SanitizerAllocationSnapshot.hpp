#pragma once
// Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
#include <cstddef>

#if defined(__clang__)
# if __has_feature(address_sanitizer) && (defined(__APPLE__) || defined(__linux__))
#  define ZYX_HAVE_ASAN_ALLOCATION_SNAPSHOT 1
#  if defined(__APPLE__)
#   define ZYX_ASAN_OPTIONAL_API __attribute__((weak_import))
#  else
#   define ZYX_ASAN_OPTIONAL_API __attribute__((weak))
#  endif
extern "C" std::size_t __sanitizer_get_current_allocated_bytes() ZYX_ASAN_OPTIONAL_API;
extern "C" std::size_t __sanitizer_get_heap_size() ZYX_ASAN_OPTIONAL_API;
#  undef ZYX_ASAN_OPTIONAL_API
# endif
#endif

namespace graph::debug {
// Optional allocator diagnostics only: never substitute these for real RSS or
// admission decisions. Weak imports keep older sanitizer runtimes usable.
struct SanitizerAllocationSnapshot {
    bool available = false;
    std::size_t live_bytes = 0U;
    std::size_t heap_bytes = 0U;
};
// Internal linkage is intentional: mixed instrumented/uninstrumented clients
// must not select another translation unit's inline feature-dependent body.
static inline SanitizerAllocationSnapshot captureSanitizerAllocations() noexcept {
#if defined(ZYX_HAVE_ASAN_ALLOCATION_SNAPSHOT)
        if (__sanitizer_get_current_allocated_bytes && __sanitizer_get_heap_size)
            return {true, __sanitizer_get_current_allocated_bytes(), __sanitizer_get_heap_size()};
#endif
        return {};
}
}
#undef ZYX_HAVE_ASAN_ALLOCATION_SNAPSHOT
