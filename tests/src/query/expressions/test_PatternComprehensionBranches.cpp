/**
 * @file test_PatternComprehensionBranches.cpp
 * @brief Focused branch tests for PatternComprehensionExpression.cpp.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"

using namespace graph::query::expressions;

TEST(PatternComprehensionBranchTest, ToStringHandlesMissingMapExpression) {
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

TEST(PatternComprehensionBranchTest, CloneHandlesMissingMapExpression) {
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
