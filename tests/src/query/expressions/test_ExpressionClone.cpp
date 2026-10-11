/**
 * @file test_ExpressionClone.cpp
 * @brief Tests for clone() and toString() methods across all Expression types,
 *        plus utility functions (toString for operators, isArithmeticOperator, etc.).
 */

#include <gtest/gtest.h>
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/ListSliceExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ReduceExpression.hpp"

using namespace graph::query::expressions;

namespace {

class ExpressionVisitorProbe final : public ExpressionVisitor, public ConstExpressionVisitor {
public:
	void visit(LiteralExpression*) override { visitedLiteralMutable = true; }
	void visit(VariableReferenceExpression*) override { visitedVariableMutable = true; }
	void visit(BinaryOpExpression*) override { visitedBinaryMutable = true; }
	void visit(UnaryOpExpression*) override { visitedUnaryMutable = true; }
	void visit(FunctionCallExpression*) override { visitedFunctionMutable = true; }
	void visit(CaseExpression*) override { visitedCaseMutable = true; }
	void visit(InExpression*) override { visitedInMutable = true; }
	void visit(ListSliceExpression*) override { visitedListSliceMutable = true; }
	void visit(ListComprehensionExpression*) override { visitedListComprehensionMutable = true; }
	void visit(ListLiteralExpression*) override { visitedListLiteralMutable = true; }
	void visit(IsNullExpression*) override { visitedMutable = true; }
	void visit(QuantifierFunctionExpression*) override { visitedQuantifierMutable = true; }
	void visit(ExistsExpression*) override { visitedExistsMutable = true; }
	void visit(PatternComprehensionExpression*) override { visitedPatternComprehensionMutable = true; }
	void visit(ReduceExpression*) override { visitedReduceMutable = true; }
	void visit(ParameterExpression*) override {}
	void visit(ShortestPathExpression*) override {}
	void visit(MapProjectionExpression*) override {}

	void visit(const LiteralExpression*) override { visitedLiteralConst = true; }
	void visit(const VariableReferenceExpression*) override { visitedVariableConst = true; }
	void visit(const BinaryOpExpression*) override { visitedBinaryConst = true; }
	void visit(const UnaryOpExpression*) override { visitedUnaryConst = true; }
	void visit(const FunctionCallExpression*) override { visitedFunctionConst = true; }
	void visit(const CaseExpression*) override { visitedCaseConst = true; }
	void visit(const InExpression*) override { visitedInConst = true; }
	void visit(const ListSliceExpression*) override { visitedListSliceConst = true; }
	void visit(const ListComprehensionExpression*) override { visitedListComprehensionConst = true; }
	void visit(const ListLiteralExpression*) override { visitedListLiteralConst = true; }
	void visit(const IsNullExpression*) override { visitedConst = true; }
	void visit(const QuantifierFunctionExpression*) override { visitedQuantifierConst = true; }
	void visit(const ExistsExpression*) override { visitedExistsConst = true; }
	void visit(const PatternComprehensionExpression*) override { visitedPatternComprehensionConst = true; }
	void visit(const ReduceExpression*) override { visitedReduceConst = true; }
	void visit(const ParameterExpression*) override {}
	void visit(const ShortestPathExpression*) override {}
	void visit(const MapProjectionExpression*) override {}

	bool visitedMutable = false;
	bool visitedConst = false;
	bool visitedLiteralMutable = false;
	bool visitedLiteralConst = false;
	bool visitedVariableMutable = false;
	bool visitedVariableConst = false;
	bool visitedBinaryMutable = false;
	bool visitedBinaryConst = false;
	bool visitedUnaryMutable = false;
	bool visitedUnaryConst = false;
	bool visitedFunctionMutable = false;
	bool visitedFunctionConst = false;
	bool visitedCaseMutable = false;
	bool visitedCaseConst = false;
	bool visitedInMutable = false;
	bool visitedInConst = false;
	bool visitedListComprehensionMutable = false;
	bool visitedListComprehensionConst = false;
	bool visitedPatternComprehensionMutable = false;
	bool visitedPatternComprehensionConst = false;
	bool visitedExistsMutable = false;
	bool visitedExistsConst = false;
	bool visitedListLiteralMutable = false;
	bool visitedListLiteralConst = false;
	bool visitedListSliceMutable = false;
	bool visitedListSliceConst = false;
	bool visitedQuantifierMutable = false;
	bool visitedQuantifierConst = false;
	bool visitedReduceMutable = false;
	bool visitedReduceConst = false;
};

} // namespace

TEST(ExpressionClone, CoreExpressionTypesDispatchVisitorsAndPreserveOptionalChildren) {
	ExpressionVisitorProbe visitor;
	auto expectDispatch = [&visitor](Expression &expression, bool &mutableVisited, bool &constVisited) {
		expression.accept(static_cast<ExpressionVisitor &>(visitor));
		static_cast<const Expression &>(expression).accept(static_cast<ConstExpressionVisitor &>(visitor));
		EXPECT_TRUE(mutableVisited);
		EXPECT_TRUE(constVisited);
	};

	LiteralExpression literal(int64_t(7));
	expectDispatch(literal, visitor.visitedLiteralMutable, visitor.visitedLiteralConst);

	VariableReferenceExpression variable("node");
	expectDispatch(variable, visitor.visitedVariableMutable, visitor.visitedVariableConst);

	BinaryOpExpression binary(
			std::make_unique<LiteralExpression>(int64_t(1)), BinaryOperatorType::BOP_ADD,
			std::make_unique<LiteralExpression>(int64_t(2)));
	expectDispatch(binary, visitor.visitedBinaryMutable, visitor.visitedBinaryConst);

	UnaryOpExpression unary(UnaryOperatorType::UOP_MINUS, std::make_unique<LiteralExpression>(int64_t(1)));
	expectDispatch(unary, visitor.visitedUnaryMutable, visitor.visitedUnaryConst);

	FunctionCallExpression function("count", {});
	expectDispatch(function, visitor.visitedFunctionMutable, visitor.visitedFunctionConst);

	CaseExpression caseExpression;
	expectDispatch(caseExpression, visitor.visitedCaseMutable, visitor.visitedCaseConst);

	InExpression inExpression(
			std::make_unique<VariableReferenceExpression>("node"),
			std::make_unique<ListLiteralExpression>(graph::PropertyValue(std::vector<graph::PropertyValue>{
					graph::PropertyValue(int64_t(1)), graph::PropertyValue(int64_t(2))})));
	expectDispatch(inExpression, visitor.visitedInMutable, visitor.visitedInConst);
	EXPECT_TRUE(inExpression.hasDynamicList());
	EXPECT_EQ(inExpression.toString(), "(node IN [1, 2])");
	EXPECT_EQ(inExpression.clone()->toString(), inExpression.toString());

	BinaryOpExpression binaryWithoutChildren(nullptr, BinaryOperatorType::BOP_ADD, nullptr);
	auto binaryClone = binaryWithoutChildren.clone();
	auto *clonedBinary = dynamic_cast<BinaryOpExpression *>(binaryClone.get());
	ASSERT_NE(clonedBinary, nullptr);
	EXPECT_EQ(clonedBinary->getLeft(), nullptr);
	EXPECT_EQ(clonedBinary->getRight(), nullptr);

	UnaryOpExpression unaryWithoutOperand(UnaryOperatorType::UOP_NOT, nullptr);
	EXPECT_EQ(unaryWithoutOperand.toString(), "NOT(null)");
	auto unaryClone = unaryWithoutOperand.clone();
	auto *clonedUnary = dynamic_cast<UnaryOpExpression *>(unaryClone.get());
	ASSERT_NE(clonedUnary, nullptr);
	EXPECT_EQ(clonedUnary->getOperand(), nullptr);

	caseExpression.setElseExpression(nullptr);
	EXPECT_EQ(caseExpression.toString(), "CASE END");
	EXPECT_EQ(caseExpression.clone()->toString(), "CASE END");
	EXPECT_EQ(toString(BinaryOperatorType::BOP_REGEX_MATCH), "=~");
	EXPECT_EQ(toString(static_cast<BinaryOperatorType>(-1)), "?");
	EXPECT_EQ(toString(static_cast<UnaryOperatorType>(-1)), "?");
}

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

TEST(ExpressionClone, LiteralPropertyValuePreservesCompositeValues) {
	graph::PropertyValue::MapType map{{"answer", graph::PropertyValue(int64_t{42})}};
	LiteralExpression expr{graph::PropertyValue(map)};
	EXPECT_TRUE(expr.isPropertyValue());
	EXPECT_EQ(expr.getPropertyValue(), graph::PropertyValue(map));
	EXPECT_EQ(expr.toString(), "{answer: 42}");

	auto cloned = expr.clone();
	auto *literal = dynamic_cast<LiteralExpression *>(cloned.get());
	ASSERT_NE(literal, nullptr);
	EXPECT_TRUE(literal->isPropertyValue());
	EXPECT_EQ(literal->getPropertyValue(), graph::PropertyValue(map));
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
// IsNullExpression clone, visitors, and toString
// ============================================================================

TEST(ExpressionClone, IsNullExpressionClonesItsChildAndDispatchesBothVisitors) {
	IsNullExpression expression(std::make_unique<VariableReferenceExpression>("n", "name"), false);

	EXPECT_EQ(expression.getExpressionType(), ExpressionType::IS_NULL);
	EXPECT_EQ(expression.toString(), "n.name IS NULL");
	ASSERT_NE(expression.getExpression(), nullptr);

	ExpressionVisitorProbe visitor;
	expression.accept(static_cast<ExpressionVisitor&>(visitor));
	static_cast<const IsNullExpression&>(expression).accept(static_cast<ConstExpressionVisitor&>(visitor));
	EXPECT_TRUE(visitor.visitedMutable);
	EXPECT_TRUE(visitor.visitedConst);

	auto clone = expression.clone();
	auto* cloned = dynamic_cast<IsNullExpression*>(clone.get());
	ASSERT_NE(cloned, nullptr);
	ASSERT_NE(cloned->getExpression(), nullptr);
	EXPECT_NE(cloned->getExpression(), expression.getExpression());
	EXPECT_FALSE(cloned->isNegated());
	EXPECT_EQ(cloned->toString(), expression.toString());
}

TEST(ExpressionClone, IsNotNullExpressionHandlesAnAbsentChild) {
	IsNullExpression expression(nullptr, true);

	EXPECT_EQ(expression.toString(), " IS NOT NULL");
	EXPECT_EQ(expression.getExpression(), nullptr);
	EXPECT_TRUE(expression.isNegated());

	auto clone = expression.clone();
	auto* cloned = dynamic_cast<IsNullExpression*>(clone.get());
	ASSERT_NE(cloned, nullptr);
	EXPECT_EQ(cloned->getExpression(), nullptr);
	EXPECT_TRUE(cloned->isNegated());
	EXPECT_EQ(cloned->toString(), " IS NOT NULL");
}

// ============================================================================
// ListComprehensionExpression clone, visitors, and toString
// ============================================================================

TEST(ExpressionClone, ListComprehensionFilterPreservesItsPredicateAndDispatchesVisitors) {
	ListComprehensionExpression expression(
		"x",
		std::make_unique<VariableReferenceExpression>("values"),
		std::make_unique<BinaryOpExpression>(
			std::make_unique<VariableReferenceExpression>("x"),
			BinaryOperatorType::BOP_GREATER,
			std::make_unique<LiteralExpression>(int64_t(1))),
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER);

	EXPECT_EQ(expression.getExpressionType(), ExpressionType::LIST_COMPREHENSION);
	EXPECT_EQ(expression.toString(), "[x IN values WHERE (x > 1)]");

	ExpressionVisitorProbe visitor;
	expression.accept(static_cast<ExpressionVisitor&>(visitor));
	static_cast<const ListComprehensionExpression&>(expression).accept(
		static_cast<ConstExpressionVisitor&>(visitor));
	EXPECT_TRUE(visitor.visitedListComprehensionMutable);
	EXPECT_TRUE(visitor.visitedListComprehensionConst);

	auto clone = expression.clone();
	auto* cloned = dynamic_cast<ListComprehensionExpression*>(clone.get());
	ASSERT_NE(cloned, nullptr);
	ASSERT_NE(cloned->getListExpr(), nullptr);
	ASSERT_NE(cloned->getWhereExpr(), nullptr);
	EXPECT_EQ(cloned->getMapExpr(), nullptr);
	EXPECT_NE(cloned->getListExpr(), expression.getListExpr());
	EXPECT_NE(cloned->getWhereExpr(), expression.getWhereExpr());
	EXPECT_EQ(cloned->toString(), expression.toString());
}

TEST(ExpressionClone, ListComprehensionExtractAndReduceRenderOptionalClauses) {
	ListComprehensionExpression filterWithoutWhere(
		"x",
		std::make_unique<VariableReferenceExpression>("values"),
		nullptr,
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER);
	EXPECT_EQ(filterWithoutWhere.toString(), "[x IN values]");

	ListComprehensionExpression extract(
		"x",
		std::make_unique<VariableReferenceExpression>("values"),
		nullptr,
		std::make_unique<VariableReferenceExpression>("x"),
		ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
	EXPECT_EQ(extract.toString(), "[x IN values | x]");

	auto extractClone = extract.clone();
	auto* clonedExtract = dynamic_cast<ListComprehensionExpression*>(extractClone.get());
	ASSERT_NE(clonedExtract, nullptr);
	EXPECT_EQ(clonedExtract->getWhereExpr(), nullptr);
	ASSERT_NE(clonedExtract->getMapExpr(), nullptr);
	EXPECT_NE(clonedExtract->getMapExpr(), extract.getMapExpr());

	ListComprehensionExpression extractWithWhere(
		"x",
		std::make_unique<VariableReferenceExpression>("values"),
		std::make_unique<LiteralExpression>(true),
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
	EXPECT_EQ(extractWithWhere.toString(), "[x IN values WHERE true]");

	ListComprehensionExpression reduce(
		"total",
		std::make_unique<VariableReferenceExpression>("values"),
		nullptr,
		std::make_unique<VariableReferenceExpression>("total"),
		ListComprehensionExpression::ComprehensionType::COMP_REDUCE);
	EXPECT_EQ(reduce.toString(), "reduce(total = values | total)");

	ListComprehensionExpression reduceWithoutMap(
		"total",
		std::make_unique<VariableReferenceExpression>("values"),
		nullptr,
		nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_REDUCE);
	EXPECT_EQ(reduceWithoutMap.toString(), "reduce(total = values)");
}

TEST(ExpressionClone, ListComprehensionClonePreservesAbsentChildren) {
	ListComprehensionExpression expression(
		"x", nullptr, nullptr, nullptr,
		ListComprehensionExpression::ComprehensionType::COMP_FILTER);

	auto clone = expression.clone();
	auto* cloned = dynamic_cast<ListComprehensionExpression*>(clone.get());
	ASSERT_NE(cloned, nullptr);
	EXPECT_EQ(cloned->getListExpr(), nullptr);
	EXPECT_EQ(cloned->getWhereExpr(), nullptr);
	EXPECT_EQ(cloned->getMapExpr(), nullptr);
}

TEST(ExpressionClone, ListComprehensionUnknownTypeRendersAnEmptyDiagnosticString) {
	ListComprehensionExpression expression(
		"x",
		std::make_unique<VariableReferenceExpression>("values"),
		nullptr,
		nullptr,
		static_cast<ListComprehensionExpression::ComprehensionType>(99));

	EXPECT_TRUE(expression.toString().empty());
}

TEST(ExpressionClone, ListLiteralExpressionValidatesClonesAndDispatchesVisitors) {
	ListLiteralExpression expression(graph::PropertyValue(std::vector<graph::PropertyValue>{
		graph::PropertyValue(int64_t(1)), graph::PropertyValue("two"), graph::PropertyValue(true)}));

	EXPECT_EQ(expression.getExpressionType(), ExpressionType::EXPR_LIST_LITERAL);
	EXPECT_EQ(expression.toString(), "[1, two, true]");

	ExpressionVisitorProbe visitor;
	expression.accept(static_cast<ExpressionVisitor&>(visitor));
	static_cast<const ListLiteralExpression&>(expression).accept(static_cast<ConstExpressionVisitor&>(visitor));
	EXPECT_TRUE(visitor.visitedListLiteralMutable);
	EXPECT_TRUE(visitor.visitedListLiteralConst);

	auto clone = expression.clone();
	auto* cloned = dynamic_cast<ListLiteralExpression*>(clone.get());
	ASSERT_NE(cloned, nullptr);
	EXPECT_EQ(cloned->getValue(), expression.getValue());
	EXPECT_EQ(cloned->toString(), expression.toString());

	EXPECT_THROW(static_cast<void>(ListLiteralExpression(graph::PropertyValue(int64_t(1)))), std::runtime_error);
}

TEST(ExpressionClone, DynamicListLiteralClonesChildrenAndRejectsStaticAccess) {
	std::vector<std::unique_ptr<Expression>> elements;
	elements.push_back(std::make_unique<VariableReferenceExpression>("value"));
	elements.push_back(std::make_unique<LiteralExpression>(int64_t(1)));
	ListLiteralExpression expression(std::move(elements));

	EXPECT_FALSE(expression.isStatic());
	EXPECT_EQ(expression.toString(), "[value, 1]");
	EXPECT_THROW(static_cast<void>(expression.getValue()), std::logic_error);

	auto clone = expression.clone();
	auto *cloned = dynamic_cast<ListLiteralExpression *>(clone.get());
	ASSERT_NE(cloned, nullptr);
	EXPECT_FALSE(cloned->isStatic());
	ASSERT_EQ(cloned->getElements().size(), 2U);
	EXPECT_EQ(cloned->getElements().at(0U)->toString(), "value");
	EXPECT_EQ(cloned->getElements().at(1U)->toString(), "1");

	std::vector<std::unique_ptr<Expression>> malformed;
	malformed.emplace_back(nullptr);
	EXPECT_THROW(static_cast<void>(ListLiteralExpression(std::move(malformed))), std::invalid_argument);
}

// ============================================================================
// ExistsExpression clone, visitors, and toString
// ============================================================================

TEST(ExpressionClone, ExistsExpressionPreservesStructuredPatternMetadata) {
	ExistsExpression expression(
		"(source)-[:KNOWS]->(target:Person)",
		"source",
		"KNOWS",
		"Person",
		PatternDirection::PAT_OUTGOING,
		std::make_unique<BinaryOpExpression>(
			std::make_unique<VariableReferenceExpression>("target", "age"),
			BinaryOperatorType::BOP_GREATER,
			std::make_unique<LiteralExpression>(int64_t(20))),
		"target");

	EXPECT_EQ(expression.getExpressionType(), ExpressionType::EXPR_EXISTS);
	EXPECT_TRUE(expression.hasStructuredPattern());
	EXPECT_TRUE(expression.hasWhereClause());
	EXPECT_EQ(expression.getSourceVar(), "source");
	EXPECT_EQ(expression.getRelType(), "KNOWS");
	EXPECT_EQ(expression.getTargetLabel(), "Person");
	EXPECT_EQ(expression.getTargetVar(), "target");
	EXPECT_EQ(expression.toString(), "EXISTS((source)-[:KNOWS]->(target:Person) WHERE (target.age > 20))");

	ExpressionVisitorProbe visitor;
	expression.accept(static_cast<ExpressionVisitor&>(visitor));
	static_cast<const ExistsExpression&>(expression).accept(static_cast<ConstExpressionVisitor&>(visitor));
	EXPECT_TRUE(visitor.visitedExistsMutable);
	EXPECT_TRUE(visitor.visitedExistsConst);

	auto clone = expression.clone();
	auto* cloned = dynamic_cast<ExistsExpression*>(clone.get());
	ASSERT_NE(cloned, nullptr);
	ASSERT_NE(cloned->getWhereExpression(), nullptr);
	EXPECT_NE(cloned->getWhereExpression(), expression.getWhereExpression());
	EXPECT_TRUE(cloned->hasStructuredPattern());
	EXPECT_EQ(cloned->getTargetVar(), "target");
	EXPECT_EQ(cloned->toString(), expression.toString());

	ExistsExpression withoutWhere(
		"(source)-[:KNOWS]->(target)", "source", "KNOWS", "", PatternDirection::PAT_OUTGOING, nullptr, "target");
	auto withoutWhereClone = withoutWhere.clone();
	auto* clonedWithoutWhere = dynamic_cast<ExistsExpression*>(withoutWhereClone.get());
	ASSERT_NE(clonedWithoutWhere, nullptr);
	EXPECT_EQ(clonedWithoutWhere->getWhereExpression(), nullptr);
}

TEST(ExpressionClone, ExistsExpressionCompatibilityConstructorClonesWithoutWhereClause) {
	ExistsExpression expression("(n)-->(m)");

	EXPECT_FALSE(expression.hasStructuredPattern());
	EXPECT_FALSE(expression.hasWhereClause());
	EXPECT_EQ(expression.toString(), "EXISTS((n)-->(m))");

	auto clone = expression.clone();
	auto* cloned = dynamic_cast<ExistsExpression*>(clone.get());
	ASSERT_NE(cloned, nullptr);
	EXPECT_FALSE(cloned->hasStructuredPattern());
	EXPECT_EQ(cloned->getWhereExpression(), nullptr);
	EXPECT_EQ(cloned->toString(), expression.toString());

	ExistsExpression withWhere("(n)-->(m)", std::make_unique<LiteralExpression>(true));
	auto withWhereClone = withWhere.clone();
	auto* clonedWithWhere = dynamic_cast<ExistsExpression*>(withWhereClone.get());
	ASSERT_NE(clonedWithWhere, nullptr);
	ASSERT_NE(clonedWithWhere->getWhereExpression(), nullptr);
	EXPECT_NE(clonedWithWhere->getWhereExpression(), withWhere.getWhereExpression());
}

// ============================================================================
// PatternComprehensionExpression clone, visitors, and toString
// ============================================================================

TEST(ExpressionClone, PatternComprehensionPreservesPatternMetadataAndExpressions) {
	PatternComprehensionExpression expression(
		"(source)-[relationship:KNOWS]->(target:Person)",
		"source",
		"target",
		"KNOWS",
		"Person",
		PatternDirection::PAT_OUTGOING,
		std::make_unique<VariableReferenceExpression>("target", "name"),
		std::make_unique<BinaryOpExpression>(
			std::make_unique<VariableReferenceExpression>("target", "age"),
			BinaryOperatorType::BOP_GREATER,
			std::make_unique<LiteralExpression>(int64_t(20))),
		"relationship");

	EXPECT_EQ(expression.getExpressionType(), ExpressionType::EXPR_PATTERN_COMPREHENSION);
	EXPECT_EQ(expression.getSourceVar(), "source");
	EXPECT_EQ(expression.getTargetVar(), "target");
	EXPECT_EQ(expression.getRelationshipVar(), "relationship");
	EXPECT_EQ(expression.getRelType(), "KNOWS");
	EXPECT_EQ(expression.getTargetLabel(), "Person");
	EXPECT_EQ(expression.getDirection(), PatternDirection::PAT_OUTGOING);
	EXPECT_EQ(expression.toString(),
		"[(source)-[relationship:KNOWS]->(target:Person) WHERE (target.age > 20) | target.name]");

	ExpressionVisitorProbe visitor;
	expression.accept(static_cast<ExpressionVisitor&>(visitor));
	static_cast<const PatternComprehensionExpression&>(expression).accept(
		static_cast<ConstExpressionVisitor&>(visitor));
	EXPECT_TRUE(visitor.visitedPatternComprehensionMutable);
	EXPECT_TRUE(visitor.visitedPatternComprehensionConst);

	auto clone = expression.clone();
	auto* cloned = dynamic_cast<PatternComprehensionExpression*>(clone.get());
	ASSERT_NE(cloned, nullptr);
	ASSERT_NE(cloned->getMapExpression(), nullptr);
	ASSERT_NE(cloned->getWhereExpression(), nullptr);
	EXPECT_NE(cloned->getMapExpression(), expression.getMapExpression());
	EXPECT_NE(cloned->getWhereExpression(), expression.getWhereExpression());
	EXPECT_EQ(cloned->toString(), expression.toString());
}

TEST(ExpressionClone, PatternComprehensionCompatibilityConstructorAllowsAbsentClauses) {
	PatternComprehensionExpression expression("(n)-->(m)", "m", nullptr);

	EXPECT_EQ(expression.getSourceVar(), "");
	EXPECT_EQ(expression.getTargetVar(), "m");
	EXPECT_EQ(expression.getRelationshipVar(), "");
	EXPECT_EQ(expression.getDirection(), PatternDirection::PAT_BOTH);
	EXPECT_FALSE(expression.hasWhereClause());
	EXPECT_EQ(expression.getMapExpression(), nullptr);
	EXPECT_EQ(expression.getWhereExpression(), nullptr);
	EXPECT_EQ(expression.toString(), "[(n)-->(m)]");

	auto clone = expression.clone();
	auto* cloned = dynamic_cast<PatternComprehensionExpression*>(clone.get());
	ASSERT_NE(cloned, nullptr);
	EXPECT_EQ(cloned->getMapExpression(), nullptr);
	EXPECT_EQ(cloned->getWhereExpression(), nullptr);
	EXPECT_EQ(cloned->toString(), expression.toString());
}

TEST(ExpressionClone, PatternComprehensionPreservesAWhereClauseWithoutAMapExpression) {
	PatternComprehensionExpression expression(
		"(source)-[:KNOWS]->(target)", "source", "target", "KNOWS", "", PatternDirection::PAT_OUTGOING,
		nullptr, std::make_unique<LiteralExpression>(true));

	EXPECT_EQ(expression.getMapExpression(), nullptr);
	ASSERT_NE(expression.getWhereExpression(), nullptr);
	EXPECT_TRUE(expression.hasWhereClause());
	EXPECT_EQ(expression.toString(), "[(source)-[:KNOWS]->(target) WHERE true]");

	auto clone = expression.clone();
	auto *cloned = dynamic_cast<PatternComprehensionExpression *>(clone.get());
	ASSERT_NE(cloned, nullptr);
	EXPECT_EQ(cloned->getMapExpression(), nullptr);
	ASSERT_NE(cloned->getWhereExpression(), nullptr);
	EXPECT_NE(cloned->getWhereExpression(), expression.getWhereExpression());
	EXPECT_EQ(cloned->toString(), expression.toString());
}

// ============================================================================
// ListSliceExpression clone, visitors, and rendering
// ============================================================================

TEST(ExpressionClone, ListSliceExpressionClonesRangeBoundsAndDispatchesVisitors) {
	ListSliceExpression expression(
		std::make_unique<VariableReferenceExpression>("items"),
		std::make_unique<LiteralExpression>(int64_t(1)),
		std::make_unique<LiteralExpression>(int64_t(3)),
		true);

	EXPECT_EQ(expression.getExpressionType(), ExpressionType::LIST_SLICE);
	EXPECT_TRUE(expression.hasRange());
	EXPECT_EQ(expression.toString(), "items[1..3]");
	ASSERT_NE(expression.getList(), nullptr);
	ASSERT_NE(expression.getStart(), nullptr);
	ASSERT_NE(expression.getEnd(), nullptr);

	ExpressionVisitorProbe visitor;
	expression.accept(static_cast<ExpressionVisitor&>(visitor));
	static_cast<const ListSliceExpression&>(expression).accept(static_cast<ConstExpressionVisitor&>(visitor));
	EXPECT_TRUE(visitor.visitedListSliceMutable);
	EXPECT_TRUE(visitor.visitedListSliceConst);

	auto clone = expression.clone();
	auto* cloned = dynamic_cast<ListSliceExpression*>(clone.get());
	ASSERT_NE(cloned, nullptr);
	EXPECT_NE(cloned->getList(), expression.getList());
	EXPECT_NE(cloned->getStart(), expression.getStart());
	EXPECT_NE(cloned->getEnd(), expression.getEnd());
	EXPECT_EQ(cloned->toString(), expression.toString());
}

TEST(ExpressionClone, ListSliceExpressionRendersOpenAndSingleElementForms) {
	ListSliceExpression openStart(
		std::make_unique<VariableReferenceExpression>("items"), nullptr,
		std::make_unique<LiteralExpression>(int64_t(-1)), true);
	EXPECT_EQ(openStart.toString(), "items[..-1]");
	auto openStartClone = openStart.clone();
	auto* clonedOpenStart = dynamic_cast<ListSliceExpression*>(openStartClone.get());
	ASSERT_NE(clonedOpenStart, nullptr);
	EXPECT_EQ(clonedOpenStart->getStart(), nullptr);
	EXPECT_NE(clonedOpenStart->getEnd(), nullptr);

	ListSliceExpression openEnd(
		std::make_unique<VariableReferenceExpression>("items"),
		std::make_unique<LiteralExpression>(int64_t(2)), nullptr, true);
	EXPECT_EQ(openEnd.toString(), "items[2..]");
	auto openEndClone = openEnd.clone();
	auto* clonedOpenEnd = dynamic_cast<ListSliceExpression*>(openEndClone.get());
	ASSERT_NE(clonedOpenEnd, nullptr);
	EXPECT_NE(clonedOpenEnd->getStart(), nullptr);
	EXPECT_EQ(clonedOpenEnd->getEnd(), nullptr);

	ListSliceExpression singleElement(
		std::make_unique<VariableReferenceExpression>("items"),
		std::make_unique<LiteralExpression>(int64_t(-1)), nullptr, false);
	EXPECT_FALSE(singleElement.hasRange());
	EXPECT_EQ(singleElement.toString(), "items[-1]");
}

// ============================================================================
// QuantifierFunctionExpression clone, visitors, and rendering
// ============================================================================

TEST(ExpressionClone, QuantifierFunctionExpressionPreservesScopedExpressionContract) {
	QuantifierFunctionExpression expression(
		"all", "item", std::make_unique<VariableReferenceExpression>("items"),
		std::make_unique<BinaryOpExpression>(
			std::make_unique<VariableReferenceExpression>("item", "active"),
			BinaryOperatorType::BOP_EQUAL,
			std::make_unique<LiteralExpression>(true)));

	EXPECT_EQ(expression.getExpressionType(), ExpressionType::EXPR_QUANTIFIER_FUNCTION);
	EXPECT_EQ(expression.getFunctionName(), "all");
	EXPECT_EQ(expression.getVariable(), "item");
	EXPECT_EQ(expression.toString(), "all(item IN items WHERE (item.active = true))");
	ASSERT_NE(expression.getListExpression(), nullptr);
	ASSERT_NE(expression.getWhereExpression(), nullptr);

	ExpressionVisitorProbe visitor;
	expression.accept(static_cast<ExpressionVisitor&>(visitor));
	static_cast<const QuantifierFunctionExpression&>(expression).accept(
		static_cast<ConstExpressionVisitor&>(visitor));
	EXPECT_TRUE(visitor.visitedQuantifierMutable);
	EXPECT_TRUE(visitor.visitedQuantifierConst);

	auto clone = expression.clone();
	auto* cloned = dynamic_cast<QuantifierFunctionExpression*>(clone.get());
	ASSERT_NE(cloned, nullptr);
	EXPECT_NE(cloned->getListExpression(), expression.getListExpression());
	EXPECT_NE(cloned->getWhereExpression(), expression.getWhereExpression());
	EXPECT_EQ(cloned->toString(), expression.toString());
}

// ============================================================================
// ReduceExpression clone, visitors, and rendering
// ============================================================================

TEST(ExpressionClone, ReduceExpressionPreservesAllScopedChildrenAndDispatchesVisitors) {
	ReduceExpression expression(
		"total", std::make_unique<LiteralExpression>(int64_t(0)), "item",
		std::make_unique<VariableReferenceExpression>("items"),
		std::make_unique<BinaryOpExpression>(
			std::make_unique<VariableReferenceExpression>("total"),
			BinaryOperatorType::BOP_ADD,
			std::make_unique<VariableReferenceExpression>("item")));

	EXPECT_EQ(expression.getExpressionType(), ExpressionType::EXPR_REDUCE);
	EXPECT_EQ(expression.getAccumulator(), "total");
	EXPECT_EQ(expression.getVariable(), "item");
	EXPECT_EQ(expression.toString(), "reduce(total = 0, item IN items | (total + item))");
	ASSERT_NE(expression.getInitialExpr(), nullptr);
	ASSERT_NE(expression.getListExpr(), nullptr);
	ASSERT_NE(expression.getBodyExpr(), nullptr);

	ExpressionVisitorProbe visitor;
	expression.accept(static_cast<ExpressionVisitor&>(visitor));
	static_cast<const ReduceExpression&>(expression).accept(static_cast<ConstExpressionVisitor&>(visitor));
	EXPECT_TRUE(visitor.visitedReduceMutable);
	EXPECT_TRUE(visitor.visitedReduceConst);

	auto clone = expression.clone();
	auto* cloned = dynamic_cast<ReduceExpression*>(clone.get());
	ASSERT_NE(cloned, nullptr);
	EXPECT_NE(cloned->getInitialExpr(), expression.getInitialExpr());
	EXPECT_NE(cloned->getListExpr(), expression.getListExpr());
	EXPECT_NE(cloned->getBodyExpr(), expression.getBodyExpr());
	EXPECT_EQ(cloned->toString(), expression.toString());
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
	EXPECT_EQ(ref.getExpressionType(), ExpressionType::VARIABLE_REFERENCE);

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
	EXPECT_EQ(inExpr.getExpressionType(), ExpressionType::IN_LIST);
}
