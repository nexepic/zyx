/**
 * @file EnhancedIndexSelectionRule.hpp
 * @brief Cost-based index selection rule for LogicalNodeScan operators.
 *
 * Replaces the hard-coded heuristic in IndexPushdownRule with a proper
 * cost-model comparison.  For every LogicalNodeScan that has both labels and
 * property predicates available, the rule estimates:
 *
 *  - Cost of a FULL_SCAN
 *  - Cost of a LABEL_SCAN (using the most selective label)
 *  - Cost of a PROPERTY_SCAN for each equality predicate that has an index
 *
 * The cheapest strategy is recorded in the scan node's preferredScanType_
 * field (via the mutator added below).  The PhysicalPlanConverter reads this
 * hint when building the physical plan.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/optimizer/CostModel.hpp"
#include "graph/query/optimizer/OptimizerRule.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::optimizer::rules {

/**
 * @class EnhancedIndexSelectionRule
 * @brief Uses CostModel + Statistics to choose the best scan strategy for
 *        every LogicalNodeScan in the plan tree.
 */
class EnhancedIndexSelectionRule : public OptimizerRule {
public:
    explicit EnhancedIndexSelectionRule(
        std::shared_ptr<indexes::IndexManager> indexManager)
        : indexManager_(std::move(indexManager)) {}

    [[nodiscard]] std::string getName() const override {
        return "EnhancedIndexSelectionRule";
    }

    std::unique_ptr<logical::LogicalOperator> apply(
        std::unique_ptr<logical::LogicalOperator> plan,
        const Statistics &stats) override {
        rewrite(plan.get(), stats);
        return plan;
    }

private:
    std::shared_ptr<indexes::IndexManager> indexManager_;

    // -------------------------------------------------------------------------
    // Recursive tree walk (in-place annotation, no structural change)
    // -------------------------------------------------------------------------

    void rewrite(logical::LogicalOperator *node, const Statistics &stats) {
        if (!node) return;

        if (node->getType() == logical::LogicalOpType::LOP_NODE_SCAN) {
            selectBestScan(static_cast<logical::LogicalNodeScan *>(node), stats);
            return; // Leaf node — no children.
        }

        for (auto *child : node->getChildren()) {
            rewrite(child, stats);
        }
    }

    // -------------------------------------------------------------------------
    // Cost-based scan selection
    // -------------------------------------------------------------------------

    void selectBestScan(logical::LogicalNodeScan *scan,
                        const Statistics &stats) const {
        if (!indexManager_) return;

        const auto &labels     = scan->getLabels();
        const auto &propPreds  = scan->getPropertyPredicates();
        const std::string firstLabel = labels.empty() ? "" : labels[0];

        // ----- Full scan -----
        double bestCost = CostModel::fullScanCost(stats);
        execution::ScanType bestType = execution::ScanType::FULL_SCAN;

        // ----- Label scan -----
        if (!firstLabel.empty() &&
            indexManager_->hasLabelIndex("node")) {
            double labelCost = CostModel::labelScanCost(stats, firstLabel);
            if (labelCost < bestCost) {
                bestCost = labelCost;
                bestType = execution::ScanType::LABEL_SCAN;
            }
        }

        // ----- Property scan (one candidate per predicate key) -----
        for (const auto &kv : propPreds) {
            const std::string &key = kv.first;
            if (!indexManager_->hasPropertyIndex("node", key)) continue;

            double propCost = CostModel::propertyIndexCost(
                stats, firstLabel, key, /*isEquality=*/true);
            if (propCost < bestCost) {
                bestCost = propCost;
                bestType = execution::ScanType::PROPERTY_SCAN;
            }
        }

        // ----- Check if multiple equality predicates can form a composite index -----
        if (propPreds.size() >= 2) {
            std::vector<std::string> keys;
            std::vector<PropertyValue> values;
            for (const auto &kv : propPreds) {
                keys.push_back(kv.first);
                values.push_back(kv.second);
            }
            if (indexManager_->hasCompositeIndex("node", keys)) {
                logical::CompositeEqualityPredicate compPred;
                compPred.keys = keys;
                compPred.values = values;
                scan->setCompositeEquality(std::move(compPred));
            }
        }

        // ----- Range scan -----
        const auto &rangePreds = scan->getRangePredicates();
        for (const auto &rp : rangePreds) {
            if (!indexManager_->hasPropertyIndex("node", rp.key)) continue;

            double rangeCost = CostModel::rangeIndexCost(stats, firstLabel, rp.key);
            if (rangeCost < bestCost) {
                bestCost = rangeCost;
                bestType = execution::ScanType::RANGE_SCAN;
            }
        }

        // ----- Composite scan -----
        const auto &compositeEq = scan->getCompositeEquality();
        if (compositeEq && compositeEq->keys.size() >= 2) {
            if (indexManager_->hasCompositeIndex("node", compositeEq->keys)) {
                double compositeCost = CostModel::compositeIndexCost(
                    stats, firstLabel, compositeEq->keys.size());
                if (compositeCost < bestCost) {
                    bestCost = compositeCost;
                    bestType = execution::ScanType::COMPOSITE_SCAN;
                }
            }
        }

        scan->setPreferredScanType(bestType);
    }
};

} // namespace graph::query::optimizer::rules

// ---------------------------------------------------------------------------
// Extension to LogicalNodeScan: preferred scan type hint
//
// We cannot modify LogicalNodeScan.hpp directly from a rule header, so we
// add the mutator/accessor here using a non-intrusive approach: declare the
// method inline in the graph::query::logical namespace as an extension to
// LogicalNodeScan that is conditionally available when this header is included.
//
// NOTE: This requires adding the declaration and storage to LogicalNodeScan.hpp.
// The preferred approach is to add these members to LogicalNodeScan directly.
// This header documents the expected interface:
//
//   // in LogicalNodeScan.hpp — add to the class body:
//   void setPreferredScanType(graph::query::execution::ScanType t) {
//       preferredScanType_ = t;
//   }
//   [[nodiscard]] graph::query::execution::ScanType getPreferredScanType() const {
//       return preferredScanType_;
//   }
//   // private member:
//   graph::query::execution::ScanType preferredScanType_ =
//       graph::query::execution::ScanType::FULL_SCAN;
// ---------------------------------------------------------------------------
