#include "graph/query/planner/RelationshipAccessPathPlanner.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::planner {
namespace {

using execution::DirectRelationshipCountConfig;
using execution::VectorPredicateOp;
using execution::VectorizedPropertyPredicate;

constexpr double kColumnarScanCost = 1'000'000'000.0;
constexpr double kColumnarMetadataCountCost = kColumnarScanCost;
constexpr double kPropertyIndexCost = 8.0;
constexpr double kTypeIndexCost = 8.0;
constexpr double kIntersectionIndexCost = 4.0;

std::vector<VectorizedPropertyPredicate> effectivePredicates(const DirectRelationshipCountConfig &config) {
	if (!config.edgePredicates.empty()) {
		return config.edgePredicates;
	}
	std::vector<VectorizedPropertyPredicate> predicates;
	predicates.reserve(config.edgeProperties.size());
	for (const auto &[key, value] : config.edgeProperties) {
		VectorizedPropertyPredicate predicate;
		predicate.propertyKey = key;
		predicate.op = VectorPredicateOp::VPO_EQ;
		predicate.value = value;
		predicates.push_back(std::move(predicate));
	}
	return predicates;
}

std::vector<VectorizedPropertyPredicate> equalityIndexPredicates(
		const DirectRelationshipCountConfig &config,
		const std::shared_ptr<indexes::IndexManager> &indexManager) {
	std::vector<VectorizedPropertyPredicate> predicates;
	for (const auto &predicate : effectivePredicates(config)) {
		if (predicate.op == VectorPredicateOp::VPO_EQ &&
		    indexManager->hasPropertyIndex("edge", predicate.propertyKey)) {
			predicates.push_back(predicate);
		}
	}
	std::sort(predicates.begin(), predicates.end(), [](const auto &left, const auto &right) {
		return left.propertyKey < right.propertyKey;
	});
	return predicates;
}

RelationshipAccessPathEstimate makeEstimate(std::optional<int64_t> cardinality,
                                             double baseCost,
                                             bool exactCardinality,
                                             std::string source) {
	RelationshipAccessPathEstimate estimate;
	estimate.cardinality = cardinality;
	estimate.cost = baseCost + static_cast<double>(cardinality.value_or(0));
	estimate.exactCardinality = exactCardinality;
	estimate.source = std::move(source);
	return estimate;
}

RelationshipAccessPathCandidate makeColumnarCandidate(const DirectRelationshipCountConfig &config) {
	RelationshipAccessPathCandidate candidate;
	candidate.config = config;
	candidate.kind = RelationshipAccessPathKind::RAK_COLUMNAR_SCAN;
	const bool hasPropertyFilters = !effectivePredicates(config).empty();
	candidate.reason = hasPropertyFilters ? "columnar_scan" : "columnar_metadata_count";
	candidate.valid = config.enabled;
	candidate.directCandidateLookup = false;
	candidate.typeSatisfied = true;
	candidate.estimate = makeEstimate(
			std::nullopt,
			hasPropertyFilters ? kColumnarScanCost : kColumnarMetadataCountCost,
			false,
			"heuristic");
	return candidate;
}

RelationshipAccessPathCandidate makeTypeCandidate(const DirectRelationshipCountConfig &config,
                                                  int64_t count) {
	RelationshipAccessPathCandidate candidate;
	candidate.config = config;
	candidate.kind = RelationshipAccessPathKind::RAK_TYPE_INDEX;
	candidate.reason = "type_index";
	candidate.valid = config.enabled;
	candidate.directCandidateLookup = true;
	candidate.typeSatisfied = true;
	candidate.estimate = makeEstimate(count, kTypeIndexCost, true, "index_count");
	return candidate;
}

RelationshipAccessPathCandidate makePropertyCandidate(const DirectRelationshipCountConfig &config,
                                                      const VectorizedPropertyPredicate &predicate,
                                                      int64_t count) {
	RelationshipAccessPathCandidate candidate;
	candidate.config = config;
	candidate.kind = RelationshipAccessPathKind::RAK_PROPERTY_INDEX;
	candidate.reason = "property_index";
	candidate.valid = config.enabled;
	candidate.directCandidateLookup = true;
	candidate.typeSatisfied = config.edgeType.empty();
	candidate.propertyKeysSatisfied.push_back(predicate.propertyKey);
	candidate.estimate = makeEstimate(count, kPropertyIndexCost, true, "index_count");
	return candidate;
}

RelationshipAccessPathCandidate makeIntersectionCandidate(const DirectRelationshipCountConfig &config,
                                                          const VectorizedPropertyPredicate &predicate,
                                                          int64_t typeCount,
                                                          int64_t propertyCount) {
	RelationshipAccessPathCandidate candidate;
	candidate.config = config;
	candidate.kind = RelationshipAccessPathKind::RAK_TYPE_PROPERTY_INTERSECTION;
	candidate.reason = "type_property_intersection";
	candidate.valid = config.enabled;
	candidate.directCandidateLookup = true;
	candidate.typeSatisfied = true;
	candidate.propertyKeysSatisfied.push_back(predicate.propertyKey);
	candidate.estimate = makeEstimate(
			std::min(typeCount, propertyCount),
			kIntersectionIndexCost,
			false,
			"index_count_upper_bound");
	return candidate;
}

bool isBetterCandidate(const RelationshipAccessPathCandidate &left,
                       const RelationshipAccessPathCandidate &right) {
	auto key = [](const RelationshipAccessPathCandidate &candidate) {
		return std::tuple{
				!candidate.valid,
				candidate.estimate.cost,
				!candidate.directCandidateLookup,
				static_cast<int>(candidate.kind),
				candidate.reason};
	};
	return key(left) < key(right);
}

} // namespace

const char *relationshipAccessPathKindName(RelationshipAccessPathKind kind) {
	switch (kind) {
		case RelationshipAccessPathKind::RAK_COLUMNAR_SCAN:
			return "columnar_scan";
		case RelationshipAccessPathKind::RAK_TYPE_INDEX:
			return "type_index";
		case RelationshipAccessPathKind::RAK_PROPERTY_INDEX:
			return "property_index";
		case RelationshipAccessPathKind::RAK_TYPE_PROPERTY_INTERSECTION:
			return "type_property_intersection";
	}
	return "unknown";
}

AccessPathSummary summarizeRelationshipAccessPath(const RelationshipAccessPathCandidate &candidate) {
	AccessPathSummary summary;
	summary.kind = relationshipAccessPathKindName(candidate.kind);
	summary.reason = candidate.reason;
	summary.estimatedCardinality = candidate.estimate.cardinality;
	summary.estimatedCost = candidate.estimate.cost;
	summary.exactCardinality = candidate.estimate.exactCardinality;
	summary.estimateSource = candidate.estimate.source;
	summary.directCandidateLookup = candidate.directCandidateLookup;
	summary.valid = candidate.valid;
	return summary;
}

execution::DirectRelationshipCandidateSourceConfig relationshipCandidateSourceForAccessPath(
		const RelationshipAccessPathCandidate &candidate) {
	execution::DirectRelationshipCandidateSourceConfig source;
	if (!candidate.directCandidateLookup || !candidate.valid) {
		return source;
	}

	source.propertyKeys = candidate.propertyKeysSatisfied;
	switch (candidate.kind) {
		case RelationshipAccessPathKind::RAK_TYPE_INDEX:
			source.type = execution::DirectRelationshipCandidateSourceType::DRCS_TYPE_INDEX;
			break;
		case RelationshipAccessPathKind::RAK_PROPERTY_INDEX:
			source.type = execution::DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX;
			break;
		case RelationshipAccessPathKind::RAK_TYPE_PROPERTY_INTERSECTION:
			source.type = execution::DirectRelationshipCandidateSourceType::DRCS_TYPE_PROPERTY_INTERSECTION;
			break;
		case RelationshipAccessPathKind::RAK_COLUMNAR_SCAN:
			source.type = execution::DirectRelationshipCandidateSourceType::DRCS_AUTO;
			source.propertyKeys.clear();
			break;
	}
	return source;
}

RelationshipAccessPathDecision chooseRelationshipAccessPathDecision(
		const DirectRelationshipCountConfig &config,
		const std::shared_ptr<indexes::IndexManager> &indexManager) {
	RelationshipAccessPathDecision decision;
	decision.candidates.push_back(makeColumnarCandidate(config));

	if (indexManager && config.enabled) {
		const bool hasTypeIndex = !config.edgeType.empty() && indexManager->hasLabelIndex("edge");
		const auto typeCount = hasTypeIndex
				? std::optional<int64_t>(static_cast<int64_t>(indexManager->estimateEdgeIdsByType(config.edgeType)))
				: std::nullopt;
		if (typeCount.has_value()) {
			decision.candidates.push_back(makeTypeCandidate(config, *typeCount));
		}

		for (const auto &predicate : equalityIndexPredicates(config, indexManager)) {
			const int64_t propertyCount = static_cast<int64_t>(
					indexManager->estimateEdgeIdsByProperty(predicate.propertyKey, predicate.value));
			decision.candidates.push_back(makePropertyCandidate(config, predicate, propertyCount));
			if (typeCount.has_value()) {
				decision.candidates.push_back(makeIntersectionCandidate(config, predicate, *typeCount, propertyCount));
			}
		}
	}

	decision.selected = *std::min_element(decision.candidates.begin(), decision.candidates.end(), isBetterCandidate);
	return decision;
}

} // namespace graph::query::planner
