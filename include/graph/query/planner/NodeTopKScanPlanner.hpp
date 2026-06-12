#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/query/execution/operators/NodeTopKScanOperator.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/planner/AccessPathSummary.hpp"

namespace graph::query::indexes {
class IndexManager;
}

namespace graph::query::planner {

struct NodeTopKScanPlan {
	execution::NodeScanConfig config;
	execution::NodeScanRequirements requirements;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	AccessPathSummary accessPath;
	std::vector<execution::operators::NodeTopKProjection> projections;
	std::string sortProperty;
	bool ascending = true;
	int64_t limit = 0;
};

[[nodiscard]] std::optional<NodeTopKScanPlan>
tryBuildNodeTopKScanPlan(const logical::LogicalProject &project);

[[nodiscard]] std::optional<NodeTopKScanPlan>
tryBuildNodeTopKScanPlan(const logical::LogicalProject &project,
                             const std::shared_ptr<indexes::IndexManager> &indexManager);

} // namespace graph::query::planner
