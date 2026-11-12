#pragma once
// Test-process ABI measurements only; no database values or allocation policy.
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <memory_resource>
#include <unordered_map>
#include "graph/utils/StandardContainerHeapBytes.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/utils/OwnedVector.hpp"

namespace graph::test::ci_diagnostics {
// Keep the diagnostic frame out of normal/ASan startup when the flag is absent.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
inline void traceLayout() noexcept {
    if (!std::getenv("ZYX_CI_LAYOUT_TRACE")) return;
    using namespace graph::query;
    using namespace graph::query::execution;
    std::fprintf(stderr,
        "CI_LAYOUT pointer=%zu string=%zu property_value=%zu property_map=%zu read_value=%zu read_row=%zu result_value=%zu result_row=%zu result_pair=%zu result_pair_alignment=%zu owning_byte_vector=%zu\n",
        sizeof(void *), sizeof(std::string), sizeof(PropertyValue), sizeof(PropertyValue::MapType),
        sizeof(LogicalPlanReadValue), sizeof(LogicalPlanReadRow), sizeof(ResultValue),
        sizeof(QueryResult::Row), sizeof(QueryResult::Row::value_type), alignof(QueryResult::Row::value_type),
        sizeof(graph::utils::OwnedVector<std::byte>));
    // A layout-compatible inert mapped slot isolates hash node/bucket/proxy
    // requests from dynamic value allocations. PMR sees the actual allocator
    // requests; the separate default-map model includes no artificial owner.
    // Short keys' non-PMR string proxies are deliberately outside this count.
    struct alignas(alignof(ResultValue)) Slot { std::byte bytes[sizeof(ResultValue)]; };
    using ReferenceMap = std::unordered_map<std::string, Slot>;
    static_assert(sizeof(ReferenceMap::value_type) == sizeof(QueryResult::Row::value_type));
    static_assert(alignof(ReferenceMap::value_type) == alignof(QueryResult::Row::value_type));
    struct Requests final : std::pmr::memory_resource {
        size_t live = 0U, allocations = 0U, largest = 0U;
        void *do_allocate(size_t bytes, size_t alignment) override {
            void *pointer = std::pmr::new_delete_resource()->allocate(bytes, alignment);
            live += bytes; ++allocations; largest = (std::max)(largest, bytes);
            return pointer;
        }
        void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
            live -= bytes; std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        }
        bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
    } requests;
    try {
        for (const size_t count : {1U, 2U}) {
            requests.allocations = requests.largest = 0U;
            ReferenceMap reference;
            std::pmr::unordered_map<std::string, Slot> actual(&requests);
            reference.reserve(count); actual.reserve(count);
            for (size_t index = 0U; index < count; ++index) {
                const char *key = index == 0U ? "a" : "b";
                reference.emplace(key, Slot{}); actual.emplace(key, Slot{});
            }
            std::fprintf(stderr,
                "CI_MAP_LAYOUT entries=%zu pair=%zu alignment=%zu buckets=%zu actual_pmr_structural=%zu model_upper=%zu allocation_requests=%zu largest_request=%zu value_heap_excluded=1\n",
                count, sizeof(ReferenceMap::value_type), alignof(ReferenceMap::value_type), actual.bucket_count(),
                requests.live, graph::utils::detail::standardUnorderedMapStructuralHeapBytes(reference),
                requests.allocations, requests.largest);
            if (count == 1U) {
                // Only simultaneously retained final structures: exclude all
                // key/value payloads, source frames and transient copies.
                constexpr size_t rows = 300U, maximum = 256U * 1024U;
                const size_t array = rows * sizeof(QueryResult::Row);
                const size_t floor = array + rows * requests.live;
                std::fprintf(stderr,
                    "CI_RESULT_STRUCTURAL_FLOOR rows=%zu columns=1 map_requests=%zu array_raw=%zu lower_bound=%zu maximum=%zu fits=%d value_heap_excluded=1\n",
                    rows, requests.live, array, floor, maximum, floor <= maximum);
            }
        }
        std::fprintf(stderr, "CI_MAP_LAYOUT_FINAL live=%zu\n", requests.live);
    } catch (...) { std::fputs("CI_MAP_LAYOUT measurement_unavailable\n", stderr); }
    std::fflush(stderr);
}
}
#endif
