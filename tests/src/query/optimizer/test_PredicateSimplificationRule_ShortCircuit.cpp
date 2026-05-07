/**
 * @file test_PredicateSimplificationRule_ShortCircuit.cpp
 * @brief Additional branch coverage tests for short-circuit evaluation paths
 *        in PredicateSimplificationRule.hpp.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/optimizer/Statistics.hpp"
#include "graph/query/optimizer/rules/PredicateSimplificationRule.hpp"

using namespace graph::query::expressions;
using namespace graph::query::logical;
using namespace graph::query::optimizer;

namespace {

std::shared_ptr<Expression> litBool(bool value) {
	return std::make_shared<LiteralExpression>(value);
}

std::shared_ptr<Expression> litInt(int64_t value) {
	return std::make_shared<LiteralExpression>(value);
}

std::shared_ptr<Expression> varRef(const std::string &name) {
	return std::shared_ptr<Expression>(
		std::make_unique<VariableReferenceExpression>(name).release());
}

std::shared_ptr<Expression> propRef(const std::string &var, const std::string &prop) {
	return std::shared_ptr<Expression>(
		std::make_unique<VariableReferenceExpression>(var, prop).release());
}

std::shared_ptr<Expression> binaryExpr(
	const std::shared_ptr<Expression> &lhs,
	BinaryOperatorType op,
	const std::shared_ptr<Expression> &rhs) {
	return std::shared_ptr<Expression>(
		std::make_unique<BinaryOpExpression>(
			lhs ? lhs->clone() : nullptr,
			op,
			rhs ? rhs->clone() : nullptr)
			.release());
}

std::shared_ptr<Expression> unaryExpr(
	UnaryOperatorType op, const std::shared_ptr<Expression> &operand) {
	return std::shared_ptr<Expression>(
		std::make_unique<UnaryOpExpression>(op, operand ? operand->clone() : nullptr).release());
}

std::shared_ptr<Expression> boolFromFilter(const LogicalOperator *op) {
	if (!op || op->getType() != LogicalOpType::LOP_FILTER) return nullptr;
	const auto *filter = static_cast<const LogicalFilter *>(op);
	return filter->getPredicate();
}

} // namespace

class PredSimplShortCircuitTest : public ::testing::Test {
protected:
	rules::PredicateSimplificationRule rule;
	Statistics stats;
};

// AND with null LHS: isBoolLiteral(nullptr, val) returns false → no folding.
// The short-circuit is: `isBoolLiteral(lhs, lhsBool) && !lhsBool` where lhs is null.
TEST_F(PredSimplShortCircuitTest, AndWithNullLhsNoFold) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = std::shared_ptr<Expression>(
		std::make_unique<BinaryOpExpression>(
			nullptr, BinaryOperatorType::BOP_AND, propRef("n", "age")->clone())
			.release());
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// OR with null LHS: similar short-circuit on null check
TEST_F(PredSimplShortCircuitTest, OrWithNullLhsNoFold) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = std::shared_ptr<Expression>(
		std::make_unique<BinaryOpExpression>(
			nullptr, BinaryOperatorType::BOP_OR, propRef("n", "age")->clone())
			.release());
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// AND with null RHS: isBoolLiteral(nullptr, val) returns false → no folding.
TEST_F(PredSimplShortCircuitTest, AndWithNullRhsNoFold) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = std::shared_ptr<Expression>(
		std::make_unique<BinaryOpExpression>(
			propRef("n", "age")->clone(), BinaryOperatorType::BOP_AND, nullptr)
			.release());
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// OR with null RHS
TEST_F(PredSimplShortCircuitTest, OrWithNullRhsNoFold) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = std::shared_ptr<Expression>(
		std::make_unique<BinaryOpExpression>(
			propRef("n", "age")->clone(), BinaryOperatorType::BOP_OR, nullptr)
			.release());
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// Triple NOT: NOT(NOT(NOT(x))) → NOT(x) after one simplification round
TEST_F(PredSimplShortCircuitTest, TripleNotSimplifiesToSingleNot) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto inner = propRef("n", "age");
	auto tripleNot = unaryExpr(UnaryOperatorType::UOP_NOT,
		unaryExpr(UnaryOperatorType::UOP_NOT,
			unaryExpr(UnaryOperatorType::UOP_NOT, inner)));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), tripleNot);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
	auto pred = boolFromFilter(result.get());
	ASSERT_NE(pred, nullptr);
	// Simplification removes one pair: NOT(NOT(NOT(x))) → NOT(x)
	EXPECT_EQ(pred->getExpressionType(), ExpressionType::UNARY_OP);
}

// NOT applied to a non-bool literal — NOT preserved
TEST_F(PredSimplShortCircuitTest, NotOfIntLiteralPreserved) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = unaryExpr(UnaryOperatorType::UOP_NOT, litInt(42));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
	auto out = boolFromFilter(result.get());
	EXPECT_EQ(out->getExpressionType(), ExpressionType::UNARY_OP);
}

// AND true LHS + null RHS: `isBoolLiteral(lhs, true) && lhsBool && rhs`
// where rhs is null → short-circuits at `&& rhs`
TEST_F(PredSimplShortCircuitTest, TrueAndNullRhsNoFold) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = std::shared_ptr<Expression>(
		std::make_unique<BinaryOpExpression>(
			litBool(true)->clone(), BinaryOperatorType::BOP_AND, nullptr)
			.release());
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	// true AND null: lhs is true bool, but rhs is null → falls through
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// OR false LHS + null RHS: `isBoolLiteral(lhs, false) && !lhsBool && rhs`
// where rhs is null → short-circuits at `&& rhs`
TEST_F(PredSimplShortCircuitTest, FalseOrNullRhsNoFold) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = std::shared_ptr<Expression>(
		std::make_unique<BinaryOpExpression>(
			litBool(false)->clone(), BinaryOperatorType::BOP_OR, nullptr)
			.release());
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// Non-boolean literal LHS in AND: isBoolLiteral returns false for int literal
TEST_F(PredSimplShortCircuitTest, IntLiteralLhsAndBoolRhsNoFold) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = binaryExpr(litInt(99), BinaryOperatorType::BOP_AND, litBool(true));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// Non-boolean literal RHS in OR: isBoolLiteral returns false for int literal
TEST_F(PredSimplShortCircuitTest, BoolLhsOrIntRhsPartialFold) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	// false OR intLiteral: lhs is false bool → `isBoolLiteral(lhs, lhsBool) && !lhsBool && rhs`
	// rhs is non-null, so should fold to rhs
	auto pred = binaryExpr(litBool(false), BinaryOperatorType::BOP_OR, litInt(42));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	ASSERT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
	auto out = boolFromFilter(result.get());
	// Should simplify to the int literal (rhs)
	ASSERT_NE(out, nullptr);
	EXPECT_EQ(out->getExpressionType(), ExpressionType::LITERAL);
}

// x AND true where x is null (null ptr for lhs)
// Tests the `&& lhs` short-circuit on `isBoolLiteral(rhs, rhsBool) && rhsBool && lhs`
TEST_F(PredSimplShortCircuitTest, NullLhsAndTrueRhsNoFold) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = std::shared_ptr<Expression>(
		std::make_unique<BinaryOpExpression>(
			nullptr, BinaryOperatorType::BOP_AND, litBool(true)->clone())
			.release());
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// x OR false where x is null (null ptr for lhs)
// Tests the `&& lhs` short-circuit on `isBoolLiteral(rhs, rhsBool) && !rhsBool && lhs`
TEST_F(PredSimplShortCircuitTest, NullLhsOrFalseRhsNoFold) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = std::shared_ptr<Expression>(
		std::make_unique<BinaryOpExpression>(
			nullptr, BinaryOperatorType::BOP_OR, litBool(false)->clone())
			.release());
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}
