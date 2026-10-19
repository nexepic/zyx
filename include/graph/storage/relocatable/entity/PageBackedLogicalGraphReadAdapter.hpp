/**
 * @file PageBackedLogicalGraphReadAdapter.hpp
 * @brief Query-facing logical records materialized from native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
		// Native implementations pre-admit decoded name and forward-key storage.
		// Compatibility resolvers reconcile capacity after their existing lookup.
		[[nodiscard]] virtual std::string resolveRelationshipTypeTokenWithNameAdmission(
			const MetadataSnapshotPin &snapshot, uint64_t tokenId, const std::function<void(size_t)> &admission) const {
			auto name = resolveRelationshipTypeToken(snapshot, tokenId);
			if (admission) admission(name.capacity());
			return name;
		}
		[[nodiscard]] virtual std::string resolvePropertyKey(const MetadataSnapshotPin &snapshot,
																		 uint32_t tokenId) const = 0;
		// Optional bounded prefetch into the existing snapshot-local name cache.
		// Compatibility resolvers retain their individual lookup behavior.
		virtual void prefetchPropertyKeyNames(const MetadataSnapshotPin &, std::span<const uint32_t>,
			const std::function<void(uint32_t, std::string_view)> &) const {}
		[[nodiscard]] virtual std::string resolvePropertyKeyWithNameAdmission(const MetadataSnapshotPin &snapshot,
			uint32_t tokenId, const std::function<void(size_t)> &admission) const {
			auto name = resolvePropertyKey(snapshot, tokenId);
			if (admission) admission(name.capacity());
			return name;
		}
		[[nodiscard]] virtual std::optional<uint32_t> resolvePropertyToken(const MetadataSnapshotPin &,
																			 std::string_view) const { return std::nullopt; }
		[[nodiscard]] virtual std::optional<uint64_t> resolveNodeLabelName(const MetadataSnapshotPin &,
																		  std::string_view) const { return std::nullopt; }
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
		PageBackedLogicalGraphAdjacencyCursor(PageBackedLogicalGraphAdjacencyCursor &&) noexcept;
		PageBackedLogicalGraphAdjacencyCursor &operator=(PageBackedLogicalGraphAdjacencyCursor &&) noexcept;

		[[nodiscard]] LogicalGraphAdjacencyPage
		next(const MetadataSnapshotPin &snapshot, std::optional<LogicalGraphAdjacencyContinuation> continuation,
			 size_t maximumRecords);

	private:
		friend class PageBackedLogicalGraphReadAdapter;
		PageBackedLogicalGraphAdjacencyCursor(const PageBackedLogicalGraphReadAdapter &adapter,
											 PageBackedGraphAdjacencyCursor native, NativeReadSession *session,
											 std::unique_ptr<NativeReadSession> ownedSession) :
			adapter_(&adapter), native_(std::move(native)), session_(session), owned_session_(std::move(ownedSession)) {}

		const PageBackedLogicalGraphReadAdapter *adapter_ = nullptr;
		std::optional<PageBackedGraphAdjacencyCursor> native_;
		NativeReadSession *session_ = nullptr;
		std::unique_ptr<NativeReadSession> owned_session_;
	};

	// This adapter is intentionally separate from PageBackedGraphReadAdapter:
	// the lower adapter carries only durable tokens, while this one owns the
	// query-facing conversion and exposes direct owner-keyed Property entries.
	// No method emits a Property ID or physical page location.
	class PageBackedLogicalGraphReadAdapter final {
	public:
		PageBackedLogicalGraphReadAdapter(PageBackedGraphReadAdapter &pages, const PageBackedGraphNameResolver &names);

		[[nodiscard]] LogicalGraphNode readNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																											 NativeReadSession *session = nullptr) const;
		[[nodiscard]] LogicalGraphNode readNodeShaped(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																	 const EntityReadShape &shape,
																	 NativeReadSession *session = nullptr) const;
		// Admission covers encoded Property entries before decoding; labels, names,
		// container capacity and the independently bounded session cache are excluded.
		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
									  const EntityReadShape &shape, NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr) const;
		[[nodiscard]] LogicalGraphEdge readEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
																																																				 NativeReadSession *session = nullptr) const;
		[[nodiscard]] LogicalGraphEdge readEdgeShaped(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
																	 const EntityReadShape &shape,
																	 NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> edgeIds,
									  const EntityReadShape &shape, NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr) const;
		void readNodePropertyCellsSorted(const MetadataSnapshotPin &, std::span<const uint64_t>,
			std::span<const std::string_view>, const LogicalGraphReadSnapshot::PropertyCellSink &,
			NativeReadSession *, const SelectedPropertyReadAdmission *) const;
		[[nodiscard]] PropertyValue readNodeProperty(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																																				 std::string_view key, NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool nodePropertyExists(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																																				 std::string_view key, NativeReadSession *session = nullptr, bool requireNonNull = false) const;
		[[nodiscard]] std::vector<PropertyValue>
		readNodePropertiesSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
									 std::string_view key, NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
															 std::span<const std::string_view> keys,
															 NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr) const;
		[[nodiscard]] PropertyValue readEdgeProperty(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
																																				 std::string_view key, NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool edgePropertyExists(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
																																				 std::string_view key, NativeReadSession *session = nullptr, bool requireNonNull = false) const;
		[[nodiscard]] std::vector<PropertyValue>
		readEdgePropertiesSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> edgeIds,
									 std::string_view key, NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readEdgePropertyColumnsSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> edgeIds,
															 std::span<const std::string_view> keys,
															 NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr) const;
		[[nodiscard]] bool nodeHasLabels(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
													 const std::vector<std::string> &labels,
													 NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::vector<bool> readNodeLabelMatchesSorted(
				const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
				const std::vector<std::string> &labels, NativeReadSession *session = nullptr) const;
		[[nodiscard]] EntityRef readNodeRef(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																	 const EntityReadShape &shape, NativeReadSession *session = nullptr) const;
		[[nodiscard]] EntityRef readEdgeRef(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
																			 const EntityReadShape &shape, NativeReadSession *session = nullptr) const;
		// Converts a native record already authenticated by an index or support-tree
		// lookup. This avoids reopening the same Entity and Property pages solely
		// to translate its tokens into a query-facing record.
		[[nodiscard]] LogicalGraphNode materializeNativeNode(const MetadataSnapshotPin &snapshot,
																	 const PageBackedGraphNode &node,
																	 NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool visitNodesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
																	   const std::function<bool(const LogicalGraphNode &)> &visitor,
																	   NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool visitNodeRefsUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
														  const std::function<bool(const EntityRef &)> &visitor,
														  NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool visitNodeRefsWithLabelMatchesUntil(
				const MetadataSnapshotPin &snapshot, size_t maximumRecords, const std::vector<std::string> &labels,
				const std::function<bool(const EntityRef &, bool)> &visitor, NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool visitEdgesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
																	   const std::function<bool(const LogicalGraphEdge &)> &visitor,
																	   NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool visitEdgeRefsUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
														  const std::function<bool(const EntityRef &)> &visitor,
														  NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool visitEdgeTopologiesUntil(
				const MetadataSnapshotPin &snapshot, size_t maximumRecords,
				const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor,
				NativeReadSession *session = nullptr) const;
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(const MetadataSnapshotPin &snapshot, uint64_t afterNodeId,
																								  size_t maximumRecords, size_t maximumMetadataPages,
																								  NativeReadSession *session = nullptr) const;
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(const MetadataSnapshotPin &snapshot, uint64_t afterNodeId,
																								  size_t maximumRecords, size_t maximumMetadataPages,
																								  const EntityReadShape &shape,
																								  NativeReadSession *session = nullptr) const;
		[[nodiscard]] LogicalGraphAdjacencyPage
		scanAdjacentEdges(const MetadataSnapshotPin &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
							  std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
							  NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr,
				const std::function<void(size_t)> &retainedBufferAdmission = {}) const;
		[[nodiscard]] LogicalGraphAdjacencyTopologyPage
		scanAdjacentEdgeTopologies(const MetadataSnapshotPin &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
									std::optional<LogicalGraphAdjacencyContinuation> continuation,
									size_t maximumRecords, NativeReadSession *session = nullptr,
									const std::function<void(size_t)> &retainedBufferAdmission = {}) const;
		[[nodiscard]] LogicalGraphAdjacencyReferencePage
		scanAdjacentEdgeRefs(const MetadataSnapshotPin &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
						 std::optional<LogicalGraphAdjacencyContinuation> continuation,
						 size_t maximumRecords, NativeReadSession *session = nullptr,
			const std::function<void(size_t)> &retainedBufferAdmission = {}) const;
		[[nodiscard]] PageBackedLogicalGraphAdjacencyCursor
		openAdjacentEdgeCursor(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
							   LogicalGraphReadDirection direction, NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool visitAdjacentEdgesUntil(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
														   LogicalGraphReadDirection direction, size_t maximumRecords,
														   const std::function<bool(const LogicalGraphEdge &)> &visitor,
														   NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool
		visitAdjacentEdgeTopologiesUntil(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
											 LogicalGraphReadDirection direction, size_t maximumRecords,
											 const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor,
											 NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool visitAdjacentEdgeRefsForNodesUntil(
				const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
				LogicalGraphReadDirection direction, size_t maximumRecords,
				const std::function<bool(const EntityRef &)> &visitor,
				NativeReadSession *session = nullptr,
				const std::function<void(size_t)> &retainedBufferAdmission = {}) const;

	private:
		[[nodiscard]] LogicalGraphNode materializeNode(const MetadataSnapshotPin &snapshot,
																	   const PageBackedGraphNode &node, NativeReadSession &session) const;
		[[nodiscard]] LogicalGraphNode materializeNode(const MetadataSnapshotPin &snapshot,
				PageBackedGraphNode &&value, NativeReadSession &session) const;
		template<typename RawNode>
		[[nodiscard]] LogicalGraphNode materializeNodeImpl(const MetadataSnapshotPin &snapshot,
				RawNode &value, NativeReadSession &session) const;
		[[nodiscard]] LogicalGraphEdge materializeEdge(const MetadataSnapshotPin &snapshot,
																	   const PageBackedGraphEdge &edge, NativeReadSession &session) const;
		[[nodiscard]] LogicalGraphEdge materializeEdge(const MetadataSnapshotPin &snapshot,
				PageBackedGraphEdge &&value, NativeReadSession &session) const;
		template<typename RawEdge>
		[[nodiscard]] LogicalGraphEdge materializeEdgeImpl(const MetadataSnapshotPin &snapshot,
				RawEdge &value, NativeReadSession &session,
				const std::function<void(size_t)> &typeNameAdmission = {},
				const std::function<void(size_t)> &propertySlotsAdmission = {},
				const std::function<void(size_t)> &temporaryAdmission = {}) const;
		[[nodiscard]] std::string resolveNodeLabelName(const MetadataSnapshotPin &snapshot, uint64_t tokenId,
																		   NativeReadSession &session) const;
		[[nodiscard]] std::string resolveRelationshipTypeName(const MetadataSnapshotPin &snapshot,
																							 uint64_t tokenId, NativeReadSession &session,
				const std::function<void(size_t)> &copyAdmission = {}) const;
		[[nodiscard]] std::string resolvePropertyKeyName(const MetadataSnapshotPin &snapshot, uint32_t tokenId,
														 NativeReadSession &session, const std::function<void(size_t)> &copyAdmission = {}) const;
		void prefetchPropertyKeyNames(const MetadataSnapshotPin &snapshot,
			std::span<const PropertyCollectionRootEntry> properties, NativeReadSession &session) const;
		[[nodiscard]] std::optional<std::vector<uint64_t>> resolveNodeLabelTokens(
				const MetadataSnapshotPin &snapshot, std::span<const std::string> labels, NativeReadSession &session) const;

		PageBackedGraphReadAdapter &pages_;
		const PageBackedGraphNameResolver &names_;
		friend class PageBackedLogicalGraphAdjacencyCursor;
	};

} // namespace graph::storage
