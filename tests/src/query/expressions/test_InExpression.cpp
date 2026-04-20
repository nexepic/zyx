/**
 * @file test_InExpression.cpp
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
// ExpressionEvaluator Tests - IN Expression
// ============================================================================

TEST_F(ExpressionsTest, InExpression_Match) {
	auto value = std::make_unique<LiteralExpression>(int64_t(2));
	std::vector<PropertyValue> listValues;
	listValues.push_back(PropertyValue(int64_t(1)));
	listValues.push_back(PropertyValue(int64_t(2)));
	listValues.push_back(PropertyValue(int64_t(3)));
	InExpression expr(std::move(value), std::move(listValues));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, InExpression_NoMatch) {
	auto value = std::make_unique<LiteralExpression>(int64_t(99));
	std::vector<PropertyValue> listValues;
	listValues.push_back(PropertyValue(int64_t(1)));
	listValues.push_back(PropertyValue(int64_t(2)));
	listValues.push_back(PropertyValue(int64_t(3)));
	InExpression expr(std::move(value), std::move(listValues));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, InExpression_NullValue) {
	auto value = std::make_unique<LiteralExpression>(); // NULL
	std::vector<PropertyValue> listValues;
	listValues.push_back(PropertyValue(int64_t(1)));
	listValues.push_back(PropertyValue(int64_t(2)));
	listValues.push_back(PropertyValue(int64_t(3)));
	InExpression expr(std::move(value), std::move(listValues));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, InExpression_EmptyList) {
	auto value = std::make_unique<LiteralExpression>(int64_t(2));
	std::vector<PropertyValue> listValues; // Empty
	InExpression expr(std::move(value), std::move(listValues));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

// ============================================================================
// Batch 4: Expression Structure Tests - toString() and clone() for InExpression
// ============================================================================

TEST_F(ExpressionsTest, InExpression_ToString) {
	auto value = std::make_unique<LiteralExpression>(int64_t(2));
	std::vector<PropertyValue> listValues;
	listValues.push_back(PropertyValue(int64_t(1)));
	listValues.push_back(PropertyValue(int64_t(2)));
	listValues.push_back(PropertyValue(int64_t(3)));
	InExpression expr(std::move(value), std::move(listValues));

	std::string str = expr.toString();
	EXPECT_TRUE(str.find("IN") != std::string::npos);
}

TEST_F(ExpressionsTest, InExpression_Clone) {
	auto value = std::make_unique<LiteralExpression>(int64_t(2));
	std::vector<PropertyValue> listValues;
	listValues.push_back(PropertyValue(int64_t(1)));
	listValues.push_back(PropertyValue(int64_t(2)));
	listValues.push_back(PropertyValue(int64_t(3)));
	InExpression expr(std::move(value), std::move(listValues));

	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	EXPECT_NE(cloned.get(), &expr);

	// Evaluate the cloned expression
	ExpressionEvaluator evaluator(*context_);
	cloned->accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

// ============================================================================
// Batch 11: IN Expression Edge Cases
// ============================================================================

TEST_F(ExpressionsTest, InExpression_StringMatch) {
	auto value = std::make_unique<LiteralExpression>(std::string("banana"));
	std::vector<PropertyValue> listValues;
	listValues.push_back(PropertyValue(std::string("apple")));
	listValues.push_back(PropertyValue(std::string("banana")));
	listValues.push_back(PropertyValue(std::string("cherry")));
	InExpression expr(std::move(value), std::move(listValues));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, InExpression_StringNoMatch) {
	auto value = std::make_unique<LiteralExpression>(std::string("grape"));
	std::vector<PropertyValue> listValues;
	listValues.push_back(PropertyValue(std::string("apple")));
	listValues.push_back(PropertyValue(std::string("banana")));
	InExpression expr(std::move(value), std::move(listValues));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, InExpression_DoubleMatch) {
	auto value = std::make_unique<LiteralExpression>(3.14);
	std::vector<PropertyValue> listValues;
	listValues.push_back(PropertyValue(1.5));
	listValues.push_back(PropertyValue(3.14));
	listValues.push_back(PropertyValue(2.7));
	InExpression expr(std::move(value), std::move(listValues));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, InExpression_DuplicateInList) {
	auto value = std::make_unique<LiteralExpression>(int64_t(2));
	std::vector<PropertyValue> listValues;
	listValues.push_back(PropertyValue(int64_t(1)));
	listValues.push_back(PropertyValue(int64_t(2)));
	listValues.push_back(PropertyValue(int64_t(2))); // Duplicate
	InExpression expr(std::move(value), std::move(listValues));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, InExpression_MixedTypes) {
	auto value = std::make_unique<LiteralExpression>(int64_t(2));
	std::vector<PropertyValue> listValues;
	listValues.push_back(PropertyValue(std::string("1")));
	listValues.push_back(PropertyValue(int64_t(2)));
	listValues.push_back(PropertyValue(3.0));
	InExpression expr(std::move(value), std::move(listValues));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, InExpression_BooleanInList) {
	auto value = std::make_unique<LiteralExpression>(true);
	std::vector<PropertyValue> listValues;
	listValues.push_back(PropertyValue(false));
	listValues.push_back(PropertyValue(true));
	InExpression expr(std::move(value), std::move(listValues));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}
