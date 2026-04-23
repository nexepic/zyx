/**
 * @file test_PredicateSimplificationRule_Folding.cpp
 * @brief Focused branch tests for PredicateSimplificationRule.
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
    return std::shared_ptr<Expression>(std::make_unique<VariableReferenceExpression>(name).release());
}

std::shared_ptr<Expression> propRef(const std::string &var, const std::string &prop) {
    return std::shared_ptr<Expression>(std::make_unique<VariableReferenceExpression>(var, prop).release());
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

class PredicateSimplificationRuleFoldingTest : public ::testing::Test {
protected:
    rules::PredicateSimplificationRule rule;
    Statistics stats;
};

TEST_F(PredicateSimplificationRuleFoldingTest, NullPredicateAndNullChildStayStable) {
    auto filter = std::make_unique<LogicalFilter>(nullptr, nullptr);
    auto result = rule.apply(std::move(filter), stats);

    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
    EXPECT_EQ(static_cast<LogicalFilter *>(result.get())->getPredicate(), nullptr);
}

TEST_F(PredicateSimplificationRuleFoldingTest, UnaryMinusIsNotSimplifiedAway) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred = unaryExpr(UnaryOperatorType::UOP_MINUS, propRef("n", "age"));
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
    EXPECT_EQ(boolFromFilter(result.get())->getExpressionType(), ExpressionType::UNARY_OP);
}

TEST_F(PredicateSimplificationRuleFoldingTest, SingleNotIsPreserved) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred = unaryExpr(UnaryOperatorType::UOP_NOT, varRef("n"));
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
    EXPECT_EQ(boolFromFilter(result.get())->toString(), pred->toString());
}

TEST_F(PredicateSimplificationRuleFoldingTest, XAndFalseSimplifiesToFalseLiteral) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred = binaryExpr(varRef("n"), BinaryOperatorType::BOP_AND, litBool(false));
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
    auto out = boolFromFilter(result.get());
    ASSERT_EQ(out->getExpressionType(), ExpressionType::LITERAL);
    EXPECT_FALSE(static_cast<LiteralExpression *>(out.get())->getBooleanValue());
}

TEST_F(PredicateSimplificationRuleFoldingTest, XAndTrueSimplifiesToX) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_AND, litBool(true));
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
    auto out = boolFromFilter(result.get());
    EXPECT_EQ(out->toString(), "n.age");
}

TEST_F(PredicateSimplificationRuleFoldingTest, TrueOrXAndXOrTrueSimplifyToTrue) {
    auto scan1 = std::make_unique<LogicalNodeScan>("n");
    auto pred1 = binaryExpr(litBool(true), BinaryOperatorType::BOP_OR, propRef("n", "age"));
    auto filter1 = std::make_unique<LogicalFilter>(std::move(scan1), pred1);

    auto result1 = rule.apply(std::move(filter1), stats);
    ASSERT_NE(result1, nullptr);
    ASSERT_EQ(result1->getType(), LogicalOpType::LOP_NODE_SCAN);

    auto scan2 = std::make_unique<LogicalNodeScan>("n");
    auto pred2 = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_OR, litBool(true));
    auto filter2 = std::make_unique<LogicalFilter>(std::move(scan2), pred2);

    auto result2 = rule.apply(std::move(filter2), stats);
    ASSERT_NE(result2, nullptr);
    ASSERT_EQ(result2->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST_F(PredicateSimplificationRuleFoldingTest, FalseOrXAndXOrFalseSimplifyToX) {
    auto scan1 = std::make_unique<LogicalNodeScan>("n");
    auto pred1 = binaryExpr(litBool(false), BinaryOperatorType::BOP_OR, propRef("n", "age"));
    auto filter1 = std::make_unique<LogicalFilter>(std::move(scan1), pred1);

    auto result1 = rule.apply(std::move(filter1), stats);
    ASSERT_NE(result1, nullptr);
    ASSERT_EQ(result1->getType(), LogicalOpType::LOP_FILTER);
    EXPECT_EQ(boolFromFilter(result1.get())->toString(), "n.age");

    auto scan2 = std::make_unique<LogicalNodeScan>("n");
    auto pred2 = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_OR, litBool(false));
    auto filter2 = std::make_unique<LogicalFilter>(std::move(scan2), pred2);

    auto result2 = rule.apply(std::move(filter2), stats);
    ASSERT_NE(result2, nullptr);
    ASSERT_EQ(result2->getType(), LogicalOpType::LOP_FILTER);
    EXPECT_EQ(boolFromFilter(result2.get())->toString(), "n.age");
}

TEST_F(PredicateSimplificationRuleFoldingTest, NonBooleanLiteralIsNotFoldedAsLogicalLiteral) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred = binaryExpr(litInt(123), BinaryOperatorType::BOP_AND, propRef("n", "age"));
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
    EXPECT_EQ(boolFromFilter(result.get())->toString(), "(123 AND n.age)");
}

TEST_F(PredicateSimplificationRuleFoldingTest, FalseLiteralFilterIsNotDropped) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), litBool(false));

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

TEST_F(PredicateSimplificationRuleFoldingTest, NullOperandInBinaryPredicateDoesNotCrash) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred = std::shared_ptr<Expression>(
        std::make_unique<BinaryOpExpression>(nullptr, BinaryOperatorType::BOP_AND, litInt(1)->clone())
            .release());
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

TEST_F(PredicateSimplificationRuleFoldingTest, ChildFilterWithNullPredicateDoesNotMerge) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto child = std::make_unique<LogicalFilter>(std::move(scan), nullptr);
    auto outer = std::make_unique<LogicalFilter>(std::move(child), varRef("n"));

    auto result = rule.apply(std::move(outer), stats);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
    auto *outerFilter = static_cast<LogicalFilter *>(result.get());
    ASSERT_EQ(outerFilter->getChildren().size(), 1u);
    ASSERT_NE(outerFilter->getChildren()[0], nullptr);
    EXPECT_EQ(outerFilter->getChildren()[0]->getType(), LogicalOpType::LOP_FILTER);
}
