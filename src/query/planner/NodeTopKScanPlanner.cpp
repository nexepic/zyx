#include "graph/query/planner/NodeTopKScanPlanner.hpp"

#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/planner/NodeAccessPathPlanner.hpp"

namespace graph::query::planner {

std::optional<NodeTopKScanPlan>
tryBuildNodeTopKScanPlan(const logical::LogicalProject &project) {
	return tryBuildNodeTopKScanPlan(project, nullptr);
}

std::optional<NodeTopKScanPlan>
tryBuildNodeTopKScanPlan(const logical::LogicalProject &project,
                             const std::shared_ptr<indexes::IndexManager> &indexManager) {
	if (project.isDistinct() || project.getItems().empty()) {
		return std::nullopt;
	}

	const auto projectChildren = project.getChildren();
	if (projectChildren.size() != 1 || projectChildren[0] == nullptr ||
	    projectChildren[0]->getType() != logical::LogicalOpType::LOP_LIMIT) {
		return std::nullopt;
	}
	const auto *limit = static_cast<const logical::LogicalLimit *>(projectChildren[0]);

	const auto limitChildren = limit->getChildren();
	if (limitChildren.size() != 1 || limitChildren[0] == nullptr ||
	    limitChildren[0]->getType() != logical::LogicalOpType::LOP_SORT) {
		return std::nullopt;
	}
	const auto *sort = static_cast<const logical::LogicalSort *>(limitChildren[0]);
	if (sort->getSortItems().size() != 1) {
		return std::nullopt;
	}

	const auto sortChildren = sort->getChildren();
	if (sortChildren.size() != 1 || sortChildren[0] == nullptr ||
	    sortChildren[0]->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
		return std::nullopt;
	}
	const auto *scan = static_cast<const logical::LogicalNodeScan *>(sortChildren[0]);
	const auto &variable = scan->getVariable();

	const auto *sortProperty = asNodePropertyAccess(sort->getSortItems()[0].expression, variable);
	if (sortProperty == nullptr) {
		return std::nullopt;
	}

	NodeTopKScanPlan plan;
	const auto accessPath = chooseNodeAccessPathDecision(*scan, indexManager);
	plan.config = accessPath.config();
	plan.accessPath = summarizeNodeAccessPath(accessPath.selected);
	plan.requirements.materialization = execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	plan.requirements.needsLabels = true;
	plan.requirements.needsActiveCheck = true;
	plan.sortProperty = sortProperty->getPropertyName();
	plan.ascending = sort->getSortItems()[0].ascending;
	plan.limit = limit->getLimit();
	addRequiredNodeProperty(plan.requirements, plan.sortProperty);

	for (const auto &item : project.getItems()) {
		const auto *projectionProperty = asNodePropertyAccess(item.expression, variable);
		if (projectionProperty == nullptr) {
			return std::nullopt;
		}
		plan.projections.push_back({projectionProperty->getPropertyName(), item.alias});
		addRequiredNodeProperty(plan.requirements, projectionProperty->getPropertyName());
	}

	if (accessPath.selectedRequiresConservativeFallback()) {
		fallbackToLabelOrFullScan(plan.config);
	}

	if (!appendResidualNodePredicates(*scan, plan.config, plan.requirements, plan.predicates)) {
		return std::nullopt;
	}

	return plan;
}

} // namespace graph::query::planner
