#include <gtest/gtest.h>
#include "query/execution/ScalarResultHandoff.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::execution;
using graph::query::execution::detail::ScalarResultHandoff;

TEST(ScalarResultHandoffTest, ExactAdmissionKeepsIntegerAndActualStorageCharged) {
    const std::string column = "count";
    const auto upper = ScalarResultHandoff::integerRowUpper(column);
    MaterializedByteBudget budget(upper + utils::detail::standardContainerProxyBytes());
    QueryResult result;
    auto charge = ScalarResultHandoff::appendFirstInteger(result, column, 42, budget, [] {});
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows().front().at(column).asPrimitive(), PropertyValue(int64_t{42}));
    const auto actual = utils::detail::standardVectorHeapBufferBytes(result.getRows()) -
        utils::detail::standardContainerProxyBytes() + graph::query::detail::resultRowHeapBytes(result.getRows().front());
    EXPECT_GE(charge.bytes(), actual);
    EXPECT_EQ(budget.used(), charge.bytes());
    result = QueryResult{};
    charge.reset();
    EXPECT_EQ(budget.used(), 0U);
}

TEST(ScalarResultHandoffTest, UnderfundedDestinationStaysEmptyAndCanRetry) {
    const std::string column = "count";
    const size_t peak = ScalarResultHandoff::integerRowUpper(column) + utils::detail::standardContainerProxyBytes();
    MaterializedByteBudget budget(peak);
    MaterializedByteReservation competitor(&budget); competitor.set(1U);
    QueryResult result;
    EXPECT_THROW(static_cast<void>(ScalarResultHandoff::appendFirstInteger(result, column, 7, budget, [] {})),
        MaterializedByteLimitExceeded);
    EXPECT_TRUE(result.getRows().empty());
    EXPECT_EQ(result.getRows().capacity(), 0U);
    EXPECT_EQ(budget.used(), 1U);
    competitor.reset();
    auto charge = ScalarResultHandoff::appendFirstInteger(result, column, 7, budget, [] {});
    EXPECT_EQ(result.getRows().front().at(column).asPrimitive(), PropertyValue(int64_t{7}));
    result = QueryResult{}; charge.reset();
    EXPECT_EQ(budget.used(), 0U);
}

TEST(ScalarResultHandoffTest, ParentPressureRefundsPartialAdmissionAndPreservesCompetingOwner) {
    using namespace graph::query::execution::detail;
    const std::string column = "count";
    const size_t peak = ScalarResultHandoff::integerRowUpper(column) + utils::detail::standardContainerProxyBytes();
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(peak));
    auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(peak));
    auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, peak);
    auto sibling = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, peak);
    ResourceLease competitor(sibling); competitor.set(1U);
    MaterializedByteBudget budget(peak, account);
    QueryResult result;
    EXPECT_THROW(static_cast<void>(ScalarResultHandoff::appendFirstInteger(result, column, 7, budget, [] {})),
        ResourceAdmissionDenied);
    EXPECT_EQ(result.getRows().capacity(), 0U);
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_EQ(account->used(), 0U);
    EXPECT_EQ(sibling->used(), 1U);
    competitor.reset();
    auto charge = ScalarResultHandoff::appendFirstInteger(result, column, 7, budget, [] {});
    EXPECT_EQ(account->used(), charge.bytes());
    EXPECT_EQ(result.getRows().front().at(column).asPrimitive(), PropertyValue(int64_t{7}));
    result = QueryResult{}; charge.reset();
    EXPECT_EQ(account->used(), 0U);
}

TEST(ScalarResultHandoffTest, EveryCheckpointFailureDestroysPartialBackingBeforeRefundAndRetries) {
    for (int failAt = 1; failAt <= 3; ++failAt) {
        SCOPED_TRACE(failAt);
        MaterializedByteBudget budget(4096U);
        QueryResult result;
        int calls = 0;
        EXPECT_THROW(static_cast<void>(ScalarResultHandoff::appendFirstInteger(result, "count", 9, budget, [&] {
            if (++calls == failAt) throw std::domain_error("checkpoint failure");
        })), std::domain_error);
        EXPECT_TRUE(result.getRows().empty());
        EXPECT_EQ(result.getRows().capacity(), 0U);
        EXPECT_EQ(budget.used(), 0U);
        auto charge = ScalarResultHandoff::appendFirstInteger(result, "count", 9, budget, [] {});
        EXPECT_EQ(result.getRows().front().at("count").asPrimitive(), PropertyValue(int64_t{9}));
        EXPECT_THROW(static_cast<void>(ScalarResultHandoff::appendFirstInteger(result, "count", 10, budget, [] {})),
            std::logic_error);
        EXPECT_EQ(result.rowCount(), 1U);
        result = QueryResult{}; charge.reset();
        EXPECT_EQ(budget.used(), 0U);
    }
}
