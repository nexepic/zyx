/**
 * @file test_ComplexArithmeticExpressions.cpp
 * @brief Comprehensive tests for complex arithmetic expressions in RETURN clause
 * @date 2026/03/10
 */

#include <gtest/gtest.h>
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/execution/Record.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

class ComplexArithmeticTest : public ::testing::Test {
protected:
	void SetUp() override {
		record_ = std::make_unique<Record>();
		context_ = std::make_unique<EvaluationContext>(*record_);

		// Set up test variables with various numeric values
		context_->setVariable("a", PropertyValue(int64_t(10)));
		context_->setVariable("b", PropertyValue(int64_t(5)));
		context_->setVariable("c", PropertyValue(int64_t(2)));
		context_->setVariable("d", PropertyValue(double(3.5)));
		context_->setVariable("e", PropertyValue(double(2.5)));
	}

	// Helper to evaluate an expression and get the result
	PropertyValue evaluate(Expression* expr) {
		ExpressionEvaluator evaluator(*context_);
		expr->accept(evaluator);
		return evaluator.getResult();
	}

	// Helper to get result as double (handles both int and double results)
	double getAsDouble(const PropertyValue& value) {
		if (value.getType() == PropertyType::INTEGER) {
			return static_cast<double>(std::get<int64_t>(value.getVariant()));
		}
		return std::get<double>(value.getVariant());
	}

	std::unique_ptr<Record> record_;
	std::unique_ptr<EvaluationContext> context_;
};

// ============================================================================
// Basic Arithmetic Operations (Two Operands)
// ============================================================================

TEST_F(ComplexArithmeticTest, Basic_Addition) {
	// Test: a + b = 10 + 5 = 15
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 15.0);
}

TEST_F(ComplexArithmeticTest, Basic_Subtraction) {
	// Test: a - b = 10 - 5 = 5
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 5.0);
}

TEST_F(ComplexArithmeticTest, Basic_Multiplication) {
	// Test: a * b = 10 * 5 = 50
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 50.0);
}

TEST_F(ComplexArithmeticTest, Basic_Division) {
	// Test: a / b = 10 / 5 = 2
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 2.0);
}

TEST_F(ComplexArithmeticTest, Basic_Modulo) {
	// Test: a % b = 10 % 5 = 0
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), int64_t(0));
}

TEST_F(ComplexArithmeticTest, Basic_Power) {
	// Test: a ^ c = 10 ^ 2 = 100
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_POWER, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 100.0);
}

// ============================================================================
// Complex Arithmetic with Three or More Operands
// ============================================================================

TEST_F(ComplexArithmeticTest, Complex_AddMultiply) {
	// Test: a * b + c = 10 * 5 + 2 = 52
	// This creates: (a * b) + c
	auto mulLeft = std::make_unique<LiteralExpression>(int64_t(10));
	auto mulRight = std::make_unique<LiteralExpression>(int64_t(5));
	auto mulExpr = std::make_unique<BinaryOpExpression>(std::move(mulLeft), BinaryOperatorType::BOP_MULTIPLY, std::move(mulRight));

	auto addRight = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(mulExpr), BinaryOperatorType::BOP_ADD, std::move(addRight));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 52.0);
}

TEST_F(ComplexArithmeticTest, Complex_AddDivide) {
	// Test: a + b / c = 10 + 5 / 2 = 12.5
	// This creates: a + (b / c)
	auto addLeft = std::make_unique<LiteralExpression>(int64_t(10));

	auto divLeft = std::make_unique<LiteralExpression>(int64_t(5));
	auto divRight = std::make_unique<LiteralExpression>(int64_t(2));
	auto divExpr = std::make_unique<BinaryOpExpression>(std::move(divLeft), BinaryOperatorType::BOP_DIVIDE, std::move(divRight));

	BinaryOpExpression expr(std::move(addLeft), BinaryOperatorType::BOP_ADD, std::move(divExpr));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 12.5);
}

TEST_F(ComplexArithmeticTest, Complex_MultiplyAddMultiply) {
	// Test: a * b + c * a = 10 * 5 + 2 * 10 = 50 + 20 = 70
	// This creates: (a * b) + (c * a)
	auto mul1Left = std::make_unique<LiteralExpression>(int64_t(10));
	auto mul1Right = std::make_unique<LiteralExpression>(int64_t(5));
	auto mul1 = std::make_unique<BinaryOpExpression>(std::move(mul1Left), BinaryOperatorType::BOP_MULTIPLY, std::move(mul1Right));

	auto mul2Left = std::make_unique<LiteralExpression>(int64_t(2));
	auto mul2Right = std::make_unique<LiteralExpression>(int64_t(10));
	auto mul2 = std::make_unique<BinaryOpExpression>(std::move(mul2Left), BinaryOperatorType::BOP_MULTIPLY, std::move(mul2Right));

	BinaryOpExpression expr(std::move(mul1), BinaryOperatorType::BOP_ADD, std::move(mul2));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 70.0);
}

TEST_F(ComplexArithmeticTest, Complex_SubtractPower) {
	// Test: a - b ^ c = 10 - 5 ^ 2 = 10 - 25 = -15
	// This creates: a - (b ^ c)
	auto subLeft = std::make_unique<LiteralExpression>(int64_t(10));

	auto powLeft = std::make_unique<LiteralExpression>(int64_t(5));
	auto powRight = std::make_unique<LiteralExpression>(int64_t(2));
	auto powExpr = std::make_unique<BinaryOpExpression>(std::move(powLeft), BinaryOperatorType::BOP_POWER, std::move(powRight));

	BinaryOpExpression expr(std::move(subLeft), BinaryOperatorType::BOP_SUBTRACT, std::move(powExpr));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), -15.0);
}

TEST_F(ComplexArithmeticTest, VeryComplex_Expression) {
	// Test: a * b + c / d - e = 10 * 5 + 2 / 3.5 - 2.5 = 50 + 0.5714... - 2.5 = 48.0714...
	// This creates: ((a * b) + (c / d)) - e

	// Build: a * b
	auto mul1Left = std::make_unique<LiteralExpression>(int64_t(10));
	auto mul1Right = std::make_unique<LiteralExpression>(int64_t(5));
	auto mul1 = std::make_unique<BinaryOpExpression>(std::move(mul1Left), BinaryOperatorType::BOP_MULTIPLY, std::move(mul1Right));

	// Build: c / d
	auto divLeft = std::make_unique<LiteralExpression>(int64_t(2));
	auto divRight = std::make_unique<LiteralExpression>(double(3.5));
	auto div = std::make_unique<BinaryOpExpression>(std::move(divLeft), BinaryOperatorType::BOP_DIVIDE, std::move(divRight));

	// Build: (a * b) + (c / d)
	auto add = std::make_unique<BinaryOpExpression>(std::move(mul1), BinaryOperatorType::BOP_ADD, std::move(div));

	// Build: ((a * b) + (c / d)) - e
	auto subRight = std::make_unique<LiteralExpression>(double(2.5));
	BinaryOpExpression expr(std::move(add), BinaryOperatorType::BOP_SUBTRACT, std::move(subRight));

	auto result = evaluate(&expr);
	EXPECT_NEAR(getAsDouble(result), 48.0714, 0.001);
}

// ============================================================================
// Mixed Integer and Double Arithmetic
// ============================================================================

TEST_F(ComplexArithmeticTest, Mixed_IntDouble_Addition) {
	// Test: a + d = 10 + 3.5 = 13.5
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(double(3.5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 13.5);
}

TEST_F(ComplexArithmeticTest, Mixed_IntDouble_Multiplication) {
	// Test: a * e = 10 * 2.5 = 25.0
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(double(2.5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 25.0);
}

TEST_F(ComplexArithmeticTest, Mixed_DoubleInt_Division) {
	// Test: d / b = 3.5 / 5 = 0.7
	auto left = std::make_unique<LiteralExpression>(double(3.5));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 0.7);
}

// ============================================================================
// Unary Operator with Binary Operations
// ============================================================================

TEST_F(ComplexArithmeticTest, UnaryMinus_BinaryAdd) {
	// Test: -a + b = -10 + 5 = -5
	auto operand = std::make_unique<LiteralExpression>(int64_t(10));
	auto unaryExpr = std::make_unique<UnaryOpExpression>(UnaryOperatorType::UOP_MINUS, std::move(operand));

	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(unaryExpr), BinaryOperatorType::BOP_ADD, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), -5.0);
}

TEST_F(ComplexArithmeticTest, UnaryMinus_BothOperands) {
	// Test: -a + -b = -10 + -5 = -15
	auto operand1 = std::make_unique<LiteralExpression>(int64_t(10));
	auto unary1 = std::make_unique<UnaryOpExpression>(UnaryOperatorType::UOP_MINUS, std::move(operand1));

	auto operand2 = std::make_unique<LiteralExpression>(int64_t(5));
	auto unary2 = std::make_unique<UnaryOpExpression>(UnaryOperatorType::UOP_MINUS, std::move(operand2));

	BinaryOpExpression expr(std::move(unary1), BinaryOperatorType::BOP_ADD, std::move(unary2));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), -15.0);
}

TEST_F(ComplexArithmeticTest, UnaryMinus_Multiplication) {
	// Test: -a * b = -10 * 5 = -50
	auto operand = std::make_unique<LiteralExpression>(int64_t(10));
	auto unaryExpr = std::make_unique<UnaryOpExpression>(UnaryOperatorType::UOP_MINUS, std::move(operand));

	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(unaryExpr), BinaryOperatorType::BOP_MULTIPLY, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), -50.0);
}

// ============================================================================
// Variable References with Arithmetic
// ============================================================================

TEST_F(ComplexArithmeticTest, VariableRef_SimpleAdd) {
	// Test: a + b (where a=10, b=5) = 15
	auto left = std::make_unique<VariableReferenceExpression>("a");
	auto right = std::make_unique<VariableReferenceExpression>("b");
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 15.0);
}

TEST_F(ComplexArithmeticTest, VariableRef_ComplexExpression) {
	// Test: a * b + c / d - e
	// Where a=10, b=5, c=2, d=3.5, e=2.5
	// Result: 10 * 5 + 2 / 3.5 - 2.5 = 50 + 0.5714... - 2.5 = 48.0714...

	// Build: a * b
	auto mul1Left = std::make_unique<VariableReferenceExpression>("a");
	auto mul1Right = std::make_unique<VariableReferenceExpression>("b");
	auto mul1 = std::make_unique<BinaryOpExpression>(std::move(mul1Left), BinaryOperatorType::BOP_MULTIPLY, std::move(mul1Right));

	// Build: c / d
	auto divLeft = std::make_unique<VariableReferenceExpression>("c");
	auto divRight = std::make_unique<VariableReferenceExpression>("d");
	auto div = std::make_unique<BinaryOpExpression>(std::move(divLeft), BinaryOperatorType::BOP_DIVIDE, std::move(divRight));

	// Build: (a * b) + (c / d)
	auto add = std::make_unique<BinaryOpExpression>(std::move(mul1), BinaryOperatorType::BOP_ADD, std::move(div));

	// Build: ((a * b) + (c / d)) - e
	auto subRight = std::make_unique<VariableReferenceExpression>("e");
	BinaryOpExpression expr(std::move(add), BinaryOperatorType::BOP_SUBTRACT, std::move(subRight));

	auto result = evaluate(&expr);
	EXPECT_NEAR(getAsDouble(result), 48.0714, 0.001);
}

TEST_F(ComplexArithmeticTest, VariableRef_WithUnaryMinus) {
	// Test: -a + b (where a=10, b=5) = -5
	auto operand = std::make_unique<VariableReferenceExpression>("a");
	auto unaryExpr = std::make_unique<UnaryOpExpression>(UnaryOperatorType::UOP_MINUS, std::move(operand));

	auto right = std::make_unique<VariableReferenceExpression>("b");
	BinaryOpExpression expr(std::move(unaryExpr), BinaryOperatorType::BOP_ADD, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), -5.0);
}

// ============================================================================
// Edge Cases and Special Values
// ============================================================================

TEST_F(ComplexArithmeticTest, ZeroInExpression) {
	// Test: a * 0 + b = 10 * 0 + 5 = 5
	auto mulLeft = std::make_unique<LiteralExpression>(int64_t(10));
	auto mulRight = std::make_unique<LiteralExpression>(int64_t(0));
	auto mul = std::make_unique<BinaryOpExpression>(std::move(mulLeft), BinaryOperatorType::BOP_MULTIPLY, std::move(mulRight));

	auto addRight = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(mul), BinaryOperatorType::BOP_ADD, std::move(addRight));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 5.0);
}

TEST_F(ComplexArithmeticTest, NegativeNumbers) {
	// Test: (-5) + (-3) = -8
	auto left = std::make_unique<LiteralExpression>(int64_t(-5));
	auto right = std::make_unique<LiteralExpression>(int64_t(-3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), -8.0);
}

TEST_F(ComplexArithmeticTest, LargeNumbers) {
	// Test: 1000000 * 1000000 + 500000 = 1000000000000 + 500000 = 1000000500000
	auto mulLeft = std::make_unique<LiteralExpression>(int64_t(1000000));
	auto mulRight = std::make_unique<LiteralExpression>(int64_t(1000000));
	auto mul = std::make_unique<BinaryOpExpression>(std::move(mulLeft), BinaryOperatorType::BOP_MULTIPLY, std::move(mulRight));

	auto addRight = std::make_unique<LiteralExpression>(int64_t(500000));
	BinaryOpExpression expr(std::move(mul), BinaryOperatorType::BOP_ADD, std::move(addRight));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 1000000500000.0);
}

TEST_F(ComplexArithmeticTest, FractionalNumbers) {
	// Test: 0.1 * 0.2 + 0.3 = 0.02 + 0.3 = 0.32
	auto mulLeft = std::make_unique<LiteralExpression>(double(0.1));
	auto mulRight = std::make_unique<LiteralExpression>(double(0.2));
	auto mul = std::make_unique<BinaryOpExpression>(std::move(mulLeft), BinaryOperatorType::BOP_MULTIPLY, std::move(mulRight));

	auto addRight = std::make_unique<LiteralExpression>(double(0.3));
	BinaryOpExpression expr(std::move(mul), BinaryOperatorType::BOP_ADD, std::move(addRight));

	auto result = evaluate(&expr);
	EXPECT_NEAR(getAsDouble(result), 0.32, 0.0001);
}

// ============================================================================
// Nested Parentheses (Explicit Operator Precedence)
// ============================================================================

TEST_F(ComplexArithmeticTest, Nested_LeftAssociative) {
	// Test: (a + b) * c = (10 + 5) * 2 = 30
	auto addLeft = std::make_unique<LiteralExpression>(int64_t(10));
	auto addRight = std::make_unique<LiteralExpression>(int64_t(5));
	auto add = std::make_unique<BinaryOpExpression>(std::move(addLeft), BinaryOperatorType::BOP_ADD, std::move(addRight));

	auto mulRight = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(add), BinaryOperatorType::BOP_MULTIPLY, std::move(mulRight));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 30.0);
}

TEST_F(ComplexArithmeticTest, Nested_RightAssociative) {
	// Test: a * (b + c) = 10 * (5 + 2) = 70
	auto mulLeft = std::make_unique<LiteralExpression>(int64_t(10));

	auto addLeft = std::make_unique<LiteralExpression>(int64_t(5));
	auto addRight = std::make_unique<LiteralExpression>(int64_t(2));
	auto add = std::make_unique<BinaryOpExpression>(std::move(addLeft), BinaryOperatorType::BOP_ADD, std::move(addRight));

	BinaryOpExpression expr(std::move(mulLeft), BinaryOperatorType::BOP_MULTIPLY, std::move(add));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 70.0);
}

TEST_F(ComplexArithmeticTest, DeeplyNested) {
	// Test: ((a + b) * (c - a)) / d
	// = ((10 + 5) * (2 - 10)) / 3.5
	// = (15 * -8) / 3.5
	// = -120 / 3.5
	// = -34.2857...

	// Build: c - a
	auto subLeft = std::make_unique<LiteralExpression>(int64_t(2));
	auto subRight = std::make_unique<LiteralExpression>(int64_t(10));
	auto sub = std::make_unique<BinaryOpExpression>(std::move(subLeft), BinaryOperatorType::BOP_SUBTRACT, std::move(subRight));

	// Build: a + b
	auto addLeft = std::make_unique<LiteralExpression>(int64_t(10));
	auto addRight = std::make_unique<LiteralExpression>(int64_t(5));
	auto add = std::make_unique<BinaryOpExpression>(std::move(addLeft), BinaryOperatorType::BOP_ADD, std::move(addRight));

	// Build: (a + b) * (c - a)
	auto mul = std::make_unique<BinaryOpExpression>(std::move(add), BinaryOperatorType::BOP_MULTIPLY, std::move(sub));

	// Build: ((a + b) * (c - a)) / d
	auto divRight = std::make_unique<LiteralExpression>(double(3.5));
	BinaryOpExpression expr(std::move(mul), BinaryOperatorType::BOP_DIVIDE, std::move(divRight));

	auto result = evaluate(&expr);
	EXPECT_NEAR(getAsDouble(result), -34.2857, 0.001);
}

// ============================================================================
// Operator Precedence Verification
// ============================================================================

TEST_F(ComplexArithmeticTest, Precedence_MultiplyOverAdd) {
	// Test: a + b * c = 10 + 5 * 2 = 20 (not 30)
	// Without explicit parentheses, multiplication should happen first
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto middle = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));

	// Build: b * c
	auto mul = std::make_unique<BinaryOpExpression>(std::move(middle), BinaryOperatorType::BOP_MULTIPLY, std::move(right));

	// Build: a + (b * c)
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(mul));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 20.0);
}

TEST_F(ComplexArithmeticTest, Precedence_DivisionOverSubtract) {
	// Test: a - b / c = 10 - 5 / 2 = 7.5 (not 2.5)
	// Without explicit parentheses, division should happen first
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto middle = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));

	// Build: b / c
	auto div = std::make_unique<BinaryOpExpression>(std::move(middle), BinaryOperatorType::BOP_DIVIDE, std::move(right));

	// Build: a - (b / c)
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(div));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 7.5);
}

TEST_F(ComplexArithmeticTest, Precedence_PowerOverMultiply) {
	// Test: a * b ^ c = 10 * 5 ^ 2 = 10 * 25 = 250 (not 2500)
	// Without explicit parentheses, power should happen first
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto middle = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));

	// Build: b ^ c
	auto pow = std::make_unique<BinaryOpExpression>(std::move(middle), BinaryOperatorType::BOP_POWER, std::move(right));

	// Build: a * (b ^ c)
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(pow));

	auto result = evaluate(&expr);
	EXPECT_DOUBLE_EQ(getAsDouble(result), 250.0);
}
