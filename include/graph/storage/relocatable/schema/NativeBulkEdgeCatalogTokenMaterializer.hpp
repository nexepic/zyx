/**
 * @file NativeBulkEdgeCatalogTokenMaterializer.hpp
 * @brief Bounded Catalog-token overlay planning for streamed native Edges.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "graph/storage/relocatable/entity/NativeBulkEdgeTokenRunBuilder.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

namespace graph::storage {

	struct NativeBulkEdgeCatalogTokenMaterializationOptions {
		size_t maximum_token_entries = 0U;
		size_t maximum_catalog_entries = 0U;
	};

	struct NativeBulkEdgeCatalogTokenResolution {
		NativeBulkEdgeTokenKind kind = NativeBulkEdgeTokenKind::NBETK_PROPERTY;
		std::string name;
		uint64_t token_id = 0U;
		bool newly_allocated = false;

		bool operator==(const NativeBulkEdgeCatalogTokenResolution &) const = default;
	};

	using NativeBulkEdgeCatalogTokenResolutionCursor =
			std::function<std::optional<NativeBulkEdgeCatalogTokenResolution>()>;
	using NativeBulkEdgeCatalogTokenResolutionSource = std::function<NativeBulkEdgeCatalogTokenResolutionCursor()>;

	// Produces a rewindable, sorted token-resolution source and a sorted final
	// Catalog overlay source. Every pass keeps one token name and one Catalog
	// lookup at most; this intentionally does not inherit the ordinary write
	// session's transaction-wide name maps. The caller owns the eventual one-
	// generation B+Tree replacement and publication.
	struct NativeBulkEdgeCatalogTokenMaterialization {
		NativeBulkEdgeCatalogTokenResolutionSource resolutions;
		OrderedBTreeEntrySource final_catalog_entries;
		uint64_t new_property_tokens = 0U;
		uint64_t new_schema_tokens = 0U;
		bool graph_cardinality_updated = false;
		bool catalog_changed = false;
	};

	class NativeBulkEdgeCatalogTokenMaterializer final {
	public:
		[[nodiscard]] static OrderedBTreeEntrySource overlayEntries(OrderedBTreeEntrySource existing, OrderedBTreeEntrySource updates);
		[[nodiscard]] static NativeBulkEdgeCatalogTokenMaterialization
		prepare(SystemCatalog &catalog, const MetadataSnapshotPin &snapshot,
				const OrderedBTreeEntrySource &sorted_token_entries,
				NativeBulkEdgeCatalogTokenMaterializationOptions options, uint64_t edge_count_increment = 0U);
	};

} // namespace graph::storage
