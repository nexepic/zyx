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
#include <unordered_map>
#include <utility>

#include "graph/core/Types.hpp"
#include "graph/core/GuardedPropertyCopy.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"

namespace graph::storage {

	namespace {

		template<typename Edge>
		void ensureAdjacencyCapacity(std::vector<Edge> &edges, size_t maximumRecords) {
			if (edges.size() >= maximumRecords)
				throw std::runtime_error("logical transaction adjacency exceeded its record bound");
			if (edges.size() < edges.capacity()) return;
			const size_t capacity = edges.capacity();
			const size_t next = capacity == 0U ? std::min<size_t>(maximumRecords, 16U)
				: capacity + std::min(capacity, maximumRecords - capacity);
			edges.reserve(next);
		}

		size_t checkedTopologySum(size_t left, size_t right) {
			if (right > std::numeric_limits<size_t>::max() - left)
				throw std::overflow_error("overlay topology buffer size overflow");
			return left + right;
		}

		size_t topologyStorageBytes(size_t capacity, size_t typeBytes) {
			if (capacity > std::numeric_limits<size_t>::max() / sizeof(LogicalGraphEdgeTopology))
				throw std::overflow_error("overlay topology slot size overflow");
			return checkedTopologySum(capacity * sizeof(LogicalGraphEdgeTopology), typeBytes);
		}

		size_t topologyTypeBytes(const std::vector<LogicalGraphEdgeTopology> &edges) {
			size_t bytes = 0U;
			for (const auto &edge: edges) bytes = checkedTopologySum(bytes, edge.type.capacity());
			return bytes;
		}

		void admitTopologyStorage(const std::function<void(size_t)> &admission,
			const std::vector<LogicalGraphEdgeTopology> &edges, size_t typeBytes, size_t otherBytes = 0U) {
			if (admission) admission(checkedTopologySum(topologyStorageBytes(edges.capacity(), typeBytes), otherBytes));
		}

		void reserveTopologySlot(std::vector<LogicalGraphEdgeTopology> &edges, size_t maximumRecords,
			size_t typeBytes, const std::function<void(size_t)> &admission, size_t otherBytes = 0U) {
			if (edges.size() >= maximumRecords)
				throw std::runtime_error("overlay topology exceeded its record bound");
			if (edges.size() == edges.capacity()) {
				const size_t oldCapacity = edges.capacity();
				const size_t next = oldCapacity == 0U ? std::min<size_t>(maximumRecords, 16U)
					: oldCapacity + std::min(oldCapacity, maximumRecords - oldCapacity);
				// Both allocations coexist during vector relocation.
				if (admission) admission(checkedTopologySum(
					topologyStorageBytes(checkedTopologySum(oldCapacity, next), typeBytes), otherBytes));
				edges.reserve(next);
			}
			admitTopologyStorage(admission, edges, typeBytes, otherBytes);
		}

		void admitStagedProperty(const PropertyValue &value, const std::function<void(size_t)> *admission) {
			if (admission == nullptr) return;
			const size_t bytes = CanonicalPropertyValueCodec::encodedSize(value);
			if (bytes > std::numeric_limits<size_t>::max() - sizeof(uint32_t)) {
				throw std::overflow_error("transaction Property entry size overflowed");
			}
			(*admission)(bytes + sizeof(uint32_t));
		}

		PropertyValue copyStagedValue(const PropertyValue &source, const std::function<void(size_t)> *admission) {
			if (admission == nullptr) return source;
			return graph::property_copy::copyPropertyValueGuarded(source, [&] { (*admission)(0U); });
		}

		LogicalGraphPropertyEntry copyStagedEntry(const LogicalGraphPropertyEntry &entry,
			const std::function<void(size_t)> *admission) {
			if (admission == nullptr) return entry;
			auto checkpoint = [&] { (*admission)(0U); };
			auto key = graph::property_copy::copyPropertyStringGuarded(entry.key, checkpoint);
			return {std::move(key), copyStagedValue(entry.value, admission)};
		}

		std::vector<LogicalGraphPropertyEntry> copyStagedProperties(
			const std::vector<LogicalGraphPropertyEntry> &source, const std::function<void(size_t)> *admission) {
			if (admission == nullptr) return source;
			// Zero-byte admission is a cancellation checkpoint, not a new charge.
			const auto checkpoint = [&] { (*admission)(0U); };
			std::vector<LogicalGraphPropertyEntry> result;
			result.reserve(source.size());
			for (const auto &entry : source) {
				auto guard = checkpoint;
				auto key = graph::property_copy::copyPropertyStringGuarded(entry.key, guard);
				result.push_back({std::move(key), graph::property_copy::copyPropertyValueGuarded(entry.value, guard)});
			}
			return result;
		}

		size_t adjacencyBufferAdd(size_t left, size_t right) {
			if (right > std::numeric_limits<size_t>::max() - left) throw std::overflow_error("overlay adjacency buffers overflowed");
			return left + right;
		}
		size_t adjacencySlots(size_t count, size_t width) {
			if (count > std::numeric_limits<size_t>::max() / width) throw std::overflow_error("overlay adjacency slots overflowed");
			return count * width;
		}
		size_t adjacencyEdgeMetadata(const LogicalGraphEdge &edge) {
			size_t bytes = adjacencySlots(edge.properties.capacity(), sizeof(LogicalGraphPropertyEntry));
			if (!edge.type.empty()) bytes = adjacencyBufferAdd(bytes, edge.type.capacity());
			for (const auto &entry : edge.properties) bytes = adjacencyBufferAdd(bytes, entry.key.capacity());
			return bytes;
		}
		size_t adjacencyMetadata(const std::vector<LogicalGraphEdge> &edges) {
			size_t bytes = 0U;
			for (const auto &edge : edges) bytes = adjacencyBufferAdd(bytes, adjacencyEdgeMetadata(edge));
			return bytes;
		}
		size_t adjacencyBuffers(const std::vector<LogicalGraphEdge> &edges, size_t metadataBytes) {
			return adjacencyBufferAdd(adjacencySlots(edges.capacity(), sizeof(LogicalGraphEdge)), metadataBytes);
		}
		void growAdmittedAdjacency(std::vector<LogicalGraphEdge> &edges, size_t bound,
			const std::function<void(size_t)> *admission, size_t metadataBytes, size_t sourceBytes = 0U) {
			if (admission == nullptr) { ensureAdjacencyCapacity(edges, bound); return; }
			if (edges.size() >= bound) throw std::runtime_error("overlay adjacency exceeded its record bound");
			if (edges.size() == edges.capacity()) {
				const size_t old = edges.capacity();
				const size_t next = old == 0U ? std::min<size_t>(bound, 16U) : old + std::min(old, bound - old);
				(*admission)(adjacencyBufferAdd(adjacencyBufferAdd(sourceBytes, adjacencyBuffers(edges, metadataBytes)), adjacencySlots(next, sizeof(LogicalGraphEdge))));
				edges.reserve(next);
			}
			(*admission)(adjacencyBufferAdd(sourceBytes, adjacencyBuffers(edges, metadataBytes)));
		}

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
		[[nodiscard]] auto readRevisionCheckpoint() const {
			const uint64_t revision = overlay_.logical_revision_;
			return [this, revision] {
				requireActive();
				if (overlay_.logical_revision_ != revision)
					throw std::runtime_error("logical transaction overlay changed during read callback");
			};
		}

		[[nodiscard]] std::function<void(size_t)> guardReadAdmission(const std::function<void(size_t)> &admission) const {
			return [check = readRevisionCheckpoint(), &admission](size_t bytes) {
				check();
				admission(bytes);
				check();
			};
		}

		template<typename Visitor>
		[[nodiscard]] auto guardReadVisitor(const Visitor &visitor) const {
			if (!visitor) throw std::invalid_argument("overlay read visitor must not be empty");
			return [check = readRevisionCheckpoint(), &visitor](const auto &...args) {
				check();
				const bool keepGoing = visitor(args...);
				check();
				return keepGoing;
			};
		}

	public:
		explicit Snapshot(const LogicalGraphTransactionOverlay &overlay) : overlay_(overlay) {}

		[[nodiscard]] bool supportsShapedPropertyReadAdmission() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsShapedPropertyReadAdmission();
		}

		void visitEdgeTopologies(size_t maximumRecords,
			const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
			requireActive();
			if (!visitor || maximumRecords == 0U) throw std::invalid_argument("overlay topology visitor requires a visitor and positive bounds");
			const auto guarded = guardReadVisitor(visitor);
			if (!supportsNativeEdgeRefScans()) {
				LogicalGraphReadSnapshot::visitEdgeTopologies(maximumRecords, guarded);
				return;
			}
			visitEdgeRefs(maximumRecords, [&](const EntityRef &reference) {
				const auto edge = readEdgeShaped(reference.entity_id, {.include_type = true});
				if (!reference.is_edge || edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U ||
					edge.edge_id != reference.entity_id || edge.source_node_id != reference.source_node_id ||
					edge.target_node_id != reference.target_node_id)
					throw std::runtime_error("overlay topology changed Edge reference identity");
				return guarded(LogicalGraphEdgeTopology{edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type});
			});
		}
		void visitAdjacentEdgeTopologies(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
			const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
			requireActive();
			if (!visitor || maximumEdges == 0U) throw std::invalid_argument("overlay topology visitor requires a visitor and positive bounds");
			const auto guarded = guardReadVisitor(visitor);
			if (!supportsNativeAdjacentEdgeTopologyPages()) {
				LogicalGraphReadSnapshot::visitAdjacentEdgeTopologies(nodeId, direction, maximumEdges, guarded);
				return;
			}
			std::optional<LogicalGraphAdjacencyContinuation> continuation;
			size_t remaining = maximumEdges;
			while (remaining > 0U) {
				const size_t requested = std::min(remaining, size_t{256U});
				const auto page = scanAdjacentEdgeTopologies(nodeId, direction, continuation, requested);
				validateBaseAdjacencyPage(page, nodeId, direction, continuation, requested);
				for (const auto &edge: page.edges) {
					--remaining;
					if (!guarded(edge)) return;
				}
				if (!page.continuation) return;
				continuation = page.continuation;
			}
		}

		[[nodiscard]] bool supportsPropertyTokenResolution() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsPropertyTokenResolution();
		}
		[[nodiscard]] bool supportsPropertyPresenceProbes() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsPropertyPresenceProbes();
		}
		[[nodiscard]] std::optional<bool> nodePropertyExists(uint64_t nodeId, std::string_view key) const override {
			requireActive();
			const auto state = overlay_.nodes_.find(nodeId);
			if (state == overlay_.nodes_.end()) return overlay_.baseSnapshot().nodePropertyExists(nodeId, key);
			if (state->second.deleted) throw std::runtime_error("logical transaction overlay Node was deleted");
			return std::ranges::any_of(state->second.node.properties, [&](const auto &entry) { return entry.key == key; });
		}
		[[nodiscard]] std::optional<bool> edgePropertyExists(uint64_t edgeId, std::string_view key) const override {
			requireActive();
			const auto state = overlay_.edges_.find(edgeId);
			if (state == overlay_.edges_.end()) {
				static_cast<void>(readEdgeShaped(edgeId, {}));
				return overlay_.baseSnapshot().edgePropertyExists(edgeId, key);
			}
			if (state->second.deleted || !overlay_.isEdgeVisible(state->second.edge, edgeId))
				throw std::runtime_error("logical transaction overlay Edge was deleted");
			return std::ranges::any_of(state->second.edge.properties, [&](const auto &entry) { return entry.key == key; });
		}

		[[nodiscard]] std::optional<uint32_t> findPropertyToken(std::string_view name) const override {
			requireActive();
			return overlay_.baseSnapshot().findPropertyToken(name);
		}

		[[nodiscard]] LogicalGraphNode readNodeShaped(uint64_t nodeId, const EntityReadShape &shape) const override {
			return readNodeShapedImpl(nodeId, shape, nullptr);
		}

		[[nodiscard]] LogicalGraphNode readNodeShapedImpl(uint64_t nodeId, const EntityReadShape &shape,
				const std::function<void(size_t)> *admission) const {
			requireActive();
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
				for (const auto &entry : source.properties) admitStagedProperty(entry.value, admission);
				result.properties = copyStagedProperties(source.properties, admission);
				return result;
			}
			// Staged records retain logical Property names. Resolve each name through
			// the pinned base schema when possible; if a generic reader cannot expose
			// tokens, conservatively retain the properties rather than returning an
			// incomplete logical value.
			for (const LogicalGraphPropertyEntry &entry: source.properties) {
				const std::optional<uint32_t> token = overlay_.baseSnapshot().findPropertyToken(entry.key);
				if (token.has_value() && std::ranges::binary_search(shape.property_tokens, *token)) {
					admitStagedProperty(entry.value, admission);
					result.properties.push_back(copyStagedEntry(entry, admission));
				} else if (!token.has_value() && !shape.property_tokens.empty() &&
					!overlay_.baseSnapshot().supportsPropertyTokenResolution()) {
					admitStagedProperty(entry.value, admission);
					result.properties.push_back(copyStagedEntry(entry, admission));
				}
			}
			return result;
		}

		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSorted(std::span<const uint64_t> nodeIds, const EntityReadShape &shape) const override {
			return readNodesShapedSortedImpl(nodeIds, shape, nullptr);
		}

		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSortedWithAdmission(std::span<const uint64_t> nodeIds, const EntityReadShape &shape,
				const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("shaped Property admission callback must not be empty");
			if (!supportsShapedPropertyReadAdmission()) throw std::logic_error("overlay base reader does not support shaped Property admission");
			const auto guarded = guardReadAdmission(admission);
			return readNodesShapedSortedImpl(nodeIds, shape, &guarded);
		}

		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSortedImpl(std::span<const uint64_t> nodeIds, const EntityReadShape &shape,
				const std::function<void(size_t)> *admission) const {
			requireActive();
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
			std::vector<LogicalGraphNode> baseNodes = baseIds.empty()
					? std::vector<LogicalGraphNode>{}
					: admission == nullptr ? overlay_.baseSnapshot().readNodesShapedSorted(baseIds, shape)
						: overlay_.baseSnapshot().readNodesShapedSortedWithAdmission(baseIds, shape, *admission);
			if (baseNodes.size() != baseIds.size()) {
				throw std::runtime_error("logical transaction overlay base Node batch returned an invalid cardinality");
			}
			std::vector<LogicalGraphNode> result;
			result.reserve(nodeIds.size());
			size_t baseOrdinal = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (overlay_.nodes_.contains(nodeId)) {
					result.push_back(readNodeShapedImpl(nodeId, shape, admission));
				} else {
					LogicalGraphNode &node = baseNodes[baseOrdinal];
					if (node.node_id != nodeId) {
						throw std::runtime_error("logical transaction overlay base Node batch returned an out-of-order record");
					}
					result.push_back(std::move(node));
					++baseOrdinal;
				}
			}
			return result;
		}

		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override {
			requireActive();
			return readNodeShaped(nodeId, completeNodeReadShape());
		}

		[[nodiscard]] bool supportsNodeExistenceProbes() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsNodeExistenceProbes();
		}

		[[nodiscard]] std::vector<bool> containsNodesSorted(std::span<const uint64_t> nodeIds) const override {
			requireActive();
			std::vector<uint64_t> baseIds;
			baseIds.reserve(nodeIds.size());
			uint64_t previous = 0U;
			for (const uint64_t id: nodeIds) {
				if (id == 0U || id <= previous) {
					throw std::invalid_argument("logical transaction overlay existence batch requires increasing Node IDs");
				}
				previous = id;
				if (!overlay_.nodes_.contains(id)) baseIds.push_back(id);
			}
			const auto baseMatches = baseIds.empty() ? std::vector<bool>{}
					: overlay_.baseSnapshot().containsNodesSorted(baseIds);
			if (baseMatches.size() != baseIds.size()) {
				throw std::runtime_error("logical transaction overlay base existence batch returned an invalid cardinality");
			}
			std::vector<bool> result;
			result.reserve(nodeIds.size());
			size_t baseOrdinal = 0U;
			for (const uint64_t id: nodeIds) {
				const auto state = overlay_.nodes_.find(id);
				result.push_back(state == overlay_.nodes_.end() ? baseMatches[baseOrdinal++] : !state->second.deleted);
			}
			return result;
		}

		[[nodiscard]] bool containsNode(uint64_t nodeId) const override {
			requireActive();
			if (const auto state = overlay_.nodes_.find(nodeId); state != overlay_.nodes_.end()) {
				return !state->second.deleted;
			}
			return overlay_.baseSnapshot().containsNode(nodeId);
		}

		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override {
			requireActive();
			return readEdgeShaped(edgeId, completeEdgeReadShape());
		}

		[[nodiscard]] LogicalGraphEdge readEdgeShaped(uint64_t edgeId, const EntityReadShape &shape) const override {
			return readEdgeShapedImpl(edgeId, shape, nullptr);
		}

		[[nodiscard]] LogicalGraphEdge readEdgeShapedImpl(uint64_t edgeId, const EntityReadShape &shape,
				const std::function<void(size_t)> *admission) const {
			requireActive();
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
				for (const auto &entry : source.properties) admitStagedProperty(entry.value, admission);
				result.properties = copyStagedProperties(source.properties, admission);
				return result;
			}
			for (const LogicalGraphPropertyEntry &entry: source.properties) {
				const std::optional<uint32_t> token = overlay_.baseSnapshot().findPropertyToken(entry.key);
				if (token.has_value() && std::ranges::binary_search(shape.property_tokens, *token)) {
					admitStagedProperty(entry.value, admission);
					result.properties.push_back(copyStagedEntry(entry, admission));
				} else if (!token.has_value() && !shape.property_tokens.empty() &&
					!overlay_.baseSnapshot().supportsPropertyTokenResolution()) {
					// Preserve correctness for readers that do not publish token lookup.
					admitStagedProperty(entry.value, admission);
					result.properties.push_back(copyStagedEntry(entry, admission));
				}
			}
			return result;
		}

		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSorted(std::span<const uint64_t> edgeIds, const EntityReadShape &shape) const override {
			return readEdgesShapedSortedImpl(edgeIds, shape, nullptr);
		}

		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSortedWithAdmission(std::span<const uint64_t> edgeIds, const EntityReadShape &shape,
				const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("shaped Property admission callback must not be empty");
			if (!supportsShapedPropertyReadAdmission()) throw std::logic_error("overlay base reader does not support shaped Property admission");
			const auto guarded = guardReadAdmission(admission);
			return readEdgesShapedSortedImpl(edgeIds, shape, &guarded);
		}

		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSortedImpl(std::span<const uint64_t> edgeIds, const EntityReadShape &shape,
				const std::function<void(size_t)> *admission) const {
			requireActive();
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
			std::vector<LogicalGraphEdge> baseEdges = baseIds.empty()
					? std::vector<LogicalGraphEdge>{}
					: admission == nullptr ? overlay_.baseSnapshot().readEdgesShapedSorted(baseIds, shape)
						: overlay_.baseSnapshot().readEdgesShapedSortedWithAdmission(baseIds, shape, *admission);
			if (baseEdges.size() != baseIds.size()) {
				throw std::runtime_error("logical transaction overlay base Edge batch returned an invalid cardinality");
			}
			std::vector<LogicalGraphEdge> result;
			result.reserve(edgeIds.size());
			size_t baseOrdinal = 0U;
			for (const uint64_t edgeId: edgeIds) {
				if (overlay_.edges_.contains(edgeId)) {
					result.push_back(readEdgeShapedImpl(edgeId, shape, admission));
				} else {
					LogicalGraphEdge &edge = baseEdges[baseOrdinal];
					if (edge.edge_id != edgeId || !overlay_.isEdgeVisible(edge, edgeId)) {
						throw std::runtime_error("logical transaction overlay base Edge batch returned an invalid record");
					}
					result.push_back(std::move(edge));
					++baseOrdinal;
				}
			}
			return result;
		}

		[[nodiscard]] bool supportsNativeNodeRefScans() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsNativeNodeRefScans();
		}

		void visitNodeRefs(size_t maximumRecords, const std::function<bool(const EntityRef &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (!supportsNativeNodeRefScans()) {
				LogicalGraphReadSnapshot::visitNodeRefs(maximumRecords, guarded);
				return;
			}
			if (!visitor) throw std::invalid_argument("logical transaction overlay Node reference scan requires a visitor");
			visitMergedNodeRefs(maximumRecords, nullptr, [&](const EntityRef &reference, bool) { return guarded(reference); });
		}

		void visitNodeRefsWithLabelMatches(size_t maximumRecords, const std::vector<std::string> &labels,
			const std::function<bool(const EntityRef &, bool)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (!supportsNativeNodeRefScans()) {
				LogicalGraphReadSnapshot::visitNodeRefsWithLabelMatches(maximumRecords, labels, guarded);
				return;
			}
			visitMergedNodeRefs(maximumRecords, &labels, guarded);
		}

	private:
		void visitMergedNodeRefs(size_t maximumRecords, const std::vector<std::string> *labels,
			const std::function<bool(const EntityRef &, bool)> &visitor) const {
			if (maximumRecords == 0U || !visitor) {
				throw std::invalid_argument("logical transaction overlay Node reference scan requires positive bounds and a visitor");
			}
			if (labels != nullptr && std::ranges::any_of(*labels, [](const std::string &label) { return label.empty(); })) {
				throw std::invalid_argument("logical transaction overlay Node label scan requires non-empty labels");
			}
			// Base scans count deleted records too. Add only known persisted
			// deletions to that source allowance; emitted visible records retain
			// the caller's original bound. Never gather the base graph or payloads.
			size_t baseAllowance = maximumRecords;
			for (const auto &[id, state]: overlay_.nodes_) {
				static_cast<void>(id);
				if (state.deleted && !state.created && baseAllowance != std::numeric_limits<size_t>::max()) ++baseAllowance;
			}
			auto staged = overlay_.nodes_.begin();
			const auto advanceCreated = [&] {
				while (staged != overlay_.nodes_.end() && (!staged->second.created || staged->second.deleted)) ++staged;
			};
			advanceCreated();
			size_t emitted = 0U;
			bool keepGoing = true;
			const auto emit = [&](const EntityRef &reference, bool matches) {
				if (emitted == maximumRecords) throw std::runtime_error("logical transaction overlay Node reference scan exceeded its record bound");
				++emitted;
				keepGoing = visitor(reference, matches);
				return keepGoing;
			};
			const auto stagedLabelsMatch = [&](const LogicalGraphNode &node) {
				return labels == nullptr || std::ranges::all_of(*labels, [&](const std::string &label) {
					return std::ranges::find(node.labels, label) != node.labels.end();
				});
			};
			const auto emitCreatedBefore = [&](std::optional<uint64_t> before) {
				while (staged != overlay_.nodes_.end() && (!before.has_value() || staged->first < *before)) {
					if (!emit({staged->first, false, 0U, 0U, 0U}, stagedLabelsMatch(staged->second.node))) return false;
					++staged;
					advanceCreated();
				}
				return true;
			};
			uint64_t previous = 0U;
			const auto consumeBase = [&](const EntityRef &reference, bool matches) {
				if (!keepGoing) throw std::runtime_error("logical transaction overlay base Node source ignored visitor stop");
				if (reference.entity_id == 0U || reference.is_edge || reference.entity_id <= previous) {
					throw std::runtime_error("logical transaction overlay base Node source returned an invalid reference order");
				}
				previous = reference.entity_id;
				const auto state = overlay_.nodes_.find(reference.entity_id);
				if (state != overlay_.nodes_.end() && state->second.created) {
					throw std::runtime_error("logical transaction overlay created Node collides with a base reference");
				}
				if (!emitCreatedBefore(reference.entity_id)) return false;
				if (state != overlay_.nodes_.end()) {
					if (state->second.deleted) return true;
					matches = stagedLabelsMatch(state->second.node);
				}
				return emit(reference, matches);
			};
			if (labels == nullptr) {
				overlay_.baseSnapshot().visitNodeRefs(baseAllowance, [&](const EntityRef &reference) { return consumeBase(reference, true); });
			} else {
				overlay_.baseSnapshot().visitNodeRefsWithLabelMatches(baseAllowance, *labels, consumeBase);
			}
			if (keepGoing) static_cast<void>(emitCreatedBefore(std::nullopt));
		}

	public:

		[[nodiscard]] bool supportsNativeEdgeRefScans() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsNativeEdgeRefScans();
		}

		void visitEdgeRefs(size_t maximumRecords, const std::function<bool(const EntityRef &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (!supportsNativeEdgeRefScans()) {
				LogicalGraphReadSnapshot::visitEdgeRefs(maximumRecords, guarded);
				return;
			}
			if (maximumRecords == 0U || !visitor) {
				throw std::invalid_argument("logical transaction overlay Edge reference scan requires positive bounds and a visitor");
			}
			size_t baseAllowance = maximumRecords;
			for (const auto &[id, state]: overlay_.edges_) {
				static_cast<void>(id);
				if (state.deleted && !state.created && baseAllowance != std::numeric_limits<size_t>::max()) ++baseAllowance;
			}
			const auto endpointVisible = [&](uint64_t id) {
				const auto node = overlay_.nodes_.find(id);
				return node == overlay_.nodes_.end() || !node->second.deleted;
			};
			auto staged = overlay_.edges_.begin();
			const auto advanceCreated = [&] {
				while (staged != overlay_.edges_.end() && (!staged->second.created || staged->second.deleted ||
					!endpointVisible(staged->second.edge.source_node_id) || !endpointVisible(staged->second.edge.target_node_id))) ++staged;
			};
			advanceCreated();
			size_t emitted = 0U;
			bool keepGoing = true;
			const auto emit = [&](const EntityRef &reference) {
				if (emitted == maximumRecords) throw std::runtime_error("logical transaction overlay Edge reference scan exceeded its record bound");
				++emitted;
				keepGoing = guarded(reference);
				return keepGoing;
			};
			const auto emitCreatedBefore = [&](std::optional<uint64_t> before) {
				while (staged != overlay_.edges_.end() && (!before.has_value() || staged->first < *before)) {
					const auto &edge = staged->second.edge;
					if (!emit({staged->first, true, edge.source_node_id, edge.target_node_id,
							staged->second.relationship_type_token})) return false;
					++staged;
					advanceCreated();
				}
				return true;
			};
			uint64_t previous = 0U;
			overlay_.baseSnapshot().visitEdgeRefs(baseAllowance, [&](const EntityRef &reference) {
				if (!keepGoing) throw std::runtime_error("logical transaction overlay base Edge source ignored visitor stop");
				if (reference.entity_id == 0U || !reference.is_edge || reference.source_node_id == 0U ||
					reference.target_node_id == 0U || reference.entity_id <= previous) {
					throw std::runtime_error("logical transaction overlay base Edge source returned invalid topology or order");
				}
				previous = reference.entity_id;
				const auto state = overlay_.edges_.find(reference.entity_id);
				if (state != overlay_.edges_.end() && state->second.created) {
					throw std::runtime_error("logical transaction overlay created Edge collides with a base reference");
				}
				if (!emitCreatedBefore(reference.entity_id)) return false;
				if ((state != overlay_.edges_.end() && state->second.deleted) ||
					!endpointVisible(reference.source_node_id) || !endpointVisible(reference.target_node_id)) return true;
				return emit(reference);
			});
			if (keepGoing) static_cast<void>(emitCreatedBefore(std::nullopt));
		}

		[[nodiscard]] bool supportsNativeRelationshipTypeTokens() const override {
			requireActive();
			if (!overlay_.baseSnapshot().supportsNativeRelationshipTypeTokens()) return false;
			// Types absent from the pinned catalog still require the string fallback.
			// Property replacement and deletion do not invalidate base type identity.
			return std::ranges::all_of(overlay_.edges_, [&](const auto &entry) {
				return !entry.second.created || entry.second.deleted ||
					entry.second.relationship_type_token != 0U;
			});
		}

		[[nodiscard]] bool supportsNativeRelationshipTypeTokens(std::string_view type) const override {
			requireActive();
			return overlay_.baseSnapshot().supportsNativeRelationshipTypeTokens() &&
				overlay_.baseSnapshot().findRelationshipTypeToken(type).has_value();
		}

		[[nodiscard]] std::optional<uint64_t> findRelationshipTypeToken(std::string_view name) const override {
			requireActive();
			return overlay_.baseSnapshot().findRelationshipTypeToken(name);
		}

		[[nodiscard]] bool supportsPropertyNonNullProbes() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsPropertyNonNullProbes();
		}

		[[nodiscard]] std::optional<bool> nodePropertyIsNonNull(uint64_t nodeId, std::string_view key) const override {
			requireActive();
			if (nodeId == 0U || key.empty()) throw std::invalid_argument("logical transaction overlay non-null Node probe requires an ID and key");
			const auto state = overlay_.nodes_.find(nodeId);
			if (state == overlay_.nodes_.end()) return overlay_.baseSnapshot().nodePropertyIsNonNull(nodeId, key);
			if (state->second.deleted) throw std::runtime_error("logical transaction overlay Node was deleted");
			const auto property = std::ranges::find(state->second.node.properties, key, &LogicalGraphPropertyEntry::key);
			return property != state->second.node.properties.end() && !std::holds_alternative<std::monostate>(property->value.getVariant());
		}

		[[nodiscard]] std::optional<bool> edgePropertyIsNonNull(uint64_t edgeId, std::string_view key) const override {
			requireActive();
			if (edgeId == 0U || key.empty()) throw std::invalid_argument("logical transaction overlay non-null Edge probe requires an ID and key");
			const auto state = overlay_.edges_.find(edgeId);
			if (state == overlay_.edges_.end()) {
				// Node mutations may hide an otherwise unchanged base Edge. Its
				// identity is sufficient for visibility; never load its Properties.
				if (!overlay_.nodes_.empty()) static_cast<void>(readEdgeShaped(edgeId, {}));
				return overlay_.baseSnapshot().edgePropertyIsNonNull(edgeId, key);
			}
			if (state->second.deleted || !overlay_.isEdgeVisible(state->second.edge, edgeId)) {
				throw std::runtime_error("logical transaction overlay Edge was deleted");
			}
			const auto property = std::ranges::find(state->second.edge.properties, key, &LogicalGraphPropertyEntry::key);
			return property != state->second.edge.properties.end() && !std::holds_alternative<std::monostate>(property->value.getVariant());
		}

		[[nodiscard]] std::vector<PropertyValue>
		readNodePropertiesSorted(std::span<const uint64_t> nodeIds, std::string_view key) const override {
			requireActive();
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
			requireActive();
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

		[[nodiscard]] bool supportsPropertyReadAdmission() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsPropertyReadAdmission();
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSorted(std::span<const uint64_t> nodeIds,
											 std::span<const std::string_view> keys) const override {
			return readNodePropertyColumnsSortedImpl(nodeIds, keys, nullptr);
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSortedWithAdmission(std::span<const uint64_t> nodeIds,
				std::span<const std::string_view> keys, const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("Property admission callback must not be empty");
			if (!supportsPropertyReadAdmission()) throw std::logic_error("overlay base reader does not support Property admission");
			const auto guarded = guardReadAdmission(admission);
			return readNodePropertyColumnsSortedImpl(nodeIds, keys, &guarded);
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSortedImpl(std::span<const uint64_t> nodeIds,
				std::span<const std::string_view> keys, const std::function<void(size_t)> *admission) const {
			requireActive();
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
			std::vector<std::vector<PropertyValue>> baseColumns = baseIds.empty()
					? std::vector<std::vector<PropertyValue>>(keys.size())
					: admission == nullptr ? overlay_.baseSnapshot().readNodePropertyColumnsSorted(baseIds, keys)
							: overlay_.baseSnapshot().readNodePropertyColumnsSortedWithAdmission(baseIds, keys, *admission);
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
					if (state == overlay_.nodes_.end()) {
						// The base reader has already admitted this owned payload.
						result[column].push_back(std::move(baseColumns[column][baseOrdinal]));
					} else {
						const auto property = std::ranges::find(state->second.node.properties,
								keys[column], &LogicalGraphPropertyEntry::key);
						if (property == state->second.node.properties.end()) {
							result[column].emplace_back();
						} else {
							admitStagedProperty(property->value, admission);
							result[column].push_back(copyStagedValue(property->value, admission));
						}
					}
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
			return readEdgePropertyColumnsSortedImpl(edgeIds, keys, nullptr);
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readEdgePropertyColumnsSortedWithAdmission(std::span<const uint64_t> edgeIds,
				std::span<const std::string_view> keys, const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("Property admission callback must not be empty");
			if (!supportsPropertyReadAdmission()) throw std::logic_error("overlay base reader does not support Property admission");
			const auto guarded = guardReadAdmission(admission);
			return readEdgePropertyColumnsSortedImpl(edgeIds, keys, &guarded);
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readEdgePropertyColumnsSortedImpl(std::span<const uint64_t> edgeIds,
				std::span<const std::string_view> keys, const std::function<void(size_t)> *admission) const {
			requireActive();
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
			std::vector<std::vector<PropertyValue>> baseColumns = baseIds.empty()
					? std::vector<std::vector<PropertyValue>>(keys.size())
					: admission == nullptr ? overlay_.baseSnapshot().readEdgePropertyColumnsSorted(baseIds, keys)
							: overlay_.baseSnapshot().readEdgePropertyColumnsSortedWithAdmission(baseIds, keys, *admission);
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
					if (state == overlay_.edges_.end()) {
						// The base reader has already admitted this owned payload.
						result[column].push_back(std::move(baseColumns[column][baseOrdinal]));
					} else {
						const auto property = std::ranges::find(state->second.edge.properties,
								keys[column], &LogicalGraphPropertyEntry::key);
						if (property == state->second.edge.properties.end()) {
							result[column].emplace_back();
						} else {
							admitStagedProperty(property->value, admission);
							result[column].push_back(copyStagedValue(property->value, admission));
						}
					}
				}
				if (state == overlay_.edges_.end()) {
					++baseOrdinal;
				}
			}
			return result;
		}

		[[nodiscard]] bool supportsNativeExactNodeCandidates() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsNativeExactNodeCandidates();
		}

		[[nodiscard]] std::optional<uint64_t> nativeSnapshotGeneration() const override {
			requireActive();
			const auto generation = overlay_.baseSnapshot().nativeSnapshotGeneration();
			if (!generation || *generation == 0U) return std::nullopt;
			if (overlay_.logical_revision_ > std::numeric_limits<uint64_t>::max() - *generation)
				throw std::overflow_error("overlay candidate generation overflowed");
			return *generation + overlay_.logical_revision_;
		}

		[[nodiscard]] std::optional<IndexCandidateBatch> findNodeCandidatesByExactProperties(
			const std::vector<std::string> &labels, const std::vector<std::pair<std::string, PropertyValue>> &properties,
			size_t maximumNodes, size_t maximumPropertyEntries) const override {
			return collectCandidateStream(true, [&](const auto &visitor) {
				return visitNodeCandidatesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries, visitor);
			});
		}

		template<typename Source>
		[[nodiscard]] std::optional<IndexCandidateBatch> collectCandidateStream(bool exact, const Source &source) const {
			IndexCandidateBatch result;
			std::map<uint64_t, std::vector<PropertyValue>> rows;
			const auto streamed = source([&](const IndexCandidateBatch &batch) {
					if (result.snapshot_generation == 0U) {
						result.snapshot_generation = batch.snapshot_generation;
						for (const auto &column: batch.verified_property_columns)
							result.verified_property_columns.push_back({column.key, {}});
					}
					for (size_t i = 0U; i < batch.candidates.size(); ++i) {
						std::vector<PropertyValue> values;
						values.reserve(batch.verified_property_columns.size());
						for (const auto &column: batch.verified_property_columns) values.push_back(column.values[i]);
						if (!rows.emplace(batch.candidates[i].entity_id, std::move(values)).second)
							throw std::runtime_error("overlay complete exact candidates contain duplicate IDs");
					}
					return true;
				});
			if (!streamed) return std::nullopt;
			if (!*streamed) throw std::runtime_error("overlay complete exact candidate stream is incomplete");
			result.snapshot_generation = *nativeSnapshotGeneration();
			result.exact_match_verified = exact;
			result.predicates_verified = result.label_scope_verified = true;
			result.candidates.reserve(rows.size());
			for (auto &column: result.verified_property_columns) column.values.reserve(rows.size());
			for (auto &[id, values]: rows) {
				result.candidates.push_back({id, false, 0U, 0U, 0U});
				for (size_t column = 0U; column < values.size(); ++column)
					result.verified_property_columns[column].values.push_back(std::move(values[column]));
			}
			return result;
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByExactProperties(
			const std::vector<std::string> &labels,
			const std::vector<std::pair<std::string, PropertyValue>> &properties,
			size_t maximumNodes, size_t maximumPropertyEntries,
			const IndexCandidateBatchVisitor &visitor) const override {
			return visitNodeCandidatesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries,
				std::numeric_limits<size_t>::max(), visitor);
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByExactProperties(
			const std::vector<std::string> &labels, const std::vector<std::pair<std::string, PropertyValue>> &properties,
			size_t maximumNodes, size_t maximumPropertyEntries, size_t maximumCandidateBytes,
			const IndexCandidateBatchVisitor &visitor) const override {
			requireActive();
			if (!supportsNativeExactNodeCandidates()) return std::nullopt;
			return visitMergedCandidates(labels, properties, nullptr, maximumNodes, maximumPropertyEntries,
				maximumCandidateBytes, visitor, [&](size_t bytes, const auto &callback) {
					return overlay_.baseSnapshot().visitNodeCandidatesByExactProperties(labels, properties,
						maximumNodes, maximumPropertyEntries, bytes, callback);
				});
		}

		template<typename Source>
		[[nodiscard]] std::optional<bool> visitMergedCandidates(
			const std::vector<std::string> &labels, const std::vector<std::pair<std::string, PropertyValue>> &properties,
			const LogicalGraphPropertyRange *range, size_t maximumNodes, size_t maximumPropertyEntries,
			size_t maximumCandidateBytes, const IndexCandidateBatchVisitor &visitor, const Source &source) const {
			requireActive();
			if (maximumNodes == 0U || maximumPropertyEntries == 0U || maximumCandidateBytes == 0U || !visitor)
				throw std::invalid_argument("overlay exact candidate stream requires positive bounds and a visitor");
			const auto generation = nativeSnapshotGeneration();
			if (!generation) throw std::runtime_error("overlay exact candidate stream has no pinned generation");
			const uint64_t revision = overlay_.logical_revision_;
			const auto check = [&] {
				requireActive();
				if (revision != overlay_.logical_revision_ || generation != nativeSnapshotGeneration())
					throw std::runtime_error("overlay changed during exact candidate delivery");
			};
			std::set<std::string> keys;
			for (const auto &[key, value]: properties) {
				static_cast<void>(value);
				if (key.empty() || !keys.insert(key).second) throw std::invalid_argument("overlay exact predicates require unique nonempty keys");
			}
			if (range && (range->key.empty() || !keys.insert(range->key).second))
				throw std::invalid_argument("overlay range predicate requires a distinct nonempty key");
			if (std::ranges::any_of(labels, [](const auto &label) { return label.empty(); }))
				throw std::invalid_argument("overlay exact labels must be nonempty");
			const size_t sourceLimit = maximumCandidateBytes == std::numeric_limits<size_t>::max()
				? maximumCandidateBytes : std::max<size_t>(1U, maximumCandidateBytes / 2U);
			const size_t capacity = std::min<size_t>(512U, std::min(maximumNodes,
				std::max<size_t>(1U, sourceLimit / 8192U)));
			IndexCandidateBatch output;
			size_t sourceBytes = 0U;
			size_t emitted = 0U;
			bool stopped = false;
			bool unverified = false;
			const auto admit = [&](size_t bytes) {
				check();
				if (sourceBytes > maximumCandidateBytes || bytes > maximumCandidateBytes - sourceBytes)
					throw std::length_error("logical graph candidate stream exceeded its byte bound");
			};
			const auto reset = [&] {
				output = {};
				output.snapshot_generation = *generation;
				output.exact_match_verified = range == nullptr;
				output.predicates_verified = output.label_scope_verified = true;
				// Admit output slots before allocation; only selected predicate fields are copied.
				size_t slots = detail::checkedLogicalGraphCandidateBytesMul(capacity, sizeof(EntityRef));
				slots = detail::checkedLogicalGraphCandidateBytesAdd(slots,
					detail::checkedLogicalGraphCandidateBytesMul(keys.size(), sizeof(IndexCandidateBatch::VerifiedPropertyColumn)));
				for (const auto &key: keys) {
					slots = detail::checkedLogicalGraphCandidateBytesAdd(slots, key.size());
					slots = detail::checkedLogicalGraphCandidateBytesAdd(slots,
						detail::checkedLogicalGraphCandidateBytesMul(capacity, sizeof(PropertyValue)));
				}
				admit(slots);
				output.candidates.reserve(capacity);
				output.verified_property_columns.reserve(keys.size());
				for (const auto &key: keys) {
					output.verified_property_columns.push_back({key, {}});
					output.verified_property_columns.back().values.reserve(capacity);
				}
			};
			const auto bytes = [&] {
				size_t result = detail::checkedLogicalGraphCandidateBytesMul(output.candidates.capacity(), sizeof(EntityRef));
				result = detail::checkedLogicalGraphCandidateBytesAdd(result,
					detail::checkedLogicalGraphCandidateBytesMul(output.verified_property_columns.capacity(), sizeof(IndexCandidateBatch::VerifiedPropertyColumn)));
				for (const auto &column: output.verified_property_columns) {
					result = detail::checkedLogicalGraphCandidateBytesAdd(result, column.key.capacity());
					result = detail::checkedLogicalGraphCandidateBytesAdd(result,
						detail::checkedLogicalGraphCandidateBytesMul(column.values.capacity(), sizeof(PropertyValue)));
					for (const auto &value: column.values)
						result = detail::checkedLogicalGraphCandidateBytesAdd(result, property_utils::getPropertyValueSize(value));
				}
				return result;
			};
			const auto flush = [&] {
				if (output.candidates.empty()) return true;
				admit(bytes());
				const bool more = visitor(output);
				check();
				output = {};
				stopped = !more;
				return more;
			};
			const auto append = [&](uint64_t id, const auto &valueAt) {
				if (emitted == maximumNodes) throw std::runtime_error("overlay exact candidate results exceeded the Node bound");
				if (output.snapshot_generation == 0U) reset();
				size_t payload = 0U;
				for (const auto &key: keys) payload = detail::checkedLogicalGraphCandidateBytesAdd(payload, property_utils::getPropertyValueSize(valueAt(key)));
				if (payload > maximumCandidateBytes - sourceBytes || bytes() > maximumCandidateBytes - sourceBytes - payload) {
					if (!flush()) return false;
					reset();
				}
				admit(detail::checkedLogicalGraphCandidateBytesAdd(bytes(), payload));
				for (auto &column: output.verified_property_columns) {
					column.values.push_back(graph::property_copy::copyPropertyValueGuarded(valueAt(column.key), check));
				}
				output.candidates.push_back({id, false, 0U, 0U, 0U});
				++emitted;
				admit(bytes());
				return output.candidates.size() != capacity || flush();
			};
			const auto &base = overlay_.baseSnapshot();
			size_t baseCount = 0U;
			const auto streamed = source(sourceLimit, [&](const IndexCandidateBatch &batch) {
					check();
					if (batch.snapshot_generation == 0U || base.nativeSnapshotGeneration() != batch.snapshot_generation)
						throw std::runtime_error("overlay base exact candidates have invalid generation");
					if ((range == nullptr && !batch.exact_match_verified) || !batch.predicates_verified || !batch.label_scope_verified) {
						if (emitted != 0U) throw std::runtime_error("overlay base exact candidate proof changed during delivery");
						unverified = true; return false;
					}
					if (batch.candidates.size() > maximumNodes - baseCount) throw std::runtime_error("overlay base exact candidates exceeded the Node bound");
					baseCount += batch.candidates.size();
					if (batch.candidates.empty()) return true;
					sourceBytes = detail::logicalGraphCandidateBatchBytes(batch);
					admit(0U);
					for (const auto &column: batch.verified_property_columns) {
						if (column.key.empty() || column.values.size() != batch.candidates.size())
							throw std::runtime_error("overlay base exact columns have invalid cardinality");
					}
					for (const auto &key: keys) {
						if (std::ranges::count(batch.verified_property_columns, key, &IndexCandidateBatch::VerifiedPropertyColumn::key) != 1)
							throw std::runtime_error("overlay base exact columns omit or duplicate a predicate");
					}
					uint64_t previous = 0U;
					for (size_t i = 0U; i < batch.candidates.size(); ++i) {
						const auto &reference = batch.candidates[i];
						if (reference.is_edge || reference.entity_id <= previous) throw std::runtime_error("overlay base exact candidate batch has invalid Node order");
						previous = reference.entity_id;
						if (overlay_.nodes_.contains(reference.entity_id)) continue;
						if (!append(reference.entity_id, [&](const auto &key) -> const PropertyValue & {
							return std::ranges::find(batch.verified_property_columns, key, &IndexCandidateBatch::VerifiedPropertyColumn::key)->values[i];
						})) return false;
					}
					return flush();
				});
			check();
			if (unverified) return std::nullopt;
			if (!streamed) {
				if (emitted != 0U) throw std::runtime_error("overlay base exact stream became unavailable after delivery");
				return std::nullopt;
			}
			if (stopped) return false;
			if (!*streamed) throw std::runtime_error("overlay base exact stream stopped before completion");
			sourceBytes = 0U;
			for (const auto &[id, state]: overlay_.nodes_) {
				check();
				if (state.deleted) continue;
				if (!std::ranges::all_of(labels, [&](const auto &label) { return std::ranges::find(state.node.labels, label) != state.node.labels.end(); })) continue;
				if (state.node.properties.size() > maximumPropertyEntries) throw std::runtime_error("overlay exact candidate exceeded the Property bound");
				if (!std::ranges::all_of(properties, [&](const auto &predicate) {
					const auto entry = std::ranges::find(state.node.properties, predicate.first, &LogicalGraphPropertyEntry::key);
					return entry != state.node.properties.end() && entry->value == predicate.second;
				})) continue;
				if (range) {
					const auto entry = std::ranges::find(state.node.properties, range->key, &LogicalGraphPropertyEntry::key);
					if (entry == state.node.properties.end() || entry->value.getType() == PropertyType::NULL_TYPE) continue;
					const auto &value = entry->value;
					if (range->min_value.getType() != PropertyType::NULL_TYPE &&
						(range->min_inclusive ? value < range->min_value : value <= range->min_value)) continue;
					if (range->max_value.getType() != PropertyType::NULL_TYPE &&
						(range->max_inclusive ? value > range->max_value : value >= range->max_value)) continue;
				}
				if (!append(id, [&](const auto &key) -> const PropertyValue & {
					return std::ranges::find(state.node.properties, key, &LogicalGraphPropertyEntry::key)->value;
				})) return false;
			}
			return flush();
		}

		[[nodiscard]] bool supportsNativeRangeNodeCandidates() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsNativeRangeNodeCandidates();
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByPropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
			size_t maximumNodes, size_t maximumPropertyEntries,
			const IndexCandidateBatchVisitor &visitor) const override {
			return visitNodeCandidatesByPropertyRange(labels, range, maximumNodes, maximumPropertyEntries, std::numeric_limits<size_t>::max(), visitor);
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByPropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
			size_t maximumNodes, size_t maximumPropertyEntries, size_t maximumCandidateBytes,
			const IndexCandidateBatchVisitor &visitor) const override {
			requireActive();
			if (!supportsNativeRangeNodeCandidates()) return std::nullopt;
			return visitMergedCandidates(labels, {}, &range, maximumNodes, maximumPropertyEntries,
				maximumCandidateBytes, visitor, [&](size_t bytes, const auto &callback) {
					return overlay_.baseSnapshot().visitNodeCandidatesByPropertyRange(labels, range, maximumNodes,
						maximumPropertyEntries, bytes, callback);
				});
		}

		[[nodiscard]] std::optional<IndexCandidateBatch> findNodeCandidatesByPropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
			size_t maximumNodes, size_t maximumPropertyEntries) const override {
			return collectCandidateStream(false, [&](const auto &visitor) {
				return visitNodeCandidatesByPropertyRange(labels, range, maximumNodes, maximumPropertyEntries, visitor);
			});
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByCompositePropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
			size_t maximumNodes, size_t maximumPropertyEntries,
			const IndexCandidateBatchVisitor &visitor) const override {
			return visitNodeCandidatesByCompositePropertyRange(labels, request, maximumNodes, maximumPropertyEntries, std::numeric_limits<size_t>::max(), visitor);
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByCompositePropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
			size_t maximumNodes, size_t maximumPropertyEntries, size_t maximumCandidateBytes,
			const IndexCandidateBatchVisitor &visitor) const override {
			requireActive();
			if (!supportsNativeRangeNodeCandidates()) return std::nullopt;
			return visitMergedCandidates(labels, request.equality_prefix, &request.range, maximumNodes, maximumPropertyEntries,
				maximumCandidateBytes, visitor, [&](size_t bytes, const auto &callback) {
					return overlay_.baseSnapshot().visitNodeCandidatesByCompositePropertyRange(labels, request, maximumNodes,
						maximumPropertyEntries, bytes, callback);
				});
		}

		[[nodiscard]] std::optional<IndexCandidateBatch> findNodeCandidatesByCompositePropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
			size_t maximumNodes, size_t maximumPropertyEntries) const override {
			return collectCandidateStream(false, [&](const auto &visitor) {
				return visitNodeCandidatesByCompositePropertyRange(labels, request, maximumNodes, maximumPropertyEntries, visitor);
			});
		}

		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
		findNodesByExactProperties(const std::vector<std::string> &labels,
											 const std::vector<std::pair<std::string, PropertyValue>> &properties,
											 size_t maximumNodes, size_t maximumPropertyEntries) const override {
			requireActive();
			if (maximumNodes == 0U || maximumPropertyEntries == 0U) {
				throw std::invalid_argument("logical transaction overlay exact Node lookup requires positive bounds");
			}
			std::optional<std::vector<LogicalGraphNode>> base;
			const auto &baseSnapshot = overlay_.baseSnapshot();
			if (baseSnapshot.supportsNativeExactNodeCandidates()) {
				std::vector<uint64_t> unchangedIds;
				size_t candidateCount = 0U;
				uint64_t previousId = 0U;
				bool verified = true;
				const auto streamed = baseSnapshot.visitNodeCandidatesByExactProperties(
						labels, properties, maximumNodes, maximumPropertyEntries,
						[&](const IndexCandidateBatch &batch) {
							if (batch.snapshot_generation == 0U ||
								baseSnapshot.nativeSnapshotGeneration() != batch.snapshot_generation) {
								throw std::runtime_error("overlay exact candidates do not belong to the pinned snapshot");
							}
							if (!batch.exact_match_verified || !batch.predicates_verified || !batch.label_scope_verified) {
								verified = false;
								return false;
							}
							if (batch.candidates.size() > maximumNodes - candidateCount) {
								throw std::runtime_error("overlay exact candidates exceeded the Node bound");
							}
							candidateCount += batch.candidates.size();
							for (const auto &reference: batch.candidates) {
								if (reference.is_edge || reference.entity_id == 0U || reference.entity_id <= previousId) {
									throw std::runtime_error("overlay exact candidates contain invalid or unordered Node references");
								}
								previousId = reference.entity_id;
								// Changed and deleted base rows are replaced by the final overlay state below.
								if (!overlay_.nodes_.contains(reference.entity_id)) unchangedIds.push_back(reference.entity_id);
							}
							return true;
							});
				if (streamed.has_value() && verified) {
					if (!*streamed) throw std::runtime_error("overlay exact candidate stream stopped before completion");
					base = unchangedIds.empty() ? std::vector<LogicalGraphNode>{}
							: baseSnapshot.readNodesShapedSorted(unchangedIds, completeNodeReadShape());
					if (base->size() != unchangedIds.size()) {
						throw std::runtime_error("overlay exact base batch returned an invalid cardinality");
					}
					for (size_t i = 0U; i < unchangedIds.size(); ++i) {
						if ((*base)[i].node_id != unchangedIds[i]) {
							throw std::runtime_error("overlay exact base batch returned an out-of-order record");
						}
					}
				}
			}
			if (!base.has_value()) {
				base = baseSnapshot.findNodesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries);
			}
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
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (maximumRecords == 0U || !visitor) {
				throw std::invalid_argument("logical transaction overlay Node visit requires a positive bound and visitor");
			}
			uint64_t after = 0U;
			size_t visited = 0U;
			while (true) {
				const LogicalGraphNodePage page = scanNodesAfter(after, std::min<size_t>(512U, maximumRecords - visited),
																std::numeric_limits<size_t>::max());
				for (const LogicalGraphNode &node: page.nodes) {
					if (!guarded(node)) {
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
			requireActive();
			return scanNodesAfter(afterNodeId, maximumRecords, maximumMetadataPages, completeNodeReadShape());
		}

		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
				size_t maximumMetadataPages, const EntityReadShape &shape) const override {
			requireActive();
			validateEntityReadShape(shape);
			if (maximumRecords == 0U || maximumMetadataPages == 0U) {
				throw std::invalid_argument("logical transaction overlay Node scan requires positive bounds");
			}
			LogicalGraphNodePage result;
			uint64_t baseAfter = afterNodeId;
			bool baseExhausted = false;
			size_t remainingMetadataPages = maximumMetadataPages;
			size_t metadataPagesVisited = 0U;
			const bool deferBaseShape = !overlay_.nodes_.empty() &&
				overlay_.baseSnapshot().supportsNativeNodeRefScans() &&
				(shape.include_labels || shape.all_properties || !shape.property_tokens.empty());
			while (result.nodes.size() < maximumRecords && !baseExhausted) {
				if (remainingMetadataPages == 0U) {
					throw std::runtime_error(
							"logical transaction overlay Node scan exceeded its metadata-page bound while filtering staged deletions");
				}
				// A base page may hide deleted records. It reports its exact metadata
				// cost, so repeated probes consume one shared total budget rather than
				// multiplying the caller's allowance.
				const LogicalGraphNodePage page = overlay_.baseSnapshot().scanNodesAfter(
						baseAfter, maximumRecords - result.nodes.size(), remainingMetadataPages,
						deferBaseShape ? EntityReadShape{} : shape);
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
				if (page.nodes.size() > maximumRecords - result.nodes.size()) {
					throw std::runtime_error("logical transaction overlay base snapshot exceeded its Node page bound");
				}
				uint64_t previousNodeId = baseAfter;
				for (const LogicalGraphNode &node: page.nodes) {
					if (node.node_id == 0U || node.node_id <= previousNodeId) {
						throw std::runtime_error("logical transaction overlay base snapshot returned non-increasing Node IDs");
					}
					previousNodeId = node.node_id;
				}
				if (deferBaseShape) {
					std::vector<uint64_t> visibleIds;
					visibleIds.reserve(page.nodes.size());
					for (const auto &node: page.nodes) {
						baseAfter = node.node_id;
						if (overlay_.isNodeVisible(node.node_id)) visibleIds.push_back(node.node_id);
					}
					// The shared shaped batch excludes replaced rows from its base read,
					// so deleted/replaced Property roots are never opened by this scan.
					if (!visibleIds.empty()) {
						auto visible = readNodesShapedSorted(visibleIds, shape);
						for (auto &node: visible) result.nodes.push_back(std::move(node));
					}
				} else {
					for (const LogicalGraphNode &node: page.nodes) {
						baseAfter = node.node_id;
						if (overlay_.isNodeVisible(node.node_id)) {
							// Reuse the shaped base record; only staged rows need replacement.
							if (overlay_.nodes_.contains(node.node_id)) {
								result.nodes.push_back(readNodeShaped(node.node_id, shape));
							} else {
								result.nodes.push_back(node);
							}
						}
					}
				}
				baseExhausted = !page.may_have_more;
				if (page.nodes.empty()) {
					break;
				}
			}
			if (result.nodes.size() == maximumRecords) {
				result.may_have_more = !baseExhausted || std::ranges::any_of(overlay_.nodes_, [&](const auto &entry) {
					return entry.second.created && !entry.second.deleted && entry.first > result.nodes.back().node_id;
				});
				result.metadata_pages_visited = metadataPagesVisited;
				return result;
			}
			if (!baseExhausted) {
				result.may_have_more = true;
				result.metadata_pages_visited = metadataPagesVisited;
				return result;
			}
			for (const auto &[nodeId, state]: overlay_.nodes_) {
				if (!state.created || state.deleted || nodeId <= afterNodeId) {
					continue;
				}
				if (result.nodes.size() == maximumRecords) {
					result.may_have_more = true;
					break;
				}
				result.nodes.push_back(readNodeShaped(nodeId, shape));
			}
			result.metadata_pages_visited = metadataPagesVisited;
			return result;
		}

		[[nodiscard]] LogicalGraphAdjacencyPage
		scanAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
							  std::optional<LogicalGraphAdjacencyContinuation> continuation,
							  size_t maximumRecords) const override {
			return scanAdjacentEdgesImpl(nodeId, direction, std::move(continuation), maximumRecords, nullptr);
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdgesImpl(uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			const std::function<void(size_t)> *admission, const std::function<void(size_t)> *bufferAdmission = nullptr) const {
			requireActive();
			if (nodeId == 0U || maximumRecords == 0U ||
					(direction != LogicalGraphReadDirection::LGRD_OUTGOING && direction != LogicalGraphReadDirection::LGRD_INCOMING)) {
				throw std::invalid_argument("logical transaction overlay adjacency scan requires valid bounds and direction");
			}
			if (overlay_.logical_revision_ != 0U && supportsNativeAdjacentEdgeTopologyPages() &&
				(bufferAdmission == nullptr || supportsAdjacencyTopologyBufferAdmission())) {
				// The merged topology page has already removed deleted and invisible Edges.
				auto topology = bufferAdmission != nullptr
					? scanAdjacentEdgeTopologiesWithBufferAdmission(nodeId, direction, continuation, maximumRecords, *bufferAdmission)
					: scanAdjacentEdgeTopologies(nodeId, direction, continuation, maximumRecords);
				const size_t topologyBytes = topologyStorageBytes(topology.edges.capacity(), topologyTypeBytes(topology.edges));
				const auto admitBuffers = [&](size_t bytes) { if (bufferAdmission) (*bufferAdmission)(bytes); };
				admitBuffers(adjacencyBufferAdd(topologyBytes, adjacencySlots(topology.edges.size(), sizeof(uint64_t))));
				std::vector<uint64_t> ids;
				ids.reserve(topology.edges.size());
				const size_t sourceBytes = adjacencyBufferAdd(topologyBytes, adjacencySlots(ids.capacity(), sizeof(uint64_t)));
				admitBuffers(sourceBytes);
				for (const auto &edge: topology.edges) ids.push_back(edge.edge_id);
				LogicalGraphAdjacencyPage result;
				result.continuation = topology.continuation;
				// Account for both the base batch and overlay result while their slots coexist.
				admitBuffers(adjacencyBufferAdd(sourceBytes,
					adjacencyBufferAdd(adjacencySlots(ids.size(), sizeof(LogicalGraphEdge)), adjacencySlots(ids.size(), sizeof(LogicalGraphEdge)))));
				result.edges = readEdgesShapedSortedImpl(ids, completeEdgeReadShape(), admission);
				for (size_t i = 0U; i < result.edges.size(); ++i) {
					const auto &edge = result.edges[i];
					const auto &expected = topology.edges[i];
					if (edge.edge_id != expected.edge_id || edge.source_node_id != expected.source_node_id ||
						edge.target_node_id != expected.target_node_id || edge.type != expected.type) {
						throw std::runtime_error("logical transaction overlay adjacency returned inconsistent topology");
					}
				}
				const size_t resultBytes = adjacencyBuffers(result.edges, adjacencyMetadata(result.edges));
				admitBuffers(adjacencyBufferAdd(sourceBytes, resultBytes));
				std::vector<uint64_t>().swap(ids);
				std::vector<LogicalGraphEdgeTopology>().swap(topology.edges);
				admitBuffers(resultBytes);
				return result;
			}
			refreshAdjacencyRevision();
			const auto continuationKey = [nodeId, direction](const LogicalGraphAdjacencyContinuation &value) {
				return std::tuple{nodeId, static_cast<uint8_t>(direction), value.previous_edge_id, value.next_edge_id};
			};
			std::optional<LogicalGraphAdjacencyContinuation> current = continuation;
			if (continuation.has_value()) {
				if (const auto found = continuations_.find(continuationKey(*continuation)); found != continuations_.end()) {
					if (found->second.created_after_edge.has_value()) {
						return createdEdgePage(nodeId, direction, *found->second.created_after_edge, maximumRecords,
																	 continuationKey, admission, bufferAdmission);
					}
					current = found->second.base_continuation;
				}
			}
			if (const auto node = overlay_.nodes_.find(nodeId);
				node != overlay_.nodes_.end() && node->second.created) {
				if (continuation.has_value()) {
					throw std::runtime_error("logical transaction overlay received an unknown created-Node adjacency continuation");
				}
				return createdEdgePage(nodeId, direction, 0U, maximumRecords, continuationKey, admission, bufferAdmission);
			}

			std::vector<LogicalGraphEdge> edges;
			size_t metadataBytes = 0U;
			while (edges.size() < maximumRecords) {
				const auto admitPage = [&](size_t bytes) {
					if (bufferAdmission) (*bufferAdmission)(adjacencyBufferAdd(adjacencyBuffers(edges, metadataBytes), bytes));
				};
				LogicalGraphAdjacencyPage page = bufferAdmission != nullptr
					? overlay_.baseSnapshot().scanAdjacentEdgesWithAdmission(nodeId, direction, current, maximumRecords - edges.size(), *admission, admitPage)
					: admission == nullptr
					? overlay_.baseSnapshot().scanAdjacentEdges(nodeId, direction, current, maximumRecords - edges.size())
					: overlay_.baseSnapshot().scanAdjacentEdgesWithPropertyAdmission(nodeId, direction, current,
						maximumRecords - edges.size(), *admission);
				validateBaseAdjacencyPage(page, nodeId, direction, current, maximumRecords - edges.size());
				size_t pageMetadataBytes = bufferAdmission ? adjacencyMetadata(page.edges) : 0U;
				for (LogicalGraphEdge &edge: page.edges) {
					if (overlay_.isEdgeVisible(edge, edge.edge_id)) {
						growAdmittedAdjacency(edges, maximumRecords, bufferAdmission, metadataBytes, bufferAdmission == nullptr ? 0U : adjacencyBuffers(page.edges, pageMetadataBytes));
						// The base adjacency page already contains a complete Edge. Only
						// staged Edge state needs an overlay lookup; re-reading every base
						// Edge would turn one adjacency page into N point reads.
						if (overlay_.edges_.contains(edge.edge_id)) {
							if (bufferAdmission) (*bufferAdmission)(adjacencyBufferAdd(adjacencyBufferAdd(adjacencyBuffers(edges, metadataBytes), adjacencyBuffers(page.edges, pageMetadataBytes)), adjacencyEdgeMetadata(overlay_.edges_.at(edge.edge_id).edge)));
							edges.push_back(readEdgeShapedImpl(edge.edge_id, {.include_type = true, .all_properties = true}, admission));
						} else {
							const size_t movedMetadata = bufferAdmission ? adjacencyEdgeMetadata(edge) : 0U;
							edges.push_back(std::move(edge));
							if (bufferAdmission) pageMetadataBytes = pageMetadataBytes - movedMetadata + adjacencyEdgeMetadata(edge);
						}
						if (bufferAdmission) {
							metadataBytes = adjacencyBufferAdd(metadataBytes, adjacencyEdgeMetadata(edges.back()));
							(*bufferAdmission)(adjacencyBufferAdd(adjacencyBuffers(edges, metadataBytes), adjacencyBuffers(page.edges, pageMetadataBytes)));
						}
					}
				}
				if (edges.size() == maximumRecords) {
					LogicalGraphAdjacencyPage result;
					result.edges = std::move(edges);
					if (page.continuation.has_value()) {
						result.continuation = {result.edges.back().edge_id, page.continuation->next_edge_id};
						rememberContinuation(continuationKey(*result.continuation),
																	  ContinuationState{page.continuation, std::nullopt});
						if (bufferAdmission) (*bufferAdmission)(adjacencyBuffers(result.edges, metadataBytes));
						return result;
					}
					// Base records have moved into the result; release consumed slots
					// before checking or appending transaction-created adjacency.
					std::vector<LogicalGraphEdge>().swap(page.edges);
					return appendCreatedEdges(nodeId, direction, std::move(result.edges), maximumRecords, continuationKey, admission, bufferAdmission);
				}
				if (!page.continuation.has_value()) {
					break;
				}
				current = page.continuation;
			}
			return appendCreatedEdges(nodeId, direction, std::move(edges), maximumRecords, continuationKey, admission, bufferAdmission);
		}

		[[nodiscard]] bool supportsAdjacencyReadAdmission() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsAdjacencyReadAdmission();
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdgesWithAdmission(
			uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			const std::function<void(size_t)> &propertyAdmission,
			const std::function<void(size_t)> &bufferAdmission) const override {
			if (!supportsAdjacencyReadAdmission()) throw std::logic_error("overlay combined adjacency admission is unavailable");
			if (!propertyAdmission || !bufferAdmission) throw std::invalid_argument("adjacency admission callbacks must not be empty");
			const auto guardedProperty = guardReadAdmission(propertyAdmission);
			const auto guardedBuffer = guardReadAdmission(bufferAdmission);
			if (overlay_.logical_revision_ == 0U) return overlay_.baseSnapshot().scanAdjacentEdgesWithAdmission(nodeId, direction,
				std::move(continuation), maximumRecords, guardedProperty, guardedBuffer);
			return scanAdjacentEdgesImpl(nodeId, direction, std::move(continuation), maximumRecords, &guardedProperty, &guardedBuffer);
		}

		[[nodiscard]] bool supportsAdjacencyPropertyReadAdmission() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsAdjacencyPropertyReadAdmission();
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdgesWithPropertyAdmission(
			uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			const std::function<void(size_t)> &admission) const override {
			if (!supportsAdjacencyPropertyReadAdmission()) throw std::logic_error("overlay adjacency Property admission is unavailable");
			if (!admission) throw std::invalid_argument("adjacency Property admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			return scanAdjacentEdgesImpl(nodeId, direction, std::move(continuation), maximumRecords, &guarded);
		}

		[[nodiscard]] bool supportsNativeAdjacentEdgeTopologyPages() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsNativeAdjacentEdgeTopologyPages();
		}

		[[nodiscard]] bool supportsNativeAdjacentEdgeRefScans() const override {
			requireActive();
			return supportsNativeAdjacentEdgeTopologyPages();
		}

		[[nodiscard]] bool supportsAdjacencyReferenceBufferAdmission() const override {
			requireActive();
			return supportsAdjacencyTopologyBufferAdmission();
		}

		[[nodiscard]] LogicalGraphAdjacencyReferencePage scanAdjacentEdgeRefsWithBufferAdmission(uint64_t nodeId,
			LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
			size_t maximumRecords, const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!supportsAdjacencyReferenceBufferAdmission()) throw std::logic_error("overlay reference admission is unavailable");
			if (!admission) throw std::invalid_argument("overlay reference admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U && overlay_.baseSnapshot().supportsAdjacencyReferenceBufferAdmission())
				return overlay_.baseSnapshot().scanAdjacentEdgeRefsWithBufferAdmission(nodeId, direction,
					std::move(continuation), maximumRecords, guarded);
			return scanAdjacentEdgeRefsImpl(nodeId, direction, continuation, maximumRecords, guarded);
		}

		[[nodiscard]] LogicalGraphAdjacencyReferencePage scanAdjacentEdgeRefs(uint64_t nodeId,
			LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
			size_t maximumRecords) const override {
			return scanAdjacentEdgeRefsImpl(nodeId, direction, continuation, maximumRecords, {});
		}

		[[nodiscard]] LogicalGraphAdjacencyReferencePage scanAdjacentEdgeRefsImpl(uint64_t nodeId,
			LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
			size_t maximumRecords, const std::function<void(size_t)> &admission) const {
			requireActive();
			auto page = admission ? scanAdjacentEdgeTopologiesWithBufferAdmission(nodeId, direction, continuation,
				maximumRecords, admission) : scanAdjacentEdgeTopologies(nodeId, direction, continuation, maximumRecords);
			const size_t sourceBytes = topologyStorageBytes(page.edges.capacity(), topologyTypeBytes(page.edges));
			const auto arrayBytes = [](size_t count, size_t width) {
				if (count > std::numeric_limits<size_t>::max() / width)
					throw std::overflow_error("overlay reference buffer size overflow");
				return count * width;
			};
			const auto admit = [&](size_t bytes) { if (admission) admission(bytes); };
			LogicalGraphAdjacencyReferencePage result;
			result.continuation = page.continuation;
			admit(checkedTopologySum(sourceBytes, arrayBytes(page.edges.size(), sizeof(EntityRef))));
			result.edges.resize(page.edges.size());
			const size_t retainedBytes = arrayBytes(result.edges.capacity(), sizeof(EntityRef));
			const size_t conversionBytes = checkedTopologySum(sourceBytes, retainedBytes);
			admit(checkedTopologySum(conversionBytes, arrayBytes(page.edges.size(), sizeof(size_t))));
			std::vector<size_t> order(page.edges.size());
			const size_t workingBytes = checkedTopologySum(conversionBytes, arrayBytes(order.capacity(), sizeof(size_t)));
			admit(workingBytes);
			// Re-admission preserves the same live-byte charge while allowing the
			// caller's cancellation guard to interrupt CPU-only preparation.
			const auto checkpoint = [&](size_t &work) {
				if (++work == 256U) { work = 0U; admit(workingBytes); }
			};
			size_t work = 0U;
			for (size_t index = 0U; index < order.size(); ++index) {
				checkpoint(work);
				order[index] = index;
			}
			admit(workingBytes);
			work = 0U;
			std::sort(order.begin(), order.end(), [&](size_t left, size_t right) {
				checkpoint(work);
				return page.edges[left].type < page.edges[right].type;
			});
			admit(workingBytes);
			work = 0U;
			std::optional<std::string_view> cachedType;
			uint64_t cachedToken = 0U;
			for (const size_t index: order) {
				checkpoint(work);
				const auto &edge = page.edges[index];
				uint64_t typeToken = 0U;
				if (const auto staged = overlay_.edges_.find(edge.edge_id);
					staged != overlay_.edges_.end() && staged->second.created) {
					typeToken = staged->second.relationship_type_token;
				} else if (overlay_.baseSnapshot().supportsNativeRelationshipTypeTokens()) {
					if (!cachedType || *cachedType != edge.type) {
						cachedToken = overlay_.baseSnapshot().findRelationshipTypeToken(edge.type).value_or(0U);
						cachedType = edge.type;
					}
					typeToken = cachedToken;
				}
				result.edges[index] = {edge.edge_id, true, edge.source_node_id, edge.target_node_id, typeToken};
			}
			admit(workingBytes);
			std::vector<size_t>().swap(order);
			std::vector<LogicalGraphEdgeTopology>().swap(page.edges);
			admit(retainedBytes);
			return result;
		}

		[[nodiscard]] bool supportsAdjacencyTopologyBufferAdmission() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsAdjacencyTopologyBufferAdmission();
		}
		[[nodiscard]] LogicalGraphAdjacencyTopologyPage
		scanAdjacentEdgeTopologiesWithBufferAdmission(uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!supportsAdjacencyTopologyBufferAdmission()) throw std::logic_error("overlay topology buffer admission is unavailable");
			if (!admission) throw std::invalid_argument("overlay topology admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U)
				return overlay_.baseSnapshot().scanAdjacentEdgeTopologiesWithBufferAdmission(
					nodeId, direction, std::move(continuation), maximumRecords, guarded);
			return scanAdjacentEdgeTopologiesImpl(nodeId, direction, std::move(continuation), maximumRecords, guarded);
		}

		[[nodiscard]] LogicalGraphAdjacencyTopologyPage scanAdjacentEdgeTopologies(uint64_t nodeId,
				LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
				size_t maximumRecords) const override {
			return scanAdjacentEdgeTopologiesImpl(nodeId, direction, std::move(continuation), maximumRecords, {});
		}

		[[nodiscard]] LogicalGraphAdjacencyTopologyPage scanAdjacentEdgeTopologiesImpl(uint64_t nodeId,
			LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
			size_t maximumRecords, const std::function<void(size_t)> &admission) const {
			requireActive();
			if (maximumRecords == 0U || nodeId == 0U ||
					(direction != LogicalGraphReadDirection::LGRD_OUTGOING && direction != LogicalGraphReadDirection::LGRD_INCOMING)) {
				throw std::invalid_argument("logical transaction overlay topology scan requires valid bounds and direction");
			}
			refreshAdjacencyRevision();
			const auto key = [nodeId, direction](const LogicalGraphAdjacencyContinuation &value) {
				return std::tuple{nodeId, static_cast<uint8_t>(direction), value.previous_edge_id, value.next_edge_id};
			};
			LogicalGraphAdjacencyTopologyPage result;
			size_t resultTypeBytes = 0U;
			std::optional<LogicalGraphAdjacencyContinuation> rawContinuation;
			const auto saved = continuation.has_value() ? topology_continuations_.find(key(*continuation))
					: topology_continuations_.end();
			// Cache the position before the probe, rather than an owned topology
			// row. Resumption uses the same bounded source and visibility checks;
			// relationship type strings do not accumulate in snapshot state.
			const auto resumed = saved != topology_continuations_.end()
				? saved->second.before_pending : continuation;
			result = scanAdjacentEdgeTopologiesRaw(nodeId, direction, resumed, maximumRecords, admission);
			resultTypeBytes = topologyTypeBytes(result.edges);
			rawContinuation = result.continuation;
			admitTopologyStorage(admission, result.edges, resultTypeBytes);
			result.continuation.reset();
			if (rawContinuation.has_value()) {
				const size_t retained = topologyStorageBytes(result.edges.capacity(), resultTypeBytes);
				auto next = scanAdjacentEdgeTopologiesRaw(nodeId, direction, rawContinuation, 1U,
					admission ? std::function<void(size_t)>([&](size_t bytes) { admission(checkedTopologySum(retained, bytes)); }) : std::function<void(size_t)>{});
				if (!next.edges.empty()) {
					result.continuation = {result.edges.back().edge_id, next.edges.front().edge_id};
					// Only deleted gaps need a translation. Ordinary page boundaries
					// already carry the exact base position and must not accumulate.
					if (result.continuation != rawContinuation) {
						requireContinuationCapacity(topology_continuations_, key(*result.continuation));
						const auto [entry, inserted] = topology_continuations_.insert_or_assign(
							key(*result.continuation), TopologyContinuationState{rawContinuation});
						static_cast<void>(entry);
						if (inserted) debug::PerfTrace::addValue("native_graph.read.overlay_topology_continuation_retained", 1);
					}
				}
			}
			admitTopologyStorage(admission, result.edges, resultTypeBytes);
			return result;
		}

		[[nodiscard]] LogicalGraphAdjacencyTopologyPage
		scanAdjacentEdgeTopologiesRaw(uint64_t nodeId, LogicalGraphReadDirection direction,
											std::optional<LogicalGraphAdjacencyContinuation> continuation,
											size_t maximumRecords, const std::function<void(size_t)> &admission = {}) const {
			if (nodeId == 0U || maximumRecords == 0U) {
				throw std::invalid_argument("logical transaction overlay topology scan requires positive bounds");
			}
			if (direction != LogicalGraphReadDirection::LGRD_OUTGOING &&
					direction != LogicalGraphReadDirection::LGRD_INCOMING) {
				throw std::invalid_argument("logical transaction overlay topology scan requires a valid direction");
			}
			const auto continuationKey = [nodeId, direction](const LogicalGraphAdjacencyContinuation &value) {
				return std::tuple{nodeId, static_cast<uint8_t>(direction), value.previous_edge_id, value.next_edge_id};
			};
		std::optional<LogicalGraphAdjacencyContinuation> current = continuation;
		if (continuation.has_value()) {
			if (const auto found = continuations_.find(continuationKey(*continuation)); found != continuations_.end()) {
				if (found->second.created_after_edge.has_value()) {
					return createdTopologyPage(nodeId, direction, *found->second.created_after_edge, maximumRecords,
													 continuationKey, admission);
				}
				current = found->second.base_continuation;
			}
		}
		if (const auto node = overlay_.nodes_.find(nodeId);
				 node != overlay_.nodes_.end() && node->second.created) {
			if (continuation.has_value()) {
				throw std::runtime_error("logical transaction overlay received an unknown created-Node topology continuation");
			}
			return createdTopologyPage(nodeId, direction, 0U, maximumRecords, continuationKey, admission);
		}

		std::vector<LogicalGraphEdgeTopology> edges;
		size_t typeBytes = 0U;
		while (edges.size() < maximumRecords) {
			size_t pageBytes = 0U;
			const auto sourceAdmission = [&](size_t bytes) {
				pageBytes = bytes;
				admitTopologyStorage(admission, edges, typeBytes, bytes);
			};
			LogicalGraphAdjacencyTopologyPage page = admission
				? overlay_.baseSnapshot().scanAdjacentEdgeTopologiesWithBufferAdmission(
					nodeId, direction, current, maximumRecords - edges.size(), sourceAdmission)
				: overlay_.baseSnapshot().scanAdjacentEdgeTopologies(
					nodeId, direction, current, maximumRecords - edges.size());
			pageBytes = topologyStorageBytes(page.edges.capacity(), topologyTypeBytes(page.edges));
			admitTopologyStorage(admission, edges, typeBytes, pageBytes);
			validateBaseAdjacencyPage(page, nodeId, direction, current, maximumRecords - edges.size());
			for (LogicalGraphEdgeTopology &edge: page.edges) {
				if (overlay_.isEdgeVisible(
						{edge.edge_id, edge.source_node_id, edge.target_node_id, {}}, edge.edge_id)) {
					reserveTopologySlot(edges, maximumRecords, typeBytes, admission, pageBytes);
					const size_t movedTypeBytes = edge.type.capacity();
					edges.push_back(std::move(edge));
					typeBytes = checkedTopologySum(typeBytes, movedTypeBytes);
					pageBytes = checkedTopologySum(pageBytes - movedTypeBytes, edge.type.capacity());
					admitTopologyStorage(admission, edges, typeBytes, pageBytes);
				}
			}
			std::vector<LogicalGraphEdgeTopology>().swap(page.edges);
			admitTopologyStorage(admission, edges, typeBytes);
			if (edges.size() == maximumRecords) {
				LogicalGraphAdjacencyTopologyPage result;
				result.edges = std::move(edges);
				if (page.continuation.has_value()) {
					result.continuation = {result.edges.back().edge_id, page.continuation->next_edge_id};
					rememberContinuation(continuationKey(*result.continuation),
																		ContinuationState{page.continuation, std::nullopt});
					return result;
				}
				return appendCreatedTopologies(nodeId, direction, 0U, std::move(result.edges), maximumRecords, continuationKey, admission);
			}
			if (!page.continuation.has_value()) {
				break;
			}
			current = page.continuation;
		}
			return appendCreatedTopologies(nodeId, direction, 0U, std::move(edges), maximumRecords, continuationKey, admission);
		}

		void visitEdges(size_t maximumRecords, const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (maximumRecords == 0U || !visitor) {
				throw std::invalid_argument("logical transaction overlay Edge visit requires a positive bound and visitor");
			}
			if (overlay_.logical_revision_ != 0U && supportsNativeEdgeRefScans()) {
				// Keep hydration inside the callback so an early stop never reads later Edge properties.
				visitEdgeRefs(maximumRecords, [&](const EntityRef &reference) {
					const LogicalGraphEdge edge = readEdgeShaped(reference.entity_id, completeEdgeReadShape());
					if (edge.edge_id != reference.entity_id || edge.source_node_id != reference.source_node_id ||
						edge.target_node_id != reference.target_node_id) {
						throw std::runtime_error("logical transaction overlay Edge visit returned inconsistent topology");
					}
					return guarded(edge);
				});
				return;
			}
			size_t visited = 0U;
			bool stopped = false;
			overlay_.baseSnapshot().visitEdges(maximumRecords, [&](const LogicalGraphEdge &edge) {
				if (stopped) {
					return false;
				}
				if (!overlay_.isEdgeVisible(edge, edge.edge_id)) {
					return true;
				}
				++visited;
				if (visited > maximumRecords) {
					throw std::runtime_error("logical transaction overlay Edge visit exceeded its bound");
				}
				stopped = !guarded(overlay_.edges_.contains(edge.edge_id) ? readEdge(edge.edge_id) : edge);
				return !stopped;
			});
			if (stopped) {
				return;
			}
			for (const auto &[edgeId, state]: overlay_.edges_) {
				if (!state.created || !overlay_.isEdgeVisible(state.edge, edgeId)) {
					continue;
				}
				if (visited == maximumRecords) {
					throw std::runtime_error("logical transaction overlay Edge visit exceeded its bound");
				}
				++visited;
				if (!guarded(state.edge)) {
					return;
				}
			}
		}

		void visitAdjacentEdgeRefsForNodes(std::span<const uint64_t> nodeIds,
			LogicalGraphReadDirection direction, size_t maximumEdges,
			const std::function<bool(const EntityRef &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) {
				overlay_.baseSnapshot().visitAdjacentEdgeRefsForNodes(nodeIds, direction, maximumEdges, guarded);
				return;
			}
			LogicalGraphReadSnapshot::visitAdjacentEdgeRefsForNodes(nodeIds, direction, maximumEdges, guarded);
		}

		void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
										const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (maximumEdges == 0U || !visitor) {
				throw std::invalid_argument("logical transaction overlay adjacency visit requires a positive bound and visitor");
			}
			std::optional<LogicalGraphAdjacencyContinuation> continuation;
			size_t visited = 0U;
			while (true) {
				const LogicalGraphAdjacencyPage page = scanAdjacentEdges(
						nodeId, direction, continuation, std::min<size_t>(512U, maximumEdges - visited));
				for (const LogicalGraphEdge &edge: page.edges) {
					if (!guarded(edge)) {
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
			requireActive();
			return overlay_.overlayFence();
		}

	private:

		void requireActive() const {
			if (overlay_.transaction_.state() != LogicalGraphTransactionState::LGTTS_ACTIVE) {
				throw std::logic_error("logical transaction overlay snapshot is no longer active");
			}
		}

		// Validate source rows before visibility filtering can hide malformed output.
		template<typename Page>
		static void validateBaseAdjacencyPage(const Page &page, uint64_t nodeId,
				LogicalGraphReadDirection direction, const std::optional<LogicalGraphAdjacencyContinuation> &current,
				size_t maximumRecords) {
			if (page.edges.empty() && page.continuation.has_value()) {
				throw std::runtime_error("logical transaction overlay base snapshot returned an empty nonterminal adjacency page");
			}
			if (page.edges.size() > maximumRecords) {
				throw std::runtime_error("logical transaction overlay base snapshot exceeded its adjacency page bound");
			}
			uint64_t previousEdgeId = current.has_value() ? current->previous_edge_id : 0U;
			if (current.has_value() && !page.edges.empty() && page.edges.front().edge_id != current->next_edge_id) {
				throw std::runtime_error("logical transaction overlay base adjacency page did not resume at its continuation");
			}
			for (const auto &edge: page.edges) {
				if (edge.edge_id == 0U || edge.edge_id <= previousEdgeId || edge.source_node_id == 0U ||
						edge.target_node_id == 0U ||
						(direction == LogicalGraphReadDirection::LGRD_OUTGOING ? edge.source_node_id : edge.target_node_id) != nodeId) {
					throw std::runtime_error("logical transaction overlay base snapshot returned an invalid adjacency record");
				}
				previousEdgeId = edge.edge_id;
			}
			if (page.continuation.has_value() &&
					(page.continuation->previous_edge_id != page.edges.back().edge_id ||
					 page.continuation->next_edge_id <= page.continuation->previous_edge_id)) {
				throw std::runtime_error("logical transaction overlay base snapshot returned an invalid adjacency continuation");
			}
		}

		void refreshAdjacencyRevision() const {
			static_cast<void>(overlay_.baseSnapshot());
			if (adjacency_revision_ != overlay_.logical_revision_) {
				topology_continuations_.clear();
				continuations_.clear();
				adjacency_revision_ = overlay_.logical_revision_;
			}
		}

		using ContinuationKey = std::tuple<uint64_t, uint8_t, uint64_t, uint64_t>;

		struct ContinuationState {
			std::optional<LogicalGraphAdjacencyContinuation> base_continuation;
			std::optional<uint64_t> created_after_edge;
		};

		template<typename State>
		void requireContinuationCapacity(const std::map<ContinuationKey, State> &cache,
				const ContinuationKey &key) const {
			// Only staged gaps and created boundaries need translations. Each
			// staged Edge can affect at most its outgoing and incoming positions.
			// Keep replay state bounded by transaction state, not total base pages;
			// division avoids overflowing a doubled mutation count.
			if (!cache.contains(key) && cache.size() / 2U >= overlay_.edges_.size()) {
				throw std::runtime_error("logical transaction overlay continuation state exceeds staged Edge bounds");
			}
		}

		void rememberContinuation(const ContinuationKey &key, const ContinuationState &state) const {
			const LogicalGraphAdjacencyContinuation publicPosition{std::get<2>(key), std::get<3>(key)};
			if (!state.created_after_edge && state.base_continuation == publicPosition) return;
			requireContinuationCapacity(continuations_, key);
			const auto [entry, inserted] = continuations_.insert_or_assign(key, state);
			static_cast<void>(entry);
			if (inserted) debug::PerfTrace::addValue("native_graph.read.overlay_continuation_retained", 1);
		}

		template<typename MakeKey>
		[[nodiscard]] LogicalGraphAdjacencyPage appendCreatedEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
																				std::vector<LogicalGraphEdge> edges, size_t maximumRecords,
																				const MakeKey &makeKey, const std::function<void(size_t)> *admission = nullptr, const std::function<void(size_t)> *bufferAdmission = nullptr) const {
			return appendCreatedEdgesFrom(nodeId, direction, 0U, std::move(edges), maximumRecords, makeKey, admission, bufferAdmission);
		}

		template<typename MakeKey>
		[[nodiscard]] LogicalGraphAdjacencyPage createdEdgePage(uint64_t nodeId, LogicalGraphReadDirection direction,
																			  uint64_t afterEdgeId, size_t maximumRecords, const MakeKey &makeKey, const std::function<void(size_t)> *admission = nullptr, const std::function<void(size_t)> *bufferAdmission = nullptr) const {
			return appendCreatedEdgesFrom(nodeId, direction, afterEdgeId, {}, maximumRecords, makeKey, admission, bufferAdmission);
		}

		template<typename MakeKey>
		[[nodiscard]] LogicalGraphAdjacencyTopologyPage createdTopologyPage(uint64_t nodeId,
															LogicalGraphReadDirection direction, uint64_t afterEdgeId,
															size_t maximumRecords, const MakeKey &makeKey,
			const std::function<void(size_t)> &admission = {}) const {
			return appendCreatedTopologies(nodeId, direction, afterEdgeId, {}, maximumRecords, makeKey, admission);
		}

		template<typename MakeKey>
		[[nodiscard]] LogicalGraphAdjacencyTopologyPage appendCreatedTopologies(uint64_t nodeId,
																LogicalGraphReadDirection direction, uint64_t afterEdgeId,
																std::vector<LogicalGraphEdgeTopology> edges,
																size_t maximumRecords, const MakeKey &makeKey,
			const std::function<void(size_t)> &admission = {}) const {
			LogicalGraphAdjacencyTopologyPage result;
			result.edges = std::move(edges);
			size_t typeBytes = topologyTypeBytes(result.edges);
			for (auto current = overlay_.edges_.upper_bound(afterEdgeId); current != overlay_.edges_.end(); ++current) {
				debug::PerfTrace::addValue("native_graph.read.overlay_created_adjacency_candidates", 1);
				const auto &[edgeId, state] = *current;
				const bool endpointMatches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
						? state.edge.source_node_id == nodeId : state.edge.target_node_id == nodeId;
				if (!state.created || !endpointMatches || !overlay_.isEdgeVisible(state.edge, edgeId)) continue;
				if (result.edges.size() == maximumRecords) {
					result.continuation = {result.edges.back().edge_id, edgeId};
					rememberContinuation(makeKey(*result.continuation),
							ContinuationState{std::nullopt, result.edges.back().edge_id});
					break;
				}
				const size_t copyBytes = std::max(state.edge.type.size(), std::string{}.capacity());
				reserveTopologySlot(result.edges, maximumRecords, typeBytes, admission, copyBytes);
				result.edges.push_back({edgeId, state.edge.source_node_id, state.edge.target_node_id, state.edge.type});
				typeBytes = checkedTopologySum(typeBytes, result.edges.back().type.capacity());
				admitTopologyStorage(admission, result.edges, typeBytes);
			}
			admitTopologyStorage(admission, result.edges, typeBytes);
			return result;
		}

		template<typename MakeKey>
		[[nodiscard]] LogicalGraphAdjacencyPage appendCreatedEdgesFrom(uint64_t nodeId,
																				 LogicalGraphReadDirection direction, uint64_t afterEdgeId,
																				 std::vector<LogicalGraphEdge> edges, size_t maximumRecords,
																				 const MakeKey &makeKey, const std::function<void(size_t)> *admission = nullptr, const std::function<void(size_t)> *bufferAdmission = nullptr) const {
			LogicalGraphAdjacencyPage result;
			result.edges = std::move(edges);
			size_t metadataBytes = bufferAdmission ? adjacencyMetadata(result.edges) : 0U;
			for (auto current = overlay_.edges_.upper_bound(afterEdgeId); current != overlay_.edges_.end(); ++current) {
				debug::PerfTrace::addValue("native_graph.read.overlay_created_adjacency_candidates", 1);
				const auto &[edgeId, state] = *current;
				const bool endpointMatches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
						? state.edge.source_node_id == nodeId : state.edge.target_node_id == nodeId;
				if (!state.created || !endpointMatches || !overlay_.isEdgeVisible(state.edge, edgeId)) continue;
				if (result.edges.size() == maximumRecords) {
					result.continuation = {result.edges.back().edge_id, edgeId};
					rememberContinuation(makeKey(*result.continuation),
							ContinuationState{std::nullopt, result.edges.back().edge_id});
					break;
				}
				growAdmittedAdjacency(result.edges, maximumRecords, bufferAdmission, metadataBytes);
				if (bufferAdmission) (*bufferAdmission)(adjacencyBufferAdd(adjacencyBuffers(result.edges, metadataBytes), adjacencyEdgeMetadata(state.edge)));
				for (const auto &entry : state.edge.properties) admitStagedProperty(entry.value, admission);
				LogicalGraphEdge copied;
				copied.edge_id = state.edge.edge_id;
				copied.source_node_id = state.edge.source_node_id;
				copied.target_node_id = state.edge.target_node_id;
				auto checkpoint = [&] { if (admission != nullptr) (*admission)(0U); };
				copied.type = graph::property_copy::copyPropertyStringGuarded(state.edge.type, checkpoint);
				copied.properties = copyStagedProperties(state.edge.properties, admission);
				result.edges.push_back(std::move(copied));
				if (bufferAdmission) metadataBytes = adjacencyBufferAdd(metadataBytes, adjacencyEdgeMetadata(result.edges.back()));
				if (bufferAdmission) (*bufferAdmission)(adjacencyBuffers(result.edges, metadataBytes));
			}
			if (bufferAdmission) (*bufferAdmission)(adjacencyBuffers(result.edges, metadataBytes));
			return result;
		}

		const LogicalGraphTransactionOverlay &overlay_;
		mutable std::map<ContinuationKey, ContinuationState> continuations_;
		struct TopologyContinuationState {
			std::optional<LogicalGraphAdjacencyContinuation> before_pending;
		};
		mutable std::map<ContinuationKey, TopologyContinuationState> topology_continuations_;
		mutable uint64_t adjacency_revision_ = 0U;
	};

	class LogicalGraphTransactionOverlay::VectorSnapshot final : public LogicalGraphVectorSearchSnapshot {
		[[nodiscard]] auto readRevisionCheckpoint() const {
			const uint64_t revision = overlay_.logical_revision_;
			return [this, revision] {
				requireActive();
				if (overlay_.logical_revision_ != revision)
					throw std::runtime_error("logical transaction overlay changed during read callback");
			};
		}

		[[nodiscard]] std::function<void(size_t)> guardReadAdmission(const std::function<void(size_t)> &admission) const {
			return [check = readRevisionCheckpoint(), &admission](size_t bytes) {
				check();
				admission(bytes);
				check();
			};
		}

		template<typename Visitor>
		[[nodiscard]] auto guardReadVisitor(const Visitor &visitor) const {
			if (!visitor) throw std::invalid_argument("overlay read visitor must not be empty");
			return [check = readRevisionCheckpoint(), &visitor](const auto &...args) {
				check();
				const bool keepGoing = visitor(args...);
				check();
				return keepGoing;
			};
		}

	public:
		explicit VectorSnapshot(const LogicalGraphTransactionOverlay &overlay,
									 LogicalGraphVectorSearchSnapshot &derivedSnapshot) :
				overlay_(overlay), derived_snapshot_(derivedSnapshot) {}

		[[nodiscard]] bool supportsNativeNodeRefScans() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsNativeNodeRefScans()
				: overlaySnapshot().supportsNativeNodeRefScans();
		}

		void visitNodeRefs(size_t maximumRecords, const std::function<bool(const EntityRef &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) derived_snapshot_.visitNodeRefs(maximumRecords, guarded);
			else {
				auto pinned = pinnedOverlaySnapshot();
				pinned->visitNodeRefs(maximumRecords, guarded);
			}
		}

		void visitNodeRefsWithLabelMatches(size_t maximumRecords, const std::vector<std::string> &labels,
			const std::function<bool(const EntityRef &, bool)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) derived_snapshot_.visitNodeRefsWithLabelMatches(maximumRecords, labels, guarded);
			else {
				auto pinned = pinnedOverlaySnapshot();
				pinned->visitNodeRefsWithLabelMatches(maximumRecords, labels, guarded);
			}
		}

		[[nodiscard]] bool supportsNativeEdgeRefScans() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsNativeEdgeRefScans()
				: overlaySnapshot().supportsNativeEdgeRefScans();
		}

		void visitEdgeRefs(size_t maximumRecords, const std::function<bool(const EntityRef &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) derived_snapshot_.visitEdgeRefs(maximumRecords, guarded);
			else {
				auto pinned = pinnedOverlaySnapshot();
				pinned->visitEdgeRefs(maximumRecords, guarded);
			}
		}

		[[nodiscard]] bool supportsNativeRelationshipTypeTokens() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsNativeRelationshipTypeTokens()
				: overlaySnapshot().supportsNativeRelationshipTypeTokens();
		}
		[[nodiscard]] bool supportsNativeRelationshipTypeTokens(std::string_view type) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U
				? derived_snapshot_.supportsNativeRelationshipTypeTokens(type)
				: overlaySnapshot().supportsNativeRelationshipTypeTokens(type);
		}

		[[nodiscard]] std::optional<uint64_t> findRelationshipTypeToken(std::string_view name) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.findRelationshipTypeToken(name)
				: overlaySnapshot().findRelationshipTypeToken(name);
		}

		[[nodiscard]] bool supportsPropertyNonNullProbes() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsPropertyNonNullProbes()
				: overlaySnapshot().supportsPropertyNonNullProbes();
		}

		[[nodiscard]] std::optional<bool> nodePropertyIsNonNull(uint64_t nodeId, std::string_view key) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.nodePropertyIsNonNull(nodeId, key)
				: overlaySnapshot().nodePropertyIsNonNull(nodeId, key);
		}

		[[nodiscard]] std::optional<bool> edgePropertyIsNonNull(uint64_t edgeId, std::string_view key) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.edgePropertyIsNonNull(edgeId, key)
				: overlaySnapshot().edgePropertyIsNonNull(edgeId, key);
		}

		[[nodiscard]] bool supportsNativeAdjacentEdgeTopologyPages() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsNativeAdjacentEdgeTopologyPages()
					: overlaySnapshot().supportsNativeAdjacentEdgeTopologyPages();
		}

		[[nodiscard]] bool supportsNativeAdjacentEdgeRefScans() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsNativeAdjacentEdgeRefScans()
					: overlaySnapshot().supportsNativeAdjacentEdgeRefScans();
		}

		[[nodiscard]] bool supportsAdjacencyReferenceBufferAdmission() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsAdjacencyReferenceBufferAdmission()
				: overlaySnapshot().supportsAdjacencyReferenceBufferAdmission();
		}
		[[nodiscard]] LogicalGraphAdjacencyReferencePage scanAdjacentEdgeRefsWithBufferAdmission(uint64_t nodeId,
			LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
			size_t maximumRecords, const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("overlay admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U) return derived_snapshot_.scanAdjacentEdgeRefsWithBufferAdmission(nodeId, direction, continuation, maximumRecords, guarded);
			auto pinned = pinnedOverlaySnapshot();
			return pinned->scanAdjacentEdgeRefsWithBufferAdmission(nodeId, direction, continuation, maximumRecords, guarded);
		}

		[[nodiscard]] LogicalGraphAdjacencyReferencePage scanAdjacentEdgeRefs(uint64_t nodeId,
				LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
				size_t maximumRecords) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.scanAdjacentEdgeRefs(nodeId, direction, continuation, maximumRecords);
			}
			return overlaySnapshot().scanAdjacentEdgeRefs(nodeId, direction, continuation, maximumRecords);
		}

		void visitEdgeTopologies(size_t maximumRecords,
			const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
			requireActive();
			if (!visitor || maximumRecords == 0U) throw std::invalid_argument("overlay topology visitor requires a visitor and positive bounds");
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) derived_snapshot_.visitEdgeTopologies(maximumRecords, guarded);
			else {
				auto pinned = pinnedOverlaySnapshot();
				pinned->visitEdgeTopologies(maximumRecords, guarded);
			}
		}
		void visitAdjacentEdgeTopologies(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
			const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
			requireActive();
			if (!visitor || maximumEdges == 0U) throw std::invalid_argument("overlay topology visitor requires a visitor and positive bounds");
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) derived_snapshot_.visitAdjacentEdgeTopologies(nodeId, direction, maximumEdges, guarded);
			else {
				auto pinned = pinnedOverlaySnapshot();
				pinned->visitAdjacentEdgeTopologies(nodeId, direction, maximumEdges, guarded);
			}
		}

		[[nodiscard]] bool supportsPropertyTokenResolution() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsPropertyTokenResolution()
				: overlaySnapshot().supportsPropertyTokenResolution();
		}
		[[nodiscard]] bool supportsPropertyPresenceProbes() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsPropertyPresenceProbes()
				: overlaySnapshot().supportsPropertyPresenceProbes();
		}
		[[nodiscard]] std::optional<bool> nodePropertyExists(uint64_t nodeId, std::string_view key) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.nodePropertyExists(nodeId, key)
				: overlaySnapshot().nodePropertyExists(nodeId, key);
		}
		[[nodiscard]] std::optional<bool> edgePropertyExists(uint64_t edgeId, std::string_view key) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.edgePropertyExists(edgeId, key)
				: overlaySnapshot().edgePropertyExists(edgeId, key);
		}

		[[nodiscard]] std::optional<uint32_t> findPropertyToken(std::string_view name) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.findPropertyToken(name);
			}
			return overlaySnapshot().findPropertyToken(name);
		}

		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
				size_t maximumMetadataPages, const EntityReadShape &shape) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.scanNodesAfter(afterNodeId, maximumRecords, maximumMetadataPages, shape);
			}
			return overlaySnapshot().scanNodesAfter(
					afterNodeId, maximumRecords, maximumMetadataPages, shape);
		}

		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSorted(std::span<const uint64_t> nodeIds, const EntityReadShape &shape) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readNodesShapedSorted(nodeIds, shape);
			}
			return overlaySnapshot().readNodesShapedSorted(nodeIds, shape);
		}

		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSorted(std::span<const uint64_t> edgeIds, const EntityReadShape &shape) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readEdgesShapedSorted(edgeIds, shape);
			}
			return overlaySnapshot().readEdgesShapedSorted(edgeIds, shape);
		}

		[[nodiscard]] std::vector<PropertyValue>
		readNodePropertiesSorted(std::span<const uint64_t> nodeIds, std::string_view key) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readNodePropertiesSorted(nodeIds, key);
			}
			return overlaySnapshot().readNodePropertiesSorted(nodeIds, key);
		}

		[[nodiscard]] bool supportsShapedPropertyReadAdmission() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsShapedPropertyReadAdmission()
					: overlaySnapshot().supportsShapedPropertyReadAdmission();
		}

		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSortedWithAdmission(std::span<const uint64_t> nodeIds, const EntityReadShape &shape,
				const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("shaped Property admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U) return derived_snapshot_.readNodesShapedSortedWithAdmission(nodeIds, shape, guarded);
			auto pinned = pinnedOverlaySnapshot();
			return pinned->readNodesShapedSortedWithAdmission(nodeIds, shape, guarded);
		}

		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSortedWithAdmission(std::span<const uint64_t> edgeIds, const EntityReadShape &shape,
				const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("shaped Property admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U) return derived_snapshot_.readEdgesShapedSortedWithAdmission(edgeIds, shape, guarded);
			auto pinned = pinnedOverlaySnapshot();
			return pinned->readEdgesShapedSortedWithAdmission(edgeIds, shape, guarded);
		}

		[[nodiscard]] bool supportsPropertyReadAdmission() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsPropertyReadAdmission()
					: overlaySnapshot().supportsPropertyReadAdmission();
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSortedWithAdmission(std::span<const uint64_t> nodeIds,
				std::span<const std::string_view> keys, const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("Property admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U) return derived_snapshot_.readNodePropertyColumnsSortedWithAdmission(nodeIds, keys, guarded);
			auto pinned = pinnedOverlaySnapshot();
			return pinned->readNodePropertyColumnsSortedWithAdmission(nodeIds, keys, guarded);
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readEdgePropertyColumnsSortedWithAdmission(std::span<const uint64_t> edgeIds,
				std::span<const std::string_view> keys, const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("Property admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U) return derived_snapshot_.readEdgePropertyColumnsSortedWithAdmission(edgeIds, keys, guarded);
			auto pinned = pinnedOverlaySnapshot();
			return pinned->readEdgePropertyColumnsSortedWithAdmission(edgeIds, keys, guarded);
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSorted(std::span<const uint64_t> nodeIds, std::span<const std::string_view> keys) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readNodePropertyColumnsSorted(nodeIds, keys);
			}
			return overlaySnapshot().readNodePropertyColumnsSorted(nodeIds, keys);
		}

		[[nodiscard]] std::vector<PropertyValue>
		readEdgePropertiesSorted(std::span<const uint64_t> edgeIds, std::string_view key) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readEdgePropertiesSorted(edgeIds, key);
			}
			return overlaySnapshot().readEdgePropertiesSorted(edgeIds, key);
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readEdgePropertyColumnsSorted(std::span<const uint64_t> edgeIds, std::span<const std::string_view> keys) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readEdgePropertyColumnsSorted(edgeIds, keys);
			}
			return overlaySnapshot().readEdgePropertyColumnsSorted(edgeIds, keys);
		}

		[[nodiscard]] LogicalGraphNode readNodeShaped(uint64_t nodeId, const EntityReadShape &shape) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readNodeShaped(nodeId, shape);
			}
			return overlaySnapshot().readNodeShaped(nodeId, shape);
		}

		[[nodiscard]] LogicalGraphEdge readEdgeShaped(uint64_t edgeId, const EntityReadShape &shape) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.readEdgeShaped(edgeId, shape);
			}
			return overlaySnapshot().readEdgeShaped(edgeId, shape);
		}

		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override {
			return readNodeShaped(nodeId, completeNodeReadShape());
		}

		[[nodiscard]] bool supportsNodeExistenceProbes() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.supportsNodeExistenceProbes()
					: overlaySnapshot().supportsNodeExistenceProbes();
		}

		[[nodiscard]] std::vector<bool> containsNodesSorted(std::span<const uint64_t> nodeIds) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.containsNodesSorted(nodeIds)
					: overlaySnapshot().containsNodesSorted(nodeIds);
		}

		[[nodiscard]] bool containsNode(uint64_t nodeId) const override {
			requireActive();
			return overlay_.logical_revision_ == 0U ? derived_snapshot_.containsNode(nodeId)
															 : overlaySnapshot().containsNode(nodeId);
		}

		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override {
			return readEdgeShaped(edgeId, completeEdgeReadShape());
		}

		[[nodiscard]] bool supportsNativeExactNodeCandidates() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsNativeExactNodeCandidates();
		}

		[[nodiscard]] std::optional<uint64_t> nativeSnapshotGeneration() const override {
			requireActive();
			const auto generation = overlay_.baseSnapshot().nativeSnapshotGeneration();
			if (!generation || *generation == 0U) return std::nullopt;
			if (overlay_.logical_revision_ > std::numeric_limits<uint64_t>::max() - *generation)
				throw std::overflow_error("overlay candidate generation overflowed");
			return *generation + overlay_.logical_revision_;
		}

		[[nodiscard]] std::optional<IndexCandidateBatch> findNodeCandidatesByExactProperties(
			const std::vector<std::string> &labels, const std::vector<std::pair<std::string, PropertyValue>> &properties,
			size_t maximumNodes, size_t maximumPropertyEntries) const override {
			return overlaySnapshot().findNodeCandidatesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries);
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByExactProperties(
			const std::vector<std::string> &labels, const std::vector<std::pair<std::string, PropertyValue>> &properties,
			size_t maximumNodes, size_t maximumPropertyEntries, const IndexCandidateBatchVisitor &visitor) const override {
			auto pinned = pinnedOverlaySnapshot();
			return pinned->visitNodeCandidatesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries, visitor);
		}
		[[nodiscard]] std::optional<bool> visitNodeCandidatesByExactProperties(
			const std::vector<std::string> &labels, const std::vector<std::pair<std::string, PropertyValue>> &properties,
			size_t maximumNodes, size_t maximumPropertyEntries, size_t maximumCandidateBytes,
			const IndexCandidateBatchVisitor &visitor) const override {
			auto pinned = pinnedOverlaySnapshot();
			return pinned->visitNodeCandidatesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries, maximumCandidateBytes, visitor);
		}

		[[nodiscard]] bool supportsNativeRangeNodeCandidates() const override {
			requireActive();
			return overlay_.baseSnapshot().supportsNativeRangeNodeCandidates();
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByPropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
			size_t maximumNodes, size_t maximumPropertyEntries,
			const IndexCandidateBatchVisitor &visitor) const override {
			auto pinned = pinnedOverlaySnapshot();
			return pinned->visitNodeCandidatesByPropertyRange(labels, range, maximumNodes, maximumPropertyEntries, visitor);
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByPropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
			size_t maximumNodes, size_t maximumPropertyEntries, size_t maximumCandidateBytes,
			const IndexCandidateBatchVisitor &visitor) const override {
			auto pinned = pinnedOverlaySnapshot();
			return pinned->visitNodeCandidatesByPropertyRange(labels, range, maximumNodes, maximumPropertyEntries, maximumCandidateBytes, visitor);
		}

		[[nodiscard]] std::optional<IndexCandidateBatch> findNodeCandidatesByPropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
			size_t maximumNodes, size_t maximumPropertyEntries) const override {
			return overlaySnapshot().findNodeCandidatesByPropertyRange(labels, range, maximumNodes, maximumPropertyEntries);
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByCompositePropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
			size_t maximumNodes, size_t maximumPropertyEntries,
			const IndexCandidateBatchVisitor &visitor) const override {
			auto pinned = pinnedOverlaySnapshot();
			return pinned->visitNodeCandidatesByCompositePropertyRange(labels, request, maximumNodes, maximumPropertyEntries, visitor);
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByCompositePropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
			size_t maximumNodes, size_t maximumPropertyEntries, size_t maximumCandidateBytes,
			const IndexCandidateBatchVisitor &visitor) const override {
			auto pinned = pinnedOverlaySnapshot();
			return pinned->visitNodeCandidatesByCompositePropertyRange(labels, request, maximumNodes, maximumPropertyEntries, maximumCandidateBytes, visitor);
		}

		[[nodiscard]] std::optional<IndexCandidateBatch> findNodeCandidatesByCompositePropertyRange(
			const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
			size_t maximumNodes, size_t maximumPropertyEntries) const override {
			return overlaySnapshot().findNodeCandidatesByCompositePropertyRange(labels, request, maximumNodes, maximumPropertyEntries);
		}

		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
		findNodesByExactProperties(const std::vector<std::string> &labels,
											const std::vector<std::pair<std::string, PropertyValue>> &properties,
											size_t maximumNodes, size_t maximumPropertyEntries) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.findNodesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries);
			}
			return overlaySnapshot().findNodesByExactProperties(
					labels, properties, maximumNodes, maximumPropertyEntries);
		}

		void visitNodes(size_t maximumRecords, const std::function<bool(const LogicalGraphNode &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) {
				derived_snapshot_.visitNodes(maximumRecords, guarded);
				return;
			}
			auto pinned = pinnedOverlaySnapshot();
			pinned->visitNodes(maximumRecords, guarded);
		}

		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																						 size_t maximumMetadataPages) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.scanNodesAfter(afterNodeId, maximumRecords, maximumMetadataPages);
			}
			return overlaySnapshot().scanNodesAfter(afterNodeId, maximumRecords,
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
			return overlaySnapshot().scanAdjacentEdges(nodeId, direction, std::move(continuation),
																												 maximumRecords);
		}

		[[nodiscard]] bool supportsAdjacencyReadAdmission() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U
				? (derived_snapshot_.supportsAdjacencyReadAdmission() || overlaySnapshot().supportsAdjacencyReadAdmission())
				: overlaySnapshot().supportsAdjacencyReadAdmission();
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdgesWithAdmission(
			uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			const std::function<void(size_t)> &propertyAdmission,
			const std::function<void(size_t)> &bufferAdmission) const override {
			if (!supportsAdjacencyReadAdmission()) throw std::logic_error("overlay combined adjacency admission is unavailable");
			if (!propertyAdmission || !bufferAdmission) throw std::invalid_argument("adjacency admission callbacks must not be empty");
			const auto guardedProperty = guardReadAdmission(propertyAdmission);
			const auto guardedBuffer = guardReadAdmission(bufferAdmission);
			if (overlay_.logical_revision_ == 0U && derived_snapshot_.supportsAdjacencyReadAdmission()) {
				return derived_snapshot_.scanAdjacentEdgesWithAdmission(nodeId, direction,
					std::move(continuation), maximumRecords, guardedProperty, guardedBuffer);
			}
			auto pinned = pinnedOverlaySnapshot();
			return pinned->scanAdjacentEdgesWithAdmission(nodeId, direction,
				std::move(continuation), maximumRecords, guardedProperty, guardedBuffer);
		}

		[[nodiscard]] bool supportsAdjacencyPropertyReadAdmission() const override {
			requireActive();
			return overlay_.logical_revision_ == 0U
				? derived_snapshot_.supportsAdjacencyPropertyReadAdmission()
				: overlaySnapshot().supportsAdjacencyPropertyReadAdmission();
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdgesWithPropertyAdmission(
			uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			const std::function<void(size_t)> &admission) const override {
			if (!supportsAdjacencyPropertyReadAdmission()) throw std::logic_error("overlay adjacency Property admission is unavailable");
			if (!admission) throw std::invalid_argument("adjacency Property admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (overlay_.logical_revision_ == 0U) return derived_snapshot_.scanAdjacentEdgesWithPropertyAdmission(nodeId, direction, std::move(continuation), maximumRecords, guarded);
			auto pinned = pinnedOverlaySnapshot();
			return pinned->scanAdjacentEdgesWithPropertyAdmission(nodeId, direction, std::move(continuation), maximumRecords, guarded);
		}

		[[nodiscard]] bool supportsAdjacencyTopologyBufferAdmission() const override {
			requireActive();
			return overlaySnapshot().supportsAdjacencyTopologyBufferAdmission();
		}
		[[nodiscard]] LogicalGraphAdjacencyTopologyPage
		scanAdjacentEdgeTopologiesWithBufferAdmission(uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			const std::function<void(size_t)> &admission) const override {
			requireActive();
			if (!admission) throw std::invalid_argument("overlay admission callback must not be empty");
			const auto guarded = guardReadAdmission(admission);
			if (!supportsAdjacencyTopologyBufferAdmission()) throw std::logic_error("overlay topology buffer admission is unavailable");
			auto pinned = pinnedOverlaySnapshot();
			return pinned->scanAdjacentEdgeTopologiesWithBufferAdmission(
				nodeId, direction, std::move(continuation), maximumRecords, guarded);
		}

		[[nodiscard]] LogicalGraphAdjacencyTopologyPage
		scanAdjacentEdgeTopologies(uint64_t nodeId, LogicalGraphReadDirection direction,
											std::optional<LogicalGraphAdjacencyContinuation> continuation,
											size_t maximumRecords) const override {
			requireActive();
			if (overlay_.logical_revision_ == 0U) {
				return derived_snapshot_.scanAdjacentEdgeTopologies(nodeId, direction, std::move(continuation), maximumRecords);
			}
			return overlaySnapshot().scanAdjacentEdgeTopologies(nodeId, direction,
																																	 std::move(continuation), maximumRecords);
		}

		void visitEdges(size_t maximumRecords, const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) {
				derived_snapshot_.visitEdges(maximumRecords, guarded);
				return;
			}
			auto pinned = pinnedOverlaySnapshot();
			pinned->visitEdges(maximumRecords, guarded);
		}

		void visitAdjacentEdgeRefsForNodes(std::span<const uint64_t> nodeIds,
			LogicalGraphReadDirection direction, size_t maximumEdges,
			const std::function<bool(const EntityRef &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) {
				derived_snapshot_.visitAdjacentEdgeRefsForNodes(nodeIds, direction, maximumEdges, guarded);
				return;
			}
			auto pinned = pinnedOverlaySnapshot();
			pinned->visitAdjacentEdgeRefsForNodes(nodeIds, direction, maximumEdges, guarded);
		}

		void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
									const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			requireActive();
			const auto guarded = guardReadVisitor(visitor);
			if (overlay_.logical_revision_ == 0U) {
				derived_snapshot_.visitAdjacentEdges(nodeId, direction, maximumEdges, guarded);
				return;
			}
			auto pinned = pinnedOverlaySnapshot();
			pinned->visitAdjacentEdges(nodeId, direction, maximumEdges, guarded);
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
			const auto betterHit = [](const LogicalGraphVectorSearchHit &left, const LogicalGraphVectorSearchHit &right) {
				return left.score == right.score ? left.node_id < right.node_id : left.score < right.score;
			};
			LogicalGraphVectorSearchStats stats;
			stats.used_exact_overlay = true;
			auto graphSnapshot = overlay_.openLogicalGraphReadSnapshot();
			const EntityReadShape shape = derived_snapshot_.exactOverlayCandidateReadShape(*index);
			validateEntityReadShape(shape);
			if (request.maximum_mapping_entries == 0U) {
				throw std::invalid_argument("logical transaction Vector scan requires a positive candidate bound");
			}
			uint64_t after = 0U;
			for (;;) {
				if (request.cancellation != nullptr) request.cancellation->check();
				if (stats.exact_overlay_candidate_scans == request.maximum_mapping_entries) {
					throw std::runtime_error("logical transaction Vector scan exceeded its candidate bound");
				}
				const auto page = graphSnapshot->scanNodesAfter(after,
						std::min(size_t(512U), request.maximum_mapping_entries - stats.exact_overlay_candidate_scans),
						std::numeric_limits<size_t>::max(), shape);
				for (const LogicalGraphNode &node: page.nodes) {
					if (request.cancellation != nullptr) request.cancellation->check();
					++stats.exact_overlay_candidate_scans;
					const auto hit = derived_snapshot_.scoreLogicalVectorExactOverlayCandidate(request, *index, node);
					if (hit.has_value() && request.top_k != 0U) {
						if (hits.size() < request.top_k) {
							hits.push_back(*hit);
							std::push_heap(hits.begin(), hits.end(), betterHit);
						} else if (betterHit(*hit, hits.front())) {
							std::pop_heap(hits.begin(), hits.end(), betterHit);
							hits.back() = *hit;
							std::push_heap(hits.begin(), hits.end(), betterHit);
						}
						debug::PerfTrace::addValue("native_graph.read.overlay_vector_retained_hits", static_cast<int64_t>(hits.size()));
					}
					after = node.node_id;
				}
				if (!page.may_have_more) break;
			}

			if (request.cancellation != nullptr) {
				request.cancellation->check();
			}
			std::sort_heap(hits.begin(), hits.end(), betterHit);
			return {std::move(hits), stats};
		}

	private:
		[[nodiscard]] std::shared_ptr<LogicalGraphReadSnapshot> pinnedOverlaySnapshot() const {
			static_cast<void>(overlaySnapshot());
			return overlay_snapshot_;
		}

		[[nodiscard]] LogicalGraphReadSnapshot &overlaySnapshot() const {
			requireActive();
			if (!overlay_snapshot_ || overlay_snapshot_revision_ != overlay_.logical_revision_) {
				overlay_snapshot_ = overlay_.openLogicalGraphReadSnapshot();
				overlay_snapshot_revision_ = overlay_.logical_revision_;
			}
			return *overlay_snapshot_;
		}


		void requireActive() const {
			if (overlay_.transaction_.state() != LogicalGraphTransactionState::LGTTS_ACTIVE) {
				throw std::logic_error("logical transaction Vector snapshot is no longer active");
			}
		}

		const LogicalGraphTransactionOverlay &overlay_;
		LogicalGraphVectorSearchSnapshot &derived_snapshot_;
		// Continuation mappings belong to one logical revision of this wrapper.
		mutable std::shared_ptr<LogicalGraphReadSnapshot> overlay_snapshot_;
		mutable uint64_t overlay_snapshot_revision_ = 0U;
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
			if (baseSnapshot().supportsNativeRelationshipTypeTokens()) {
				state.relationship_type_token = baseSnapshot().findRelationshipTypeToken(request.type).value_or(0U);
			}
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
