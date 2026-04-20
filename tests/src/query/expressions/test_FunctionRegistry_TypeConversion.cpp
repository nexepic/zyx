/**
 * @file test_FunctionRegistry_TypeConversion.cpp
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
// Type Conversion Function Tests
// ============================================================================

TEST_F(FunctionRegistryTest, ToIntegerFunction_FromInteger) {
	const ScalarFunction* func = registry->lookupScalarFunction("tointeger");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 42);
}

TEST_F(FunctionRegistryTest, ToIntegerFunction_FromDouble) {
	const ScalarFunction* func = registry->lookupScalarFunction("tointeger");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(3.7));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 3);
}

TEST_F(FunctionRegistryTest, ToIntegerFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("tointeger");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ToIntegerFunction_Empty) {
	const ScalarFunction* func = registry->lookupScalarFunction("tointeger");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ToFloatFunction_FromInteger) {
	const ScalarFunction* func = registry->lookupScalarFunction("tofloat");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 42.0);
}

TEST_F(FunctionRegistryTest, ToFloatFunction_FromDouble) {
	const ScalarFunction* func = registry->lookupScalarFunction("tofloat");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(3.14));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.14);
}

TEST_F(FunctionRegistryTest, ToFloatFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("tofloat");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ToFloatFunction_Empty) {
	const ScalarFunction* func = registry->lookupScalarFunction("tofloat");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ToBooleanFunction_True) {
	const ScalarFunction* func = registry->lookupScalarFunction("toboolean");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(true));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(FunctionRegistryTest, ToBooleanFunction_False) {
	const ScalarFunction* func = registry->lookupScalarFunction("toboolean");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(false));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_FALSE(std::get<bool>(result.getVariant()));
}

TEST_F(FunctionRegistryTest, ToBooleanFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("toboolean");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ToBooleanFunction_Empty) {
	const ScalarFunction* func = registry->lookupScalarFunction("toboolean");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ToIntegerFunction_NullArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("toInteger");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ToFloatFunction_NullArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("toFloat");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ToBooleanFunction_NullArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("toBoolean");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}
