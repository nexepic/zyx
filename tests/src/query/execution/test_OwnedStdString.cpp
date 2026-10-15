#include <gtest/gtest.h>
#include "graph/utils/OwnedStdString.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"

namespace {
using namespace graph;
using namespace query::execution;

TEST(OwnedStdStringTest, AdmissionPrecedesCopiesAndRejectedAssignmentKeepsThePreviousValue) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024U);
    const std::string large(8192U, 'x');
    {
        utils::OwnedAllocationScope scope(memory);
        utils::OwnedStdString value("original");
        const auto retained = memory->used();
        EXPECT_THROW(value = large, MaterializedByteLimitExceeded);
        EXPECT_EQ(static_cast<const std::string &>(value), "original");
        EXPECT_EQ(memory->used(), retained);
        EXPECT_EQ(large.size(), 8192U);
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST(OwnedStdStringTest, CopiesSelectTheActiveOwnerAndSwapsPreserveBothAuthorities) {
    auto first = std::make_shared<MaterializedByteBudget>(65536U);
    auto second = std::make_shared<MaterializedByteBudget>(65536U);
    std::optional<utils::OwnedStdString> source, copy;
    {
        utils::OwnedAllocationScope scope(first);
        source.emplace(std::string(8192U, 's'));
    }
    const auto firstBytes = first->used();
    ASSERT_GT(firstBytes, 8192U);
    {
        utils::OwnedAllocationScope scope(second);
        copy.emplace(*source);
    }
    EXPECT_EQ(first->used(), firstBytes);
    EXPECT_GT(second->used(), 8192U);
    source->swap(*copy);
    source.reset();
    EXPECT_EQ(second->used(), 0U);
    EXPECT_EQ(first->used(), firstBytes);
    EXPECT_EQ(copy->size(), 8192U);
    copy.reset();
    EXPECT_EQ(first->used(), 0U);
}

TEST(OwnedStdStringTest, MovedSourcesRemainAdmittedAndReusableAfterTheDestinationDies) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536U);
    {
        utils::OwnedAllocationScope scope(memory);
        utils::OwnedStdString source(std::string(8192U, 's'));
        const auto retained = memory->used();
        {
            utils::OwnedStdString moved(std::move(source));
            EXPECT_EQ(moved.size(), 8192U);
            EXPECT_EQ(memory->used(), retained + utils::detail::standardContainerProxyBytes());
        }
        EXPECT_EQ(memory->used(), utils::detail::standardContainerProxyBytes());
        source = std::string(4096U, 'r');
        EXPECT_EQ(source.size(), 4096U);
        EXPECT_GT(memory->used(), 4096U);
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST(OwnedStdStringTest, ScanVariableDenialRefundsThePartiallyAllocatedOperatorObject) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024U);
    const std::string variable(8192U, 'v');
    {
        utils::OwnedAllocationScope scope(memory);
        EXPECT_THROW(static_cast<void>(std::make_unique<query::logical::LogicalNodeScan>(variable)),
            MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(variable.size(), 8192U);
}
} // namespace

TEST(OwnedStdStringMutationTest, DeniedReserveAndCountAssignmentPreserveValueCapacityAndCredit) {
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(4096U);
    const std::string input(1024U, 'x');
    {
        graph::utils::OwnedAllocationScope scope(memory);
        graph::utils::OwnedStdString value(input);
        const auto retained = memory->used(), capacity = value.capacity();
        EXPECT_THROW(value.reserve(8192U), graph::query::execution::MaterializedByteLimitExceeded);
        EXPECT_THROW(value.assign(8192U, 'y'), graph::query::execution::MaterializedByteLimitExceeded);
        EXPECT_EQ(value, input); EXPECT_EQ(value.capacity(), capacity); EXPECT_EQ(memory->used(), retained);
        value.clear(); EXPECT_TRUE(value.empty()); EXPECT_EQ(memory->used(), retained);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedStdStringMutationTest, SuccessfulGrowthAndCountReplacementKeepOnlyTheirRetainedStorage) {
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(32768U);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        graph::utils::OwnedStdString value("short"); value.reserve(4096U);
        EXPECT_EQ(memory->used(), graph::utils::detail::dynamicStringBufferBytes(value));
        value.assign(2048U, 'v'); EXPECT_EQ(value.size(), 2048U);
        EXPECT_EQ(memory->used(), graph::utils::detail::dynamicStringBufferBytes(value));
    }
    EXPECT_EQ(memory->used(), 0U);
}
