#include <gtest/gtest.h>
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
namespace {
using namespace graph;
using namespace query::expressions;
using namespace query::execution;
TEST(OwnedMapProjectionItemsTest, FailedDeepCopyGrowthPreservesExistingKeysAndChildren) {
    auto memory = std::make_shared<MaterializedByteBudget>(32768U);
    {
        utils::OwnedAllocationScope scope(memory);
        utils::OwnedStdVector<MapProjectionItem> values;
        values.emplace_back(MapProjectionItemType::MPROP_LITERAL, std::string(80U, 'k'),
            std::make_unique<LiteralExpression>(std::string(8192U, 'v')));
        const auto retained = memory->used();
        utils::OwnedHeapReservation blocker(memory, 32768U - retained - 1024U);
        const auto before = memory->used();
        EXPECT_THROW(values.reserve(2U), MaterializedByteLimitExceeded);
        EXPECT_EQ(values.size(), 1U);
        EXPECT_EQ(values[0].key, std::string(80U, 'k'));
        EXPECT_EQ(static_cast<LiteralExpression &>(*values[0].expression).getStringValue(), std::string(8192U, 'v'));
        EXPECT_EQ(memory->used(), before);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedMapProjectionItemsTest, DeepCopiesUseDestinationAuthorityAndHaveIndependentChildren) {
    auto first = std::make_shared<MaterializedByteBudget>(65536U);
    auto second = std::make_shared<MaterializedByteBudget>(65536U);
    std::optional<utils::OwnedStdVector<MapProjectionItem>> values, copy;
    {
        utils::OwnedAllocationScope scope(first);
        values.emplace();
        values->emplace_back(MapProjectionItemType::MPROP_LITERAL, std::string(80U, 'k'),
            std::make_unique<LiteralExpression>(std::string(8192U, 'v')));
    }
    { utils::OwnedAllocationScope scope(second); copy.emplace(*values); }
    EXPECT_NE((*values)[0].expression.get(), (*copy)[0].expression.get());
    values.reset(); EXPECT_EQ(first->used(), 0U);
    EXPECT_GT(second->used(), 8192U);
    copy.reset(); EXPECT_EQ(second->used(), 0U);
}
} // namespace
