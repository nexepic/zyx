/**
 * @file CostModel.hpp
 * @brief Cost estimation model for the query optimizer.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include "Statistics.hpp"

namespace graph::query::optimizer {

/**
 * @brief Provides cost estimates for different scan and join strategies.
 *
 * Cost units are abstract — only relative comparisons matter.
 */
class CostModel {
public:
	static constexpr double SCAN_COST_PER_ROW = 1.0;
	static constexpr double INDEX_LOOKUP_COST = 0.2;
	static constexpr double FILTER_COST_PER_ROW = 0.5;
	static constexpr double JOIN_COST_PER_ROW = 2.0;

	/** Cost of a full table scan. */
	[[nodiscard]] static double fullScanCost(const Statistics &stats) {
		return static_cast<double>(stats.totalNodeCount) * SCAN_COST_PER_ROW;
	}

	/** Cost of a label-index scan. */
	[[nodiscard]] static double labelScanCost(const Statistics &stats, const std::string &label) {
		return static_cast<double>(stats.estimateLabelCount(label)) * SCAN_COST_PER_ROW;
	}

	/** Cost of a property-index lookup. */
	[[nodiscard]] static double propertyIndexCost(const Statistics &stats, const std::string &label,
	                                               const std::string &property, bool isEquality) {
		auto *propStats = stats.getPropertyStats(label, property);
		if (!propStats) {
			// No stats — assume moderate selectivity
			return static_cast<double>(stats.estimateLabelCount(label)) * 0.1 * INDEX_LOOKUP_COST;
		}
		double selectivity = isEquality ? propStats->equalitySelectivity()
		                                : PropertyStatistics::rangeSelectivity();
		double estimatedRows = static_cast<double>(stats.estimateLabelCount(label)) * selectivity;
		return estimatedRows * INDEX_LOOKUP_COST;
	}

	/** Estimated cardinality of a cross join. */
	[[nodiscard]] static double crossJoinCardinality(double leftCard, double rightCard) {
		return leftCard * rightCard;
	}

	/** Estimated cost of a cross join. */
	[[nodiscard]] static double crossJoinCost(double leftCard, double rightCard) {
		return crossJoinCardinality(leftCard, rightCard) * JOIN_COST_PER_ROW;
	}

	/**
	 * @brief Estimates the output cardinality of a scan on a given label.
	 */
	[[nodiscard]] static double estimateScanCardinality(const Statistics &stats,
	                                                     const std::vector<std::string> &labels) {
		if (labels.empty()) return static_cast<double>(stats.totalNodeCount);
		// Use the most selective label
		double minCount = static_cast<double>(stats.totalNodeCount);
		for (const auto &label : labels) {
			double c = static_cast<double>(stats.estimateLabelCount(label));
			if (c < minCount) minCount = c;
		}
		return minCount;
	}
};

} // namespace graph::query::optimizer
