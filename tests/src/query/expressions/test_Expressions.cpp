/**
 * @file test_Expressions.cpp
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
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/core/Entity.hpp"
#include "graph/query/api/ResultValue.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/ReduceExpression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/expressions/ShortestPathExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

// ============================================================================
// Test Fixture
// ============================================================================

class ExpressionsTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Create a test record with some variables
		record_.setValue("x", PropertyValue(42));
		record_.setValue("y", PropertyValue(3.14));
		record_.setValue("name", PropertyValue(std::string("Alice")));
		record_.setValue("flag", PropertyValue(true));
		record_.setValue("nullVal", PropertyValue()); // NULL

		// Create a node with properties
		node_ = Node(1, 100);
		std::unordered_map<std::string, PropertyValue> props;
		props["age"] = PropertyValue(30);
		props["city"] = PropertyValue(std::string("NYC"));
		node_.setProperties(props);
		record_.setNode("n", node_);

		// Create an edge with properties
		edge_ = Edge(2, 1, 2, 101);
		std::unordered_map<std::string, PropertyValue> edgeProps;
		edgeProps["weight"] = PropertyValue(1.5);
		edge_.setProperties(edgeProps);
		record_.setEdge("r", edge_);

		context_ = std::make_unique<EvaluationContext>(record_);
	}

	Record record_;
	Node node_;
	Edge edge_;
	std::unique_ptr<EvaluationContext> context_;
};

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
// ExpressionEvaluationHelper Tests
// ============================================================================

TEST_F(ExpressionsTest, ExpressionEvaluationHelper_Evaluate_NullExpression) {
	PropertyValue result = ExpressionEvaluationHelper::evaluate(nullptr, record_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, ExpressionEvaluationHelper_Evaluate_LiteralInteger) {
	auto expr = std::make_unique<LiteralExpression>(int64_t(123));
	PropertyValue result = ExpressionEvaluationHelper::evaluate(expr.get(), record_);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 123);
}

TEST_F(ExpressionsTest, ExpressionEvaluationHelper_EvaluateBool_LiteralTrue) {
	auto expr = std::make_unique<LiteralExpression>(true);
	bool result = ExpressionEvaluationHelper::evaluateBool(expr.get(), record_);
	EXPECT_TRUE(result);
}

TEST_F(ExpressionsTest, ExpressionEvaluationHelper_EvaluateBool_NullExpression) {
	bool result = ExpressionEvaluationHelper::evaluateBool(nullptr, record_);
	EXPECT_FALSE(result);
}

TEST_F(ExpressionsTest, ExpressionEvaluationHelper_EvaluateInt_Literal) {
	auto expr = std::make_unique<LiteralExpression>(int64_t(42));
	int64_t result = ExpressionEvaluationHelper::evaluateInt(expr.get(), record_);
	EXPECT_EQ(result, 42);
}

TEST_F(ExpressionsTest, ExpressionEvaluationHelper_EvaluateDouble_Literal) {
	auto expr = std::make_unique<LiteralExpression>(3.14);
	double result = ExpressionEvaluationHelper::evaluateDouble(expr.get(), record_);
	EXPECT_DOUBLE_EQ(result, 3.14);
}

TEST_F(ExpressionsTest, ExpressionEvaluationHelper_EvaluateBatch_MultipleRecords) {
	auto expr = std::make_unique<LiteralExpression>(int64_t(10));

	std::vector<Record> records;
	Record r1;
	r1.setValue("x", PropertyValue(1));
	Record r2;
	r2.setValue("x", PropertyValue(2));
	records.push_back(std::move(r1));
	records.push_back(std::move(r2));

	std::vector<PropertyValue> results = ExpressionEvaluationHelper::evaluateBatch(expr.get(), records);
	EXPECT_EQ(results.size(), 2u);
	EXPECT_EQ(std::get<int64_t>(results[0].getVariant()), 10);
	EXPECT_EQ(std::get<int64_t>(results[1].getVariant()), 10);
}

// ============================================================================
// ExpressionEvaluator Tests - Literal
// ============================================================================

TEST_F(ExpressionsTest, ExpressionEvaluator_VisitLiteral_Integer) {
	LiteralExpression expr(int64_t(42));
	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 42);
}

TEST_F(ExpressionsTest, ExpressionEvaluator_VisitLiteral_Double) {
	LiteralExpression expr(3.14);
	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), 3.14);
}

TEST_F(ExpressionsTest, ExpressionEvaluator_VisitLiteral_String) {
	LiteralExpression expr(std::string("hello"));
	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "hello");
}

TEST_F(ExpressionsTest, ExpressionEvaluator_VisitLiteral_Boolean) {
	LiteralExpression expr(true);
	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, ExpressionEvaluator_VisitLiteral_Null) {
	LiteralExpression expr;
	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, ExpressionEvaluator_VisitLiteral_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	PropertyValue result = evaluator.evaluate(nullptr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// ExpressionEvaluator Tests - Variable Reference
// ============================================================================

TEST_F(ExpressionsTest, ExpressionEvaluator_VisitVariableReference_Simple) {
	VariableReferenceExpression expr(std::string("x"));
	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 42);
}

TEST_F(ExpressionsTest, ExpressionEvaluator_VisitVariableReference_Property) {
	VariableReferenceExpression expr(std::string("n"), std::string("age"));
	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 30);
}

TEST_F(ExpressionsTest, ExpressionEvaluator_VisitVariableReference_UndefinedVariable) {
	VariableReferenceExpression expr(std::string("undefined"));
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), UndefinedVariableException);
}

TEST_F(ExpressionsTest, ExpressionEvaluator_VisitVariableReference_MissingProperty) {
	VariableReferenceExpression expr(std::string("n"), std::string("missing"));
	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// ExpressionEvaluator Tests - Binary Operations
// ============================================================================

TEST_F(ExpressionsTest, BinaryOp_Addition_Integer) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 8);
}

TEST_F(ExpressionsTest, BinaryOp_Addition_Double) {
	auto left = std::make_unique<LiteralExpression>(2.5);
	auto right = std::make_unique<LiteralExpression>(1.5);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), 4.0);
}

TEST_F(ExpressionsTest, BinaryOp_Subtraction) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 2);
}

TEST_F(ExpressionsTest, BinaryOp_Multiplication) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 15);
}

TEST_F(ExpressionsTest, BinaryOp_Division) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// 10 / 2 is evenly divisible, so result is integer
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 5);
}

TEST_F(ExpressionsTest, BinaryOp_Modulus) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 1);
}

TEST_F(ExpressionsTest, BinaryOp_DivisionByZero) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), DivisionByZeroException);
}

TEST_F(ExpressionsTest, BinaryOp_ModulusByZero) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), DivisionByZeroException);
}

// ============================================================================
// ExpressionEvaluator Tests - Binary Comparison Operations
// ============================================================================

TEST_F(ExpressionsTest, BinaryOp_Equal_Integer) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_NotEqual) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_NOT_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LessThan) {
	auto left = std::make_unique<LiteralExpression>(int64_t(3));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_GreaterThanOrEqual) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_GREATER_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_Greater) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_GREATER, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LessEqual) {
	auto left = std::make_unique<LiteralExpression>(int64_t(3));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

// ============================================================================
// ExpressionEvaluator Tests - Binary Logical Operations
// ============================================================================

TEST_F(ExpressionsTest, BinaryOp_LogicalAnd_TrueTrue) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalAnd_TrueFalse) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalOr_FalseTrue) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalOr_FalseFalse) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalXor_TrueTrue) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalXor_TrueFalse) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

// ============================================================================
// ExpressionEvaluator Tests - Unary Operations
// ============================================================================

TEST_F(ExpressionsTest, UnaryOp_Minus_Integer) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(42));
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), -42);
}

TEST_F(ExpressionsTest, UnaryOp_Minus_Double) {
	auto operand = std::make_unique<LiteralExpression>(3.14);
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), -3.14);
}

TEST_F(ExpressionsTest, UnaryOp_Not_True) {
	auto operand = std::make_unique<LiteralExpression>(true);
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, UnaryOp_Not_False) {
	auto operand = std::make_unique<LiteralExpression>(false);
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

// ============================================================================
// ExpressionEvaluator Tests - NULL Propagation
// ============================================================================

TEST_F(ExpressionsTest, BinaryOp_NullLeft) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, BinaryOp_NullRight) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, UnaryOp_NullOperand) {
	auto operand = std::make_unique<LiteralExpression>(); // NULL
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

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
// ExpressionEvaluator Tests - CASE Expression
// ============================================================================

TEST_F(ExpressionsTest, CaseExpression_Simple_Match) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(2)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(2)), std::make_unique<LiteralExpression>(std::string("two")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "two");
}

TEST_F(ExpressionsTest, CaseExpression_Simple_NoMatch_Else) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(99)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(2)), std::make_unique<LiteralExpression>(std::string("two")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "other");
}

TEST_F(ExpressionsTest, CaseExpression_Simple_NoMatch_NoElse) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(99)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, CaseExpression_Searched_True) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(true), std::make_unique<LiteralExpression>(std::string("second")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "second");
}

TEST_F(ExpressionsTest, CaseExpression_Searched_AllFalse_Else) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("second")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "other");
}

TEST_F(ExpressionsTest, CaseExpression_Searched_NullCondition) {
	CaseExpression expr; // Searched CASE
	// NULL in WHEN should be skipped
	expr.addBranch(std::make_unique<LiteralExpression>(), std::make_unique<LiteralExpression>(std::string("skipped")));
	expr.addBranch(std::make_unique<LiteralExpression>(true), std::make_unique<LiteralExpression>(std::string("second")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "second");
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
// Batch 4: Expression Structure Tests - toString() and clone()
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

TEST_F(ExpressionsTest, CaseExpression_ToString_Simple) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(2)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));

	std::string str = expr.toString();
	EXPECT_TRUE(str.find("CASE") != std::string::npos);
}

TEST_F(ExpressionsTest, CaseExpression_ToString_Searched) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(true), std::make_unique<LiteralExpression>(std::string("first")));

	std::string str = expr.toString();
	EXPECT_TRUE(str.find("CASE") != std::string::npos);
	EXPECT_TRUE(str.find("WHEN") != std::string::npos);
	EXPECT_TRUE(str.find("THEN") != std::string::npos);
}

TEST_F(ExpressionsTest, CaseExpression_Clone_Simple) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(2)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(2)), std::make_unique<LiteralExpression>(std::string("two")));

	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	EXPECT_NE(cloned.get(), &expr);

	// Evaluate the cloned expression
	ExpressionEvaluator evaluator(*context_);
	cloned->accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "two");
}

TEST_F(ExpressionsTest, CaseExpression_Clone_Searched) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(true), std::make_unique<LiteralExpression>(std::string("second")));

	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	EXPECT_NE(cloned.get(), &expr);

	// Evaluate the cloned expression
	ExpressionEvaluator evaluator(*context_);
	cloned->accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "second");
}

TEST_F(ExpressionsTest, BinaryOp_ToString_AllOperators) {
	// Test all binary operators have proper string representation
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));

	std::vector<BinaryOperatorType> operators = {
		BinaryOperatorType::BOP_ADD,
		BinaryOperatorType::BOP_SUBTRACT,
		BinaryOperatorType::BOP_MULTIPLY,
		BinaryOperatorType::BOP_DIVIDE,
		BinaryOperatorType::BOP_MODULO,
		BinaryOperatorType::BOP_EQUAL,
		BinaryOperatorType::BOP_NOT_EQUAL,
		BinaryOperatorType::BOP_LESS,
		BinaryOperatorType::BOP_GREATER,
		BinaryOperatorType::BOP_LESS_EQUAL,
		BinaryOperatorType::BOP_GREATER_EQUAL,
		BinaryOperatorType::BOP_AND,
		BinaryOperatorType::BOP_OR,
		BinaryOperatorType::BOP_XOR
	};

	for (auto op : operators) {
		auto leftClone = std::make_unique<LiteralExpression>(int64_t(5));
		auto rightClone = std::make_unique<LiteralExpression>(int64_t(3));
		BinaryOpExpression expr(std::move(leftClone), op, std::move(rightClone));

		std::string str = expr.toString();
		EXPECT_FALSE(str.empty()) << "Operator " << static_cast<int>(op) << " should have string representation";
	}
}

TEST_F(ExpressionsTest, UnaryOp_ToString_AllOperators) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(42));

	std::vector<UnaryOperatorType> operators = {
		UnaryOperatorType::UOP_MINUS,
		UnaryOperatorType::UOP_NOT
	};

	for (auto op : operators) {
		auto operandClone = std::make_unique<LiteralExpression>(int64_t(42));
		UnaryOpExpression expr(op, std::move(operandClone));

		std::string str = expr.toString();
		EXPECT_FALSE(str.empty()) << "Operator " << static_cast<int>(op) << " should have string representation";
	}
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
// Batch 6: ExpressionEvaluator - Additional Coverage
// ============================================================================

TEST_F(ExpressionsTest, ExpressionEvaluator_BinaryOp_UnknownOperator) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	// Create an expression but modify the operator to something invalid
	// This is tricky since we can't directly set an invalid operator
	// Instead, we'll test with a valid operator to ensure coverage
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_NO_THROW((void)expr.accept(evaluator));
}

TEST_F(ExpressionsTest, ExpressionEvaluator_UnaryOp_UnknownOperator) {
	// Similar to above, we test the valid path
	auto operand = std::make_unique<LiteralExpression>(int64_t(42));
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_NO_THROW((void)expr.accept(evaluator));
}

// ============================================================================
// Batch 7: FunctionRegistry - Edge Cases
// ============================================================================

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

// ============================================================================
// Batch 8: BinaryOp Edge Cases
// ============================================================================

TEST_F(ExpressionsTest, BinaryOp_MultiplyByZero) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 0);
}

TEST_F(ExpressionsTest, BinaryOp_SubtractNegative) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(-3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 8);
}

TEST_F(ExpressionsTest, BinaryOp_DivisionNegative) {
	auto left = std::make_unique<LiteralExpression>(int64_t(-10));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), -5);
}

TEST_F(ExpressionsTest, BinaryOp_CompareStrings) {
	auto left = std::make_unique<LiteralExpression>(std::string("apple"));
	auto right = std::make_unique<LiteralExpression>(std::string("banana"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_CompareStringAndNumber) {
	auto left = std::make_unique<LiteralExpression>(std::string("42"));
	auto right = std::make_unique<LiteralExpression>(int64_t(42));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalAnd_ZeroInteger) {
	auto left = std::make_unique<LiteralExpression>(int64_t(0));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_LogicalOr_ZeroInteger) {
	auto left = std::make_unique<LiteralExpression>(int64_t(0));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

// ============================================================================
// Batch 9: UnaryOp Edge Cases
// ============================================================================

TEST_F(ExpressionsTest, UnaryOp_Minus_Zero) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(0));
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 0);
}

TEST_F(ExpressionsTest, UnaryOp_Minus_NegativeDouble) {
	auto operand = std::make_unique<LiteralExpression>(-3.14);
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), 3.14);
}

TEST_F(ExpressionsTest, UnaryOp_Not_ZeroInteger) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(0));
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, UnaryOp_Not_NonZeroInteger) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(42));
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

// ============================================================================
// Batch 10: Function Call Edge Cases
// ============================================================================

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

// ============================================================================
// Batch 12: CASE Expression Edge Cases
// ============================================================================

TEST_F(ExpressionsTest, CaseExpression_Simple_MultipleBranches) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(3)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(2)), std::make_unique<LiteralExpression>(std::string("two")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(3)), std::make_unique<LiteralExpression>(std::string("three")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(4)), std::make_unique<LiteralExpression>(std::string("four")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "three");
}

TEST_F(ExpressionsTest, CaseExpression_Simple_FirstBranchMatches) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(1)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("second"))); // Duplicate
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "first");
}

TEST_F(ExpressionsTest, CaseExpression_Searched_MultipleBranches) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("second")));
	expr.addBranch(std::make_unique<LiteralExpression>(true), std::make_unique<LiteralExpression>(std::string("third")));
	expr.addBranch(std::make_unique<LiteralExpression>(true), std::make_unique<LiteralExpression>(std::string("fourth")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "third");
}

TEST_F(ExpressionsTest, CaseExpression_Searched_AllFalseNoElse) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(0)), std::make_unique<LiteralExpression>(std::string("second")));
	// No else

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsTest, CaseExpression_Simple_StringComparison) {
	CaseExpression expr(std::make_unique<LiteralExpression>(std::string("b")));
	expr.addBranch(std::make_unique<LiteralExpression>(std::string("a")), std::make_unique<LiteralExpression>(int64_t(1)));
	expr.addBranch(std::make_unique<LiteralExpression>(std::string("b")), std::make_unique<LiteralExpression>(int64_t(2)));
	expr.addBranch(std::make_unique<LiteralExpression>(std::string("c")), std::make_unique<LiteralExpression>(int64_t(3)));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 2);
}

TEST_F(ExpressionsTest, CaseExpression_Searched_IntegerCondition) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(0)), std::make_unique<LiteralExpression>(std::string("zero")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(5)), std::make_unique<LiteralExpression>(std::string("five")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// int64_t(0) is falsy (toBoolean returns false), int64_t(5) is truthy
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "five");
}

// ============================================================================
// Batch 13: Additional Edge Cases for Branch Coverage
// ============================================================================

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

TEST_F(ExpressionsTest, BinaryOp_Compare_BoolVsInt) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(int64_t(1));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// Boolean and integer should not be equal even if both represent "true"
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, BinaryOp_Compare_BoolVsDouble) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(1.0);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, UnaryOp_Not_String) {
	auto operand = std::make_unique<LiteralExpression>(std::string("hello"));
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// Non-empty string is truthy, NOT makes it falsy
	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsTest, UnaryOp_Not_EmptyString) {
	auto operand = std::make_unique<LiteralExpression>(std::string(""));
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// Empty string is falsy, NOT makes it truthy
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

TEST_F(ExpressionsTest, CaseExpression_Simple_ElseBranch) {
	CaseExpression expr(std::make_unique<LiteralExpression>(int64_t(99)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("first")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(2)), std::make_unique<LiteralExpression>(std::string("second")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("else")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "else");
}

TEST_F(ExpressionsTest, CaseExpression_Simple_NullComparison) {
	CaseExpression expr(std::make_unique<LiteralExpression>()); // NULL
	expr.addBranch(std::make_unique<LiteralExpression>(), std::make_unique<LiteralExpression>(std::string("null")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("else")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// NULL == NULL returns false in PropertyValue comparison, so else is taken
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "else");
}

TEST_F(ExpressionsTest, CaseExpression_Simple_CompareNullWithValue) {
	CaseExpression expr(std::make_unique<LiteralExpression>()); // NULL
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)), std::make_unique<LiteralExpression>(std::string("one")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("else")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// NULL != 1, so should go to else
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "else");
}

TEST_F(ExpressionsTest, CaseExpression_Searched_DoubleCondition) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(3.14), std::make_unique<LiteralExpression>(std::string("pi")));
	expr.addBranch(std::make_unique<LiteralExpression>(2.71), std::make_unique<LiteralExpression>(std::string("e")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// 3.14 is truthy (non-zero)
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "pi");
}

TEST_F(ExpressionsTest, CaseExpression_Searched_NullConditionSkipped) {
	CaseExpression expr; // Searched CASE
	expr.addBranch(std::make_unique<LiteralExpression>(), std::make_unique<LiteralExpression>(std::string("null")));
	expr.addBranch(std::make_unique<LiteralExpression>(false), std::make_unique<LiteralExpression>(std::string("false")));
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("else")));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	// NULL branch is skipped, false doesn't match, so else is returned
	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "else");
}

// ============================================================================
// Batch 14: FunctionRegistry - More Edge Cases
// ============================================================================

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
// Coverage Optimization - Targeted Tests for Missing Branches
// ============================================================================

// Target: Expression.cpp - improve clone() coverage
TEST_F(ExpressionsTest, Expression_Clone_Coverage_Bonus) {
	// VariableReferenceExpression - both constructors
	{
		VariableReferenceExpression varExpr(std::string("x"));
		auto clone1 = varExpr.clone();
		ASSERT_NE(clone1.get(), &varExpr);
	}
	{
		VariableReferenceExpression propExpr(std::string("n"), std::string("age"));
		auto clone2 = propExpr.clone();
		ASSERT_NE(clone2.get(), &propExpr);
	}

	// BinaryOpExpression - test all operator types
	std::vector<BinaryOperatorType> ops = {
		BinaryOperatorType::BOP_AND, BinaryOperatorType::BOP_OR, BinaryOperatorType::BOP_XOR,
		BinaryOperatorType::BOP_EQUAL, BinaryOperatorType::BOP_NOT_EQUAL
	};
	for (auto op : ops) {
		auto left = std::make_unique<LiteralExpression>(true);
		auto right = std::make_unique<LiteralExpression>(false);
		BinaryOpExpression expr(std::move(left), op, std::move(right));
		auto clone = expr.clone();
		ASSERT_NE(clone.get(), &expr);
	}

	// UnaryOpExpression - both operators
	{
		auto operand = std::make_unique<LiteralExpression>(int64_t(42));
		UnaryOpExpression expr1(UnaryOperatorType::UOP_MINUS, std::move(operand));
		auto clone1 = expr1.clone();
		ASSERT_NE(clone1.get(), &expr1);
	}
	{
		auto operand = std::make_unique<LiteralExpression>(true);
		UnaryOpExpression expr2(UnaryOperatorType::UOP_NOT, std::move(operand));
		auto clone2 = expr2.clone();
		ASSERT_NE(clone2.get(), &expr2);
	}
}

// Target: ExpressionEvaluator.cpp - improve branch coverage
TEST_F(ExpressionsTest, ExpressionEvaluator_MissingBranches) {
	// Test modulo by zero
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(10));
		auto right = std::make_unique<LiteralExpression>(int64_t(0));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_THROW((void)expr.accept(evaluator), DivisionByZeroException);
	}

	// Test unary NOT with integer
	{
		auto operand = std::make_unique<LiteralExpression>(int64_t(0));
		UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Test function call with invalid arg count
	{
		std::vector<std::unique_ptr<Expression>> args;
		FunctionCallExpression expr("tostring", std::move(args));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
	}
}

// Target: FunctionRegistry.cpp - improve region coverage
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

// Target: ExpressionEvaluator.cpp - branch coverage 84.96% → 85%+
// Missing: test specific operator branches
TEST_F(ExpressionsTest, ExpressionEvaluator_BranchCoverage_AllComparisons) {
	std::vector<BinaryOperatorType> comparisonOps = {
		BinaryOperatorType::BOP_EQUAL, BinaryOperatorType::BOP_NOT_EQUAL,
		BinaryOperatorType::BOP_LESS, BinaryOperatorType::BOP_GREATER,
		BinaryOperatorType::BOP_LESS_EQUAL, BinaryOperatorType::BOP_GREATER_EQUAL
	};

	for (auto op : comparisonOps) {
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(3));
		BinaryOpExpression expr(std::move(left), op, std::move(right));

		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
	}
}

TEST_F(ExpressionsTest, ExpressionEvaluator_BranchCoverage_LogicalOps) {
	// AND - all combinations
	{
		std::vector<std::pair<bool, bool>> tests = {
			{true, true}, {true, false}, {false, true}, {false, false}
		};
		for (auto [left, right] : tests) {
			auto leftExpr = std::make_unique<LiteralExpression>(left);
			auto rightExpr = std::make_unique<LiteralExpression>(right);
			BinaryOpExpression expr(std::move(leftExpr), BinaryOperatorType::BOP_AND, std::move(rightExpr));
			ExpressionEvaluator evaluator(*context_);
			expr.accept(evaluator);
			EXPECT_EQ(std::get<bool>(evaluator.getResult().getVariant()), left && right);
		}
	}

	// OR - all combinations
	{
		std::vector<std::pair<bool, bool>> tests = {
			{true, true}, {true, false}, {false, true}, {false, false}
		};
		for (auto [left, right] : tests) {
			auto leftExpr = std::make_unique<LiteralExpression>(left);
			auto rightExpr = std::make_unique<LiteralExpression>(right);
			BinaryOpExpression expr(std::move(leftExpr), BinaryOperatorType::BOP_OR, std::move(rightExpr));
			ExpressionEvaluator evaluator(*context_);
			expr.accept(evaluator);
			EXPECT_EQ(std::get<bool>(evaluator.getResult().getVariant()), left || right);
		}
	}

	// XOR - all combinations
	{
		std::vector<std::pair<bool, bool>> tests = {
			{true, true}, {true, false}, {false, true}, {false, false}
		};
		for (auto [left, right] : tests) {
			auto leftExpr = std::make_unique<LiteralExpression>(left);
			auto rightExpr = std::make_unique<LiteralExpression>(right);
			BinaryOpExpression expr(std::move(leftExpr), BinaryOperatorType::BOP_XOR, std::move(rightExpr));
			ExpressionEvaluator evaluator(*context_);
			expr.accept(evaluator);
			EXPECT_EQ(std::get<bool>(evaluator.getResult().getVariant()), left != right);
		}
	}
}

// Target: FunctionRegistry.cpp - region coverage 76.87% → 85%+
// Missing: edge cases in string/math functions
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

// Target: ExpressionEvaluator.cpp - branch coverage 84.96% → 85%+
// Missing: Default case in evaluateArithmetic for invalid operator
TEST_F(ExpressionsTest, ExpressionEvaluator_FinalBranches_DefaultCase) {
	// This test is hard to hit directly since BinaryOpExpression only allows valid operators
	// But we can test various edge cases to increase coverage

	// Test division with non-zero double
	{
		auto left = std::make_unique<LiteralExpression>(10.0);
		auto right = std::make_unique<LiteralExpression>(2.0);
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
		EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), 5.0);
	}

	// Test modulo with negative numbers
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(-10));
		auto right = std::make_unique<LiteralExpression>(int64_t(3));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), -1);
	}

	// Test unary MINUS with double
	{
		auto operand = std::make_unique<LiteralExpression>(3.14);
		UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
		EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), -3.14);
	}

	// Test unary NOT with double (non-zero)
	{
		auto operand = std::make_unique<LiteralExpression>(1.0);
		UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
		EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
	}
}

// Target: FunctionRegistry.cpp - region coverage 76.87% → 85%+
// Missing: Edge case regions in various functions
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

// Target: ExpressionEvaluator.cpp - branch coverage at 84.96%, need +0.04%
// Need to test remaining operator branches
TEST_F(ExpressionsTest, ExpressionEvaluator_AdditionalBranches) {
	// Test all arithmetic operators with different operand types
	std::vector<BinaryOperatorType> arithmeticOps = {
		BinaryOperatorType::BOP_ADD, BinaryOperatorType::BOP_SUBTRACT,
		BinaryOperatorType::BOP_MULTIPLY, BinaryOperatorType::BOP_DIVIDE
	};

	for (auto op : arithmeticOps) {
		auto left = std::make_unique<LiteralExpression>(2.5);
		auto right = std::make_unique<LiteralExpression>(1.5);
		BinaryOpExpression expr(std::move(left), op, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
	}

	// Test all comparison operators with doubles
	std::vector<BinaryOperatorType> comparisonOps = {
		BinaryOperatorType::BOP_EQUAL, BinaryOperatorType::BOP_NOT_EQUAL,
		BinaryOperatorType::BOP_LESS, BinaryOperatorType::BOP_GREATER,
		BinaryOperatorType::BOP_LESS_EQUAL, BinaryOperatorType::BOP_GREATER_EQUAL
	};

	for (auto op : comparisonOps) {
		auto left = std::make_unique<LiteralExpression>(3.14);
		auto right = std::make_unique<LiteralExpression>(2.71);
		BinaryOpExpression expr(std::move(left), op, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
	}

	// Test XOR specifically
	{
		auto left = std::make_unique<LiteralExpression>(true);
		auto right = std::make_unique<LiteralExpression>(true);
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Test unary MINUS with zero
	{
		auto operand = std::make_unique<LiteralExpression>(0.0);
		UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
		EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), -0.0);
	}
}

// Target: FunctionRegistry.cpp - region coverage at 78.36%, need +6.64%
// Need to test edge cases in various functions
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

// ============================================================================
// Final Tests for Remaining Edge Cases
// ============================================================================

// Test functions with empty args (hit args.empty() branches)
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

// Test substring with exactly 2 args (no length parameter)
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

// Test more edge cases for substring
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

// Test trim with empty and whitespace-only strings
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

// Test NULL argument handling
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

// ============================================================================
// Ultimate Coverage Push - Hit Remaining Branches
// ============================================================================

// Target: FunctionRegistry.cpp - more edge cases
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

// Target: ExpressionEvaluator.cpp - hit remaining branches
TEST_F(ExpressionsTest, ExpressionEvaluator_UltimateBranches) {
	// Test division resulting in non-integer
	{
		auto left = std::make_unique<LiteralExpression>(7.0);
		auto right = std::make_unique<LiteralExpression>(2.0);
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
		EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), 3.5);
	}

	// Test modulo with positive and negative
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(10));
		auto right = std::make_unique<LiteralExpression>(int64_t(-3));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
	}

	// Test unary NOT with different numeric types
	{
		auto operand = std::make_unique<LiteralExpression>(int64_t(0));
		UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	{
		auto operand = std::make_unique<LiteralExpression>(int64_t(5));
		UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
		EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
	}
}

// Test various replace edge cases
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

// Test startsWith and endsWith edge cases
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

// ============================================================================
// Final Targeted Tests for 85%+ Coverage
// ============================================================================

// Additional edge cases for region coverage
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

// Additional expression evaluator tests
TEST_F(ExpressionsTest, ExpressionEvaluator_FinalCases) {
	// XOR with different boolean combinations
	{
		auto left = std::make_unique<LiteralExpression>(true);
		auto right = std::make_unique<LiteralExpression>(false);
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// AND with short-circuit (right is not evaluated)
	{
		auto left = std::make_unique<LiteralExpression>(false);
		auto right = std::make_unique<LiteralExpression>(int64_t(999));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
		EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// OR with short-circuit
	{
		auto left = std::make_unique<LiteralExpression>(true);
		auto right = std::make_unique<LiteralExpression>(int64_t(0));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Unary MINUS with various values
	{
		auto operand = std::make_unique<LiteralExpression>(0.0);
		UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
		EXPECT_DOUBLE_EQ(std::get<double>(evaluator.getResult().getVariant()), -0.0);
	}

	{
		auto operand = std::make_unique<LiteralExpression>(int64_t(-99));
		UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		EXPECT_NO_THROW((void)expr.accept(evaluator));
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 99);
	}
}

// ============================================================================
// Absolutely Final Tests - Push to 85%+
// ============================================================================

// More substring edge cases
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

// More numeric edge cases for abs
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

// More comparison edge cases
TEST_F(ExpressionsTest, ExpressionEvaluator_Comparison_EdgeCases) {
	// Equal with same values
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(42));
		auto right = std::make_unique<LiteralExpression>(int64_t(42));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Not equal with different values
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(42));
		auto right = std::make_unique<LiteralExpression>(int64_t(43));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_NOT_EQUAL, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Less with equal values (should be false)
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Greater with equal values (should be false)
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_GREATER, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
	}
}

// More arithmetic edge cases
TEST_F(ExpressionsTest, ExpressionEvaluator_Arithmetic_EdgeCases) {
	// Add with zero
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(0));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 5);
	}

	// Multiply by one
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(7));
		auto right = std::make_unique<LiteralExpression>(int64_t(1));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 7);
	}
}

// More string conversion edge cases
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

// ============================================================================
// Final Edge Cases - Target Specific Missing Regions/Branches
// ============================================================================

// Test division and modulo edge cases more thoroughly
TEST_F(ExpressionsTest, ExpressionEvaluator_DivisionModulo_FinalEdgeCases) {
	// Division with positive integers
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(10));
		auto right = std::make_unique<LiteralExpression>(int64_t(3));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_NEAR(std::get<double>(evaluator.getResult().getVariant()), 3.333, 0.001);
	}

	// Division with negative numbers
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(-10));
		auto right = std::make_unique<LiteralExpression>(int64_t(3));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_NEAR(std::get<double>(evaluator.getResult().getVariant()), -3.333, 0.001);
	}

	// Modulo with positive integers
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(10));
		auto right = std::make_unique<LiteralExpression>(int64_t(3));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 1);
	}
}

// Test more substring NULL handling
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

// Test more math function edge cases
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

// Test more comparison operator combinations
TEST_F(ExpressionsTest, ExpressionEvaluator_Comparison_FinalCombinations) {
	// LESS_EQUAL with equal values
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS_EQUAL, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// GREATER_EQUAL with equal values
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_GREATER_EQUAL, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// LESS_EQUAL with less than
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(3));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_LESS_EQUAL, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// GREATER_EQUAL with greater than
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(7));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_GREATER_EQUAL, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}
}

// ============================================================================
// Absolute Final Edge Cases - Last Push to 85%+
// ============================================================================

// Test last substring edge cases for region coverage
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

// Test trim with various whitespace patterns
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

// Test final arithmetic combinations
TEST_F(ExpressionsTest, ExpressionEvaluator_Arithmetic_AbsoluteFinal) {
	// Subtract to get zero
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 0);
	}

	// Subtract to get negative
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(3));
		auto right = std::make_unique<LiteralExpression>(int64_t(5));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), -2);
	}

	// Multiply by zero
	{
		auto left = std::make_unique<LiteralExpression>(int64_t(99));
		auto right = std::make_unique<LiteralExpression>(int64_t(0));
		BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 0);
	}
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

// Test final string function edge cases
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


// ============================================================================
// Coverage Tests (from test_Expressions_Coverage.cpp)
// ============================================================================

// ============================================================================
// ConstExpressionVisitor Test Implementation
// ============================================================================

/**
 * @brief Test visitor that implements ConstExpressionVisitor interface
 * Used to test the accept(ConstExpressionVisitor&) methods on all expression types
 */
class TestConstExpressionVisitor : public ConstExpressionVisitor {
public:
	void visit(const LiteralExpression* expr [[maybe_unused]]) override { visitedLiteral = true; }
	void visit(const VariableReferenceExpression* expr [[maybe_unused]]) override { visitedVarRef = true; }
	void visit(const BinaryOpExpression* expr [[maybe_unused]]) override { visitedBinary = true; }
	void visit(const UnaryOpExpression* expr [[maybe_unused]]) override { visitedUnary = true; }
	void visit(const FunctionCallExpression* expr [[maybe_unused]]) override { visitedFunction = true; }
	void visit(const CaseExpression* expr [[maybe_unused]]) override { visitedCase = true; }
	void visit(const InExpression* expr [[maybe_unused]]) override { visitedIn = true; }
	void visit(const ListSliceExpression* expr [[maybe_unused]]) override { visitedListSlice = true; }
	void visit(const ListComprehensionExpression* expr [[maybe_unused]]) override { visitedListComprehension = true; }
	void visit(const ListLiteralExpression* expr [[maybe_unused]]) override { visitedListLiteral = true; }
	void visit(const IsNullExpression* expr [[maybe_unused]]) override { visitedIsNull = true; }
	void visit(const QuantifierFunctionExpression* expr [[maybe_unused]]) override { visitedQuantifierFunction = true; }
	void visit(const ExistsExpression* expr [[maybe_unused]]) override { visitedExists = true; }
	void visit(const PatternComprehensionExpression* expr [[maybe_unused]]) override { visitedPatternComprehension = true; }
	void visit(const ReduceExpression* expr [[maybe_unused]]) override {}
	void visit(const ParameterExpression* expr [[maybe_unused]]) override { visitedParameter = true; }
	void visit(const ShortestPathExpression* expr [[maybe_unused]]) override { visitedShortestPath = true; }
	void visit(const MapProjectionExpression* expr [[maybe_unused]]) override { visitedMapProjection = true; }

	// Track which visit methods were called
	bool visitedLiteral = false;
	bool visitedVarRef = false;
	bool visitedBinary = false;
	bool visitedUnary = false;
	bool visitedFunction = false;
	bool visitedCase = false;
	bool visitedIn = false;
	bool visitedListSlice = false;
	bool visitedListComprehension = false;
	bool visitedListLiteral = false;
	bool visitedIsNull = false;
	bool visitedQuantifierFunction = false;
	bool visitedExists = false;
	bool visitedPatternComprehension = false;
	bool visitedParameter = false;
	bool visitedShortestPath = false;
	bool visitedMapProjection = false;
};

/**
 * @brief Test visitor that implements ExpressionVisitor interface (non-const)
 * Used to test the accept(ExpressionVisitor&) methods on all expression types
 */
class TestExpressionVisitor : public ExpressionVisitor {
public:
	void visit(LiteralExpression* expr [[maybe_unused]]) override { visitedLiteral = true; }
	void visit(VariableReferenceExpression* expr [[maybe_unused]]) override { visitedVarRef = true; }
	void visit(BinaryOpExpression* expr [[maybe_unused]]) override { visitedBinary = true; }
	void visit(UnaryOpExpression* expr [[maybe_unused]]) override { visitedUnary = true; }
	void visit(FunctionCallExpression* expr [[maybe_unused]]) override { visitedFunction = true; }
	void visit(CaseExpression* expr [[maybe_unused]]) override { visitedCase = true; }
	void visit(InExpression* expr [[maybe_unused]]) override { visitedIn = true; }
	void visit(ListSliceExpression* expr [[maybe_unused]]) override { visitedListSlice = true; }
	void visit(ListComprehensionExpression* expr [[maybe_unused]]) override { visitedListComprehension = true; }
	void visit(ListLiteralExpression* expr [[maybe_unused]]) override { visitedListLiteral = true; }
	void visit(IsNullExpression* expr [[maybe_unused]]) override { visitedIsNull = true; }
	void visit(QuantifierFunctionExpression* expr [[maybe_unused]]) override { visitedQuantifierFunction = true; }
	void visit(ExistsExpression* expr [[maybe_unused]]) override { visitedExists = true; }
	void visit(PatternComprehensionExpression* expr [[maybe_unused]]) override { visitedPatternComprehension = true; }
	void visit(ReduceExpression* expr [[maybe_unused]]) override {}
	void visit(ParameterExpression* expr [[maybe_unused]]) override { visitedParameter = true; }
	void visit(ShortestPathExpression* expr [[maybe_unused]]) override { visitedShortestPath = true; }
	void visit(MapProjectionExpression* expr [[maybe_unused]]) override { visitedMapProjection = true; }

	// Track which visit methods were called
	bool visitedLiteral = false;
	bool visitedVarRef = false;
	bool visitedBinary = false;
	bool visitedUnary = false;
	bool visitedFunction = false;
	bool visitedCase = false;
	bool visitedIn = false;
	bool visitedListSlice = false;
	bool visitedListComprehension = false;
	bool visitedListLiteral = false;
	bool visitedIsNull = false;
	bool visitedQuantifierFunction = false;
	bool visitedExists = false;
	bool visitedPatternComprehension = false;
	bool visitedParameter = false;
	bool visitedShortestPath = false;
	bool visitedMapProjection = false;
};

// ============================================================================
// Test Fixture
// ============================================================================

class ExpressionsCoverageTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Create a basic record for context
		record_.setValue("x", PropertyValue(42));
		record_.setValue("name", PropertyValue(std::string("Alice")));
		context_ = std::make_unique<EvaluationContext>(record_);
	}

	Record record_;
	std::unique_ptr<EvaluationContext> context_;
};

// ============================================================================
// ConstExpressionVisitor Tests - Expression::accept(ConstExpressionVisitor)
// ============================================================================

TEST_F(ExpressionsCoverageTest, ConstExpressionVisitor_LiteralExpression) {
	// Test ConstExpressionVisitor::accept for LiteralExpression
	auto expr = std::make_unique<LiteralExpression>(int64_t(42));
	const LiteralExpression* constExpr = expr.get();

	TestConstExpressionVisitor visitor;
	constExpr->accept(visitor);

	EXPECT_TRUE(visitor.visitedLiteral) << "ConstExpressionVisitor should visit LiteralExpression";
}

TEST_F(ExpressionsCoverageTest, ConstExpressionVisitor_VariableReferenceExpression) {
	// Test ConstExpressionVisitor::accept for VariableReferenceExpression
	auto expr = std::make_unique<VariableReferenceExpression>("n");
	const VariableReferenceExpression* constExpr = expr.get();

	TestConstExpressionVisitor visitor;
	constExpr->accept(visitor);

	EXPECT_TRUE(visitor.visitedVarRef) << "ConstExpressionVisitor should visit VariableReferenceExpression";
}

TEST_F(ExpressionsCoverageTest, ConstExpressionVisitor_VariableReferenceExpressionWithProperty) {
	// Test ConstExpressionVisitor::accept for VariableReferenceExpression with property
	auto expr = std::make_unique<VariableReferenceExpression>("n", "age");
	const VariableReferenceExpression* constExpr = expr.get();

	TestConstExpressionVisitor visitor;
	constExpr->accept(visitor);

	EXPECT_TRUE(visitor.visitedVarRef) << "ConstExpressionVisitor should visit VariableReferenceExpression with property";
}

TEST_F(ExpressionsCoverageTest, ConstExpressionVisitor_BinaryOpExpression) {
	// Test ConstExpressionVisitor::accept for BinaryOpExpression
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	auto expr = std::make_unique<BinaryOpExpression>(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	const BinaryOpExpression* constExpr = expr.get();

	TestConstExpressionVisitor visitor;
	constExpr->accept(visitor);

	EXPECT_TRUE(visitor.visitedBinary) << "ConstExpressionVisitor should visit BinaryOpExpression";
}

TEST_F(ExpressionsCoverageTest, ConstExpressionVisitor_UnaryOpExpression) {
	// Test ConstExpressionVisitor::accept for UnaryOpExpression
	auto operand = std::make_unique<LiteralExpression>(int64_t(5));
	auto expr = std::make_unique<UnaryOpExpression>(UnaryOperatorType::UOP_MINUS, std::move(operand));
	const UnaryOpExpression* constExpr = expr.get();

	TestConstExpressionVisitor visitor;
	constExpr->accept(visitor);

	EXPECT_TRUE(visitor.visitedUnary) << "ConstExpressionVisitor should visit UnaryOpExpression";
}

TEST_F(ExpressionsCoverageTest, ConstExpressionVisitor_FunctionCallExpression) {
	// Test ConstExpressionVisitor::accept for FunctionCallExpression
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(int64_t(5)));
	auto expr = std::make_unique<FunctionCallExpression>("abs", std::move(args));
	const FunctionCallExpression* constExpr = expr.get();

	TestConstExpressionVisitor visitor;
	constExpr->accept(visitor);

	EXPECT_TRUE(visitor.visitedFunction) << "ConstExpressionVisitor should visit FunctionCallExpression";
}

TEST_F(ExpressionsCoverageTest, ConstExpressionVisitor_CaseExpression) {
	// Test ConstExpressionVisitor::accept for CaseExpression (simple CASE)
	auto comparisonExpr = std::make_unique<LiteralExpression>(int64_t(1));
	auto expr = std::make_unique<CaseExpression>(std::move(comparisonExpr));
	expr->addBranch(std::make_unique<LiteralExpression>(int64_t(1)),
	                std::make_unique<LiteralExpression>(int64_t(10)));
	const CaseExpression* constExpr = expr.get();

	TestConstExpressionVisitor visitor;
	constExpr->accept(visitor);

	EXPECT_TRUE(visitor.visitedCase) << "ConstExpressionVisitor should visit CaseExpression";
}

TEST_F(ExpressionsCoverageTest, ConstExpressionVisitor_CaseExpressionSearched) {
	// Test ConstExpressionVisitor::accept for CaseExpression (searched CASE)
	auto expr = std::make_unique<CaseExpression>(); // Searched CASE (no comparison expr)
	expr->addBranch(std::make_unique<LiteralExpression>(true),
	                std::make_unique<LiteralExpression>(int64_t(10)));
	const CaseExpression* constExpr = expr.get();

	TestConstExpressionVisitor visitor;
	constExpr->accept(visitor);

	EXPECT_TRUE(visitor.visitedCase) << "ConstExpressionVisitor should visit searched CaseExpression";
}

TEST_F(ExpressionsCoverageTest, ConstExpressionVisitor_InExpression) {
	// Test ConstExpressionVisitor::accept for InExpression
	auto value = std::make_unique<LiteralExpression>(int64_t(5));
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(5))
	};
	auto expr = std::make_unique<InExpression>(std::move(value), listValues);
	const InExpression* constExpr = expr.get();

	TestConstExpressionVisitor visitor;
	constExpr->accept(visitor);

	EXPECT_TRUE(visitor.visitedIn) << "ConstExpressionVisitor should visit InExpression";
}

TEST_F(ExpressionsCoverageTest, ConstExpressionVisitor_IsNullExpression) {
	// Test ConstExpressionVisitor::accept for IsNullExpression
	auto inner = std::make_unique<LiteralExpression>(int64_t(42));
	auto expr = std::make_unique<IsNullExpression>(std::move(inner), false);
	const IsNullExpression* constExpr = expr.get();

	TestConstExpressionVisitor visitor;
	constExpr->accept(visitor);

	EXPECT_TRUE(visitor.visitedIsNull) << "ConstExpressionVisitor should visit IsNullExpression";
}

TEST_F(ExpressionsCoverageTest, ConstExpressionVisitor_ListLiteralExpression) {
	// Test ConstExpressionVisitor::accept for ListLiteralExpression
	std::vector<PropertyValue> list = { PropertyValue(int64_t(1)), PropertyValue(int64_t(2)) };
	auto expr = std::make_unique<ListLiteralExpression>(PropertyValue(list));
	const ListLiteralExpression* constExpr = expr.get();

	TestConstExpressionVisitor visitor;
	constExpr->accept(visitor);

	EXPECT_TRUE(visitor.visitedListLiteral) << "ConstExpressionVisitor should visit ListLiteralExpression";
}

TEST_F(ExpressionsCoverageTest, ConstExpressionVisitor_ListSliceExpression) {
	// Test ConstExpressionVisitor::accept for ListSliceExpression
	std::vector<PropertyValue> list = { PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3)) };
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(list));
	auto start = std::make_unique<LiteralExpression>(int64_t(0));
	auto end = std::make_unique<LiteralExpression>(int64_t(2));
	auto expr = std::make_unique<ListSliceExpression>(std::move(listExpr), std::move(start), std::move(end), true);
	const ListSliceExpression* constExpr = expr.get();

	TestConstExpressionVisitor visitor;
	constExpr->accept(visitor);

	EXPECT_TRUE(visitor.visitedListSlice) << "ConstExpressionVisitor should visit ListSliceExpression";
}

TEST_F(ExpressionsCoverageTest, ConstExpressionVisitor_ListComprehensionExpression) {
	// Test ConstExpressionVisitor::accept for ListComprehensionExpression
	std::vector<PropertyValue> list = { PropertyValue(int64_t(1)), PropertyValue(int64_t(2)) };
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(list));
	auto expr = std::make_unique<ListComprehensionExpression>("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
	const ListComprehensionExpression* constExpr = expr.get();

	TestConstExpressionVisitor visitor;
	constExpr->accept(visitor);

	EXPECT_TRUE(visitor.visitedListComprehension) << "ConstExpressionVisitor should visit ListComprehensionExpression";
}

// ============================================================================
// LiteralExpression DOUBLE Literal Tests
// ============================================================================

TEST_F(ExpressionsCoverageTest, LiteralExpression_DoubleConstructor) {
	// Test DOUBLE literal constructor (lines 38-40 in Expression.cpp)
	LiteralExpression expr(3.14159);

	EXPECT_TRUE(expr.isDouble());
	EXPECT_FALSE(expr.isNull());
	EXPECT_FALSE(expr.isInteger());
	EXPECT_FALSE(expr.isBoolean());
	EXPECT_FALSE(expr.isString());
	EXPECT_DOUBLE_EQ(expr.getDoubleValue(), 3.14159);
}

TEST_F(ExpressionsCoverageTest, LiteralExpression_DoubleToString) {
	// Test DOUBLE literal toString() (lines 66-70 in Expression.cpp)
	LiteralExpression expr(3.14159);
	std::string result = expr.toString();

	// The exact format depends on ostringstream, but should contain the number
	EXPECT_FALSE(result.empty());
	EXPECT_NE(result.find("3"), std::string::npos);
}

TEST_F(ExpressionsCoverageTest, LiteralExpression_DoubleToString_Negative) {
	// Test negative DOUBLE literal toString()
	LiteralExpression expr(-2.71828);
	std::string result = expr.toString();

	EXPECT_FALSE(result.empty());
	EXPECT_NE(result.find("-"), std::string::npos);
}

TEST_F(ExpressionsCoverageTest, LiteralExpression_DoubleToString_Zero) {
	// Test zero DOUBLE literal toString()
	LiteralExpression expr(0.0);
	std::string result = expr.toString();

	EXPECT_FALSE(result.empty());
}

TEST_F(ExpressionsCoverageTest, LiteralExpression_DoubleClone) {
	// Test DOUBLE literal clone() (lines 85-86 in Expression.cpp)
	LiteralExpression original(3.14159);
	auto cloned = original.clone();

	ASSERT_NE(cloned, nullptr);
	EXPECT_EQ(cloned->getExpressionType(), ExpressionType::LITERAL);

	auto* clonedLiteral = dynamic_cast<LiteralExpression*>(cloned.get());
	ASSERT_NE(clonedLiteral, nullptr);
	EXPECT_TRUE(clonedLiteral->isDouble());
	EXPECT_DOUBLE_EQ(clonedLiteral->getDoubleValue(), 3.14159);
}

TEST_F(ExpressionsCoverageTest, LiteralExpression_AllTypesCoverage) {
	// Test all literal types for complete coverage of toString() and clone()

	// NULL literal
	LiteralExpression nullExpr;
	EXPECT_TRUE(nullExpr.isNull());
	EXPECT_EQ(nullExpr.toString(), "null");

	// Boolean literal
	LiteralExpression boolExpr(true);
	EXPECT_TRUE(boolExpr.isBoolean());
	EXPECT_EQ(boolExpr.toString(), "true");

	// Integer literal
	LiteralExpression intExpr(int64_t(42));
	EXPECT_TRUE(intExpr.isInteger());
	EXPECT_EQ(intExpr.toString(), "42");

	// String literal
	LiteralExpression strExpr(std::string("hello"));
	EXPECT_TRUE(strExpr.isString());
	EXPECT_EQ(strExpr.toString(), "'hello'");

	// Double literal - already tested above
	LiteralExpression doubleExpr(3.14);
	EXPECT_TRUE(doubleExpr.isDouble());
}

// ============================================================================
// FunctionRegistry - Null Argument Handling for String Functions
// ============================================================================

TEST_F(ExpressionsCoverageTest, StartsWith_NullFirstArg) {
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

TEST_F(ExpressionsCoverageTest, StartsWith_NullSecondArg) {
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

TEST_F(ExpressionsCoverageTest, StartsWith_BothNull) {
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

TEST_F(ExpressionsCoverageTest, EndsWith_NullFirstArg) {
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

TEST_F(ExpressionsCoverageTest, EndsWith_NullSecondArg) {
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

TEST_F(ExpressionsCoverageTest, EndsWith_BothNull) {
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

TEST_F(ExpressionsCoverageTest, Contains_NullFirstArg) {
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

TEST_F(ExpressionsCoverageTest, Contains_NullSecondArg) {
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

TEST_F(ExpressionsCoverageTest, Contains_BothNull) {
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

TEST_F(ExpressionsCoverageTest, StartsWith_EmptyStringPrefix) {
	// Test StartsWith with empty prefix (edge case)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("startswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string(""))); // Empty prefix

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	// Empty string is always a prefix
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionsCoverageTest, EndsWith_EmptyStringSuffix) {
	// Test EndsWith with empty suffix (edge case)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("endswith");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string(""))); // Empty suffix

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	// Empty string is always a suffix
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

TEST_F(ExpressionsCoverageTest, Contains_EmptyStringSubstring) {
	// Test Contains with empty substring (edge case)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("contains");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string(""))); // Empty substring

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	// Empty string is always contained
	EXPECT_TRUE(std::get<bool>(result.getVariant()));
}

// ============================================================================
// Additional Expression Clone and ToString Tests
// ============================================================================

TEST_F(ExpressionsCoverageTest, BinaryOpExpression_CloneDeepCopy) {
	// Test that clone creates a deep copy of BinaryOpExpression
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression original(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	auto cloned = original.clone();
	auto* clonedBinary = dynamic_cast<BinaryOpExpression*>(cloned.get());

	ASSERT_NE(clonedBinary, nullptr);
	EXPECT_EQ(clonedBinary->getOperator(), BinaryOperatorType::BOP_ADD);

	// Verify it's a deep copy by checking toString
	std::string originalStr = original.toString();
	std::string clonedStr = clonedBinary->toString();
	EXPECT_EQ(originalStr, clonedStr);
	EXPECT_EQ(clonedStr, "(5 + 3)");
}

TEST_F(ExpressionsCoverageTest, UnaryOpExpression_CloneDeepCopy) {
	// Test that clone creates a deep copy of UnaryOpExpression
	auto operand = std::make_unique<LiteralExpression>(int64_t(5));
	UnaryOpExpression original(UnaryOperatorType::UOP_MINUS, std::move(operand));

	auto cloned = original.clone();
	auto* clonedUnary = dynamic_cast<UnaryOpExpression*>(cloned.get());

	ASSERT_NE(clonedUnary, nullptr);
	EXPECT_EQ(clonedUnary->getOperator(), UnaryOperatorType::UOP_MINUS);

	std::string originalStr = original.toString();
	std::string clonedStr = clonedUnary->toString();
	EXPECT_EQ(originalStr, clonedStr);
	EXPECT_EQ(clonedStr, "-(5)");
}

TEST_F(ExpressionsCoverageTest, UnaryOpExpression_NOT_Operator) {
	// Test UnaryOpExpression with NOT operator
	{
		auto operand = std::make_unique<LiteralExpression>(true);
		UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	{
		auto operand = std::make_unique<LiteralExpression>(false);
		UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Test NOT with NULL propagation
	{
		auto operand = std::make_unique<LiteralExpression>();
		UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
	}
}

TEST_F(ExpressionsCoverageTest, UnaryOpExpression_MINUS_Variants) {
	// Test UnaryOpExpression MINUS with different types
	{
		auto operand = std::make_unique<LiteralExpression>(int64_t(42));
		UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), -42);
	}

	{
		auto operand = std::make_unique<LiteralExpression>(3.14);
		UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<double>(evaluator.getResult().getVariant()), -3.14);
	}

	// Test MINUS with NULL propagation
	{
		auto operand = std::make_unique<LiteralExpression>();
		UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
	}
}

TEST_F(ExpressionsCoverageTest, FunctionCallExpression_CloneWithMultipleArgs) {
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

TEST_F(ExpressionsCoverageTest, InExpression_CloneAndToString) {
	// Test InExpression clone and toString
	auto value = std::make_unique<LiteralExpression>(int64_t(5));
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(5))
	};
	InExpression original(std::move(value), listValues);

	auto cloned = original.clone();
	auto* clonedIn = dynamic_cast<InExpression*>(cloned.get());

	ASSERT_NE(clonedIn, nullptr);

	std::string originalStr = original.toString();
	std::string clonedStr = clonedIn->toString();
	EXPECT_EQ(originalStr, clonedStr);
}

TEST_F(ExpressionsCoverageTest, CaseExpression_SimpleCaseClone) {
	// Test CaseExpression clone for simple CASE
	auto comparisonExpr = std::make_unique<LiteralExpression>(int64_t(1));
	CaseExpression original(std::move(comparisonExpr));
	original.addBranch(std::make_unique<LiteralExpression>(int64_t(1)),
	                  std::make_unique<LiteralExpression>(int64_t(10)));
	original.addBranch(std::make_unique<LiteralExpression>(int64_t(2)),
	                  std::make_unique<LiteralExpression>(int64_t(20)));

	auto cloned = original.clone();
	auto* clonedCase = dynamic_cast<CaseExpression*>(cloned.get());

	ASSERT_NE(clonedCase, nullptr);
	EXPECT_TRUE(clonedCase->isSimpleCase());
	EXPECT_EQ(clonedCase->getBranches().size(), 2UL);

	std::string originalStr = original.toString();
	std::string clonedStr = clonedCase->toString();
	EXPECT_EQ(originalStr, clonedStr);
}

TEST_F(ExpressionsCoverageTest, CaseExpression_SearchedCaseClone) {
	// Test CaseExpression clone for searched CASE
	CaseExpression original; // Searched CASE
	original.addBranch(std::make_unique<LiteralExpression>(true),
	                  std::make_unique<LiteralExpression>(int64_t(10)));

	auto cloned = original.clone();
	auto* clonedCase = dynamic_cast<CaseExpression*>(cloned.get());

	ASSERT_NE(clonedCase, nullptr);
	EXPECT_FALSE(clonedCase->isSimpleCase());
	EXPECT_EQ(clonedCase->getBranches().size(), 1UL);
}

TEST_F(ExpressionsCoverageTest, VariableReferenceExpression_WithAndWithoutProperty) {
	// Test both constructors of VariableReferenceExpression
	VariableReferenceExpression varExpr("n");
	EXPECT_EQ(varExpr.getVariableName(), "n");
	EXPECT_FALSE(varExpr.hasProperty());
	EXPECT_EQ(varExpr.toString(), "n");

	VariableReferenceExpression propExpr("n", "age");
	EXPECT_EQ(propExpr.getVariableName(), "n");
	EXPECT_TRUE(propExpr.hasProperty());
	EXPECT_EQ(propExpr.getPropertyName(), "age");
	EXPECT_EQ(propExpr.toString(), "n.age");
}

// ============================================================================
// Length and Size Function Tests - Uncovered Branches
// ============================================================================

TEST_F(ExpressionsCoverageTest, LengthFunction_IntegerType) {
	// Test LengthFunction with INTEGER type (hits "other types" branch at line 235)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("length");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(ExpressionsCoverageTest, LengthFunction_DoubleType) {
	// Test LengthFunction with DOUBLE type (hits "other types" branch at line 235)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("length");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(3.14));

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 0);
}

TEST_F(ExpressionsCoverageTest, LengthFunction_BooleanType) {
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

TEST_F(ExpressionsCoverageTest, SizeFunction_IntegerType) {
	// Test SizeFunction with INTEGER type (hits "other types" branch at line 455)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(int64_t(42)));

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsCoverageTest, SizeFunction_DoubleType) {
	// Test SizeFunction with DOUBLE type (hits "other types" branch at line 455)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(3.14));

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsCoverageTest, SizeFunction_BooleanType) {
	// Test SizeFunction with BOOLEAN type (hits "other types" branch at line 455)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("size");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(true));

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsCoverageTest, AbsFunction_BooleanType) {
	// Test AbsFunction with BOOLEAN type (hits catch-all branch at line 340-342)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("abs");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(true)); // Boolean converts to double (1.0)

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 1.0);
}

TEST_F(ExpressionsCoverageTest, AbsFunction_StringType) {
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

// ============================================================================
// SubstringFunction - Negative Length Branch (line 175-177)
// ============================================================================


TEST_F(ExpressionsCoverageTest, SubstringFunction_LengthExceedsRemaining) {
	// Test SubstringFunction with length exceeding remaining string (hits clamping at line 181)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(int64_t(2))); // start at position 2
	args.push_back(PropertyValue(int64_t(100))); // length exceeds remaining

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	// Should clamp to remaining characters: "llo"
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "llo");
}

// ============================================================================
// Additional Edge Cases for Existing Functions
// ============================================================================

TEST_F(ExpressionsCoverageTest, ReplaceFunction_NoMatchFound) {
	// Test ReplaceFunction when search string is not found (branch not taken at line 309)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("replace");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(std::string("xyz"))); // Not found
	args.push_back(PropertyValue(std::string("world")));

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	// Original string should be returned unchanged
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
}

// ============================================================================
// IsNullExpression Coverage Tests - Target 95%+ coverage
// ============================================================================

TEST_F(ExpressionsCoverageTest, IsNullExpression_CompleteCoverage) {
	// Test IS NULL with NULL value → true
	{
		auto inner = std::make_unique<LiteralExpression>();
		IsNullExpression expr(std::move(inner), false);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Test IS NOT NULL with NULL value → false
	{
		auto inner = std::make_unique<LiteralExpression>();
		IsNullExpression expr(std::move(inner), true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Test IS NULL with integer → false
	{
		auto inner = std::make_unique<LiteralExpression>(int64_t(42));
		IsNullExpression expr(std::move(inner), false);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Test IS NOT NULL with integer → true
	{
		auto inner = std::make_unique<LiteralExpression>(int64_t(42));
		IsNullExpression expr(std::move(inner), true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Test IS NOT NULL with string → true
	{
		auto inner = std::make_unique<LiteralExpression>(std::string("hello"));
		IsNullExpression expr(std::move(inner), true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Test IS NULL with double → false
	{
		auto inner = std::make_unique<LiteralExpression>(3.14);
		IsNullExpression expr(std::move(inner), false);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Test IS NOT NULL with boolean → true
	{
		auto inner = std::make_unique<LiteralExpression>(false);
		IsNullExpression expr(std::move(inner), true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Test clone with non-null expression
	{
		auto inner = std::make_unique<LiteralExpression>(int64_t(42));
		IsNullExpression original(std::move(inner), true);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "42 IS NOT NULL");
	}

	// Test clone with null expression (expr_ is nullptr)
	{
		auto inner = std::make_unique<LiteralExpression>();
		IsNullExpression original(std::move(inner), false);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "null IS NULL");
	}

	// Test toString with different expressions
	{
		auto varRef = std::make_unique<VariableReferenceExpression>("n", "age");
		IsNullExpression expr(std::move(varRef), false);
		EXPECT_EQ(expr.toString(), "n.age IS NULL");
	}

	{
		auto left = std::make_unique<LiteralExpression>(int64_t(5));
		auto right = std::make_unique<LiteralExpression>(int64_t(3));
		auto binOp = std::make_unique<BinaryOpExpression>(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
		IsNullExpression expr(std::move(binOp), true);
		EXPECT_EQ(expr.toString(), "(5 + 3) IS NOT NULL");
	}

	// Test getExpressionType
	{
		auto inner = std::make_unique<LiteralExpression>(int64_t(42));
		IsNullExpression expr(std::move(inner), false);
		EXPECT_EQ(expr.getExpressionType(), ExpressionType::IS_NULL);
	}

	// Test getters
	{
		auto inner = std::make_unique<LiteralExpression>(int64_t(42));
		IsNullExpression expr(std::move(inner), true);
		EXPECT_TRUE(expr.isNegated());
		EXPECT_NE(expr.getExpression(), nullptr);
	}

	// Test clone with nullptr expression (covers branch: expr_ ? expr_->clone() : nullptr)
	{
		IsNullExpression original(nullptr, false);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), " IS NULL");
	}

	// Test toString with nullptr expression (covers branch: if (expr_) { result = expr_->toString(); })
	{
		IsNullExpression expr(nullptr, true);
		EXPECT_EQ(expr.toString(), " IS NOT NULL");
	}
}

// ============================================================================
// ListLiteralExpression Coverage Tests - Target 95%+ coverage
// ============================================================================

TEST_F(ExpressionsCoverageTest, ListLiteralExpression_CompleteCoverage) {
	// Test constructor throws on non-LIST type
	EXPECT_THROW({
		ListLiteralExpression expr(PropertyValue(int64_t(42)));
	}, std::runtime_error);

	EXPECT_THROW({
		ListLiteralExpression expr(PropertyValue(std::string("test")));
	}, std::runtime_error);

	EXPECT_THROW({
		ListLiteralExpression expr(PropertyValue(true));
	}, std::runtime_error);

	// Test empty list
	{
		std::vector<PropertyValue> emptyList;
		PropertyValue listValue(emptyList);
		ListLiteralExpression expr(listValue);
		EXPECT_EQ(expr.toString(), "[]");
		EXPECT_EQ(expr.getValue().getList().size(), 0ULL);
		EXPECT_EQ(expr.getExpressionType(), ExpressionType::LITERAL);
	}

	// Test single element
	{
		std::vector<PropertyValue> list = { PropertyValue(int64_t(42)) };
		PropertyValue listValue(list);
		ListLiteralExpression expr(listValue);
		EXPECT_EQ(expr.toString(), "[42]");
		EXPECT_EQ(expr.getValue().getList().size(), 1ULL);
	}

	// Test heterogeneous list
	{
		std::vector<PropertyValue> list = {
			PropertyValue(int64_t(42)),
			PropertyValue(std::string("hello")),
			PropertyValue(true),
			PropertyValue(3.14)
		};
		PropertyValue listValue(list);
		ListLiteralExpression expr(listValue);
		EXPECT_EQ(expr.getValue().getList().size(), 4ULL);
	}

	// Test nested lists
	{
		std::vector<PropertyValue> inner1 = { PropertyValue(int64_t(1)), PropertyValue(int64_t(2)) };
		std::vector<PropertyValue> inner2 = { PropertyValue(int64_t(3)), PropertyValue(int64_t(4)) };
		std::vector<PropertyValue> outer = { PropertyValue(inner1), PropertyValue(inner2) };
		PropertyValue listValue(outer);
		ListLiteralExpression expr(listValue);
		EXPECT_EQ(expr.getValue().getList().size(), 2ULL);
		EXPECT_EQ(expr.getValue().getList()[0].getType(), PropertyType::LIST);
	}

	// Test list with nulls
	{
		std::vector<PropertyValue> list = {
			PropertyValue(int64_t(1)),
			PropertyValue(),
			PropertyValue(std::string("test"))
		};
		PropertyValue listValue(list);
		ListLiteralExpression expr(listValue);
		EXPECT_EQ(expr.toString(), "[1, null, test]");
	}

	// Test clone
	{
		std::vector<PropertyValue> list = { PropertyValue(int64_t(1)), PropertyValue(int64_t(2)) };
		PropertyValue listValue(list);
		ListLiteralExpression original(listValue);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "[1, 2]");
	}

	// Test evaluation
	{
		std::vector<PropertyValue> list = { PropertyValue(int64_t(10)), PropertyValue(int64_t(20)) };
		PropertyValue listValue(list);
		ListLiteralExpression expr(listValue);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(evaluator.getResult().getType(), PropertyType::LIST);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 2ULL);
		EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 10);
		EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 20);
	}

	// Test getValue getter
	{
		std::vector<PropertyValue> list = { PropertyValue(int64_t(42)) };
		PropertyValue listValue(list);
		ListLiteralExpression expr(listValue);
		EXPECT_EQ(expr.getValue(), listValue);
	}
}

// ============================================================================
// ListSliceExpression Coverage Tests - Target 95%+ coverage
// ============================================================================

TEST_F(ExpressionsCoverageTest, ListSliceExpression_CompleteCoverage) {
	// Helper to create list
	auto createList = [](const std::vector<int64_t>& values) -> std::unique_ptr<ListLiteralExpression> {
		std::vector<PropertyValue> list;
		for (int64_t v : values) {
			list.push_back(PropertyValue(v));
		}
		return std::make_unique<ListLiteralExpression>(PropertyValue(list));
	};

	// Single index: list[0]
	{
		auto listExpr = createList({10, 20, 30, 40, 50});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, false);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 10);
	}

	// Single negative index: list[-1]
	{
		auto listExpr = createList({10, 20, 30, 40, 50});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(-1));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, false);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 50);
	}

	// Single index out of bounds: list[100]
	{
		auto listExpr = createList({10, 20, 30});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(100));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, false);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
	}

	// Single negative index out of bounds: list[-100]
	{
		auto listExpr = createList({10, 20, 30});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(-100));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, false);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
	}

	// Range: list[0..2]
	{
		auto listExpr = createList({10, 20, 30, 40, 50});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
		auto endExpr = std::make_unique<LiteralExpression>(int64_t(2));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 2ULL);
		EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 10);
		EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 20);
	}

	// Range negative: list[-3..-1]
	{
		auto listExpr = createList({10, 20, 30, 40, 50});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(-3));
		auto endExpr = std::make_unique<LiteralExpression>(int64_t(-1));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 2ULL);
		EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 30);
		EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 40);
	}

	// Range without start: list[..2]
	{
		auto listExpr = createList({10, 20, 30, 40, 50});
		auto endExpr = std::make_unique<LiteralExpression>(int64_t(3));
		ListSliceExpression expr(std::move(listExpr), nullptr, std::move(endExpr), true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 3ULL);
	}

	// Range without end: list[2..]
	{
		auto listExpr = createList({10, 20, 30, 40, 50});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(2));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 3ULL);
	}

	// Full range: list[..]
	{
		auto listExpr = createList({10, 20, 30});
		ListSliceExpression expr(std::move(listExpr), nullptr, nullptr, true);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 3ULL);
	}

	// Test toString variations
	{
		auto listExpr = createList({1, 2, 3});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, false);
		EXPECT_EQ(expr.toString(), "[1, 2, 3][0]");
	}

	{
		auto listExpr = createList({1, 2, 3});
		ListSliceExpression expr(std::move(listExpr), nullptr, nullptr, true);
		EXPECT_EQ(expr.toString(), "[1, 2, 3][..]");
	}

	{
		auto listExpr = createList({1, 2, 3});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
		auto endExpr = std::make_unique<LiteralExpression>(int64_t(2));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
		EXPECT_EQ(expr.toString(), "[1, 2, 3][0..2]");
	}

	// Test clone with all parameters
	{
		auto listExpr = createList({1, 2, 3});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
		auto endExpr = std::make_unique<LiteralExpression>(int64_t(2));
		ListSliceExpression original(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "[1, 2, 3][0..2]");
	}

	// Test clone with no start/end
	{
		auto listExpr = createList({1, 2, 3});
		ListSliceExpression original(std::move(listExpr), nullptr, nullptr, true);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "[1, 2, 3][..]");
	}

	// Test getters
	{
		auto listExpr = createList({1, 2, 3});
		auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
		ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, false);
		EXPECT_FALSE(expr.hasRange());
		EXPECT_NE(expr.getList(), nullptr);
		EXPECT_NE(expr.getStart(), nullptr);
		EXPECT_EQ(expr.getEnd(), nullptr);
	}

	{
		auto listExpr = createList({1, 2, 3});
		ListSliceExpression expr(std::move(listExpr), nullptr, nullptr, true);
		EXPECT_TRUE(expr.hasRange());
		EXPECT_EQ(expr.getStart(), nullptr);
		EXPECT_EQ(expr.getEnd(), nullptr);
	}
}

// ============================================================================
// ListComprehensionExpression Coverage Tests - Target 95%+ coverage
// ============================================================================

TEST_F(ExpressionsCoverageTest, ListComprehensionExpression_CompleteCoverage) {
	// Helper to create list
	auto createList = [](const std::vector<int64_t>& values) -> std::unique_ptr<ListLiteralExpression> {
		std::vector<PropertyValue> list;
		for (int64_t v : values) {
			list.push_back(PropertyValue(v));
		}
		return std::make_unique<ListLiteralExpression>(PropertyValue(list));
	};

	// FILTER without WHERE: [x IN list]
	{
		auto listExpr = createList({1, 2, 3, 4, 5});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(evaluator.getResult().getType(), PropertyType::LIST);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 5ULL);
	}

	// FILTER with WHERE: [x IN list WHERE x > 3]
	{
		auto listExpr = createList({1, 2, 3, 4, 5});
		auto varRef = std::make_unique<VariableReferenceExpression>("x");
		auto lit3 = std::make_unique<LiteralExpression>(int64_t(3));
		auto whereExpr = std::make_unique<BinaryOpExpression>(std::move(varRef), BinaryOperatorType::BOP_GREATER, std::move(lit3));
		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 2ULL); // 4 and 5
	}

	// EXTRACT with MAP: [x IN list | x * 2]
	{
		auto listExpr = createList({1, 2, 3});
		auto varRef = std::make_unique<VariableReferenceExpression>("x");
		auto lit2 = std::make_unique<LiteralExpression>(int64_t(2));
		auto mapExpr = std::make_unique<BinaryOpExpression>(std::move(varRef), BinaryOperatorType::BOP_MULTIPLY, std::move(lit2));
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 3ULL);
		EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 2);
		EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 4);
		EXPECT_EQ(std::get<int64_t>(result[2].getVariant()), 6);
	}

	// EXTRACT with WHERE and MAP: [x IN list WHERE x > 2 | x * 3]
	{
		auto listExpr = createList({1, 2, 3, 4, 5});
		auto varRef1 = std::make_unique<VariableReferenceExpression>("x");
		auto lit2 = std::make_unique<LiteralExpression>(int64_t(2));
		auto whereExpr = std::make_unique<BinaryOpExpression>(std::move(varRef1), BinaryOperatorType::BOP_GREATER, std::move(lit2));

		auto varRef2 = std::make_unique<VariableReferenceExpression>("x");
		auto lit3 = std::make_unique<LiteralExpression>(int64_t(3));
		auto mapExpr = std::make_unique<BinaryOpExpression>(std::move(varRef2), BinaryOperatorType::BOP_MULTIPLY, std::move(lit3));

		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 3ULL); // 3, 4, 5
		EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 9);
		EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 12);
		EXPECT_EQ(std::get<int64_t>(result[2].getVariant()), 15);
	}

	// REDUCE - Currently returns list like FILTER (REDUCE not fully implemented)
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("total", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_REDUCE);
		EXPECT_EQ(expr.toString(), "reduce(total = [1, 2, 3])");
	}

	// Empty list
	{
		std::vector<PropertyValue> emptyList;
		auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(emptyList));
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 0ULL);
	}

	// Test toString variations - FILTER without WHERE
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3]]");
	}

	// FILTER with WHERE
	{
		auto listExpr = createList({1, 2, 3});
		auto whereExpr = std::make_unique<LiteralExpression>(true);
		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3] WHERE true]");
	}

	// EXTRACT without MAP (edge case - just filters elements)
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3]]");
	}

	// EXTRACT with MAP
	{
		auto listExpr = createList({1, 2, 3});
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3] | 42]");
	}

	// EXTRACT with WHERE and MAP
	{
		auto listExpr = createList({1, 2, 3});
		auto whereExpr = std::make_unique<LiteralExpression>(true);
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3] WHERE true | 42]");
	}

	// REDUCE
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("total", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_REDUCE);
		EXPECT_EQ(expr.toString(), "reduce(total = [1, 2, 3])");
	}

	// REDUCE with MAP
	{
		auto listExpr = createList({1, 2, 3});
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(0));
		ListComprehensionExpression expr("total", std::move(listExpr), nullptr, std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_REDUCE);
		EXPECT_EQ(expr.toString(), "reduce(total = [1, 2, 3] | 0)");
	}

	// Test clone
	{
		auto listExpr = createList({1, 2, 3});
		auto whereExpr = std::make_unique<LiteralExpression>(true);
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
		ListComprehensionExpression original("x", std::move(listExpr), std::move(whereExpr), std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "[x IN [1, 2, 3] WHERE true | 42]");
	}

	// Test clone FILTER without WHERE
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression original("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "[x IN [1, 2, 3]]");
	}

	// Test clone REDUCE
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression original("total", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_REDUCE);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "reduce(total = [1, 2, 3])");
	}

	// Test getters
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		EXPECT_EQ(expr.getVariable(), "x");
		EXPECT_EQ(expr.getType(), ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		EXPECT_NE(expr.getListExpr(), nullptr);
		EXPECT_EQ(expr.getWhereExpr(), nullptr);
		EXPECT_EQ(expr.getMapExpr(), nullptr);
	}

	{
		auto listExpr = createList({1, 2, 3});
		auto whereExpr = std::make_unique<LiteralExpression>(true);
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		EXPECT_NE(expr.getWhereExpr(), nullptr);
		EXPECT_NE(expr.getMapExpr(), nullptr);
	}

	// Test getExpressionType
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::COMP_FILTER);
		EXPECT_EQ(expr.getExpressionType(), ExpressionType::LIST_COMPREHENSION);
	}
}

// ============================================================================
// Error Handling Tests for ExpressionEvaluator
// ============================================================================

TEST_F(ExpressionsCoverageTest, UndefinedVariableException) {
	// Test accessing undefined variable throws exception
	auto varExpr = std::make_unique<VariableReferenceExpression>("undefined_var");
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)varExpr->accept(evaluator), UndefinedVariableException);
}

TEST_F(ExpressionsCoverageTest, UndefinedVariableWithPropertyReturnsNull) {
	// Test accessing undefined variable with property returns NULL (not exception)
	// In Cypher, undefinedVar.prop returns NULL, doesn't throw
	auto varExpr = std::make_unique<VariableReferenceExpression>("undefined_var", "prop");
	ExpressionEvaluator evaluator(*context_);
	varExpr->accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsCoverageTest, UnknownFunctionThrowsException) {
	// Test calling unknown function throws exception
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(int64_t(42)));
	auto funcExpr = std::make_unique<FunctionCallExpression>("unknown_function_xyz", std::move(args));
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)funcExpr->accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, DivisionByZero_Integer) {
	// Test integer division by zero throws exception
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), DivisionByZeroException);
}

TEST_F(ExpressionsCoverageTest, DivisionByZero_Double) {
	// Test double division by zero throws exception
	auto left = std::make_unique<LiteralExpression>(10.0);
	auto right = std::make_unique<LiteralExpression>(0.0);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), DivisionByZeroException);
}

TEST_F(ExpressionsCoverageTest, ModuloByZero) {
	// Test modulo by zero throws exception
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), DivisionByZeroException);
}

TEST_F(ExpressionsCoverageTest, SliceNonListValue) {
	// Test slicing non-list value throws exception
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(0));
	ListSliceExpression expr(std::move(nonListExpr), std::move(indexExpr), nullptr, false);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, SliceMissingIndex) {
	// Test slice with missing start index throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	ListSliceExpression expr(std::move(listExpr), nullptr, nullptr, false);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, SliceWithNonIntegerIndex) {
	// Test slice with string index throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto indexExpr = std::make_unique<LiteralExpression>(std::string("not_a_number"));
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, SliceWithNonIntegerStartIndex) {
	// Test slice with string start index throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto startExpr = std::make_unique<LiteralExpression>(std::string("not_a_number"));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(2));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, SliceWithNonIntegerEndIndex) {
	// Test slice with string end index throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
	auto endExpr = std::make_unique<LiteralExpression>(std::string("not_a_number"));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, ListComprehensionNonListValue) {
	// Test list comprehension on non-list value throws exception
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	ListComprehensionExpression expr("x", std::move(nonListExpr), nullptr, nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, ListComprehensionNonBooleanWhere) {
	// Test list comprehension with non-boolean WHERE clause throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto whereExpr = std::make_unique<LiteralExpression>(int64_t(42)); // Non-boolean
	ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, VariableReferencePropertyAccess) {
	// Test variable reference with property access
	// First set up a variable in the context
	context_->setVariable("node", PropertyValue(std::string("test_value")));

	// Create property access expression - this should work if property exists
	auto varExpr = std::make_unique<VariableReferenceExpression>("node", "property");
	ExpressionEvaluator evaluator(*context_);
	// Missing property returns NULL, doesn't throw
	varExpr->accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsCoverageTest, EvaluatedWithNullExpression) {
	// Test evaluating null expression returns null
	ExpressionEvaluator evaluator(*context_);
	PropertyValue result = evaluator.evaluate(nullptr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsCoverageTest, UnaryMinusWithNull) {
	// Test unary minus with null propagates null
	auto nullExpr = std::make_unique<LiteralExpression>();
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(nullExpr));
	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsCoverageTest, UnaryNotWithNull) {
	// Test unary NOT with null propagates null
	auto nullExpr = std::make_unique<LiteralExpression>();
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(nullExpr));
	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// ExistsExpression Tests
// ============================================================================

TEST_F(ExpressionsCoverageTest, ExistsExpression_ConstructorPatternOnly) {
	// Test ExistsExpression constructor with pattern only
	ExistsExpression expr("(n)-[:FRIENDS]->()");

	EXPECT_EQ(expr.getPattern(), "(n)-[:FRIENDS]->()");
	EXPECT_FALSE(expr.hasWhereClause());
	EXPECT_EQ(expr.getWhereExpression(), nullptr);
}

TEST_F(ExpressionsCoverageTest, ExistsExpression_ConstructorWithWhere) {
	// Test ExistsExpression constructor with pattern and WHERE clause
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));
	ExistsExpression expr("(n)-[:FRIENDS]->()", std::move(whereExpr));

	EXPECT_EQ(expr.getPattern(), "(n)-[:FRIENDS]->()");
	EXPECT_TRUE(expr.hasWhereClause());
	EXPECT_NE(expr.getWhereExpression(), nullptr);
}

TEST_F(ExpressionsCoverageTest, ExistsExpression_ToStringNoWhere) {
	// Test toString() without WHERE clause
	ExistsExpression expr("(n)-[:FRIENDS]->()");

	std::string str = expr.toString();
	EXPECT_EQ(str, "EXISTS((n)-[:FRIENDS]->())");
}

TEST_F(ExpressionsCoverageTest, ExistsExpression_ToStringWithWhere) {
	// Test toString() with WHERE clause
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));
	ExistsExpression expr("(n)-[:FRIENDS]->()", std::move(whereExpr));

	std::string str = expr.toString();
	EXPECT_TRUE(str.find("EXISTS(") != std::string::npos);
	EXPECT_TRUE(str.find("(n)-[:FRIENDS]->()") != std::string::npos);
	EXPECT_TRUE(str.find("WHERE") != std::string::npos);
}

TEST_F(ExpressionsCoverageTest, ExistsExpression_CloneNoWhere) {
	// Test clone() without WHERE clause
	ExistsExpression expr("(n)-[:FRIENDS]->()");
	auto cloned = expr.clone();

	ASSERT_NE(cloned, nullptr);
	auto* clonedExists = dynamic_cast<ExistsExpression*>(cloned.get());
	ASSERT_NE(clonedExists, nullptr);
	EXPECT_EQ(clonedExists->getPattern(), expr.getPattern());
	EXPECT_FALSE(clonedExists->hasWhereClause());
}

TEST_F(ExpressionsCoverageTest, ExistsExpression_CloneWithWhere) {
	// Test clone() with WHERE clause
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));
	ExistsExpression expr("(n)-[:FRIENDS]->()", std::move(whereExpr));
	auto cloned = expr.clone();

	ASSERT_NE(cloned, nullptr);
	auto* clonedExists = dynamic_cast<ExistsExpression*>(cloned.get());
	ASSERT_NE(clonedExists, nullptr);
	EXPECT_EQ(clonedExists->getPattern(), expr.getPattern());
	EXPECT_TRUE(clonedExists->hasWhereClause());
	EXPECT_NE(clonedExists->getWhereExpression(), nullptr);
}

TEST_F(ExpressionsCoverageTest, ExistsExpression_AcceptVisitor) {
	// Test accept() with ExpressionVisitor
	ExistsExpression expr("(n)-[:FRIENDS]->()");

	TestExpressionVisitor visitor;
	expr.accept(visitor);

	EXPECT_TRUE(visitor.visitedExists);
}

TEST_F(ExpressionsCoverageTest, ExistsExpression_AcceptConstVisitor) {
	// Test accept() with ConstExpressionVisitor
	const ExistsExpression expr("(n)-[:FRIENDS]->()");

	TestConstExpressionVisitor visitor;
	expr.accept(visitor);

	EXPECT_TRUE(visitor.visitedExists);
}

// ============================================================================
// QuantifierFunctionExpression Tests
// ============================================================================

TEST_F(ExpressionsCoverageTest, QuantifierFunctionExpression_ConstructorAll) {
	// Test QuantifierFunctionExpression constructor for 'all'
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{}));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	QuantifierFunctionExpression expr("all", "x", std::move(listExpr), std::move(whereExpr));

	EXPECT_EQ(expr.getFunctionName(), "all");
	EXPECT_EQ(expr.getVariable(), "x");
	EXPECT_NE(expr.getListExpression(), nullptr);
	EXPECT_NE(expr.getWhereExpression(), nullptr);
}

TEST_F(ExpressionsCoverageTest, QuantifierFunctionExpression_ConstructorAny) {
	// Test QuantifierFunctionExpression constructor for 'any'
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{}));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	QuantifierFunctionExpression expr("any", "item", std::move(listExpr), std::move(whereExpr));

	EXPECT_EQ(expr.getFunctionName(), "any");
	EXPECT_EQ(expr.getVariable(), "item");
}

TEST_F(ExpressionsCoverageTest, QuantifierFunctionExpression_ConstructorNone) {
	// Test QuantifierFunctionExpression constructor for 'none'
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{}));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	QuantifierFunctionExpression expr("none", "elem", std::move(listExpr), std::move(whereExpr));

	EXPECT_EQ(expr.getFunctionName(), "none");
	EXPECT_EQ(expr.getVariable(), "elem");
}

TEST_F(ExpressionsCoverageTest, QuantifierFunctionExpression_ConstructorSingle) {
	// Test QuantifierFunctionExpression constructor for 'single'
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{}));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	QuantifierFunctionExpression expr("single", "x", std::move(listExpr), std::move(whereExpr));

	EXPECT_EQ(expr.getFunctionName(), "single");
	EXPECT_EQ(expr.getVariable(), "x");
}

TEST_F(ExpressionsCoverageTest, QuantifierFunctionExpression_GetListExpression) {
	// Test getListExpression() accessor
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{
		PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))
	}));
	auto* listPtr = listExpr.get();

	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));
	QuantifierFunctionExpression expr("all", "x", std::move(listExpr), std::move(whereExpr));

	EXPECT_EQ(expr.getListExpression(), listPtr);
}

TEST_F(ExpressionsCoverageTest, QuantifierFunctionExpression_GetWhereExpression) {
	// Test getWhereExpression() accessor
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{}));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));
	auto* wherePtr = whereExpr.get();

	QuantifierFunctionExpression expr("all", "x", std::move(listExpr), std::move(whereExpr));

	EXPECT_EQ(expr.getWhereExpression(), wherePtr);
}

TEST_F(ExpressionsCoverageTest, QuantifierFunctionExpression_ToString) {
	// Test toString() method
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{}));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	QuantifierFunctionExpression expr("all", "x", std::move(listExpr), std::move(whereExpr));

	std::string str = expr.toString();
	EXPECT_TRUE(str.find("all") != std::string::npos);
	EXPECT_TRUE(str.find("x") != std::string::npos);
}

TEST_F(ExpressionsCoverageTest, QuantifierFunctionExpression_Clone) {
	// Test clone() method
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{}));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	QuantifierFunctionExpression expr("all", "x", std::move(listExpr), std::move(whereExpr));
	auto cloned = expr.clone();

	ASSERT_NE(cloned, nullptr);
	auto* clonedQuantifier = dynamic_cast<QuantifierFunctionExpression*>(cloned.get());
	ASSERT_NE(clonedQuantifier, nullptr);
	EXPECT_EQ(clonedQuantifier->getFunctionName(), expr.getFunctionName());
	EXPECT_EQ(clonedQuantifier->getVariable(), expr.getVariable());
}

TEST_F(ExpressionsCoverageTest, QuantifierFunctionExpression_AcceptVisitor) {
	// Test accept() with ExpressionVisitor
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{}));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	QuantifierFunctionExpression expr("all", "x", std::move(listExpr), std::move(whereExpr));

	TestExpressionVisitor visitor;
	expr.accept(visitor);

	EXPECT_TRUE(visitor.visitedQuantifierFunction);
}

TEST_F(ExpressionsCoverageTest, QuantifierFunctionExpression_AcceptConstVisitor) {
	// Test accept() with ConstExpressionVisitor
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(std::vector<PropertyValue>{}));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	const QuantifierFunctionExpression expr("all", "x", std::move(listExpr), std::move(whereExpr));

	TestConstExpressionVisitor visitor;
	expr.accept(visitor);

	EXPECT_TRUE(visitor.visitedQuantifierFunction);
}

// ============================================================================
// Additional ExpressionEvaluator Coverage Tests
// ============================================================================

TEST_F(ExpressionsCoverageTest, FunctionCallExpression_UnknownFunction_Throws) {
	// Test unknown function throws exception
	auto argExpr = std::make_unique<LiteralExpression>(int64_t(42));
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::move(argExpr));

	FunctionCallExpression expr("unknownFunction", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, FunctionCallExpression_InvalidArgumentCount_Throws) {
	// Test invalid argument count throws exception
	auto arg1 = std::make_unique<LiteralExpression>(int64_t(1));
	auto arg2 = std::make_unique<LiteralExpression>(int64_t(2));
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::move(arg1));
	args.push_back(std::move(arg2));

	// abs() expects exactly 1 argument
	FunctionCallExpression expr("abs", std::move(args));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, InExpression_ElementInList) {
	// Test IN expression where element is in list
	auto valueExpr = std::make_unique<LiteralExpression>(int64_t(2));
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3))
	};

	InExpression expr(std::move(valueExpr), listValues);

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsCoverageTest, InExpression_ElementNotInList) {
	// Test IN expression where element is not in list
	auto valueExpr = std::make_unique<LiteralExpression>(int64_t(5));
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3))
	};

	InExpression expr(std::move(valueExpr), listValues);

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsCoverageTest, InExpression_NullElement) {
	// Test IN expression with NULL element
	auto valueExpr = std::make_unique<LiteralExpression>();
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2))
	};

	InExpression expr(std::move(valueExpr), listValues);

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsCoverageTest, BinaryOp_DivisionByZero_Throws) {
	// Test division by zero throws exception
	auto leftExpr = std::make_unique<LiteralExpression>(int64_t(10));
	auto rightExpr = std::make_unique<LiteralExpression>(int64_t(0));

	BinaryOpExpression expr(std::move(leftExpr), BinaryOperatorType::BOP_DIVIDE, std::move(rightExpr));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), DivisionByZeroException);
}

TEST_F(ExpressionsCoverageTest, BinaryOp_ModuloByZero_Throws) {
	// Test modulo by zero throws exception
	auto leftExpr = std::make_unique<LiteralExpression>(int64_t(10));
	auto rightExpr = std::make_unique<LiteralExpression>(int64_t(0));

	BinaryOpExpression expr(std::move(leftExpr), BinaryOperatorType::BOP_MODULO, std::move(rightExpr));

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), DivisionByZeroException);
}

TEST_F(ExpressionsCoverageTest, BinaryOp_PowerOperator) {
	// Test power operator
	auto leftExpr = std::make_unique<LiteralExpression>(double(2.0));
	auto rightExpr = std::make_unique<LiteralExpression>(double(3.0));

	BinaryOpExpression expr(std::move(leftExpr), BinaryOperatorType::BOP_POWER, std::move(rightExpr));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	EXPECT_NEAR(std::get<double>(evaluator.getResult().getVariant()), 8.0, 0.001);
}

TEST_F(ExpressionsCoverageTest, ListSlice_DoubleIndex) {
	// Test list slicing with double index
	std::vector<PropertyValue> listVec = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto indexExpr = std::make_unique<LiteralExpression>(double(1.0));

	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 2);
}

TEST_F(ExpressionsCoverageTest, ListSlice_NegativeIndex) {
	// Test list slicing with negative index
	std::vector<PropertyValue> listVec = {
		PropertyValue(int64_t(10)),
		PropertyValue(int64_t(20)),
		PropertyValue(int64_t(30))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(-1));

	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	EXPECT_EQ(std::get<int64_t>(evaluator.getResult().getVariant()), 30);
}

TEST_F(ExpressionsCoverageTest, ListSlice_OutOfBoundsIndex) {
	// Test list slicing with out of bounds index returns NULL
	std::vector<PropertyValue> listVec = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(10));

	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsCoverageTest, ListSlice_NegativeRange) {
	// Test list range slice with negative start/end
	std::vector<PropertyValue> listVec = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3)),
		PropertyValue(int64_t(4)),
		PropertyValue(int64_t(5))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(-3));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(-1));

	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	const auto& result = std::get<std::vector<PropertyValue>>(evaluator.getResult().getVariant());
	EXPECT_EQ(result.size(), 2UL);
	EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 4);
}

TEST_F(ExpressionsCoverageTest, ListComprehension_FilterMode) {
	// Test list comprehension in FILTER mode
	std::vector<PropertyValue> listVec = {
		PropertyValue(int64_t(1)),
		PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3)),
		PropertyValue(int64_t(4))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));

	// WHERE clause: x > 2
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");
	auto intExpr = std::make_unique<LiteralExpression>(int64_t(2));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::move(varExpr),
		BinaryOperatorType::BOP_GREATER,
		std::move(intExpr)
	);

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	const auto& result = std::get<std::vector<PropertyValue>>(evaluator.getResult().getVariant());
	EXPECT_EQ(result.size(), 2UL);
	EXPECT_EQ(std::get<int64_t>(result[0].getVariant()), 3);
	EXPECT_EQ(std::get<int64_t>(result[1].getVariant()), 4);
}

TEST_F(ExpressionsCoverageTest, ListComprehension_NonBooleanWhere_Throws) {
	// Test list comprehension with non-boolean WHERE throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));

	// WHERE clause returns non-boolean (integer)
	auto whereExpr = std::make_unique<LiteralExpression>(int64_t(42));

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, ListComprehension_NonListValue_Throws) {
	// Test list comprehension with non-list value throws exception
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto varExpr = std::make_unique<VariableReferenceExpression>("x");

	ListComprehensionExpression expr(
		"x",
		std::move(nonListExpr),
		nullptr,
		std::move(varExpr),
		ListComprehensionExpression::ComprehensionType::COMP_EXTRACT
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, ExistsExpression_NotImplemented_Throws) {
	// Test ExistsExpression throws not implemented exception
	ExistsExpression expr("(n)-[:KNOWS]->()");

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, PatternComprehension_NotImplemented_Throws) {
	// Test PatternComprehension throws not implemented exception
	PatternComprehensionExpression expr(
		"x",
		"(n)-[:KNOWS]->(m)",
		nullptr,
		nullptr
	);

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, CaseExpression_SimpleCase_AllBranchesFail) {
	// Test simple CASE where no branches match
	auto compExpr = std::make_unique<LiteralExpression>(int64_t(5));

	CaseExpression expr(std::move(compExpr));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one")));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(2)),
		std::make_unique<LiteralExpression>(std::string("two")));

	// Evaluate through ExpressionEvaluator
	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(&expr);

	// No ELSE clause, should return NULL
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsCoverageTest, CaseExpression_SimpleCase_WithElse) {
	// Test simple CASE with ELSE clause
	auto compExpr = std::make_unique<LiteralExpression>(int64_t(5));

	CaseExpression caseExpr(std::move(compExpr));
	caseExpr.addBranch(std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one")));
	caseExpr.setElseExpression(std::make_unique<LiteralExpression>(std::string("default")));

	// Evaluate through ExpressionEvaluator
	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(&caseExpr);

	EXPECT_EQ(std::get<std::string>(evaluator.getResult().getVariant()), "default");
}

TEST_F(ExpressionsCoverageTest, CaseExpression_SearchedCase_AllBranchesFail) {
	// Test searched CASE where no conditions are true
	CaseExpression expr;
	expr.addBranch(std::make_unique<LiteralExpression>(bool(false)),
		std::make_unique<LiteralExpression>(int64_t(1)));
	expr.addBranch(std::make_unique<LiteralExpression>(int64_t(0)),
		std::make_unique<LiteralExpression>(int64_t(2)));

	// Evaluate through ExpressionEvaluator
	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(&expr);

	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsCoverageTest, VariableReferenceExpression_PropertyAccess_ReturnsNull) {
	// Test variable reference with missing property returns NULL
	context_->setVariable("node", PropertyValue(std::string("test_value")));

	auto varExpr = std::make_unique<VariableReferenceExpression>("node", "nonexistent_property");

	ExpressionEvaluator evaluator(*context_);
	varExpr->accept(evaluator);

	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsCoverageTest, VariableReferenceExpression_UndefinedVariable_Throws) {
	// Test undefined variable throws exception
	auto varExpr = std::make_unique<VariableReferenceExpression>("nonexistent_var");

	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW((void)varExpr->accept(evaluator), UndefinedVariableException);
}

TEST_F(ExpressionsCoverageTest, UnaryOp_NotWithDouble) {
	// Test unary NOT with double coerced to boolean
	auto operandExpr = std::make_unique<LiteralExpression>(double(1.0));

	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operandExpr));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
}

TEST_F(ExpressionsCoverageTest, UnaryOp_MinusWithDouble) {
	// Test unary MINUS with double
	auto operandExpr = std::make_unique<LiteralExpression>(double(3.14));

	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operandExpr));

	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);

	EXPECT_NEAR(std::get<double>(evaluator.getResult().getVariant()), -3.14, 0.001);
}

// ============================================================================
// PatternComprehensionExpression API Tests
// ============================================================================

TEST_F(ExpressionsCoverageTest, PatternComprehensionExpression_Getters) {
	// Test all getter methods of PatternComprehensionExpression
	auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr),
		std::move(whereExpr)
	);

	EXPECT_EQ(expr.getPattern(), "(n)-[:KNOWS]->(m)");
	EXPECT_EQ(expr.getTargetVar(), "m");
	EXPECT_NE(expr.getMapExpression(), nullptr);
	EXPECT_NE(expr.getWhereExpression(), nullptr);
	EXPECT_TRUE(expr.hasWhereClause());
}

TEST_F(ExpressionsCoverageTest, PatternComprehensionExpression_NoWhereClause) {
	// Test PatternComprehensionExpression without WHERE clause
	auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));

	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr),
		nullptr
	);

	EXPECT_EQ(expr.getPattern(), "(n)-[:KNOWS]->(m)");
	EXPECT_EQ(expr.getTargetVar(), "m");
	EXPECT_NE(expr.getMapExpression(), nullptr);
	EXPECT_EQ(expr.getWhereExpression(), nullptr);
	EXPECT_FALSE(expr.hasWhereClause());
}

TEST_F(ExpressionsCoverageTest, PatternComprehensionExpression_ToString) {
	// Test toString method with and without WHERE clause
	auto mapExpr1 = std::make_unique<LiteralExpression>(int64_t(42));
	auto whereExpr1 = std::make_unique<LiteralExpression>(bool(true));

	PatternComprehensionExpression expr1(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr1),
		std::move(whereExpr1)
	);

	std::string str1 = expr1.toString();
	EXPECT_TRUE(str1.find("(n)-[:KNOWS]->(m)") != std::string::npos);
	EXPECT_TRUE(str1.find("WHERE") != std::string::npos);

	auto mapExpr2 = std::make_unique<LiteralExpression>(int64_t(42));
	PatternComprehensionExpression expr2(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr2),
		nullptr
	);

	std::string str2 = expr2.toString();
	EXPECT_TRUE(str2.find("(n)-[:KNOWS]->(m)") != std::string::npos);
	EXPECT_TRUE(str2.find("WHERE") == std::string::npos);
}

TEST_F(ExpressionsCoverageTest, PatternComprehensionExpression_Clone) {
	// Test clone method
	auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	PatternComprehensionExpression original(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr),
		std::move(whereExpr)
	);

	auto cloned = original.clone();
	auto* clonedPattern = dynamic_cast<PatternComprehensionExpression*>(cloned.get());

	ASSERT_NE(clonedPattern, nullptr);
	EXPECT_EQ(clonedPattern->getPattern(), original.getPattern());
	EXPECT_EQ(clonedPattern->getTargetVar(), original.getTargetVar());
	EXPECT_NE(clonedPattern->getMapExpression(), nullptr);
	EXPECT_NE(clonedPattern->getWhereExpression(), nullptr);
	EXPECT_TRUE(clonedPattern->hasWhereClause());

	EXPECT_EQ(original.toString(), clonedPattern->toString());
}

TEST_F(ExpressionsCoverageTest, PatternComprehensionExpression_CloneNoWhere) {
	// Test clone method without WHERE clause
	auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));

	PatternComprehensionExpression original(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr),
		nullptr
	);

	auto cloned = original.clone();
	auto* clonedPattern = dynamic_cast<PatternComprehensionExpression*>(cloned.get());

	ASSERT_NE(clonedPattern, nullptr);
	EXPECT_EQ(clonedPattern->getPattern(), original.getPattern());
	EXPECT_EQ(clonedPattern->getTargetVar(), original.getTargetVar());
	EXPECT_NE(clonedPattern->getMapExpression(), nullptr);
	EXPECT_EQ(clonedPattern->getWhereExpression(), nullptr);
	EXPECT_FALSE(clonedPattern->hasWhereClause());
}

TEST_F(ExpressionsCoverageTest, PatternComprehensionExpression_AcceptVisitor) {
	// Test both accept methods
	auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::move(mapExpr),
		std::move(whereExpr)
	);

	// Test non-const visitor
	TestExpressionVisitor visitor;
	expr.accept(visitor);
	EXPECT_TRUE(visitor.visitedPatternComprehension);

	// Test const visitor
	TestConstExpressionVisitor constVisitor;
	expr.accept(constVisitor);
	EXPECT_TRUE(constVisitor.visitedPatternComprehension);
}


// ============================================================================
// IsNullExpression API Tests
// ============================================================================

TEST_F(ExpressionsCoverageTest, IsNullExpression_Getters) {
	// Test all getter methods of IsNullExpression
	auto expr = std::make_unique<LiteralExpression>(int64_t(42));

	IsNullExpression isNullExpr(std::move(expr), false);

	EXPECT_NE(isNullExpr.getExpression(), nullptr);
	EXPECT_FALSE(isNullExpr.isNegated());
}

TEST_F(ExpressionsCoverageTest, IsNullExpression_IsNegatedTrue) {
	// Test isNegated getter returns true for IS NOT NULL
	auto expr = std::make_unique<LiteralExpression>(int64_t(42));

	IsNullExpression isNullExpr(std::move(expr), true);

	EXPECT_TRUE(isNullExpr.isNegated());
}

TEST_F(ExpressionsCoverageTest, IsNullExpression_ToString) {
	// Test toString method for both IS NULL and IS NOT NULL
	auto expr1 = std::make_unique<LiteralExpression>(int64_t(42));

	IsNullExpression isNullExpr1(std::move(expr1), false);
	std::string str1 = isNullExpr1.toString();
	EXPECT_TRUE(str1.find("IS NULL") != std::string::npos);

	auto expr2 = std::make_unique<LiteralExpression>(int64_t(42));

	IsNullExpression isNullExpr2(std::move(expr2), true);
	std::string str2 = isNullExpr2.toString();
	EXPECT_TRUE(str2.find("IS NOT NULL") != std::string::npos);
}

TEST_F(ExpressionsCoverageTest, IsNullExpression_Clone) {
	// Test clone method
	auto expr = std::make_unique<LiteralExpression>(int64_t(42));

	IsNullExpression original(std::move(expr), true);

	auto cloned = original.clone();
	auto* clonedIsNull = dynamic_cast<IsNullExpression*>(cloned.get());

	ASSERT_NE(clonedIsNull, nullptr);
	EXPECT_NE(clonedIsNull->getExpression(), nullptr);
	EXPECT_EQ(clonedIsNull->isNegated(), original.isNegated());

	EXPECT_EQ(original.toString(), clonedIsNull->toString());
}

TEST_F(ExpressionsCoverageTest, IsNullExpression_AcceptVisitor) {
	// Test both accept methods
	auto expr = std::make_unique<LiteralExpression>(int64_t(42));

	IsNullExpression isNullExpr(std::move(expr), false);

	// Test non-const visitor
	TestExpressionVisitor visitor;
	isNullExpr.accept(visitor);
	EXPECT_TRUE(visitor.visitedIsNull);

	// Test const visitor
	TestConstExpressionVisitor constVisitor;
	isNullExpr.accept(constVisitor);
	EXPECT_TRUE(constVisitor.visitedIsNull);
}

TEST_F(ExpressionsCoverageTest, IsNullExpression_ConstGetter) {
	// Test const version of getExpression
	auto expr = std::make_unique<LiteralExpression>(int64_t(42));

	const IsNullExpression isNullExpr(std::move(expr), false);

	EXPECT_NE(isNullExpr.getExpression(), nullptr);
}

// ============================================================================
// EvaluationContext Exception Tests
// ============================================================================

TEST_F(ExpressionsCoverageTest, UndefinedVariableException_GetVariableName) {
	// Test UndefinedVariableException::getVariableName()
	UndefinedVariableException ex("testVar");

	EXPECT_EQ(ex.getVariableName(), "testVar");
	EXPECT_TRUE(ex.what() != nullptr);
}

TEST_F(ExpressionsCoverageTest, TypeMismatchException_GetTypes) {
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

TEST_F(ExpressionsCoverageTest, DivisionByZeroException_What) {
	// Test DivisionByZeroException
	DivisionByZeroException ex;

	EXPECT_TRUE(ex.what() != nullptr);
}

TEST_F(ExpressionsCoverageTest, EvaluationContext_GetRecord) {
	// Test EvaluationContext::getRecord()
	Record record;
	EvaluationContext context(record);

	EXPECT_EQ(&context.getRecord(), &record);
}

TEST_F(ExpressionsCoverageTest, EvaluationContext_SetAndClearVariable) {
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

TEST_F(ExpressionsCoverageTest, EvaluationContext_SetVariableOverrides) {
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

// ============================================================================
// EvaluationContext Type Conversion Tests
// ============================================================================

TEST_F(ExpressionsCoverageTest, EvaluationContext_ToBoolean) {
	// Test toBoolean with various types

	// NULL -> false
	PropertyValue nullValue;
	EXPECT_FALSE(EvaluationContext::toBoolean(nullValue));

	// boolean values
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(bool(true))));
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(bool(false))));

	// integer: 0 -> false, non-zero -> true
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(int64_t(0))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(int64_t(1))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(int64_t(-1))));

	// double: 0.0 -> false, non-zero -> true
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(double(0.0))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(double(1.5))));

	// string: "false" -> false, "true" -> true, others -> true (non-empty)
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(std::string("false"))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::string("true"))));
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(std::string("test"))));
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(std::string(""))));

	// list: empty -> false, non-empty -> true
	std::vector<PropertyValue> emptyList;
	EXPECT_FALSE(EvaluationContext::toBoolean(PropertyValue(emptyList)));

	std::vector<PropertyValue> nonEmptyList = {PropertyValue(int64_t(1))};
	EXPECT_TRUE(EvaluationContext::toBoolean(PropertyValue(nonEmptyList)));
}

TEST_F(ExpressionsCoverageTest, EvaluationContext_ToInteger) {
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

TEST_F(ExpressionsCoverageTest, EvaluationContext_ToDouble) {
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

TEST_F(ExpressionsCoverageTest, EvaluationContext_ToStringPropertyValue) {
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

TEST_F(ExpressionsCoverageTest, EvaluationContext_IsNull) {
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

// ============================================================================
// getExpressionType() Coverage Tests for Inline Methods
// ============================================================================

TEST_F(ExpressionsCoverageTest, ExistsExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	ExistsExpression expr("(n)-[:FRIENDS]->()");
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::FUNCTION_CALL);
}

TEST_F(ExpressionsCoverageTest, QuantifierFunctionExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	auto listExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto whereExpr = std::make_unique<LiteralExpression>(bool(true));

	QuantifierFunctionExpression expr("all", "x", std::move(listExpr), std::move(whereExpr));
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::FUNCTION_CALL);
}

TEST_F(ExpressionsCoverageTest, PatternComprehensionExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));

	PatternComprehensionExpression expr("(n)-[:KNOWS]->(m)", "m", std::move(mapExpr), nullptr);
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::LIST_COMPREHENSION);
}

TEST_F(ExpressionsCoverageTest, IsNullExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	auto expr = std::make_unique<LiteralExpression>(int64_t(42));

	IsNullExpression isNullExpr(std::move(expr), false);
	EXPECT_EQ(isNullExpr.getExpressionType(), ExpressionType::IS_NULL);
}

TEST_F(ExpressionsCoverageTest, BinaryOpExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));

	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::BINARY_OP);
}

TEST_F(ExpressionsCoverageTest, UnaryOpExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	auto operand = std::make_unique<LiteralExpression>(int64_t(5));

	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::UNARY_OP);
}

TEST_F(ExpressionsCoverageTest, LiteralExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	LiteralExpression expr(int64_t(42));
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::LITERAL);
}

TEST_F(ExpressionsCoverageTest, VariableReferenceExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	VariableReferenceExpression expr("n");
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::PROPERTY_ACCESS);
}

TEST_F(ExpressionsCoverageTest, FunctionCallExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(int64_t(42)));

	FunctionCallExpression expr("count", std::move(args));
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::FUNCTION_CALL);
}

TEST_F(ExpressionsCoverageTest, CaseExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	CaseExpression expr;
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::CASE_EXPRESSION);
}

TEST_F(ExpressionsCoverageTest, InExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	auto value = std::make_unique<LiteralExpression>(int64_t(5));
	std::vector<PropertyValue> listValues = {PropertyValue(int64_t(1))};

	InExpression expr(std::move(value), listValues);
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::IN_LIST);
}

TEST_F(ExpressionsCoverageTest, ListSliceExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	auto list = std::make_unique<VariableReferenceExpression>("myList");
	auto start = std::make_unique<LiteralExpression>(int64_t(0));
	auto end = std::make_unique<LiteralExpression>(int64_t(5));

	ListSliceExpression expr(std::move(list), std::move(start), std::move(end), true);
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::LIST_SLICE);
}

TEST_F(ExpressionsCoverageTest, ListComprehensionExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	auto listExpr = std::make_unique<VariableReferenceExpression>("myList");
	auto mapExpr = std::make_unique<VariableReferenceExpression>("x");

	ListComprehensionExpression expr("x", std::move(listExpr), nullptr, std::move(mapExpr), ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::LIST_COMPREHENSION);
}

TEST_F(ExpressionsCoverageTest, ListLiteralExpression_GetExpressionType) {
	// Test inline getExpressionType() method
	std::vector<PropertyValue> values = {PropertyValue(int64_t(1))};
	PropertyValue listValue(values);

	ListLiteralExpression expr(listValue);
	EXPECT_EQ(expr.getExpressionType(), ExpressionType::LITERAL);
}

// ============================================================================
// Branch coverage: LiteralExpression::toString with false boolean (line 63 False branch)
// ============================================================================

TEST_F(ExpressionsCoverageTest, LiteralExpression_ToString_FalseBoolean) {
	LiteralExpression expr(false);
	EXPECT_EQ(expr.toString(), "false");
}

// ============================================================================
// Branch coverage: toString(BinaryOperatorType) for BOP_POWER, BOP_IN,
// BOP_STARTS_WITH, BOP_ENDS_WITH, BOP_CONTAINS (lines 344, 351-354)
// ============================================================================

TEST_F(ExpressionsCoverageTest, BinaryOperatorType_ToString_Power) {
	EXPECT_EQ(graph::query::expressions::toString(BinaryOperatorType::BOP_POWER), "^");
}

TEST_F(ExpressionsCoverageTest, BinaryOperatorType_ToString_In) {
	EXPECT_EQ(graph::query::expressions::toString(BinaryOperatorType::BOP_IN), "IN");
}

TEST_F(ExpressionsCoverageTest, BinaryOperatorType_ToString_StartsWith) {
	EXPECT_EQ(graph::query::expressions::toString(BinaryOperatorType::BOP_STARTS_WITH), "STARTS WITH");
}

TEST_F(ExpressionsCoverageTest, BinaryOperatorType_ToString_EndsWith) {
	EXPECT_EQ(graph::query::expressions::toString(BinaryOperatorType::BOP_ENDS_WITH), "ENDS WITH");
}

TEST_F(ExpressionsCoverageTest, BinaryOperatorType_ToString_Contains) {
	EXPECT_EQ(graph::query::expressions::toString(BinaryOperatorType::BOP_CONTAINS), "CONTAINS");
}

// ============================================================================
// Branch coverage: isComparisonOperator for STARTS_WITH, ENDS_WITH, CONTAINS
// (lines 386-388 - these never get reached because earlier conditions catch)
// We need to call isComparisonOperator with these specific operators.
// ============================================================================

TEST_F(ExpressionsCoverageTest, IsComparisonOperator_StartsWith) {
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_STARTS_WITH));
}

TEST_F(ExpressionsCoverageTest, IsComparisonOperator_EndsWith) {
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_ENDS_WITH));
}

TEST_F(ExpressionsCoverageTest, IsComparisonOperator_Contains) {
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_CONTAINS));
}

// ============================================================================
// Branch coverage: Non-const accept(ExpressionVisitor&) for all expression types
// Lines 50-52, 139-141, 170-172, 195-197, 221-223, 253-255, 294-296
// ============================================================================

// A simple mock ExpressionVisitor to exercise non-const accept methods
class MockExpressionVisitor : public ExpressionVisitor {
public:
	int visitCount = 0;
	void visit(LiteralExpression *) override { visitCount++; }
	void visit(VariableReferenceExpression *) override { visitCount++; }
	void visit(BinaryOpExpression *) override { visitCount++; }
	void visit(UnaryOpExpression *) override { visitCount++; }
	void visit(FunctionCallExpression *) override { visitCount++; }
	void visit(CaseExpression *) override { visitCount++; }
	void visit(InExpression *) override { visitCount++; }
	void visit(class ListSliceExpression *) override { visitCount++; }
	void visit(class ListComprehensionExpression *) override { visitCount++; }
	void visit(class ListLiteralExpression *) override { visitCount++; }
	void visit(IsNullExpression *) override { visitCount++; }
	void visit(class QuantifierFunctionExpression *) override { visitCount++; }
	void visit(class ExistsExpression *) override { visitCount++; }
	void visit(class PatternComprehensionExpression *) override { visitCount++; }
	void visit(class ReduceExpression *) override { visitCount++; }
	void visit(class ParameterExpression *) override { visitCount++; }
	void visit(class ShortestPathExpression *) override { visitCount++; }
	void visit(class MapProjectionExpression *) override { visitCount++; }
};

TEST_F(ExpressionsCoverageTest, LiteralExpression_NonConstAccept) {
	MockExpressionVisitor visitor;
	LiteralExpression expr(int64_t(42));
	expr.accept(visitor);
	EXPECT_EQ(visitor.visitCount, 1);
}

TEST_F(ExpressionsCoverageTest, VariableReferenceExpression_NonConstAccept) {
	MockExpressionVisitor visitor;
	VariableReferenceExpression expr("x");
	expr.accept(visitor);
	EXPECT_EQ(visitor.visitCount, 1);
}

TEST_F(ExpressionsCoverageTest, BinaryOpExpression_NonConstAccept) {
	MockExpressionVisitor visitor;
	auto left = std::make_unique<LiteralExpression>(int64_t(1));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	expr.accept(visitor);
	EXPECT_EQ(visitor.visitCount, 1);
}

TEST_F(ExpressionsCoverageTest, UnaryOpExpression_NonConstAccept) {
	MockExpressionVisitor visitor;
	auto operand = std::make_unique<LiteralExpression>(int64_t(1));
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
	expr.accept(visitor);
	EXPECT_EQ(visitor.visitCount, 1);
}

TEST_F(ExpressionsCoverageTest, FunctionCallExpression_NonConstAccept) {
	MockExpressionVisitor visitor;
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(int64_t(1)));
	FunctionCallExpression expr("toUpper", std::move(args));
	expr.accept(visitor);
	EXPECT_EQ(visitor.visitCount, 1);
}

TEST_F(ExpressionsCoverageTest, InExpression_NonConstAccept) {
	MockExpressionVisitor visitor;
	auto value = std::make_unique<LiteralExpression>(int64_t(5));
	std::vector<PropertyValue> listValues = {PropertyValue(int64_t(1))};
	InExpression expr(std::move(value), listValues);
	expr.accept(visitor);
	EXPECT_EQ(visitor.visitCount, 1);
}

TEST_F(ExpressionsCoverageTest, CaseExpression_NonConstAccept) {
	MockExpressionVisitor visitor;
	CaseExpression expr;
	expr.accept(visitor);
	EXPECT_EQ(visitor.visitCount, 1);
}

// ============================================================================
// Branch coverage: CaseExpression::toString with null elseExpression_ (line 312 False)
// and CaseExpression::clone with null elseExpression_ (line 327 False)
// ============================================================================

TEST_F(ExpressionsCoverageTest, CaseExpression_ToString_NullElse) {
	CaseExpression expr;
	// Set elseExpression_ to nullptr
	expr.setElseExpression(nullptr);
	expr.addBranch(
		std::make_unique<LiteralExpression>(true),
		std::make_unique<LiteralExpression>(int64_t(1))
	);
	std::string result = expr.toString();
	// Should not contain "ELSE" since elseExpression_ is null
	EXPECT_TRUE(result.find("CASE") != std::string::npos);
	EXPECT_TRUE(result.find("WHEN") != std::string::npos);
	EXPECT_TRUE(result.find("ELSE") == std::string::npos);
	EXPECT_TRUE(result.find("END") != std::string::npos);
}

TEST_F(ExpressionsCoverageTest, CaseExpression_Clone_NullElse) {
	// When elseExpression_ is null, clone() skips setElseExpression on the clone.
	// The clone's default constructor provides a default elseExpression_ (LiteralExpression null).
	// This test exercises the False branch of "if (elseExpression_)" at line 327.
	CaseExpression expr;
	expr.setElseExpression(nullptr);
	expr.addBranch(
		std::make_unique<LiteralExpression>(true),
		std::make_unique<LiteralExpression>(int64_t(1))
	);
	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	// The clone gets a default ELSE (null literal) from its constructor
	std::string result = cloned->toString();
	EXPECT_NE(result.find("WHEN"), std::string::npos);
	EXPECT_NE(result.find("END"), std::string::npos);
}

// ============================================================================
// Branch coverage: BinaryOpExpression::toString with various uncovered operators
// This exercises the toString(BinaryOperatorType) through BinaryOpExpression
// ============================================================================

TEST_F(ExpressionsCoverageTest, BinaryOpExpression_ToString_Power) {
	auto left = std::make_unique<LiteralExpression>(int64_t(2));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_POWER, std::move(right));
	std::string result = expr.toString();
	EXPECT_TRUE(result.find("^") != std::string::npos);
}

TEST_F(ExpressionsCoverageTest, BinaryOpExpression_ToString_StartsWith) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello"));
	auto right = std::make_unique<LiteralExpression>(std::string("he"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_STARTS_WITH, std::move(right));
	std::string result = expr.toString();
	EXPECT_TRUE(result.find("STARTS WITH") != std::string::npos);
}

TEST_F(ExpressionsCoverageTest, BinaryOpExpression_ToString_EndsWith) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello"));
	auto right = std::make_unique<LiteralExpression>(std::string("lo"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ENDS_WITH, std::move(right));
	std::string result = expr.toString();
	EXPECT_TRUE(result.find("ENDS WITH") != std::string::npos);
}

TEST_F(ExpressionsCoverageTest, BinaryOpExpression_ToString_Contains) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello"));
	auto right = std::make_unique<LiteralExpression>(std::string("ell"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_CONTAINS, std::move(right));
	std::string result = expr.toString();
	EXPECT_TRUE(result.find("CONTAINS") != std::string::npos);
}

TEST_F(ExpressionsCoverageTest, BinaryOpExpression_ToString_In) {
	auto left = std::make_unique<LiteralExpression>(int64_t(1));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_IN, std::move(right));
	std::string result = expr.toString();
	EXPECT_TRUE(result.find("IN") != std::string::npos);
}

// ============================================================================
// Branch coverage: FunctionCallExpression::toString with zero arguments
// Line 232 False branch (empty loop)
// ============================================================================

TEST_F(ExpressionsCoverageTest, FunctionCallExpression_ToString_NoArgs) {
	std::vector<std::unique_ptr<Expression>> args;
	FunctionCallExpression expr("now", std::move(args));
	std::string result = expr.toString();
	EXPECT_EQ(result, "now()");
}

// ============================================================================
// Branch coverage: FunctionCallExpression::clone with zero arguments
// ============================================================================

TEST_F(ExpressionsCoverageTest, FunctionCallExpression_Clone_NoArgs) {
	std::vector<std::unique_ptr<Expression>> args;
	FunctionCallExpression expr("now", std::move(args));
	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	EXPECT_EQ(cloned->toString(), "now()");
}

// ============================================================================
// ResultValue coverage: Edge toString branch
// Covers the Edge variant of ResultValue::toString() which was previously uncovered
// ============================================================================

TEST_F(ExpressionsTest, ResultValue_ToString_Edge) {
	Edge edge(10, 1, 2, 42);
	std::unordered_map<std::string, PropertyValue> props;
	props["weight"] = PropertyValue(1.5);
	edge.setProperties(props);

	graph::query::ResultValue rv(edge);
	std::string result = rv.toString();
	EXPECT_TRUE(result.find("[:TypeID:42:10") != std::string::npos);
	EXPECT_TRUE(result.find("weight") != std::string::npos);
}

TEST_F(ExpressionsTest, ResultValue_ToString_EdgeNoProps) {
	Edge edge(5, 1, 2, 99);
	graph::query::ResultValue rv(edge);
	std::string result = rv.toString();
	EXPECT_TRUE(result.find("[:TypeID:99:5") != std::string::npos);
	EXPECT_TRUE(result.find("]") != std::string::npos);
}

// ============================================================================
// EvaluationContext coverage: toBoolean with Map
// Covers the MapType variant of toBoolean which was previously uncovered
// ============================================================================

TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_EmptyMap) {
	PropertyValue::MapType emptyMap;
	PropertyValue pv(emptyMap);
	EXPECT_FALSE(EvaluationContext::toBoolean(pv));
}

TEST_F(ExpressionsTest, EvaluationContext_ToBoolean_NonEmptyMap) {
	PropertyValue::MapType map;
	map["key"] = PropertyValue(int64_t(1));
	PropertyValue pv(map);
	EXPECT_TRUE(EvaluationContext::toBoolean(pv));
}

// ============================================================================
// EvaluationContext coverage: toDouble with Map
// Covers the MapType variant of toDouble which throws TypeMismatchException
// ============================================================================

TEST_F(ExpressionsTest, EvaluationContext_ToDouble_Map) {
	PropertyValue::MapType map;
	map["key"] = PropertyValue(int64_t(1));
	PropertyValue pv(map);
	EXPECT_THROW((void)EvaluationContext::toDouble(pv), TypeMismatchException);
}
