#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "graph/core/Node.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/ScanConfigs.hpp"

namespace graph::storage {
	class DataManager;
}

namespace graph::query::execution {
	class PropertyPredicateScanKernel;
}

namespace graph::query::execution::node_columnar_predicate_counter_detail {

	void appendRowsMissingFromBulkMatch(
			const std::vector<size_t> &externalRows,
			std::vector<size_t> loadedRows,
			std::vector<size_t> &fallbackRows);

	bool isCompleteFullNodeCandidateSet(
			const storage::DataManager &dm,
			const std::vector<int64_t> &candidateIds,
			const NodeScanConfig &config,
			const NodeScanRequirements &requirements);

	std::vector<size_t> normalizeFallbackRows(std::vector<size_t> rows);

	std::optional<Node> makePropertyEntityFallbackNode(
			const std::vector<int64_t> &nodeIds,
			const std::vector<int64_t> &propertyEntityIds,
			size_t row);

	size_t countPropertyEntityFallbackMatches(
			storage::DataManager &dm,
			const PropertyPredicateScanKernel &scanKernel,
			std::vector<size_t> fallbackRows,
			const std::vector<int64_t> &nodeIds,
			const std::vector<int64_t> &propertyEntityIds);

	size_t countBlobFallbackMatches(
			storage::DataManager &dm,
			const PropertyPredicateScanKernel &scanKernel,
			const std::vector<NodePropertyCandidateRef> &blobRefs);

	void recordPredicateCountTrace(
			bool traceEnabled,
			std::chrono::steady_clock::time_point traceStart);

} // namespace graph::query::execution::node_columnar_predicate_counter_detail
