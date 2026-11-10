#include <gtest/gtest.h>
#include <memory>
#include "query/execution/ConsumedPrefixCompaction.hpp"

using namespace graph;
using namespace graph::query::execution;
using graph::query::execution::detail::compactConsumedPrefix;
namespace {
class TrackingBudget final : public std::pmr::memory_resource {
public:
    explicit TrackingBudget(size_t maximum) : budget_(maximum) {}
    TrackingBudget(size_t maximum, std::shared_ptr<graph::query::execution::detail::ResourceAccount> account)
        : budget_(maximum, std::move(account)) {}
    size_t used() const { return budget_.used(); }
    size_t peak() const { return peak_; }
    bool fail_next = false;
private:
    void *do_allocate(size_t bytes, size_t alignment) override {
        if (std::exchange(fail_next, false)) throw std::bad_alloc();
        void *result = budget_.allocate(bytes, alignment);
        peak_ = (std::max)(peak_, budget_.used());
        return result;
    }
    void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
        budget_.deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
    MaterializedByteBudget budget_;
    size_t peak_ = 0U;
};
}

TEST(ConsumedPrefixCompactionTest, ChargesOverlappingBuffersAndPreservesPayloadIdentity) {
    using Row = std::unique_ptr<int>;
    auto budget = std::make_shared<TrackingBudget>(6U * sizeof(Row));
    {
        utils::OwnedVector<Row> rows{utils::OwnedPmrAllocator<Row>(budget)};
        rows.reserve(4U);
        for (int id = 0; id < 4; ++id) rows.push_back(std::make_unique<int>(id));
        auto *third = rows[2].get(); auto *fourth = rows[3].get();
        rows[0].reset(); rows[1].reset();
        size_t first = 2U;
        EXPECT_FALSE(compactConsumedPrefix(rows, first, 2U * sizeof(Row) - 1U));
        EXPECT_EQ(first, 2U); EXPECT_EQ(rows.capacity(), 4U);
        ASSERT_TRUE(compactConsumedPrefix(rows, first, 2U * sizeof(Row)));
        EXPECT_EQ(first, 0U); ASSERT_EQ(rows.size(), 2U);
        EXPECT_EQ(rows.capacity(), 2U);
        EXPECT_EQ(rows[0].get(), third); EXPECT_EQ(rows[1].get(), fourth);
        EXPECT_EQ(budget->peak(), 6U * sizeof(Row));
        EXPECT_EQ(budget->used(), 2U * sizeof(Row));
    }
    EXPECT_EQ(budget->used(), 0U);
}

TEST(ConsumedPrefixCompactionTest, AllocatorRejectionLeavesSourceAndRetryIntact) {
    auto budget = std::make_shared<MaterializedByteBudget>(6U * sizeof(int) - 1U);
    {
        utils::OwnedVector<int> rows{utils::OwnedPmrAllocator<int>(budget)};
        rows.reserve(4U); for (int id = 0; id < 4; ++id) rows.push_back(id);
        size_t first = 2U;
        EXPECT_FALSE(compactConsumedPrefix(rows, first, 2U * sizeof(int)));
        EXPECT_EQ(first, 2U); EXPECT_EQ(rows.size(), 4U);
        EXPECT_EQ(rows[2], 2); EXPECT_EQ(rows[3], 3);
        EXPECT_EQ(budget->used(), 4U * sizeof(int));
        // No payload was moved before reserve failed; the original path remains usable.
        rows.erase(rows.begin(), rows.begin() + 2);
        EXPECT_EQ(rows[0], 2); EXPECT_EQ(rows[1], 3);
    }
    EXPECT_EQ(budget->used(), 0U);
}

TEST(ConsumedPrefixCompactionTest, GeometricCompactionBoundsTotalMoves) {
    struct Row {
        int id; size_t *moves;
        Row(int value, size_t &counter) : id(value), moves(&counter) {}
        Row(Row &&other) noexcept : id(other.id), moves(other.moves) { ++*moves; }
    };
    utils::OwnedVector<Row> rows; rows.reserve(64U);
    size_t moves = 0U, first = 0U;
    for (int id = 0; id < 64; ++id) rows.emplace_back(id, moves);
    for (int expected = 0; expected < 64; ++expected, ++first) {
        compactConsumedPrefix(rows, first, SIZE_MAX);
        ASSERT_EQ(rows[first].id, expected);
    }
    EXPECT_GT(moves, 0U);
    EXPECT_LT(moves, 3U * 64U);
}

TEST(ConsumedPrefixCompactionTest, ReclaimsBackingBeforeRetainedOutputsConsumeTheTemporaryHeadroom) {
    auto budget = std::make_shared<TrackingBudget>(16U * sizeof(int));
    void *output = budget->allocate(2U * sizeof(int), alignof(int));
    {
        utils::OwnedVector<int> rows{utils::OwnedPmrAllocator<int>(budget)};
        rows.reserve(8U); for (int id = 0; id < 8; ++id) rows.push_back(id);
        size_t first = 2U;
        ASSERT_TRUE(compactConsumedPrefix(rows, first, 6U * sizeof(int)));
        EXPECT_EQ(budget->peak(), 16U * sizeof(int));
        EXPECT_EQ(budget->used(), 8U * sizeof(int));
        ASSERT_EQ(rows.size(), 6U); EXPECT_EQ(rows.front(), 2); EXPECT_EQ(rows.back(), 7);
    }
    EXPECT_EQ(budget->used(), 2U * sizeof(int));
    budget->deallocate(output, 2U * sizeof(int), alignof(int));
    EXPECT_EQ(budget->used(), 0U);
}

TEST(ConsumedPrefixCompactionTest, AllocationFailureFallsBackWithoutMovingAndAllowsRetry) {
    auto budget = std::make_shared<TrackingBudget>(6U * sizeof(int));
    {
        utils::OwnedVector<int> rows{utils::OwnedPmrAllocator<int>(budget)};
        rows.reserve(4U); for (int id = 0; id < 4; ++id) rows.push_back(id);
        size_t first = 2U; budget->fail_next = true;
        EXPECT_FALSE(compactConsumedPrefix(rows, first, 2U * sizeof(int)));
        EXPECT_FALSE(budget->fail_next);
        EXPECT_EQ(first, 2U); EXPECT_EQ(rows[2], 2); EXPECT_EQ(rows[3], 3);
        EXPECT_EQ(budget->used(), 4U * sizeof(int));
        EXPECT_TRUE(compactConsumedPrefix(rows, first, 2U * sizeof(int)));
        EXPECT_EQ(first, 0U); EXPECT_EQ(rows[0], 2); EXPECT_EQ(rows[1], 3);
    }
    EXPECT_EQ(budget->used(), 0U);
}

TEST(ConsumedPrefixCompactionTest, HierarchicalQuotaRejectionRefundsAndCanRetryAfterCompetingOwnerReleases) {
    using namespace graph::query::execution::detail;
    constexpr size_t limit = 6U * sizeof(int);
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(limit));
    auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(limit));
    auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, limit);
    auto competingAccount = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, limit);
    ResourceLease competing(competingAccount); competing.set(1U);
    auto budget = std::make_shared<TrackingBudget>(limit, account);
    {
        utils::OwnedVector<int> rows{utils::OwnedPmrAllocator<int>(budget)};
        rows.reserve(4U); for (int id = 0; id < 4; ++id) rows.push_back(id);
        size_t first = 2U;
        EXPECT_FALSE(compactConsumedPrefix(rows, first, 2U * sizeof(int)));
        EXPECT_EQ(first, 2U); EXPECT_EQ(rows[2], 2); EXPECT_EQ(rows[3], 3);
        EXPECT_EQ(account->used(), 4U * sizeof(int)); EXPECT_EQ(competingAccount->used(), 1U);
        competing.reset();
        EXPECT_TRUE(compactConsumedPrefix(rows, first, 2U * sizeof(int)));
        EXPECT_EQ(first, 0U); EXPECT_EQ(rows[0], 2); EXPECT_EQ(rows[1], 3);
        EXPECT_EQ(account->used(), 2U * sizeof(int));
        EXPECT_EQ(budget->peak(), limit);
    }
    EXPECT_EQ(budget->used(), 0U); EXPECT_EQ(account->used(), 0U);
    EXPECT_EQ(competingAccount->used(), 0U);
}

TEST(ConsumedPrefixCompactionTest, PotentiallyThrowingMovesKeepOriginalStorage) {
    struct Row {
        explicit Row(int id) : id(id) {}
        Row(Row &&other) noexcept(false) : id(other.id) { throw std::runtime_error("move"); }
        Row(const Row &) = default;
        int id;
    };
    utils::OwnedVector<Row> rows; rows.reserve(2U); rows.emplace_back(1); rows.emplace_back(2);
    size_t first = 1U;
    EXPECT_FALSE(compactConsumedPrefix(rows, first, SIZE_MAX));
    EXPECT_EQ(first, 1U); EXPECT_EQ(rows[1].id, 2);
}
