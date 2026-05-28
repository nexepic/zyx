#include "graph/query/planner/RelationshipCountFastPathPlanner.hpp"

#include <algorithm>
#include <utility>

#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

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

bool hasValueBound(const PropertyValue &value) {
	return value.getType() != PropertyType::NULL_TYPE;
}

execution::VectorizedPropertyPredicate makeRangePredicate(const std::string &variable,
                                                          const std::string &key,
                                                          execution::VectorPredicateOp op,
                                                          const PropertyValue &value) {
	execution::VectorizedPropertyPredicate predicate;
	predicate.variable = variable;
	predicate.propertyKey = key;
	predicate.op = op;
	predicate.value = value;
	return predicate;
}

bool addRangePredicates(RelationshipCountFastPathPlan &plan,
                        const std::string &variable,
                        const logical::RangePredicate &range) {
	const bool hasMin = hasValueBound(range.minValue);
	const bool hasMax = hasValueBound(range.maxValue);
	if (!hasMin && !hasMax) {
		return false;
	}

	addRequiredProperty(plan.seedRequirements, range.key);
	if (hasMin && hasMax && range.minInclusive && range.maxInclusive) {
		auto predicate = makeRangePredicate(variable, range.key, execution::VectorPredicateOp::VPO_RANGE_CLOSED, range.minValue);
		predicate.upperValue = range.maxValue;
		plan.seedPredicates.push_back(std::move(predicate));
		return true;
	}

	if (hasMin) {
		plan.seedPredicates.push_back(makeRangePredicate(
				variable,
				range.key,
				range.minInclusive ? execution::VectorPredicateOp::VPO_GE : execution::VectorPredicateOp::VPO_GT,
				range.minValue));
	}
	if (hasMax) {
		plan.seedPredicates.push_back(makeRangePredicate(
				variable,
				range.key,
				range.maxInclusive ? execution::VectorPredicateOp::VPO_LE : execution::VectorPredicateOp::VPO_LT,
				range.maxValue));
	}
	return true;
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

bool hasSelectiveSeedPredicate(const logical::LogicalNodeScan &scan) {
	return !scan.getPropertyPredicates().empty() ||
	       !scan.getRangePredicates().empty() ||
	       scan.getCompositeEquality().has_value();
}

bool isIndexCandidateSource(execution::ScanType type) {
	return type == execution::ScanType::PROPERTY_SCAN ||
	       type == execution::ScanType::RANGE_SCAN ||
	       type == execution::ScanType::COMPOSITE_SCAN;
}

bool isEqualityHandledBySeedConfig(const execution::NodeScanConfig &config, const std::string &key) {
	if (config.type == execution::ScanType::PROPERTY_SCAN) {
		return config.indexKey == key;
	}
	if (config.type != execution::ScanType::COMPOSITE_SCAN) {
		return false;
	}
	return std::find(config.compositeKeys.begin(), config.compositeKeys.end(), key) != config.compositeKeys.end();
}

bool hasOpenRangeBounds(const execution::NodeScanConfig &config) {
	return config.type == execution::ScanType::RANGE_SCAN &&
	       (config.rangeMin.getType() == PropertyType::NULL_TYPE || config.rangeMax.getType() == PropertyType::NULL_TYPE);
}

bool isRangeHandledBySeedConfig(const execution::NodeScanConfig &config, const logical::RangePredicate &range) {
	return config.type == execution::ScanType::RANGE_SCAN &&
	       config.indexKey == range.key &&
	       config.minInclusive == range.minInclusive &&
	       config.maxInclusive == range.maxInclusive &&
	       !hasOpenRangeBounds(config);
}

void fillPreferredSeedConfig(execution::NodeScanConfig &config, const logical::LogicalNodeScan &scan) {
	config.type = scan.getPreferredScanType();
	switch (config.type) {
		case execution::ScanType::PROPERTY_SCAN:
			if (!scan.getPropertyPredicates().empty()) {
				config.indexKey = scan.getPropertyPredicates().front().first;
				config.indexValue = scan.getPropertyPredicates().front().second;
			}
			break;
		case execution::ScanType::RANGE_SCAN:
			if (!scan.getRangePredicates().empty()) {
				const auto &range = scan.getRangePredicates().front();
				config.indexKey = range.key;
				config.rangeMin = range.minValue;
				config.rangeMax = range.maxValue;
				config.minInclusive = range.minInclusive;
				config.maxInclusive = range.maxInclusive;
			}
			break;
		case execution::ScanType::COMPOSITE_SCAN:
			if (scan.getCompositeEquality().has_value()) {
				config.compositeKeys = scan.getCompositeEquality()->keys;
				config.compositeValues = scan.getCompositeEquality()->values;
			}
			break;
		case execution::ScanType::LABEL_SCAN:
		case execution::ScanType::FULL_SCAN:
			break;
	}
}

execution::NodeScanConfig chooseSeedConfig(const logical::LogicalNodeScan &scan,
                                           const std::shared_ptr<indexes::IndexManager> &indexManager) {
	execution::NodeScanConfig config;
	config.variable = scan.getVariable();
	config.labels = scan.getLabels();

	if (!indexManager) {
		fillPreferredSeedConfig(config, scan);
		return config;
	}

	if (const auto &composite = scan.getCompositeEquality();
	    composite.has_value() &&
	    composite->keys.size() >= 2 &&
	    composite->keys.size() == composite->values.size() &&
	    indexManager->hasCompositeIndex("node", composite->keys)) {
		config.type = execution::ScanType::COMPOSITE_SCAN;
		config.compositeKeys = composite->keys;
		config.compositeValues = composite->values;
		return config;
	}

	for (const auto &[key, value] : scan.getPropertyPredicates()) {
		if (indexManager->hasPropertyIndex("node", key)) {
			config.type = execution::ScanType::PROPERTY_SCAN;
			config.indexKey = key;
			config.indexValue = value;
			return config;
		}
	}

	for (const auto &range : scan.getRangePredicates()) {
		if (indexManager->hasPropertyIndex("node", range.key)) {
			config.type = execution::ScanType::RANGE_SCAN;
			config.indexKey = range.key;
			config.rangeMin = range.minValue;
			config.rangeMax = range.maxValue;
			config.minInclusive = range.minInclusive;
			config.maxInclusive = range.maxInclusive;
			return config;
		}
	}

	if (!config.label().empty() && indexManager->hasLabelIndex("node")) {
		config.type = execution::ScanType::LABEL_SCAN;
	}
	return config;
}

bool hasValidIndexCandidateConfig(const execution::NodeScanConfig &config) {
	switch (config.type) {
		case execution::ScanType::PROPERTY_SCAN:
		case execution::ScanType::RANGE_SCAN:
			return !config.indexKey.empty();
		case execution::ScanType::COMPOSITE_SCAN:
			return !config.compositeKeys.empty() && config.compositeKeys.size() == config.compositeValues.size();
		case execution::ScanType::LABEL_SCAN:
		case execution::ScanType::FULL_SCAN:
			return true;
	}
	return false;
}

} // namespace

std::optional<RelationshipCountFastPathPlan>
tryBuildRelationshipCountFastPathPlan(const logical::LogicalAggregate &aggregate) {
	return tryBuildRelationshipCountFastPathPlan(aggregate, nullptr);
}

std::optional<RelationshipCountFastPathPlan>
tryBuildRelationshipCountFastPathPlan(const logical::LogicalAggregate &aggregate,
                                      const std::shared_ptr<indexes::IndexManager> &indexManager) {
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
	plan.seedConfig = chooseSeedConfig(*seedScan, indexManager);
	plan.seedRequirements.materialization = execution::NodeMaterializationMode::NSM_ID_ONLY;
	plan.seedRequirements.countOnly = true;
	plan.seedRequirements.needsLabels = true;
	plan.seedRequirements.needsActiveCheck = true;
	plan.outputAlias = agg.alias.empty() ? "count" : agg.alias;

	for (const auto &[key, value] : seedScan->getPropertyPredicates()) {
		if (!isEqualityHandledBySeedConfig(plan.seedConfig, key)) {
			addEqualityPredicate(plan, seedScan->getVariable(), key, value);
		}
	}

	for (const auto &range : seedScan->getRangePredicates()) {
		if (isRangeHandledBySeedConfig(plan.seedConfig, range)) {
			continue;
		}
		if (!addRangePredicates(plan, seedScan->getVariable(), range)) {
			return std::nullopt;
		}
	}

	if (seedScan->getCompositeEquality().has_value()) {
		const auto &composite = seedScan->getCompositeEquality().value();
		if (composite.keys.size() != composite.values.size()) {
			return std::nullopt;
		}
		for (size_t i = 0; i < composite.keys.size(); ++i) {
			if (!isEqualityHandledBySeedConfig(plan.seedConfig, composite.keys[i])) {
				addEqualityPredicate(plan, seedScan->getVariable(), composite.keys[i], composite.values[i]);
			}
		}
	}

	if (hasSelectiveSeedPredicate(*seedScan)) {
		if (!isIndexCandidateSource(plan.seedConfig.type) || !hasValidIndexCandidateConfig(plan.seedConfig)) {
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
