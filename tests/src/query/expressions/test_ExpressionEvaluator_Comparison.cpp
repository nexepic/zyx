/**
 * @file test_ExpressionEvaluator_Comparison.cpp
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

#include "query/expressions/ExpressionEvaluatorTestFixture.hpp"

// ============================================================================
// Comparison Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Comparison_StartsWith_True) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello world"));
	auto right = std::make_unique<LiteralExpression>(std::string("hello"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_STARTS_WITH, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Comparison_StartsWith_False) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello world"));
	auto right = std::make_unique<LiteralExpression>(std::string("world"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_STARTS_WITH, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Comparison_StartsWith_LongerPattern) {
	auto left = std::make_unique<LiteralExpression>(std::string("hi"));
	auto right = std::make_unique<LiteralExpression>(std::string("hello"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_STARTS_WITH, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Comparison_EndsWith_True) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello world"));
	auto right = std::make_unique<LiteralExpression>(std::string("world"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ENDS_WITH, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Comparison_EndsWith_False) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello world"));
	auto right = std::make_unique<LiteralExpression>(std::string("hello"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ENDS_WITH, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Comparison_EndsWith_LongerPattern) {
	auto left = std::make_unique<LiteralExpression>(std::string("hi"));
	auto right = std::make_unique<LiteralExpression>(std::string("hello"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ENDS_WITH, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Comparison_Contains_True) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello world"));
	auto right = std::make_unique<LiteralExpression>(std::string("lo wo"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_CONTAINS, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Comparison_Contains_False) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello world"));
	auto right = std::make_unique<LiteralExpression>(std::string("xyz"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_CONTAINS, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Comparison_LessThan) {
	auto left = std::make_unique<LiteralExpression>(int64_t(1));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Comparison_GreaterThan) {
	auto left = std::make_unique<LiteralExpression>(int64_t(2));
	auto right = std::make_unique<LiteralExpression>(int64_t(1));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_GREATER, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Comparison_LessEqual) {
	auto left = std::make_unique<LiteralExpression>(int64_t(2));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS_EQUAL, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Comparison_GreaterEqual) {
	auto left = std::make_unique<LiteralExpression>(int64_t(2));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_GREATER_EQUAL, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Comparison_NotEqual) {
	auto left = std::make_unique<LiteralExpression>(int64_t(1));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_NOT_EQUAL, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Comparison_Equal_True) {
	auto left = std::make_unique<LiteralExpression>(int64_t(42));
	auto right = std::make_unique<LiteralExpression>(int64_t(42));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Comparison_Equal_False) {
	auto left = std::make_unique<LiteralExpression>(int64_t(1));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Comparison_Equal_Strings) {
	auto left = std::make_unique<LiteralExpression>(std::string("abc"));
	auto right = std::make_unique<LiteralExpression>(std::string("abc"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Comparison_NotEqual_Strings) {
	auto left = std::make_unique<LiteralExpression>(std::string("abc"));
	auto right = std::make_unique<LiteralExpression>(std::string("xyz"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_NOT_EQUAL, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Comparison_StartsWith_ExactMatch) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello"));
	auto right = std::make_unique<LiteralExpression>(std::string("hello"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_STARTS_WITH, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Comparison_EndsWith_ExactMatch) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello"));
	auto right = std::make_unique<LiteralExpression>(std::string("hello"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ENDS_WITH, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Comparison_RegexMatch_True) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello123"));
	auto right = std::make_unique<LiteralExpression>(std::string("hello[0-9]+"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_REGEX_MATCH, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionEvaluatorTest, Comparison_RegexMatch_False) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello"));
	auto right = std::make_unique<LiteralExpression>(std::string("[0-9]+"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_REGEX_MATCH, std::move(right));
	auto result = eval(&expr);
	EXPECT_FALSE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionEvaluatorTest, Comparison_RegexMatch_InvalidPattern) {
	auto left = std::make_unique<LiteralExpression>(std::string("test"));
	auto right = std::make_unique<LiteralExpression>(std::string("[invalid"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_REGEX_MATCH, std::move(right));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}
