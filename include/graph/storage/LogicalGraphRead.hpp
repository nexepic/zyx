/**
 * @file LogicalGraphRead.hpp
 * @brief Format-independent, bounded logical graph read contract.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "graph/core/PropertyTypes.hpp"

namespace graph::storage {

	// Prefixes keep the public enum safe from Windows macro collisions.
	enum class LogicalGraphReadDirection : uint8_t {
		LGRD_OUTGOING = 1,
		LGRD_INCOMING = 2,
	};

	struct LogicalGraphPropertyEntry {
		std::string key;
		PropertyValue value;

		bool operator==(const LogicalGraphPropertyEntry &) const = default;
	};

	struct LogicalGraphNode {
		uint64_t node_id = 0;
		std::vector<std::string> labels;
		std::vector<LogicalGraphPropertyEntry> properties;

		LogicalGraphNode() = default;
		LogicalGraphNode(uint64_t nodeId, std::vector<std::string> nodeLabels,
						 std::vector<LogicalGraphPropertyEntry> ownerProperties = {}) :
			node_id(nodeId), labels(std::move(nodeLabels)), properties(std::move(ownerProperties)) {}

		bool operator==(const LogicalGraphNode &) const = default;
	};

	// A storage-neutral scalar Property interval. A NULL bound denotes an open
	// side; backends that cannot prove a complete bounded answer return nullopt.
	struct LogicalGraphPropertyRange {
		std::string key;
		PropertyValue min_value;
		PropertyValue max_value;
		bool min_inclusive = true;
		bool max_inclusive = true;

		bool operator==(const LogicalGraphPropertyRange &) const = default;
	};

	// A composite ordered-index request consists of an equality prefix followed
	// by one scalar interval. Backends return nullopt unless they can prove the
	// complete result from one pinned composite index under the supplied bounds.
	struct LogicalGraphCompositePropertyRange {
		std::vector<std::pair<std::string, PropertyValue>> equality_prefix;
		LogicalGraphPropertyRange range;

		bool operator==(const LogicalGraphCompositePropertyRange &) const = default;
	};

	struct LogicalGraphEdge {
		uint64_t edge_id = 0;
		uint64_t source_node_id = 0;
		uint64_t target_node_id = 0;
		// An empty type represents an untyped relationship (`()-->()`). It is
		// distinct from an absent relationship-type filter.
		std::string type;
		std::vector<LogicalGraphPropertyEntry> properties;

		LogicalGraphEdge() = default;
		LogicalGraphEdge(uint64_t edgeId, uint64_t sourceNodeId, uint64_t targetNodeId, std::string edgeType,
						 std::vector<LogicalGraphPropertyEntry> ownerProperties = {}) :
			edge_id(edgeId), source_node_id(sourceNodeId), target_node_id(targetNodeId), type(std::move(edgeType)),
			properties(std::move(ownerProperties)) {}

		bool operator==(const LogicalGraphEdge &) const = default;
	};

	// A topology-only Edge view deliberately excludes Properties. Read paths such
	// as an unfiltered relationship count need identity, endpoints, and type for
	// semantic validation, but must not force Property-page reads for every Edge.
	struct LogicalGraphEdgeTopology {
		uint64_t edge_id = 0;
		uint64_t source_node_id = 0;
		uint64_t target_node_id = 0;
		std::string type;

		LogicalGraphEdgeTopology() = default;
		LogicalGraphEdgeTopology(uint64_t edgeId, uint64_t sourceNodeId, uint64_t targetNodeId, std::string edgeType) :
			edge_id(edgeId), source_node_id(sourceNodeId), target_node_id(targetNodeId), type(std::move(edgeType)) {}

		bool operator==(const LogicalGraphEdgeTopology &) const = default;
	};

	// A bounded, resumable Node scan page. To continue, pass the last returned
	// Node ID as after_node_id. A full page may conservatively report more work
	// even when it ended at the final Node; the following bounded probe then
	// returns an empty page.
	struct LogicalGraphNodePage {
		std::vector<LogicalGraphNode> nodes;
		bool may_have_more = false;
		// Exact metadata-directory page reads performed to produce this page.
		// An empty terminal page may report zero when the backend has no Node
		// directory to inspect. Layered readers use this to enforce one total I/O
		// budget while filtering transaction-local state.
		size_t metadata_pages_visited = 0U;

		bool operator==(const LogicalGraphNodePage &) const = default;
	};

	// A resumable adjacency position is expressed only in stable logical Edge
	// IDs. The storage adapter validates the pair against its pinned snapshot
	// before resuming, so callers never retain a physical record location.
	struct LogicalGraphAdjacencyContinuation {
		uint64_t previous_edge_id = 0U;
		uint64_t next_edge_id = 0U;

		constexpr bool operator==(const LogicalGraphAdjacencyContinuation &) const = default;
	};

	// A bounded adjacency page. Edges must be in strictly increasing logical Edge
	// ID order. A present continuation names the last returned Edge and the exact
	// next Edge to read. A backend may reuse a snapshot-local cursor for this
	// pair; an externally supplied or cache-missed pair is always revalidated
	// against the pinned logical Edge-ID order.
	struct LogicalGraphAdjacencyPage {
		std::vector<LogicalGraphEdge> edges;
		std::optional<LogicalGraphAdjacencyContinuation> continuation;

		bool operator==(const LogicalGraphAdjacencyPage &) const = default;
	};

	// An opaque optimistic fence proves that the graph-visible roots of a logical
	// read snapshot have not changed. Callers can carry it into a mutation batch,
	// but cannot derive page locations, Segment IDs, or storage placement from it.
	// Metadata-only allocation may advance an implementation's publication
	// generation without invalidating this fence.
	struct LogicalGraphMutationFence {
		// Each graph-visible root is represented by its physical slot, frame, and
		// generation. Keep the storage width derived from that contract instead of
		// repeating a total that silently becomes stale when the root set evolves.
		static constexpr size_t GRAPH_STATE_ROOT_COUNT = 7U;
		static constexpr size_t GRAPH_STATE_TOKENS_PER_ROOT = 3U;
		static constexpr size_t GRAPH_STATE_TOKEN_COUNT = GRAPH_STATE_ROOT_COUNT * GRAPH_STATE_TOKENS_PER_ROOT;

		uint64_t backend_token = 0U;
		uint64_t format_token = 0U;
		std::array<uint64_t, GRAPH_STATE_TOKEN_COUNT> graph_state_tokens{};
		// A transaction overlay has no new physical root until commit, but may
		// still create multiple logical snapshots. This opaque local revision lets
		// derived in-memory views detect those staged graph changes without
		// exposing or fabricating physical page locations.
		uint64_t logical_revision = 0U;

		[[nodiscard]] constexpr bool isValid() const { return backend_token != 0U && format_token != 0U; }
		constexpr bool operator==(const LogicalGraphMutationFence &) const = default;
	};

	// A concrete snapshot pins one backend's immutable metadata generation. Every
	// streaming method takes a caller bound so a query adapter cannot accidentally
	// materialize an unbounded graph or relationship chain. A visitor returning
	// false must stop traversal before the next logical record is read.
	class LogicalGraphReadSnapshot {
	public:
		virtual ~LogicalGraphReadSnapshot() = default;

		[[nodiscard]] virtual LogicalGraphNode readNode(uint64_t nodeId) const = 0;
		// Probes one pinned snapshot without materializing a Node. A false result
		// is a proof of absence; malformed directory or entity state must still
		// throw rather than being misclassified as an application-level miss.
		[[nodiscard]] virtual bool containsNode(uint64_t /*nodeId*/) const {
			throw std::logic_error("logical graph snapshot does not support Node existence probes");
		}
		// `containsNode()` is optional because the original logical-read contract
		// required only complete Node reads. Callers that can benefit from a
		// cheaper existence probe must opt in explicitly and otherwise preserve
		// the complete-read fallback.
		[[nodiscard]] virtual bool supportsNodeExistenceProbes() const { return false; }
		[[nodiscard]] virtual LogicalGraphEdge readEdge(uint64_t edgeId) const = 0;
		// Returns a complete, Node-ID-sorted result only when the backend can
		// prove an exact label/property lookup without a graph scan (for example,
		// through a snapshot-pinned unique constraint). A nullopt result means the
		// caller must use its normal scan path. Implementations must preserve the
		// supplied result and Property bounds and never return a partial lookup.
		[[nodiscard]] virtual std::optional<std::vector<LogicalGraphNode>>
		findNodesByExactProperties(const std::vector<std::string> & /*labels*/,
								   const std::vector<std::pair<std::string, PropertyValue>> & /*properties*/,
								   size_t /*maximumNodes*/, size_t /*maximumPropertyEntries*/) const {
			return std::nullopt;
		}
		// Returns Node-ID-sorted results only when a backend can prove this complete
		// interval from a snapshot-pinned ordered index. nullopt is a fallback
		// signal, never authorization for an unbounded backend graph scan.
		[[nodiscard]] virtual std::optional<std::vector<LogicalGraphNode>>
		findNodesByPropertyRange(const std::vector<std::string> & /*labels*/,
								 const LogicalGraphPropertyRange & /*range*/, size_t /*maximumNodes*/,
								 size_t /*maximumPropertyEntries*/) const {
			return std::nullopt;
		}
		// A complete equality prefix plus range result must be Node-ID-sorted. The
		// default deliberately declines rather than asking a backend to scan its
		// graph while compiling or executing an index hint.
		[[nodiscard]] virtual std::optional<std::vector<LogicalGraphNode>>
		findNodesByCompositePropertyRange(const std::vector<std::string> & /*labels*/,
									  const LogicalGraphCompositePropertyRange & /*request*/, size_t /*maximumNodes*/,
									  size_t /*maximumPropertyEntries*/) const {
			return std::nullopt;
		}
		virtual void visitNodes(size_t maximumRecords,
								const std::function<bool(const LogicalGraphNode &)> &visitor) const = 0;
		// Reads at most maximum_records Nodes after after_node_id from the same
		// pinned snapshot. maximum_metadata_pages bounds metadata-directory I/O
		// independently of result cardinality, which is essential for sparse IDs.
		// The returned page reports the exact metadata-page cost, which must not
		// exceed maximum_metadata_pages.
		[[nodiscard]] virtual LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																  size_t maximumMetadataPages) const = 0;
		[[nodiscard]] virtual LogicalGraphAdjacencyPage
		scanAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
						  std::optional<LogicalGraphAdjacencyContinuation> continuation,
						  size_t maximumRecords) const = 0;
		virtual void visitEdges(size_t maximumRecords,
								const std::function<bool(const LogicalGraphEdge &)> &visitor) const = 0;
		// Implementations with a cheaper physical topology projection should
		// override this. The default preserves the contract for logical backends
		// that currently expose only complete Edge records.
		virtual void visitEdgeTopologies(
				size_t maximumRecords, const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const {
			if (!visitor) {
				throw std::invalid_argument("logical graph Edge topology scan requires a visitor");
			}
			visitEdges(maximumRecords, [&visitor](const LogicalGraphEdge &edge) {
				return visitor({edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type});
			});
		}
		// Adjacency topology scans are an optional optimization for queries that do
		// not inspect Edge properties. The default preserves compatibility by
		// projecting the complete adjacency records supplied by older backends.
		virtual void
		visitAdjacentEdgeTopologies(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
									const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const {
			if (!visitor) {
				throw std::invalid_argument("logical graph adjacency topology scan requires a visitor");
			}
			visitAdjacentEdges(nodeId, direction, maximumEdges, [&visitor](const LogicalGraphEdge &edge) {
				return visitor({edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type});
			});
		}
		virtual void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
										const std::function<bool(const LogicalGraphEdge &)> &visitor) const = 0;
		// Backends that also provide logical writes return an opaque generation
		// fence. Read-only-only adapters may return nullopt; callers then must not
		// attempt a read-dependent optimistic mutation through that adapter.
		[[nodiscard]] virtual std::optional<LogicalGraphMutationFence> mutationFence() const { return std::nullopt; }
	};

	// Storage backends opt in only after they can provide a snapshot-safe logical
	// read surface. This is intentionally separate from StorageBackend: lifecycle
	// capability must never imply query or transaction compatibility.
	class LogicalGraphReadBackend {
	public:
		virtual ~LogicalGraphReadBackend() = default;

		[[nodiscard]] virtual std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const = 0;
	};

	// The planning boundary receives only small snapshot-safe cardinalities. It
	// deliberately cannot request a graph scan, physical page location, or
	// mutable Catalog state while compiling a logical plan.
	struct LogicalGraphCardinalityStatistics {
		uint64_t node_count = 0U;
		uint64_t edge_count = 0U;

		bool operator==(const LogicalGraphCardinalityStatistics &) const = default;
	};

	class LogicalGraphStatisticsBackend {
	public:
		virtual ~LogicalGraphStatisticsBackend() = default;
		// `nullopt` is an explicit unavailable/legacy state, never permission for
		// a planning-time whole-graph fallback scan.
		[[nodiscard]] virtual std::optional<LogicalGraphCardinalityStatistics>
		readLogicalGraphCardinalityStatistics() const = 0;
	};

} // namespace graph::storage
