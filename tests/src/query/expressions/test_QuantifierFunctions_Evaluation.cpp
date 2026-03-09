/**
 * @file test_QuantifierFunctions_Evaluation.cpp
 * @date 2025
 *
 * @copyright Copyright (c) 2025
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
#include <memory>
#include <vector>
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/execution/Record.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

/**
 * Test fixture for quantifier function evaluation
 */
class QuantifierFunctionsEvaluationTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Verify functions are registered
		const auto& registry = FunctionRegistry::getInstance();
		ASSERT_NE(registry.lookupScalarFunction("all"), nullptr);
		ASSERT_NE(registry.lookupScalarFunction("any"), nullptr);
		ASSERT_NE(registry.lookupScalarFunction("none"), nullptr);
		ASSERT_NE(registry.lookupScalarFunction("single"), nullptr);

		// Create a basic record for context
		record_ = Record();
		context_ = std::make_unique<EvaluationContext>(record_);
	}

	Record record_;
	std::unique_ptr<EvaluationContext> context_;
};

/**
 * Helper function to create a quantifier function expression
 */
static std::unique_ptr<QuantifierFunctionExpression> createQuantifierExpr(
	const std::string& funcName,
	const std::string& varName,
	std::vector<PropertyValue> listValues,
	int64_t whereValue) {

	// Create list expression - wrap vector in PropertyValue
	PropertyValue listValue(listValues);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// Create WHERE expression: var > whereValue
	auto varExpr = std::make_unique<VariableReferenceExpression>(varName);
	auto intExpr = std::make_unique<LiteralExpression>(whereValue);
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::move(varExpr),
		BinaryOperatorType::BOP_GREATER,
		std::move(intExpr)
	);

	return std::make_unique<QuantifierFunctionExpression>(
		funcName, varName, std::move(listExpr), std::move(whereExpr)
	);
}

/**
 * Test all() function with all true values
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_AllTrue) {
	auto quantExpr = createQuantifierExpr("all", "x",
		{PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

/**
 * Test all() function with one false value
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_OneFalse) {
	auto quantExpr = createQuantifierExpr("all", "x",
		{PropertyValue(int64_t(1)), PropertyValue(int64_t(-1)), PropertyValue(int64_t(3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

/**
 * Test all() function with empty list (vacuous truth)
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_EmptyList) {
	auto quantExpr = createQuantifierExpr("all", "x", std::vector<PropertyValue>{}, 0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // Vacuous truth
}

/**
 * Test any() function with at least one true value
 */
TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_OneTrue) {
	auto quantExpr = createQuantifierExpr("any", "x",
		{PropertyValue(int64_t(-1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

/**
 * Test any() function with all false values
 */
TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_AllFalse) {
	auto quantExpr = createQuantifierExpr("any", "x",
		{PropertyValue(int64_t(-1)), PropertyValue(int64_t(-2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

/**
 * Test any() function with empty list
 */
TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_EmptyList) {
	auto quantExpr = createQuantifierExpr("any", "x", std::vector<PropertyValue>{}, 0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));  // Empty list = false
}

/**
 * Test none() function with no matching values
 */
TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_NoMatches) {
	auto quantExpr = createQuantifierExpr("none", "x",
		{PropertyValue(int64_t(-1)), PropertyValue(int64_t(-2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // None match = true
}

/**
 * Test none() function with one matching value
 */
TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_OneMatch) {
	auto quantExpr = createQuantifierExpr("none", "x",
		{PropertyValue(int64_t(-1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));  // One match = false
}

/**
 * Test none() function with empty list
 */
TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_EmptyList) {
	auto quantExpr = createQuantifierExpr("none", "x", std::vector<PropertyValue>{}, 0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // Empty list = true
}

/**
 * Test single() function with exactly one match
 */
TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_OneMatch) {
	auto quantExpr = createQuantifierExpr("single", "x",
		{PropertyValue(int64_t(-1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

/**
 * Test single() function with zero matches
 */
TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_ZeroMatches) {
	auto quantExpr = createQuantifierExpr("single", "x",
		{PropertyValue(int64_t(-1)), PropertyValue(int64_t(-2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));  // Zero matches = false
}

/**
 * Test single() function with two matches
 */
TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_TwoMatches) {
	auto quantExpr = createQuantifierExpr("single", "x",
		{PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(-3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));  // Two matches = false
}

/**
 * Test single() function with empty list
 */
TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_EmptyList) {
	auto quantExpr = createQuantifierExpr("single", "x", std::vector<PropertyValue>{}, 0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));  // Empty list = false
}

/**
 * Test all() function with non-boolean WHERE results (integers)
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_NumericWhereResult) {
	auto quantExpr = createQuantifierExpr("all", "x",
		{PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))},
		15);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));  // Only 20 and 30 > 15
}

/**
 * Test all() function with non-boolean WHERE results (strings)
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_StringComparison) {
	// Create custom expression for string comparison - all elements are "banana"
	std::vector<PropertyValue> stringList = {
		PropertyValue(std::string("banana")),
		PropertyValue(std::string("banana")),
		PropertyValue(std::string("banana"))  // All are "banana"
	};
	PropertyValue listValue(stringList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");
	auto strExpr = std::make_unique<LiteralExpression>(std::string("banana"));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::move(varExpr),
		BinaryOperatorType::BOP_EQUAL,
		std::move(strExpr)
	);

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(whereExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // All match banana
}

/**
 * Test quantifier functions with NULL values in list
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_WithNulls) {
	auto quantExpr = createQuantifierExpr("all", "x",
		{PropertyValue(int64_t(1)), PropertyValue(), PropertyValue(int64_t(3))},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));  // NULL is not > 0
}

/**
 * Test any() function with all NULL values
 */
TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_AllNulls) {
	auto quantExpr = createQuantifierExpr("any", "x",
		{PropertyValue(), PropertyValue(), PropertyValue()},
		0);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));  // All NULLs are not > 0
}

/**
 * Test with boolean WHERE clause
 */
TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_BooleanWhere) {
	// Create list of boolean values
	std::vector<PropertyValue> boolList = {
		PropertyValue(false),
		PropertyValue(true),
		PropertyValue(false)
	};
	PropertyValue listValue(boolList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x (just check if x is true)
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"any", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // One true value exists
}

/**
 * Test with type coercion in WHERE clause
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_TypeCoercion) {
	// Test with integers that should be coerced to booleans
	std::vector<PropertyValue> intList = {
		PropertyValue(int64_t(1)),   // true
		PropertyValue(int64_t(2)),   // true
		PropertyValue(int64_t(3))    // true
	};
	PropertyValue listValue(intList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x (coerce to boolean)
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // All are truthy
}

/**
 * Test unknown function throws exception
 */
TEST_F(QuantifierFunctionsEvaluationTest, UnknownFunction_Throws) {
	std::vector<PropertyValue> list = {PropertyValue(int64_t(1))};
	PropertyValue listValue(list);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	auto varExpr = std::make_unique<VariableReferenceExpression>("x");
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::move(varExpr),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"unknown_func", "x", std::move(listExpr), std::move(whereExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(evaluator.visit(quantExpr.get()), ExpressionEvaluationException);
}

/**
 * Test all() short-circuits on first false
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_ShortCircuit) {
	// Create a list with values where the second element is false
	std::vector<PropertyValue> boolList = {
		PropertyValue(true),
		PropertyValue(false),  // Should short-circuit here
		PropertyValue(true)
	};
	PropertyValue listValue(boolList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));  // Short-circuited
}

/**
 * Test single() continues searching until it finds exactly one
 */
TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_FindsExactlyOne) {
	// Create a list where the second element is the only match
	std::vector<PropertyValue> boolList = {
		PropertyValue(false),
		PropertyValue(true),   // First match
		PropertyValue(true)    // Second match (should fail)
	};
	PropertyValue listValue(boolList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"single", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));  // Found two matches, not one
}

/**
 * Test all() with non-list argument throws exception
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_NonListArgument_Throws) {
	// Create a literal expression (not a list)
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(nonListExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(evaluator.visit(quantExpr.get()), ExpressionEvaluationException);
}

/**
 * Test all() with double type coercion
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_DoubleCoercion) {
	// Create list with double values that should be coerced to booleans
	std::vector<PropertyValue> doubleList = {
		PropertyValue(1.5),   // true (non-zero)
		PropertyValue(0.0),   // false (zero)
		PropertyValue(2.5)    // true (non-zero)
	};
	PropertyValue listValue(doubleList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x (coerce double to boolean)
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));  // 0.0 coerces to false
}

/**
 * Test any() with double type coercion
 */
TEST_F(QuantifierFunctionsEvaluationTest, AnyFunction_DoubleCoercion) {
	// Create list with double values
	std::vector<PropertyValue> doubleList = {
		PropertyValue(0.0),   // false
		PropertyValue(0.0),   // false
		PropertyValue(3.14)   // true
	};
	PropertyValue listValue(doubleList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x (coerce double to boolean)
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"any", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // 3.14 is truthy
}

/**
 * Test all() with string truthiness
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_StringTruthiness) {
	// Create list with string values (non-empty strings are truthy)
	std::vector<PropertyValue> stringList = {
		PropertyValue(std::string("hello")),
		PropertyValue(std::string("world")),
		PropertyValue(std::string("!"))
	};
	PropertyValue listValue(stringList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x (check if string is truthy)
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // All non-empty strings are truthy
}

/**
 * Test all() with empty string (falsy)
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_EmptyString_Falsy) {
	// Create list with one empty string
	std::vector<PropertyValue> stringList = {
		PropertyValue(std::string("hello")),
		PropertyValue(std::string("")),   // empty string is falsy
		PropertyValue(std::string("!"))
	};
	PropertyValue listValue(stringList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x (check if string is truthy)
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));  // Empty string is falsy
}

/**
 * Test all() with list truthiness
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_ListTruthiness) {
	// Create list of lists (non-empty lists are truthy)
	std::vector<PropertyValue> outerList = {
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1))}),
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(2))}),
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(3))})
	};
	PropertyValue listValue(outerList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x (check if list is truthy)
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // All non-empty lists are truthy
}

/**
 * Test all() with empty list in list (falsy)
 */
TEST_F(QuantifierFunctionsEvaluationTest, AllFunction_EmptyList_Falsy) {
	// Create list of lists with one empty list
	std::vector<PropertyValue> outerList = {
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1))}),
		PropertyValue(std::vector<PropertyValue>{}),   // empty list is falsy
		PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(3))})
	};
	PropertyValue listValue(outerList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x (check if list is truthy)
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"all", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));  // Empty list is falsy
}

/**
 * Test none() with double type coercion
 */
TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_DoubleCoercion) {
	// Create list with double values
	std::vector<PropertyValue> doubleList = {
		PropertyValue(0.0),   // false
		PropertyValue(0.0),   // false
		PropertyValue(0.0)    // false
	};
	PropertyValue listValue(doubleList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x (coerce double to boolean)
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"none", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // All are falsy, so none match
}

/**
 * Test none() with string truthiness
 */
TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_StringTruthiness) {
	// Create list with empty strings (falsy)
	std::vector<PropertyValue> stringList = {
		PropertyValue(std::string("")),   // falsy
		PropertyValue(std::string("")),   // falsy
		PropertyValue(std::string(""))    // falsy
	};
	PropertyValue listValue(stringList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x (check if string is truthy)
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"none", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // None are truthy
}

/**
 * Test single() with double type coercion
 */
TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_DoubleCoercion) {
	// Create list with double values
	std::vector<PropertyValue> doubleList = {
		PropertyValue(0.0),   // false
		PropertyValue(3.14),  // true
		PropertyValue(0.0)    // false
	};
	PropertyValue listValue(doubleList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x (coerce double to boolean)
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"single", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // Exactly one is truthy
}

/**
 * Test single() with string truthiness
 */
TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_StringTruthiness) {
	// Create list with strings
	std::vector<PropertyValue> stringList = {
		PropertyValue(std::string("")),      // falsy
		PropertyValue(std::string("hello")), // truthy
		PropertyValue(std::string(""))       // falsy
	};
	PropertyValue listValue(stringList);
	auto listExpr = std::make_unique<ListLiteralExpression>(listValue);

	// WHERE clause: x (check if string is truthy)
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"single", "x", std::move(listExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(quantExpr.get());

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));  // Exactly one is truthy
}

/**
 * Test none() with non-list argument throws exception
 */
TEST_F(QuantifierFunctionsEvaluationTest, NoneFunction_NonListArgument_Throws) {
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"none", "x", std::move(nonListExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(evaluator.visit(quantExpr.get()), ExpressionEvaluationException);
}

/**
 * Test single() with non-list argument throws exception
 */
TEST_F(QuantifierFunctionsEvaluationTest, SingleFunction_NonListArgument_Throws) {
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	auto quantExpr = std::make_unique<QuantifierFunctionExpression>(
		"single", "x", std::move(nonListExpr), std::move(varExpr)
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(evaluator.visit(quantExpr.get()), ExpressionEvaluationException);
}
