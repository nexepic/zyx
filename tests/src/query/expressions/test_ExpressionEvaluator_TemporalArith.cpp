/**
 * @file test_ExpressionEvaluator_TemporalArith.cpp
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "query/expressions/ExpressionEvaluatorTestFixture.hpp"
#include "graph/core/TemporalTypes.hpp"

class TemporalArithmeticTest : public ::testing::Test {
protected:
	void SetUp() override {
		record_.setValue("d1", PropertyValue(TemporalDate::fromYMD(2024, 3, 15)));
		record_.setValue("d2", PropertyValue(TemporalDate::fromYMD(2024, 1, 10)));
		record_.setValue("dt1", PropertyValue(TemporalDateTime::fromComponents(2024, 3, 15, 10, 30, 45)));
		record_.setValue("dt2", PropertyValue(TemporalDateTime::fromComponents(2024, 1, 10, 8, 15, 20)));
		record_.setValue("dur1", PropertyValue(TemporalDuration{2, 10, 5000000000LL})); // 2 months, 10 days, 5s
		record_.setValue("dur2", PropertyValue(TemporalDuration{1, 3, 2000000000LL}));  // 1 month, 3 days, 2s
		record_.setValue("durNeg", PropertyValue(TemporalDuration{-15, 0, 0})); // negative months for m<=0 branch
		context_ = std::make_unique<EvaluationContext>(record_);
	}

	PropertyValue eval(const Expression* expr) {
		ExpressionEvaluator evaluator(*context_);
		return evaluator.evaluate(expr);
	}

	Record record_;
	std::unique_ptr<EvaluationContext> context_;
};

// DURATION + DURATION -> DURATION
TEST_F(TemporalArithmeticTest, DurationPlusDuration) {
	auto left = std::make_unique<VariableReferenceExpression>("dur1");
	auto right = std::make_unique<VariableReferenceExpression>("dur2");
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DURATION);
	const auto& dur = std::get<TemporalDuration>(result.getVariant());
	EXPECT_EQ(dur.months, 3);
	EXPECT_EQ(dur.days, 13);
	EXPECT_EQ(dur.nanos, 7000000000LL);
}

// DURATION - DURATION -> DURATION
TEST_F(TemporalArithmeticTest, DurationMinusDuration) {
	auto left = std::make_unique<VariableReferenceExpression>("dur1");
	auto right = std::make_unique<VariableReferenceExpression>("dur2");
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DURATION);
	const auto& dur = std::get<TemporalDuration>(result.getVariant());
	EXPECT_EQ(dur.months, 1);
	EXPECT_EQ(dur.days, 7);
}

// DATE - DURATION -> DATE
TEST_F(TemporalArithmeticTest, DateMinusDuration) {
	auto left = std::make_unique<VariableReferenceExpression>("d1");
	auto right = std::make_unique<VariableReferenceExpression>("dur2");
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DATE);
}

// DATETIME - DURATION -> DATETIME
TEST_F(TemporalArithmeticTest, DateTimeMinusDuration) {
	auto left = std::make_unique<VariableReferenceExpression>("dt1");
	auto right = std::make_unique<VariableReferenceExpression>("dur2");
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DATETIME);
}

// DATE - DATE -> DURATION
TEST_F(TemporalArithmeticTest, DateMinusDate) {
	auto left = std::make_unique<VariableReferenceExpression>("d1");
	auto right = std::make_unique<VariableReferenceExpression>("d2");
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DURATION);
	const auto& dur = std::get<TemporalDuration>(result.getVariant());
	EXPECT_EQ(dur.months, 0);
	// The difference in days between 2024-03-15 and 2024-01-10
	EXPECT_NE(dur.days, 0);
}

// DATETIME - DATETIME -> DURATION
TEST_F(TemporalArithmeticTest, DateTimeMinusDateTime) {
	auto left = std::make_unique<VariableReferenceExpression>("dt1");
	auto right = std::make_unique<VariableReferenceExpression>("dt2");
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DURATION);
}

// DATE + DURATION with negative months (m <= 0 branch)
TEST_F(TemporalArithmeticTest, DatePlusDuration_NegativeMonthAdjust) {
	auto left = std::make_unique<VariableReferenceExpression>("d2");
	auto right = std::make_unique<VariableReferenceExpression>("durNeg");
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DATE);
}

// DATETIME + DURATION with negative months (m <= 0 branch)
TEST_F(TemporalArithmeticTest, DateTimePlusDuration_NegativeMonthAdjust) {
	auto left = std::make_unique<VariableReferenceExpression>("dt2");
	auto right = std::make_unique<VariableReferenceExpression>("durNeg");
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DATETIME);
}

// Temporal component access: datetime month, day, second
TEST_F(TemporalArithmeticTest, TemporalComponent_DateTimeMonth) {
	VariableReferenceExpression expr("dt1", "month");
	auto result = eval(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 3);
}

TEST_F(TemporalArithmeticTest, TemporalComponent_DateTimeDay) {
	VariableReferenceExpression expr("dt1", "day");
	auto result = eval(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 15);
}

TEST_F(TemporalArithmeticTest, TemporalComponent_DateTimeMinute) {
	VariableReferenceExpression expr("dt1", "minute");
	auto result = eval(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 30);
}

TEST_F(TemporalArithmeticTest, TemporalComponent_DateTimeSecond) {
	VariableReferenceExpression expr("dt1", "second");
	auto result = eval(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 45);
}

// Duration component: seconds and nanoseconds
TEST_F(TemporalArithmeticTest, TemporalComponent_DurationSeconds) {
	VariableReferenceExpression expr("dur1", "seconds");
	auto result = eval(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 5);
}

TEST_F(TemporalArithmeticTest, TemporalComponent_DurationNanoseconds) {
	VariableReferenceExpression expr("dur1", "nanoseconds");
	auto result = eval(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 5000000000LL);
}

// Unknown component on datetime returns null
TEST_F(TemporalArithmeticTest, TemporalComponent_DateTimeUnknown) {
	VariableReferenceExpression expr("dt1", "unknown");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// Unknown component on duration returns null
TEST_F(TemporalArithmeticTest, TemporalComponent_DurationUnknown) {
	VariableReferenceExpression expr("dur1", "unknown");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// Property access on non-temporal variable returns null
TEST_F(TemporalArithmeticTest, VarRef_PropertyOnNonTemporal_ReturnsNull) {
	record_.setValue("num", PropertyValue(int64_t(42)));
	context_ = std::make_unique<EvaluationContext>(record_);
	VariableReferenceExpression expr("num", "year");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}
