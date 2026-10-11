/**
 * @file BTreeIndex.hpp
 * @brief Snapshot-pinned read traversal for isolated relocatable B+Tree pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/CancellationCheckpoint.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"

namespace graph::storage {

	class RelocatableMetadataStore;

	// This read-only layer is deliberately separate from structural mutation.
	// One snapshot pins the Index Root and Page directories for the entire
	// traversal, so every PageId resolves against one metadata generation.
	class BTreeIndex final {
	public:
		BTreeIndex(uint64_t indexId, BTreePageStore &pages, RelocatableMetadataStore &metadata,
					   BTreeLeafValueLayout leafValueLayout = BTreeLeafValueLayout::BLVL_U64);

		[[nodiscard]] std::optional<uint64_t> find(const MetadataSnapshotPin &snapshot,
												   std::span<const std::byte> key) const;
		[[nodiscard]] std::vector<BTreePageEntry> scan(const MetadataSnapshotPin &snapshot,
																	   std::optional<std::span<const std::byte>> lowerBound,
																	   size_t maximumEntries,
																	   BufferPoolReadHint initialLeafReadHint = BufferPoolReadHint::BPRH_SEQUENTIAL,
																	   const CancellationCheckpoint *cancellation = nullptr,
																	   bool excludeLowerBound = false) const;
		// Produces fresh, sequential cursors over one pinned tree without retaining
		// the full tree in memory. The snapshot must outlive every opened cursor.
		[[nodiscard]] OrderedBTreeEntrySource orderedSource(const MetadataSnapshotPin &snapshot,
																	 size_t maximumEntries,
																	 size_t cursorBatchEntries = 0U) const;

	private:
		struct TraversalStep {
			PageId page_id{};
			uint16_t level = 0U;
			size_t child_index = 0U;
			std::optional<std::vector<std::byte>> parent_lower_inclusive;
			std::optional<std::vector<std::byte>> parent_upper_exclusive;
		};

		struct LeafRoute {
			PageId page_id{};
			std::optional<std::vector<std::byte>> lower_inclusive;
			std::optional<std::vector<std::byte>> upper_exclusive;
		};

		[[nodiscard]] std::optional<BTreePage> resolveRoot(const MetadataSnapshotPin &snapshot) const;
		[[nodiscard]] BTreePage descendToLeaf(const MetadataSnapshotPin &snapshot, BTreePage page,
											  std::optional<std::span<const std::byte>> key,
											  BufferPoolReadHint leafReadHint,
											  const CancellationCheckpoint *cancellation,
											  std::vector<TraversalStep> *traversal = nullptr) const;
		[[nodiscard]] LeafRoute advanceToExpectedSiblingLeaf(const MetadataSnapshotPin &snapshot,
																 std::vector<TraversalStep> &traversal,
																 const CancellationCheckpoint *cancellation) const;

		uint64_t index_id_ = 0;
		BTreePageStore &pages_;
		RelocatableMetadataStore &metadata_;
		BTreeLeafValueLayout leaf_value_layout_ = BTreeLeafValueLayout::BLVL_U64;
	};

} // namespace graph::storage
