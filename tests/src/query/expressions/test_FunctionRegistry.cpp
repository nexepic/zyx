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

#include <gtest/gtest.h>
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/execution/Record.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

// ============================================================================
// Test Fixture
// ============================================================================

class FunctionRegistryTest : public ::testing::Test {
protected:
	void SetUp() override {
		registry = &FunctionRegistry::getInstance();

		// Create a basic record for context
		record_.setValue("x", PropertyValue(42));
		record_.setValue("name", PropertyValue(std::string("Alice")));
		context_ = std::make_unique<EvaluationContext>(record_);
	}

	FunctionRegistry* registry;
	Record record_;
	std::unique_ptr<EvaluationContext> context_;
};

// ============================================================================
// FunctionRegistry Class Tests
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

// ============================================================================
// String Function Tests - ToString
// ============================================================================

TEST_F(FunctionRegistryTest, ToStringFunction_Integer) {
	const ScalarFunction* func = registry->lookupScalarFunction("tostring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "42");
}

TEST_F(FunctionRegistryTest, ToStringFunction_Double) {
	const ScalarFunction* func = registry->lookupScalarFunction("tostring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(3.14));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "3.14");
}

TEST_F(FunctionRegistryTest, ToStringFunction_Boolean) {
	const ScalarFunction* func = registry->lookupScalarFunction("tostring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(true));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "true");
}

TEST_F(FunctionRegistryTest, ToStringFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("tostring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "null");
}

TEST_F(FunctionRegistryTest, ToStringFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("tostring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args; // Empty
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// String Function Tests - Upper/Lower
// ============================================================================

TEST_F(FunctionRegistryTest, UpperFunction_MixedCase) {
	const ScalarFunction* func = registry->lookupScalarFunction("upper");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("Hello")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "HELLO");
}

TEST_F(FunctionRegistryTest, UpperFunction_AlreadyUpper) {
	const ScalarFunction* func = registry->lookupScalarFunction("upper");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("WORLD")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "WORLD");
}

TEST_F(FunctionRegistryTest, UpperFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("upper");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LowerFunction_MixedCase) {
	const ScalarFunction* func = registry->lookupScalarFunction("lower");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("Hello")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
}

TEST_F(FunctionRegistryTest, LowerFunction_AlreadyLower) {
	const ScalarFunction* func = registry->lookupScalarFunction("lower");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("world")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "world");
}

TEST_F(FunctionRegistryTest, LowerFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("lower");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// String Function Tests - Substring
// ============================================================================

TEST_F(FunctionRegistryTest, SubstringFunction_StartOnly) {
	const ScalarFunction* func = registry->lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(int64_t(1)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "ello");
}

TEST_F(FunctionRegistryTest, SubstringFunction_StartAndLength) {
	const ScalarFunction* func = registry->lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(int64_t(1)));
	args.push_back(PropertyValue(int64_t(2)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "el");
}

TEST_F(FunctionRegistryTest, SubstringFunction_NegativeStart) {
	const ScalarFunction* func = registry->lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(int64_t(-2))); // From end
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "lo");
}

TEST_F(FunctionRegistryTest, SubstringFunction_StartBeyondLength) {
	const ScalarFunction* func = registry->lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(int64_t(100)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "");
}

TEST_F(FunctionRegistryTest, SubstringFunction_NullArgument) {
	const ScalarFunction* func = registry->lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue()); // NULL start
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// String Function Tests - Trim
// ============================================================================

TEST_F(FunctionRegistryTest, TrimFunction_LeadingSpaces) {
	const ScalarFunction* func = registry->lookupScalarFunction("trim");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("  hello")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
}

TEST_F(FunctionRegistryTest, TrimFunction_TrailingSpaces) {
	const ScalarFunction* func = registry->lookupScalarFunction("trim");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello  ")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
}

TEST_F(FunctionRegistryTest, TrimFunction_BothSides) {
	const ScalarFunction* func = registry->lookupScalarFunction("trim");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("  hello  ")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
}

TEST_F(FunctionRegistryTest, TrimFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("trim");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// String Function Tests - Length
// ============================================================================

TEST_F(FunctionRegistryTest, LengthFunction_String) {
	const ScalarFunction* func = registry->lookupScalarFunction("length");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 5);
}

TEST_F(FunctionRegistryTest, LengthFunction_EmptyString) {
	const ScalarFunction* func = registry->lookupScalarFunction("length");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(FunctionRegistryTest, LengthFunction_List) {
	const ScalarFunction* func = registry->lookupScalarFunction("length");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list = {PropertyValue(1.0), PropertyValue(2.0), PropertyValue(3.0)};
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 3);
}

TEST_F(FunctionRegistryTest, LengthFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("length");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// String Function Tests - Replace
// ============================================================================

TEST_F(FunctionRegistryTest, ReplaceFunction_Basic) {
	const ScalarFunction* func = registry->lookupScalarFunction("replace");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello world")));
	args.push_back(PropertyValue(std::string("world")));
	args.push_back(PropertyValue(std::string("there")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello there");
}

TEST_F(FunctionRegistryTest, ReplaceFunction_MultipleOccurrences) {
	const ScalarFunction* func = registry->lookupScalarFunction("replace");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello hello hello")));
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string("hi")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hi hi hi");
}

TEST_F(FunctionRegistryTest, ReplaceFunction_EmptySearchString) {
	const ScalarFunction* func = registry->lookupScalarFunction("replace");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string(""))); // Empty search
	args.push_back(PropertyValue(std::string("world")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
}

TEST_F(FunctionRegistryTest, ReplaceFunction_NullArgument) {
	const ScalarFunction* func = registry->lookupScalarFunction("replace");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string("world")));
	args.push_back(PropertyValue()); // NULL replacement
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// String Function Tests - StartsWith/EndsWith/Contains
// ============================================================================

TEST_F(FunctionRegistryTest, StartsWithFunction_Match) {
	const ScalarFunction* func = registry->lookupScalarFunction("startswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string("he")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(FunctionRegistryTest, StartsWithFunction_NoMatch) {
	const ScalarFunction* func = registry->lookupScalarFunction("startswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string("lo")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_FALSE(std::get<bool>(result.getVariant()));
}

TEST_F(FunctionRegistryTest, EndsWithFunction_Match) {
	const ScalarFunction* func = registry->lookupScalarFunction("endswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string("lo")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(FunctionRegistryTest, EndsWithFunction_NoMatch) {
	const ScalarFunction* func = registry->lookupScalarFunction("endswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string("he")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_FALSE(std::get<bool>(result.getVariant()));
}

TEST_F(FunctionRegistryTest, ContainsFunction_Match) {
	const ScalarFunction* func = registry->lookupScalarFunction("contains");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello world")));
	args.push_back(PropertyValue(std::string("lo wo")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(FunctionRegistryTest, ContainsFunction_NoMatch) {
	const ScalarFunction* func = registry->lookupScalarFunction("contains");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string("xyz")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_FALSE(std::get<bool>(result.getVariant()));
}

// ============================================================================
// Math Function Tests - Abs
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

// ============================================================================
// Math Function Tests - Ceil
// ============================================================================

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

// ============================================================================
// Math Function Tests - Floor
// ============================================================================

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

// ============================================================================
// Math Function Tests - Round
// ============================================================================

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

// ============================================================================
// Math Function Tests - Sqrt
// ============================================================================

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

// ============================================================================
// Math Function Tests - Sign
// ============================================================================

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

// ============================================================================
// Utility Function Tests - Coalesce
// ============================================================================

TEST_F(FunctionRegistryTest, CoalesceFunction_FirstNonNull) {
	const ScalarFunction* func = registry->lookupScalarFunction("coalesce");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue(int64_t(42)));
	args.push_back(PropertyValue(int64_t(99)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 42);
}

TEST_F(FunctionRegistryTest, CoalesceFunction_SecondNonNull) {
	const ScalarFunction* func = registry->lookupScalarFunction("coalesce");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue(int64_t(99)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 99);
}

TEST_F(FunctionRegistryTest, CoalesceFunction_AllNull) {
	const ScalarFunction* func = registry->lookupScalarFunction("coalesce");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, CoalesceFunction_FirstNonNullImmediate) {
	const ScalarFunction* func = registry->lookupScalarFunction("coalesce");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("first")));
	args.push_back(PropertyValue(std::string("second")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "first");
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

TEST_F(FunctionRegistryTest, CoalesceFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("coalesce");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args; // Empty
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Utility Function Tests - Size
// ============================================================================

TEST_F(FunctionRegistryTest, SizeFunction_List) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list = {PropertyValue(1.0), PropertyValue(2.0), PropertyValue(3.0)};
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 3);
}

TEST_F(FunctionRegistryTest, SizeFunction_String) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 5);
}

TEST_F(FunctionRegistryTest, SizeFunction_EmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list; // Empty
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(FunctionRegistryTest, SizeFunction_EmptyString) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(FunctionRegistryTest, SizeFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// ScalarFunction Validation Tests
// ============================================================================

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

// ============================================================================
// List Function Tests - Range
// ============================================================================

TEST_F(FunctionRegistryTest, RangeFunction_Basic) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(5)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 6UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 4);
	EXPECT_EQ(std::get<int64_t>(list[5].getVariant()), 5);
}

TEST_F(FunctionRegistryTest, RangeFunction_WithPositiveStep) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(10)));
	args.push_back(PropertyValue(int64_t(2)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 6UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 4);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 6);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 8);
	EXPECT_EQ(std::get<int64_t>(list[5].getVariant()), 10);
}

TEST_F(FunctionRegistryTest, RangeFunction_WithNegativeStep) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(5)));
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(-1)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 6UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 5);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 4);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(list[5].getVariant()), 0);
}

TEST_F(FunctionRegistryTest, RangeFunction_EmptyPositiveStep) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(5)));
	args.push_back(PropertyValue(int64_t(0))); // start >= end with positive default step
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 0UL);
}

TEST_F(FunctionRegistryTest, RangeFunction_EmptyNegativeStep) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(5)));
	args.push_back(PropertyValue(int64_t(-1))); // start < end with negative step
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 0UL);
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

TEST_F(FunctionRegistryTest, RangeFunction_NullArgument) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue()); // NULL end
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RangeFunction_ZeroStep) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(5)));
	args.push_back(PropertyValue(int64_t(0))); // step = 0

	EXPECT_THROW((void)func->evaluate(args, *context_), std::runtime_error);
}

TEST_F(FunctionRegistryTest, RangeFunction_NegativeStartPositiveEnd) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(-3)));
	args.push_back(PropertyValue(int64_t(2)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 6UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), -3);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), -2);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), -1);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(list[5].getVariant()), 2);
}

TEST_F(FunctionRegistryTest, RangeFunction_LargeStep) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(100)));
	args.push_back(PropertyValue(int64_t(25)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 5UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 25);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 50);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 75);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 100);
}

TEST_F(FunctionRegistryTest, RangeFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RangeFunction_TooManyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("range");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	args.push_back(PropertyValue(int64_t(5)));
	args.push_back(PropertyValue(int64_t(1)));
	args.push_back(PropertyValue(int64_t(1)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Type Conversion Function Tests - toInteger
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

// ============================================================================
// Type Conversion Function Tests - toFloat
// ============================================================================

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

// ============================================================================
// Type Conversion Function Tests - toBoolean
// ============================================================================

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

// ============================================================================
// Additional String Function Tests - left
// ============================================================================

TEST_F(FunctionRegistryTest, LeftFunction_Basic) {
	const ScalarFunction* func = registry->lookupScalarFunction("left");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(int64_t(3)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hel");
}

TEST_F(FunctionRegistryTest, LeftFunction_LengthExceedsString) {
	const ScalarFunction* func = registry->lookupScalarFunction("left");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hi")));
	args.push_back(PropertyValue(int64_t(10)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hi");
}

TEST_F(FunctionRegistryTest, LeftFunction_NegativeLength) {
	const ScalarFunction* func = registry->lookupScalarFunction("left");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(int64_t(-1)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LeftFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("left");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	args.push_back(PropertyValue(int64_t(3)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Additional String Function Tests - right
// ============================================================================

TEST_F(FunctionRegistryTest, RightFunction_Basic) {
	const ScalarFunction* func = registry->lookupScalarFunction("right");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(int64_t(3)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "llo");
}

TEST_F(FunctionRegistryTest, RightFunction_LengthExceedsString) {
	const ScalarFunction* func = registry->lookupScalarFunction("right");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hi")));
	args.push_back(PropertyValue(int64_t(10)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hi");
}

TEST_F(FunctionRegistryTest, RightFunction_NegativeLength) {
	const ScalarFunction* func = registry->lookupScalarFunction("right");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(int64_t(-1)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RightFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("right");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	args.push_back(PropertyValue(int64_t(3)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Additional String Function Tests - lTrim
// ============================================================================

TEST_F(FunctionRegistryTest, LTrimFunction_LeadingSpaces) {
	const ScalarFunction* func = registry->lookupScalarFunction("ltrim");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("  hello  ")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello  ");
}

TEST_F(FunctionRegistryTest, LTrimFunction_NoSpaces) {
	const ScalarFunction* func = registry->lookupScalarFunction("ltrim");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
}

TEST_F(FunctionRegistryTest, LTrimFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("ltrim");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LTrimFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("ltrim");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Additional String Function Tests - rTrim
// ============================================================================

TEST_F(FunctionRegistryTest, RTrimFunction_TrailingSpaces) {
	const ScalarFunction* func = registry->lookupScalarFunction("rtrim");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("  hello  ")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "  hello");
}

TEST_F(FunctionRegistryTest, RTrimFunction_NoSpaces) {
	const ScalarFunction* func = registry->lookupScalarFunction("rtrim");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
}

TEST_F(FunctionRegistryTest, RTrimFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("rtrim");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RTrimFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("rtrim");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Additional String Function Tests - split
// ============================================================================

TEST_F(FunctionRegistryTest, SplitFunction_Basic) {
	const ScalarFunction* func = registry->lookupScalarFunction("split");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("a,b,c")));
	args.push_back(PropertyValue(std::string(",")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	ASSERT_EQ(list.size(), 3UL);
	EXPECT_EQ(std::get<std::string>(list[0].getVariant()), "a");
	EXPECT_EQ(std::get<std::string>(list[1].getVariant()), "b");
	EXPECT_EQ(std::get<std::string>(list[2].getVariant()), "c");
}

TEST_F(FunctionRegistryTest, SplitFunction_EmptyDelimiter) {
	const ScalarFunction* func = registry->lookupScalarFunction("split");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("abc")));
	args.push_back(PropertyValue(std::string("")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	ASSERT_EQ(list.size(), 3UL);
	EXPECT_EQ(std::get<std::string>(list[0].getVariant()), "a");
	EXPECT_EQ(std::get<std::string>(list[1].getVariant()), "b");
	EXPECT_EQ(std::get<std::string>(list[2].getVariant()), "c");
}

TEST_F(FunctionRegistryTest, SplitFunction_NoMatch) {
	const ScalarFunction* func = registry->lookupScalarFunction("split");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string(",")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	ASSERT_EQ(list.size(), 1UL);
	EXPECT_EQ(std::get<std::string>(list[0].getVariant()), "hello");
}

TEST_F(FunctionRegistryTest, SplitFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("split");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	args.push_back(PropertyValue(std::string(",")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Additional String Function Tests - reverse
// ============================================================================

TEST_F(FunctionRegistryTest, ReverseFunction_String) {
	const ScalarFunction* func = registry->lookupScalarFunction("reverse");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "olleh");
}

TEST_F(FunctionRegistryTest, ReverseFunction_List) {
	const ScalarFunction* func = registry->lookupScalarFunction("reverse");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& rlist = std::get<std::vector<PropertyValue>>(result.getVariant());
	ASSERT_EQ(rlist.size(), 3UL);
	EXPECT_EQ(std::get<int64_t>(rlist[0].getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(rlist[1].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(rlist[2].getVariant()), 1);
}

TEST_F(FunctionRegistryTest, ReverseFunction_NonStringNonList) {
	const ScalarFunction* func = registry->lookupScalarFunction("reverse");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(123)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "321");
}

TEST_F(FunctionRegistryTest, ReverseFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("reverse");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ReverseFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("reverse");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// List Function Tests - head
// ============================================================================

TEST_F(FunctionRegistryTest, HeadFunction_NonEmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("head");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list = {PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))};
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 10);
}

TEST_F(FunctionRegistryTest, HeadFunction_EmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("head");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list;
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, HeadFunction_NonList) {
	const ScalarFunction* func = registry->lookupScalarFunction("head");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, HeadFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("head");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// List Function Tests - tail
// ============================================================================

TEST_F(FunctionRegistryTest, TailFunction_NonEmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("tail");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list = {PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))};
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& rlist = std::get<std::vector<PropertyValue>>(result.getVariant());
	ASSERT_EQ(rlist.size(), 2UL);
	EXPECT_EQ(std::get<int64_t>(rlist[0].getVariant()), 20);
	EXPECT_EQ(std::get<int64_t>(rlist[1].getVariant()), 30);
}

TEST_F(FunctionRegistryTest, TailFunction_EmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("tail");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list;
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::LIST);
	const auto& rlist = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(rlist.size(), 0UL);
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

TEST_F(FunctionRegistryTest, TailFunction_NonList) {
	const ScalarFunction* func = registry->lookupScalarFunction("tail");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, TailFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("tail");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// List Function Tests - last
// ============================================================================

TEST_F(FunctionRegistryTest, LastFunction_NonEmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("last");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list = {PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))};
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 30);
}

TEST_F(FunctionRegistryTest, LastFunction_EmptyList) {
	const ScalarFunction* func = registry->lookupScalarFunction("last");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> list;
	args.push_back(PropertyValue(list));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LastFunction_NonList) {
	const ScalarFunction* func = registry->lookupScalarFunction("last");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LastFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("last");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Utility Function Tests - timestamp
// ============================================================================

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

// ============================================================================
// Utility Function Tests - randomUUID
// ============================================================================

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

// ============================================================================
// ============================================================================
// Substring Edge Cases
// ============================================================================

TEST_F(FunctionRegistryTest, SubstringFunction_NullLengthArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(int64_t(1)));
	args.push_back(PropertyValue()); // NULL length
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, SubstringFunction_NegativeLength) {
	const ScalarFunction* func = registry->lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(int64_t(1)));
	args.push_back(PropertyValue(int64_t(-1)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
}

// ============================================================================
// Abs Edge Cases
// ============================================================================

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

// ============================================================================
// Length Edge Cases
// ============================================================================

TEST_F(FunctionRegistryTest, LengthFunction_NonStringNonList) {
	const ScalarFunction* func = registry->lookupScalarFunction("length");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(FunctionRegistryTest, LengthFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("length");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Size Edge Cases
// ============================================================================

TEST_F(FunctionRegistryTest, SizeFunction_NonStringNonList) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, SizeFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// StartsWith/EndsWith/Contains Edge Cases
// ============================================================================

TEST_F(FunctionRegistryTest, StartsWithFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("startswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	args.push_back(PropertyValue(std::string("he")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, EndsWithFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("endswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	args.push_back(PropertyValue(std::string("lo")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ContainsFunction_Null) {
	const ScalarFunction* func = registry->lookupScalarFunction("contains");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue());
	args.push_back(PropertyValue(std::string("he")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Upper/Lower Edge Cases
// ============================================================================

TEST_F(FunctionRegistryTest, UpperFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("upper");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LowerFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("lower");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, TrimFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("trim");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, HeadFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("head");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, TailFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("tail");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LastFunction_EmptyArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("last");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch Coverage: substring with fewer than 2 args
// ============================================================================

TEST_F(FunctionRegistryTest, SubstringFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	// Only 1 arg: hits args.size() < 2 true branch
	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch Coverage: startsWith with fewer than 2 args
// ============================================================================

TEST_F(FunctionRegistryTest, StartsWithFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("startswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch Coverage: endsWith with fewer than 2 args
// ============================================================================

TEST_F(FunctionRegistryTest, EndsWithFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("endswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch Coverage: contains with fewer than 2 args
// ============================================================================

TEST_F(FunctionRegistryTest, ContainsFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("contains");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch Coverage: replace with fewer than 3 args, null args[0], null args[1]
// ============================================================================

TEST_F(FunctionRegistryTest, ReplaceFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("replace");
	ASSERT_NE(func, nullptr);

	// Only 2 args: hits args.size() < 3 true branch
	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string("world")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ReplaceFunction_NullFirstArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("replace");
	ASSERT_NE(func, nullptr);

	// Null first arg: hits isNull(args[0]) true branch
	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue(std::string("world")));
	args.push_back(PropertyValue(std::string("there")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ReplaceFunction_NullSecondArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("replace");
	ASSERT_NE(func, nullptr);

	// Null second arg: hits isNull(args[1]) true branch
	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue(std::string("there")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch Coverage: ceil/floor/round/sqrt/sign with empty args
// ============================================================================

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

// ============================================================================
// Branch Coverage: left/right with too few args and null second arg
// ============================================================================

TEST_F(FunctionRegistryTest, LeftFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("left");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, LeftFunction_NullSecondArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("left");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue()); // NULL length
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RightFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("right");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RightFunction_NullSecondArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("right");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue()); // NULL length
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch Coverage: lTrim/rTrim with empty string (loop boundary)
// ============================================================================

TEST_F(FunctionRegistryTest, LTrimFunction_EmptyString) {
	const ScalarFunction* func = registry->lookupScalarFunction("ltrim");
	ASSERT_NE(func, nullptr);

	// Empty string: start < str.length() is immediately false
	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "");
}

TEST_F(FunctionRegistryTest, RTrimFunction_EmptyString) {
	const ScalarFunction* func = registry->lookupScalarFunction("rtrim");
	ASSERT_NE(func, nullptr);

	// Empty string: end > 0 is immediately false
	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "");
}

// ============================================================================
// Branch Coverage: split with too few args and null second arg
// ============================================================================

TEST_F(FunctionRegistryTest, SplitFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("split");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, SplitFunction_NullSecondArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("split");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue()); // NULL delimiter
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}
