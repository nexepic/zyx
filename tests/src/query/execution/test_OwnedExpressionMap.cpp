#include <gtest/gtest.h>
#include "graph/query/expressions/OwnedExpressionMap.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
namespace {
using namespace graph;
using namespace query::expressions;
using namespace query::execution;
TEST(OwnedExpressionMapTest, RejectedMetadataCopyPreservesExistingMapAndAuthority) {
    const std::unordered_map<std::string, std::shared_ptr<Expression>> input{{std::string(8192U, 'k'), {}}};
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    {
        utils::OwnedAllocationScope scope(memory);
        OwnedExpressionMap values;
        const auto before = memory->used();
        EXPECT_THROW(values = input, MaterializedByteLimitExceeded);
        EXPECT_TRUE(values.empty()); EXPECT_EQ(memory->used(), before);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedExpressionMapTest, PartialDeepCloneFailureRefundsMetadataAndKeepsSourceChildren) {
    std::unordered_map<std::string, std::shared_ptr<Expression>> input;
    input.emplace(std::string(80U, 'a'), utils::makeSharedOwned<LiteralExpression>(std::string(8192U, 'a')));
    input.emplace(std::string(80U, 'b'), utils::makeSharedOwned<LiteralExpression>(std::string(8192U, 'b')));
    auto memory = std::make_shared<MaterializedByteBudget>(65536U);
    size_t copies = 0U;
    {
        utils::OwnedAllocationScope scope(memory);
        EXPECT_THROW(OwnedExpressionMap::transformValues(input, [&](const auto &value) {
            if (++copies == 2U) throw std::runtime_error("cancel partial clone");
            return utils::shareOwned(value->clone());
        }), std::runtime_error);
    }
    EXPECT_EQ(copies, 2U); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(input.size(), 2U);
    EXPECT_EQ(static_cast<LiteralExpression &>(*input.at(std::string(80U, 'a'))).getStringValue(), std::string(8192U, 'a'));
}
TEST(OwnedExpressionMapTest, CloneOwnsDestinationAndMovedEmptyMapRemainsReusable) {
    const std::unordered_map<std::string, std::shared_ptr<Expression>> input{{std::string(80U, 'k'),
        utils::makeSharedOwned<LiteralExpression>(std::string(8192U, 'v'))}};
    auto memory = std::make_shared<MaterializedByteBudget>(65536U);
    std::optional<OwnedExpressionMap> values;
    { utils::OwnedAllocationScope scope(memory); values.emplace(cloneExpressionMap(input)); }
    EXPECT_GT(memory->used(), 8192U);
    {
        OwnedExpressionMap moved(std::move(*values));
        EXPECT_NE(moved.begin()->second.get(), input.begin()->second.get());
        EXPECT_TRUE(values->empty());
    }
    *values = input;
    EXPECT_EQ(values->size(), 1U);
    values.reset(); EXPECT_EQ(memory->used(), 0U);
}
} // namespace
