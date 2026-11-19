#include <gtest/gtest.h>
#include "query/execution/ResultPropertyMapAdmission.hpp"
#include "graph/core/GuardedPropertyCopy.hpp"
#include "graph/core/PropertyHeapRequestBytes.hpp"

namespace graph::query::execution::detail {
namespace {
struct MapRequests {
    inline static thread_local size_t live = 0U;
};
template<class T> struct MapRequestAllocator {
    using value_type = T;
    MapRequestAllocator() = default;
    template<class U> MapRequestAllocator(const MapRequestAllocator<U> &) {}
    T *allocate(size_t count) {
        T *result = std::allocator<T>{}.allocate(count);
        MapRequests::live += count * sizeof(T);
        return result;
    }
    void deallocate(T *pointer, size_t count) {
        MapRequests::live -= count * sizeof(T);
        std::allocator<T>{}.deallocate(pointer, count);
    }
    template<class U> bool operator==(const MapRequestAllocator<U> &) const noexcept { return true; }
};
struct Entry { std::string key; PropertyValue value; };
}
TEST(ResultPropertyMapAdmissionTest, FreshNumericMapAdmitsObservedBucketAndNodeRequests) {
    using Pair = std::pair<const std::string, PropertyValue>;
    using Map = std::unordered_map<std::string, PropertyValue, std::hash<std::string>,
        std::equal_to<std::string>, MapRequestAllocator<Pair>>;
    for (const size_t count : {0U, 1U, 8U}) {
        SCOPED_TRACE(count);
        std::vector<Entry> entries;
        for (size_t i = 0U; i < count; ++i)
            entries.push_back({std::to_string(i), PropertyValue(static_cast<int64_t>(i))});
        const size_t upper = freshResultEntryMapHeapUpper(entries, [] {});
        ASSERT_EQ(MapRequests::live, 0U);
        {
            Map map;
            map.reserve(entries.size());
            for (const auto &entry : entries) map.emplace(entry.key, entry.value);
            // Independent requests from the actual STL allocator, not a
            // reconstruction of the implementation's bucket/node formula.
            EXPECT_GE(upper, MapRequests::live);
            EXPECT_LE(upper, 2U * MapRequests::live + 64U);
        }
        EXPECT_EQ(MapRequests::live, 0U);
    }
}
TEST(ResultPropertyMapAdmissionTest, CancellationLeavesSourceAndAllocatorUntouched) {
    const std::vector<Entry> entries{{"payload", PropertyValue(std::string(5000U, 'p'))}};
    const char *owner = std::get<std::string>(entries.front().value.getVariant()).data();
    size_t checks = 0U;
    EXPECT_THROW(static_cast<void>(freshResultEntryMapHeapUpper(entries, [&] {
        ++checks; throw std::domain_error("cancelled result admission");
    })), std::domain_error);
    EXPECT_EQ(checks, 1U);
    EXPECT_EQ(std::get<std::string>(entries.front().value.getVariant()).data(), owner);
    EXPECT_EQ(std::get<std::string>(entries.front().value.getVariant()), std::string(5000U, 'p'));
    EXPECT_EQ(MapRequests::live, 0U);
}
TEST(ResultPropertyMapAdmissionTest, GuardedCopyCanGrowAnAdoptedMapBeyondItsSourceLayout) {
    PropertyValue::MapType adopted;
    adopted.max_load_factor(16.0F);
    adopted.rehash(1U);
    for (int64_t i = 0; i < 8; ++i) adopted.emplace(std::to_string(i), PropertyValue(i));
    const PropertyValue source(std::move(adopted));
    const size_t before = property_heap::retainedRequestUpper(source);
    const size_t upper = storage::CanonicalPropertyValueCodec::guardedCopyHeapUpper(source, [] {});
    const PropertyValue copy = graph::property_copy::copyPropertyValueGuarded(source, [] {});
    // The actual copied hash table, rather than a duplicated prediction model,
    // proves that source capacity alone cannot fund a fresh guarded copy.
    EXPECT_GT(copy.getMap().bucket_count(), source.getMap().bucket_count());
    EXPECT_GT(property_heap::retainedRequestUpper(copy), before);
    EXPECT_GE(upper, property_heap::retainedRequestUpper(copy));
    EXPECT_EQ(copy, source);
    EXPECT_NE(&copy.getMap().at("0"), &source.getMap().at("0"));
}

} // namespace graph::query::execution::detail
