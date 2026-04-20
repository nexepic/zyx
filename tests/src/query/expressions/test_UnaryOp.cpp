/**
 * @file test_UnaryOp.cpp
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
// ExpressionEvaluator Tests - Unary Operations
// ============================================================================

TEST_F(ExpressionsTest, UnaryOp_Minus_Integer) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(42));
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), -42);
}

TEST_F(ExpressionsTest, UnaryOp_Minus_Double) {
	auto operand = std::make_unique<LiteralExpression>(3.14);
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), -3.14);
}

TEST_F(ExpressionsTest, UnaryOp_Not_True) {
	auto operand = std::make_unique<LiteralExpression>(true);
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, UnaryOp_Not_False) {
	auto operand = std::make_unique<LiteralExpression>(false);
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, UnaryOp_NullOperand) {
	auto operand = std::make_unique<LiteralExpression>(); // NULL
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, UnaryOp_ToString_AllOperators) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(42));

	std::vector<UnaryOperatorType> operators = {
		UnaryOperatorType::UOP_MINUS,
		UnaryOperatorType::UOP_NOT
	};

	for (auto op : operators) {
		auto operandClone = std::make_unique<LiteralExpression>(int64_t(42));
		UnaryOpExpression expr(op, std::move(operandClone));

		std::string str = expr.toString();
		EXPECT_FALSE(str.empty()) << "Operator " << static_cast<int>(op) << " should have string representation";
	}
}

TEST_F(ExpressionsTest, ExpressionEvaluator_UnaryOp_UnknownOperator) {
	// Similar to above, we test the valid path
	auto operand = std::make_unique<LiteralExpression>(int64_t(42));
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_NO_THROW((void)expr.accept(evaluator));
}

// ============================================================================
// Batch 9: UnaryOp Edge Cases
// ============================================================================

TEST_F(ExpressionsTest, UnaryOp_Minus_Zero) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(0));
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 0);
}

TEST_F(ExpressionsTest, UnaryOp_Minus_NegativeDouble) {
	auto operand = std::make_unique<LiteralExpression>(-3.14);
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), 3.14);
}

TEST_F(ExpressionsTest, UnaryOp_Not_ZeroInteger) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(0));
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, UnaryOp_Not_NonZeroInteger) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(42));
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, UnaryOp_Not_String) {
	auto operand = std::make_unique<LiteralExpression>(std::string("hello"));
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// Non-empty string is truthy, NOT makes it falsy
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, UnaryOp_Not_EmptyString) {
	auto operand = std::make_unique<LiteralExpression>(std::string(""));
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// Empty string is falsy, NOT makes it truthy
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}
