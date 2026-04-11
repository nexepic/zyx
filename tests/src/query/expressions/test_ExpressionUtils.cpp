/**
 * @file test_ExpressionUtils.cpp
 * @brief Unit tests for ExpressionUtils: aggregate detection, variable
 *        collection, and aggregate extraction/replacement.
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

// ============================================================================
// Helpers — small expression builders
// ============================================================================

static std::unique_ptr<Expression> lit(int64_t v) {
	return std::make_unique<LiteralExpression>(v);
}

static std::unique_ptr<Expression> var(const std::string& name) {
	return std::make_unique<VariableReferenceExpression>(name);
}

static std::unique_ptr<Expression> prop(const std::string& varName,
                                        const std::string& propName) {
	return std::make_unique<VariableReferenceExpression>(varName, propName);
}

static std::unique_ptr<Expression> aggCall(const std::string& fn,
                                           std::unique_ptr<Expression> arg,
                                           bool distinct = false) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::move(arg));
	return std::make_unique<FunctionCallExpression>(fn, std::move(args), distinct);
}

static std::unique_ptr<Expression> funcCall(const std::string& fn,
                                            std::vector<std::unique_ptr<Expression>> args) {
	return std::make_unique<FunctionCallExpression>(fn, std::move(args), false);
}

// ============================================================================
// isAggregateFunction
// ============================================================================

class ExpressionUtilsTest : public ::testing::Test {};

TEST_F(ExpressionUtilsTest, IsAggregate_RecognisesAllNames) {
	EXPECT_TRUE(ExpressionUtils::isAggregateFunction("count"));
	EXPECT_TRUE(ExpressionUtils::isAggregateFunction("SUM"));
	EXPECT_TRUE(ExpressionUtils::isAggregateFunction("Avg"));
	EXPECT_TRUE(ExpressionUtils::isAggregateFunction("MIN"));
	EXPECT_TRUE(ExpressionUtils::isAggregateFunction("MAX"));
	EXPECT_TRUE(ExpressionUtils::isAggregateFunction("collect"));
}

TEST_F(ExpressionUtilsTest, IsAggregate_RejectsNonAgg) {
	EXPECT_FALSE(ExpressionUtils::isAggregateFunction("toUpper"));
	EXPECT_FALSE(ExpressionUtils::isAggregateFunction("size"));
	EXPECT_FALSE(ExpressionUtils::isAggregateFunction(""));
}

// ============================================================================
// containsAggregate — cover all expression-type branches
// ============================================================================

TEST_F(ExpressionUtilsTest, ContainsAggregate_Nullptr) {
	EXPECT_FALSE(ExpressionUtils::containsAggregate(nullptr));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_Literal) {
	auto e = lit(1);
	EXPECT_FALSE(ExpressionUtils::containsAggregate(e.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_FunctionCallWithAgg) {
	auto e = aggCall("count", var("n"));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(e.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_FunctionCallNoAgg) {
	auto e = funcCall("toUpper", {});
	EXPECT_FALSE(ExpressionUtils::containsAggregate(e.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_BinaryOp) {
	auto e = std::make_unique<BinaryOpExpression>(
		aggCall("sum", var("x")), BinaryOperatorType::BOP_ADD, lit(1));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(e.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_UnaryOp) {
	auto e = std::make_unique<UnaryOpExpression>(
		UnaryOperatorType::UOP_MINUS, aggCall("count", var("x")));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(e.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_CaseSimpleWithAggInComparison) {
	// CASE count(x) WHEN 1 THEN 'a' END
	auto caseExpr = std::make_unique<CaseExpression>(aggCall("count", var("x")));
	caseExpr->addBranch(lit(1), var("a"));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(caseExpr.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_CaseWithAggInBranch) {
	// CASE WHEN true THEN count(x) END
	auto caseExpr = std::make_unique<CaseExpression>();
	caseExpr->addBranch(lit(1), aggCall("count", var("x")));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(caseExpr.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_CaseWithAggInWhen) {
	// CASE WHEN count(x) > 0 THEN 'yes' END
	auto caseExpr = std::make_unique<CaseExpression>();
	caseExpr->addBranch(aggCall("count", var("x")), lit(1));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(caseExpr.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_CaseNoAggNoElse) {
	// CASE WHEN true THEN 1 END  (no else, no aggregate)
	auto caseExpr = std::make_unique<CaseExpression>();
	caseExpr->addBranch(lit(1), lit(2));
	EXPECT_FALSE(ExpressionUtils::containsAggregate(caseExpr.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_IsNull) {
	auto e = std::make_unique<IsNullExpression>(aggCall("count", var("x")), false);
	EXPECT_TRUE(ExpressionUtils::containsAggregate(e.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_InListDynamic) {
	// x IN [count(y)]  — dynamic list
	auto e = std::make_unique<InExpression>(
		var("x"), aggCall("count", var("y")));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(e.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_InListDynamicNoAgg) {
	auto e = std::make_unique<InExpression>(var("x"), var("list"));
	EXPECT_FALSE(ExpressionUtils::containsAggregate(e.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_ExistsWithWhere) {
	// EXISTS { (n)-[:KNOWS]->(m) WHERE count(m) > 0 }
	auto e = std::make_unique<ExistsExpression>(
		"(n)-[:KNOWS]->(m)", "n", "KNOWS", "Person",
		PatternDirection::PAT_OUTGOING,
		aggCall("count", var("m")));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(e.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_ExistsNoWhere) {
	auto e = std::make_unique<ExistsExpression>("(n)-[:KNOWS]->(m)");
	EXPECT_FALSE(ExpressionUtils::containsAggregate(e.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_PatternComprehensionWithAggInMap) {
	auto e = std::make_unique<PatternComprehensionExpression>(
		"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "Person",
		PatternDirection::PAT_OUTGOING,
		aggCall("count", var("m")));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(e.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_PatternComprehensionWithAggInWhere) {
	auto e = std::make_unique<PatternComprehensionExpression>(
		"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "Person",
		PatternDirection::PAT_OUTGOING,
		var("m"),  // map expr — no agg
		aggCall("count", var("m")));  // where — has agg
	EXPECT_TRUE(ExpressionUtils::containsAggregate(e.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_MapProjectionWithAgg) {
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_LITERAL, "cnt",
	                   aggCall("count", var("x")));
	auto e = std::make_unique<MapProjectionExpression>("n", std::move(items));
	EXPECT_TRUE(ExpressionUtils::containsAggregate(e.get()));
}

TEST_F(ExpressionUtilsTest, ContainsAggregate_MapProjectionNoAgg) {
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_PROPERTY, "name");
	auto e = std::make_unique<MapProjectionExpression>("n", std::move(items));
	EXPECT_FALSE(ExpressionUtils::containsAggregate(e.get()));
}

// ============================================================================
// collectVariables — cover all expression-type branches (both set and unordered_set)
// ============================================================================

TEST_F(ExpressionUtilsTest, CollectVars_Nullptr) {
	std::set<std::string> s;
	ExpressionUtils::collectVariables(nullptr, s);
	EXPECT_TRUE(s.empty());
}

TEST_F(ExpressionUtilsTest, CollectVars_Variable) {
	std::set<std::string> s;
	auto e = var("x");
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_EQ(s.size(), 1u);
	EXPECT_TRUE(s.count("x"));
}

TEST_F(ExpressionUtilsTest, CollectVars_PropertyAccess) {
	std::set<std::string> s;
	auto e = prop("n", "age");
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("n"));
}

TEST_F(ExpressionUtilsTest, CollectVars_BinaryOp) {
	std::set<std::string> s;
	auto e = std::make_unique<BinaryOpExpression>(
		var("a"), BinaryOperatorType::BOP_ADD, var("b"));
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("a"));
	EXPECT_TRUE(s.count("b"));
}

TEST_F(ExpressionUtilsTest, CollectVars_UnaryOp) {
	std::set<std::string> s;
	auto e = std::make_unique<UnaryOpExpression>(UnaryOperatorType::UOP_MINUS, var("x"));
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("x"));
}

TEST_F(ExpressionUtilsTest, CollectVars_IsNull) {
	std::set<std::string> s;
	auto e = std::make_unique<IsNullExpression>(var("x"), false);
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("x"));
}

TEST_F(ExpressionUtilsTest, CollectVars_InListDynamic) {
	std::set<std::string> s;
	auto e = std::make_unique<InExpression>(var("x"), var("myList"));
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("x"));
	EXPECT_TRUE(s.count("myList"));
}

TEST_F(ExpressionUtilsTest, CollectVars_InListStatic) {
	std::set<std::string> s;
	std::vector<PropertyValue> vals = {PropertyValue(1), PropertyValue(2)};
	auto e = std::make_unique<InExpression>(var("x"), vals);
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("x"));
	EXPECT_EQ(s.size(), 1u);
}

TEST_F(ExpressionUtilsTest, CollectVars_FunctionCall) {
	std::set<std::string> s;
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(var("a"));
	args.push_back(var("b"));
	auto e = std::make_unique<FunctionCallExpression>("myFunc", std::move(args), false);
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("a"));
	EXPECT_TRUE(s.count("b"));
}

TEST_F(ExpressionUtilsTest, CollectVars_CaseExpression) {
	std::set<std::string> s;
	auto caseExpr = std::make_unique<CaseExpression>(var("x"));
	caseExpr->addBranch(lit(1), var("y"));
	caseExpr->setElseExpression(var("z"));
	ExpressionUtils::collectVariables(caseExpr.get(), s);
	EXPECT_TRUE(s.count("x"));
	EXPECT_TRUE(s.count("y"));
	EXPECT_TRUE(s.count("z"));
}

TEST_F(ExpressionUtilsTest, CollectVars_ListSlice) {
	std::set<std::string> s;
	auto e = std::make_unique<ListSliceExpression>(var("list"), var("a"), var("b"), true);
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("list"));
	EXPECT_TRUE(s.count("a"));
	EXPECT_TRUE(s.count("b"));
}

TEST_F(ExpressionUtilsTest, CollectVars_ListComprehension) {
	std::set<std::string> s;
	auto e = std::make_unique<ListComprehensionExpression>(
		"x", var("list"), var("filterExpr"), var("mapExpr"),
		ComprehensionType::COMP_FILTER);
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("list"));
	EXPECT_TRUE(s.count("filterExpr"));
	EXPECT_TRUE(s.count("mapExpr"));
}

TEST_F(ExpressionUtilsTest, CollectVars_QuantifierFunction) {
	std::set<std::string> s;
	auto e = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", var("list"), var("pred"));
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("list"));
	EXPECT_TRUE(s.count("pred"));
}

TEST_F(ExpressionUtilsTest, CollectVars_Reduce) {
	std::set<std::string> s;
	auto e = std::make_unique<ReduceExpression>(
		"acc", lit(0), "x", var("list"), var("body"));
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("list"));
	EXPECT_TRUE(s.count("body"));
}

TEST_F(ExpressionUtilsTest, CollectVars_ExistsWithWhere) {
	std::set<std::string> s;
	auto e = std::make_unique<ExistsExpression>(
		"(n)-[:KNOWS]->(m)", "n", "KNOWS", "Person",
		PatternDirection::PAT_OUTGOING,
		var("filterVar"));
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("n"));        // sourceVar
	EXPECT_TRUE(s.count("filterVar")); // where clause
}

TEST_F(ExpressionUtilsTest, CollectVars_PatternComprehensionWithWhere) {
	std::set<std::string> s;
	auto e = std::make_unique<PatternComprehensionExpression>(
		"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "Person",
		PatternDirection::PAT_OUTGOING,
		var("mapVar"),
		var("whereVar"));
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("n"));         // sourceVar
	EXPECT_TRUE(s.count("mapVar"));
	EXPECT_TRUE(s.count("whereVar"));
}

TEST_F(ExpressionUtilsTest, CollectVars_MapProjection) {
	std::set<std::string> s;
	std::vector<MapProjectionItem> items;
	items.emplace_back(MapProjectionItemType::MPROP_LITERAL, "k", var("v"));
	auto e = std::make_unique<MapProjectionExpression>("n", std::move(items));
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("n"));
	EXPECT_TRUE(s.count("v"));
}

TEST_F(ExpressionUtilsTest, CollectVars_ShortestPath) {
	std::set<std::string> s;
	auto e = std::make_unique<ShortestPathExpression>(
		"a", "b", "KNOWS", PatternDirection::PAT_OUTGOING, 1, 5, false);
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("a"));
	EXPECT_TRUE(s.count("b"));
}

TEST_F(ExpressionUtilsTest, CollectVars_Literal) {
	std::set<std::string> s;
	auto e = lit(42);
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.empty());
}

// Also test unordered_set overload for the same branch-heavy cases
TEST_F(ExpressionUtilsTest, CollectVars_UnorderedSet_ShortestPath) {
	std::unordered_set<std::string> s;
	auto e = std::make_unique<ShortestPathExpression>(
		"start", "end", "REL", PatternDirection::PAT_BOTH, 1, 3, true);
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("start"));
	EXPECT_TRUE(s.count("end"));
}

TEST_F(ExpressionUtilsTest, CollectVars_UnorderedSet_InListDynamic) {
	std::unordered_set<std::string> s;
	auto e = std::make_unique<InExpression>(var("x"), var("dynList"));
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("x"));
	EXPECT_TRUE(s.count("dynList"));
}

TEST_F(ExpressionUtilsTest, CollectVars_UnorderedSet_ExistsWithWhere) {
	std::unordered_set<std::string> s;
	auto e = std::make_unique<ExistsExpression>(
		"(n)-[:R]->(m)", "n", "R", "M",
		PatternDirection::PAT_OUTGOING, var("w"));
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("n"));
	EXPECT_TRUE(s.count("w"));
}

TEST_F(ExpressionUtilsTest, CollectVars_UnorderedSet_PatternCompWithWhere) {
	std::unordered_set<std::string> s;
	auto e = std::make_unique<PatternComprehensionExpression>(
		"(n)-[:R]->(m)", "n", "m", "R", "M",
		PatternDirection::PAT_OUTGOING, var("map"), var("wh"));
	ExpressionUtils::collectVariables(e.get(), s);
	EXPECT_TRUE(s.count("n"));
	EXPECT_TRUE(s.count("map"));
	EXPECT_TRUE(s.count("wh"));
}

// ============================================================================
// extractAndReplaceAggregates
// ============================================================================

TEST_F(ExpressionUtilsTest, ExtractAgg_Nullptr) {
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	auto result = ExpressionUtils::extractAndReplaceAggregates(nullptr, items, counter);
	EXPECT_EQ(result, nullptr);
	EXPECT_TRUE(items.empty());
}

TEST_F(ExpressionUtilsTest, ExtractAgg_SimpleAggFunction) {
	// count(x) → __agg_0
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	auto e = aggCall("count", var("x"));
	auto result = ExpressionUtils::extractAndReplaceAggregates(e.get(), items, counter);
	ASSERT_EQ(items.size(), 1u);
	EXPECT_EQ(items[0].functionName, "count");
	EXPECT_EQ(items[0].alias, "__agg_0");
	EXPECT_EQ(result->getExpressionType(), ExpressionType::VARIABLE_REFERENCE);
}

TEST_F(ExpressionUtilsTest, ExtractAgg_DistinctAgg) {
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	auto e = aggCall("count", var("x"), true);
	auto result = ExpressionUtils::extractAndReplaceAggregates(e.get(), items, counter);
	ASSERT_EQ(items.size(), 1u);
	EXPECT_TRUE(items[0].distinct);
}

TEST_F(ExpressionUtilsTest, ExtractAgg_NonAggFunctionWithAggArgs) {
	// toUpper(count(x)) → toUpper(__agg_0)
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(aggCall("count", var("x")));
	auto e = std::make_unique<FunctionCallExpression>("toUpper", std::move(args), false);
	auto result = ExpressionUtils::extractAndReplaceAggregates(e.get(), items, counter);
	ASSERT_EQ(items.size(), 1u);
	EXPECT_EQ(items[0].functionName, "count");
	EXPECT_EQ(result->getExpressionType(), ExpressionType::FUNCTION_CALL);
}

TEST_F(ExpressionUtilsTest, ExtractAgg_BinaryOp) {
	// count(x) + sum(y) → __agg_0 + __agg_1
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	auto e = std::make_unique<BinaryOpExpression>(
		aggCall("count", var("x")), BinaryOperatorType::BOP_ADD,
		aggCall("sum", var("y")));
	auto result = ExpressionUtils::extractAndReplaceAggregates(e.get(), items, counter);
	EXPECT_EQ(items.size(), 2u);
	EXPECT_EQ(result->getExpressionType(), ExpressionType::BINARY_OP);
}

TEST_F(ExpressionUtilsTest, ExtractAgg_UnaryOp) {
	// -count(x)
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	auto e = std::make_unique<UnaryOpExpression>(
		UnaryOperatorType::UOP_MINUS, aggCall("count", var("x")));
	auto result = ExpressionUtils::extractAndReplaceAggregates(e.get(), items, counter);
	EXPECT_EQ(items.size(), 1u);
	EXPECT_EQ(result->getExpressionType(), ExpressionType::UNARY_OP);
}

TEST_F(ExpressionUtilsTest, ExtractAgg_IsNull) {
	// count(x) IS NULL
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	auto e = std::make_unique<IsNullExpression>(aggCall("count", var("x")), false);
	auto result = ExpressionUtils::extractAndReplaceAggregates(e.get(), items, counter);
	EXPECT_EQ(items.size(), 1u);
	EXPECT_EQ(result->getExpressionType(), ExpressionType::IS_NULL);
}

TEST_F(ExpressionUtilsTest, ExtractAgg_InListDynamic) {
	// count(x) IN someList
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	auto e = std::make_unique<InExpression>(
		aggCall("count", var("x")), var("someList"));
	auto result = ExpressionUtils::extractAndReplaceAggregates(e.get(), items, counter);
	EXPECT_EQ(items.size(), 1u);
	EXPECT_EQ(result->getExpressionType(), ExpressionType::IN_LIST);
}

TEST_F(ExpressionUtilsTest, ExtractAgg_InListStatic) {
	// count(x) IN [1, 2]
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	std::vector<PropertyValue> vals = {PropertyValue(1), PropertyValue(2)};
	auto e = std::make_unique<InExpression>(aggCall("count", var("x")), vals);
	auto result = ExpressionUtils::extractAndReplaceAggregates(e.get(), items, counter);
	EXPECT_EQ(items.size(), 1u);
	EXPECT_EQ(result->getExpressionType(), ExpressionType::IN_LIST);
}

TEST_F(ExpressionUtilsTest, ExtractAgg_CaseSimple) {
	// CASE count(x) WHEN 1 THEN 'a' ELSE 'b' END
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	auto caseExpr = std::make_unique<CaseExpression>(aggCall("count", var("x")));
	caseExpr->addBranch(lit(1), var("a"));
	caseExpr->setElseExpression(var("b"));
	auto result = ExpressionUtils::extractAndReplaceAggregates(caseExpr.get(), items, counter);
	EXPECT_EQ(items.size(), 1u);
	EXPECT_EQ(result->getExpressionType(), ExpressionType::CASE_EXPRESSION);
}

TEST_F(ExpressionUtilsTest, ExtractAgg_CaseSearched) {
	// CASE WHEN true THEN count(x) ELSE 0 END
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	auto caseExpr = std::make_unique<CaseExpression>();
	caseExpr->addBranch(lit(1), aggCall("count", var("x")));
	caseExpr->setElseExpression(lit(0));
	auto result = ExpressionUtils::extractAndReplaceAggregates(caseExpr.get(), items, counter);
	EXPECT_EQ(items.size(), 1u);
	EXPECT_EQ(result->getExpressionType(), ExpressionType::CASE_EXPRESSION);
}

TEST_F(ExpressionUtilsTest, ExtractAgg_DefaultClone) {
	// Variable reference falls through to default → clone
	std::vector<query::logical::LogicalAggItem> items;
	size_t counter = 0;
	auto e = var("x");
	auto result = ExpressionUtils::extractAndReplaceAggregates(e.get(), items, counter);
	EXPECT_TRUE(items.empty());
	EXPECT_EQ(result->getExpressionType(), ExpressionType::VARIABLE_REFERENCE);
}
