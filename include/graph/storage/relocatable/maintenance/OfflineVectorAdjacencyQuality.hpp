/**
 * @file OfflineVectorAdjacencyQuality.hpp
 * @brief Bounded quality measurement for a rebuilt Vector adjacency graph.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "graph/storage/relocatable/maintenance/OfflineVectorAdjacencyRebuild.hpp"

namespace graph::storage {

	// This evaluator measures retained exact neighbors in an adjacency graph. It
	// deliberately does not represent end-to-end ANN query recall, which also
	// depends on the query traversal and raw-vector refinement path.
	struct VectorAdjacencyQualityOptions {
		size_t maximum_records = 0;
		uint16_t recall_at = 0;
	};

	struct VectorAdjacencyQualityReport {
		uint64_t vector_count = 0;
		uint64_t expected_exact_neighbors = 0;
		uint64_t retained_exact_neighbors = 0;
		double adjacency_neighbor_recall = 0.0;

		bool operator==(const VectorAdjacencyQualityReport &) const = default;
	};

	class OfflineVectorAdjacencyQuality final {
	public:
		[[nodiscard]] static VectorAdjacencyQualityReport evaluate(
				std::span<const CanonicalVectorRecord> records, std::span<const VectorAdjacencyRecord> adjacency,
				const VectorAdjacencyQualityOptions &options);
	};

} // namespace graph::storage
