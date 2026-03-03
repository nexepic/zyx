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

	// Track which visit methods were called
	bool visitedLiteral = false;
	bool visitedVarRef = false;
	bool visitedBinary = false;
	bool visitedUnary = false;
	bool visitedFunction = false;
	bool visitedCase = false;
	bool visitedIn = false;
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
	auto expr = std::make_unique<BinaryOpExpression>(std::move(left), BinaryOperatorType::ADD, std::move(right));
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
	BinaryOpExpression original(std::move(left), BinaryOperatorType::ADD, std::move(right));

	auto cloned = original.clone();
	auto* clonedBinary = dynamic_cast<BinaryOpExpression*>(cloned.get());

	ASSERT_NE(clonedBinary, nullptr);
	EXPECT_EQ(clonedBinary->getOperator(), BinaryOperatorType::ADD);

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
