/**
 * @file LogicalPlanOptimizer.hpp
 * @brief Storage-neutral, semantic-preserving logical-plan optimization.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "graph/query/logical/LogicalOperator.hpp"

namespace graph::query::optimization {

/**
 * @brief A bounded cardinality estimate supplied by a native statistics owner.
 *
 * The optimizer deliberately has no storage dependency. A future Catalog or
 * backend statistics implementation can provide estimates through this value
 * object without exposing pages, indexes, or physical operators here.
 */
struct LogicalCardinalityEstimate {
	uint64_t lower_bound = 0U;
	uint64_t expected_rows = 0U;
	uint64_t upper_bound = 0U;

	[[nodiscard]] bool isValid() const noexcept {
		return lower_bound <= expected_rows && expected_rows <= upper_bound;
	}
};

/** @brief Optional storage-neutral source of logical cardinality estimates. */
class LogicalStatisticsProvider {
public:
	virtual ~LogicalStatisticsProvider() = default;
	[[nodiscard]] virtual std::optional<LogicalCardinalityEstimate>
	estimate(const logical::LogicalOperator &root) const noexcept = 0;
};

struct LogicalOptimizerOptions {
	size_t maximum_rewrite_passes = 8U;
	size_t maximum_plan_depth = 1024U;
};

struct LogicalOptimizationResult {
	std::unique_ptr<logical::LogicalOperator> root;
	std::optional<LogicalCardinalityEstimate> root_cardinality;
	size_t rewrite_count = 0U;
};

/**
 * @brief Native logical optimizer foundation.
 *
 * Rules in this class must preserve observable Cypher semantics without
 * requiring a storage layout or a physical execution operator. The initial
 * rules normalize adjacent constant row windows and push an inspectable,
 * literal equality predicate into a direct Node scan. The runtime can honor
	 * that shape through its snapshot-safe exact lookup boundary. When a bounded
	 * statistics provider can compare both Cartesian inputs, the smaller input is
	 * placed on the runtime's materialized right side.
 */
class LogicalPlanOptimizer final {
public:
	explicit LogicalPlanOptimizer(LogicalOptimizerOptions options = {});

	[[nodiscard]] LogicalOptimizationResult optimize(
			std::unique_ptr<logical::LogicalOperator> root,
			const LogicalStatisticsProvider *statistics = nullptr) const;

private:
	struct RewriteState {
		size_t rewrite_count = 0U;
		bool changed = false;
		const LogicalStatisticsProvider *statistics = nullptr;
	};

	[[nodiscard]] std::unique_ptr<logical::LogicalOperator> rewrite(
			std::unique_ptr<logical::LogicalOperator> root, size_t depth, RewriteState &state) const;

	LogicalOptimizerOptions options_;
};

} // namespace graph::query::optimization
