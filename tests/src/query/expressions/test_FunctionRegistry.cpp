/**
 * @file test_FunctionRegistry.cpp
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
// FunctionRegistry API Tests
// ============================================================================

TEST_F(FunctionRegistryTest, GetInstance_ReturnsSameInstance) {
	FunctionRegistry& instance1 = FunctionRegistry::getInstance();
	FunctionRegistry& instance2 = FunctionRegistry::getInstance();
	EXPECT_EQ(&instance1, &instance2);
}

TEST_F(FunctionRegistryTest, Lookup_CaseInsensitive) {
	const ScalarFunction* upper1 = registry->lookupScalarFunction("UPPER");
	const ScalarFunction* upper2 = registry->lookupScalarFunction("upper");
	const ScalarFunction* upper3 = registry->lookupScalarFunction("UpPeR");

	ASSERT_NE(upper1, nullptr);
	ASSERT_NE(upper2, nullptr);
	ASSERT_NE(upper3, nullptr);
	EXPECT_EQ(upper1, upper2);
	EXPECT_EQ(upper2, upper3);
}

TEST_F(FunctionRegistryTest, Lookup_UnknownFunction) {
	const ScalarFunction* func = registry->lookupScalarFunction("unknownFunction");
	EXPECT_EQ(func, nullptr);
}

TEST_F(FunctionRegistryTest, HasFunction_Existing) {
	EXPECT_TRUE(registry->hasFunction("upper"));
	EXPECT_TRUE(registry->hasFunction("LOWER"));
	EXPECT_TRUE(registry->hasFunction("Abs"));
}

TEST_F(FunctionRegistryTest, HasFunction_NonExisting) {
	EXPECT_FALSE(registry->hasFunction("nonexistent"));
	EXPECT_FALSE(registry->hasFunction("foo"));
}

TEST_F(FunctionRegistryTest, GetRegisteredFunctionNames_NotEmpty) {
	auto names = registry->getRegisteredFunctionNames();
	EXPECT_GT(names.size(), 0UL);
}

TEST_F(FunctionRegistryTest, GetRegisteredFunctionNames_ContainsBuiltIns) {
	auto names = registry->getRegisteredFunctionNames();

	// Check that some built-in functions are present
	bool hasUpper = false;
	bool hasAbs = false;
	bool hasCoalesce = false;

	for (const auto& name : names) {
		if (name == "upper") hasUpper = true;
		if (name == "abs") hasAbs = true;
		if (name == "coalesce") hasCoalesce = true;
	}

	EXPECT_TRUE(hasUpper);
	EXPECT_TRUE(hasAbs);
	EXPECT_TRUE(hasCoalesce);
}

TEST_F(FunctionRegistryTest, RegisterFunction_CustomFunction) {
	// Create a custom function
	class CustomFunction : public ScalarFunction {
	public:
		CustomFunction() : ScalarFunction(FunctionSignature("customDouble", 1, 1)) {}
		[[nodiscard]] PropertyValue evaluate(
			const std::vector<PropertyValue>& args,
			[[maybe_unused]] const EvaluationContext& context
		) const override {
			if (args.empty()) return PropertyValue();
			return PropertyValue(std::get<int64_t>(args[0].getVariant()) * 2);
		}
	};

	// Register it
	registry->registerFunction(std::make_unique<CustomFunction>());

	// Look it up
	const ScalarFunction* func = registry->lookupScalarFunction("customDouble");
	ASSERT_NE(func, nullptr);

	// Call it
	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(5)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 10);
}

TEST_F(FunctionRegistryTest, ValidateArgCount_FixedArgs) {
	class TestFunction : public ScalarFunction {
	public:
		TestFunction() : ScalarFunction(FunctionSignature("test", 2, 2)) {}
		[[nodiscard]] PropertyValue evaluate(
			const std::vector<PropertyValue>&,
			const EvaluationContext&
		) const override { return PropertyValue(); }
	};

	TestFunction func;
	EXPECT_TRUE(func.validateArgCount(2));
	EXPECT_FALSE(func.validateArgCount(1));
	EXPECT_FALSE(func.validateArgCount(3));
}

TEST_F(FunctionRegistryTest, ValidateArgCount_Variadic) {
	class TestFunction : public ScalarFunction {
	public:
		TestFunction() : ScalarFunction(FunctionSignature("test", 1, SIZE_MAX, true)) {}
		[[nodiscard]] PropertyValue evaluate(
			const std::vector<PropertyValue>&,
			const EvaluationContext&
		) const override { return PropertyValue(); }
	};

	TestFunction func;
	EXPECT_TRUE(func.validateArgCount(1));
	EXPECT_TRUE(func.validateArgCount(2));
	EXPECT_TRUE(func.validateArgCount(10));
	EXPECT_FALSE(func.validateArgCount(0));
}

TEST_F(FunctionRegistryTest, TimestampFunction_ReturnsInteger) {
	const ScalarFunction* func = registry->lookupScalarFunction("timestamp");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_GT(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(FunctionRegistryTest, TimestampFunction_IncreasingValues) {
	const ScalarFunction* func = registry->lookupScalarFunction("timestamp");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result1 = func->evaluate(args, *context_);
	PropertyValue result2 = func->evaluate(args, *context_);
	EXPECT_GE(std::get<int64_t>(result2.getVariant()), std::get<int64_t>(result1.getVariant()));
}

TEST_F(FunctionRegistryTest, RandomUUIDFunction_ReturnsString) {
	const ScalarFunction* func = registry->lookupScalarFunction("randomuuid");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	const auto& uuid = std::get<std::string>(result.getVariant());
	EXPECT_EQ(uuid.size(), 36UL);
	EXPECT_EQ(uuid[8], '-');
	EXPECT_EQ(uuid[13], '-');
	EXPECT_EQ(uuid[14], '4'); // Version 4
	EXPECT_EQ(uuid[18], '-');
	EXPECT_EQ(uuid[23], '-');
}

TEST_F(FunctionRegistryTest, RandomUUIDFunction_UniqueValues) {
	const ScalarFunction* func = registry->lookupScalarFunction("randomuuid");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result1 = func->evaluate(args, *context_);
	PropertyValue result2 = func->evaluate(args, *context_);
	EXPECT_NE(std::get<std::string>(result1.getVariant()),
	           std::get<std::string>(result2.getVariant()));
}

TEST_F(FunctionRegistryTest, TimestampFunction) {
	const ScalarFunction* func = registry->lookupScalarFunction("timestamp");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args;
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
}

TEST_F(FunctionRegistryTest, RandomUUIDFunction) {
	const ScalarFunction* func = registry->lookupScalarFunction("randomUUID");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args;
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	auto& uuid = std::get<std::string>(result.getVariant());
	EXPECT_EQ(uuid.length(), 36u);
	EXPECT_EQ(uuid[14], '4'); // version 4
}
