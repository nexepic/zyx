#include "graph/query/execution/RelationshipCandidateSource.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>

namespace graph::query::execution {
namespace {
	struct IndexedRelationshipPredicate {
		VectorizedPropertyPredicate predicate;
		std::vector<int64_t> ids;
	};

	void normalizeIds(std::vector<int64_t> &ids) {
		if (ids.size() <= 1) {
			return;
		}
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	}

	void intersectSorted(std::vector<int64_t> &left, const std::vector<int64_t> &right) {
		std::vector<int64_t> intersection;
		intersection.reserve(std::min(left.size(), right.size()));
		std::set_intersection(left.begin(), left.end(), right.begin(), right.end(), std::back_inserter(intersection));
		left = std::move(intersection);
	}

	std::vector<VectorizedPropertyPredicate> effectiveEdgePredicates(const DirectRelationshipCountConfig &config) {
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

	std::vector<VectorizedPropertyPredicate> indexedEqualityPredicates(
			const DirectRelationshipCountConfig &config,
			const std::shared_ptr<indexes::IndexManager> &im) {
		std::vector<VectorizedPropertyPredicate> predicates;
		for (const auto &predicate : effectiveEdgePredicates(config)) {
			if (predicate.op == VectorPredicateOp::VPO_EQ &&
			    im->hasPropertyIndex("edge", predicate.propertyKey)) {
				predicates.push_back(predicate);
			}
		}
		std::sort(predicates.begin(), predicates.end(), [](const auto &left, const auto &right) {
			return left.propertyKey < right.propertyKey;
		});
		return predicates;
	}

	std::optional<IndexedRelationshipPredicate> loadIndexedPredicate(
			const std::shared_ptr<indexes::IndexManager> &im,
			const VectorizedPropertyPredicate &predicate) {
		if (predicate.op != VectorPredicateOp::VPO_EQ ||
			!im->hasPropertyIndex("edge", predicate.propertyKey)) {
			return std::nullopt;
		}

		IndexedRelationshipPredicate indexed;
		indexed.predicate = predicate;
		indexed.ids = im->findEdgeIdsByProperty(predicate.propertyKey, predicate.value);
		normalizeIds(indexed.ids);
		return indexed;
	}

	std::optional<IndexedRelationshipPredicate> loadIndexedPredicateByKey(
			const DirectRelationshipCountConfig &config,
			const std::shared_ptr<indexes::IndexManager> &im,
			const std::string &key) {
		for (const auto &predicate : indexedEqualityPredicates(config, im)) {
			if (predicate.propertyKey == key) {
				return loadIndexedPredicate(im, predicate);
			}
		}
		return std::nullopt;
	}

	std::optional<std::vector<int64_t>> loadTypeIds(
			const DirectRelationshipCountConfig &config,
			const std::shared_ptr<indexes::IndexManager> &im) {
		if (config.edgeType.empty() || !im->hasLabelIndex("edge")) {
			return std::nullopt;
		}
		auto ids = im->findEdgeIdsByType(config.edgeType);
		normalizeIds(ids);
		return ids;
	}

	RelationshipCandidateSet makeTypeCandidate(
			const DirectRelationshipCountConfig &config,
			const std::shared_ptr<indexes::IndexManager> &im) {
		RelationshipCandidateSet result;
		auto typeIds = loadTypeIds(config, im);
		if (!typeIds.has_value()) {
			return result;
		}

		result.ids = std::move(*typeIds);
		result.available = true;
		result.activeOnly = true;
		result.typeSatisfied = true;
		return result;
	}

	RelationshipCandidateSet makePropertyCandidate(
			const DirectRelationshipCountConfig &config,
			const std::shared_ptr<indexes::IndexManager> &im,
			const std::vector<std::string> &propertyKeys) {
		RelationshipCandidateSet result;
		if (propertyKeys.empty()) {
			return result;
		}

		auto requestedKeys = propertyKeys;
		std::sort(requestedKeys.begin(), requestedKeys.end());
		requestedKeys.erase(std::unique(requestedKeys.begin(), requestedKeys.end()), requestedKeys.end());

		std::vector<IndexedRelationshipPredicate> indexedPredicates;
		indexedPredicates.reserve(requestedKeys.size());
		for (const auto &key : requestedKeys) {
			auto indexed = loadIndexedPredicateByKey(config, im, key);
			if (!indexed.has_value()) {
				return {};
			}
			indexedPredicates.push_back(std::move(*indexed));
		}
		std::sort(indexedPredicates.begin(), indexedPredicates.end(), [](const auto &left, const auto &right) {
			if (left.ids.size() != right.ids.size()) {
				return left.ids.size() < right.ids.size();
			}
			return left.predicate.propertyKey < right.predicate.propertyKey;
		});

		result.ids = std::move(indexedPredicates.front().ids);
		result.propertyKeysSatisfied.push_back(indexedPredicates.front().predicate.propertyKey);
		for (size_t index = 1; index < indexedPredicates.size(); ++index) {
			intersectSorted(result.ids, indexedPredicates[index].ids);
			result.propertyKeysSatisfied.push_back(indexedPredicates[index].predicate.propertyKey);
			if (result.ids.empty()) {
				break;
			}
		}
		std::sort(result.propertyKeysSatisfied.begin(), result.propertyKeysSatisfied.end());
		result.available = true;
		result.activeOnly = true;
		result.typeSatisfied = config.edgeType.empty();
		return result;
	}

	void intersectWithTypeIds(RelationshipCandidateSet &result, std::vector<int64_t> typeIds) {
		if (typeIds.size() < result.ids.size()) {
			auto propertyIds = std::move(result.ids);
			result.ids = std::move(typeIds);
			intersectSorted(result.ids, propertyIds);
		} else {
			intersectSorted(result.ids, typeIds);
		}
		result.typeSatisfied = true;
	}

	RelationshipCandidateSet makeIntersectionCandidate(
			const DirectRelationshipCountConfig &config,
			const std::shared_ptr<indexes::IndexManager> &im,
			const std::vector<std::string> &propertyKeys) {
		auto result = makePropertyCandidate(config, im, propertyKeys);
		if (!result.available) {
			return {};
		}
		auto typeIds = loadTypeIds(config, im);
		if (!typeIds.has_value()) {
			return {};
		}
		intersectWithTypeIds(result, std::move(*typeIds));
		return result;
	}

	RelationshipCandidateSet collectAuto(
			const DirectRelationshipCountConfig &config,
			const std::shared_ptr<indexes::IndexManager> &im) {
		RelationshipCandidateSet result;

		std::optional<IndexedRelationshipPredicate> propertyIds;
		for (const auto &predicate : indexedEqualityPredicates(config, im)) {
			auto indexed = loadIndexedPredicate(im, predicate);
			if (indexed.has_value() &&
			    (!propertyIds.has_value() ||
			     indexed->ids.size() < propertyIds->ids.size())) {
				propertyIds = std::move(indexed);
			}
		}

		auto typeIds = loadTypeIds(config, im);

		if (propertyIds.has_value()) {
			result.ids = std::move(propertyIds->ids);
			result.propertyKeysSatisfied.push_back(propertyIds->predicate.propertyKey);
			result.available = true;
			result.activeOnly = true;
			result.typeSatisfied = config.edgeType.empty();
			if (typeIds.has_value()) {
				intersectWithTypeIds(result, std::move(*typeIds));
			}
			return result;
		}

		if (typeIds.has_value()) {
			result.ids = std::move(*typeIds);
			result.available = true;
			result.activeOnly = true;
			result.typeSatisfied = true;
		}
		return result;
	}
} // namespace

	RelationshipCandidateSource::RelationshipCandidateSource(std::shared_ptr<storage::DataManager> dm,
	                                                       std::shared_ptr<indexes::IndexManager> im)
		: dm_(std::move(dm)), im_(std::move(im)) {}

	RelationshipCandidateSet RelationshipCandidateSource::collect(const DirectRelationshipCountConfig &config) const {
		RelationshipCandidateSet result;
		if (!dm_ || !im_) {
			return result;
		}

		switch (config.candidateSource.type) {
			case DirectRelationshipCandidateSourceType::DRCS_TYPE_INDEX:
				return makeTypeCandidate(config, im_);
			case DirectRelationshipCandidateSourceType::DRCS_PROPERTY_INDEX:
				return makePropertyCandidate(config, im_, config.candidateSource.propertyKeys);
			case DirectRelationshipCandidateSourceType::DRCS_TYPE_PROPERTY_INTERSECTION:
				return makeIntersectionCandidate(config, im_, config.candidateSource.propertyKeys);
			case DirectRelationshipCandidateSourceType::DRCS_AUTO:
				return collectAuto(config, im_);
		}
		return result;
	}

} // namespace graph::query::execution
