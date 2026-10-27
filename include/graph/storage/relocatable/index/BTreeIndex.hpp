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

    // A descriptor-bound read must resolve exactly this root under exactly this
    // metadata generation. PageId/pool epoch alone can remain unchanged across
    // COW updates, so they cannot replace the pinned semantic context.
    struct BTreeRootReadBinding {
        DatabaseId database_id{};
        Generation snapshot_generation{};
        IndexRootLocation root{};
    };


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

			// Admits the owned separator key used to validate the next leaf.
			// Reports retained capacity and zero when released, as well as admission
			// before a new copy. Rejection before emission permits retry.
			// Decoded leaf entries and cursor pages are separate buffers.
			[[nodiscard]] std::optional<BTreePageEntry> next(const std::function<void(size_t)> &boundaryKeyAdmission = {});
			[[nodiscard]] std::optional<BTreeFixedKey> nextFixedKey(const std::function<void(size_t)> &boundaryKeyAdmission = {});
			[[nodiscard]] size_t fixedKeyWidth() const noexcept;

		private:
			struct State;
			friend class BTreeIndex;
			explicit RangeCursor(std::unique_ptr<State> state);

			std::unique_ptr<State> state_;
		};

		BTreeIndex(uint64_t indexId, BTreePageStore &pages, RelocatableMetadataStore &metadata,
					   BTreeLeafValueLayout leafValueLayout = BTreeLeafValueLayout::BLVL_U64,
            std::optional<BTreeRootReadBinding> rootBinding = {});

		[[nodiscard]] std::optional<uint64_t> find(const MetadataSnapshotPin &snapshot,
												   std::span<const std::byte> key) const;
		// Exact probes for strictly increasing keys. Reuse a decoded leaf only
		// within its authenticated separator bounds; never scan unrelated gaps.
		[[nodiscard]] std::vector<std::optional<uint64_t>> findSorted(
				const MetadataSnapshotPin &snapshot, std::span<const std::vector<std::byte>> keys) const;
        [[nodiscard]] utils::OwnedStdVector<std::optional<uint64_t>> findSortedOwned(
            const MetadataSnapshotPin &snapshot, std::span<const utils::OwnedStdVector<std::byte>> keys) const;

		[[nodiscard]] utils::OwnedStdVector<BTreePageEntry> scan(const MetadataSnapshotPin &snapshot,
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
		// Optional fixed-key representation for long scans over EMPTY-value leaves.
		// The initial descent keeps ordinary validation; successor leaves decode
		// into one accounted array instead of individually allocated key vectors.
		// Resource pressure during conversion retains the ordinary cursor.
		[[nodiscard]] RangeCursor openFixedKeyRangeCursor(
			const MetadataSnapshotPin &snapshot, std::optional<std::span<const std::byte>> lowerBound,
			size_t maximumEntries, size_t keyWidth,
			BufferPoolReadHint initialLeafReadHint = BufferPoolReadHint::BPRH_SEQUENTIAL,
			bool excludeLowerBound = false, const CancellationCheckpoint *cancellation = nullptr) const;
		// Produces fresh, sequential cursors over one pinned tree without retaining
		// the full tree in memory. The snapshot must outlive every opened cursor.
		[[nodiscard]] OrderedBTreeEntrySource orderedSource(const MetadataSnapshotPin &snapshot,
																	 size_t maximumEntries,
																	 size_t cursorBatchEntries = 0U) const;

	private:
        template<class Key>
        [[nodiscard]] utils::OwnedStdVector<std::optional<uint64_t>> findSortedKeys(
            const MetadataSnapshotPin &snapshot, std::span<const Key> keys) const;
		struct TraversalStep {
            using OwnedHeapElement = void;
			PageId page_id{};
			uint16_t level = 0U;
			size_t child_index = 0U;
			std::optional<utils::OwnedStdVector<std::byte>> parent_lower_inclusive;
			std::optional<utils::OwnedStdVector<std::byte>> parent_upper_exclusive;
			// The route is already authenticated while descending from the root.
			// Retain the decoded routing entries so successor-leaf traversal does
			// not reread the same internal pages for every leaf in this cursor.
			BTreePageHeader header{};
			utils::OwnedStdVector<BTreePageEntry> entries;
		};

		struct LeafRoute {
			PageId page_id{};
			std::optional<utils::OwnedStdVector<std::byte>> lower_inclusive;
			std::optional<utils::OwnedStdVector<std::byte>> upper_exclusive;
		};

		[[nodiscard]] std::optional<BTreePage> resolveRoot(const MetadataSnapshotPin &snapshot,
			utils::OwnedStdVector<BTreePageEntry> *decodedEntries = nullptr) const;
		[[nodiscard]] BTreePage descendToLeaf(const MetadataSnapshotPin &snapshot, BTreePage page,
											  std::optional<std::span<const std::byte>> key,
											  BufferPoolReadHint leafReadHint,
											  const CancellationCheckpoint *cancellation,
											  utils::OwnedStdVector<TraversalStep> *traversal = nullptr,
				utils::OwnedStdVector<BTreePageEntry> *leafEntries = nullptr, LeafRoute *leafRoute = nullptr,
			utils::OwnedStdVector<BTreePageEntry> *decodedRootEntries = nullptr) const;
		// Borrow the immutable root across exact probes. An empty return means
		// the root itself is the leaf; all descent validation is shared with scans.
		[[nodiscard]] std::optional<BTreePage> descendToLeafFromRoot(
				const MetadataSnapshotPin &snapshot, const BTreePage &root,
				std::optional<std::span<const std::byte>> key, BufferPoolReadHint leafReadHint,
				const CancellationCheckpoint *cancellation, utils::OwnedStdVector<TraversalStep> *traversal = nullptr,
				utils::OwnedStdVector<BTreePageEntry> *leafEntries = nullptr, LeafRoute *leafRoute = nullptr,
			utils::OwnedStdVector<BTreePageEntry> *decodedRootEntries = nullptr) const;
		[[nodiscard]] LeafRoute advanceToExpectedSiblingLeaf(const MetadataSnapshotPin &snapshot,
																 utils::OwnedStdVector<TraversalStep> &traversal,
																					 const CancellationCheckpoint *cancellation) const;
		[[nodiscard]] std::optional<BTreePageEntry> nextRangeCursor(RangeCursor::State &state,
				const std::function<void(size_t)> &boundaryKeyAdmission) const;
		[[nodiscard]] bool prepareRangeCursorNext(RangeCursor::State &state,
			const std::function<void(size_t)> &boundaryKeyAdmission) const;

		std::optional<BTreeRootReadBinding> root_binding_;
		uint64_t index_id_ = 0;
		BTreePageStore &pages_;
		RelocatableMetadataStore &metadata_;
		BTreeLeafValueLayout leaf_value_layout_ = BTreeLeafValueLayout::BLVL_U64;
	};

} // namespace graph::storage
