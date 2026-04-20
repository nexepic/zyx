/**
 * @file test_FunctionRegistry_MathFunctions.cpp
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

#include "query/expressions/FunctionRegistryTestFixture.hpp"

// ============================================================================
// Math Function Tests
// ============================================================================

TEST_F(FunctionRegistryTest, AbsFunction_Positive) {
	const ScalarFunction* func = registry->lookupScalarFunction("abs");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 42);
}

TEST_F(FunctionRegistryTest, AbsFunction_Negative) {
	const ScalarFunction* func = registry->lookupScalarFunction("abs");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(-42)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 42);
}

TEST_F(FunctionRegistryTest, AbsFunction_Double) {
	const ScalarFunction* func = registry->lookupScalarFunction("abs");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(-3.14));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.14);
}

TEST_F(FunctionRegistryTest, AbsFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("abs");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, CeilFunction_Integer) {
	const ScalarFunction* func = registry->lookupScalarFunction("ceil");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(3)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.0);
}

TEST_F(FunctionRegistryTest, CeilFunction_DoubleUp) {
	const ScalarFunction* func = registry->lookupScalarFunction("ceil");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(3.7));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 4.0);
}

TEST_F(FunctionRegistryTest, CeilFunction_DoubleDown) {
	const ScalarFunction* func = registry->lookupScalarFunction("ceil");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(3.2));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 4.0);
}

TEST_F(FunctionRegistryTest, CeilFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("ceil");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, FloorFunction_Integer) {
	const ScalarFunction* func = registry->lookupScalarFunction("floor");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(3)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.0);
}

TEST_F(FunctionRegistryTest, FloorFunction_DoubleDown) {
	const ScalarFunction* func = registry->lookupScalarFunction("floor");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(3.7));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.0);
}

TEST_F(FunctionRegistryTest, FloorFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("floor");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RoundFunction_Down) {
	const ScalarFunction* func = registry->lookupScalarFunction("round");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(3.4));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.0);
}

TEST_F(FunctionRegistryTest, RoundFunction_Up) {
	const ScalarFunction* func = registry->lookupScalarFunction("round");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(3.5));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 4.0);
}

TEST_F(FunctionRegistryTest, RoundFunction_Integer) {
	const ScalarFunction* func = registry->lookupScalarFunction("round");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(5)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 5.0);
}

TEST_F(FunctionRegistryTest, RoundFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("round");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, SqrtFunction_PerfectSquare) {
	const ScalarFunction* func = registry->lookupScalarFunction("sqrt");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(16.0));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 4.0);
}

TEST_F(FunctionRegistryTest, SqrtFunction_NonSquare) {
	const ScalarFunction* func = registry->lookupScalarFunction("sqrt");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(2.0));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 1.414, 0.001);
}

TEST_F(FunctionRegistryTest, SqrtFunction_Negative) {
	const ScalarFunction* func = registry->lookupScalarFunction("sqrt");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(-4.0));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, SqrtFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("sqrt");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, SignFunction_Positive) {
	const ScalarFunction* func = registry->lookupScalarFunction("sign");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(42.0));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 1);
}

TEST_F(FunctionRegistryTest, SignFunction_Negative) {
	const ScalarFunction* func = registry->lookupScalarFunction("sign");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(-42.0));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), -1);
}

TEST_F(FunctionRegistryTest, SignFunction_Zero) {
	const ScalarFunction* func = registry->lookupScalarFunction("sign");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(0.0));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(FunctionRegistryTest, SignFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("sign");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, CoalesceFunction_SingleArgument) {
	const ScalarFunction* func = registry->lookupScalarFunction("coalesce");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 42);
}

TEST_F(FunctionRegistryTest, RangeFunction_SingleValue) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(1)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 2UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 1);
}

TEST_F(FunctionRegistryTest, TailFunction_SingleElement) {
	const ScalarFunction* func = registry->lookupScalarFunction("tail");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list = {PropertyValue(int64_t(10))};
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& rlist = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(rlist.size(), 0UL);
}

TEST_F(FunctionRegistryTest, AbsFunction_StringToDouble) {
	const ScalarFunction* func = registry->lookupScalarFunction("abs");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("-5")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 5.0);
}

TEST_F(FunctionRegistryTest, AbsFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("abs");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, CeilFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("ceil");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, FloorFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("floor");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RoundFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("round");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, SqrtFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("sqrt");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, SignFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("sign");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LogFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("log");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LogFunction_NegativeValue) {
	const ScalarFunction* func = registry->lookupScalarFunction("log");
	std::vector<PropertyValue> args{PropertyValue(-1.0)};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LogFunction_Zero) {
	const ScalarFunction* func = registry->lookupScalarFunction("log");
	std::vector<PropertyValue> args{PropertyValue(0.0)};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LogFunction_ValidValue) {
	const ScalarFunction* func = registry->lookupScalarFunction("log");
	std::vector<PropertyValue> args{PropertyValue(2.718281828)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 1.0, 0.001);
}

TEST_F(FunctionRegistryTest, Log10Function_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("log10");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Log10Function_NegativeValue) {
	const ScalarFunction* func = registry->lookupScalarFunction("log10");
	std::vector<PropertyValue> args{PropertyValue(-5.0)};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, Log10Function_ValidValue) {
	const ScalarFunction* func = registry->lookupScalarFunction("log10");
	std::vector<PropertyValue> args{PropertyValue(100.0)};
	auto result = func->evaluate(args, *context_);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 2.0, 0.001);
}

TEST_F(FunctionRegistryTest, ExpFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("exp");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ExpFunction_ValidValue) {
	const ScalarFunction* func = registry->lookupScalarFunction("exp");
	std::vector<PropertyValue> args{PropertyValue(1.0)};
	auto result = func->evaluate(args, *context_);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 2.71828, 0.001);
}

TEST_F(FunctionRegistryTest, PowFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("pow");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue(), PropertyValue(2.0)};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, PowFunction_ValidValue) {
	const ScalarFunction* func = registry->lookupScalarFunction("pow");
	std::vector<PropertyValue> args{PropertyValue(2.0), PropertyValue(3.0)};
	auto result = func->evaluate(args, *context_);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 8.0, 0.001);
}

TEST_F(FunctionRegistryTest, SinFunction_NullAndValid) {
	const ScalarFunction* func = registry->lookupScalarFunction("sin");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> nullArgs{PropertyValue()};
	EXPECT_EQ(func->evaluate(nullArgs, *context_).getType(), PropertyType::NULL_TYPE);
	std::vector<PropertyValue> validArgs{PropertyValue(0.0)};
	EXPECT_NEAR(std::get<double>(func->evaluate(validArgs, *context_).getVariant()), 0.0, 0.001);
}

TEST_F(FunctionRegistryTest, CosFunction_NullAndValid) {
	const ScalarFunction* func = registry->lookupScalarFunction("cos");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> nullArgs{PropertyValue()};
	EXPECT_EQ(func->evaluate(nullArgs, *context_).getType(), PropertyType::NULL_TYPE);
	std::vector<PropertyValue> validArgs{PropertyValue(0.0)};
	EXPECT_NEAR(std::get<double>(func->evaluate(validArgs, *context_).getVariant()), 1.0, 0.001);
}

TEST_F(FunctionRegistryTest, TanFunction_NullAndValid) {
	const ScalarFunction* func = registry->lookupScalarFunction("tan");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> nullArgs{PropertyValue()};
	EXPECT_EQ(func->evaluate(nullArgs, *context_).getType(), PropertyType::NULL_TYPE);
	std::vector<PropertyValue> validArgs{PropertyValue(0.0)};
	EXPECT_NEAR(std::get<double>(func->evaluate(validArgs, *context_).getVariant()), 0.0, 0.001);
}

TEST_F(FunctionRegistryTest, AsinFunction_NullValidDomainError) {
	const ScalarFunction* func = registry->lookupScalarFunction("asin");
	ASSERT_NE(func, nullptr);
	// null
	std::vector<PropertyValue> nullArgs{PropertyValue()};
	EXPECT_EQ(func->evaluate(nullArgs, *context_).getType(), PropertyType::NULL_TYPE);
	// domain error
	std::vector<PropertyValue> domainArgs{PropertyValue(2.0)};
	EXPECT_EQ(func->evaluate(domainArgs, *context_).getType(), PropertyType::NULL_TYPE);
	// valid
	std::vector<PropertyValue> validArgs{PropertyValue(0.5)};
	auto result = func->evaluate(validArgs, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
}

TEST_F(FunctionRegistryTest, AcosFunction_NullValidDomainError) {
	const ScalarFunction* func = registry->lookupScalarFunction("acos");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> nullArgs{PropertyValue()};
	EXPECT_EQ(func->evaluate(nullArgs, *context_).getType(), PropertyType::NULL_TYPE);
	std::vector<PropertyValue> domainArgs{PropertyValue(-2.0)};
	EXPECT_EQ(func->evaluate(domainArgs, *context_).getType(), PropertyType::NULL_TYPE);
	std::vector<PropertyValue> validArgs{PropertyValue(0.5)};
	EXPECT_EQ(func->evaluate(validArgs, *context_).getType(), PropertyType::DOUBLE);
}

TEST_F(FunctionRegistryTest, AtanFunction_NullAndValid) {
	const ScalarFunction* func = registry->lookupScalarFunction("atan");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> nullArgs{PropertyValue()};
	EXPECT_EQ(func->evaluate(nullArgs, *context_).getType(), PropertyType::NULL_TYPE);
	std::vector<PropertyValue> validArgs{PropertyValue(1.0)};
	EXPECT_EQ(func->evaluate(validArgs, *context_).getType(), PropertyType::DOUBLE);
}

TEST_F(FunctionRegistryTest, Atan2Function_NullAndValid) {
	const ScalarFunction* func = registry->lookupScalarFunction("atan2");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> nullArgs{PropertyValue(), PropertyValue(1.0)};
	EXPECT_EQ(func->evaluate(nullArgs, *context_).getType(), PropertyType::NULL_TYPE);
	std::vector<PropertyValue> validArgs{PropertyValue(1.0), PropertyValue(1.0)};
	EXPECT_EQ(func->evaluate(validArgs, *context_).getType(), PropertyType::DOUBLE);
}

TEST_F(FunctionRegistryTest, PiFunction) {
	const ScalarFunction* func = registry->lookupScalarFunction("pi");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args;
	auto result = func->evaluate(args, *context_);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 3.14159, 0.001);
}

TEST_F(FunctionRegistryTest, EFunction) {
	const ScalarFunction* func = registry->lookupScalarFunction("e");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args;
	auto result = func->evaluate(args, *context_);
	EXPECT_NEAR(std::get<double>(result.getVariant()), 2.71828, 0.001);
}

TEST_F(FunctionRegistryTest, RandFunction) {
	const ScalarFunction* func = registry->lookupScalarFunction("rand");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args;
	auto result = func->evaluate(args, *context_);
	double val = std::get<double>(result.getVariant());
	EXPECT_GE(val, 0.0);
	EXPECT_LT(val, 1.0);
}
