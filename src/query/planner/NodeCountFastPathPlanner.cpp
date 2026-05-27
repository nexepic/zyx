#include "graph/query/planner/NodeCountFastPathPlanner.hpp"

#include <algorithm>
#include <utility>

#include "graph/query/logical/operators/LogicalNodeScan.hpp"

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

void addRequiredProperty(execution::NodeScanRequirements &requirements, const std::string &key) {
	if (std::find(requirements.requiredProperties.begin(), requirements.requiredProperties.end(), key) ==
	    requirements.requiredProperties.end()) {
		requirements.requiredProperties.push_back(key);
	}
}

void addEqualityPredicate(NodeCountFastPathPlan &plan,
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

bool addRangePredicates(NodeCountFastPathPlan &plan,
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

} // namespace

std::optional<NodeCountFastPathPlan>
tryBuildNodeCountFastPathPlan(const logical::LogicalAggregate &aggregate) {
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
	plan.config.type = scan->getPreferredScanType();
	plan.config.variable = scan->getVariable();
	plan.config.labels = scan->getLabels();
	plan.outputAlias = agg.alias.empty() ? "count" : agg.alias;
	plan.requirements.materialization = execution::NodeMaterializationMode::NSM_ID_ONLY;
	plan.requirements.countOnly = true;
	plan.requirements.needsLabels = true;
	plan.requirements.needsActiveCheck = true;

	for (const auto &[key, value] : scan->getPropertyPredicates()) {
		addEqualityPredicate(plan, scan->getVariable(), key, value);
	}

	for (const auto &range : scan->getRangePredicates()) {
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
			addEqualityPredicate(plan, scan->getVariable(), composite.keys[i], composite.values[i]);
		}
	}

	if (!plan.predicates.empty()) {
		plan.requirements.materialization = execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	}

	// Keep candidate discovery aligned with the optimizer's selected scan strategy.
	switch (plan.config.type) {
		case execution::ScanType::PROPERTY_SCAN:
			if (!scan->getPropertyPredicates().empty()) {
				plan.config.indexKey = scan->getPropertyPredicates().front().first;
				plan.config.indexValue = scan->getPropertyPredicates().front().second;
			}
			break;
		case execution::ScanType::RANGE_SCAN:
			if (!scan->getRangePredicates().empty()) {
				const auto &range = scan->getRangePredicates().front();
				plan.config.indexKey = range.key;
				plan.config.rangeMin = range.minValue;
				plan.config.rangeMax = range.maxValue;
				plan.config.minInclusive = range.minInclusive;
				plan.config.maxInclusive = range.maxInclusive;
			}
			break;
		case execution::ScanType::COMPOSITE_SCAN:
			if (scan->getCompositeEquality().has_value()) {
				plan.config.compositeKeys = scan->getCompositeEquality()->keys;
				plan.config.compositeValues = scan->getCompositeEquality()->values;
			}
			break;
		case execution::ScanType::LABEL_SCAN:
		case execution::ScanType::FULL_SCAN:
			break;
	}

	if (hasOpenRangeBounds(plan.config)) {
		plan.config.type = plan.config.labels.empty() ? execution::ScanType::FULL_SCAN : execution::ScanType::LABEL_SCAN;
	}

	return plan;
}

} // namespace graph::query::planner
