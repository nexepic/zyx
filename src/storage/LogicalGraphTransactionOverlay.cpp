/**
 * @file LogicalGraphTransactionOverlay.cpp
 * @brief In-memory final-state view for one logical graph transaction.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/storage/LogicalGraphTransactionOverlay.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "graph/core/Types.hpp"

namespace graph::storage {

	namespace {

		using PropertyMap = std::map<std::string, PropertyValue>;

		[[nodiscard]] EntityReadShape completeNodeReadShape() {
			return {.include_labels = true, .include_type = false, .all_properties = true};
		}

		[[nodiscard]] EntityReadShape completeEdgeReadShape() {
			return {.include_labels = false, .include_type = true, .all_properties = true};
		}

		[[nodiscard]] bool isEmpty(const LogicalGraphMutationBatch &batch) {
			return batch.node_creates.empty() && batch.edge_creates.empty() && batch.property_replacements.empty() &&
				   batch.node_label_replacements.empty() && batch.edge_deletes.empty() && batch.node_deletes.empty() &&
				   !batch.creation_identity_plan.has_value();
		}

		[[nodiscard]] PropertyMap makePropertyMap(const std::vector<LogicalGraphWriteProperty> &properties) {
			PropertyMap result;
			for (const LogicalGraphWriteProperty &property: properties) {
				if (property.key.empty() || !result.emplace(property.key, property.value).second) {
					throw std::invalid_argument("logical graph transaction overlay requires nonempty unique Property keys");
				}
			}
			return result;
		}

		[[nodiscard]] std::vector<LogicalGraphPropertyEntry> makePropertyEntries(const PropertyMap &properties) {
			std::vector<LogicalGraphPropertyEntry> result;
			result.reserve(properties.size());
			for (const auto &[key, value]: properties) {
				result.push_back({key, value});
			}
			return result;
		}

		[[nodiscard]] PropertyMap makePropertyMap(const std::vector<LogicalGraphPropertyEntry> &properties,
														 const char *context) {
			PropertyMap result;
			for (const LogicalGraphPropertyEntry &property: properties) {
				if (property.key.empty() || !result.emplace(property.key, property.value).second) {
					throw std::runtime_error(std::string(context) + " has invalid owner-keyed Property entries");
				}
			}
			return result;
		}

		[[nodiscard]] std::vector<std::string> canonicalLabels(std::vector<std::string> labels) {
			std::ranges::sort(labels);
			if (std::ranges::any_of(labels, [](const std::string &label) { return label.empty(); }) ||
				std::ranges::adjacent_find(labels) != labels.end()) {
				throw std::invalid_argument("logical graph transaction overlay requires nonempty unique labels");
			}
			return labels;
		}

		[[nodiscard]] LogicalGraphMutationEntityReference persistedNodeReference(uint64_t id) {
			return {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id};
		}

		[[nodiscard]] LogicalGraphMutationEntityReference persistedEdgeReference(uint64_t id) {
			return {LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id};
		}

	} // namespace

	class LogicalGraphTransactionOverlay::Snapshot final : public LogicalGraphReadSnapshot {
	public:
		explicit Snapshot(const LogicalGraphTransactionOverlay &overlay) : overlay_(overlay) {}

		[[nodiscard]] LogicalGraphNode readNodeShaped(uint64_t nodeId, const EntityReadShape &shape) const override {
			validateEntityReadShape(shape);
			const auto state = overlay_.nodes_.find(nodeId);
			if (state == overlay_.nodes_.end()) {
				return overlay_.baseSnapshot().readNodeShaped(nodeId, shape);
			}
			if (state->second.deleted) {
				throw std::runtime_error("logical transaction overlay Node was deleted");
			}
			const LogicalGraphNode &source = state->second.node;
			LogicalGraphNode result;
			result.node_id = source.node_id;
			if (shape.include_labels) {
				result.labels = source.labels;
			}
			if (shape.all_properties) {
				result.properties = source.properties;
				return result;
			}
			// Staged records retain logical Property names. Resolve each name through
			// the pinned base schema when possible; if a generic reader cannot expose
			// tokens, conservatively retain the properties rather than returning an
			// incomplete logical value.
			for (const LogicalGraphPropertyEntry &entry: source.properties) {
				const std::optional<uint32_t> token = overlay_.baseSnapshot().findPropertyToken(entry.key);
				if (token.has_value() && std::ranges::binary_search(shape.property_tokens, *token)) {
					result.properties.push_back(entry);
				} else if (!token.has_value() && !shape.property_tokens.empty()) {
					result.properties.push_back(entry);
				}
			}
			return result;
		}

		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSorted(std::span<const uint64_t> nodeIds, const EntityReadShape &shape) const override {
			validateEntityReadShape(shape);
			std::vector<uint64_t> baseIds;
			baseIds.reserve(nodeIds.size());
			uint64_t previous = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (nodeId == 0U || nodeId <= previous) {
					throw std::invalid_argument("logical transaction overlay shaped Node batch requires increasing IDs");
				}
				previous = nodeId;
				const auto state = overlay_.nodes_.find(nodeId);
				if (state != overlay_.nodes_.end()) {
					if (state->second.deleted) {
						throw std::runtime_error("logical transaction overlay Node was deleted");
					}
					continue;
				}
				baseIds.push_back(nodeId);
			}
			const std::vector<LogicalGraphNode> baseNodes = baseIds.empty()
					? std::vector<LogicalGraphNode>{}
					: overlay_.baseSnapshot().readNodesShapedSorted(baseIds, shape);
			if (baseNodes.size() != baseIds.size()) {
				throw std::runtime_error("logical transaction overlay base Node batch returned an invalid cardinality");
			}
			std::vector<LogicalGraphNode> result;
			result.reserve(nodeIds.size());
			size_t baseOrdinal = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (overlay_.nodes_.contains(nodeId)) {
					result.push_back(readNodeShaped(nodeId, shape));
				} else {
					const LogicalGraphNode &node = baseNodes[baseOrdinal];
					if (node.node_id != nodeId) {
						throw std::runtime_error("logical transaction overlay base Node batch returned an out-of-order record");
					}
					result.push_back(node);
					++baseOrdinal;
				}
			}
			return result;
		}

		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override {
			return readNodeShaped(nodeId, completeNodeReadShape());
		}

		[[nodiscard]] bool containsNode(uint64_t nodeId) const override {
			if (const auto state = overlay_.nodes_.find(nodeId); state != overlay_.nodes_.end()) {
				return !state->second.deleted;
			}
			return overlay_.baseSnapshot().containsNode(nodeId);
		}

		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override {
			return readEdgeShaped(edgeId, completeEdgeReadShape());
		}

		[[nodiscard]] LogicalGraphEdge readEdgeShaped(uint64_t edgeId, const EntityReadShape &shape) const override {
			validateEntityReadShape(shape);
			const auto state = overlay_.edges_.find(edgeId);
			if (state == overlay_.edges_.end()) {
				LogicalGraphEdge edge = overlay_.baseSnapshot().readEdgeShaped(edgeId, shape);
				if (!overlay_.isEdgeVisible(edge, edgeId)) {
					throw std::runtime_error("logical transaction overlay Edge was deleted");
				}
				return edge;
			}
			if (state->second.deleted || !overlay_.isEdgeVisible(state->second.edge, edgeId)) {
				throw std::runtime_error("logical transaction overlay Edge was deleted");
			}
			const LogicalGraphEdge &source = state->second.edge;
			LogicalGraphEdge result;
			result.edge_id = source.edge_id;
			result.source_node_id = source.source_node_id;
			result.target_node_id = source.target_node_id;
			if (shape.include_type) {
				result.type = source.type;
			}
			if (shape.all_properties) {
				result.properties = source.properties;
				return result;
			}
			for (const LogicalGraphPropertyEntry &entry: source.properties) {
				const std::optional<uint32_t> token = overlay_.baseSnapshot().findPropertyToken(entry.key);
				if (token.has_value() && std::ranges::binary_search(shape.property_tokens, *token)) {
					result.properties.push_back(entry);
				} else if (!token.has_value() && !shape.property_tokens.empty()) {
					// Preserve correctness for readers that do not publish token lookup.
					result.properties.push_back(entry);
				}
			}
			return result;
		}

		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSorted(std::span<const uint64_t> edgeIds, const EntityReadShape &shape) const override {
			validateEntityReadShape(shape);
			std::vector<uint64_t> baseIds;
			baseIds.reserve(edgeIds.size());
			uint64_t previous = 0U;
			for (const uint64_t edgeId: edgeIds) {
				if (edgeId == 0U || edgeId <= previous) {
					throw std::invalid_argument("logical transaction overlay shaped Edge batch requires increasing IDs");
				}
				previous = edgeId;
				const auto state = overlay_.edges_.find(edgeId);
				if (state != overlay_.edges_.end()) {
					if (state->second.deleted || !overlay_.isEdgeVisible(state->second.edge, edgeId)) {
						throw std::runtime_error("logical transaction overlay Edge was deleted");
					}
					continue;
				}
				baseIds.push_back(edgeId);
			}
			const std::vector<LogicalGraphEdge> baseEdges = baseIds.empty()
					? std::vector<LogicalGraphEdge>{}
					: overlay_.baseSnapshot().readEdgesShapedSorted(baseIds, shape);
			if (baseEdges.size() != baseIds.size()) {
				throw std::runtime_error("logical transaction overlay base Edge batch returned an invalid cardinality");
			}
			std::vector<LogicalGraphEdge> result;
			result.reserve(edgeIds.size());
			size_t baseOrdinal = 0U;
			for (const uint64_t edgeId: edgeIds) {
				if (overlay_.edges_.contains(edgeId)) {
					result.push_back(readEdgeShaped(edgeId, shape));
				} else {
					const LogicalGraphEdge &edge = baseEdges[baseOrdinal];
					if (edge.edge_id != edgeId || !overlay_.isEdgeVisible(edge, edgeId)) {
						throw std::runtime_error("logical transaction overlay base Edge batch returned an invalid record");
					}
					result.push_back(edge);
					++baseOrdinal;
				}
			}
			return result;
		}

		[[nodiscard]] std::vector<PropertyValue>
		readNodePropertiesSorted(std::span<const uint64_t> nodeIds, std::string_view key) const override {
			if (key.empty()) {
				throw std::invalid_argument("logical transaction overlay sorted Node Property read requires a non-empty key");
			}
			std::vector<uint64_t> baseIds;
			baseIds.reserve(nodeIds.size());
			uint64_t previous = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (nodeId == 0U || nodeId <= previous) {
					throw std::invalid_argument("logical transaction overlay sorted Node Property read requires increasing IDs");
				}
				previous = nodeId;
				const auto state = overlay_.nodes_.find(nodeId);
				if (state != overlay_.nodes_.end()) {
					if (state->second.deleted) {
						throw std::runtime_error("logical transaction overlay Node was deleted");
					}
					continue;
				}
				baseIds.push_back(nodeId);
			}
			const std::vector<PropertyValue> baseValues = baseIds.empty()
					? std::vector<PropertyValue>{}
					: overlay_.baseSnapshot().readNodePropertiesSorted(baseIds, key);
			if (baseValues.size() != baseIds.size()) {
				throw std::runtime_error("logical transaction overlay base Node Property batch returned an invalid cardinality");
			}
			std::vector<PropertyValue> result;
			result.reserve(nodeIds.size());
			size_t baseOrdinal = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (overlay_.nodes_.contains(nodeId)) {
					result.push_back(readNodeProperty(nodeId, key));
				} else {
					result.push_back(baseValues[baseOrdinal++]);
				}
			}
			return result;
		}

		[[nodiscard]] std::vector<PropertyValue>
		readEdgePropertiesSorted(std::span<const uint64_t> edgeIds, std::string_view key) const override {
			if (key.empty()) {
				throw std::invalid_argument("logical transaction overlay sorted Edge Property read requires a non-empty key");
			}
			std::vector<uint64_t> baseIds;
			baseIds.reserve(edgeIds.size());
			uint64_t previous = 0U;
			for (const uint64_t edgeId: edgeIds) {
				if (edgeId == 0U || edgeId <= previous) {
					throw std::invalid_argument("logical transaction overlay sorted Edge Property read requires increasing IDs");
				}
				previous = edgeId;
				const auto state = overlay_.edges_.find(edgeId);
				if (state != overlay_.edges_.end()) {
					if (state->second.deleted || !overlay_.isEdgeVisible(state->second.edge, edgeId)) {
						throw std::runtime_error("logical transaction overlay Edge was deleted");
					}
					continue;
				}
				baseIds.push_back(edgeId);
			}
			const std::vector<PropertyValue> baseValues = baseIds.empty()
					? std::vector<PropertyValue>{}
					: overlay_.baseSnapshot().readEdgePropertiesSorted(baseIds, key);
			if (baseValues.size() != baseIds.size()) {
				throw std::runtime_error("logical transaction overlay base Edge Property batch returned an invalid cardinality");
			}
			std::vector<PropertyValue> result;
			result.reserve(edgeIds.size());
			size_t baseOrdinal = 0U;
			for (const uint64_t edgeId: edgeIds) {
				if (overlay_.edges_.contains(edgeId)) {
					result.push_back(readEdgeProperty(edgeId, key));
				} else {
					result.push_back(baseValues[baseOrdinal++]);
				}
			}
			return result;
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSorted(std::span<const uint64_t> nodeIds,
											 std::span<const std::string_view> keys) const override {
			for (const std::string_view key: keys) {
				if (key.empty()) {
					throw std::invalid_argument("logical transaction overlay Node Property columns require non-empty keys");
				}
			}
			std::vector<uint64_t> baseIds;
			baseIds.reserve(nodeIds.size());
			uint64_t previous = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (nodeId == 0U || nodeId <= previous) {
					throw std::invalid_argument("logical transaction overlay Node Property columns require increasing IDs");
				}
				previous = nodeId;
				const auto state = overlay_.nodes_.find(nodeId);
				if (state != overlay_.nodes_.end()) {
					if (state->second.deleted) {
						throw std::runtime_error("logical transaction overlay Node was deleted");
					}
					continue;
				}
				baseIds.push_back(nodeId);
			}
			const std::vector<std::vector<PropertyValue>> baseColumns = baseIds.empty()
					? std::vector<std::vector<PropertyValue>>(keys.size())
					: overlay_.baseSnapshot().readNodePropertyColumnsSorted(baseIds, keys);
			if (baseColumns.size() != keys.size()) {
				throw std::runtime_error("logical transaction overlay base Node Property columns returned an invalid count");
			}
			for (const auto &column: baseColumns) {
				if (column.size() != baseIds.size()) {
					throw std::runtime_error("logical transaction overlay base Node Property columns returned an invalid cardinality");
				}
			}
			std::vector<std::vector<PropertyValue>> result(keys.size());
			for (auto &column: result) {
				column.reserve(nodeIds.size());
			}
			size_t baseOrdinal = 0U;
			for (const uint64_t nodeId: nodeIds) {
				const auto state = overlay_.nodes_.find(nodeId);
				for (size_t column = 0U; column < keys.size(); ++column) {
					result[column].push_back(state != overlay_.nodes_.end()
							? readNodeProperty(nodeId, keys[column])
							: baseColumns[column][baseOrdinal]);
				}
				if (state == overlay_.nodes_.end()) {
					++baseOrdinal;
				}
			}
			return result;
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readEdgePropertyColumnsSorted(std::span<const uint64_t> edgeIds,
											 std::span<const std::string_view> keys) const override {
			for (const std::string_view key: keys) {
				if (key.empty()) {
					throw std::invalid_argument("logical transaction overlay Edge Property columns require non-empty keys");
				}
			}
			std::vector<uint64_t> baseIds;
			baseIds.reserve(edgeIds.size());
			uint64_t previous = 0U;
			for (const uint64_t edgeId: edgeIds) {
				if (edgeId == 0U || edgeId <= previous) {
					throw std::invalid_argument("logical transaction overlay Edge Property columns require increasing IDs");
				}
				previous = edgeId;
				const auto state = overlay_.edges_.find(edgeId);
				if (state != overlay_.edges_.end()) {
					if (state->second.deleted || !overlay_.isEdgeVisible(state->second.edge, edgeId)) {
						throw std::runtime_error("logical transaction overlay Edge was deleted");
					}
					continue;
				}
				baseIds.push_back(edgeId);
			}
			const std::vector<std::vector<PropertyValue>> baseColumns = baseIds.empty()
					? std::vector<std::vector<PropertyValue>>(keys.size())
					: overlay_.baseSnapshot().readEdgePropertyColumnsSorted(baseIds, keys);
			if (baseColumns.size() != keys.size()) {
				throw std::runtime_error("logical transaction overlay base Edge Property columns returned an invalid count");
			}
			for (const auto &column: baseColumns) {
				if (column.size() != baseIds.size()) {
					throw std::runtime_error("logical transaction overlay base Edge Property columns returned an invalid cardinality");
				}
			}
			std::vector<std::vector<PropertyValue>> result(keys.size());
			for (auto &column: result) {
				column.reserve(edgeIds.size());
			}
			size_t baseOrdinal = 0U;
			for (const uint64_t edgeId: edgeIds) {
				const auto state = overlay_.edges_.find(edgeId);
				for (size_t column = 0U; column < keys.size(); ++column) {
					result[column].push_back(state != overlay_.edges_.end()
							? readEdgeProperty(edgeId, keys[column])
							: baseColumns[column][baseOrdinal]);
				}
				if (state == overlay_.edges_.end()) {
					++baseOrdinal;
				}
			}
			return result;
		}

		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
		findNodesByExactProperties(const std::vector<std::string> &labels,
											 const std::vector<std::pair<std::string, PropertyValue>> &properties,
											 size_t maximumNodes, size_t maximumPropertyEntries) const override {
			if (maximumNodes == 0U || maximumPropertyEntries == 0U) {
				throw std::invalid_argument("logical transaction overlay exact Node lookup requires positive bounds");
			}
			const auto base = overlay_.baseSnapshot().findNodesByExactProperties(
					labels, properties, maximumNodes, maximumPropertyEntries);
			if (!base.has_value()) {
				return std::nullopt;
			}
			const auto matches = [&labels, &properties, maximumPropertyEntries](const LogicalGraphNode &node) {
				std::set<std::string> nodeLabels(node.labels.begin(), node.labels.end());
				if (nodeLabels.size() != node.labels.size() ||
					std::ranges::any_of(node.labels, [](const std::string &label) { return label.empty(); })) {
					throw std::runtime_error("logical transaction overlay exact Node lookup found invalid labels");
				}
				for (const std::string &label: labels) {
					if (label.empty() || !nodeLabels.contains(label)) {
						return false;
					}
				}
				const auto hasExpectedValues = [&properties](const auto &entries) {
					for (const auto &[key, expected]: properties) {
						const auto value = entries.find(key);
						if (key.empty() || value == entries.end() || value->second != expected) {
							return false;
						}
					}
					return true;
				};
				if (node.properties.size() > maximumPropertyEntries) {
					throw std::runtime_error(
							"logical transaction overlay exact Node lookup exceeded its Property bound");
				}
				return hasExpectedValues(makePropertyMap(node.properties, "logical transaction overlay exact Node lookup"));
			};

			std::map<uint64_t, LogicalGraphNode> matched;
			const auto addIfMatched = [&](const LogicalGraphNode &node) {
				if (!matches(node)) {
					return;
				}
				if (matched.contains(node.node_id)) {
					return;
				}
				if (matched.size() == maximumNodes) {
					throw std::runtime_error("logical transaction overlay exact Node lookup exceeded its result bound");
				}
				matched.emplace(node.node_id, node);
			};
			for (const LogicalGraphNode &node: *base) {
				const auto state = overlay_.nodes_.find(node.node_id);
				if (state != overlay_.nodes_.end()) {
					if (!state->second.deleted) {
						addIfMatched(state->second.node);
					}
				} else {
					addIfMatched(node);
				}
			}
			for (const auto &[nodeId, state]: overlay_.nodes_) {
				static_cast<void>(nodeId);
				if (!state.deleted) {
					addIfMatched(state.node);
				}
			}
			std::vector<LogicalGraphNode> result;
			result.reserve(matched.size());
			for (const auto &[nodeId, node]: matched) {
				static_cast<void>(nodeId);
				result.push_back(node);
			}
			return result;
		}

		void visitNodes(size_t maximumRecords, const std::function<bool(const LogicalGraphNode &)> &visitor) const override {
			if (maximumRecords == 0U || !visitor) {
				throw std::invalid_argument("logical transaction overlay Node visit requires a positive bound and visitor");
			}
			uint64_t after = 0U;
			size_t visited = 0U;
			while (true) {
				const LogicalGraphNodePage page = scanNodesAfter(after, maximumRecords - visited,
																std::numeric_limits<size_t>::max());
				for (const LogicalGraphNode &node: page.nodes) {
					if (!visitor(node)) {
						return;
					}
					after = node.node_id;
					++visited;
					if (visited == maximumRecords) {
						if (page.may_have_more) {
							throw std::runtime_error("logical transaction overlay Node visit exceeded its bound");
						}
						return;
					}
				}
				if (!page.may_have_more) {
					return;
				}
				if (page.nodes.empty()) {
					throw std::runtime_error("logical transaction overlay returned an empty nonterminal Node page");
				}
			}
		}

		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																				  size_t maximumMetadataPages) const override {
			if (maximumRecords == 0U || maximumMetadataPages == 0U) {
				throw std::invalid_argument("logical transaction overlay Node scan requires positive bounds");
			}
			LogicalGraphNodePage result;
			uint64_t baseAfter = afterNodeId;
			bool baseExhausted = false;
			size_t remainingMetadataPages = maximumMetadataPages;
			size_t metadataPagesVisited = 0U;
			while (result.nodes.size() < maximumRecords && !baseExhausted) {
				if (remainingMetadataPages == 0U) {
					throw std::runtime_error(
							"logical transaction overlay Node scan exceeded its metadata-page bound while filtering staged deletions");
				}
				// A base page may hide deleted records. It reports its exact metadata
				// cost, so repeated probes consume one shared total budget rather than
				// multiplying the caller's allowance.
				const LogicalGraphNodePage page = overlay_.baseSnapshot().scanNodesAfter(
						baseAfter, maximumRecords - result.nodes.size(), remainingMetadataPages);
				if (page.metadata_pages_visited > remainingMetadataPages ||
					(page.metadata_pages_visited == 0U && (!page.nodes.empty() || page.may_have_more))) {
					throw std::runtime_error(
							"logical transaction overlay base snapshot reported an invalid metadata-page cost");
				}
				remainingMetadataPages -= page.metadata_pages_visited;
				metadataPagesVisited += page.metadata_pages_visited;
				if (page.nodes.empty() && page.may_have_more) {
					throw std::runtime_error("logical transaction overlay base snapshot returned an empty nonterminal Node page");
				}
				for (const LogicalGraphNode &node: page.nodes) {
					baseAfter = node.node_id;
					if (overlay_.isNodeVisible(node.node_id)) {
						// The base scan already materializes the complete Node. Only a
						// staged Node needs overlay replacement; avoid a second point read
						// for every persisted row in the page.
						if (overlay_.nodes_.contains(node.node_id)) {
							result.nodes.push_back(readNode(node.node_id));
						} else {
							result.nodes.push_back(node);
						}
					}
				}
				baseExhausted = !page.may_have_more;
				if (page.nodes.empty()) {
					break;
				}
			}
			if (result.nodes.size() == maximumRecords) {
				result.may_have_more = true;
				result.metadata_pages_visited = metadataPagesVisited;
				return result;
			}
			if (!baseExhausted) {
				result.may_have_more = true;
				result.metadata_pages_visited = metadataPagesVisited;
				return result;
			}
			for (const auto &[nodeId, state]: overlay_.nodes_) {
				if (result.nodes.size() == maximumRecords) {
					result.may_have_more = true;
					break;
				}
				if (state.created && !state.deleted && nodeId > afterNodeId) {
					result.nodes.push_back(state.node);
				}
			}
			result.metadata_pages_visited = metadataPagesVisited;
			return result;
		}

		[[nodiscard]] LogicalGraphAdjacencyPage
		scanAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
							  std::optional<LogicalGraphAdjacencyContinuation> continuation,
							  size_t maximumRecords) const override {
			if (nodeId == 0U || maximumRecords == 0U) {
				throw std::invalid_argument("logical transaction overlay adjacency scan requires positive bounds");
			}
			const auto continuationKey = [nodeId, direction](const LogicalGraphAdjacencyContinuation &value) {
				return std::tuple{nodeId, static_cast<uint8_t>(direction), value.previous_edge_id, value.next_edge_id};
			};
			std::optional<LogicalGraphAdjacencyContinuation> current = continuation;
			if (continuation.has_value()) {
				if (const auto found = continuations_.find(continuationKey(*continuation)); found != continuations_.end()) {
					if (found->second.created_offset.has_value()) {
						return createdEdgePage(nodeId, direction, *found->second.created_offset, maximumRecords,
																	 continuationKey);
					}
					current = found->second.base_continuation;
				}
			}
			if (const auto node = overlay_.nodes_.find(nodeId);
				node != overlay_.nodes_.end() && node->second.created) {
				if (continuation.has_value()) {
					throw std::runtime_error("logical transaction overlay received an unknown created-Node adjacency continuation");
				}
				return createdEdgePage(nodeId, direction, 0U, maximumRecords, continuationKey);
			}

			std::vector<LogicalGraphEdge> edges;
			edges.reserve(maximumRecords);
			while (edges.size() < maximumRecords) {
				const LogicalGraphAdjacencyPage page = overlay_.baseSnapshot().scanAdjacentEdges(
						nodeId, direction, current, maximumRecords - edges.size());
				if (page.edges.empty() && page.continuation.has_value()) {
					throw std::runtime_error(
							"logical transaction overlay base snapshot returned an empty nonterminal adjacency page");
				}
				for (const LogicalGraphEdge &edge: page.edges) {
					if (overlay_.isEdgeVisible(edge, edge.edge_id)) {
						// The base adjacency page already contains a complete Edge. Only
						// staged Edge state needs an overlay lookup; re-reading every base
						// Edge would turn one adjacency page into N point reads.
						if (overlay_.edges_.contains(edge.edge_id)) {
							edges.push_back(readEdge(edge.edge_id));
						} else {
							edges.push_back(edge);
						}
					}
				}
				if (edges.size() == maximumRecords) {
					LogicalGraphAdjacencyPage result;
					result.edges = std::move(edges);
					if (page.continuation.has_value()) {
						result.continuation = {result.edges.back().edge_id, page.continuation->next_edge_id};
						continuations_.insert_or_assign(continuationKey(*result.continuation),
																	  ContinuationState{page.continuation, std::nullopt});
						return result;
					}
					return appendCreatedEdges(nodeId, direction, std::move(result.edges), maximumRecords, continuationKey);
				}
				if (!page.continuation.has_value()) {
					break;
				}
				current = page.continuation;
			}
			return appendCreatedEdges(nodeId, direction, std::move(edges), maximumRecords, continuationKey);
		}

		[[nodiscard]] LogicalGraphAdjacencyTopologyPage
		scanAdjacentEdgeTopologies(uint64_t nodeId, LogicalGraphReadDirection direction,
											std::optional<LogicalGraphAdjacencyContinuation> continuation,
											size_t maximumRecords) const override {
			if (nodeId == 0U || maximumRecords == 0U) {
				throw std::invalid_argument("logical transaction overlay topology scan requires positive bounds");
			}
			const auto continuationKey = [nodeId, direction](const LogicalGraphAdjacencyContinuation &value) {
				return std::tuple{nodeId, static_cast<uint8_t>(direction), value.previous_edge_id, value.next_edge_id};
			};
		std::optional<LogicalGraphAdjacencyContinuation> current = continuation;
		if (continuation.has_value()) {
			if (const auto found = continuations_.find(continuationKey(*continuation)); found != continuations_.end()) {
				if (found->second.created_offset.has_value()) {
					return createdTopologyPage(nodeId, direction, *found->second.created_offset, maximumRecords,
													 continuationKey);
				}
				current = found->second.base_continuation;
			}
		}
		if (const auto node = overlay_.nodes_.find(nodeId);
				 node != overlay_.nodes_.end() && node->second.created) {
			if (continuation.has_value()) {
				throw std::runtime_error("logical transaction overlay received an unknown created-Node topology continuation");
			}
			return createdTopologyPage(nodeId, direction, 0U, maximumRecords, continuationKey);
		}

		std::vector<LogicalGraphEdgeTopology> edges;
		edges.reserve(maximumRecords);
		while (edges.size() < maximumRecords) {
			const LogicalGraphAdjacencyTopologyPage page = overlay_.baseSnapshot().scanAdjacentEdgeTopologies(
					nodeId, direction, current, maximumRecords - edges.size());
			if (page.edges.empty() && page.continuation.has_value()) {
				throw std::runtime_error(
						"logical transaction overlay base snapshot returned an empty nonterminal topology page");
			}
			for (const LogicalGraphEdgeTopology &edge: page.edges) {
				if (overlay_.isEdgeVisible(
						{edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type}, edge.edge_id)) {
					edges.push_back(edge);
				}
			}
			if (edges.size() == maximumRecords) {
				LogicalGraphAdjacencyTopologyPage result;
				result.edges = std::move(edges);
				if (page.continuation.has_value()) {
					result.continuation = {result.edges.back().edge_id, page.continuation->next_edge_id};
					continuations_.insert_or_assign(continuationKey(*result.continuation),
																		ContinuationState{page.continuation, std::nullopt});
					return result;
				}
				return appendCreatedTopologies(nodeId, direction, 0U, std::move(result.edges), maximumRecords, continuationKey);
			}
			if (!page.continuation.has_value()) {
				break;
			}
			current = page.continuation;
		}
			return appendCreatedTopologies(nodeId, direction, 0U, std::move(edges), maximumRecords, continuationKey);
		}

		void visitEdges(size_t maximumRecords, const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			if (maximumRecords == 0U || !visitor) {
				throw std::invalid_argument("logical transaction overlay Edge visit requires a positive bound and visitor");
			}
			size_t visited = 0U;
			overlay_.baseSnapshot().visitEdges(maximumRecords, [&](const LogicalGraphEdge &edge) {
				if (!overlay_.isEdgeVisible(edge, edge.edge_id)) {
					return true;
				}
				++visited;
				if (visited > maximumRecords) {
					throw std::runtime_error("logical transaction overlay Edge visit exceeded its bound");
				}
				return visitor(overlay_.edges_.contains(edge.edge_id) ? readEdge(edge.edge_id) : edge);
			});
			for (const auto &[edgeId, state]: overlay_.edges_) {
				if (!state.created || !overlay_.isEdgeVisible(state.edge, edgeId)) {
					continue;
				}
				if (visited == maximumRecords) {
					throw std::runtime_error("logical transaction overlay Edge visit exceeded its bound");
				}
				++visited;
				if (!visitor(state.edge)) {
					return;
				}
			}
		}

		void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
										const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			if (maximumEdges == 0U || !visitor) {
				throw std::invalid_argument("logical transaction overlay adjacency visit requires a positive bound and visitor");
			}
			std::optional<LogicalGraphAdjacencyContinuation> continuation;
			size_t visited = 0U;
			while (true) {
				const LogicalGraphAdjacencyPage page = scanAdjacentEdges(nodeId, direction, continuation, maximumEdges - visited);
				for (const LogicalGraphEdge &edge: page.edges) {
					if (!visitor(edge)) {
						return;
					}
					++visited;
				}
				if (!page.continuation.has_value()) {
					return;
				}
				if (visited == maximumEdges) {
					throw std::runtime_error("logical transaction overlay adjacency visit exceeded its bound");
				}
				continuation = page.continuation;
			}
		}

		[[nodiscard]] std::optional<LogicalGraphMutationFence> mutationFence() const override {
			return overlay_.overlayFence();
		}

	private:
		using ContinuationKey = std::tuple<uint64_t, uint8_t, uint64_t, uint64_t>;

		struct ContinuationState {
			std::optional<LogicalGraphAdjacencyContinuation> base_continuation;
			std::optional<size_t> created_offset;
		};

		template<typename MakeKey>
		[[nodiscard]] LogicalGraphAdjacencyPage appendCreatedEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
																				std::vector<LogicalGraphEdge> edges, size_t maximumRecords,
																				const MakeKey &makeKey) const {
			return appendCreatedEdgesFrom(nodeId, direction, 0U, std::move(edges), maximumRecords, makeKey);
		}

		template<typename MakeKey>
		[[nodiscard]] LogicalGraphAdjacencyPage createdEdgePage(uint64_t nodeId, LogicalGraphReadDirection direction,
																			  size_t offset, size_t maximumRecords, const MakeKey &makeKey) const {
			return appendCreatedEdgesFrom(nodeId, direction, offset, {}, maximumRecords, makeKey);
		}

		template<typename MakeKey>
		[[nodiscard]] LogicalGraphAdjacencyTopologyPage createdTopologyPage(uint64_t nodeId,
															LogicalGraphReadDirection direction, size_t offset,
															size_t maximumRecords, const MakeKey &makeKey) const {
			return appendCreatedTopologies(nodeId, direction, offset, {}, maximumRecords, makeKey);
		}

		template<typename MakeKey>
		[[nodiscard]] LogicalGraphAdjacencyTopologyPage appendCreatedTopologies(uint64_t nodeId,
																LogicalGraphReadDirection direction, size_t offset,
																std::vector<LogicalGraphEdgeTopology> edges,
																size_t maximumRecords, const MakeKey &makeKey) const {
			std::vector<LogicalGraphEdgeTopology> created;
			for (const auto &[edgeId, state]: overlay_.edges_) {
				const bool endpointMatches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
															 ? state.edge.source_node_id == nodeId
															 : state.edge.target_node_id == nodeId;
				if (state.created && endpointMatches && overlay_.isEdgeVisible(state.edge, edgeId)) {
					created.push_back({edgeId, state.edge.source_node_id, state.edge.target_node_id, state.edge.type});
				}
			}
			if (offset > created.size()) {
				throw std::runtime_error("logical transaction overlay received an invalid created topology continuation");
			}
			size_t index = offset;
			while (index < created.size() && edges.size() < maximumRecords) {
				edges.push_back(created[index++]);
			}
			LogicalGraphAdjacencyTopologyPage result;
			result.edges = std::move(edges);
			if (index < created.size()) {
				result.continuation = {result.edges.back().edge_id, created[index].edge_id};
				continuations_.insert_or_assign(makeKey(*result.continuation), ContinuationState{std::nullopt, index});
			}
			return result;
		}

		template<typename MakeKey>
		[[nodiscard]] LogicalGraphAdjacencyPage appendCreatedEdgesFrom(uint64_t nodeId,
																				 LogicalGraphReadDirection direction, size_t offset,
																				 std::vector<LogicalGraphEdge> edges, size_t maximumRecords,
																				 const MakeKey &makeKey) const {
			std::vector<LogicalGraphEdge> created;
			for (const auto &[edgeId, state]: overlay_.edges_) {
				const bool endpointMatches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
																 ? state.edge.source_node_id == nodeId
																 : state.edge.target_node_id == nodeId;
				if (state.created && endpointMatches && overlay_.isEdgeVisible(state.edge, edgeId)) {
					created.push_back(state.edge);
				}
			}
			if (offset > created.size()) {
				throw std::runtime_error("logical transaction overlay received an invalid created Edge continuation");
			}
			size_t index = offset;
			while (index < created.size() && edges.size() < maximumRecords) {
				edges.push_back(created[index++]);
			}
			LogicalGraphAdjacencyPage result;
			result.edges = std::move(edges);
			if (index < created.size()) {
				result.continuation = {result.edges.back().edge_id, created[index].edge_id};
				continuations_.insert_or_assign(makeKey(*result.continuation),
															  ContinuationState{std::nullopt, index});
			}
			return result;
		}

		const LogicalGraphTransactionOverlay &overlay_;
		mutable std::map<ContinuationKey, ContinuationState> continuations_;
	};

	class LogicalGraphTransactionOverlay::VectorSnapshot final : public LogicalGraphVectorSearchSnapshot {
	public:
		explicit VectorSnapshot(const LogicalGraphTransactionOverlay &overlay,
									 LogicalGraphVectorSearchSnapshot &derivedSnapshot) :
				overlay_(overlay), derived_snapshot_(derivedSnapshot) {}

		[[nodiscard]] LogicalGraphNode readNodeShaped(uint64_t nodeId, const EntityReadShape &shape) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readNodeShaped(nodeId, shape);
			}
			return overlay_.openLogicalGraphReadSnapshot()->readNodeShaped(nodeId, shape);
		}

		[[nodiscard]] LogicalGraphEdge readEdgeShaped(uint64_t edgeId, const EntityReadShape &shape) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readEdgeShaped(edgeId, shape);
			}
			return overlay_.openLogicalGraphReadSnapshot()->readEdgeShaped(edgeId, shape);
		}

		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override {
			return readNodeShaped(nodeId, completeNodeReadShape());
		}

		[[nodiscard]] bool containsNode(uint64_t nodeId) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.containsNode(nodeId)
															 : overlay_.openLogicalGraphReadSnapshot()->containsNode(nodeId);
		}

		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override {
			return readEdgeShaped(edgeId, completeEdgeReadShape());
		}

		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
		findNodesByExactProperties(const std::vector<std::string> &labels,
											const std::vector<std::pair<std::string, PropertyValue>> &properties,
											size_t maximumNodes, size_t maximumPropertyEntries) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.findNodesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries);
			}
			return overlay_.openLogicalGraphReadSnapshot()->findNodesByExactProperties(
					labels, properties, maximumNodes, maximumPropertyEntries);
		}

		void visitNodes(size_t maximumRecords, const std::function<bool(const LogicalGraphNode &)> &visitor) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				derived_snapshot_.visitNodes(maximumRecords, visitor);
				return;
			}
			overlay_.openLogicalGraphReadSnapshot()->visitNodes(maximumRecords, visitor);
		}

		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																						 size_t maximumMetadataPages) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.scanNodesAfter(afterNodeId, maximumRecords, maximumMetadataPages);
			}
			return overlay_.openLogicalGraphReadSnapshot()->scanNodesAfter(afterNodeId, maximumRecords,
																				maximumMetadataPages);
		}

		[[nodiscard]] LogicalGraphAdjacencyPage
		scanAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
							  std::optional<LogicalGraphAdjacencyContinuation> continuation,
							  size_t maximumRecords) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.scanAdjacentEdges(nodeId, direction, std::move(continuation), maximumRecords);
			}
			return overlay_.openLogicalGraphReadSnapshot()->scanAdjacentEdges(nodeId, direction, std::move(continuation),
																												 maximumRecords);
		}

		[[nodiscard]] LogicalGraphAdjacencyTopologyPage
		scanAdjacentEdgeTopologies(uint64_t nodeId, LogicalGraphReadDirection direction,
											std::optional<LogicalGraphAdjacencyContinuation> continuation,
											size_t maximumRecords) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.scanAdjacentEdgeTopologies(nodeId, direction, std::move(continuation), maximumRecords);
			}
			return overlay_.openLogicalGraphReadSnapshot()->scanAdjacentEdgeTopologies(nodeId, direction,
																																	 std::move(continuation), maximumRecords);
		}

		void visitEdges(size_t maximumRecords, const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				derived_snapshot_.visitEdges(maximumRecords, visitor);
				return;
			}
			overlay_.openLogicalGraphReadSnapshot()->visitEdges(maximumRecords, visitor);
		}

		void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
									const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				derived_snapshot_.visitAdjacentEdges(nodeId, direction, maximumEdges, visitor);
				return;
			}
			overlay_.openLogicalGraphReadSnapshot()->visitAdjacentEdges(nodeId, direction, maximumEdges, visitor);
		}

		[[nodiscard]] std::optional<LogicalGraphMutationFence> mutationFence() const override {
			requireActive();
			return overlay_.overlayFence();
		}

		[[nodiscard]] LogicalGraphVectorSearchResult
		searchLogicalVector(const LogicalGraphVectorSearchRequest &request) const override {
			requireActive();
			if (request.cancellation != nullptr) {
				request.cancellation->check();
			}
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.searchLogicalVector(request);
			}
			const std::optional<LogicalGraphVectorIndexDescriptor> index =
					derived_snapshot_.describeLogicalVectorIndex(request);
			if (!index.has_value()) {
				throw std::runtime_error(
						"logical transaction Vector search has no exact overlay evaluator for its pinned generation");
			}
			std::vector<LogicalGraphVectorSearchHit> hits;
			LogicalGraphVectorSearchStats stats;
			stats.used_exact_overlay = true;
			auto graphSnapshot = overlay_.openLogicalGraphReadSnapshot();
			graphSnapshot->visitNodes(request.maximum_mapping_entries, [&](const LogicalGraphNode &node) {
				if (request.cancellation != nullptr) {
					request.cancellation->check();
				}
				++stats.exact_overlay_candidate_scans;
				const std::optional<LogicalGraphVectorSearchHit> hit =
						derived_snapshot_.scoreLogicalVectorExactOverlayCandidate(request, *index, node);
				if (hit.has_value()) {
					hits.push_back(*hit);
				}
				return true;
			});
			if (request.cancellation != nullptr) {
				request.cancellation->check();
			}
			std::ranges::sort(hits, [](const LogicalGraphVectorSearchHit &left,
														 const LogicalGraphVectorSearchHit &right) {
				return left.score == right.score ? left.node_id < right.node_id : left.score < right.score;
			});
			if (hits.size() > request.top_k) {
				hits.resize(request.top_k);
			}
			return {std::move(hits), stats};
		}

	private:
		void requireActive() const {
			if (overlay_.transaction_.state() != LogicalGraphTransactionState::LGTTS_ACTIVE) {
				throw std::logic_error("logical transaction Vector snapshot is no longer active");
			}
		}

		const LogicalGraphTransactionOverlay &overlay_;
		LogicalGraphVectorSearchSnapshot &derived_snapshot_;
	};

	LogicalGraphTransactionOverlay::LogicalGraphTransactionOverlay(LogicalGraphTransaction &transaction) :
		transaction_(transaction) {
		if (transaction_.state() != LogicalGraphTransactionState::LGTTS_ACTIVE) {
			throw std::logic_error("logical transaction overlay requires an active transaction");
		}
		const std::optional<LogicalGraphMutationFence> fence = baseFence();
		if (!fence.has_value() || !fence->isValid()) {
			throw std::logic_error("logical transaction overlay requires an optimistic graph-state fence");
		}
	}

	LogicalGraphTransactionOverlay::~LogicalGraphTransactionOverlay() = default;

	std::unique_ptr<LogicalGraphReadSnapshot> LogicalGraphTransactionOverlay::openLogicalGraphReadSnapshot() const {
		if (transaction_.state() != LogicalGraphTransactionState::LGTTS_ACTIVE) {
			throw std::logic_error("logical transaction overlay cannot open a snapshot after transaction completion");
		}
		return std::make_unique<Snapshot>(*this);
	}

	std::unique_ptr<LogicalGraphVectorSearchSnapshot>
	LogicalGraphTransactionOverlay::openLogicalGraphVectorSearchSnapshot() const {
		if (transaction_.state() != LogicalGraphTransactionState::LGTTS_ACTIVE) {
			throw std::logic_error("logical transaction overlay cannot open a Vector snapshot after transaction completion");
		}
		auto *provider = dynamic_cast<LogicalGraphTransactionVectorSnapshotProvider *>(&transaction_);
		if (provider == nullptr) {
			throw std::runtime_error("logical transaction does not own a pinned Vector search snapshot");
		}
		return std::make_unique<VectorSnapshot>(*this, provider->pinnedLogicalGraphVectorSearchSnapshot());
	}

	std::vector<uint64_t>
	LogicalGraphTransactionOverlay::createLogicalNodes(const std::vector<LogicalGraphNodeCreateRequest> &requests) {
		LogicalGraphMutationBatch batch;
		batch.node_creates = requests;
		return executeLogicalGraphMutationBatch(batch).created_node_ids;
	}

	std::vector<uint64_t>
	LogicalGraphTransactionOverlay::createLogicalEdges(const std::vector<LogicalGraphEdgeCreateRequest> &requests) {
		LogicalGraphMutationBatch batch;
		batch.edge_creates.reserve(requests.size());
		for (const LogicalGraphEdgeCreateRequest &request: requests) {
			batch.edge_creates.push_back(
					{persistedNodeReference(request.source_node_id), persistedNodeReference(request.target_node_id), request.type,
					 request.properties});
		}
		return executeLogicalGraphMutationBatch(batch).created_edge_ids;
	}

	void LogicalGraphTransactionOverlay::replaceLogicalNodeProperties(
			const std::vector<LogicalGraphPropertyReplaceRequest> &requests) {
		LogicalGraphMutationBatch batch;
		batch.property_replacements.reserve(requests.size());
		for (const LogicalGraphPropertyReplaceRequest &request: requests) {
			batch.property_replacements.push_back({persistedNodeReference(request.entity_id), request.properties});
		}
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	void LogicalGraphTransactionOverlay::replaceLogicalEdgeProperties(
			const std::vector<LogicalGraphPropertyReplaceRequest> &requests) {
		LogicalGraphMutationBatch batch;
		batch.property_replacements.reserve(requests.size());
		for (const LogicalGraphPropertyReplaceRequest &request: requests) {
			batch.property_replacements.push_back({persistedEdgeReference(request.entity_id), request.properties});
		}
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	void LogicalGraphTransactionOverlay::deleteLogicalNodes(const std::vector<LogicalGraphNodeDeleteRequest> &requests) {
		LogicalGraphMutationBatch batch;
		batch.node_deletes = requests;
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	void LogicalGraphTransactionOverlay::deleteLogicalEdges(const std::vector<LogicalGraphEdgeDeleteRequest> &requests) {
		LogicalGraphMutationBatch batch;
		batch.edge_deletes = requests;
		static_cast<void>(executeLogicalGraphMutationBatch(batch));
	}

	LogicalGraphMutationBatchResult LogicalGraphTransactionOverlay::executeLogicalGraphMutationBatch(
			const LogicalGraphMutationBatch &batch, std::optional<LogicalGraphMutationFence> expectedFence) {
		if (transaction_.state() != LogicalGraphTransactionState::LGTTS_ACTIVE) {
			throw std::logic_error("logical transaction overlay cannot stage a mutation after transaction completion");
		}
		// A plan built from this overlay observes both the durable base fence and
		// the overlay revision. Comparing it to the base fence alone rejects every
		// statement after the first successful staged mutation.
		if (expectedFence.has_value() && (!expectedFence->isValid() || overlayFence() != expectedFence)) {
			throw LogicalGraphMutationConflict("logical transaction overlay mutation does not match its current read fence");
		}
		if (isEmpty(batch)) {
			return {};
		}
		if (logical_revision_ == std::numeric_limits<uint64_t>::max()) {
			throw std::overflow_error("logical transaction overlay revision overflowed");
		}

		const LogicalGraphMutationBatchResult preview = transaction_.previewMutationBatch(batch);
		const auto nodesBefore = nodes_;
		const auto edgesBefore = edges_;
		try {
			applyBatch(batch, preview);
			const LogicalGraphMutationBatchResult staged = transaction_.stageMutationBatch(batch);
			if (staged != preview) {
				throw std::logic_error("logical transaction changed a batch's provisional identity after validation");
			}
			++logical_revision_;
			return staged;
		} catch (...) {
			nodes_ = nodesBefore;
			edges_ = edgesBefore;
			throw;
		}
	}

	LogicalGraphReadSnapshot &LogicalGraphTransactionOverlay::baseSnapshot() const { return transaction_.readSnapshot(); }

	std::optional<LogicalGraphMutationFence> LogicalGraphTransactionOverlay::baseFence() const {
		return baseSnapshot().mutationFence();
	}

	std::optional<LogicalGraphMutationFence> LogicalGraphTransactionOverlay::overlayFence() const {
		std::optional<LogicalGraphMutationFence> fence = baseFence();
		if (fence.has_value()) {
			fence->logical_revision = logical_revision_;
		}
		return fence;
	}

	LogicalGraphTransactionOverlay::NodeState &LogicalGraphTransactionOverlay::ensureNode(uint64_t nodeId) {
		if (nodeId == 0U) {
			throw std::invalid_argument("logical transaction overlay requires a nonzero Node ID");
		}
		if (const auto existing = nodes_.find(nodeId); existing != nodes_.end()) {
			if (existing->second.deleted) {
				throw std::runtime_error("logical transaction overlay cannot mutate a deleted Node");
			}
			return existing->second;
		}
		auto [inserted, success] = nodes_.emplace(
				nodeId, NodeState{baseSnapshot().readNodeShaped(nodeId, completeNodeReadShape()), false, false});
		static_cast<void>(success);
		return inserted->second;
	}

	LogicalGraphTransactionOverlay::EdgeState &LogicalGraphTransactionOverlay::ensureEdge(uint64_t edgeId) {
		if (edgeId == 0U) {
			throw std::invalid_argument("logical transaction overlay requires a nonzero Edge ID");
		}
		if (const auto existing = edges_.find(edgeId); existing != edges_.end()) {
			if (existing->second.deleted) {
				throw std::runtime_error("logical transaction overlay cannot mutate a deleted Edge");
			}
			return existing->second;
		}
		auto [inserted, success] = edges_.emplace(
				edgeId, EdgeState{baseSnapshot().readEdgeShaped(edgeId, completeEdgeReadShape()), false, false});
		static_cast<void>(success);
		return inserted->second;
	}

	void LogicalGraphTransactionOverlay::applyBatch(const LogicalGraphMutationBatch &batch,
																	 const LogicalGraphMutationBatchResult &result) {
		if (result.created_node_ids.size() != batch.node_creates.size() ||
			result.created_edge_ids.size() != batch.edge_creates.size()) {
			throw std::logic_error("logical transaction overlay received an incomplete provisional identity result");
		}
		const auto resolveReference = [&result](const LogicalGraphMutationEntityReference &reference) {
			switch (reference.reference_kind) {
				case LogicalGraphMutationReferenceKind::LGRK_PERSISTED:
					return reference.value;
				case LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE:
					if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
						reference.value >= result.created_node_ids.size()) {
						throw std::invalid_argument("logical transaction overlay has an invalid created Node reference");
					}
					return result.created_node_ids[static_cast<size_t>(reference.value)];
				case LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE:
					if (reference.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE ||
						reference.value >= result.created_edge_ids.size()) {
						throw std::invalid_argument("logical transaction overlay has an invalid created Edge reference");
					}
					return result.created_edge_ids[static_cast<size_t>(reference.value)];
			}
			throw std::invalid_argument("logical transaction overlay has an unknown entity reference kind");
		};
		for (size_t index = 0U; index < batch.node_creates.size(); ++index) {
			const uint64_t nodeId = result.created_node_ids[index];
			const LogicalGraphNodeCreateRequest &request = batch.node_creates[index];
			NodeState state;
			state.node.node_id = nodeId;
			state.node.labels = canonicalLabels(request.labels);
			state.created = true;
			const PropertyMap properties = makePropertyMap(request.properties);
			state.node.properties = makePropertyEntries(properties);
			if (!nodes_.emplace(nodeId, std::move(state)).second) {
				throw std::logic_error("logical transaction overlay created a duplicate Node identity");
			}
		}

		for (const LogicalGraphNodeLabelMutationRequest &mutation: batch.node_label_replacements) {
			if (mutation.node.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
				throw std::invalid_argument("logical transaction overlay label mutation requires a Node");
			}
			ensureNode(resolveReference(mutation.node)).node.labels = canonicalLabels(mutation.labels);
		}

		for (size_t index = 0U; index < batch.edge_creates.size(); ++index) {
			const LogicalGraphBatchEdgeCreateRequest &request = batch.edge_creates[index];
			if (request.source.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE ||
				request.target.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
				throw std::invalid_argument("logical transaction overlay Edge creation has invalid endpoints");
			}
			const uint64_t sourceId = resolveReference(request.source);
			const uint64_t targetId = resolveReference(request.target);
			if (!isNodeVisible(sourceId) || !isNodeVisible(targetId)) {
				throw std::runtime_error("logical transaction overlay Edge creation targets a deleted Node");
			}
			const uint64_t edgeId = result.created_edge_ids[index];
			EdgeState state;
			state.edge = {edgeId, sourceId, targetId, request.type, {}};
			state.created = true;
			const PropertyMap properties = makePropertyMap(request.properties);
			state.edge.properties = makePropertyEntries(properties);
			if (!edges_.emplace(edgeId, std::move(state)).second) {
				throw std::logic_error("logical transaction overlay created a duplicate Edge identity");
			}
		}

		for (const LogicalGraphPropertyMutationRequest &mutation: batch.property_replacements) {
			const uint64_t entityId = resolveReference(mutation.entity);
			const PropertyMap properties = makePropertyMap(mutation.properties);
			if (mutation.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
				NodeState &state = ensureNode(entityId);
				state.node.properties = makePropertyEntries(properties);
			} else if (mutation.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE) {
				EdgeState &state = ensureEdge(entityId);
				state.edge.properties = makePropertyEntries(properties);
			} else {
				throw std::invalid_argument("logical transaction overlay Property mutation has an unknown entity kind");
			}
		}

		for (const LogicalGraphEdgeDeleteRequest &request: batch.edge_deletes) {
			EdgeState &edge = ensureEdge(request.edge_id);
			edge.deleted = true;
		}
		for (const LogicalGraphNodeDeleteRequest &request: batch.node_deletes) {
			NodeState &node = ensureNode(request.node_id);
			if (request.detach) {
				std::set<uint64_t> detachedIds;
				const auto markIncident = [this, &detachedIds, &request](const LogicalGraphEdge &edge) {
					if (const auto existing = edges_.find(edge.edge_id);
						existing != edges_.end() && existing->second.deleted) {
						return true;
					}
					if (detachedIds.insert(edge.edge_id).second) {
						if (detachedIds.size() > request.maximum_detach_edges) {
							throw std::runtime_error("logical transaction overlay detach delete exceeded its explicit Edge bound");
						}
							ensureEdge(edge.edge_id).deleted = true;
					}
					return true;
				};
				const auto markIncidentTopology = [&markIncident](const LogicalGraphEdgeTopology &topology) {
					const LogicalGraphEdge edge{topology.edge_id, topology.source_node_id, topology.target_node_id, topology.type};
					return markIncident(edge);
				};
				baseSnapshot().visitAdjacentEdgeTopologies(request.node_id, LogicalGraphReadDirection::LGRD_OUTGOING,
														 request.maximum_detach_edges, markIncidentTopology);
				baseSnapshot().visitAdjacentEdgeTopologies(request.node_id, LogicalGraphReadDirection::LGRD_INCOMING,
														 request.maximum_detach_edges, markIncidentTopology);
				for (auto &[edgeId, edge]: edges_) {
					if (edge.created && !edge.deleted &&
						(edge.edge.source_node_id == request.node_id || edge.edge.target_node_id == request.node_id)) {
						if (!detachedIds.insert(edgeId).second) {
							continue;
						}
						if (detachedIds.size() > request.maximum_detach_edges) {
							throw std::runtime_error("logical transaction overlay detach delete exceeded its explicit Edge bound");
						}
						edge.deleted = true;
					}
				}
			} else if (hasVisibleIncidentEdge(request.node_id)) {
				throw std::runtime_error(
						"logical transaction overlay non-detach delete requires an isolated Node");
			}
			node.deleted = true;
		}
	}

	bool LogicalGraphTransactionOverlay::hasVisibleIncidentEdge(uint64_t nodeId) const {
		if (nodeId == 0U || edges_.size() == std::numeric_limits<size_t>::max()) {
			throw std::invalid_argument("logical transaction overlay cannot bound an incident Edge inspection");
		}
		for (const auto &[edgeId, edge]: edges_) {
			if (edge.created && !edge.deleted &&
				(edge.edge.source_node_id == nodeId || edge.edge.target_node_id == nodeId)) {
				return true;
			}
		}
		if (const auto node = nodes_.find(nodeId); node != nodes_.end() && node->second.created) {
			// A transaction-created Node has no base-snapshot adjacency. Every
			// possible incident Edge is already represented in this overlay.
			return false;
		}
		// Every persisted Edge already hidden by the overlay has an entry in
		// `edges_`. Scanning one more record than that set either finds a visible
		// relationship or proves the remaining adjacency is entirely deleted.
		const size_t maximumEdges = edges_.size() + 1U;
		for (const LogicalGraphReadDirection direction:
				{LogicalGraphReadDirection::LGRD_OUTGOING, LogicalGraphReadDirection::LGRD_INCOMING}) {
			bool found = false;
			baseSnapshot().visitAdjacentEdgeTopologies(nodeId, direction, maximumEdges,
					[this, &found](const LogicalGraphEdgeTopology &topology) {
				const LogicalGraphEdge edge{topology.edge_id, topology.source_node_id, topology.target_node_id, topology.type};
				if (isEdgeVisible(edge, edge.edge_id)) {
					found = true;
					return false;
				}
				return true;
				});
			if (found) {
				return true;
			}
		}
		return false;
	}

	bool LogicalGraphTransactionOverlay::isNodeVisible(uint64_t nodeId) const {
		if (const auto state = nodes_.find(nodeId); state != nodes_.end()) {
			return !state->second.deleted;
		}
		if (const auto cached = persisted_node_visibility_.find(nodeId); cached != persisted_node_visibility_.end()) {
			return cached->second;
		}
		const bool visible = baseSnapshot().containsNode(nodeId);
		persisted_node_visibility_.emplace(nodeId, visible);
		return visible;
	}

	bool LogicalGraphTransactionOverlay::isEdgeVisible(const LogicalGraphEdge &edge, uint64_t edgeId) const {
		if (const auto state = edges_.find(edgeId); state != edges_.end() && state->second.deleted) {
			return false;
		}
		return isNodeVisible(edge.source_node_id) && isNodeVisible(edge.target_node_id);
	}

} // namespace graph::storage
