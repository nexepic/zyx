/**
 * @file test_FilterPushdownRule_EdgeCases.cpp
 * @brief Focused branch tests for FilterPushdownRule.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/optimizer/Statistics.hpp"
#include "graph/query/optimizer/rules/FilterPushdownRule.hpp"

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

std::shared_ptr<Expression> litDouble(double value) {
    return std::make_shared<LiteralExpression>(value);
}

std::shared_ptr<Expression> litNull() {
    return std::make_shared<LiteralExpression>();
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

std::shared_ptr<Expression> andExpr(
    const std::shared_ptr<Expression> &lhs,
    const std::shared_ptr<Expression> &rhs) {
    return binaryExpr(lhs, BinaryOperatorType::BOP_AND, rhs);
}

std::shared_ptr<Expression> unaryNotExpr(const std::shared_ptr<Expression> &operand) {
    return std::shared_ptr<Expression>(
        std::make_unique<UnaryOpExpression>(UnaryOperatorType::UOP_NOT, operand ? operand->clone() : nullptr)
            .release());
}

std::shared_ptr<Expression> isNullExpr(const std::shared_ptr<Expression> &operand) {
    return std::shared_ptr<Expression>(
        std::make_unique<IsNullExpression>(operand ? operand->clone() : nullptr, false).release());
}

std::shared_ptr<Expression> inListExpr(const std::shared_ptr<Expression> &valueExpr) {
    std::vector<graph::PropertyValue> values;
    values.emplace_back(int64_t(1));
    values.emplace_back(int64_t(2));
    return std::shared_ptr<Expression>(
        std::make_unique<InExpression>(valueExpr ? valueExpr->clone() : nullptr, values).release());
}

} // namespace

class FilterPushdownRuleEdgeCasesTest : public ::testing::Test {
protected:
    rules::FilterPushdownRule rule;
    Statistics stats;
};

TEST_F(FilterPushdownRuleEdgeCasesTest, NullChildFilterIsReturnedAsIs) {
    auto filter = std::make_unique<LogicalFilter>(nullptr, varRef("n"));
    auto result = rule.apply(std::move(filter), stats);

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

TEST_F(FilterPushdownRuleEdgeCasesTest, ProjectWithoutGrandChildCannotBePushedThrough) {
    std::vector<LogicalProjectItem> items;
    items.emplace_back(varRef("n"), "n");
    auto project = std::make_unique<LogicalProject>(nullptr, std::move(items));
    auto filter = std::make_unique<LogicalFilter>(std::move(project), varRef("n"));

    auto result = rule.apply(std::move(filter), stats);

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
    auto *f = static_cast<LogicalFilter *>(result.get());
    ASSERT_EQ(f->getChildren().size(), 1u);
    ASSERT_NE(f->getChildren()[0], nullptr);
    EXPECT_EQ(f->getChildren()[0]->getType(), LogicalOpType::LOP_PROJECT);
}

TEST_F(FilterPushdownRuleEdgeCasesTest, ReversedEqualityLiteralEqualsPropertyPushesIntoScan) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred = binaryExpr(litInt(42), BinaryOperatorType::BOP_EQUAL, propRef("n", "age"));
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);

    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
    auto *s = static_cast<LogicalNodeScan *>(result.get());
    ASSERT_EQ(s->getPropertyPredicates().size(), 1u);
    EXPECT_EQ(s->getPropertyPredicates()[0].first, "age");
}

TEST_F(FilterPushdownRuleEdgeCasesTest, DuplicateEqualityKeyDoesNotDuplicatePredicate) {
    std::vector<std::pair<std::string, graph::PropertyValue>> existing = {
        {"age", graph::PropertyValue(int64_t(1))}
    };
    auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{}, existing);
    auto pred = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_EQUAL, litInt(99));
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);

    ASSERT_NE(result, nullptr);
    // Duplicate key is ignored, so filter wrapper remains.
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
    auto *f = static_cast<LogicalFilter *>(result.get());
    ASSERT_EQ(f->getChildren().size(), 1u);
    auto *s = static_cast<LogicalNodeScan *>(f->getChildren()[0]);
    EXPECT_EQ(s->getPropertyPredicates().size(), 1u);
    EXPECT_EQ(s->getPropertyPredicates()[0].first, "age");
}

TEST_F(FilterPushdownRuleEdgeCasesTest, EqualityWithoutPropertyAccessDoesNotPush) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred = binaryExpr(varRef("n"), BinaryOperatorType::BOP_EQUAL, litInt(42));
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

TEST_F(FilterPushdownRuleEdgeCasesTest, EqualityWithNullOperandIsRejected) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred = std::shared_ptr<Expression>(
        std::make_unique<BinaryOpExpression>(nullptr, BinaryOperatorType::BOP_EQUAL, litInt(7)->clone()).release());
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);

    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->getType(), LogicalOpType::LOP_FILTER);
}

TEST_F(FilterPushdownRuleEdgeCasesTest, RangeRejectsNullAndBooleanLiterals) {
    auto scan1 = std::make_unique<LogicalNodeScan>("n");
    auto nullRange = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_GREATER, litNull());
    auto filter1 = std::make_unique<LogicalFilter>(std::move(scan1), nullRange);
    auto result1 = rule.apply(std::move(filter1), stats);
    ASSERT_NE(result1, nullptr);
    EXPECT_EQ(result1->getType(), LogicalOpType::LOP_FILTER);

    auto scan2 = std::make_unique<LogicalNodeScan>("n");
    auto boolRange = binaryExpr(propRef("n", "age"), BinaryOperatorType::BOP_GREATER, litBool(true));
    auto filter2 = std::make_unique<LogicalFilter>(std::move(scan2), boolRange);
    auto result2 = rule.apply(std::move(filter2), stats);
    ASSERT_NE(result2, nullptr);
    EXPECT_EQ(result2->getType(), LogicalOpType::LOP_FILTER);
}

TEST_F(FilterPushdownRuleEdgeCasesTest, ReversedRangeOperatorsAreMappedAndPushed) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto c1 = binaryExpr(litInt(5), BinaryOperatorType::BOP_LESS, propRef("n", "age"));
    auto c2 = binaryExpr(litInt(5), BinaryOperatorType::BOP_LESS_EQUAL, propRef("n", "age"));
    auto c3 = binaryExpr(litInt(99), BinaryOperatorType::BOP_GREATER, propRef("n", "age"));
    auto c4 = binaryExpr(litInt(99), BinaryOperatorType::BOP_GREATER_EQUAL, propRef("n", "age"));
    auto pred = andExpr(andExpr(c1, c2), andExpr(c3, c4));
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);

    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
    auto *s = static_cast<LogicalNodeScan *>(result.get());
    ASSERT_EQ(s->getRangePredicates().size(), 1u);
    const auto &rp = s->getRangePredicates()[0];
    EXPECT_EQ(rp.key, "age");
    EXPECT_NE(rp.minValue.getType(), graph::PropertyType::NULL_TYPE);
    EXPECT_NE(rp.maxValue.getType(), graph::PropertyType::NULL_TYPE);
}

TEST_F(FilterPushdownRuleEdgeCasesTest, DoubleRangeUsesDoublePropertyValueBranch) {
    auto scan = std::make_unique<LogicalNodeScan>("n");
    auto pred = binaryExpr(propRef("n", "score"), BinaryOperatorType::BOP_GREATER, litDouble(1.25));
    auto filter = std::make_unique<LogicalFilter>(std::move(scan), pred);

    auto result = rule.apply(std::move(filter), stats);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_NODE_SCAN);
    auto *s = static_cast<LogicalNodeScan *>(result.get());
    ASSERT_EQ(s->getRangePredicates().size(), 1u);
    EXPECT_EQ(s->getRangePredicates()[0].key, "score");
}

TEST_F(FilterPushdownRuleEdgeCasesTest, JoinPushdownCollectsVarsFromUnaryAndIsNull) {
    auto left = std::make_unique<LogicalNodeScan>("n");
    auto right = std::make_unique<LogicalNodeScan>("m");
    auto join = std::make_unique<LogicalJoin>(std::move(left), std::move(right));
    auto pred = unaryNotExpr(isNullExpr(propRef("n", "age")));
    auto filter = std::make_unique<LogicalFilter>(std::move(join), pred);

    auto result = rule.apply(std::move(filter), stats);

    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_JOIN);
    auto *j = static_cast<LogicalJoin *>(result.get());
    ASSERT_NE(j->getLeft(), nullptr);
    EXPECT_EQ(j->getLeft()->getType(), LogicalOpType::LOP_FILTER);
}

TEST_F(FilterPushdownRuleEdgeCasesTest, JoinPushdownCollectsVarsFromInList) {
    auto left = std::make_unique<LogicalNodeScan>("n");
    auto right = std::make_unique<LogicalNodeScan>("m");
    auto join = std::make_unique<LogicalJoin>(std::move(left), std::move(right));
    auto pred = inListExpr(propRef("n", "age"));
    auto filter = std::make_unique<LogicalFilter>(std::move(join), pred);

    auto result = rule.apply(std::move(filter), stats);

    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->getType(), LogicalOpType::LOP_JOIN);
    auto *j = static_cast<LogicalJoin *>(result.get());
    ASSERT_NE(j->getLeft(), nullptr);
    EXPECT_EQ(j->getLeft()->getType(), LogicalOpType::LOP_FILTER);
}
