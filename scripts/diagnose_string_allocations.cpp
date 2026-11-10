// Standalone diagnostic: measure fresh std::string requests independently of
// the production admission estimate. Run as a separate process.
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <array>
#include <unordered_map>
#include "../include/graph/utils/StandardContainerHeapBytes.hpp"

namespace {
bool observing = false;
std::size_t requested = 0;
std::size_t allocations = 0;
struct Request { void *pointer = nullptr; std::size_t bytes = 0; };
std::array<Request, 4096> liveRequests{};
bool trackLive = false, trackingOverflow = false;
std::size_t liveBytes = 0, peakBytes = 0;
}

void *operator new(std::size_t bytes) {
    if (observing) {
        requested += bytes;
        ++allocations;
    }
    if (void *value = std::malloc(bytes)) {
        if (observing && trackLive) {
            bool recorded = false;
            for (auto &entry : liveRequests) if (!entry.pointer) {
                entry = {value, bytes}; recorded = true; break;
            }
            if (!recorded) trackingOverflow = true;
            liveBytes += bytes;
            if (liveBytes > peakBytes) peakBytes = liveBytes;
        }
        return value;
    }
    throw std::bad_alloc();
}
void operator delete(void *value) noexcept {
    if (trackLive) for (auto &entry : liveRequests) if (entry.pointer == value && value) {
        liveBytes -= entry.bytes; entry = {}; break;
    }
    std::free(value);
}
void operator delete(void *value, std::size_t) noexcept { ::operator delete(value); }

template<std::size_t Width>
bool checkMapRequests() {
    // Match the observed eight-byte value alignment without using the
    // production value/measurement implementation to count actual requests.
    struct alignas(8) Value { std::array<std::byte, Width> bytes{}; };
    std::array<std::string, 256> keys;
    for (std::size_t i = 0; i < keys.size(); ++i) keys[i] = "key" + std::to_string(i);
    for (const std::size_t count : {0U, 1U, 3U, 16U, 256U}) {
        requested = allocations = liveBytes = peakBytes = 0;
        trackingOverflow = false; trackLive = observing = true;
        bool valid = false;
        {
            std::unordered_map<std::string, Value> map;
            map.reserve(count);
            for (std::size_t i = 0; i < count; ++i) map.emplace(keys[i], Value{});
            observing = false;
            auto upper = graph::utils::detail::standardUnorderedMapStructuralHeapBytes(map);
            auto fresh = graph::utils::detail::standardFreshUnorderedMapHeapUpper<std::string, Value>(count);
            for (std::size_t i = 0; i < count; ++i) {
                const auto key = graph::utils::detail::standardFreshStringHeapUpper(keys[i].size());
                upper += key; fresh += key;
            }
            std::printf("MAP_ALLOC value_width=%zu alignment=%zu entries=%zu buckets=%zu live=%zu peak=%zu retained_upper=%zu fresh_upper=%zu calls=%zu overflow=%d\n",
                sizeof(Value), alignof(Value), count, map.bucket_count(), liveBytes, peakBytes, upper, fresh, allocations, trackingOverflow);
            valid = !trackingOverflow && liveBytes <= upper && peakBytes <= fresh && (count == 0 || allocations > 0);
        }
        trackLive = false;
        if (!valid || liveBytes != 0) return false;
    }
    return true;
}

int main() {
    static char input[1024U * 1024U]{};
    std::printf("sizeof_string=%zu", sizeof(std::string));
#ifdef _LIBCPP_VERSION
    std::printf(" libcxx=%d", _LIBCPP_VERSION);
#endif
#ifdef _MSVC_STL_VERSION
    std::printf(" msvc_stl=%d iterator_debug=%d", _MSVC_STL_VERSION, _ITERATOR_DEBUG_LEVEL);
#endif
#ifdef _MSVC_STL_UPDATE
    std::printf(" msvc_stl_update=%ld", static_cast<long>(_MSVC_STL_UPDATE));
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
    if (!checkMapRequests<88>() || !checkMapRequests<104>() || !checkMapRequests<352>()) return 3;
}
