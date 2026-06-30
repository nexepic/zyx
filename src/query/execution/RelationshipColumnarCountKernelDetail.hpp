#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace graph::storage {
	class DataManager;
}

namespace graph::query::execution {
	class PropertyPredicateScanKernel;
}

namespace graph::query::execution::relationship_columnar_count_detail {

	std::vector<size_t> sequenceRows(size_t count);

	void appendRowsMissingFromBulkMatch(
			size_t rowCount,
			std::vector<size_t> loadedRows,
			std::vector<size_t> &fallbackRows);

	size_t countFallbackPropertyRows(
			storage::DataManager &dm,
			const PropertyPredicateScanKernel &scanKernel,
			const std::vector<size_t> &fallbackRows,
			const std::vector<int64_t> &propertyEdgeIds);

	size_t countFallbackEdgeIds(
			storage::DataManager &dm,
			const PropertyPredicateScanKernel &scanKernel,
			const std::vector<int64_t> &edgeIds);

} // namespace graph::query::execution::relationship_columnar_count_detail
