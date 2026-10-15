#include <gtest/gtest.h>
#include "graph/utils/OwnedStdVector.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"

namespace {
using namespace graph;
using namespace query::execution;
TEST(OwnedStdVectorTest, GrowthDenialPreservesValuesCapacityAndTheirOriginalCredit) {
    auto memory = std::make_shared<MaterializedByteBudget>(128U);
    {
        utils::OwnedAllocationScope scope(memory);
        utils::OwnedStdVector<std::unique_ptr<int>> values;
        values.push_back(std::make_unique<int>(7));
        const auto capacity = values.capacity(); const auto retained = memory->used();
        EXPECT_THROW(values.reserve(1024U), MaterializedByteLimitExceeded);
        ASSERT_EQ(values.size(), 1U); EXPECT_EQ(*values[0], 7);
        EXPECT_EQ(values.capacity(), capacity); EXPECT_EQ(memory->used(), retained);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedStdVectorTest, AdmissionPrecedesAdoptingAnExternalVector) {
    std::vector<std::unique_ptr<int>> input;
    input.reserve(1024U); input.push_back(std::make_unique<int>(7));
    auto memory = std::make_shared<MaterializedByteBudget>(128U);
    {
        utils::OwnedAllocationScope scope(memory);
        EXPECT_THROW(static_cast<void>(utils::OwnedStdVector<std::unique_ptr<int>>(std::move(input))), MaterializedByteLimitExceeded);
    }
    ASSERT_EQ(input.size(), 1U); EXPECT_EQ(*input[0], 7); EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedStdVectorTest, AliasedMovedElementsRemainValidAcrossBackingGrowth) {
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    {
        utils::OwnedAllocationScope scope(memory);
        utils::OwnedStdVector<std::unique_ptr<int>> values;
        values.push_back(std::make_unique<int>(7));
        ASSERT_EQ(values.size(), values.capacity());
        values.push_back(std::move(values[0]));
        ASSERT_EQ(values.size(), 2U); EXPECT_FALSE(values[0]); EXPECT_EQ(*values[1], 7);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedStdVectorTest, MovedEmptyVectorsRetainTheirAuthorityForReuse) {
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    std::optional<utils::OwnedStdVector<std::unique_ptr<int>>> values;
    {
        utils::OwnedAllocationScope scope(memory);
        values.emplace(); values->push_back(std::make_unique<int>(7));
    }
    { utils::OwnedStdVector<std::unique_ptr<int>> moved(std::move(*values)); EXPECT_EQ(*moved[0], 7); }
    EXPECT_EQ(memory->used(), utils::detail::standardContainerProxyBytes());
    values->push_back(std::make_unique<int>(8));
    EXPECT_GT(memory->used(), 0U);
    values.reset(); EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedStdVectorTest, InitializerReplacementUsesReceiverAuthorityAndPreservesValuesOnDenial) {
    const size_t proxy = utils::detail::standardContainerProxyBytes();
    auto memory = std::make_shared<MaterializedByteBudget>(2U * proxy + 3U * sizeof(int));
    std::optional<utils::OwnedStdVector<int>> values;
    {
        utils::OwnedAllocationScope scope(memory);
        values.emplace(std::initializer_list<int>{1, 2});
    }
    const auto retained = memory->used();
    EXPECT_THROW((*values = {3, 4, 5, 6, 7}), MaterializedByteLimitExceeded);
    ASSERT_EQ(values->size(), 2U);
    EXPECT_EQ((*values)[0], 1); EXPECT_EQ((*values)[1], 2);
    EXPECT_EQ(memory->used(), retained);
    *values = {9};
    ASSERT_EQ(values->size(), 1U); EXPECT_EQ(values->front(), 9);
    EXPECT_EQ(memory->used(), proxy + sizeof(int));
    values.reset(); EXPECT_EQ(memory->used(), 0U);
}
} // namespace
