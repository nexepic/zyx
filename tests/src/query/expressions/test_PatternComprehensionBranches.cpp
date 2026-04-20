/**
 * @file test_PatternComprehensionBranches.cpp
 * @brief Tests for PatternComprehensionExpression.
 */

#include "query/expressions/ExpressionsTestFixture.hpp"

TEST_F(ExpressionsTest, PatternComprehension_ToStringHandlesMissingMapExpression) {
	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)",
		"n",
		"m",
		"KNOWS",
		"Person",
		PatternDirection::PAT_OUTGOING,
		nullptr,
		std::make_unique<LiteralExpression>(true));

	const auto s = expr.toString();
	EXPECT_NE(s.find("WHERE true"), std::string::npos);
	EXPECT_EQ(s.find(" | "), std::string::npos);
}

TEST_F(ExpressionsTest, PatternComprehension_CloneHandlesMissingMapExpression) {
	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)",
		"m",
		nullptr,
		nullptr);

	auto cloned = expr.clone();
	auto *clonedExpr = dynamic_cast<PatternComprehensionExpression *>(cloned.get());
	ASSERT_NE(clonedExpr, nullptr);
	EXPECT_EQ(clonedExpr->getMapExpression(), nullptr);
	EXPECT_EQ(clonedExpr->getWhereExpression(), nullptr);
}


TEST_F(ExpressionsTest, PatternComprehensionExpression_Getters) {
	// Test all getter methods of PatternComprehensionExpression
	auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr),
		std::move(whereExpr)
	);

	EXPECT_EQ(expr.getPattern(), "(n)-[:KNOWS]->(m)");
	EXPECT_EQ(expr.getTargetVar(), "m");
	EXPECT_NE(expr.getMapExpression(), nullptr);
	EXPECT_NE(expr.getWhereExpression(), nullptr);
	EXPECT_TRUE(expr.hasWhereClause());
}

TEST_F(ExpressionsTest, PatternComprehensionExpression_NoWhereClause) {
	// Test PatternComprehensionExpression without WHERE clause
	auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));

	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr),
		nullptr
	);

	EXPECT_EQ(expr.getPattern(), "(n)-[:KNOWS]->(m)");
	EXPECT_EQ(expr.getTargetVar(), "m");
	EXPECT_NE(expr.getMapExpression(), nullptr);
	EXPECT_EQ(expr.getWhereExpression(), nullptr);
	EXPECT_FALSE(expr.hasWhereClause());
}

TEST_F(ExpressionsTest, PatternComprehensionExpression_ToString) {
	// Test toString method with and without WHERE clause
	auto mapExpr1 = std::make_unique<LiteralExpression>(int64_t(42));
	auto whereExpr1 = std::make_unique<LiteralExpression>(bool(true));

	PatternComprehensionExpression expr1(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr1),
		std::move(whereExpr1)
	);

	std::string str1 = expr1.toString();
	EXPECT_TRUE(str1.find("(n)-[:KNOWS]->(m)") != std::string::npos);
	EXPECT_TRUE(str1.find("WHERE") != std::string::npos);

	auto mapExpr2 = std::make_unique<LiteralExpression>(int64_t(42));
	PatternComprehensionExpression expr2(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr2),
		nullptr
	);

	std::string str2 = expr2.toString();
	EXPECT_TRUE(str2.find("(n)-[:KNOWS]->(m)") != std::string::npos);
	EXPECT_TRUE(str2.find("WHERE") == std::string::npos);
}

TEST_F(ExpressionsTest, PatternComprehensionExpression_Clone) {
	// Test clone method
	auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	PatternComprehensionExpression original(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr),
		std::move(whereExpr)
	);

	auto cloned = original.clone();
	auto* clonedPattern = dynamic_cast<PatternComprehensionExpression*>(cloned.get());

	ASSERT_NE(clonedPattern, nullptr);
	EXPECT_EQ(clonedPattern->getPattern(), original.getPattern());
	EXPECT_EQ(clonedPattern->getTargetVar(), original.getTargetVar());
	EXPECT_NE(clonedPattern->getMapExpression(), nullptr);
	EXPECT_NE(clonedPattern->getWhereExpression(), nullptr);
	EXPECT_TRUE(clonedPattern->hasWhereClause());

	EXPECT_EQ(original.toString(), clonedPattern->toString());
}

TEST_F(ExpressionsTest, PatternComprehensionExpression_CloneNoWhere) {
	// Test clone method without WHERE clause
	auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));

	PatternComprehensionExpression original(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr),
		nullptr
	);

	auto cloned = original.clone();
	auto* clonedPattern = dynamic_cast<PatternComprehensionExpression*>(cloned.get());

	ASSERT_NE(clonedPattern, nullptr);
	EXPECT_EQ(clonedPattern->getPattern(), original.getPattern());
	EXPECT_EQ(clonedPattern->getTargetVar(), original.getTargetVar());
	EXPECT_NE(clonedPattern->getMapExpression(), nullptr);
	EXPECT_EQ(clonedPattern->getWhereExpression(), nullptr);
	EXPECT_FALSE(clonedPattern->hasWhereClause());
}

TEST_F(ExpressionsTest, PatternComprehensionExpression_AcceptVisitor) {
	// Test both accept methods
	auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr),
		std::move(whereExpr)
	);

	// Test non-const visitor
	TestExpressionVisitor visitor;
	expr.accept(visitor);
	EXPECT_TRUE(visitor.visitedPatternComprehension);

	// Test const visitor
	TestConstExpressionVisitor constVisitor;
	expr.accept(constVisitor);
	EXPECT_TRUE(constVisitor.visitedPatternComprehension);
}

TEST_F(ExpressionsTest, PatternComprehension_NotImplemented_Throws) {
	// Test PatternComprehension throws not implemented exception
	PatternComprehensionExpression expr(
		"x",
		"(n)-[:KNOWS]->(m)",
		nullptr,
		nullptr
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}
