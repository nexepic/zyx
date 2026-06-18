#include "graph/query/planner/NodeProjectionScanPlanner.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/planner/NodeAccessPathPlanner.hpp"

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

	const logical::LogicalNodeScan *unwrapNodeScan(const logical::LogicalOperator *op,
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
		if (op == nullptr || op->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
			return nullptr;
		}
		return static_cast<const logical::LogicalNodeScan *>(op);
	}
} // namespace

std::optional<NodeProjectionScanPlan>
tryBuildNodeProjectionScanPlan(const logical::LogicalProject &project) {
	return tryBuildNodeProjectionScanPlan(project, nullptr);
}

std::optional<NodeProjectionScanPlan>
tryBuildNodeProjectionScanPlan(const logical::LogicalProject &project,
                               const std::shared_ptr<indexes::IndexManager> &indexManager) {
	if (project.isDistinct() || project.getItems().empty()) {
		return std::nullopt;
	}

	const auto projectChildren = project.getChildren();
	if (projectChildren.size() != 1 || projectChildren[0] == nullptr) {
		return std::nullopt;
	}

	std::optional<size_t> limit;
	const auto *scan = unwrapNodeScan(projectChildren[0], limit);
	if (scan == nullptr) {
		return std::nullopt;
	}
	const auto &variable = scan->getVariable();

	NodeProjectionScanPlan plan;
	const auto accessPath = chooseNodeAccessPathDecision(*scan, indexManager);
	plan.config = accessPath.config();
	plan.accessPath = summarizeNodeAccessPath(accessPath.selected);
	plan.limit = limit;
	plan.requirements.materialization = execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	plan.requirements.needsLabels = true;
	plan.requirements.needsActiveCheck = true;

	for (const auto &item : project.getItems()) {
		const auto *projection = asNodePropertyAccess(item.expression, variable);
		if (projection == nullptr) {
			return std::nullopt;
		}
		plan.projections.push_back({projection->getPropertyName(), item.alias});
		addRequiredNodeProperty(plan.requirements, projection->getPropertyName());
	}

	if (accessPath.selectedRequiresConservativeFallback() ||
	    (!indexManager && isIndexCandidateSource(plan.config.type))) {
		fallbackToLabelOrFullScan(plan.config);
	}

	if (!appendResidualNodePredicates(*scan, plan.config, plan.requirements, plan.predicates)) {
		return std::nullopt;
	}

	return plan;
}

} // namespace graph::query::planner
