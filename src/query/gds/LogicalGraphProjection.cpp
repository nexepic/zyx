/**
 * @file LogicalGraphProjection.cpp
 * @brief Storage-neutral immutable graph projection implementation.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/gds/LogicalGraphProjection.hpp"

#include "graph/core/PropertyTypes.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <variant>

namespace graph::query::gds {

	namespace {

		const std::vector<LogicalProjectionEdge> EMPTY_EDGES;
		// Keep one physical adjacency read bounded even when a caller grants a
		// very large per-Node logical budget. The cursor continuation preserves
		// correctness across pages, while this cap prevents a supernode from
		// becoming an unbounded projection staging buffer.
		constexpr size_t MAXIMUM_ADJACENCY_BATCH = 256U;

		[[nodiscard]] bool containsLabel(const storage::LogicalGraphNode &node,
												const std::unordered_set<std::string_view> &labels) {
			if (labels.empty()) {
				return true;
			}
			return std::ranges::any_of(node.labels, [&labels](const std::string &label) { return labels.contains(label); });
		}

		void validateWeight(const double weight, const std::string_view context) {
			if (!std::isfinite(weight) || weight < 0.0) {
				throw std::invalid_argument(std::string(context) + " must be a non-negative finite number");
			}
		}

		[[nodiscard]] double resolveWeightValue(const PropertyValue &value, const LogicalProjectionWeight &spec) {
			if (value.getType() == PropertyType::NULL_TYPE) {
				validateWeight(spec.default_weight, "logical projection default weight");
				return spec.default_weight;
			}
			double weight = 0.0;
			if (value.getType() == PropertyType::INTEGER) {
				weight = static_cast<double>(std::get<int64_t>(value.getVariant()));
			} else if (value.getType() == PropertyType::DOUBLE) {
				weight = std::get<double>(value.getVariant());
			} else {
				throw std::invalid_argument("logical projection weight property must be numeric");
			}
			validateWeight(weight, "logical projection property weight");
			return weight;
		}

		void sortAdjacency(std::unordered_map<uint64_t, std::vector<LogicalProjectionEdge>> &adjacency) {
			for (auto &[_, edges]: adjacency) {
				std::ranges::sort(edges, [](const LogicalProjectionEdge &left, const LogicalProjectionEdge &right) {
					return std::tie(left.target_node_id, left.relationship_id, left.synthetic_reverse) <
						   std::tie(right.target_node_id, right.relationship_id, right.synthetic_reverse);
				});
			}
		}

		void checkBuildCheckpoint(const LogicalProjectionBuildCheckpoint &checkpoint) {
			if (checkpoint) {
				checkpoint();
			}
		}

		[[nodiscard]] size_t checkedPropertyColumnBytes(const std::vector<PropertyValue> &values,
				std::string_view context) {
			size_t bytes = 0U;
			for (const PropertyValue &value: values) {
				const size_t valueBytes = property_utils::getPropertyValueSize(value);
				if (valueBytes > std::numeric_limits<size_t>::max() - bytes) {
					throw std::overflow_error(std::string(context) + " size overflows its host size domain");
				}
				bytes += valueBytes;
			}
			return bytes;
		}

	} // namespace

	void LogicalGraphProjection::addArc(LogicalGraphProjection &projection, const uint64_t sourceNodeId,
			const uint64_t targetNodeId, const double weight, const uint64_t relationshipId,
			const bool syntheticReverse) {
		projection.outgoing_[sourceNodeId].push_back({targetNodeId, weight, relationshipId, syntheticReverse});
		projection.incoming_[targetNodeId].push_back({sourceNodeId, weight, relationshipId, syntheticReverse});
		++projection.edge_count_;
	}

	void LogicalGraphProjection::addRelationship(LogicalGraphProjection &projection, const storage::LogicalGraphEdge &edge,
			const LogicalRelationshipProjection &relationship, const double weight) {
		switch (relationship.orientation) {
		case LogicalProjectionOrientation::LPO_NATURAL:
			addArc(projection, edge.source_node_id, edge.target_node_id, weight, edge.edge_id, false);
			return;
		case LogicalProjectionOrientation::LPO_REVERSE:
			addArc(projection, edge.target_node_id, edge.source_node_id, weight, edge.edge_id, false);
			return;
		case LogicalProjectionOrientation::LPO_UNDIRECTED:
			addArc(projection, edge.source_node_id, edge.target_node_id, weight, edge.edge_id, false);
			if (edge.source_node_id != edge.target_node_id) {
				addArc(projection, edge.target_node_id, edge.source_node_id, weight, edge.edge_id, true);
			}
			return;
		}
		throw std::logic_error("logical projection has an unknown relationship orientation");
	}

	LogicalGraphProjection LogicalGraphProjection::build(const storage::LogicalGraphReadSnapshot &snapshot,
			const LogicalGraphProjectionSpec &spec, const LogicalProjectionBuildBounds bounds,
			const LogicalProjectionBuildCheckpoint &checkpoint) {
		checkBuildCheckpoint(checkpoint);
		if (spec.name.empty()) {
			throw std::invalid_argument("logical projection name must not be empty");
		}
		if (bounds.maximum_nodes == 0U || bounds.maximum_metadata_pages == 0U ||
				bounds.maximum_edges_per_node == 0U || bounds.maximum_property_bytes == 0U) {
			throw std::invalid_argument("logical projection requires positive Node, metadata, and Edge bounds");
		}
		if (bounds.maximum_nodes > std::numeric_limits<size_t>::max() / bounds.maximum_edges_per_node) {
			throw std::overflow_error("logical projection adjacency bound overflows its size domain");
		}

		std::unordered_set<std::string_view> labelFilter;
		labelFilter.reserve(spec.node_labels.size());
		for (const std::string &label: spec.node_labels) {
			if (label.empty() || !labelFilter.insert(label).second) {
				throw std::invalid_argument("logical projection labels must be nonempty and unique");
			}
		}

		LogicalRelationshipProjection defaultRelationship;
		defaultRelationship.orientation = spec.default_orientation;
		const std::vector<LogicalRelationshipProjection> *relationships = &spec.relationships;
		if (relationships->empty()) {
			relationships = nullptr;
		}
		std::unordered_map<std::string_view, const LogicalRelationshipProjection *> byType;
		const LogicalRelationshipProjection *allTypes = relationships == nullptr ? &defaultRelationship : nullptr;
		if (relationships != nullptr) {
			byType.reserve(relationships->size());
			for (const LogicalRelationshipProjection &relationship: *relationships) {
				if (relationship.type.empty()) {
					if (allTypes != nullptr) {
						throw std::invalid_argument("logical projection may define at most one all-types relationship");
					}
					allTypes = &relationship;
					continue;
				}
				if (!byType.emplace(relationship.type, &relationship).second) {
					throw std::invalid_argument("logical projection relationship types must be unique");
				}
			}
		}

		LogicalGraphProjection projection;
		projection.is_weighted_ = relationships != nullptr && std::ranges::any_of(*relationships,
				[](const LogicalRelationshipProjection &relationship) {
					return relationship.weight.kind != LogicalProjectionWeightKind::LPWK_NONE;
				});

		// Consume the Node cursor page by page. A physical page may be smaller than
		// the logical projection bound, so rejecting `may_have_more` from the first
		// call would incorrectly truncate valid multi-page projections. The total
		// record and metadata budgets remain enforced across the complete cursor.
		std::vector<storage::LogicalGraphNode> scannedNodes;
		scannedNodes.reserve(bounds.maximum_nodes);
		uint64_t afterNodeId = 0U;
		size_t metadataPagesVisited = 0U;
		for (;;) {
			if (scannedNodes.size() == bounds.maximum_nodes) {
				throw std::runtime_error("logical projection exceeded its bounded Node scan");
			}
			const size_t remainingNodes = bounds.maximum_nodes - scannedNodes.size();
			const size_t remainingMetadata = bounds.maximum_metadata_pages - metadataPagesVisited;
			if (remainingMetadata == 0U) {
				throw std::runtime_error("logical projection exceeded its bounded metadata scan");
			}
			const storage::LogicalGraphNodePage page =
					snapshot.scanNodesAfter(afterNodeId, remainingNodes, remainingMetadata);
			if (page.nodes.size() > remainingNodes || page.metadata_pages_visited > remainingMetadata) {
				throw std::runtime_error("logical projection exceeded its bounded Node scan");
			}
			if (page.nodes.empty() && page.may_have_more) {
				throw std::runtime_error("logical projection returned an empty nonterminal Node page");
			}
			for (const storage::LogicalGraphNode &node: page.nodes) {
				if (node.node_id == 0U || (!scannedNodes.empty() && node.node_id <= scannedNodes.back().node_id)) {
					throw std::runtime_error("logical projection snapshot returned unordered Nodes");
				}
				scannedNodes.push_back(node);
			}
			if (page.metadata_pages_visited > std::numeric_limits<size_t>::max() - metadataPagesVisited) {
				throw std::overflow_error("logical projection metadata-page count overflowed");
			}
			metadataPagesVisited += page.metadata_pages_visited;
			if (!page.may_have_more) {
				break;
			}
			if (page.nodes.empty() || page.nodes.back().node_id <= afterNodeId) {
				throw std::runtime_error("logical projection returned an invalid Node continuation");
			}
			afterNodeId = page.nodes.back().node_id;
		}
		std::unordered_set<uint64_t> selectedNodes;
		selectedNodes.reserve(scannedNodes.size());
		uint64_t previousNodeId = 0U;
		for (const storage::LogicalGraphNode &node: scannedNodes) {
			checkBuildCheckpoint(checkpoint);
			if (node.node_id == 0U || node.node_id <= previousNodeId) {
				throw std::runtime_error("logical projection snapshot returned unordered or invalid Nodes");
			}
			previousNodeId = node.node_id;
			if (containsLabel(node, labelFilter)) {
				projection.node_ids_.push_back(node.node_id);
			}
		}
		selectedNodes.insert(projection.node_ids_.begin(), projection.node_ids_.end());

		// Keep topology reads separate from Property reads.  Unweighted projections
		// never need to materialize an Edge; weighted projections fetch each named
		// weight column once after all adjacency pages have been collected.
		struct PendingRelationship {
			storage::LogicalGraphEdgeTopology topology;
			const LogicalRelationshipProjection *relationship = nullptr;
			double weight = 1.0;
		};
		std::vector<PendingRelationship> pending;
		pending.reserve(projection.node_ids_.size());
		std::map<std::string, std::vector<size_t>> weightColumns;
		for (const uint64_t nodeId: projection.node_ids_) {
			// A physical adjacency page may be smaller than the logical per-Node
			// budget. Consume its continuation until the bounded budget is exhausted;
			// rejecting the first continuation would incorrectly fail valid high-degree
			// projections on backends with small pages.
			std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
			size_t scannedEdges = 0U;
			uint64_t previousEdgeId = 0U;
			for (;;) {
				const size_t remaining = bounds.maximum_edges_per_node - scannedEdges;
				if (remaining == 0U) {
					throw std::runtime_error("logical projection exceeded its bounded adjacency scan");
				}
				const storage::LogicalGraphAdjacencyTopologyPage page = snapshot.scanAdjacentEdgeTopologies(
						nodeId, storage::LogicalGraphReadDirection::LGRD_OUTGOING, continuation,
						std::min(remaining, MAXIMUM_ADJACENCY_BATCH));
				if (page.edges.size() > remaining || (page.edges.empty() && page.continuation.has_value()) ||
						(continuation.has_value() &&
							(page.edges.empty() || page.edges.front().edge_id != continuation->next_edge_id))) {
					throw std::runtime_error("logical projection returned an invalid adjacency continuation");
				}
				for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
				checkBuildCheckpoint(checkpoint);
				if (edge.edge_id == 0U || edge.edge_id <= previousEdgeId || edge.source_node_id == 0U ||
					edge.target_node_id == 0U ||
					edge.source_node_id != nodeId) {
					throw std::runtime_error("logical projection snapshot returned an invalid relationship");
				}
				previousEdgeId = edge.edge_id;
				if (!selectedNodes.contains(edge.source_node_id) || !selectedNodes.contains(edge.target_node_id)) {
					continue;
				}
				const auto type = byType.find(edge.type);
				const LogicalRelationshipProjection *relationship = type == byType.end() ? allTypes : type->second;
				if (relationship == nullptr) {
					continue;
				}
				const size_t pendingIndex = pending.size();
				pending.push_back({edge, relationship, 1.0});
				if (relationship->weight.kind == LogicalProjectionWeightKind::LPWK_PROPERTY) {
					if (relationship->weight.property_name.empty()) {
						throw std::invalid_argument("logical projection weight property name must not be empty");
					}
					validateWeight(relationship->weight.default_weight, "logical projection default weight");
					weightColumns[relationship->weight.property_name].push_back(pendingIndex);
				} else if (relationship->weight.kind == LogicalProjectionWeightKind::LPWK_CONSTANT) {
					validateWeight(relationship->weight.constant_weight, "logical projection constant weight");
					pending.back().weight = relationship->weight.constant_weight;
				} else if (relationship->weight.kind != LogicalProjectionWeightKind::LPWK_NONE) {
					throw std::logic_error("logical projection has an unknown weight kind");
				}
			}
				scannedEdges += page.edges.size();
				if (!page.continuation.has_value()) {
					break;
				}
				if (scannedEdges == bounds.maximum_edges_per_node ||
						page.continuation->previous_edge_id != page.edges.back().edge_id ||
						page.continuation->next_edge_id == 0U ||
						page.continuation->previous_edge_id == page.continuation->next_edge_id) {
					throw std::runtime_error("logical projection exceeded its bounded adjacency scan");
				}
				continuation = page.continuation;
			}
		}
		for (auto &[key, indexes]: weightColumns) {
			std::vector<uint64_t> edgeIds;
			edgeIds.reserve(indexes.size());
			for (const size_t index: indexes) {
				edgeIds.push_back(pending[index].topology.edge_id);
			}
			std::ranges::sort(edgeIds);
			edgeIds.erase(std::ranges::unique(edgeIds).begin(), edgeIds.end());
			const std::vector<PropertyValue> values = snapshot.readEdgePropertiesSorted(edgeIds, key);
			if (values.size() != edgeIds.size()) {
				throw std::runtime_error("logical projection weight column changed cardinality");
			}
			if (checkedPropertyColumnBytes(values, "logical projection weight column") > bounds.maximum_property_bytes) {
				throw std::runtime_error("logical projection weight column exceeded its byte bound");
			}
			std::unordered_map<uint64_t, PropertyValue> byEdge;
			byEdge.reserve(edgeIds.size());
			for (size_t index = 0U; index < edgeIds.size(); ++index) {
				byEdge.emplace(edgeIds[index], values[index]);
			}
			for (const size_t index: indexes) {
				const auto found = byEdge.find(pending[index].topology.edge_id);
				if (found == byEdge.end()) {
					throw std::runtime_error("logical projection weight column lost an Edge");
				}
				pending[index].weight = resolveWeightValue(found->second, pending[index].relationship->weight);
			}
		}
		for (const PendingRelationship &relationship: pending) {
			const auto &edge = relationship.topology;
			storage::LogicalGraphEdge logicalEdge{edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type};
			LogicalGraphProjection::addRelationship(projection, logicalEdge, *relationship.relationship,
					relationship.weight);
		}
		sortAdjacency(projection.outgoing_);
		sortAdjacency(projection.incoming_);
		return projection;
	}

	const std::vector<LogicalProjectionEdge> &LogicalGraphProjection::outgoing(const uint64_t nodeId) const noexcept {
		const auto found = outgoing_.find(nodeId);
		return found == outgoing_.end() ? EMPTY_EDGES : found->second;
	}

	const std::vector<LogicalProjectionEdge> &LogicalGraphProjection::incoming(const uint64_t nodeId) const noexcept {
		const auto found = incoming_.find(nodeId);
		return found == incoming_.end() ? EMPTY_EDGES : found->second;
	}

	size_t LogicalGraphProjection::estimatedMemoryBytes() const noexcept {
		size_t bytes = sizeof(*this) + node_ids_.capacity() * sizeof(uint64_t);
		for (const auto &[_, edges]: outgoing_) {
			bytes += sizeof(std::pair<const uint64_t, std::vector<LogicalProjectionEdge>>) +
					edges.capacity() * sizeof(LogicalProjectionEdge);
		}
		for (const auto &[_, edges]: incoming_) {
			bytes += sizeof(std::pair<const uint64_t, std::vector<LogicalProjectionEdge>>) +
					edges.capacity() * sizeof(LogicalProjectionEdge);
		}
		return bytes;
	}

	LogicalProjectionCsrCacheState LogicalGraphProjection::csrCacheState() const {
		std::lock_guard lock(*csr_mutex_);
		return {.materialized = static_cast<bool>(csr_cache_),
				.memory_bytes = csr_cache_ ? csr_cache_->estimatedMemoryBytes() : 0U};
	}

	std::shared_ptr<const LogicalProjectionCsr> LogicalGraphProjection::csr() const {
		std::lock_guard lock(*csr_mutex_);
		if (csr_cache_) {
			return csr_cache_;
		}
		auto result = std::make_shared<LogicalProjectionCsr>();
		result->node_ids = node_ids_;
		result->offsets.reserve(node_ids_.size() + 1U);
		result->target_node_ids.reserve(edge_count_);
		result->weights.reserve(edge_count_);
		result->relationship_ids.reserve(edge_count_);
		result->synthetic_reverse.reserve(edge_count_);
		for (const uint64_t nodeId: node_ids_) {
			result->offsets.push_back(result->target_node_ids.size());
			for (const LogicalProjectionEdge &edge: outgoing(nodeId)) {
				result->target_node_ids.push_back(edge.target_node_id);
				result->weights.push_back(edge.weight);
				result->relationship_ids.push_back(edge.relationship_id);
				result->synthetic_reverse.push_back(edge.synthetic_reverse ? uint8_t{1U} : uint8_t{0U});
			}
		}
		result->offsets.push_back(result->target_node_ids.size());
		if (result->edgeCount() != edge_count_) {
			throw std::logic_error("logical projection CSR disagrees with immutable adjacency");
		}
		csr_cache_ = std::move(result);
		return csr_cache_;
	}

} // namespace graph::query::gds
