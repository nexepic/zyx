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
} // namespace
} // namespace graph::utils
