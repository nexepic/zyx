/**
 * @file test_LogicalGraphProjection.cpp
 * @brief Unit tests for storage-neutral logical graph projections.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/gds/LogicalGraphAlgorithms.hpp"
#include "graph/query/gds/LogicalGraphProjection.hpp"
#include "graph/query/gds/LogicalProjectionCatalog.hpp"

namespace graph::query::gds {

	struct LogicalGraphAlgorithmsTestAccess final {
		static void refineDisconnectedCommunities(const LogicalGraphAlgorithms::LogicalWeightedAdjacency &adjacency,
				std::vector<size_t> &community, const std::vector<double> &degree, const double totalWeight,
				const double resolution, const double refinementThreshold, size_t &usedWork,
				const LogicalGraphAlgorithmBounds &bounds, const LogicalGraphAlgorithmCheckpoint &checkpoint = {}) {
			LogicalGraphAlgorithms::refineDisconnectedCommunities(adjacency, community, degree, totalWeight, resolution,
					refinementThreshold, usedWork, bounds, checkpoint);
		}
	};

	class LogicalProjectionCatalogTestAccess final {
	public:
		static void setNextRevisionToken(LogicalProjectionCatalog &catalog, const uint64_t token) {
			catalog.next_revision_token_ = token;
		}

		static void setSourceRevisionReferences(LogicalProjectionCatalog &catalog,
													const storage::LogicalGraphMutationFence &revision,
													const size_t references) {
			const auto found = catalog.source_revision_tokens_.find(revision);
			if (found == catalog.source_revision_tokens_.end()) {
				throw std::logic_error("test requires a retained source revision");
			}
			found->second.source_references = references;
		}

		static void eraseSourceRevision(LogicalProjectionCatalog &catalog,
												const storage::LogicalGraphMutationFence &revision) {
			catalog.source_revision_tokens_.erase(revision);
		}
	};

	namespace {

		class Snapshot final : public storage::LogicalGraphReadSnapshot {
		public:
		std::vector<storage::LogicalGraphNode> nodes;
		std::vector<storage::LogicalGraphEdge> edges;
		mutable size_t full_edge_reads = 0U;
		mutable size_t topology_reads = 0U;
			mutable size_t batched_property_reads = 0U;
			mutable std::vector<size_t> batched_property_read_sizes;
			bool support_property_read_admission = false;
			mutable size_t admitted_property_values_copied = 0U;
			mutable size_t legacy_property_values_copied = 0U;
			mutable std::vector<storage::EntityReadShape> node_scan_shapes;
			mutable size_t node_scan_property_rows = 0U;
			bool return_full_nodes_for_shaped_scan = false;
			bool return_mismatched_adjacency = false;
			bool return_unbounded_adjacency_page = false;
			bool paginate_adjacency = false;
			size_t adjacency_page_limit_override = 0U;
			bool return_oversized_node_page = false;
			bool paginate_nodes = false;
			bool return_excess_metadata_pages = false;
			bool preserve_node_scan_order = false;
			bool return_nodes_before_cursor = false;
			std::optional<storage::LogicalGraphMutationFence> revision =
					storage::LogicalGraphMutationFence{.backend_token = 1U, .format_token = 1U};

			[[nodiscard]] storage::LogicalGraphNode readNode(const uint64_t nodeId) const override {
				const auto found = std::ranges::find(nodes, nodeId, &storage::LogicalGraphNode::node_id);
				if (found == nodes.end()) {
					throw std::out_of_range("unknown Node");
				}
				return *found;
			}

			[[nodiscard]] bool containsNode(const uint64_t nodeId) const override {
				return std::ranges::find(nodes, nodeId, &storage::LogicalGraphNode::node_id) != nodes.end();
			}

			[[nodiscard]] storage::LogicalGraphEdge readEdge(const uint64_t edgeId) const override {
				++full_edge_reads;
				const auto found = std::ranges::find(edges, edgeId, &storage::LogicalGraphEdge::edge_id);
				if (found == edges.end()) {
					throw std::out_of_range("unknown relationship");
				}
				return *found;
			}

			void visitNodes(const size_t maximumRecords,
							const std::function<bool(const storage::LogicalGraphNode &)> &visitor) const override {
				if (maximumRecords == 0U) {
					return;
				}
				size_t visited = 0U;
				for (const storage::LogicalGraphNode &node: nodes) {
					if (++visited > maximumRecords || !visitor(node)) {
						return;
					}
				}
			}

			[[nodiscard]] storage::LogicalGraphNodePage
			scanNodesAfter(const uint64_t afterNodeId, const size_t maximumRecords,
						   const size_t maximumMetadataPages) const override {
				if (maximumRecords == 0U || maximumMetadataPages == 0U) {
					throw std::invalid_argument("invalid Node scan bounds");
				}
			std::vector<storage::LogicalGraphNode> sorted = nodes;
			std::ranges::sort(sorted, {}, &storage::LogicalGraphNode::node_id);
			const std::vector<storage::LogicalGraphNode> &ordered = preserve_node_scan_order ? nodes : sorted;
			storage::LogicalGraphNodePage page;
			const size_t pageLimit = paginate_nodes ? std::min<size_t>(maximumRecords, 1U) : maximumRecords;
			for (const storage::LogicalGraphNode &node: ordered) {
				if (!return_nodes_before_cursor && node.node_id <= afterNodeId) {
					continue;
				}
				if (!return_oversized_node_page && page.nodes.size() == pageLimit) {
					page.may_have_more = true;
					break;
				}
				page.nodes.push_back(node);
			}
			page.metadata_pages_visited = page.nodes.empty() ? 0U : 1U;
			if (return_excess_metadata_pages) {
				page.metadata_pages_visited = maximumMetadataPages + 1U;
			}
			return page;
			}

			[[nodiscard]] storage::LogicalGraphNodePage scanNodesAfter(
					const uint64_t afterNodeId, const size_t maximumRecords, const size_t maximumMetadataPages,
					const storage::EntityReadShape &shape) const override {
				storage::validateEntityReadShape(shape);
				node_scan_shapes.push_back(shape);
				storage::LogicalGraphNodePage page = scanNodesAfter(afterNodeId, maximumRecords, maximumMetadataPages);
				if (return_full_nodes_for_shaped_scan) {
					for (const storage::LogicalGraphNode &node: page.nodes) {
						if (!node.properties.empty()) ++node_scan_property_rows;
					}
					return page;
				}
				if (!shape.include_labels) {
					for (storage::LogicalGraphNode &node: page.nodes) {
						node.labels.clear();
					}
				}
				if (!shape.all_properties && shape.property_tokens.empty()) {
					for (storage::LogicalGraphNode &node: page.nodes) {
						node.properties.clear();
					}
				}
				return page;
			}

			[[nodiscard]] storage::LogicalGraphAdjacencyPage
			scanAdjacentEdges(const uint64_t nodeId, const storage::LogicalGraphReadDirection direction,
							  const std::optional<storage::LogicalGraphAdjacencyContinuation> continuation,
							  const size_t maximumRecords) const override {
				if (maximumRecords == 0U || (continuation.has_value() && !paginate_adjacency)) {
					throw std::invalid_argument("invalid paged relationship scan request");
				}
				storage::LogicalGraphAdjacencyPage page;
				const size_t pageLimit = paginate_adjacency
						? std::min(maximumRecords, adjacency_page_limit_override == 0U ? 1U : adjacency_page_limit_override)
						: maximumRecords;
				bool afterContinuation = !continuation.has_value();
				for (const storage::LogicalGraphEdge &edge: edges) {
					const bool matches = return_mismatched_adjacency ||
							(direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
									 ? edge.source_node_id == nodeId
									 : edge.target_node_id == nodeId);
					if (!matches) {
						continue;
					}
					if (!afterContinuation) {
						if (edge.edge_id == continuation->next_edge_id) {
							afterContinuation = true;
						} else {
							continue;
						}
					}
					if (!return_unbounded_adjacency_page && page.edges.size() == pageLimit) {
						page.continuation = storage::LogicalGraphAdjacencyContinuation{
								.previous_edge_id = page.edges.back().edge_id, .next_edge_id = edge.edge_id};
						break;
					}
					page.edges.push_back(edge);
				}
				return page;
			}

			[[nodiscard]] storage::LogicalGraphAdjacencyTopologyPage scanAdjacentEdgeTopologies(
					const uint64_t nodeId, const storage::LogicalGraphReadDirection direction,
					const std::optional<storage::LogicalGraphAdjacencyContinuation> continuation,
					const size_t maximumRecords) const override {
				++topology_reads;
				const storage::LogicalGraphAdjacencyPage full = scanAdjacentEdges(nodeId, direction, continuation, maximumRecords);
				storage::LogicalGraphAdjacencyTopologyPage result;
				result.continuation = full.continuation;
				for (const storage::LogicalGraphEdge &edge: full.edges) {
					result.edges.push_back({edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type});
				}
				return result;
			}

			[[nodiscard]] std::vector<PropertyValue> readEdgePropertiesSorted(
					const std::span<const uint64_t> edgeIds, const std::string_view key) const override {
				++batched_property_reads;
				batched_property_read_sizes.push_back(edgeIds.size());
				std::vector<PropertyValue> result;
				result.reserve(edgeIds.size());
				for (const uint64_t edgeId: edgeIds) {
					const auto edge = std::ranges::find(edges, edgeId, &storage::LogicalGraphEdge::edge_id);
					if (edge == edges.end()) {
						throw std::out_of_range("unknown relationship");
					}
					const auto property = std::ranges::find(edge->properties, key, &storage::LogicalGraphPropertyEntry::key);
					++legacy_property_values_copied;
					result.push_back(property == edge->properties.end() ? PropertyValue() : property->value);
				}
				return result;
			}

			[[nodiscard]] bool supportsPropertyReadAdmission() const override {
				return support_property_read_admission;
			}

			[[nodiscard]] std::vector<std::vector<PropertyValue>> readEdgePropertyColumnsSortedWithAdmission(
					const std::span<const uint64_t> edgeIds, const std::span<const std::string_view> keys,
					const std::function<void(size_t)> &admission) const override {
				if (!support_property_read_admission || keys.size() != 1U) {
					throw std::logic_error("unexpected selected-Property admission request");
				}
				++batched_property_reads;
				batched_property_read_sizes.push_back(edgeIds.size());
				std::vector<PropertyValue> values;
				values.reserve(edgeIds.size());
				for (const uint64_t edgeId: edgeIds) {
					const auto edge = std::ranges::find(edges, edgeId, &storage::LogicalGraphEdge::edge_id);
					if (edge == edges.end()) throw std::out_of_range("unknown relationship");
					const auto property = std::ranges::find(edge->properties, keys.front(), &storage::LogicalGraphPropertyEntry::key);
					const PropertyValue *value = property == edge->properties.end() ? nullptr : &property->value;
					const size_t bytes = value == nullptr ? property_utils::getPropertyValueSize(PropertyValue())
							: property_utils::getPropertyValueSize(*value);
					admission(bytes);
					++admitted_property_values_copied;
					values.push_back(value == nullptr ? PropertyValue() : *value);
				}
				return {std::move(values)};
			}

			void visitEdges(const size_t maximumRecords,
							const std::function<bool(const storage::LogicalGraphEdge &)> &visitor) const override {
				size_t visited = 0U;
				for (const storage::LogicalGraphEdge &edge: edges) {
					if (++visited > maximumRecords || !visitor(edge)) {
						return;
					}
				}
			}

			void
			visitAdjacentEdges(const uint64_t nodeId, const storage::LogicalGraphReadDirection direction,
							   const size_t maximumEdges,
							   const std::function<bool(const storage::LogicalGraphEdge &)> &visitor) const override {
				size_t visited = 0U;
				for (const storage::LogicalGraphEdge &edge: edges) {
					const bool matches = return_mismatched_adjacency ||
										 (direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
												  ? edge.source_node_id == nodeId
												  : edge.target_node_id == nodeId);
					if (!matches) {
						continue;
					}
					if (++visited > maximumEdges || !visitor(edge)) {
						return;
					}
				}
			}

			[[nodiscard]] std::optional<storage::LogicalGraphMutationFence> mutationFence() const override {
				return revision;
			}
		};

		[[nodiscard]] LogicalProjectionBuildBounds bounds() {
			return {.maximum_nodes = 16U, .maximum_metadata_pages = 2U, .maximum_edges_per_node = 16U};
		}

		[[nodiscard]] LogicalGraphAlgorithmBounds algorithmBounds() {
			return {.maximum_nodes = 16U, .maximum_edges = 64U, .maximum_iterations = 32U, .maximum_work_units = 512U};
		}

		[[nodiscard]] storage::LogicalGraphPropertyEntry property(std::string key, PropertyValue value) {
			return {.key = std::move(key), .value = std::move(value)};
		}

	} // namespace

	TEST(LogicalGraphProjectionTest, BuildsFilteredStableWeightedAdjacencyFromLogicalSnapshot) {
		Snapshot snapshot;
		snapshot.nodes = {
				{1U, {"Code"}},
				{2U, {"Person"}},
				{3U, {"Code", "Service"}},
		};
		snapshot.edges = {
				{11U, 1U, 3U, "CALLS", {property("cost", PropertyValue(2.5))}},
				{12U, 1U, 2U, "CALLS", {property("cost", PropertyValue(9.0))}},
				{13U, 3U, 1U, "IMPORTS", {}},
		};

		LogicalGraphProjectionSpec spec;
		spec.name = "code-callers";
		spec.node_labels = {"Code"};
		spec.relationships = {{.type = "CALLS",
							   .orientation = LogicalProjectionOrientation::LPO_NATURAL,
							   .weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY,
										  .property_name = "cost",
										  .default_weight = 1.0}}};

		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());
		EXPECT_EQ(projection.nodeIds(), (std::vector<uint64_t>{1U, 3U}));
		EXPECT_EQ(projection.nodeCount(), 2U);
		EXPECT_EQ(projection.edgeCount(), 1U);
		EXPECT_TRUE(projection.isWeighted());
		ASSERT_EQ(projection.outgoing(1U).size(), 1U);
		EXPECT_EQ(projection.outgoing(1U).front(),
				  (LogicalProjectionEdge{.target_node_id = 3U, .weight = 2.5, .relationship_id = 11U}));
		ASSERT_EQ(projection.incoming(3U).size(), 1U);
		EXPECT_EQ(projection.incoming(3U).front().target_node_id, 1U);
		EXPECT_TRUE(projection.outgoing(2U).empty());
		EXPECT_GT(snapshot.topology_reads, 0U);
		EXPECT_EQ(snapshot.full_edge_reads, 0U);
		EXPECT_EQ(snapshot.batched_property_reads, 1U);
		ASSERT_FALSE(snapshot.node_scan_shapes.empty());
		for (const storage::EntityReadShape &shape: snapshot.node_scan_shapes) {
			EXPECT_EQ(shape, (storage::EntityReadShape{.include_labels = true}));
		}
		EXPECT_GT(projection.estimatedMemoryBytes(), sizeof(LogicalGraphProjection));
		const std::shared_ptr<const LogicalProjectionCsr> firstCsr = projection.csr();
		const std::shared_ptr<const LogicalProjectionCsr> secondCsr = projection.csr();
		EXPECT_EQ(firstCsr, secondCsr);
		EXPECT_EQ(firstCsr->node_ids, (std::vector<uint64_t>{1U, 3U}));
		EXPECT_EQ(firstCsr->offsets, (std::vector<size_t>{0U, 1U, 1U}));
		EXPECT_EQ(firstCsr->target_node_ids, (std::vector<uint64_t>{3U}));
		EXPECT_EQ(firstCsr->weights, (std::vector<double>{2.5}));
	}

	TEST(LogicalGraphProjectionTest, ReadsLargeWeightColumnsInBoundedEdgeBatches) {
		Snapshot snapshot;
		snapshot.support_property_read_admission = true;
		snapshot.paginate_adjacency = true;
		snapshot.adjacency_page_limit_override = 256U;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges.reserve(300U);
		for (uint64_t edgeId = 1U; edgeId <= 300U; ++edgeId) {
			snapshot.edges.push_back({edgeId, 1U, 2U, "CALLS", {property("cost", PropertyValue(2.0))}});
		}
		LogicalGraphProjectionSpec spec;
		spec.name = "batched-weight-column";
		spec.relationships = {{.type = "CALLS",
				.weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY, .property_name = "cost"}}};
		LogicalProjectionBuildBounds projectionBounds = bounds();
		projectionBounds.maximum_edges_per_node = 512U;
		projectionBounds.maximum_property_bytes = 300U * sizeof(double);

		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, projectionBounds);

		EXPECT_EQ(projection.edgeCount(), 300U);
		ASSERT_EQ(projection.outgoing(1U).size(), 300U);
		EXPECT_DOUBLE_EQ(projection.outgoing(1U).front().weight, 2.0);
		EXPECT_DOUBLE_EQ(projection.outgoing(1U).back().weight, 2.0);
		EXPECT_EQ(snapshot.batched_property_read_sizes, (std::vector<size_t>{256U, 44U}));
		EXPECT_EQ(snapshot.admitted_property_values_copied, 300U);

		snapshot.batched_property_reads = 0U;
		snapshot.batched_property_read_sizes.clear();
		snapshot.admitted_property_values_copied = 0U;
		projectionBounds.maximum_property_bytes = 280U * sizeof(double);
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec, projectionBounds)),
				std::runtime_error);
		EXPECT_EQ(snapshot.batched_property_read_sizes, (std::vector<size_t>{256U, 44U}));
		EXPECT_EQ(snapshot.admitted_property_values_copied, 280U);
	}

	TEST(LogicalGraphProjectionTest, RequestsIdentityOnlyNodeScanWithoutLabelFilter) {
		Snapshot snapshot;
		snapshot.nodes = {
				{1U, {"Code"}, {property("source", PropertyValue("a"))}},
				{2U, {"Person"}, {property("source", PropertyValue("b"))}},
		};
		snapshot.edges = {{11U, 1U, 2U, "CALLS", {}}};

		LogicalGraphProjectionSpec spec;
		spec.name = "all-nodes";
		spec.relationships = {{.type = "CALLS",
								.orientation = LogicalProjectionOrientation::LPO_NATURAL,
								.weight = {.kind = LogicalProjectionWeightKind::LPWK_CONSTANT, .constant_weight = 1.0}}};

		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());
		EXPECT_EQ(projection.nodeIds(), (std::vector<uint64_t>{1U, 2U}));
		EXPECT_EQ(projection.edgeCount(), 1U);
		ASSERT_FALSE(snapshot.node_scan_shapes.empty());
		for (const storage::EntityReadShape &shape: snapshot.node_scan_shapes) {
			EXPECT_EQ(shape, (storage::EntityReadShape{}));
		}
	}

	TEST(LogicalGraphProjectionTest, ShapedNodeScanMatchesCompleteNodeScanProjection) {
		Snapshot shapedSnapshot;
		shapedSnapshot.nodes = {
				{1U, {"Code"}, {property("source", PropertyValue(std::string(4096U, 'a')))}},
				{2U, {"Person"}, {property("source", PropertyValue(std::string(4096U, 'b')))}},
				{3U, {"Code", "Service"}, {property("source", PropertyValue(std::string(4096U, 'c')))}},
		};
		shapedSnapshot.edges = {
				{11U, 1U, 3U, "CALLS", {property("cost", PropertyValue(2.5))}},
				{12U, 1U, 2U, "CALLS", {property("cost", PropertyValue(9.0))}},
				{13U, 3U, 1U, "IMPORTS", {}},
		};
		Snapshot completeSnapshot = shapedSnapshot;
		completeSnapshot.return_full_nodes_for_shaped_scan = true;

		LogicalGraphProjectionSpec spec;
		spec.name = "shape-equivalence";
		spec.node_labels = {"Code"};
		spec.relationships = {{.type = "CALLS",
				.orientation = LogicalProjectionOrientation::LPO_NATURAL,
				.weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY,
						.property_name = "cost", .default_weight = 1.0}}};

		const LogicalGraphProjection shaped = LogicalGraphProjection::build(shapedSnapshot, spec, bounds());
		const LogicalGraphProjection complete = LogicalGraphProjection::build(completeSnapshot, spec, bounds());

		EXPECT_EQ(shaped.nodeIds(), complete.nodeIds());
		EXPECT_EQ(shaped.nodeIds(), (std::vector<uint64_t>{1U, 3U}));
		EXPECT_EQ(shaped.edgeCount(), complete.edgeCount());
		EXPECT_EQ(shaped.outgoing(1U), complete.outgoing(1U));
		EXPECT_EQ(shaped.incoming(3U), complete.incoming(3U));
		EXPECT_EQ(shaped.csr()->node_ids, complete.csr()->node_ids);
		EXPECT_EQ(shaped.csr()->offsets, complete.csr()->offsets);
		EXPECT_EQ(shaped.csr()->target_node_ids, complete.csr()->target_node_ids);
		EXPECT_EQ(shaped.csr()->weights, complete.csr()->weights);
		EXPECT_EQ(shapedSnapshot.node_scan_property_rows, 0U);
		EXPECT_EQ(completeSnapshot.node_scan_property_rows, 3U);
		ASSERT_FALSE(shapedSnapshot.node_scan_shapes.empty());
		for (const storage::EntityReadShape &shape: shapedSnapshot.node_scan_shapes) {
			EXPECT_EQ(shape, (storage::EntityReadShape{.include_labels = true}));
		}
		EXPECT_EQ(shapedSnapshot.batched_property_reads, completeSnapshot.batched_property_reads);
	}

	TEST(LogicalGraphProjectionTest, AppliesUndirectedAndReverseOrientationsWithoutDuplicatingSelfLoops) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{21U, 1U, 2U, "USES", {}}, {22U, 2U, 2U, "USES", {}}};

		LogicalGraphProjectionSpec undirected;
		undirected.name = "undirected";
		undirected.relationships = {
				{.type = "USES",
				 .orientation = LogicalProjectionOrientation::LPO_UNDIRECTED,
				 .weight = {.kind = LogicalProjectionWeightKind::LPWK_CONSTANT, .constant_weight = 3.0}}};
		const LogicalGraphProjection symmetric = LogicalGraphProjection::build(snapshot, undirected, bounds());
		EXPECT_EQ(symmetric.edgeCount(), 3U);
		ASSERT_EQ(symmetric.outgoing(2U).size(), 2U);
		EXPECT_TRUE(symmetric.outgoing(2U).front().synthetic_reverse);
		EXPECT_FALSE(symmetric.outgoing(2U).back().synthetic_reverse);

		LogicalGraphProjectionSpec reverse = undirected;
		reverse.name = "reverse";
		reverse.relationships.front().orientation = LogicalProjectionOrientation::LPO_REVERSE;
		const LogicalGraphProjection reversed = LogicalGraphProjection::build(snapshot, reverse, bounds());
		ASSERT_EQ(reversed.outgoing(2U).size(), 2U);
		EXPECT_EQ(reversed.outgoing(2U).front().target_node_id, 1U);
		EXPECT_EQ(reversed.outgoing(2U).front().relationship_id, 21U);
	}

	TEST(LogicalGraphProjectionTest, RejectsInvalidSpecsAndInvalidSnapshotRelationships) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{31U, 1U, 2U, "CALLS", {property("weight", PropertyValue("bad"))}}};

		LogicalGraphProjectionSpec spec;
		spec.name = "valid";
		spec.relationships = {
				{.type = "CALLS",
				 .weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY, .property_name = "weight"}}};
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec, bounds())), std::invalid_argument);

		spec.name.clear();
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec, bounds())), std::invalid_argument);
		spec.name = "duplicate-type";
		spec.relationships.push_back(spec.relationships.front());
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec, bounds())), std::invalid_argument);

		snapshot.edges = {{32U, 2U, 1U, "CALLS", {}}};
		snapshot.return_mismatched_adjacency = true;
		spec.relationships.resize(1U);
		spec.relationships.front().weight = {};
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec, bounds())), std::runtime_error);
	}

	TEST(LogicalGraphProjectionTest, RejectsNonMonotonicTopologyBeforeBuildingAdjacency) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}};
		snapshot.edges = {{42U, 1U, 2U, "CALLS", {}}, {41U, 1U, 3U, "CALLS", {}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "non-monotonic-topology";
		spec.relationships = {{.type = "CALLS"}};

		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec,
				{.maximum_nodes = 3U, .maximum_metadata_pages = 1U, .maximum_edges_per_node = 2U})),
				std::runtime_error);
		EXPECT_EQ(snapshot.full_edge_reads, 0U);
		EXPECT_EQ(snapshot.batched_property_reads, 0U);
	}

	TEST(LogicalGraphProjectionTest, ConsumesMultiplePhysicalAdjacencyPagesWithinTheLogicalBudget) {
		Snapshot snapshot;
		snapshot.paginate_adjacency = true;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}, {4U, {"Code"}}};
		snapshot.edges = {{41U, 1U, 2U, "CALLS", {}}, {42U, 1U, 3U, "CALLS", {}},
				{43U, 1U, 4U, "CALLS", {}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "paged";
		spec.relationships = {{.type = "CALLS"}};
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec,
				{.maximum_nodes = 4U, .maximum_metadata_pages = 1U, .maximum_edges_per_node = 3U});
		EXPECT_EQ(projection.edgeCount(), 3U);
		ASSERT_EQ(projection.outgoing(1U).size(), 3U);
		EXPECT_EQ(projection.outgoing(1U)[0].relationship_id, 41U);
		EXPECT_EQ(projection.outgoing(1U)[1].relationship_id, 42U);
		EXPECT_EQ(projection.outgoing(1U)[2].relationship_id, 43U);
		EXPECT_EQ(snapshot.topology_reads, 6U);
	}

	TEST(LogicalGraphProjectionTest, CapsAValidLargeAdjacencyReadIntoBoundedBatches) {
		Snapshot snapshot;
		snapshot.paginate_adjacency = true;
		snapshot.adjacency_page_limit_override = 256U;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges.reserve(300U);
		for (uint64_t edgeId = 1U; edgeId <= 300U; ++edgeId) {
			snapshot.edges.push_back({edgeId, 1U, 2U, "CALLS", {}});
		}
		LogicalGraphProjectionSpec spec;
		spec.name = "bounded-large-adjacency";
		spec.relationships = {{.type = "CALLS"}};
		const LogicalGraphProjection projection = LogicalGraphProjection::build(
				snapshot, spec, {.maximum_nodes = 2U, .maximum_metadata_pages = 1U, .maximum_edges_per_node = 300U});
		EXPECT_EQ(projection.edgeCount(), 300U);
		// The projection budget is larger than one physical request, but no request
		// may exceed the shared bounded adjacency batch size.
		EXPECT_EQ(snapshot.topology_reads, 3U);
	}

	TEST(LogicalGraphProjectionTest, ConsumesMultiplePhysicalNodePagesWithinTheLogicalBudget) {
		Snapshot snapshot;
		snapshot.paginate_nodes = true;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}};
		snapshot.edges = {{51U, 1U, 2U, "CALLS", {}}, {52U, 2U, 3U, "CALLS", {}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "paged-nodes";
		spec.node_labels = {"Code"};
		spec.relationships = {{.type = "CALLS"}};
		const LogicalGraphProjection projection = LogicalGraphProjection::build(
				snapshot, spec, {.maximum_nodes = 3U, .maximum_metadata_pages = 3U, .maximum_edges_per_node = 2U});
		EXPECT_EQ(projection.nodeIds(), (std::vector<uint64_t>{1U, 2U, 3U}));
		EXPECT_EQ(projection.edgeCount(), 2U);
	}

	TEST(LogicalGraphProjectionTest, RejectsAnAdjacencyCursorThatWouldTruncateASupernode) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}};
		snapshot.edges = {{1U, 1U, 2U, "CALLS", {}}, {2U, 1U, 3U, "CALLS", {}}};

		LogicalGraphProjectionSpec spec;
		spec.name = "bounded-supernode";
		const LogicalProjectionBuildBounds tightBounds{
				.maximum_nodes = 3U, .maximum_metadata_pages = 1U, .maximum_edges_per_node = 1U};
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec, tightBounds)), std::runtime_error);
	}

	TEST(LogicalGraphProjectionTest, RejectsAMalformedAdjacencyCursorThatExceedsItsAdvertisedBound) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}};
		for (uint64_t id = 1U; id <= 257U; ++id) snapshot.edges.push_back({id, 1U, 2U, "CALLS", {}});
		snapshot.return_unbounded_adjacency_page = true;

		LogicalGraphProjectionSpec spec;
		spec.name = "malformed-adjacency-page";
		const LogicalProjectionBuildBounds tightBounds{
				.maximum_nodes = 3U, .maximum_metadata_pages = 1U, .maximum_edges_per_node = 512U};
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec, tightBounds)), std::runtime_error);
	}

	TEST(LogicalGraphProjectionTest, ValidatesSnapshotBoundsLabelsAndPropertyWeightSemantics) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {
				{1U, 1U, 2U, "CALLS", {property("other", PropertyValue(9.0))}},
				{2U, 1U, 2U, "CALLS", {property("cost", PropertyValue())}},
				{3U, 1U, 2U, "CALLS", {property("cost", PropertyValue(int64_t{2}))}},
				{4U, 1U, 2U, "CALLS", {property("cost", PropertyValue(3.5))}},
		};
		LogicalGraphProjectionSpec spec;
		spec.name = "projection-validation";
		spec.relationships = {{.type = "CALLS",
							   .weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY,
										  .property_name = "cost",
										  .default_weight = 7.0}}};
		const LogicalGraphProjection weighted = LogicalGraphProjection::build(snapshot, spec, bounds());
		ASSERT_EQ(weighted.outgoing(1U).size(), 4U);
		EXPECT_EQ(weighted.outgoing(1U)[0U].weight, 7.0);
		EXPECT_EQ(weighted.outgoing(1U)[1U].weight, 7.0);
		EXPECT_EQ(weighted.outgoing(1U)[2U].weight, 2.0);
		EXPECT_EQ(weighted.outgoing(1U)[3U].weight, 3.5);
		// Weight properties are fetched as one selected column after topology
		// collection; projection must not materialize complete Edges on this path.
		EXPECT_EQ(snapshot.batched_property_reads, 1U);
		EXPECT_EQ(snapshot.full_edge_reads, 0U);
		EXPECT_TRUE(weighted.incoming(99U).empty());

		auto invalidBounds = bounds();
		invalidBounds.maximum_nodes = 0U;
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec, invalidBounds)),
					 std::invalid_argument);
		invalidBounds = bounds();
		invalidBounds.maximum_metadata_pages = 0U;
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec, invalidBounds)),
					 std::invalid_argument);
		invalidBounds = bounds();
		invalidBounds.maximum_edges_per_node = 0U;
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec, invalidBounds)),
					 std::invalid_argument);
		invalidBounds = bounds();
		invalidBounds.maximum_nodes = std::numeric_limits<size_t>::max();
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec, invalidBounds)),
					 std::overflow_error);

		LogicalGraphProjectionSpec invalid = spec;
		invalid.node_labels = {"", "Code"};
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, invalid, bounds())), std::invalid_argument);
		invalid.node_labels = {"Code", "Code"};
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, invalid, bounds())), std::invalid_argument);
		invalid = spec;
		invalid.relationships = {{}, {}};
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, invalid, bounds())), std::invalid_argument);
		invalid = spec;
		invalid.relationships.front().weight.property_name.clear();
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, invalid, bounds())), std::invalid_argument);
		invalid = spec;
		invalid.relationships.front().weight.default_weight = -1.0;
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, invalid, bounds())), std::invalid_argument);
		invalid = spec;
		invalid.relationships.front().weight = {
				.kind = LogicalProjectionWeightKind::LPWK_CONSTANT,
				.constant_weight = std::numeric_limits<double>::infinity()};
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, invalid, bounds())), std::invalid_argument);

		Snapshot malformed = snapshot;
		malformed.return_oversized_node_page = true;
		const LogicalProjectionBuildBounds oneNode{
				.maximum_nodes = 1U, .maximum_metadata_pages = 1U, .maximum_edges_per_node = 8U};
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(malformed, spec, oneNode)), std::runtime_error);
		malformed = snapshot;
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(malformed, spec, oneNode)), std::runtime_error);
		malformed = snapshot;
		malformed.return_excess_metadata_pages = true;
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(malformed, spec, bounds())), std::runtime_error);
		malformed = snapshot;
		malformed.nodes = {{0U, {"Code"}}};
		malformed.return_nodes_before_cursor = true;
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(malformed, spec, bounds())), std::runtime_error);
		malformed = snapshot;
		malformed.nodes = {{2U, {"Code"}}, {1U, {"Code"}}};
		malformed.preserve_node_scan_order = true;
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(malformed, spec, bounds())), std::runtime_error);
		malformed = snapshot;
		malformed.edges = {{0U, 1U, 2U, "CALLS", {}}};
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(malformed, spec, bounds())), std::runtime_error);
		malformed = snapshot;
		malformed.edges = {{1U, 1U, 0U, "CALLS", {}}};
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(malformed, spec, bounds())), std::runtime_error);
		malformed = snapshot;
		malformed.edges = {{1U, 0U, 2U, "CALLS", {}}};
		malformed.return_mismatched_adjacency = true;
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(malformed, spec, bounds())), std::runtime_error);

		size_t checkpointCalls = 0U;
		EXPECT_NO_THROW(static_cast<void>(LogicalGraphProjection::build(
				snapshot, spec, bounds(), [&checkpointCalls] { ++checkpointCalls; })));
		EXPECT_GT(checkpointCalls, 0U);
	}

	TEST(LogicalGraphProjectionTest, RejectsAnOversizedWeightColumnBeforeRetainingTheProjection) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{1U, 1U, 2U, "CALLS", {property("cost", PropertyValue(std::string(128U, 'x')))}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "bounded-weight-column";
		spec.relationships = {{.type = "CALLS",
										 .weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY,
													 .property_name = "cost"}}};
		LogicalProjectionBuildBounds limited = bounds();
		limited.maximum_property_bytes = 16U;
		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec, limited)), std::runtime_error);
	}

	TEST(LogicalGraphProjectionTest, AdmitsWeightPayloadBeforeSelectedColumnCopy) {
		Snapshot snapshot;
		snapshot.support_property_read_admission = true;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{1U, 1U, 2U, "CALLS", {property("cost", PropertyValue(std::string(128U, 'x')))}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "admitted-weight-column";
		spec.relationships = {{.type = "CALLS",
				.weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY, .property_name = "cost"}}};
		LogicalProjectionBuildBounds limited = bounds();
		limited.maximum_property_bytes = 16U;

		EXPECT_THROW(static_cast<void>(LogicalGraphProjection::build(snapshot, spec, limited)), std::runtime_error);
		EXPECT_EQ(snapshot.admitted_property_values_copied, 0U);
		EXPECT_EQ(snapshot.legacy_property_values_copied, 0U);
	}

	TEST(LogicalProjectionCatalogTest, OwnsImmutableProjectionsAndReportsSnapshotStaleness) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{41U, 1U, 2U, "CALLS", {}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "code";

		LogicalProjectionCatalog catalog;
		const LogicalProjectionDescriptor created = catalog.create(spec, snapshot, bounds());
		EXPECT_EQ(created.name, "code");
		EXPECT_EQ(created.node_count, 2U);
		EXPECT_EQ(created.edge_count, 1U);
		EXPECT_FALSE(created.stale);
		EXPECT_GT(created.source_revision, 0U);
		EXPECT_EQ(created.source_revision, created.current_revision);
		EXPECT_TRUE(catalog.exists("code"));
		EXPECT_EQ(catalog.get("code")->nodeCount(), 2U);
		EXPECT_THROW(static_cast<void>(catalog.create(spec, snapshot, bounds())), std::invalid_argument);

		snapshot.revision->backend_token = 2U;
		const LogicalProjectionDescriptor staleDescriptor = catalog.describe("code", snapshot);
		EXPECT_TRUE(staleDescriptor.stale);
		EXPECT_EQ(staleDescriptor.source_revision, created.source_revision);
		EXPECT_GT(staleDescriptor.current_revision, created.source_revision);
		const std::vector<LogicalProjectionDescriptor> listed = catalog.list(snapshot, 16U);
		ASSERT_EQ(listed.size(), 1U);
		EXPECT_TRUE(listed.front().stale);
		EXPECT_EQ(listed.front().source_revision, created.source_revision);
		EXPECT_EQ(listed.front().current_revision, staleDescriptor.current_revision);
		EXPECT_TRUE(catalog.drop("code"));
		EXPECT_FALSE(catalog.drop("code"));
		EXPECT_FALSE(catalog.exists("code"));
		EXPECT_THROW(static_cast<void>(catalog.get("code")), std::out_of_range);

		snapshot.revision.reset();
		EXPECT_THROW(static_cast<void>(catalog.create(spec, snapshot, bounds())), std::runtime_error);
	}

	TEST(LogicalProjectionCatalogTest, ValidatesPublicInputsAndRetainsSharedSourceRevisions) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{43U, 1U, 2U, "CALLS", {}}};
		LogicalProjectionCatalog catalog;
		LogicalGraphProjectionSpec emptySpec;

		EXPECT_THROW(static_cast<void>(catalog.prepareCreate(emptySpec, snapshot, bounds())), std::invalid_argument);
		EXPECT_FALSE(catalog.exists(""));
		EXPECT_THROW(static_cast<void>(catalog.prepareDrop("")), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(catalog.drop("")), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(catalog.get("")), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(catalog.describe("", snapshot)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(catalog.describe("missing", snapshot)), std::out_of_range);

		snapshot.revision = storage::LogicalGraphMutationFence{.backend_token = 0U, .format_token = 1U};
		LogicalGraphProjectionSpec invalidRevisionSpec;
		invalidRevisionSpec.name = "invalid-revision";
		EXPECT_THROW(static_cast<void>(catalog.create(invalidRevisionSpec, snapshot, bounds())), std::runtime_error);
		EXPECT_THROW(static_cast<void>(catalog.list(snapshot, 1U)), std::runtime_error);

		snapshot.revision = storage::LogicalGraphMutationFence{.backend_token = 1U, .format_token = 1U};
		// Observing one revision without retaining it must not cause a distinct
		// later source revision to reuse that token.
		Snapshot earlierSnapshot = snapshot;
		earlierSnapshot.revision->backend_token = 7U;
		LogicalProjectionCatalog observedOnlyCatalog;
		EXPECT_TRUE(observedOnlyCatalog.list(earlierSnapshot, 1U).empty());
		LogicalGraphProjectionSpec observedLaterSpec;
		observedLaterSpec.name = "observed-later";
		const LogicalProjectionDescriptor observedLater =
				observedOnlyCatalog.create(observedLaterSpec, snapshot, bounds());
		EXPECT_NE(observedLater.source_revision, 1U);

		LogicalGraphProjectionSpec firstSpec;
		firstSpec.name = "first";
		LogicalGraphProjectionSpec secondSpec;
		secondSpec.name = "second";
		const LogicalProjectionDescriptor first = catalog.create(firstSpec, snapshot, bounds());
		const LogicalProjectionDescriptor second = catalog.create(secondSpec, snapshot, bounds());
		EXPECT_EQ(first.source_revision, second.source_revision);

		EXPECT_TRUE(catalog.drop(firstSpec.name));
		const LogicalProjectionDescriptor retained = catalog.describe(secondSpec.name, snapshot);
		EXPECT_EQ(retained.source_revision, second.source_revision);
		EXPECT_EQ(retained.current_revision, second.source_revision);
		EXPECT_TRUE(catalog.drop(secondSpec.name));
	}

	TEST(LogicalProjectionCatalogTest, FailsClosedWhenInternalRevisionAccountingIsCorrupt) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{45U, 1U, 2U, "CALLS", {}}};

		LogicalProjectionCatalog exhaustedTokenCatalog;
		LogicalProjectionCatalogTestAccess::setNextRevisionToken(exhaustedTokenCatalog, 0U);
		EXPECT_THROW(static_cast<void>(exhaustedTokenCatalog.list(snapshot, 1U)), std::overflow_error);

		LogicalProjectionCatalog referenceCatalog;
		LogicalGraphProjectionSpec anchorSpec;
		anchorSpec.name = "anchor";
		static_cast<void>(referenceCatalog.create(anchorSpec, snapshot, bounds()));
		LogicalProjectionCatalogTestAccess::setSourceRevisionReferences(
				referenceCatalog, *snapshot.revision, std::numeric_limits<size_t>::max());
		LogicalGraphProjectionSpec overflowSpec;
		overflowSpec.name = "overflow";
		EXPECT_THROW(static_cast<void>(referenceCatalog.create(overflowSpec, snapshot, bounds())), std::overflow_error);

		LogicalProjectionCatalogTestAccess::setSourceRevisionReferences(referenceCatalog, *snapshot.revision, 0U);
		EXPECT_THROW(static_cast<void>(referenceCatalog.drop(anchorSpec.name)), std::logic_error);

		LogicalProjectionCatalog missingReferenceCatalog;
		static_cast<void>(missingReferenceCatalog.create(anchorSpec, snapshot, bounds()));
		LogicalProjectionCatalogTestAccess::eraseSourceRevision(missingReferenceCatalog, *snapshot.revision);
		EXPECT_THROW(static_cast<void>(missingReferenceCatalog.drop(anchorSpec.name)), std::logic_error);
	}

	TEST(LogicalProjectionCatalogTest, CancelsAtBothPostBuildPublicationBoundaries) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{44U, 1U, 2U, "CALLS", {}}};
		LogicalProjectionCatalog catalog;
		LogicalGraphProjectionSpec spec;
		spec.name = "post-build-cancellation";

		size_t baselineCalls = 0U;
		{
			LogicalProjectionCatalogCreatePreparation preparation = catalog.prepareCreate(
					spec, snapshot, bounds(), [&baselineCalls] { ++baselineCalls; });
			ASSERT_NE(preparation.mutation, nullptr);
		}
		ASSERT_GE(baselineCalls, 2U);

		for (const size_t cancellationCall: {baselineCalls - 1U, baselineCalls}) {
			size_t checkpointCalls = 0U;
			EXPECT_THROW(static_cast<void>(catalog.prepareCreate(
						 spec, snapshot, bounds(), [&checkpointCalls, cancellationCall] {
							 ++checkpointCalls;
							 if (checkpointCalls == cancellationCall) {
								 throw std::runtime_error("cancel after immutable projection build");
							 }
						 })),
					 std::runtime_error);
			EXPECT_EQ(checkpointCalls, cancellationCall);
			EXPECT_FALSE(catalog.exists(spec.name));
		}
	}

	TEST(LogicalProjectionCatalogTest, CancelledBuildNeverPublishesAPartialProjection) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{42U, 1U, 2U, "CALLS", {}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "cancelled";

		LogicalProjectionCatalog catalog;
		size_t checkpointCount = 0U;
		EXPECT_THROW(static_cast<void>(catalog.create(spec, snapshot, bounds(),
													  [&checkpointCount] {
														  ++checkpointCount;
														  // The fifth checkpoint is inside adjacency materialization.
														  // The projection object may be partially built, but catalog
														  // publication must not happen until the complete build has
														  // returned.
														  if (checkpointCount == 5U) {
															  throw std::runtime_error("projection build cancelled");
														  }
													  })),
					 std::runtime_error);
		EXPECT_EQ(checkpointCount, 5U);
		EXPECT_FALSE(catalog.exists(spec.name));
		EXPECT_THROW(static_cast<void>(catalog.get(spec.name)), std::out_of_range);

		const LogicalProjectionDescriptor retried = catalog.create(spec, snapshot, bounds());
		EXPECT_EQ(retried.node_count, 2U);
		EXPECT_EQ(retried.edge_count, 1U);
		EXPECT_TRUE(catalog.exists(spec.name));
	}

	TEST(LogicalProjectionCatalogTest, PreparedMutationsPublishOnlyOnOneExplicitCommit) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{42U, 1U, 2U, "CALLS", {}}};
		LogicalProjectionCatalog catalog;
		LogicalGraphProjectionSpec spec;
		spec.name = "prepared";

		uint64_t abandonedSourceRevision = 0U;
		{
			LogicalProjectionCatalogCreatePreparation preparation = catalog.prepareCreate(spec, snapshot, bounds());
			ASSERT_NE(preparation.mutation, nullptr);
			abandonedSourceRevision = preparation.descriptor.source_revision;
			// Scope exit abandons the retained source revision and cannot publish it.
		}
		EXPECT_FALSE(catalog.exists(spec.name));

		LogicalProjectionCatalogCreatePreparation created = catalog.prepareCreate(spec, snapshot, bounds());
		ASSERT_NE(created.mutation, nullptr);
		EXPECT_EQ(created.descriptor.source_revision, abandonedSourceRevision);
		created.mutation->commit();
		EXPECT_THROW(created.mutation->commit(), std::logic_error);
		created.mutation.reset();
		EXPECT_TRUE(catalog.exists(spec.name));

		{
			std::unique_ptr<PreparedLogicalProjectionCatalogMutation> dropped = catalog.prepareDrop(spec.name);
			ASSERT_NE(dropped, nullptr);
			// Abandoning a removal must leave the immutable projection visible.
		}
		EXPECT_TRUE(catalog.exists(spec.name));

		std::unique_ptr<PreparedLogicalProjectionCatalogMutation> dropped = catalog.prepareDrop(spec.name);
		ASSERT_NE(dropped, nullptr);
		dropped->commit();
		EXPECT_THROW(dropped->commit(), std::logic_error);
		dropped.reset();
		EXPECT_FALSE(catalog.exists(spec.name));

		static_cast<void>(catalog.create(spec, snapshot, bounds()));
		{
			LogicalProjectionCatalogDropAllPreparation preparation = catalog.prepareDropAll();
			ASSERT_NE(preparation.mutation, nullptr);
			EXPECT_EQ(preparation.dropped_count, 1U);
			// Destruction leaves the catalog untouched when a procedure pipeline fails.
		}
		EXPECT_TRUE(catalog.exists(spec.name));

		LogicalProjectionCatalogDropAllPreparation cleared = catalog.prepareDropAll();
		ASSERT_NE(cleared.mutation, nullptr);
		EXPECT_EQ(cleared.dropped_count, 1U);
		cleared.mutation->commit();
		EXPECT_THROW(cleared.mutation->commit(), std::logic_error);
		cleared.mutation.reset();
		EXPECT_FALSE(catalog.exists(spec.name));
	}

	TEST(LogicalProjectionCatalogTest, ReusesTheLatestObservedRevisionWhenItBecomesAProjectionSource) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{51U, 1U, 2U, "CALLS", {}}};
		LogicalProjectionCatalog catalog;
		LogicalGraphProjectionSpec anchorSpec;
		anchorSpec.name = "anchor";
		const LogicalProjectionDescriptor anchor = catalog.create(anchorSpec, snapshot, bounds());

		// A catalog can observe arbitrarily many current graph revisions while a
		// projection remains live. Only the latest observation is cacheable; when
		// it becomes a source revision, it must retain that existing opaque token.
		uint64_t latestObservedToken = anchor.current_revision;
		for (uint64_t backendToken = 2U; backendToken <= 256U; ++backendToken) {
			snapshot.revision->backend_token = backendToken;
			const std::vector<LogicalProjectionDescriptor> listed = catalog.list(snapshot, 16U);
			ASSERT_EQ(listed.size(), 1U);
			EXPECT_EQ(listed.front().source_revision, anchor.source_revision);
			EXPECT_TRUE(listed.front().stale);
			EXPECT_GT(listed.front().current_revision, latestObservedToken);
			latestObservedToken = listed.front().current_revision;
		}

		ASSERT_TRUE(catalog.drop("anchor"));
		LogicalGraphProjectionSpec finalSpec;
		finalSpec.name = "final";
		const LogicalProjectionDescriptor finalProjection = catalog.create(finalSpec, snapshot, bounds());
		EXPECT_EQ(finalProjection.source_revision, latestObservedToken);
		EXPECT_EQ(finalProjection.current_revision, latestObservedToken);
		EXPECT_FALSE(finalProjection.stale);
	}

	TEST(LogicalProjectionCatalogTest, BoundsAndCancelsEnumerationWithoutChangingCatalogContents) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{1U, 1U, 2U, "CALLS", {}}};
		LogicalProjectionCatalog catalog;
		LogicalGraphProjectionSpec first;
		first.name = "first";
		LogicalGraphProjectionSpec second;
		second.name = "second";
		static_cast<void>(catalog.create(first, snapshot, bounds()));
		static_cast<void>(catalog.create(second, snapshot, bounds()));

		EXPECT_THROW(static_cast<void>(catalog.list(snapshot, 0U)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(catalog.list(snapshot, 1U)), std::runtime_error);

		size_t checkpointCalls = 0U;
		EXPECT_THROW(static_cast<void>(catalog.list(snapshot, 2U,
													[&checkpointCalls] {
														++checkpointCalls;
														throw std::runtime_error("cancel catalog enumeration");
													})),
					 std::runtime_error);
		EXPECT_EQ(checkpointCalls, 1U);

		checkpointCalls = 0U;
		const std::vector<LogicalProjectionDescriptor> listed =
				catalog.list(snapshot, 2U, [&checkpointCalls] { ++checkpointCalls; });
		EXPECT_EQ(checkpointCalls, 2U);
		ASSERT_EQ(listed.size(), 2U);
		EXPECT_EQ(listed[0U].name, "first");
		EXPECT_EQ(listed[1U].name, "second");
	}

	TEST(LogicalGraphProjectionTest, BuildsOneThreadSafeLazyCsrForConcurrentReaders) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}};
		snapshot.edges = {{1U, 1U, 2U, "CALLS", {}}, {2U, 1U, 3U, "CALLS", {}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "concurrent";
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());
		std::vector<std::shared_ptr<const LogicalProjectionCsr>> results(8U);
		std::vector<std::thread> threads;
		threads.reserve(results.size());
		for (size_t index = 0U; index < results.size(); ++index) {
			threads.emplace_back([&projection, &results, index] { results[index] = projection.csr(); });
		}
		for (std::thread &thread: threads) {
			thread.join();
		}
		for (const std::shared_ptr<const LogicalProjectionCsr> &result: results) {
			ASSERT_EQ(result, results.front());
			EXPECT_EQ(result->offsets, (std::vector<size_t>{0U, 2U, 2U, 2U}));
		}
	}

	TEST(LogicalGraphProjectionTest, ReportsAnAtomicLazyCsrCacheStateDuringConcurrentMaterialization) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{1U, 1U, 2U, "CALLS", {}}, {2U, 2U, 1U, "CALLS", {}}, {3U, 1U, 1U, "CALLS", {}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "csr-state";
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());

		EXPECT_EQ(projection.csrCacheState(),
				  (LogicalProjectionCsrCacheState{.materialized = false, .memory_bytes = 0U}));
		std::atomic<bool> started = false;
		std::atomic<bool> finished = false;
		std::thread materializer([&projection, &started, &finished] {
			started.store(true, std::memory_order_release);
			static_cast<void>(projection.csr());
			finished.store(true, std::memory_order_release);
		});
		while (!started.load(std::memory_order_acquire)) {
		}
		while (!finished.load(std::memory_order_acquire)) {
			const LogicalProjectionCsrCacheState state = projection.csrCacheState();
			EXPECT_EQ(state.materialized, state.memory_bytes != 0U);
		}
		materializer.join();
		const LogicalProjectionCsrCacheState finalState = projection.csrCacheState();
		EXPECT_TRUE(finalState.materialized);
		EXPECT_GT(finalState.memory_bytes, 0U);
	}

	TEST(LogicalProjectionCatalogTest, SupportsConcurrentPublicationAndObservationOfImmutableEntries) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{1U, 1U, 2U, "CALLS", {}}};
		LogicalProjectionCatalog catalog;
		constexpr size_t CREATOR_COUNT = 8U;
		std::vector<std::thread> creators;
		std::vector<std::exception_ptr> failures(CREATOR_COUNT);
		std::atomic<bool> creatorsStarted = false;
		std::atomic<bool> observerSawInvalidState = false;
		std::thread observer([&] {
			while (!creatorsStarted.load(std::memory_order_acquire)) {
			}
			for (size_t iteration = 0U; iteration < 64U; ++iteration) {
				const auto descriptors = catalog.list(snapshot, CREATOR_COUNT);
				if (descriptors.size() > CREATOR_COUNT ||
					!std::ranges::is_sorted(descriptors, {}, &LogicalProjectionDescriptor::name)) {
					observerSawInvalidState.store(true, std::memory_order_release);
					return;
				}
			}
		});
		for (size_t index = 0U; index < CREATOR_COUNT; ++index) {
			// Snapshot instrumentation records mutable scan vectors. Give each
			// creator its own reader for the same immutable graph/revision.
			creators.emplace_back([&catalog, snapshot, &failures, index] {
				try {
					LogicalGraphProjectionSpec spec;
					spec.name = "projection-" + std::to_string(index);
					static_cast<void>(catalog.create(spec, snapshot, bounds()));
				} catch (...) {
					failures[index] = std::current_exception();
				}
			});
		}
		creatorsStarted.store(true, std::memory_order_release);
		for (std::thread &creator: creators) {
			creator.join();
		}
		observer.join();
		EXPECT_FALSE(observerSawInvalidState.load(std::memory_order_acquire));
		for (const std::exception_ptr &failure: failures) {
			EXPECT_EQ(failure, nullptr);
		}
		EXPECT_EQ(catalog.list(snapshot, CREATOR_COUNT).size(), CREATOR_COUNT);
	}

	TEST(LogicalProjectionCatalogTest, DropAllPublishesOnlyTheCompleteOldOrEmptyCatalogToConcurrentReaders) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{1U, 1U, 2U, "CALLS", {}}};
		LogicalProjectionCatalog catalog;
		constexpr size_t PROJECTION_COUNT = 16U;
		for (size_t index = 0U; index < PROJECTION_COUNT; ++index) {
			LogicalGraphProjectionSpec spec;
			spec.name = "drop-all-" + std::to_string(index);
			static_cast<void>(catalog.create(spec, snapshot, bounds()));
		}

		std::atomic<bool> start = false;
		std::atomic<bool> dropped = false;
		std::atomic<bool> invalidObservation = false;
		std::vector<std::thread> readers;
		readers.reserve(4U);
		for (size_t readerIndex = 0U; readerIndex < 4U; ++readerIndex) {
			readers.emplace_back([&] {
				while (!start.load(std::memory_order_acquire)) {
				}
				for (size_t iteration = 0U; iteration < 256U; ++iteration) {
					const std::vector<LogicalProjectionDescriptor> descriptors =
							catalog.list(snapshot, PROJECTION_COUNT);
					if ((descriptors.size() != PROJECTION_COUNT && !descriptors.empty()) ||
						!std::ranges::is_sorted(descriptors, {}, &LogicalProjectionDescriptor::name)) {
						invalidObservation.store(true, std::memory_order_release);
						return;
					}
					for (const LogicalProjectionDescriptor &descriptor: descriptors) {
						if (!descriptor.name.starts_with("drop-all-") || descriptor.node_count != 2U ||
							descriptor.edge_count != 1U) {
							invalidObservation.store(true, std::memory_order_release);
							return;
						}
						try {
							const std::shared_ptr<const LogicalGraphProjection> projection =
									catalog.get(descriptor.name);
							if (projection->nodeCount() != 2U || projection->edgeCount() != 1U) {
								invalidObservation.store(true, std::memory_order_release);
								return;
							}
						} catch (const std::out_of_range &) {
							// dropAll may publish after list() returned its valid old snapshot.
						}
					}
					if (dropped.load(std::memory_order_acquire) && descriptors.empty()) {
						return;
					}
				}
			});
		}

		std::thread dropper([&] {
			while (!start.load(std::memory_order_acquire)) {
			}
			EXPECT_EQ(catalog.dropAll(), PROJECTION_COUNT);
			dropped.store(true, std::memory_order_release);
		});
		start.store(true, std::memory_order_release);
		dropper.join();
		for (std::thread &reader: readers) {
			reader.join();
		}

		EXPECT_FALSE(invalidObservation.load(std::memory_order_acquire));
		EXPECT_TRUE(dropped.load(std::memory_order_acquire));
		EXPECT_TRUE(catalog.list(snapshot, PROJECTION_COUNT).empty());
	}

	TEST(LogicalGraphAlgorithmsTest, ComputesDeterministicWccPageRankAndWeightedDijkstraFromCsr) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}, {4U, {"Code"}}, {5U, {"Code"}}};
		snapshot.edges = {
				{1U, 1U, 2U, "CALLS", {property("cost", PropertyValue(1.0))}},
				{2U, 2U, 3U, "CALLS", {property("cost", PropertyValue(2.0))}},
				{3U, 1U, 3U, "CALLS", {property("cost", PropertyValue(10.0))}},
				{4U, 4U, 5U, "CALLS", {property("cost", PropertyValue(4.0))}},
		};
		LogicalGraphProjectionSpec spec;
		spec.name = "algorithms";
		spec.relationships = {
				{.type = "CALLS",
				 .weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY, .property_name = "cost"}}};
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());

		const std::vector<LogicalGdsNodeComponent> components =
				LogicalGraphAlgorithms::weaklyConnectedComponents(projection, algorithmBounds());
		ASSERT_EQ(components.size(), 5U);
		EXPECT_EQ(components[0], (LogicalGdsNodeComponent{1U, 1U}));
		EXPECT_EQ(components[1], (LogicalGdsNodeComponent{2U, 1U}));
		EXPECT_EQ(components[2], (LogicalGdsNodeComponent{3U, 1U}));
		EXPECT_EQ(components[3], (LogicalGdsNodeComponent{4U, 4U}));
		EXPECT_EQ(components[4], (LogicalGdsNodeComponent{5U, 4U}));

		const LogicalGdsWeightedPath path = LogicalGraphAlgorithms::dijkstra(projection, 1U, 3U, algorithmBounds());
		EXPECT_EQ(path.node_ids, (std::vector<uint64_t>{1U, 2U, 3U}));
		EXPECT_EQ(path.step_weights, (std::vector<double>{0.0, 1.0, 2.0}));
		EXPECT_DOUBLE_EQ(path.total_weight, 3.0);
		auto stateLimited = algorithmBounds();
		stateLimited.maximum_materialized_bytes = 120U; // 5 Nodes x (distance, parent, parent weight).
		try {
			static_cast<void>(LogicalGraphAlgorithms::dijkstra(projection, 1U, 3U, stateLimited));
			FAIL() << "Dijkstra accepted its frontier beyond the configured byte limit";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical Dijkstra exceeded its materialized state byte bound");
		}
		stateLimited.maximum_materialized_bytes = 4096U;
		EXPECT_EQ(LogicalGraphAlgorithms::dijkstra(projection, 1U, 3U, stateLimited), path);
		EXPECT_TRUE(LogicalGraphAlgorithms::dijkstra(projection, 1U, 99U, algorithmBounds()).node_ids.empty());

		const std::vector<LogicalGdsNodeScore> scores = LogicalGraphAlgorithms::pageRank(
				projection, {.maximum_iterations = 32U, .damping_factor = 0.85, .tolerance = 1.0e-12},
				algorithmBounds());
		ASSERT_EQ(scores.size(), 5U);
		double total = 0.0;
		for (size_t index = 0U; index < scores.size(); ++index) {
			total += scores[index].score;
			if (index != 0U) {
				EXPECT_GE(scores[index - 1U].score, scores[index].score);
			}
		}
		EXPECT_NEAR(total, 1.0, 1.0e-9);
		const auto nodeThree = std::ranges::find(scores, 3U, &LogicalGdsNodeScore::node_id);
		const auto nodeOne = std::ranges::find(scores, 1U, &LogicalGdsNodeScore::node_id);
		ASSERT_NE(nodeThree, scores.end());
		ASSERT_NE(nodeOne, scores.end());
		EXPECT_GT(nodeThree->score, nodeOne->score);
		auto byteLimited = algorithmBounds();
		byteLimited.maximum_materialized_bytes = 159U; // 5 Nodes x two score arrays plus returned scores.
		try {
			static_cast<void>(LogicalGraphAlgorithms::pageRank(
					projection, {.maximum_iterations = 32U, .damping_factor = 0.85, .tolerance = 1.0e-12}, byteLimited));
			FAIL() << "PageRank allocated its state and result beyond the configured byte limit";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical PageRank exceeded its materialized byte bound");
		}
		byteLimited.maximum_materialized_bytes = 119U; // 5 parent slots plus returned components.
		try {
			static_cast<void>(LogicalGraphAlgorithms::weaklyConnectedComponents(projection, byteLimited));
			FAIL() << "WCC allocated its parent and result arrays beyond the configured byte limit";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical WCC exceeded its materialized byte bound");
		}
	}

	TEST(LogicalGraphAlgorithmsTest, RejectsProjectionAndLookupIndexBoundsBeforeMaterializingCsr) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "algorithm-preflight";
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());
		ASSERT_FALSE(projection.csrCacheState().materialized);

		auto nodeLimited = algorithmBounds();
		nodeLimited.maximum_nodes = 2U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::weaklyConnectedComponents(projection, nodeLimited)),
				 std::runtime_error);
		EXPECT_FALSE(projection.csrCacheState().materialized);

		auto byteLimited = algorithmBounds();
		byteLimited.maximum_materialized_bytes = 3U *
				(sizeof(std::pair<const uint64_t, size_t>) + 3U * sizeof(void *)) - 1U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::pageRank(projection, {}, byteLimited)),
				 std::runtime_error);
		EXPECT_FALSE(projection.csrCacheState().materialized);
	}

	TEST(LogicalGraphAlgorithmsTest, ChecksCancellationWhileValidatingCsrRouting) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}};
		snapshot.edges = {{1U, 1U, 2U, "CALLS", {}}, {2U, 2U, 3U, "CALLS", {}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "cancel-csr-validation";
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());
		size_t checkpoints = 0U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::weaklyConnectedComponents(
				projection, algorithmBounds(), [&checkpoints] {
					if (++checkpoints == 3U) throw std::runtime_error("cancel CSR validation");
				})), std::runtime_error);
		EXPECT_EQ(checkpoints, 3U);
		EXPECT_TRUE(projection.csrCacheState().materialized);
		EXPECT_EQ(LogicalGraphAlgorithms::weaklyConnectedComponents(projection, algorithmBounds()).size(), 3U);
	}

	TEST(LogicalGraphAlgorithmsTest, KeepsDijkstraParentChainsAcyclicAcrossZeroWeightCycles) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}, {4U, {"Code"}}};
		snapshot.edges = {
				{1U, 3U, 2U, "CALLS", {property("cost", PropertyValue(0.0))}},
				{2U, 3U, 1U, "CALLS", {property("cost", PropertyValue(0.0))}},
				{3U, 1U, 2U, "CALLS", {property("cost", PropertyValue(0.0))}},
				{4U, 2U, 1U, "CALLS", {property("cost", PropertyValue(0.0))}},
				{5U, 2U, 4U, "CALLS", {property("cost", PropertyValue(1.0))}},
		};
		LogicalGraphProjectionSpec spec;
		spec.name = "dijkstra-zero-cycle";
		spec.relationships = {
				{.type = "CALLS",
				 .weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY, .property_name = "cost"}}};
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());

		const LogicalGdsWeightedPath first = LogicalGraphAlgorithms::dijkstra(projection, 3U, 4U, algorithmBounds());
		EXPECT_EQ(first.node_ids, (std::vector<uint64_t>{3U, 2U, 4U}));
		EXPECT_EQ(first.step_weights, (std::vector<double>{0.0, 0.0, 1.0}));
		EXPECT_DOUBLE_EQ(first.total_weight, 1.0);
		EXPECT_EQ(first, LogicalGraphAlgorithms::dijkstra(projection, 3U, 4U, algorithmBounds()));
	}

	TEST(LogicalGraphAlgorithmsTest, HandlesZeroEdgeSelfLoopAndOverflowAlgorithmBoundaries) {
		Snapshot edgeless;
		edgeless.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "edgeless-algorithm-boundaries";
		const LogicalGraphProjection edgelessProjection = LogicalGraphProjection::build(edgeless, spec, bounds());
		EXPECT_EQ(LogicalGraphAlgorithms::betweennessCentrality(
					edgelessProjection, {.sampling_size = 2U}, algorithmBounds()).size(),
				  2U);
		auto insufficientWork = algorithmBounds();
		insufficientWork.maximum_work_units = 1U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::betweennessCentrality(
					edgelessProjection, {}, insufficientWork)), std::runtime_error);

		Snapshot selfLoop;
		selfLoop.nodes = {{1U, {"Code"}}};
		selfLoop.edges = {{1U, 1U, 1U, "CALLS", {}}};
		spec.name = "self-loop-algorithm-boundaries";
		const LogicalGraphProjection selfLoopProjection = LogicalGraphProjection::build(selfLoop, spec, bounds());
		EXPECT_EQ(LogicalGraphAlgorithms::betweennessCentrality(selfLoopProjection, {}, algorithmBounds()).size(), 1U);
		EXPECT_EQ(LogicalGraphAlgorithms::closenessCentrality(selfLoopProjection, algorithmBounds()).size(), 1U);

		Snapshot overflowing;
		overflowing.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}};
		overflowing.edges = {
				{1U, 1U, 2U, "CALLS", {property("cost", PropertyValue(std::numeric_limits<double>::max()))}},
				{2U, 2U, 3U, "CALLS", {property("cost", PropertyValue(std::numeric_limits<double>::max()))}},
		};
		spec.name = "dijkstra-overflow";
		spec.relationships = {
				{.type = "CALLS",
				 .weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY, .property_name = "cost"}}};
		const LogicalGraphProjection overflowingProjection = LogicalGraphProjection::build(overflowing, spec, bounds());
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::dijkstra(
					overflowingProjection, 1U, 3U, algorithmBounds())), std::runtime_error);
	}

	TEST(LogicalGraphAlgorithmsTest, PreservesRandomizedWccDijkstraAndPageRankProperties) {
		std::mt19937_64 random(0xC02D2U);
		for (size_t trial = 0U; trial < 16U; ++trial) {
			Snapshot snapshot;
			constexpr uint64_t NODE_COUNT = 6U;
			for (uint64_t nodeId = 1U; nodeId <= NODE_COUNT; ++nodeId) {
				snapshot.nodes.push_back({nodeId, {"Code"}});
			}
			uint64_t edgeId = 1U;
			for (uint64_t source = 1U; source <= NODE_COUNT; ++source) {
				for (uint64_t target = 1U; target <= NODE_COUNT; ++target) {
					if (source != target && (random() % 100U) < 32U) {
						const double weight = static_cast<double>((random() % 7U) + 1U);
						snapshot.edges.push_back(
								{edgeId++, source, target, "CALLS", {property("cost", PropertyValue(weight))}});
					}
				}
			}

			LogicalGraphProjectionSpec spec;
			spec.name = "random-algorithms-" + std::to_string(trial);
			spec.relationships = {
					{.type = "CALLS",
					 .weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY, .property_name = "cost"}}};
			const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());

			// Use an independent union-find oracle for weak connectivity.
			std::vector<size_t> parent(NODE_COUNT);
			std::iota(parent.begin(), parent.end(), 0U);
			auto find = [&parent](size_t value) {
				size_t root = value;
				while (parent[root] != root) {
					root = parent[root];
				}
				while (parent[value] != value) {
					const size_t next = parent[value];
					parent[value] = root;
					value = next;
				}
				return root;
			};
			for (const storage::LogicalGraphEdge &edge: snapshot.edges) {
				const size_t sourceRoot = find(static_cast<size_t>(edge.source_node_id - 1U));
				const size_t targetRoot = find(static_cast<size_t>(edge.target_node_id - 1U));
				if (sourceRoot != targetRoot) {
					parent[std::max(sourceRoot, targetRoot)] = std::min(sourceRoot, targetRoot);
				}
			}
			const std::vector<LogicalGdsNodeComponent> components =
					LogicalGraphAlgorithms::weaklyConnectedComponents(projection, algorithmBounds());
			ASSERT_EQ(components.size(), NODE_COUNT) << "trial=" << trial;
			for (const LogicalGdsNodeComponent &component: components) {
				EXPECT_EQ(component.component_id,
						  static_cast<uint64_t>(find(static_cast<size_t>(component.node_id - 1U)) + 1U))
						<< "trial=" << trial;
			}

			// Independently compute the directed shortest-path distance used by Dijkstra.
			std::vector<double> distances(NODE_COUNT, std::numeric_limits<double>::infinity());
			std::vector<std::vector<std::pair<size_t, double>>> adjacency(NODE_COUNT);
			for (const storage::LogicalGraphEdge &edge: snapshot.edges) {
				adjacency[static_cast<size_t>(edge.source_node_id - 1U)].emplace_back(
						static_cast<size_t>(edge.target_node_id - 1U),
						std::get<double>(edge.properties.front().value.getVariant()));
			}
			using PendingDistance = std::pair<double, size_t>;
			std::priority_queue<PendingDistance, std::vector<PendingDistance>, std::greater<>> pending;
			distances.front() = 0.0;
			pending.emplace(0.0, 0U);
			while (!pending.empty()) {
				const auto [distance, source] = pending.top();
				pending.pop();
				if (distance != distances[source]) {
					continue;
				}
				for (const auto [target, weight]: adjacency[source]) {
					if (distance + weight < distances[target]) {
						distances[target] = distance + weight;
						pending.emplace(distances[target], target);
					}
				}
			}
			const LogicalGdsWeightedPath path =
					LogicalGraphAlgorithms::dijkstra(projection, 1U, NODE_COUNT, algorithmBounds());
			if (std::isfinite(distances.back())) {
				ASSERT_FALSE(path.node_ids.empty()) << "trial=" << trial;
				ASSERT_EQ(path.node_ids.front(), 1U) << "trial=" << trial;
				ASSERT_EQ(path.node_ids.back(), NODE_COUNT) << "trial=" << trial;
				ASSERT_EQ(path.node_ids.size(), path.step_weights.size()) << "trial=" << trial;
				EXPECT_DOUBLE_EQ(path.total_weight, distances.back()) << "trial=" << trial;
			} else {
				EXPECT_TRUE(path.node_ids.empty()) << "trial=" << trial;
			}

			const LogicalPageRankOptions options{
					.maximum_iterations = 8U, .damping_factor = 0.85, .tolerance = 1.0e-12};
			const std::vector<LogicalGdsNodeScore> first =
					LogicalGraphAlgorithms::pageRank(projection, options, algorithmBounds());
			EXPECT_EQ(first, LogicalGraphAlgorithms::pageRank(projection, options, algorithmBounds()))
					<< "trial=" << trial;
			double total = 0.0;
			for (const LogicalGdsNodeScore &score: first) {
				EXPECT_TRUE(std::isfinite(score.score)) << "trial=" << trial;
				EXPECT_GE(score.score, 0.0) << "trial=" << trial;
				total += score.score;
			}
			EXPECT_NEAR(total, 1.0, 1.0e-12) << "trial=" << trial;
		}
	}

	TEST(LogicalGraphAlgorithmsTest, PreservesRandomizedUndirectedCentralityMetamorphicProperties) {
		std::mt19937_64 random(0xCE17A1U);
		for (size_t trial = 0U; trial < 16U; ++trial) {
			Snapshot forward;
			constexpr uint64_t NODE_COUNT = 5U;
			for (uint64_t nodeId = 1U; nodeId <= NODE_COUNT; ++nodeId) {
				forward.nodes.push_back({nodeId, {"Code"}});
			}
			uint64_t edgeId = 1U;
			for (uint64_t source = 1U; source <= NODE_COUNT; ++source) {
				for (uint64_t target = source + 1U; target <= NODE_COUNT; ++target) {
					if ((random() % 100U) < 45U) {
						forward.edges.push_back({edgeId++, source, target, "CALLS", {}});
					}
				}
			}

			Snapshot reversed = forward;
			for (storage::LogicalGraphEdge &edge: reversed.edges) {
				std::swap(edge.source_node_id, edge.target_node_id);
			}
			LogicalGraphProjectionSpec spec;
			spec.name = "random-centrality-" + std::to_string(trial);
			const LogicalGraphProjection forwardProjection = LogicalGraphProjection::build(forward, spec, bounds());
			spec.name += "-reversed";
			const LogicalGraphProjection reversedProjection = LogicalGraphProjection::build(reversed, spec, bounds());

			const LogicalBetweennessOptions sampled{.sampling_size = 3U};
			const std::vector<LogicalGdsNodeScore> betweenness =
					LogicalGraphAlgorithms::betweennessCentrality(forwardProjection, sampled, algorithmBounds());
			EXPECT_EQ(betweenness,
					  LogicalGraphAlgorithms::betweennessCentrality(forwardProjection, sampled, algorithmBounds()))
					<< "trial=" << trial;
			EXPECT_EQ(betweenness,
					  LogicalGraphAlgorithms::betweennessCentrality(reversedProjection, sampled, algorithmBounds()))
					<< "trial=" << trial;
			const std::vector<LogicalGdsNodeScore> fullBetweenness =
					LogicalGraphAlgorithms::betweennessCentrality(forwardProjection, {}, algorithmBounds());

			const std::vector<LogicalGdsNodeScore> closeness =
					LogicalGraphAlgorithms::closenessCentrality(forwardProjection, algorithmBounds());
			EXPECT_EQ(closeness, LogicalGraphAlgorithms::closenessCentrality(reversedProjection, algorithmBounds()))
					<< "trial=" << trial;

			// Independently enumerate every unweighted shortest path in this tiny
			// undirected graph. This checks the centrality values themselves rather
			// than only their determinism and direction-reversal invariants.
			std::vector<std::vector<size_t>> adjacency(NODE_COUNT);
			for (const storage::LogicalGraphEdge &edge: forward.edges) {
				const size_t source = static_cast<size_t>(edge.source_node_id - 1U);
				const size_t target = static_cast<size_t>(edge.target_node_id - 1U);
				adjacency[source].push_back(target);
				adjacency[target].push_back(source);
			}
			std::vector<double> expectedBetweenness(NODE_COUNT, 0.0);
			std::vector<double> expectedCloseness(NODE_COUNT, 0.0);
			for (size_t source = 0U; source < NODE_COUNT; ++source) {
				std::vector<size_t> distance(NODE_COUNT, NODE_COUNT);
				std::vector<size_t> queue = {source};
				distance[source] = 0U;
				for (size_t head = 0U; head < queue.size(); ++head) {
					for (const size_t target: adjacency[queue[head]]) {
						if (distance[target] == NODE_COUNT) {
							distance[target] = distance[queue[head]] + 1U;
							queue.push_back(target);
						}
					}
				}
				size_t reachable = 0U;
				size_t totalDistance = 0U;
				for (size_t target = 0U; target < NODE_COUNT; ++target) {
					if (target != source && distance[target] != NODE_COUNT) {
						++reachable;
						totalDistance += distance[target];
					}
				}
				expectedCloseness[source] = reachable == 0U ? 0.0 :
						static_cast<double>(reachable) / static_cast<double>(totalDistance);

				for (size_t target = source + 1U; target < NODE_COUNT; ++target) {
					if (distance[target] == NODE_COUNT) {
						continue;
					}
					std::vector<size_t> path = {source};
					std::vector<size_t> pathVisits(NODE_COUNT, 0U);
					size_t shortestPathCount = 0U;
					std::function<void(size_t)> enumerateShortestPaths = [&](const size_t current) {
						if (current == target) {
							++shortestPathCount;
							for (size_t index = 1U; index + 1U < path.size(); ++index) {
								++pathVisits[path[index]];
							}
							return;
						}
						for (const size_t next: adjacency[current]) {
							if (distance[next] == distance[current] + 1U) {
								path.push_back(next);
								enumerateShortestPaths(next);
								path.pop_back();
							}
						}
					};
					enumerateShortestPaths(source);
					ASSERT_GT(shortestPathCount, 0U) << "trial=" << trial;
					for (size_t index = 0U; index < NODE_COUNT; ++index) {
						expectedBetweenness[index] +=
								static_cast<double>(pathVisits[index]) / static_cast<double>(shortestPathCount);
					}
				}
			}
			for (size_t index = 0U; index < NODE_COUNT; ++index) {
				const auto actualBetweenness =
						std::ranges::find(fullBetweenness, static_cast<uint64_t>(index + 1U), &LogicalGdsNodeScore::node_id);
				const auto actualCloseness =
						std::ranges::find(closeness, static_cast<uint64_t>(index + 1U), &LogicalGdsNodeScore::node_id);
				ASSERT_NE(actualBetweenness, fullBetweenness.end()) << "trial=" << trial;
				ASSERT_NE(actualCloseness, closeness.end()) << "trial=" << trial;
				EXPECT_DOUBLE_EQ(actualBetweenness->score, expectedBetweenness[index]) << "trial=" << trial;
				EXPECT_DOUBLE_EQ(actualCloseness->score, expectedCloseness[index]) << "trial=" << trial;
			}
			for (const LogicalGdsNodeScore &score: betweenness) {
				EXPECT_TRUE(std::isfinite(score.score)) << "trial=" << trial;
				EXPECT_GE(score.score, 0.0) << "trial=" << trial;
			}
			for (const LogicalGdsNodeScore &score: closeness) {
				EXPECT_TRUE(std::isfinite(score.score)) << "trial=" << trial;
				EXPECT_GE(score.score, 0.0) << "trial=" << trial;
			}
		}
	}

	TEST(LogicalGraphAlgorithmsTest, RejectsInvalidOptionsAndTerminatesAtTheConfiguredWorkBound) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{1U, 1U, 2U, "CALLS", {}}, {2U, 2U, 1U, "CALLS", {}}, {3U, 1U, 1U, "CALLS", {}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "bounds";
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());
		auto invalid = algorithmBounds();
		invalid.maximum_work_units = 2U;
		EXPECT_THROW(
				static_cast<void>(LogicalGraphAlgorithms::pageRank(projection, {.maximum_iterations = 2U}, invalid)),
				std::runtime_error);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::weaklyConnectedComponents(projection, invalid)),
					 std::runtime_error);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::dijkstra(projection, 1U, 2U, invalid)),
					 std::runtime_error);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::pageRank(projection, {.maximum_iterations = 0U},
																		algorithmBounds())),
					 std::invalid_argument);
		bool checkpointCalled = false;
		EXPECT_NO_THROW(static_cast<void>(LogicalGraphAlgorithms::dijkstra(
				projection, 1U, 2U, algorithmBounds(), [&checkpointCalled] { checkpointCalled = true; })));
		EXPECT_TRUE(checkpointCalled);
	}

	TEST(LogicalGraphAlgorithmsTest, HandlesEmptyProjectionsAndRejectsInvalidPublicAlgorithmInputs) {
		Snapshot emptySnapshot;
		LogicalGraphProjectionSpec emptySpec;
		emptySpec.name = "empty-algorithms";
		const LogicalGraphProjection emptyProjection =
				LogicalGraphProjection::build(emptySnapshot, emptySpec, bounds());
		EXPECT_TRUE(LogicalGraphAlgorithms::pageRank(emptyProjection, {}, algorithmBounds()).empty());
		EXPECT_TRUE(LogicalGraphAlgorithms::weaklyConnectedComponents(emptyProjection, algorithmBounds()).empty());
		EXPECT_TRUE(LogicalGraphAlgorithms::betweennessCentrality(emptyProjection, {}, algorithmBounds()).empty());
		EXPECT_TRUE(LogicalGraphAlgorithms::closenessCentrality(emptyProjection, algorithmBounds()).empty());
		EXPECT_TRUE(LogicalGraphAlgorithms::leiden(emptyProjection, {}, algorithmBounds()).empty());

		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}, {4U, {"Code"}}};
		snapshot.edges = {
				{1U, 1U, 2U, "CALLS", {property("weight", PropertyValue(10.0))}},
				{2U, 1U, 3U, "CALLS", {property("weight", PropertyValue(1.0))}},
				{3U, 3U, 2U, "CALLS", {property("weight", PropertyValue(1.0))}},
				{4U, 2U, 2U, "CALLS", {property("weight", PropertyValue(1.0))}},
		};
		LogicalGraphProjectionSpec spec;
		spec.name = "algorithm-inputs";
		spec.relationships = {
				{.type = "CALLS",
				 .weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY, .property_name = "weight"}}};
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());

		auto invalidBounds = algorithmBounds();
		invalidBounds.maximum_nodes = 0U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::pageRank(projection, {}, invalidBounds)),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::pageRank(
							 projection,
							 {.maximum_iterations = 1U, .damping_factor = std::numeric_limits<double>::infinity()},
							 algorithmBounds())),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::pageRank(
							 projection, {.maximum_iterations = 1U, .damping_factor = -0.01}, algorithmBounds())),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::pageRank(
							 projection, {.maximum_iterations = 1U, .damping_factor = 1.01}, algorithmBounds())),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::pageRank(
							 projection, {.maximum_iterations = 1U, .damping_factor = 0.85, .tolerance = 0.0},
							 algorithmBounds())),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::pageRank(
							 projection, {.maximum_iterations = 33U}, algorithmBounds())), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::pageRank(
							 projection,
							 {.maximum_iterations = 1U, .tolerance = std::numeric_limits<double>::infinity()},
							 algorithmBounds())),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leiden(
							 projection, {.maximum_iterations = 1U, .maximum_levels = 1U, .resolution = 0.0},
							 algorithmBounds())),
					 std::invalid_argument);
		EXPECT_THROW(
				static_cast<void>(LogicalGraphAlgorithms::leiden(
							 projection, {.maximum_iterations = 1U, .maximum_levels = 1U, .refinement_threshold = -0.01},
							 algorithmBounds())),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leiden(
							 projection, {.maximum_iterations = 33U, .maximum_levels = 1U}, algorithmBounds())),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leiden(
							 projection,
							 {.maximum_iterations = 1U, .maximum_levels = 1U,
							  .resolution = std::numeric_limits<double>::infinity()},
							 algorithmBounds())),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leiden(
							 projection,
							 {.maximum_iterations = 1U, .maximum_levels = 1U,
							  .refinement_threshold = std::numeric_limits<double>::infinity()},
							 algorithmBounds())),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leidenModularity(
							 projection, {{1U, 1U}, {1U, 2U}, {3U, 3U}, {4U, 4U}})),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leidenModularity(
							 projection, {{1U, 1U}, {2U, 2U}, {3U, 3U}, {99U, 4U}})),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leidenModularity(
							 projection, {{1U, 1U}, {2U, 2U}, {3U, 3U}, {4U, 4U}}, 0.0)),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leidenModularity(
							 projection, {{0U, 1U}, {2U, 2U}, {3U, 3U}, {4U, 4U}})),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leidenModularity(
							 projection, {{1U, 0U}, {2U, 2U}, {3U, 3U}, {4U, 4U}})),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leidenModularity(
							 projection, {{1U, 1U}, {2U, 2U}, {3U, 3U}})),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leidenModularity(
							 projection, {{1U, 1U}, {2U, 2U}, {3U, 3U}, {4U, 4U}},
							 std::numeric_limits<double>::infinity())),
					 std::invalid_argument);

		auto nodeBound = algorithmBounds();
		nodeBound.maximum_nodes = 3U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::dijkstra(projection, 1U, 2U, nodeBound)),
					 std::runtime_error);
		auto edgeBound = algorithmBounds();
		edgeBound.maximum_edges = 3U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::dijkstra(projection, 1U, 2U, edgeBound)),
					 std::runtime_error);
		for (auto [maximumNodes, maximumEdges, maximumIterations, maximumWork]:
				 std::array<std::array<size_t, 4U>, 3U>{{{16U, 0U, 32U, 512U},
																	 {16U, 64U, 0U, 512U},
																	 {16U, 64U, 32U, 0U}}}) {
			LogicalGraphAlgorithmBounds invalid = algorithmBounds();
			invalid.maximum_nodes = maximumNodes;
			invalid.maximum_edges = maximumEdges;
			invalid.maximum_iterations = maximumIterations;
			invalid.maximum_work_units = maximumWork;
			EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::dijkstra(projection, 1U, 2U, invalid)),
						 std::invalid_argument);
		}
		EXPECT_TRUE(LogicalGraphAlgorithms::dijkstra(projection, 99U, 1U, algorithmBounds()).node_ids.empty());

		// CSR is immutable to normal callers. This test injects a corrupted
		// in-memory cache frame to ensure modularity uses the same fail-closed
		// validation boundary as every executable algorithm.
		const std::shared_ptr<const LogicalProjectionCsr> originalCsr = projection.csr();
		auto corruptedCsr = std::const_pointer_cast<LogicalProjectionCsr>(originalCsr);
		const std::vector<LogicalGdsNodeCommunity> validCommunities =
				{{1U, 1U}, {2U, 2U}, {3U, 3U}, {4U, 4U}};
		const auto expectMalformedCsr = [&] {
			EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leidenModularity(projection, validCommunities)),
						 std::runtime_error);
		};

		const std::vector<size_t> originalOffsets = corruptedCsr->offsets;
		corruptedCsr->offsets.clear();
		expectMalformedCsr();
		corruptedCsr->offsets = originalOffsets;
		corruptedCsr->offsets.pop_back();
		expectMalformedCsr();
		corruptedCsr->offsets = originalOffsets;
		corruptedCsr->offsets.front() = 1U;
		expectMalformedCsr();
		corruptedCsr->offsets = originalOffsets;
		corruptedCsr->offsets[1U] = corruptedCsr->edgeCount();
		expectMalformedCsr();
		corruptedCsr->offsets = originalOffsets;
		corruptedCsr->offsets[1U] = corruptedCsr->edgeCount() + 1U;
		expectMalformedCsr();
		corruptedCsr->offsets = originalOffsets;

		const std::vector<uint64_t> originalNodeIds = corruptedCsr->node_ids;
		corruptedCsr->node_ids.front() = 0U;
		expectMalformedCsr();
		corruptedCsr->node_ids = originalNodeIds;
		corruptedCsr->node_ids[1U] = corruptedCsr->node_ids.front();
		expectMalformedCsr();
		corruptedCsr->node_ids = originalNodeIds;

		const std::vector<double> originalWeights = corruptedCsr->weights;
		corruptedCsr->weights.pop_back();
		expectMalformedCsr();
		corruptedCsr->weights = originalWeights;
		corruptedCsr->weights.front() = std::numeric_limits<double>::infinity();
		expectMalformedCsr();
		corruptedCsr->weights = originalWeights;

		const std::vector<uint64_t> originalRelationshipIds = corruptedCsr->relationship_ids;
		corruptedCsr->relationship_ids.pop_back();
		expectMalformedCsr();
		corruptedCsr->relationship_ids = originalRelationshipIds;

		const std::vector<uint8_t> originalSyntheticReverse = corruptedCsr->synthetic_reverse;
		corruptedCsr->synthetic_reverse.pop_back();
		expectMalformedCsr();
		corruptedCsr->synthetic_reverse = originalSyntheticReverse;
		ASSERT_FALSE(corruptedCsr->synthetic_reverse.empty());
		corruptedCsr->synthetic_reverse.front() = uint8_t{2U};
		expectMalformedCsr();
		corruptedCsr->synthetic_reverse.front() = uint8_t{0U};

		const size_t originalFinalOffset = corruptedCsr->offsets.back();
		corruptedCsr->offsets.back() = originalFinalOffset - 1U;
		expectMalformedCsr();
		corruptedCsr->offsets.back() = originalFinalOffset;

		const uint64_t originalTarget = corruptedCsr->target_node_ids.front();
		corruptedCsr->target_node_ids.front() = 99U;
		expectMalformedCsr();
		corruptedCsr->target_node_ids.front() = originalTarget;

		corruptedCsr->weights.front() = -1.0;
		expectMalformedCsr();
		corruptedCsr->weights = originalWeights;

		// The direct path to Node 2 is deliberately superseded through Node 3.
		// Looking for disconnected Node 4 drains the stale frontier entry before
		// returning the documented no-path result.
		EXPECT_TRUE(LogicalGraphAlgorithms::dijkstra(projection, 1U, 4U, algorithmBounds()).node_ids.empty());
	}

	TEST(LogicalGraphAlgorithmsTest, ComputesDeterministicUndirectedBetweennessAndClosenessFromDirectedCsr) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}, {4U, {"Code"}}};
		snapshot.edges = {{1U, 1U, 2U, "CALLS", {}}, {2U, 2U, 3U, "CALLS", {}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "centrality";
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());

		const std::vector<LogicalGdsNodeScore> betweenness =
				LogicalGraphAlgorithms::betweennessCentrality(projection, {}, algorithmBounds());
		ASSERT_EQ(betweenness.size(), 4U);
		const auto middle = std::ranges::find(betweenness, 2U, &LogicalGdsNodeScore::node_id);
		const auto first = std::ranges::find(betweenness, 1U, &LogicalGdsNodeScore::node_id);
		const auto isolated = std::ranges::find(betweenness, 4U, &LogicalGdsNodeScore::node_id);
		ASSERT_NE(middle, betweenness.end());
		ASSERT_NE(first, betweenness.end());
		ASSERT_NE(isolated, betweenness.end());
		EXPECT_DOUBLE_EQ(middle->score, 1.0);
		EXPECT_DOUBLE_EQ(first->score, 0.0);
		EXPECT_DOUBLE_EQ(isolated->score, 0.0);
		EXPECT_EQ(LogicalGraphAlgorithms::betweennessCentrality(projection, {.sampling_size = 2U}, algorithmBounds()),
				  LogicalGraphAlgorithms::betweennessCentrality(projection, {.sampling_size = 2U}, algorithmBounds()));
		auto betweennessByteBound = algorithmBounds();
		betweennessByteBound.maximum_materialized_bytes = 1U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::betweennessCentrality(
				projection, {}, betweennessByteBound)),
				 std::runtime_error);

		const std::vector<LogicalGdsNodeScore> closeness =
				LogicalGraphAlgorithms::closenessCentrality(projection, algorithmBounds());
		const auto closenessMiddle = std::ranges::find(closeness, 2U, &LogicalGdsNodeScore::node_id);
		const auto closenessFirst = std::ranges::find(closeness, 1U, &LogicalGdsNodeScore::node_id);
		const auto closenessIsolated = std::ranges::find(closeness, 4U, &LogicalGdsNodeScore::node_id);
		ASSERT_NE(closenessMiddle, closeness.end());
		ASSERT_NE(closenessFirst, closeness.end());
		ASSERT_NE(closenessIsolated, closeness.end());
		EXPECT_DOUBLE_EQ(closenessMiddle->score, 1.0);
		EXPECT_DOUBLE_EQ(closenessFirst->score, 2.0 / 3.0);
		EXPECT_DOUBLE_EQ(closenessIsolated->score, 0.0);
		auto byteBound = algorithmBounds();
		byteBound.maximum_materialized_bytes = 1U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::closenessCentrality(projection, byteBound)),
				 std::runtime_error);

		auto workBound = algorithmBounds();
		workBound.maximum_work_units = 1U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::betweennessCentrality(projection, {}, workBound)),
					 std::runtime_error);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::closenessCentrality(projection, workBound)),
					 std::runtime_error);
	}

	TEST(LogicalGraphAlgorithmsTest, RefinesLeidenCommunitiesAndUsesStableOriginalNodeIds) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}},
						  {4U, {"Code"}}, {5U, {"Code"}}, {6U, {"Code"}}};
		snapshot.edges = {
				{1U, 1U, 2U, "CALLS", {}}, {2U, 2U, 3U, "CALLS", {}}, {3U, 3U, 1U, "CALLS", {}},
				{4U, 4U, 5U, "CALLS", {}}, {5U, 5U, 6U, "CALLS", {}}, {6U, 6U, 4U, "CALLS", {}},
				{7U, 1U, 4U, "CALLS", {}},
		};
		LogicalGraphProjectionSpec spec;
		spec.name = "leiden";
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());
		const std::vector<LogicalGdsNodeCommunity> communities = LogicalGraphAlgorithms::leiden(
				projection,
				{.maximum_iterations = 8U, .maximum_levels = 4U, .resolution = 1.0, .refinement_threshold = 0.01},
				algorithmBounds());
		ASSERT_EQ(communities.size(), 6U);
		std::unordered_map<uint64_t, uint64_t> byNode;
		for (const LogicalGdsNodeCommunity &community: communities) {
			byNode.emplace(community.node_id, community.community_id);
		}
		EXPECT_EQ(byNode.at(1U), byNode.at(2U));
		EXPECT_EQ(byNode.at(2U), byNode.at(3U));
		EXPECT_EQ(byNode.at(4U), byNode.at(5U));
		EXPECT_EQ(byNode.at(5U), byNode.at(6U));
		EXPECT_NE(byNode.at(1U), byNode.at(4U));
		EXPECT_EQ(byNode.at(1U), 1U);
		EXPECT_EQ(byNode.at(4U), 4U);
		const std::vector<LogicalGdsNodeCommunity> singleton = {{1U, 1U}, {2U, 2U}, {3U, 3U},
																{4U, 4U}, {5U, 5U}, {6U, 6U}};
		EXPECT_GE(LogicalGraphAlgorithms::leidenModularity(projection, communities),
				  LogicalGraphAlgorithms::leidenModularity(projection, singleton) - 1.0e-12);
		EXPECT_EQ(LogicalGraphAlgorithms::leiden(projection,
												 {.maximum_iterations = 8U,
												  .maximum_levels = 4U,
												  .resolution = 1.0,
												  .refinement_threshold = 0.01},
												 algorithmBounds()),
				  communities);
		EXPECT_THROW(static_cast<void>(
							 LogicalGraphAlgorithms::leiden(projection, {.maximum_iterations = 0U}, algorithmBounds())),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leiden(
							 projection, {.maximum_iterations = 1U, .maximum_levels = 0U}, algorithmBounds())),
					 std::invalid_argument);
		auto constrained = algorithmBounds();
		constrained.maximum_work_units = 1U;
		EXPECT_THROW(
				static_cast<void>(LogicalGraphAlgorithms::leiden(projection, {.maximum_iterations = 1U}, constrained)),
				std::runtime_error);
		auto byteLimited = algorithmBounds();
		byteLimited.maximum_materialized_bytes = 1U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leiden(
					projection, {.maximum_iterations = 8U, .maximum_levels = 4U}, byteLimited)),
				std::runtime_error);
	}

	TEST(LogicalGraphAlgorithmsTest, AppliesLeidenRefinementThresholdAtTheDocumentedSplitGainBoundary) {
		const std::vector<std::vector<std::pair<size_t, double>>> adjacency = {
				{{1U, 1.0}}, {{0U, 1.0}}, {{3U, 1.0}}, {{2U, 1.0}}};
		const std::vector<double> degree = {1.0, 1.0, 1.0, 1.0};
		const double totalWeight = 4.0;
		size_t usedWork = 0U;

		std::vector<size_t> accepted = {0U, 0U, 0U, 0U};
		LogicalGraphAlgorithmsTestAccess::refineDisconnectedCommunities(
				adjacency, accepted, degree, totalWeight, 1.0, 0.5, usedWork, algorithmBounds());
		EXPECT_EQ(accepted[0U], accepted[1U]);
		EXPECT_EQ(accepted[2U], accepted[3U]);
		EXPECT_NE(accepted[0U], accepted[2U]);

		std::vector<size_t> rejected = {0U, 0U, 0U, 0U};
		usedWork = 0U;
		LogicalGraphAlgorithmsTestAccess::refineDisconnectedCommunities(
				adjacency, rejected, degree, totalWeight, 1.0, 0.500001, usedWork, algorithmBounds());
		EXPECT_EQ(rejected, (std::vector<size_t>{0U, 0U, 0U, 0U}));

		std::vector<size_t> disabled = {0U, 0U, 0U, 0U};
		usedWork = 0U;
		LogicalGraphAlgorithmsTestAccess::refineDisconnectedCommunities(
				adjacency, disabled, degree, totalWeight, 1.0, 0.0, usedWork, algorithmBounds());
		EXPECT_EQ(disabled, (std::vector<size_t>{0U, 0U, 0U, 0U}));
	}

	TEST(LogicalGraphAlgorithmsTest, RejectsMalformedLeidenRefinementInputsBeforeMutatingAssignments) {
		const std::vector<std::vector<std::pair<size_t, double>>> adjacency = {{{1U, 1.0}}, {{0U, 1.0}}};
		const std::vector<double> degree = {1.0, 1.0};
		const auto expectInvalid = [&](const auto &candidateAdjacency, const auto &candidateDegree,
				const double totalWeight, const double resolution, const double threshold) {
			std::vector<size_t> community = {0U, 0U};
			const std::vector<size_t> before = community;
			size_t usedWork = 0U;
			EXPECT_THROW(LogicalGraphAlgorithmsTestAccess::refineDisconnectedCommunities(
						 candidateAdjacency, community, candidateDegree, totalWeight, resolution, threshold, usedWork,
						 algorithmBounds()), std::invalid_argument);
			EXPECT_EQ(community, before);
		};
		expectInvalid(std::vector<std::vector<std::pair<size_t, double>>>{{{1U, 1.0}}}, degree, 2.0, 1.0, 0.1);
		expectInvalid(adjacency, std::vector<double>{1.0}, 2.0, 1.0, 0.1);
		expectInvalid(adjacency, degree, std::numeric_limits<double>::infinity(), 1.0, 0.1);
		expectInvalid(adjacency, degree, -1.0, 1.0, 0.1);
		expectInvalid(adjacency, degree, 2.0, 0.0, 0.1);
		expectInvalid(adjacency, degree, 2.0, 1.0, -0.1);

		std::vector<size_t> community = {0U, 0U};
		size_t usedWork = 0U;
		const std::vector<std::vector<std::pair<size_t, double>>> invalidNeighbor = {{{2U, 1.0}}, {{0U, 1.0}}};
		EXPECT_THROW(LogicalGraphAlgorithmsTestAccess::refineDisconnectedCommunities(
					 invalidNeighbor, community, degree, 2.0, 1.0, 0.1, usedWork, algorithmBounds()),
				 std::invalid_argument);
		community = {0U, 0U};
		usedWork = 0U;
		const std::vector<std::vector<std::pair<size_t, double>>> invalidWeight = {
				{{1U, std::numeric_limits<double>::quiet_NaN()}}, {{0U, 1.0}}};
		EXPECT_THROW(LogicalGraphAlgorithmsTestAccess::refineDisconnectedCommunities(
					 invalidWeight, community, degree, 2.0, 1.0, 0.1, usedWork, algorithmBounds()),
				 std::invalid_argument);
	}

	TEST(LogicalGraphAlgorithmsTest, RejectsLeidenWeightAggregationOverflow) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {
				{1U, 1U, 2U, "CALLS", {property("weight", PropertyValue(std::numeric_limits<double>::max()))}},
				{2U, 1U, 2U, "CALLS", {property("weight", PropertyValue(std::numeric_limits<double>::max()))}},
		};
		LogicalGraphProjectionSpec spec;
		spec.name = "leiden-weight-overflow";
		spec.relationships = {{.type = "CALLS",
				.weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY, .property_name = "weight"}}};
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leiden(projection, {}, algorithmBounds())),
				 std::runtime_error);
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leidenModularity(
					projection, {{1U, 1U}, {2U, 2U}})), std::runtime_error);
	}

	TEST(LogicalGraphAlgorithmsTest, PreservesLeidenDeterminismAssignmentsAndModularityAcrossRandomGraphs) {
		std::mt19937_64 random(0x5A17U);
		concurrent::ThreadPool twoWorkerPool(2U);
		concurrent::ThreadPool fourWorkerPool(4U);
		auto generousBounds = algorithmBounds();
		generousBounds.maximum_work_units = 16'384U;
		for (size_t trial = 0U; trial < 12U; ++trial) {
			Snapshot snapshot;
			for (uint64_t id = 1U; id <= 8U; ++id)
				snapshot.nodes.push_back({id, {"Code"}});
			uint64_t edgeId = 1U;
			for (uint64_t source = 1U; source <= 8U; ++source) {
				for (uint64_t target = 1U; target <= 8U; ++target) {
					if (source != target && (random() % 100U) < 28U) {
						snapshot.edges.push_back({edgeId++, source, target, "CALLS", {}});
					}
				}
			}
			if (snapshot.edges.empty())
				snapshot.edges.push_back({edgeId, 1U, 2U, "CALLS", {}});
			LogicalGraphProjectionSpec spec;
			spec.name = "random-" + std::to_string(trial);
			const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());
			const LogicalLeidenOptions options{
					.maximum_iterations = 6U, .maximum_levels = 4U, .resolution = 1.0, .refinement_threshold = 0.01};
			const std::vector<LogicalGdsNodeCommunity> first =
					LogicalGraphAlgorithms::leiden(projection, options, generousBounds);
			const std::vector<LogicalGdsNodeCommunity> second =
					LogicalGraphAlgorithms::leiden(projection, options, generousBounds);
			EXPECT_EQ(first, second) << "trial=" << trial;
			// Degree accumulation is the only parallel phase today, but its result
			// must be independent of pool width and the caller's worker cap.
			for (concurrent::ThreadPool *pool: std::array{&twoWorkerPool, &fourWorkerPool}) {
				for (const size_t concurrency: {0U, 1U, 2U, 4U, 64U}) {
					auto parallelOptions = options;
					parallelOptions.concurrency = concurrency;
					EXPECT_EQ(first, LogicalGraphAlgorithms::leiden(projection, parallelOptions, generousBounds, {}, pool))
							<< "trial=" << trial << " pool=" << pool->getThreadCount()
							<< " concurrency=" << concurrency;
				}
			}
			ASSERT_EQ(first.size(), 8U);
			std::vector<LogicalGdsNodeCommunity> singleton;
			singleton.reserve(8U);
			for (uint64_t id = 1U; id <= 8U; ++id)
				singleton.push_back({id, id});
			const double initial = LogicalGraphAlgorithms::leidenModularity(projection, singleton);
			const double final = LogicalGraphAlgorithms::leidenModularity(projection, first);
			EXPECT_TRUE(std::isfinite(final));
			EXPECT_GE(final, initial - 1.0e-12) << "trial=" << trial;
			for (const LogicalGdsNodeCommunity &community: first) {
				EXPECT_GE(community.node_id, 1U);
				EXPECT_LE(community.node_id, 8U);
				EXPECT_GE(community.community_id, 1U);
				EXPECT_LE(community.community_id, 8U);
			}
		}
	}

	TEST(LogicalGraphAlgorithmsTest, TreatsSyntheticUndirectedArcsAsOneRelationshipUnderAWorkBudget) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}};
		snapshot.edges = {{1U, 1U, 2U, "CALLS", {}}};
		LogicalGraphProjectionSpec spec;
		spec.name = "undirected-leiden";
		spec.relationships = {{.type = "CALLS", .orientation = LogicalProjectionOrientation::LPO_UNDIRECTED}};
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());

		// The projection stores one real and one synthetic reverse arc. Leiden's
		// weakly-undirected view must consume that relationship exactly once rather
		// than charging and weighting it twice.
		auto tightBounds = algorithmBounds();
		tightBounds.maximum_work_units = 10U;
		const std::vector<LogicalGdsNodeCommunity> communities =
				LogicalGraphAlgorithms::leiden(projection, {}, tightBounds);
		ASSERT_EQ(communities.size(), 2U);
		EXPECT_EQ(communities[0U].community_id, communities[1U].community_id);
		EXPECT_NEAR(LogicalGraphAlgorithms::leidenModularity(projection, communities), 0.0, 1.0e-12);

		const std::vector<LogicalGdsNodeScore> closeness =
				LogicalGraphAlgorithms::closenessCentrality(projection, algorithmBounds());
		ASSERT_EQ(closeness.size(), 2U);
		EXPECT_DOUBLE_EQ(closeness[0U].score, 1.0);
		EXPECT_DOUBLE_EQ(closeness[1U].score, 1.0);
	}

	TEST(LogicalGraphAlgorithmsTest, HandlesEdgelessAndWeightedLeidenInputsAndHonorsCancellation) {
		Snapshot edgeless;
		edgeless.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}};
		LogicalGraphProjectionSpec edgelessSpec;
		edgelessSpec.name = "edgeless-leiden";
		const LogicalGraphProjection edgelessProjection =
				LogicalGraphProjection::build(edgeless, edgelessSpec, bounds());
		const std::vector<LogicalGdsNodeCommunity> edgelessCommunities =
				LogicalGraphAlgorithms::leiden(edgelessProjection, {}, algorithmBounds());
		ASSERT_EQ(edgelessCommunities.size(), 3U);
		EXPECT_EQ(edgelessCommunities[0U].community_id, 1U);
		EXPECT_EQ(edgelessCommunities[1U].community_id, 2U);
		EXPECT_EQ(edgelessCommunities[2U].community_id, 3U);
		EXPECT_DOUBLE_EQ(LogicalGraphAlgorithms::leidenModularity(edgelessProjection, edgelessCommunities), 0.0);

		Snapshot weighted;
		weighted.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}, {4U, {"Code"}}};
		weighted.edges = {
				{1U, 1U, 2U, "CALLS", {property("weight", PropertyValue(8.0))}},
				{2U, 1U, 2U, "CALLS", {property("weight", PropertyValue(2.0))}},
				{3U, 3U, 4U, "CALLS", {property("weight", PropertyValue(8.0))}},
				{4U, 2U, 3U, "CALLS", {property("weight", PropertyValue(0.1))}},
				{5U, 1U, 1U, "CALLS", {property("weight", PropertyValue(1.0))}},
		};
		LogicalGraphProjectionSpec weightedSpec;
		weightedSpec.name = "weighted-leiden";
		weightedSpec.relationships = {
				{.type = "CALLS",
				 .weight = {.kind = LogicalProjectionWeightKind::LPWK_PROPERTY, .property_name = "weight"}}};
		const LogicalGraphProjection weightedProjection =
				LogicalGraphProjection::build(weighted, weightedSpec, bounds());
		const std::vector<LogicalGdsNodeCommunity> weightedCommunities = LogicalGraphAlgorithms::leiden(
				weightedProjection, {.maximum_iterations = 12U, .maximum_levels = 4U}, algorithmBounds());
		ASSERT_EQ(weightedCommunities.size(), 4U);
		std::unordered_map<uint64_t, uint64_t> byNode;
		for (const LogicalGdsNodeCommunity &community: weightedCommunities) {
			byNode.emplace(community.node_id, community.community_id);
		}
		EXPECT_EQ(byNode.at(1U), byNode.at(2U));
		EXPECT_EQ(byNode.at(3U), byNode.at(4U));
		EXPECT_NE(byNode.at(1U), byNode.at(3U));

		size_t checkpointCount = 0U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leiden(weightedProjection, {}, algorithmBounds(),
																	  [&checkpointCount] {
																		  ++checkpointCount;
																		  throw std::runtime_error("cancelled");
																	  })),
					 std::runtime_error);
		EXPECT_EQ(checkpointCount, 1U);
	}

	TEST(LogicalGraphAlgorithmsTest, CancelsAfterAlgorithmWorkAndLeavesProjectionReusable) {
		Snapshot snapshot;
		snapshot.nodes = {{1U, {"Code"}}, {2U, {"Code"}}, {3U, {"Code"}}, {4U, {"Code"}}};
		snapshot.edges = {
				{1U, 1U, 2U, "CALLS", {}},
				{2U, 2U, 3U, "CALLS", {}},
				{3U, 3U, 4U, "CALLS", {}},
		};
		LogicalGraphProjectionSpec spec;
		spec.name = "mid-algorithm-cancellation";
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, bounds());
		const std::shared_ptr<const LogicalProjectionCsr> originalCsr = projection.csr();

		// Each threshold is beyond that algorithm's entry checkpoint, proving that
		// cancellation is observed while its local working state already exists.
		size_t pageRankChecks = 0U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::pageRank(
							 projection, {.maximum_iterations = 8U, .tolerance = 1.0e-12}, algorithmBounds(),
							 [&pageRankChecks] {
								 ++pageRankChecks;
								 if (pageRankChecks == 2U)
									 throw std::runtime_error("PageRank cancelled");
							 })),
					 std::runtime_error);
		EXPECT_EQ(pageRankChecks, 2U);

		size_t leidenChecks = 0U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leiden(
							 projection, {.maximum_iterations = 4U, .maximum_levels = 2U}, algorithmBounds(),
							 [&leidenChecks] {
								 ++leidenChecks;
								 if (leidenChecks == 6U)
									 throw std::runtime_error("Leiden cancelled");
							 })),
					 std::runtime_error);
		EXPECT_EQ(leidenChecks, 6U);

		size_t dijkstraChecks = 0U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::dijkstra(projection, 1U, 4U, algorithmBounds(),
																		[&dijkstraChecks] {
																			++dijkstraChecks;
																			if (dijkstraChecks == 2U)
																				throw std::runtime_error(
																						"Dijkstra cancelled");
																		})),
					 std::runtime_error);
		EXPECT_EQ(dijkstraChecks, 2U);

		// Logical algorithms are read-only. A cancelled invocation must not change
		// the immutable CSR or make a later invocation unusable.
		EXPECT_EQ(projection.csr(), originalCsr);
		EXPECT_EQ(LogicalGraphAlgorithms::pageRank(projection, {.maximum_iterations = 8U, .tolerance = 1.0e-12},
												   algorithmBounds())
						  .size(),
				  4U);
		EXPECT_EQ(LogicalGraphAlgorithms::leiden(projection, {.maximum_iterations = 4U, .maximum_levels = 2U},
												 algorithmBounds())
						  .size(),
				  4U);
		const LogicalGdsWeightedPath path = LogicalGraphAlgorithms::dijkstra(projection, 1U, 4U, algorithmBounds());
		EXPECT_EQ(path.node_ids, (std::vector<uint64_t>{1U, 2U, 3U, 4U}));
		EXPECT_DOUBLE_EQ(path.total_weight, 3.0);
	}

	TEST(LogicalGraphAlgorithmsTest, ChecksCancellationBetweenParallelLeidenDegreeBatches) {
		Snapshot snapshot;
		constexpr uint64_t NODE_COUNT = 300U;
		for (uint64_t nodeId = 1U; nodeId <= NODE_COUNT; ++nodeId) {
			snapshot.nodes.push_back({nodeId, {"Code"}});
			if (nodeId > 1U) {
				snapshot.edges.push_back({nodeId - 1U, nodeId - 1U, nodeId, "CALLS", {}});
			}
		}
		LogicalGraphProjectionSpec spec;
		spec.name = "parallel-degree-cancellation";
		const LogicalProjectionBuildBounds projectionBounds{
				.maximum_nodes = static_cast<size_t>(NODE_COUNT),
				.maximum_metadata_pages = 1U,
				.maximum_edges_per_node = 2U};
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, projectionBounds);
		const LogicalGraphAlgorithmBounds largeBounds{
				.maximum_nodes = static_cast<size_t>(NODE_COUNT),
				.maximum_edges = static_cast<size_t>(NODE_COUNT - 1U),
				.maximum_iterations = 32U,
				.maximum_work_units = 16U * static_cast<size_t>(NODE_COUNT)};
		concurrent::ThreadPool pool(2U);

		size_t checkpointCount = 0U;
		EXPECT_THROW(static_cast<void>(LogicalGraphAlgorithms::leiden(
						 projection, {.maximum_iterations = 4U, .maximum_levels = 2U, .concurrency = 2U}, largeBounds,
						 [&checkpointCount] {
							 ++checkpointCount;
							 // The build phase accounts for one checkpoint per Node plus
							 // one level entry. The next check occurs on the caller before
							 // dispatching the first bounded parallel degree batch.
							 if (checkpointCount == NODE_COUNT + 2U) {
								 throw std::runtime_error("parallel Leiden cancellation");
							 }
						 },
						 &pool)),
				 std::runtime_error);
		EXPECT_EQ(checkpointCount, NODE_COUNT + 2U);
		EXPECT_EQ(projection.csr()->node_ids.size(), static_cast<size_t>(NODE_COUNT));
	}

	TEST(LogicalGraphAlgorithmsTest, PreservesDisjointPairCommunitiesAcrossNativeParallelDegreeScanning) {
		// This is deliberately larger than the small randomized fixtures: it ports
		// the legacy parallel-pair invariant through the native logical projection
		// boundary, without depending on a physical storage layout.
		constexpr uint64_t NODE_COUNT = 1024U;
		Snapshot snapshot;
		snapshot.nodes.reserve(static_cast<size_t>(NODE_COUNT));
		snapshot.edges.reserve(static_cast<size_t>(NODE_COUNT / 2U));
		for (uint64_t nodeId = 1U; nodeId <= NODE_COUNT; ++nodeId) {
			snapshot.nodes.push_back({nodeId, {"Code"}});
			if ((nodeId & 1U) == 0U) {
				snapshot.edges.push_back({nodeId / 2U, nodeId - 1U, nodeId, "CALLS", {}});
			}
		}

		LogicalGraphProjectionSpec spec;
		spec.name = "parallel-disjoint-pairs";
		const LogicalProjectionBuildBounds projectionBounds{
			.maximum_nodes = static_cast<size_t>(NODE_COUNT),
			.maximum_metadata_pages = 1U,
			.maximum_edges_per_node = 2U};
		const LogicalGraphProjection projection = LogicalGraphProjection::build(snapshot, spec, projectionBounds);
		const LogicalGraphAlgorithmBounds parallelBounds{
			.maximum_nodes = static_cast<size_t>(NODE_COUNT),
			.maximum_edges = static_cast<size_t>(NODE_COUNT / 2U),
			.maximum_iterations = 8U,
			.maximum_work_units = 32U * static_cast<size_t>(NODE_COUNT)};
		const LogicalLeidenOptions options{
			.maximum_iterations = 8U,
			.maximum_levels = 2U,
			.resolution = 1.0,
			.refinement_threshold = 0.01,
			.concurrency = 4U};

		concurrent::ThreadPool pool(4U);
		const std::vector<LogicalGdsNodeCommunity> serial =
				LogicalGraphAlgorithms::leiden(projection, options, parallelBounds);
		const std::vector<LogicalGdsNodeCommunity> parallel =
				LogicalGraphAlgorithms::leiden(projection, options, parallelBounds, {}, &pool);
		EXPECT_EQ(parallel, serial);
		ASSERT_EQ(parallel.size(), static_cast<size_t>(NODE_COUNT));

		std::unordered_map<uint64_t, uint64_t> communityByNode;
		communityByNode.reserve(parallel.size());
		for (const LogicalGdsNodeCommunity &community: parallel) {
			ASSERT_TRUE(communityByNode.emplace(community.node_id, community.community_id).second);
		}
		std::unordered_set<uint64_t> distinctCommunities;
		for (uint64_t nodeId = 1U; nodeId <= NODE_COUNT; nodeId += 2U) {
			EXPECT_EQ(communityByNode.at(nodeId), communityByNode.at(nodeId + 1U));
			distinctCommunities.insert(communityByNode.at(nodeId));
		}
		EXPECT_EQ(distinctCommunities.size(), static_cast<size_t>(NODE_COUNT / 2U));
	}

} // namespace graph::query::gds
