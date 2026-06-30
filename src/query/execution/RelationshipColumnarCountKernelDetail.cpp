#include "src/query/execution/RelationshipColumnarCountKernelDetail.hpp"

#include <algorithm>

#include "graph/query/execution/PropertyPredicateScanKernel.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution::relationship_columnar_count_detail {

std::vector<size_t> sequenceRows(size_t count) {
	std::vector<size_t> rows;
	rows.reserve(count);
	for (size_t row = 0; row < count; ++row) {
		rows.push_back(row);
	}
	return rows;
}

void appendRowsMissingFromBulkMatch(
		size_t rowCount,
		std::vector<size_t> loadedRows,
		std::vector<size_t> &fallbackRows) {
	if (loadedRows.size() == rowCount) {
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

size_t countFallbackPropertyRows(
		storage::DataManager &dm,
		const PropertyPredicateScanKernel &scanKernel,
		const std::vector<size_t> &fallbackRows,
		const std::vector<int64_t> &propertyEdgeIds) {
	size_t matches = 0;
	for (const size_t row : fallbackRows) {
		if (row >= propertyEdgeIds.size()) {
			continue;
		}
		if (scanKernel.matchesMap(dm.getEdgeProperties(propertyEdgeIds[row]))) {
			++matches;
		}
	}
	return matches;
}

size_t countFallbackEdgeIds(
		storage::DataManager &dm,
		const PropertyPredicateScanKernel &scanKernel,
		const std::vector<int64_t> &edgeIds) {
	size_t matches = 0;
	for (const int64_t edgeId : edgeIds) {
		if (scanKernel.matchesMap(dm.getEdgeProperties(edgeId))) {
			++matches;
		}
	}
	return matches;
}

} // namespace graph::query::execution::relationship_columnar_count_detail
