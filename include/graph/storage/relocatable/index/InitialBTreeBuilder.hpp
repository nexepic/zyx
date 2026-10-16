/**
 * @file InitialBTreeBuilder.hpp
 * @brief Bounded in-memory planning for an initially materialized relocatable B+Tree.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "graph/storage/relocatable/index/BTreePage.hpp"

namespace graph::storage {

	struct InitialBTreePlanNode {
		uint16_t level = 0U;
		std::vector<std::byte> first_key;
		utils::OwnedStdVector<BTreePageEntry> leaf_entries;
		std::vector<size_t> children;
	};

	struct InitialBTreePlan {
		BTreeLeafValueLayout leaf_value_layout = BTreeLeafValueLayout::BLVL_U64;
		// Append without relocating owned leaf payloads. Their portable moves may
		// require iterator-proxy credit, so vector growth otherwise deep-copies.
		std::deque<InitialBTreePlanNode> nodes;
		size_t leaf_count = 0U;
		size_t root_ordinal = 0U;
	};

	// Plans and materializes a complete immutable B+Tree from a bounded,
	// strictly ordered entry set. Planning has no storage side effects, so a
	// compound transaction can allocate all logical page IDs before any page
	// becomes durable or reachable.
	class InitialBTreeBuilder final {
	public:
		[[nodiscard]] static InitialBTreePlan
		plan(uint64_t indexId, std::span<const BTreePageEntry> entries,
			 BTreeLeafValueLayout leafValueLayout = BTreeLeafValueLayout::BLVL_U64);
		[[nodiscard]] static std::vector<BTreePage> materialize(uint64_t indexId, const InitialBTreePlan &plan,
																std::span<const PageId> pageIds,
																std::span<const Generation> generations,
																LogSequenceNumber checkpointLsn);
	};

} // namespace graph::storage
