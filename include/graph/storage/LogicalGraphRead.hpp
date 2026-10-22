/**
 * @file LogicalGraphRead.hpp
 * @brief Format-independent, bounded logical graph read contract.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once
#include <memory_resource>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/PropertyScalarReadSink.hpp"

namespace graph::query::execution::detail { class DatabaseResourceBudget; }

namespace graph::storage {
	// Only admission pressure may trigger source batching adaptation. Cancellation,
	// corruption and visitor failures must propagate without replay.
	class LogicalGraphBufferPressure final : public std::runtime_error {
	public:
		LogicalGraphBufferPressure() : std::runtime_error("shared materialization exceeded its byte bound") {}
	};


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

	// Internal read shape used by native runtimes to request only identity,
	// topology, labels, or selected Property tokens. Token IDs remain storage
	// local; public adapters resolve names only at the result boundary.
	struct EntityReadShape {
		bool include_labels = false;
		bool include_type = false;
		bool all_properties = false;
		std::vector<uint32_t> property_tokens;

		bool operator==(const EntityReadShape &) const = default;
	};

	// Property tokens are an ordered, duplicate-free request.  Keeping this
	// invariant at the storage-neutral boundary lets every native adapter use the
	// same merge plan and prevents an accidental O(columns^2) de-duplication in
	// hot readers.  `all_properties` may be combined with selected tokens for
	// callers that need both a complete result and a few early predicate fields.
	inline void validateEntityReadShape(const EntityReadShape &shape) {
		uint32_t previousToken = 0U;
		for (const uint32_t token: shape.property_tokens) {
			if (token == 0U || token <= previousToken) {
				throw std::invalid_argument("logical graph Entity read shape requires strictly increasing Property tokens");
			}
			previousToken = token;
		}
	}

	struct EntityRef {
		uint64_t entity_id = 0U;
		bool is_edge = false;
		uint64_t source_node_id = 0U;
		uint64_t target_node_id = 0U;
		uint64_t type_token = 0U;

		bool operator==(const EntityRef &) const = default;
	};

	struct IndexCandidateBatch {
		struct VerifiedPropertyColumn {
			std::string key;
			std::vector<PropertyValue> values;

			bool operator==(const VerifiedPropertyColumn &) const = default;
		};

		std::vector<EntityRef> candidates;
		uint64_t snapshot_generation = 0U;
		// A true value proves that the backend rechecked every requested exact
		// label and Property predicate in the same pinned snapshot. Runtime rows
		// may then remain EntityRef values until a later operator needs fields.
		bool exact_match_verified = false;
		// When true, the backend has verified the indexed predicate values from
		// authoritative Property records in the same snapshot. Entries are aligned
		// with candidates and contain only fields needed by the retained filter.
		bool predicates_verified = false;
		// Columnar storage avoids copying the same Property key once per candidate.
		// Each column has one value aligned with each candidate.
		std::vector<VerifiedPropertyColumn> verified_property_columns;
		// A native index candidate can optionally prove the requested label scope
		// while validating liveness.  Keeping this fact separate from predicate
		// verification lets the runtime avoid a second label-page pass without
		// weakening adapters that only verify indexed Property values.
		bool label_scope_verified = false;

		bool operator==(const IndexCandidateBatch &) const = default;
	};

	namespace detail {

		[[nodiscard]] inline size_t checkedLogicalGraphCandidateBytesAdd(size_t current, size_t extra) {
			if (extra > std::numeric_limits<size_t>::max() - current) {
				throw std::overflow_error("logical graph candidate byte accounting overflowed");
			}
			return current + extra;
		}

		[[nodiscard]] inline size_t checkedLogicalGraphCandidateBytesMul(size_t left, size_t right) {
			if (left != 0U && right > std::numeric_limits<size_t>::max() / left) {
				throw std::overflow_error("logical graph candidate byte accounting overflowed");
			}
			return left * right;
		}

		[[nodiscard]] inline size_t logicalGraphCandidateBatchBytes(const IndexCandidateBatch &batch) {
			size_t bytes = checkedLogicalGraphCandidateBytesMul(batch.candidates.size(), sizeof(EntityRef));
			for (const IndexCandidateBatch::VerifiedPropertyColumn &column: batch.verified_property_columns) {
				bytes = checkedLogicalGraphCandidateBytesAdd(bytes, column.key.size());
				bytes = checkedLogicalGraphCandidateBytesAdd(
						bytes, checkedLogicalGraphCandidateBytesMul(column.values.size(), sizeof(PropertyValue)));
				for (const PropertyValue &value: column.values) {
					bytes = checkedLogicalGraphCandidateBytesAdd(bytes, property_utils::getPropertyValueSize(value));
				}
			}
			return bytes;
		}

		inline void enforceLogicalGraphCandidateBatchBytes(
				const IndexCandidateBatch &batch, size_t maximumBytes) {
			if (maximumBytes == 0U) {
				throw std::invalid_argument("logical graph Node candidate stream requires a positive byte bound");
			}
			if (maximumBytes != std::numeric_limits<size_t>::max() &&
					logicalGraphCandidateBatchBytes(batch) > maximumBytes) {
				throw std::length_error("logical graph candidate stream exceeded its byte bound");
			}
		}

		[[nodiscard]] inline IndexCandidateBatch makeLogicalGraphFallbackCandidateBatch(
				const std::vector<LogicalGraphNode> &nodes) {
			IndexCandidateBatch batch;
			batch.candidates.reserve(nodes.size());
			uint64_t previousNodeId = 0U;
			for (const LogicalGraphNode &node: nodes) {
				if (node.node_id == 0U || node.node_id <= previousNodeId) {
					throw std::runtime_error(
							"logical graph fallback candidate stream returned non-monotonic Node IDs");
				}
				previousNodeId = node.node_id;
				batch.candidates.push_back({node.node_id, false, 0U, 0U, 0U});
			}
			return batch;
		}

	} // namespace detail

	// Candidate batches are delivered synchronously while the snapshot is pinned.
	// Returning false is an explicit early-termination signal; the producer must
	// stop before reading another index leaf or prefetching another batch. Native
	// streaming implementations order IDs within each batch; callers must not
	// infer global Entity-ID ordering across value-major index batches. Complete
	// candidate-result APIs retain their separate global ordering contract.
	using IndexCandidateBatchVisitor = std::function<bool(const IndexCandidateBatch &)>;

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

	// The topology form preserves the same continuation contract while avoiding
	// Property materialization for graph algorithms and type-only traversal.
	struct LogicalGraphAdjacencyTopologyPage {
		std::vector<LogicalGraphEdgeTopology> edges;
		std::optional<LogicalGraphAdjacencyContinuation> continuation;

		bool operator==(const LogicalGraphAdjacencyTopologyPage &) const = default;
	};

	// Native adjacency readers can retain endpoint and relationship-type tokens
	// without constructing a relationship type name for each frontier Edge. The
	// reference carries no physical address and remains valid only in the pinned
	// snapshot that produced the page.
	struct LogicalGraphAdjacencyReferencePage {
		std::vector<EntityRef> edges;
		std::optional<LogicalGraphAdjacencyContinuation> continuation;

		bool operator==(const LogicalGraphAdjacencyReferencePage &) const = default;
	};

	// An opaque optimistic fence proves that the graph-visible roots of a logical
	// read snapshot have not changed. Callers can carry it into a mutation batch,
	// but cannot derive page locations, Segment IDs, or storage placement from it.
	// Metadata-only allocation may advance an implementation's publication
	// generation without invalidating this fence.
	// Authenticated catalog observation for query-local property/schema binding.
	// Catalog tokens are opaque equality data, never dereferenceable locations.
	// This is conservative: catalog statistics and shared page-directory changes
	// can invalidate it too. It is not an independent persistent schema epoch.
	// Overlay view tokens distinguish unrelated transactions at equal revisions.
	struct LogicalGraphSchemaIdentity {
		uint64_t database_token = 0U;
		uint64_t format_token = 0U;
		std::array<uint64_t, 6U> catalog_tokens{};
		uint64_t view_token = 0U;
		uint64_t logical_revision = 0U;
		[[nodiscard]] constexpr bool isValid() const {
			return database_token != 0U && format_token != 0U && (view_token != 0U || logical_revision == 0U);
		}
		constexpr bool operator==(const LogicalGraphSchemaIdentity &) const = default;
	};

	struct LogicalGraphMutationFence {
		// Each graph-visible root is represented by its physical slot, frame, and
		// generation. Keep the storage width derived from that contract instead of
		// repeating a total that silently becomes stale when the root set evolves.
		static constexpr size_t GRAPH_STATE_ROOT_COUNT = 8U;
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
		// Shape-aware reads let execution operators declare exactly which logical
		// fields they need. Implementations that cannot resolve compact tokens may
		// conservatively fall back to a complete read; native readers override this
		// boundary to avoid decoding unrequested labels and Properties.
		[[nodiscard]] virtual LogicalGraphNode readNodeShaped(uint64_t nodeId, const EntityReadShape &shape) const {
			validateEntityReadShape(shape);
			return readNode(nodeId);
		}
		// Resolves a strictly increasing Node-ID batch through one storage-neutral
		// boundary. Native readers override this to merge Entity-Map work and
		// selected columns; the default keeps readers that expose only point reads
		// semantically compatible.
		[[nodiscard]] virtual std::vector<LogicalGraphNode>
		readNodesShapedSorted(std::span<const uint64_t> nodeIds, const EntityReadShape &shape) const {
			validateEntityReadShape(shape);
			std::vector<LogicalGraphNode> result;
			result.reserve(nodeIds.size());
			uint64_t previousNodeId = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (nodeId == 0U || nodeId <= previousNodeId) {
					throw std::invalid_argument(
							"logical graph shaped Node batch requires strictly increasing Node IDs");
				}
				previousNodeId = nodeId;
				LogicalGraphNode node = readNodeShaped(nodeId, shape);
				if (node.node_id != nodeId) {
					throw std::runtime_error("logical graph shaped Node batch returned a mismatched Node identity");
				}
				result.push_back(std::move(node));
			}
			return result;
		}
		[[nodiscard]] virtual EntityRef readNodeRef(uint64_t nodeId, const EntityReadShape &shape) const {
			validateEntityReadShape(shape);
			// A reference only needs logical identity.  Use the shaped boundary so a
			// backend that supports projection does not materialize labels or
			// Properties merely to produce an EntityRef.
			const LogicalGraphNode node = readNodeShaped(nodeId, EntityReadShape{});
			if (node.node_id != nodeId) {
				throw std::runtime_error("logical graph Node reference read changed logical identity");
			}
			return {node.node_id, false, 0U, 0U, 0U};
		}
		// Reads one named Property without requiring a complete entity materialization.
		// Backends that cannot resolve names to compact tokens retain a defensive
		// complete-read fallback.
		[[nodiscard]] virtual PropertyValue readNodeProperty(uint64_t nodeId, std::string_view key) const {
			const LogicalGraphNode node = readNodeShaped(nodeId, EntityReadShape{.all_properties = true});
			for (const LogicalGraphPropertyEntry &entry: node.properties) {
				if (entry.key == key) {
					return entry.value;
				}
			}
			return PropertyValue();
		}
		[[nodiscard]] virtual std::optional<bool> nodePropertyExists(uint64_t nodeId, std::string_view key) const {
			const LogicalGraphNode node = readNodeShaped(nodeId, EntityReadShape{.all_properties = true});
			return std::ranges::any_of(node.properties, [&](const LogicalGraphPropertyEntry &entry) { return entry.key == key; });
		}
		// Presence probes distinguish a missing field from stored NULL without
		// materializing unrelated Property values. Covers both Nodes and Edges.
		[[nodiscard]] virtual bool supportsPropertyPresenceProbes() const { return false; }
		// Allows consumers to omit COUNT-only Property payloads from prefetch.
		// Both Node and Edge non-null probes must be implemented when advertised.
		[[nodiscard]] virtual bool supportsPropertyNonNullProbes() const { return false; }
		// Optional COUNT predicate: false includes both missing and explicit NULL.
		// Key existence alone cannot prove this; unsupported readers return nullopt.
		[[nodiscard]] virtual std::optional<bool> nodePropertyIsNonNull(uint64_t, std::string_view) const {
			return std::nullopt;
		}

		// See readEdgePropertiesSorted(). Native Node pages override this to merge
		// owner locations and decode one requested Property column at a time.
		[[nodiscard]] virtual std::vector<PropertyValue>
		readNodePropertiesSorted(std::span<const uint64_t> nodeIds, std::string_view key) const {
			if (key.empty()) {
				throw std::invalid_argument("logical graph sorted Node Property read requires a non-empty key");
			}
			std::vector<PropertyValue> result;
			result.reserve(nodeIds.size());
			uint64_t previousNodeId = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (nodeId == 0U || nodeId <= previousNodeId) {
					throw std::invalid_argument(
							"logical graph sorted Node Property read requires strictly increasing Node IDs");
				}
				previousNodeId = nodeId;
				result.push_back(readNodeProperty(nodeId, key));
			}
			return result;
		}
		// Returns one column per requested key, each in the supplied strictly
		// increasing Node-ID order. Native readers override this to merge owner
		// locations and decode all selected tokens from one authenticated payload.
		[[nodiscard]] virtual std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSorted(std::span<const uint64_t> nodeIds,
												 std::span<const std::string_view> keys) const {
			std::vector<std::vector<PropertyValue>> columns;
			columns.reserve(keys.size());
			for (const std::string_view key: keys) {
				columns.push_back(readNodePropertiesSorted(nodeIds, key));
			}
			return columns;
		}
		// Optional synchronous cell producer. IDs are strictly increasing/nonzero;
		// ordinals refer to input IDs/keys, not physical page order. Omitted cells
		// are missing; an engaged NULL is stored NULL. No callback borrow escapes.
		// A failed production must be discarded before any consumer effects.
		using PropertyCellSink = std::function<void(size_t, size_t, std::optional<PropertyValue>)>;
		[[nodiscard]] virtual bool supportsNodePropertyCellBatches() const { return false; }
		virtual void readNodePropertyCellsSortedWithAdmission(std::span<const uint64_t>,
			std::span<const std::string_view>, const PropertyCellSink &,
			const std::function<void(size_t)> &) const {
			throw std::logic_error("logical graph reader does not support direct Property cells");
		}
        // Native scalar producers may bypass PropertyValue. Existing readers
        // and overlays retain their owning-cell route through this fallback.
        virtual void readNodePropertyCellsWithScalarsSortedWithAdmission(std::span<const uint64_t> ids,
            std::span<const std::string_view> keys, const PropertyCellSink &values,
            const PropertyScalarReadSink &scalars, const std::function<void(size_t)> &admission) const {
            if (!values || !scalars || !admission) throw std::invalid_argument("direct scalar cells require sinks and admission");
            readNodePropertyCellsSortedWithAdmission(ids, keys, values, admission);
        }
        virtual void readNodePropertyCellsWithViewsSortedWithAdmission(std::span<const uint64_t> ids,
            std::span<const std::string_view> keys, const PropertyCellSink &values,
            const PropertyScalarReadSink &scalars, const PropertyTextReadSink &texts,
            const std::function<void(size_t)> &admission) const {
            if (!texts) throw std::invalid_argument("direct text cells require a sink");
            readNodePropertyCellsWithScalarsSortedWithAdmission(ids, keys, values, scalars, admission);
        }
		// Readers advertising admission invoke it before selected entry payload
		// reads/copies. Native storage reports canonical token + value wire bytes;
		// these are source scratch estimates, not decoded object capacity or RSS.
		[[nodiscard]] virtual bool supportsPropertyReadAdmission() const { return false; }
		[[nodiscard]] virtual std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSortedWithAdmission(std::span<const uint64_t>,
				std::span<const std::string_view>, const std::function<void(size_t)> &) const {
			throw std::logic_error("logical graph reader does not support Property admission");
		}
		[[nodiscard]] virtual std::vector<std::vector<PropertyValue>>
		readEdgePropertyColumnsSortedWithAdmission(std::span<const uint64_t>,
				std::span<const std::string_view>, const std::function<void(size_t)> &) const {
			throw std::logic_error("logical graph reader does not support Property admission");
		}
		// Shaped admission covers canonical token + value bytes before Property
		// decoding/copying. Names, labels, encoded input and container capacities
		// require separate accounting; selected-column support does not imply this.
		[[nodiscard]] virtual bool supportsShapedPropertyReadAdmission() const { return false; }
		[[nodiscard]] virtual std::vector<LogicalGraphNode>
		readNodesShapedSortedWithAdmission(std::span<const uint64_t>, const EntityReadShape &,
				const std::function<void(size_t)> &) const {
			throw std::logic_error("logical graph reader does not support shaped Property admission");
		}
		[[nodiscard]] virtual std::vector<LogicalGraphEdge>
		readEdgesShapedSortedWithAdmission(std::span<const uint64_t>, const EntityReadShape &,
				const std::function<void(size_t)> &) const {
			throw std::logic_error("logical graph reader does not support shaped Property admission");
		}
		// Tests required labels without requiring Property or result-object
		// materialization. Native backends resolve names to snapshot-local tokens;
		// the default retains exact logical semantics for other implementations.
		[[nodiscard]] virtual bool nodeHasLabels(uint64_t nodeId,
															 const std::vector<std::string> &requiredLabels) const {
			const LogicalGraphNode node = readNodeShaped(nodeId, EntityReadShape{.include_labels = true});
			for (const std::string &required: requiredLabels) {
				if (required.empty() || std::find(node.labels.begin(), node.labels.end(), required) == node.labels.end()) {
					return false;
				}
			}
			return true;
		}
		// Tests one fixed label predicate for strictly increasing Node IDs. Native
		// readers override this to resolve the predicate once and share ordered
		// Entity-page work; the fallback preserves the point-read contract.
		[[nodiscard]] virtual std::vector<bool> readNodeLabelMatchesSorted(
				std::span<const uint64_t> nodeIds, const std::vector<std::string> &requiredLabels) const {
			std::vector<bool> result;
			result.reserve(nodeIds.size());
			uint64_t previousNodeId = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (nodeId == 0U || nodeId <= previousNodeId) {
					throw std::invalid_argument(
							"logical graph sorted Node label read requires strictly increasing Node IDs");
				}
				previousNodeId = nodeId;
				result.push_back(nodeHasLabels(nodeId, requiredLabels));
			}
			return result;
		}
		// Probes one pinned snapshot without materializing a Node. A false result
		// is a proof of absence; malformed directory or entity state must still
		// throw rather than being misclassified as an application-level miss.
		[[nodiscard]] virtual bool containsNode(uint64_t /*nodeId*/) const {
			throw std::logic_error("logical graph snapshot does not support Node existence probes");
		}
		// Batch form for topology consumers that already have a sorted, unique
		// endpoint set. Native readers resolve the Entity Directory once for the
		// missing IDs; the default preserves semantics for compatible readers.
		[[nodiscard]] virtual std::vector<bool>
		containsNodesSorted(std::span<const uint64_t> nodeIds) const {
			std::vector<bool> result(nodeIds.size(), false);
			uint64_t previousNodeId = 0U;
			for (size_t index = 0U; index < nodeIds.size(); ++index) {
				const uint64_t nodeId = nodeIds[index];
				if (nodeId == 0U || nodeId <= previousNodeId) {
					throw std::invalid_argument(
							"logical graph sorted Node existence read requires strictly increasing Node IDs");
				}
				previousNodeId = nodeId;
				result[index] = containsNode(nodeId);
			}
			return result;
		}
		// `containsNode()` is optional because the original logical-read contract
		// required only complete Node reads. Callers that can benefit from a
		// cheaper existence probe must opt in explicitly and otherwise preserve
		// the complete-read fallback.
		[[nodiscard]] virtual bool supportsNodeExistenceProbes() const { return false; }
		[[nodiscard]] virtual LogicalGraphEdge readEdge(uint64_t edgeId) const = 0;
		[[nodiscard]] virtual LogicalGraphEdge readEdgeShaped(uint64_t edgeId, const EntityReadShape &shape) const {
			validateEntityReadShape(shape);
			return readEdge(edgeId);
		}
		// See readNodesShapedSorted(). The batch contract deliberately exposes only
		// logical IDs and records, never page or slot locations.
		[[nodiscard]] virtual std::vector<LogicalGraphEdge>
		readEdgesShapedSorted(std::span<const uint64_t> edgeIds, const EntityReadShape &shape) const {
			validateEntityReadShape(shape);
			std::vector<LogicalGraphEdge> result;
			result.reserve(edgeIds.size());
			uint64_t previousEdgeId = 0U;
			for (const uint64_t edgeId: edgeIds) {
				if (edgeId == 0U || edgeId <= previousEdgeId) {
					throw std::invalid_argument(
							"logical graph shaped Edge batch requires strictly increasing Edge IDs");
				}
				previousEdgeId = edgeId;
				LogicalGraphEdge edge = readEdgeShaped(edgeId, shape);
				if (edge.edge_id != edgeId) {
					throw std::runtime_error("logical graph shaped Edge batch returned a mismatched Edge identity");
				}
				result.push_back(std::move(edge));
			}
			return result;
		}
		[[nodiscard]] virtual EntityRef readEdgeRef(uint64_t edgeId, const EntityReadShape &shape) const {
			validateEntityReadShape(shape);
			// EntityRef carries topology, not arbitrary Properties. Keep the default
			// fallback projection-aware even when the caller's shape contains extra
			// fields that cannot be represented by the reference itself.
			const LogicalGraphEdge edge = readEdgeShaped(edgeId, EntityReadShape{});
			if (edge.edge_id != edgeId) {
				throw std::runtime_error("logical graph Edge reference read changed logical identity");
			}
			return {edge.edge_id, true, edge.source_node_id, edge.target_node_id, 0U};
		}
		[[nodiscard]] virtual PropertyValue readEdgeProperty(uint64_t edgeId, std::string_view key) const {
			const LogicalGraphEdge edge = readEdgeShaped(edgeId, EntityReadShape{.include_type = false, .all_properties = true});
			for (const LogicalGraphPropertyEntry &entry: edge.properties) {
				if (entry.key == key) {
					return entry.value;
				}
			}
			return PropertyValue();
		}
		[[nodiscard]] virtual std::optional<bool> edgePropertyExists(uint64_t edgeId, std::string_view key) const {
			const LogicalGraphEdge edge = readEdgeShaped(edgeId, EntityReadShape{.all_properties = true});
			return std::ranges::any_of(edge.properties, [&](const LogicalGraphPropertyEntry &entry) { return entry.key == key; });
		}
		// Optional COUNT predicate: false includes both missing and explicit NULL.
		// Key existence alone cannot prove this; unsupported readers return nullopt.
		[[nodiscard]] virtual std::optional<bool> edgePropertyIsNonNull(uint64_t, std::string_view) const {
			return std::nullopt;
		}

		// Reads one named Property for strictly increasing Edge IDs. Implementations
		// with token-aware pages override this to merge the Property Directory and
		// pin each shared page once. The default preserves the public contract for
		// logical readers that only expose point reads.
		[[nodiscard]] virtual std::vector<PropertyValue>
		readEdgePropertiesSorted(std::span<const uint64_t> edgeIds, std::string_view key) const {
			if (key.empty()) {
				throw std::invalid_argument("logical graph sorted Edge Property read requires a non-empty key");
			}
			std::vector<PropertyValue> result;
			result.reserve(edgeIds.size());
			uint64_t previousEdgeId = 0U;
			for (const uint64_t edgeId: edgeIds) {
				if (edgeId == 0U || edgeId <= previousEdgeId) {
					throw std::invalid_argument(
							"logical graph sorted Edge Property read requires strictly increasing Edge IDs");
				}
				previousEdgeId = edgeId;
				result.push_back(readEdgeProperty(edgeId, key));
			}
			return result;
		}
		// See readNodePropertyColumnsSorted(). The default retains compatibility for
		// readers that expose only one selected Property at a time.
		[[nodiscard]] virtual std::vector<std::vector<PropertyValue>>
		readEdgePropertyColumnsSorted(std::span<const uint64_t> edgeIds,
												 std::span<const std::string_view> keys) const {
			std::vector<std::vector<PropertyValue>> columns;
			columns.reserve(keys.size());
			for (const std::string_view key: keys) {
				columns.push_back(readEdgePropertiesSorted(edgeIds, key));
			}
			return columns;
		}
		// Index acceleration may return candidates without claiming that covered
		// predicates are complete. The default adapts existing complete lookups,
		// but deliberately leaves verification false: only a native backend that
		// validated candidates and their snapshot provenance may authorize runtime
		// deferred materialization.
		[[nodiscard]] virtual std::optional<IndexCandidateBatch> findNodeCandidatesByExactProperties(
				const std::vector<std::string> &labels,
				const std::vector<std::pair<std::string, PropertyValue>> &properties,
				size_t maximumNodes, size_t maximumPropertyEntries) const {
			const auto nodes = findNodesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries);
			if (!nodes.has_value()) {
				return std::nullopt;
			}
			return detail::makeLogicalGraphFallbackCandidateBatch(*nodes);
		}
		// Candidate verification is a stronger native contract than the complete
		// lookup API. The default adapter must retain the latter's defensive row
		// validation, so only a backend that explicitly opts in may bypass it.
		[[nodiscard]] virtual bool supportsNativeExactNodeCandidates() const { return false; }
		// Streaming form of the exact candidate contract. A nullopt result means
		// that this snapshot cannot provide the requested index path; an engaged
		// bool reports whether the visitor consumed the complete stream.
		[[nodiscard]] virtual std::optional<bool> visitNodeCandidatesByExactProperties(
				const std::vector<std::string> &labels,
				const std::vector<std::pair<std::string, PropertyValue>> &properties,
				size_t maximumNodes, size_t maximumPropertyEntries,
				const IndexCandidateBatchVisitor &visitor) const {
			if (!visitor) {
				throw std::invalid_argument("logical graph indexed Node candidate stream requires a visitor");
			}
			const auto batch = findNodeCandidatesByExactProperties(labels, properties, maximumNodes,
					maximumPropertyEntries);
			if (!batch.has_value()) {
				return std::nullopt;
			}
			return visitor(*batch);
		}
		// Byte-bounded streaming form. Readers that do not provide a native
		// producer-side implementation retain their existing visitor dispatch while
		// this boundary enforces the emitted candidate batch size.
		[[nodiscard]] virtual std::optional<bool> visitNodeCandidatesByExactProperties(
				const std::vector<std::string> &labels,
				const std::vector<std::pair<std::string, PropertyValue>> &properties,
				size_t maximumNodes, size_t maximumPropertyEntries, size_t maximumCandidateBytes,
				const IndexCandidateBatchVisitor &visitor) const {
			if (!visitor) {
				throw std::invalid_argument("logical graph indexed Node candidate stream requires a visitor");
			}
			const auto guardedVisitor = [&](const IndexCandidateBatch &batch) {
				detail::enforceLogicalGraphCandidateBatchBytes(batch, maximumCandidateBytes);
				return visitor(batch);
			};
			return visitNodeCandidatesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries,
					guardedVisitor);
		}
		// Range candidates use the same snapshot/generation contract as exact
		// candidates, but do not claim uniqueness. The default adapts the complete
		// range API and deliberately leaves verification disabled.
		[[nodiscard]] virtual std::optional<IndexCandidateBatch> findNodeCandidatesByPropertyRange(
				const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
				size_t maximumNodes, size_t maximumPropertyEntries) const {
			const auto nodes = findNodesByPropertyRange(labels, range, maximumNodes, maximumPropertyEntries);
			if (!nodes.has_value()) {
				return std::nullopt;
			}
			return detail::makeLogicalGraphFallbackCandidateBatch(*nodes);
		}
		[[nodiscard]] virtual std::optional<IndexCandidateBatch> findNodeCandidatesByCompositePropertyRange(
				const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
				size_t maximumNodes, size_t maximumPropertyEntries) const {
			const auto nodes = findNodesByCompositePropertyRange(labels, request, maximumNodes, maximumPropertyEntries);
			if (!nodes.has_value()) {
				return std::nullopt;
			}
			return detail::makeLogicalGraphFallbackCandidateBatch(*nodes);
		}
		[[nodiscard]] virtual bool supportsNativeRangeNodeCandidates() const { return false; }
		[[nodiscard]] virtual std::optional<bool> visitNodeCandidatesByPropertyRange(
				const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
				size_t maximumNodes, size_t maximumPropertyEntries,
				const IndexCandidateBatchVisitor &visitor) const {
			if (!visitor) {
				throw std::invalid_argument("logical graph range Node candidate stream requires a visitor");
			}
			const auto batch = findNodeCandidatesByPropertyRange(labels, range, maximumNodes,
					maximumPropertyEntries);
			if (!batch.has_value()) {
				return std::nullopt;
			}
			return visitor(*batch);
		}
		[[nodiscard]] virtual std::optional<bool> visitNodeCandidatesByPropertyRange(
				const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
				size_t maximumNodes, size_t maximumPropertyEntries, size_t maximumCandidateBytes,
				const IndexCandidateBatchVisitor &visitor) const {
			if (!visitor) {
				throw std::invalid_argument("logical graph range Node candidate stream requires a visitor");
			}
			const auto guardedVisitor = [&](const IndexCandidateBatch &batch) {
				detail::enforceLogicalGraphCandidateBatchBytes(batch, maximumCandidateBytes);
				return visitor(batch);
			};
			return visitNodeCandidatesByPropertyRange(labels, range, maximumNodes, maximumPropertyEntries,
					guardedVisitor);
		}
		[[nodiscard]] virtual std::optional<bool> visitNodeCandidatesByCompositePropertyRange(
				const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
				size_t maximumNodes, size_t maximumPropertyEntries,
				const IndexCandidateBatchVisitor &visitor) const {
			if (!visitor) {
				throw std::invalid_argument("logical graph composite range Node candidate stream requires a visitor");
			}
			const auto batch = findNodeCandidatesByCompositePropertyRange(labels, request, maximumNodes,
					maximumPropertyEntries);
			if (!batch.has_value()) {
				return std::nullopt;
			}
			return visitor(*batch);
		}
		[[nodiscard]] virtual std::optional<bool> visitNodeCandidatesByCompositePropertyRange(
				const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
				size_t maximumNodes, size_t maximumPropertyEntries, size_t maximumCandidateBytes,
				const IndexCandidateBatchVisitor &visitor) const {
			if (!visitor) {
				throw std::invalid_argument("logical graph composite range Node candidate stream requires a visitor");
			}
			const auto guardedVisitor = [&](const IndexCandidateBatch &batch) {
				detail::enforceLogicalGraphCandidateBatchBytes(batch, maximumCandidateBytes);
				return visitor(batch);
			};
			return visitNodeCandidatesByCompositePropertyRange(labels, request, maximumNodes, maximumPropertyEntries,
					guardedVisitor);
		}
		// Native candidate batches are valid only for the pinned snapshot that
		// produced them. Generic readers need not expose a durable generation.
		[[nodiscard]] virtual std::optional<uint64_t> nativeSnapshotGeneration() const { return std::nullopt; }
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
		// A projection-aware scan may yield stable logical references without
		// decoding labels or Properties. The default keeps every existing backend
		// compatible by projecting complete Node records.
		virtual void visitNodeRefs(size_t maximumRecords, const std::function<bool(const EntityRef &)> &visitor) const {
			if (!visitor) {
				throw std::invalid_argument("logical graph Node reference scan requires a visitor");
			}
			visitNodes(maximumRecords, [&visitor](const LogicalGraphNode &node) {
				if (node.node_id == 0U) {
					throw std::runtime_error("logical graph Node reference scan returned an invalid Node identity");
				}
				return visitor({node.node_id, false, 0U, 0U, 0U});
			});
		}
		// Native readers test resolved label tokens while their Entity page is
		// already pinned, but still report every scanned reference. That preserves
		// runtime resource accounting while avoiding a second Entity-Map lookup to
		// re-evaluate labels. The fallback keeps the original logical contract.
		virtual void visitNodeRefsWithLabelMatches(size_t maximumRecords,
													 const std::vector<std::string> &requiredLabels,
													 const std::function<bool(const EntityRef &, bool)> &visitor) const {
			if (!visitor) {
				throw std::invalid_argument("logical graph Node reference label scan requires a visitor");
			}
			visitNodeRefs(maximumRecords, [this, &requiredLabels, &visitor](const EntityRef &reference) {
				if (reference.is_edge) {
					throw std::runtime_error("logical graph Node reference scan returned an Edge reference");
				}
				return visitor(reference, nodeHasLabels(reference.entity_id, requiredLabels));
			});
		}
		// Predicate access may enumerate only an indexed label's candidates. Each
		// emitted reference is validated against primary records; false denotes
		// a candidate rejected by another required label. Bounds count candidates
		// examined, not the population skipped by an index. The fallback remains
		// an exhaustive scan; overlays use it until they can merge memberships.
		virtual void visitNodeRefsForLabelPredicate(size_t maximumRecords,
				const std::vector<std::string> &requiredLabels,
				const std::function<bool(const EntityRef &, bool)> &visitor) const {
			auto canonical = requiredLabels;
			std::sort(canonical.begin(), canonical.end());
			canonical.erase(std::unique(canonical.begin(), canonical.end()), canonical.end());
			visitNodeRefsWithLabelMatches(maximumRecords, canonical, visitor);
		}
		// The default reference projection is semantically compatible but may not
		// preserve a backend's stronger resumable-page validation contract. Query
		// execution uses it only after this explicit capability opt-in.
		[[nodiscard]] virtual bool supportsNativeNodeRefScans() const { return false; }
		// Reads at most maximum_records Nodes after after_node_id from the same
		// pinned snapshot. maximum_metadata_pages bounds metadata-directory I/O
		// independently of result cardinality, which is essential for sparse IDs.
		// The returned page reports the exact metadata-page cost, which must not
		// exceed maximum_metadata_pages.
		[[nodiscard]] virtual LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																								  size_t maximumMetadataPages) const = 0;
		// Shape-aware resumable Node scan. The default delegates to the complete
		// scan so existing adapters remain compatible; native readers override this
		// boundary to avoid decoding labels or Properties that the caller did not
		// request. Identity and continuation semantics are unchanged.
		[[nodiscard]] virtual LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																								  size_t maximumMetadataPages,
																								  const EntityReadShape &shape) const {
			validateEntityReadShape(shape);
			LogicalGraphNodePage result = scanNodesAfter(afterNodeId, maximumRecords, maximumMetadataPages);
			if (!shape.include_labels) {
				for (LogicalGraphNode &node: result.nodes) {
					node.labels.clear();
				}
			}
			// A generic adapter cannot map compact property tokens back to logical
			// names without its schema resolver. Retain complete Properties for that
			// uncommon fallback; identity/label-only scans can still drop them.
			if (!shape.all_properties && shape.property_tokens.empty()) {
				for (LogicalGraphNode &node: result.nodes) {
					node.properties.clear();
				}
			}
			return result;
		}
		[[nodiscard]] virtual LogicalGraphAdjacencyPage
		scanAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
						  std::optional<LogicalGraphAdjacencyContinuation> continuation,
						  size_t maximumRecords) const = 0;
		// Additive Property payload admission before decoding complete adjacency roots.
		// Cursor, Entity, resolver and output-buffer allocations are separate.
		[[nodiscard]] virtual bool supportsAdjacencyPropertyReadAdmission() const { return false; }
		[[nodiscard]] virtual LogicalGraphAdjacencyPage scanAdjacentEdgesWithPropertyAdmission(
				uint64_t, LogicalGraphReadDirection, std::optional<LogicalGraphAdjacencyContinuation>, size_t,
				const std::function<void(size_t)> &) const {
			throw std::logic_error("logical snapshot does not support adjacency Property admission");
		}
		// Independent additive payload and absolute live array-buffer admission.
		// Resolver names, Property containers and session caches remain separate.
		[[nodiscard]] virtual bool supportsAdjacencyReadAdmission() const { return false; }
		[[nodiscard]] virtual LogicalGraphAdjacencyPage scanAdjacentEdgesWithAdmission(
			uint64_t, LogicalGraphReadDirection, std::optional<LogicalGraphAdjacencyContinuation>, size_t,
			const std::function<void(size_t)> &, const std::function<void(size_t)> &) const {
			throw std::logic_error("logical snapshot does not support adjacency read admission");
		}
		// Buffer admission covers ID/record/output arrays and retained type names.
		// Fixed cursor pages, session caches and resolver/Entity internals are separate.
		[[nodiscard]] virtual bool supportsAdjacencyTopologyBufferAdmission() const { return false; }
		[[nodiscard]] virtual LogicalGraphAdjacencyTopologyPage
		scanAdjacentEdgeTopologiesWithBufferAdmission(uint64_t, LogicalGraphReadDirection,
			std::optional<LogicalGraphAdjacencyContinuation>, size_t,
			const std::function<void(size_t)> &) const {
			throw std::logic_error("logical snapshot does not support topology buffer admission");
		}
		// Backends with a native topology reader override this paging API. The
		// fallback retains compatibility while making the expensive projection
		// explicit at the storage-neutral boundary.
		[[nodiscard]] virtual LogicalGraphAdjacencyTopologyPage
		scanAdjacentEdgeTopologies(uint64_t nodeId, LogicalGraphReadDirection direction,
									 std::optional<LogicalGraphAdjacencyContinuation> continuation,
									 size_t maximumRecords) const {
			const LogicalGraphAdjacencyPage full =
					scanAdjacentEdges(nodeId, direction, std::move(continuation), maximumRecords);
			LogicalGraphAdjacencyTopologyPage result;
			result.edges.reserve(full.edges.size());
			for (const LogicalGraphEdge &edge: full.edges) {
				result.edges.push_back({edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type});
			}
			result.continuation = full.continuation;
			return result;
		}
		// A native reference page preserves compact relationship type tokens for
		// algorithms that already have a snapshot-local type token. The fallback
		// intentionally projects complete Edges and is usable only when a backend
		// has not opted into the stronger capability below.
		[[nodiscard]] virtual LogicalGraphAdjacencyReferencePage
		scanAdjacentEdgeRefs(uint64_t nodeId, LogicalGraphReadDirection direction,
						 std::optional<LogicalGraphAdjacencyContinuation> continuation,
						 size_t maximumRecords) const {
			const LogicalGraphAdjacencyPage full =
					scanAdjacentEdges(nodeId, direction, std::move(continuation), maximumRecords);
			LogicalGraphAdjacencyReferencePage result;
			result.edges.reserve(full.edges.size());
			for (const LogicalGraphEdge &edge: full.edges) {
				result.edges.push_back({edge.edge_id, true, edge.source_node_id, edge.target_node_id, 0U});
			}
			result.continuation = full.continuation;
			return result;
		}
		// Absolute bytes retained by ID/record/reference arrays during this read.
		// Fixed cursor pages and session caches remain separately bounded.
		[[nodiscard]] virtual bool supportsAdjacencyReferenceBufferAdmission() const { return false; }
		[[nodiscard]] virtual LogicalGraphAdjacencyReferencePage
		scanAdjacentEdgeRefsWithBufferAdmission(uint64_t, LogicalGraphReadDirection,
			std::optional<LogicalGraphAdjacencyContinuation>, size_t,
			const std::function<void(size_t)> &) const {
			throw std::logic_error("logical snapshot does not support reference buffer admission");
		}
		[[nodiscard]] virtual bool supportsNativeAdjacentEdgeRefScans() const { return false; }
		// A paged topology cursor is stronger than the visitor-only capability:
		// callers may request one bounded page, validate its continuation, and
		// batch endpoint/label work before visiting the next page.
		[[nodiscard]] virtual bool supportsNativeAdjacentEdgeTopologyPages() const { return false; }
		virtual void visitEdges(size_t maximumRecords,
								const std::function<bool(const LogicalGraphEdge &)> &visitor) const = 0;
		// See visitNodeRefs(). Native readers override this to retain edge topology
		// while deferring relationship type and Property name materialization.
		virtual void visitEdgeRefs(size_t maximumRecords, const std::function<bool(const EntityRef &)> &visitor) const {
			if (!visitor) {
				throw std::invalid_argument("logical graph Edge reference scan requires a visitor");
			}
			visitEdges(maximumRecords, [&visitor](const LogicalGraphEdge &edge) {
				if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
					throw std::runtime_error("logical graph Edge reference scan returned invalid topology");
				}
				return visitor({edge.edge_id, true, edge.source_node_id, edge.target_node_id, 0U});
			});
		}
		// A resolved, snapshot-local type token is an access hint. An indexed
		// backend may omit unrelated types; callers still validate/filter refs.
		// Fallbacks and overlays retain exhaustive scans until membership merging
		// is supported. Token zero remains the untyped relationship identity.
		virtual void visitEdgeRefsForTypePredicate(size_t maximumRecords,
				std::optional<uint64_t> typeToken,
				const std::function<bool(const EntityRef &)> &visitor) const {
			static_cast<void>(typeToken);
			visitEdgeRefs(maximumRecords, visitor);
		}
		[[nodiscard]] virtual bool supportsNativeEdgeRefScans() const { return false; }
		// A native snapshot may resolve a relationship-type name once into its
		// snapshot-local opaque token. The token is logical schema identity, not a
		// physical location; it can be compared only with EntityRef::type_token
		// from the same snapshot. Returning false preserves the string-topology
		// fallback for storage implementations without compact type tokens.
		[[nodiscard]] virtual bool supportsNativeRelationshipTypeTokens() const { return false; }
		// Some transactional readers can expose native tokens for types already
		// present in the pinned catalog while falling back for newly staged types.
		// This query-specific capability preserves the native path for known types
		// without claiming that every relationship reference has a token.
		[[nodiscard]] virtual bool supportsNativeRelationshipTypeTokens(std::string_view type) const {
			static_cast<void>(type);
			return supportsNativeRelationshipTypeTokens();
		}
		[[nodiscard]] virtual std::optional<uint64_t>
		findRelationshipTypeToken(std::string_view /*name*/) const {
			return std::nullopt;
		}
		// Resolves a Property name once in the pinned schema. The token is useful
		// when constructing an EntityReadShape for a selected-column read.
		// A resolving backend can exclude names with no token from token-selected
		// shapes. Staged names may have values before acquiring a catalog token;
		// lack of a token must not be interpreted as logical Property absence.
		[[nodiscard]] virtual bool supportsPropertyTokenResolution() const { return false; }
		[[nodiscard]] virtual std::optional<uint32_t> findPropertyToken(std::string_view /*name*/) const {
			return std::nullopt;
		}
		// Implementations with a cheaper physical topology projection should
		// override this. The default preserves the contract for logical backends
		// that currently expose only complete Edge records.
		virtual void visitEdgeTopologies(
				size_t maximumRecords, const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const {
			if (!visitor) {
				throw std::invalid_argument("logical graph Edge topology scan requires a visitor");
			}
			visitEdges(maximumRecords, [&visitor](const LogicalGraphEdge &edge) {
				if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
					throw std::runtime_error("logical graph Edge topology scan returned invalid topology");
				}
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
			visitAdjacentEdges(nodeId, direction, maximumEdges, [&visitor, nodeId, direction](const LogicalGraphEdge &edge) {
				if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
					throw std::runtime_error("logical graph adjacency topology scan returned invalid topology");
				}
				const bool endpointMatches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
						? edge.source_node_id == nodeId : edge.target_node_id == nodeId;
				if (!endpointMatches) {
					throw std::runtime_error("logical graph adjacency topology scan returned an Edge for the wrong Node");
				}
				return visitor({edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type});
			});
		}
		virtual void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
										const std::function<bool(const LogicalGraphEdge &)> &visitor) const = 0;
		// Batch adjacency traversal for many source IDs. Native implementations may
		// consume one ordered adjacency cursor; the fallback preserves semantics by
		// using the existing per-source cursor.
		// Absolute live source-buffer bytes, including buffers retained while invoking visitor.
		[[nodiscard]] virtual bool supportsBatchAdjacencyReferenceBufferAdmission() const { return false; }
		virtual void visitAdjacentEdgeRefsForNodesWithBufferAdmission(std::span<const uint64_t>,
			LogicalGraphReadDirection, size_t, const std::function<bool(const EntityRef &)> &,
			const std::function<void(size_t)> &) const {
			throw std::logic_error("logical graph batch adjacency reference admission is unsupported");
		}

		virtual void visitAdjacentEdgeRefsForNodes(std::span<const uint64_t> nodeIds,
										  LogicalGraphReadDirection direction, size_t maximumEdges,
										  const std::function<bool(const EntityRef &)> &visitor) const {
			if (nodeIds.empty() || maximumEdges == 0U || !visitor) {
				throw std::invalid_argument("logical graph batch adjacency traversal requires nodes, bound, and visitor");
			}
			uint64_t previousNodeId = 0U;
			size_t visitedEdges = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (nodeId == 0U || nodeId <= previousNodeId) {
					throw std::invalid_argument(
							"logical graph batch adjacency traversal requires strictly increasing Node IDs");
				}
				previousNodeId = nodeId;
				bool continueVisiting = true;
				visitAdjacentEdges(nodeId, direction, maximumEdges - visitedEdges,
						[&visitor, &visitedEdges, &continueVisiting, maximumEdges, nodeId, direction](const LogicalGraphEdge &edge) {
							if (!continueVisiting) {
								throw std::runtime_error(
										"logical graph batch adjacency backend ignored the visitor stop request");
							}
							if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
								throw std::runtime_error("logical graph batch adjacency returned invalid topology");
							}
							const bool endpointMatches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
									? edge.source_node_id == nodeId : edge.target_node_id == nodeId;
							if (!endpointMatches) {
								throw std::runtime_error("logical graph batch adjacency returned an Edge for the wrong Node");
							}
							if (visitedEdges == maximumEdges) {
								throw std::runtime_error(
										"logical graph batch adjacency traversal exceeded its Edge bound");
							}
							++visitedEdges;
							continueVisiting = visitor({edge.edge_id, true, edge.source_node_id, edge.target_node_id, 0U});
							return continueVisiting;
						});
				if (!continueVisiting || visitedEdges == maximumEdges) {
					return;
				}
			}
		}
		[[nodiscard]] virtual bool supportsNativeBatchAdjacentEdgeRefs() const { return false; }
		// Backends that also provide logical writes return an opaque generation
		// fence. Read-only-only adapters may return nullopt; callers then must not
		// attempt a read-dependent optimistic mutation through that adapter.
		[[nodiscard]] virtual std::optional<LogicalGraphMutationFence> mutationFence() const { return std::nullopt; }
        // True only when fence/schema/token identities cannot change for this
        // snapshot's lifetime. Mutable overlays keep the default validation.
        [[nodiscard]] virtual bool hasImmutableReadIdentity() const noexcept { return false; }
		// Unsupported readers explicitly decline authenticated schema binding.
		// The identity does not replace a data snapshot/fence, semantic plan key,
		// capability validation or owning lifetime; it enables no plan cache alone.
		[[nodiscard]] virtual std::optional<LogicalGraphSchemaIdentity> bindingSchemaIdentity() const { return std::nullopt; }
	};

	// Storage backends opt in only after they can provide a snapshot-safe logical
	// read surface. This is intentionally separate from StorageBackend: lifecycle
	// capability must never imply query or transaction compatibility.
	class LogicalGraphReadBackend {
	public:
		virtual ~LogicalGraphReadBackend() = default;
		[[nodiscard]] virtual std::shared_ptr<query::execution::detail::DatabaseResourceBudget> queryResourceDomain() const { return {}; }
        // Cache metadata and retained unbound operator objects share this owner.
        [[nodiscard]] virtual std::shared_ptr<std::pmr::memory_resource> logicalPlanCacheMemoryOwner() const { return {}; }

		[[nodiscard]] virtual std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const = 0;

		// Optional bounded metadata observation for unbound logical IR caches.
		// It must not open a logical graph snapshot or scan graph data.
		[[nodiscard]] virtual std::optional<LogicalGraphSchemaIdentity> logicalPlanCacheIdentity() const { return std::nullopt; }
	};

	// The planning boundary receives only small snapshot-safe cardinalities. It
	// deliberately cannot request a graph scan, physical page location, or
	// mutable Catalog state while compiling a logical plan.
	struct LogicalGraphCardinalityStatistics {
		uint64_t node_count = 0U;
		uint64_t edge_count = 0U;

		bool operator==(const LogicalGraphCardinalityStatistics &) const = default;
	};

	struct LogicalGraphPlanningStatistics {
		std::optional<LogicalGraphCardinalityStatistics> graph;
		// One optional cost estimate per requested label combination, in order.
		std::vector<std::optional<uint64_t>> node_counts;
        // Requested nonempty relationship-type names, in order; missing is unknown.
        std::vector<std::optional<uint64_t>> edge_type_counts;
	};

	class LogicalGraphStatisticsBackend {
	public:
		virtual ~LogicalGraphStatisticsBackend() = default;
		// `nullopt` is an explicit unavailable/legacy state, never permission for
		// a planning-time whole-graph fallback scan.
		[[nodiscard]] virtual std::optional<LogicalGraphCardinalityStatistics>
		readLogicalGraphCardinalityStatistics() const = 0;
		// Optional metadata-only estimate; unavailable statistics must never
		// trigger a graph scan or become a query result/count.
		[[nodiscard]] virtual std::optional<uint64_t>
		estimateLogicalNodeCardinality(std::span<const std::string>) const { return std::nullopt; }
		// Optional batch capability. Native storage reads one pinned metadata
		// snapshot; the compatibility default uses existing optional interfaces.
		[[nodiscard]] virtual LogicalGraphPlanningStatistics readLogicalPlanningStatistics(
				std::span<const std::vector<std::string>> labels) const {
			LogicalGraphPlanningStatistics result;
			result.graph = readLogicalGraphCardinalityStatistics();
			result.node_counts.reserve(labels.size());
			for (const auto &combination : labels) {
				try { result.node_counts.push_back(estimateLogicalNodeCardinality(combination)); }
				catch (...) { result.node_counts.push_back(std::nullopt); }
			}
			return result;
		}

        [[nodiscard]] virtual LogicalGraphPlanningStatistics readLogicalPlanningStatistics(
                std::span<const std::vector<std::string>> labels, std::span<const std::string> types) const {
            auto result = readLogicalPlanningStatistics(labels);
            result.edge_type_counts.resize(types.size());
            return result;
        }


	};

} // namespace graph::storage
