/**
 * @file VectorAdjacencyMapping.hpp
 * @brief Immutable VectorId-to-derived-adjacency-page mapping contract.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"

namespace graph::storage {

	struct VectorAdjacencyPageMapping {
		uint64_t vector_id = 0;
		PageId page_id{};

		constexpr bool operator==(const VectorAdjacencyPageMapping &) const = default;
	};

	struct VectorAdjacencyMappingBuildOptions {
		BulkBTreeBuildOptions tree;
		size_t maximum_entries = 0;
	};

	// The B+Tree wire format is identical to the PQ mapping, but this separate
	// type prevents a Manifest from binding an adjacency generation to a PQ root.
	class VectorAdjacencyMapping final {
	public:
		static void validateMappingIndexId(uint64_t mappingIndexId);
		[[nodiscard]] static constexpr bool isMappingIndexId(uint64_t mappingIndexId) {
			return (mappingIndexId & RSS_DERIVED_MAPPING_INDEX_TYPE_MASK) ==
				   RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX;
		}
		[[nodiscard]] static BulkBTreeBuildResult
		build(BTreePageStore &pages, RelocatableMetadataStore &metadata, uint64_t mappingIndexId,
			  std::span<const VectorAdjacencyPageMapping> mappings, const VectorAdjacencyMappingBuildOptions &options,
			  LogSequenceNumber checkpointLsn, const MetadataPublishOptions &publishOptions = {});
		[[nodiscard]] static std::optional<PageId> findPage(const MetadataSnapshotPin &snapshot, BTreePageStore &pages,
															RelocatableMetadataStore &metadata, uint64_t mappingIndexId,
															uint64_t vectorId);
		[[nodiscard]] static std::vector<std::byte> key(uint64_t vectorId);
	};

} // namespace graph::storage
