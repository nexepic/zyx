/**
 * @file LimitPushdownRule.hpp
 * @brief Optimizer rule that pushes Limit operators closer to data sources.
 *
 * Pushes LogicalLimit past LogicalProject when the projection does not
 * filter rows (i.e., is not DISTINCT). This reduces the number of rows
 * that the more expensive Project operator must process.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/optimizer/OptimizerRule.hpp"

namespace graph::query::optimizer::rules {

/**
 * @class LimitPushdownRule
 * @brief Pushes Limit below Project when the Project does not filter rows.
 *
 * Transform:  Limit(N) → Project → child
 * Into:       Project → Limit(N) → child
 *
 * Condition: The Project must NOT be DISTINCT (DISTINCT can reduce row count,
 * so pushing Limit below it would change semantics).
 */
class LimitPushdownRule : public OptimizerRule {
public:
	[[nodiscard]] std::string getName() const override { return "LimitPushdownRule"; }

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

		// Only process Limit operators
		if (node->getType() != logical::LogicalOpType::LOP_LIMIT) return node;

		auto* limitOp = static_cast<logical::LogicalLimit*>(node.get());
		auto children = node->getChildren();
		if (children.empty() || !children[0]) return node;

		// Check if child is a non-DISTINCT Project
		if (children[0]->getType() != logical::LogicalOpType::LOP_PROJECT) return node;

		auto* projectOp = static_cast<logical::LogicalProject*>(children[0]);
		if (projectOp->isDistinct()) return node;

		// Swap: Limit(Project(child)) → Project(Limit(child))
		int64_t limitVal = limitOp->getLimit();

		// Detach Project from Limit
		auto projectNode = node->detachChild(0);
		// Detach child from Project
		auto grandchild = projectNode->detachChild(0);

		// Build new tree: Project( Limit( grandchild ) )
		auto newLimit = std::make_unique<logical::LogicalLimit>(
			std::move(grandchild), limitVal);
		projectNode->setChild(0, std::move(newLimit));

		return projectNode;
	}
};

} // namespace graph::query::optimizer::rules
