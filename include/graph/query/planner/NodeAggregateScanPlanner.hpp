#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/planner/AccessPathSummary.hpp"

namespace graph::query::indexes {
class IndexManager;
}

namespace graph::query::planner {

struct NodeCountScanPlan {
	execution::NodeScanConfig config;
	execution::NodeScanRequirements requirements;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	AccessPathSummary accessPath;
	std::string outputAlias;
};

struct NodeDistinctCountScanPlan {
	execution::NodeScanConfig config;
	execution::NodeScanRequirements requirements;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	AccessPathSummary accessPath;
	std::string distinctProperty;
	std::string outputAlias;
};

struct NodeGroupCountScanPlan {
	execution::NodeScanConfig config;
	execution::NodeScanRequirements requirements;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	AccessPathSummary accessPath;
	std::string groupProperty;
	std::string groupAlias;
	std::string outputAlias;
};

[[nodiscard]] std::optional<NodeCountScanPlan>
tryBuildNodeCountScanPlan(const logical::LogicalAggregate &aggregate);

[[nodiscard]] std::optional<NodeCountScanPlan>
tryBuildNodeCountScanPlan(const logical::LogicalAggregate &aggregate,
                              const std::shared_ptr<indexes::IndexManager> &indexManager);

[[nodiscard]] std::optional<NodeDistinctCountScanPlan>
tryBuildNodeDistinctCountScanPlan(const logical::LogicalAggregate &aggregate);

[[nodiscard]] std::optional<NodeDistinctCountScanPlan>
tryBuildNodeDistinctCountScanPlan(const logical::LogicalAggregate &aggregate,
                                      const std::shared_ptr<indexes::IndexManager> &indexManager);

[[nodiscard]] std::optional<NodeGroupCountScanPlan>
tryBuildNodeGroupCountScanPlan(const logical::LogicalAggregate &aggregate);

[[nodiscard]] std::optional<NodeGroupCountScanPlan>
tryBuildNodeGroupCountScanPlan(const logical::LogicalAggregate &aggregate,
                                   const std::shared_ptr<indexes::IndexManager> &indexManager);

} // namespace graph::query::planner
