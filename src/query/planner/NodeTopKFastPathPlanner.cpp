#include "graph/query/planner/NodeTopKFastPathPlanner.hpp"

#include <algorithm>
#include <utility>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::planner {
namespace {

const expressions::VariableReferenceExpression *asPropertyAccess(
		const std::shared_ptr<expressions::Expression> &expression,
		const std::string &variable) {
	const auto *property = dynamic_cast<const expressions::VariableReferenceExpression *>(expression.get());
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

void addEqualityPredicate(NodeTopKFastPathPlan &plan,
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

bool addRangePredicates(NodeTopKFastPathPlan &plan,
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

execution::NodeScanConfig chooseConfig(const logical::LogicalNodeScan &scan,
                                       const std::shared_ptr<indexes::IndexManager> &indexManager) {
	execution::NodeScanConfig config;
	config.variable = scan.getVariable();
	config.labels = scan.getLabels();

	if (!indexManager) {
		fillPreferredConfig(config, scan);
		return config;
	}

	if (const auto &composite = scan.getCompositeEquality();
	    composite.has_value() && composite->keys.size() >= 2 && composite->keys.size() == composite->values.size() &&
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
		if (indexManager->hasPropertyIndex("node", range.key) && hasValueBound(range.minValue) && hasValueBound(range.maxValue)) {
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

std::optional<NodeTopKFastPathPlan>
tryBuildNodeTopKFastPathPlan(const logical::LogicalProject &project) {
	return tryBuildNodeTopKFastPathPlan(project, nullptr);
}

std::optional<NodeTopKFastPathPlan>
tryBuildNodeTopKFastPathPlan(const logical::LogicalProject &project,
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

	const auto *sortProperty = asPropertyAccess(sort->getSortItems()[0].expression, variable);
	if (sortProperty == nullptr) {
		return std::nullopt;
	}

	NodeTopKFastPathPlan plan;
	plan.config = chooseConfig(*scan, indexManager);
	plan.requirements.materialization = execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	plan.requirements.needsLabels = true;
	plan.requirements.needsActiveCheck = true;
	plan.sortProperty = sortProperty->getPropertyName();
	plan.ascending = sort->getSortItems()[0].ascending;
	plan.limit = limit->getLimit();
	addRequiredProperty(plan.requirements, plan.sortProperty);

	for (const auto &item : project.getItems()) {
		const auto *projectionProperty = asPropertyAccess(item.expression, variable);
		if (projectionProperty == nullptr) {
			return std::nullopt;
		}
		plan.projections.push_back({projectionProperty->getPropertyName(), item.alias});
		addRequiredProperty(plan.requirements, projectionProperty->getPropertyName());
	}

	if (!hasValidIndexConfig(plan.config) || hasOpenRangeBounds(plan.config)) {
		fallbackToLabelOrFull(plan.config);
	}

	for (const auto &[key, value] : scan->getPropertyPredicates()) {
		if (!isEqualityHandledByConfig(plan.config, key)) {
			addEqualityPredicate(plan, variable, key, value);
		}
	}

	for (const auto &range : scan->getRangePredicates()) {
		if (isRangeHandledByConfig(plan.config, range)) {
			continue;
		}
		if (!addRangePredicates(plan, variable, range)) {
			return std::nullopt;
		}
	}

	if (scan->getCompositeEquality().has_value()) {
		const auto &composite = scan->getCompositeEquality().value();
		if (composite.keys.size() != composite.values.size()) {
			return std::nullopt;
		}
		for (size_t index = 0; index < composite.keys.size(); ++index) {
			if (!isEqualityHandledByConfig(plan.config, composite.keys[index])) {
				addEqualityPredicate(plan, variable, composite.keys[index], composite.values[index]);
			}
		}
	}

	return plan;
}

} // namespace graph::query::planner
