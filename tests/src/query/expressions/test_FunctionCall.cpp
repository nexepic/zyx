/**
 * @file test_FunctionCall.cpp
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

#include "query/expressions/ExpressionsTestFixture.hpp"

// ============================================================================
// ExpressionEvaluator Tests - Function Calls
// ============================================================================

TEST_F(ExpressionsTest, FunctionCall_ToString) {
	auto arg = std::make_unique<LiteralExpression>(int64_t(42));
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::move(arg));
	FunctionCallExpression expr("tostring", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "42");
}

TEST_F(ExpressionsTest, FunctionCall_Upper) {
	auto arg = std::make_unique<LiteralExpression>(std::string("hello"));
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::move(arg));
	FunctionCallExpression expr("upper", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "HELLO");
}

TEST_F(ExpressionsTest, FunctionCall_Abs) {
	auto arg = std::make_unique<LiteralExpression>(int64_t(-42));
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::move(arg));
	FunctionCallExpression expr("abs", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 42);
}

TEST_F(ExpressionsTest, FunctionCall_Coalesce_FirstNonNull) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>()); // NULL
	args.push_back(std::make_unique<LiteralExpression>(int64_t(42)));
	args.push_back(std::make_unique<LiteralExpression>(int64_t(99)));
	FunctionCallExpression expr("coalesce", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 42);
}

TEST_F(ExpressionsTest, FunctionCall_UnknownFunction) {
	auto arg = std::make_unique<LiteralExpression>(int64_t(42));
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::move(arg));
	FunctionCallExpression expr("unknownFunction", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsTest, FunctionCall_InvalidArgumentCount) {
	// tostring expects 1 argument, provide 0
	std::vector<std::unique_ptr<Expression>> args; // Empty
	FunctionCallExpression expr("tostring", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsTest, SizeFunction_Integer) {
	const ScalarFunction* func = FunctionRegistry::getInstance().lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42))); // Invalid type
	PropertyValue result = func->evaluate(args, *context_);

	// For unsupported types, returns NULL or 0
	EXPECT_TRUE(result.getType() == PropertyType::NULL_TYPE || result.getType() == PropertyType::INTEGER);
}

TEST_F(ExpressionsTest, LengthFunction_Integer) {
	const ScalarFunction* func = FunctionRegistry::getInstance().lookupScalarFunction("length");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42))); // Invalid type
	PropertyValue result = func->evaluate(args, *context_);

	// For other types, return 0
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(ExpressionsTest, SubstringFunction_EmptyString) {
	const ScalarFunction* func = FunctionRegistry::getInstance().lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("")));
	args.push_back(PropertyValue(int64_t(0)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "");
}

TEST_F(ExpressionsTest, SubstringFunction_NegativeLength) {
	const ScalarFunction* func = FunctionRegistry::getInstance().lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(int64_t(1)));
	args.push_back(PropertyValue(int64_t(-2))); // Negative length
	PropertyValue result = func->evaluate(args, *context_);

	// Should return from start with negative length adjustment
	EXPECT_EQ(result.getType(), PropertyType::STRING);
}

TEST_F(ExpressionsTest, AbsFunction_Zero) {
	const ScalarFunction* func = FunctionRegistry::getInstance().lookupScalarFunction("abs");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(0)));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(ExpressionsTest, SignFunction_Zero) {
	const ScalarFunction* func = FunctionRegistry::getInstance().lookupScalarFunction("sign");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(0.0));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(ExpressionsTest, StartsWithFunction_EmptyPrefix) {
	const ScalarFunction* func = FunctionRegistry::getInstance().lookupScalarFunction("startswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string("")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionsTest, EndsWithFunction_EmptySuffix) {
	const ScalarFunction* func = FunctionRegistry::getInstance().lookupScalarFunction("endswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string("")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionsTest, ContainsFunction_EmptySubstring) {
	const ScalarFunction* func = FunctionRegistry::getInstance().lookupScalarFunction("contains");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string("")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionsTest, ContainsFunction_EmptyString) {
	const ScalarFunction* func = FunctionRegistry::getInstance().lookupScalarFunction("contains");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("")));
	args.push_back(PropertyValue(std::string("")));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionsTest, FunctionCall_ToString_Double) {
	auto arg = std::make_unique<LiteralExpression>(3.14159);
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::move(arg));
	FunctionCallExpression expr("tostring", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::STRING);
}

TEST_F(ExpressionsTest, FunctionCall_Lower_MixedCase) {
	auto arg = std::make_unique<LiteralExpression>(std::string("HELLO"));
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::move(arg));
	FunctionCallExpression expr("lower", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "hello");
}

TEST_F(ExpressionsTest, FunctionCall_Length_Null) {
	auto arg = std::make_unique<LiteralExpression>(); // NULL
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::move(arg));
	FunctionCallExpression expr("length", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, FunctionCall_Substring_ThreeArgs) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(std::string("hello")));
	args.push_back(std::make_unique<LiteralExpression>(int64_t(1)));
	args.push_back(std::make_unique<LiteralExpression>(int64_t(2)));
	FunctionCallExpression expr("substring", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "el");
}

TEST_F(ExpressionsTest, FunctionCall_Coalesce_AllNull) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>()); // NULL
	args.push_back(std::make_unique<LiteralExpression>()); // NULL
	args.push_back(std::make_unique<LiteralExpression>()); // NULL
	FunctionCallExpression expr("coalesce", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, FunctionCall_Sqrt_Zero) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(0.0));
	FunctionCallExpression expr("sqrt", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), 0.0);
}

TEST_F(ExpressionsTest, FunctionCall_Sqrt_VerySmallNumber) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(0.0001));
	FunctionCallExpression expr("sqrt", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_NEAR(std::get<double>(evaluator.getResult().getVariant()), 0.01, 0.001);
}

TEST_F(ExpressionsTest, FunctionCall_Round_HalfUp) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(2.5));
	FunctionCallExpression expr("round", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), 3.0);
}

TEST_F(ExpressionsTest, FunctionCall_Round_HalfDown) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(1.5));
	FunctionCallExpression expr("round", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), 2.0);
}

TEST_F(ExpressionsTest, FunctionCall_Ceil_NegativeFraction) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(-3.7));
	FunctionCallExpression expr("ceil", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), -3.0);
}

TEST_F(ExpressionsTest, FunctionCall_Floor_NegativeFraction) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(-3.7));
	FunctionCallExpression expr("floor", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), -4.0);
}

TEST_F(ExpressionsTest, FunctionCall_Substring_StartAtEnd) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(std::string("hello")));
	args.push_back(std::make_unique<LiteralExpression>(int64_t(5)));
	FunctionCallExpression expr("substring", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "");
}

TEST_F(ExpressionsTest, FunctionCall_Replace_NoMatch) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(std::string("hello world")));
	args.push_back(std::make_unique<LiteralExpression>(std::string("foo")));
	args.push_back(std::make_unique<LiteralExpression>(std::string("bar")));
	FunctionCallExpression expr("replace", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "hello world");
}

TEST_F(ExpressionsTest, FunctionCall_StartsWith_SameString) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(std::string("hello")));
	args.push_back(std::make_unique<LiteralExpression>(std::string("hello")));
	FunctionCallExpression expr("startswith", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, FunctionCall_EndsWith_SameString) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(std::string("hello")));
	args.push_back(std::make_unique<LiteralExpression>(std::string("hello")));
	FunctionCallExpression expr("endswith", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, FunctionCall_Contains_SameString) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(std::string("hello")));
	args.push_back(std::make_unique<LiteralExpression>(std::string("hello")));
	FunctionCallExpression expr("contains", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, FunctionCall_Size_Null) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>());
	FunctionCallExpression expr("size", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, FunctionCall_Length_EmptyList) {
	// Test length with empty list
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("length");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	std::vector<PropertyValue> emptyList;
	args.push_back(PropertyValue(emptyList));
	PropertyValue result = func->evaluate(args, *context_);

	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(ExpressionsTest, FunctionRegistry_Singleton) {
	auto& instance1 = FunctionRegistry::getInstance();
	auto& instance2 = FunctionRegistry::getInstance();
	EXPECT_EQ(&instance1, &instance2);
}

TEST_F(ExpressionsTest, FunctionRegistry_FunctionNames) {
	auto names = FunctionRegistry::getInstance().getRegisteredFunctionNames();
	EXPECT_GT(names.size(), 0UL);

	// Check some expected functions exist
	bool hasAbs = false, hasUpper = false, hasCoalesce = false;
	for (const auto& name : names) {
		if (name == "abs") hasAbs = true;
		if (name == "upper") hasUpper = true;
		if (name == "coalesce") hasCoalesce = true;
	}
	EXPECT_TRUE(hasAbs);
	EXPECT_TRUE(hasUpper);
	EXPECT_TRUE(hasCoalesce);
}

TEST_F(ExpressionsTest, FunctionRegistry_CaseInsensitiveLookup) {
	auto& registry = FunctionRegistry::getInstance();
	auto func1 = registry.lookupScalarFunction("UPPER");
	auto func2 = registry.lookupScalarFunction("upper");
	auto func3 = registry.lookupScalarFunction("UpPeR");

	EXPECT_NE(func1, nullptr);
	EXPECT_NE(func2, nullptr);
	EXPECT_NE(func3, nullptr);
	EXPECT_EQ(func1, func2);
	EXPECT_EQ(func2, func3);
}

TEST_F(ExpressionsTest, FunctionRegistry_HasFunction) {
	auto& registry = FunctionRegistry::getInstance();
	EXPECT_TRUE(registry.hasFunction("abs"));
	EXPECT_FALSE(registry.hasFunction("nonexistent"));
}

TEST_F(ExpressionsTest, FunctionRegistry_UnknownFunction) {
	auto& registry = FunctionRegistry::getInstance();
	auto func = registry.lookupScalarFunction("nonexistent");
	EXPECT_EQ(func, nullptr);
}

TEST_F(ExpressionsTest, FunctionRegistry_MissingRegions) {
	auto& registry = FunctionRegistry::getInstance();

	// Sqrt with very small number - tests double precision region
	{
		const ScalarFunction* func = registry.lookupScalarFunction("sqrt");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(0.0001));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_NEAR(std::get<double>(result.getVariant()), 0.01, 0.001);
	}

	// Substring with negative length - tests length adjustment region
	{
		const ScalarFunction* func = registry.lookupScalarFunction("substring");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(1)));
		args.push_back(PropertyValue(int64_t(-1))); // Negative length
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(result.getType(), PropertyType::STRING);
	}

	// Replace with empty search string - tests empty search region
	{
		const ScalarFunction* func = registry.lookupScalarFunction("replace");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(std::string(""))); // Empty search
		args.push_back(PropertyValue(std::string("world")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}

	// Sign with zero - tests zero branch
	{
		const ScalarFunction* func = registry.lookupScalarFunction("sign");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(0.0));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_RegionCoverage_StringEdgeCases) {
	auto& registry = FunctionRegistry::getInstance();

	// Trim with various whitespace patterns
	{
		const ScalarFunction* func = registry.lookupScalarFunction("trim");
		ASSERT_NE(func, nullptr);

		// Only tabs
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("\t\thello\t\t")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}

	// Substring with start at end
	{
		const ScalarFunction* func = registry.lookupScalarFunction("substring");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(5))); // Start at end
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "");
	}

	// Length with empty string
	{
		const ScalarFunction* func = registry.lookupScalarFunction("length");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_RegionCoverage_MathEdgeCases) {
	auto& registry = FunctionRegistry::getInstance();

	// Ceil/Floor/Round with exact integers
	{
		const ScalarFunction* ceilFunc = registry.lookupScalarFunction("ceil");
		const ScalarFunction* floorFunc = registry.lookupScalarFunction("floor");
		const ScalarFunction* roundFunc = registry.lookupScalarFunction("round");
		ASSERT_NE(ceilFunc, nullptr);
		ASSERT_NE(floorFunc, nullptr);
		ASSERT_NE(roundFunc, nullptr);

		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(int64_t(5)));
		EXPECT_DOUBLE_EQ(std::get<double>(ceilFunc->evaluate(args, *context_).getVariant()), 5.0);
		EXPECT_DOUBLE_EQ(std::get<double>(floorFunc->evaluate(args, *context_).getVariant()), 5.0);
		EXPECT_DOUBLE_EQ(std::get<double>(roundFunc->evaluate(args, *context_).getVariant()), 5.0);
	}

	// Ceil/Floor with negative numbers
	{
		const ScalarFunction* ceilFunc = registry.lookupScalarFunction("ceil");
		const ScalarFunction* floorFunc = registry.lookupScalarFunction("floor");
		ASSERT_NE(ceilFunc, nullptr);
		ASSERT_NE(floorFunc, nullptr);

		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(-3.7));
		EXPECT_DOUBLE_EQ(std::get<double>(ceilFunc->evaluate(args, *context_).getVariant()), -3.0);
		EXPECT_DOUBLE_EQ(std::get<double>(floorFunc->evaluate(args, *context_).getVariant()), -4.0);
	}

	// Round at .5 boundary
	{
		const ScalarFunction* roundFunc = registry.lookupScalarFunction("round");
		ASSERT_NE(roundFunc, nullptr);

		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(2.5));
		EXPECT_DOUBLE_EQ(std::get<double>(roundFunc->evaluate(args, *context_).getVariant()), 3.0);

		args.clear();
		args.push_back(PropertyValue(1.5));
		EXPECT_DOUBLE_EQ(std::get<double>(roundFunc->evaluate(args, *context_).getVariant()), 2.0);
	}

	// Abs with double
	{
		const ScalarFunction* func = registry.lookupScalarFunction("abs");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(-3.14));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), 3.14);
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_FinalRegions_Substring) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	// Negative start position
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(-2)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "lo");
	}

	// Negative length (from end)
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(1)));
		args.push_back(PropertyValue(int64_t(-2)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "el");
	}

	// Start beyond length (returns empty)
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hi")));
		args.push_back(PropertyValue(int64_t(10)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "");
	}

	// Length clamping
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(1)));
		args.push_back(PropertyValue(int64_t(100))); // Longer than remaining
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "ello");
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_FinalRegions_Trim) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("trim");
	ASSERT_NE(func, nullptr);

	// Only leading whitespace
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("   hello")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}

	// Only trailing whitespace
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello   ")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}

	// Mixed whitespace (tabs, newlines, spaces)
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("\n \t hello \t \n")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}

	// All whitespace
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("   \t\n  ")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "");
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_FinalRegions_Sqrt) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("sqrt");
	ASSERT_NE(func, nullptr);

	// Negative number returns NULL
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(-1.0));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(EvaluationContext::isNull(result));
	}

	// Zero
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(0.0));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 0.0);
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_FinalRegions_Sign) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("sign");
	ASSERT_NE(func, nullptr);

	// All three branches: positive, negative, zero
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(42.0));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 1);
	}
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(-42.0));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), -1);
	}
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(0.0));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 0);
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_FinalRegions_Abs) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("abs");
	ASSERT_NE(func, nullptr);

	// Integer path
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(int64_t(-42)));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 42);
	}

	// Double path
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(3.14));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), 3.14);
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_FinalRegions_Length) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("length");
	ASSERT_NE(func, nullptr);

	// String path
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 5);
	}

	// LIST path
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(2.0), PropertyValue(3.0)}));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 3);
	}

	// Other types return 0
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(42));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 0);
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_FinalRegions_Size) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	// String path
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("test")));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 4);
	}

	// LIST path
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(2.0)}));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 2);
	}

	// Empty string
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("")));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 0);
	}

	// Empty list
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::vector<PropertyValue>()));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 0);
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_FinalRegions_Replace) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("replace");
	ASSERT_NE(func, nullptr);

	// Multiple occurrences
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello hello hello")));
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(std::string("hi")));
		EXPECT_EQ(std::get<std::string>(func->evaluate(args, *context_).getVariant()), "hi hi hi");
	}

	// Search string not found
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(std::string("xyz")));
		args.push_back(PropertyValue(std::string("abc")));
		EXPECT_EQ(std::get<std::string>(func->evaluate(args, *context_).getVariant()), "hello");
	}

	// Empty replace string (deletion)
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello world")));
		args.push_back(PropertyValue(std::string(" ")));
		args.push_back(PropertyValue(std::string("")));
		EXPECT_EQ(std::get<std::string>(func->evaluate(args, *context_).getVariant()), "helloworld");
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_AdditionalRegions_StringFunctions) {
	auto& registry = FunctionRegistry::getInstance();

	// Upper with special characters
	{
		const ScalarFunction* func = registry.lookupScalarFunction("upper");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello!@#")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "HELLO!@#");
	}

	// Lower with already lowercase
	{
		const ScalarFunction* func = registry.lookupScalarFunction("lower");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}

	// Lower with mixed case
	{
		const ScalarFunction* func = registry.lookupScalarFunction("lower");
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("HeLLo")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}

	// StartsWith with empty prefix
	{
		const ScalarFunction* func = registry.lookupScalarFunction("startswith");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(std::string("")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(std::get<bool>(result.getVariant()));
	}

	// EndsWith with empty suffix
	{
		const ScalarFunction* func = registry.lookupScalarFunction("endswith");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(std::string("")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(std::get<bool>(result.getVariant()));
	}

	// EndsWith with longer suffix
	{
		const ScalarFunction* func = registry.lookupScalarFunction("endswith");
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(std::string("lo world")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_FALSE(std::get<bool>(result.getVariant()));
	}

	// Contains with empty substring
	{
		const ScalarFunction* func = registry.lookupScalarFunction("contains");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(std::string("")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(std::get<bool>(result.getVariant()));
	}

	// Contains with matching substring
	{
		const ScalarFunction* func = registry.lookupScalarFunction("contains");
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello world")));
		args.push_back(PropertyValue(std::string("world")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(std::get<bool>(result.getVariant()));
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_AdditionalRegions_MathFunctions) {
	auto& registry = FunctionRegistry::getInstance();

	// Round with various edge cases
	{
		const ScalarFunction* func = registry.lookupScalarFunction("round");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;

		args.push_back(PropertyValue(0.5));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), 1.0);

		args.clear();
		args.push_back(PropertyValue(-0.5));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), -1.0);

		args.clear();
		args.push_back(PropertyValue(0.4));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), 0.0);
	}

	// Ceil with positive and negative
	{
		const ScalarFunction* func = registry.lookupScalarFunction("ceil");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;

		args.push_back(PropertyValue(0.1));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), 1.0);

		args.clear();
		args.push_back(PropertyValue(-0.1));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), 0.0);
	}

	// Floor with positive and negative
	{
		const ScalarFunction* func = registry.lookupScalarFunction("floor");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;

		args.push_back(PropertyValue(0.9));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), 0.0);

		args.clear();
		args.push_back(PropertyValue(-0.9));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), -1.0);
	}

	// Sqrt with small positive number
	{
		const ScalarFunction* func = registry.lookupScalarFunction("sqrt");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(0.25));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), 0.5);
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_AdditionalRegions_UtilityFunctions) {
	auto& registry = FunctionRegistry::getInstance();

	// Coalesce with all NULL
	{
		const ScalarFunction* func = registry.lookupScalarFunction("coalesce");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue());
		args.push_back(PropertyValue());
		args.push_back(PropertyValue());
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(EvaluationContext::isNull(result));
	}

	// Coalesce with second non-NULL
	{
		const ScalarFunction* func = registry.lookupScalarFunction("coalesce");
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue());
		args.push_back(PropertyValue(int64_t(42)));
		args.push_back(PropertyValue(int64_t(99)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<int64_t>(result.getVariant()), 42);
	}

	// Size with non-empty string and list
	{
		const ScalarFunction* func = registry.lookupScalarFunction("size");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;

		args.push_back(PropertyValue(std::string("test")));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 4);

		args.clear();
		args.push_back(PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(2.0), PropertyValue(3.0)}));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 3);
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_EdgeCases_EmptyArgs) {
	auto& registry = FunctionRegistry::getInstance();

	// ToString with empty args
	{
		const ScalarFunction* func = registry.lookupScalarFunction("tostring");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;  // Empty
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(EvaluationContext::isNull(result));
	}

	// Upper with empty args
	{
		const ScalarFunction* func = registry.lookupScalarFunction("upper");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;  // Empty
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(EvaluationContext::isNull(result));
	}

	// Lower with empty args
	{
		const ScalarFunction* func = registry.lookupScalarFunction("lower");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;  // Empty
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(EvaluationContext::isNull(result));
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_Substring_NoLength) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	// Only 2 arguments, no length
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(2)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "llo");
	}

	// Start at 0 with no length
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(0)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_Substring_EdgeCases) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	// Negative start that goes beyond beginning
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(-100)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}

	// Negative length that results in 0
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(2)));
		args.push_back(PropertyValue(int64_t(-100)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "");
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_Trim_EdgeCases) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("trim");
	ASSERT_NE(func, nullptr);

	// Empty string
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "");
	}

	// Single space
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string(" ")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "");
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_NULLArguments) {
	auto& registry = FunctionRegistry::getInstance();

	// Upper with NULL
	{
		const ScalarFunction* func = registry.lookupScalarFunction("upper");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue());  // NULL
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(EvaluationContext::isNull(result));
	}

	// Lower with NULL
	{
		const ScalarFunction* func = registry.lookupScalarFunction("lower");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue());  // NULL
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(EvaluationContext::isNull(result));
	}

	// Length with NULL
	{
		const ScalarFunction* func = registry.lookupScalarFunction("length");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue());  // NULL
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(EvaluationContext::isNull(result));
	}

	// Trim with NULL
	{
		const ScalarFunction* func = registry.lookupScalarFunction("trim");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue());  // NULL
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(EvaluationContext::isNull(result));
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_UltimateRegions) {
	auto& registry = FunctionRegistry::getInstance();

	// Length with non-string, non-list types (returns 0)
	{
		const ScalarFunction* func = registry.lookupScalarFunction("length");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(int64_t(42)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
	}

	// Length with double
	{
		const ScalarFunction* func = registry.lookupScalarFunction("length");
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(3.14));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
	}

	// Abs with boolean (converted to double)
	{
		const ScalarFunction* func = registry.lookupScalarFunction("abs");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(true));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 1.0);
	}

	// Substring with start=0 and length
	{
		const ScalarFunction* func = registry.lookupScalarFunction("substring");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(0)));
		args.push_back(PropertyValue(int64_t(3)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hel");
	}

	// Substring where length equals remaining string
	{
		const ScalarFunction* func = registry.lookupScalarFunction("substring");
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(2)));
		args.push_back(PropertyValue(int64_t(3)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "llo");
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_Replace_EdgeCases) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("replace");
	ASSERT_NE(func, nullptr);

	// Replace with same string (no change)
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(std::string("hello")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}

	// Replace at beginning
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello world")));
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(std::string("hi")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hi world");
	}

	// Replace at end
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello world")));
		args.push_back(PropertyValue(std::string("world")));
		args.push_back(PropertyValue(std::string("there")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello there");
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_StringComparison_EdgeCases) {
	auto& registry = FunctionRegistry::getInstance();

	// startsWith with exact match
	{
		const ScalarFunction* func = registry.lookupScalarFunction("startswith");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(std::string("hello")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(std::get<bool>(result.getVariant()));
	}

	// startsWith with longer prefix
	{
		const ScalarFunction* func = registry.lookupScalarFunction("startswith");
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hi")));
		args.push_back(PropertyValue(std::string("hello")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_FALSE(std::get<bool>(result.getVariant()));
	}

	// endsWith with exact match
	{
		const ScalarFunction* func = registry.lookupScalarFunction("endswith");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(std::string("hello")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(std::get<bool>(result.getVariant()));
	}

	// endsWith with longer suffix
	{
		const ScalarFunction* func = registry.lookupScalarFunction("endswith");
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hi")));
		args.push_back(PropertyValue(std::string("hello")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_FALSE(std::get<bool>(result.getVariant()));
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_FinalEdgeCases) {
	auto& registry = FunctionRegistry::getInstance();

	// Substring with start at 0, no length
	{
		const ScalarFunction* func = registry.lookupScalarFunction("substring");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(0)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}

	// Substring with negative length exactly at boundary
	{
		const ScalarFunction* func = registry.lookupScalarFunction("substring");
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(1)));
		args.push_back(PropertyValue(int64_t(-3)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "e");
	}

	// Trim with newlines only
	{
		const ScalarFunction* func = registry.lookupScalarFunction("trim");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("\nhello\n")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}

	// Replace with overlapping patterns
	{
		const ScalarFunction* func = registry.lookupScalarFunction("replace");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("aaa")));
		args.push_back(PropertyValue(std::string("aa")));
		args.push_back(PropertyValue(std::string("b")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "ba");
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_Substring_MoreEdgeCases) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	// Substring start=1, length=1
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(1)));
		args.push_back(PropertyValue(int64_t(1)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "e");
	}

	// Substring start=2, length=2
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(2)));
		args.push_back(PropertyValue(int64_t(2)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "ll");
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_Abs_MoreEdgeCases) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("abs");
	ASSERT_NE(func, nullptr);

	// Abs with zero
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(int64_t(0)));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 0);
	}

	// Abs with very large integer
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(int64_t(-999999)));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 999999);
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_StringConversion_MoreEdgeCases) {
	auto& registry = FunctionRegistry::getInstance();

	// tostring with boolean
	{
		const ScalarFunction* func = registry.lookupScalarFunction("tostring");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(true));
		EXPECT_EQ(std::get<std::string>(func->evaluate(args, *context_).getVariant()), "true");
	}

	{
		const ScalarFunction* func = registry.lookupScalarFunction("tostring");
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(false));
		EXPECT_EQ(std::get<std::string>(func->evaluate(args, *context_).getVariant()), "false");
	}

	// tostring with double
	{
		const ScalarFunction* func = registry.lookupScalarFunction("tostring");
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(3.14159));
		std::string result = std::get<std::string>(func->evaluate(args, *context_).getVariant());
		EXPECT_TRUE(result.find("3.14") == 0);
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_Substring_NULLHandling) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	// NULL first argument
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue());  // NULL
		args.push_back(PropertyValue(int64_t(1)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(EvaluationContext::isNull(result));
	}

	// NULL second argument
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue());  // NULL
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(EvaluationContext::isNull(result));
	}

	// NULL third argument
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(1)));
		args.push_back(PropertyValue());  // NULL
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_TRUE(EvaluationContext::isNull(result));
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_Math_FinalEdgeCases) {
	auto& registry = FunctionRegistry::getInstance();

	// Ceil with very small positive
	{
		const ScalarFunction* func = registry.lookupScalarFunction("ceil");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(0.001));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), 1.0);
	}

	// Floor with very small negative
	{
		const ScalarFunction* func = registry.lookupScalarFunction("floor");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(-0.001));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), -1.0);
	}

	// Round with various values
	{
		const ScalarFunction* func = registry.lookupScalarFunction("round");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;

		args.push_back(PropertyValue(0.4));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), 0.0);

		args.clear();
		args.push_back(PropertyValue(-0.4));
		EXPECT_DOUBLE_EQ(std::get<double>(func->evaluate(args, *context_).getVariant()), 0.0);
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_Substring_AbsoluteFinal) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	// Substring with length exactly matching remaining
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(2)));
		args.push_back(PropertyValue(int64_t(3)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "llo");
	}

	// Substring where start + length equals string length
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		args.push_back(PropertyValue(int64_t(0)));
		args.push_back(PropertyValue(int64_t(5)));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_Trim_AbsoluteFinal) {
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("trim");
	ASSERT_NE(func, nullptr);

	// Trim with mixed whitespace at both ends
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("  \t\nhello\n\t  ")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}

	// Trim with no whitespace (no change)
	{
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("hello")));
		PropertyValue result = func->evaluate(args, *context_);
		EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
	}
}

TEST_F(ExpressionsTest, FunctionRegistry_String_AbsoluteFinal) {
	auto& registry = FunctionRegistry::getInstance();

	// Length with single character string
	{
		const ScalarFunction* func = registry.lookupScalarFunction("length");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("a")));
		EXPECT_EQ(std::get<int64_t>(func->evaluate(args, *context_).getVariant()), 1);
	}

	// Upper with single character
	{
		const ScalarFunction* func = registry.lookupScalarFunction("upper");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("a")));
		EXPECT_EQ(std::get<std::string>(func->evaluate(args, *context_).getVariant()), "A");
	}

	// Lower with single character
	{
		const ScalarFunction* func = registry.lookupScalarFunction("lower");
		ASSERT_NE(func, nullptr);
		std::vector<PropertyValue> args;
		args.push_back(PropertyValue(std::string("A")));
		EXPECT_EQ(std::get<std::string>(func->evaluate(args, *context_).getVariant()), "a");
	}
}



TEST_F(ExpressionsTest, StartsWith_NullFirstArg) {
	// Test StartsWith with NULL first argument (lines 242-244 in FunctionRegistry.cpp)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("startswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue(std::string("prefix")));

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, StartsWith_NullSecondArg) {
	// Test StartsWith with NULL second argument
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("startswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue()); // NULL

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, StartsWith_BothNull) {
	// Test StartsWith with both arguments NULL
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("startswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue()); // NULL

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, EndsWith_NullFirstArg) {
	// Test EndsWith with NULL first argument (lines 259-261 in FunctionRegistry.cpp)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("endswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue(std::string("suffix")));

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, EndsWith_NullSecondArg) {
	// Test EndsWith with NULL second argument
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("endswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue()); // NULL

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, EndsWith_BothNull) {
	// Test EndsWith with both arguments NULL
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("endswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue()); // NULL

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, Contains_NullFirstArg) {
	// Test Contains with NULL first argument (lines 276-278 in FunctionRegistry.cpp)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("contains");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue(std::string("substring")));

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, Contains_NullSecondArg) {
	// Test Contains with NULL second argument
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("contains");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue()); // NULL

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, Contains_BothNull) {
	// Test Contains with both arguments NULL
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("contains");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue()); // NULL
	args.push_back(PropertyValue()); // NULL

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, LengthFunction_BooleanType) {
	// Test LengthFunction with BOOLEAN type (hits "other types" branch at line 235)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("length");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(true));

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(ExpressionsTest, SizeFunction_IntegerType) {
	// Test SizeFunction with INTEGER type (hits "other types" branch at line 455)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, SizeFunction_DoubleType) {
	// Test SizeFunction with DOUBLE type (hits "other types" branch at line 455)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(3.14));

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, SizeFunction_BooleanType) {
	// Test SizeFunction with BOOLEAN type (hits "other types" branch at line 455)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(true));

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, AbsFunction_StringType) {
	// Test AbsFunction with STRING type (hits catch-all branch at line 340-342)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("abs");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("42"))); // String converts to double

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 42.0);
}

TEST_F(ExpressionsTest, FunctionCallExpression_CloneWithMultipleArgs) {
	// Test FunctionCallExpression clone with multiple arguments
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(int64_t(1)));
	args.push_back(std::make_unique<LiteralExpression>(int64_t(2)));
	args.push_back(std::make_unique<LiteralExpression>(int64_t(3)));
	FunctionCallExpression original("coalesce", std::move(args));

	auto cloned = original.clone();
	auto* clonedFunc = dynamic_cast<FunctionCallExpression*>(cloned.get());

	ASSERT_NE(clonedFunc, nullptr);
	EXPECT_EQ(clonedFunc->getFunctionName(), "coalesce");
	EXPECT_EQ(clonedFunc->getArgumentCount(), 3UL);

	std::string originalStr = original.toString();
	std::string clonedStr = clonedFunc->toString();
	EXPECT_EQ(originalStr, clonedStr);
	EXPECT_EQ(clonedStr, "coalesce(1, 2, 3)");
}

TEST_F(ExpressionsTest, FunctionCallExpression_ToString_NoArgs) {
	std::vector<std::unique_ptr<Expression>> args;
	FunctionCallExpression expr("now", std::move(args));
	std::string result = expr.toString();
	EXPECT_EQ(result, "now()");
}

TEST_F(ExpressionsTest, FunctionCallExpression_Clone_NoArgs) {
	std::vector<std::unique_ptr<Expression>> args;
	FunctionCallExpression expr("now", std::move(args));
	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	EXPECT_EQ(cloned->toString(), "now()");
}

TEST_F(ExpressionsTest, FunctionCallExpression_NonConstAccept) {
	MockExpressionVisitor visitor;
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(int64_t(1)));
	FunctionCallExpression expr("toUpper", std::move(args));
	expr.accept(visitor);
	EXPECT_EQ(visitor.visitCount, 1);
}
