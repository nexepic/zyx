#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "graph/query/execution/operators/NodeProjectionScanOperator.hpp"
#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/planner/AccessPathSummary.hpp"

namespace graph::query::indexes {
class IndexManager;
}

namespace graph::query::planner {

struct NodeProjectionScanPlan {
	execution::NodeScanConfig config;
	execution::NodeScanRequirements requirements;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	std::vector<execution::operators::NodeProjectionScanItem> projections;
	std::optional<size_t> limit;
	AccessPathSummary accessPath;
};

[[nodiscard]] std::optional<NodeProjectionScanPlan>
tryBuildNodeProjectionScanPlan(const logical::LogicalProject &project);

[[nodiscard]] std::optional<NodeProjectionScanPlan>
tryBuildNodeProjectionScanPlan(const logical::LogicalProject &project,
                               const std::shared_ptr<indexes::IndexManager> &indexManager);

} // namespace graph::query::planner
