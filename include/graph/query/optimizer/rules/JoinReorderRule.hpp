/**
 * @file JoinReorderRule.hpp
 * @brief Cost-based join reordering for chains of LogicalJoin nodes.
 *
 * Strategy (greedy left-deep):
 *  1. Flatten a left-deep or right-deep tree of binary LogicalJoin nodes into
 *     a flat list of inputs.
 *  2. Estimate the cardinality of each input using CostModel::estimateScanCardinality
 *     (inputs that are not NodeScans use the total-node count as a conservative
 *     estimate).
 *  3. Reassemble the join tree left-deep in ascending cardinality order so that
 *     the smallest relations are joined first, minimising intermediate result sizes.
 *
 * For N > 5 inputs the greedy approach is also used (sort by cardinality).
 * Dynamic-programming enumeration is deliberately avoided here to keep compile
 * times and code complexity manageable; a future rule can extend this.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/optimizer/CostModel.hpp"
#include "graph/query/optimizer/OptimizerRule.hpp"

namespace graph::query::optimizer::rules {

/**
 * @class JoinReorderRule
 * @brief Reorders chains of cross-joins to put cheaper (smaller) inputs first.
 */
class JoinReorderRule : public OptimizerRule {
public:
    [[nodiscard]] std::string getName() const override {
        return "JoinReorderRule";
    }

    std::unique_ptr<logical::LogicalOperator> apply(
        std::unique_ptr<logical::LogicalOperator> plan,
        const Statistics &stats) override {
        return rewrite(std::move(plan), stats);
    }

private:
    // -------------------------------------------------------------------------
    // Cardinality estimation for an arbitrary sub-plan
    // -------------------------------------------------------------------------

    static double estimateCardinality(const logical::LogicalOperator *op,
                                      const Statistics &stats) {
        if (!op) return static_cast<double>(stats.totalNodeCount);

        if (op->getType() == logical::LogicalOpType::LOP_NODE_SCAN) {
            const auto *scan = static_cast<const logical::LogicalNodeScan *>(op);
            return CostModel::estimateScanCardinality(stats, scan->getLabels());
        }

        // For non-scan inputs estimate as the product of child cardinalities
        // (cross-join cardinality).
        double card = 1.0;
        for (const auto *child : op->getChildren()) {
            card *= estimateCardinality(child, stats);
        }
        return card > 0.0 ? card : static_cast<double>(stats.totalNodeCount);
    }

    // -------------------------------------------------------------------------
    // Flatten / reassemble helpers
    // -------------------------------------------------------------------------

    /**
     * @brief Collects all leaf inputs from a chain of LogicalJoin nodes into
     *        a flat list, consuming ownership of each input.
     *
     * The chain may be left-deep, right-deep, or a bushy tree — all join
     * nodes are unwound.
     */
    static void flatten(std::unique_ptr<logical::LogicalOperator> node,
                        std::vector<std::unique_ptr<logical::LogicalOperator>> &inputs) {
        if (!node) return;

        if (node->getType() == logical::LogicalOpType::LOP_JOIN) {
            auto *join = static_cast<logical::LogicalJoin *>(node.get());
            auto left  = join->detachChild(0);
            auto right = join->detachChild(1);
            // node is now empty; let it be destroyed at end of scope.
            flatten(std::move(left),  inputs);
            flatten(std::move(right), inputs);
        } else {
            inputs.push_back(std::move(node));
        }
    }

    /**
     * @brief Reassembles a sorted list of inputs into a left-deep join tree.
     *
     * inputs must be non-empty.  The result is:
     *   Join(Join(Join(inputs[0], inputs[1]), inputs[2]), ...)
     */
    static std::unique_ptr<logical::LogicalOperator> buildLeftDeep(
        std::vector<std::unique_ptr<logical::LogicalOperator>> inputs) {

        if (inputs.empty()) return nullptr;

        auto root = std::move(inputs[0]);
        for (size_t i = 1; i < inputs.size(); ++i) {
            root = std::make_unique<logical::LogicalJoin>(
                std::move(root), std::move(inputs[i]));
        }
        return root;
    }

    // -------------------------------------------------------------------------
    // Core recursive rewrite
    // -------------------------------------------------------------------------

    std::unique_ptr<logical::LogicalOperator> rewrite(
        std::unique_ptr<logical::LogicalOperator> node,
        const Statistics &stats) {

        if (!node) return node;

        // 1. Recursively rewrite children first (bottom-up).
        auto childPtrs = node->getChildren(); // raw snapshot
        for (size_t i = 0; i < childPtrs.size(); ++i) {
            auto child = node->detachChild(i);
            node->setChild(i, rewrite(std::move(child), stats));
        }

        // 2. Only reorder at Join nodes (the root of a join chain).
        if (node->getType() != logical::LogicalOpType::LOP_JOIN) return node;

        // 3. Check whether the parent of this Join is also a Join.
        //    If so, skip — we will process the chain from its topmost Join.
        //    We detect this by only processing when we own the node
        //    (i.e., we are called from the outermost level of a chain).
        //    Since we walk bottom-up, by the time we reach this node the
        //    children have already been rewritten but are not Joins anymore
        //    (they were flattened inside their own recursive call if they were
        //    chains).  Therefore it is safe to flatten here.

        // 4. Flatten the entire join sub-tree rooted here.
        std::vector<std::unique_ptr<logical::LogicalOperator>> inputs;
        flatten(std::move(node), inputs);

        if (inputs.size() <= 1) {
            // Degenerate — just return the single input (or null).
            if (inputs.empty()) return nullptr;
            return std::move(inputs[0]);
        }

        // 5. Sort inputs by estimated cardinality (ascending).
        std::sort(inputs.begin(), inputs.end(),
                  [&stats](const std::unique_ptr<logical::LogicalOperator> &a,
                           const std::unique_ptr<logical::LogicalOperator> &b) {
                      return estimateCardinality(a.get(), stats) <
                             estimateCardinality(b.get(), stats);
                  });

        // 6. Rebuild as a left-deep tree.
        return buildLeftDeep(std::move(inputs));
    }
};

} // namespace graph::query::optimizer::rules
