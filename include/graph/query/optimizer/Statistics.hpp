/**
 * @file Statistics.hpp
 * @brief Data structures for query optimizer statistics.
 *
 * Statistics drive cost-based optimization decisions such as index selection
 * and join reordering. They are collected lazily during save/index operations.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include "graph/core/PropertyTypes.hpp"

namespace graph::query::optimizer {

/**
 * @brief Statistics for a single property across nodes with a given label.
 */
struct PropertyStatistics {
	int64_t distinctValueCount = 0;
	PropertyValue minValue;
	PropertyValue maxValue;
	int64_t nullCount = 0;

	/** Estimated selectivity for an equality predicate: 1/NDV. */
	[[nodiscard]] double equalitySelectivity() const {
		return distinctValueCount > 0 ? 1.0 / static_cast<double>(distinctValueCount) : 1.0;
	}

	/** Estimated selectivity for a range predicate (rough: 1/3). */
	[[nodiscard]] static double rangeSelectivity() { return 0.33; }
};

/**
 * @brief Statistics for all nodes sharing a given label.
 */
struct LabelStatistics {
	std::string label;
	int64_t nodeCount = 0;
	std::unordered_map<std::string, PropertyStatistics> properties;
};

/**
 * @brief Global statistics for the entire database.
 */
struct Statistics {
	int64_t totalNodeCount = 0;
	int64_t totalEdgeCount = 0;
	std::unordered_map<std::string, LabelStatistics> labelStats;

	/** Returns estimated node count for a label, or totalNodeCount if unknown. */
	[[nodiscard]] int64_t estimateLabelCount(const std::string &label) const {
		auto it = labelStats.find(label);
		return it != labelStats.end() ? it->second.nodeCount : totalNodeCount;
	}

	/** Returns property statistics if available. */
	[[nodiscard]] const PropertyStatistics *getPropertyStats(const std::string &label,
	                                                         const std::string &property) const {
		auto labelIt = labelStats.find(label);
		if (labelIt == labelStats.end()) return nullptr;
		auto propIt = labelIt->second.properties.find(property);
		if (propIt == labelIt->second.properties.end()) return nullptr;
		return &propIt->second;
	}
};

} // namespace graph::query::optimizer
