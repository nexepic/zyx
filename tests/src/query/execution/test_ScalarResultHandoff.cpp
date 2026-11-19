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

namespace {
size_t retainedIntegerRows(const QueryResult &result, const std::string &column) {
    using namespace graph::utils::detail;
    size_t bytes = standardVectorHeapBufferBytes(result.getRows()) - standardContainerProxyBytes();
    for (const auto &row : result.getRows()) {
        const size_t logical = column.size() + sizeof(ResultValue) + sizeof(int64_t);
        bytes = addStandardHeapBytes(bytes, (std::max)(logical, graph::query::detail::resultRowHeapBytes(row)));
    }
    return bytes;
}
}

TEST(ScalarResultHandoffTest, AppendsUnderTheSeparateRelocationAndRetainedPeaks) {
    using namespace graph::utils::detail;
    const std::string column = "value";
    // Observe the completed public layout independently of the handoff. Permit
    // its logical floor too; no executing source row or third owning map may
    // be hidden by this boundary.
    QueryResult expected;
    expected.reserveRows(2U);
    for (int64_t value : {7, 8}) {
        QueryResult::Row row; row.reserve(1U); row.emplace(column, PropertyValue(value));
        expected.addRow(std::move(row));
    }
    QueryResult first;
    first.reserveRows(1U);
    QueryResult::Row firstRow; firstRow.reserve(1U); firstRow.emplace(column, PropertyValue(int64_t{7}));
    first.addRow(std::move(firstRow));
    const size_t completed = retainedIntegerRows(expected, column);
    const size_t relocation = retainedIntegerRows(first, column) +
        standardAllocatorRequestBytes(2U * sizeof(QueryResult::Row), alignof(QueryResult::Row)) +
        graph::query::detail::resultRowsRelocationHeapUpper(first.getRows());
    // Fresh map construction reserves a conservative request upper before
    // reconciling its observed storage; that upper is a separate live stage.
    const size_t incomingUpper = (std::max)(column.size() + sizeof(ResultValue) + sizeof(int64_t),
        standardFreshUnorderedMapHeapUpper<std::string, ResultValue>(1U) + standardFreshStringHeapUpper(column.size()));
    const size_t construction = retainedIntegerRows(first, column) + sizeof(QueryResult::Row) + incomingUpper;
    const size_t cap = (std::max)({completed, relocation, construction}) + standardContainerProxyBytes();
    MaterializedByteBudget budget(cap);
    QueryResult result;
    auto charge = ScalarResultHandoff::appendFirstInteger(result, column, 7, budget, [] {});
    ScalarResultHandoff::appendInteger(result, column, 8, budget, charge, [] {});
    ASSERT_EQ(result.rowCount(), 2U);
    EXPECT_EQ(result.getRows()[0].at(column).asPrimitive(), PropertyValue(int64_t{7}));
    EXPECT_EQ(result.getRows()[1].at(column).asPrimitive(), PropertyValue(int64_t{8}));
    EXPECT_EQ(charge.bytes(), retainedIntegerRows(result, column));
    EXPECT_EQ(budget.used(), charge.bytes());
    EXPECT_LE(budget.used(), cap);
    result = QueryResult{}; charge.reset();
    EXPECT_EQ(budget.used(), 0U);
}

TEST(ScalarResultHandoffTest, SubsequentCheckpointFailuresKeepExistingRowsAndFundRetainedCapacity) {
    for (int failAt = 1; failAt <= 3; ++failAt) {
        SCOPED_TRACE(failAt);
        MaterializedByteBudget budget(4096U);
        QueryResult result;
        auto charge = ScalarResultHandoff::appendFirstInteger(result, "value", 7, budget, [] {});
        int calls = 0;
        EXPECT_THROW(ScalarResultHandoff::appendInteger(result, "value", 8, budget, charge, [&] {
            if (++calls == failAt) throw std::domain_error("checkpoint failure");
        }), std::domain_error);
        ASSERT_EQ(result.rowCount(), 1U);
        EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(int64_t{7}));
        EXPECT_EQ(charge.bytes(), retainedIntegerRows(result, "value"));
        EXPECT_EQ(budget.used(), charge.bytes());
        ScalarResultHandoff::appendInteger(result, "value", 8, budget, charge, [] {});
        ASSERT_EQ(result.rowCount(), 2U);
        EXPECT_EQ(result.getRows().back().at("value").asPrimitive(), PropertyValue(int64_t{8}));
        result = QueryResult{}; charge.reset();
        EXPECT_EQ(budget.used(), 0U);
    }
}

TEST(ScalarResultHandoffTest, SubsequentParentDenialKeepsExistingRowsAndCompetingOwner) {
    using namespace graph::query::execution::detail;
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(4096U));
    auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(4096U));
    auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, 4096U);
    MaterializedByteBudget budget(4096U, account);
    QueryResult result;
    auto charge = ScalarResultHandoff::appendFirstInteger(result, "value", 7, budget, [] {});
    ResourceLease competitor(account); competitor.set(4096U - charge.bytes());
    EXPECT_THROW(ScalarResultHandoff::appendInteger(result, "value", 8, budget, charge, [] {}),
        ResourceAdmissionDenied);
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(int64_t{7}));
    EXPECT_EQ(budget.used(), charge.bytes());
    EXPECT_EQ(account->used(), charge.bytes() + competitor.bytes());
    competitor.reset();
    ScalarResultHandoff::appendInteger(result, "value", 8, budget, charge, [] {});
    ASSERT_EQ(result.rowCount(), 2U);
    result = QueryResult{}; charge.reset();
    EXPECT_EQ(account->used(), 0U);
}

TEST(ScalarResultHandoffTest, OptionalDoublingLeavesRoomForTheNextMapAdmission) {
    using namespace graph::utils::detail;
    for (size_t existing : {2U, 4U}) {
        SCOPED_TRACE(existing);
        MaterializedByteBudget budget(16384U);
        QueryResult result;
        auto charge = ScalarResultHandoff::appendFirstInteger(result, "value", 1, budget, [] {});
        for (size_t index = 1U; index < existing; ++index)
            ScalarResultHandoff::appendInteger(result, "value", static_cast<int64_t>(index + 1U), budget, charge, [] {});
        ASSERT_EQ(result.getRows().capacity(), existing);
        const size_t oldBacking = standardVectorHeapBufferBytes(result.getRows()) - standardContainerProxyBytes();
        const size_t newBacking = standardAllocatorRequestBytes((existing + 1U) * sizeof(QueryResult::Row), alignof(QueryResult::Row));
        const size_t incoming = (std::max)(size_t{5U} + sizeof(ResultValue) + sizeof(int64_t),
            standardFreshUnorderedMapHeapUpper<std::string, ResultValue>(1U) + standardFreshStringHeapUpper(5U));
        const size_t extra = (std::max)(newBacking + graph::query::detail::resultRowsRelocationHeapUpper(result.getRows()),
            newBacking - oldBacking + incoming);
        MaterializedByteReservation competitor(&budget); competitor.set(budget.available() - extra);
        ScalarResultHandoff::appendInteger(result, "value", static_cast<int64_t>(existing + 1U), budget, charge, [] {});
        ASSERT_EQ(result.rowCount(), existing + 1U);
        EXPECT_EQ(result.getRows().capacity(), existing + 1U);
        for (size_t index = 0U; index < result.rowCount(); ++index)
            EXPECT_EQ(result.getRows()[index].at("value").asPrimitive(), PropertyValue(static_cast<int64_t>(index + 1U)));
        EXPECT_EQ(charge.bytes(), retainedIntegerRows(result, "value"));
        EXPECT_EQ(budget.used(), charge.bytes() + competitor.bytes());
        result = QueryResult{}; charge.reset(); competitor.reset();
        EXPECT_EQ(budget.used(), 0U);
    }
}

TEST(ScalarResultHandoffTest, ReclaimsOptionalStorageBeforeBothAppendAdmissions) {
    MaterializedByteBudget budget(4096U);
    QueryResult result;
    auto charge = ScalarResultHandoff::appendFirstInteger(result, "value", 7, budget, [] {});
    MaterializedByteReservation cache(&budget); cache.set(budget.available());
    size_t calls = 0U;
    ScalarResultHandoff::appendInteger(result, "value", 8, budget, charge, [] {},
        [&](size_t requested, size_t previous) {
            ++calls;
            if (requested - previous > budget.available()) cache.reset();
        });
    EXPECT_EQ(calls, 2U); // row-array relocation, then new row construction
    EXPECT_EQ(cache.bytes(), 0U);
    ASSERT_EQ(result.rowCount(), 2U);
    EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(int64_t{7}));
    EXPECT_EQ(result.getRows().back().at("value").asPrimitive(), PropertyValue(int64_t{8}));
    EXPECT_EQ(charge.bytes(), retainedIntegerRows(result, "value"));
    EXPECT_EQ(budget.used(), charge.bytes());
    result = QueryResult{}; charge.reset();
    EXPECT_EQ(budget.used(), 0U);
}

TEST(ScalarResultHandoffTest, ParentDenialAfterGrowthPreservesExistingRowsAndTheNewBackingCharge) {
    using namespace graph::query::execution::detail;
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(4096U));
    auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(4096U));
    auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, 4096U);
    MaterializedByteBudget budget(4096U, account);
    QueryResult result;
    auto charge = ScalarResultHandoff::appendFirstInteger(result, "value", 7, budget, [] {});
    ResourceLease competitor(account);
    EXPECT_THROW(ScalarResultHandoff::appendInteger(result, "value", 8, budget, charge, [] {},
        [&](size_t, size_t previous) {
            if (previous == 0U) competitor.set(4096U - account->used());
        }), ResourceAdmissionDenied);
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_GE(result.getRows().capacity(), 2U);
    EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(int64_t{7}));
    EXPECT_EQ(charge.bytes(), retainedIntegerRows(result, "value"));
    EXPECT_EQ(budget.used(), charge.bytes());
    EXPECT_EQ(account->used(), charge.bytes() + competitor.bytes());
    competitor.reset();
    ScalarResultHandoff::appendInteger(result, "value", 8, budget, charge, [] {});
    ASSERT_EQ(result.rowCount(), 2U);
    result = QueryResult{}; charge.reset();
    EXPECT_EQ(account->used(), 0U);
}
