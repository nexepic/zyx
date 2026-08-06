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

		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override {
			const auto state = overlay_.nodes_.find(nodeId);
			if (state != overlay_.nodes_.end()) {
				if (state->second.deleted) {
					throw std::runtime_error("logical transaction overlay Node was deleted");
				}
				return state->second.node;
			}
			return overlay_.baseSnapshot().readNode(nodeId);
		}

		[[nodiscard]] bool containsNode(uint64_t nodeId) const override {
			if (const auto state = overlay_.nodes_.find(nodeId); state != overlay_.nodes_.end()) {
				return !state->second.deleted;
			}
			return overlay_.baseSnapshot().containsNode(nodeId);
		}

		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override {
			const auto state = overlay_.edges_.find(edgeId);
			if (state != overlay_.edges_.end()) {
				if (state->second.deleted || !overlay_.isEdgeVisible(state->second.edge, edgeId)) {
					throw std::runtime_error("logical transaction overlay Edge was deleted");
				}
				return state->second.edge;
			}
			const LogicalGraphEdge edge = overlay_.baseSnapshot().readEdge(edgeId);
			if (!overlay_.isEdgeVisible(edge, edgeId)) {
				throw std::runtime_error("logical transaction overlay Edge was deleted");
			}
			return edge;
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
			const auto matches = [this, &labels, &properties, maximumPropertyEntries](const LogicalGraphNode &node) {
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
						result.nodes.push_back(readNode(node.node_id));
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
						edges.push_back(readEdge(edge.edge_id));
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
				return visitor(readEdge(edge.edge_id));
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
			return overlay_.baseFence();
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
		if (expectedFence.has_value() && (!expectedFence->isValid() || baseFence() != expectedFence)) {
			throw LogicalGraphMutationConflict("logical transaction overlay mutation does not match its pinned read fence");
		}
		if (isEmpty(batch)) {
			return {};
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
		auto [inserted, success] = nodes_.emplace(nodeId, NodeState{baseSnapshot().readNode(nodeId), false, false});
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
		auto [inserted, success] = edges_.emplace(edgeId, EdgeState{baseSnapshot().readEdge(edgeId), false, false});
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
				baseSnapshot().visitAdjacentEdges(request.node_id, LogicalGraphReadDirection::LGRD_OUTGOING,
																 request.maximum_detach_edges, markIncident);
				baseSnapshot().visitAdjacentEdges(request.node_id, LogicalGraphReadDirection::LGRD_INCOMING,
																 request.maximum_detach_edges, markIncident);
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
			baseSnapshot().visitAdjacentEdges(nodeId, direction, maximumEdges, [this, &found](const LogicalGraphEdge &edge) {
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
