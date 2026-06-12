#include "graph/query/planner/NodeAccessPathPlanner.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::planner {
namespace {

execution::VectorizedPropertyPredicate makePredicate(const std::string &variable,
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

void appendEqualityPredicate(const std::string &variable,
                             const std::string &key,
                             const PropertyValue &value,
                             execution::NodeScanRequirements &requirements,
                             std::vector<execution::VectorizedPropertyPredicate> &predicates) {
	addRequiredNodeProperty(requirements, key);
	predicates.push_back(makePredicate(variable, key, execution::VectorPredicateOp::VPO_EQ, value));
}

bool appendRangePredicates(const std::string &variable,
                           const logical::RangePredicate &range,
                           execution::NodeScanRequirements &requirements,
                           std::vector<execution::VectorizedPropertyPredicate> &predicates) {
	const bool hasMin = hasBoundValue(range.minValue);
	const bool hasMax = hasBoundValue(range.maxValue);
	if (!hasMin && !hasMax) {
		return false;
	}

	addRequiredNodeProperty(requirements, range.key);
	if (hasMin && hasMax && range.minInclusive && range.maxInclusive) {
		auto predicate = makePredicate(variable, range.key, execution::VectorPredicateOp::VPO_RANGE_CLOSED, range.minValue);
		predicate.upperValue = range.maxValue;
		predicates.push_back(std::move(predicate));
		return true;
	}

	if (hasMin) {
		predicates.push_back(makePredicate(
				variable,
				range.key,
				range.minInclusive ? execution::VectorPredicateOp::VPO_GE : execution::VectorPredicateOp::VPO_GT,
				range.minValue));
	}
	if (hasMax) {
		predicates.push_back(makePredicate(
				variable,
				range.key,
				range.maxInclusive ? execution::VectorPredicateOp::VPO_LE : execution::VectorPredicateOp::VPO_LT,
				range.maxValue));
	}
	return true;
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

execution::NodeScanConfig makeBaseConfig(const logical::LogicalNodeScan &scan) {
	execution::NodeScanConfig config;
	config.variable = scan.getVariable();
	config.labels = scan.getLabels();
	return config;
}

NodeAccessPathKind kindForScanType(execution::ScanType type) {
	switch (type) {
		case execution::ScanType::PROPERTY_SCAN:
			return NodeAccessPathKind::NAP_PROPERTY_INDEX;
		case execution::ScanType::RANGE_SCAN:
			return NodeAccessPathKind::NAP_RANGE_INDEX;
		case execution::ScanType::COMPOSITE_SCAN:
			return NodeAccessPathKind::NAP_COMPOSITE_INDEX;
		case execution::ScanType::LABEL_SCAN:
			return NodeAccessPathKind::NAP_LABEL_SCAN;
		case execution::ScanType::FULL_SCAN:
			return NodeAccessPathKind::NAP_FULL_SCAN;
	}
	return NodeAccessPathKind::NAP_FULL_SCAN;
}

bool hasNodePropertyIndexCandidate(
		const logical::LogicalNodeScan &scan,
		const std::shared_ptr<indexes::IndexManager> &indexManager,
		const std::string &key) {
	if (!indexManager || key.empty()) {
		return false;
	}
	const auto &labels = scan.getLabels();
	if (labels.size() == 1 && indexManager->hasNodePropertyIndexForLabel(labels.front(), key)) {
		return true;
	}
	return indexManager->hasPropertyIndex("node", key);
}

double heuristicCostForKind(NodeAccessPathKind kind) {
	switch (kind) {
		case NodeAccessPathKind::NAP_COMPOSITE_INDEX:
			return 4.0;
		case NodeAccessPathKind::NAP_PROPERTY_INDEX:
		case NodeAccessPathKind::NAP_RANGE_INDEX:
			return 8.0;
		case NodeAccessPathKind::NAP_LABEL_SCAN:
			return 1'000'000'000'000.0;
		case NodeAccessPathKind::NAP_FULL_SCAN:
			return 2'000'000'000'000.0;
	}
	return std::numeric_limits<double>::max();
}

std::optional<int64_t> estimateExactCandidateCardinality(
		const execution::NodeScanConfig &config,
		const std::shared_ptr<indexes::IndexManager> &indexManager) {
	if (!indexManager) {
		return std::nullopt;
	}

	switch (config.type) {
		case execution::ScanType::PROPERTY_SCAN:
			if (config.labels.size() == 1 &&
			    indexManager->hasNodePropertyIndexForLabel(config.label(), config.indexKey)) {
				return static_cast<int64_t>(
						indexManager->estimateNodeIdsByLabelAndProperty(config.label(), config.indexKey, config.indexValue));
			}
			return static_cast<int64_t>(indexManager->estimateNodeIdsByProperty(config.indexKey, config.indexValue));

		case execution::ScanType::RANGE_SCAN:
			if (config.labels.size() == 1 &&
			    indexManager->hasNodePropertyIndexForLabel(config.label(), config.indexKey)) {
				return static_cast<int64_t>(
						indexManager->estimateNodeIdsByLabelAndPropertyRange(
								config.label(),
								config.indexKey,
								config.rangeMin,
								config.rangeMax,
								config.minInclusive,
								config.maxInclusive));
			}
			return static_cast<int64_t>(
					indexManager->estimateNodeIdsByPropertyRange(
							config.indexKey,
							config.rangeMin,
							config.rangeMax,
							config.minInclusive,
							config.maxInclusive));

		case execution::ScanType::COMPOSITE_SCAN:
			return static_cast<int64_t>(
					indexManager->estimateNodeIdsByCompositeIndex(config.compositeKeys, config.compositeValues));

		case execution::ScanType::LABEL_SCAN:
			if (!config.label().empty() && indexManager->hasLabelIndex("node")) {
				return static_cast<int64_t>(indexManager->estimateNodeIdsByLabel(config.label()));
			}
			break;

		case execution::ScanType::FULL_SCAN:
			break;
	}
	return std::nullopt;
}

NodeAccessPathEstimate estimateCandidate(
		const execution::NodeScanConfig &config,
		NodeAccessPathKind kind,
		const std::shared_ptr<indexes::IndexManager> &indexManager) {
	NodeAccessPathEstimate estimate;
	estimate.cost = heuristicCostForKind(kind);
	estimate.source = "heuristic";

	if (auto exact = estimateExactCandidateCardinality(config, indexManager)) {
		estimate.cardinality = *exact;
		estimate.exactCardinality = true;
		estimate.cost += static_cast<double>(*exact);
		estimate.source = "index_count";
	}
	return estimate;
}

bool isBetterCandidate(const NodeAccessPathCandidate &left,
                       const NodeAccessPathCandidate &right) {
	if (left.valid != right.valid) {
		return left.valid;
	}
	if (left.estimate.cost != right.estimate.cost) {
		return left.estimate.cost < right.estimate.cost;
	}
	return static_cast<int>(left.kind) < static_cast<int>(right.kind);
}

NodeAccessPathCandidate makeCandidate(execution::NodeScanConfig config,
                                      std::string reason,
                                      const std::shared_ptr<indexes::IndexManager> &indexManager,
                                      bool preferred = false) {
	NodeAccessPathCandidate candidate;
	candidate.kind = kindForScanType(config.type);
	candidate.config = std::move(config);
	candidate.reason = std::move(reason);
	candidate.preferred = preferred;
	candidate.valid = hasValidNodeCandidateConfig(candidate.config);
	candidate.directCandidateLookup = isIndexCandidateSource(candidate.config.type);
	candidate.openRange = hasOpenRangeBounds(candidate.config);
	candidate.estimate = estimateCandidate(
			candidate.config,
			candidate.kind,
			candidate.valid ? indexManager : nullptr);
	return candidate;
}

void appendFullScanFallback(const logical::LogicalNodeScan &scan,
                            std::vector<NodeAccessPathCandidate> &candidates) {
	auto config = makeBaseConfig(scan);
	config.type = execution::ScanType::FULL_SCAN;
	candidates.push_back(makeCandidate(std::move(config), "full_scan_fallback", nullptr));
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

} // namespace

const char *nodeAccessPathKindName(NodeAccessPathKind kind) {
	switch (kind) {
		case NodeAccessPathKind::NAP_FULL_SCAN:
			return "full_scan";
		case NodeAccessPathKind::NAP_LABEL_SCAN:
			return "label_scan";
		case NodeAccessPathKind::NAP_PROPERTY_INDEX:
			return "property_index";
		case NodeAccessPathKind::NAP_RANGE_INDEX:
			return "range_index";
		case NodeAccessPathKind::NAP_COMPOSITE_INDEX:
			return "composite_index";
	}
	return "unknown";
}

AccessPathSummary summarizeNodeAccessPath(const NodeAccessPathCandidate &candidate) {
	AccessPathSummary summary;
	summary.kind = nodeAccessPathKindName(candidate.kind);
	summary.reason = candidate.reason;
	summary.estimatedCardinality = candidate.estimate.cardinality;
	summary.estimatedCost = candidate.estimate.cost;
	summary.exactCardinality = candidate.estimate.exactCardinality;
	summary.estimateSource = candidate.estimate.source;
	summary.directCandidateLookup = candidate.directCandidateLookup;
	summary.valid = candidate.valid;
	return summary;
}

bool isNodeVariableReference(const std::shared_ptr<expressions::Expression> &expression,
                             const std::string &variable) {
	if (!expression) {
		return true;
	}
	const auto *var = dynamic_cast<const expressions::VariableReferenceExpression *>(expression.get());
	return var != nullptr && !var->hasProperty() && var->getVariableName() == variable;
}

const expressions::VariableReferenceExpression *asNodePropertyAccess(
		const std::shared_ptr<expressions::Expression> &expression,
		const std::string &variable) {
	const auto *property = dynamic_cast<const expressions::VariableReferenceExpression *>(expression.get());
	if (!property || !property->hasProperty() || property->getVariableName() != variable) {
		return nullptr;
	}
	return property;
}

void addRequiredNodeProperty(execution::NodeScanRequirements &requirements,
                             const std::string &key) {
	if (std::find(requirements.requiredProperties.begin(), requirements.requiredProperties.end(), key) ==
	    requirements.requiredProperties.end()) {
		requirements.requiredProperties.push_back(key);
	}
}

bool hasBoundValue(const PropertyValue &value) {
	return value.getType() != PropertyType::NULL_TYPE;
}

bool hasOpenRangeBounds(const execution::NodeScanConfig &config) {
	return config.type == execution::ScanType::RANGE_SCAN &&
	       (config.rangeMin.getType() == PropertyType::NULL_TYPE || config.rangeMax.getType() == PropertyType::NULL_TYPE);
}

bool isIndexCandidateSource(execution::ScanType type) {
	return type == execution::ScanType::PROPERTY_SCAN ||
	       type == execution::ScanType::RANGE_SCAN ||
	       type == execution::ScanType::COMPOSITE_SCAN;
}

bool hasValidNodeCandidateConfig(const execution::NodeScanConfig &config) {
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

void fallbackToLabelOrFullScan(execution::NodeScanConfig &config) {
	config.type = config.labels.empty() ? execution::ScanType::FULL_SCAN : execution::ScanType::LABEL_SCAN;
	config.indexKey.clear();
	config.indexValue = PropertyValue();
	config.rangeMin = PropertyValue();
	config.rangeMax = PropertyValue();
	config.compositeKeys.clear();
	config.compositeValues.clear();
}

execution::NodeScanConfig chooseNodeAccessPathConfig(
		const logical::LogicalNodeScan &scan,
		const std::shared_ptr<indexes::IndexManager> &indexManager,
		NodeAccessPathOptions options) {
	return chooseNodeAccessPathDecision(scan, indexManager, options).selected.config;
}

NodeAccessPathDecision chooseNodeAccessPathDecision(
		const logical::LogicalNodeScan &scan,
		const std::shared_ptr<indexes::IndexManager> &indexManager,
		NodeAccessPathOptions options) {
	NodeAccessPathDecision decision;
	if (!indexManager) {
		auto config = makeBaseConfig(scan);
		fillPreferredConfig(config, scan);
		decision.candidates.push_back(makeCandidate(std::move(config), "preferred_scan", indexManager, true));
		decision.selected = decision.candidates.front();
		return decision;
	}

	if (const auto &composite = scan.getCompositeEquality();
	    composite.has_value() &&
	    composite->keys.size() >= 2 &&
	    composite->keys.size() == composite->values.size() &&
	    indexManager->hasCompositeIndex("node", composite->keys)) {
		auto config = makeBaseConfig(scan);
		config.type = execution::ScanType::COMPOSITE_SCAN;
		config.compositeKeys = composite->keys;
		config.compositeValues = composite->values;
		decision.candidates.push_back(makeCandidate(std::move(config), "composite_index", indexManager));
	}

	for (const auto &[key, value] : scan.getPropertyPredicates()) {
		if (hasNodePropertyIndexCandidate(scan, indexManager, key)) {
			auto config = makeBaseConfig(scan);
			config.type = execution::ScanType::PROPERTY_SCAN;
			config.indexKey = key;
			config.indexValue = value;
			decision.candidates.push_back(makeCandidate(std::move(config), "property_index", indexManager));
		}
	}

	for (const auto &range : scan.getRangePredicates()) {
		const bool hasClosedCandidateBounds = hasBoundValue(range.minValue) && hasBoundValue(range.maxValue);
		if (hasNodePropertyIndexCandidate(scan, indexManager, range.key) &&
		    (options.allowOpenRangeIndex || hasClosedCandidateBounds)) {
			auto config = makeBaseConfig(scan);
			config.type = execution::ScanType::RANGE_SCAN;
			config.indexKey = range.key;
			config.rangeMin = range.minValue;
			config.rangeMax = range.maxValue;
			config.minInclusive = range.minInclusive;
			config.maxInclusive = range.maxInclusive;
			decision.candidates.push_back(makeCandidate(std::move(config), "range_index", indexManager));
		}
	}

	auto config = makeBaseConfig(scan);
	if (!config.label().empty() && indexManager->hasLabelIndex("node")) {
		config.type = execution::ScanType::LABEL_SCAN;
		decision.candidates.push_back(makeCandidate(std::move(config), "label_index", indexManager));
	}
	appendFullScanFallback(scan, decision.candidates);
	decision.selected = *std::min_element(decision.candidates.begin(), decision.candidates.end(), isBetterCandidate);
	return decision;
}

bool appendResidualNodePredicates(
		const logical::LogicalNodeScan &scan,
		const execution::NodeScanConfig &config,
		execution::NodeScanRequirements &requirements,
		std::vector<execution::VectorizedPropertyPredicate> &predicates) {
	for (const auto &[key, value] : scan.getPropertyPredicates()) {
		if (!isEqualityHandledByConfig(config, key)) {
			appendEqualityPredicate(scan.getVariable(), key, value, requirements, predicates);
		}
	}

	for (const auto &range : scan.getRangePredicates()) {
		if (isRangeHandledByConfig(config, range)) {
			continue;
		}
		if (!appendRangePredicates(scan.getVariable(), range, requirements, predicates)) {
			return false;
		}
	}

	if (scan.getCompositeEquality().has_value()) {
		const auto &composite = scan.getCompositeEquality().value();
		if (composite.keys.size() != composite.values.size()) {
			return false;
		}
		for (size_t index = 0; index < composite.keys.size(); ++index) {
			if (!isEqualityHandledByConfig(config, composite.keys[index])) {
				appendEqualityPredicate(scan.getVariable(), composite.keys[index], composite.values[index], requirements, predicates);
			}
		}
	}

	return true;
}

} // namespace graph::query::planner
