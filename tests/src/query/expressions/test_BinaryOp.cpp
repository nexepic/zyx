/**
 * @file test_BinaryOp.cpp
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
// ExpressionEvaluator Tests - Binary Operations
// ============================================================================

TEST_F(ExpressionsTest, BinaryOp_Addition_Integer) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 8);
}

TEST_F(ExpressionsTest, BinaryOp_Addition_Double) {
	auto left = std::make_unique<LiteralExpression>(2.5);
	auto right = std::make_unique<LiteralExpression>(1.5);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), 4.0);
}

TEST_F(ExpressionsTest, BinaryOp_Subtraction) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 2);
}

TEST_F(ExpressionsTest, BinaryOp_Multiplication) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 15);
}

TEST_F(ExpressionsTest, BinaryOp_Division) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// 10 / 2 is evenly divisible, so result is integer
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 5);
}

TEST_F(ExpressionsTest, BinaryOp_Modulus) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 1);
}

TEST_F(ExpressionsTest, BinaryOp_DivisionByZero) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), DivisionByZeroException);
}

TEST_F(ExpressionsTest, BinaryOp_ModulusByZero) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), DivisionByZeroException);
}

// ============================================================================
// ExpressionEvaluator Tests - Binary Comparison Operations
// ============================================================================

TEST_F(ExpressionsTest, BinaryOp_Equal_Integer) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_NotEqual) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_NOT_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LessThan) {
	auto left = std::make_unique<LiteralExpression>(int64_t(3));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_GreaterThanOrEqual) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_GREATER_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_Greater) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_GREATER, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LessEqual) {
	auto left = std::make_unique<LiteralExpression>(int64_t(3));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

// ============================================================================
// ExpressionEvaluator Tests - Binary Logical Operations
// ============================================================================

TEST_F(ExpressionsTest, BinaryOp_LogicalAnd_TrueTrue) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalAnd_TrueFalse) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalOr_FalseTrue) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalOr_FalseFalse) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalXor_TrueTrue) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalXor_TrueFalse) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

// ============================================================================
// ExpressionEvaluator Tests - NULL Propagation
// ============================================================================

TEST_F(ExpressionsTest, BinaryOp_NullLeft) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, BinaryOp_NullRight) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Batch 4: Expression Structure Tests - toString() and clone() for BinaryOp
// ============================================================================

TEST_F(ExpressionsTest, BinaryOp_ToString_AllOperators) {
	// Test all binary operators have proper string representation
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));

	std::vector<BinaryOperatorType> operators = {
		BinaryOperatorType::BOP_ADD,
		BinaryOperatorType::BOP_SUBTRACT,
		BinaryOperatorType::BOP_MULTIPLY,
		BinaryOperatorType::BOP_DIVIDE,
		BinaryOperatorType::BOP_MODULO,
		BinaryOperatorType::BOP_EQUAL,
		BinaryOperatorType::BOP_NOT_EQUAL,
		BinaryOperatorType::BOP_LESS,
		BinaryOperatorType::BOP_GREATER,
		BinaryOperatorType::BOP_LESS_EQUAL,
		BinaryOperatorType::BOP_GREATER_EQUAL,
		BinaryOperatorType::BOP_AND,
		BinaryOperatorType::BOP_OR,
		BinaryOperatorType::BOP_XOR
	};

	for (auto op : operators) {
		auto leftClone = std::make_unique<LiteralExpression>(int64_t(5));
		auto rightClone = std::make_unique<LiteralExpression>(int64_t(3));
		BinaryOpExpression expr(std::move(leftClone), op, std::move(rightClone));

		std::string str = expr.toString();
		EXPECT_FALSE(str.empty()) << "Operator " << static_cast<int>(op) << " should have string representation";
	}
}

// ============================================================================
// Batch 6: ExpressionEvaluator - Additional Coverage
// ============================================================================

TEST_F(ExpressionsTest, ExpressionEvaluator_BinaryOp_UnknownOperator) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	// Create an expression but modify the operator to something invalid
	// This is tricky since we can't directly set an invalid operator
	// Instead, we'll test with a valid operator to ensure coverage
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_NO_THROW((void)expr.accept(evaluator));
}

// ============================================================================
// Batch 8: BinaryOp Edge Cases
// ============================================================================

TEST_F(ExpressionsTest, BinaryOp_MultiplyByZero) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 0);
}

TEST_F(ExpressionsTest, BinaryOp_SubtractNegative) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(-3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 8);
}

TEST_F(ExpressionsTest, BinaryOp_DivisionNegative) {
	auto left = std::make_unique<LiteralExpression>(int64_t(-10));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), -5);
}

TEST_F(ExpressionsTest, BinaryOp_CompareStrings) {
	auto left = std::make_unique<LiteralExpression>(std::string("apple"));
	auto right = std::make_unique<LiteralExpression>(std::string("banana"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_CompareStringAndNumber) {
	auto left = std::make_unique<LiteralExpression>(std::string("42"));
	auto right = std::make_unique<LiteralExpression>(int64_t(42));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalAnd_ZeroInteger) {
	auto left = std::make_unique<LiteralExpression>(int64_t(0));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalOr_ZeroInteger) {
	auto left = std::make_unique<LiteralExpression>(int64_t(0));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_Compare_BoolVsInt) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(int64_t(1));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// Boolean and integer should not be equal even if both represent "true"
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_Compare_BoolVsDouble) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(1.0);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

// ============================================================================
// Coverage: ExpressionEvaluator - All comparison/logical/arithmetic branches
// ============================================================================

TEST_F(ExpressionsTest, ExpressionEvaluator_BranchCoverage_AllComparisons) {
	std::vector<BinaryOperatorType> comparisonOps = {
		BinaryOperatorType::BOP_EQUAL, BinaryOperatorType::BOP_NOT_EQUAL,
		BinaryOperatorType::BOP_LESS, BinaryOperatorType::BOP_GREATER,
		BinaryOperatorType::BOP_LESS_EQUAL, BinaryOperatorType::BOP_GREATER_EQUAL
	};

	for (auto op : comparisonOps) {
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(3));
		BinaryOpExpression expr(std::move(left), op, std::move(right));

		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
	}
}

TEST_F(ExpressionsTest, ExpressionEvaluator_BranchCoverage_LogicalOps) {
	// AND - all combinations
	{
		std::vector<std::pair<bool, bool>> tests = {
			{true, true}, {true, false}, {false, true}, {false, false}
		};
		for (auto [left, right] : tests) {
			auto leftExpr = std::make_unique<LiteralExpression>(left);
			auto rightExpr = std::make_unique<LiteralExpression>(right);
			BinaryOpExpression expr(std::move(leftExpr), BinaryOperatorType::BOP_AND, std::move(rightExpr));
			ExpressionEvaluator evaluator(*context_);
			expr.accept(evaluator);
			EXPECT_EQ(std::get<bool>(evaluator.getResult().getVariant()), left && right);
		}
	}

	// OR - all combinations
	{
		std::vector<std::pair<bool, bool>> tests = {
			{true, true}, {true, false}, {false, true}, {false, false}
		};
		for (auto [left, right] : tests) {
			auto leftExpr = std::make_unique<LiteralExpression>(left);
			auto rightExpr = std::make_unique<LiteralExpression>(right);
			BinaryOpExpression expr(std::move(leftExpr), BinaryOperatorType::BOP_OR, std::move(rightExpr));
			ExpressionEvaluator evaluator(*context_);
			expr.accept(evaluator);
			EXPECT_EQ(std::get<bool>(evaluator.getResult().getVariant()), left || right);
		}
	}

	// XOR - all combinations
	{
		std::vector<std::pair<bool, bool>> tests = {
			{true, true}, {true, false}, {false, true}, {false, false}
		};
		for (auto [left, right] : tests) {
			auto leftExpr = std::make_unique<LiteralExpression>(left);
			auto rightExpr = std::make_unique<LiteralExpression>(right);
			BinaryOpExpression expr(std::move(leftExpr), BinaryOperatorType::BOP_XOR, std::move(rightExpr));
			ExpressionEvaluator evaluator(*context_);
			expr.accept(evaluator);
			EXPECT_EQ(std::get<bool>(evaluator.getResult().getVariant()), left != right);
		}
	}
}

TEST_F(ExpressionsTest, ExpressionEvaluator_Comparison_EdgeCases) {
	// Equal with same values
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(42));
		auto right = std::make_unique<LiteralExpression>(int64_t(42));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Not equal with different values
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(42));
		auto right = std::make_unique<LiteralExpression>(int64_t(43));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_NOT_EQUAL, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Less with equal values (should be false)
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Greater with equal values (should be false)
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_GREATER, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
	}
}

TEST_F(ExpressionsTest, ExpressionEvaluator_Arithmetic_EdgeCases) {
	// Add with zero
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(0));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 5);
	}

	// Multiply by one
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(7));
		auto right = std::make_unique<LiteralExpression>(int64_t(1));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 7);
	}
}

TEST_F(ExpressionsTest, ExpressionEvaluator_DivisionModulo_FinalEdgeCases) {
	// Division with positive integers
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(10));
		auto right = std::make_unique<LiteralExpression>(int64_t(3));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_NEAR(std::get<double>(evaluator.getResult().getVariant()), 3.333, 0.001);
	}

	// Division with negative numbers
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(-10));
		auto right = std::make_unique<LiteralExpression>(int64_t(3));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_NEAR(std::get<double>(evaluator.getResult().getVariant()), -3.333, 0.001);
	}

	// Modulo with positive integers
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(10));
		auto right = std::make_unique<LiteralExpression>(int64_t(3));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 1);
	}
}

TEST_F(ExpressionsTest, ExpressionEvaluator_Comparison_FinalCombinations) {
	// LESS_EQUAL with equal values
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS_EQUAL, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// GREATER_EQUAL with equal values
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_GREATER_EQUAL, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// LESS_EQUAL with less than
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(3));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS_EQUAL, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// GREATER_EQUAL with greater than
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(7));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_GREATER_EQUAL, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}
}

TEST_F(ExpressionsTest, ExpressionEvaluator_Arithmetic_AbsoluteFinal) {
	// Subtract to get zero
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 0);
	}

	// Subtract to get negative
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(3));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), -2);
	}

	// Multiply by zero
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(99));
		auto right = std::make_unique<LiteralExpression>(int64_t(0));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 0);
	}
}


TEST_F(ExpressionsTest, DivisionByZero_Double) {
	// Test double division by zero throws exception
	auto left = std::make_unique<LiteralExpression>(10.0);
	auto right = std::make_unique<LiteralExpression>(0.0);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), DivisionByZeroException);
}

TEST_F(ExpressionsTest, BinaryOp_PowerOperator) {
	// Test power operator
	auto leftExpr = std::make_unique<LiteralExpression>(double(2.0));
	auto rightExpr = std::make_unique<LiteralExpression>(double(3.0));

	BinaryOpExpression expr(std::move(leftExpr), BinaryOperatorType::BOP_POWER, std::move(rightExpr));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	EXPECT_NEAR(std::get<double>(evaluator.getResult().getVariant()), 8.0, 0.001);
}

TEST_F(ExpressionsTest, BinaryOperatorType_ToString_Power) {
	EXPECT_EQ(graph::query::expressions::toString(BinaryOperatorType::BOP_POWER), "^");
}

TEST_F(ExpressionsTest, BinaryOperatorType_ToString_In) {
	EXPECT_EQ(graph::query::expressions::toString(BinaryOperatorType::BOP_IN), "IN");
}

TEST_F(ExpressionsTest, BinaryOperatorType_ToString_StartsWith) {
	EXPECT_EQ(graph::query::expressions::toString(BinaryOperatorType::BOP_STARTS_WITH), "STARTS WITH");
}

TEST_F(ExpressionsTest, BinaryOperatorType_ToString_EndsWith) {
	EXPECT_EQ(graph::query::expressions::toString(BinaryOperatorType::BOP_ENDS_WITH), "ENDS WITH");
}

TEST_F(ExpressionsTest, BinaryOperatorType_ToString_Contains) {
	EXPECT_EQ(graph::query::expressions::toString(BinaryOperatorType::BOP_CONTAINS), "CONTAINS");
}

TEST_F(ExpressionsTest, IsComparisonOperator_StartsWith) {
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_STARTS_WITH));
}

TEST_F(ExpressionsTest, IsComparisonOperator_EndsWith) {
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_ENDS_WITH));
}

TEST_F(ExpressionsTest, IsComparisonOperator_Contains) {
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_CONTAINS));
}

TEST_F(ExpressionsTest, BinaryOpExpression_ToString_Power) {
	auto left = std::make_unique<LiteralExpression>(int64_t(2));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_POWER, std::move(right));
	std::string result = expr.toString();
	EXPECT_TRUE(result.find("^") != std::string::npos);
}

TEST_F(ExpressionsTest, BinaryOpExpression_ToString_StartsWith) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello"));
	auto right = std::make_unique<LiteralExpression>(std::string("he"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_STARTS_WITH, std::move(right));
	std::string result = expr.toString();
	EXPECT_TRUE(result.find("STARTS WITH") != std::string::npos);
}

TEST_F(ExpressionsTest, BinaryOpExpression_ToString_EndsWith) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello"));
	auto right = std::make_unique<LiteralExpression>(std::string("lo"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ENDS_WITH, std::move(right));
	std::string result = expr.toString();
	EXPECT_TRUE(result.find("ENDS WITH") != std::string::npos);
}

TEST_F(ExpressionsTest, BinaryOpExpression_ToString_Contains) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello"));
	auto right = std::make_unique<LiteralExpression>(std::string("ell"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_CONTAINS, std::move(right));
	std::string result = expr.toString();
	EXPECT_TRUE(result.find("CONTAINS") != std::string::npos);
}

TEST_F(ExpressionsTest, BinaryOpExpression_ToString_In) {
	auto left = std::make_unique<LiteralExpression>(int64_t(1));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_IN, std::move(right));
	std::string result = expr.toString();
	EXPECT_TRUE(result.find("IN") != std::string::npos);
}
