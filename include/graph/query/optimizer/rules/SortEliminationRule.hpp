/**
 * @file SortEliminationRule.hpp
 * @brief Optimizer rule that eliminates redundant Sort operators.
 *
 * Eliminates LogicalSort when the child scan already provides data in the
 * required order (e.g., a property index scan on the sort key).
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/optimizer/OptimizerRule.hpp"

namespace graph::query::optimizer::rules {

/**
 * @class SortEliminationRule
 * @brief Removes Sort when the underlying scan already provides ordered results.
 *
 * Condition: Sort has a single ASC sort key that is a property access (var.prop),
 * and the child is a LogicalNodeScan on the same variable using a PROPERTY_SCAN
 * or RANGE_SCAN on the same property. In this case, the index already provides
 * ordered results and the Sort is redundant.
 */
class SortEliminationRule : public OptimizerRule {
public:
	[[nodiscard]] std::string getName() const override { return "SortEliminationRule"; }

	std::unique_ptr<logical::LogicalOperator> apply(
		std::unique_ptr<logical::LogicalOperator> plan,
		const Statistics& stats) override {
		return rewrite(std::move(plan), stats);
	}

private:
	std::unique_ptr<logical::LogicalOperator> rewrite(
		std::unique_ptr<logical::LogicalOperator> node,
		const Statistics& stats) {

		if (!node) return node;

		// Recurse into children first (bottom-up)
		auto childCount = node->getChildren().size();
		for (size_t i = 0; i < childCount; ++i) {
			auto child = node->detachChild(i);
			node->setChild(i, rewrite(std::move(child), stats));
		}

		// Only process Sort operators
		if (node->getType() != logical::LogicalOpType::LOP_SORT) return node;

		auto* sortOp = static_cast<logical::LogicalSort*>(node.get());
		const auto& sortItems = sortOp->getSortItems();

		// Only handle single-key ascending sorts for now
		if (sortItems.size() != 1 || !sortItems[0].ascending) return node;

		auto* sortExpr = sortItems[0].expression.get();
		if (!sortExpr) return node;

		// The sort key must be a property access (e.g., n.age)
		if (sortExpr->getExpressionType() != expressions::ExpressionType::PROPERTY_ACCESS)
			return node;

		auto* varExpr = static_cast<const expressions::VariableReferenceExpression*>(sortExpr);
		const std::string& sortVar = varExpr->getVariableName();
		const std::string& sortProp = varExpr->getPropertyName();

		// Check if the child is a NodeScan using a property index on this key
		auto children = node->getChildren();
		if (children.empty() || !children[0]) return node;

		if (children[0]->getType() != logical::LogicalOpType::LOP_NODE_SCAN) return node;

		auto* scanOp = static_cast<logical::LogicalNodeScan*>(children[0]);
		if (scanOp->getVariable() != sortVar) return node;

		auto scanType = scanOp->getPreferredScanType();
		if (scanType != execution::ScanType::PROPERTY_SCAN &&
			scanType != execution::ScanType::RANGE_SCAN)
			return node;

		// Check the scan's property predicates match the sort property
		const auto& predicates = scanOp->getPropertyPredicates();
		bool matchesSortProperty = false;
		for (const auto& [key, val] : predicates) {
			if (key == sortProp) {
				matchesSortProperty = true;
				break;
			}
		}

		// Also check range predicates
		if (!matchesSortProperty) {
			for (const auto& rp : scanOp->getRangePredicates()) {
				if (rp.key == sortProp) {
					matchesSortProperty = true;
					break;
				}
			}
		}

		if (!matchesSortProperty) return node;

		// The index on sortProp provides ordered results → eliminate Sort
		return node->detachChild(0);
	}
};

} // namespace graph::query::optimizer::rules
