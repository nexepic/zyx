#pragma once

/**
 * @file test_LogicalGraphTransactionOverlay.cpp
 * @brief Unit tests for transaction-local logical graph read overlays.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Types.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/LogicalGraphReadExecutor.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"

namespace graph::storage::transaction_overlay_test {

	class InMemoryLogicalSnapshot final : public LogicalGraphReadSnapshot {
		public:
			std::map<uint64_t, LogicalGraphNode> nodes;
			std::map<uint64_t, LogicalGraphEdge> edges;
			LogicalGraphMutationFence fence{1U, 2U, {}};
			size_t required_node_metadata_pages = 1U;
			bool return_empty_nonterminal_node_page = false;
			bool return_zero_cost_nonterminal_node_page = false;
			bool return_oversized_node_metadata_cost = false;
			bool return_zero_cost_terminal_node_page = false;
			bool return_empty_nonterminal_adjacency_page = false;
			bool hide_edges_from_adjacency = false;
			bool ignore_edge_visit_bound = false;
			bool ignore_adjacency_visit_bound = false;
			bool return_no_mutation_fence = false;
			bool adjacency_read_admission = false;
			[[nodiscard]] bool supportsAdjacencyReadAdmission() const override { return adjacency_read_admission; }
			[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdgesWithAdmission(uint64_t nodeId,
				LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
				size_t maximumRecords, const std::function<void(size_t)> &propertyAdmission,
				const std::function<void(size_t)> &bufferAdmission) const override {
				bufferAdmission(29U);
				propertyAdmission(17U);
				return scanAdjacentEdges(nodeId, direction, std::move(continuation), maximumRecords);
			}
			bool adjacency_property_admission = false;
			[[nodiscard]] bool supportsAdjacencyPropertyReadAdmission() const override { return adjacency_property_admission; }
			[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdgesWithPropertyAdmission(uint64_t nodeId,
				LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
				size_t maximumRecords, const std::function<void(size_t)> &admission) const override {
				if (!adjacency_property_admission) throw std::logic_error("adjacency admission is unsupported");
				admission(17U);
				return scanAdjacentEdges(nodeId, direction, std::move(continuation), maximumRecords);
			}
			bool native_topology_pages = false;
			[[nodiscard]] bool supportsNativeAdjacentEdgeTopologyPages() const override { return native_topology_pages; }
			std::optional<LogicalGraphNodePage> forced_node_page;
			std::optional<LogicalGraphAdjacencyTopologyPage> forced_topology_page;
			std::optional<LogicalGraphAdjacencyPage> forced_adjacency_page;
			std::function<std::optional<std::vector<LogicalGraphNode>>(
					const std::vector<std::string> &, const std::vector<std::pair<std::string, PropertyValue>>, size_t,
					size_t)>
					exact_node_lookup;
			bool native_exact_candidates = false;
			std::optional<uint64_t> exact_candidate_generation;
			std::function<std::optional<bool>(const IndexCandidateBatchVisitor &)> exact_candidate_stream;
			[[nodiscard]] bool supportsNativeExactNodeCandidates() const override { return native_exact_candidates; }
			[[nodiscard]] std::optional<uint64_t> nativeSnapshotGeneration() const override { return exact_candidate_generation; }
			[[nodiscard]] std::optional<bool> visitNodeCandidatesByExactProperties(
				const std::vector<std::string> &, const std::vector<std::pair<std::string, PropertyValue>> &,
				size_t, size_t, const IndexCandidateBatchVisitor &visitor) const override {
				return exact_candidate_stream ? exact_candidate_stream(visitor) : std::nullopt;
			}

		bool native_node_refs = false;
		bool ignore_node_ref_stop = false;
		std::optional<std::vector<EntityRef>> forced_node_refs;
		mutable std::vector<size_t> node_ref_bounds;
		[[nodiscard]] bool supportsNativeNodeRefScans() const override { return native_node_refs; }
		void visitNodeRefs(size_t maximumRecords, const std::function<bool(const EntityRef &)> &visitor) const override {
			visitNodeRefsWithLabelMatches(maximumRecords, {}, [&](const EntityRef &ref, bool) { return visitor(ref); });
		}
		void visitNodeRefsWithLabelMatches(size_t maximumRecords, const std::vector<std::string> &labels,
			const std::function<bool(const EntityRef &, bool)> &visitor) const override {
			if (!native_node_refs) {
				LogicalGraphReadSnapshot::visitNodeRefsWithLabelMatches(maximumRecords, labels, visitor);
				return;
			}
			node_ref_bounds.push_back(maximumRecords);
			size_t visited = 0U;
			const auto emit = [&](const EntityRef &ref) {
				if (visited++ == maximumRecords) throw std::runtime_error("test Node reference bound exceeded");
				const auto node = nodes.find(ref.entity_id);
				const bool matches = node != nodes.end() && std::ranges::all_of(labels, [&](const std::string &label) {
					return std::ranges::find(node->second.labels, label) != node->second.labels.end();
				});
				return visitor(ref, matches) || ignore_node_ref_stop;
			};
			if (forced_node_refs) {
				for (const auto &ref: *forced_node_refs) if (!emit(ref)) return;
			} else {
				for (const auto &[id, node]: nodes) {
					static_cast<void>(node);
					if (!emit({id, false, 0U, 0U, 0U})) return;
				}
			}
		}

		bool native_edge_refs = false;
		mutable size_t relationship_type_token_lookup_calls = 0U;
		bool ignore_edge_ref_stop = false;
		std::optional<std::vector<EntityRef>> forced_edge_refs;
		mutable std::vector<size_t> edge_ref_bounds;
		[[nodiscard]] bool supportsNativeEdgeRefScans() const override { return native_edge_refs; }
		[[nodiscard]] bool supportsNativeRelationshipTypeTokens() const override { return native_edge_refs; }
		[[nodiscard]] std::optional<uint64_t> findRelationshipTypeToken(std::string_view name) const override {
			++relationship_type_token_lookup_calls;
			return name == "BASE_LINK" ? std::optional<uint64_t>{5U} : std::nullopt;
		}
		void visitEdgeRefs(size_t maximumRecords, const std::function<bool(const EntityRef &)> &visitor) const override {
			if (!native_edge_refs) {
				LogicalGraphReadSnapshot::visitEdgeRefs(maximumRecords, visitor);
				return;
			}
			edge_ref_bounds.push_back(maximumRecords);
			size_t visited = 0U;
			const auto emit = [&](const EntityRef &ref) {
				if (visited++ == maximumRecords) throw std::runtime_error("test Edge reference bound exceeded");
				return visitor(ref) || ignore_edge_ref_stop;
			};
			if (forced_edge_refs) {
				for (const auto &ref: *forced_edge_refs) if (!emit(ref)) return;
			} else {
				for (const auto &[id, edge]: edges) {
					const uint64_t typeToken = edge.type == "BASE_LINK" ? 5U : 0U;
					if (!emit({id, true, edge.source_node_id, edge.target_node_id, typeToken})) return;
				}
			}
		}

		bool shaped_property_admission = false;
		[[nodiscard]] bool supportsShapedPropertyReadAdmission() const override { return shaped_property_admission; }
		bool selected_property_admission = false;
		[[nodiscard]] bool supportsPropertyReadAdmission() const override { return selected_property_admission; }
		template<typename Entities>
		void admitSelectedProperties(std::span<const uint64_t> ids, std::span<const std::string_view> keys,
				const std::function<void(size_t)> &admission, const Entities &entities) const {
			for (const uint64_t id: ids) {
				for (const std::string_view key: keys) {
					const auto &entity = entities.at(id);
					const auto property = std::ranges::find(entity.properties, key,
							&LogicalGraphPropertyEntry::key);
					if (property != entity.properties.end()) {
						admission(CanonicalPropertyValueCodec::encodedSize(property->value) + sizeof(uint32_t));
					}
				}
			}
		}
		[[nodiscard]] std::vector<std::vector<PropertyValue>> readNodePropertyColumnsSortedWithAdmission(
				std::span<const uint64_t> ids, std::span<const std::string_view> keys,
				const std::function<void(size_t)> &admission) const override {
			if (!selected_property_admission) throw std::logic_error("selected Property admission is unsupported");
			admitSelectedProperties(ids, keys, admission, nodes);
			return readNodePropertyColumnsSorted(ids, keys);
		}
		[[nodiscard]] std::vector<std::vector<PropertyValue>> readEdgePropertyColumnsSortedWithAdmission(
				std::span<const uint64_t> ids, std::span<const std::string_view> keys,
				const std::function<void(size_t)> &admission) const override {
			if (!selected_property_admission) throw std::logic_error("selected Property admission is unsupported");
			admitSelectedProperties(ids, keys, admission, edges);
			return readEdgePropertyColumnsSorted(ids, keys);
		}
		template<typename Entities>
		void admitShapedProperties(std::span<const uint64_t> ids, const EntityReadShape &shape,
				const std::function<void(size_t)> &admission, const Entities &entities) const {
			for (uint64_t id : ids) for (const auto &entry : entities.at(id).properties) {
				const auto token = findPropertyToken(entry.key);
				if (shape.all_properties || (token && std::ranges::binary_search(shape.property_tokens, *token))) {
					admission(CanonicalPropertyValueCodec::encodedSize(entry.value) + sizeof(uint32_t));
				}
			}
		}
		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSortedWithAdmission(std::span<const uint64_t> ids, const EntityReadShape &shape,
				const std::function<void(size_t)> &admission) const override {
			admitShapedProperties(ids, shape, admission, nodes);
			return readNodesShapedSorted(ids, shape);
		}
		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSortedWithAdmission(std::span<const uint64_t> ids, const EntityReadShape &shape,
				const std::function<void(size_t)> &admission) const override {
			admitShapedProperties(ids, shape, admission, edges);
			return readEdgesShapedSorted(ids, shape);
		}

		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override {
			++read_node_calls;
			return nodes.at(nodeId);
		}
		[[nodiscard]] LogicalGraphNode readNodeShaped(uint64_t nodeId, const EntityReadShape &shape) const override {
			node_shapes.push_back(shape);
			return readNode(nodeId);
		}
		mutable size_t read_node_calls = 0U;
		mutable std::vector<EntityReadShape> node_shapes;
		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSorted(std::span<const uint64_t> nodeIds, const EntityReadShape &shape) const override {
			++read_node_batch_calls;
			return LogicalGraphReadSnapshot::readNodesShapedSorted(nodeIds, shape);
		}
		mutable size_t read_node_batch_calls = 0U;
		[[nodiscard]] std::vector<PropertyValue>
		readNodePropertiesSorted(std::span<const uint64_t> nodeIds, std::string_view key) const override {
			++read_node_property_batch_calls;
			return LogicalGraphReadSnapshot::readNodePropertiesSorted(nodeIds, key);
		}
		mutable size_t read_node_property_batch_calls = 0U;
		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSorted(std::span<const uint64_t> nodeIds,
				std::span<const std::string_view> keys) const override {
			++read_node_property_column_calls;
			return LogicalGraphReadSnapshot::readNodePropertyColumnsSorted(nodeIds, keys);
		}
		mutable size_t read_node_property_column_calls = 0U;
		bool native_existence_probes = false;
		mutable std::vector<std::vector<uint64_t>> existence_batches;
		std::optional<std::vector<bool>> forced_existence_batch;
		[[nodiscard]] bool supportsNodeExistenceProbes() const override { return native_existence_probes; }
		[[nodiscard]] std::vector<bool> containsNodesSorted(std::span<const uint64_t> ids) const override {
			existence_batches.emplace_back(ids.begin(), ids.end());
			if (forced_existence_batch.has_value()) return *forced_existence_batch;
			return LogicalGraphReadSnapshot::containsNodesSorted(ids);
		}
		[[nodiscard]] bool containsNode(uint64_t nodeId) const override { return nodes.contains(nodeId); }
		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override {
			++read_edge_calls;
			return edges.at(edgeId);
		}
		[[nodiscard]] LogicalGraphEdge readEdgeShaped(uint64_t edgeId, const EntityReadShape &shape) const override {
			edge_shapes.push_back(shape);
			return readEdge(edgeId);
		}
		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSorted(std::span<const uint64_t> edgeIds, const EntityReadShape &shape) const override {
			++read_edge_batch_calls;
			return LogicalGraphReadSnapshot::readEdgesShapedSorted(edgeIds, shape);
		}
		mutable size_t read_edge_calls = 0U;
		mutable size_t read_edge_batch_calls = 0U;
		mutable std::vector<EntityReadShape> edge_shapes;
		[[nodiscard]] std::vector<PropertyValue>
		readEdgePropertiesSorted(std::span<const uint64_t> edgeIds, std::string_view key) const override {
			++read_edge_property_batch_calls;
			return LogicalGraphReadSnapshot::readEdgePropertiesSorted(edgeIds, key);
		}
		mutable size_t read_edge_property_batch_calls = 0U;
		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readEdgePropertyColumnsSorted(std::span<const uint64_t> edgeIds,
				std::span<const std::string_view> keys) const override {
			++read_edge_property_column_calls;
			return LogicalGraphReadSnapshot::readEdgePropertyColumnsSorted(edgeIds, keys);
		}
		mutable size_t read_edge_property_column_calls = 0U;
		mutable size_t topology_scan_calls = 0U;
		mutable std::vector<size_t> node_scan_bounds;
		mutable std::vector<size_t> adjacency_scan_bounds;
		mutable std::vector<EntityReadShape> scan_node_shapes;
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
				size_t maximumMetadataPages, const EntityReadShape &shape) const override {
			validateEntityReadShape(shape);
			scan_node_shapes.push_back(shape);
			auto page = scanNodesAfter(afterNodeId, maximumRecords, maximumMetadataPages);
			for (auto &node: page.nodes) {
				if (!shape.include_labels) node.labels.clear();
				if (!shape.all_properties) {
					std::erase_if(node.properties, [&](const LogicalGraphPropertyEntry &entry) {
						const auto token = findPropertyToken(entry.key);
						return !token.has_value() || !std::ranges::binary_search(shape.property_tokens, *token);
					});
				}
			}
			return page;
		}
		[[nodiscard]] std::optional<uint32_t> findPropertyToken(std::string_view key) const override {
			if (key == "name") return 1U;
			if (key == "rank") return 2U;
			if (key == "weight") return 3U;
			if (key == "value") return 4U;
			if (key == "payload") return 5U;
			return std::nullopt;
		}
			[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
			findNodesByExactProperties(const std::vector<std::string> &labels,
											 const std::vector<std::pair<std::string, PropertyValue>> &properties,
											 size_t maximumNodes, size_t maximumPropertyEntries) const override {
				if (!exact_node_lookup) {
					return std::nullopt;
				}
				return exact_node_lookup(labels, properties, maximumNodes, maximumPropertyEntries);
			}
			void visitNodes(size_t maximumRecords, const std::function<bool(const LogicalGraphNode &)> &visitor) const override {
				size_t visited = 0U;
				for (const auto &[id, node]: nodes) {
					static_cast<void>(id);
					if (visited == maximumRecords) {
						throw std::runtime_error("test Node visit bound exceeded");
					}
					++visited;
					if (!visitor(node)) {
						return;
					}
				}
			}
			[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																				  size_t maximumMetadataPages) const override {
				node_scan_bounds.push_back(maximumRecords);
				if (maximumRecords == 0U || maximumMetadataPages == 0U) {
					throw std::invalid_argument("test Node scan requires positive bounds");
				}
				if (maximumMetadataPages < required_node_metadata_pages) {
					throw std::runtime_error("test Node scan metadata-page budget was too small");
				}
				LogicalGraphNodePage result;
				result.metadata_pages_visited = required_node_metadata_pages;
				if (forced_node_page.has_value()) {
					return *forced_node_page;
				}
				if (return_zero_cost_terminal_node_page) {
					result.metadata_pages_visited = 0U;
					return result;
				}
				if (return_empty_nonterminal_node_page) {
					result.may_have_more = true;
					if (return_zero_cost_nonterminal_node_page) {
						result.metadata_pages_visited = 0U;
					}
					if (return_oversized_node_metadata_cost) {
						result.metadata_pages_visited = maximumMetadataPages + 1U;
					}
					return result;
				}
				for (const auto &[id, node]: nodes) {
					if (id <= afterNodeId) {
						continue;
					}
					if (result.nodes.size() == maximumRecords) {
						result.may_have_more = true;
						break;
					}
					result.nodes.push_back(node);
				}
				return result;
			}
			[[nodiscard]] LogicalGraphAdjacencyPage
			scanAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
							  std::optional<LogicalGraphAdjacencyContinuation> continuation,
							  size_t maximumRecords) const override {
				adjacency_scan_bounds.push_back(maximumRecords);
				if (maximumRecords == 0U) {
					throw std::invalid_argument("test adjacency scan requires a positive bound");
				}
				if (return_empty_nonterminal_adjacency_page) {
					return {{}, LogicalGraphAdjacencyContinuation{1U, 2U}};
				}
				if (forced_adjacency_page.has_value()) return *forced_adjacency_page;
				const uint64_t after = continuation.has_value() ? continuation->next_edge_id - 1U : 0U;
				LogicalGraphAdjacencyPage result;
				for (const auto &[id, edge]: edges) {
					const bool matches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
																 ? edge.source_node_id == nodeId
																 : edge.target_node_id == nodeId;
					if (hide_edges_from_adjacency || id <= after || !matches) {
						continue;
					}
					if (result.edges.size() == maximumRecords) {
						result.continuation = {result.edges.back().edge_id, id};
						break;
					}
					result.edges.push_back(edge);
				}
				return result;
			}
			[[nodiscard]] LogicalGraphAdjacencyTopologyPage
			scanAdjacentEdgeTopologies(uint64_t nodeId, LogicalGraphReadDirection direction,
									std::optional<LogicalGraphAdjacencyContinuation> continuation,
									size_t maximumRecords) const override {
				if (maximumRecords == 0U) {
					throw std::invalid_argument("test topology scan requires a positive bound");
				}
				++topology_scan_calls;
				if (forced_topology_page.has_value()) return *forced_topology_page;
				const uint64_t after = continuation.has_value() ? continuation->next_edge_id - 1U : 0U;
				LogicalGraphAdjacencyTopologyPage result;
				for (const auto &[id, edge]: edges) {
					const bool matches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
											? edge.source_node_id == nodeId
											: edge.target_node_id == nodeId;
					if (id <= after || !matches) {
						continue;
					}
					if (result.edges.size() == maximumRecords) {
						result.continuation = {result.edges.back().edge_id, id};
						break;
					}
					result.edges.emplace_back(edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type);
				}
				return result;
			}
			void visitEdges(size_t maximumRecords, const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
				size_t visited = 0U;
				for (const auto &[id, edge]: edges) {
					static_cast<void>(id);
					if (!ignore_edge_visit_bound && visited == maximumRecords) {
						throw std::runtime_error("test Edge visit bound exceeded");
					}
					++visited;
					if (!visitor(edge)) {
						return;
					}
				}
			}
			void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
													const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
				if (ignore_adjacency_visit_bound) {
					for (const auto &[id, edge]: edges) {
						static_cast<void>(id);
						const bool matches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
																 ? edge.source_node_id == nodeId
																 : edge.target_node_id == nodeId;
						if (matches && !visitor(edge)) {
							return;
						}
					}
					return;
				}
				std::optional<LogicalGraphAdjacencyContinuation> continuation;
				size_t visited = 0U;
				while (true) {
					const LogicalGraphAdjacencyPage page = scanAdjacentEdges(nodeId, direction, continuation,
																				 maximumEdges - visited);
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
						throw std::runtime_error("test adjacency visit bound exceeded");
					}
					continuation = page.continuation;
				}
			}
		[[nodiscard]] std::optional<LogicalGraphMutationFence> mutationFence() const override {
			return return_no_mutation_fence ? std::nullopt : std::optional<LogicalGraphMutationFence>(fence);
		}
		std::optional<LogicalGraphSchemaIdentity> schema_identity;
		[[nodiscard]] std::optional<LogicalGraphSchemaIdentity> bindingSchemaIdentity() const override {
			return schema_identity;
		}
	};

	class InMemoryVectorSnapshot final : public LogicalGraphVectorSearchSnapshot {
	public:
		explicit InMemoryVectorSnapshot(const InMemoryLogicalSnapshot &snapshot) : snapshot_(snapshot) {}
		[[nodiscard]] bool supportsNativeAdjacentEdgeTopologyPages() const override {
			return snapshot_.supportsNativeAdjacentEdgeTopologyPages();
		}
		[[nodiscard]] LogicalGraphAdjacencyTopologyPage scanAdjacentEdgeTopologies(uint64_t nodeId,
				LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
				size_t maximumRecords) const override {
			return snapshot_.scanAdjacentEdgeTopologies(nodeId, direction, continuation, maximumRecords);
		}

		[[nodiscard]] std::optional<uint32_t> findPropertyToken(std::string_view name) const override {
			return snapshot_.findPropertyToken(name);
		}
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
				size_t maximumMetadataPages, const EntityReadShape &shape) const override {
			return snapshot_.scanNodesAfter(afterNodeId, maximumRecords, maximumMetadataPages, shape);
		}

		[[nodiscard]] bool supportsShapedPropertyReadAdmission() const override { return snapshot_.supportsShapedPropertyReadAdmission(); }
		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSortedWithAdmission(std::span<const uint64_t> ids, const EntityReadShape &shape,
				const std::function<void(size_t)> &admission) const override {
			return snapshot_.readNodesShapedSortedWithAdmission(ids, shape, admission);
		}
		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSortedWithAdmission(std::span<const uint64_t> ids, const EntityReadShape &shape,
				const std::function<void(size_t)> &admission) const override {
			return snapshot_.readEdgesShapedSortedWithAdmission(ids, shape, admission);
		}
		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSorted(std::span<const uint64_t> nodeIds, const EntityReadShape &shape) const override {
			return snapshot_.readNodesShapedSorted(nodeIds, shape);
		}
		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSorted(std::span<const uint64_t> edgeIds, const EntityReadShape &shape) const override {
			return snapshot_.readEdgesShapedSorted(edgeIds, shape);
		}

		[[nodiscard]] std::vector<PropertyValue>
		readNodePropertiesSorted(std::span<const uint64_t> nodeIds, std::string_view key) const override {
			return snapshot_.readNodePropertiesSorted(nodeIds, key);
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSorted(std::span<const uint64_t> nodeIds, std::span<const std::string_view> keys) const override {
			return snapshot_.readNodePropertyColumnsSorted(nodeIds, keys);
		}

		[[nodiscard]] std::vector<PropertyValue>
		readEdgePropertiesSorted(std::span<const uint64_t> edgeIds, std::string_view key) const override {
			return snapshot_.readEdgePropertiesSorted(edgeIds, key);
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readEdgePropertyColumnsSorted(std::span<const uint64_t> edgeIds, std::span<const std::string_view> keys) const override {
			return snapshot_.readEdgePropertyColumnsSorted(edgeIds, keys);
		}

		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override { return snapshot_.readNode(nodeId); }
		[[nodiscard]] LogicalGraphNode readNodeShaped(uint64_t nodeId, const EntityReadShape &shape) const override {
			return snapshot_.readNodeShaped(nodeId, shape);
		}
		[[nodiscard]] bool supportsNodeExistenceProbes() const override {
			return snapshot_.supportsNodeExistenceProbes();
		}
		[[nodiscard]] std::vector<bool> containsNodesSorted(std::span<const uint64_t> ids) const override {
			return snapshot_.containsNodesSorted(ids);
		}
		[[nodiscard]] bool containsNode(uint64_t nodeId) const override { return snapshot_.containsNode(nodeId); }
		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override { return snapshot_.readEdge(edgeId); }
		[[nodiscard]] LogicalGraphEdge readEdgeShaped(uint64_t edgeId, const EntityReadShape &shape) const override {
			return snapshot_.readEdgeShaped(edgeId, shape);
		}
		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
		findNodesByExactProperties(const std::vector<std::string> &labels,
									 const std::vector<std::pair<std::string, PropertyValue>> &properties,
									 size_t maximumNodes, size_t maximumPropertyEntries) const override {
			return snapshot_.findNodesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries);
		}
		void visitNodes(size_t maximumRecords, const std::function<bool(const LogicalGraphNode &)> &visitor) const override {
			snapshot_.visitNodes(maximumRecords, visitor);
		}
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																		 size_t maximumMetadataPages) const override {
			return snapshot_.scanNodesAfter(afterNodeId, maximumRecords, maximumMetadataPages);
		}
		[[nodiscard]] LogicalGraphAdjacencyPage
		scanAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
						  std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const override {
			return snapshot_.scanAdjacentEdges(nodeId, direction, continuation, maximumRecords);
		}
		void visitEdges(size_t maximumRecords, const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			snapshot_.visitEdges(maximumRecords, visitor);
		}
		void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
									const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			snapshot_.visitAdjacentEdges(nodeId, direction, maximumEdges, visitor);
		}
		[[nodiscard]] std::optional<LogicalGraphMutationFence> mutationFence() const override {
			return snapshot_.mutationFence();
		}
		[[nodiscard]] LogicalGraphVectorSearchResult
		searchLogicalVector(const LogicalGraphVectorSearchRequest &) const override {
			throw std::logic_error("test derived Vector snapshot must use the exact overlay fallback");
		}
		[[nodiscard]] std::optional<LogicalGraphVectorIndexDescriptor>
		describeLogicalVectorIndex(const LogicalGraphVectorSearchRequest &) const override {
			return LogicalGraphVectorIndexDescriptor{1U, "Embedding", "value", 2U, "l2"};
		}
		[[nodiscard]] EntityReadShape exactOverlayCandidateReadShape(const LogicalGraphVectorIndexDescriptor &index) const override {
			return selected_candidates ? EntityReadShape{.include_labels = true, .property_tokens = {4U}}
					: LogicalGraphVectorSearchSnapshot::exactOverlayCandidateReadShape(index);
		}
		[[nodiscard]] std::optional<LogicalGraphVectorSearchHit>
		scoreLogicalVectorExactOverlayCandidate(const LogicalGraphVectorSearchRequest &,
															 const LogicalGraphVectorIndexDescriptor &,
															 const LogicalGraphNode &node) const override {
			++score_calls;
			scored_nodes.push_back(node);
			if (!std::ranges::binary_search(node.labels, std::string("Embedding"))) {
				return std::nullopt;
			}
			const auto score = scores.find(node.node_id);
			return LogicalGraphVectorSearchHit{node.node_id, score == scores.end() ? 0.0F : score->second};
		}

		mutable size_t score_calls = 0U;
		bool selected_candidates = true;
		std::map<uint64_t, float> scores;
		mutable std::vector<LogicalGraphNode> scored_nodes;

	private:
		const InMemoryLogicalSnapshot &snapshot_;
	};

	class ThrowAfterCancellationCheckpoint final : public CancellationCheckpoint {
	public:
		explicit ThrowAfterCancellationCheckpoint(size_t throwAfter) : throw_after_(throwAfter) {}

		void check() const override {
			++checks;
			if (checks == throw_after_) {
				throw std::runtime_error("test transaction-overlay cancellation checkpoint");
			}
		}

		mutable size_t checks = 0U;

	private:
		size_t throw_after_ = 0U;
	};

	class InMemoryLogicalTransaction final : public LogicalGraphTransaction,
												 public LogicalGraphTransactionVectorSnapshotProvider {
	public:
		explicit InMemoryLogicalTransaction(bool readOnly = false, size_t requiredNodeMetadataPages = 1U) :
			vector_snapshot_(snapshot_), read_only_(readOnly) {
			snapshot_.required_node_metadata_pages = requiredNodeMetadataPages;
			snapshot_.nodes.emplace(1U, LogicalGraphNode{1U, {"Base"}, {{"name", PropertyValue("Ada")}}});
			snapshot_.nodes.emplace(2U, LogicalGraphNode{2U, {"Base"}});
			snapshot_.edges.emplace(10U, LogicalGraphEdge{10U, 1U, 2U, "BASE_LINK"});
		}

		[[nodiscard]] LogicalGraphTransactionState state() const override { return state_; }
		[[nodiscard]] bool isReadOnly() const override { return read_only_; }
		void setIncompletePreview(bool enabled) { incomplete_preview_ = enabled; }
		void setRejectCreatedPropertyOverrides(bool enabled) { reject_created_property_overrides_ = enabled; }
		void setMismatchedStageResult(bool enabled) { mismatched_stage_result_ = enabled; }
		void setRejectStage(bool enabled) { reject_stage_ = enabled; }
		void setDuplicateNodeIds(bool enabled) { duplicate_node_ids_ = enabled; }
		void setDuplicateEdgeIds(bool enabled) { duplicate_edge_ids_ = enabled; }
		[[nodiscard]] LogicalGraphReadSnapshot &readSnapshot() override {
			requireActive();
			return snapshot_;
		}
		[[nodiscard]] LogicalGraphVectorSearchSnapshot &pinnedLogicalGraphVectorSearchSnapshot() override {
			requireActive();
			return vector_snapshot_;
		}
		[[nodiscard]] size_t vectorScoreCalls() const { return vector_snapshot_.score_calls; }
		[[nodiscard]] size_t topologyScanCalls() const { return snapshot_.topology_scan_calls; }
		[[nodiscard]] size_t readEdgeCalls() const { return snapshot_.read_edge_calls; }
		[[nodiscard]] size_t readNodeCalls() const { return snapshot_.read_node_calls; }
		[[nodiscard]] size_t readNodeBatchCalls() const { return snapshot_.read_node_batch_calls; }
		[[nodiscard]] size_t readEdgeBatchCalls() const { return snapshot_.read_edge_batch_calls; }
		[[nodiscard]] size_t readNodePropertyBatchCalls() const { return snapshot_.read_node_property_batch_calls; }
		[[nodiscard]] size_t readEdgePropertyBatchCalls() const { return snapshot_.read_edge_property_batch_calls; }
		[[nodiscard]] LogicalGraphMutationBatchResult
		previewMutationBatch(const LogicalGraphMutationBatch &batch) const override {
			requireActive();
			if (reject_created_property_overrides_) {
				for (const auto &request : batch.property_replacements) {
					if ((request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE &&
						 !batch.node_creates.at(static_cast<size_t>(request.entity.value)).properties.empty()) ||
						(request.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE &&
						 !batch.edge_creates.at(static_cast<size_t>(request.entity.value)).properties.empty()))
						throw std::invalid_argument("Created properties must be folded into creation requests");
				}
			}
			if (read_only_) {
				throw std::logic_error("read-only test transaction cannot preview writes");
			}
			LogicalGraphMutationBatchResult result;
			for (size_t index = 0U; index < batch.node_creates.size(); ++index) {
				result.created_node_ids.push_back(100U + staged_node_count_ + index);
			}
			for (size_t index = 0U; index < batch.edge_creates.size(); ++index) {
				result.created_edge_ids.push_back(200U + staged_edge_count_ + index);
			}
			if (incomplete_preview_) {
				if (!result.created_node_ids.empty()) {
					result.created_node_ids.pop_back();
				} else if (!result.created_edge_ids.empty()) {
					result.created_edge_ids.pop_back();
				}
			}
			if (duplicate_node_ids_ && result.created_node_ids.size() > 1U) {
				result.created_node_ids[1] = result.created_node_ids.front();
			}
			if (duplicate_edge_ids_ && result.created_edge_ids.size() > 1U) {
				result.created_edge_ids[1] = result.created_edge_ids.front();
			}
			return result;
		}
		[[nodiscard]] LogicalGraphMutationBatchResult stageMutationBatch(LogicalGraphMutationBatch batch) override {
			LogicalGraphMutationBatchResult result = previewMutationBatch(batch);
			if (reject_stage_) throw std::runtime_error("injected statement staging failure");
			staged_node_count_ += batch.node_creates.size();
			staged_edge_count_ += batch.edge_creates.size();
			if (mismatched_stage_result_) {
				if (!result.created_node_ids.empty()) {
					++result.created_node_ids.front();
				} else if (!result.created_edge_ids.empty()) {
					++result.created_edge_ids.front();
				}
			}
			return result;
		}
		[[nodiscard]] LogicalGraphMutationBatchResult commit() override {
			requireActive();
			state_ = LogicalGraphTransactionState::LGTTS_COMMITTED;
			return {};
		}
		void rollback() override {
			requireActive();
			state_ = LogicalGraphTransactionState::LGTTS_ROLLED_BACK;
		}

	private:
		void requireActive() const {
			if (state_ != LogicalGraphTransactionState::LGTTS_ACTIVE) {
				throw std::logic_error("test transaction is not active");
			}
		}

		InMemoryLogicalSnapshot snapshot_;
		InMemoryVectorSnapshot vector_snapshot_;
		LogicalGraphTransactionState state_ = LogicalGraphTransactionState::LGTTS_ACTIVE;
		bool read_only_ = false;
		size_t staged_node_count_ = 0U;
		size_t staged_edge_count_ = 0U;
		bool incomplete_preview_ = false;
		bool reject_created_property_overrides_ = false;
		bool mismatched_stage_result_ = false;
		bool reject_stage_ = false;
		bool duplicate_node_ids_ = false;
		bool duplicate_edge_ids_ = false;
	};


} // namespace graph::storage::transaction_overlay_test
