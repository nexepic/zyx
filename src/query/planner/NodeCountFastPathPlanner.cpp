#include "graph/query/planner/NodeCountFastPathPlanner.hpp"

#include <algorithm>
#include <utility>

#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::planner {
namespace {

bool isVariableCountArgument(const std::shared_ptr<expressions::Expression> &argument,
                             const std::string &variable) {
	if (!argument) {
		return true;
	}
	const auto *var = dynamic_cast<const expressions::VariableReferenceExpression *>(argument.get());
	return var != nullptr && !var->hasProperty() && var->getVariableName() == variable;
}

const expressions::VariableReferenceExpression *
asPropertyAccess(const std::shared_ptr<expressions::Expression> &argument,
                 const std::string &variable) {
	const auto *property = dynamic_cast<const expressions::VariableReferenceExpression *>(argument.get());
	if (!property || !property->hasProperty() || property->getVariableName() != variable) {
		return nullptr;
	}
	return property;
}

void addRequiredProperty(execution::NodeScanRequirements &requirements, const std::string &key) {
	if (std::find(requirements.requiredProperties.begin(), requirements.requiredProperties.end(), key) ==
	    requirements.requiredProperties.end()) {
		requirements.requiredProperties.push_back(key);
	}
}

template <typename Plan>
void addEqualityPredicate(Plan &plan,
                          const std::string &variable,
                          const std::string &key,
                          const PropertyValue &value) {
	addRequiredProperty(plan.requirements, key);
	execution::VectorizedPropertyPredicate predicate;
	predicate.variable = variable;
	predicate.propertyKey = key;
	predicate.op = execution::VectorPredicateOp::VPO_EQ;
	predicate.value = value;
	plan.predicates.push_back(std::move(predicate));
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

bool hasOpenRangeBounds(const execution::NodeScanConfig &config) {
	return config.type == execution::ScanType::RANGE_SCAN &&
	       (config.rangeMin.getType() == PropertyType::NULL_TYPE || config.rangeMax.getType() == PropertyType::NULL_TYPE);
}

bool isEqualityHandledByConfig(const execution::NodeScanConfig &config, const std::string &key) {
	if (config.type == execution::ScanType::PROPERTY_SCAN) {
		return config.indexKey == key;
	}
	if (config.type != execution::ScanType::COMPOSITE_SCAN) {
		return false;
	}
	return std::find(config.compositeKeys.begin(), config.compositeKeys.end(), key) != config.compositeKeys.end();
}

bool isRangeHandledByConfig(const execution::NodeScanConfig &config, const logical::RangePredicate &range) {
	return config.type == execution::ScanType::RANGE_SCAN &&
	       config.indexKey == range.key &&
	       config.minInclusive == range.minInclusive &&
	       config.maxInclusive == range.maxInclusive &&
	       !hasOpenRangeBounds(config);
}

template <typename Plan>
bool addRangePredicates(Plan &plan,
                        const std::string &variable,
                        const logical::RangePredicate &range) {
	const bool hasMin = hasValueBound(range.minValue);
	const bool hasMax = hasValueBound(range.maxValue);
	if (!hasMin && !hasMax) {
		return false;
	}

	addRequiredProperty(plan.requirements, range.key);
	if (hasMin && hasMax && range.minInclusive && range.maxInclusive) {
		auto predicate = makeRangePredicate(variable, range.key, execution::VectorPredicateOp::VPO_RANGE_CLOSED, range.minValue);
		predicate.upperValue = range.maxValue;
		plan.predicates.push_back(std::move(predicate));
		return true;
	}

	if (hasMin) {
		plan.predicates.push_back(makeRangePredicate(
				variable,
				range.key,
				range.minInclusive ? execution::VectorPredicateOp::VPO_GE : execution::VectorPredicateOp::VPO_GT,
				range.minValue));
	}
	if (hasMax) {
		plan.predicates.push_back(makeRangePredicate(
				variable,
				range.key,
				range.maxInclusive ? execution::VectorPredicateOp::VPO_LE : execution::VectorPredicateOp::VPO_LT,
				range.maxValue));
	}
	return true;
}

void fillPreferredConfig(execution::NodeScanConfig &config, const logical::LogicalNodeScan &scan) {
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

execution::NodeScanConfig chooseCountConfig(const logical::LogicalNodeScan &scan,
                                            const std::shared_ptr<indexes::IndexManager> &indexManager) {
	execution::NodeScanConfig config;
	config.variable = scan.getVariable();
	config.labels = scan.getLabels();

	if (!indexManager) {
		fillPreferredConfig(config, scan);
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
		if (indexManager->hasPropertyIndex("node", range.key) &&
		    hasValueBound(range.minValue) &&
		    hasValueBound(range.maxValue)) {
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

bool hasValidIndexConfig(const execution::NodeScanConfig &config) {
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

void fallbackToLabelOrFull(execution::NodeScanConfig &config) {
	config.type = config.labels.empty() ? execution::ScanType::FULL_SCAN : execution::ScanType::LABEL_SCAN;
	config.indexKey.clear();
	config.indexValue = PropertyValue();
	config.rangeMin = PropertyValue();
	config.rangeMax = PropertyValue();
	config.compositeKeys.clear();
	config.compositeValues.clear();
}

} // namespace

std::optional<NodeCountFastPathPlan>
tryBuildNodeCountFastPathPlan(const logical::LogicalAggregate &aggregate) {
	return tryBuildNodeCountFastPathPlan(aggregate, nullptr);
}

std::optional<NodeCountFastPathPlan>
tryBuildNodeCountFastPathPlan(const logical::LogicalAggregate &aggregate,
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
	if (!isVariableCountArgument(agg.argument, scan->getVariable())) {
		return std::nullopt;
	}

	NodeCountFastPathPlan plan;
	plan.config = chooseCountConfig(*scan, indexManager);
	plan.outputAlias = agg.alias.empty() ? "count" : agg.alias;
	plan.requirements.materialization = execution::NodeMaterializationMode::NSM_ID_ONLY;
	plan.requirements.countOnly = true;
	plan.requirements.needsLabels = true;
	plan.requirements.needsActiveCheck = true;

	if (!hasValidIndexConfig(plan.config) || hasOpenRangeBounds(plan.config)) {
		fallbackToLabelOrFull(plan.config);
	}

	for (const auto &[key, value] : scan->getPropertyPredicates()) {
		if (!isEqualityHandledByConfig(plan.config, key)) {
			addEqualityPredicate(plan, scan->getVariable(), key, value);
		}
	}

	for (const auto &range : scan->getRangePredicates()) {
		if (isRangeHandledByConfig(plan.config, range)) {
			continue;
		}
		if (!addRangePredicates(plan, scan->getVariable(), range)) {
			return std::nullopt;
		}
	}

	if (scan->getCompositeEquality().has_value()) {
		const auto &composite = scan->getCompositeEquality().value();
		if (composite.keys.size() != composite.values.size()) {
			return std::nullopt;
		}
		for (size_t i = 0; i < composite.keys.size(); ++i) {
			if (!isEqualityHandledByConfig(plan.config, composite.keys[i])) {
				addEqualityPredicate(plan, scan->getVariable(), composite.keys[i], composite.values[i]);
			}
		}
	}

	if (!plan.predicates.empty()) {
		plan.requirements.materialization = execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	}

	return plan;
}

std::optional<NodeDistinctCountFastPathPlan>
tryBuildNodeDistinctCountFastPathPlan(const logical::LogicalAggregate &aggregate) {
	return tryBuildNodeDistinctCountFastPathPlan(aggregate, nullptr);
}

std::optional<NodeDistinctCountFastPathPlan>
tryBuildNodeDistinctCountFastPathPlan(const logical::LogicalAggregate &aggregate,
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
	const auto *distinctProperty = asPropertyAccess(agg.argument, scan->getVariable());
	if (distinctProperty == nullptr) {
		return std::nullopt;
	}

	NodeDistinctCountFastPathPlan plan;
	plan.config = chooseCountConfig(*scan, indexManager);
	plan.distinctProperty = distinctProperty->getPropertyName();
	plan.outputAlias = agg.alias.empty() ? "count" : agg.alias;
	plan.requirements.materialization = execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	plan.requirements.countOnly = true;
	plan.requirements.needsLabels = true;
	plan.requirements.needsActiveCheck = true;
	addRequiredProperty(plan.requirements, plan.distinctProperty);

	if (!hasValidIndexConfig(plan.config) || hasOpenRangeBounds(plan.config)) {
		fallbackToLabelOrFull(plan.config);
	}

	for (const auto &[key, value] : scan->getPropertyPredicates()) {
		if (!isEqualityHandledByConfig(plan.config, key)) {
			addEqualityPredicate(plan, scan->getVariable(), key, value);
		}
	}

	for (const auto &range : scan->getRangePredicates()) {
		if (isRangeHandledByConfig(plan.config, range)) {
			continue;
		}
		if (!addRangePredicates(plan, scan->getVariable(), range)) {
			return std::nullopt;
		}
	}

	if (scan->getCompositeEquality().has_value()) {
		const auto &composite = scan->getCompositeEquality().value();
		if (composite.keys.size() != composite.values.size()) {
			return std::nullopt;
		}
		for (size_t i = 0; i < composite.keys.size(); ++i) {
			if (!isEqualityHandledByConfig(plan.config, composite.keys[i])) {
				addEqualityPredicate(plan, scan->getVariable(), composite.keys[i], composite.values[i]);
			}
		}
	}

	return plan;
}

std::optional<NodeGroupCountFastPathPlan>
tryBuildNodeGroupCountFastPathPlan(const logical::LogicalAggregate &aggregate) {
	return tryBuildNodeGroupCountFastPathPlan(aggregate, nullptr);
}

std::optional<NodeGroupCountFastPathPlan>
tryBuildNodeGroupCountFastPathPlan(const logical::LogicalAggregate &aggregate,
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
	if (!isVariableCountArgument(agg.argument, scan->getVariable())) {
		return std::nullopt;
	}

	const auto *groupProperty = asPropertyAccess(aggregate.getGroupByExprs()[0], scan->getVariable());
	if (groupProperty == nullptr) {
		return std::nullopt;
	}

	NodeGroupCountFastPathPlan plan;
	plan.config = chooseCountConfig(*scan, indexManager);
	plan.groupProperty = groupProperty->getPropertyName();
	const auto &aliases = aggregate.getGroupByAliases();
	plan.groupAlias = !aliases.empty() && !aliases[0].empty() ? aliases[0] : aggregate.getGroupByExprs()[0]->toString();
	plan.outputAlias = agg.alias.empty() ? "count" : agg.alias;
	plan.requirements.materialization = execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	plan.requirements.countOnly = true;
	plan.requirements.needsLabels = true;
	plan.requirements.needsActiveCheck = true;
	addRequiredProperty(plan.requirements, plan.groupProperty);

	if (!hasValidIndexConfig(plan.config) || hasOpenRangeBounds(plan.config)) {
		fallbackToLabelOrFull(plan.config);
	}

	for (const auto &[key, value] : scan->getPropertyPredicates()) {
		if (!isEqualityHandledByConfig(plan.config, key)) {
			addEqualityPredicate(plan, scan->getVariable(), key, value);
		}
	}

	for (const auto &range : scan->getRangePredicates()) {
		if (isRangeHandledByConfig(plan.config, range)) {
			continue;
		}
		if (!addRangePredicates(plan, scan->getVariable(), range)) {
			return std::nullopt;
		}
	}

	if (scan->getCompositeEquality().has_value()) {
		const auto &composite = scan->getCompositeEquality().value();
		if (composite.keys.size() != composite.values.size()) {
			return std::nullopt;
		}
		for (size_t i = 0; i < composite.keys.size(); ++i) {
			if (!isEqualityHandledByConfig(plan.config, composite.keys[i])) {
				addEqualityPredicate(plan, scan->getVariable(), composite.keys[i], composite.values[i]);
			}
		}
	}

	return plan;
}

} // namespace graph::query::planner
