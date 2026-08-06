/**
 * @file NativeConstraintSupportTreeBuilder.hpp
 * @brief Snapshot-pinned native construction plans for constraint support trees.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/schema/ConstraintDefinition.hpp"

namespace graph::storage {

	class MetadataSnapshotPin;
	class PageBackedGraphReadAdapter;

	// Native DDL resolves Property names from the same System Catalog snapshot
	// as Entity and Property pages. The planner deliberately owns no second
	// token registry or dictionary fallback.
	using NativeConstraintPropertyTokenResolver =
			std::function<std::optional<uint32_t>(const MetadataSnapshotPin &, std::string_view)>;

	struct NativeConstraintSupportTreeBuildOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		size_t maximum_entries = 0U;
		size_t maximum_pages = 0U;
		size_t sort_run_bytes = 0U;
		size_t maximum_sort_runs = 0U;
		size_t maximum_collisions = 0U;
	};

	// Builds a bounded, rewindable native support-tree plan without publishing
	// any page or root. Entity IDs and Property tokens come directly from one
	// pinned native graph generation; the enclosing graph transaction owns the
	// eventual root publication.
	class NativeConstraintSupportTreeBuilder final {
	public:
		[[nodiscard]] static BTreeInitialTreeMutationPlan plan(
				BTreePageStore &pages, RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot,
				const ConstraintDefinition &definition, PageBackedGraphReadAdapter &graph,
				const NativeConstraintSupportTreeBuildOptions &options,
				const NativeConstraintPropertyTokenResolver &propertyTokenResolver);
	};

} // namespace graph::storage
