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
#include <span>
#include <utility>
#include <vector>

#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeReadSession.hpp"
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

	struct PageBackedGraphAdjacencyTopologyPage {
		std::vector<EdgeEntityRecord> edges;
		std::vector<uint64_t> edge_ids;
		std::optional<PageBackedGraphAdjacencyContinuation> continuation;

		bool operator==(const PageBackedGraphAdjacencyTopologyPage &) const = default;
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
										   std::unique_ptr<PropertyCollectionRootScanCursor> propertyCursor,
										   NativeReadSession *session,
										   std::unique_ptr<NativeReadSession> ownedSession) :
			adapter_(&adapter), node_id_(nodeId), direction_(direction),
							property_cursor_(std::move(propertyCursor)), session_(session),
							owned_session_(std::move(ownedSession)) {}

		const PageBackedGraphReadAdapter *adapter_ = nullptr;
		uint64_t node_id_ = 0U;
		PageBackedGraphReadDirection direction_ = PageBackedGraphReadDirection::PBGRD_OUTGOING;
		std::unique_ptr<PropertyCollectionRootScanCursor> property_cursor_;
		NativeReadSession *session_ = nullptr;
		std::unique_ptr<NativeReadSession> owned_session_;
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

		[[nodiscard]] PageBackedGraphNode readNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																		  NativeReadSession *session = nullptr) const;
		[[nodiscard]] NodeEntityRecord readNodeRecord(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																		 NativeReadSession *session = nullptr) const;
		// Resolves strictly increasing logical IDs with one Entity-Map batch. The
		// returned records contain no physical locations and are cached only in the
		// caller's snapshot-bound session.
		[[nodiscard]] std::vector<NodeEntityRecord>
		readNodeRecordsSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
								  NativeReadSession *session = nullptr) const;
		// Reads the Node's authenticated Property root without opening its label
		// set. Shape-aware logical reads use this when labels are not requested.
		[[nodiscard]] std::vector<PropertyCollectionRootEntry>
		readNodeProperties(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																	 NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr) const;
		// Token enumeration authenticates the Node or Edge before opening its
		// property directory and never decodes PropertyValue payloads.
		[[nodiscard]] std::vector<uint32_t> readEntityPropertyTokens(
				const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
				const SelectedPropertyReadAdmission &tokenAdmission,
				const SelectedPropertyReadAdmission &transientAdmission,
				NativeReadSession *session = nullptr,
				const CancellationCheckpoint *cancellation = nullptr) const;
		// Reads complete Node Property roots in logical-ID order through one
		// snapshot-bound Property Directory cursor. Missing roots produce empty
		// vectors while malformed locations still fail closed.
		[[nodiscard]] std::vector<std::vector<PropertyCollectionRootEntry>>
		readNodePropertiesSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
																	 NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr) const;
		// Returns compact label tokens without opening the Node Property root.
		[[nodiscard]] std::vector<uint64_t> readNodeLabelTokens(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																																								   NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::vector<std::vector<uint64_t>> readNodeLabelTokensSorted(
				const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
				NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::optional<PropertyValue> readNodeProperty(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																																								 uint32_t propertyToken,
																																								 NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool nodePropertyExists(const MetadataSnapshotPin &snapshot, uint64_t nodeId, uint32_t propertyToken,
																											 NativeReadSession *session = nullptr, bool requireNonNull = false) const;
		// Reads one selected Property for strictly ascending Node IDs through the
		// Property-directory multi-lookup path. Output positions match input IDs.
		[[nodiscard]] std::vector<std::optional<PropertyValue>>
		readNodePropertiesSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
																								 uint32_t propertyToken,
																								 NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::vector<std::vector<std::optional<PropertyValue>>>
		readNodePropertyColumnsSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
																																					 std::span<const uint32_t> propertyTokens,
																																				 NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr) const;
		// Reads selected Node Property columns and their authenticated root counts
		// through one ordered Property Directory merge.
		[[nodiscard]] std::pair<std::vector<std::vector<std::optional<PropertyValue>>>,
													std::vector<std::optional<size_t>>>
		readNodePropertyColumnsAndCountsSorted(const MetadataSnapshotPin &snapshot,
																																 std::span<const uint64_t> nodeIds,
																																	 std::span<const uint32_t> propertyTokens,
																																				 NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr,
				const SelectedPropertyReadAdmission *transientBufferAdmission = nullptr) const;
		// Reads only the authenticated Property-root framing used to enforce a
		// caller-supplied bound. It deliberately does not decode Property values.
		[[nodiscard]] size_t nodePropertyCount(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																									 NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::vector<std::optional<size_t>> nodePropertyCountsSorted(
																		const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
																		NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool nodeHasLabelTokens(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
													 std::span<const uint64_t> labelTokens,
													 NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::vector<bool> readNodeLabelMatchesSorted(
				const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
				std::span<const uint64_t> labelTokens, NativeReadSession *session = nullptr,
			const SelectedPropertyReadAdmission *entityBufferAdmission = nullptr) const;
		[[nodiscard]] std::optional<PropertyValue> readEdgeProperty(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
																																								 uint32_t propertyToken,
																																								 NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool edgePropertyExists(const MetadataSnapshotPin &snapshot, uint64_t edgeId, uint32_t propertyToken,
																											 NativeReadSession *session = nullptr, bool requireNonNull = false) const;
		// Edge IDs use the same ordered selected-read primitive under their distinct
		// owner namespace, so Node and Edge property batches never alias.
		[[nodiscard]] std::vector<std::optional<PropertyValue>>
		readEdgePropertiesSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> edgeIds,
																								 uint32_t propertyToken,
																								 NativeReadSession *session = nullptr) const;
		[[nodiscard]] std::vector<std::vector<std::optional<PropertyValue>>>
		readEdgePropertyColumnsSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> edgeIds,
																								 std::span<const uint32_t> propertyTokens,
																								 NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr) const;
		// Verifies an index candidate through fixed-record labels and only the
		// requested Property tokens. It deliberately avoids constructing a full
		// graph-facing Node for index covered predicates.
		void validateNodeExactProperties(const MetadataSnapshotPin &snapshot, uint64_t nodeId, uint64_t labelToken,
															 std::span<const uint32_t> propertyTokens,
															 std::span<const PropertyValue> expectedValues,
															 size_t maximumPropertyEntries,
															 NativeReadSession *session = nullptr) const;
		// Validates a strictly ascending candidate batch with one ordered Property
		// Directory merge. A valid B+Tree page authenticates its own bytes, not the
		// cross-record relationship to the authoritative Property collection, so the
		// value comparison remains mandatory even for an exact index lookup.
		void validateNodeExactPropertiesSorted(const MetadataSnapshotPin &snapshot,
															   std::span<const uint64_t> nodeIds, uint64_t labelToken,
															   std::span<const uint32_t> propertyTokens,
															   std::span<const PropertyValue> expectedValues,
															   size_t maximumPropertyEntries,
															   NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr,
				const SelectedPropertyReadAdmission *transientBufferAdmission = nullptr) const;
		[[nodiscard]] PageBackedGraphEdge readEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
																		  NativeReadSession *session = nullptr) const;
		// Internal topology point read used by projection-aware query rows. It
		// intentionally does not open an owner-keyed Property root.
		[[nodiscard]] EdgeEntityRecord readEdgeTopology(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
																						 NativeReadSession *session = nullptr) const;
		// Reads the Edge's authenticated Property root without reopening its fixed
		// Entity record. Complete shaped batches use this to materialize directly
		// from their already-resolved topology records.
		[[nodiscard]] std::vector<PropertyCollectionRootEntry>
		readEdgeProperties(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
																	 NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr) const;
		[[nodiscard]] std::vector<std::vector<PropertyCollectionRootEntry>>
		readEdgePropertiesSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> edgeIds,
																	 NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr) const;
		[[nodiscard]] std::vector<EdgeEntityRecord>
		readEdgeTopologiesSorted(const MetadataSnapshotPin &snapshot, std::span<const uint64_t> edgeIds,
											 NativeReadSession *session = nullptr) const;
		// These scans consume Entity Map entries directly through EntityPageStore.
		// They retain only logical IDs above the storage boundary and do not probe
		// physical page order or legacy fixed records.
		[[nodiscard]] bool visitNodesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
														   const std::function<bool(const PageBackedGraphNode &)> &visitor,
														   NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool visitNodeRefsUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
														  const std::function<bool(const EntityRef &)> &visitor,
														  NativeReadSession *session = nullptr) const;
		// The Entity-page visitor already authenticates each fixed Node record. This
		// variant consumes the same record to test label tokens before exposing a
		// logical reference, avoiding a second Entity-Map point read per candidate.
		[[nodiscard]] bool visitNodeRefsWithLabelTokenMatchesUntil(
				const MetadataSnapshotPin &snapshot, size_t maximumRecords, std::span<const uint64_t> labelTokens,
				const std::function<bool(const EntityRef &, bool)> &visitor, NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool visitEdgesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
														   const std::function<bool(const PageBackedGraphEdge &)> &visitor,
														   NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool visitEdgeRefsUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
														  const std::function<bool(const EntityRef &)> &visitor,
														  NativeReadSession *session = nullptr) const;
		// Streams durable Edge topology without opening the optional Property root.
		// It is valid only for callers whose semantics do not inspect Properties.
		[[nodiscard]] bool visitEdgeTopologiesUntil(
				const MetadataSnapshotPin &snapshot, size_t maximumRecords,
				const std::function<bool(uint64_t edgeId, const EdgeEntityRecord &)> &visitor,
				NativeReadSession *session = nullptr) const;
		[[nodiscard]] PageBackedGraphNodePage scanNodesAfter(const MetadataSnapshotPin &snapshot, uint64_t afterNodeId,
																								 size_t maximumRecords, size_t maximumMetadataPages,
																								 NativeReadSession *session = nullptr) const;
		[[nodiscard]] PageBackedGraphNodePage scanNodesAfter(const MetadataSnapshotPin &snapshot, uint64_t afterNodeId,
																								 size_t maximumRecords, size_t maximumMetadataPages,
																								 const EntityReadShape &shape,
																								 NativeReadSession *session = nullptr) const;
		[[nodiscard]] PageBackedGraphAdjacencyPage
		scanAdjacentEdges(const MetadataSnapshotPin &snapshot, uint64_t nodeId, PageBackedGraphReadDirection direction,
						  std::optional<PageBackedGraphAdjacencyContinuation> continuation,
						  size_t maximumRecords, NativeReadSession *session = nullptr,
				const SelectedPropertyReadAdmission *admission = nullptr,
				const std::function<void(size_t)> &retainedBufferAdmission = {}) const;
		// Absolute live ID/record/output buffer admission, including growth peaks.
		// Entity reader internals, cursor pages and session caches remain separate.
		[[nodiscard]] PageBackedGraphAdjacencyTopologyPage
		scanAdjacentEdgeTopologies(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
									PageBackedGraphReadDirection direction,
									std::optional<PageBackedGraphAdjacencyContinuation> continuation,
									size_t maximumRecords, NativeReadSession *session = nullptr,
									const std::function<void(size_t)> &retainedBufferAdmission = {}) const;
		// Builds a checked native cursor for a new logical adjacency scan. Callers
		// must retain the same MetadataSnapshotPin and resume only through the
		// returned logical continuation.
		[[nodiscard]] PageBackedGraphAdjacencyCursor
		openAdjacentEdgeCursor(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
							   PageBackedGraphReadDirection direction,
							   NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool
		visitAdjacentEdgesUntil(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
								PageBackedGraphReadDirection direction, size_t maximumRecords,
								const std::function<bool(const PageBackedGraphEdge &)> &visitor,
								NativeReadSession *session = nullptr) const;
		// Streams only endpoint/type records and never opens the optional Edge
		// Property root. The caller must not use this path for Property predicates.
		[[nodiscard]] bool
		visitAdjacentEdgeTopologiesUntil(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
																	 PageBackedGraphReadDirection direction, size_t maximumRecords,
																	 const std::function<bool(uint64_t, const EdgeEntityRecord &)> &visitor,
																	 NativeReadSession *session = nullptr) const;
		[[nodiscard]] bool visitAdjacentEdgeRefsForNodesUntil(
				const MetadataSnapshotPin &snapshot, std::span<const uint64_t> nodeIds,
				PageBackedGraphReadDirection direction, size_t maximumRecords,
				const std::function<bool(const EntityRef &)> &visitor,
				NativeReadSession *session = nullptr,
				const std::function<void(size_t)> &retainedBufferAdmission = {}) const;

	private:
		friend class PageBackedGraphAdjacencyCursor;
		[[nodiscard]] std::vector<PropertyCollectionRootEntry>
		readProperties(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
					   PropertyCollectionRootScanCursor *scanCursor,
				const SelectedPropertyReadAdmission *admission = nullptr, const SelectedPropertyReadAdmission *slotsAdmission = nullptr) const;
		[[nodiscard]] PageBackedGraphNode materializeNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
														  const NodeEntityRecord &record,
														  PropertyCollectionRootScanCursor *scanCursor) const;
		[[nodiscard]] PageBackedGraphEdge materializeEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
																	  const EdgeEntityRecord &record,
																	  PropertyCollectionRootScanCursor *scanCursor,
				const SelectedPropertyReadAdmission *admission = nullptr, const SelectedPropertyReadAdmission *slotsAdmission = nullptr) const;
		[[nodiscard]] PageBackedGraphAdjacencyPage
		scanAdjacentEdges(const MetadataSnapshotPin &snapshot, uint64_t nodeId, PageBackedGraphReadDirection direction,
							  std::optional<PageBackedGraphAdjacencyContinuation> continuation, size_t maximumRecords,
							  PropertyCollectionRootScanCursor *propertyCursor, NativeReadSession *session,
				const SelectedPropertyReadAdmission *admission = nullptr,
				const std::function<void(size_t)> &retainedBufferAdmission = {}) const;

		RelocatableMetadataStore &metadata_;
		EntityPageStore &entities_;
		EntityLabelSetStore &labels_;
		PropertyCollectionRootStore &properties_;
		GlobalAdjacencyIndex adjacency_;
		size_t maximum_adjacency_edges_ = 0U;
	};

} // namespace graph::storage
