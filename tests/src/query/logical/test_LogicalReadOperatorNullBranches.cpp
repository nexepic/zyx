/**
 * @file test_LogicalReadOperatorNullBranches.cpp
 * @brief Null/edge branch coverage tests for logical read operators.
 */

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::logical;

TEST(LogicalFilterNullBranchTest, HandlesNullChildAndNullPredicate) {
	LogicalFilter filter(nullptr, nullptr);

	EXPECT_TRUE(filter.getOutputVariables().empty());
	EXPECT_EQ(filter.toString(), "Filter(null)");

	auto cloned = filter.clone();
	auto *clonedFilter = dynamic_cast<LogicalFilter *>(cloned.get());
	ASSERT_NE(clonedFilter, nullptr);
	ASSERT_EQ(clonedFilter->getChildren().size(), 1UL);
	EXPECT_EQ(clonedFilter->getChildren()[0], nullptr);
	EXPECT_EQ(clonedFilter->getPredicate(), nullptr);
}

TEST(LogicalSortNullBranchTest, CloneCoversNullExpressionAndNullChild) {
	std::vector<LogicalSortItem> items;
	std::shared_ptr<Expression> nullExpr;
	items.emplace_back(nullExpr, true);
	LogicalSort sort(nullptr, std::move(items));

	EXPECT_TRUE(sort.getOutputVariables().empty());

	auto cloned = sort.clone();
	auto *clonedSort = dynamic_cast<LogicalSort *>(cloned.get());
	ASSERT_NE(clonedSort, nullptr);
	ASSERT_EQ(clonedSort->getChildren().size(), 1UL);
	EXPECT_EQ(clonedSort->getChildren()[0], nullptr);
	ASSERT_EQ(clonedSort->getSortItems().size(), 1UL);
	EXPECT_EQ(clonedSort->getSortItems()[0].expression, nullptr);

	EXPECT_THROW(sort.setChild(1, std::make_unique<LogicalNodeScan>("x")), std::out_of_range);
	EXPECT_THROW(sort.detachChild(1), std::out_of_range);
}

TEST(LogicalProjectNullBranchTest, CloneCoversNullExpressionAndNullChild) {
	std::vector<LogicalProjectItem> items;
	std::shared_ptr<Expression> nullExpr;
	items.emplace_back(nullExpr, "x");
	LogicalProject project(nullptr, std::move(items), true);

	auto cloned = project.clone();
	auto *clonedProject = dynamic_cast<LogicalProject *>(cloned.get());
	ASSERT_NE(clonedProject, nullptr);
	ASSERT_EQ(clonedProject->getChildren().size(), 1UL);
	EXPECT_EQ(clonedProject->getChildren()[0], nullptr);
	ASSERT_EQ(clonedProject->getItems().size(), 1UL);
	EXPECT_EQ(clonedProject->getItems()[0].expression, nullptr);

	EXPECT_THROW(project.setChild(1, std::make_unique<LogicalNodeScan>("x")), std::out_of_range);
	EXPECT_THROW(project.detachChild(1), std::out_of_range);
}

TEST(LogicalAggregateNullBranchTest, OutputAndCloneHandleNullExpressionsAndChild) {
	std::vector<std::shared_ptr<Expression>> groupByExprs;
	groupByExprs.push_back(nullptr);

	std::vector<LogicalAggItem> aggregations;
	aggregations.emplace_back("count", nullptr, "cnt", false);

	LogicalAggregate aggregate(nullptr, std::move(groupByExprs), std::move(aggregations));
	auto vars = aggregate.getOutputVariables();
	ASSERT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "cnt");

	auto cloned = aggregate.clone();
	auto *clonedAggregate = dynamic_cast<LogicalAggregate *>(cloned.get());
	ASSERT_NE(clonedAggregate, nullptr);
	ASSERT_EQ(clonedAggregate->getChildren().size(), 1UL);
	EXPECT_EQ(clonedAggregate->getChildren()[0], nullptr);
	ASSERT_EQ(clonedAggregate->getAggregations().size(), 1UL);
	EXPECT_EQ(clonedAggregate->getAggregations()[0].argument, nullptr);

	EXPECT_THROW(aggregate.setChild(1, std::make_unique<LogicalNodeScan>("x")), std::out_of_range);
	EXPECT_THROW(aggregate.detachChild(1), std::out_of_range);
}

TEST(LogicalJoinNullBranchTest, OutputAndCloneHandleNullChildren) {
	LogicalJoin join(nullptr, nullptr);
	EXPECT_TRUE(join.getOutputVariables().empty());

	auto cloned = join.clone();
	auto *clonedJoin = dynamic_cast<LogicalJoin *>(cloned.get());
	ASSERT_NE(clonedJoin, nullptr);
	ASSERT_EQ(clonedJoin->getChildren().size(), 2UL);
	EXPECT_EQ(clonedJoin->getChildren()[0], nullptr);
	EXPECT_EQ(clonedJoin->getChildren()[1], nullptr);
}

TEST(LogicalUnwindNullBranchTest, OutputAndCloneHandleNullChildInLiteralMode) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2))};
	LogicalUnwind unwind(nullptr, "x", values);

	auto vars = unwind.getOutputVariables();
	ASSERT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "x");

	auto cloned = unwind.clone();
	auto *clonedUnwind = dynamic_cast<LogicalUnwind *>(cloned.get());
	ASSERT_NE(clonedUnwind, nullptr);
	ASSERT_EQ(clonedUnwind->getChildren().size(), 1UL);
	EXPECT_EQ(clonedUnwind->getChildren()[0], nullptr);
	EXPECT_TRUE(clonedUnwind->hasLiteralList());
	EXPECT_EQ(clonedUnwind->getAlias(), "x");
}

