/**
 * @file test_Expressions_Coverage.cpp
 * @brief Coverage tests for expressions module to achieve 95%+ coverage
 * @date 2026/03/03
 */

#include <gtest/gtest.h>
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/execution/Record.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

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
	auto expr = std::make_unique<UnaryOpExpression>(UnaryOperatorType::MINUS, std::move(operand));
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
	auto expr = std::make_unique<ListComprehensionExpression>("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::FILTER);
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
	UnaryOpExpression original(UnaryOperatorType::MINUS, std::move(operand));

	auto cloned = original.clone();
	auto* clonedUnary = dynamic_cast<UnaryOpExpression*>(cloned.get());

	ASSERT_NE(clonedUnary, nullptr);
	EXPECT_EQ(clonedUnary->getOperator(), UnaryOperatorType::MINUS);

	std::string originalStr = original.toString();
	std::string clonedStr = clonedUnary->toString();
	EXPECT_EQ(originalStr, clonedStr);
	EXPECT_EQ(clonedStr, "-(5)");
}

TEST_F(ExpressionsCoverageTest, UnaryOpExpression_NOT_Operator) {
	// Test UnaryOpExpression with NOT operator
	{
		auto operand = std::make_unique<LiteralExpression>(true);
		UnaryOpExpression expr(UnaryOperatorType::NOT, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_FALSE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	{
		auto operand = std::make_unique<LiteralExpression>(false);
		UnaryOpExpression expr(UnaryOperatorType::NOT, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_TRUE(std::get<bool>(evaluator.getResult().getVariant()));
	}

	// Test NOT with NULL propagation
	{
		auto operand = std::make_unique<LiteralExpression>();
		UnaryOpExpression expr(UnaryOperatorType::NOT, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
	}
}

TEST_F(ExpressionsCoverageTest, UnaryOpExpression_MINUS_Variants) {
	// Test UnaryOpExpression MINUS with different types
	{
		auto operand = std::make_unique<LiteralExpression>(int64_t(42));
		UnaryOpExpression expr(UnaryOperatorType::MINUS, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<double>(evaluator.getResult().getVariant()), -42.0);
	}

	{
		auto operand = std::make_unique<LiteralExpression>(3.14);
		UnaryOpExpression expr(UnaryOperatorType::MINUS, std::move(operand));
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		EXPECT_EQ(std::get<double>(evaluator.getResult().getVariant()), -3.14);
	}

	// Test MINUS with NULL propagation
	{
		auto operand = std::make_unique<LiteralExpression>();
		UnaryOpExpression expr(UnaryOperatorType::MINUS, std::move(operand));
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

TEST_F(ExpressionsCoverageTest, SubstringFunction_NegativeLength) {
	// Test SubstringFunction with negative length parameter (hits line 175-177)
	auto& registry = FunctionRegistry::getInstance();
	const ScalarFunction* func = registry.lookupScalarFunction("substring");
	ASSERT_NE(func, nullptr);

	std::vector<PropertyValue> args;
	args.push_back(PropertyValue(std::string("hello")));
	args.push_back(PropertyValue(int64_t(1))); // start at position 1
	args.push_back(PropertyValue(int64_t(-2))); // negative length (from end)

	PropertyValue result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::STRING);
	// "ello" with negative length -2 should return "el" (last 2 chars from position 1)
	EXPECT_EQ(std::get<std::string>(result.getVariant()), "el");
}

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
		EXPECT_EQ(expr.getExpressionType(), ExpressionType::BINARY_OP);
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
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::FILTER);
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
		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), nullptr, ListComprehensionExpression::ComprehensionType::FILTER);
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
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, std::move(mapExpr), ListComprehensionExpression::ComprehensionType::EXTRACT);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 3ULL);
		EXPECT_EQ(std::get<double>(result[0].getVariant()), 2.0);
		EXPECT_EQ(std::get<double>(result[1].getVariant()), 4.0);
		EXPECT_EQ(std::get<double>(result[2].getVariant()), 6.0);
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

		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), std::move(mapExpr), ListComprehensionExpression::ComprehensionType::EXTRACT);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 3ULL); // 3, 4, 5
		EXPECT_EQ(std::get<double>(result[0].getVariant()), 9.0);
		EXPECT_EQ(std::get<double>(result[1].getVariant()), 12.0);
		EXPECT_EQ(std::get<double>(result[2].getVariant()), 15.0);
	}

	// REDUCE - Currently returns list like FILTER (REDUCE not fully implemented)
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("total", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::REDUCE);
		EXPECT_EQ(expr.toString(), "reduce(total = [1, 2, 3])");
	}

	// Empty list
	{
		std::vector<PropertyValue> emptyList;
		auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(emptyList));
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::FILTER);
		ExpressionEvaluator evaluator(*context_);
		expr.accept(evaluator);
		const auto& result = evaluator.getResult().getList();
		EXPECT_EQ(result.size(), 0ULL);
	}

	// Test toString variations - FILTER without WHERE
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::FILTER);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3]]");
	}

	// FILTER with WHERE
	{
		auto listExpr = createList({1, 2, 3});
		auto whereExpr = std::make_unique<LiteralExpression>(true);
		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), nullptr, ListComprehensionExpression::ComprehensionType::FILTER);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3] WHERE true]");
	}

	// EXTRACT without MAP (edge case - just filters elements)
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::EXTRACT);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3]]");
	}

	// EXTRACT with MAP
	{
		auto listExpr = createList({1, 2, 3});
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, std::move(mapExpr), ListComprehensionExpression::ComprehensionType::EXTRACT);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3] | 42]");
	}

	// EXTRACT with WHERE and MAP
	{
		auto listExpr = createList({1, 2, 3});
		auto whereExpr = std::make_unique<LiteralExpression>(true);
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), std::move(mapExpr), ListComprehensionExpression::ComprehensionType::EXTRACT);
		EXPECT_EQ(expr.toString(), "[x IN [1, 2, 3] WHERE true | 42]");
	}

	// REDUCE
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("total", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::REDUCE);
		EXPECT_EQ(expr.toString(), "reduce(total = [1, 2, 3])");
	}

	// REDUCE with MAP
	{
		auto listExpr = createList({1, 2, 3});
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(0));
		ListComprehensionExpression expr("total", std::move(listExpr), nullptr, std::move(mapExpr), ListComprehensionExpression::ComprehensionType::REDUCE);
		EXPECT_EQ(expr.toString(), "reduce(total = [1, 2, 3] | 0)");
	}

	// Test clone
	{
		auto listExpr = createList({1, 2, 3});
		auto whereExpr = std::make_unique<LiteralExpression>(true);
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
		ListComprehensionExpression original("x", std::move(listExpr), std::move(whereExpr), std::move(mapExpr), ListComprehensionExpression::ComprehensionType::EXTRACT);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "[x IN [1, 2, 3] WHERE true | 42]");
	}

	// Test clone FILTER without WHERE
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression original("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::FILTER);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "[x IN [1, 2, 3]]");
	}

	// Test clone REDUCE
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression original("total", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::REDUCE);
		auto cloned = original.clone();
		ASSERT_NE(cloned, nullptr);
		EXPECT_EQ(cloned->toString(), "reduce(total = [1, 2, 3])");
	}

	// Test getters
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::FILTER);
		EXPECT_EQ(expr.getVariable(), "x");
		EXPECT_EQ(expr.getType(), ListComprehensionExpression::ComprehensionType::FILTER);
		EXPECT_NE(expr.getListExpr(), nullptr);
		EXPECT_EQ(expr.getWhereExpr(), nullptr);
		EXPECT_EQ(expr.getMapExpr(), nullptr);
	}

	{
		auto listExpr = createList({1, 2, 3});
		auto whereExpr = std::make_unique<LiteralExpression>(true);
		auto mapExpr = std::make_unique<LiteralExpression>(int64_t(42));
		ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), std::move(mapExpr), ListComprehensionExpression::ComprehensionType::EXTRACT);
		EXPECT_NE(expr.getWhereExpr(), nullptr);
		EXPECT_NE(expr.getMapExpr(), nullptr);
	}

	// Test getExpressionType
	{
		auto listExpr = createList({1, 2, 3});
		ListComprehensionExpression expr("x", std::move(listExpr), nullptr, nullptr, ListComprehensionExpression::ComprehensionType::FILTER);
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
	EXPECT_THROW(varExpr->accept(evaluator), UndefinedVariableException);
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
	EXPECT_THROW(funcExpr->accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, DivisionByZero_Integer) {
	// Test integer division by zero throws exception
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(expr.accept(evaluator), DivisionByZeroException);
}

TEST_F(ExpressionsCoverageTest, DivisionByZero_Double) {
	// Test double division by zero throws exception
	auto left = std::make_unique<LiteralExpression>(10.0);
	auto right = std::make_unique<LiteralExpression>(0.0);
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_DIVIDE, std::move(right));
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(expr.accept(evaluator), DivisionByZeroException);
}

TEST_F(ExpressionsCoverageTest, ModuloByZero) {
	// Test modulo by zero throws exception
	auto left = std::make_unique<LiteralExpression>(int64_t(10));
	auto right = std::make_unique<LiteralExpression>(int64_t(0));
	BinaryOpExpression expr(std::move(left), BinaryOperatorType::BOP_MODULO, std::move(right));
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(expr.accept(evaluator), DivisionByZeroException);
}

TEST_F(ExpressionsCoverageTest, SliceNonListValue) {
	// Test slicing non-list value throws exception
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	auto indexExpr = std::make_unique<LiteralExpression>(int64_t(0));
	ListSliceExpression expr(std::move(nonListExpr), std::move(indexExpr), nullptr, false);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, SliceMissingIndex) {
	// Test slice with missing start index throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	ListSliceExpression expr(std::move(listExpr), nullptr, nullptr, false);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, SliceWithNonIntegerIndex) {
	// Test slice with string index throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto indexExpr = std::make_unique<LiteralExpression>(std::string("not_a_number"));
	ListSliceExpression expr(std::move(listExpr), std::move(indexExpr), nullptr, false);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, SliceWithNonIntegerStartIndex) {
	// Test slice with string start index throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto startExpr = std::make_unique<LiteralExpression>(std::string("not_a_number"));
	auto endExpr = std::make_unique<LiteralExpression>(int64_t(2));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, SliceWithNonIntegerEndIndex) {
	// Test slice with string end index throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto startExpr = std::make_unique<LiteralExpression>(int64_t(0));
	auto endExpr = std::make_unique<LiteralExpression>(std::string("not_a_number"));
	ListSliceExpression expr(std::move(listExpr), std::move(startExpr), std::move(endExpr), true);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, ListComprehensionNonListValue) {
	// Test list comprehension on non-list value throws exception
	auto nonListExpr = std::make_unique<LiteralExpression>(int64_t(42));
	ListComprehensionExpression expr("x", std::move(nonListExpr), nullptr, nullptr,
		ListComprehensionExpression::ComprehensionType::FILTER);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(expr.accept(evaluator), ExpressionEvaluationException);
}

TEST_F(ExpressionsCoverageTest, ListComprehensionNonBooleanWhere) {
	// Test list comprehension with non-boolean WHERE clause throws exception
	std::vector<PropertyValue> listVec = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2)), PropertyValue(int64_t(3))};
	auto listExpr = std::make_unique<ListLiteralExpression>(PropertyValue(listVec));
	auto whereExpr = std::make_unique<LiteralExpression>(int64_t(42)); // Non-boolean
	ListComprehensionExpression expr("x", std::move(listExpr), std::move(whereExpr), nullptr,
		ListComprehensionExpression::ComprehensionType::FILTER);
	ExpressionEvaluator evaluator(*context_);
	EXPECT_THROW(expr.accept(evaluator), ExpressionEvaluationException);
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
	UnaryOpExpression expr(UnaryOperatorType::MINUS, std::move(nullExpr));
	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(ExpressionsCoverageTest, UnaryNotWithNull) {
	// Test unary NOT with null propagates null
	auto nullExpr = std::make_unique<LiteralExpression>();
	UnaryOpExpression expr(UnaryOperatorType::NOT, std::move(nullExpr));
	ExpressionEvaluator evaluator(*context_);
	expr.accept(evaluator);
	EXPECT_EQ(evaluator.getResult().getType(), PropertyType::NULL_TYPE);
}
