/**
 * @file StatisticsCollector.cpp
 * @brief Implementation of statistics collection for the query optimizer.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/query/optimizer/StatisticsCollector.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include <variant>

namespace graph::query::optimizer {

Statistics StatisticsCollector::collect() const {
	Statistics stats;

	if (!dm_) return stats;

	// Estimate total node count from segment information
	// We iterate a range to get an approximation
	auto nodes = dm_->getNodesInRange(1, INT64_MAX, 100000);
	stats.totalNodeCount = static_cast<int64_t>(nodes.size());

	auto edges = dm_->getEdgesInRange(1, INT64_MAX, 100000);
	stats.totalEdgeCount = static_cast<int64_t>(edges.size());

	return stats;
}

LabelStatistics StatisticsCollector::collectLabelStats(const std::string &label) const {
	LabelStatistics labelStats;
	labelStats.label = label;

	if (!im_) return labelStats;

	// Use label index to count nodes with this label
	auto nodeIds = im_->findNodeIdsByLabel(label);
	labelStats.nodeCount = static_cast<int64_t>(nodeIds.size());

	if (!dm_ || nodeIds.empty()) return labelStats;

	// Sample nodes for property statistics
	std::vector<int64_t> sampleIds;
	if (static_cast<int64_t>(nodeIds.size()) <= MAX_SAMPLE_SIZE) {
		sampleIds = nodeIds;
	} else {
		// Reservoir sampling with fixed seed for reproducibility
		sampleIds.reserve(static_cast<size_t>(MAX_SAMPLE_SIZE));
		std::mt19937 rng(42);
		for (size_t i = 0; i < nodeIds.size(); ++i) {
			if (static_cast<int64_t>(i) < MAX_SAMPLE_SIZE) {
				sampleIds.push_back(nodeIds[i]);
			} else {
				std::uniform_int_distribution<size_t> dist(0, i);
				size_t j = dist(rng);
				if (static_cast<int64_t>(j) < MAX_SAMPLE_SIZE) {
					sampleIds[j] = nodeIds[i];
				}
			}
		}
	}

	// Collect property statistics from sampled nodes
	std::unordered_map<std::string, std::unordered_set<std::string>> distinctValues;
	std::unordered_map<std::string, int64_t> nullCounts;

	for (auto nodeId : sampleIds) {
		auto props = dm_->getNodeProperties(nodeId);
		for (const auto &[key, value] : props) {
			if (std::holds_alternative<std::monostate>(value.getVariant())) {
				nullCounts[key]++;
			} else {
				distinctValues[key].insert(value.toString());
			}
		}
	}

	// Compute property statistics, scaling estimates for sampled data
	double scaleFactor = labelStats.nodeCount > 0 && !sampleIds.empty()
		? static_cast<double>(labelStats.nodeCount) / static_cast<double>(sampleIds.size())
		: 1.0;

	for (auto &[key, values] : distinctValues) {
		PropertyStatistics propStat;
		propStat.distinctValueCount = static_cast<int64_t>(
			static_cast<double>(values.size()) * std::min(scaleFactor, 2.0));
		if (propStat.distinctValueCount < static_cast<int64_t>(values.size())) {
			propStat.distinctValueCount = static_cast<int64_t>(values.size());
		}
		propStat.nullCount = static_cast<int64_t>(
			static_cast<double>(nullCounts[key]) * scaleFactor);
		labelStats.properties[key] = propStat;
	}

	return labelStats;
}

} // namespace graph::query::optimizer
