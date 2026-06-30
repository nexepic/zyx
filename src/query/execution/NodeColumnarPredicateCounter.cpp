#include "graph/query/execution/NodeColumnarPredicateCounter.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
#include "graph/query/execution/PropertyPredicateScanKernel.hpp"
#include "src/query/execution/NodeColumnarPredicateCounterDetail.hpp"

namespace graph::query::execution {
namespace {
	using Clock = std::chrono::steady_clock;
	namespace detail = node_columnar_predicate_counter_detail;

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

		const bool completeFullScan = detail::isCompleteFullNodeCandidateSet(*dm_, candidateIds, config, requirements);
		if (completeFullScan) {
			if (auto ownerCount = scanKernel.countAllOwnerProperties(EntityType::Node)) {
				result.count = static_cast<int64_t>(ownerCount->matchedCount);
				result.available = true;
				detail::recordPredicateCountTrace(traceEnabled, traceStart);
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
						detail::appendRowsMissingFromBulkMatch(
								detailedCandidates->propertyRows, std::move(predicateResult.loadedRows), propertyFallbackRows);
						predicateCount.loadedCount = predicateResult.loadedCount;
						predicateCount.matchedCount = predicateResult.matchedCount;
						fullScanCandidates = std::move(detailedCandidates);
					}
					result.count += static_cast<int64_t>(predicateCount.matchedCount);
				}
				if (!propertyFallbackRows.empty()) {
					result.count += static_cast<int64_t>(detail::countPropertyEntityFallbackMatches(
							*dm_, scanKernel, std::move(propertyFallbackRows),
							fullScanCandidates->propertyNodeIds,
							fullScanCandidates->propertyEntityIds));
				}
				if (!fullScanCandidates->blobRefs.empty()) {
					result.count += static_cast<int64_t>(
							detail::countBlobFallbackMatches(*dm_, scanKernel, fullScanCandidates->blobRefs));
				}
				result.available = true;
				detail::recordPredicateCountTrace(traceEnabled, traceStart);
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
					detail::appendRowsMissingFromBulkMatch(
						detailedCandidates->propertyRows, std::move(predicateResult.loadedRows), propertyFallbackRows);
					predicateCount.loadedCount = predicateResult.loadedCount;
					predicateCount.matchedCount = predicateResult.matchedCount;
					candidates = std::move(detailedCandidates);
				}
				result.count += static_cast<int64_t>(predicateCount.matchedCount);
			}

			if (!propertyFallbackRows.empty()) {
				result.count += static_cast<int64_t>(detail::countPropertyEntityFallbackMatches(
						*dm_, scanKernel, std::move(propertyFallbackRows),
						candidates->propertyNodeIds,
						candidates->propertyEntityIds));
			}

			if (!candidates->blobRefs.empty()) {
				result.count += static_cast<int64_t>(
						detail::countBlobFallbackMatches(*dm_, scanKernel, candidates->blobRefs));
			}

			begin = end;
		}

		result.available = true;
		detail::recordPredicateCountTrace(traceEnabled, traceStart);
		return result;
	}

} // namespace graph::query::execution
