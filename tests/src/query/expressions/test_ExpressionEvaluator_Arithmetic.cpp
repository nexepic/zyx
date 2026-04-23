/**
 * @file test_ExpressionEvaluator_Arithmetic.cpp
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

#include "query/expressions/ExpressionEvaluatorTestFixture.hpp"
#include "graph/core/TemporalTypes.hpp"

// ============================================================================
// Arithmetic Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedIntDouble_Add) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(2.5);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 12.5);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedIntDouble_Subtract) {
	auto left = std::make_unique<LiteralExpression>(2.5);
	auto right = std::make_unique<LiteralExpression>(int64_t(1));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 1.5);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedIntDouble_Multiply) {
	auto left = std::make_unique<LiteralExpression>(2.0);
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 6.0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedIntDouble_Divide) {
	auto left = std::make_unique<LiteralExpression>(7.0);
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.5);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDivideByZero) {
	auto left = std::make_unique<LiteralExpression>(7.0);
	auto right = std::make_unique<LiteralExpression>(0.0);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	EXPECT_THROW(eval(&expr), DivisionByZeroException);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedModulo) {
	auto left = std::make_unique<LiteralExpression>(7.5);
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 1); // 7 % 3
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedModuloByZero) {
	auto left = std::make_unique<LiteralExpression>(7.0);
	auto right = std::make_unique<LiteralExpression>(0.0);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
	EXPECT_THROW(eval(&expr), DivisionByZeroException);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedPower) {
	auto left = std::make_unique<LiteralExpression>(2.0);
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_POWER, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 8.0);
}

// Integer division that is not evenly divisible -> double
TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerDivision_NotEvenlyDivisible) {
	auto left = std::make_unique<LiteralExpression>(int64_t(7));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.5);
}

// Integer division that IS evenly divisible -> integer
TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerDivision_EvenlyDivisible) {
	auto left = std::make_unique<LiteralExpression>(int64_t(6));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 3);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerDivideByZero) {
	auto left = std::make_unique<LiteralExpression>(int64_t(7));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	EXPECT_THROW(eval(&expr), DivisionByZeroException);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerModuloByZero) {
	auto left = std::make_unique<LiteralExpression>(int64_t(7));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
	EXPECT_THROW(eval(&expr), DivisionByZeroException);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerPower) {
	auto left = std::make_unique<LiteralExpression>(int64_t(2));
	auto right = std::make_unique<LiteralExpression>(int64_t(10));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_POWER, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 1024.0);
}

// String concatenation
TEST_F(ExpressionEvaluatorTest, Arithmetic_StringConcat) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello "));
	auto right = std::make_unique<LiteralExpression>(std::string("world"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello world");
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_StringConcatWithInt) {
	auto left = std::make_unique<LiteralExpression>(std::string("val="));
	auto right = std::make_unique<LiteralExpression>(int64_t(42));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "val=42");
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerSubtract) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 7);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerMultiply) {
	auto left = std::make_unique<LiteralExpression>(int64_t(4));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 20);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerModulo) {
	auto left = std::make_unique<LiteralExpression>(int64_t(7));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 1);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntConcatWithString) {
	auto left = std::make_unique<LiteralExpression>(int64_t(42));
	auto right = std::make_unique<LiteralExpression>(std::string(" items"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "42 items");
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleConcatWithString) {
	auto left = std::make_unique<LiteralExpression>(std::string("pi="));
	auto right = std::make_unique<LiteralExpression>(3.14);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	// toString for doubles may vary, just check it starts with "pi="
	auto str = std::get<std::string>(result.getVariant());
	EXPECT_TRUE(str.find("pi=") == 0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDouble_Add) {
	auto left = std::make_unique<LiteralExpression>(1.5);
	auto right = std::make_unique<LiteralExpression>(2.5);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 4.0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDouble_Subtract) {
	auto left = std::make_unique<LiteralExpression>(5.5);
	auto right = std::make_unique<LiteralExpression>(1.5);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 4.0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDouble_Multiply) {
	auto left = std::make_unique<LiteralExpression>(2.5);
	auto right = std::make_unique<LiteralExpression>(4.0);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 10.0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDouble_Divide) {
	auto left = std::make_unique<LiteralExpression>(7.5);
	auto right = std::make_unique<LiteralExpression>(2.5);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDouble_Power) {
	auto left = std::make_unique<LiteralExpression>(2.0);
	auto right = std::make_unique<LiteralExpression>(3.0);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_POWER, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 8.0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDouble_Modulo) {
	auto left = std::make_unique<LiteralExpression>(7.5);
	auto right = std::make_unique<LiteralExpression>(2.5);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	// toInteger(7.5) = 7, toInteger(2.5) = 2, 7 % 2 = 1
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 1);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerAdd) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(20));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 30);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_BoolConcatWithString) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(std::string(" flag"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntLeftBoolRight_Subtract) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(true); // toDouble(true)=1.0
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 9.0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_BoolBool_Add) {
	auto left = std::make_unique<LiteralExpression>(true);  // toDouble=1.0
	auto right = std::make_unique<LiteralExpression>(false); // toDouble=0.0
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 1.0);
}

// ============================================================================
// Temporal Arithmetic Tests (covers DATE+DURATION, DURATION+DATE,
// DATETIME+DURATION, DURATION+DATETIME branches in evaluateArithmetic)
// ============================================================================

// Helper: set temporal values in the record using a sub-fixture
class ExpressionEvaluatorTemporalTest : public ::testing::Test {
protected:
	void SetUp() override {
		record_.setValue("d", PropertyValue(TemporalDate::fromYMD(2024, 3, 15)));
		record_.setValue("dt", PropertyValue(TemporalDateTime::fromComponents(2024, 3, 15, 10, 30, 0)));
		record_.setValue("dur", PropertyValue(TemporalDuration{1, 5, 0})); // 1 month, 5 days
		context_ = std::make_unique<EvaluationContext>(record_);
	}

	PropertyValue eval(const Expression* expr) {
		ExpressionEvaluator evaluator(*context_);
		return evaluator.evaluate(expr);
	}

	Record record_;
	std::unique_ptr<EvaluationContext> context_;
};

// DATE + DURATION -> DATE
TEST_F(ExpressionEvaluatorTemporalTest, Arithmetic_DatePlusDuration) {
	auto left  = std::make_unique<VariableReferenceExpression>("d");
	auto right = std::make_unique<VariableReferenceExpression>("dur");
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DATE);
}

// DURATION + DATE -> DATE (commutative path)
TEST_F(ExpressionEvaluatorTemporalTest, Arithmetic_DurationPlusDate) {
	auto left  = std::make_unique<VariableReferenceExpression>("dur");
	auto right = std::make_unique<VariableReferenceExpression>("d");
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DATE);
}

// DATETIME + DURATION -> DATETIME
TEST_F(ExpressionEvaluatorTemporalTest, Arithmetic_DateTimePlusDuration) {
	auto left  = std::make_unique<VariableReferenceExpression>("dt");
	auto right = std::make_unique<VariableReferenceExpression>("dur");
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DATETIME);
}

// DURATION + DATETIME -> DATETIME (commutative path)
TEST_F(ExpressionEvaluatorTemporalTest, Arithmetic_DurationPlusDateTime) {
	auto left  = std::make_unique<VariableReferenceExpression>("dur");
	auto right = std::make_unique<VariableReferenceExpression>("dt");
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DATETIME);
}

// ============================================================================
// Temporal Component Access Tests (covers extractTemporalComponent)
// ============================================================================

// Accessing .year on a DATE variable triggers extractTemporalComponent for DATE
TEST_F(ExpressionEvaluatorTemporalTest, TemporalComponent_DateYear) {
	VariableReferenceExpression expr("d", "year");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 2024);
}

// Accessing .month on a DATE variable
TEST_F(ExpressionEvaluatorTemporalTest, TemporalComponent_DateMonth) {
	VariableReferenceExpression expr("d", "month");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 3);
}

// Accessing .day on a DATE variable
TEST_F(ExpressionEvaluatorTemporalTest, TemporalComponent_DateDay) {
	VariableReferenceExpression expr("d", "day");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 15);
}

// Accessing .epochDays on a DATE variable
TEST_F(ExpressionEvaluatorTemporalTest, TemporalComponent_DateEpochDays) {
	VariableReferenceExpression expr("d", "epochDays");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
}

// Accessing .year on a DATETIME variable triggers extractTemporalComponent for DATETIME
TEST_F(ExpressionEvaluatorTemporalTest, TemporalComponent_DateTimeYear) {
	VariableReferenceExpression expr("dt", "year");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 2024);
}

// Accessing .hour on a DATETIME variable
TEST_F(ExpressionEvaluatorTemporalTest, TemporalComponent_DateTimeHour) {
	VariableReferenceExpression expr("dt", "hour");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 10);
}

// Accessing .epochMillis on a DATETIME variable
TEST_F(ExpressionEvaluatorTemporalTest, TemporalComponent_DateTimeEpochMillis) {
	VariableReferenceExpression expr("dt", "epochMillis");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
}

// Accessing .months on a DURATION variable triggers extractTemporalComponent for DURATION
TEST_F(ExpressionEvaluatorTemporalTest, TemporalComponent_DurationMonths) {
	VariableReferenceExpression expr("dur", "months");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 1);
}

// Accessing .days on a DURATION variable
TEST_F(ExpressionEvaluatorTemporalTest, TemporalComponent_DurationDays) {
	VariableReferenceExpression expr("dur", "days");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 5);
}

// Accessing an unknown component returns NULL
TEST_F(ExpressionEvaluatorTemporalTest, TemporalComponent_UnknownComponent_ReturnsNull) {
	VariableReferenceExpression expr("d", "unknownField");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}
