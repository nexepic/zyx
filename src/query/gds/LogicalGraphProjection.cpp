/**
 * @file LogicalGraphProjection.cpp
 * @brief Storage-neutral immutable graph projection implementation.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/gds/LogicalGraphProjection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <variant>

namespace graph::query::gds {

	namespace {

		const std::vector<LogicalProjectionEdge> EMPTY_EDGES;

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

		[[nodiscard]] double resolveWeight(const storage::LogicalGraphEdge &edge, const LogicalProjectionWeight &spec) {
			switch (spec.kind) {
			case LogicalProjectionWeightKind::LPWK_NONE:
				return 1.0;
			case LogicalProjectionWeightKind::LPWK_CONSTANT:
				validateWeight(spec.constant_weight, "logical projection constant weight");
				return spec.constant_weight;
			case LogicalProjectionWeightKind::LPWK_PROPERTY:
				if (spec.property_name.empty()) {
					throw std::invalid_argument("logical projection weight property name must not be empty");
				}
				validateWeight(spec.default_weight, "logical projection default weight");
				for (const storage::LogicalGraphPropertyEntry &property: edge.properties) {
					if (property.key != spec.property_name || property.value.getType() == PropertyType::NULL_TYPE) {
						continue;
					}
					double weight = 0.0;
					if (property.value.getType() == PropertyType::INTEGER) {
						weight = static_cast<double>(std::get<int64_t>(property.value.getVariant()));
					} else if (property.value.getType() == PropertyType::DOUBLE) {
						weight = std::get<double>(property.value.getVariant());
					} else {
						throw std::invalid_argument("logical projection weight property must be numeric");
					}
					validateWeight(weight, "logical projection property weight");
					return weight;
				}
				return spec.default_weight;
			}
			throw std::logic_error("logical projection has an unknown weight kind");
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
			bounds.maximum_edges_per_node == 0U) {
			throw std::invalid_argument("logical projection requires positive Node, metadata, and Edge bounds");
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

		const storage::LogicalGraphNodePage page =
				snapshot.scanNodesAfter(0U, bounds.maximum_nodes, bounds.maximum_metadata_pages);
		if (page.nodes.size() > bounds.maximum_nodes || page.metadata_pages_visited > bounds.maximum_metadata_pages ||
			page.may_have_more) {
			throw std::runtime_error("logical projection exceeded its bounded Node scan");
		}
		std::unordered_set<uint64_t> selectedNodes;
		selectedNodes.reserve(page.nodes.size());
		uint64_t previousNodeId = 0U;
		for (const storage::LogicalGraphNode &node: page.nodes) {
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

		for (const uint64_t nodeId: projection.node_ids_) {
			// A cursor continuation is proof that the caller-supplied per-Node
			// budget would truncate the projection. The older visitor contract
			// cannot express that fact, so using it here could silently omit edges.
			const storage::LogicalGraphAdjacencyPage page = snapshot.scanAdjacentEdges(
					nodeId, storage::LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt,
					bounds.maximum_edges_per_node);
			if (page.edges.size() > bounds.maximum_edges_per_node || page.continuation.has_value()) {
				throw std::runtime_error("logical projection exceeded its bounded adjacency scan");
			}
			for (const storage::LogicalGraphEdge &edge: page.edges) {
				checkBuildCheckpoint(checkpoint);
				if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U ||
					edge.source_node_id != nodeId) {
					throw std::runtime_error("logical projection snapshot returned an invalid relationship");
				}
				if (!selectedNodes.contains(edge.source_node_id) || !selectedNodes.contains(edge.target_node_id)) {
					continue;
				}
				const auto type = byType.find(edge.type);
				const LogicalRelationshipProjection *relationship = type == byType.end() ? allTypes : type->second;
				if (relationship == nullptr) {
					continue;
				}
				LogicalGraphProjection::addRelationship(projection, edge, *relationship,
						resolveWeight(edge, relationship->weight));
			}
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
