/**
 * @file VectorPqMapping.hpp
 * @brief Immutable VectorId-to-PQ-page B+Tree contract for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/vector/VectorMappingIds.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"

namespace graph::storage {

	struct VectorPqMappingBuildOptions {
		BulkBTreeBuildOptions tree;
		size_t maximum_entries = 0;
	};

	class VectorPqMapping final {
	public:
		// Mapping Index IDs are metadata-owned and must never overlap public index
		// definitions. The containing Manifest persists this ID with its root.
		static void validateMappingIndexId(uint64_t mappingIndexId);
		[[nodiscard]] static constexpr bool isMappingIndexId(uint64_t mappingIndexId) {
			return (mappingIndexId & RSS_DERIVED_MAPPING_INDEX_TYPE_MASK) ==
				   RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX;
		}
		[[nodiscard]] static constexpr bool isDerivedMappingIndexId(uint64_t mappingIndexId) {
			const uint64_t type = mappingIndexId & RSS_DERIVED_MAPPING_INDEX_TYPE_MASK;
			return type == RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX ||
				   type == RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX ||
				   type == RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX;
		}
		[[nodiscard]] static BulkBTreeBuildResult
		build(BTreePageStore &pages, RelocatableMetadataStore &metadata, uint64_t mappingIndexId,
			  std::span<const VectorPqPageMapping> mappings, const VectorPqMappingBuildOptions &options,
			  LogSequenceNumber checkpointLsn, const MetadataPublishOptions &publishOptions = {});
		[[nodiscard]] static std::optional<PageId> findPage(const MetadataSnapshotPin &snapshot, BTreePageStore &pages,
															RelocatableMetadataStore &metadata, uint64_t mappingIndexId,
															uint64_t vectorId);

		[[nodiscard]] static std::vector<std::byte> key(uint64_t vectorId);
	};

} // namespace graph::storage
