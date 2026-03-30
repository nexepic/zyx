/**
 * @file OptimizerRule.hpp
 * @brief Abstract interface for query optimizer rules.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <memory>
#include <string>
#include "graph/query/logical/LogicalOperator.hpp"
#include "Statistics.hpp"

namespace graph::query::optimizer {

/**
 * @brief Abstract base class for optimization rules.
 *
 * Each rule transforms a logical plan tree, returning the (possibly modified) root.
 * Rules are applied in fixed-point iteration until the plan stabilizes.
 */
class OptimizerRule {
public:
	virtual ~OptimizerRule() = default;

	/** @brief Returns the name of this rule (for logging/debugging). */
	[[nodiscard]] virtual std::string getName() const = 0;

	/**
	 * @brief Applies this rule to the logical plan.
	 * @param plan The root of the logical plan tree.
	 * @param stats Database statistics for cost-based decisions.
	 * @return The (possibly rewritten) root of the plan tree.
	 */
	virtual std::unique_ptr<logical::LogicalOperator> apply(
		std::unique_ptr<logical::LogicalOperator> plan,
		const Statistics &stats) = 0;
};

} // namespace graph::query::optimizer
