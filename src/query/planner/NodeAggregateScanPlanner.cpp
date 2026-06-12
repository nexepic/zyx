#include "graph/query/planner/NodeAggregateScanPlanner.hpp"

#include "graph/query/planner/NodeAccessPathPlanner.hpp"

namespace graph::query::planner {

std::optional<NodeCountScanPlan>
tryBuildNodeCountScanPlan(const logical::LogicalAggregate &aggregate) {
	return tryBuildNodeCountScanPlan(aggregate, nullptr);
}

std::optional<NodeCountScanPlan>
tryBuildNodeCountScanPlan(const logical::LogicalAggregate &aggregate,
                              const std::shared_ptr<indexes::IndexManager> &indexManager) {
	if (!aggregate.getGroupByExprs().empty() || aggregate.getAggregations().size() != 1) {
		return std::nullopt;
	}

	const auto &agg = aggregate.getAggregations()[0];
	if (agg.functionName != "count" || agg.distinct) {
		return std::nullopt;
	}

	const auto children = aggregate.getChildren();
	if (children.size() != 1 || children[0] == nullptr ||
	    children[0]->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
		return std::nullopt;
	}

	const auto *scan = static_cast<const logical::LogicalNodeScan *>(children[0]);
	if (!isNodeVariableReference(agg.argument, scan->getVariable())) {
		return std::nullopt;
	}

	NodeCountScanPlan plan;
	const auto accessPath = chooseNodeAccessPathDecision(*scan, indexManager);
	plan.config = accessPath.config();
	plan.accessPath = summarizeNodeAccessPath(accessPath.selected);
	plan.outputAlias = agg.alias.empty() ? "count" : agg.alias;
	plan.requirements.materialization = execution::NodeMaterializationMode::NSM_ID_ONLY;
	plan.requirements.countOnly = true;
	plan.requirements.needsLabels = true;
	plan.requirements.needsActiveCheck = true;

	if (accessPath.selectedRequiresConservativeFallback()) {
		fallbackToLabelOrFullScan(plan.config);
	}

	if (!appendResidualNodePredicates(*scan, plan.config, plan.requirements, plan.predicates)) {
		return std::nullopt;
	}

	if (!plan.predicates.empty()) {
		plan.requirements.materialization = execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	}

	return plan;
}

std::optional<NodeDistinctCountScanPlan>
tryBuildNodeDistinctCountScanPlan(const logical::LogicalAggregate &aggregate) {
	return tryBuildNodeDistinctCountScanPlan(aggregate, nullptr);
}

std::optional<NodeDistinctCountScanPlan>
tryBuildNodeDistinctCountScanPlan(const logical::LogicalAggregate &aggregate,
                                      const std::shared_ptr<indexes::IndexManager> &indexManager) {
	if (!aggregate.getGroupByExprs().empty() || aggregate.getAggregations().size() != 1) {
		return std::nullopt;
	}

	const auto &agg = aggregate.getAggregations()[0];
	if (agg.functionName != "count" || !agg.distinct || !agg.argument) {
		return std::nullopt;
	}

	const auto children = aggregate.getChildren();
	if (children.size() != 1 || children[0] == nullptr ||
	    children[0]->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
		return std::nullopt;
	}

	const auto *scan = static_cast<const logical::LogicalNodeScan *>(children[0]);
	const auto *distinctProperty = asNodePropertyAccess(agg.argument, scan->getVariable());
	if (distinctProperty == nullptr) {
		return std::nullopt;
	}

	NodeDistinctCountScanPlan plan;
	const auto accessPath = chooseNodeAccessPathDecision(*scan, indexManager);
	plan.config = accessPath.config();
	plan.accessPath = summarizeNodeAccessPath(accessPath.selected);
	plan.distinctProperty = distinctProperty->getPropertyName();
	plan.outputAlias = agg.alias.empty() ? "count" : agg.alias;
	plan.requirements.materialization = execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	plan.requirements.countOnly = true;
	plan.requirements.needsLabels = true;
	plan.requirements.needsActiveCheck = true;
	addRequiredNodeProperty(plan.requirements, plan.distinctProperty);

	if (accessPath.selectedRequiresConservativeFallback()) {
		fallbackToLabelOrFullScan(plan.config);
	}

	if (!appendResidualNodePredicates(*scan, plan.config, plan.requirements, plan.predicates)) {
		return std::nullopt;
	}

	return plan;
}

std::optional<NodeGroupCountScanPlan>
tryBuildNodeGroupCountScanPlan(const logical::LogicalAggregate &aggregate) {
	return tryBuildNodeGroupCountScanPlan(aggregate, nullptr);
}

std::optional<NodeGroupCountScanPlan>
tryBuildNodeGroupCountScanPlan(const logical::LogicalAggregate &aggregate,
                                   const std::shared_ptr<indexes::IndexManager> &indexManager) {
	if (aggregate.getGroupByExprs().size() != 1 || aggregate.getAggregations().size() != 1) {
		return std::nullopt;
	}

	const auto &agg = aggregate.getAggregations()[0];
	if (agg.functionName != "count" || agg.distinct) {
		return std::nullopt;
	}

	const auto children = aggregate.getChildren();
	if (children.size() != 1 || children[0] == nullptr ||
	    children[0]->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
		return std::nullopt;
	}

	const auto *scan = static_cast<const logical::LogicalNodeScan *>(children[0]);
	if (!isNodeVariableReference(agg.argument, scan->getVariable())) {
		return std::nullopt;
	}

	const auto *groupProperty = asNodePropertyAccess(aggregate.getGroupByExprs()[0], scan->getVariable());
	if (groupProperty == nullptr) {
		return std::nullopt;
	}

	NodeGroupCountScanPlan plan;
	const auto accessPath = chooseNodeAccessPathDecision(*scan, indexManager);
	plan.config = accessPath.config();
	plan.accessPath = summarizeNodeAccessPath(accessPath.selected);
	plan.groupProperty = groupProperty->getPropertyName();
	const auto &aliases = aggregate.getGroupByAliases();
	plan.groupAlias = !aliases.empty() && !aliases[0].empty() ? aliases[0] : aggregate.getGroupByExprs()[0]->toString();
	plan.outputAlias = agg.alias.empty() ? "count" : agg.alias;
	plan.requirements.materialization = execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	plan.requirements.countOnly = true;
	plan.requirements.needsLabels = true;
	plan.requirements.needsActiveCheck = true;
	addRequiredNodeProperty(plan.requirements, plan.groupProperty);

	if (accessPath.selectedRequiresConservativeFallback()) {
		fallbackToLabelOrFullScan(plan.config);
	}

	if (!appendResidualNodePredicates(*scan, plan.config, plan.requirements, plan.predicates)) {
		return std::nullopt;
	}

	return plan;
}

} // namespace graph::query::planner
