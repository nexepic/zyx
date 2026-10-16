/**
 * @file VectorDeltaMapping.hpp
 * @brief Typed VectorId overlay mapping for mutable Vector derived state.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/CancellationCheckpoint.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/vector/VectorMappingIds.hpp"

namespace graph::storage {

	class BTreePageStore;
	class MetadataSnapshotPin;
	class RelocatableMetadataStore;

	// A delta entry overlays a base PQ/adjacency generation. Inserts are read
	// exactly until the next offline rebuild; erases prevent stale base candidates
	// from becoming visible through an old adjacency edge.
	enum class VectorDeltaOperation : uint8_t {
		VDO_INSERT = 1,
		VDO_ERASE = 2,
	};

	struct VectorDeltaEntry {
		uint64_t vector_id = 0U;
		uint64_t node_id = 0U;
		VectorDeltaOperation operation = VectorDeltaOperation::VDO_INSERT;

		constexpr bool operator==(const VectorDeltaEntry &) const = default;
	};

	struct VectorDeltaMappingBuildOptions {
		BulkBTreeBuildOptions tree;
		size_t maximum_entries = 0U;
	};

	class VectorDeltaMapping final {
	public:
		// A delta leaf stores the fixed-width VectorId/operation key and NodeId
		// value in a slotted B+Tree page. The bound includes the internal B+Tree
		// levels required to reach multiple leaves, so an accepted overlay cap
		// always fits the configured reachable-tree page budget.
		[[nodiscard]] static constexpr size_t maximumEntriesForPageBudget(size_t maximumPages) {
			constexpr size_t HEADER_BYTES = RSS_SLOTTED_PAGE_HEADER_BYTES + RSS_SLOTTED_PAGE_SLOT_BYTES +
					RSS_BTREE_PAGE_HEADER_BYTES;
			constexpr size_t ENTRY_BYTES =
					RSS_SLOTTED_PAGE_SLOT_BYTES + RSS_BTREE_ENTRY_HEADER_BYTES + sizeof(uint64_t) + sizeof(uint8_t);
			constexpr size_t ENTRIES_PER_LEAF =
					(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - HEADER_BYTES) / ENTRY_BYTES;
			static_assert(ENTRIES_PER_LEAF != 0U);
			constexpr size_t INTERNAL_FANOUT = ENTRIES_PER_LEAF + 1U;

			// Internal pages hold one leftmost child plus one child per separator.
			// Binary-searching leaf pages keeps this constexpr calculation bounded
			// even when callers validate a large operator-supplied page budget.
			const auto fitsPageBudget = [maximumPages](size_t leafPages) constexpr {
				size_t usedPages = leafPages;
				size_t childCount = leafPages;
				while (childCount > 1U) {
					const size_t parentCount = childCount / INTERNAL_FANOUT +
							static_cast<size_t>(childCount % INTERNAL_FANOUT != 0U);
					if (parentCount > maximumPages - usedPages) {
						return false;
					}
					usedPages += parentCount;
					childCount = parentCount;
				}
				return true;
			};

			size_t low = 0U;
			size_t high = maximumPages;
			while (low < high) {
				const size_t difference = high - low;
				const size_t middle = low + difference / 2U + difference % 2U;
				if (fitsPageBudget(middle)) {
					low = middle;
				} else {
					high = middle - 1U;
				}
			}
			constexpr size_t PERSISTED_MAXIMUM = static_cast<size_t>(std::numeric_limits<uint32_t>::max());
			return low > PERSISTED_MAXIMUM / ENTRIES_PER_LEAF
					   ? PERSISTED_MAXIMUM
					   : low * ENTRIES_PER_LEAF;
		}

		static void validateMappingIndexId(uint64_t mappingIndexId);
		[[nodiscard]] static constexpr bool isMappingIndexId(uint64_t mappingIndexId) {
			return (mappingIndexId & RSS_DERIVED_MAPPING_INDEX_TYPE_MASK) ==
				   RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX;
		}
		[[nodiscard]] static uint64_t selectMappingIndexId(const MetadataSnapshotPin &snapshot,
															 RelocatableMetadataStore &metadata, uint64_t vectorIndexId);
		[[nodiscard]] static BulkBTreeBuildResult
		build(BTreePageStore &pages, RelocatableMetadataStore &metadata, uint64_t mappingIndexId,
			  std::span<const VectorDeltaEntry> entries, const VectorDeltaMappingBuildOptions &options,
			  LogSequenceNumber checkpointLsn, const MetadataPublishOptions &publishOptions = {});
		[[nodiscard]] static std::optional<VectorDeltaOperation>
		findOperation(const MetadataSnapshotPin &snapshot, BTreePageStore &pages, RelocatableMetadataStore &metadata,
					  uint64_t mappingIndexId, uint64_t vectorId);
		[[nodiscard]] static std::vector<VectorDeltaEntry>
		scan(const MetadataSnapshotPin &snapshot, BTreePageStore &pages, RelocatableMetadataStore &metadata,
			 uint64_t mappingIndexId, size_t maximumEntries, const CancellationCheckpoint *cancellation = nullptr);
		[[nodiscard]] static utils::OwnedStdVector<VectorDeltaEntry>
		scanOwned(const MetadataSnapshotPin &snapshot, BTreePageStore &pages, RelocatableMetadataStore &metadata,
			 uint64_t mappingIndexId, size_t maximumEntries, const CancellationCheckpoint *cancellation = nullptr);
		// Current Raw locations describe the post-mutation semantic state. Inserts
		// must resolve there, while erase tombstones name records retained only by
		// an older derived base and therefore must no longer have a Raw location.
		static void validateCurrentRawMembership(const VectorDeltaEntry &entry, bool hasCurrentRawLocation);
		// The operation is part of the key while the leaf value carries NodeId.
		// This keeps the full uint64 Node namespace available and lets readers
		// validate both directions of a delta transition without a full scan.
		[[nodiscard]] static std::vector<std::byte> key(uint64_t vectorId, VectorDeltaOperation operation);
		[[nodiscard]] static VectorDeltaEntry decodeEntry(const BTreePageEntry &entry);
		[[nodiscard]] static uint64_t encodeOperation(VectorDeltaOperation operation);
		[[nodiscard]] static VectorDeltaOperation decodeOperation(uint64_t value);
	};

} // namespace graph::storage
