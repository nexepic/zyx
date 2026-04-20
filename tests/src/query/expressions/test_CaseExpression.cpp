/**
 * @file test_CaseExpression.cpp
 * @author ZYX Contributors
 * @date 2026
 *
 * @copyright Copyright (c) 2026
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "query/expressions/ExpressionsTestFixture.hpp"

// ============================================================================
// ExpressionEvaluator Tests - CASE Expression
// ============================================================================

TEST_F(ExpressionsTest, CaseExpression_Simple_Match) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(2)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(2)), std::make_unique<LiteralExpression>(std::string("two")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "two");
}

TEST_F(ExpressionsTest, CaseExpression_Simple_NoMatch_Else) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(99)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(2)), std::make_unique<LiteralExpression>(std::string("two")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "other");
}

TEST_F(ExpressionsTest, CaseExpression_Simple_NoMatch_NoElse) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(99)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, CaseExpression_Searched_True) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(true), std::make_unique<LiteralExpression>(std::string("second")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "second");
}

TEST_F(ExpressionsTest, CaseExpression_Searched_AllFalse_Else) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("second")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "other");
}

TEST_F(ExpressionsTest, CaseExpression_Searched_NullCondition) {
	CaseExpression expr; // Searched CASE
	// NULL in WHEN should be skipped
	expr.addBranch(std::make_unique<LiteralExpression>(), std::make_unique<LiteralExpression>(std::string("skipped")));
	expr.addBranch(std::make_unique<LiteralExpression>(true), std::make_unique<LiteralExpression>(std::string("second")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "second");
}

TEST_F(ExpressionsTest, CaseExpression_ToString_Simple) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(2)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));

	std::string str = expr.toString();
	EXPECT_TRUE(str.find("CASE") != std::string::npos);
}

TEST_F(ExpressionsTest, CaseExpression_ToString_Searched) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(true), std::make_unique<LiteralExpression>(std::string("first")));

	std::string str = expr.toString();
	EXPECT_TRUE(str.find("CASE") != std::string::npos);
	EXPECT_TRUE(str.find("WHEN") != std::string::npos);
	EXPECT_TRUE(str.find("THEN") != std::string::npos);
}

TEST_F(ExpressionsTest, CaseExpression_Clone_Simple) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(2)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(2)), std::make_unique<LiteralExpression>(std::string("two")));

	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	EXPECT_NE(cloned.get(), &expr);

	// Evaluate the cloned expression
	ExpressionEvaluator evaluator(*context_);
	cloned->accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "two");
}

TEST_F(ExpressionsTest, CaseExpression_Clone_Searched) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(true), std::make_unique<LiteralExpression>(std::string("second")));

	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	EXPECT_NE(cloned.get(), &expr);

	// Evaluate the cloned expression
	ExpressionEvaluator evaluator(*context_);
	cloned->accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "second");
}

// ============================================================================
// Batch 12: CASE Expression Edge Cases
// ============================================================================

TEST_F(ExpressionsTest, CaseExpression_Simple_MultipleBranches) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(3)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(2)), std::make_unique<LiteralExpression>(std::string("two")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(3)), std::make_unique<LiteralExpression>(std::string("three")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(4)), std::make_unique<LiteralExpression>(std::string("four")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "three");
}

TEST_F(ExpressionsTest, CaseExpression_Simple_FirstBranchMatches) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(1)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("second"))); // Duplicate
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "first");
}

TEST_F(ExpressionsTest, CaseExpression_Searched_MultipleBranches) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("second")));
	expr.addBranch(std::make_unique<LiteralExpression>(true), std::make_unique<LiteralExpression>(std::string("third")));
	expr.addBranch(std::make_unique<LiteralExpression>(true), std::make_unique<LiteralExpression>(std::string("fourth")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "third");
}

TEST_F(ExpressionsTest, CaseExpression_Searched_AllFalseNoElse) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(0)), std::make_unique<LiteralExpression>(std::string("second")));
	// No else

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, CaseExpression_Simple_StringComparison) {
	CaseExpression expr(std::make_unique<LiteralExpression>(std::string("b")));
	expr.addBranch(std::make_unique<LiteralExpression>(std::string("a")), std::make_unique<LiteralExpression>(int64_t(1)));
	expr.addBranch(std::make_unique<LiteralExpression>(std::string("b")), std::make_unique<LiteralExpression>(int64_t(2)));
	expr.addBranch(std::make_unique<LiteralExpression>(std::string("c")), std::make_unique<LiteralExpression>(int64_t(3)));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 2);
}

TEST_F(ExpressionsTest, CaseExpression_Searched_IntegerCondition) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(0)), std::make_unique<LiteralExpression>(std::string("zero")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(5)), std::make_unique<LiteralExpression>(std::string("five")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// int64_t(0) is falsy (toBoolean returns false), int64_t(5) is truthy
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "five");
}

TEST_F(ExpressionsTest, CaseExpression_Simple_ElseBranch) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(99)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(2)), std::make_unique<LiteralExpression>(std::string("second")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("else")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "else");
}

TEST_F(ExpressionsTest, CaseExpression_Simple_NullComparison) {
	CaseExpression expr(std::make_unique<LiteralExpression>()); // NULL
	expr.addBranch(std::make_unique<LiteralExpression>(), std::make_unique<LiteralExpression>(std::string("null")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("else")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// NULL == NULL returns false in PropertyValue comparison, so else is taken
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "else");
}

TEST_F(ExpressionsTest, CaseExpression_Simple_CompareNullWithValue) {
	CaseExpression expr(std::make_unique<LiteralExpression>()); // NULL
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("else")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// NULL != 1, so should go to else
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "else");
}

TEST_F(ExpressionsTest, CaseExpression_Searched_DoubleCondition) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(3.14), std::make_unique<LiteralExpression>(std::string("pi")));
	expr.addBranch(std::make_unique<LiteralExpression>(2.71), std::make_unique<LiteralExpression>(std::string("e")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// 3.14 is truthy (non-zero)
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "pi");
}

TEST_F(ExpressionsTest, CaseExpression_Searched_NullConditionSkipped) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(), std::make_unique<LiteralExpression>(std::string("null")));
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("false")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("else")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// NULL branch is skipped, false doesn't match, so else is returned
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "else");
}


TEST_F(ExpressionsTest, CaseExpression_ToString_NullElse) {
	CaseExpression expr;
	// Set elseExpression_ to nullptr
	expr.setElseExpression(nullptr);
	expr.addBranch(
		std::make_unique<LiteralExpression>(true),
		std::make_unique<LiteralExpression>(int64_t(1))
	);
	std::string result = expr.toString();
	// Should not contain "ELSE" since elseExpression_ is null
	EXPECT_TRUE(result.find("CASE") != std::string::npos);
	EXPECT_TRUE(result.find("WHEN") != std::string::npos);
	EXPECT_TRUE(result.find("ELSE") == std::string::npos);
	EXPECT_TRUE(result.find("END") != std::string::npos);
}

TEST_F(ExpressionsTest, CaseExpression_Clone_NullElse) {
	// When elseExpression_ is null, clone() skips setElseExpression on the clone.
	// The clone's default constructor provides a default elseExpression_ (LiteralExpression null).
	// This test exercises the False branch of "if (elseExpression_)" at line 327.
	CaseExpression expr;
	expr.setElseExpression(nullptr);
	expr.addBranch(
		std::make_unique<LiteralExpression>(true),
		std::make_unique<LiteralExpression>(int64_t(1))
	);
	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	// The clone gets a default ELSE (null literal) from its constructor
	std::string result = cloned->toString();
	EXPECT_NE(result.find("WHEN"), std::string::npos);
	EXPECT_NE(result.find("END"), std::string::npos);
}

TEST_F(ExpressionsTest, CaseExpression_NonConstAccept) {
	MockExpressionVisitor visitor;
	CaseExpression expr;
	expr.accept(visitor);
	EXPECT_EQ(visitor.visitCount, 1);
}
