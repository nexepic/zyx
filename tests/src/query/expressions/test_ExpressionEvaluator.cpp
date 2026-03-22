/**
 * @file test_ExpressionEvaluator.cpp
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
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

class ExpressionEvaluatorTest : public ::testing::Test {
protected:
	void SetUp() override {
		record_.setValue("x", PropertyValue(int64_t(42)));
		record_.setValue("y", PropertyValue(3.14));
		record_.setValue("name", PropertyValue(std::string("Alice")));
		record_.setValue("flag", PropertyValue(true));
		record_.setValue("nullVal", PropertyValue());

		Node node(1, 100);
		std::unordered_map<std::string, PropertyValue> props;
		props["age"] = PropertyValue(int64_t(30));
		props["city"] = PropertyValue(std::string("NYC"));
		node.setProperties(props);
		record_.setNode("n", node);

		Edge edge(2, 1, 2, 101);
		std::unordered_map<std::string, PropertyValue> edgeProps;
		edgeProps["weight"] = PropertyValue(1.5);
		edge.setProperties(edgeProps);
		record_.setEdge("r", edge);

		context_ = std::make_unique<EvaluationContext>(record_);
	}

	PropertyValue eval(const Expression* expr) {
		ExpressionEvaluator evaluator(*context_);
		return evaluator.evaluate(expr);
	}

	Record record_;
	std::unique_ptr<EvaluationContext> context_;
};

// ============================================================================
// Null pointer expression
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Evaluate_NullExpression) {
	auto result = eval(nullptr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Arithmetic: mixed type (int + double)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedIntDouble_Add) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(2.5);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 12.5);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedIntDouble_Subtract) {
	auto left = std::make_unique<LiteralExpression>(2.5);
	auto right = std::make_unique<LiteralExpression>(int64_t(1));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 1.5);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedIntDouble_Multiply) {
	auto left = std::make_unique<LiteralExpression>(2.0);
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 6.0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedIntDouble_Divide) {
	auto left = std::make_unique<LiteralExpression>(7.0);
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.5);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDivideByZero) {
	auto left = std::make_unique<LiteralExpression>(7.0);
	auto right = std::make_unique<LiteralExpression>(0.0);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	EXPECT_THROW(eval(&expr), DivisionByZeroException);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedModulo) {
	auto left = std::make_unique<LiteralExpression>(7.5);
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 1); // 7 % 3
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedModuloByZero) {
	auto left = std::make_unique<LiteralExpression>(7.0);
	auto right = std::make_unique<LiteralExpression>(0.0);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
	EXPECT_THROW(eval(&expr), DivisionByZeroException);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_MixedPower) {
	auto left = std::make_unique<LiteralExpression>(2.0);
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_POWER, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 8.0);
}

// Integer division that is not evenly divisible -> double
TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerDivision_NotEvenlyDivisible) {
	auto left = std::make_unique<LiteralExpression>(int64_t(7));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.5);
}

// Integer division that IS evenly divisible -> integer
TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerDivision_EvenlyDivisible) {
	auto left = std::make_unique<LiteralExpression>(int64_t(6));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 3);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerDivideByZero) {
	auto left = std::make_unique<LiteralExpression>(int64_t(7));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	EXPECT_THROW(eval(&expr), DivisionByZeroException);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerModuloByZero) {
	auto left = std::make_unique<LiteralExpression>(int64_t(7));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
	EXPECT_THROW(eval(&expr), DivisionByZeroException);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerPower) {
	auto left = std::make_unique<LiteralExpression>(int64_t(2));
	auto right = std::make_unique<LiteralExpression>(int64_t(10));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_POWER, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 1024.0);
}

// String concatenation
TEST_F(ExpressionEvaluatorTest, Arithmetic_StringConcat) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello "));
	auto right = std::make_unique<LiteralExpression>(std::string("world"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello world");
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_StringConcatWithInt) {
	auto left = std::make_unique<LiteralExpression>(std::string("val="));
	auto right = std::make_unique<LiteralExpression>(int64_t(42));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "val=42");
}

// ============================================================================
// Comparison: STARTS_WITH, ENDS_WITH, CONTAINS
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

// ============================================================================
// Logical operators with NULL (three-valued logic)
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

// ============================================================================
// NULL propagation in non-logical operators
// ============================================================================

TEST_F(ExpressionEvaluatorTest, NullPropagation_Arithmetic) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, NullPropagation_Comparison) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_EQUAL, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, NullPropagation_Unary) {
	auto operand = std::make_unique<LiteralExpression>(); // NULL
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// FunctionCallExpression
// ============================================================================

TEST_F(ExpressionEvaluatorTest, FunctionCall_UnknownFunction) {
	std::vector<std::unique_ptr<Expression>> args;
	FunctionCallExpression expr("nonexistent_function", std::move(args));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, FunctionCall_WrongArgCount) {
	// upper() requires exactly 1 argument
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(std::string("a")));
	args.push_back(std::make_unique<LiteralExpression>(std::string("b")));
	FunctionCallExpression expr("upper", std::move(args));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, FunctionCall_EntityIntrospection_NonVariableRef) {
	// id() requires a variable reference argument, not a literal
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(std::string("n")));
	FunctionCallExpression expr("id", std::move(args));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, FunctionCall_EntityIntrospection_PropertyRef) {
	// id() requires a plain variable ref, not a property access
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<VariableReferenceExpression>("n", "age"));
	FunctionCallExpression expr("id", std::move(args));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, FunctionCall_EntityIntrospection_Success) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<VariableReferenceExpression>("n"));
	FunctionCallExpression expr("id", std::move(args));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 1);
}

TEST_F(ExpressionEvaluatorTest, FunctionCall_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const FunctionCallExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// InExpression
// ============================================================================

TEST_F(ExpressionEvaluatorTest, InExpression_Found) {
	std::vector<PropertyValue> list = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	InExpression expr(std::make_unique<LiteralExpression>(int64_t(2)), list);
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, InExpression_NotFound) {
	std::vector<PropertyValue> list = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	InExpression expr(std::make_unique<LiteralExpression>(int64_t(5)), list);
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, InExpression_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const InExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// CaseExpression
// ============================================================================

TEST_F(ExpressionEvaluatorTest, CaseExpression_SimpleCase_Match) {
	auto caseExpr = std::make_unique<CaseExpression>(std::make_unique<LiteralExpression>(int64_t(2)));
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(2)),
		std::make_unique<LiteralExpression>(std::string("two"))
	);
	caseExpr->setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "two");
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SimpleCase_NullWhenBranch) {
	auto caseExpr = std::make_unique<CaseExpression>(std::make_unique<LiteralExpression>(int64_t(1)));
	// First branch has NULL WHEN -> should be skipped
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(), // NULL
		std::make_unique<LiteralExpression>(std::string("null branch"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one"))
	);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "one");
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SearchedCase_Match) {
	auto caseExpr = std::make_unique<CaseExpression>(); // searched case
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("nope"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(true),
		std::make_unique<LiteralExpression>(std::string("yes"))
	);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "yes");
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SearchedCase_NullWhenSkipped) {
	auto caseExpr = std::make_unique<CaseExpression>(); // searched case
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(), // NULL - skipped
		std::make_unique<LiteralExpression>(std::string("null"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(true),
		std::make_unique<LiteralExpression>(std::string("yes"))
	);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "yes");
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_NoMatch_ReturnsElse) {
	auto caseExpr = std::make_unique<CaseExpression>(); // searched case
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("nope"))
	);
	caseExpr->setElseExpression(std::make_unique<LiteralExpression>(std::string("default")));

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "default");
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const CaseExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// VariableReferenceExpression - uncovered branches
// ============================================================================

TEST_F(ExpressionEvaluatorTest, VarRef_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const VariableReferenceExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, VarRef_MissingProperty_ReturnsNull) {
	VariableReferenceExpression expr("n", "nonexistent");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, VarRef_UndefinedVariable_Throws) {
	VariableReferenceExpression expr("undefined_var");
	EXPECT_THROW(eval(&expr), UndefinedVariableException);
}

// ============================================================================
// BinaryOpExpression - null ptr
// ============================================================================

TEST_F(ExpressionEvaluatorTest, BinaryOp_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const BinaryOpExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// UnaryOpExpression - null ptr
// ============================================================================

TEST_F(ExpressionEvaluatorTest, UnaryOp_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const UnaryOpExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// ListSlice - null ptr
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListSlice_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const ListSliceExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// ListComprehension - null ptr
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListComprehension_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const ListComprehensionExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// ListLiteral - null ptr
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListLiteral_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const ListLiteralExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// IsNullExpression - null ptr
// ============================================================================

TEST_F(ExpressionEvaluatorTest, IsNull_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const IsNullExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// QuantifierFunctionExpression - null ptr
// ============================================================================

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const QuantifierFunctionExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// ExistsExpression - null ptr
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Exists_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const ExistsExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// PatternComprehensionExpression - null ptr
// ============================================================================

TEST_F(ExpressionEvaluatorTest, PatternComprehension_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const PatternComprehensionExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Comparison operators
// ============================================================================

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

// ============================================================================
// Integer arithmetic: subtract, multiply, modulo
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerSubtract) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 7);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerMultiply) {
	auto left = std::make_unique<LiteralExpression>(int64_t(4));
	auto right = std::make_unique<LiteralExpression>(int64_t(5));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 20);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerModulo) {
	auto left = std::make_unique<LiteralExpression>(int64_t(7));
	auto right = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 1);
}

// ============================================================================
// Additional coverage tests: Unary operators (non-null paths)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Unary_MinusInteger) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(42));
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), -42);
}

TEST_F(ExpressionEvaluatorTest, Unary_MinusDouble) {
	auto operand = std::make_unique<LiteralExpression>(3.14);
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), -3.14);
}

TEST_F(ExpressionEvaluatorTest, Unary_Not_True) {
	auto operand = std::make_unique<LiteralExpression>(true);
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, Unary_Not_False) {
	auto operand = std::make_unique<LiteralExpression>(false);
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

// ============================================================================
// Additional coverage tests: String concatenation (int + string)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntConcatWithString) {
	auto left = std::make_unique<LiteralExpression>(int64_t(42));
	auto right = std::make_unique<LiteralExpression>(std::string(" items"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "42 items");
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleConcatWithString) {
	auto left = std::make_unique<LiteralExpression>(std::string("pi="));
	auto right = std::make_unique<LiteralExpression>(3.14);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	// toString for doubles may vary, just check it starts with "pi="
	auto str = std::get<std::string>(result.getVariant());
	EXPECT_TRUE(str.find("pi=") == 0);
}

// ============================================================================
// Additional coverage tests: Double-double arithmetic
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDouble_Add) {
	auto left = std::make_unique<LiteralExpression>(1.5);
	auto right = std::make_unique<LiteralExpression>(2.5);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 4.0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDouble_Subtract) {
	auto left = std::make_unique<LiteralExpression>(5.5);
	auto right = std::make_unique<LiteralExpression>(1.5);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 4.0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDouble_Multiply) {
	auto left = std::make_unique<LiteralExpression>(2.5);
	auto right = std::make_unique<LiteralExpression>(4.0);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MULTIPLY, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 10.0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDouble_Divide) {
	auto left = std::make_unique<LiteralExpression>(7.5);
	auto right = std::make_unique<LiteralExpression>(2.5);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDouble_Power) {
	auto left = std::make_unique<LiteralExpression>(2.0);
	auto right = std::make_unique<LiteralExpression>(3.0);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_POWER, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 8.0);
}

TEST_F(ExpressionEvaluatorTest, Arithmetic_DoubleDouble_Modulo) {
	auto left = std::make_unique<LiteralExpression>(7.5);
	auto right = std::make_unique<LiteralExpression>(2.5);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	// toInteger(7.5) = 7, toInteger(2.5) = 2, 7 % 2 = 1
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 1);
}

// ============================================================================
// Additional coverage tests: IsNullExpression (non-null ptr paths)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, IsNull_NullLiteral) {
	auto inner = std::make_unique<LiteralExpression>(); // NULL
	IsNullExpression expr(std::move(inner), false);     // IS NULL
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, IsNull_NonNullLiteral) {
	auto inner = std::make_unique<LiteralExpression>(int64_t(42));
	IsNullExpression expr(std::move(inner), false); // IS NULL
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, IsNotNull_NullLiteral) {
	auto inner = std::make_unique<LiteralExpression>(); // NULL
	IsNullExpression expr(std::move(inner), true);      // IS NOT NULL
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, IsNotNull_NonNullLiteral) {
	auto inner = std::make_unique<LiteralExpression>(int64_t(42));
	IsNullExpression expr(std::move(inner), true); // IS NOT NULL
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

// ============================================================================
// Additional coverage tests: ListLiteralExpression (non-null ptr path)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListLiteral_ReturnsStoredValue) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2))};
	auto pv = PropertyValue(values);
	ListLiteralExpression expr(std::move(pv));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 2);
}

// ============================================================================
// Additional coverage tests: ListSliceExpression (non-null ptr paths)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListSlice_SingleIndex) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(1));
	// hasRange=false for single index access
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 20);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_NegativeIndex) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(-1));
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 30);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_OutOfBounds) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(5));
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_DoubleIndex) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto indexExpr = std::make_unique<LiteralExpression>(1.0);
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 20);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_Range) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)),
		PropertyValue(int64_t(30)), PropertyValue(int64_t(40))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(1));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(3));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 20);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 30);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeNegativeStart) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)),
		PropertyValue(int64_t(30)), PropertyValue(int64_t(40))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(-2));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(4));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 30);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 40);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeNegativeEnd) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)),
		PropertyValue(int64_t(30)), PropertyValue(int64_t(40))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(-1));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(3));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 10);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 30);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_NonListThrows) {
	auto inner = std::make_unique<LiteralExpression>(int64_t(42));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(0));
	ListSliceExpression expr(std::move(inner), std::move(indexExpr), nullptr, false);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeWithDoubleIndices) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(0.0);
	auto endExpr = std::make_unique<LiteralExpression>(2.0);
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
}

TEST_F(ExpressionEvaluatorTest, ListSlice_StringIndexThrows) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto indexExpr = std::make_unique<LiteralExpression>(std::string("bad"));
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeStringStartThrows) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(std::string("bad"));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(1));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeStringEndThrows) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
	auto endExpr = std::make_unique<LiteralExpression>(std::string("bad"));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeNoStart) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(2));
	// start=nullptr means from beginning, hasRange=true
	ListSliceExpression expr(std::move(listExpr), nullptr, std::move(endExpr), true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 10);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 20);
}

TEST_F(ExpressionEvaluatorTest, ListSlice_RangeNoEnd) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(1));
	// end=nullptr means to the end, hasRange=true
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), nullptr, true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 20);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 30);
}

// ============================================================================
// Additional coverage tests: CaseExpression - no match, no else -> NULL
// ============================================================================

TEST_F(ExpressionEvaluatorTest, CaseExpression_NoMatch_NoElse_ReturnsNull) {
	auto caseExpr = std::make_unique<CaseExpression>(); // searched case
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("nope"))
	);
	// No else expression set

	auto result = eval(caseExpr.get());
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SimpleCase_NoMatch_NoElse) {
	auto caseExpr = std::make_unique<CaseExpression>(std::make_unique<LiteralExpression>(int64_t(99)));
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one"))
	);
	// No match and no else

	auto result = eval(caseExpr.get());
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SearchedCase_FalseBranch_NotMatched) {
	auto caseExpr = std::make_unique<CaseExpression>(); // searched case
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("nope"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("still nope"))
	);
	caseExpr->setElseExpression(std::make_unique<LiteralExpression>(std::string("default")));

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "default");
}

// ============================================================================
// Additional coverage tests: FunctionCall - regular function with args
// ============================================================================

TEST_F(ExpressionEvaluatorTest, FunctionCall_Upper) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(std::string("hello")));
	FunctionCallExpression expr("upper", std::move(args));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "HELLO");
}

TEST_F(ExpressionEvaluatorTest, FunctionCall_Lower) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(std::string("HELLO")));
	FunctionCallExpression expr("lower", std::move(args));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "hello");
}

TEST_F(ExpressionEvaluatorTest, FunctionCall_Abs) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(int64_t(-5)));
	FunctionCallExpression expr("abs", std::move(args));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 5);
}

// ============================================================================
// Additional coverage tests: Entity introspection functions
// ============================================================================

TEST_F(ExpressionEvaluatorTest, FunctionCall_Labels) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<VariableReferenceExpression>("n"));
	FunctionCallExpression expr("labels", std::move(args));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
}

TEST_F(ExpressionEvaluatorTest, FunctionCall_Type_Edge) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<VariableReferenceExpression>("r"));
	FunctionCallExpression expr("type", std::move(args));
	auto result = eval(&expr);
	// type() on an edge returns the label id as integer
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 101);
}

// ============================================================================
// Additional coverage tests: Variable reference - simple variable resolves
// ============================================================================

TEST_F(ExpressionEvaluatorTest, VarRef_SimpleVariable_ReturnsValue) {
	VariableReferenceExpression expr("x");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 42);
}

TEST_F(ExpressionEvaluatorTest, VarRef_PropertyAccess_Found) {
	VariableReferenceExpression expr("n", "age");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 30);
}

TEST_F(ExpressionEvaluatorTest, VarRef_EdgeProperty) {
	VariableReferenceExpression expr("r", "weight");
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 1.5);
}

// ============================================================================
// Additional coverage tests: Literal visit branches (double, null)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Literal_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const LiteralExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Literal_Double) {
	LiteralExpression expr(3.14);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 3.14);
}

TEST_F(ExpressionEvaluatorTest, Literal_Null) {
	LiteralExpression expr;
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Literal_Integer) {
	LiteralExpression expr(int64_t(100));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 100);
}

TEST_F(ExpressionEvaluatorTest, Literal_Boolean) {
	LiteralExpression expr(true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Literal_String) {
	LiteralExpression expr(std::string("test"));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "test");
}

// ============================================================================
// Additional coverage tests: Logical XOR with null on right
// ============================================================================

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

// ============================================================================
// Additional coverage tests: Logical AND/OR non-null paths
// ============================================================================

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

// ============================================================================
// Additional coverage tests: InExpression with dynamic list expression
// ============================================================================

TEST_F(ExpressionEvaluatorTest, InExpression_StringMatch) {
	std::vector<PropertyValue> list = {
		PropertyValue(std::string("alice")),
		PropertyValue(std::string("bob")),
		PropertyValue(std::string("charlie"))
	};
	InExpression expr(std::make_unique<LiteralExpression>(std::string("bob")), list);
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, InExpression_EmptyList) {
	std::vector<PropertyValue> list;
	InExpression expr(std::make_unique<LiteralExpression>(int64_t(1)), list);
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

// ============================================================================
// Additional coverage tests: Null propagation on right operand
// ============================================================================

TEST_F(ExpressionEvaluatorTest, NullPropagation_RightNull) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Additional coverage tests: ExistsExpression (non-null ptr path)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Exists_ThrowsNotImplemented) {
	ExistsExpression expr("(n)-[:KNOWS]->(m)");
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// Additional coverage tests: PatternComprehensionExpression (non-null ptr path)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, PatternComprehension_ThrowsNotImplemented) {
	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::make_unique<LiteralExpression>(std::string("m.name"))
	);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// ListComprehensionExpression - WHERE filter and MAP expression paths
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListComprehension_FilterOnly) {
	// [x IN [1,2,3,4,5] WHERE x > 3] => [4, 5]
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3)), PropertyValue(int64_t(4)),
		PropertyValue(int64_t(5))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));

	// WHERE x > 3
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(3))
	);

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		nullptr, // no map expression - FILTER mode
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 4);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 5);
}

TEST_F(ExpressionEvaluatorTest, ListComprehension_ExtractOnly) {
	// [x IN [1,2,3] | x * 2] => [2, 4, 6]
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));

	// | x * 2
	auto mapExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_MULTIPLY,
		std::make_unique<LiteralExpression>(int64_t(2))
	);

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		nullptr, // no WHERE clause
		std::move(mapExpr),
		ListComprehensionExpression::ComprehensionType::COMP_EXTRACT
	);

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(3));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 4);
	EXPECT_EQ(std::get<int64_t>(list[2].getVariant()), 6);
}

TEST_F(ExpressionEvaluatorTest, ListComprehension_FilterAndMap) {
	// [x IN [1,2,3,4] WHERE x > 2 | x * 10] => [30, 40]
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(2)),
		PropertyValue(int64_t(3)), PropertyValue(int64_t(4))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));

	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(2))
	);

	auto mapExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_MULTIPLY,
		std::make_unique<LiteralExpression>(int64_t(10))
	);

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		std::move(mapExpr),
		ListComprehensionExpression::ComprehensionType::COMP_EXTRACT
	);

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	auto list = result.getList();
	EXPECT_EQ(list.size(), size_t(2));
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 30);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 40);
}

TEST_F(ExpressionEvaluatorTest, ListComprehension_NonListThrows) {
	auto listExpr = std::make_unique<LiteralExpression>(int64_t(42)); // not a list
	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		nullptr,
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, ListComprehension_EmptyList) {
	std::vector<PropertyValue> empty;
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(empty));
	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		nullptr,
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	EXPECT_EQ(result.getList().size(), size_t(0));
}

// ============================================================================
// QuantifierFunctionExpression - all(), any(), none(), single()
// ============================================================================

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_All_True) {
	// all(x IN [2, 4, 6] WHERE x > 0) => true
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(2)), PropertyValue(int64_t(4)),
		PropertyValue(int64_t(6))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("all", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_All_False) {
	// all(x IN [2, -1, 6] WHERE x > 0) => false
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(2)), PropertyValue(int64_t(-1)),
		PropertyValue(int64_t(6))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("all", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_Any_True) {
	// any(x IN [1, -2, -3] WHERE x > 0) => true
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(-2)),
		PropertyValue(int64_t(-3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("any", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_Any_False) {
	// any(x IN [-1, -2, -3] WHERE x > 0) => false
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(-1)), PropertyValue(int64_t(-2)),
		PropertyValue(int64_t(-3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("any", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_None_True) {
	// none(x IN [-1, -2, -3] WHERE x > 0) => true
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(-1)), PropertyValue(int64_t(-2)),
		PropertyValue(int64_t(-3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("none", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_Single_True) {
	// single(x IN [1, -2, -3] WHERE x > 0) => true (exactly one)
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(-2)),
		PropertyValue(int64_t(-3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("single", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_Single_False_Multiple) {
	// single(x IN [1, 2, -3] WHERE x > 0) => false (two match)
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(2)),
		PropertyValue(int64_t(-3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("x"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(0))
	);

	QuantifierFunctionExpression expr("single", "x", std::move(listExpr), std::move(whereExpr));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_UnknownFunction) {
	std::vector<PropertyValue> listValues = {PropertyValue(int64_t(1))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<LiteralExpression>(true);

	QuantifierFunctionExpression expr("nonexistent_quantifier", "x", std::move(listExpr), std::move(whereExpr));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, QuantifierFunction_NonPredicateFunction) {
	// "upper" is a scalar function but not a PredicateFunction
	std::vector<PropertyValue> listValues = {PropertyValue(int64_t(1))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<LiteralExpression>(true);

	QuantifierFunctionExpression expr("upper", "x", std::move(listExpr), std::move(whereExpr));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// Entity introspection - wrong arg count
// ============================================================================

TEST_F(ExpressionEvaluatorTest, FunctionCall_EntityIntrospection_WrongArgCount) {
	// id() with 0 arguments
	std::vector<std::unique_ptr<Expression>> args;
	FunctionCallExpression expr("id", std::move(args));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, FunctionCall_EntityIntrospection_TooManyArgs) {
	// id(n, m) - 2 arguments
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<VariableReferenceExpression>("n"));
	args.push_back(std::make_unique<VariableReferenceExpression>("r"));
	FunctionCallExpression expr("id", std::move(args));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// ListComprehension WHERE non-boolean throws
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListComprehension_NonBooleanWhereThrows) {
	std::vector<PropertyValue> listValues = {PropertyValue(int64_t(1))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));

	// WHERE returns an integer, not boolean
	auto whereExpr = std::make_unique<LiteralExpression>(int64_t(42));

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// Unknown binary operator (BOP_IN used directly in BinaryOpExpression hits
// the else branch at line 124 since it's not arithmetic, comparison, or logical)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, BinaryOp_UnknownOperator_Throws) {
	// BOP_IN is not arithmetic, comparison, or logical -> hits "Unknown binary operator"
	auto left = std::make_unique<LiteralExpression>(int64_t(1));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_IN, std::move(right));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// Variadic function wrong arg count error message (sig.variadic true branch)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, FunctionCall_VariadicWrongArgCount) {
	// coalesce() is variadic with minArgs=1, calling with 0 args triggers the
	// variadic branch of the error message
	std::vector<std::unique_ptr<Expression>> args;
	FunctionCallExpression expr("coalesce", std::move(args));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// Simple CASE: comparison value doesn't match any WHEN (non-null WHEN values)
// Tests the comparisonVal == whenVal false branch at line 244
// ============================================================================

TEST_F(ExpressionEvaluatorTest, CaseExpression_SimpleCase_NoMatch_WithElse) {
	auto caseExpr = std::make_unique<CaseExpression>(std::make_unique<LiteralExpression>(int64_t(99)));
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(2)),
		std::make_unique<LiteralExpression>(std::string("two"))
	);
	caseExpr->setElseExpression(std::make_unique<LiteralExpression>(std::string("default")));

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "default");
}

// ============================================================================
// Searched CASE: conditionMet false path (non-null, non-true WHEN value)
// Tests the conditionMet false branch at line 263
// ============================================================================

TEST_F(ExpressionEvaluatorTest, CaseExpression_SearchedCase_FalseCondition_SkippedToMatch) {
	auto caseExpr = std::make_unique<CaseExpression>();
	// First branch: false -> skip (conditionMet = false)
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("skipped"))
	);
	// Second branch: true -> match (conditionMet = true)
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(true),
		std::make_unique<LiteralExpression>(std::string("matched"))
	);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "matched");
}

// ============================================================================
// ListSlice: single index with no start expression -> throws
// Tests the !expr->getStart() branch at line 492
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListSlice_SingleIndex_NoStartThrows) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	// hasRange=false, start=nullptr -> should throw "List slice requires index"
	ListSliceExpression expr(std::move(listExpr), nullptr, nullptr, false);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// ListSlice: negative index that resolves to still-negative -> out of bounds
// Tests line 516: index < 0 after adjustment
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListSlice_NegativeIndex_StillNegativeAfterAdjust) {
	std::vector<PropertyValue> values = {PropertyValue(int64_t(10))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	// list size=1, index=-5 -> adjusted index = 1 + (-5) = -4 -> still < 0 -> NULL
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(-5));
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Entity introspection: !varRef branch (when argument is not a
// VariableReferenceExpression at all, e.g., a BinaryOpExpression)
// This specifically tests the !varRef part of line 182
// ============================================================================

TEST_F(ExpressionEvaluatorTest, FunctionCall_EntityIntrospection_NonVarRefExprType) {
	// Pass a BinaryOpExpression as argument instead of VariableReferenceExpression
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<BinaryOpExpression>(
		std::make_unique<LiteralExpression>(int64_t(1)),
		BinaryOperatorType::BOP_ADD,
		std::make_unique<LiteralExpression>(int64_t(2))
	));
	FunctionCallExpression expr("id", std::move(args));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// Comparison: equality between same values (true path)
// ============================================================================

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

// ============================================================================
// Integer arithmetic: ADD (both integers path, case BOP_ADD)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntegerAdd) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(20));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 30);
}

// ============================================================================
// ListComprehension: WHERE false skips element (condition false continue branch)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListComprehension_WhereFiltersFalseElements) {
	// [x IN [1, 2, 3] WHERE false] => empty
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));
	auto whereExpr = std::make_unique<LiteralExpression>(false);

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);

	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	EXPECT_EQ(result.getList().size(), size_t(0));
}

// ============================================================================
// Logical: AND with both true values (final return at line 420)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Logical_AND_BothTrue) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

// ============================================================================
// Logical: OR with both false (final return at line 433)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Logical_OR_BothFalse) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

// ============================================================================
// FunctionCall: regular function evaluation path (non-introspection)
// Tests the loop at line 194 and function call at line 200
// ============================================================================

TEST_F(ExpressionEvaluatorTest, FunctionCall_Coalesce) {
	// coalesce(null, null, 42) => 42
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>()); // NULL
	args.push_back(std::make_unique<LiteralExpression>()); // NULL
	args.push_back(std::make_unique<LiteralExpression>(int64_t(42)));
	FunctionCallExpression expr("coalesce", std::move(args));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 42);
}

// ============================================================================
// Unary minus on double (non-integer path of UOP_MINUS)
// This ensures both branches of line 451 are covered
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Unary_MinusBoolean) {
	// Boolean is not INTEGER, so it takes the double path
	auto operand = std::make_unique<LiteralExpression>(true);
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	// toDouble(true) = 1.0, so -1.0
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), -1.0);
}

// ============================================================================
// Simple CASE: multiple non-matching, non-null WHEN branches
// Ensures repeated iteration through the comparison loop
// ============================================================================

TEST_F(ExpressionEvaluatorTest, CaseExpression_SimpleCase_MultipleNonMatchBranches) {
	auto caseExpr = std::make_unique<CaseExpression>(std::make_unique<LiteralExpression>(int64_t(5)));
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(2)),
		std::make_unique<LiteralExpression>(std::string("two"))
	);
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(5)),
		std::make_unique<LiteralExpression>(std::string("five"))
	);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "five");
}

// ============================================================================
// NULL propagation: both operands NULL for non-logical operator
// ============================================================================

TEST_F(ExpressionEvaluatorTest, NullPropagation_BothNull) {
	auto left = std::make_unique<LiteralExpression>();  // NULL
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Entity introspection: keys() function
// ============================================================================

TEST_F(ExpressionEvaluatorTest, FunctionCall_Keys) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<VariableReferenceExpression>("n"));
	FunctionCallExpression expr("keys", std::move(args));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
}

// ============================================================================
// Entity introspection: properties() function
// ============================================================================

TEST_F(ExpressionEvaluatorTest, FunctionCall_Properties) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<VariableReferenceExpression>("n"));
	FunctionCallExpression expr("properties", std::move(args));
	auto result = eval(&expr);
	// properties() returns a map-like structure, check it evaluates without error
	EXPECT_NE(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch coverage: CaseExpression with explicitly null else expression
// Covers line 271 False branch (getElseExpression() returns nullptr)
// The CaseExpression constructor defaults to a NULL LiteralExpression else,
// so we must explicitly set it to nullptr to cover the else branch at line 274.
// ============================================================================

TEST_F(ExpressionEvaluatorTest, CaseExpression_SearchedCase_NoElse_ExplicitNull) {
	auto caseExpr = std::make_unique<CaseExpression>();
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(std::string("nope"))
	);
	// Explicitly set else to nullptr to cover the nullptr branch
	caseExpr->setElseExpression(nullptr);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, CaseExpression_SimpleCase_NoElse_ExplicitNull) {
	auto caseExpr = std::make_unique<CaseExpression>(std::make_unique<LiteralExpression>(int64_t(99)));
	caseExpr->addBranch(
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one"))
	);
	// Explicitly set else to nullptr
	caseExpr->setElseExpression(nullptr);

	auto result = eval(caseExpr.get());
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch coverage: String concatenation where right operand is the string
// and left is non-string (covers line 287 second sub-branch more directly)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Arithmetic_BoolConcatWithString) {
	auto left = std::make_unique<LiteralExpression>(true);
	auto right = std::make_unique<LiteralExpression>(std::string(" flag"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
}

// ============================================================================
// Branch coverage: Integer left + non-integer right arithmetic (covers
// line 292-293 where left is INTEGER but right is not, so bothIntegers=false)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Arithmetic_IntLeftBoolRight_Subtract) {
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(true); // toDouble(true)=1.0
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_SUBTRACT, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 9.0);
}

// ============================================================================
// Branch coverage: Boolean left + Boolean right arithmetic (covers
// line 292 where left is not INTEGER, so bothIntegers=false early)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Arithmetic_BoolBool_Add) {
	auto left = std::make_unique<LiteralExpression>(true);  // toDouble=1.0
	auto right = std::make_unique<LiteralExpression>(false); // toDouble=0.0
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), 1.0);
}

// ============================================================================
// Branch coverage: STARTS_WITH where left equals right exactly
// Covers the second sub-branch of line 383 (l.substr == r is true)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Comparison_StartsWith_ExactMatch) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello"));
	auto right = std::make_unique<LiteralExpression>(std::string("hello"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_STARTS_WITH, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

// ============================================================================
// Branch coverage: ENDS_WITH where left equals right exactly
// Covers the second sub-branch of line 389 (l.substr == r is true)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Comparison_EndsWith_ExactMatch) {
	auto left = std::make_unique<LiteralExpression>(std::string("hello"));
	auto right = std::make_unique<LiteralExpression>(std::string("hello"));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ENDS_WITH, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

// ============================================================================
// Branch coverage: Logical AND where left is NULL and right is true
// Exercises leftNull=true path to ensure the third check at line 419 is hit
// with leftNull true
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Logical_AND_NullAndTrue_ReturnsNull) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(true);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_AND, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch coverage: Logical OR where left is NULL and right is false
// Exercises the path where left is null, right is not null but also not true,
// hitting the leftNull branch at line 432
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Logical_OR_NullOrFalse_ReturnsNull) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch coverage: Logical OR where right is null and left is false
// Exercises the rightNull branch at line 432
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Logical_OR_FalseOrNull_ReturnsNull) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_OR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch coverage: Logical XOR where left null, right non-null
// Exercises the leftNull true path at line 439
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Logical_XOR_NullXorFalse) {
	auto left = std::make_unique<LiteralExpression>(); // NULL
	auto right = std::make_unique<LiteralExpression>(false);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch coverage: Logical XOR where right null, left non-null (false)
// Exercises the rightNull true path at line 439
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Logical_XOR_FalseXorNull) {
	auto left = std::make_unique<LiteralExpression>(false);
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_XOR, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Branch coverage: ListComprehension WHERE clause returns NULL -> continue
// Exercises the isNull(whereResult) check at line 256 in searched CASE
// and also the NULL WHERE path in list comprehension
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListComprehension_WhereReturnsNull_TreatedAsFalse) {
	// WHERE returns NULL for each element -> all elements should be skipped
	std::vector<PropertyValue> listValues = {
		PropertyValue(int64_t(1)), PropertyValue(int64_t(2))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listValues));

	// WHERE returns a non-boolean integer -> throws
	// Instead, use a variable reference to "nullVal" which is NULL in our context
	auto whereExpr = std::make_unique<VariableReferenceExpression>("nullVal");

	ListComprehensionExpression expr(
		"x",
		std::move(listExpr),
		std::move(whereExpr),
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER
	);
	// The WHERE returns NULL which is not BOOLEAN type -> throws
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// Branch coverage: Multiple function args evaluation (more than 2 args)
// Covers the loop at line 194 with more iterations
// ============================================================================

TEST_F(ExpressionEvaluatorTest, FunctionCall_CoalesceMultipleArgs) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>()); // NULL
	args.push_back(std::make_unique<LiteralExpression>()); // NULL
	args.push_back(std::make_unique<LiteralExpression>()); // NULL
	args.push_back(std::make_unique<LiteralExpression>(int64_t(99)));
	FunctionCallExpression expr("coalesce", std::move(args));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 99);
}

// ============================================================================
// Branch coverage: Non-variadic function wrong arg count (sig.variadic false
// branch at line 170)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, FunctionCall_NonVariadicWrongArgCount) {
	// abs() requires exactly 1 argument (non-variadic), calling with 0 triggers
	// the non-variadic branch of the error message
	std::vector<std::unique_ptr<Expression>> args;
	FunctionCallExpression expr("abs", std::move(args));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// Branch coverage: Comparison operators with string values
// Ensures more comparison branches are exercised
// ============================================================================

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

// ============================================================================
// Branch coverage: Unary NOT on non-boolean (e.g., integer)
// toBoolean(integer) should work, covering the implicit conversion path
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Unary_Not_Integer) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(0));
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	// toBoolean(0) = false, NOT false = true
	EXPECT_EQ(std::get<bool>(result.getVariant()), true);
}

TEST_F(ExpressionEvaluatorTest, Unary_Not_NonZeroInteger) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(42));
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::BOOLEAN);
	// toBoolean(42) = true, NOT true = false
	EXPECT_EQ(std::get<bool>(result.getVariant()), false);
}

// ============================================================================
// Branch coverage: ListSlice range with empty result (start >= end)
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListSlice_Range_EmptyResult) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(2));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(1)); // end < start
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
	EXPECT_EQ(result.getList().size(), size_t(0));
}

// ============================================================================
// Branch coverage: ListSlice with negative index exactly at boundary
// index=-N where N=listSize -> index becomes 0 -> valid
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListSlice_NegativeIndex_ExactBoundary) {
	std::vector<PropertyValue> values = {
		PropertyValue(int64_t(10)), PropertyValue(int64_t(20)), PropertyValue(int64_t(30))
	};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(values));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(-3)); // -3 + 3 = 0
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 10);
}

// ============================================================================
// Branch coverage: EntityIntrospectionFunction called with != 1 arguments
// Covers the argsExpr.size() == 1 false branch at line 179.
// A variadic EntityIntrospectionFunction that accepts 0+ args, called with 0 args,
// triggers varRef = nullptr, which then throws the expected error.
// ============================================================================

TEST_F(ExpressionEvaluatorTest, EntityIntrospection_ZeroArgs_ThrowsError) {
	// Register a variadic entity introspection function (accepts 0+ args)
	auto& registry = FunctionRegistry::getInstance();
	registry.registerFunction(std::make_unique<LambdaEntityIntrospectionFunction>(
		FunctionSignature("__test_variadic_introspect__", 0, 10, true),
		[](const std::vector<PropertyValue>&, const EvaluationContext&) -> PropertyValue {
			return PropertyValue(int64_t(0));
		}
	));

	// Call with 0 arguments - argsExpr.size() != 1, so varRef becomes nullptr
	FunctionCallExpression expr("__test_variadic_introspect__", {});
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, EntityIntrospection_TwoArgs_ThrowsError) {
	// Register a variadic entity introspection function (accepts 0+ args)
	auto& registry = FunctionRegistry::getInstance();
	if (!registry.hasFunction("__test_variadic_introspect2__")) {
		registry.registerFunction(std::make_unique<LambdaEntityIntrospectionFunction>(
			FunctionSignature("__test_variadic_introspect2__", 0, 10, true),
			[](const std::vector<PropertyValue>&, const EvaluationContext&) -> PropertyValue {
				return PropertyValue(int64_t(0));
			}
		));
	}

	// Call with 2 arguments - argsExpr.size() != 1, so varRef becomes nullptr
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<VariableReferenceExpression>("n"));
	args.push_back(std::make_unique<LiteralExpression>(int64_t(42)));
	FunctionCallExpression expr("__test_variadic_introspect2__", std::move(args));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}
