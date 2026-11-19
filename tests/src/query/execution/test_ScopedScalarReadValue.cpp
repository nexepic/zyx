#include <gtest/gtest.h>
#include "query/execution/ScopedScalarReadValue.hpp"
using graph::PropertyValue;
using namespace graph::query::execution;
using graph::query::execution::detail::ScopedScalarReadValue;
TEST(ScopedScalarReadValueTest, OwnsStringPayloadWithoutASecondDeepCopy) {
    const char *produced = nullptr;
    const ScopedScalarReadValue value([&]() -> LogicalPlanReadValue {
        LogicalPlanReadValue result(PropertyValue(std::string(4096U,'x')));
        produced=std::get<std::string>(std::get<PropertyValue>(result).getVariant()).data();
        return result;
    });
    EXPECT_EQ(std::get<std::string>(value.get().getVariant()).data(),produced);
    EXPECT_EQ(value.get(),PropertyValue(std::string(4096U,'x')));
    EXPECT_FALSE(value.borrowed());
}
TEST(ScopedScalarReadValueTest, BorrowsAlreadyOwnedParameterAndPreservesItsIdentity) {
    const PropertyValue input(std::string(4096U,'p'));
    { const ScopedScalarReadValue value(input); EXPECT_TRUE(value.borrowed());EXPECT_EQ(&value.get(),&input); }
    EXPECT_EQ(input,PropertyValue(std::string(4096U,'p')));
}
TEST(ScopedScalarReadValueTest, OwnsNestedListWithoutCopyingItsBacking) {
    const PropertyValue *produced=nullptr;
    const ScopedScalarReadValue value([&]() -> LogicalPlanReadValue {
        LogicalPlanReadValue result(PropertyValue(std::vector<PropertyValue>{PropertyValue(std::string(4096U,'p')),PropertyValue(int64_t{42})}));
        produced=std::get<std::vector<PropertyValue>>(std::get<PropertyValue>(result).getVariant()).data();return result;
    });
    const auto &list=std::get<std::vector<PropertyValue>>(value.get().getVariant());
    EXPECT_EQ(list.data(),produced);EXPECT_EQ(list[1],PropertyValue(int64_t{42}));
}
TEST(ScopedScalarReadValueTest, ConvertsNodeEdgeAndReferenceToTheirScalarIds) {
    const ScopedScalarReadValue n([]()->LogicalPlanReadValue { graph::storage::LogicalGraphNode n;n.node_id=7;return n; });
    const ScopedScalarReadValue e([]()->LogicalPlanReadValue { graph::storage::LogicalGraphEdge e;e.edge_id=9;return e; });
    const ScopedScalarReadValue r([]()->LogicalPlanReadValue { return graph::storage::EntityRef{11,false,0,0,0}; });
    EXPECT_EQ(n.get(),PropertyValue(uint64_t{7}));EXPECT_EQ(e.get(),PropertyValue(uint64_t{9}));EXPECT_EQ(r.get(),PropertyValue(uint64_t{11}));
}

TEST(ScopedScalarReadValueTest, PropagatesProducerFailureWithoutEvaluatingItTwice) {
    size_t calls = 0U;
    EXPECT_THROW((ScopedScalarReadValue([&]() -> LogicalPlanReadValue {
        ++calls; throw std::runtime_error("scalar producer failure");
    })), std::runtime_error);
    EXPECT_EQ(calls, 1U);
}
static_assert(!std::is_copy_constructible_v<ScopedScalarReadValue>);
static_assert(!std::is_move_constructible_v<ScopedScalarReadValue>);
template<class T> concept HasTemporaryScalarView = requires(T &&value) { std::move(value).get(); };
static_assert(!HasTemporaryScalarView<ScopedScalarReadValue>);
