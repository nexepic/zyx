#include "graph/query/execution/RelationshipColumnarCountKernel.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/PropertyPredicateKernel.hpp"
#include "graph/query/execution/RelationshipMetadataColumnLoader.hpp"

namespace graph::query::execution {
	namespace {
		using Clock = std::chrono::steady_clock;

		uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}

		void addProfile(const char *phase, Clock::time_point start) {
			if (debug::PerfTrace::isEnabled()) {
				debug::PerfTrace::addDuration(phase, elapsedNs(start));
			}
		}

		PropertyPredicateKernel makePredicateKernel(const RelationshipColumnarCountRequest &request) {
			if (!request.vectorPredicates.empty()) {
				return PropertyPredicateKernel(request.vectorPredicates);
			}
			return PropertyPredicateKernel::fromEqualityPredicates(request.propertyPredicates);
		}

		bool areEqualityPredicates(const std::vector<VectorizedPropertyPredicate> &predicates) {
			return std::all_of(predicates.begin(), predicates.end(), [](const auto &predicate) {
				return predicate.op == VectorPredicateOp::VPO_EQ;
			});
		}

		std::unordered_map<std::string, PropertyValue>
		toEqualityMap(const std::vector<VectorizedPropertyPredicate> &predicates) {
			std::unordered_map<std::string, PropertyValue> equality;
			equality.reserve(predicates.size());
			for (const auto &predicate : predicates) {
				equality[predicate.propertyKey] = predicate.value;
			}
			return equality;
		}

		size_t countPropertyEntityPredicates(const std::shared_ptr<storage::DataManager> &dm,
		                                    const std::vector<int64_t> &propertyEntityIds,
		                                    const PropertyPredicateKernel &predicateKernel,
		                                    concurrent::ThreadPool *pool) {
			const auto &predicates = predicateKernel.predicates();
			if (predicates.empty()) {
				return 0;
			}
			if (areEqualityPredicates(predicates)) {
				return dm->bulkCountPropertyEntityPredicates(propertyEntityIds, toEqualityMap(predicates), pool);
			}

			std::vector<size_t> rows;
			rows.reserve(propertyEntityIds.size());
			for (size_t row = 0; row < propertyEntityIds.size(); ++row) {
				rows.push_back(row);
			}
			return dm->bulkMatchPropertyEntityPredicateSpecs(
					propertyEntityIds,
					rows,
					propertyEntityIds.size(),
					predicateKernel.toStoragePredicates(),
					pool).matchedCount;
		}
	} // namespace

	RelationshipColumnarCountKernel::RelationshipColumnarCountKernel(std::shared_ptr<storage::DataManager> dm,
																	 concurrent::ThreadPool *pool) :
		dm_(std::move(dm)), pool_(pool) {}

	std::optional<RelationshipColumnarCountResult>
	RelationshipColumnarCountKernel::count(const RelationshipColumnarCountRequest &request) const {
		if (!dm_) {
			return std::nullopt;
		}

		RelationshipMetadataColumnLoader metadataLoader(dm_);
		RelationshipColumnarCountResult result;
		const auto predicateKernel = makePredicateKernel(request);
		if (predicateKernel.empty()) {
			auto count = metadataLoader.countActiveByType(request.beginId, request.endId, request.typeId);
			if (!count.has_value()) {
				return std::nullopt;
			}
			result.count = *count;
			return result;
		}

		auto candidates =
				metadataLoader.collectPropertyCountCandidatesByType(request.beginId, request.endId, request.typeId);
		if (!candidates.has_value()) {
			return std::nullopt;
		}

		result.propertyCandidates = candidates->propertyEntityIds.size();
		result.fallbackEdges = candidates->fallbackEdgeIds.size();

		const auto propertyStart = Clock::now();
		result.count += static_cast<int64_t>(
				countPropertyEntityPredicates(dm_, candidates->propertyEntityIds, predicateKernel, pool_));
		addProfile("relationship_count.property_predicate", propertyStart);

		if (!candidates->fallbackEdgeIds.empty()) {
			const auto fallbackStart = Clock::now();
			for (const int64_t edgeId: candidates->fallbackEdgeIds) {
				result.count += propertyMapMatches(dm_->getEdgeProperties(edgeId), predicateKernel)
										? int64_t{1}
										: int64_t{0};
			}
			addProfile("relationship_count.property_fallback", fallbackStart);
		}
		return result;
	}

	bool RelationshipColumnarCountKernel::propertyMapMatches(
			const std::unordered_map<std::string, PropertyValue> &properties,
			const PropertyPredicateKernel &predicateKernel) const {
		return predicateKernel.matchesMap(properties);
	}

} // namespace graph::query::execution
