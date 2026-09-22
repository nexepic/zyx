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
		// A cursor batch is an execution bound, not a persisted page-capacity
		// assumption. Callers may request a smaller bound when their own resource
		// policy requires it.
		static constexpr size_t DEFAULT_CURSOR_BATCH_ENTRIES = 256U;

		// A range cursor is bound to one caller-owned metadata snapshot. It keeps
		// one decoded leaf and its root-to-leaf route, so resuming does not restart
		// a root descent or retain an unbounded history of visited leaves.
		class RangeCursor final {
		public:
			~RangeCursor();
			RangeCursor(const RangeCursor &) = delete;
			RangeCursor &operator=(const RangeCursor &) = delete;
			RangeCursor(RangeCursor &&) noexcept;
			RangeCursor &operator=(RangeCursor &&) noexcept;

			[[nodiscard]] std::optional<BTreePageEntry> next();

		private:
			struct State;
			friend class BTreeIndex;
			explicit RangeCursor(std::unique_ptr<State> state);

			std::unique_ptr<State> state_;
		};

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
		// Opens a true continuation cursor. Unlike repeatedly calling scan() with
		// the previous key, this descends from the root exactly once.
		[[nodiscard]] RangeCursor openRangeCursor(
				const MetadataSnapshotPin &snapshot, std::optional<std::span<const std::byte>> lowerBound,
				size_t maximumEntries,
				BufferPoolReadHint initialLeafReadHint = BufferPoolReadHint::BPRH_SEQUENTIAL,
				bool excludeLowerBound = false, const CancellationCheckpoint *cancellation = nullptr) const;
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
			// The route is already authenticated while descending from the root.
			// Retain the decoded routing entries so successor-leaf traversal does
			// not reread the same internal pages for every leaf in this cursor.
			BTreePageHeader header{};
			std::vector<BTreePageEntry> entries;
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
		[[nodiscard]] std::optional<BTreePageEntry> nextRangeCursor(RangeCursor::State &state) const;

		uint64_t index_id_ = 0;
		BTreePageStore &pages_;
		RelocatableMetadataStore &metadata_;
		BTreeLeafValueLayout leaf_value_layout_ = BTreeLeafValueLayout::BLVL_U64;
	};

} // namespace graph::storage
