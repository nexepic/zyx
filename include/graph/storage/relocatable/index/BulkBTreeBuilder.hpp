/**
 * @file BulkBTreeBuilder.hpp
 * @brief Dense, bounded, bottom-up materialization of immutable relocatable B+Trees.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "graph/storage/relocatable/index/BTreePageStore.hpp"

namespace graph::storage {

	struct BulkBTreeBuildOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		uint64_t pool_owner_id = 0;
		// Both input records and generated pages are bounded before allocation,
		// making a malformed migration source fail before it can grow memory or
		// reserve an unbounded number of physical segments.
		size_t maximum_entries = 0;
		size_t maximum_pages = 0;
		// Internal pages always carry child PageIds. Leaf pages may instead be
		// key-only membership ranges, such as ordinary graph adjacency.
		BTreeLeafValueLayout leaf_value_layout = BTreeLeafValueLayout::BLVL_U64;
	};

	struct BulkBTreeBuildResult {
		Superblock superblock;
		uint64_t entry_count = 0;
		size_t page_count = 0;
		uint16_t root_level = 0;
		uint32_t semantic_crc = 0;

		constexpr bool operator==(const BulkBTreeBuildResult &) const = default;
	};

	// Produces a densely packed leaf level and parent levels bottom-up. It does
	// not call per-entry upsert, so build cost is linear in input plus output
	// pages and root publication occurs exactly once.
	class BulkBTreeBuilder final {
        friend class RelocatableMetadataStore;
        // Metadata-owned image preparation before any data write/cost admission.
        // Ordinary callers retain the existing write-through native preparer.
        using NativeImageSink = utils::OwnedFunction<void(const CatalogEntry &, std::span<BTreePage>,
            std::span<PageLocation>, LogSequenceNumber)>;
        [[nodiscard]] static BTreeInitialTreeMutationPlan planNativeTreeWithImageSink(
            BTreePageStore &pages, RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot,
            uint64_t indexId, const OrderedBTreeEntrySource &source, const BulkBTreeBuildOptions &options,
            NativeImageSink sink);
	public:
		// Plans the same dense two-pass tree without making any metadata root
		// reachable. The returned fresh-pool plan can be consumed by a native graph
		// transaction; the caller still owns the snapshot and publication boundary.
		[[nodiscard]] static BTreeInitialTreeMutationPlan
		planNativeTree(BTreePageStore &pages, RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot,
						   uint64_t indexId, const OrderedBTreeEntrySource &source,
						   const BulkBTreeBuildOptions &options);
		// Plans a dense replacement for an already published root. The caller must
		// retire that exact root in its enclosing metadata publication before the
		// returned fresh tree becomes reachable.
		[[nodiscard]] static BTreeInitialTreeMutationPlan
		planNativeReplacementTree(BTreePageStore &pages, RelocatableMetadataStore &metadata,
								 const MetadataSnapshotPin &snapshot, uint64_t indexId,
								 const OrderedBTreeEntrySource &source, const BulkBTreeBuildOptions &options);
		[[nodiscard]] static BulkBTreeBuildResult
		build(BTreePageStore &pages, RelocatableMetadataStore &metadata, uint64_t indexId,
			  const OrderedBTreeEntrySource &source, const BulkBTreeBuildOptions &options,
			  LogSequenceNumber checkpointLsn, const MetadataPublishOptions &publishOptions = {});
	};

} // namespace graph::storage
