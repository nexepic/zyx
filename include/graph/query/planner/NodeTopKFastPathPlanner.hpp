#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/query/execution/operators/NodeTopKFastPathOperator.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"

namespace graph::query::indexes {
class IndexManager;
}

namespace graph::query::planner {

struct NodeTopKFastPathPlan {
	execution::NodeScanConfig config;
	execution::NodeScanRequirements requirements;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	std::vector<execution::operators::NodeTopKProjection> projections;
	std::string sortProperty;
	bool ascending = true;
	int64_t limit = 0;
};

[[nodiscard]] std::optional<NodeTopKFastPathPlan>
tryBuildNodeTopKFastPathPlan(const logical::LogicalProject &project);

[[nodiscard]] std::optional<NodeTopKFastPathPlan>
tryBuildNodeTopKFastPathPlan(const logical::LogicalProject &project,
                             const std::shared_ptr<indexes::IndexManager> &indexManager);

} // namespace graph::query::planner
