#include "graph/query/execution/RelationshipColumnarCountKernel.hpp"

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/PropertyPredicateKernel.hpp"
#include "graph/query/execution/PropertyPredicateScanKernel.hpp"
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

		PropertyPredicateScanKernel makePredicateScanKernel(
				const std::shared_ptr<storage::DataManager> &dm,
				const RelationshipColumnarCountRequest &request,
				concurrent::ThreadPool *pool) {
			if (!request.vectorPredicates.empty()) {
				return PropertyPredicateScanKernel(dm, request.vectorPredicates, pool);
			}
			return PropertyPredicateScanKernel::fromEqualityPredicates(dm, request.propertyPredicates, pool);
		}

		std::vector<size_t> sequenceRows(size_t count) {
			std::vector<size_t> rows;
			rows.reserve(count);
			for (size_t row = 0; row < count; ++row) {
				rows.push_back(row);
			}
			return rows;
		}

		void appendRowsMissingFromBulkMatch(size_t rowCount,
		                                    std::vector<size_t> loadedRows,
		                                    std::vector<size_t> &fallbackRows) {
			if (loadedRows.size() == rowCount) { // ZYX_COV_EXCL_LINE: mismatch callers normally have missing rows.
				return;
			}
			std::sort(loadedRows.begin(), loadedRows.end());
			loadedRows.erase(std::unique(loadedRows.begin(), loadedRows.end()), loadedRows.end());
			for (size_t row = 0; row < rowCount; ++row) {
				if (!std::binary_search(loadedRows.begin(), loadedRows.end(), row)) {
					fallbackRows.push_back(row);
				}
			}
		}

		constexpr int64_t kPropertyOwnerScanMinRange = 1024;
		constexpr int64_t kDirtyOverlayCountMinRange = 128;
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
		const auto scanKernel = makePredicateScanKernel(dm_, request, pool_);
		if (scanKernel.empty()) {
			auto count = metadataLoader.countActiveByType(request.beginId, request.endId, request.typeId, pool_);
			if (!count.has_value() && dm_->hasUnsavedChanges() && request.endId >= request.beginId &&
				request.endId - request.beginId + 1 >= kDirtyOverlayCountMinRange) {
				count = dm_->countActiveEdgesByTypeFromSegmentStats(request.beginId, request.endId, request.typeId);
			}
			if (!count.has_value()) {
				return std::nullopt;
			}
			result.count = *count;
			return result;
		}

		auto tryCountTypedPropertyCandidates = [&]() -> std::optional<RelationshipColumnarCountResult> {
			if (request.typeId == 0) {
				return std::nullopt;
			}

			RelationshipPropertyCountCandidateOptions countOptions;
			countOptions.collectPropertyEdgeRefs = true;
			auto candidates = metadataLoader.collectPropertyCountCandidatesByType(
					request.beginId, request.endId, request.typeId, pool_, countOptions);
			if (!candidates.has_value()) {
				return std::nullopt;
			}

			RelationshipColumnarCountResult typedResult;
			typedResult.propertyCandidates = candidates->propertyEntityIds.size();
			typedResult.fallbackEdges = candidates->fallbackEdgeIds.size();

			const auto propertyStart = Clock::now();
			auto predicateCount = scanKernel.countPropertyEntityMatches(candidates->propertyEntityIds);
			if (predicateCount.loadedCount != candidates->propertyEntityIds.size()) {
				return std::nullopt;
			}
			typedResult.count = static_cast<int64_t>(predicateCount.matchedCount);
			addProfile("relationship_count.property_predicate", propertyStart);

			if (!candidates->fallbackEdgeIds.empty()) {
				const auto fallbackStart = Clock::now();
				for (const int64_t edgeId: candidates->fallbackEdgeIds) {
					typedResult.count += propertyMapMatches(dm_->getEdgeProperties(edgeId), scanKernel)
												 ? int64_t{1}
												 : int64_t{0};
				}
				addProfile("relationship_count.property_fallback", fallbackStart);
			}
			return typedResult;
		};

		if (auto typedCandidateCount = tryCountTypedPropertyCandidates()) {
			return typedCandidateCount;
		}

		int64_t ownerScanBegin = request.beginId;
		int64_t ownerScanEnd = request.endId;
		if (ownerScanEnd >= ownerScanBegin &&
			ownerScanEnd - ownerScanBegin + 1 >= kPropertyOwnerScanMinRange) {
			storage::PropertyEntityOwnerPredicateScanOptions ownerScanOptions;
			ownerScanOptions.beginOwnerId = ownerScanBegin;
			ownerScanOptions.endOwnerId = ownerScanEnd;

			const auto ownerScanStart = Clock::now();
			auto matchingEdgeIds = scanKernel.collectAllOwnerIds(EntityType::Edge, ownerScanOptions);
			if (matchingEdgeIds.has_value()) {
				result.propertyCandidates = matchingEdgeIds->size();
				addProfile("relationship_count.property_owner_scan", ownerScanStart);
				if (request.typeId == 0) {
					result.count = static_cast<int64_t>(matchingEdgeIds->size());
					return result;
				}

				const auto typeFilterStart = Clock::now();
				auto typedCount = dm_->countActivePersistedEdgeIdsByType(*matchingEdgeIds, request.typeId, pool_);
				if (typedCount.has_value()) {
					result.count = *typedCount;
					addProfile("relationship_count.property_owner_type_filter", typeFilterStart);
					return result;
				}
			}
		}

		RelationshipPropertyCountCandidateOptions countOptions;
		countOptions.collectPropertyEdgeRefs = false;
		auto candidates =
				metadataLoader.collectPropertyCountCandidatesByType(
						request.beginId, request.endId, request.typeId, pool_, countOptions);
		if (!candidates.has_value()) {
			return std::nullopt;
		}

		result.propertyCandidates = candidates->propertyEntityIds.size();
		result.fallbackEdges = candidates->fallbackEdgeIds.size();

		const auto propertyStart = Clock::now();
		auto predicateCount = scanKernel.countPropertyEntityMatches(candidates->propertyEntityIds);
		std::vector<size_t> propertyFallbackRows;
		if (predicateCount.loadedCount != candidates->propertyEntityIds.size()) {
			auto detailedCandidates =
					metadataLoader.collectPropertyCountCandidatesByType(request.beginId, request.endId, request.typeId, pool_);
			if (!detailedCandidates.has_value()) { // ZYX_COV_EXCL_LINE
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}
			storage::PropertyEntityPredicateMatchOptions matchOptions;
			matchOptions.collectLoadedRows = true;
			matchOptions.collectMatchedRows = false;
			auto rows = sequenceRows(detailedCandidates->propertyEntityIds.size());
			auto predicateResult = scanKernel.matchPropertyEntities(
					detailedCandidates->propertyEntityIds, rows, rows.size(), matchOptions);
			appendRowsMissingFromBulkMatch(rows.size(), std::move(predicateResult.loadedRows), propertyFallbackRows);
			predicateCount.loadedCount = predicateResult.loadedCount;
			predicateCount.matchedCount = predicateResult.matchedCount;
			candidates = std::move(detailedCandidates);
		}
		result.count += static_cast<int64_t>(predicateCount.matchedCount);
		addProfile("relationship_count.property_predicate", propertyStart);

		if (!propertyFallbackRows.empty()) {
			const auto fallbackStart = Clock::now();
			for (const size_t row: propertyFallbackRows) {
				if (row >= candidates->propertyEdgeIds.size()) { // ZYX_COV_EXCL_LINE
					continue; // ZYX_COV_EXCL_LINE
				}
				result.count += propertyMapMatches(dm_->getEdgeProperties(candidates->propertyEdgeIds[row]), scanKernel)
										? int64_t{1}
										: int64_t{0};
			}
			addProfile("relationship_count.property_fallback", fallbackStart);
		}

		if (!candidates->fallbackEdgeIds.empty()) {
			const auto fallbackStart = Clock::now();
			for (const int64_t edgeId: candidates->fallbackEdgeIds) {
				result.count += propertyMapMatches(dm_->getEdgeProperties(edgeId), scanKernel)
										? int64_t{1}
										: int64_t{0};
			}
			addProfile("relationship_count.property_fallback", fallbackStart);
		}
		return result;
	}

	bool RelationshipColumnarCountKernel::propertyMapMatches(
			const std::unordered_map<std::string, PropertyValue> &properties,
			const PropertyPredicateScanKernel &scanKernel) const {
		return scanKernel.matchesMap(properties);
	}

} // namespace graph::query::execution
