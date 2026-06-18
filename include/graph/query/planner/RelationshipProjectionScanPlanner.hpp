#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/RelationshipExpandConfig.hpp"
#include "graph/query/execution/operators/RelationshipProjectionScanOperator.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/planner/AccessPathSummary.hpp"

namespace graph::query::indexes {
class IndexManager;
}

namespace graph::query::planner {

struct RelationshipProjectionScanPlan {
	execution::DirectRelationshipCountConfig config;
	std::string targetVariable;
	std::vector<std::string> targetLabels;
	std::vector<execution::operators::RelationshipProjectionScanItem> projections;
	std::optional<size_t> limit;
	AccessPathSummary relationshipAccessPath;
};

[[nodiscard]] std::optional<RelationshipProjectionScanPlan>
tryBuildRelationshipProjectionScanPlan(const logical::LogicalProject &project);

[[nodiscard]] std::optional<RelationshipProjectionScanPlan>
tryBuildRelationshipProjectionScanPlan(const logical::LogicalProject &project,
                                       const std::shared_ptr<indexes::IndexManager> &indexManager);

} // namespace graph::query::planner
