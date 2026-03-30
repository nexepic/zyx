/**
 * @file Optimizer.hpp
 * @brief Multi-rule query optimizer engine.
 *
 * Applies optimization rules in fixed-point iteration until the plan stabilizes.
 * Also provides backward-compatible optimizeNodeScan() for the transition period.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <memory>
#include <vector>

#include "OptimizerRule.hpp"
#include "Statistics.hpp"
#include "StatisticsCollector.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "rules/IndexPushdownRule.hpp"

namespace graph::storage {
class DataManager;
}

namespace graph::query::optimizer {

class Optimizer {
public:
	static constexpr int MAX_ITERATIONS = 10;

	Optimizer(std::shared_ptr<indexes::IndexManager> indexManager,
	          std::shared_ptr<storage::DataManager> dataManager = nullptr)
		: indexManager_(std::move(indexManager)) {
		// Legacy rule (for backward-compat scanOp path)
		indexPushdownRule_ = std::make_unique<rules::IndexPushdownRule>(indexManager_);

		// Statistics collector
		if (dataManager) {
			statsCollector_ = std::make_unique<StatisticsCollector>(std::move(dataManager), indexManager_);
		}

		// Register optimization rules
		registerDefaultRules();
	}

	/**
	 * @brief Legacy: Resolves the best strategy for scanning nodes.
	 *
	 * Used by the old code path (QueryPlanner::scanOp). Will be removed
	 * once the full logical IR pipeline is active.
	 */
	[[nodiscard]] execution::NodeScanConfig optimizeNodeScan(const std::string &variable,
	                                                          const std::vector<std::string> &labels,
	                                                          const std::string &key,
	                                                          const PropertyValue &value) const {
		return indexPushdownRule_->apply(variable, labels, key, value);
	}

	/**
	 * @brief Optimizes a logical plan using all registered rules.
	 *
	 * Applies rules in fixed-point iteration until the plan stops changing
	 * or MAX_ITERATIONS is reached.
	 */
	std::unique_ptr<logical::LogicalOperator> optimize(
		std::unique_ptr<logical::LogicalOperator> plan) {
		if (!plan) return plan;

		Statistics stats;
		if (statsCollector_) {
			stats = statsCollector_->getCachedStatistics();
		}

		return optimize(std::move(plan), stats);
	}

	/**
	 * @brief Optimizes with explicit statistics (for testing).
	 */
	std::unique_ptr<logical::LogicalOperator> optimize(
		std::unique_ptr<logical::LogicalOperator> plan,
		const Statistics &stats) {
		if (!plan) return plan;

		for (int i = 0; i < MAX_ITERATIONS; ++i) {
			bool changed = false;
			for (auto &rule : rules_) {
				auto before = plan->toString();
				plan = rule->apply(std::move(plan), stats);
				if (plan->toString() != before) {
					changed = true;
				}
			}
			if (!changed) break;
		}

		return plan;
	}

	/** @brief Invalidates cached statistics. */
	void invalidateStatistics() {
		if (statsCollector_) {
			statsCollector_->invalidate();
		}
	}

	/** @brief Adds a custom rule (for testing). */
	void addRule(std::unique_ptr<OptimizerRule> rule) {
		rules_.push_back(std::move(rule));
	}

	/** @brief Returns the statistics collector (for enriching stats). */
	StatisticsCollector *getStatisticsCollector() { return statsCollector_.get(); }

private:
	std::shared_ptr<indexes::IndexManager> indexManager_;
	std::unique_ptr<rules::IndexPushdownRule> indexPushdownRule_;
	std::unique_ptr<StatisticsCollector> statsCollector_;
	std::vector<std::unique_ptr<OptimizerRule>> rules_;

	void registerDefaultRules();
};

} // namespace graph::query::optimizer
