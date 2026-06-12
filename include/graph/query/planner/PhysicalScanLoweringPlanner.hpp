#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/planner/NodeAggregateScanPlanner.hpp"
#include "graph/query/planner/NodeTopKScanPlanner.hpp"
#include "graph/query/planner/RelationshipCountScanPlanner.hpp"

namespace graph::query::indexes {
class IndexManager;
}

namespace graph::query::planner {

enum class PhysicalScanLoweringKind {
	PSLK_NODE_TOPK_SCAN,
	PSLK_NODE_COUNT_SCAN,
	PSLK_NODE_DISTINCT_COUNT_SCAN,
	PSLK_NODE_GROUP_COUNT_SCAN,
	PSLK_RELATIONSHIP_COUNT_SCAN,
};

enum class ScanSpecializationShape {
	SSS_PROJECT,
	SSS_AGGREGATE,
};

using PhysicalScanLoweringPlan = std::variant<
		NodeTopKScanPlan,
		NodeCountScanPlan,
		NodeDistinctCountScanPlan,
		NodeGroupCountScanPlan,
		RelationshipCountScanPlan>;

struct ScanPlanCandidate {
	ScanSpecializationShape shape = ScanSpecializationShape::SSS_PROJECT;
	PhysicalScanLoweringKind kind = PhysicalScanLoweringKind::PSLK_NODE_TOPK_SCAN;
	PhysicalScanLoweringPlan plan;
	double estimatedCost = 0.0;
	std::string ruleName;
	std::string reason;
	std::vector<execution::PhysicalOperator::ExplainAttribute> explainAttributes;
};

using PhysicalScanLowering = ScanPlanCandidate;

using ProjectScanRuleBuilder = std::optional<ScanPlanCandidate> (*)(
		const logical::LogicalProject &,
		const std::shared_ptr<indexes::IndexManager> &);
using AggregateScanRuleBuilder = std::optional<ScanPlanCandidate> (*)(
		const logical::LogicalAggregate &,
		const std::shared_ptr<indexes::IndexManager> &);

struct ScanSpecializationRule {
	std::string_view name;
	ScanSpecializationShape shape = ScanSpecializationShape::SSS_PROJECT;
	ProjectScanRuleBuilder projectBuilder = nullptr;
	AggregateScanRuleBuilder aggregateBuilder = nullptr;
};

[[nodiscard]] std::vector<ScanPlanCandidate>
collectProjectScanCandidates(const logical::LogicalProject &project,
                             const std::shared_ptr<indexes::IndexManager> &indexManager);

[[nodiscard]] std::vector<ScanPlanCandidate>
collectAggregateScanCandidates(const logical::LogicalAggregate &aggregate,
                               const std::shared_ptr<indexes::IndexManager> &indexManager);

[[nodiscard]] std::optional<ScanPlanCandidate>
chooseBestScanCandidate(std::vector<ScanPlanCandidate> candidates);

[[nodiscard]] std::optional<PhysicalScanLowering>
tryLowerProjectToScan(const logical::LogicalProject &project,
                      const std::shared_ptr<indexes::IndexManager> &indexManager);

[[nodiscard]] std::optional<PhysicalScanLowering>
tryLowerAggregateToScan(const logical::LogicalAggregate &aggregate,
                        const std::shared_ptr<indexes::IndexManager> &indexManager);

} // namespace graph::query::planner
