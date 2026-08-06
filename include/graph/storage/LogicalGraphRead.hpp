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

	// A bounded adjacency page. A present continuation names the last returned
	// Edge and the exact next Edge to read. A backend may reuse a snapshot-local
	// cursor for this pair; an externally supplied or cache-missed pair is always
	// revalidated against the pinned logical Edge-ID order.
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

} // namespace graph::storage
