/**
 * @file test_ExpressionEvaluator_Logical.cpp
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
// Logical Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Logical_AND_FalseAndNull) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Logical_AND_NullAndFalse) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Logical_AND_TrueAndNull) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Logical_AND_NullAndNull) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Logical_AND_TrueAndTrue) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Logical_OR_TrueOrNull) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Logical_OR_NullOrTrue) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Logical_OR_FalseOrNull) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Logical_OR_NullOrNull) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Logical_OR_FalseOrFalse) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Logical_XOR_TrueXorFalse) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Logical_XOR_TrueXorTrue) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Logical_XOR_NullXorTrue) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Logical_XOR_TrueXorNull) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Logical_XOR_NullXorNull) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Logical_XOR_FalseXorFalse) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Logical_AND_FalseAndFalse) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Logical_AND_TrueAndFalse) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Logical_OR_TrueOrFalse) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Logical_OR_TrueOrTrue) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Logical_AND_NullAndTrue) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Logical_OR_NullOrFalse) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Logical_AND_BothTrue) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Logical_OR_BothFalse) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Logical_AND_NullAndTrue_ReturnsNull) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Logical_OR_NullOrFalse_ReturnsNull) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Logical_OR_FalseOrNull_ReturnsNull) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Logical_XOR_NullXorFalse) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Logical_XOR_FalseXorNull) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}
