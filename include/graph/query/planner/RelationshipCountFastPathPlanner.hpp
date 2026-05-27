#pragma once

#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/RelationshipExpandConfig.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"

namespace graph::query::planner {

struct RelationshipCountFastPathPlan {
	execution::NodeScanConfig seedConfig;
	execution::NodeScanRequirements seedRequirements;
	std::vector<execution::VectorizedPropertyPredicate> seedPredicates;
	std::vector<execution::RelationshipExpandConfig> hops;
	std::string outputAlias;
};

[[nodiscard]] std::optional<RelationshipCountFastPathPlan>
tryBuildRelationshipCountFastPathPlan(const logical::LogicalAggregate &aggregate);

} // namespace graph::query::planner
