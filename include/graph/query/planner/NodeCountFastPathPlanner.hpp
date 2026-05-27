#pragma once

#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"

namespace graph::query::planner {

struct NodeCountFastPathPlan {
	execution::NodeScanConfig config;
	execution::NodeScanRequirements requirements;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	std::string outputAlias;
};

[[nodiscard]] std::optional<NodeCountFastPathPlan>
tryBuildNodeCountFastPathPlan(const logical::LogicalAggregate &aggregate);

} // namespace graph::query::planner
