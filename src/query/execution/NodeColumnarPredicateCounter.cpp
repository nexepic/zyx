#include "graph/query/execution/NodeColumnarPredicateCounter.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
#include "graph/query/execution/PropertyPredicateKernel.hpp"

namespace graph::query::execution {
namespace {
	using Clock = std::chrono::steady_clock;

	uint64_t elapsedNs(Clock::time_point start) {
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
	}

	std::vector<int64_t> resolveLabelIds(const std::shared_ptr<storage::DataManager> &dm,
	                                    const NodeScanConfig &config,
	                                    const NodeScanRequirements &requirements) {
		std::vector<int64_t> labelIds;
		if (!requirements.needsLabels) {
			return labelIds;
		}
		labelIds.reserve(config.labels.size());
		for (const auto &label : config.labels) {
			const int64_t labelId = dm->resolveTokenId(label);
			labelIds.push_back(labelId == 0 ? -1 : labelId);
		}
		return labelIds;
	}

	bool rowMatchesLabels(const NodeMetadataBatch &batch, size_t row, const std::vector<int64_t> &labelIds) {
		for (const int64_t labelId : labelIds) {
			if (!batch.hasLabelId(row, labelId)) {
				return false;
			}
		}
		return true;
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
		const auto labelIds = resolveLabelIds(dm_, config, requirements);
		NodeMetadataColumnLoader metadataLoader(dm_);

		static constexpr size_t kColumnarCountBatchSize = 65536;
		for (size_t begin = 0; begin < candidateIds.size();) {
			const size_t end = std::min(candidateIds.size(), begin + kColumnarCountBatchSize);
			auto metadata = metadataLoader.loadBatch(candidateIds, begin, end);
			if (!metadata.has_value()) {
				return NodeColumnarPredicateCountResult{};
			}

			std::vector<int64_t> propertyEntityIds;
			std::vector<size_t> propertyRows;
			std::vector<size_t> fallbackRows;
			propertyEntityIds.reserve(metadata->size());
			propertyRows.reserve(metadata->size());

			for (size_t row = 0; row < metadata->size(); ++row) {
				if (!metadata->isValid(row)) {
					continue;
				}
				if (requirements.needsActiveCheck && metadata->active[row] == 0) { // ZYX_COV_EXCL_LINE
					continue;
				}
				if (!labelIds.empty() && !rowMatchesLabels(*metadata, row, labelIds)) {
					continue;
				}

				const int64_t propertyEntityId = metadata->propertyEntityIds[row];
				const auto storageType = metadata->propertyStorageTypes[row];
				if (storageType == PropertyStorageType::PROPERTY_ENTITY && propertyEntityId != 0) {
					propertyEntityIds.push_back(propertyEntityId);
					propertyRows.push_back(row);
				} else if (storageType == PropertyStorageType::BLOB_ENTITY && propertyEntityId != 0) {
					fallbackRows.push_back(row);
				}
			}

			if (!propertyEntityIds.empty()) {
				auto predicateResult = dm_->bulkMatchPropertyEntityPredicateSpecs(
					propertyEntityIds, propertyRows, metadata->size(), storagePredicates, threadPool_);
				result.count += static_cast<int64_t>(predicateResult.matchedRows.size());
				appendRowsMissingFromBulkMatch(propertyRows, std::move(predicateResult.loadedRows), fallbackRows);
			}

			if (!fallbackRows.empty()) {
				std::sort(fallbackRows.begin(), fallbackRows.end());
				fallbackRows.erase(std::unique(fallbackRows.begin(), fallbackRows.end()), fallbackRows.end());
				for (const size_t row : fallbackRows) {
					Node node = metadata->toNode(row);
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
