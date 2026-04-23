/**
 * @file test_ExpressionUtils_Coverage.cpp
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include <gtest/gtest.h>
#include "graph/query/expressions/ExpressionUtils.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ReduceExpression.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/ShortestPathExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"

#include <set>
#include <unordered_set>

using namespace graph::query::expressions;
using namespace graph;
using ComprehensionType = ListComprehensionExpression::ComprehensionType;

static std::unique_ptr<Expression> lit(int64_t v) {
	return std::make_unique<LiteralExpression>(v);
}

static std::unique_ptr<Expression> var(const std::string& name) {
	return std::make_unique<VariableReferenceExpression>(name);
}

static std::unique_ptr<Expression> aggCall(const std::string& fn,
                                           std::unique_ptr<Expression> arg,
                                           bool distinct = false) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::move(arg));
	return std::make_unique<FunctionCallExpression>(fn, std::move(args), distinct);
}

class ExpressionUtilsCoverageTest : public ::testing::Test {};

// ============================================================================
// isAggregateFunction: stdev, stdevp, percentiledisc, percentilecont
// ============================================================================

TEST_F(ExpressionUtilsCoverageTest, IsAggregate_Stdev) {
	EXPECT_TRUE(ExpressionUtils::isAggregateFunction("stdev"));
	EXPECT_TRUE(ExpressionUtils::isAggregateFunction("stdevp"));
	EXPECT_TRUE(ExpressionUtils::isAggregateFunction("percentiledisc"));
	EXPECT_TRUE(ExpressionUtils::isAggregateFunction("percentilecont"));
	EXPECT_TRUE(ExpressionUtils::isAggregateFunction("STDEV"));
	EXPECT_TRUE(ExpressionUtils::isAggregateFunction("PERCENTILEDISC"));
}

// ============================================================================
// containsAggregate: ListSlice with agg in start/end
// ============================================================================

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_ListSlice_AggInList) {
	auto slice = std::make_unique<ListSliceExpression>(
		aggCall("count", var("x")), lit(0), lit(1), true);
	EXPECT_TRUE(ExpressionUtils::containsAggregate(slice.get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_ListSlice_AggInStart) {
	auto slice = std::make_unique<ListSliceExpression>(
		var("list"), aggCall("count", var("x")), lit(1), true);
	EXPECT_TRUE(ExpressionUtils::containsAggregate(slice.get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_ListSlice_AggInEnd) {
	auto slice = std::make_unique<ListSliceExpression>(
		var("list"), lit(0), aggCall("count", var("x")), true);
	EXPECT_TRUE(ExpressionUtils::containsAggregate(slice.get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_ListSlice_NoAgg) {
	auto slice = std::make_unique<ListSliceExpression>(
		var("list"), lit(0), lit(1), true);
	EXPECT_FALSE(ExpressionUtils::containsAggregate(slice.get()));
}

// ============================================================================
// containsAggregate: ListComprehension with agg in list/where/map
// ============================================================================

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_ListComp_AggInList) {
	auto comp = std::make_unique<ListComprehensionExpression>(
		"x", aggCall("count", var("y")), nullptr, nullptr, ComprehensionType::COMP_FILTER);
	EXPECT_TRUE(ExpressionUtils::containsAggregate(comp.get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_ListComp_AggInWhere) {
	auto comp = std::make_unique<ListComprehensionExpression>(
		"x", var("list"), aggCall("count", var("y")), nullptr, ComprehensionType::COMP_FILTER);
	EXPECT_TRUE(ExpressionUtils::containsAggregate(comp.get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_ListComp_AggInMap) {
	auto comp = std::make_unique<ListComprehensionExpression>(
		"x", var("list"), nullptr, aggCall("count", var("y")), ComprehensionType::COMP_EXTRACT);
	EXPECT_TRUE(ExpressionUtils::containsAggregate(comp.get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_ListComp_NoAgg) {
	auto comp = std::make_unique<ListComprehensionExpression>(
		"x", var("list"), nullptr, nullptr, ComprehensionType::COMP_FILTER);
	EXPECT_FALSE(ExpressionUtils::containsAggregate(comp.get()));
}

// ============================================================================
// containsAggregate: Quantifier with agg in list/where
// ============================================================================

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_Quantifier_AggInList) {
	auto quant = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", aggCall("count", var("y")), var("pred"));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(quant.get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_Quantifier_AggInWhere) {
	auto quant = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", var("list"), aggCall("count", var("y")));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(quant.get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_Quantifier_NoAgg) {
	auto quant = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", var("list"), var("pred"));
	EXPECT_FALSE(ExpressionUtils::containsAggregate(quant.get()));
}

// ============================================================================
// containsAggregate: Reduce with agg in init/list/body
// ============================================================================

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_Reduce_AggInInit) {
	auto reduce = std::make_unique<ReduceExpression>(
		"acc", aggCall("count", var("x")), "x", var("list"), var("body"));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(reduce.get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_Reduce_AggInList) {
	auto reduce = std::make_unique<ReduceExpression>(
		"acc", lit(0), "x", aggCall("count", var("x")), var("body"));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(reduce.get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_Reduce_AggInBody) {
	auto reduce = std::make_unique<ReduceExpression>(
		"acc", lit(0), "x", var("list"), aggCall("count", var("x")));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(reduce.get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_Reduce_NoAgg) {
	auto reduce = std::make_unique<ReduceExpression>(
		"acc", lit(0), "x", var("list"), var("body"));
	EXPECT_FALSE(ExpressionUtils::containsAggregate(reduce.get()));
}

// ============================================================================
// containsAggregate: InList with agg in value (not dynamic)
// ============================================================================

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_InList_AggInValue) {
	std::vector<PropertyValue> vals = {PropertyValue(1)};
	auto inExpr = std::make_unique<InExpression>(aggCall("count", var("x")), vals);
	EXPECT_TRUE(ExpressionUtils::containsAggregate(inExpr.get()));
}

// ============================================================================
// containsAggregate: FunctionCall with non-agg but agg in args
// ============================================================================

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_FuncCall_AggInArgs) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(aggCall("sum", var("x")));
	auto func = std::make_unique<FunctionCallExpression>("upper", std::move(args), false);
	EXPECT_TRUE(ExpressionUtils::containsAggregate(func.get()));
}

// ============================================================================
// containsAggregate: CaseExpression with agg in else
// ============================================================================

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_CaseElse) {
	auto caseExpr = std::make_unique<CaseExpression>();
	caseExpr->addBranch(lit(1), lit(2));
	caseExpr->setElseExpression(aggCall("count", var("x")));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(caseExpr.get()));
}

// containsAggregate: Variable reference, parameter, shortest path, list literal
TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_VariableRef) {
	EXPECT_FALSE(ExpressionUtils::containsAggregate(var("x").get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_Parameter) {
	auto param = std::make_unique<ParameterExpression>("p");
	EXPECT_FALSE(ExpressionUtils::containsAggregate(param.get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_ShortestPath) {
	auto sp = std::make_unique<ShortestPathExpression>(
		"a", "b", "", PatternDirection::PAT_BOTH, 1, -1, false);
	EXPECT_FALSE(ExpressionUtils::containsAggregate(sp.get()));
}

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_ListLiteral) {
	auto ll = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{}));
	EXPECT_FALSE(ExpressionUtils::containsAggregate(ll.get()));
}

// ============================================================================
// containsAggregate: PatternComprehension with no agg in map, no where
// ============================================================================

TEST_F(ExpressionUtilsCoverageTest, ContainsAggregate_PatComp_NoAgg_NoWhere) {
	auto e = std::make_unique<PatternComprehensionExpression>(
		"(n)-[:R]->(m)", "n", "m", "R", "Person",
		PatternDirection::PAT_OUTGOING, var("m"));
	EXPECT_FALSE(ExpressionUtils::containsAggregate(e.get()));
}

// ============================================================================
// collectVariables: Parameter and ListLiteral
// ============================================================================

TEST_F(ExpressionUtilsCoverageTest, CollectVars_Parameter) {
	std::set<std::string> s;
	auto param = std::make_unique<ParameterExpression>("p");
	ExpressionUtils::collectVariables(param.get(), s);
	EXPECT_TRUE(s.empty());
}

TEST_F(ExpressionUtilsCoverageTest, CollectVars_ListLiteral) {
	std::set<std::string> s;
	auto ll = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{}));
	ExpressionUtils::collectVariables(ll.get(), s);
	EXPECT_TRUE(s.empty());
}

// unordered_set versions of the same
TEST_F(ExpressionUtilsCoverageTest, CollectVars_Unordered_Parameter) {
	std::unordered_set<std::string> s;
	auto param = std::make_unique<ParameterExpression>("p");
	ExpressionUtils::collectVariables(param.get(), s);
	EXPECT_TRUE(s.empty());
}

TEST_F(ExpressionUtilsCoverageTest, CollectVars_Unordered_ListLiteral) {
	std::unordered_set<std::string> s;
	auto ll = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{}));
	ExpressionUtils::collectVariables(ll.get(), s);
	EXPECT_TRUE(s.empty());
}

// collectVariables: Exists without where, empty sourceVar
TEST_F(ExpressionUtilsCoverageTest, CollectVars_Exists_NoWhere_EmptySource) {
	std::set<std::string> s;
	auto e = std::make_unique<ExistsExpression>("(n)-[:R]->(m)");
	ExpressionUtils::collectVariables(e.get(), s);
	// sourceVar is empty for raw pattern string
	EXPECT_TRUE(s.empty());
}

// collectVariables: PatternComprehension without where, empty sourceVar
TEST_F(ExpressionUtilsCoverageTest, CollectVars_PatComp_NoWhere) {
	std::set<std::string> s;
	auto e = std::make_unique<PatternComprehensionExpression>(
		"(n)-[:R]->(m)", "n", "m", "R", "Person",
		PatternDirection::PAT_OUTGOING, var("mapExpr"));
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("n"));
	EXPECT_TRUE(s.count("mapExpr"));
}

// collectVariables: CaseExpression without comparison and without else
TEST_F(ExpressionUtilsCoverageTest, CollectVars_Case_NoComparison_NoElse) {
	std::set<std::string> s;
	auto caseExpr = std::make_unique<CaseExpression>();
	caseExpr->addBranch(var("a"), var("b"));
	ExpressionUtils::collectVariables(caseExpr.get(), s);
	EXPECT_TRUE(s.count("a"));
	EXPECT_TRUE(s.count("b"));
}

// collectVariables: MapProjection with no expression in items
TEST_F(ExpressionUtilsCoverageTest, CollectVars_MapProjection_NoExpression) {
	std::set<std::string> s;
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_PROPERTY, "name");
	auto e = std::make_unique<MapProjectionExpression>("n", std::move(items));
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("n"));
	EXPECT_EQ(s.size(), 1u);
}

// ============================================================================
// extractAndReplaceAggregates: percentile functions with extra arg
// ============================================================================

TEST_F(ExpressionUtilsCoverageTest, ExtractAgg_PercentileDisc) {
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(var("x"));
	args.push_back(std::make_unique<LiteralExpression>(0.5));
	auto e = std::make_unique<FunctionCallExpression>("percentiledisc", std::move(args), false);
	auto result = ExpressionUtils::extractAndReplaceAggregates(e.get(), items, counter);
	ASSERT_EQ(items.size(), 1u);
	EXPECT_EQ(items[0].functionName, "percentiledisc");
	EXPECT_NE(items[0].extraArg, nullptr);
}

TEST_F(ExpressionUtilsCoverageTest, ExtractAgg_PercentileCont) {
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(var("x"));
	args.push_back(std::make_unique<LiteralExpression>(0.75));
	auto e = std::make_unique<FunctionCallExpression>("percentilecont", std::move(args), false);
	auto result = ExpressionUtils::extractAndReplaceAggregates(e.get(), items, counter);
	ASSERT_EQ(items.size(), 1u);
	EXPECT_EQ(items[0].functionName, "percentilecont");
	EXPECT_NE(items[0].extraArg, nullptr);
}

// extractAndReplaceAggregates: agg with 0 args (count(*))
TEST_F(ExpressionUtilsCoverageTest, ExtractAgg_CountStar) {
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	std::vector<std::unique_ptr<Expression>> args;
	auto e = std::make_unique<FunctionCallExpression>("count", std::move(args), false);
	auto result = ExpressionUtils::extractAndReplaceAggregates(e.get(), items, counter);
	ASSERT_EQ(items.size(), 1u);
	EXPECT_EQ(items[0].functionName, "count");
	EXPECT_EQ(items[0].argument, nullptr);
}
