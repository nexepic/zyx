#include <gtest/gtest.h>
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
namespace {
using namespace graph;
using namespace query::logical;
using namespace query::execution;
TEST(OwnedOutputVariablesTest, LongScanColumnRejectsBeforeCopyAndLeavesPlanIntact) {
    LogicalNodeScan scan(std::string(8192U, 'v'));
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    { utils::OwnedAllocationScope scope(memory);
      EXPECT_THROW(scan.getOwnedOutputVariables(), MaterializedByteLimitExceeded); }
    EXPECT_EQ(scan.getVariable(), std::string(8192U, 'v'));
    EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedOutputVariablesTest, AdmittedColumnsKeepTheirOwnerAfterTheProducingScopeEnds) {
    LogicalNodeScan scan(std::string(8192U, 'v'));
    auto memory = std::make_shared<MaterializedByteBudget>(65536U);
    std::optional<utils::OwnedStringVector> columns;
    { utils::OwnedAllocationScope scope(memory); columns.emplace(scan.getOwnedOutputVariables()); }
    ASSERT_EQ(columns->size(), 1U);
    EXPECT_EQ(columns->front(), scan.getVariable());
    EXPECT_GT(memory->used(), 8192U);
    columns.reset(); EXPECT_EQ(memory->used(), 0U);
}
} // namespace
