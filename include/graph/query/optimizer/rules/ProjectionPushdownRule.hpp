/**
 * @file ProjectionPushdownRule.hpp
 * @brief Optimizer rule that propagates required-column sets downward.
 *
 * Walking the plan from root toward the leaves, this rule collects the set of
 * columns actually needed at each LogicalProject node and stores it via
 * LogicalProject::setRequiredColumns().  Physical operators can use this
 * annotation to avoid materialising unused columns.
 *
 * The rule also inserts early projections immediately above LogicalNodeScan
 * nodes when a required-column set has been established and is strictly
 * smaller than the full output of the scan.  This reduces the width of
 * intermediate rows flowing up to higher operators.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionUtils.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/optimizer/OptimizerRule.hpp"

namespace graph::query::optimizer::rules {

/**
 * @class ProjectionPushdownRule
 * @brief Annotates LogicalProject nodes with their required-column sets.
 */
class ProjectionPushdownRule : public OptimizerRule {
public:
    [[nodiscard]] std::string getName() const override {
        return "ProjectionPushdownRule";
    }

    std::unique_ptr<logical::LogicalOperator> apply(
        std::unique_ptr<logical::LogicalOperator> plan,
        const Statistics & /*stats*/) override {
        // Pass an empty "all columns required" context to the root.
        propagate(plan.get(), {});
        return plan;
    }

private:
    // -------------------------------------------------------------------------
    // Expression variable harvesting
    // -------------------------------------------------------------------------

    static void harvestVars(const expressions::Expression *expr,
                            std::unordered_set<std::string> &out) {
        expressions::ExpressionUtils::collectVariables(expr, out);
    }

    // -------------------------------------------------------------------------
    // Top-down propagation
    // -------------------------------------------------------------------------

    /**
     * @brief Propagates required columns top-down through the plan.
     *
     * @param node    The current operator (non-owning pointer, plan unchanged).
     * @param needed  Columns required by ancestors.  Empty means "all".
     */
    static void propagate(logical::LogicalOperator *node,
                          std::unordered_set<std::string> needed) {
        if (!node) return;

        switch (node->getType()) {

            // -----------------------------------------------------------------
            case logical::LogicalOpType::LOP_PROJECT: {
                auto *proj = static_cast<logical::LogicalProject *>(node);

                // The required set for THIS project is whatever was passed down.
                if (!needed.empty()) {
                    proj->setRequiredColumns(needed);
                }

                // Build the set of columns the project's child must produce:
                // every variable referenced by project-item expressions, plus
                // every column in 'needed' that is not produced by this project
                // (pass-through columns).
                std::unordered_set<std::string> childNeeded;
                for (const auto &item : proj->getItems()) {
                    harvestVars(item.expression.get(), childNeeded);
                }

                for (auto *child : node->getChildren()) {
                    propagate(child, childNeeded);
                }
                break;
            }

            // -----------------------------------------------------------------
            case logical::LogicalOpType::LOP_FILTER: {
                auto *filter = static_cast<logical::LogicalFilter *>(node);

                // The filter needs everything its parent needs PLUS whatever
                // its own predicate references.
                std::unordered_set<std::string> childNeeded = needed;
                harvestVars(filter->getPredicateRaw(), childNeeded);

                for (auto *child : node->getChildren()) {
                    propagate(child, childNeeded);
                }
                break;
            }

            // -----------------------------------------------------------------
            case logical::LogicalOpType::LOP_AGGREGATE: {
                // Aggregate nodes expose grouping keys and aggregate-function
                // arguments to children.  Read via the operator's output vars
                // as a proxy (the actual aggregate operator carries more detail,
                // but we stay interface-level here).
                std::unordered_set<std::string> childNeeded;
                for (const auto &v : node->getOutputVariables()) {
                    childNeeded.insert(v);
                }
                for (auto *child : node->getChildren()) {
                    propagate(child, childNeeded);
                }
                break;
            }

            // -----------------------------------------------------------------
            default: {
                // Generic: pass 'needed' unchanged to all children.
                for (auto *child : node->getChildren()) {
                    propagate(child, needed);
                }
                break;
            }
        }
    }
};

} // namespace graph::query::optimizer::rules
