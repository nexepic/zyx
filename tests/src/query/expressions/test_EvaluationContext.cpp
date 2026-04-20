/**
 * @file test_EvaluationContext.cpp
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
// EvaluationContext Tests
// ============================================================================

TEST_F(ExpressionsTest, EvaluationContext_ResolveVariable_Integer) {
	auto result = context_->resolveVariable("x");
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result->getVariant()), 42);
}

TEST_F(ExpressionsTest, EvaluationContext_ResolveProperty_NodeProperty) {
	auto result = context_->resolveProperty("n", "age");
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result->getVariant()), 30);
}

TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_Null) {
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue()));
}

TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_True) {
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(true)));
}

TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_NonZeroInteger) {
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(int64_t(42))));
}

TEST_F(ExpressionsTest, EvaluationContext_ToInteger_Null) {
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue()), 0);
}

TEST_F(ExpressionsTest, EvaluationContext_ToInteger_BooleanTrue) {
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(true)), 1);
}

TEST_F(ExpressionsTest, EvaluationContext_ToDouble_Null) {
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue()), 0.0);
}

TEST_F(ExpressionsTest, EvaluationContext_ToDouble_Integer) {
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(42))), 42.0);
}

TEST_F(ExpressionsTest, EvaluationContext_IsNull_Null) {
	EXPECT_TRUE(EvaluationContext::isNull(PropertyValue()));
}

TEST_F(ExpressionsTest, EvaluationContext_IsNull_Integer) {
	EXPECT_FALSE(EvaluationContext::isNull(PropertyValue(42)));
}

// ============================================================================
// Batch 3: EvaluationContext Error Path Tests
// ============================================================================

TEST_F(ExpressionsTest, EvaluationContext_ToInteger_InvalidString) {
	EXPECT_THROW((void)EvaluationContext::toInteger(PropertyValue(std::string("not a number"))), TypeMismatchException);
}

TEST_F(ExpressionsTest, EvaluationContext_ToDouble_InvalidString) {
	EXPECT_THROW((void)EvaluationContext::toDouble(PropertyValue(std::string("not a number"))), TypeMismatchException);
}

TEST_F(ExpressionsTest, EvaluationContext_ToInteger_ListThrows) {
	std::vector<PropertyValue> list = {PropertyValue(1.0), PropertyValue(2.0)};
	EXPECT_THROW((void)EvaluationContext::toInteger(PropertyValue(list)), TypeMismatchException);
}

TEST_F(ExpressionsTest, EvaluationContext_ToDouble_ListThrows) {
	std::vector<PropertyValue> list = {PropertyValue(1.0), PropertyValue(2.0)};
	EXPECT_THROW((void)EvaluationContext::toDouble(PropertyValue(list)), TypeMismatchException);
}

TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_StringTrue) {
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::string("true"))));
}

TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_StringFalse) {
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(std::string("false"))));
}

// ============================================================================
// Batch 5: Coverage Optimization - Additional Branch Tests
// ============================================================================

// EvaluationContext - toBoolean additional branches
TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_ZeroInteger) {
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(int64_t(0))));
}

TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_ZeroDouble) {
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(0.0)));
}

TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_NonZeroDouble) {
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(3.14)));
}

TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_NonEmptyString) {
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::string("hello"))));
}

TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_EmptyString) {
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(std::string(""))));
}

TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_EmptyList) {
	std::vector<PropertyValue> emptyList;
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(emptyList)));
}

TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_NonEmptyList) {
	std::vector<PropertyValue> list = {PropertyValue(1.0), PropertyValue(2.0), PropertyValue(3.0)};
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(list)));
}

// EvaluationContext - toInteger additional branches
TEST_F(ExpressionsTest, EvaluationContext_ToInteger_Integer) {
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(int64_t(42))), 42);
}

TEST_F(ExpressionsTest, EvaluationContext_ToInteger_NegativeInteger) {
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(int64_t(-42))), -42);
}

TEST_F(ExpressionsTest, EvaluationContext_ToInteger_BooleanFalse) {
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(false)), 0);
}

TEST_F(ExpressionsTest, EvaluationContext_ToInteger_Double_Truncate) {
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(3.9)), 3);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(-3.9)), -3);
}

TEST_F(ExpressionsTest, EvaluationContext_ToInteger_Double_Integer) {
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(5.0)), 5);
}

TEST_F(ExpressionsTest, EvaluationContext_ToInteger_ValidString) {
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(std::string("42"))), 42);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(std::string("-123"))), -123);
}

// EvaluationContext - toDouble additional branches
TEST_F(ExpressionsTest, EvaluationContext_ToDouble_BooleanFalse) {
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(false)), 0.0);
}

TEST_F(ExpressionsTest, EvaluationContext_ToDouble_BooleanTrue) {
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(true)), 1.0);
}

TEST_F(ExpressionsTest, EvaluationContext_ToDouble_Double) {
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(3.14)), 3.14);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(-2.5)), -2.5);
}

TEST_F(ExpressionsTest, EvaluationContext_ToDouble_ValidString) {
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(std::string("3.14"))), 3.14);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(std::string("-2.5"))), -2.5);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(std::string("42"))), 42.0);
}

// EvaluationContext - resolveVariable with Edge
TEST_F(ExpressionsTest, EvaluationContext_ResolveVariable_Edge) {
	// Edge 'r' is already set up in SetUp()
	auto result = context_->resolveVariable("r");
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result->getVariant()), 2); // Edge ID
}

TEST_F(ExpressionsTest, EvaluationContext_ResolveVariable_Node) {
	// Node 'n' is already set up in SetUp()
	auto result = context_->resolveVariable("n");
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result->getVariant()), 1); // Node ID
}

// EvaluationContext - resolveProperty with Edge
TEST_F(ExpressionsTest, EvaluationContext_ResolveProperty_EdgeProperty) {
	// Edge 'r' has property 'weight' = 1.5
	auto result = context_->resolveProperty("r", "weight");
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result->getVariant()), 1.5);
}

TEST_F(ExpressionsTest, EvaluationContext_ResolveProperty_EdgeMissingProperty) {
	auto result = context_->resolveProperty("r", "missing");
	EXPECT_FALSE(result.has_value());
}

TEST_F(ExpressionsTest, EvaluationContext_ResolveProperty_NodeMissingProperty) {
	// Node 'n' doesn't have property 'missing'
	auto result = context_->resolveProperty("n", "missing");
	EXPECT_FALSE(result.has_value());
}

TEST_F(ExpressionsTest, EvaluationContext_ResolveProperty_UnknownVariable) {
	auto result = context_->resolveProperty("unknown", "prop");
	EXPECT_FALSE(result.has_value());
}

// ============================================================================
// Coverage Optimization - Targeted Tests for Missing Branches
// ============================================================================

// Target: EvaluationContext.cpp - improve branch coverage from 80.29% to 85%+
TEST_F(ExpressionsTest, EvaluationContext_ToInteger_DoubleNegative) {
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(-3.7)), -3);
}

TEST_F(ExpressionsTest, EvaluationContext_ToInteger_StringNegative) {
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(std::string("-42"))), -42);
}

// ============================================================================
// Final Coverage Optimization - Target 85%+ for All Metrics
// ============================================================================

// Target: EvaluationContext.cpp - branch coverage 80.29% → 85%+
// Missing: toInteger with double, toDouble with boolean
TEST_F(ExpressionsTest, EvaluationContext_BranchCoverage_IntegerConversion) {
	// toInteger with double - different fractional parts
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(3.0)), 3);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(-3.0)), -3);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(3.1)), 3);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(-3.9)), -3);

	// toInteger with boolean
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(true)), 1);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(false)), 0);
}

TEST_F(ExpressionsTest, EvaluationContext_BranchCoverage_DoubleConversion) {
	// toDouble with boolean
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(true)), 1.0);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(false)), 0.0);

	// toDouble with integer - positive, negative, zero
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(42))), 42.0);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(-42))), -42.0);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(0))), 0.0);
}

TEST_F(ExpressionsTest, EvaluationContext_BranchCoverage_StringToNumber) {
	// toInteger with string representations
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(std::string("0"))), 0);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(std::string("-0"))), 0);

	// toDouble with string representations
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(std::string("3.14"))), 3.14);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(std::string("-2.5"))), -2.5);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(std::string("0.0"))), 0.0);
}

// ============================================================================
// Final Targeted Tests - Push All Metrics to 85%+
// ============================================================================

// Target: EvaluationContext.cpp - branch coverage 80.29% → 85%+
// Missing: String to boolean conversion paths, LIST to number throws
TEST_F(ExpressionsTest, EvaluationContext_FinalBranches_ToBoolean) {
	// String "true" and "false"
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::string("true"))));
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(std::string("false"))));

	// Non-empty string that's not "false"
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::string("hello"))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::string(" "))));

	// Empty list → false
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(std::vector<PropertyValue>())));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})));
}

TEST_F(ExpressionsTest, EvaluationContext_FinalBranches_ToIntegerThrows) {
	// toInteger with invalid string throws
	EXPECT_THROW((void)EvaluationContext::toInteger(PropertyValue(std::string("not a number"))), TypeMismatchException);
	EXPECT_THROW((void)EvaluationContext::toInteger(PropertyValue(std::string("abc123"))), TypeMismatchException);

	// toInteger with LIST throws
	EXPECT_THROW((void)EvaluationContext::toInteger(PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})), TypeMismatchException);
}

TEST_F(ExpressionsTest, EvaluationContext_FinalBranches_ToDoubleThrows) {
	// toDouble with invalid string throws
	EXPECT_THROW((void)EvaluationContext::toDouble(PropertyValue(std::string("not a number"))), TypeMismatchException);
	EXPECT_THROW((void)EvaluationContext::toDouble(PropertyValue(std::string("abc"))), TypeMismatchException);

	// toDouble with LIST throws
	EXPECT_THROW((void)EvaluationContext::toDouble(PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})), TypeMismatchException);
}

TEST_F(ExpressionsTest, EvaluationContext_FinalBranches_ValidStringConversions) {
	// Valid integer strings
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(std::string("123"))), 123);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(std::string("-456"))), -456);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(std::string("0"))), 0);

	// Valid double strings
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(std::string("123.45"))), 123.45);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(std::string("-99.9"))), -99.9);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(std::string("1e10"))), 1e10);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(std::string("-1.5e-3"))), -1.5e-3);
}

// ============================================================================
// Additional Tests for Remaining Missing Coverage
// ============================================================================

// Target: EvaluationContext.cpp - branch coverage still at 80.29%
// Need to hit remaining type conversion branches
TEST_F(ExpressionsTest, EvaluationContext_AdditionalBranches) {
	// toBoolean with zero values
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(int64_t(0))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(int64_t(1))));
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(0.0)));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(0.1)));

	// toBoolean with various string values
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(std::string(""))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::string("a"))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::string("FALSE"))));
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(std::string("false"))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::string("TRUE"))));

	// toInteger with double values
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(3.14)), 3);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(-3.99)), -3);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(0.0)), 0);

	// toInteger with NULL
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue()), 0);

	// toDouble with NULL
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue()), 0.0);

	// toDouble with int64_t values
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(0))), 0.0);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(-999))), -999.0);
}

// Target: EvaluationContext.cpp - hit remaining type conversion branches
TEST_F(ExpressionsTest, EvaluationContext_UltimateBranches) {
	// toInteger with all numeric types
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(std::monostate{})), 0);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(true)), 1);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(false)), 0);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(int64_t(42))), 42);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(-3.14)), -3);

	// toDouble with all numeric types
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(std::monostate{})), 0.0);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(true)), 1.0);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(false)), 0.0);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(42))), 42.0);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(-3.14)), -3.14);

	// toBoolean with all types
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(std::monostate{})));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(true)));
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(false)));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(int64_t(1))));
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(int64_t(0))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(0.5)));
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(0.0)));
}

// Additional type conversion tests
TEST_F(ExpressionsTest, EvaluationContext_FinalConversions) {
	// More toBoolean cases
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(int64_t(-1))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(-0.1)));

	// More toInteger cases
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(0.9)), 0);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(-0.9)), 0);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(999999.9)), 999999);

	// More toDouble cases
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(-999999))), -999999.0);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(0))), 0.0);
}

// Test toBoolean with more edge cases
TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_FinalEdgeCases) {
	// Test with positive and negative integers
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(int64_t(100))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(int64_t(-100))));

	// Test with positive and negative doubles
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(0.0001)));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(-0.0001)));

	// Test with strings that are not "true" or "false"
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::string("TRUE"))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::string("FALSE"))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::string("random"))));
}

// Test final type conversions
TEST_F(ExpressionsTest, EvaluationContext_Conversions_AbsoluteFinal) {
	// toDouble with various integers
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(1))), 1.0);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(-1))), -1.0);
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(1000000))), 1000000.0);

	// toInteger with various doubles
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(0.0)), 0);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(1.0)), 1);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(-1.0)), -1);
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(100.999)), 100);
}


TEST_F(ExpressionsTest, EvaluationContext_GetRecord) {
	// Test EvaluationContext::getRecord()
	Record record;
	EvaluationContext context(record);

	EXPECT_EQ(&context.getRecord(), &record);
}

TEST_F(ExpressionsTest, EvaluationContext_SetAndClearVariable) {
	// Test setVariable and clearVariable methods
	Record record;
	EvaluationContext context(record);

	// Set a temporary variable
	context.setVariable("tempVar", PropertyValue(int64_t(123)));

	// Verify it's set
	auto resolved = context.resolveVariable("tempVar");
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(std::get<int64_t>(resolved->getVariant()), 123);

	// Clear the variable
	context.clearVariable("tempVar");

	// Verify it's cleared
	auto resolvedAfterClear = context.resolveVariable("tempVar");
	EXPECT_FALSE(resolvedAfterClear.has_value());
}

TEST_F(ExpressionsTest, EvaluationContext_SetVariableOverrides) {
	// Test that setVariable can override existing variables
	Record record;
	EvaluationContext context(record);

	// Set initial value
	context.setVariable("x", PropertyValue(int64_t(100)));

	// Override with new value
	context.setVariable("x", PropertyValue(int64_t(200)));

	auto resolved = context.resolveVariable("x");
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(std::get<int64_t>(resolved->getVariant()), 200);
}

TEST_F(ExpressionsTest, EvaluationContext_ToInteger) {
	// Test toInteger with various types

	// NULL -> 0
	PropertyValue nullValue;
	EXPECT_EQ(EvaluationContext::toInteger(nullValue), int64_t(0));

	// boolean -> 0 or 1
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(bool(true))), int64_t(1));
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(bool(false))), int64_t(0));

	// double -> truncated
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(double(3.9))), int64_t(3));
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(double(-2.7))), int64_t(-2));

	// string -> parsed
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(std::string("42"))), int64_t(42));
	EXPECT_EQ(EvaluationContext::toInteger(PropertyValue(std::string("-10"))), int64_t(-10));

	// Invalid string should throw
	EXPECT_THROW(
		(void)EvaluationContext::toInteger(PropertyValue(std::string("abc"))),
		TypeMismatchException
	);
}

TEST_F(ExpressionsTest, EvaluationContext_ToDouble) {
	// Test toDouble with various types

	// NULL -> 0.0
	PropertyValue nullValue;
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(nullValue), double(0.0));

	// boolean -> 0.0 or 1.0
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(bool(true))), double(1.0));
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(bool(false))), double(0.0));

	// integer -> cast to double
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(42))), double(42.0));
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(int64_t(-3))), double(-3.0));

	// string -> parsed
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(std::string("3.14"))), double(3.14));
	EXPECT_DOUBLE_EQ(EvaluationContext::toDouble(PropertyValue(std::string("-2.5"))), double(-2.5));

	// Invalid string should throw
	EXPECT_THROW(
		(void)EvaluationContext::toDouble(PropertyValue(std::string("abc"))),
		TypeMismatchException
	);
}

TEST_F(ExpressionsTest, EvaluationContext_ToStringPropertyValue) {
	// Test toString with various types

	// NULL -> "null"
	PropertyValue nullValue;
	EXPECT_EQ(EvaluationContext::toString(nullValue), std::string("null"));

	// boolean -> "true" or "false"
	EXPECT_EQ(EvaluationContext::toString(PropertyValue(bool(true))), std::string("true"));
	EXPECT_EQ(EvaluationContext::toString(PropertyValue(bool(false))), std::string("false"));

	// integer -> decimal string
	EXPECT_EQ(EvaluationContext::toString(PropertyValue(int64_t(42))), std::string("42"));
	EXPECT_EQ(EvaluationContext::toString(PropertyValue(int64_t(-10))), std::string("-10"));

	// double -> decimal string
	EXPECT_EQ(EvaluationContext::toString(PropertyValue(double(3.14))), std::string("3.14"));

	// string -> value as-is
	EXPECT_EQ(EvaluationContext::toString(PropertyValue(std::string("test"))), std::string("test"));

	// list -> "[...]"
	std::vector<PropertyValue> list = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2))};
	std::string listStr = EvaluationContext::toString(PropertyValue(list));
	EXPECT_TRUE(listStr.find("[") != std::string::npos);
}

TEST_F(ExpressionsTest, EvaluationContext_IsNull) {
	// Test isNull static method

	// NULL value
	PropertyValue nullValue;
	EXPECT_TRUE(EvaluationContext::isNull(nullValue));

	// Non-null values
	EXPECT_FALSE(EvaluationContext::isNull(PropertyValue(bool(true))));
	EXPECT_FALSE(EvaluationContext::isNull(PropertyValue(int64_t(42))));
	EXPECT_FALSE(EvaluationContext::isNull(PropertyValue(double(3.14))));
	EXPECT_FALSE(EvaluationContext::isNull(PropertyValue(std::string("test"))));

	std::vector<PropertyValue> list = {PropertyValue(int64_t(1))};
	EXPECT_FALSE(EvaluationContext::isNull(PropertyValue(list)));
}

TEST_F(ExpressionsTest, UndefinedVariableException_GetVariableName) {
	// Test UndefinedVariableException::getVariableName()
	UndefinedVariableException ex("testVar");

	EXPECT_EQ(ex.getVariableName(), "testVar");
	EXPECT_TRUE(ex.what() != nullptr);
}

TEST_F(ExpressionsTest, TypeMismatchException_GetTypes) {
	// Test TypeMismatchException getters
	TypeMismatchException ex(
		"Type mismatch",
		PropertyType::INTEGER,
		PropertyType::STRING
	);

	EXPECT_EQ(ex.getExpectedType(), PropertyType::INTEGER);
	EXPECT_EQ(ex.getActualType(), PropertyType::STRING);
	EXPECT_TRUE(ex.what() != nullptr);
}

TEST_F(ExpressionsTest, DivisionByZeroException_What) {
	// Test DivisionByZeroException
	DivisionByZeroException ex;

	EXPECT_TRUE(ex.what() != nullptr);
}
