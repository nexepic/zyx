#include "graph/query/execution/RelationshipColumnarCountKernel.hpp"

#include <chrono>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/RelationshipMetadataColumnLoader.hpp"
#include "graph/storage/IDAllocator.hpp"

namespace graph::query::execution {
namespace {
	using Clock = std::chrono::steady_clock;

	uint64_t elapsedNs(Clock::time_point start) {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
	}

	void addProfile(const char *phase, Clock::time_point start) {
		if (debug::PerfTrace::isEnabled()) {
			debug::PerfTrace::addDuration(phase, elapsedNs(start));
		}
	}
} // namespace

	RelationshipColumnarCountKernel::RelationshipColumnarCountKernel(
			std::shared_ptr<storage::DataManager> dm,
			concurrent::ThreadPool *pool)
		: dm_(std::move(dm)), pool_(pool) {}

	std::optional<RelationshipColumnarCountResult>
	RelationshipColumnarCountKernel::count(const RelationshipColumnarCountRequest &request) const {
		if (!dm_) {
			return std::nullopt;
		}

		RelationshipMetadataColumnLoader metadataLoader(dm_);
		RelationshipColumnarCountResult result;
		const bool coversAllEdges =
			request.beginId == 1 &&
			request.endId == dm_->getIdAllocator(EntityType::Edge)->getCurrentMaxId();
		if (request.propertyPredicates.empty()) {
			if (coversAllEdges) {
				const auto cacheStart = Clock::now();
				if (auto cached = dm_->getCachedActiveEdgeCountByType(request.typeId)) {
					result.count = *cached;
					addProfile("relationship_count.type_cache", cacheStart);
					return result;
				}
			}
			auto count = metadataLoader.countActiveByType(request.beginId, request.endId, request.typeId);
			if (!count.has_value()) {
				return std::nullopt;
			}
			result.count = *count;
			if (coversAllEdges) {
				dm_->cacheActiveEdgeCountByType(request.typeId, result.count);
			}
			return result;
		}

		if (coversAllEdges) {
			const auto cacheStart = Clock::now();
			if (auto cached = dm_->getCachedActiveEdgeCountByTypeAndProperties(
					request.typeId, request.propertyPredicates)) {
				result.count = *cached;
				addProfile("relationship_count.property_cache", cacheStart);
				return result;
			}
		}

		auto candidates = metadataLoader.collectPropertyCountCandidatesByType(
			request.beginId, request.endId, request.typeId);
		if (!candidates.has_value()) {
			return std::nullopt;
		}

		result.propertyCandidates = candidates->propertyEntityIds.size();
		result.fallbackEdges = candidates->fallbackEdgeIds.size();

		const auto propertyStart = Clock::now();
		result.count += static_cast<int64_t>(dm_->bulkCountPropertyEntityPredicates(
			candidates->propertyEntityIds, request.propertyPredicates, pool_));
		addProfile("relationship_count.property_predicate", propertyStart);

		if (!candidates->fallbackEdgeIds.empty()) {
			const auto fallbackStart = Clock::now();
			for (const int64_t edgeId : candidates->fallbackEdgeIds) {
				result.count += propertyMapMatches(dm_->getEdgeProperties(edgeId), request.propertyPredicates)
					? int64_t{1}
					: int64_t{0};
			}
			addProfile("relationship_count.property_fallback", fallbackStart);
		}

		if (coversAllEdges) {
			dm_->cacheActiveEdgeCountByTypeAndProperties(
				request.typeId, request.propertyPredicates, result.count);
		}
		return result;
	}

	bool RelationshipColumnarCountKernel::propertyMapMatches(
			const std::unordered_map<std::string, PropertyValue> &properties,
			const std::unordered_map<std::string, PropertyValue> &expected) const {
		for (const auto &[key, value] : expected) {
			const auto it = properties.find(key);
			if (it == properties.end() || it->second != value) {
				return false;
			}
		}
		return true;
	}

} // namespace graph::query::execution
