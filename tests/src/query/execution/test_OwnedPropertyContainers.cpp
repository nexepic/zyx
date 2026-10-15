#include <gtest/gtest.h>
#include "graph/core/OwnedPropertyMap.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
namespace {
using namespace graph;
using namespace query::execution;
using namespace query::logical;
TEST(OwnedPropertyContainersTest, NestedVectorDenialPreservesOldDataAndRefundsPartialCopy) {
    const std::vector<PropertyValue> large{PropertyValue(std::string(8192U, 'v'))};
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    {
        utils::OwnedAllocationScope scope(memory);
        OwnedPropertyVector values{PropertyValue(42)};
        const auto retained = memory->used();
        EXPECT_THROW(values = large, MaterializedByteLimitExceeded);
        EXPECT_EQ(values.front(), PropertyValue(42));
        EXPECT_EQ(memory->used(), retained);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedPropertyContainersTest, SparseNestedMapCopyRetainsOwnerBeyondScopeAndRejectsReplacement) {
    PropertyValue::MapType input; input.rehash(4096U);
    input.emplace(std::string(80U, 'k'), PropertyValue(std::vector<PropertyValue>{PropertyValue(std::string(4096U, 'v'))}));
    auto memory = std::make_shared<MaterializedByteBudget>(128U*1024U);
    std::optional<OwnedPropertyMap> values;
    { utils::OwnedAllocationScope scope(memory); values.emplace(input); }
    EXPECT_GT(memory->used(), 4096U);
    PropertyValue::MapType large{{"new", PropertyValue(std::string(256U*1024U, 'v'))}};
    const auto retained = memory->used();
    EXPECT_THROW(*values = large, MaterializedByteLimitExceeded);
    EXPECT_EQ(values->size(), 1U);
    EXPECT_EQ(memory->used(), retained);
    values.reset(); EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedPropertyContainersTest, PredicateReplacementAdmissionPrecedesChangingOriginalPlan) {
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    const std::vector<RangePredicate> large{{"large", PropertyValue(std::string(8192U, 'v')), PropertyValue(), true, false}};
    {
        utils::OwnedAllocationScope scope(memory);
        LogicalNodeScan scan("n");
        scan.setRangePredicates({RangePredicate{"age", PropertyValue(18), PropertyValue(65), true, true}});
        const auto retained = memory->used();
        EXPECT_THROW(scan.setRangePredicates(large), MaterializedByteLimitExceeded);
        EXPECT_EQ(scan.getRangePredicates().front().key, "age");
        EXPECT_EQ(memory->used(), retained);
        auto clone = scan.clone();
        EXPECT_EQ(static_cast<LogicalNodeScan &>(*clone).getRangePredicates().front().minValue, PropertyValue(18));
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedPropertyContainersTest, CopiesIntoAnotherAuthorityDoNotRetainSourceAccount) {
    auto first = std::make_shared<MaterializedByteBudget>(65536U);
    auto second = std::make_shared<MaterializedByteBudget>(65536U);
    const std::vector<PropertyValue> input{PropertyValue(std::string(4096U, 'v'))};
    std::optional<OwnedPropertyVector> source, copy;
    { utils::OwnedAllocationScope scope(first); source.emplace(input); }
    { utils::OwnedAllocationScope scope(second); copy.emplace(*source); }
    source.reset(); EXPECT_EQ(first->used(), 0U);
    EXPECT_GT(second->used(), 4096U);
    copy.reset(); EXPECT_EQ(second->used(), 0U);
}
} // namespace
