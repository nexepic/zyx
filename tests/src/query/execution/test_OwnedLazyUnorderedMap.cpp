/**
 * @file test_OwnedLazyUnorderedMap.cpp
 * @brief Cold-state, captured-owner and failure-refund contracts.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include "graph/utils/OwnedLazyUnorderedMap.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
namespace graph::utils {
namespace {
using query::execution::MaterializedByteBudget;
using query::execution::MaterializedByteLimitExceeded;
using Map = OwnedLazyUnorderedMap<uint64_t, uint64_t>;
TEST(OwnedLazyUnorderedMapTest, ColdReadsAndEmptyClearRequireNoAdmission) {
    auto memory = std::make_shared<MaterializedByteBudget>(1U);
    OwnedAllocationScope scope(memory);
    Map values;
    EXPECT_TRUE(values.empty()); EXPECT_EQ(values.begin(), values.end());
    EXPECT_EQ(values.find(1U), values.end()); EXPECT_FALSE(values.contains(1U));
    const auto &read = values;
    EXPECT_EQ(read.begin(), read.end()); EXPECT_EQ(read.find(1U), read.end());
    values.reserve(0U); values.clear(); EXPECT_EQ(memory->used(), 0U);
    EXPECT_THROW(values.emplace(1U, 2U), MaterializedByteLimitExceeded);
    EXPECT_TRUE(values.empty()); EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedLazyUnorderedMapTest, FailedInitialReservePublishesNoStateAndRefundsEveryRequest) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024U);
    OwnedAllocationScope scope(memory);
    Map values;
    EXPECT_THROW(values.reserve(1024U * 1024U), MaterializedByteLimitExceeded);
    EXPECT_TRUE(values.empty()); EXPECT_EQ(values.begin(), values.end());
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_TRUE(values.emplace(1U, 2U).second);
    EXPECT_EQ(values.find(1U)->second, 2U);
    values.clear(); EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedLazyUnorderedMapTest, MutationsKeepTheirOriginatingAuthorityAndClearReleasesTheWholeState) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536U);
    std::weak_ptr<std::pmr::memory_resource> authority = memory;
    Map values;
    {
        OwnedAllocationScope scope(memory);
        values.emplace(1U, 2U);
    }
    const auto before = memory->used(); EXPECT_GT(before, 0U);
    auto other = std::make_shared<MaterializedByteBudget>(1U);
    {
        OwnedAllocationScope scope(other);
        values.reserve(128U); values.emplace(3U, 4U);
    }
    EXPECT_EQ(other->used(), 0U); EXPECT_GT(memory->used(), before);
    memory.reset(); EXPECT_FALSE(authority.expired());
    values.erase(values.find(1U)); EXPECT_FALSE(values.contains(1U));
    EXPECT_TRUE(values.contains(3U));
    values.clear(); EXPECT_TRUE(authority.expired()); EXPECT_EQ(values.begin(), values.end());
}
TEST(OwnedLazyUnorderedMapTest, MovesTransferTheWholeStateAndLeaveAReusableColdSource) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536U);
    OwnedAllocationScope scope(memory);
    Map first; first.emplace(1U, 2U);
    const auto before = memory->used();
    Map second(std::move(first));
    EXPECT_EQ(memory->used(), before); EXPECT_TRUE(first.empty());
    EXPECT_EQ(second.find(1U)->second, 2U);
    first.emplace(3U, 4U); EXPECT_GT(memory->used(), before);
    first.clear(); EXPECT_EQ(memory->used(), before);
    second.clear(); EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedLazyUnorderedMapTest, HeterogeneousLookupUsesThePublishedHashState) {
    struct SaltedHash {
        using is_transparent = void;
        static size_t nextSeed() { static size_t value = 0U; return ++value; }
        size_t seed = nextSeed();
        size_t operator()(std::string_view key) const { return std::hash<std::string_view>{}(key) ^ seed; }
    };
    auto memory = std::make_shared<MaterializedByteBudget>(65536U);
    OwnedAllocationScope scope(memory);
    OwnedLazyUnorderedMap<std::string, uint64_t, SaltedHash, std::equal_to<>> values;
    values.emplace("owned-key", 42U);
    const std::string_view key = "owned-key";
    ASSERT_NE(values.find(key), values.end());
    EXPECT_EQ(values.find(key)->second, 42U);
    const auto &read = values;
    ASSERT_NE(read.find(key), read.end());
    EXPECT_EQ(read.find(key)->second, 42U);
    values.clear(); EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedLazyUnorderedMapTest, EveryColdAllocationFailureUnwindsAndAllowsRetry) {
    class FaultResource final : public std::pmr::memory_resource {
    public:
        size_t calls = 0U, fail_at = (std::numeric_limits<size_t>::max)(), live = 0U;
    private:
        void *do_allocate(size_t bytes, size_t alignment) override {
            if (calls++ == fail_at) throw MaterializedByteLimitExceeded();
            void *p = std::pmr::new_delete_resource()->allocate(bytes, alignment);
            live += bytes; return p;
        }
        void do_deallocate(void *p, size_t bytes, size_t alignment) override {
            std::pmr::new_delete_resource()->deallocate(p, bytes, alignment); live -= bytes;
        }
        bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
    };
    auto observed = std::make_shared<FaultResource>();
    {
        OwnedAllocationScope scope(observed);
        Map values; values.emplace(1U, 2U);
    }
    ASSERT_EQ(observed->live, 0U);
    ASSERT_GT(observed->calls, 0U);
    for (size_t failure = 0; failure < observed->calls; ++failure) {
        SCOPED_TRACE(failure);
        auto memory = std::make_shared<FaultResource>(); memory->fail_at = failure;
        OwnedAllocationScope scope(memory);
        Map values;
        EXPECT_THROW(values.emplace(1U, 2U), MaterializedByteLimitExceeded);
        EXPECT_EQ(memory->calls, failure + 1U);
        EXPECT_EQ(memory->live, 0U);
        EXPECT_TRUE(values.empty());
        memory->fail_at = (std::numeric_limits<size_t>::max)();
        ASSERT_TRUE(values.emplace(1U, 2U).second);
        EXPECT_EQ(values.find(1U)->second, 2U);
        values.clear(); EXPECT_EQ(memory->live, 0U);
    }
}
} // namespace
} // namespace graph::utils
