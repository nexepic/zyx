/**
 * @file RelocatableVectorSemanticScanner.hpp
 * @brief Bounded logical Vector extraction from migrated relocatable graph data.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/schema/IndexDefinition.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"

namespace graph::storage {

	struct RelocatableVectorSemanticScanOptions {
		size_t maximum_definitions = 0;
		size_t maximum_batch_records = 0;
		size_t maximum_property_names = 0;
		uint64_t maximum_vectors_per_index = 0;
		uint32_t maximum_dimensions = 0;
	};

	struct RelocatableVectorSemanticRecord {
		uint64_t index_id = 0;
		uint64_t node_id = 0;
		// One little-endian BFloat16 coordinate per configured dimension. This is
		// the logical raw-vector representation, never legacy Blob bytes.
		std::vector<std::byte> raw_bfloat16;

		bool operator==(const RelocatableVectorSemanticRecord &) const = default;
	};

	struct RelocatableVectorSemanticScanResult {
		uint64_t vector_count = 0;
		uint32_t semantic_crc = 0;

		constexpr bool operator==(const RelocatableVectorSemanticScanResult &) const = default;
	};

	using RelocatableVectorSemanticBatchConsumer = std::function<void(std::span<const RelocatableVectorSemanticRecord>)>;

	// Extracts Vector-indexed Node properties from native Entity pages and
	// owner-keyed Property roots. It intentionally does not inspect legacy
	// DiskANN mappings, Blob chains, PQ codes, or adjacency bytes; those are
	// physical caches rebuilt from this semantic stream.
	class RelocatableVectorSemanticScanner final {
	public:
		[[nodiscard]] static RelocatableVectorSemanticScanResult
		scan(std::span<const IndexDefinition> definitions, EntityPageStore &entities, EntityLabelSetStore &labels,
				 PropertyCollectionRootStore &properties, SystemCatalog &catalog,
			 RelocatableMetadataStore &metadata, const RelocatableVectorSemanticScanOptions &options,
			 const RelocatableVectorSemanticBatchConsumer &consumer);
	};

} // namespace graph::storage
