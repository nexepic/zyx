/**
 * @file test_SemanticAnalyzer.cpp
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include <gtest/gtest.h>
#include "graph/query/ir/SemanticAnalyzer.hpp"
#include "graph/query/ir/QueryAST.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"

using namespace graph::query::ir;
using namespace graph::query::expressions;
using namespace graph;

static std::shared_ptr<Expression> lit(int64_t v) {
	return std::make_shared<LiteralExpression>(v);
}

static std::shared_ptr<Expression> var(const std::string& name) {
	return std::make_shared<VariableReferenceExpression>(name);
}

static std::shared_ptr<Expression> prop(const std::string& varName,
                                        const std::string& propName) {
	return std::make_shared<VariableReferenceExpression>(varName, propName);
}

static std::shared_ptr<Expression> aggCall(const std::string& fn,
                                           std::shared_ptr<Expression> arg) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::unique_ptr<Expression>(arg->clone()));
	return std::make_shared<FunctionCallExpression>(fn, std::move(args), false);
}

class SemanticAnalyzerTest : public ::testing::Test {};

// ============================================================================
// Non-aggregate projection: alias expansion in ORDER BY
// ============================================================================

TEST_F(SemanticAnalyzerTest, NonAgg_NoOrderBy) {
	ProjectionBody body;
	body.items.push_back({var("x"), "x"});
	SemanticAnalyzer::analyzeProjection(body);
	EXPECT_FALSE(body.hasAggregates);
	EXPECT_FALSE(body.items[0].containsAggregate);
}

TEST_F(SemanticAnalyzerTest, NonAgg_OrderByAlias_ExpandsToExpression) {
	ProjectionBody body;
	body.items.push_back({prop("n", "name"), "name"});
	body.orderBy.push_back({var("name"), true});
	SemanticAnalyzer::analyzeProjection(body);
	EXPECT_FALSE(body.hasAggregates);
	// The ORDER BY expression should have been expanded from alias "name"
	// to the original property access expression
	auto* expanded = dynamic_cast<VariableReferenceExpression*>(body.orderBy[0].expression.get());
	ASSERT_NE(expanded, nullptr);
	EXPECT_TRUE(expanded->hasProperty());
	EXPECT_EQ(expanded->getVariableName(), "n");
	EXPECT_EQ(expanded->getPropertyName(), "name");
}

TEST_F(SemanticAnalyzerTest, NonAgg_OrderByNonAlias_NotExpanded) {
	ProjectionBody body;
	body.items.push_back({var("x"), "x"});
	// ORDER BY a variable not matching any alias
	body.orderBy.push_back({var("y"), true});
	SemanticAnalyzer::analyzeProjection(body);
	auto* expr = dynamic_cast<VariableReferenceExpression*>(body.orderBy[0].expression.get());
	ASSERT_NE(expr, nullptr);
	EXPECT_EQ(expr->getVariableName(), "y");
}

TEST_F(SemanticAnalyzerTest, NonAgg_OrderByWithProperty_NotExpanded) {
	ProjectionBody body;
	body.items.push_back({var("n"), "n"});
	// ORDER BY n.age (hasProperty=true) should NOT be expanded
	body.orderBy.push_back({prop("n", "age"), true});
	SemanticAnalyzer::analyzeProjection(body);
	auto* expr = dynamic_cast<VariableReferenceExpression*>(body.orderBy[0].expression.get());
	ASSERT_NE(expr, nullptr);
	EXPECT_TRUE(expr->hasProperty());
}

TEST_F(SemanticAnalyzerTest, NonAgg_OrderByNullExpr_Skipped) {
	ProjectionBody body;
	body.items.push_back({var("x"), "x"});
	body.orderBy.push_back({nullptr, true});
	// Should not crash
	SemanticAnalyzer::analyzeProjection(body);
}

// ============================================================================
// Non-aggregate: ORDER BY with ListSlice alias expansion
// ============================================================================

TEST_F(SemanticAnalyzerTest, NonAgg_OrderByListSlice_ExpandsBase) {
	ProjectionBody body;
	// items: collect AS coll
	std::vector<PropertyValue> vals = {PropertyValue(1), PropertyValue(2)};
	auto listLit = std::make_shared<LiteralExpression>(int64_t(42));
	body.items.push_back({listLit, "coll"});

	// ORDER BY coll[0] - a ListSliceExpression whose base is "coll"
	auto sliceExpr = std::make_shared<ListSliceExpression>(
		std::make_unique<VariableReferenceExpression>("coll"),
		std::make_unique<LiteralExpression>(int64_t(0)),
		nullptr, false);
	body.orderBy.push_back({sliceExpr, true});

	SemanticAnalyzer::analyzeProjection(body);
	// The base should have been expanded
	auto* expanded = dynamic_cast<ListSliceExpression*>(body.orderBy[0].expression.get());
	ASSERT_NE(expanded, nullptr);
}

TEST_F(SemanticAnalyzerTest, NonAgg_OrderByListSlice_BaseWithProperty_NotExpanded) {
	ProjectionBody body;
	body.items.push_back({var("n"), "n"});

	// ORDER BY n.items[0] - base is "n" with property "items"
	auto sliceExpr = std::make_shared<ListSliceExpression>(
		std::make_unique<VariableReferenceExpression>("n", "items"),
		std::make_unique<LiteralExpression>(int64_t(0)),
		nullptr, false);
	body.orderBy.push_back({sliceExpr, true});

	SemanticAnalyzer::analyzeProjection(body);
	// Should NOT be expanded because base has property
}

TEST_F(SemanticAnalyzerTest, NonAgg_OrderByListSlice_BaseNotInAlias) {
	ProjectionBody body;
	body.items.push_back({var("x"), "x"});

	// ORDER BY other[0] - base "other" not in alias map
	auto sliceExpr = std::make_shared<ListSliceExpression>(
		std::make_unique<VariableReferenceExpression>("other"),
		std::make_unique<LiteralExpression>(int64_t(0)),
		nullptr, false);
	body.orderBy.push_back({sliceExpr, true});

	SemanticAnalyzer::analyzeProjection(body);
	// Not expanded, original expression preserved
}

TEST_F(SemanticAnalyzerTest, NonAgg_OrderByListSlice_WithRange) {
	ProjectionBody body;
	body.items.push_back({lit(42), "coll"});

	auto sliceExpr = std::make_shared<ListSliceExpression>(
		std::make_unique<VariableReferenceExpression>("coll"),
		std::make_unique<LiteralExpression>(int64_t(0)),
		std::make_unique<LiteralExpression>(int64_t(2)),
		true);
	body.orderBy.push_back({sliceExpr, true});

	SemanticAnalyzer::analyzeProjection(body);
	auto* expanded = dynamic_cast<ListSliceExpression*>(body.orderBy[0].expression.get());
	ASSERT_NE(expanded, nullptr);
	EXPECT_TRUE(expanded->hasRange());
}

// ============================================================================
// Aggregate projection: items classification
// ============================================================================

TEST_F(SemanticAnalyzerTest, Agg_TopLevelAggregate) {
	ProjectionBody body;
	body.items.push_back({aggCall("count", var("n")), "cnt"});
	SemanticAnalyzer::analyzeProjection(body);
	EXPECT_TRUE(body.hasAggregates);
	EXPECT_TRUE(body.items[0].containsAggregate);
	EXPECT_TRUE(body.items[0].isTopLevelAggregate);
}

TEST_F(SemanticAnalyzerTest, Agg_NonTopLevel_AggInBinaryOp) {
	// count(n) + 1 - contains aggregate but is not top-level
	auto binOp = std::make_shared<BinaryOpExpression>(
		std::unique_ptr<Expression>(aggCall("count", var("n"))->clone()),
		BinaryOperatorType::BOP_ADD,
		std::make_unique<LiteralExpression>(int64_t(1)));
	ProjectionBody body;
	body.items.push_back({binOp, "cnt_plus_one"});
	SemanticAnalyzer::analyzeProjection(body);
	EXPECT_TRUE(body.hasAggregates);
	EXPECT_TRUE(body.items[0].containsAggregate);
	EXPECT_FALSE(body.items[0].isTopLevelAggregate);
}

TEST_F(SemanticAnalyzerTest, Agg_OrderByNotExpanded) {
	// When hasAggregates=true, ORDER BY aliases should NOT be expanded
	ProjectionBody body;
	body.items.push_back({aggCall("count", var("n")), "cnt"});
	body.orderBy.push_back({var("cnt"), true});
	SemanticAnalyzer::analyzeProjection(body);
	EXPECT_TRUE(body.hasAggregates);
	// orderBy should still reference "cnt" alias, not expanded
	auto* expr = dynamic_cast<VariableReferenceExpression*>(body.orderBy[0].expression.get());
	ASSERT_NE(expr, nullptr);
	EXPECT_EQ(expr->getVariableName(), "cnt");
	EXPECT_FALSE(expr->hasProperty());
}

TEST_F(SemanticAnalyzerTest, Agg_MixedItems) {
	ProjectionBody body;
	body.items.push_back({var("n"), "n"});
	body.items.push_back({aggCall("count", var("n")), "cnt"});
	SemanticAnalyzer::analyzeProjection(body);
	EXPECT_TRUE(body.hasAggregates);
	EXPECT_FALSE(body.items[0].containsAggregate);
	EXPECT_TRUE(body.items[1].containsAggregate);
}

TEST_F(SemanticAnalyzerTest, NonAgg_OrderByListSlice_NonVarBase_NotExpanded) {
	ProjectionBody body;
	body.items.push_back({var("x"), "x"});

	// ORDER BY (1 + 2)[0] - base is not a VariableReferenceExpression
	auto sliceExpr = std::make_shared<ListSliceExpression>(
		std::make_unique<BinaryOpExpression>(
			std::make_unique<LiteralExpression>(int64_t(1)),
			BinaryOperatorType::BOP_ADD,
			std::make_unique<LiteralExpression>(int64_t(2))),
		std::make_unique<LiteralExpression>(int64_t(0)),
		nullptr, false);
	body.orderBy.push_back({sliceExpr, true});

	SemanticAnalyzer::analyzeProjection(body);
	// Not expanded since base is not a variable
}

TEST_F(SemanticAnalyzerTest, NonAgg_OrderByNonVarNonSlice_NotExpanded) {
	ProjectionBody body;
	body.items.push_back({var("x"), "x"});

	// ORDER BY 1 + 2 (a BinaryOpExpression, not variable or slice)
	auto binExpr = std::make_shared<BinaryOpExpression>(
		std::make_unique<LiteralExpression>(int64_t(1)),
		BinaryOperatorType::BOP_ADD,
		std::make_unique<LiteralExpression>(int64_t(2)));
	body.orderBy.push_back({binExpr, true});

	SemanticAnalyzer::analyzeProjection(body);
	// Should not be expanded
}
