#include "src/query/execution/NodeColumnarPredicateCounterDetail.hpp"

#include <algorithm>
#include <cstdint>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/PropertyPredicateScanKernel.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution::node_columnar_predicate_counter_detail {

void appendRowsMissingFromBulkMatch(
		const std::vector<size_t> &externalRows,
		std::vector<size_t> loadedRows,
		std::vector<size_t> &fallbackRows) {
	std::sort(loadedRows.begin(), loadedRows.end());
	loadedRows.erase(std::unique(loadedRows.begin(), loadedRows.end()), loadedRows.end());
	for (const size_t row : externalRows) {
		if (!std::binary_search(loadedRows.begin(), loadedRows.end(), row)) {
			fallbackRows.push_back(row);
		}
	}
}

bool isCompleteFullNodeCandidateSet(
		const storage::DataManager &dm,
		const std::vector<int64_t> &candidateIds,
		const NodeScanConfig &config,
		const NodeScanRequirements &requirements) {
	if (config.type != ScanType::FULL_SCAN || !config.labels.empty() || requirements.needsLabels) {
		return false;
	}
	const auto allocator = dm.getIdAllocator(EntityType::Node);
	const int64_t maxId = allocator->getCurrentMaxId();
	return maxId > 0 && candidateIds.front() == 1 && candidateIds.back() == maxId &&
		   candidateIds.size() == static_cast<size_t>(maxId) &&
		   std::is_sorted(candidateIds.begin(), candidateIds.end());
}

std::vector<size_t> normalizeFallbackRows(std::vector<size_t> rows) {
	std::sort(rows.begin(), rows.end());
	rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
	return rows;
}

std::optional<Node> makePropertyEntityFallbackNode(
		const std::vector<int64_t> &nodeIds,
		const std::vector<int64_t> &propertyEntityIds,
		size_t row) {
	if (row >= nodeIds.size() || row >= propertyEntityIds.size()) {
		return std::nullopt;
	}

	Node node;
	auto &metadata = node.getMutableMetadata();
	metadata.id = nodeIds[row];
	metadata.propertyEntityId = propertyEntityIds[row];
	metadata.propertyStorageType = static_cast<uint32_t>(PropertyStorageType::PROPERTY_ENTITY);
	metadata.isActive = true;
	return node;
}

size_t countPropertyEntityFallbackMatches(
		storage::DataManager &dm,
		const PropertyPredicateScanKernel &scanKernel,
		std::vector<size_t> fallbackRows,
		const std::vector<int64_t> &nodeIds,
		const std::vector<int64_t> &propertyEntityIds) {
	size_t matches = 0;
	for (const size_t row : normalizeFallbackRows(std::move(fallbackRows))) {
		auto node = makePropertyEntityFallbackNode(nodeIds, propertyEntityIds, row);
		if (node.has_value() && scanKernel.matchesMap(dm.getNodePropertiesDirect(*node))) {
			++matches;
		}
	}
	return matches;
}

size_t countBlobFallbackMatches(
		storage::DataManager &dm,
		const PropertyPredicateScanKernel &scanKernel,
		const std::vector<NodePropertyCandidateRef> &blobRefs) {
	size_t matches = 0;
	for (const auto &fallback : blobRefs) {
		if (scanKernel.matchesMap(dm.getNodePropertiesDirect(fallback.toNode()))) {
			++matches;
		}
	}
	return matches;
}

void recordPredicateCountTrace(
		bool traceEnabled,
		std::chrono::steady_clock::time_point traceStart) {
	if (!traceEnabled) {
		return;
	}
	const auto elapsedNs = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - traceStart)
					.count());
	debug::PerfTrace::addDuration("node_scan.predicate_count", elapsedNs);
}

} // namespace graph::query::execution::node_columnar_predicate_counter_detail
