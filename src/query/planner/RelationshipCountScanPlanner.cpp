#include "graph/query/planner/RelationshipCountScanPlanner.hpp"

#include <algorithm>
#include <utility>

#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/planner/NodeAccessPathPlanner.hpp"
#include "graph/query/planner/RelationshipAccessPathPlanner.hpp"
#include "graph/query/planner/RelationshipPropertyPredicatePlanner.hpp"

namespace graph::query::planner {
namespace {

bool isCountVariableAllowed(const std::shared_ptr<expressions::Expression> &argument,
                            const std::vector<execution::RelationshipExpandConfig> &hops,
                            const std::string &seedVariable) {
	if (!argument) {
		return true;
	}
	const auto *var = dynamic_cast<const expressions::VariableReferenceExpression *>(argument.get());
	if (var == nullptr || var->hasProperty()) {
		return false;
	}
	const auto &name = var->getVariableName();
	if (name == seedVariable) {
		return true;
	}
	return std::any_of(hops.begin(), hops.end(), [&](const auto &hop) {
		return name == hop.edgeVar || name == hop.sourceVar || name == hop.targetVar; // ZYX_COV_EXCL_LINE
	});
}

bool hasUnsupportedTraversalTargetFilters(const logical::LogicalTraversal &traversal) {
	return !traversal.getTargetProperties().empty();
}

execution::RelationshipExpandConfig makeHopConfig(const logical::LogicalTraversal &traversal) {
	execution::RelationshipExpandConfig config;
	config.sourceVar = traversal.getSourceVar();
	config.edgeVar = traversal.getEdgeVar();
	config.targetVar = traversal.getTargetVar();
	config.edgeType = traversal.getEdgeType();
	config.direction = traversal.getDirection();
	config.targetLabels = traversal.getTargetLabels();
	return config;
}

bool hasSelectiveSeedPredicate(const logical::LogicalNodeScan &scan) {
	return !scan.getPropertyPredicates().empty() ||
	       !scan.getRangePredicates().empty() ||
	       scan.getCompositeEquality().has_value();
}

bool isPlainUnanchoredSeed(const logical::LogicalNodeScan &scan) {
	return scan.getLabels().empty() &&
	       scan.getPropertyPredicates().empty() && // ZYX_COV_EXCL_LINE
	       scan.getRangePredicates().empty() &&
	       !scan.getCompositeEquality().has_value(); // ZYX_COV_EXCL_LINE
}

bool isDirectEdgeCountArgument(const std::shared_ptr<expressions::Expression> &argument,
                               const logical::LogicalTraversal &traversal) {
	if (!argument) {
		return true;
	}
	const auto *var = dynamic_cast<const expressions::VariableReferenceExpression *>(argument.get());
	return var != nullptr && !var->hasProperty() && var->getVariableName() == traversal.getEdgeVar();
}

bool canUseDirectRelationshipCount(const logical::LogicalNodeScan &seedScan,
                                   const std::vector<const logical::LogicalTraversal *> &traversalChain,
                                   const std::shared_ptr<expressions::Expression> &argument) {
	if (traversalChain.size() != 1 || !isPlainUnanchoredSeed(seedScan)) {
		return false;
	}

	const auto &traversal = *traversalChain.front();
	return traversal.getDirection() == "out" &&
	       traversal.getTargetLabels().empty() &&
	       traversal.getTargetProperties().empty() && // ZYX_COV_EXCL_LINE
	       isDirectEdgeCountArgument(argument, traversal);
}

} // namespace

std::optional<RelationshipCountScanPlan>
tryBuildRelationshipCountScanPlan(const logical::LogicalAggregate &aggregate) {
	return tryBuildRelationshipCountScanPlan(aggregate, nullptr);
}

std::optional<RelationshipCountScanPlan>
tryBuildRelationshipCountScanPlan(const logical::LogicalAggregate &aggregate,
                                      const std::shared_ptr<indexes::IndexManager> &indexManager) {
	if (!aggregate.getGroupByExprs().empty() || aggregate.getAggregations().size() != 1) {
		return std::nullopt;
	}

	const auto &agg = aggregate.getAggregations()[0];
	if (agg.functionName != "count" || agg.distinct) {
		return std::nullopt;
	}

	const auto children = aggregate.getChildren();
	if (children.size() != 1 || children[0] == nullptr) { // ZYX_COV_EXCL_LINE
		return std::nullopt;
	}

	std::vector<const logical::LogicalTraversal *> traversalChain;
	const logical::LogicalFilter *edgeFilter = nullptr;
	const logical::LogicalOperator *cursor = children[0];
	if (cursor->getType() == logical::LogicalOpType::LOP_FILTER) {
		edgeFilter = static_cast<const logical::LogicalFilter *>(cursor);
		const auto filterChildren = edgeFilter->getChildren();
		if (filterChildren.size() != 1 || filterChildren[0] == nullptr) { // ZYX_COV_EXCL_LINE
			return std::nullopt;
		}
		cursor = filterChildren[0];
	}
	while (cursor != nullptr && cursor->getType() == logical::LogicalOpType::LOP_TRAVERSAL) {
		const auto *traversal = static_cast<const logical::LogicalTraversal *>(cursor);
		if (hasUnsupportedTraversalTargetFilters(*traversal)) {
			return std::nullopt;
		}
		traversalChain.push_back(traversal);
		const auto traversalChildren = traversal->getChildren();
		if (traversalChildren.size() != 1) { // ZYX_COV_EXCL_LINE
			return std::nullopt;
		}
		cursor = traversalChildren[0];
	}

	if (traversalChain.empty() || traversalChain.size() > 2 || cursor == nullptr || cursor->getType() != logical::LogicalOpType::LOP_NODE_SCAN) { // ZYX_COV_EXCL_LINE
		return std::nullopt;
	}

	const auto *seedScan = static_cast<const logical::LogicalNodeScan *>(cursor);
	RelationshipCountScanPlan plan;

	const bool directRelationshipCount = canUseDirectRelationshipCount(*seedScan, traversalChain, agg.argument);
	NodeAccessPathOptions seedAccessOptions;
	seedAccessOptions.allowOpenRangeIndex = true;
	const auto seedAccessPath = chooseNodeAccessPathDecision(*seedScan, indexManager, seedAccessOptions);
	plan.seedConfig = seedAccessPath.config();
	plan.seedAccessPath = summarizeNodeAccessPath(seedAccessPath.selected);
	plan.seedRequirements.materialization = execution::NodeMaterializationMode::NSM_ID_ONLY;
	plan.seedRequirements.countOnly = true;
	plan.seedRequirements.needsLabels = true;
	plan.seedRequirements.needsActiveCheck = true;
	plan.outputAlias = agg.alias.empty() ? "count" : agg.alias;

	if (directRelationshipCount) {
		const auto &traversal = *traversalChain.front();
		const auto predicatePlan = buildRelationshipPropertyPredicatePlan(
				traversal.getEdgeVar(),
				traversal.getEdgeProperties(),
				edgeFilter == nullptr ? nullptr : edgeFilter->getPredicate());
		if (!predicatePlan.has_value()) {
			return std::nullopt;
		}
		plan.directCount.enabled = true;
		plan.directCount.edgeType = traversal.getEdgeType();
		plan.directCount.direction = traversal.getDirection();
		plan.directCount.edgeProperties = std::move(predicatePlan->equalityProperties);
		plan.directCount.edgePredicates = std::move(predicatePlan->predicates);
		const auto relationshipAccessPath = chooseRelationshipAccessPathDecision(plan.directCount, indexManager);
		plan.directCount.candidateSource = relationshipCandidateSourceForAccessPath(relationshipAccessPath.selected);
		plan.relationshipAccessPath = summarizeRelationshipAccessPath(relationshipAccessPath.selected);
		plan.hops.push_back(makeHopConfig(traversal));
		return plan;
	}

	if (edgeFilter != nullptr ||
	    std::any_of(traversalChain.begin(), traversalChain.end(), [](const auto *traversal) {
		    return traversal != nullptr && !traversal->getEdgeProperties().empty(); // ZYX_COV_EXCL_LINE
	    })) {
		return std::nullopt;
	}

	if (!appendResidualNodePredicates(*seedScan, plan.seedConfig, plan.seedRequirements, plan.seedPredicates)) {
		return std::nullopt;
	}

	if (hasSelectiveSeedPredicate(*seedScan)) {
		if (!seedAccessPath.selectedSupportsDirectCandidateLookup()) { // ZYX_COV_EXCL_LINE
			return std::nullopt;
		}
	}

	if (!plan.seedPredicates.empty()) {
		plan.seedRequirements.materialization = execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	}

	for (auto it = traversalChain.rbegin(); it != traversalChain.rend(); ++it) {
		plan.hops.push_back(makeHopConfig(**it));
	}

	if (!isCountVariableAllowed(agg.argument, plan.hops, seedScan->getVariable())) {
		return std::nullopt;
	}

	return plan;
}

} // namespace graph::query::planner
