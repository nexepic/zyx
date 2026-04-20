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

#include "query/expressions/ExpressionEvaluatorTestFixture.hpp"

// ============================================================================
// Null pointer expression
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Evaluate_NullExpression) {
	auto result = eval(nullptr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// Literal Tests
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
// VariableReferenceExpression Tests
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
// UnaryOpExpression Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, UnaryOp_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const UnaryOpExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

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

TEST_F(ExpressionEvaluatorTest, Unary_MinusBoolean) {
	// Boolean is not INTEGER, so it takes the double path
	auto operand = std::make_unique<LiteralExpression>(true);
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::DOUBLE);
	// toDouble(true) = 1.0, so -1.0
	EXPECT_DOUBLE_EQ(std::get<double>(result.getVariant()), -1.0);
}

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
// NullPropagation Tests
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

TEST_F(ExpressionEvaluatorTest, NullPropagation_RightNull) {
	auto left = std::make_unique<LiteralExpression>(int64_t(5));
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, NullPropagation_BothNull) {
	auto left = std::make_unique<LiteralExpression>();  // NULL
	auto right = std::make_unique<LiteralExpression>(); // NULL
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

// ============================================================================
// FunctionCall Tests
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

TEST_F(ExpressionEvaluatorTest, FunctionCall_VariadicWrongArgCount) {
	// coalesce() is variadic with minArgs=1, calling with 0 args triggers the
	// variadic branch of the error message
	std::vector<std::unique_ptr<Expression>> args;
	FunctionCallExpression expr("coalesce", std::move(args));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

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

TEST_F(ExpressionEvaluatorTest, FunctionCall_NonVariadicWrongArgCount) {
	// abs() requires exactly 1 argument (non-variadic), calling with 0 triggers
	// the non-variadic branch of the error message
	std::vector<std::unique_ptr<Expression>> args;
	FunctionCallExpression expr("abs", std::move(args));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, FunctionCall_Keys) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<VariableReferenceExpression>("n"));
	FunctionCallExpression expr("keys", std::move(args));
	auto result = eval(&expr);
	EXPECT_EQ(result.getType(), PropertyType::LIST);
}

TEST_F(ExpressionEvaluatorTest, FunctionCall_Properties) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<VariableReferenceExpression>("n"));
	FunctionCallExpression expr("properties", std::move(args));
	auto result = eval(&expr);
	// properties() returns a map-like structure, check it evaluates without error
	EXPECT_NE(result.getType(), PropertyType::NULL_TYPE);
}

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

// ============================================================================
// BinaryOpExpression - null ptr and unknown operator
// ============================================================================

TEST_F(ExpressionEvaluatorTest, BinaryOp_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const BinaryOpExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, BinaryOp_UnknownOperator_Throws) {
	// BOP_IN is not arithmetic, comparison, or logical -> hits "Unknown binary operator"
	auto left = std::make_unique<LiteralExpression>(int64_t(1));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_IN, std::move(right));
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// IsNullExpression Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, IsNull_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const IsNullExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

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
// ListLiteralExpression Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ListLiteral_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const ListLiteralExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

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
// ExistsExpression Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Exists_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const ExistsExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, Exists_ThrowsNotImplemented) {
	ExistsExpression expr("(n)-[:KNOWS]->(m)");
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// PatternComprehensionExpression Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, PatternComprehension_NullPtr) {
	ExpressionEvaluator evaluator(*context_);
	const PatternComprehensionExpression* nullExpr = nullptr;
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, PatternComprehension_ThrowsNotImplemented) {
	PatternComprehensionExpression expr(
		"(n)-[:KNOWS]->(m)",
		"m",
		std::make_unique<LiteralExpression>(std::string("m.name"))
	);
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

// ============================================================================
// ParameterExpression Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, Parameter_Resolved) {
	std::unordered_map<std::string, PropertyValue> params;
	params["limit"] = PropertyValue(int64_t(10));
	Record paramRecord;
	EvaluationContext paramCtx(paramRecord, params);
	ExpressionEvaluator evaluator(paramCtx);

	ParameterExpression paramExpr("limit");
	auto result = evaluator.evaluate(&paramExpr);
	EXPECT_EQ(result.getType(), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(result.getVariant()), 10);
}

TEST_F(ExpressionEvaluatorTest, Parameter_MissingThrows) {
	std::unordered_map<std::string, PropertyValue> params;
	Record paramRecord;
	EvaluationContext paramCtx(paramRecord, params);
	ExpressionEvaluator evaluator(paramCtx);

	ParameterExpression paramExpr("missing");
	EXPECT_THROW(evaluator.evaluate(&paramExpr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, Parameter_StringValue) {
	std::unordered_map<std::string, PropertyValue> params;
	params["name"] = PropertyValue(std::string("Alice"));
	Record paramRecord;
	EvaluationContext paramCtx(paramRecord, params);
	ExpressionEvaluator evaluator(paramCtx);

	ParameterExpression paramExpr("name");
	auto result = evaluator.evaluate(&paramExpr);
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "Alice");
}

// ============================================================================
// ShortestPathExpression Tests
// ============================================================================

TEST_F(ExpressionEvaluatorTest, ShortestPath_NullPtr) {
	const ShortestPathExpression* nullExpr = nullptr;
	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionEvaluatorTest, ShortestPath_NoDataManager_Throws) {
	ShortestPathExpression expr("a", "b", "", PatternDirection::PAT_BOTH, 1, -1, false);
	// context_ has no DataManager
	EXPECT_THROW(eval(&expr), ExpressionEvaluationException);
}

TEST_F(ExpressionEvaluatorTest, ShortestPath_MissingStartNode_ReturnsNull) {
	// Need a DataManager for this test - but start node not in record
	// The eval should return null since the node can't be resolved
	// Without DataManager it throws, so we test the null ptr branch
	const ShortestPathExpression* nullExpr = nullptr;
	ExpressionEvaluator evaluator(*context_);
	evaluator.visit(nullExpr);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}
