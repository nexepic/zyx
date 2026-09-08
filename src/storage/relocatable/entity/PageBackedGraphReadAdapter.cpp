/**
 * @file PageBackedGraphReadAdapter.cpp
 * @brief Logical graph records reconstructed from native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

namespace graph::storage {

	namespace {

		// This is a runtime work bound rather than a B+Tree layout capacity. It
		// keeps visitor traversal responsive to early termination even for a
		// supernode that spans many packed leaves.
		constexpr size_t ADJACENCY_VISIT_BATCH_ENTRIES = 256U;
		constexpr size_t TOPOLOGY_ENTITY_SCAN_THRESHOLD = 512U;

		[[noreturn]] void throwAdapterError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] GlobalAdjacencyDirection globalDirection(PageBackedGraphReadDirection direction) {
			if (direction == PageBackedGraphReadDirection::PBGRD_OUTGOING) {
				return GlobalAdjacencyDirection::GAD_OUT;
			}
			if (direction == PageBackedGraphReadDirection::PBGRD_INCOMING) {
				return GlobalAdjacencyDirection::GAD_IN;
			}
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency scan has an unknown direction");
		}

		[[nodiscard]] uint64_t elapsedNanoseconds(const std::chrono::steady_clock::time_point start) {
			return static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
		}

	} // namespace

	PageBackedGraphAdjacencyCursor::~PageBackedGraphAdjacencyCursor() = default;
	PageBackedGraphAdjacencyCursor::PageBackedGraphAdjacencyCursor(PageBackedGraphAdjacencyCursor &&) noexcept =
			default;
	PageBackedGraphAdjacencyCursor &
	PageBackedGraphAdjacencyCursor::operator=(PageBackedGraphAdjacencyCursor &&) noexcept = default;

	PageBackedGraphAdjacencyPage
	PageBackedGraphAdjacencyCursor::next(const MetadataSnapshotPin &snapshot,
										 std::optional<PageBackedGraphAdjacencyContinuation> continuation,
										 size_t maximumRecords) {
		if (adapter_ == nullptr || property_cursor_ == nullptr) {
			throwAdapterError(FormatError::FERR_INVALID_STATE, "page-backed adjacency cursor has no scan state");
		}
		return adapter_->scanAdjacentEdges(snapshot, node_id_, direction_, continuation, maximumRecords,
																 property_cursor_.get());
	}

	PageBackedGraphReadAdapter::PageBackedGraphReadAdapter(RelocatableMetadataStore &metadata,
																   EntityPageStore &entities, EntityLabelSetStore &labels,
																   PropertyCollectionRootStore &properties,
																   BTreePageStore &indexes,
																   size_t maximumAdjacencyEdges) :
		metadata_(metadata), entities_(entities), labels_(labels), properties_(properties),
		adjacency_(indexes, metadata), maximum_adjacency_edges_(maximumAdjacencyEdges) {
		if (!entities_.usesMetadataStore(metadata_) || !labels_.usesMetadataStore(metadata_) ||
			!properties_.usesMetadataStore(metadata_) || !indexes.usesMetadataStore(metadata_) ||
			maximum_adjacency_edges_ == 0U ||
			maximum_adjacency_edges_ == std::numeric_limits<size_t>::max()) {
			throw std::invalid_argument("page-backed graph read adapter requires stores in one metadata domain");
		}
	}

	std::vector<PropertyCollectionRootEntry>
	PageBackedGraphReadAdapter::readProperties(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &key,
											   PropertyCollectionRootScanCursor *scanCursor) const {
		std::optional<PropertyCollectionRoot> root = scanCursor == nullptr
															 ? properties_.readEntityOptional(snapshot, key)
															 : scanCursor->readEntityOptional(snapshot, key);
		if (!root.has_value()) {
			return {};
		}
		return std::move(root->entries);
	}

	PageBackedGraphNode
	PageBackedGraphReadAdapter::materializeNode(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
												const NodeEntityRecord &record,
												PropertyCollectionRootScanCursor *scanCursor) const {
		return {nodeId, labels_.readNodeLabels(snapshot, record),
				readProperties(snapshot, {EntityPropertyOwnerKind::EPOK_NODE, nodeId}, scanCursor)};
	}

	PageBackedGraphEdge
	PageBackedGraphReadAdapter::materializeEdge(const MetadataSnapshotPin &snapshot, uint64_t edgeId,
												const EdgeEntityRecord &record,
												PropertyCollectionRootScanCursor *scanCursor) const {
		return {edgeId, record.source_node_id, record.target_node_id, record.type_id,
				readProperties(snapshot, {EntityPropertyOwnerKind::EPOK_EDGE, edgeId}, scanCursor)};
	}

	PageBackedGraphNode PageBackedGraphReadAdapter::readNode(const MetadataSnapshotPin &snapshot,
															 uint64_t nodeId) const {
		if (nodeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph Node read requires a nonzero logical Node ID");
		}
		// EntityPageStore resolves NodeId through the snapshot-pinned Entity Map.
		const NodeEntityRecord record = entities_.readNode(snapshot, nodeId);
		return materializeNode(snapshot, nodeId, record, nullptr);
	}

	PageBackedGraphEdge PageBackedGraphReadAdapter::readEdge(const MetadataSnapshotPin &snapshot,
															 uint64_t edgeId) const {
		if (edgeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph Edge read requires a nonzero logical Edge ID");
		}
		// EntityPageStore resolves EdgeId through the snapshot-pinned Entity Map.
		const EdgeEntityRecord record = entities_.readEdge(snapshot, edgeId);
		return materializeEdge(snapshot, edgeId, record, nullptr);
	}

	bool
	PageBackedGraphReadAdapter::visitNodesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
												const std::function<bool(const PageBackedGraphNode &)> &visitor) const {
		if (maximumRecords == 0U || !visitor) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph Node scan requires a nonzero record bound and visitor");
		}
		PropertyCollectionRootScanCursor propertyCursor = properties_.openScanCursor(snapshot);
		return entities_.visitNodesUntil(
				snapshot, maximumRecords,
				[this, &snapshot, &visitor, &propertyCursor](uint64_t nodeId, const NodeEntityRecord &record) {
					return visitor(materializeNode(snapshot, nodeId, record, &propertyCursor));
				});
	}

	bool
	PageBackedGraphReadAdapter::visitEdgesUntil(const MetadataSnapshotPin &snapshot, size_t maximumRecords,
												const std::function<bool(const PageBackedGraphEdge &)> &visitor) const {
		if (maximumRecords == 0U || !visitor) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph Edge scan requires a nonzero record bound and visitor");
		}
		debug::ScopedPerfTimer timer("native_graph.read.edge_native_scan");
		const bool tracing = debug::PerfTrace::isEnabled();
		uint64_t materializationNanoseconds = 0U;
		uint64_t visitorNanoseconds = 0U;
		uint64_t records = 0U;
		PropertyCollectionRootScanCursor propertyCursor = properties_.openScanCursor(snapshot);
		const bool exhausted = entities_.visitEdgesUntil(
				snapshot, maximumRecords,
				[this, &snapshot, &visitor, &propertyCursor, tracing, &materializationNanoseconds, &visitorNanoseconds,
				 &records](uint64_t edgeId, const EdgeEntityRecord &record) {
					const auto materializationStart = tracing ? std::chrono::steady_clock::now()
																	 : std::chrono::steady_clock::time_point{};
					PageBackedGraphEdge edge = materializeEdge(snapshot, edgeId, record, &propertyCursor);
					if (tracing) {
						materializationNanoseconds += elapsedNanoseconds(materializationStart);
					}
					const auto visitorStart = tracing ? std::chrono::steady_clock::now()
																	 : std::chrono::steady_clock::time_point{};
					const bool keepGoing = visitor(edge);
					if (tracing) {
						visitorNanoseconds += elapsedNanoseconds(visitorStart);
						++records;
					}
					return keepGoing;
				});
		if (tracing) {
			debug::PerfTrace::addDurationBatch("native_graph.read.edge_native_materialization",
											 materializationNanoseconds, records);
			debug::PerfTrace::addDurationBatch("native_graph.read.edge_logical_visitor", visitorNanoseconds, records);
		}
		return exhausted;
	}

	bool PageBackedGraphReadAdapter::visitEdgeTopologiesUntil(
			const MetadataSnapshotPin &snapshot, size_t maximumRecords,
			const std::function<bool(uint64_t edgeId, const EdgeEntityRecord &)> &visitor) const {
		if (maximumRecords == 0U || !visitor) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph Edge topology scan requires a nonzero record bound and visitor");
		}
		return entities_.visitEdgesUntil(snapshot, maximumRecords, visitor);
	}

	PageBackedGraphNodePage PageBackedGraphReadAdapter::scanNodesAfter(const MetadataSnapshotPin &snapshot,
																	   uint64_t afterNodeId, size_t maximumRecords,
																	   size_t maximumMetadataPages) const {
		if (maximumRecords == 0U || maximumMetadataPages == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph resumable Node scan requires positive bounds");
		}
		PageBackedGraphNodePage result;
		size_t metadataPagesVisited = 0U;
		PropertyCollectionRootScanCursor propertyCursor = properties_.openScanCursor(snapshot);
		const bool exhausted = entities_.visitNodesAfter(
				snapshot, afterNodeId, maximumRecords, maximumMetadataPages,
				[this, &snapshot, &result, &propertyCursor](uint64_t nodeId, const NodeEntityRecord &record) {
					result.nodes.push_back(materializeNode(snapshot, nodeId, record, &propertyCursor));
					return true;
				},
				&metadataPagesVisited);
		result.may_have_more = !exhausted;
		result.metadata_pages_visited = metadataPagesVisited;
		return result;
	}

	PageBackedGraphAdjacencyPage PageBackedGraphReadAdapter::scanAdjacentEdges(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, PageBackedGraphReadDirection direction,
			std::optional<PageBackedGraphAdjacencyContinuation> continuation, size_t maximumRecords) const {
		return scanAdjacentEdges(snapshot, nodeId, direction, continuation, maximumRecords, nullptr);
	}

	PageBackedGraphAdjacencyPage PageBackedGraphReadAdapter::scanAdjacentEdges(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, PageBackedGraphReadDirection direction,
			std::optional<PageBackedGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			PropertyCollectionRootScanCursor *propertyCursor) const {
		if (nodeId == 0U || maximumRecords == 0U || maximumRecords == std::numeric_limits<size_t>::max()) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency scan requires a Node ID and positive record bound");
		}
		const GlobalAdjacencyDirection adjacencyDirection = globalDirection(direction);
		if (continuation.has_value() && (continuation->previous_edge_id == 0U || continuation->next_edge_id == 0U)) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency continuation has a zero Edge ID");
		}

		static_cast<void>(entities_.readNode(snapshot, nodeId));
		std::optional<GlobalAdjacencyContinuation> adjacencyContinuation;
		if (continuation.has_value()) {
			adjacencyContinuation = {snapshot.superblock().generation, adjacencyDirection, nodeId,
									 continuation->previous_edge_id};
		}
		const size_t boundedRecords = std::min(maximumRecords, maximum_adjacency_edges_);
		if (boundedRecords == 0U) { // constructor validation keeps both bounds positive.
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency scan has an invalid configured edge bound");
		}
		const GlobalAdjacencyRange edgeIds = adjacency_.scan(snapshot, adjacencyDirection, nodeId, boundedRecords + 1U,
																									 adjacencyContinuation);
		if (continuation.has_value() &&
			(edgeIds.edge_ids.empty() || edgeIds.edge_ids.front() != continuation->next_edge_id)) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency continuation does not match the global adjacency range");
		}
		const auto endpointMatches = [nodeId, direction](const EdgeEntityRecord &edge) {
			return direction == PageBackedGraphReadDirection::PBGRD_OUTGOING ? edge.source_node_id == nodeId
																			 : edge.target_node_id == nodeId;
		};
		std::optional<PropertyCollectionRootScanCursor> localPropertyCursor;
		if (propertyCursor == nullptr) {
			localPropertyCursor.emplace(properties_.openScanCursor(snapshot));
			propertyCursor = &*localPropertyCursor;
		}
		PageBackedGraphAdjacencyPage result;
		const size_t resultCount = std::min(boundedRecords, edgeIds.edge_ids.size());
		result.edges.reserve(resultCount);
		for (size_t ordinal = 0U; ordinal < resultCount; ++ordinal) {
			const uint64_t edgeId = edgeIds.edge_ids[ordinal];
			const EdgeEntityRecord edge = entities_.readEdge(snapshot, edgeId);
			if (!endpointMatches(edge)) { // adjacency and Entity Map records share one pinned publication.
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed graph adjacency record has an inconsistent endpoint");
			}
			result.edges.push_back(materializeEdge(snapshot, edgeId, edge, propertyCursor));
		}
		if (edgeIds.edge_ids.size() > resultCount) {
			if (result.edges.empty()) { // a continuation requires a positive bounded result count.
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed adjacency scan cannot return a continuation without an Edge");
			}
			result.continuation = PageBackedGraphAdjacencyContinuation{result.edges.back().edge_id,
																												   edgeIds.edge_ids[resultCount]};
		}
		return result;
	}

	PageBackedGraphAdjacencyCursor
	PageBackedGraphReadAdapter::openAdjacentEdgeCursor(const MetadataSnapshotPin &snapshot, uint64_t nodeId,
													   PageBackedGraphReadDirection direction) const {
		if (nodeId == 0U) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency cursor requires a nonzero Node ID");
		}
		if (direction != PageBackedGraphReadDirection::PBGRD_OUTGOING &&
			direction != PageBackedGraphReadDirection::PBGRD_INCOMING) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency cursor has an unknown direction");
		}
		static_cast<void>(entities_.readNode(snapshot, nodeId));
		static_cast<void>(globalDirection(direction));
		return PageBackedGraphAdjacencyCursor(
				*this, nodeId, direction,
				std::make_unique<PropertyCollectionRootScanCursor>(properties_.openScanCursor(snapshot)));
	}

	bool PageBackedGraphReadAdapter::visitAdjacentEdgesUntil(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, PageBackedGraphReadDirection direction,
			size_t maximumRecords, const std::function<bool(const PageBackedGraphEdge &)> &visitor) const {
		if (maximumRecords == 0U || !visitor) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency traversal requires a nonzero record bound and visitor");
		}

		PageBackedGraphAdjacencyCursor cursor = openAdjacentEdgeCursor(snapshot, nodeId, direction);
		std::optional<PageBackedGraphAdjacencyContinuation> continuation;
		size_t remaining = maximumRecords;
		for (;;) {
			const size_t batchLimit = std::min(remaining, ADJACENCY_VISIT_BATCH_ENTRIES);
			const PageBackedGraphAdjacencyPage page = cursor.next(snapshot, continuation, batchLimit);
			for (const PageBackedGraphEdge &edge: page.edges) {
				if (!visitor(edge)) {
					return false;
				}
			}
			if (!page.continuation.has_value()) {
				return true;
			}
			if (page.edges.empty() || page.edges.size() > remaining) { // the internal cursor returns only bounded nonempty continuation pages.
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed graph adjacency cursor returned an invalid bounded page");
			}
			remaining -= page.edges.size();
			if (remaining == 0U) {
				throwAdapterError(FormatError::FERR_INVALID_STATE,
								  "page-backed graph adjacency traversal exceeded its caller-supplied edge bound");
			}
			continuation = page.continuation;
		}
	}

	bool PageBackedGraphReadAdapter::visitAdjacentEdgeTopologiesUntil(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, PageBackedGraphReadDirection direction,
			size_t maximumRecords, const std::function<bool(uint64_t, const EdgeEntityRecord &)> &visitor) const {
		if (nodeId == 0U || maximumRecords == 0U || maximumRecords == std::numeric_limits<size_t>::max() || !visitor) {
			throwAdapterError(FormatError::FERR_INVALID_HEADER,
							  "page-backed graph adjacency topology traversal requires a Node ID, bound, and visitor");
		}
		const GlobalAdjacencyDirection adjacencyDirection = globalDirection(direction);
		static_cast<void>(entities_.readNode(snapshot, nodeId));
		const GlobalAdjacencyRange edgeIds = adjacency_.scan(snapshot, adjacencyDirection, nodeId, maximumRecords);
		if (edgeIds.continuation.has_value()) {
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency topology exceeded its caller-supplied edge bound");
		}
		if (edgeIds.edge_ids.size() <= TOPOLOGY_ENTITY_SCAN_THRESHOLD) {
			for (const uint64_t edgeId: edgeIds.edge_ids) {
				const EdgeEntityRecord edge = entities_.readEdge(snapshot, edgeId);
				const bool endpointMatches = direction == PageBackedGraphReadDirection::PBGRD_OUTGOING
													 ? edge.source_node_id == nodeId
													 : edge.target_node_id == nodeId;
				if (!endpointMatches) { // adjacency and Entity Map records share one pinned publication.
					throwAdapterError(FormatError::FERR_INVALID_STATE,
									  "page-backed graph adjacency topology has an inconsistent endpoint");
				}
				if (!visitor(edgeId, edge)) {
					return false;
				}
			}
			return true;
		}

		// Large topology-only counts are sorted by Edge ID in both directories. A
		// single sequential Entity Map pass avoids one metadata lookup per Edge,
		// while the adjacency result remains bounded by the caller.
		size_t requested = 0U;
		bool stoppedByVisitor = false;
		static_cast<void>(entities_.visitEdgesUntil(
				snapshot, std::numeric_limits<size_t>::max(), [&](uint64_t edgeId, const EdgeEntityRecord &edge) {
					if (requested == edgeIds.edge_ids.size()) { // the callback stops after accepting the final requested Edge.
						return false;
					}
					const uint64_t wanted = edgeIds.edge_ids[requested];
					if (edgeId < wanted) {
						return true;
					}
					if (edgeId > wanted) { // the immutable Entity Map contains every published adjacency Edge.
						throwAdapterError(FormatError::FERR_INVALID_STATE,
										  "page-backed graph adjacency topology references a missing Edge");
					}
					const bool endpointMatches = direction == PageBackedGraphReadDirection::PBGRD_OUTGOING
														 ? edge.source_node_id == nodeId
														 : edge.target_node_id == nodeId;
					if (!endpointMatches) { // adjacency and Entity Map records share one pinned publication.
						throwAdapterError(FormatError::FERR_INVALID_STATE,
										  "page-backed graph adjacency topology has an inconsistent endpoint");
					}
					if (!visitor(edgeId, edge)) {
						stoppedByVisitor = true;
						return false;
					}
					++requested;
					return requested < edgeIds.edge_ids.size();
				}));
		if (stoppedByVisitor) {
			return false;
		}
		if (requested != edgeIds.edge_ids.size()) { // the immutable Entity Map contains every published adjacency Edge.
			throwAdapterError(FormatError::FERR_INVALID_STATE,
							  "page-backed graph adjacency topology Entity Map is missing an Edge");
		}
		return true;
	}

} // namespace graph::storage
