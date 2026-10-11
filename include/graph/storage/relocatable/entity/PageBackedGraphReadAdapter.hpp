/**
 * @file PageBackedGraphReadAdapter.hpp
 * @brief Logical graph records reconstructed from native Entity pages.
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
#include <vector>

#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"

namespace graph::storage {

	class RelocatableMetadataStore;
	class PropertyCollectionRootStore;
	class PropertyCollectionRootScanCursor;
	class BTreePageStore;
	struct EntityPropertyKey;

	// These values form the graph-facing view of a native page record. Logical
	// identity belongs to the Entity Map, so neither type retains a PageId, slot,
	// generation, or legacy fixed-record Property reference.
	struct PageBackedGraphNode {
		uint64_t node_id = 0U;
		std::vector<uint64_t> label_token_ids;
		std::vector<PropertyCollectionRootEntry> properties;

		bool operator==(const PageBackedGraphNode &) const = default;
	};

	struct PageBackedGraphEdge {
		uint64_t edge_id = 0U;
		uint64_t source_node_id = 0U;
		uint64_t target_node_id = 0U;
		uint64_t type_id = 0U;
		std::vector<PropertyCollectionRootEntry> properties;

		bool operator==(const PageBackedGraphEdge &) const = default;
	};

	// Prefixes keep this internal enum safe from Windows macro collisions.
	enum class PageBackedGraphReadDirection : uint8_t {
		PBGRD_OUTGOING = 1,
		PBGRD_INCOMING = 2,
	};

	// A continuation is entirely logical. `previous_edge_id` and `next_edge_id`
	// are adjacent in the immutable sorted Edge-ID result under one pinned
	// snapshot; neither is a physical page location or Edge-record link.
	struct PageBackedGraphAdjacencyContinuation {
		uint64_t previous_edge_id = 0U;
		uint64_t next_edge_id = 0U;

		constexpr bool operator==(const PageBackedGraphAdjacencyContinuation &) const = default;
	};

	struct PageBackedGraphNodePage {
		std::vector<PageBackedGraphNode> nodes;
		bool may_have_more = false;
		// Counts only Entity Map metadata pages; Entity record frames are data-page
		// reads and remain independently governed by the buffer pool.
		size_t metadata_pages_visited = 0U;

		bool operator==(const PageBackedGraphNodePage &) const = default;
	};

	struct PageBackedGraphAdjacencyPage {
		std::vector<PageBackedGraphEdge> edges;
		std::optional<PageBackedGraphAdjacencyContinuation> continuation;

		bool operator==(const PageBackedGraphAdjacencyPage &) const = default;
	};

	class PageBackedGraphReadAdapter;

	// This cursor is an implementation detail of one logical read snapshot. It
	// keeps no physical B+Tree position; each page resumes from the logical
	// Edge-ID continuation under the caller's pinned snapshot.
	class PageBackedGraphAdjacencyCursor final {
	public:
		~PageBackedGraphAdjacencyCursor();
		PageBackedGraphAdjacencyCursor(const PageBackedGraphAdjacencyCursor &) = delete;
		PageBackedGraphAdjacencyCursor &operator=(const PageBackedGraphAdjacencyCursor &) = delete;
		PageBackedGraphAdjacencyCursor(PageBackedGraphAdjacencyCursor &&) noexcept;
		PageBackedGraphAdjacencyCursor &operator=(PageBackedGraphAdjacencyCursor &&) noexcept;

		[[nodiscard]] PageBackedGraphAdjacencyPage
		next(const MetadataSnapshotPin &snapshot, std::optional<PageBackedGraphAdjacencyContinuation> continuation,
			 size_t maximumRecords);

	private:
		friend class PageBackedGraphReadAdapter;
		PageBackedGraphAdjacencyCursor(const PageBackedGraphReadAdapter &adapter, uint64_t nodeId,
									   PageBackedGraphReadDirection direction,
									   std::unique_ptr<PropertyCollectionRootScanCursor> propertyCursor) :
			adapter_(&adapter), node_id_(nodeId), direction_(direction),
			property_cursor_(std::move(propertyCursor)) {}

		const PageBackedGraphReadAdapter *adapter_ = nullptr;
		uint64_t node_id_ = 0U;
		PageBackedGraphReadDirection direction_ = PageBackedGraphReadDirection::PBGRD_OUTGOING;
		std::unique_ptr<PropertyCollectionRootScanCursor> property_cursor_;
	};

	// The adapter is deliberately read-only. Write-side publication keeps page
	// mutations, Entity Map updates, Property changes, and index plans together
	// in the native graph transaction rather than exposing physical records here.
	class PageBackedGraphReadAdapter final {
	public:
		PageBackedGraphReadAdapter(RelocatableMetadataStore &metadata, EntityPageStore &entities,
								   EntityLabelSetStore &labels, PropertyCollectionRootStore &properties,
								   BTreePageStore &indexes,
								   size_t maximumAdjacencyEdges = 10'000'000U);

		[[nodiscard]] PageBackedGraphNode readNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId) const;
		[[nodiscard]] PageBackedGraphEdge readEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId) const;
		// These scans consume Entity Map entries directly through EntityPageStore.
		// They retain only logical IDs above the storage boundary and do not probe
		// physical page order or legacy fixed records.
		[[nodiscard]] bool visitNodesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
										   const std::function<bool(const PageBackedGraphNode &)> &visitor) const;
		[[nodiscard]] bool visitEdgesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
										   const std::function<bool(const PageBackedGraphEdge &)> &visitor) const;
		// Streams durable Edge topology without opening the optional Property root.
		// It is valid only for callers whose semantics do not inspect Properties.
		[[nodiscard]] bool visitEdgeTopologiesUntil(
				const MetadataSnapshotPin &snapshot, size_t maximumRecords,
				const std::function<bool(uint64_t edgeId, const EdgeEntityRecord &)> &visitor) const;
		[[nodiscard]] PageBackedGraphNodePage scanNodesAfter(const MetadataSnapshotPin &snapshot, uint64_t afterNodeId,
															 size_t maximumRecords, size_t maximumMetadataPages) const;
		[[nodiscard]] PageBackedGraphAdjacencyPage
		scanAdjacentEdges(const MetadataSnapshotPin &snapshot, uint64_t nodeId, PageBackedGraphReadDirection direction,
						  std::optional<PageBackedGraphAdjacencyContinuation> continuation,
						  size_t maximumRecords) const;
		// Builds a checked native cursor for a new logical adjacency scan. Callers
		// must retain the same MetadataSnapshotPin and resume only through the
		// returned logical continuation.
		[[nodiscard]] PageBackedGraphAdjacencyCursor
		openAdjacentEdgeCursor(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
							   PageBackedGraphReadDirection direction) const;
		[[nodiscard]] bool
		visitAdjacentEdgesUntil(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
								PageBackedGraphReadDirection direction, size_t maximumRecords,
								const std::function<bool(const PageBackedGraphEdge &)> &visitor) const;
		// Streams only endpoint/type records and never opens the optional Edge
		// Property root. The caller must not use this path for Property predicates.
		[[nodiscard]] bool
		visitAdjacentEdgeTopologiesUntil(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
										 PageBackedGraphReadDirection direction, size_t maximumRecords,
										 const std::function<bool(uint64_t, const EdgeEntityRecord &)> &visitor) const;

	private:
		friend class PageBackedGraphAdjacencyCursor;
		[[nodiscard]] std::vector<PropertyCollectionRootEntry>
		readProperties(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
					   PropertyCollectionRootScanCursor *scanCursor) const;
		[[nodiscard]] PageBackedGraphNode materializeNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
														  const NodeEntityRecord &record,
														  PropertyCollectionRootScanCursor *scanCursor) const;
		[[nodiscard]] PageBackedGraphEdge materializeEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
																	  const EdgeEntityRecord &record,
																	  PropertyCollectionRootScanCursor *scanCursor) const;
		[[nodiscard]] PageBackedGraphAdjacencyPage
		scanAdjacentEdges(const MetadataSnapshotPin &snapshot, uint64_t nodeId, PageBackedGraphReadDirection direction,
							  std::optional<PageBackedGraphAdjacencyContinuation> continuation, size_t maximumRecords,
							  PropertyCollectionRootScanCursor *propertyCursor) const;

		RelocatableMetadataStore &metadata_;
		EntityPageStore &entities_;
		EntityLabelSetStore &labels_;
		PropertyCollectionRootStore &properties_;
		GlobalAdjacencyIndex adjacency_;
		size_t maximum_adjacency_edges_ = 0U;
	};

} // namespace graph::storage
