/**
 * @file test_ExpressionClone.cpp
 * @brief Tests for clone() and toString() methods across all Expression types,
 *        plus utility functions (toString for operators, isArithmeticOperator, etc.).
 */

#include <gtest/gtest.h>
#include "graph/query/expressions/Expression.hpp"

using namespace graph::query::expressions;

// ============================================================================
// LiteralExpression clone and toString
// ============================================================================

TEST(ExpressionClone, LiteralNull) {
	LiteralExpression expr;
	EXPECT_TRUE(expr.isNull());
	EXPECT_EQ(expr.toString(), "null");

	auto cloned = expr.clone();
	ASSERT_NE(cloned, nullptr);
	auto *lit = dynamic_cast<LiteralExpression *>(cloned.get());
	ASSERT_NE(lit, nullptr);
	EXPECT_TRUE(lit->isNull());
	EXPECT_EQ(lit->toString(), "null");
}

TEST(ExpressionClone, LiteralBoolTrue) {
	LiteralExpression expr(true);
	EXPECT_TRUE(expr.isBoolean());
	EXPECT_TRUE(expr.getBooleanValue());
	EXPECT_EQ(expr.toString(), "true");

	auto cloned = expr.clone();
	auto *lit = dynamic_cast<LiteralExpression *>(cloned.get());
	ASSERT_NE(lit, nullptr);
	EXPECT_TRUE(lit->isBoolean());
	EXPECT_TRUE(lit->getBooleanValue());
	EXPECT_EQ(lit->toString(), "true");
}

TEST(ExpressionClone, LiteralBoolFalse) {
	LiteralExpression expr(false);
	EXPECT_EQ(expr.toString(), "false");

	auto cloned = expr.clone();
	auto *lit = dynamic_cast<LiteralExpression *>(cloned.get());
	ASSERT_NE(lit, nullptr);
	EXPECT_FALSE(lit->getBooleanValue());
	EXPECT_EQ(lit->toString(), "false");
}

TEST(ExpressionClone, LiteralInteger) {
	LiteralExpression expr(int64_t(42));
	EXPECT_TRUE(expr.isInteger());
	EXPECT_EQ(expr.getIntegerValue(), 42);
	EXPECT_EQ(expr.toString(), "42");

	auto cloned = expr.clone();
	auto *lit = dynamic_cast<LiteralExpression *>(cloned.get());
	ASSERT_NE(lit, nullptr);
	EXPECT_TRUE(lit->isInteger());
	EXPECT_EQ(lit->getIntegerValue(), 42);
}

TEST(ExpressionClone, LiteralDouble) {
	LiteralExpression expr(3.14);
	EXPECT_TRUE(expr.isDouble());
	EXPECT_DOUBLE_EQ(expr.getDoubleValue(), 3.14);
	// toString uses ostringstream, so just check it's non-empty
	EXPECT_FALSE(expr.toString().empty());

	auto cloned = expr.clone();
	auto *lit = dynamic_cast<LiteralExpression *>(cloned.get());
	ASSERT_NE(lit, nullptr);
	EXPECT_TRUE(lit->isDouble());
	EXPECT_DOUBLE_EQ(lit->getDoubleValue(), 3.14);
}

TEST(ExpressionClone, LiteralString) {
	LiteralExpression expr(std::string("hello"));
	EXPECT_TRUE(expr.isString());
	EXPECT_EQ(expr.getStringValue(), "hello");
	EXPECT_EQ(expr.toString(), "'hello'");

	auto cloned = expr.clone();
	auto *lit = dynamic_cast<LiteralExpression *>(cloned.get());
	ASSERT_NE(lit, nullptr);
	EXPECT_TRUE(lit->isString());
	EXPECT_EQ(lit->getStringValue(), "hello");
	EXPECT_EQ(lit->toString(), "'hello'");
}

// ============================================================================
// VariableReferenceExpression clone and toString
// ============================================================================

TEST(ExpressionClone, VariableReferenceSimple) {
	VariableReferenceExpression expr("n");
	EXPECT_EQ(expr.getVariableName(), "n");
	EXPECT_FALSE(expr.hasProperty());
	EXPECT_EQ(expr.toString(), "n");

	auto cloned = expr.clone();
	auto *ref = dynamic_cast<VariableReferenceExpression *>(cloned.get());
	ASSERT_NE(ref, nullptr);
	EXPECT_EQ(ref->getVariableName(), "n");
	EXPECT_FALSE(ref->hasProperty());
	EXPECT_EQ(ref->toString(), "n");
}

TEST(ExpressionClone, VariableReferenceWithProperty) {
	VariableReferenceExpression expr("n", "age");
	EXPECT_TRUE(expr.hasProperty());
	EXPECT_EQ(expr.getPropertyName(), "age");
	EXPECT_EQ(expr.toString(), "n.age");

	auto cloned = expr.clone();
	auto *ref = dynamic_cast<VariableReferenceExpression *>(cloned.get());
	ASSERT_NE(ref, nullptr);
	EXPECT_TRUE(ref->hasProperty());
	EXPECT_EQ(ref->getVariableName(), "n");
	EXPECT_EQ(ref->getPropertyName(), "age");
	EXPECT_EQ(ref->toString(), "n.age");
}

// ============================================================================
// BinaryOpExpression clone and toString
// ============================================================================

TEST(ExpressionClone, BinaryOpCloneAndToString) {
	auto left = std::make_unique<LiteralExpression>(int64_t(1));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));

	EXPECT_EQ(expr.getOperator(), BinaryOperatorType::BOP_ADD);
	EXPECT_EQ(expr.toString(), "(1 + 2)");

	auto cloned = expr.clone();
	auto *bin = dynamic_cast<BinaryOpExpression *>(cloned.get());
	ASSERT_NE(bin, nullptr);
	EXPECT_EQ(bin->getOperator(), BinaryOperatorType::BOP_ADD);
	EXPECT_EQ(bin->toString(), "(1 + 2)");

	// Verify deep copy: original children still valid
	EXPECT_NE(expr.getLeft(), nullptr);
	EXPECT_NE(expr.getRight(), nullptr);
}

TEST(ExpressionClone, BinaryOpNestedClone) {
	// (1 + 2) * 3
	auto one = std::make_unique<LiteralExpression>(int64_t(1));
	auto two = std::make_unique<LiteralExpression>(int64_t(2));
	auto add = std::make_unique<BinaryOpExpression>(std::move(one), BinaryOperatorType::BOP_ADD, std::move(two));
	auto three = std::make_unique<LiteralExpression>(int64_t(3));
	BinaryOpExpression expr(std::move(add), BinaryOperatorType::BOP_MULTIPLY, std::move(three));

	EXPECT_EQ(expr.toString(), "((1 + 2) * 3)");

	auto cloned = expr.clone();
	EXPECT_EQ(cloned->toString(), "((1 + 2) * 3)");
}

// ============================================================================
// UnaryOpExpression clone and toString
// ============================================================================

TEST(ExpressionClone, UnaryOpMinusClone) {
	auto operand = std::make_unique<LiteralExpression>(int64_t(5));
	UnaryOpExpression expr(UnaryOperatorType::UOP_MINUS, std::move(operand));

	EXPECT_EQ(expr.getOperator(), UnaryOperatorType::UOP_MINUS);
	EXPECT_EQ(expr.toString(), "-(5)");

	auto cloned = expr.clone();
	auto *unary = dynamic_cast<UnaryOpExpression *>(cloned.get());
	ASSERT_NE(unary, nullptr);
	EXPECT_EQ(unary->getOperator(), UnaryOperatorType::UOP_MINUS);
	EXPECT_EQ(unary->toString(), "-(5)");
}

TEST(ExpressionClone, UnaryOpNotClone) {
	auto operand = std::make_unique<LiteralExpression>(true);
	UnaryOpExpression expr(UnaryOperatorType::UOP_NOT, std::move(operand));

	EXPECT_EQ(expr.toString(), "NOT(true)");

	auto cloned = expr.clone();
	EXPECT_EQ(cloned->toString(), "NOT(true)");
}

// ============================================================================
// FunctionCallExpression clone and toString
// ============================================================================

TEST(ExpressionClone, FunctionCallNoArgs) {
	std::vector<std::unique_ptr<Expression>> args;
	FunctionCallExpression expr("count", std::move(args));

	EXPECT_EQ(expr.getFunctionName(), "count");
	EXPECT_EQ(expr.getArgumentCount(), 0u);
	EXPECT_EQ(expr.toString(), "count()");

	auto cloned = expr.clone();
	auto *fn = dynamic_cast<FunctionCallExpression *>(cloned.get());
	ASSERT_NE(fn, nullptr);
	EXPECT_EQ(fn->getFunctionName(), "count");
	EXPECT_EQ(fn->getArgumentCount(), 0u);
	EXPECT_EQ(fn->toString(), "count()");
}

TEST(ExpressionClone, FunctionCallOneArg) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<VariableReferenceExpression>("n", "salary"));
	FunctionCallExpression expr("sum", std::move(args));

	EXPECT_EQ(expr.getArgumentCount(), 1u);
	EXPECT_EQ(expr.toString(), "sum(n.salary)");

	auto cloned = expr.clone();
	auto *fn = dynamic_cast<FunctionCallExpression *>(cloned.get());
	ASSERT_NE(fn, nullptr);
	EXPECT_EQ(fn->getArgumentCount(), 1u);
	EXPECT_EQ(fn->toString(), "sum(n.salary)");
}

TEST(ExpressionClone, FunctionCallMultipleArgs) {
	std::vector<std::unique_ptr<Expression>> args;
	args.push_back(std::make_unique<LiteralExpression>(int64_t(1)));
	args.push_back(std::make_unique<LiteralExpression>(int64_t(2)));
	args.push_back(std::make_unique<LiteralExpression>(int64_t(3)));
	FunctionCallExpression expr("coalesce", std::move(args));

	EXPECT_EQ(expr.getArgumentCount(), 3u);
	EXPECT_EQ(expr.toString(), "coalesce(1, 2, 3)");

	auto cloned = expr.clone();
	auto *fn = dynamic_cast<FunctionCallExpression *>(cloned.get());
	ASSERT_NE(fn, nullptr);
	EXPECT_EQ(fn->getArgumentCount(), 3u);
	EXPECT_EQ(fn->toString(), "coalesce(1, 2, 3)");
}

// ============================================================================
// InExpression clone and toString
// ============================================================================

TEST(ExpressionClone, InExpressionCloneAndToString) {
	auto value = std::make_unique<VariableReferenceExpression>("n", "age");
	std::vector<graph::PropertyValue> listValues;
	listValues.emplace_back(int64_t(25));
	listValues.emplace_back(int64_t(30));
	listValues.emplace_back(int64_t(35));

	InExpression expr(std::move(value), std::move(listValues));

	std::string str = expr.toString();
	EXPECT_NE(str.find("n.age"), std::string::npos);
	EXPECT_NE(str.find("IN"), std::string::npos);
	EXPECT_NE(str.find("25"), std::string::npos);
	EXPECT_NE(str.find("30"), std::string::npos);
	EXPECT_NE(str.find("35"), std::string::npos);

	auto cloned = expr.clone();
	auto *in = dynamic_cast<InExpression *>(cloned.get());
	ASSERT_NE(in, nullptr);
	EXPECT_EQ(in->getListValues().size(), 3u);
	EXPECT_EQ(in->toString(), expr.toString());
}

TEST(ExpressionClone, InExpressionEmptyList) {
	auto value = std::make_unique<LiteralExpression>(int64_t(1));
	std::vector<graph::PropertyValue> listValues;

	InExpression expr(std::move(value), std::move(listValues));

	EXPECT_EQ(expr.toString(), "(1 IN [])");

	auto cloned = expr.clone();
	auto *in = dynamic_cast<InExpression *>(cloned.get());
	ASSERT_NE(in, nullptr);
	EXPECT_EQ(in->getListValues().size(), 0u);
}

// ============================================================================
// CaseExpression clone and toString
// ============================================================================

TEST(ExpressionClone, CaseExpressionSimpleClone) {
	// Simple CASE: CASE x WHEN 1 THEN 'one' ELSE 'other' END
	auto compExpr = std::make_unique<VariableReferenceExpression>("x");
	CaseExpression expr(std::move(compExpr));
	expr.addBranch(
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(std::string("one"))
	);
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("other")));

	EXPECT_TRUE(expr.isSimpleCase());
	std::string str = expr.toString();
	EXPECT_NE(str.find("CASE x"), std::string::npos);
	EXPECT_NE(str.find("WHEN 1 THEN 'one'"), std::string::npos);
	EXPECT_NE(str.find("ELSE 'other'"), std::string::npos);
	EXPECT_NE(str.find("END"), std::string::npos);

	auto cloned = expr.clone();
	auto *caseExpr = dynamic_cast<CaseExpression *>(cloned.get());
	ASSERT_NE(caseExpr, nullptr);
	EXPECT_TRUE(caseExpr->isSimpleCase());
	EXPECT_EQ(caseExpr->getBranches().size(), 1u);
	EXPECT_EQ(caseExpr->toString(), str);
}

TEST(ExpressionClone, CaseExpressionSearchedClone) {
	// Searched CASE: CASE WHEN n.age > 18 THEN 'adult' ELSE 'minor' END
	CaseExpression expr;
	EXPECT_FALSE(expr.isSimpleCase());

	auto whenExpr = std::make_unique<BinaryOpExpression>(
		std::make_unique<VariableReferenceExpression>("n", "age"),
		BinaryOperatorType::BOP_GREATER,
		std::make_unique<LiteralExpression>(int64_t(18))
	);
	expr.addBranch(
		std::move(whenExpr),
		std::make_unique<LiteralExpression>(std::string("adult"))
	);
	expr.setElseExpression(std::make_unique<LiteralExpression>(std::string("minor")));

	std::string str = expr.toString();
	// Searched CASE has no comparison expression after CASE
	EXPECT_EQ(str.substr(0, 9), "CASE WHEN");
	EXPECT_NE(str.find("ELSE 'minor'"), std::string::npos);
	EXPECT_NE(str.find("END"), std::string::npos);

	auto cloned = expr.clone();
	auto *caseExpr = dynamic_cast<CaseExpression *>(cloned.get());
	ASSERT_NE(caseExpr, nullptr);
	EXPECT_FALSE(caseExpr->isSimpleCase());
	EXPECT_EQ(caseExpr->getBranches().size(), 1u);
	EXPECT_NE(caseExpr->getElseExpression(), nullptr);
	EXPECT_EQ(caseExpr->toString(), str);
}

TEST(ExpressionClone, CaseExpressionMultipleBranches) {
	CaseExpression expr;
	expr.addBranch(
		std::make_unique<LiteralExpression>(true),
		std::make_unique<LiteralExpression>(int64_t(1))
	);
	expr.addBranch(
		std::make_unique<LiteralExpression>(false),
		std::make_unique<LiteralExpression>(int64_t(2))
	);

	EXPECT_EQ(expr.getBranches().size(), 2u);

	auto cloned = expr.clone();
	auto *caseExpr = dynamic_cast<CaseExpression *>(cloned.get());
	ASSERT_NE(caseExpr, nullptr);
	EXPECT_EQ(caseExpr->getBranches().size(), 2u);
}

TEST(ExpressionClone, CaseExpressionDefaultElse) {
	// Default constructor sets elseExpression to a LiteralExpression (null)
	CaseExpression expr;
	expr.addBranch(
		std::make_unique<LiteralExpression>(true),
		std::make_unique<LiteralExpression>(int64_t(1))
	);
	// Don't set else, it defaults to null

	std::string str = expr.toString();
	EXPECT_NE(str.find("ELSE null"), std::string::npos);
}

// ============================================================================
// toString(BinaryOperatorType) - all operators
// ============================================================================

TEST(ExpressionUtility, BinaryOperatorToStringAllTypes) {
	EXPECT_EQ(toString(BinaryOperatorType::BOP_ADD), "+");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_SUBTRACT), "-");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_MULTIPLY), "*");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_DIVIDE), "/");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_MODULO), "%");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_POWER), "^");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_EQUAL), "=");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_NOT_EQUAL), "<>");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_LESS), "<");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_GREATER), ">");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_LESS_EQUAL), "<=");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_GREATER_EQUAL), ">=");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_IN), "IN");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_STARTS_WITH), "STARTS WITH");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_ENDS_WITH), "ENDS WITH");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_CONTAINS), "CONTAINS");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_AND), "AND");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_OR), "OR");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_XOR), "XOR");
}

TEST(ExpressionUtility, BinaryOperatorToStringDefaultCase) {
	// Cast an invalid value to trigger the default fallthrough returning "?"
	auto invalid = static_cast<BinaryOperatorType>(999);
	EXPECT_EQ(toString(invalid), "?");
}

// ============================================================================
// toString(UnaryOperatorType) - all operators
// ============================================================================

TEST(ExpressionUtility, UnaryOperatorToStringAllTypes) {
	EXPECT_EQ(toString(UnaryOperatorType::UOP_MINUS), "-");
	EXPECT_EQ(toString(UnaryOperatorType::UOP_NOT), "NOT");
}

TEST(ExpressionUtility, UnaryOperatorToStringDefaultCase) {
	// Cast an invalid value to trigger the default fallthrough returning "?"
	auto invalid = static_cast<UnaryOperatorType>(999);
	EXPECT_EQ(toString(invalid), "?");
}

// ============================================================================
// isArithmeticOperator
// ============================================================================

TEST(ExpressionUtility, IsArithmeticOperator) {
	// True cases
	EXPECT_TRUE(isArithmeticOperator(BinaryOperatorType::BOP_ADD));
	EXPECT_TRUE(isArithmeticOperator(BinaryOperatorType::BOP_SUBTRACT));
	EXPECT_TRUE(isArithmeticOperator(BinaryOperatorType::BOP_MULTIPLY));
	EXPECT_TRUE(isArithmeticOperator(BinaryOperatorType::BOP_DIVIDE));
	EXPECT_TRUE(isArithmeticOperator(BinaryOperatorType::BOP_MODULO));
	EXPECT_TRUE(isArithmeticOperator(BinaryOperatorType::BOP_POWER));

	// False cases
	EXPECT_FALSE(isArithmeticOperator(BinaryOperatorType::BOP_EQUAL));
	EXPECT_FALSE(isArithmeticOperator(BinaryOperatorType::BOP_AND));
	EXPECT_FALSE(isArithmeticOperator(BinaryOperatorType::BOP_OR));
	EXPECT_FALSE(isArithmeticOperator(BinaryOperatorType::BOP_IN));
	EXPECT_FALSE(isArithmeticOperator(BinaryOperatorType::BOP_CONTAINS));
	EXPECT_FALSE(isArithmeticOperator(BinaryOperatorType::BOP_STARTS_WITH));
}

// ============================================================================
// isComparisonOperator
// ============================================================================

TEST(ExpressionUtility, IsComparisonOperator) {
	// True cases
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_EQUAL));
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_NOT_EQUAL));
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_LESS));
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_GREATER));
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_LESS_EQUAL));
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_GREATER_EQUAL));
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_STARTS_WITH));
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_ENDS_WITH));
	EXPECT_TRUE(isComparisonOperator(BinaryOperatorType::BOP_CONTAINS));

	// False cases
	EXPECT_FALSE(isComparisonOperator(BinaryOperatorType::BOP_ADD));
	EXPECT_FALSE(isComparisonOperator(BinaryOperatorType::BOP_AND));
	EXPECT_FALSE(isComparisonOperator(BinaryOperatorType::BOP_OR));
	EXPECT_FALSE(isComparisonOperator(BinaryOperatorType::BOP_IN));
	EXPECT_FALSE(isComparisonOperator(BinaryOperatorType::BOP_MODULO));
}

// ============================================================================
// isLogicalOperator
// ============================================================================

TEST(ExpressionUtility, IsLogicalOperator) {
	// True cases
	EXPECT_TRUE(isLogicalOperator(BinaryOperatorType::BOP_AND));
	EXPECT_TRUE(isLogicalOperator(BinaryOperatorType::BOP_OR));
	EXPECT_TRUE(isLogicalOperator(BinaryOperatorType::BOP_XOR));

	// False cases
	EXPECT_FALSE(isLogicalOperator(BinaryOperatorType::BOP_ADD));
	EXPECT_FALSE(isLogicalOperator(BinaryOperatorType::BOP_EQUAL));
	EXPECT_FALSE(isLogicalOperator(BinaryOperatorType::BOP_IN));
	EXPECT_FALSE(isLogicalOperator(BinaryOperatorType::BOP_CONTAINS));
	EXPECT_FALSE(isLogicalOperator(BinaryOperatorType::BOP_MULTIPLY));
}

// ============================================================================
// ExpressionType checks
// ============================================================================

TEST(ExpressionClone, ExpressionTypes) {
	LiteralExpression lit(int64_t(1));
	EXPECT_EQ(lit.getExpressionType(), ExpressionType::LITERAL);

	VariableReferenceExpression ref("n");
	EXPECT_EQ(ref.getExpressionType(), ExpressionType::PROPERTY_ACCESS);

	auto left = std::make_unique<LiteralExpression>(int64_t(1));
	auto right = std::make_unique<LiteralExpression>(int64_t(2));
	BinaryOpExpression bin(std::move(left), BinaryOperatorType::BOP_ADD, std::move(right));
	EXPECT_EQ(bin.getExpressionType(), ExpressionType::BINARY_OP);

	auto operand = std::make_unique<LiteralExpression>(int64_t(1));
	UnaryOpExpression unary(UnaryOperatorType::UOP_MINUS, std::move(operand));
	EXPECT_EQ(unary.getExpressionType(), ExpressionType::UNARY_OP);

	std::vector<std::unique_ptr<Expression>> args;
	FunctionCallExpression fn("f", std::move(args));
	EXPECT_EQ(fn.getExpressionType(), ExpressionType::FUNCTION_CALL);

	CaseExpression caseExpr;
	EXPECT_EQ(caseExpr.getExpressionType(), ExpressionType::CASE_EXPRESSION);

	auto val = std::make_unique<LiteralExpression>(int64_t(1));
	std::vector<graph::PropertyValue> list;
	InExpression inExpr(std::move(val), std::move(list));
	EXPECT_EQ(inExpr.getExpressionType(), ExpressionType::BINARY_OP);
}
