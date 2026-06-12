#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/RelationshipExpandConfig.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/planner/AccessPathSummary.hpp"

namespace graph::query::indexes {
class IndexManager;
}

namespace graph::query::planner {

struct RelationshipCountScanPlan {
	execution::NodeScanConfig seedConfig;
	execution::NodeScanRequirements seedRequirements;
	std::vector<execution::VectorizedPropertyPredicate> seedPredicates;
	AccessPathSummary seedAccessPath;
	std::vector<execution::RelationshipExpandConfig> hops;
	execution::DirectRelationshipCountConfig directCount;
	std::optional<AccessPathSummary> relationshipAccessPath;
	std::string outputAlias;
};

[[nodiscard]] std::optional<RelationshipCountScanPlan>
tryBuildRelationshipCountScanPlan(const logical::LogicalAggregate &aggregate);

[[nodiscard]] std::optional<RelationshipCountScanPlan>
tryBuildRelationshipCountScanPlan(const logical::LogicalAggregate &aggregate,
                                      const std::shared_ptr<indexes::IndexManager> &indexManager);

} // namespace graph::query::planner
