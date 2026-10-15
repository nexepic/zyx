#include <gtest/gtest.h>
#include "graph/utils/OwnedStringVector.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
namespace {
using namespace graph;
using namespace query::execution;
TEST(OwnedStringVectorTest, RejectsNestedCopyBeforeChangingTheSourceOrOldValues) {
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    const std::vector<std::string> large(64U, std::string(8192U, 's'));
    {
        utils::OwnedAllocationScope scope(memory);
        utils::OwnedStringVector values;
        values.push_back("old"); const auto retained = memory->used();
        EXPECT_THROW(values = large, MaterializedByteLimitExceeded);
        ASSERT_EQ(values.size(), 1U); EXPECT_EQ(values[0], "old");
        EXPECT_EQ(memory->used(), retained); EXPECT_EQ(large[0].size(), 8192U);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedStringVectorTest, AppendingAnAliasedStringSurvivesSsoAndLongBufferRelocation) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536U);
    {
        utils::OwnedAllocationScope scope(memory);
        utils::OwnedStringVector values;
        values.push_back("short"); values.push_back(values[0]);
        values.emplace_back(std::string(8192U, 's'));
        while (values.size() < values.capacity()) values.push_back("fill");
        values.emplace_back(values[0]);
        EXPECT_EQ(values.back(), "short"); EXPECT_EQ(values[2].size(), 8192U);
        values.clear();
        EXPECT_EQ(memory->used(), utils::detail::standardVectorHeapBufferBytes(static_cast<const std::vector<std::string> &>(values)));
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedStringVectorTest, CopiesAndMovesRetainNestedCreditAfterTheAllocationScopeEnds) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536U);
    std::optional<utils::OwnedStringVector> values;
    {
        utils::OwnedAllocationScope scope(memory);
        values.emplace(std::vector<std::string>{std::string(8192U, 's')});
    }
    {
        utils::OwnedStringVector moved(std::move(*values));
        EXPECT_EQ(moved[0].size(), 8192U);
    }
    EXPECT_EQ(memory->used(), utils::detail::standardContainerProxyBytes());
    values->push_back("reuse"); EXPECT_EQ((*values)[0], "reuse");
    values.reset(); EXPECT_EQ(memory->used(), 0U);
}
} // namespace
