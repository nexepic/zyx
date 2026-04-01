/**
 * @file FilterPushdownRule.hpp
 * @brief Optimizer rule that pushes filter predicates toward data sources.
 *
 * Transformations applied:
 *  - Split conjunctive (AND) predicates into independent filters.
 *  - Push filters past LogicalProject nodes when the predicate only
 *    references columns available below the projection.
 *  - Push filters into one side of a LogicalJoin when the predicate
 *    references variables from only that side.
 *  - Merge property-equality filters directly into LogicalNodeScan's
 *    propertyPredicates_ (predicate-into-scan pushdown).
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/optimizer/OptimizerRule.hpp"

namespace graph::query::optimizer::rules {

/**
 * @class FilterPushdownRule
 * @brief Pushes filter predicates as close to their data sources as possible.
 */
class FilterPushdownRule : public OptimizerRule {
public:
    [[nodiscard]] std::string getName() const override { return "FilterPushdownRule"; }

    std::unique_ptr<logical::LogicalOperator> apply(
        std::unique_ptr<logical::LogicalOperator> plan,
        const Statistics & /*stats*/) override {
        return pushDown(std::move(plan));
    }

private:
    // -------------------------------------------------------------------------
    // Variable collection helper
    // -------------------------------------------------------------------------

    /**
     * @brief Collects all variable names referenced inside an expression.
     *
     * Only the base variable name (e.g. "n" from "n.age") is recorded so that
     * we can compare against getOutputVariables().
     */
    static void collectReferencedVariables(
        const expressions::Expression *expr,
        std::set<std::string> &out) {

        if (!expr) return;

        using expressions::ExpressionType;

        switch (expr->getExpressionType()) {
            case ExpressionType::VARIABLE_REFERENCE:
            case ExpressionType::PROPERTY_ACCESS: {
                const auto *varExpr =
                    static_cast<const expressions::VariableReferenceExpression *>(expr);
                out.insert(varExpr->getVariableName());
                break;
            }
            case ExpressionType::BINARY_OP: {
                const auto *binExpr =
                    static_cast<const expressions::BinaryOpExpression *>(expr);
                collectReferencedVariables(binExpr->getLeft(), out);
                collectReferencedVariables(binExpr->getRight(), out);
                break;
            }
            case ExpressionType::UNARY_OP: {
                const auto *unExpr =
                    static_cast<const expressions::UnaryOpExpression *>(expr);
                collectReferencedVariables(unExpr->getOperand(), out);
                break;
            }
            case ExpressionType::IS_NULL: {
                const auto *isNullExpr =
                    static_cast<const expressions::IsNullExpression *>(expr);
                collectReferencedVariables(isNullExpr->getExpression(), out);
                break;
            }
            case ExpressionType::IN_LIST: {
                const auto *inExpr =
                    static_cast<const expressions::InExpression *>(expr);
                collectReferencedVariables(inExpr->getValue(), out);
                break;
            }
            default:
                // Literals, function calls, etc. — no variable references to collect.
                break;
        }
    }

    // -------------------------------------------------------------------------
    // Conjunct splitting
    // -------------------------------------------------------------------------

    /**
     * @brief Recursively splits an AND expression into its component predicates.
     *
     * Example: (a AND b) AND c  →  {a, b, c}
     */
    static void splitConjuncts(
        std::shared_ptr<expressions::Expression> expr,
        std::vector<std::shared_ptr<expressions::Expression>> &out) {

        if (!expr) return;

        if (expr->getExpressionType() == expressions::ExpressionType::BINARY_OP) {
            const auto *binExpr =
                static_cast<const expressions::BinaryOpExpression *>(expr.get());
            if (binExpr->getOperator() == expressions::BinaryOperatorType::BOP_AND) {
                // Reclaim ownership of each child clone so we can store them independently.
                splitConjuncts(
                    std::shared_ptr<expressions::Expression>(binExpr->getLeft()->clone()),
                    out);
                splitConjuncts(
                    std::shared_ptr<expressions::Expression>(binExpr->getRight()->clone()),
                    out);
                return;
            }
        }
        out.push_back(std::move(expr));
    }

    // -------------------------------------------------------------------------
    // Helpers: variable-set utilities
    // -------------------------------------------------------------------------

    static std::set<std::string> outputVarSet(const logical::LogicalOperator *op) {
        std::set<std::string> s;
        if (!op) return s;
        for (const auto &v : op->getOutputVariables()) s.insert(v);
        return s;
    }

    static bool subsetOf(const std::set<std::string> &a,
                         const std::set<std::string> &b) {
        for (const auto &x : a)
            if (b.find(x) == b.end()) return false;
        return true;
    }

    // -------------------------------------------------------------------------
    // Check whether a predicate is a simple property-equality suitable for
    // merging into a NodeScan.
    //
    // Matches:  <var>.prop = <literal>   or   <literal> = <var>.prop
    // Returns true and sets key/value when matched.
    // -------------------------------------------------------------------------

    static bool extractPropertyEquality(
        const expressions::Expression *pred,
        const std::string &scanVariable,
        std::string &outKey,
        graph::PropertyValue &outValue) {

        if (!pred) return false;
        if (pred->getExpressionType() != expressions::ExpressionType::BINARY_OP)
            return false;

        const auto *bin = static_cast<const expressions::BinaryOpExpression *>(pred);
        if (bin->getOperator() != expressions::BinaryOperatorType::BOP_EQUAL) return false;

        const expressions::Expression *lhs = bin->getLeft();
        const expressions::Expression *rhs = bin->getRight();

        // Try  (n.prop = literal)
        auto tryMatch = [&](const expressions::Expression *propSide,
                            const expressions::Expression *litSide) -> bool {
            if (!propSide || !litSide) return false;
            if (propSide->getExpressionType() != expressions::ExpressionType::PROPERTY_ACCESS)
                return false;
            if (litSide->getExpressionType() != expressions::ExpressionType::LITERAL)
                return false;

            const auto *varExpr =
                static_cast<const expressions::VariableReferenceExpression *>(propSide);
            if (varExpr->getVariableName() != scanVariable) return false;
            if (!varExpr->hasProperty()) return false;

            const auto *litExpr =
                static_cast<const expressions::LiteralExpression *>(litSide);
            outKey = varExpr->getPropertyName();

            if (litExpr->isNull())         outValue = graph::PropertyValue{};
            else if (litExpr->isBoolean()) outValue = graph::PropertyValue(litExpr->getBooleanValue());
            else if (litExpr->isInteger()) outValue = graph::PropertyValue(litExpr->getIntegerValue());
            else if (litExpr->isDouble())  outValue = graph::PropertyValue(litExpr->getDoubleValue());
            else                           outValue = graph::PropertyValue(litExpr->getStringValue());
            return true;
        };

        return tryMatch(lhs, rhs) || tryMatch(rhs, lhs);
    }

    // -------------------------------------------------------------------------
    // Check whether a predicate is a range comparison suitable for merging
    // into a NodeScan's rangePredicates.
    //
    // Matches: <var>.prop >/>=/</<= <literal>  or  <literal> >/>=/</<= <var>.prop
    // Returns true and sets key, value, isMin (bound direction), inclusive.
    // -------------------------------------------------------------------------

    static bool extractPropertyRange(
        const expressions::Expression *pred,
        const std::string &scanVariable,
        std::string &outKey,
        graph::PropertyValue &outValue,
        bool &outIsMin,
        bool &outInclusive) {

        if (!pred) return false;
        if (pred->getExpressionType() != expressions::ExpressionType::BINARY_OP)
            return false;

        const auto *bin = static_cast<const expressions::BinaryOpExpression *>(pred);
        auto op = bin->getOperator();

        // Only handle comparison operators
        if (op != expressions::BinaryOperatorType::BOP_LESS &&
            op != expressions::BinaryOperatorType::BOP_LESS_EQUAL &&
            op != expressions::BinaryOperatorType::BOP_GREATER &&
            op != expressions::BinaryOperatorType::BOP_GREATER_EQUAL)
            return false;

        const expressions::Expression *lhs = bin->getLeft();
        const expressions::Expression *rhs = bin->getRight();

        // Try (var.prop OP literal)
        auto tryMatch = [&](const expressions::Expression *propSide,
                            const expressions::Expression *litSide,
                            bool reversed) -> bool {
            if (!propSide || !litSide) return false;
            if (propSide->getExpressionType() != expressions::ExpressionType::PROPERTY_ACCESS)
                return false;
            if (litSide->getExpressionType() != expressions::ExpressionType::LITERAL)
                return false;

            const auto *varExpr =
                static_cast<const expressions::VariableReferenceExpression *>(propSide);
            if (varExpr->getVariableName() != scanVariable) return false;
            if (!varExpr->hasProperty()) return false;

            const auto *litExpr =
                static_cast<const expressions::LiteralExpression *>(litSide);
            outKey = varExpr->getPropertyName();

            if (litExpr->isNull()) return false; // null range makes no sense
            if (litExpr->isBoolean()) return false; // boolean range not meaningful
            if (litExpr->isInteger()) outValue = graph::PropertyValue(litExpr->getIntegerValue());
            else if (litExpr->isDouble()) outValue = graph::PropertyValue(litExpr->getDoubleValue());
            else outValue = graph::PropertyValue(litExpr->getStringValue());

            // Determine direction:
            // var.prop > literal  → literal is min bound (exclusive)
            // var.prop >= literal → literal is min bound (inclusive)
            // var.prop < literal  → literal is max bound (exclusive)
            // var.prop <= literal → literal is max bound (inclusive)
            // When reversed (literal OP var.prop), flip the direction
            auto effectiveOp = op;
            if (reversed) {
                if (op == expressions::BinaryOperatorType::BOP_LESS)
                    effectiveOp = expressions::BinaryOperatorType::BOP_GREATER;
                else if (op == expressions::BinaryOperatorType::BOP_LESS_EQUAL)
                    effectiveOp = expressions::BinaryOperatorType::BOP_GREATER_EQUAL;
                else if (op == expressions::BinaryOperatorType::BOP_GREATER)
                    effectiveOp = expressions::BinaryOperatorType::BOP_LESS;
                else if (op == expressions::BinaryOperatorType::BOP_GREATER_EQUAL)
                    effectiveOp = expressions::BinaryOperatorType::BOP_LESS_EQUAL;
            }

            if (effectiveOp == expressions::BinaryOperatorType::BOP_GREATER) {
                outIsMin = true; outInclusive = false;
            } else if (effectiveOp == expressions::BinaryOperatorType::BOP_GREATER_EQUAL) {
                outIsMin = true; outInclusive = true;
            } else if (effectiveOp == expressions::BinaryOperatorType::BOP_LESS) {
                outIsMin = false; outInclusive = false;
            } else { // BOP_LESS_EQUAL
                outIsMin = false; outInclusive = true;
            }
            return true;
        };

        return tryMatch(lhs, rhs, false) || tryMatch(rhs, lhs, true);
    }

    // -------------------------------------------------------------------------
    // Core recursive rewrite
    // -------------------------------------------------------------------------

    /**
     * @brief Recursively rewrites the plan tree, pushing filters downward.
     *
     * The function processes children bottom-up first, then handles any
     * LogicalFilter at the current node.
     */
    std::unique_ptr<logical::LogicalOperator> pushDown(
        std::unique_ptr<logical::LogicalOperator> node) {

        if (!node) return node;

        // 1. Process children recursively (bottom-up).
        auto children = node->getChildren(); // raw pointers, for counting
        for (size_t i = 0; i < children.size(); ++i) {
            auto child = node->detachChild(i);
            node->setChild(i, pushDown(std::move(child)));
        }

        // 2. Only act on Filter nodes.
        if (node->getType() != logical::LogicalOpType::LOP_FILTER) return node;

        auto *filterNode = static_cast<logical::LogicalFilter *>(node.get());
        auto predicate    = filterNode->getPredicate(); // shared_ptr

        // 3. Split conjunctive predicates.
        std::vector<std::shared_ptr<expressions::Expression>> conjuncts;
        splitConjuncts(predicate, conjuncts);

        // If we split into multiple parts, rebuild as stacked filters on the
        // detached child so each part can be pushed independently.
        if (conjuncts.size() > 1) {
            auto child = filterNode->detachChild(0);
            // Build the stack from bottom (last conjunct) to top (first conjunct).
            for (int i = static_cast<int>(conjuncts.size()) - 1; i >= 0; --i) {
                child = std::make_unique<logical::LogicalFilter>(
                    std::move(child), conjuncts[static_cast<size_t>(i)]);
            }
            // Re-enter pushDown on the newly built stack.
            return pushDown(std::move(child));
        }

        // 4. Single predicate — try to push it past the child.
        logical::LogicalOperator *child = filterNode->getChildren()[0];
        if (!child) return node;

        const logical::LogicalOpType childType = child->getType();

        // --- 4a. Push past a Project ---
        if (childType == logical::LogicalOpType::LOP_PROJECT) {
            // The predicate can move below the project if every variable it
            // references is available in the project's child output.
            auto *proj = static_cast<logical::LogicalProject *>(child);
            logical::LogicalOperator *grandChild =
                proj->getChildren().empty() ? nullptr : proj->getChildren()[0];

            if (grandChild) {
                std::set<std::string> predVars;
                collectReferencedVariables(predicate.get(), predVars);
                std::set<std::string> gcVars = outputVarSet(grandChild);

                if (subsetOf(predVars, gcVars)) {
                    // Detach: filter → project → grandChild
                    // Rebuild: project → filter → grandChild
                    auto ownedProj  = filterNode->detachChild(0);          // project
                    auto ownedGrand = static_cast<logical::LogicalProject *>(
                        ownedProj.get())->detachChild(0);                  // grandChild
                    auto newFilter  = std::make_unique<logical::LogicalFilter>(
                        std::move(ownedGrand), predicate);
                    ownedProj->setChild(0, std::move(newFilter));
                    return pushDown(std::move(ownedProj));
                }
            }
        }

        // --- 4b. Push past a Join ---
        if (childType == logical::LogicalOpType::LOP_JOIN) {
            auto *join = static_cast<logical::LogicalJoin *>(child);
            logical::LogicalOperator *left  = join->getLeft();
            logical::LogicalOperator *right = join->getRight();

            std::set<std::string> predVars;
            collectReferencedVariables(predicate.get(), predVars);
            std::set<std::string> leftVars  = outputVarSet(left);
            std::set<std::string> rightVars = outputVarSet(right);

            if (subsetOf(predVars, leftVars)) {
                auto ownedJoin  = filterNode->detachChild(0);
                auto ownedLeft  = static_cast<logical::LogicalJoin *>(
                    ownedJoin.get())->detachChild(0);
                auto newFilter  = std::make_unique<logical::LogicalFilter>(
                    std::move(ownedLeft), predicate);
                ownedJoin->setChild(0, std::move(newFilter));
                return pushDown(std::move(ownedJoin));
            }

            if (subsetOf(predVars, rightVars)) {
                auto ownedJoin  = filterNode->detachChild(0);
                auto ownedRight = static_cast<logical::LogicalJoin *>(
                    ownedJoin.get())->detachChild(1);
                auto newFilter  = std::make_unique<logical::LogicalFilter>(
                    std::move(ownedRight), predicate);
                ownedJoin->setChild(1, std::move(newFilter));
                return pushDown(std::move(ownedJoin));
            }
        }

        // --- 4c. Merge property-equality into NodeScan ---
        if (childType == logical::LogicalOpType::LOP_NODE_SCAN) {
            auto *scan = static_cast<logical::LogicalNodeScan *>(child);
            std::string propKey;
            graph::PropertyValue propVal;

            if (extractPropertyEquality(predicate.get(), scan->getVariable(),
                                        propKey, propVal)) {
                // Augment the scan's predicates and eliminate the filter.
                auto preds = scan->getPropertyPredicates();
                // Avoid duplicate keys.
                bool found = false;
                for (auto &kv : preds) {
                    if (kv.first == propKey) { found = true; break; }
                }
                if (!found) {
                    preds.emplace_back(propKey, propVal);
                    scan->setPropertyPredicates(std::move(preds));
                    // Return just the child (scan), dropping the filter wrapper.
                    return filterNode->detachChild(0);
                }
            }

            // --- 4d. Merge range comparison into NodeScan ---
            std::string rangeKey;
            graph::PropertyValue rangeValue;
            bool isMin = false;
            bool inclusive = true;

            if (extractPropertyRange(predicate.get(), scan->getVariable(),
                                     rangeKey, rangeValue, isMin, inclusive)) {
                auto rangePreds = scan->getRangePredicates();
                // Find or create a RangePredicate for this key
                logical::RangePredicate *existing = nullptr;
                for (auto &rp : rangePreds) {
                    if (rp.key == rangeKey) { existing = &rp; break; }
                }
                if (!existing) {
                    rangePreds.push_back(logical::RangePredicate{rangeKey, {}, {}, true, true});
                    existing = &rangePreds.back();
                }
                if (isMin) {
                    // Keep tighter bound
                    if (existing->minValue.getType() == graph::PropertyType::NULL_TYPE ||
                        rangeValue > existing->minValue ||
                        (rangeValue == existing->minValue && !inclusive)) {
                        existing->minValue = rangeValue;
                        existing->minInclusive = inclusive;
                    }
                } else {
                    // Keep tighter bound
                    if (existing->maxValue.getType() == graph::PropertyType::NULL_TYPE ||
                        rangeValue < existing->maxValue ||
                        (rangeValue == existing->maxValue && !inclusive)) {
                        existing->maxValue = rangeValue;
                        existing->maxInclusive = inclusive;
                    }
                }
                scan->setRangePredicates(std::move(rangePreds));
                return filterNode->detachChild(0);
            }
        }

        // No transformation applicable — return as-is.
        return node;
    }
};

} // namespace graph::query::optimizer::rules
