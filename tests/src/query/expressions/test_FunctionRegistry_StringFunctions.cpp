/**
 * @file test_FunctionRegistry_StringFunctions.cpp
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
// String Function Tests
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

TEST_F(FunctionRegistryTest, SubstringFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	// Only 1 arg: hits args.size() < 2 true branch
	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, StartsWithFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("startswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, EndsWithFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("endswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, ContainsFunction_TooFewArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("contains");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

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

TEST_F(FunctionRegistryTest, ToUpperFunction_NullAndValid) {
	const ScalarFunction* func = registry->lookupScalarFunction("toUpper");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> nullArgs{PropertyValue()};
	EXPECT_EQ(func->evaluate(nullArgs, *context_).getType(), PropertyType::NULL_TYPE);
	std::vector<PropertyValue> validArgs{PropertyValue(std::string("hello"))};
	EXPECT_EQ(std::get<std::string>(func->evaluate(validArgs, *context_).getVariant()), "HELLO");
}

TEST_F(FunctionRegistryTest, ToLowerFunction_NullAndValid) {
	const ScalarFunction* func = registry->lookupScalarFunction("toLower");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> nullArgs{PropertyValue()};
	EXPECT_EQ(func->evaluate(nullArgs, *context_).getType(), PropertyType::NULL_TYPE);
	std::vector<PropertyValue> validArgs{PropertyValue(std::string("HELLO"))};
	EXPECT_EQ(std::get<std::string>(func->evaluate(validArgs, *context_).getVariant()), "hello");
}

TEST_F(FunctionRegistryTest, StartsWithFunction_PrefixLongerThanText) {
	const ScalarFunction* func = registry->lookupScalarFunction("startsWith");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue(std::string("hi")), PropertyValue(std::string("hello"))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(FunctionRegistryTest, EndsWithFunction_SuffixLongerThanText) {
	const ScalarFunction* func = registry->lookupScalarFunction("endsWith");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue(std::string("hi")), PropertyValue(std::string("hello"))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(FunctionRegistryTest, SubstringFunction_NegativeLength_Branch) {
	const ScalarFunction* func = registry->lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue(std::string("hello")), PropertyValue(static_cast<int64_t>(0)), PropertyValue(static_cast<int64_t>(-2))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
}

TEST_F(FunctionRegistryTest, LengthFunction_ListArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("length");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> list{PropertyValue(static_cast<int64_t>(1)), PropertyValue(static_cast<int64_t>(2)), PropertyValue(static_cast<int64_t>(3))};
	std::vector<PropertyValue> args{PropertyValue(list)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 3);
}

TEST_F(FunctionRegistryTest, LengthFunction_DoubleArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("length");
	std::vector<PropertyValue> args{PropertyValue(42.0)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(FunctionRegistryTest, ReverseFunction_ListArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("reverse");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> list{PropertyValue(static_cast<int64_t>(1)), PropertyValue(static_cast<int64_t>(2)), PropertyValue(static_cast<int64_t>(3))};
	std::vector<PropertyValue> args{PropertyValue(list)};
	auto result = func->evaluate(args, *context_);
	auto& reversed = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(std::get<int64_t>(reversed[0].getVariant()), 3);
}

TEST_F(FunctionRegistryTest, ReverseFunction_IntegerArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("reverse");
	std::vector<PropertyValue> args{PropertyValue(static_cast<int64_t>(123))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
}

TEST_F(FunctionRegistryTest, ReplaceFunction_EmptySearch) {
	const ScalarFunction* func = registry->lookupScalarFunction("replace");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue(std::string("hello")), PropertyValue(std::string("")), PropertyValue(std::string("x"))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
}

TEST_F(FunctionRegistryTest, SplitFunction_EmptyDelimiterStr) {
	const ScalarFunction* func = registry->lookupScalarFunction("split");
	std::vector<PropertyValue> args{PropertyValue(std::string("abc")), PropertyValue(std::string(""))};
	auto result = func->evaluate(args, *context_);
	auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
	EXPECT_EQ(list.size(), 3u);
}

TEST_F(FunctionRegistryTest, LeftFunction_NegativeLen) {
	const ScalarFunction* func = registry->lookupScalarFunction("left");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue(std::string("hello")), PropertyValue(static_cast<int64_t>(-1))};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RightFunction_NegativeLen) {
	const ScalarFunction* func = registry->lookupScalarFunction("right");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue(std::string("hello")), PropertyValue(static_cast<int64_t>(-1))};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, RightFunction_LengthGreaterThanString) {
	const ScalarFunction* func = registry->lookupScalarFunction("right");
	std::vector<PropertyValue> args{PropertyValue(std::string("hi")), PropertyValue(static_cast<int64_t>(100))};
	EXPECT_EQ(std::get<std::string>(func->evaluate(args, *context_).getVariant()), "hi");
}

TEST_F(FunctionRegistryTest, SubstringFunction_NegStart) {
	const ScalarFunction* func = registry->lookupScalarFunction("substring");
	std::vector<PropertyValue> args{PropertyValue(std::string("hello")), PropertyValue(static_cast<int64_t>(-2))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	// -2 → max(0, 5+(-2)) = 3, so substr from index 3 → "lo"
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "lo");
}

TEST_F(FunctionRegistryTest, SubstringFunction_StartPastEnd) {
	const ScalarFunction* func = registry->lookupScalarFunction("substring");
	std::vector<PropertyValue> args{PropertyValue(std::string("hello")), PropertyValue(static_cast<int64_t>(100))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "");
}
