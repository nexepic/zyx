#include "graph/query/planner/RelationshipCountFastPathPlanner.hpp"

#include <algorithm>
#include <utility>

#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"

namespace graph::query::planner {
namespace {

void addRequiredProperty(execution::NodeScanRequirements &requirements, const std::string &key) {
	if (std::find(requirements.requiredProperties.begin(), requirements.requiredProperties.end(), key) == requirements.requiredProperties.end()) {
		requirements.requiredProperties.push_back(key);
	}
}

void addEqualityPredicate(RelationshipCountFastPathPlan &plan,
                          const std::string &variable,
                          const std::string &key,
                          const PropertyValue &value) {
	addRequiredProperty(plan.seedRequirements, key);
	execution::VectorizedPropertyPredicate predicate;
	predicate.variable = variable;
	predicate.propertyKey = key;
	predicate.op = execution::VectorPredicateOp::VPO_EQ;
	predicate.value = value;
	plan.seedPredicates.push_back(std::move(predicate));
}

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
		return name == hop.edgeVar || name == hop.sourceVar || name == hop.targetVar;
	});
}

bool hasUnsupportedTraversalFilters(const logical::LogicalTraversal &traversal) {
	return !traversal.getTargetProperties().empty() || !traversal.getEdgeProperties().empty();
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

} // namespace

std::optional<RelationshipCountFastPathPlan>
tryBuildRelationshipCountFastPathPlan(const logical::LogicalAggregate &aggregate) {
	if (!aggregate.getGroupByExprs().empty() || aggregate.getAggregations().size() != 1) {
		return std::nullopt;
	}

	const auto &agg = aggregate.getAggregations()[0];
	if (agg.functionName != "count" || agg.distinct) {
		return std::nullopt;
	}

	const auto children = aggregate.getChildren();
	if (children.size() != 1 || children[0] == nullptr || children[0]->getType() != logical::LogicalOpType::LOP_TRAVERSAL) {
		return std::nullopt;
	}

	std::vector<const logical::LogicalTraversal *> traversalChain;
	const logical::LogicalOperator *cursor = children[0];
	while (cursor != nullptr && cursor->getType() == logical::LogicalOpType::LOP_TRAVERSAL) {
		const auto *traversal = static_cast<const logical::LogicalTraversal *>(cursor);
		if (hasUnsupportedTraversalFilters(*traversal)) {
			return std::nullopt;
		}
		traversalChain.push_back(traversal);
		const auto traversalChildren = traversal->getChildren();
		if (traversalChildren.size() != 1) {
			return std::nullopt;
		}
		cursor = traversalChildren[0];
	}

	if (traversalChain.empty() || traversalChain.size() > 2 || cursor == nullptr || cursor->getType() != logical::LogicalOpType::LOP_NODE_SCAN) {
		return std::nullopt;
	}

	const auto *seedScan = static_cast<const logical::LogicalNodeScan *>(cursor);
	RelationshipCountFastPathPlan plan;
	plan.seedConfig.type = seedScan->getPreferredScanType();
	plan.seedConfig.variable = seedScan->getVariable();
	plan.seedConfig.labels = seedScan->getLabels();
	plan.seedRequirements.materialization = execution::NodeMaterializationMode::NSM_ID_ONLY;
	plan.seedRequirements.countOnly = true;
	plan.seedRequirements.needsLabels = true;
	plan.seedRequirements.needsActiveCheck = true;
	plan.outputAlias = agg.alias.empty() ? "count" : agg.alias;

	for (const auto &[key, value] : seedScan->getPropertyPredicates()) {
		addEqualityPredicate(plan, seedScan->getVariable(), key, value);
	}

	if (!plan.seedPredicates.empty()) {
		plan.seedRequirements.materialization = execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	}

	switch (plan.seedConfig.type) {
		case execution::ScanType::PROPERTY_SCAN:
			if (!seedScan->getPropertyPredicates().empty()) {
				plan.seedConfig.indexKey = seedScan->getPropertyPredicates().front().first;
				plan.seedConfig.indexValue = seedScan->getPropertyPredicates().front().second;
			}
			break;
		case execution::ScanType::RANGE_SCAN:
			if (!seedScan->getRangePredicates().empty()) {
				const auto &range = seedScan->getRangePredicates().front();
				plan.seedConfig.indexKey = range.key;
				plan.seedConfig.rangeMin = range.minValue;
				plan.seedConfig.rangeMax = range.maxValue;
				plan.seedConfig.minInclusive = range.minInclusive;
				plan.seedConfig.maxInclusive = range.maxInclusive;
			}
			break;
		case execution::ScanType::COMPOSITE_SCAN:
			if (seedScan->getCompositeEquality().has_value()) {
				plan.seedConfig.compositeKeys = seedScan->getCompositeEquality()->keys;
				plan.seedConfig.compositeValues = seedScan->getCompositeEquality()->values;
			}
			break;
		case execution::ScanType::LABEL_SCAN:
		case execution::ScanType::FULL_SCAN:
			break;
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
