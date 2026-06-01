#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"

namespace graph::query::indexes {
class IndexManager;
}

namespace graph::query::planner {

struct NodeCountFastPathPlan {
	execution::NodeScanConfig config;
	execution::NodeScanRequirements requirements;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	std::string outputAlias;
};

struct NodeDistinctCountFastPathPlan {
	execution::NodeScanConfig config;
	execution::NodeScanRequirements requirements;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	std::string distinctProperty;
	std::string outputAlias;
};

struct NodeGroupCountFastPathPlan {
	execution::NodeScanConfig config;
	execution::NodeScanRequirements requirements;
	std::vector<execution::VectorizedPropertyPredicate> predicates;
	std::string groupProperty;
	std::string groupAlias;
	std::string outputAlias;
};

[[nodiscard]] std::optional<NodeCountFastPathPlan>
tryBuildNodeCountFastPathPlan(const logical::LogicalAggregate &aggregate);

[[nodiscard]] std::optional<NodeCountFastPathPlan>
tryBuildNodeCountFastPathPlan(const logical::LogicalAggregate &aggregate,
                              const std::shared_ptr<indexes::IndexManager> &indexManager);

[[nodiscard]] std::optional<NodeDistinctCountFastPathPlan>
tryBuildNodeDistinctCountFastPathPlan(const logical::LogicalAggregate &aggregate);

[[nodiscard]] std::optional<NodeDistinctCountFastPathPlan>
tryBuildNodeDistinctCountFastPathPlan(const logical::LogicalAggregate &aggregate,
                                      const std::shared_ptr<indexes::IndexManager> &indexManager);

[[nodiscard]] std::optional<NodeGroupCountFastPathPlan>
tryBuildNodeGroupCountFastPathPlan(const logical::LogicalAggregate &aggregate);

[[nodiscard]] std::optional<NodeGroupCountFastPathPlan>
tryBuildNodeGroupCountFastPathPlan(const logical::LogicalAggregate &aggregate,
                                   const std::shared_ptr<indexes::IndexManager> &indexManager);

} // namespace graph::query::planner
