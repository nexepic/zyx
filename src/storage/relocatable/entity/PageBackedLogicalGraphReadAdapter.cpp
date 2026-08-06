/**
 * @file PageBackedLogicalGraphReadAdapter.cpp
 * @brief Query-facing logical records materialized from native Entity pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/PageBackedLogicalGraphReadAdapter.hpp"

#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void throwLogicalAdapterError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] PageBackedGraphReadDirection pageDirection(LogicalGraphReadDirection direction) {
			switch (direction) {
				case LogicalGraphReadDirection::LGRD_OUTGOING:
					return PageBackedGraphReadDirection::PBGRD_OUTGOING;
				case LogicalGraphReadDirection::LGRD_INCOMING:
					return PageBackedGraphReadDirection::PBGRD_INCOMING;
			}
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									 "page-backed logical graph adapter has an unknown read direction");
		}

		[[nodiscard]] std::optional<PageBackedGraphAdjacencyContinuation>
		pageContinuation(const std::optional<LogicalGraphAdjacencyContinuation> &continuation) {
			if (!continuation.has_value()) {
				return std::nullopt;
			}
			return PageBackedGraphAdjacencyContinuation{continuation->previous_edge_id, continuation->next_edge_id};
		}

		[[nodiscard]] std::optional<LogicalGraphAdjacencyContinuation>
		logicalContinuation(const std::optional<PageBackedGraphAdjacencyContinuation> &continuation) {
			if (!continuation.has_value()) {
				return std::nullopt;
			}
			return LogicalGraphAdjacencyContinuation{continuation->previous_edge_id, continuation->next_edge_id};
		}

	} // namespace

	LogicalGraphAdjacencyPage PageBackedLogicalGraphAdjacencyCursor::next(
			const MetadataSnapshotPin &snapshot, std::optional<LogicalGraphAdjacencyContinuation> continuation,
			size_t maximumRecords) {
		if (adapter_ == nullptr || !native_.has_value()) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									 "page-backed logical adjacency cursor has no native scan state");
		}
		const PageBackedGraphAdjacencyPage raw = native_->next(snapshot, pageContinuation(continuation), maximumRecords);
		LogicalGraphAdjacencyPage result;
		result.edges.reserve(raw.edges.size());
		for (const PageBackedGraphEdge &edge: raw.edges) {
			result.edges.push_back(adapter_->materializeEdge(snapshot, edge));
		}
		result.continuation = logicalContinuation(raw.continuation);
		return result;
	}

	PageBackedLogicalGraphReadAdapter::PageBackedLogicalGraphReadAdapter(PageBackedGraphReadAdapter &pages,
																		 const PageBackedGraphNameResolver &names) :
		pages_(pages), names_(names) {}

	LogicalGraphNode PageBackedLogicalGraphReadAdapter::materializeNode(const MetadataSnapshotPin &snapshot,
																		const PageBackedGraphNode &node) const {
		if (node.node_id == 0U) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
									 "page-backed logical graph adapter received a Node with no logical ID");
		}
		LogicalGraphNode result;
		result.node_id = node.node_id;
		result.labels.reserve(node.label_token_ids.size());
		std::unordered_set<std::string> labelNames;
		labelNames.reserve(node.label_token_ids.size());
		for (const uint64_t tokenId: node.label_token_ids) {
			if (tokenId == 0U) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed logical graph adapter received a zero Node label token");
			}
			std::string name = names_.resolveNodeLabelToken(snapshot, tokenId);
			if (name.empty() || !labelNames.insert(name).second) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed logical graph adapter resolved invalid or duplicate Node labels");
			}
			result.labels.push_back(std::move(name));
		}
		result.properties.reserve(node.properties.size());
		std::unordered_set<std::string> propertyNames;
		propertyNames.reserve(node.properties.size());
		for (const PropertyCollectionRootEntry &property: node.properties) {
			if (property.key_token == 0U) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed logical graph adapter received a zero Node Property token");
			}
			std::string key = names_.resolvePropertyKey(snapshot, property.key_token);
			if (key.empty() || !propertyNames.insert(key).second) {
				throwLogicalAdapterError(
						FormatError::FERR_INVALID_STATE,
						"page-backed logical graph adapter resolved invalid or duplicate Node Properties");
			}
			result.properties.push_back({std::move(key), property.value});
		}
		return result;
	}

	LogicalGraphEdge PageBackedLogicalGraphReadAdapter::materializeEdge(const MetadataSnapshotPin &snapshot,
																		const PageBackedGraphEdge &edge) const {
		if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
			throwLogicalAdapterError(
					FormatError::FERR_INVALID_STATE,
					"page-backed logical graph adapter received an Edge with invalid identity or endpoints");
		}
		LogicalGraphEdge result;
		result.edge_id = edge.edge_id;
		result.source_node_id = edge.source_node_id;
		result.target_node_id = edge.target_node_id;
		if (edge.type_id != 0U) {
			result.type = names_.resolveRelationshipTypeToken(snapshot, edge.type_id);
			if (result.type.empty()) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed logical graph adapter resolved an empty Edge type name");
			}
		}
		result.properties.reserve(edge.properties.size());
		std::unordered_set<std::string> propertyNames;
		propertyNames.reserve(edge.properties.size());
		for (const PropertyCollectionRootEntry &property: edge.properties) {
			if (property.key_token == 0U) {
				throwLogicalAdapterError(FormatError::FERR_INVALID_STATE,
										 "page-backed logical graph adapter received a zero Edge Property token");
			}
			std::string key = names_.resolvePropertyKey(snapshot, property.key_token);
			if (key.empty() || !propertyNames.insert(key).second) {
				throwLogicalAdapterError(
						FormatError::FERR_INVALID_STATE,
						"page-backed logical graph adapter resolved invalid or duplicate Edge Properties");
			}
			result.properties.push_back({std::move(key), property.value});
		}
		return result;
	}

	LogicalGraphNode PageBackedLogicalGraphReadAdapter::readNode(const MetadataSnapshotPin &snapshot,
																 uint64_t nodeId) const {
		return materializeNode(snapshot, pages_.readNode(snapshot, nodeId));
	}

	LogicalGraphEdge PageBackedLogicalGraphReadAdapter::readEdge(const MetadataSnapshotPin &snapshot,
																 uint64_t edgeId) const {
		return materializeEdge(snapshot, pages_.readEdge(snapshot, edgeId));
	}

	bool PageBackedLogicalGraphReadAdapter::visitNodesUntil(
			const MetadataSnapshotPin &snapshot, size_t maximumRecords,
			const std::function<bool(const LogicalGraphNode &)> &visitor) const {
		if (!visitor) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									 "page-backed logical graph Node scan requires a visitor");
		}
		return pages_.visitNodesUntil(snapshot, maximumRecords,
									  [this, &snapshot, &visitor](const PageBackedGraphNode &node) {
										  return visitor(materializeNode(snapshot, node));
									  });
	}

	bool PageBackedLogicalGraphReadAdapter::visitEdgesUntil(
			const MetadataSnapshotPin &snapshot, size_t maximumRecords,
			const std::function<bool(const LogicalGraphEdge &)> &visitor) const {
		if (!visitor) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									 "page-backed logical graph Edge scan requires a visitor");
		}
		return pages_.visitEdgesUntil(snapshot, maximumRecords,
									  [this, &snapshot, &visitor](const PageBackedGraphEdge &edge) {
										  return visitor(materializeEdge(snapshot, edge));
									  });
	}

	LogicalGraphNodePage PageBackedLogicalGraphReadAdapter::scanNodesAfter(const MetadataSnapshotPin &snapshot,
																		   uint64_t afterNodeId, size_t maximumRecords,
																		   size_t maximumMetadataPages) const {
		const PageBackedGraphNodePage raw =
				pages_.scanNodesAfter(snapshot, afterNodeId, maximumRecords, maximumMetadataPages);
		LogicalGraphNodePage result;
		result.may_have_more = raw.may_have_more;
		result.metadata_pages_visited = raw.metadata_pages_visited;
		result.nodes.reserve(raw.nodes.size());
		for (const PageBackedGraphNode &node: raw.nodes) {
			result.nodes.push_back(materializeNode(snapshot, node));
		}
		return result;
	}

	LogicalGraphAdjacencyPage PageBackedLogicalGraphReadAdapter::scanAdjacentEdges(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const {
		const PageBackedGraphAdjacencyPage raw = pages_.scanAdjacentEdges(
				snapshot, nodeId, pageDirection(direction), pageContinuation(continuation), maximumRecords);
		LogicalGraphAdjacencyPage result;
		result.edges.reserve(raw.edges.size());
		for (const PageBackedGraphEdge &edge: raw.edges) {
			result.edges.push_back(materializeEdge(snapshot, edge));
		}
		result.continuation = logicalContinuation(raw.continuation);
		return result;
	}

	PageBackedLogicalGraphAdjacencyCursor PageBackedLogicalGraphReadAdapter::openAdjacentEdgeCursor(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction) const {
		return PageBackedLogicalGraphAdjacencyCursor(*this, pages_.openAdjacentEdgeCursor(
				snapshot, nodeId, pageDirection(direction)));
	}

	bool PageBackedLogicalGraphReadAdapter::visitAdjacentEdgesUntil(
			const MetadataSnapshotPin &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
			size_t maximumRecords, const std::function<bool(const LogicalGraphEdge &)> &visitor) const {
		if (!visitor) {
			throwLogicalAdapterError(FormatError::FERR_INVALID_HEADER,
									 "page-backed logical graph adjacency traversal requires a visitor");
		}
		return pages_.visitAdjacentEdgesUntil(snapshot, nodeId, pageDirection(direction), maximumRecords,
											  [this, &snapshot, &visitor](const PageBackedGraphEdge &edge) {
												  return visitor(materializeEdge(snapshot, edge));
											  });
	}

} // namespace graph::storage
