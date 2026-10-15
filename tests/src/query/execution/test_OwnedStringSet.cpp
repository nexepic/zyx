#include <gtest/gtest.h>
#include "graph/utils/OwnedStringSet.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
namespace {
using namespace graph;
using namespace query::execution;
TEST(OwnedStringSetTest, RejectedReplacementPreservesItsKeysAndOriginalAuthority) {
    auto memory = std::make_shared<MaterializedByteBudget>(4096U);
    const std::unordered_set<std::string> large{std::string(8192U, 's')};
    {
        utils::OwnedAllocationScope scope(memory);
        utils::OwnedStringSet values(std::unordered_set<std::string>{"old"});
        const auto retained = memory->used();
        EXPECT_THROW(values = large, MaterializedByteLimitExceeded);
        EXPECT_EQ(values.count("old"), 1U); EXPECT_EQ(memory->used(), retained);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(OwnedStringSetTest, SparseCopiesAndMovedSourcesRetainCreditUntilTheirStorageDies) {
    auto memory = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
    std::unordered_set<std::string> input; input.rehash(4096U); input.insert(std::string(8192U, 's'));
    std::optional<utils::OwnedStringSet> values;
    {
        utils::OwnedAllocationScope scope(memory); values.emplace(input);
    }
    EXPECT_GT(memory->used(), 8192U);
    {
        utils::OwnedStringSet moved(std::move(*values));
        EXPECT_EQ(moved.count(std::string(8192U, 's')), 1U);
    }
    *values = std::unordered_set<std::string>{"reuse"};
    EXPECT_EQ(values->count("reuse"), 1U);
    values.reset(); EXPECT_EQ(memory->used(), 0U);
}
} // namespace
