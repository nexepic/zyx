/**
 * @file VectorNodeMapping.hpp
 * @brief Immutable VectorId-to-NodeId B+Tree contract for relocatable.
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

namespace graph::storage {

	class RelocatableMetadataStore;

	struct VectorNodeMappingEntry {
		uint64_t vector_id = 0U;
		uint64_t node_id = 0U;

		constexpr bool operator==(const VectorNodeMappingEntry &) const = default;
	};

	struct VectorNodeMappingBuildOptions {
		BulkBTreeBuildOptions tree;
		size_t maximum_entries = 0U;
	};

	// The reverse mapping is authoritative semantic metadata. It is deliberately
	// distinct from the physical Vector Location Directory: compaction can move a
	// raw record without changing candidate-to-Node resolution.
	class VectorNodeMapping final {
	public:
		static void validateMappingIndexId(uint64_t mappingIndexId);
		[[nodiscard]] static constexpr bool isMappingIndexId(uint64_t mappingIndexId) {
			return (mappingIndexId & RSS_DERIVED_MAPPING_INDEX_TYPE_MASK) ==
				   RSS_NODE_MAPPING_INTERNAL_INDEX_PREFIX;
		}
		// Mapping IDs are selected from a deterministic, bounded probe sequence.
		// The selected identity is persisted in the Manifest, so hash collisions
		// cannot change an established index's durable namespace.
		[[nodiscard]] static uint64_t selectMappingIndexId(const MetadataSnapshotPin &snapshot,
													 RelocatableMetadataStore &metadata, uint64_t vectorIndexId);
		[[nodiscard]] static BulkBTreeBuildResult
		build(BTreePageStore &pages, RelocatableMetadataStore &metadata, uint64_t mappingIndexId,
			  const OrderedBTreeEntrySource &source, const VectorNodeMappingBuildOptions &options,
			  LogSequenceNumber checkpointLsn, const MetadataPublishOptions &publishOptions = {});
		[[nodiscard]] static std::optional<uint64_t>
		findNode(const MetadataSnapshotPin &snapshot, BTreePageStore &pages, RelocatableMetadataStore &metadata,
				 uint64_t mappingIndexId, uint64_t vectorId);
		[[nodiscard]] static std::vector<std::byte> key(uint64_t vectorId);
	};

} // namespace graph::storage
