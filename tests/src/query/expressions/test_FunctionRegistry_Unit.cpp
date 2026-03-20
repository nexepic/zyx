/**
 * @file test_FunctionRegistry_Unit.cpp
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
	EXPECT_EQ(list.size(), 5UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 4);
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
	EXPECT_EQ(list.size(), 5UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 4);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 6);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 8);
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
	EXPECT_EQ(list.size(), 5UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 5);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 4);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 1);
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
	EXPECT_EQ(list.size(), 1UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 0);
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

	EXPECT_THROW(func->evaluate(args, *context_), std::runtime_error);
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
	EXPECT_EQ(list.size(), 5UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), -3);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), -2);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), -1);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(list[4].getVariant()), 1);
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
	EXPECT_EQ(list.size(), 4UL);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 0);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 25);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 50);
	EXPECT_EQ(std::get<int64_t>(list[3].getVariant()), 75);
}
