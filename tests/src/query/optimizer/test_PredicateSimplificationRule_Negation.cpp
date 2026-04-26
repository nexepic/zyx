/**
 * @file test_PredicateSimplificationRule_Negation.cpp
 * @brief Additional branch coverage tests for PredicateSimplificationRule.hpp.
 *        Covers: rebuild path when predicate ptr changes, non-NOT unary op inner,
 *        binary non-logical op passthrough, null plan, non-filter passthrough.
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

} // namespace

class PredSimplNegationTest : public ::testing::Test {
protected:
	rules::PredicateSimplificationRule rule;
	Statistics stats;
};

// Null plan returns null.
TEST_F(PredSimplNegationTest, NullPlanReturnsNull) {
	auto result = rule.apply(nullptr, stats);
	EXPECT_EQ(result, nullptr);
}

// Non-filter node passes through unchanged.
TEST_F(PredSimplNegationTest, NonFilterPassesThrough) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto result = rule.apply(std::move(scan), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
}

// NOT applied to a non-unary inner (binary op) — NOT is preserved.
TEST_F(PredSimplNegationTest, NotOfBinaryOpIsPreserved) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto inner = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_EQUAL, litInt(5));
	auto pred = unaryExpr(UnaryOperatorType::UOP_NOT, inner);
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// Non-logical binary op (BOP_EQUAL) is not folded.
TEST_F(PredSimplNegationTest, NonLogicalBinaryOpNotFolded) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_EQUAL, litInt(42));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// BOP_LESS is not a logical op — passthrough.
TEST_F(PredSimplNegationTest, ComparisonBinaryOpNotFolded) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_LESS, litInt(10));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// Double-NOT with non-null inner simplifies and triggers rebuild path (line 205).
TEST_F(PredSimplNegationTest, DoubleNotSimplifiedTriggersRebuild) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto inner = propRef("n", "age");
	auto doubleNot = unaryExpr(UnaryOperatorType::UOP_NOT,
							   unaryExpr(UnaryOperatorType::UOP_NOT, inner));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), doubleNot);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
	// The simplified predicate should be n.age, not NOT(NOT(n.age))
	auto *f = static_cast<LogicalFilter *>(result.get());
	ASSERT_NE(f->getPredicate(), nullptr);
	EXPECT_EQ(f->getPredicate()->toString(), "n.age");
}

// UOP_MINUS unary (non-NOT) does not trigger double-NOT path.
TEST_F(PredSimplNegationTest, UnaryMinusDoesNotSimplify) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = unaryExpr(UnaryOperatorType::UOP_MINUS, propRef("n", "val"));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// Filter with null predicate and non-null child — pred is null, no simplification.
TEST_F(PredSimplNegationTest, NullPredicateWithScanChild) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), nullptr);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// AND with two non-literal operands — no folding, stays as-is.
TEST_F(PredSimplNegationTest, AndWithNoLiteralsNotFolded) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = binaryExpr(propRef("n", "a"), BinaryOperatorType::BOP_AND, propRef("n", "b"));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// OR with two non-literal operands — no folding.
TEST_F(PredSimplNegationTest, OrWithNoLiteralsNotFolded) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = binaryExpr(propRef("n", "a"), BinaryOperatorType::BOP_OR, propRef("n", "b"));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// Covers: line 193 false branch — outer filter has null pred, child filter has non-null pred.
// When pred is null, the merge condition `pred && childPred` evaluates false on `pred`.
TEST_F(PredSimplNegationTest, NullPredOverFilterChildSkipsMerge) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto childFilter = std::make_unique<LogicalFilter>(std::move(scan), propRef("n", "age"));
	auto outerFilter = std::make_unique<LogicalFilter>(std::move(childFilter), nullptr);

	auto result = rule.apply(std::move(outerFilter), stats);
	ASSERT_NE(result, nullptr);
	// The outer filter stays because pred is null — no merge or dedup.
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

// Covers: NOT with non-unary inner (a literal) — NOT is not eliminated.
TEST_F(PredSimplNegationTest, NotOfLiteralIsPreserved) {
	auto scan = std::make_unique<LogicalNodeScan>("n");
	auto pred = unaryExpr(UnaryOperatorType::UOP_NOT, litBool(true));
	auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

	auto result = rule.apply(std::move(filter), stats);
	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}
