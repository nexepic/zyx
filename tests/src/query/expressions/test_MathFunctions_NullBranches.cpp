/**
 * @file test_MathFunctions_NullBranches.cpp
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include <cmath>
#include <gtest/gtest.h>
#include "query/expressions/FunctionRegistryTestFixture.hpp"

// ============================================================================
// Null/empty-args branches for math functions
// ============================================================================

TEST_F(FunctionRegistryTest, Abs_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("abs");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Abs_Double) {
	auto* func = registry->lookupScalarFunction("abs");
	std::vector<PropertyValue> args = {PropertyValue(-3.5)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.5);
}

TEST_F(FunctionRegistryTest, Abs_Boolean_FallbackToDouble) {
	auto* func = registry->lookupScalarFunction("abs");
	std::vector<PropertyValue> args = {PropertyValue(true)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
}

TEST_F(FunctionRegistryTest, Ceil_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("ceil");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Floor_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("floor");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Round_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("round");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Sqrt_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("sqrt");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Sqrt_NegativeValue_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("sqrt");
	std::vector<PropertyValue> args = {PropertyValue(-4.0)};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Sign_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("sign");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Sign_Positive) {
	auto* func = registry->lookupScalarFunction("sign");
	std::vector<PropertyValue> args = {PropertyValue(5.0)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 1);
}

TEST_F(FunctionRegistryTest, Sign_Negative) {
	auto* func = registry->lookupScalarFunction("sign");
	std::vector<PropertyValue> args = {PropertyValue(-5.0)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), -1);
}

TEST_F(FunctionRegistryTest, Sign_Zero) {
	auto* func = registry->lookupScalarFunction("sign");
	std::vector<PropertyValue> args = {PropertyValue(0.0)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(FunctionRegistryTest, Log_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("log");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Log_Zero_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("log");
	std::vector<PropertyValue> args = {PropertyValue(0.0)};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Log10_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("log10");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Log10_Negative_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("log10");
	std::vector<PropertyValue> args = {PropertyValue(-5.0)};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Exp_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("exp");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Pow_NullExponent_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("pow");
	std::vector<PropertyValue> args = {PropertyValue(2.0), PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Pow_TooFewArgs_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("pow");
	std::vector<PropertyValue> args = {PropertyValue(2.0)};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Sin_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("sin");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Cos_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("cos");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Tan_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("tan");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Asin_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("asin");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Asin_BelowNegativeOne_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("asin");
	std::vector<PropertyValue> args = {PropertyValue(-1.5)};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Acos_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("acos");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Acos_BelowNegativeOne_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("acos");
	std::vector<PropertyValue> args = {PropertyValue(-1.5)};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Atan_Null_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("atan");
	std::vector<PropertyValue> args = {PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Atan2_Null_Y_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("atan2");
	std::vector<PropertyValue> args = {PropertyValue(), PropertyValue(1.0)};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Atan2_Null_X_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("atan2");
	std::vector<PropertyValue> args = {PropertyValue(1.0), PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Atan2_TooFewArgs_ReturnsNull) {
	auto* func = registry->lookupScalarFunction("atan2");
	std::vector<PropertyValue> args = {PropertyValue(1.0)};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Abs_PositiveInteger) {
	auto* func = registry->lookupScalarFunction("abs");
	std::vector<PropertyValue> args = {PropertyValue(int64_t(7))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 7);
}
