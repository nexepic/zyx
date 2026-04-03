/**
 * @file PredicateSimplificationRule.hpp
 * @brief Optimizer rule that simplifies and merges filter predicates.
 *
 * Transformations applied:
 *  - Constant folding: `true AND x` → `x`, `false OR x` → `x`,
 *    `true OR x` → `true`, `false AND x` → `false`.
 *  - NOT elimination: `NOT (NOT x)` → `x`.
 *  - Adjacent-filter merging: two consecutive LogicalFilters →
 *    one filter with their predicates AND-combined.
 *  - Duplicate-filter elimination: two consecutive filters with
 *    syntactically identical predicates → one filter.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <memory>
#include <string>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/optimizer/OptimizerRule.hpp"

namespace graph::query::optimizer::rules {

/**
 * @class PredicateSimplificationRule
 * @brief Simplifies predicate expressions and merges redundant filter nodes.
 */
class PredicateSimplificationRule : public OptimizerRule {
public:
    [[nodiscard]] std::string getName() const override {
        return "PredicateSimplificationRule";
    }

    std::unique_ptr<logical::LogicalOperator> apply(
        std::unique_ptr<logical::LogicalOperator> plan,
        const Statistics & /*stats*/) override {
        return rewrite(std::move(plan));
    }

private:
    // -------------------------------------------------------------------------
    // Expression simplification
    // -------------------------------------------------------------------------

    /**
     * @brief Simplifies a single expression tree.
     *
     * Returns a new (possibly different) expression, or the original if no
     * transformation applies.  Ownership is transferred in and out.
     */
    static std::shared_ptr<expressions::Expression> simplifyExpr(
        std::shared_ptr<expressions::Expression> expr) {

        if (!expr) return expr;

        using expressions::ExpressionType;
        using expressions::BinaryOperatorType;
        using expressions::UnaryOperatorType;

        // --- Unary: NOT(NOT(x)) → x ---
        if (expr->getExpressionType() == ExpressionType::UNARY_OP) {
            const auto *unary =
                static_cast<const expressions::UnaryOpExpression *>(expr.get());
            if (unary->getOperator() == UnaryOperatorType::UOP_NOT) {
                const expressions::Expression *inner = unary->getOperand();
                if (inner &&
                    inner->getExpressionType() == ExpressionType::UNARY_OP) {
                    const auto *innerUnary =
                        static_cast<const expressions::UnaryOpExpression *>(inner);
                    if (innerUnary->getOperator() == UnaryOperatorType::UOP_NOT) {
                        // Double negation — return clone of the inner operand.
                        if (!innerUnary->getOperand()) return expr;
                        return std::shared_ptr<expressions::Expression>(
                            innerUnary->getOperand()->clone());
                    }
                }
            }
        }

        // --- Binary constant folding ---
        if (expr->getExpressionType() == ExpressionType::BINARY_OP) {
            auto *bin = static_cast<expressions::BinaryOpExpression *>(expr.get());
            BinaryOperatorType op = bin->getOperator();

            // Only fold logical operators.
            if (op == BinaryOperatorType::BOP_AND ||
                op == BinaryOperatorType::BOP_OR) {

                const expressions::Expression *lhs = bin->getLeft();
                const expressions::Expression *rhs = bin->getRight();

                auto isBoolLiteral = [](const expressions::Expression *e,
                                        bool &val) -> bool {
                    if (!e) return false;
                    if (e->getExpressionType() != ExpressionType::LITERAL) return false;
                    const auto *lit =
                        static_cast<const expressions::LiteralExpression *>(e);
                    if (!lit->isBoolean()) return false;
                    val = lit->getBooleanValue();
                    return true;
                };

                bool lhsBool = false;
                bool rhsBool = false;

                if (op == BinaryOperatorType::BOP_AND) {
                    // false AND x → false
                    if (isBoolLiteral(lhs, lhsBool) && !lhsBool)
                        return std::make_shared<expressions::LiteralExpression>(false);
                    // x AND false → false
                    if (isBoolLiteral(rhs, rhsBool) && !rhsBool)
                        return std::make_shared<expressions::LiteralExpression>(false);
                    // true AND x → x
                    if (isBoolLiteral(lhs, lhsBool) && lhsBool && rhs)
                        return std::shared_ptr<expressions::Expression>(rhs->clone());
                    // x AND true → x
                    if (isBoolLiteral(rhs, rhsBool) && rhsBool && lhs)
                        return std::shared_ptr<expressions::Expression>(lhs->clone());
                }

                if (op == BinaryOperatorType::BOP_OR) {
                    // true OR x → true
                    if (isBoolLiteral(lhs, lhsBool) && lhsBool)
                        return std::make_shared<expressions::LiteralExpression>(true);
                    // x OR true → true
                    if (isBoolLiteral(rhs, rhsBool) && rhsBool)
                        return std::make_shared<expressions::LiteralExpression>(true);
                    // false OR x → x
                    if (isBoolLiteral(lhs, lhsBool) && !lhsBool && rhs)
                        return std::shared_ptr<expressions::Expression>(rhs->clone());
                    // x OR false → x
                    if (isBoolLiteral(rhs, rhsBool) && !rhsBool && lhs)
                        return std::shared_ptr<expressions::Expression>(lhs->clone());
                }
            }
        }

        return expr;
    }

    // -------------------------------------------------------------------------
    // Tree rewrite
    // -------------------------------------------------------------------------

    std::unique_ptr<logical::LogicalOperator> rewrite(
        std::unique_ptr<logical::LogicalOperator> node) {

        if (!node) return node;

        // 1. Process children recursively (bottom-up).
        auto childPtrs = node->getChildren();
        for (size_t i = 0; i < childPtrs.size(); ++i) {
            auto child = node->detachChild(i);
            node->setChild(i, rewrite(std::move(child)));
        }

        // 2. Only act on Filter nodes.
        if (node->getType() != logical::LogicalOpType::LOP_FILTER) return node;

        auto *filterNode = static_cast<logical::LogicalFilter *>(node.get());

        // 2a. Simplify this filter's predicate.
        auto pred = simplifyExpr(filterNode->getPredicate());

        // If the predicate simplified to literal true, drop the filter entirely.
        if (pred &&
            pred->getExpressionType() == expressions::ExpressionType::LITERAL) {
            const auto *lit =
                static_cast<const expressions::LiteralExpression *>(pred.get());
            if (lit->isBoolean() && lit->getBooleanValue()) {
                return filterNode->detachChild(0);
            }
        }

        // 2b. Check if child is also a Filter → merge or deduplicate.
        logical::LogicalOperator *child = filterNode->getChildren()[0];
        if (child && child->getType() == logical::LogicalOpType::LOP_FILTER) {
            auto *childFilter = static_cast<logical::LogicalFilter *>(child);
            auto childPred    = childFilter->getPredicate();

            // Deduplicate: same string representation → drop outer filter.
            if (pred && childPred &&
                pred->toString() == childPred->toString()) {
                return filterNode->detachChild(0);
            }

            // Merge: combine both predicates with AND.
            if (pred && childPred) {
                auto combined = std::make_shared<expressions::BinaryOpExpression>(
                    pred->clone(),
                    expressions::BinaryOperatorType::BOP_AND,
                    childPred->clone());
                auto grandChild = childFilter->detachChild(0);
                return std::make_unique<logical::LogicalFilter>(
                    std::move(grandChild), simplifyExpr(std::move(combined)));
            }
        }

        // Rebuild with the (possibly simplified) predicate.
        if (pred != filterNode->getPredicate()) {
            auto grandChild = filterNode->detachChild(0);
            return std::make_unique<logical::LogicalFilter>(
                std::move(grandChild), std::move(pred));
        }

        return node;
    }
};

} // namespace graph::query::optimizer::rules
