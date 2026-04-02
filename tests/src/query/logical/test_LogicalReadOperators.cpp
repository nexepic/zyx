/**
 * @file test_LogicalReadOperators.cpp
 * @brief Unit tests for logical read operator classes.
 *
 * Tests cover construction, type identification, children management,
 * output variables, cloning, and toString for read-path logical operators:
 * NodeScan, Filter, Project, Join, Sort, Aggregate, Limit, Skip,
 * SingleRow, Unwind, OptionalMatch, Union.
 */

#include <gtest/gtest.h>

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"
#include "graph/query/logical/operators/LogicalOptionalMatch.hpp"
#include "graph/query/logical/operators/LogicalUnion.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/expressions/Expression.hpp"

using namespace graph::query::logical;
using namespace graph::query::expressions;

// =============================================================================
// Helper: create a simple predicate expression (n.age = 25)
// =============================================================================

static std::shared_ptr<Expression> makePropertyEqualityExpr(
    const std::string &var, const std::string &prop, int64_t value) {
    auto lhs = std::make_unique<VariableReferenceExpression>(var, prop);
    auto rhs = std::make_unique<LiteralExpression>(value);
    auto binExpr = std::make_unique<BinaryOpExpression>(
        std::move(lhs), BinaryOperatorType::BOP_EQUAL, std::move(rhs));
    return std::shared_ptr<Expression>(binExpr.release());
}

static std::shared_ptr<Expression> makeVarRefExpr(const std::string &var) {
    return std::shared_ptr<Expression>(
        std::make_unique<VariableReferenceExpression>(var).release());
}

static std::shared_ptr<Expression> makeLiteralExpr(bool val) {
    return std::make_shared<LiteralExpression>(val);
}

// =============================================================================
// LogicalNodeScan
// =============================================================================

class LogicalNodeScanTest : public ::testing::Test {};

TEST_F(LogicalNodeScanTest, BasicConstruction) {
    LogicalNodeScan scan("n", {"Person"});
    EXPECT_EQ(scan.getType(), LogicalOpType::LOP_NODE_SCAN);
    EXPECT_EQ(scan.getVariable(), "n");
    EXPECT_EQ(scan.getLabels().size(), 1u);
    EXPECT_EQ(scan.getLabels()[0], "Person");
    EXPECT_TRUE(scan.getChildren().empty());
}

TEST_F(LogicalNodeScanTest, OutputVariables) {
    LogicalNodeScan scan("p");
    auto vars = scan.getOutputVariables();
    ASSERT_EQ(vars.size(), 1u);
    EXPECT_EQ(vars[0], "p");
}

TEST_F(LogicalNodeScanTest, PropertyPredicates) {
    std::vector<std::pair<std::string, graph::PropertyValue>> preds = {
        {"name", graph::PropertyValue(std::string("Alice"))},
        {"age", graph::PropertyValue(int64_t(30))}
    };
    LogicalNodeScan scan("n", {"Person"}, preds);
    EXPECT_EQ(scan.getPropertyPredicates().size(), 2u);
    EXPECT_EQ(scan.getPropertyPredicates()[0].first, "name");
}

TEST_F(LogicalNodeScanTest, MutablePropertyPredicates) {
    LogicalNodeScan scan("n", {"Person"});
    EXPECT_TRUE(scan.getPropertyPredicates().empty());

    std::vector<std::pair<std::string, graph::PropertyValue>> preds = {
        {"age", graph::PropertyValue(int64_t(25))}
    };
    scan.setPropertyPredicates(std::move(preds));
    EXPECT_EQ(scan.getPropertyPredicates().size(), 1u);
}

TEST_F(LogicalNodeScanTest, PreferredScanType) {
    LogicalNodeScan scan("n");
    EXPECT_EQ(scan.getPreferredScanType(), graph::query::execution::ScanType::FULL_SCAN);
    scan.setPreferredScanType(graph::query::execution::ScanType::LABEL_SCAN);
    EXPECT_EQ(scan.getPreferredScanType(), graph::query::execution::ScanType::LABEL_SCAN);
}

TEST_F(LogicalNodeScanTest, ClonePreservesState) {
    std::vector<std::pair<std::string, graph::PropertyValue>> preds = {
        {"name", graph::PropertyValue(std::string("Bob"))}
    };
    LogicalNodeScan scan("n", {"Person"}, preds);
    auto cloned = scan.clone();
    auto *clonedScan = dynamic_cast<LogicalNodeScan *>(cloned.get());
    ASSERT_NE(clonedScan, nullptr);
    EXPECT_EQ(clonedScan->getVariable(), "n");
    EXPECT_EQ(clonedScan->getLabels().size(), 1u);
    EXPECT_EQ(clonedScan->getPropertyPredicates().size(), 1u);
}

TEST_F(LogicalNodeScanTest, ToString) {
    LogicalNodeScan scan("n", {"Person", "Employee"});
    auto str = scan.toString();
    EXPECT_NE(str.find("NodeScan"), std::string::npos);
    EXPECT_NE(str.find("Person"), std::string::npos);
}

TEST_F(LogicalNodeScanTest, LeafNodeRejectsChildren) {
    LogicalNodeScan scan("n");
    EXPECT_THROW(scan.setChild(0, std::make_unique<LogicalNodeScan>("m")), std::logic_error);
    EXPECT_THROW(scan.detachChild(0), std::logic_error);
}

// =============================================================================
// LogicalFilter
// =============================================================================

class LogicalFilterTest : public ::testing::Test {};

TEST_F(LogicalFilterTest, BasicConstruction) {
    auto child = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
    auto pred = makePropertyEqualityExpr("n", "age", 25);
    LogicalFilter filter(std::move(child), pred);

    EXPECT_EQ(filter.getType(), LogicalOpType::LOP_FILTER);
    EXPECT_NE(filter.getPredicate(), nullptr);
    ASSERT_EQ(filter.getChildren().size(), 1u);
    EXPECT_NE(filter.getChildren()[0], nullptr);
}

TEST_F(LogicalFilterTest, OutputVariablesFromChild) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    auto pred = makeLiteralExpr(true);
    LogicalFilter filter(std::move(child), pred);

    auto vars = filter.getOutputVariables();
    ASSERT_EQ(vars.size(), 1u);
    EXPECT_EQ(vars[0], "n");
}

TEST_F(LogicalFilterTest, DetachAndSetChild) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    auto pred = makeLiteralExpr(true);
    LogicalFilter filter(std::move(child), pred);

    auto detached = filter.detachChild(0);
    ASSERT_NE(detached, nullptr);
    EXPECT_EQ(detached->getType(), LogicalOpType::LOP_NODE_SCAN);

    // After detach, child should be null
    EXPECT_EQ(filter.getChildren()[0], nullptr);

    // Set it back
    filter.setChild(0, std::move(detached));
    EXPECT_NE(filter.getChildren()[0], nullptr);
}

TEST_F(LogicalFilterTest, OutOfRangeChild) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    auto pred = makeLiteralExpr(true);
    LogicalFilter filter(std::move(child), pred);

    EXPECT_THROW(filter.setChild(1, std::make_unique<LogicalNodeScan>("m")), std::out_of_range);
    EXPECT_THROW(filter.detachChild(1), std::out_of_range);
}

TEST_F(LogicalFilterTest, Clone) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    auto pred = makePropertyEqualityExpr("n", "age", 25);
    LogicalFilter filter(std::move(child), pred);

    auto cloned = filter.clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->getType(), LogicalOpType::LOP_FILTER);

    auto *clonedFilter = dynamic_cast<LogicalFilter *>(cloned.get());
    ASSERT_NE(clonedFilter, nullptr);
    EXPECT_NE(clonedFilter->getPredicate(), nullptr);
    ASSERT_EQ(clonedFilter->getChildren().size(), 1u);
}

TEST_F(LogicalFilterTest, ToStringContainsPredicate) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    auto pred = makeLiteralExpr(true);
    LogicalFilter filter(std::move(child), pred);

    auto str = filter.toString();
    EXPECT_NE(str.find("Filter"), std::string::npos);
}

// =============================================================================
// LogicalProject
// =============================================================================

class LogicalProjectTest : public ::testing::Test {};

TEST_F(LogicalProjectTest, BasicConstruction) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<LogicalProjectItem> items;
    items.emplace_back(makeVarRefExpr("n"), "n");
    LogicalProject project(std::move(child), std::move(items));

    EXPECT_EQ(project.getType(), LogicalOpType::LOP_PROJECT);
    EXPECT_FALSE(project.isDistinct());
    EXPECT_EQ(project.getItems().size(), 1u);
}

TEST_F(LogicalProjectTest, DistinctFlag) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<LogicalProjectItem> items;
    items.emplace_back(makeVarRefExpr("n"), "n");
    LogicalProject project(std::move(child), std::move(items), true);

    EXPECT_TRUE(project.isDistinct());
    EXPECT_NE(project.toString().find("Distinct"), std::string::npos);
}

TEST_F(LogicalProjectTest, OutputVariablesMatchAliases) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<LogicalProjectItem> items;
    items.emplace_back(makePropertyEqualityExpr("n", "name", 0), "personName");
    items.emplace_back(makePropertyEqualityExpr("n", "age", 0), "personAge");
    LogicalProject project(std::move(child), std::move(items));

    auto vars = project.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "personName");
    EXPECT_EQ(vars[1], "personAge");
}

TEST_F(LogicalProjectTest, RequiredColumns) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<LogicalProjectItem> items;
    items.emplace_back(makeVarRefExpr("n"), "n");
    LogicalProject project(std::move(child), std::move(items));

    EXPECT_TRUE(project.getRequiredColumns().empty());

    std::unordered_set<std::string> cols = {"n"};
    project.setRequiredColumns(cols);
    EXPECT_EQ(project.getRequiredColumns().size(), 1u);
    EXPECT_NE(project.getRequiredColumns().find("n"), project.getRequiredColumns().end());
}

TEST_F(LogicalProjectTest, Clone) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<LogicalProjectItem> items;
    items.emplace_back(makeVarRefExpr("n"), "n");
    LogicalProject project(std::move(child), std::move(items));

    std::unordered_set<std::string> cols = {"n"};
    project.setRequiredColumns(cols);

    auto cloned = project.clone();
    auto *clonedProj = dynamic_cast<LogicalProject *>(cloned.get());
    ASSERT_NE(clonedProj, nullptr);
    EXPECT_EQ(clonedProj->getItems().size(), 1u);
    EXPECT_EQ(clonedProj->getRequiredColumns().size(), 1u);
}

// =============================================================================
// LogicalJoin
// =============================================================================

class LogicalJoinTest : public ::testing::Test {};

TEST_F(LogicalJoinTest, BasicConstruction) {
    auto left = std::make_unique<LogicalNodeScan>("n");
    auto right = std::make_unique<LogicalNodeScan>("m");
    LogicalJoin join(std::move(left), std::move(right));

    EXPECT_EQ(join.getType(), LogicalOpType::LOP_JOIN);
    ASSERT_EQ(join.getChildren().size(), 2u);
    EXPECT_NE(join.getLeft(), nullptr);
    EXPECT_NE(join.getRight(), nullptr);
}

TEST_F(LogicalJoinTest, OutputVariablesCombined) {
    auto left = std::make_unique<LogicalNodeScan>("n");
    auto right = std::make_unique<LogicalNodeScan>("m");
    LogicalJoin join(std::move(left), std::move(right));

    auto vars = join.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "n");
    EXPECT_EQ(vars[1], "m");
}

TEST_F(LogicalJoinTest, DetachChildren) {
    auto left = std::make_unique<LogicalNodeScan>("n");
    auto right = std::make_unique<LogicalNodeScan>("m");
    LogicalJoin join(std::move(left), std::move(right));

    auto detachedLeft = join.detachChild(0);
    ASSERT_NE(detachedLeft, nullptr);
    EXPECT_EQ(join.getLeft(), nullptr);

    auto detachedRight = join.detachChild(1);
    ASSERT_NE(detachedRight, nullptr);
    EXPECT_EQ(join.getRight(), nullptr);
}

TEST_F(LogicalJoinTest, SetChildren) {
    auto left = std::make_unique<LogicalNodeScan>("n");
    auto right = std::make_unique<LogicalNodeScan>("m");
    LogicalJoin join(std::move(left), std::move(right));

    join.setChild(0, std::make_unique<LogicalNodeScan>("a"));
    join.setChild(1, std::make_unique<LogicalNodeScan>("b"));

    auto vars = join.getOutputVariables();
    EXPECT_EQ(vars[0], "a");
    EXPECT_EQ(vars[1], "b");
}

TEST_F(LogicalJoinTest, OutOfRangeChild) {
    auto left = std::make_unique<LogicalNodeScan>("n");
    auto right = std::make_unique<LogicalNodeScan>("m");
    LogicalJoin join(std::move(left), std::move(right));

    EXPECT_THROW(join.setChild(2, nullptr), std::out_of_range);
    EXPECT_THROW(join.detachChild(2), std::out_of_range);
}

TEST_F(LogicalJoinTest, Clone) {
    auto left = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
    auto right = std::make_unique<LogicalNodeScan>("m", std::vector<std::string>{"Company"});
    LogicalJoin join(std::move(left), std::move(right));

    auto cloned = join.clone();
    auto *clonedJoin = dynamic_cast<LogicalJoin *>(cloned.get());
    ASSERT_NE(clonedJoin, nullptr);
    EXPECT_NE(clonedJoin->getLeft(), nullptr);
    EXPECT_NE(clonedJoin->getRight(), nullptr);
}

// =============================================================================
// LogicalSort
// =============================================================================

class LogicalSortTest : public ::testing::Test {};

TEST_F(LogicalSortTest, BasicConstruction) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<LogicalSortItem> sortItems;
    sortItems.emplace_back(makeVarRefExpr("n"), true);
    LogicalSort sort(std::move(child), std::move(sortItems));

    EXPECT_EQ(sort.getType(), LogicalOpType::LOP_SORT);
    EXPECT_EQ(sort.getSortItems().size(), 1u);
    EXPECT_TRUE(sort.getSortItems()[0].ascending);
}

TEST_F(LogicalSortTest, DescendingSort) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<LogicalSortItem> sortItems;
    sortItems.emplace_back(makeVarRefExpr("n"), false);
    LogicalSort sort(std::move(child), std::move(sortItems));

    EXPECT_FALSE(sort.getSortItems()[0].ascending);
}

TEST_F(LogicalSortTest, OutputVariablesPassThrough) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<LogicalSortItem> sortItems;
    sortItems.emplace_back(makeVarRefExpr("n"), true);
    LogicalSort sort(std::move(child), std::move(sortItems));

    auto vars = sort.getOutputVariables();
    ASSERT_EQ(vars.size(), 1u);
    EXPECT_EQ(vars[0], "n");
}

TEST_F(LogicalSortTest, Clone) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<LogicalSortItem> sortItems;
    sortItems.emplace_back(makeVarRefExpr("n"), false);
    LogicalSort sort(std::move(child), std::move(sortItems));

    auto cloned = sort.clone();
    auto *clonedSort = dynamic_cast<LogicalSort *>(cloned.get());
    ASSERT_NE(clonedSort, nullptr);
    EXPECT_EQ(clonedSort->getSortItems().size(), 1u);
    EXPECT_FALSE(clonedSort->getSortItems()[0].ascending);
}

// =============================================================================
// LogicalAggregate
// =============================================================================

class LogicalAggregateTest : public ::testing::Test {};

TEST_F(LogicalAggregateTest, BasicConstruction) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<std::shared_ptr<Expression>> groupByExprs;
    groupByExprs.push_back(makeVarRefExpr("n"));

    std::vector<LogicalAggItem> aggItems;
    aggItems.emplace_back("count", nullptr, "cnt", false);

    std::vector<std::string> aliases = {"n"};
    LogicalAggregate agg(std::move(child), std::move(groupByExprs),
                         std::move(aggItems), std::move(aliases));

    EXPECT_EQ(agg.getType(), LogicalOpType::LOP_AGGREGATE);
    EXPECT_EQ(agg.getGroupByExprs().size(), 1u);
    EXPECT_EQ(agg.getAggregations().size(), 1u);
    EXPECT_EQ(agg.getGroupByAliases().size(), 1u);
}

TEST_F(LogicalAggregateTest, AggItemProperties) {
    LogicalAggItem item("sum", makeVarRefExpr("n"), "total", true);
    EXPECT_EQ(item.functionName, "sum");
    EXPECT_EQ(item.alias, "total");
    EXPECT_TRUE(item.distinct);
    EXPECT_NE(item.argument, nullptr);
}

TEST_F(LogicalAggregateTest, OutputVariablesIncludesGroupByAndAggs) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<std::shared_ptr<Expression>> groupByExprs;
    groupByExprs.push_back(makeVarRefExpr("n"));

    std::vector<LogicalAggItem> aggItems;
    aggItems.emplace_back("count", nullptr, "cnt", false);

    LogicalAggregate agg(std::move(child), std::move(groupByExprs), std::move(aggItems));

    auto vars = agg.getOutputVariables();
    // Should have group-by expr toString() + aggregate alias
    EXPECT_GE(vars.size(), 2u);
}

TEST_F(LogicalAggregateTest, Clone) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<std::shared_ptr<Expression>> groupByExprs;
    groupByExprs.push_back(makeVarRefExpr("n"));

    std::vector<LogicalAggItem> aggItems;
    aggItems.emplace_back("count", nullptr, "cnt", false);

    std::vector<std::string> aliases = {"n"};
    LogicalAggregate agg(std::move(child), std::move(groupByExprs),
                         std::move(aggItems), std::move(aliases));

    auto cloned = agg.clone();
    auto *clonedAgg = dynamic_cast<LogicalAggregate *>(cloned.get());
    ASSERT_NE(clonedAgg, nullptr);
    EXPECT_EQ(clonedAgg->getGroupByExprs().size(), 1u);
    EXPECT_EQ(clonedAgg->getAggregations().size(), 1u);
    EXPECT_EQ(clonedAgg->getGroupByAliases().size(), 1u);
}

// =============================================================================
// LogicalLimit and LogicalSkip
// =============================================================================

TEST(LogicalLimitTest, BasicConstruction) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalLimit limit(std::move(child), 10);

    EXPECT_EQ(limit.getType(), LogicalOpType::LOP_LIMIT);
    EXPECT_EQ(limit.getLimit(), 10);
}

TEST(LogicalSkipTest, BasicConstruction) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalSkip skip(std::move(child), 5);

    EXPECT_EQ(skip.getType(), LogicalOpType::LOP_SKIP);
    EXPECT_EQ(skip.getOffset(), 5);
}

// =============================================================================
// LogicalSingleRow
// =============================================================================

TEST(LogicalSingleRowTest, IsLeaf) {
    LogicalSingleRow sr;
    EXPECT_EQ(sr.getType(), LogicalOpType::LOP_SINGLE_ROW);
    EXPECT_TRUE(sr.getChildren().empty());
    EXPECT_TRUE(sr.getOutputVariables().empty());
}

TEST(LogicalSingleRowTest, Clone) {
    LogicalSingleRow sr;
    auto cloned = sr.clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->getType(), LogicalOpType::LOP_SINGLE_ROW);
}

// =============================================================================
// LogicalUnwind
// =============================================================================

class LogicalUnwindTest : public ::testing::Test {};

TEST_F(LogicalUnwindTest, LiteralListConstruction) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<graph::PropertyValue> vals;
    vals.emplace_back(int64_t(1));
    vals.emplace_back(int64_t(2));
    LogicalUnwind unwind(std::move(child), "x", std::move(vals));

    EXPECT_EQ(unwind.getType(), LogicalOpType::LOP_UNWIND);
    EXPECT_EQ(unwind.getAlias(), "x");
    EXPECT_TRUE(unwind.hasLiteralList());
    EXPECT_EQ(unwind.getListValues().size(), 2u);
    EXPECT_EQ(unwind.getListExpr(), nullptr);
}

TEST_F(LogicalUnwindTest, ExpressionConstruction) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    auto expr = makeVarRefExpr("myList");
    LogicalUnwind unwind(std::move(child), "elem", expr);

    EXPECT_FALSE(unwind.hasLiteralList());
    EXPECT_NE(unwind.getListExpr(), nullptr);
    EXPECT_TRUE(unwind.getListValues().empty());
}

TEST_F(LogicalUnwindTest, OutputVariablesIncludesAlias) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<graph::PropertyValue> vals;
    vals.emplace_back(int64_t(1));
    LogicalUnwind unwind(std::move(child), "x", std::move(vals));

    auto vars = unwind.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "n");
    EXPECT_EQ(vars[1], "x");
}

TEST_F(LogicalUnwindTest, CloneLiteralList) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<graph::PropertyValue> vals;
    vals.emplace_back(int64_t(42));
    LogicalUnwind unwind(std::move(child), "x", std::move(vals));

    auto cloned = unwind.clone();
    auto *cu = dynamic_cast<LogicalUnwind *>(cloned.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_TRUE(cu->hasLiteralList());
    EXPECT_EQ(cu->getAlias(), "x");
    EXPECT_EQ(cu->getListValues().size(), 1u);
}

TEST_F(LogicalUnwindTest, CloneExpression) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    auto expr = makeVarRefExpr("myList");
    LogicalUnwind unwind(std::move(child), "elem", expr);

    auto cloned = unwind.clone();
    auto *cu = dynamic_cast<LogicalUnwind *>(cloned.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_FALSE(cu->hasLiteralList());
    EXPECT_NE(cu->getListExpr(), nullptr);
}

TEST_F(LogicalUnwindTest, SetAndDetachChild) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<graph::PropertyValue> vals;
    LogicalUnwind unwind(std::move(child), "x", std::move(vals));

    auto detached = unwind.detachChild(0);
    ASSERT_NE(detached, nullptr);
    EXPECT_EQ(unwind.getChildren()[0], nullptr);

    unwind.setChild(0, std::move(detached));
    EXPECT_NE(unwind.getChildren()[0], nullptr);
}

TEST_F(LogicalUnwindTest, OutOfRangeChild) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<graph::PropertyValue> vals;
    LogicalUnwind unwind(std::move(child), "x", std::move(vals));

    EXPECT_THROW(unwind.setChild(1, nullptr), std::out_of_range);
    EXPECT_THROW(unwind.detachChild(1), std::out_of_range);
}

TEST_F(LogicalUnwindTest, ToString) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<graph::PropertyValue> vals;
    LogicalUnwind unwind(std::move(child), "x", std::move(vals));

    EXPECT_EQ(unwind.toString(), "Unwind(x)");
}

// =============================================================================
// LogicalOptionalMatch
// =============================================================================

class LogicalOptionalMatchTest : public ::testing::Test {};

TEST_F(LogicalOptionalMatchTest, BasicConstruction) {
    auto input = std::make_unique<LogicalNodeScan>("n");
    auto optional = std::make_unique<LogicalNodeScan>("m");
    LogicalOptionalMatch om(std::move(input), std::move(optional));

    EXPECT_EQ(om.getType(), LogicalOpType::LOP_OPTIONAL_MATCH);
    ASSERT_EQ(om.getChildren().size(), 2u);
    EXPECT_NE(om.getInput(), nullptr);
    EXPECT_NE(om.getOptionalPattern(), nullptr);
    EXPECT_TRUE(om.getRequiredVariables().empty());
}

TEST_F(LogicalOptionalMatchTest, RequiredVariables) {
    auto input = std::make_unique<LogicalNodeScan>("n");
    auto optional = std::make_unique<LogicalNodeScan>("m");
    LogicalOptionalMatch om(std::move(input), std::move(optional), {"n"});

    ASSERT_EQ(om.getRequiredVariables().size(), 1u);
    EXPECT_EQ(om.getRequiredVariables()[0], "n");
}

TEST_F(LogicalOptionalMatchTest, OutputVariablesBothChildren) {
    auto input = std::make_unique<LogicalNodeScan>("n");
    auto optional = std::make_unique<LogicalNodeScan>("m");
    LogicalOptionalMatch om(std::move(input), std::move(optional));

    auto vars = om.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "n");
    EXPECT_EQ(vars[1], "m");
}

TEST_F(LogicalOptionalMatchTest, OutputVariablesWithNullChildren) {
    LogicalOptionalMatch om(nullptr, nullptr);
    auto vars = om.getOutputVariables();
    EXPECT_TRUE(vars.empty());
}

TEST_F(LogicalOptionalMatchTest, OutputVariablesOneChildNull) {
    auto input = std::make_unique<LogicalNodeScan>("n");
    LogicalOptionalMatch om(std::move(input), nullptr);
    auto vars = om.getOutputVariables();
    ASSERT_EQ(vars.size(), 1u);
    EXPECT_EQ(vars[0], "n");
}

TEST_F(LogicalOptionalMatchTest, SetAndDetachChildren) {
    auto input = std::make_unique<LogicalNodeScan>("n");
    auto optional = std::make_unique<LogicalNodeScan>("m");
    LogicalOptionalMatch om(std::move(input), std::move(optional));

    auto d0 = om.detachChild(0);
    ASSERT_NE(d0, nullptr);
    EXPECT_EQ(om.getInput(), nullptr);

    auto d1 = om.detachChild(1);
    ASSERT_NE(d1, nullptr);
    EXPECT_EQ(om.getOptionalPattern(), nullptr);

    om.setChild(0, std::move(d0));
    om.setChild(1, std::move(d1));
    EXPECT_NE(om.getInput(), nullptr);
    EXPECT_NE(om.getOptionalPattern(), nullptr);
}

TEST_F(LogicalOptionalMatchTest, OutOfRangeChild) {
    LogicalOptionalMatch om(nullptr, nullptr);
    EXPECT_THROW(om.setChild(2, nullptr), std::out_of_range);
    EXPECT_THROW(om.detachChild(2), std::out_of_range);
}

TEST_F(LogicalOptionalMatchTest, Clone) {
    auto input = std::make_unique<LogicalNodeScan>("n");
    auto optional = std::make_unique<LogicalNodeScan>("m");
    LogicalOptionalMatch om(std::move(input), std::move(optional), {"n"});

    auto cloned = om.clone();
    auto *com = dynamic_cast<LogicalOptionalMatch *>(cloned.get());
    ASSERT_NE(com, nullptr);
    EXPECT_NE(com->getInput(), nullptr);
    EXPECT_NE(com->getOptionalPattern(), nullptr);
    ASSERT_EQ(com->getRequiredVariables().size(), 1u);
    EXPECT_EQ(com->getRequiredVariables()[0], "n");
}

TEST_F(LogicalOptionalMatchTest, CloneWithNullChildren) {
    LogicalOptionalMatch om(nullptr, nullptr);
    auto cloned = om.clone();
    auto *com = dynamic_cast<LogicalOptionalMatch *>(cloned.get());
    ASSERT_NE(com, nullptr);
    EXPECT_EQ(com->getInput(), nullptr);
    EXPECT_EQ(com->getOptionalPattern(), nullptr);
}

TEST_F(LogicalOptionalMatchTest, ToString) {
    LogicalOptionalMatch om(nullptr, nullptr);
    EXPECT_EQ(om.toString(), "OptionalMatch");
}

// =============================================================================
// LogicalUnion
// =============================================================================

class LogicalUnionTest : public ::testing::Test {};

TEST_F(LogicalUnionTest, UnionAllConstruction) {
    auto left = std::make_unique<LogicalNodeScan>("n");
    auto right = std::make_unique<LogicalNodeScan>("n");
    LogicalUnion u(std::move(left), std::move(right), true);

    EXPECT_EQ(u.getType(), LogicalOpType::LOP_UNION);
    EXPECT_TRUE(u.isAll());
    EXPECT_NE(u.getLeft(), nullptr);
    EXPECT_NE(u.getRight(), nullptr);
}

TEST_F(LogicalUnionTest, UnionDistinctConstruction) {
    auto left = std::make_unique<LogicalNodeScan>("n");
    auto right = std::make_unique<LogicalNodeScan>("n");
    LogicalUnion u(std::move(left), std::move(right), false);

    EXPECT_FALSE(u.isAll());
}

TEST_F(LogicalUnionTest, ToStringDifference) {
    auto left1 = std::make_unique<LogicalNodeScan>("n");
    auto right1 = std::make_unique<LogicalNodeScan>("n");
    LogicalUnion unionAll(std::move(left1), std::move(right1), true);
    EXPECT_EQ(unionAll.toString(), "UnionAll");

    auto left2 = std::make_unique<LogicalNodeScan>("n");
    auto right2 = std::make_unique<LogicalNodeScan>("n");
    LogicalUnion unionDistinct(std::move(left2), std::move(right2), false);
    EXPECT_EQ(unionDistinct.toString(), "Union");
}

TEST_F(LogicalUnionTest, OutputVariablesFromLeft) {
    auto left = std::make_unique<LogicalNodeScan>("n");
    auto right = std::make_unique<LogicalNodeScan>("m");
    LogicalUnion u(std::move(left), std::move(right), true);

    auto vars = u.getOutputVariables();
    ASSERT_EQ(vars.size(), 1u);
    EXPECT_EQ(vars[0], "n");
}

TEST_F(LogicalUnionTest, OutputVariablesNullLeft) {
    LogicalUnion u(nullptr, std::make_unique<LogicalNodeScan>("m"), true);
    EXPECT_TRUE(u.getOutputVariables().empty());
}

TEST_F(LogicalUnionTest, SetAndDetachChildren) {
    auto left = std::make_unique<LogicalNodeScan>("n");
    auto right = std::make_unique<LogicalNodeScan>("m");
    LogicalUnion u(std::move(left), std::move(right), true);

    auto d0 = u.detachChild(0);
    ASSERT_NE(d0, nullptr);
    EXPECT_EQ(u.getLeft(), nullptr);

    auto d1 = u.detachChild(1);
    ASSERT_NE(d1, nullptr);
    EXPECT_EQ(u.getRight(), nullptr);

    u.setChild(0, std::move(d0));
    u.setChild(1, std::move(d1));
    EXPECT_NE(u.getLeft(), nullptr);
    EXPECT_NE(u.getRight(), nullptr);
}

TEST_F(LogicalUnionTest, OutOfRangeChild) {
    LogicalUnion u(nullptr, nullptr, true);
    EXPECT_THROW(u.setChild(2, nullptr), std::out_of_range);
    EXPECT_THROW(u.detachChild(2), std::out_of_range);
}

TEST_F(LogicalUnionTest, Clone) {
    auto left = std::make_unique<LogicalNodeScan>("n");
    auto right = std::make_unique<LogicalNodeScan>("m");
    LogicalUnion u(std::move(left), std::move(right), true);

    auto cloned = u.clone();
    auto *cu = dynamic_cast<LogicalUnion *>(cloned.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_TRUE(cu->isAll());
    EXPECT_NE(cu->getLeft(), nullptr);
    EXPECT_NE(cu->getRight(), nullptr);
}

TEST_F(LogicalUnionTest, CloneWithNullChildren) {
    LogicalUnion u(nullptr, nullptr, false);
    auto cloned = u.clone();
    auto *cu = dynamic_cast<LogicalUnion *>(cloned.get());
    ASSERT_NE(cu, nullptr);
    EXPECT_FALSE(cu->isAll());
    EXPECT_EQ(cu->getLeft(), nullptr);
    EXPECT_EQ(cu->getRight(), nullptr);
}

// =============================================================================
// Deep tree: multi-level clone
// =============================================================================

TEST(LogicalTreeTest, DeepTreeClone) {
    // Build: Filter(predicate) -> Project(items) -> NodeScan("n")
    auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});

    std::vector<LogicalProjectItem> items;
    items.emplace_back(makeVarRefExpr("n"), "n");
    auto project = std::make_unique<LogicalProject>(std::move(scan), std::move(items));

    auto pred = makeLiteralExpr(true);
    auto filter = std::make_unique<LogicalFilter>(std::move(project), pred);

    // Clone the entire tree
    auto cloned = filter->clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->getType(), LogicalOpType::LOP_FILTER);

    auto *clonedFilter = dynamic_cast<LogicalFilter *>(cloned.get());
    ASSERT_NE(clonedFilter, nullptr);
    ASSERT_EQ(clonedFilter->getChildren().size(), 1u);

    auto *childProject = dynamic_cast<LogicalProject *>(clonedFilter->getChildren()[0]);
    ASSERT_NE(childProject, nullptr);
    ASSERT_EQ(childProject->getChildren().size(), 1u);

    auto *leafScan = dynamic_cast<LogicalNodeScan *>(childProject->getChildren()[0]);
    ASSERT_NE(leafScan, nullptr);
    EXPECT_EQ(leafScan->getVariable(), "n");
}
