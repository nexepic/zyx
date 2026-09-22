#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"

namespace graph::query::execution {
	namespace {

		using storage::EntityRef;
		using storage::LogicalGraphAdjacencyPage;
		using storage::LogicalGraphEdge;
		using storage::LogicalGraphEdgeTopology;
		using storage::LogicalGraphNode;
		using storage::LogicalGraphNodePage;
		using storage::LogicalGraphReadBackend;
		using storage::LogicalGraphReadDirection;
		using storage::LogicalGraphReadSnapshot;

		struct DirectEdgeGraph {
			std::map<uint64_t, LogicalGraphNode> nodes;
			std::vector<LogicalGraphEdge> edges;
			std::vector<uint64_t> node_read_ids;
			std::vector<uint64_t> edge_read_ids;
			size_t node_shaped_reads = 0U;
			size_t edge_shaped_reads = 0U;
			size_t topology_scans = 0U;
			size_t topology_records = 0U;
			size_t edge_ref_scans = 0U;
			size_t edge_ref_records = 0U;
			bool duplicate_first_edge_reference = false;
			bool truncate_node_existence_batch = false;
			bool truncate_node_label_batch = false;
			bool truncate_node_property_batch = false;
			bool truncate_edge_property_batch = false;
			std::optional<EntityRef> forced_node_reference;
			std::optional<EntityRef> forced_edge_reference;
			size_t adjacency_ref_scans = 0U;
			size_t adjacency_ref_records = 0U;
			size_t batch_adjacency_ref_scans = 0U;
			size_t batch_adjacency_ref_records = 0U;
			size_t node_existence_probes = 0U;
			size_t node_existence_batch_probes = 0U;
			bool enable_adjacency_reference_pages = false;
			bool enable_batch_adjacency_reference_scan = false;
			std::optional<uint64_t> forced_edge_reference_type_token;
			size_t adjacency_scans = 0U;
			size_t node_property_point_reads = 0U;
			size_t node_property_sorted_reads = 0U;
			std::vector<std::vector<uint64_t>> node_property_sorted_ids;
			size_t node_property_column_batch_reads = 0U;
			std::vector<std::vector<std::string>> node_property_column_batch_keys;
			size_t node_label_point_reads = 0U;
			size_t node_label_match_scans = 0U;
			size_t node_label_match_records = 0U;
			size_t node_label_sorted_reads = 0U;
			std::vector<std::vector<uint64_t>> node_label_sorted_ids;
			size_t edge_property_point_reads = 0U;
			size_t edge_property_sorted_reads = 0U;
			std::vector<std::vector<uint64_t>> edge_property_sorted_ids;
			size_t edge_property_column_batch_reads = 0U;
			std::vector<std::vector<std::string>> edge_property_column_batch_keys;
		};

		[[nodiscard]] const PropertyValue *
		findProperty(const std::vector<storage::LogicalGraphPropertyEntry> &properties, std::string_view key) {
			const auto found = std::ranges::find(properties, key, &storage::LogicalGraphPropertyEntry::key);
			return found == properties.end() ? nullptr : &found->value;
		}

		class DirectEdgeSnapshot final : public LogicalGraphReadSnapshot {
		public:
			explicit DirectEdgeSnapshot(DirectEdgeGraph &graph) : graph_(graph) {}

			[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override {
				graph_.node_read_ids.push_back(nodeId);
				return graph_.nodes.at(nodeId);
			}

			[[nodiscard]] LogicalGraphNode readNodeShaped(uint64_t nodeId,
														  const storage::EntityReadShape &shape) const override {
				++graph_.node_shaped_reads;
				graph_.node_read_ids.push_back(nodeId);
				EXPECT_TRUE(shape.include_labels || shape.all_properties);
				return graph_.nodes.at(nodeId);
			}

			[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override {
				graph_.edge_read_ids.push_back(edgeId);
				const auto found = std::ranges::find(graph_.edges, edgeId, &LogicalGraphEdge::edge_id);
				if (found == graph_.edges.end()) {
					throw std::out_of_range("direct Edge test record is absent");
				}
				return *found;
			}

			[[nodiscard]] LogicalGraphEdge readEdgeShaped(uint64_t edgeId,
														  const storage::EntityReadShape &shape) const override {
				++graph_.edge_shaped_reads;
				graph_.edge_read_ids.push_back(edgeId);
				EXPECT_TRUE(shape.include_type);
				const auto found = std::ranges::find(graph_.edges, edgeId, &LogicalGraphEdge::edge_id);
				if (found == graph_.edges.end()) {
					throw std::out_of_range("direct Edge test record is absent");
				}
				return *found;
			}

			[[nodiscard]] PropertyValue readNodeProperty(uint64_t nodeId, std::string_view key) const override {
				++graph_.node_property_point_reads;
				const PropertyValue *value = findProperty(graph_.nodes.at(nodeId).properties, key);
				return value == nullptr ? PropertyValue() : *value;
			}

			[[nodiscard]] std::vector<PropertyValue>
			readNodePropertiesSorted(const std::span<const uint64_t> nodeIds,
									 const std::string_view key) const override {
				++graph_.node_property_sorted_reads;
				graph_.node_property_sorted_ids.emplace_back(nodeIds.begin(), nodeIds.end());
				std::vector<PropertyValue> result;
				result.reserve(nodeIds.size());
				uint64_t previous = 0U;
				for (const uint64_t nodeId: nodeIds) {
					if (nodeId == 0U || nodeId <= previous) {
						throw std::invalid_argument("direct Node test sorted Property IDs are invalid");
					}
					previous = nodeId;
					const PropertyValue *value = findProperty(graph_.nodes.at(nodeId).properties, key);
					result.push_back(value == nullptr ? PropertyValue() : *value);
				}
				return result;
			}

			[[nodiscard]] std::vector<std::vector<PropertyValue>>
			readNodePropertyColumnsSorted(const std::span<const uint64_t> nodeIds,
										  const std::span<const std::string_view> keys) const override {
				++graph_.node_property_column_batch_reads;
				std::vector<std::string> copiedKeys;
				copiedKeys.reserve(keys.size());
				for (const std::string_view key: keys) {
					copiedKeys.emplace_back(key);
				}
				graph_.node_property_column_batch_keys.push_back(std::move(copiedKeys));
				std::vector<std::vector<PropertyValue>> columns(keys.size());
				for (size_t column = 0U; column < keys.size(); ++column) {
					columns[column].reserve(nodeIds.size());
					for (const uint64_t nodeId: nodeIds) {
						const PropertyValue *value = findProperty(graph_.nodes.at(nodeId).properties, keys[column]);
						columns[column].push_back(value == nullptr ? PropertyValue() : *value);
					}
				}
				if (graph_.truncate_node_property_batch && !columns.empty() && !columns.front().empty()) {
					columns.front().pop_back();
				}
				return columns;
			}

			[[nodiscard]] PropertyValue readEdgeProperty(uint64_t edgeId, std::string_view key) const override {
				++graph_.edge_property_point_reads;
				const LogicalGraphEdge edge = readEdge(edgeId);
				const PropertyValue *value = findProperty(edge.properties, key);
				return value == nullptr ? PropertyValue() : *value;
			}

			[[nodiscard]] std::vector<PropertyValue>
			readEdgePropertiesSorted(const std::span<const uint64_t> edgeIds,
									 const std::string_view key) const override {
				++graph_.edge_property_sorted_reads;
				graph_.edge_property_sorted_ids.emplace_back(edgeIds.begin(), edgeIds.end());
				std::vector<PropertyValue> result;
				result.reserve(edgeIds.size());
				uint64_t previous = 0U;
				for (const uint64_t edgeId: edgeIds) {
					if (edgeId == 0U || edgeId <= previous) {
						throw std::invalid_argument("direct Edge test sorted Property IDs are invalid");
					}
					previous = edgeId;
					const LogicalGraphEdge edge = readEdge(edgeId);
					const PropertyValue *value = findProperty(edge.properties, key);
					result.push_back(value == nullptr ? PropertyValue() : *value);
				}
				return result;
			}

			[[nodiscard]] std::vector<std::vector<PropertyValue>>
			readEdgePropertyColumnsSorted(const std::span<const uint64_t> edgeIds,
										  const std::span<const std::string_view> keys) const override {
				++graph_.edge_property_column_batch_reads;
				std::vector<std::string> copiedKeys;
				copiedKeys.reserve(keys.size());
				for (const std::string_view key: keys) {
					copiedKeys.emplace_back(key);
				}
				graph_.edge_property_column_batch_keys.push_back(std::move(copiedKeys));
				std::vector<std::vector<PropertyValue>> columns(keys.size());
				for (size_t column = 0U; column < keys.size(); ++column) {
					columns[column].reserve(edgeIds.size());
					for (const uint64_t edgeId: edgeIds) {
						const LogicalGraphEdge edge = readEdge(edgeId);
						const PropertyValue *value = findProperty(edge.properties, keys[column]);
						columns[column].push_back(value == nullptr ? PropertyValue() : *value);
					}
				}
				if (graph_.truncate_edge_property_batch && !columns.empty() && !columns.front().empty()) {
					columns.front().pop_back();
				}
				return columns;
			}

			[[nodiscard]] bool nodeHasLabels(uint64_t nodeId,
											 const std::vector<std::string> &requiredLabels) const override {
				++graph_.node_label_point_reads;
				const auto &actual = graph_.nodes.at(nodeId).labels;
				return std::ranges::all_of(requiredLabels, [&actual](const std::string &required) {
					return !required.empty() && std::ranges::find(actual, required) != actual.end();
				});
			}

			[[nodiscard]] std::vector<bool>
			readNodeLabelMatchesSorted(const std::span<const uint64_t> nodeIds,
									   const std::vector<std::string> &requiredLabels) const override {
				++graph_.node_label_sorted_reads;
				graph_.node_label_sorted_ids.emplace_back(nodeIds.begin(), nodeIds.end());
				std::vector<bool> result;
				result.reserve(nodeIds.size());
				uint64_t previous = 0U;
				for (const uint64_t nodeId: nodeIds) {
					if (nodeId == 0U || nodeId <= previous) {
						throw std::invalid_argument("direct Node test sorted label IDs are invalid");
					}
					previous = nodeId;
					const auto &actual = graph_.nodes.at(nodeId).labels;
					result.push_back(std::ranges::all_of(requiredLabels, [&actual](const std::string &required) {
						return !required.empty() && std::ranges::find(actual, required) != actual.end();
					}));
				}
				if (graph_.truncate_node_label_batch && !result.empty()) {
					result.pop_back();
				}
				return result;
			}

			void visitNodeRefsWithLabelMatches(
					size_t maximumRecords, const std::vector<std::string> &requiredLabels,
					const std::function<bool(const EntityRef &, bool)> &visitor) const override {
				++graph_.node_label_match_scans;
				size_t visited = 0U;
				for (const auto &[nodeId, node]: graph_.nodes) {
					if (visited == maximumRecords) {
						return;
					}
					++visited;
					++graph_.node_label_match_records;
					const bool matches = std::ranges::all_of(requiredLabels, [&node](const std::string &required) {
						return !required.empty() && std::ranges::find(node.labels, required) != node.labels.end();
					});
					const EntityRef reference = graph_.forced_node_reference.value_or(
							EntityRef{nodeId, false, 0U, 0U, 0U});
					if (!visitor(reference, matches)) {
						return;
					}
				}
			}

			void visitNodes(size_t maximumRecords,
							const std::function<bool(const LogicalGraphNode &)> &visitor) const override {
				size_t visited = 0U;
				for (const auto &[id, node]: graph_.nodes) {
					static_cast<void>(id);
					if (visited++ == maximumRecords || !visitor(node)) {
						return;
					}
				}
			}

			void visitNodeRefs(size_t maximumRecords,
							   const std::function<bool(const EntityRef &)> &visitor) const override {
				size_t visited = 0U;
				for (const auto &[nodeId, node]: graph_.nodes) {
					static_cast<void>(node);
					if (visited == maximumRecords) {
						return;
					}
					++visited;
					const EntityRef reference = graph_.forced_node_reference.value_or(
							EntityRef{nodeId, false, 0U, 0U, 0U});
					if (!visitor(reference)) {
						return;
					}
				}
			}

			[[nodiscard]] bool supportsNativeNodeRefScans() const override { return true; }

			[[nodiscard]] bool containsNode(uint64_t nodeId) const override {
				++graph_.node_existence_probes;
				return graph_.nodes.contains(nodeId);
			}
			[[nodiscard]] std::vector<bool> containsNodesSorted(
					const std::span<const uint64_t> nodeIds) const override {
				++graph_.node_existence_batch_probes;
				std::vector<bool> result;
				result.reserve(nodeIds.size());
				uint64_t previous = 0U;
				for (const uint64_t nodeId: nodeIds) {
					if (nodeId == 0U || nodeId <= previous) {
						throw std::invalid_argument("direct Edge test sorted Node probes are invalid");
					}
					previous = nodeId;
					result.push_back(graph_.nodes.contains(nodeId));
				}
				if (graph_.truncate_node_existence_batch && !result.empty()) {
					result.pop_back();
				}
				return result;
			}

			[[nodiscard]] bool supportsNodeExistenceProbes() const override { return true; }

			[[nodiscard]] LogicalGraphNodePage scanNodesAfter(
					uint64_t, size_t, size_t) const override {
				throw std::logic_error("direct Edge test must not use paged Node scans");
			}

			[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdges(
					uint64_t, LogicalGraphReadDirection, std::optional<storage::LogicalGraphAdjacencyContinuation>,
					size_t) const override {
				++graph_.adjacency_scans;
				throw std::logic_error("direct Edge test must not use adjacency pages");
			}

			[[nodiscard]] storage::LogicalGraphAdjacencyReferencePage scanAdjacentEdgeRefs(
				uint64_t nodeId, LogicalGraphReadDirection direction,
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation,
				size_t maximumRecords) const override {
				++graph_.adjacency_ref_scans;
				if (maximumRecords == 0U) {
					throw std::invalid_argument("direct Edge test adjacency reference page size is zero");
				}
				std::vector<const LogicalGraphEdge *> candidates;
				for (const LogicalGraphEdge &edge: graph_.edges) {
					const bool matches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
							? edge.source_node_id == nodeId
							: edge.target_node_id == nodeId;
					if (matches) {
						candidates.push_back(&edge);
					}
				}
				std::ranges::sort(candidates, [](const LogicalGraphEdge *left, const LogicalGraphEdge *right) {
					return left->edge_id < right->edge_id;
				});
				size_t begin = 0U;
				if (continuation.has_value()) {
					const auto next = std::ranges::find(candidates, continuation->next_edge_id,
																											&LogicalGraphEdge::edge_id);
					if (next == candidates.end()) {
						throw std::invalid_argument("direct Edge test continuation does not name the next record");
					}
					begin = static_cast<size_t>(next - candidates.begin());
				}
				storage::LogicalGraphAdjacencyReferencePage result;
				for (size_t index = begin; index < candidates.size(); ++index) {
					const LogicalGraphEdge *edge = candidates[index];
					if (result.edges.size() == maximumRecords) {
						break;
					}
					++graph_.adjacency_ref_records;
					result.edges.push_back({edge->edge_id, true, edge->source_node_id, edge->target_node_id,
							edge->type == "KNOWS" ? 1U : 0U});
				}
				const size_t nextIndex = begin + result.edges.size();
				if (nextIndex < candidates.size() && !result.edges.empty()) {
					result.continuation = storage::LogicalGraphAdjacencyContinuation{
							result.edges.back().entity_id, candidates[nextIndex]->edge_id};
				}
				return result;
			}

			[[nodiscard]] bool supportsNativeAdjacentEdgeRefScans() const override {
				return graph_.enable_adjacency_reference_pages;
			}

			void visitAdjacentEdgeRefsForNodes(
					const std::span<const uint64_t> nodeIds, const LogicalGraphReadDirection direction,
					const size_t maximumEdges, const std::function<bool(const EntityRef &)> &visitor) const override {
				if (!graph_.enable_batch_adjacency_reference_scan || nodeIds.empty() || maximumEdges == 0U || !visitor ||
					!std::ranges::is_sorted(nodeIds) || std::ranges::adjacent_find(nodeIds) != nodeIds.end()) {
					throw std::invalid_argument("direct Edge test batch adjacency request is invalid");
				}
				++graph_.batch_adjacency_ref_scans;
				size_t visited = 0U;
				for (const uint64_t nodeId: nodeIds) {
					for (const LogicalGraphEdge &edge: graph_.edges) {
						const bool matches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
								? edge.source_node_id == nodeId : edge.target_node_id == nodeId;
						if (!matches) {
							continue;
						}
						if (visited == maximumEdges) {
							return;
						}
						++visited;
						++graph_.batch_adjacency_ref_records;
						if (!visitor({edge.edge_id, true, edge.source_node_id, edge.target_node_id,
								edge.type == "KNOWS" ? 1U : 0U})) {
							return;
						}
					}
				}
			}

			[[nodiscard]] bool supportsNativeBatchAdjacentEdgeRefs() const override {
				return graph_.enable_batch_adjacency_reference_scan;
			}

			void visitEdges(size_t maximumRecords,
							const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
				size_t visited = 0U;
				for (const LogicalGraphEdge &edge: graph_.edges) {
					if (visited++ == maximumRecords || !visitor(edge)) {
						return;
					}
				}
			}

			void visitEdgeRefs(size_t maximumRecords,
							   const std::function<bool(const EntityRef &)> &visitor) const override {
				++graph_.edge_ref_scans;
				size_t visited = 0U;
				for (size_t edgeIndex = 0U; edgeIndex < graph_.edges.size(); ++edgeIndex) {
					const LogicalGraphEdge &edge = graph_.edges[edgeIndex];
					if (visited == maximumRecords) {
						return;
					}
					++visited;
					++graph_.edge_ref_records;
					const uint64_t typeToken = graph_.forced_edge_reference_type_token.value_or(
							edge.type == "KNOWS" ? 1U : 0U);
					const EntityRef reference = graph_.forced_edge_reference.value_or(
							EntityRef{edge.edge_id, true, edge.source_node_id, edge.target_node_id, typeToken});
					if (!visitor(reference)) {
						return;
					}
					if (graph_.duplicate_first_edge_reference && edgeIndex == 0U) {
						if (visited == maximumRecords) {
							return;
						}
						++visited;
						++graph_.edge_ref_records;
						if (!visitor(reference)) {
							return;
						}
					}
				}
			}

			[[nodiscard]] bool supportsNativeEdgeRefScans() const override { return true; }

			[[nodiscard]] bool supportsNativeRelationshipTypeTokens() const override { return true; }

			[[nodiscard]] std::optional<uint64_t>
			findRelationshipTypeToken(std::string_view name) const override {
				return name == "KNOWS" ? std::optional<uint64_t>{1U} : std::nullopt;
			}

			void visitEdgeTopologies(
					size_t maximumRecords,
					const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
				++graph_.topology_scans;
				for (const LogicalGraphEdge &edge: graph_.edges) {
					if (graph_.topology_records == maximumRecords) {
						return;
					}
					++graph_.topology_records;
					if (!visitor({edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type})) {
						return;
					}
				}
			}

			void visitAdjacentEdges(uint64_t, LogicalGraphReadDirection, size_t,
									const std::function<bool(const LogicalGraphEdge &)> &) const override {
				++graph_.adjacency_scans;
				throw std::logic_error("direct Edge test must not use adjacency visitors");
			}

		private:
			DirectEdgeGraph &graph_;
		};

		class DirectEdgeBackend final : public LogicalGraphReadBackend {
		public:
			explicit DirectEdgeBackend(DirectEdgeGraph &graph) : graph_(graph) {}

			[[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
				return std::make_unique<DirectEdgeSnapshot>(graph_);
			}

		private:
			DirectEdgeGraph &graph_;
		};

		[[nodiscard]] LogicalPlanReadRuntimeBounds directBounds() {
			return {.maximum_node_records = 8U,
					.maximum_node_page_records = 2U,
					.maximum_metadata_pages_per_node_page = 2U,
					.maximum_edges_per_node = 8U,
					.maximum_edge_records = 16U,
					.maximum_adjacency_page_records = 2U,
					.maximum_property_entries = 8U,
					.maximum_rows = 16U,
					.maximum_distinct_rows = 16U};
		}

		[[nodiscard]] DirectEdgeGraph directGraph() {
			DirectEdgeGraph graph;
			graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Person"}, {{"rank", PropertyValue(1)}}});
			graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Person"}, {{"rank", PropertyValue(2)}}});
			graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Other"}, {{"rank", PropertyValue(3)}}});
			graph.edges.emplace_back(10U, 1U, 2U, "KNOWS",
									 std::vector<storage::LogicalGraphPropertyEntry>{{"weight", PropertyValue(7)},
																								 {"tier", PropertyValue(1)}});
			graph.edges.emplace_back(11U, 1U, 3U, "KNOWS",
									 std::vector<storage::LogicalGraphPropertyEntry>{{"weight", PropertyValue(7)},
																								 {"tier", PropertyValue(2)}});
			graph.edges.emplace_back(12U, 2U, 1U, "KNOWS",
									 std::vector<storage::LogicalGraphPropertyEntry>{{"weight", PropertyValue(8)}});
			return graph;
		}

	} // namespace

	TEST(LogicalPlanDirectEdgeRuntimeTest, AppliesSourceEdgeAndTargetPredicatesWithoutAdjacencyDescent) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)}});
		logical::LogicalTraversal traversal(
				std::move(source), "a", "r", "b", "KNOWS", "out", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(2)}},
				{{"weight", PropertyValue(7)}});

		std::vector<uint64_t> edges;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				traversal, directBounds(), [&edges](const LogicalPlanReadRow &row) {
					edges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
					return true;
				});

		EXPECT_EQ(edges, std::vector<uint64_t>{10U});
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.edge_ref_records, 3U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight"}}));
		EXPECT_EQ(stats.nodes_scanned, 3U);
		EXPECT_EQ(stats.edges_scanned, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesEdgePredicatesOnAnchoredReferencePages) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)}});
		logical::LogicalTraversal traversal(
				std::move(source), "a", "r", "b", "KNOWS", "out", {}, {},
				{{"weight", PropertyValue(7)}});

		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U, 11U}));
		EXPECT_EQ(graph.adjacency_ref_scans, 1U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsOversizedAnchoredEdgePredicateBatchBeforeFullEdgeRead) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.edges.front().properties.emplace_back(
				"payload", PropertyValue(PropertyValue::Bytes(2U * 1024U * 1024U, std::byte{0})));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)}});
		logical::LogicalTraversal traversal(
				std::move(source), "a", "r", "b", "KNOWS", "out", {}, {},
				{{"payload", PropertyValue(PropertyValue::Bytes(1U, std::byte{1}))}});
		LogicalPlanReadRuntimeBounds bounds = directBounds();
		bounds.maximum_materialized_bytes = 1U * 1024U * 1024U;

		// The payload is requested only for the predicate and matches no Edge. The
		// selected-column staging boundary must still fail before full Edge materialization.
		EXPECT_THROW(static_cast<void>(runtime.execute(traversal, bounds, [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_shaped_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesEveryEdgePredicateAndPreservesSelectedValues) {
		DirectEdgeGraph graph = directGraph();
		std::swap(graph.edges[0], graph.edges[1]);
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)}});
		logical::LogicalTraversal traversal(
				std::move(source), "a", "r", "b", "KNOWS", "out", {}, {},
				{{"weight", PropertyValue(7)}, {"tier", PropertyValue(1)}});

		std::vector<uint64_t> edges;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edges](const LogicalPlanReadRow &row) {
			edges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edges, std::vector<uint64_t>{10U});
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_property_sorted_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys,
				  (std::vector<std::vector<std::string>>{{"tier", "weight"}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, DeduplicatesPropertyReadsButPreservesDuplicateEdgeRows) {
		DirectEdgeGraph graph = directGraph();
		graph.duplicate_first_edge_reference = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out", {}, {},
				{{"weight", PropertyValue(7)}});

		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U, 10U, 11U}));
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight"}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, SplitsLargeEdgeProjectionIntoBoundedBatches) {
		DirectEdgeGraph graph = directGraph();
		for (uint64_t edgeId = 100U; edgeId < 800U; ++edgeId) {
			graph.edges.emplace_back(edgeId, 1U, 2U, "KNOWS",
					std::vector<storage::LogicalGraphPropertyEntry>{{"weight", PropertyValue(7)}});
		}
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out", {}, {},
				{{"weight", PropertyValue(7)}});
		auto bounds = directBounds();
		bounds.maximum_edge_records = 1024U;
		bounds.maximum_edges_per_node = 1024U;
		bounds.maximum_rows = 1024U;
		bounds.maximum_node_records = 16U;
		size_t resultCount = 0U;
		static_cast<void>(runtime.execute(traversal, bounds, [&resultCount](const LogicalPlanReadRow &) {
			++resultCount;
			return true;
		}));
		EXPECT_EQ(resultCount, 702U);
		// The bounded batch capacity is intentionally smaller than this input; no
		// unbounded Edge/property vector is retained by the runtime.
		EXPECT_EQ(graph.edge_property_column_batch_reads, 2U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesEdgeFilterPropertyAndReusesItForProjection) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>("a");
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::move(source), "a", "r", "b", "KNOWS", "out");
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(int64_t{7}));
		auto filter = std::make_unique<logical::LogicalFilter>(std::move(traversal), std::move(predicate));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "weight"), "weight");
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b"), "target");
		logical::LogicalProject project(std::move(filter), std::move(items));

		std::vector<PropertyValue> weights;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				project, directBounds(), [&weights](const LogicalPlanReadRow &row) {
					weights.push_back(std::get<PropertyValue>(row.values.at("weight")));
					return true;
				});

		EXPECT_EQ(weights, (std::vector<PropertyValue>{PropertyValue(7), PropertyValue(7)}));
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(stats.edges_scanned, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, GlobalEdgePropertyPathDoesNotScanUnconstrainedSources) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("source"), "source", "relationship", "target", "KNOWS", "out",
				std::vector<std::string>{}, std::vector<std::pair<std::string, PropertyValue>>{},
				{{"weight", PropertyValue(7)}});

		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("relationship")).edge_id);
			return true;
		}));

		EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U, 11U}));
		EXPECT_EQ(graph.node_label_match_scans, 0U);
		EXPECT_EQ(graph.node_label_match_records, 0U);
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_existence_probes, 0U);
		EXPECT_EQ(graph.node_existence_batch_probes, 1U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, GlobalEdgePropertyPathFailsClosedForMissingSourceEndpoint) {
		DirectEdgeGraph graph = directGraph();
		graph.edges.emplace_back(20U, 99U, 2U, "KNOWS",
				std::vector<storage::LogicalGraphPropertyEntry>{{"weight", PropertyValue(7)}});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("source"), "source", "relationship", "target", "KNOWS", "out",
				std::vector<std::string>{}, std::vector<std::pair<std::string, PropertyValue>>{},
				{{"weight", PropertyValue(7)}});

		EXPECT_THROW(static_cast<void>(runtime.execute(traversal, directBounds(),
				[](const LogicalPlanReadRow &) { return true; })), std::runtime_error);
		EXPECT_EQ(graph.node_existence_probes, 0U);
		EXPECT_EQ(graph.node_existence_batch_probes, 1U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesMultiplePropertiesFromOneFilterExpression) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		auto weight = std::make_unique<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(int64_t{7}));
		auto tier = std::make_unique<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r", "tier"),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(int64_t{1}));
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::move(weight), expressions::BinaryOperatorType::BOP_AND, std::move(tier));
		logical::LogicalFilter filter(std::move(traversal), std::move(predicate));

		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(filter, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edgeIds, std::vector<uint64_t>{10U});
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys,
				  (std::vector<std::vector<std::string>>{{"weight", "tier"}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesDeferredEdgeProjectionAcrossTheTraversal) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>("a");
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::move(source), "a", "r", "b", "KNOWS", "out");
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "weight"), "weight");
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "tier"), "tier");
		logical::LogicalProject project(std::move(traversal), std::move(items));

		std::vector<std::pair<PropertyValue, PropertyValue>> values;
		static_cast<void>(runtime.execute(project, directBounds(), [&values](const LogicalPlanReadRow &row) {
			values.emplace_back(std::get<PropertyValue>(row.values.at("weight")),
					std::get<PropertyValue>(row.values.at("tier")));
			return true;
		}));

		EXPECT_EQ(values, (std::vector<std::pair<PropertyValue, PropertyValue>>{
					{PropertyValue(7), PropertyValue(1)}, {PropertyValue(7), PropertyValue(2)},
					{PropertyValue(8), PropertyValue()}}));
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight", "tier"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, EnforcesMaterializedByteBoundForLargeSelectedProperties) {
		DirectEdgeGraph graph = directGraph();
		graph.edges.front().properties.emplace_back(
				"payload", PropertyValue(PropertyValue::Bytes(2U * 1024U * 1024U, std::byte{0})));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		std::vector<logical::LogicalSortItem> sortItems;
		sortItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "payload"), true);
		logical::LogicalSort sort(std::move(traversal), std::move(sortItems));
		LogicalPlanReadRuntimeBounds bounds = directBounds();
		bounds.maximum_materialized_bytes = 1U * 1024U * 1024U;

		try {
			static_cast<void>(runtime.execute(sort, bounds, [](const LogicalPlanReadRow &) { return true; }));
			FAIL() << "oversized selected Property columns must fail before row-cache enrichment";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical-plan Property batch exceeded its byte bound");
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsOversizedSelectedEdgePredicateBatchBeforeOutputMaterialization) {
		DirectEdgeGraph graph = directGraph();
		graph.edges.front().properties.emplace_back(
				"payload", PropertyValue(PropertyValue::Bytes(2U * 1024U * 1024U, std::byte{0})));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out", {}, {},
				{{"payload", PropertyValue(PropertyValue::Bytes(1U, std::byte{1}))}});
		LogicalPlanReadRuntimeBounds bounds = directBounds();
		bounds.maximum_materialized_bytes = 1U * 1024U * 1024U;

		// The predicate rejects every Edge, so this assertion exercises the selected-column
		// staging budget rather than the public complete-entity result boundary.
		EXPECT_THROW(static_cast<void>(runtime.execute(traversal, bounds, [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_read_ids, (std::vector<uint64_t>{10U, 11U, 12U}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, EnforcesMaterializedByteBoundAtPublicEntityResultBoundary) {
		DirectEdgeGraph graph = directGraph();
		graph.edges.front().properties.emplace_back(
				"payload", PropertyValue(PropertyValue::Bytes(2U * 1024U * 1024U, std::byte{0})));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		LogicalPlanReadRuntimeBounds limited = directBounds();
		limited.maximum_materialized_bytes = 1U * 1024U * 1024U;

		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(traversal, limited)), std::runtime_error);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ReusesQueryBoundEdgeMaterializationForRepeatedIntrospection) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		std::vector<std::unique_ptr<expressions::Expression>> firstArguments;
		firstArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("r"));
		std::vector<std::unique_ptr<expressions::Expression>> secondArguments;
		secondArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("r"));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("type", std::move(firstArguments)),
						   "first_type");
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("type", std::move(secondArguments)),
						   "second_type");
		logical::LogicalProject project(std::move(traversal), std::move(items));

		std::vector<std::pair<PropertyValue, PropertyValue>> types;
		static_cast<void>(runtime.execute(project, directBounds(), [&types](const LogicalPlanReadRow &row) {
			types.emplace_back(std::get<PropertyValue>(row.values.at("first_type")),
							   std::get<PropertyValue>(row.values.at("second_type")));
			return true;
		}));

		EXPECT_EQ(types, (std::vector<std::pair<PropertyValue, PropertyValue>>{
							 {PropertyValue("KNOWS"), PropertyValue("KNOWS")},
							 {PropertyValue("KNOWS"), PropertyValue("KNOWS")},
							 {PropertyValue("KNOWS"), PropertyValue("KNOWS")}}));
		// The type-only shape is resolved once even though type(r) is evaluated twice;
		// it does not force complete Property materialization.
		EXPECT_EQ(graph.edge_read_ids, (std::vector<uint64_t>{10U, 11U, 12U}));
		EXPECT_EQ(graph.edge_shaped_reads, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, InvalidatesEntityMetadataWhenUnwindRebindsAnEntityVariable) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);

		auto labelsArguments = std::vector<std::unique_ptr<expressions::Expression>>{};
		labelsArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		auto labelsExpression = std::make_unique<expressions::FunctionCallExpression>(
				"labels", std::move(labelsArguments));
		auto expectedLabels = std::make_unique<expressions::ListLiteralExpression>(
				PropertyValue(std::vector<PropertyValue>{PropertyValue("Person")}));
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::move(labelsExpression), expressions::BinaryOperatorType::BOP_EQUAL, std::move(expectedLabels));
		auto filtered = std::make_unique<logical::LogicalFilter>(
				std::make_unique<logical::LogicalNodeScan>("n"), std::move(predicate));
		auto rebound = std::make_unique<logical::LogicalUnwind>(
				std::move(filtered), "n", std::vector<PropertyValue>{PropertyValue(1)});
		std::vector<logical::LogicalProjectItem> items;
		auto reboundLabelsArguments = std::vector<std::unique_ptr<expressions::Expression>>{};
		reboundLabelsArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>(
				"labels", std::move(reboundLabelsArguments)), "labels");
		logical::LogicalProject project(std::move(rebound), std::move(items));

		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(project, directBounds())), std::runtime_error);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsRelationshipReferenceWithStaleTypeToken) {
		DirectEdgeGraph graph = directGraph();
		graph.forced_edge_reference_type_token = 99U;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");

		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(traversal, directBounds())), std::runtime_error);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, DoesNotCacheMaterializedEdgesBeyondTheByteBudget) {
		DirectEdgeGraph graph = directGraph();
		// A complete entity cache is a convenience for repeated bindings, not an
		// alternate LargeValue store.  Oversized payloads must be read on demand so
		// a single query cannot retain an unbounded copy of the property pages.
		graph.edges.front().properties.emplace_back(
				"source", PropertyValue(PropertyValue::Bytes(5U * 1024U * 1024U, std::byte{0})));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		std::vector<std::unique_ptr<expressions::Expression>> firstArguments;
		firstArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("r"));
		std::vector<std::unique_ptr<expressions::Expression>> secondArguments;
		secondArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("r"));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("type", std::move(firstArguments)),
								   "first_type");
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("type", std::move(secondArguments)),
								   "second_type");
		logical::LogicalProject project(std::move(traversal), std::move(items));

		static_cast<void>(runtime.execute(project, directBounds(), [](const LogicalPlanReadRow &) { return true; }));
		// Metadata-only type() remains cacheable even when the complete entity carries
		// an oversized value. The complete-object byte budget is enforced separately.
		EXPECT_EQ(graph.edge_read_ids, (std::vector<uint64_t>{10U, 11U, 12U}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesEdgePropertySortKeysBeforeOrdering) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		std::vector<logical::LogicalSortItem> sortItems;
		sortItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "weight"), true);
		logical::LogicalSort sort(std::move(traversal), std::move(sortItems));

		std::vector<int64_t> weights;
		static_cast<void>(runtime.execute(sort, directBounds(), [&weights](const LogicalPlanReadRow &row) {
			weights.push_back(std::get<int64_t>(
					std::get<LogicalGraphEdge>(row.values.at("r")).properties.front().value.getVariant()));
			return true;
		}));

		EXPECT_EQ(weights, (std::vector<int64_t>{7, 7, 8}));
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, PreservesReversedPropertyComparisonSemantics) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>("a");
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::move(source), "a", "r", "b", "KNOWS", "out");
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::LiteralExpression>(int64_t{7}),
				expressions::BinaryOperatorType::BOP_LESS,
				std::make_unique<expressions::VariableReferenceExpression>("r", "weight"));
		logical::LogicalFilter filter(std::move(traversal), std::move(predicate));

		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(filter, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edgeIds, std::vector<uint64_t>{12U});
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesAllSourcePredicateColumnsInOneSortedRead) {
		DirectEdgeGraph graph = directGraph();
		graph.nodes.at(1U).properties.emplace_back("score", PropertyValue(int64_t{10}));
		graph.nodes.at(2U).properties.emplace_back("score", PropertyValue(int64_t{20}));
		graph.nodes.at(3U).properties.emplace_back("score", PropertyValue(int64_t{30}));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)},
																							 {"score", PropertyValue(int64_t{10})}});
		logical::LogicalTraversal traversal(std::move(source), "a", "r", "b", "KNOWS", "out");

		std::vector<uint64_t> edges;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edges](const LogicalPlanReadRow &row) {
			edges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edges, (std::vector<uint64_t>{10U, 11U}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_property_sorted_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys,
				  (std::vector<std::vector<std::string>>{{"rank", "score"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesNodeScanPredicateColumnsBeforeRowMaterialization) {
		DirectEdgeGraph graph = directGraph();
		graph.nodes.at(1U).properties.emplace_back("score", PropertyValue(int64_t{10}));
		graph.nodes.at(2U).properties.emplace_back("score", PropertyValue(int64_t{20}));
		graph.nodes.at(3U).properties.emplace_back("score", PropertyValue(int64_t{30}));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan(
				"n", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)},
																							 {"score", PropertyValue(int64_t{10})}});

		std::vector<uint64_t> nodes;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				scan, directBounds(), [&nodes](const LogicalPlanReadRow &row) {
					nodes.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
					return true;
				});

		EXPECT_EQ(nodes, (std::vector<uint64_t>{1U}));
		EXPECT_EQ(stats.nodes_scanned, 3U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_property_sorted_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys,
				  (std::vector<std::vector<std::string>>{{"rank", "score"}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesPropertyProjectionDirectlyOverNodeScan) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> ranks;
		static_cast<void>(runtime.execute(project, directBounds(), [&ranks](const LogicalPlanReadRow &row) {
			ranks.push_back(std::get<PropertyValue>(row.values.at("rank")));
			return true;
		}));

		EXPECT_EQ(ranks, (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)}));
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_property_sorted_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys,
				  (std::vector<std::vector<std::string>>{{"rank"}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, DeduplicatesComplexProjectionPropertyRequestsInOneBatch) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");

		auto rankCase = std::make_shared<expressions::CaseExpression>();
		rankCase->addBranch(
				std::make_unique<expressions::BinaryOpExpression>(
						std::make_unique<expressions::VariableReferenceExpression>("n", "rank"),
						expressions::BinaryOperatorType::BOP_GREATER,
						std::make_unique<expressions::LiteralExpression>(int64_t{1})),
				std::make_unique<expressions::VariableReferenceExpression>("n", "rank"));
		rankCase->setElseExpression(
				std::make_unique<expressions::VariableReferenceExpression>("n", "rank"));

		std::vector<std::unique_ptr<expressions::Expression>> functionArguments;
		functionArguments.push_back(
				std::make_unique<expressions::VariableReferenceExpression>("n", "rank"));
		auto rankText = std::make_shared<expressions::FunctionCallExpression>(
				"toString", std::move(functionArguments));
		auto selected = std::make_shared<expressions::InExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("n", "rank"),
				std::vector<PropertyValue>{PropertyValue(int64_t{1}), PropertyValue(int64_t{3})});

		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(rankCase), "rank_case");
		items.emplace_back(std::move(rankText), "rank_text");
		items.emplace_back(std::move(selected), "selected");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<LogicalPlanReadRow> rows;
		static_cast<void>(runtime.execute(project, directBounds(), [&rows](const LogicalPlanReadRow &row) {
			rows.push_back(row);
			return true;
		}));

		ASSERT_EQ(rows.size(), 3U);
		for (size_t index = 0U; index < rows.size(); ++index) {
			const int64_t rank = static_cast<int64_t>(index + 1U);
			EXPECT_EQ(std::get<PropertyValue>(rows[index].values.at("rank_case")), PropertyValue(rank));
			EXPECT_EQ(std::get<PropertyValue>(rows[index].values.at("rank_text")),
					PropertyValue(std::to_string(rank)));
			EXPECT_EQ(std::get<PropertyValue>(rows[index].values.at("selected")),
					PropertyValue(rank != 2));
		}
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys,
				  (std::vector<std::vector<std::string>>{{"rank"}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesPropertySubsetOfMixedProjection) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank");
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "entity");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> ranks;
		std::vector<uint64_t> entityIds;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			ranks.push_back(std::get<PropertyValue>(row.values.at("rank")));
			entityIds.push_back(std::get<LogicalGraphNode>(row.values.at("entity")).node_id);
			return true;
		}));

		EXPECT_EQ(ranks, (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)}));
		EXPECT_EQ(entityIds, (std::vector<uint64_t>{1U, 2U, 3U}));
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys,
				  (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, (std::vector<uint64_t>{1U, 2U, 3U}));
		EXPECT_EQ(graph.node_shaped_reads, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, TopologyOnlyExistsUsesReferencePagesWithoutEntityMaterialization) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::ExistsExpression>(
				"(n)-[:KNOWS]->()", "n", "KNOWS", "", expressions::PatternDirection::PAT_OUTGOING),
				"has_outgoing_knows");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> results;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				project, directBounds(), [&results](const LogicalPlanReadRow &row) {
					results.push_back(std::get<PropertyValue>(row.values.at("has_outgoing_knows")));
					return true;
				});

		EXPECT_EQ(results, (std::vector<PropertyValue>{PropertyValue(true), PropertyValue(true), PropertyValue(false)}));
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
		EXPECT_EQ(graph.adjacency_ref_scans, 3U);
		EXPECT_EQ(graph.adjacency_ref_records, 3U);
		EXPECT_EQ(graph.node_existence_probes, 0U);
		EXPECT_EQ(graph.node_existence_batch_probes, 2U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
		EXPECT_EQ(stats.nodes_scanned, 3U);
		EXPECT_EQ(stats.edges_scanned, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesTargetNodePredicatesByDistinctSortedTargetIds) {
		DirectEdgeGraph graph = directGraph();
		graph.nodes.at(3U).labels = {"Person"};
		graph.edges.emplace_back(13U, 1U, 2U, "KNOWS",
								 std::vector<storage::LogicalGraphPropertyEntry>{{"weight", PropertyValue(9)}});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)}});
		logical::LogicalTraversal traversal(
				std::move(source), "a", "r", "b", "KNOWS", "out", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(2)}});

		std::vector<uint64_t> edges;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edges](const LogicalPlanReadRow &row) {
			edges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edges, (std::vector<uint64_t>{10U, 13U}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_property_sorted_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 2U);
		EXPECT_EQ(graph.node_property_column_batch_keys,
				  (std::vector<std::vector<std::string>>{{"rank"}, {"rank"}}));
		EXPECT_EQ(graph.node_label_sorted_reads, 1U);
		EXPECT_EQ(graph.node_label_sorted_ids, (std::vector<std::vector<uint64_t>>{{2U, 3U}}));
		EXPECT_EQ(graph.node_label_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CountsNodesFromReferencesWithoutMaterializingNodeRows) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
		logical::LogicalAggregate aggregate(
				std::move(scan), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("n"), "count", false}});

		std::vector<PropertyValue> results;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
					results.push_back(std::get<PropertyValue>(row.values.at("count")));
					return true;
				});

		EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{2})});
		EXPECT_EQ(stats.nodes_scanned, 3U);
		EXPECT_EQ(stats.rows_emitted, 1U);
		EXPECT_EQ(graph.node_label_match_scans, 1U);
		EXPECT_EQ(graph.node_label_match_records, 3U);
		EXPECT_EQ(graph.node_label_point_reads, 0U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsInvalidNodeReferencesAtTheDirectCountBoundary) {
		for (const EntityRef reference: {
				 EntityRef{0U, false, 0U, 0U, 0U},
				 EntityRef{10U, true, 1U, 2U, 1U},
		}) {
			DirectEdgeGraph graph = directGraph();
			graph.forced_node_reference = reference;
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
			logical::LogicalAggregate aggregate(
					std::move(scan), {},
					{{"count", std::make_shared<expressions::VariableReferenceExpression>("n"), "count", false}});

			EXPECT_THROW(static_cast<void>(runtime.execute(
					aggregate, directBounds(), [](const LogicalPlanReadRow &) { return true; })), std::runtime_error);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsInvalidEdgeReferencesAtTheDirectTraversalBoundary) {
		for (const EntityRef reference: {
				 EntityRef{0U, true, 1U, 2U, 1U},
				 EntityRef{1U, false, 0U, 0U, 0U},
				 EntityRef{10U, true, 0U, 2U, 1U},
				 EntityRef{10U, true, 1U, 0U, 1U},
		}) {
			DirectEdgeGraph graph = directGraph();
			graph.forced_edge_reference = reference;
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			logical::LogicalTraversal traversal(
					std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");

			EXPECT_THROW(static_cast<void>(runtime.execute(
					traversal, directBounds(), [](const LogicalPlanReadRow &) { return true; })), std::runtime_error);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AggregatesNodePropertiesThroughOneSelectedColumnBatch) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		logical::LogicalAggregate aggregate(
				std::move(scan), {},
				{{"sum", std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "total", false}});

		std::vector<PropertyValue> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			results.push_back(std::get<PropertyValue>(row.values.at("total")));
			return true;
		}));

		EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{6})});
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, SharesOneBatchAcrossGroupedAndMultipleAggregatePropertyExpressions) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto group = std::make_shared<expressions::VariableReferenceExpression>("n", "rank");
		auto value = std::make_shared<expressions::VariableReferenceExpression>("n", "rank");
		logical::LogicalAggregate aggregate(
				std::make_unique<logical::LogicalNodeScan>("n"), {group},
				{{"sum", value, "total", false}, {"avg", value, "average", false}}, {"rank"});

		std::map<int64_t, std::pair<int64_t, double>> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			const int64_t rank = std::get<int64_t>(std::get<PropertyValue>(row.values.at("rank")).getVariant());
			results.emplace(rank,
					std::make_pair(std::get<int64_t>(std::get<PropertyValue>(row.values.at("total")).getVariant()),
							std::get<double>(std::get<PropertyValue>(row.values.at("average")).getVariant())));
			return true;
		}));

		ASSERT_EQ(results.size(), 3U);
		EXPECT_EQ(results.at(1).first, 1);
		EXPECT_EQ(results.at(2).first, 2);
		EXPECT_EQ(results.at(3).first, 3);
		EXPECT_DOUBLE_EQ(results.at(1).second, 1.0);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AggregatesEdgePropertiesThroughOneSelectedColumnBatch) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		logical::LogicalAggregate aggregate(
				std::move(traversal), {},
				{{"sum", std::make_shared<expressions::VariableReferenceExpression>("r", "weight"), "total", false}});

		std::vector<PropertyValue> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			results.push_back(std::get<PropertyValue>(row.values.at("total")));
			return true;
		}));

		EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{22})});
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_ref_scans, 1U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CountsEdgePropertyMatchesThroughSortedColumnsWithoutRowMaterialization) {
		DirectEdgeGraph graph = directGraph();
		std::swap(graph.edges[0], graph.edges[1]);
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto seed = std::make_unique<logical::LogicalNodeScan>("a");
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::move(seed), "a", "r", "b", "KNOWS", "out", std::vector<std::string>{},
				std::vector<std::pair<std::string, PropertyValue>>{},
				std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue(7)}, {"tier", PropertyValue(1)}});
		logical::LogicalAggregate aggregate(
				std::move(traversal), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("r"), "count", false}});

		std::vector<PropertyValue> results;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
					results.push_back(std::get<PropertyValue>(row.values.at("count")));
					return true;
				});

		EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{1})});
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys,
				  (std::vector<std::vector<std::string>>{{"tier", "weight"}}));
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
		EXPECT_EQ(stats.edges_scanned, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, FusesFilteredEdgeCountIntoTheSelectedColumnScan) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		auto weight = std::make_unique<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(int64_t{7}));
		auto tier = std::make_unique<expressions::BinaryOpExpression>(
				std::make_unique<expressions::LiteralExpression>(int64_t{1}),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::VariableReferenceExpression>("r", "tier"));
		auto filter = std::make_unique<logical::LogicalFilter>(
				std::move(traversal), std::make_shared<expressions::BinaryOpExpression>(
						std::move(weight), expressions::BinaryOperatorType::BOP_AND, std::move(tier)));
		logical::LogicalAggregate aggregate(
				std::move(filter), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("r"), "count", false}});

		std::vector<PropertyValue> results;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
					results.push_back(std::get<PropertyValue>(row.values.at("count")));
					return true;
				});

		EXPECT_EQ(results, (std::vector<PropertyValue>{PropertyValue(int64_t{1})}));
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys,
				  (std::vector<std::vector<std::string>>{{"weight", "tier"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
		EXPECT_EQ(graph.node_existence_batch_probes, 1U);
		EXPECT_EQ(stats.edges_scanned, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, FilteredEdgeCountValidatesEndpointsOnceAndFailsClosed) {
		DirectEdgeGraph graph = directGraph();
		graph.nodes.erase(3U);
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(int64_t{7}));
		auto filter = std::make_unique<logical::LogicalFilter>(std::move(traversal), std::move(predicate));
		logical::LogicalAggregate aggregate(
				std::move(filter), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("r"), "count", false}});

		EXPECT_THROW(static_cast<void>(runtime.execute(
				aggregate, directBounds(), [](const LogicalPlanReadRow &) { return true; })), std::runtime_error);
		EXPECT_EQ(graph.node_existence_batch_probes, 1U);
		EXPECT_EQ(graph.edge_ref_scans, 1U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ShortCircuitsContradictoryInlineAndFilterEdgeCounts) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out",
				std::vector<std::string>{}, std::vector<std::pair<std::string, PropertyValue>>{},
				std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue(7)}});
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(int64_t{8}));
		auto filter = std::make_unique<logical::LogicalFilter>(std::move(traversal), std::move(predicate));
		logical::LogicalAggregate aggregate(
				std::move(filter), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("r"), "count", false}});

		std::vector<PropertyValue> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			results.push_back(std::get<PropertyValue>(row.values.at("count")));
			return true;
		}));

		EXPECT_EQ(results, (std::vector<PropertyValue>{PropertyValue(int64_t{0})}));
		EXPECT_EQ(graph.edge_ref_scans, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CountsUnconstrainedEdgesThroughReferencesWithoutTypeMaterialization) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto seed = std::make_unique<logical::LogicalNodeScan>("a");
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::move(seed), "a", "r", "b", "", "out");
		logical::LogicalAggregate aggregate(
				std::move(traversal), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("r"), "count", false}});

		std::vector<PropertyValue> results;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
					results.push_back(std::get<PropertyValue>(row.values.at("count")));
					return true;
				});

		EXPECT_EQ(results, (std::vector<PropertyValue>{PropertyValue(int64_t{3})}));
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.edge_ref_records, 3U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(stats.edges_scanned, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, DoesNotMaterializeUnboundTopologyOnlyTargetNodes) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "", "KNOWS", "out");

		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U, 11U, 12U}));
		// Public materialization still validates the bound source Node, but a
		// topology-only traversal must never read an unbound target (Node 3 is
		// only a target in this graph).
		EXPECT_EQ(graph.node_read_ids, (std::vector<uint64_t>{1U, 2U}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsAnUnknownNativeRelationshipTypeBeforeScanningEdges) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "MISSING", "out");

		std::vector<uint64_t> edges;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				traversal, directBounds(), [&edges](const LogicalPlanReadRow &row) {
					edges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
					return true;
				});

		EXPECT_TRUE(edges.empty());
		EXPECT_EQ(graph.edge_ref_scans, 0U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(stats.edges_scanned, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, DoesNotApplyDirectNodeCountWhenAParameterPredicateFiltersTheScan) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
		scan->setParameterPropertyPredicates({{"rank", "rank"}});
		logical::LogicalAggregate aggregate(
				std::move(scan), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("n"), "count", false}});
		QueryContext context;
		context.parameters.emplace("rank", PropertyValue(int64_t{1}));

		std::vector<PropertyValue> results;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
					results.push_back(std::get<PropertyValue>(row.values.at("count")));
					return true;
				}, context);

		EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{1})});
		EXPECT_EQ(stats.nodes_scanned, 3U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, EnforcesGlobalAndPerSourceEdgeBounds) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto makeTraversal = [] {
			return logical::LogicalTraversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");
		};

		auto globalBounds = directBounds();
		globalBounds.maximum_edge_records = 1U;
		auto globalTraversal = makeTraversal();
		EXPECT_THROW(static_cast<void>(runtime.execute(globalTraversal, globalBounds,
															 [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);

		graph.topology_records = 0U;
		auto perSourceBounds = directBounds();
		perSourceBounds.maximum_edges_per_node = 1U;
		perSourceBounds.maximum_adjacency_page_records = 1U;
		auto perSourceTraversal = makeTraversal();
		std::vector<uint64_t> edges;
		static_cast<void>(runtime.execute(perSourceTraversal, perSourceBounds,
													 [&edges](const LogicalPlanReadRow &row) {
														edges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
														return true;
													 }));
		EXPECT_EQ(edges, (std::vector<uint64_t>{10U, 12U}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AcceptsUnorderedPhysicalTopologyAndHonorsEarlyStop) {
		DirectEdgeGraph unordered = directGraph();
		std::swap(unordered.edges[0], unordered.edges[1]);
		DirectEdgeBackend unorderedBackend(unordered);
		LogicalPlanReadRuntime unorderedRuntime(unorderedBackend);
		logical::LogicalTraversal unorderedTraversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");
		std::vector<uint64_t> unorderedEdges;
		static_cast<void>(unorderedRuntime.execute(unorderedTraversal, directBounds(), [&unorderedEdges](const LogicalPlanReadRow &row) {
			unorderedEdges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));
		EXPECT_EQ(unorderedEdges, (std::vector<uint64_t>{11U, 10U, 12U}));
		EXPECT_EQ(unordered.edge_ref_scans, 1U);
		EXPECT_EQ(unordered.topology_scans, 0U);

		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");
		std::vector<uint64_t> sourceNodes;
		static_cast<void>(runtime.execute(traversal, directBounds(), [](const LogicalPlanReadRow &) { return false; }));
		EXPECT_EQ(graph.edge_ref_records, 1U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.adjacency_scans, 0U);

		graph.edge_ref_records = 0U;
		logical::LogicalTraversal materializedSourceTraversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");
		static_cast<void>(runtime.execute(materializedSourceTraversal, directBounds(),
				[&sourceNodes](const LogicalPlanReadRow &row) {
					sourceNodes.push_back(std::get<LogicalGraphNode>(row.values.at("a")).node_id);
					return true;
				}));
		EXPECT_EQ(sourceNodes, (std::vector<uint64_t>{1U, 1U, 2U}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, LimitStopsTopologyOnlyDirectTraversalBeforeReadAhead) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		logical::LogicalLimit limit(std::move(traversal), 1);

		const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setValueCollectionEnabled(true);
		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(limit, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));
		const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

		EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U}));
		// A topology-only direct traversal uses its single-record path, allowing
		// LIMIT to stop the native cursor before it observes another Edge.
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.edge_ref_records, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 0U);
		// One shaped read is necessary to bind the returned relationship, but no
		// additional relationship can be materialized after LIMIT has stopped it.
		EXPECT_EQ(graph.edge_shaped_reads, 1U);
		ASSERT_TRUE(trace.contains("logical_plan.direct_edge_records"));
		EXPECT_EQ(trace.at("logical_plan.direct_edge_records").totalValue, 1);
		ASSERT_TRUE(trace.contains("logical_plan.limit.delivered_rows"));
		EXPECT_EQ(trace.at("logical_plan.limit.delivered_rows").totalValue, 1);
		ASSERT_TRUE(trace.contains("logical_plan.limit.early_stop"));
		EXPECT_EQ(trace.at("logical_plan.limit.early_stop").totalValue, 1);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsAnUnincrementableGlobalEdgeBound) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");
		auto bounds = directBounds();
		bounds.maximum_edge_records = std::numeric_limits<size_t>::max();
		EXPECT_THROW(static_cast<void>(runtime.execute(
					 traversal, bounds, [](const LogicalPlanReadRow &) { return true; })),
					 std::invalid_argument);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.edge_ref_scans, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchAdjacencyCountPreservesDirectionAndSelfLoopSemantics) {
		for (const auto &[direction, expected]:
				 std::vector<std::pair<std::string, int64_t>>{{"out", 4}, {"in", 4}, {"both", 7}}) {
			DirectEdgeGraph graph = directGraph();
			graph.enable_batch_adjacency_reference_scan = true;
			graph.edges.emplace_back(13U, 1U, 1U, "KNOWS",
					std::vector<storage::LogicalGraphPropertyEntry>{});
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			auto traversal = std::make_unique<logical::LogicalTraversal>(
					std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", direction);
			logical::LogicalAggregate aggregate(
					std::move(traversal), {},
					{{"count", std::make_shared<expressions::VariableReferenceExpression>("b"), "count", false}});

			std::vector<PropertyValue> results;
			static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
				results.push_back(std::get<PropertyValue>(row.values.at("count")));
				return true;
			}));

			EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(expected)}) << direction;
			EXPECT_EQ(graph.batch_adjacency_ref_scans, direction == "both" ? 2U : 1U) << direction;
			EXPECT_EQ(graph.edge_ref_scans, 0U) << direction;
			EXPECT_EQ(graph.adjacency_ref_scans, 0U) << direction;
			EXPECT_GT(graph.node_existence_batch_probes, 0U) << direction;
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchAdjacencyCountAppliesTypeLabelsAndEndpointLiveness) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_batch_adjacency_reference_scan = true;
		graph.edges.emplace_back(13U, 1U, 2U, "OTHER",
				std::vector<storage::LogicalGraphPropertyEntry>{});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out",
				std::vector<std::string>{"Person"});
		logical::LogicalAggregate aggregate(
				std::move(traversal), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("b"), "count", false}});

		std::vector<PropertyValue> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			results.push_back(std::get<PropertyValue>(row.values.at("count")));
			return true;
		}));

		EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{2})});
		EXPECT_EQ(graph.batch_adjacency_ref_scans, 1U);
		EXPECT_GT(graph.node_label_sorted_reads, 0U);

		graph.nodes.erase(3U);
		auto invalidTraversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		logical::LogicalAggregate invalidAggregate(
				std::move(invalidTraversal), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("b"), "count", false}});
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidAggregate, directBounds(),
													   [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NativeAdjacencySkipsTargetIoAfterEdgePredicatesRejectTheBatch) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>(
						"a", std::vector<std::string>{"Person"},
						std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(int64_t{1})}}),
				"a", "r", "b", "KNOWS", "out", std::vector<std::string>{},
				std::vector<std::pair<std::string, PropertyValue>>{},
				std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue()}});

		std::vector<LogicalPlanReadRow> rows;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&rows](const LogicalPlanReadRow &row) {
			rows.push_back(row);
			return true;
		}));

		EXPECT_TRUE(rows.empty());
		EXPECT_EQ(graph.edge_property_column_batch_reads, 0U);
		EXPECT_EQ(graph.node_existence_batch_probes, 0U);
		EXPECT_EQ(graph.node_label_sorted_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U); // Source predicate only.
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NativeAdjacencyRejectsInvalidTargetPropertyDefinitions) {
		for (const auto &targetProperties:
			 {std::vector<std::pair<std::string, PropertyValue>>{{"", PropertyValue(int64_t{2})}},
			  std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(int64_t{2})},
																 {"rank", PropertyValue(int64_t{2})}}}) {
			DirectEdgeGraph graph = directGraph();
			graph.enable_adjacency_reference_pages = true;
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			logical::LogicalTraversal traversal(
					std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Person"}), "a", "r", "b",
					"KNOWS", "out", std::vector<std::string>{}, targetProperties);

			EXPECT_THROW(static_cast<void>(runtime.execute(traversal, directBounds(),
														   [](const LogicalPlanReadRow &) { return true; })),
						 std::invalid_argument);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NativeAdjacencyTreatsTargetLabelMissAsAFilter) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>(
						"a", std::vector<std::string>{},
						std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(int64_t{1})}}),
				"a", "r", "b", "KNOWS", "out", std::vector<std::string>{"Employee"});

		std::vector<LogicalPlanReadRow> rows;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&rows](const LogicalPlanReadRow &row) {
			rows.push_back(row);
			return true;
		}));

		EXPECT_TRUE(rows.empty());
		EXPECT_GT(graph.node_label_sorted_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NativeAdjacencyRejectsMissingEndpointsAndBatchCardinalityChanges) {
		auto makeTraversal = [] {
			return logical::LogicalTraversal(
					std::make_unique<logical::LogicalNodeScan>(
							"a", std::vector<std::string>{},
							std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(int64_t{1})}}),
					"a", "r", "b", "KNOWS", "out");
		};
		for (const auto &configure: {
					 std::function<void(DirectEdgeGraph &)>{[](DirectEdgeGraph &graph) { graph.nodes.erase(3U); }},
					 std::function<void(DirectEdgeGraph &)>{
							 [](DirectEdgeGraph &graph) { graph.truncate_node_existence_batch = true; }},
			 }) {
			DirectEdgeGraph graph = directGraph();
			graph.enable_adjacency_reference_pages = true;
			configure(graph);
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			auto traversal = makeTraversal();
			EXPECT_THROW(static_cast<void>(runtime.execute(traversal, directBounds(),
														   [](const LogicalPlanReadRow &) { return true; })),
						 std::runtime_error);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NativeAdjacencyRejectsCorruptedSelectedColumnCardinality) {
		for (const bool corruptEdgeColumn: {false, true}) {
			DirectEdgeGraph graph = directGraph();
			graph.enable_adjacency_reference_pages = true;
			graph.truncate_edge_property_batch = corruptEdgeColumn;
			graph.truncate_node_property_batch = !corruptEdgeColumn;
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			logical::LogicalTraversal traversal(
					std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Person"}), "a", "r", "b",
					"KNOWS", "out", std::vector<std::string>{},
					std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(int64_t{2})}},
					std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue(int64_t{7})}});

			EXPECT_THROW(static_cast<void>(runtime.execute(traversal, directBounds(),
														   [](const LogicalPlanReadRow &) { return true; })),
						 std::runtime_error);
		}
	}

} // namespace graph::query::execution
