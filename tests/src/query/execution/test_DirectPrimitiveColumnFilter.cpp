#include "query/execution/DirectPropertyColumns.hpp"
#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"

namespace graph::query::execution {
using namespace direct_edge_test;

TEST(DirectPrimitiveColumnFilterTest, MatchesScalarSemanticsForAllComparisonsIncludingNanAndNull) {
    DirectEdgeGraph graph = directGraph(); DirectEdgeSnapshot snapshot(graph);
    for (const auto &input : std::vector<std::vector<PropertyValue>>{
        {PropertyValue(-2), PropertyValue(0), PropertyValue(7), PropertyValue{}},
        {PropertyValue(-0.0), PropertyValue(1.0), PropertyValue(std::numeric_limits<double>::quiet_NaN()), PropertyValue{}},
        {PropertyValue(false), PropertyValue(true), PropertyValue(false), PropertyValue{}}}) {
        MaterializedByteBudget budget(65536);
        {
            detail::DirectPropertyColumns columns(snapshot,input.size()+1,1,budget);
            for(size_t i=0;i<input.size();++i) columns.append(i,0,input[i],[]{});
            columns.seal();
            for(auto operation : {detail::ColumnPredicateOp::CPO_EQUAL,detail::ColumnPredicateOp::CPO_NOT_EQUAL,
                detail::ColumnPredicateOp::CPO_LESS,detail::ColumnPredicateOp::CPO_GREATER,
                detail::ColumnPredicateOp::CPO_LESS_EQUAL,detail::ColumnPredicateOp::CPO_GREATER_EQUAL,
                detail::ColumnPredicateOp::CPO_NON_NULL}) {
                const detail::ColumnPredicate predicate{0,operation,&input[1]};
                SelectionVector selection(input.size()+1);const auto before=budget.used();
                ASSERT_TRUE(columns.tryFilterPrimitive(predicate,selection,[]{}));
                EXPECT_EQ(budget.used(),before);
                for(size_t i=0;i<input.size();++i)
                    EXPECT_EQ(selection.includes(i),detail::columnPredicateMatches(input[i],predicate));
                EXPECT_FALSE(selection.includes(input.size()));
            }
        }
        EXPECT_EQ(budget.used(),0U);
    }
}

TEST(DirectPrimitiveColumnFilterTest, MixedTypesDeclineWithoutSelectionEffectsAndCancellationPropagates) {
    DirectEdgeGraph graph = directGraph(); DirectEdgeSnapshot snapshot(graph);
    MaterializedByteBudget budget(65536);
    detail::DirectPropertyColumns mixed(snapshot,3,1,budget);
    mixed.append(0,0,PropertyValue(1),[]{});mixed.append(1,0,PropertyValue(2.0),[]{});mixed.seal();
    const PropertyValue expected(1);const detail::ColumnPredicate predicate{0,detail::ColumnPredicateOp::CPO_EQUAL,&expected};
    SelectionVector nonNull(3);
    ASSERT_TRUE(mixed.tryFilterPrimitive({0,detail::ColumnPredicateOp::CPO_NON_NULL,nullptr},nonNull,[]{}));
    EXPECT_EQ(nonNull.selectedCount(),2U);EXPECT_FALSE(nonNull.includes(2));
    SelectionVector selected(3);
    EXPECT_FALSE(mixed.tryFilterPrimitive(predicate,selected,[]{}));
    EXPECT_EQ(selected.selectedCount(),3U);
    selected.reject(1);
    EXPECT_TRUE(mixed.tryFilterPrimitive(predicate,selected,[]{}));
    EXPECT_EQ(selected.selectedCount(),1U);EXPECT_TRUE(selected.includes(0));
    detail::DirectPropertyColumns uniform(snapshot,65,1,budget);
    for(size_t i=0;i<65;++i)uniform.append(i,0,PropertyValue(1),[]{});
    uniform.seal();SelectionVector all(65);size_t checks=0;
    EXPECT_THROW(uniform.tryFilterPrimitive(predicate,all,[&]{if(++checks==2)throw std::runtime_error("cancelled");}),std::runtime_error);
    EXPECT_EQ(all.selectedCount(),65U); // cancelled in preflight before consumer effects
    uniform.invalidate();
    EXPECT_THROW(uniform.tryFilterPrimitive(predicate,all,[]{}),std::invalid_argument);
}
}
