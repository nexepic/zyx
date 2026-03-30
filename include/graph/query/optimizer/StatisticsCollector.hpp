/**
 * @file StatisticsCollector.hpp
 * @brief Collects database statistics for the query optimizer.
 *
 * Statistics are collected lazily during save/index operations,
 * not on every query. Stale stats produce suboptimal but correct plans.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Statistics.hpp"

namespace graph::storage {
class DataManager;
}

namespace graph::query::indexes {
class IndexManager;
}

namespace graph::query::optimizer {

/**
 * @brief Collects statistics from storage for use by the optimizer.
 *
 * Uses reservoir sampling for large datasets to avoid full scans.
 */
class StatisticsCollector {
public:
	static constexpr int64_t MAX_SAMPLE_SIZE = 10000;

	StatisticsCollector(std::shared_ptr<storage::DataManager> dm,
	                    std::shared_ptr<indexes::IndexManager> im)
		: dm_(std::move(dm)), im_(std::move(im)) {}

	/**
	 * @brief Collects fresh statistics from the database.
	 *
	 * Scans label indexes to count nodes per label, samples property values
	 * to estimate NDV, min/max, and null counts.
	 */
	Statistics collect() const;

	/**
	 * @brief Collects statistics for a specific label.
	 */
	LabelStatistics collectLabelStats(const std::string &label) const;

	/**
	 * @brief Ensures statistics for the given labels are cached.
	 */
	void ensureLabelStats(Statistics &stats, const std::vector<std::string> &labels) {
		for (const auto &label : labels) {
			if (stats.labelStats.find(label) == stats.labelStats.end()) {
				stats.labelStats[label] = collectLabelStats(label);
			}
		}
	}

	/**
	 * @brief Returns cached statistics, collecting if stale or empty.
	 */
	const Statistics &getCachedStatistics() {
		if (!cached_.has_value()) {
			cached_ = collect();
		}
		return cached_.value();
	}

	/** @brief Invalidates cached statistics (call after data modifications). */
	void invalidate() { cached_.reset(); }

private:
	std::shared_ptr<storage::DataManager> dm_;
	std::shared_ptr<indexes::IndexManager> im_;
	mutable std::optional<Statistics> cached_;
};

} // namespace graph::query::optimizer
