/**
 * @file LogicalGraphProjection.hpp
 * @brief Storage-neutral immutable graph projection built from a logical snapshot.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/storage/LogicalGraphRead.hpp"

namespace graph::query::gds {

	enum class LogicalProjectionOrientation : uint8_t {
		LPO_NATURAL,
		LPO_REVERSE,
		LPO_UNDIRECTED,
	};

	enum class LogicalProjectionWeightKind : uint8_t {
		LPWK_NONE,
		LPWK_CONSTANT,
		LPWK_PROPERTY,
	};

	struct LogicalProjectionWeight {
		LogicalProjectionWeightKind kind = LogicalProjectionWeightKind::LPWK_NONE;
		double constant_weight = 1.0;
		std::string property_name;
		double default_weight = 1.0;
	};

	struct LogicalRelationshipProjection {
		// An empty type accepts every relationship type, including untyped edges.
		std::string type;
		LogicalProjectionOrientation orientation = LogicalProjectionOrientation::LPO_NATURAL;
		LogicalProjectionWeight weight;
	};

	struct LogicalGraphProjectionSpec {
		std::string name;
		// An empty label collection accepts every Node.
		std::vector<std::string> node_labels;
		// An empty collection accepts every relationship with default_orientation.
		std::vector<LogicalRelationshipProjection> relationships;
		LogicalProjectionOrientation default_orientation = LogicalProjectionOrientation::LPO_NATURAL;
	};

	struct LogicalProjectionBuildBounds {
		size_t maximum_nodes = 0U;
		size_t maximum_metadata_pages = 0U;
		size_t maximum_edges_per_node = 0U;
	};

	// Projection construction may scan a large graph. The callback keeps its
	// cancellation boundary storage- and executor-neutral while allowing callers
	// to stop before any partially-built projection becomes catalog-visible.
	using LogicalProjectionBuildCheckpoint = std::function<void()>;

	struct LogicalProjectionEdge {
		uint64_t target_node_id = 0U;
		double weight = 1.0;
		uint64_t relationship_id = 0U;
		bool synthetic_reverse = false;

		bool operator==(const LogicalProjectionEdge &) const = default;
	};

	// Compact immutable adjacency used by native graph algorithms. Offsets are
	// indexed by node_ids, so scanning one Node's outgoing arcs is contiguous.
	// The source projection remains the authority for semantic construction;
	// this is a lazily cached derived representation, never persisted state.
	struct LogicalProjectionCsr {
		std::vector<uint64_t> node_ids;
		std::vector<size_t> offsets;
		std::vector<uint64_t> target_node_ids;
		std::vector<double> weights;
		std::vector<uint64_t> relationship_ids;
		std::vector<uint8_t> synthetic_reverse;

		[[nodiscard]] size_t edgeCount() const noexcept { return target_node_ids.size(); }
		[[nodiscard]] size_t estimatedMemoryBytes() const noexcept {
			return sizeof(*this) + node_ids.capacity() * sizeof(uint64_t) + offsets.capacity() * sizeof(size_t) +
					target_node_ids.capacity() * sizeof(uint64_t) + weights.capacity() * sizeof(double) +
					relationship_ids.capacity() * sizeof(uint64_t) + synthetic_reverse.capacity() * sizeof(uint8_t);
		}
	};

	struct LogicalProjectionCsrCacheState {
		bool materialized = false;
		size_t memory_bytes = 0U;

		bool operator==(const LogicalProjectionCsrCacheState &) const = default;
	};

	class LogicalGraphProjection final {
	public:
		[[nodiscard]] static LogicalGraphProjection build(const storage::LogicalGraphReadSnapshot &snapshot,
				const LogicalGraphProjectionSpec &spec, LogicalProjectionBuildBounds bounds,
				const LogicalProjectionBuildCheckpoint &checkpoint = {});

		[[nodiscard]] const std::vector<uint64_t> &nodeIds() const noexcept { return node_ids_; }
		[[nodiscard]] const std::vector<LogicalProjectionEdge> &outgoing(uint64_t nodeId) const noexcept;
		[[nodiscard]] const std::vector<LogicalProjectionEdge> &incoming(uint64_t nodeId) const noexcept;
		[[nodiscard]] size_t nodeCount() const noexcept { return node_ids_.size(); }
		[[nodiscard]] size_t edgeCount() const noexcept { return edge_count_; }
		[[nodiscard]] bool isWeighted() const noexcept { return is_weighted_; }
		[[nodiscard]] size_t estimatedMemoryBytes() const noexcept;
		// This observes the cache atomically without forcing CSR materialization.
		[[nodiscard]] LogicalProjectionCsrCacheState csrCacheState() const;
		[[nodiscard]] std::shared_ptr<const LogicalProjectionCsr> csr() const;

	private:
		static void addArc(LogicalGraphProjection &projection, uint64_t sourceNodeId, uint64_t targetNodeId,
						   double weight, uint64_t relationshipId, bool syntheticReverse);
		static void addRelationship(LogicalGraphProjection &projection, const storage::LogicalGraphEdge &edge,
								const LogicalRelationshipProjection &relationship, double weight);

		std::vector<uint64_t> node_ids_;
		std::unordered_map<uint64_t, std::vector<LogicalProjectionEdge>> outgoing_;
		std::unordered_map<uint64_t, std::vector<LogicalProjectionEdge>> incoming_;
		size_t edge_count_ = 0U;
		bool is_weighted_ = false;
		mutable std::shared_ptr<const LogicalProjectionCsr> csr_cache_;
		// A shared mutex object preserves value semantics for projections while
		// serializing first-use CSR construction across catalog readers.
		mutable std::shared_ptr<std::mutex> csr_mutex_ = std::make_shared<std::mutex>();
	};

} // namespace graph::query::gds
