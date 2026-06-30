#include "graph/query/planner/PhysicalScanLoweringPlanner.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace graph::query::planner {
namespace {

	std::string formatCost(double cost) {
		if (!std::isfinite(cost)) {
			return "inf";
		}
		return formatAccessPathCost(cost);
	}

	template<typename Plan>
	double accessPathCost(const Plan &plan) {
		return plan.accessPath.estimatedCost;
	}

	double accessPathCost(const RelationshipProjectionScanPlan &plan) {
		return plan.relationshipAccessPath.estimatedCost;
	}

	double accessPathCost(const RelationshipCountScanPlan &plan) {
		double cost = plan.seedAccessPath.estimatedCost;
		if (plan.relationshipAccessPath.has_value()) {
			cost += plan.relationshipAccessPath->estimatedCost;
		}
		return cost;
	}

	template<typename Plan>
	ScanPlanCandidate makeCandidate(ScanSpecializationShape shape,
	                                PhysicalScanLoweringKind kind,
	                                Plan &&plan,
	                                double cost,
	                                std::string ruleName,
	                                std::string reason) {
		ScanPlanCandidate candidate;
		candidate.shape = shape;
		candidate.kind = kind;
		candidate.estimatedCost = cost;
		candidate.ruleName = std::move(ruleName);
		candidate.reason = std::move(reason);
		candidate.explainAttributes.emplace_back("scan_specialization.rule", candidate.ruleName);
		candidate.explainAttributes.emplace_back("scan_specialization.reason", candidate.reason);
		candidate.explainAttributes.emplace_back("scan_specialization.estimated_cost", formatCost(candidate.estimatedCost));
		candidate.plan = std::forward<Plan>(plan);
		return candidate;
	}

	std::optional<ScanPlanCandidate>
	buildNodeProjectionRule(const logical::LogicalProject &project,
	                        const std::shared_ptr<indexes::IndexManager> &indexManager) {
		auto plan = tryBuildNodeProjectionScanPlan(project, indexManager);
		if (!plan.has_value()) {
			return std::nullopt;
		}
		const double cost = accessPathCost(*plan) + 0.5;
		return makeCandidate(
				ScanSpecializationShape::SSS_PROJECT,
				PhysicalScanLoweringKind::PSLK_NODE_PROJECTION_SCAN,
				std::move(*plan),
				cost,
				"node_projection_scan",
				"project_limit_node_scan");
	}

	std::optional<ScanPlanCandidate>
	buildNodeTopKRule(const logical::LogicalProject &project,
	                  const std::shared_ptr<indexes::IndexManager> &indexManager) {
		auto plan = tryBuildNodeTopKScanPlan(project, indexManager);
		if (!plan.has_value()) {
			return std::nullopt;
		}
		const double cost = accessPathCost(*plan);
		return makeCandidate(
				ScanSpecializationShape::SSS_PROJECT,
				PhysicalScanLoweringKind::PSLK_NODE_TOPK_SCAN,
				std::move(*plan),
				cost,
				"node_topk_scan",
				"project_sort_limit_node_scan");
	}

	std::optional<ScanPlanCandidate>
	buildRelationshipProjectionRule(const logical::LogicalProject &project,
	                                const std::shared_ptr<indexes::IndexManager> &indexManager) {
		auto plan = tryBuildRelationshipProjectionScanPlan(project, indexManager);
		if (!plan.has_value()) {
			return std::nullopt;
		}
		const double cost = accessPathCost(*plan) + 0.5;
		return makeCandidate(
				ScanSpecializationShape::SSS_PROJECT,
				PhysicalScanLoweringKind::PSLK_RELATIONSHIP_PROJECTION_SCAN,
				std::move(*plan),
				cost,
				"relationship_projection_scan",
				"project_limit_unanchored_relationship_scan");
	}

	std::optional<ScanPlanCandidate>
	buildNodeCountRule(const logical::LogicalAggregate &aggregate,
	                   const std::shared_ptr<indexes::IndexManager> &indexManager) {
		auto plan = tryBuildNodeCountScanPlan(aggregate, indexManager);
		if (!plan.has_value()) {
			return std::nullopt;
		}
		const double cost = accessPathCost(*plan);
		return makeCandidate(
				ScanSpecializationShape::SSS_AGGREGATE,
				PhysicalScanLoweringKind::PSLK_NODE_COUNT_SCAN,
				std::move(*plan),
				cost,
				"node_count_scan",
				"aggregate_count_node_scan");
	}

	std::optional<ScanPlanCandidate>
	buildNodeDistinctCountRule(const logical::LogicalAggregate &aggregate,
	                           const std::shared_ptr<indexes::IndexManager> &indexManager) {
		auto plan = tryBuildNodeDistinctCountScanPlan(aggregate, indexManager);
		if (!plan.has_value()) {
			return std::nullopt;
		}
		const double cost = accessPathCost(*plan) + 1.0;
		return makeCandidate(
				ScanSpecializationShape::SSS_AGGREGATE,
				PhysicalScanLoweringKind::PSLK_NODE_DISTINCT_COUNT_SCAN,
				std::move(*plan),
				cost,
				"node_distinct_count_scan",
				"aggregate_distinct_count_node_scan");
	}

	std::optional<ScanPlanCandidate>
	buildNodeGroupCountRule(const logical::LogicalAggregate &aggregate,
	                        const std::shared_ptr<indexes::IndexManager> &indexManager) {
		auto plan = tryBuildNodeGroupCountScanPlan(aggregate, indexManager);
		if (!plan.has_value()) {
			return std::nullopt;
		}
		const double cost = accessPathCost(*plan) + 2.0;
		return makeCandidate(
				ScanSpecializationShape::SSS_AGGREGATE,
				PhysicalScanLoweringKind::PSLK_NODE_GROUP_COUNT_SCAN,
				std::move(*plan),
				cost,
				"node_group_count_scan",
				"aggregate_group_count_node_scan");
	}

	std::optional<ScanPlanCandidate>
	buildRelationshipCountRule(const logical::LogicalAggregate &aggregate,
	                           const std::shared_ptr<indexes::IndexManager> &indexManager) {
		auto plan = tryBuildRelationshipCountScanPlan(aggregate, indexManager);
		if (!plan.has_value()) {
			return std::nullopt;
		}
		const double cost = accessPathCost(*plan);
		return makeCandidate(
				ScanSpecializationShape::SSS_AGGREGATE,
				PhysicalScanLoweringKind::PSLK_RELATIONSHIP_COUNT_SCAN,
				std::move(*plan),
				cost,
				"relationship_count_scan",
				"aggregate_count_relationship_scan_or_expand");
	}

	const std::array<ProjectScanSpecializationRule, 3> &projectScanSpecializationRules() {
		static const std::array<ProjectScanSpecializationRule, 3> rules = {{
				{"node_projection_scan", buildNodeProjectionRule},
				{"node_topk_scan", buildNodeTopKRule},
				{"relationship_projection_scan", buildRelationshipProjectionRule},
		}};
		return rules;
	}

	const std::array<AggregateScanSpecializationRule, 4> &aggregateScanSpecializationRules() {
		static const std::array<AggregateScanSpecializationRule, 4> rules = {{
				{"node_count_scan", buildNodeCountRule},
				{"node_distinct_count_scan", buildNodeDistinctCountRule},
				{"node_group_count_scan", buildNodeGroupCountRule},
				{"relationship_count_scan", buildRelationshipCountRule},
		}};
		return rules;
	}

} // namespace

std::vector<ScanPlanCandidate>
collectProjectScanCandidates(const logical::LogicalProject &project,
                             const std::shared_ptr<indexes::IndexManager> &indexManager) {
	std::vector<ScanPlanCandidate> candidates;
	for (const auto &rule : projectScanSpecializationRules()) {
		if (auto candidate = rule.builder(project, indexManager)) {
			candidate->shape = ScanSpecializationShape::SSS_PROJECT;
			candidate->ruleName = std::string(rule.name);
			candidate->explainAttributes[0].second = candidate->ruleName;
			candidates.push_back(std::move(*candidate));
		}
	}
	return candidates;
}

std::vector<ScanPlanCandidate>
collectAggregateScanCandidates(const logical::LogicalAggregate &aggregate,
                               const std::shared_ptr<indexes::IndexManager> &indexManager) {
	std::vector<ScanPlanCandidate> candidates;
	for (const auto &rule : aggregateScanSpecializationRules()) {
		if (auto candidate = rule.builder(aggregate, indexManager)) {
			candidate->shape = ScanSpecializationShape::SSS_AGGREGATE;
			candidate->ruleName = std::string(rule.name);
			candidate->explainAttributes[0].second = candidate->ruleName;
			candidates.push_back(std::move(*candidate));
		}
	}
	return candidates;
}

std::optional<ScanPlanCandidate> chooseBestScanCandidate(std::vector<ScanPlanCandidate> candidates) {
	if (candidates.empty()) {
		return std::nullopt;
	}
	auto best = std::min_element(
			candidates.begin(),
			candidates.end(),
			[](const ScanPlanCandidate &left, const ScanPlanCandidate &right) {
				if (left.estimatedCost != right.estimatedCost) {
					return left.estimatedCost < right.estimatedCost;
				}
				return left.ruleName < right.ruleName;
			});
	return std::move(*best);
}

std::optional<PhysicalScanLowering>
tryLowerProjectToScan(const logical::LogicalProject &project,
                      const std::shared_ptr<indexes::IndexManager> &indexManager) {
	return chooseBestScanCandidate(collectProjectScanCandidates(project, indexManager));
}

std::optional<PhysicalScanLowering>
tryLowerAggregateToScan(const logical::LogicalAggregate &aggregate,
                        const std::shared_ptr<indexes::IndexManager> &indexManager) {
	return chooseBestScanCandidate(collectAggregateScanCandidates(aggregate, indexManager));
}

} // namespace graph::query::planner
