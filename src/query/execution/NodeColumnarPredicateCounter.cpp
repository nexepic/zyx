#include "graph/query/execution/NodeColumnarPredicateCounter.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeMetadataFilter.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
#include "graph/query/execution/PropertyPredicateKernel.hpp"

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
		const PropertyPredicateKernel predicateKernel(predicates);
		const auto storagePredicates = predicateKernel.toStoragePredicates();
		const NodeScanRequirements requirements = relaxSatisfiedCandidateChecks(inputRequirements, candidateSet);
		const NodeMetadataRowFilter rowFilter(dm_, config, requirements);
		NodeMetadataColumnLoader metadataLoader(dm_);

		static constexpr size_t kColumnarCountBatchSize = 65536;
		for (size_t begin = 0; begin < candidateIds.size();) {
			const size_t end = std::min(candidateIds.size(), begin + kColumnarCountBatchSize);

			std::vector<int64_t> propertyEntityIds;
			std::vector<size_t> propertyRows;
			std::vector<NodeMetadataRow> propertyRowMetadata;
			std::vector<size_t> propertyFallbackRows;
			std::vector<NodeMetadataRow> fallbackRows;
			propertyEntityIds.reserve(end - begin);
			propertyRows.reserve(end - begin);
			propertyRowMetadata.reserve(end - begin);

			const bool visited = metadataLoader.visitBatch(candidateIds, begin, end, [&](size_t, const NodeMetadataRow &metadata) {
				if (!rowFilter.accepts(metadata)) {
					return true;
				}

				const int64_t propertyEntityId = metadata.propertyEntityId;
				const auto storageType = metadata.propertyStorageType;
				if (storageType == PropertyStorageType::PROPERTY_ENTITY && propertyEntityId != 0) {
					const size_t propertyRow = propertyRowMetadata.size();
					propertyEntityIds.push_back(propertyEntityId);
					propertyRows.push_back(propertyRow);
					propertyRowMetadata.push_back(metadata);
				} else if (storageType == PropertyStorageType::BLOB_ENTITY && propertyEntityId != 0) {
					fallbackRows.push_back(metadata);
				}
				return true;
			});
			if (!visited) {
				return NodeColumnarPredicateCountResult{};
			}

			if (!propertyEntityIds.empty()) {
				storage::PropertyEntityPredicateMatchOptions matchOptions;
				matchOptions.collectLoadedRows = true;
				matchOptions.collectMatchedRows = false;
				auto predicateResult = dm_->bulkMatchPropertyEntityPredicateSpecs(
					propertyEntityIds, propertyRows, propertyRowMetadata.size(), storagePredicates, threadPool_, matchOptions);
				result.count += static_cast<int64_t>(predicateResult.matchedCount);
				appendRowsMissingFromBulkMatch(
					propertyRows, std::move(predicateResult.loadedRows), propertyFallbackRows);
			}

			if (!propertyFallbackRows.empty()) {
				std::sort(propertyFallbackRows.begin(), propertyFallbackRows.end());
				propertyFallbackRows.erase(std::unique(propertyFallbackRows.begin(), propertyFallbackRows.end()),
				                           propertyFallbackRows.end());
				for (const size_t row : propertyFallbackRows) {
					if (row >= propertyRowMetadata.size()) { // ZYX_COV_EXCL_LINE
						continue;
					}
					Node node = propertyRowMetadata[row].toNode();
					if (predicateKernel.matchesMap(dm_->getNodePropertiesDirect(node))) {
						++result.count;
					}
				}
			}

			if (!fallbackRows.empty()) {
				for (const auto &fallback : fallbackRows) {
					Node node = fallback.toNode();
					if (predicateKernel.matchesMap(dm_->getNodePropertiesDirect(node))) {
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
