/**
 * @file GlobalAdjacencyIndex.hpp
 * @brief Snapshot-pinned range reads for the global ordinary-graph adjacency tree.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <functional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"

namespace graph::storage {

	struct GlobalAdjacencyContinuation {
		Generation generation{};
		GlobalAdjacencyDirection direction = GlobalAdjacencyDirection::GAD_OUT;
		uint64_t node_id = 0U;
		uint64_t last_edge_id = 0U;

		constexpr bool operator==(const GlobalAdjacencyContinuation &) const = default;
	};

	struct GlobalAdjacencyRange {
		std::vector<uint64_t> edge_ids;
		std::optional<GlobalAdjacencyContinuation> continuation;
	};

	// The adapter exposes only logical Edge IDs. It pins no physical location in
	// its continuation, so a Page Directory relocation cannot invalidate a read.
	class GlobalAdjacencyIndex final {
	public:
		GlobalAdjacencyIndex(BTreePageStore &pages, RelocatableMetadataStore &metadata);

		[[nodiscard]] GlobalAdjacencyRange
		scan(const MetadataSnapshotPin &snapshot, GlobalAdjacencyDirection direction, uint64_t nodeId,
				size_t maximumEntries, std::optional<GlobalAdjacencyContinuation> continuation = {}) const;
		// Visits one direction of the adjacency tree in key order while matching a
		// sorted set of source nodes. A single range cursor replaces one B+Tree root
		// descent per source node for high-cardinality anchored traversals. The
		// entry bound covers both matched callbacks and intervening physical keys;
		// false therefore also reports a physical scan-budget exhaustion.
		[[nodiscard]] bool visitForNodes(const MetadataSnapshotPin &snapshot, GlobalAdjacencyDirection direction,
									 std::span<const uint64_t> nodeIds, size_t maximumEntries,
									 const std::function<bool(uint64_t, uint64_t)> &visitor) const;

	private:
		BTreeIndex index_;
	};

} // namespace graph::storage
