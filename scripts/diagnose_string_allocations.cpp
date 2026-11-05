// Standalone diagnostic: measure fresh std::string requests independently of
// the production admission estimate. Run as a separate process.
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include "../include/graph/utils/StandardContainerHeapBytes.hpp"

namespace {
bool observing = false;
std::size_t requested = 0;
std::size_t allocations = 0;
}

void *operator new(std::size_t bytes) {
    if (observing) {
        requested += bytes;
        ++allocations;
    }
    if (void *value = std::malloc(bytes)) return value;
    throw std::bad_alloc();
}
void operator delete(void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }

int main() {
    static char input[1024U * 1024U]{};
    std::printf("sizeof_string=%zu", sizeof(std::string));
#ifdef _LIBCPP_VERSION
    std::printf(" libcxx=%d", _LIBCPP_VERSION);
#endif
#ifdef _MSVC_STL_VERSION
    std::printf(" msvc_stl=%d iterator_debug=%d", _MSVC_STL_VERSION, _ITERATOR_DEBUG_LEVEL);
#endif
    std::printf("\n");
    for (const std::size_t length : {std::size_t{22}, std::size_t{23}, std::size_t{512}, std::size_t{8192}}) {
        requested = allocations = 0;
        observing = true;
        std::string value{std::string_view(input, length)};
        observing = false;
        std::printf("length=%zu allocations=%zu requested=%zu capacity=%zu\n",
                    length, allocations, requested, value.capacity());
        if (length == 8192 && allocations == 0)
            std::printf("measurement_unavailable: allocator interposition was not observed\n");
        if (value.size() != length) return 1;
    }
    std::size_t maxExtra = 0, maxExtraLength = 0, multipleAllocationLengths = 0;
    constexpr std::size_t exhaustiveMaximum = 65536U;
    for (std::size_t length = 0; length <= exhaustiveMaximum; ++length) {
        requested = allocations = 0;
        observing = true;
        std::string value{std::string_view(input, length)};
        observing = false;
        if (value.size() != length) return 1;
        if (allocations > 1) ++multipleAllocationLengths;
        if (requested > length && requested - length > maxExtra) {
            maxExtra = requested - length;
            maxExtraLength = length;
        }
    }
    std::printf("range=0..%zu max_requested_minus_length=%zu at_length=%zu multiple_allocation_lengths=%zu\n",
                exhaustiveMaximum, maxExtra, maxExtraLength, multipleAllocationLengths);
    std::size_t checked = 0;
    const auto check = [&](std::size_t length) {
        const auto upper = graph::utils::detail::standardFreshStringHeapUpper(length);
        for (unsigned mode = 0; mode < 3; ++mode) {
            // Prepare source outside the observed interval. Copy from a source
            // with spare capacity as well as count and string-view constructors.
            std::string source(input, length);
            source.reserve(length + 128U);
            requested = allocations = 0;
            observing = true;
            std::string value = mode == 0 ? std::string(std::string_view(input, length))
                : mode == 1 ? std::string(length, 'x') : std::string(source);
            observing = false;
            ++checked;
            if (value.size() != length || requested > upper || (length >= 8192U && allocations == 0U)) {
                std::fprintf(stderr, "STRING_BOUND_FAILURE length=%zu mode=%u requested=%zu upper=%zu allocations=%zu\n",
                    length, mode, requested, upper, allocations);
                return false;
            }
        }
        return true;
    };
    for (std::size_t length = 0; length <= exhaustiveMaximum; ++length) if (!check(length)) return 2;
    for (std::size_t boundary = 131072U; boundary <= sizeof(input); boundary *= 2U)
        for (std::size_t delta = 0; delta <= 2U; ++delta)
            if (!check(boundary - delta)) return 2;
    std::printf("fresh_string_upper_validated constructions=%zu maximum=%zu modes=3\n", checked, sizeof(input));
}
