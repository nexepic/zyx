#include <gtest/gtest.h>
#include <array>
#include "query/execution/TrivialBudgetBuffer.hpp"
#include "query/execution/BudgetMemoryResource.hpp"

namespace graph::query::execution::detail {
namespace {
struct BufferRequests final : std::pmr::memory_resource {
    size_t calls = 0U, live = 0U;
    bool deny = false;
    void *do_allocate(size_t n, size_t alignment) override {
        ++calls;
        if (deny) throw std::bad_alloc();
        void *p = std::pmr::new_delete_resource()->allocate(n, alignment);
        live += n; return p;
    }
    void do_deallocate(void *p, size_t n, size_t alignment) override {
        live -= n; std::pmr::new_delete_resource()->deallocate(p, n, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};
}
TEST(TrivialBudgetBufferTest, ConstructionAndReleaseNeverRequestHiddenStorage) {
    MaterializedByteBudget budget(64U); BufferRequests upstream;
    BudgetMemoryResource memory(budget, &upstream);
    {
        TrivialBudgetBuffer<uint64_t> values(&memory);
        EXPECT_EQ(upstream.calls, 0U); EXPECT_EQ(budget.used(), 0U);
        EXPECT_TRUE(std::span<const uint64_t>(values).empty());
        values.reserve(4U); values.push_back(7U);
        EXPECT_EQ(budget.used(), 32U);
        const size_t calls = upstream.calls; upstream.deny = true;
        values.release(); values.release();
        EXPECT_EQ(upstream.calls, calls); EXPECT_EQ(upstream.live, 0U);
        EXPECT_EQ(values.capacity(), 0U); EXPECT_EQ(budget.used(), 0U);
        upstream.deny = false; values.push_back(9U); EXPECT_EQ(values.at(0), 9U);
    }
    EXPECT_EQ(budget.used(), 0U); EXPECT_EQ(upstream.live, 0U);
}
TEST(TrivialBudgetBufferTest, RefusedGrowthKeepsPrefixAndChargesOldNewOverlap) {
    MaterializedByteBudget budget(39U); BufferRequests upstream;
    BudgetMemoryResource memory(budget, &upstream); TrivialBudgetBuffer<uint64_t> values(&memory);
    values.reserve(2U); values.push_back(11U); values.push_back(12U);
    EXPECT_THROW(values.reserve(3U), MaterializedByteLimitExceeded);
    EXPECT_EQ(values.size(), 2U); EXPECT_EQ(values.at(0), 11U); EXPECT_EQ(values.at(1), 12U);
    EXPECT_EQ(budget.used(), 16U); EXPECT_EQ(upstream.live, 16U);
    EXPECT_THROW(values.reserve(SIZE_MAX), std::length_error);
    values.clear(); EXPECT_EQ(budget.used(), 16U);
    upstream.deny = true;
    EXPECT_THROW(values.reserve(4U), MaterializedByteLimitExceeded);
    EXPECT_EQ(values.capacity(), 2U);
}
TEST(TrivialBudgetBufferTest, UpstreamFailureRefundsAdmissionAndAssignmentOwnsInput) {
    MaterializedByteBudget budget(1024U); BufferRequests upstream;
    BudgetMemoryResource memory(budget, &upstream); TrivialBudgetBuffer<uint64_t> values(&memory);
    std::array<uint64_t, 3U> input{1U, 2U, 3U};
    upstream.deny = true;
    EXPECT_THROW(values.assign(input.begin(), input.end()), std::bad_alloc);
    EXPECT_TRUE(values.empty()); EXPECT_EQ(budget.used(), 0U);
    upstream.deny = false; values.assign(input.begin(), input.end()); input.fill(99U);
    EXPECT_EQ(values.at(1), 2U);
    values.assign(values.begin() + 1U, values.end());
    EXPECT_EQ(values.size(), 2U); EXPECT_EQ(values.at(0), 2U); EXPECT_EQ(values.at(1), 3U);
    EXPECT_THROW(values.at(2U), std::out_of_range);
    EXPECT_THROW((TrivialBudgetBuffer<uint64_t>(nullptr)), std::invalid_argument);
}
TEST(TrivialBudgetBufferTest, EmptyAndAlignedRangesPreserveStorageAndValidateBounds) {
    struct alignas(64) Cell { uint64_t value = 0U; };
    MaterializedByteBudget budget(1024U); BufferRequests upstream;
    BudgetMemoryResource memory(budget, &upstream); TrivialBudgetBuffer<Cell> values(&memory);
    values.assign(values.begin(), values.end());
    EXPECT_EQ(upstream.calls, 0U);
    const std::array<Cell, 2U> input{Cell{7U}, Cell{8U}};
    values.assign(input.begin(), input.end());
    EXPECT_EQ(reinterpret_cast<uintptr_t>(values.data()) % alignof(Cell), 0U);
    EXPECT_EQ(budget.used(), 2U * sizeof(Cell));
    EXPECT_THROW(values.assign(input.end(), input.begin()), std::invalid_argument);
    EXPECT_EQ(values.at(1).value, 8U);
    values.resize(1U); EXPECT_EQ(values.at(0).value, 7U);
    values.resize(2U); EXPECT_EQ(values.at(1).value, 0U);
    values.release(); EXPECT_EQ(budget.used(), 0U);
}
}
