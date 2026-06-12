#include "graph/query/execution/NodeColumnarPredicateCounter.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
#include "graph/query/execution/PropertyPredicateScanKernel.hpp"

namespace graph::query::execution {
namespace {
	using Clock = std::chrono::steady_clock;

	uint64_t elapsedNs(Clock::time_point start) {
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
	}

	void appendRowsMissingFromBulkMatch(const std::vector<size_t> &externalRows,
	                                  std::vector<size_t> loadedRows,
	                                  std::vector<size_t> &fallbackRows) {
		if (loadedRows.size() == externalRows.size()) {
			return;
		}
		std::sort(loadedRows.begin(), loadedRows.end());
		loadedRows.erase(std::unique(loadedRows.begin(), loadedRows.end()), loadedRows.end());
		for (const size_t row : externalRows) {
			if (!std::binary_search(loadedRows.begin(), loadedRows.end(), row)) {
				fallbackRows.push_back(row);
			}
		}
	}

	bool isCompleteFullNodeCandidateSet(const storage::DataManager &dm,
	                                    const std::vector<int64_t> &candidateIds,
	                                    const NodeScanConfig &config,
	                                    const NodeScanRequirements &requirements) {
		if (config.type != ScanType::FULL_SCAN || !config.labels.empty() || requirements.needsLabels ||
		    candidateIds.empty()) {
			return false;
		}
		const auto allocator = dm.getIdAllocator(EntityType::Node);
		if (!allocator) {
			return false;
		}
		const int64_t maxId = allocator->getCurrentMaxId();
		return maxId > 0 && candidateIds.front() == 1 && candidateIds.back() == maxId &&
		       candidateIds.size() == static_cast<size_t>(maxId) &&
		       std::is_sorted(candidateIds.begin(), candidateIds.end());
	}

} // namespace

	NodeColumnarPredicateCounter::NodeColumnarPredicateCounter(std::shared_ptr<storage::DataManager> dm,
	                                                       concurrent::ThreadPool *threadPool)
		: dm_(std::move(dm)), threadPool_(threadPool) {}

	NodeColumnarPredicateCountResult NodeColumnarPredicateCounter::count(
			const std::vector<int64_t> &candidateIds,
			const NodeCandidateSet &candidateSet,
			const NodeScanConfig &config,
			const NodeScanRequirements &inputRequirements,
			const std::vector<VectorizedPropertyPredicate> &predicates) const {
		NodeColumnarPredicateCountResult result;
		if (!dm_ || candidateIds.empty() || predicates.empty()) {
			return result;
		}

		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto traceStart = traceEnabled ? Clock::now() : Clock::time_point{};
		const PropertyPredicateScanKernel scanKernel(dm_, predicates, threadPool_);
		const NodeScanRequirements requirements = relaxSatisfiedCandidateChecks(inputRequirements, candidateSet);
		NodeMetadataColumnLoader metadataLoader(dm_);
		NodePropertyCountCandidateOptions countOptions;
		countOptions.collectFallbackRefs = false;

		static constexpr size_t kColumnarCountBatchSize = 65536;
		std::vector<size_t> propertyFallbackRows;

		const bool completeFullScan = isCompleteFullNodeCandidateSet(*dm_, candidateIds, config, requirements);
		if (completeFullScan) {
			if (auto ownerCount = scanKernel.countAllOwnerProperties(EntityType::Node)) {
				result.count = static_cast<int64_t>(ownerCount->matchedCount);
				result.available = true;
				if (traceEnabled) {
					debug::PerfTrace::addDuration("node_scan.predicate_count", elapsedNs(traceStart));
				}
				return result;
			}
		}

		if (completeFullScan) {
			if (auto fullScanCandidates =
					metadataLoader.collectFullScanPropertyCountCandidates(config, requirements, threadPool_, countOptions)) {
				if (!fullScanCandidates->propertyEntityIds.empty()) {
					auto predicateCount = scanKernel.countPropertyEntityMatches(fullScanCandidates->propertyEntityIds);
					if (predicateCount.loadedCount != fullScanCandidates->propertyRowCount()) {
						auto detailedCandidates =
								metadataLoader.collectFullScanPropertyCountCandidates(config, requirements, threadPool_);
						if (!detailedCandidates.has_value()) { // ZYX_COV_EXCL_LINE
							return NodeColumnarPredicateCountResult{}; // ZYX_COV_EXCL_LINE
						}
						storage::PropertyEntityPredicateMatchOptions matchOptions;
						matchOptions.collectLoadedRows = true;
						matchOptions.collectMatchedRows = false;
						auto predicateResult = scanKernel.matchPropertyEntities(
								detailedCandidates->propertyEntityIds, detailedCandidates->propertyRows,
								detailedCandidates->propertyRowCount(), matchOptions);
						appendRowsMissingFromBulkMatch(
								detailedCandidates->propertyRows, std::move(predicateResult.loadedRows), propertyFallbackRows);
						predicateCount.loadedCount = predicateResult.loadedCount;
						predicateCount.matchedCount = predicateResult.matchedCount;
						fullScanCandidates = std::move(detailedCandidates);
					}
					result.count += static_cast<int64_t>(predicateCount.matchedCount);
				}
				if (!propertyFallbackRows.empty()) {
					std::sort(propertyFallbackRows.begin(), propertyFallbackRows.end());
					propertyFallbackRows.erase(std::unique(propertyFallbackRows.begin(), propertyFallbackRows.end()),
					                           propertyFallbackRows.end());
					for (const size_t row : propertyFallbackRows) {
						if (row >= fullScanCandidates->propertyNodeIds.size() ||
							row >= fullScanCandidates->propertyEntityIds.size()) { // ZYX_COV_EXCL_LINE
							continue;
						}
						Node node;
						auto &metadata = node.getMutableMetadata();
						metadata.id = fullScanCandidates->propertyNodeIds[row];
						metadata.propertyEntityId = fullScanCandidates->propertyEntityIds[row];
						metadata.propertyStorageType = static_cast<uint32_t>(PropertyStorageType::PROPERTY_ENTITY);
						metadata.isActive = true;
						if (scanKernel.matchesMap(dm_->getNodePropertiesDirect(node))) {
							++result.count;
						}
					}
				}
				if (!fullScanCandidates->blobRefs.empty()) {
					for (const auto &fallback : fullScanCandidates->blobRefs) {
						if (scanKernel.matchesMap(dm_->getNodePropertiesDirect(fallback.toNode()))) {
							++result.count;
						}
					}
				}
				result.available = true;
				if (traceEnabled) {
					debug::PerfTrace::addDuration("node_scan.predicate_count", elapsedNs(traceStart));
				}
				return result;
			}
		}

		for (size_t begin = 0; begin < candidateIds.size();) {
			const size_t end = std::min(candidateIds.size(), begin + kColumnarCountBatchSize);
			propertyFallbackRows.clear();

			auto candidates = metadataLoader.collectPropertyCountCandidates(
					candidateIds, begin, end, config, requirements, threadPool_, countOptions);
			if (!candidates.has_value()) {
				return NodeColumnarPredicateCountResult{};
			}

			if (!candidates->propertyEntityIds.empty()) {
				auto predicateCount = scanKernel.countPropertyEntityMatches(candidates->propertyEntityIds);
				if (predicateCount.loadedCount != candidates->propertyRowCount()) {
					// The common persisted-property path only needs counts. Collect row ids lazily
					// when a sparse/corrupt property entity forces row-level fallback.
					auto detailedCandidates = metadataLoader.collectPropertyCountCandidates(
							candidateIds, begin, end, config, requirements, threadPool_);
					if (!detailedCandidates.has_value()) { // ZYX_COV_EXCL_LINE
						return NodeColumnarPredicateCountResult{}; // ZYX_COV_EXCL_LINE
					}
					storage::PropertyEntityPredicateMatchOptions matchOptions;
					matchOptions.collectLoadedRows = true;
					matchOptions.collectMatchedRows = false;
					auto predicateResult = scanKernel.matchPropertyEntities(
							detailedCandidates->propertyEntityIds, detailedCandidates->propertyRows,
							detailedCandidates->propertyRowCount(), matchOptions);
					appendRowsMissingFromBulkMatch(
						detailedCandidates->propertyRows, std::move(predicateResult.loadedRows), propertyFallbackRows);
					predicateCount.loadedCount = predicateResult.loadedCount;
					predicateCount.matchedCount = predicateResult.matchedCount;
					candidates = std::move(detailedCandidates);
				}
				result.count += static_cast<int64_t>(predicateCount.matchedCount);
			}

			if (!propertyFallbackRows.empty()) {
				std::sort(propertyFallbackRows.begin(), propertyFallbackRows.end());
				propertyFallbackRows.erase(std::unique(propertyFallbackRows.begin(), propertyFallbackRows.end()),
				                           propertyFallbackRows.end());
				for (const size_t row : propertyFallbackRows) {
					if (row >= candidates->propertyNodeIds.size() || row >= candidates->propertyEntityIds.size()) { // ZYX_COV_EXCL_LINE
						continue;
					}
					Node node;
					auto &metadata = node.getMutableMetadata();
					metadata.id = candidates->propertyNodeIds[row];
					metadata.propertyEntityId = candidates->propertyEntityIds[row];
					metadata.propertyStorageType = static_cast<uint32_t>(PropertyStorageType::PROPERTY_ENTITY);
					metadata.isActive = true;
					if (scanKernel.matchesMap(dm_->getNodePropertiesDirect(node))) {
						++result.count;
					}
				}
			}

			if (!candidates->blobRefs.empty()) {
				for (const auto &fallback : candidates->blobRefs) {
					if (scanKernel.matchesMap(dm_->getNodePropertiesDirect(fallback.toNode()))) {
						++result.count;
					}
				}
			}

			begin = end;
		}

		result.available = true;
		if (traceEnabled) {
			debug::PerfTrace::addDuration("node_scan.predicate_count", elapsedNs(traceStart));
		}
		return result;
	}

} // namespace graph::query::execution
