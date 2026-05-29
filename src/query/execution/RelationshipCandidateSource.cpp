#include "graph/query/execution/RelationshipCandidateSource.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>

namespace graph::query::execution {
namespace {
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
} // namespace

	RelationshipCandidateSource::RelationshipCandidateSource(std::shared_ptr<storage::DataManager> dm,
	                                                       std::shared_ptr<indexes::IndexManager> im)
		: dm_(std::move(dm)), im_(std::move(im)) {}

	RelationshipCandidateSet RelationshipCandidateSource::collect(const DirectRelationshipCountConfig &config) const {
		RelationshipCandidateSet result;
		if (!dm_ || !im_) {
			return result;
		}

		std::optional<std::vector<int64_t>> propertyIds;
		std::string propertyKey;
		for (const auto &[key, value] : config.edgeProperties) {
			if (!im_->hasPropertyIndex("edge", key)) {
				continue;
			}
			auto ids = im_->findEdgeIdsByProperty(key, value);
			normalizeIds(ids);
			if (!propertyIds.has_value() || ids.size() < propertyIds->size()) {
				propertyKey = key;
				propertyIds = std::move(ids);
			}
		}

		std::optional<std::vector<int64_t>> typeIds;
		if (!config.edgeType.empty() && im_->hasLabelIndex("edge")) {
			auto ids = im_->findEdgeIdsByType(config.edgeType);
			normalizeIds(ids);
			typeIds = std::move(ids);
		}

		if (propertyIds.has_value()) {
			result.ids = std::move(*propertyIds);
			result.propertyKeysSatisfied.push_back(std::move(propertyKey));
			result.available = true;
			result.activeOnly = true;
			result.typeSatisfied = config.edgeType.empty();
			if (typeIds.has_value()) {
				intersectSorted(result.ids, *typeIds);
				result.typeSatisfied = true;
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

} // namespace graph::query::execution
