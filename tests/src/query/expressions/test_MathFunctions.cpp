/**
 * @file test_MathFunctions.cpp
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

#include <cmath>
#include <gtest/gtest.h>
#include "graph/query/expressions/FunctionRegistry.hpp"

// Define M_PI for Windows/MSVC compatibility
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/execution/Record.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

// ============================================================================
// Test Fixture
// ============================================================================

class MathFunctionsTest : public ::testing::Test {
protected:
	void SetUp() override {
		registry = &FunctionRegistry::getInstance();

		record_.setValue("x", PropertyValue(42));
		record_.setValue("name", PropertyValue(std::string("Alice")));
		context_ = std::make_unique<EvaluationContext>(record_);
	}

	FunctionRegistry* registry;
	Record record_;
	std::unique_ptr<EvaluationContext> context_;
};

// ============================================================================
// log
// ============================================================================

TEST_F(MathFunctionsTest, Log_NaturalLog) {
	auto* func = registry->lookupScalarFunction("log");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(std::exp(1.0))};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 1.0, 1e-10);
}

TEST_F(MathFunctionsTest, Log_One) {
	auto* func = registry->lookupScalarFunction("log");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(1.0)};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 0.0, 1e-10);
}

TEST_F(MathFunctionsTest, Log_Negative) {
	auto* func = registry->lookupScalarFunction("log");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(-1.0)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(MathFunctionsTest, Log_Null) {
	auto* func = registry->lookupScalarFunction("log");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue()};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// log10
// ============================================================================

TEST_F(MathFunctionsTest, Log10_Hundred) {
	auto* func = registry->lookupScalarFunction("log10");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(100.0)};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 2.0, 1e-10);
}

TEST_F(MathFunctionsTest, Log10_Zero) {
	auto* func = registry->lookupScalarFunction("log10");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(0.0)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// exp
// ============================================================================

TEST_F(MathFunctionsTest, Exp_Zero) {
	auto* func = registry->lookupScalarFunction("exp");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(0.0)};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 1.0, 1e-10);
}

TEST_F(MathFunctionsTest, Exp_One) {
	auto* func = registry->lookupScalarFunction("exp");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(1.0)};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 2.718281828, 1e-6);
}

// ============================================================================
// pow
// ============================================================================

TEST_F(MathFunctionsTest, Pow_TwoCubed) {
	auto* func = registry->lookupScalarFunction("pow");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(2.0), PropertyValue(3.0)};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 8.0, 1e-10);
}

TEST_F(MathFunctionsTest, Pow_NullBase) {
	auto* func = registry->lookupScalarFunction("pow");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(), PropertyValue(2.0)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// sin
// ============================================================================

TEST_F(MathFunctionsTest, Sin_Zero) {
	auto* func = registry->lookupScalarFunction("sin");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(0.0)};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 0.0, 1e-10);
}

TEST_F(MathFunctionsTest, Sin_PiOver2) {
	auto* func = registry->lookupScalarFunction("sin");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(M_PI / 2.0)};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 1.0, 1e-10);
}

// ============================================================================
// cos
// ============================================================================

TEST_F(MathFunctionsTest, Cos_Zero) {
	auto* func = registry->lookupScalarFunction("cos");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(0.0)};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 1.0, 1e-10);
}

// ============================================================================
// tan
// ============================================================================

TEST_F(MathFunctionsTest, Tan_Zero) {
	auto* func = registry->lookupScalarFunction("tan");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(0.0)};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 0.0, 1e-10);
}

// ============================================================================
// asin
// ============================================================================

TEST_F(MathFunctionsTest, Asin_One) {
	auto* func = registry->lookupScalarFunction("asin");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(1.0)};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), M_PI / 2.0, 1e-10);
}

TEST_F(MathFunctionsTest, Asin_DomainError) {
	auto* func = registry->lookupScalarFunction("asin");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(2.0)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// acos
// ============================================================================

TEST_F(MathFunctionsTest, Acos_One) {
	auto* func = registry->lookupScalarFunction("acos");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(1.0)};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 0.0, 1e-10);
}

TEST_F(MathFunctionsTest, Acos_DomainError) {
	auto* func = registry->lookupScalarFunction("acos");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(2.0)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// atan
// ============================================================================

TEST_F(MathFunctionsTest, Atan_Zero) {
	auto* func = registry->lookupScalarFunction("atan");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(0.0)};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 0.0, 1e-10);
}

// ============================================================================
// atan2
// ============================================================================

TEST_F(MathFunctionsTest, Atan2_OneOne) {
	auto* func = registry->lookupScalarFunction("atan2");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(1.0), PropertyValue(1.0)};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), M_PI / 4.0, 1e-10);
}

// ============================================================================
// rand
// ============================================================================

TEST_F(MathFunctionsTest, Rand_InRange) {
	auto* func = registry->lookupScalarFunction("rand");
	ASSERT_NE(func, nullptr);

	for (int i = 0; i < 20; ++i) {
		std::vector<PropertyValue> args;
		auto result = func->evaluate(args, *context_);
		ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
		double val = std::get<double>(result.getVariant());
		EXPECT_GE(val, 0.0);
		EXPECT_LT(val, 1.0);
	}
}

// ============================================================================
// pi
// ============================================================================

TEST_F(MathFunctionsTest, Pi_Value) {
	auto* func = registry->lookupScalarFunction("pi");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 3.14159265358979, 1e-10);
}

// ============================================================================
// e
// ============================================================================

TEST_F(MathFunctionsTest, E_Value) {
	auto* func = registry->lookupScalarFunction("e");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 2.71828182845904, 1e-10);
}

// ============================================================================
// upper (toUpper)
// ============================================================================

TEST_F(MathFunctionsTest, ToUpper_Hello) {
	auto* func = registry->lookupScalarFunction("upper");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(std::string("hello"))};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "HELLO");
}

// ============================================================================
// lower (toLower)
// ============================================================================

TEST_F(MathFunctionsTest, ToLower_HELLO) {
	auto* func = registry->lookupScalarFunction("lower");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args = {PropertyValue(std::string("HELLO"))};
	auto result = func->evaluate(args, *context_);
	ASSERT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
}
