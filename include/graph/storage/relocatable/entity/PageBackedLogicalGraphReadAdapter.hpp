/**
 * @file PageBackedLogicalGraphReadAdapter.hpp
 * @brief Query-facing logical records materialized from native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"

namespace graph::storage {

	// Query-facing graph records use schema and Property names, whereas native
	// pages persist compact tokens. The resolver remains snapshot-aware so a
	// logical read can never combine names from a later metadata generation with
	// an earlier Entity-page image.
	class PageBackedGraphNameResolver {
	public:
		virtual ~PageBackedGraphNameResolver() = default;

		// Labels and relationship types have separate catalog namespaces. Carry the
		// entity semantic through the resolver so a corrupt or ambiguous catalog
		// entry cannot be silently interpreted in the wrong namespace.
		[[nodiscard]] virtual std::string resolveNodeLabelToken(const MetadataSnapshotPin &snapshot,
																			uint64_t tokenId) const = 0;
		[[nodiscard]] virtual std::string resolveRelationshipTypeToken(const MetadataSnapshotPin &snapshot,
																				  uint64_t tokenId) const = 0;
		[[nodiscard]] virtual std::string resolvePropertyKey(const MetadataSnapshotPin &snapshot,
															 uint32_t tokenId) const = 0;
	};

	class PageBackedLogicalGraphReadAdapter;

	// Retains a native scan position only inside one logical snapshot. The type
	// translates native records before they reach the query layer and never
	// exposes a PageId, slot, or segment identity.
	class PageBackedLogicalGraphAdjacencyCursor final {
	public:
		~PageBackedLogicalGraphAdjacencyCursor() = default;
		PageBackedLogicalGraphAdjacencyCursor(const PageBackedLogicalGraphAdjacencyCursor &) = delete;
		PageBackedLogicalGraphAdjacencyCursor &operator=(const PageBackedLogicalGraphAdjacencyCursor &) = delete;
		PageBackedLogicalGraphAdjacencyCursor(PageBackedLogicalGraphAdjacencyCursor &&) noexcept = default;
		PageBackedLogicalGraphAdjacencyCursor &operator=(PageBackedLogicalGraphAdjacencyCursor &&) noexcept = default;

		[[nodiscard]] LogicalGraphAdjacencyPage
		next(const MetadataSnapshotPin &snapshot, std::optional<LogicalGraphAdjacencyContinuation> continuation,
			 size_t maximumRecords);

	private:
		friend class PageBackedLogicalGraphReadAdapter;
		PageBackedLogicalGraphAdjacencyCursor(const PageBackedLogicalGraphReadAdapter &adapter,
											 PageBackedGraphAdjacencyCursor native) :
			adapter_(&adapter), native_(std::move(native)) {}

		const PageBackedLogicalGraphReadAdapter *adapter_ = nullptr;
		std::optional<PageBackedGraphAdjacencyCursor> native_;
	};

	// This adapter is intentionally separate from PageBackedGraphReadAdapter:
	// the lower adapter carries only durable tokens, while this one owns the
	// query-facing conversion and exposes direct owner-keyed Property entries.
	// No method emits a Property ID or physical page location.
	class PageBackedLogicalGraphReadAdapter final {
	public:
		PageBackedLogicalGraphReadAdapter(PageBackedGraphReadAdapter &pages, const PageBackedGraphNameResolver &names);

		[[nodiscard]] LogicalGraphNode readNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId) const;
		[[nodiscard]] LogicalGraphEdge readEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId) const;
		[[nodiscard]] bool visitNodesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
										   const std::function<bool(const LogicalGraphNode &)> &visitor) const;
		[[nodiscard]] bool visitEdgesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
										   const std::function<bool(const LogicalGraphEdge &)> &visitor) const;
		[[nodiscard]] bool visitEdgeTopologiesUntil(
				const MetadataSnapshotPin &snapshot, size_t maximumRecords,
				const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const;
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(const MetadataSnapshotPin &snapshot, uint64_t afterNodeId,
														  size_t maximumRecords, size_t maximumMetadataPages) const;
		[[nodiscard]] LogicalGraphAdjacencyPage
		scanAdjacentEdges(const MetadataSnapshotPin &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
						  std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const;
		[[nodiscard]] PageBackedLogicalGraphAdjacencyCursor
		openAdjacentEdgeCursor(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
							   LogicalGraphReadDirection direction) const;
		[[nodiscard]] bool visitAdjacentEdgesUntil(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
												   LogicalGraphReadDirection direction, size_t maximumRecords,
												   const std::function<bool(const LogicalGraphEdge &)> &visitor) const;
		[[nodiscard]] bool
		visitAdjacentEdgeTopologiesUntil(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
										 LogicalGraphReadDirection direction, size_t maximumRecords,
										 const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const;

	private:
		[[nodiscard]] LogicalGraphNode materializeNode(const MetadataSnapshotPin &snapshot,
													   const PageBackedGraphNode &node) const;
		[[nodiscard]] LogicalGraphEdge materializeEdge(const MetadataSnapshotPin &snapshot,
																	   const PageBackedGraphEdge &edge) const;
		[[nodiscard]] std::string resolveNodeLabelName(const MetadataSnapshotPin &snapshot, uint64_t tokenId) const;
		[[nodiscard]] std::string resolveRelationshipTypeName(const MetadataSnapshotPin &snapshot,
																				uint64_t tokenId) const;
		[[nodiscard]] std::string resolvePropertyKeyName(const MetadataSnapshotPin &snapshot, uint32_t tokenId) const;
		// Caller holds name_cache_mutex_. A cache entry is never shared across
		// metadata generations, so a pinned older snapshot cannot observe a name
		// resolved from a later System Catalog generation.
		void alignNameCacheGeneration(uint64_t generation) const;
		struct TokenNameCacheEntry {
			uint64_t token_id = 0U;
			std::string name;
		};
		// The cache is an opportunistic direct map, not a catalog. A collision
		// simply resolves the name again, keeping memory bounded for huge schemas.
		static constexpr size_t NAME_CACHE_SLOT_COUNT = 256U;
		using TokenNameCache = std::array<TokenNameCacheEntry, NAME_CACHE_SLOT_COUNT>;

		PageBackedGraphReadAdapter &pages_;
		const PageBackedGraphNameResolver &names_;
		mutable std::mutex name_cache_mutex_;
		mutable uint64_t name_cache_generation_ = 0U;
		mutable TokenNameCache node_label_names_{};
		mutable TokenNameCache relationship_type_names_{};
		mutable TokenNameCache property_key_names_{};
		friend class PageBackedLogicalGraphAdjacencyCursor;
	};

} // namespace graph::storage
