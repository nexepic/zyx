#include "graph/query/planner/RelationshipProjectionScanPlanner.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/planner/RelationshipAccessPathPlanner.hpp"
#include "graph/query/planner/RelationshipPropertyPredicatePlanner.hpp"
#include "graph/query/QueryPlan.hpp"

namespace graph::query::planner {
namespace {
	std::optional<size_t> normalizeLimit(int64_t limit) {
		if (limit <= 0) {
			return size_t{0};
		}
		const auto unsignedLimit = static_cast<uint64_t>(limit);
		const auto maxSize = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
		return static_cast<size_t>(std::min(unsignedLimit, maxSize));
	}

	bool isPlainUnanchoredSeed(const logical::LogicalNodeScan &scan) {
		return (scan.getVariable().empty() || isAnonymousVariable(scan.getVariable())) &&
		       scan.getLabels().empty() &&
		       scan.getPropertyPredicates().empty() &&
		       scan.getRangePredicates().empty() &&
		       !scan.getCompositeEquality().has_value();
	}

	const expressions::VariableReferenceExpression *asPropertyAccess(
			const std::shared_ptr<expressions::Expression> &expression) {
		const auto *property = dynamic_cast<const expressions::VariableReferenceExpression *>(expression.get());
		if (property == nullptr || !property->hasProperty()) {
			return nullptr;
		}
		return property;
	}

	const logical::LogicalTraversal *unwrapTraversal(const logical::LogicalOperator *op,
	                                               const logical::LogicalFilter *&edgeFilter,
	                                               std::optional<size_t> &limit) {
		if (op == nullptr) {
			return nullptr;
		}
		if (op->getType() == logical::LogicalOpType::LOP_LIMIT) {
			const auto *limitOp = static_cast<const logical::LogicalLimit *>(op);
			limit = normalizeLimit(limitOp->getLimit());
			const auto children = limitOp->getChildren();
			if (children.size() != 1) {
				return nullptr;
			}
			op = children[0];
		}
		if (op != nullptr && op->getType() == logical::LogicalOpType::LOP_FILTER) {
			edgeFilter = static_cast<const logical::LogicalFilter *>(op);
			const auto children = edgeFilter->getChildren();
			if (children.size() != 1) {
				return nullptr;
			}
			op = children[0];
		}
		if (op == nullptr || op->getType() != logical::LogicalOpType::LOP_TRAVERSAL) {
			return nullptr;
		}
		return static_cast<const logical::LogicalTraversal *>(op);
	}
} // namespace

std::optional<RelationshipProjectionScanPlan>
tryBuildRelationshipProjectionScanPlan(const logical::LogicalProject &project) {
	return tryBuildRelationshipProjectionScanPlan(project, nullptr);
}

std::optional<RelationshipProjectionScanPlan>
tryBuildRelationshipProjectionScanPlan(const logical::LogicalProject &project,
                                       const std::shared_ptr<indexes::IndexManager> &indexManager) {
	if (project.isDistinct() || project.getItems().empty()) {
		return std::nullopt;
	}

	const auto projectChildren = project.getChildren();
	if (projectChildren.size() != 1 || projectChildren[0] == nullptr) {
		return std::nullopt;
	}

	const logical::LogicalFilter *edgeFilter = nullptr;
	std::optional<size_t> limit;
	const auto *traversal = unwrapTraversal(projectChildren[0], edgeFilter, limit);
	if (traversal == nullptr || traversal->getDirection() != "out" || !traversal->getTargetProperties().empty()) {
		return std::nullopt;
	}

	const auto traversalChildren = traversal->getChildren();
	if (traversalChildren.size() != 1 || traversalChildren[0] == nullptr ||
	    traversalChildren[0]->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
		return std::nullopt;
	}
	const auto *seedScan = static_cast<const logical::LogicalNodeScan *>(traversalChildren[0]);
	if (!isPlainUnanchoredSeed(*seedScan)) {
		return std::nullopt;
	}

	RelationshipProjectionScanPlan plan;
	plan.targetVariable = traversal->getTargetVar();
	plan.targetLabels = traversal->getTargetLabels();
	plan.limit = limit;

	for (const auto &item : project.getItems()) {
		const auto *property = asPropertyAccess(item.expression);
		if (property == nullptr) {
			return std::nullopt;
		}
		if (property->getVariableName() == traversal->getEdgeVar()) {
			plan.projections.push_back({execution::operators::RelationshipProjectionSource::RPS_EDGE,
			                            property->getPropertyName(), item.alias});
		} else if (property->getVariableName() == traversal->getTargetVar()) {
			plan.projections.push_back({execution::operators::RelationshipProjectionSource::RPS_TARGET_NODE,
			                            property->getPropertyName(), item.alias});
		} else {
			return std::nullopt;
		}
	}

	auto predicatePlan = buildRelationshipPropertyPredicatePlan(
			traversal->getEdgeVar(),
			traversal->getEdgeProperties(),
			edgeFilter == nullptr ? nullptr : edgeFilter->getPredicate());
	if (!predicatePlan.has_value()) {
		return std::nullopt;
	}

	plan.config.enabled = true;
	plan.config.edgeType = traversal->getEdgeType();
	plan.config.direction = traversal->getDirection();
	plan.config.edgeProperties = std::move(predicatePlan->equalityProperties);
	plan.config.edgePredicates = std::move(predicatePlan->predicates);
	const auto accessPath = chooseRelationshipAccessPathDecision(plan.config, indexManager);
	plan.config.candidateSource = relationshipCandidateSourceForAccessPath(accessPath.selected);
	plan.relationshipAccessPath = summarizeRelationshipAccessPath(accessPath.selected);
	return plan;
}

} // namespace graph::query::planner
