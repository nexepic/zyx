#pragma once

#include <algorithm>
#include <array>
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
#include "graph/query/execution/RetainedPropertyBytes.hpp"
#include "query/execution/BudgetMemoryResource.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/ListComprehensionExpression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/QuantifierFunctionExpression.hpp"
#include "graph/query/expressions/ReduceExpression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalOptionalMatch.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalUnion.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"

namespace graph::query::execution::direct_edge_test {
		using storage::EntityRef;
		using storage::LogicalGraphAdjacencyPage;
		using storage::LogicalGraphAdjacencyTopologyPage;
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
			bool enable_native_node_ref_scans = true;
			bool enable_native_topology_pages = false;
			bool enable_shaped_property_admission = false;
			bool enable_adjacency_read_admission = false;
			bool enable_property_read_admission = false;
			std::vector<uint64_t> node_read_ids;
			std::vector<uint64_t> node_ref_scan_order;
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
			size_t node_property_column_admission_reads = 0U;
			std::vector<std::vector<std::string>> node_property_column_batch_keys;
			std::vector<std::vector<uint64_t>> node_property_column_batch_ids;
			size_t node_shaped_batch_reads = 0U;
			size_t node_shaped_admission_reads = 0U;
			std::vector<std::vector<uint32_t>> node_shaped_batch_property_tokens;
			std::vector<bool> node_shaped_batch_all_properties;
			size_t node_label_point_reads = 0U;
			size_t node_label_match_scans = 0U;
			size_t node_label_match_records = 0U;
			size_t node_label_sorted_reads = 0U;
			std::vector<std::vector<uint64_t>> node_label_sorted_ids;
			size_t edge_property_point_reads = 0U;
			size_t edge_property_sorted_reads = 0U;
			std::vector<std::vector<uint64_t>> edge_property_sorted_ids;
			size_t edge_property_column_batch_reads = 0U;
			size_t edge_property_column_admission_reads = 0U;
			std::vector<std::vector<std::string>> edge_property_column_batch_keys;
			size_t edge_shaped_batch_reads = 0U;
			size_t edge_shaped_admission_reads = 0U;
			std::vector<bool> edge_shaped_batch_all_properties;
			std::vector<std::vector<uint32_t>> edge_shaped_batch_property_tokens;
			size_t node_property_column_extra_slots = 0U;
			bool enable_property_tokens = false;
			bool enable_non_null_probes = false;
			std::vector<std::pair<uint64_t, std::string>> node_non_null_probes;
			std::vector<std::pair<uint64_t, std::string>> edge_non_null_probes;
		};

		[[nodiscard]] inline const PropertyValue *
		findProperty(const std::vector<storage::LogicalGraphPropertyEntry> &properties, std::string_view key) {
			const auto found = std::ranges::find(properties, key, &storage::LogicalGraphPropertyEntry::key);
			return found == properties.end() ? nullptr : &found->value;
		}

		class DirectEdgeSnapshot final : public LogicalGraphReadSnapshot {
		public:
			explicit DirectEdgeSnapshot(DirectEdgeGraph &graph) : graph_(graph) {}

			[[nodiscard]] bool supportsPropertyNonNullProbes() const override { return graph_.enable_non_null_probes; }
			[[nodiscard]] std::optional<bool> nodePropertyIsNonNull(uint64_t id, std::string_view key) const override {
				if (!graph_.enable_non_null_probes) return std::nullopt;
				graph_.node_non_null_probes.emplace_back(id, key);
				const auto *value = findProperty(graph_.nodes.at(id).properties, key);
				return value != nullptr && !std::holds_alternative<std::monostate>(value->getVariant());
			}
			[[nodiscard]] std::optional<bool> edgePropertyIsNonNull(uint64_t id, std::string_view key) const override {
				if (!graph_.enable_non_null_probes) return std::nullopt;
				graph_.edge_non_null_probes.emplace_back(id, key);
				const auto edge = std::ranges::find(graph_.edges, id, &LogicalGraphEdge::edge_id);
				if (edge == graph_.edges.end()) throw std::out_of_range("non-null probe Edge is missing");
				const auto *value = findProperty(edge->properties, key);
				return value != nullptr && !std::holds_alternative<std::monostate>(value->getVariant());
			}

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

			[[nodiscard]] std::vector<LogicalGraphNode> readNodesShapedSorted(
					const std::span<const uint64_t> nodeIds,
					const storage::EntityReadShape &shape) const override {
				if (!graph_.enable_property_tokens) {
					return LogicalGraphReadSnapshot::readNodesShapedSorted(nodeIds, shape);
				}
				++graph_.node_shaped_batch_reads;
				graph_.node_shaped_batch_property_tokens.push_back(shape.property_tokens);
				graph_.node_shaped_batch_all_properties.push_back(shape.all_properties);
				std::vector<LogicalGraphNode> result;
				result.reserve(nodeIds.size());
				uint64_t previous = 0U;
				for (const uint64_t nodeId: nodeIds) {
					if (nodeId == 0U || nodeId <= previous) {
						throw std::invalid_argument("direct Node shaped batch IDs are invalid");
					}
					previous = nodeId;
					LogicalGraphNode node = graph_.nodes.at(nodeId);
					if (!shape.include_labels) {
						node.labels.clear();
					}
					if (!shape.all_properties) {
						node.properties.erase(std::remove_if(node.properties.begin(), node.properties.end(),
								[&](const auto &entry) {
									const auto token = entry.key == "rank" ? 1U : entry.key == "weight" ? 2U : 3U;
									return std::ranges::find(shape.property_tokens, token) == shape.property_tokens.end();
								}), node.properties.end());
					}
					result.push_back(std::move(node));
				}
				return result;
			}

			[[nodiscard]] bool supportsShapedPropertyReadAdmission() const override {
				return graph_.enable_shaped_property_admission;
			}
			[[nodiscard]] std::vector<LogicalGraphNode> readNodesShapedSortedWithAdmission(
					std::span<const uint64_t> nodeIds, const storage::EntityReadShape &shape,
					const std::function<void(size_t)> &admission) const override {
				if (!admission) throw std::invalid_argument("direct Node admission callback is empty");
				++graph_.node_shaped_admission_reads;
				for (const uint64_t id: nodeIds) {
					for (const auto &entry: graph_.nodes.at(id).properties) {
						const uint32_t token = entry.key == "rank" ? 1U : entry.key == "weight" ? 2U : 3U;
						if (!shape.all_properties &&
							std::ranges::find(shape.property_tokens, token) == shape.property_tokens.end()) continue;
						admission(property_utils::getPropertyValueSize(entry.value) + sizeof(uint32_t));
					}
				}
				return readNodesShapedSorted(nodeIds, shape);
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

			[[nodiscard]] std::vector<LogicalGraphEdge> readEdgesShapedSorted(
					const std::span<const uint64_t> edgeIds,
					const storage::EntityReadShape &shape) const override {
				if (!graph_.enable_property_tokens) {
					return LogicalGraphReadSnapshot::readEdgesShapedSorted(edgeIds, shape);
				}
				++graph_.edge_shaped_batch_reads;
				graph_.edge_shaped_batch_all_properties.push_back(shape.all_properties);
				graph_.edge_shaped_batch_property_tokens.push_back(shape.property_tokens);
				std::vector<LogicalGraphEdge> result;
				result.reserve(edgeIds.size());
				uint64_t previous = 0U;
				for (const uint64_t edgeId: edgeIds) {
					if (edgeId == 0U || edgeId <= previous) {
						throw std::invalid_argument("direct Edge shaped batch IDs are invalid");
					}
					previous = edgeId;
					const auto found = std::ranges::find(graph_.edges, edgeId, &LogicalGraphEdge::edge_id);
					if (found == graph_.edges.end()) {
						throw std::out_of_range("direct Edge shaped batch record is absent");
					}
					LogicalGraphEdge edge = *found;
					if (!shape.include_type) {
						edge.type.clear();
					}
					if (!shape.all_properties) {
						edge.properties.erase(std::remove_if(edge.properties.begin(), edge.properties.end(),
								[&](const auto &entry) {
									const auto token = entry.key == "weight" ? 2U : entry.key == "tier" ? 3U : 1U;
									return std::ranges::find(shape.property_tokens, token) == shape.property_tokens.end();
								}), edge.properties.end());
					}
					result.push_back(std::move(edge));
				}
				return result;
			}
			[[nodiscard]] std::vector<LogicalGraphEdge> readEdgesShapedSortedWithAdmission(
					std::span<const uint64_t> edgeIds, const storage::EntityReadShape &shape,
					const std::function<void(size_t)> &admission) const override {
				if (!admission) throw std::invalid_argument("direct Edge admission callback is empty");
				++graph_.edge_shaped_admission_reads;
				for (const uint64_t id: edgeIds) {
					const auto edge = std::ranges::find(graph_.edges, id, &LogicalGraphEdge::edge_id);
					if (edge == graph_.edges.end()) throw std::out_of_range("direct Edge admission record is absent");
					for (const auto &entry: edge->properties) {
						const uint32_t token = entry.key == "weight" ? 2U : entry.key == "tier" ? 3U : 1U;
						if (!shape.all_properties &&
							std::ranges::find(shape.property_tokens, token) == shape.property_tokens.end()) continue;
						admission(property_utils::getPropertyValueSize(entry.value) + sizeof(uint32_t));
					}
				}
				return readEdgesShapedSorted(edgeIds, shape);
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
				graph_.node_property_column_batch_ids.emplace_back(nodeIds.begin(), nodeIds.end());
				std::vector<std::string> copiedKeys;
				copiedKeys.reserve(keys.size());
				for (const std::string_view key: keys) {
					copiedKeys.emplace_back(key);
				}
				graph_.node_property_column_batch_keys.push_back(std::move(copiedKeys));
				std::vector<std::vector<PropertyValue>> columns(keys.size());
				for (size_t column = 0U; column < keys.size(); ++column) {
					columns[column].reserve(nodeIds.size() + graph_.node_property_column_extra_slots);
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

			[[nodiscard]] bool supportsPropertyReadAdmission() const override {
				return graph_.enable_property_read_admission;
			}

			[[nodiscard]] std::vector<std::vector<PropertyValue>> readNodePropertyColumnsSortedWithAdmission(
					std::span<const uint64_t> nodeIds, std::span<const std::string_view> keys,
					const std::function<void(size_t)> &admission) const override {
				++graph_.node_property_column_admission_reads;
				for (const std::string_view key: keys) {
					for (const uint64_t id: nodeIds) {
						const PropertyValue *value = findProperty(graph_.nodes.at(id).properties, key);
						if (value != nullptr) admission(sizeof(uint32_t) + property_utils::getPropertyValueSize(*value));
					}
				}
				return readNodePropertyColumnsSorted(nodeIds, keys);
			}

			[[nodiscard]] std::vector<std::vector<PropertyValue>> readEdgePropertyColumnsSortedWithAdmission(
					std::span<const uint64_t> edgeIds, std::span<const std::string_view> keys,
					const std::function<void(size_t)> &admission) const override {
				++graph_.edge_property_column_admission_reads;
				for (const std::string_view key: keys) {
					for (const uint64_t id: edgeIds) {
						const auto edge = std::ranges::find(graph_.edges, id, &LogicalGraphEdge::edge_id);
						if (edge == graph_.edges.end()) throw std::out_of_range("direct Edge Property admission record is absent");
						const PropertyValue *value = findProperty(edge->properties, key);
						if (value != nullptr) admission(sizeof(uint32_t) + property_utils::getPropertyValueSize(*value));
					}
				}
				return readEdgePropertyColumnsSorted(edgeIds, keys);
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
				std::vector<uint64_t> order = graph_.node_ref_scan_order;
				if (order.empty()) for (const auto &[id, node]: graph_.nodes) order.push_back(id);
				for (const uint64_t nodeId: order) {
					const auto &node = graph_.nodes.at(nodeId);
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

			[[nodiscard]] bool supportsNativeNodeRefScans() const override { return graph_.enable_native_node_ref_scans; }

			[[nodiscard]] std::optional<uint32_t> findPropertyToken(std::string_view key) const override {
			if (!graph_.enable_property_tokens) {
				return std::nullopt;
			}
			if (key == "rank") return 1U;
			if (key == "weight") return 2U;
			if (key == "tier") return 3U;
			return std::nullopt;
		}

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
					uint64_t afterNodeId, size_t maximumRecords, size_t maximumMetadataPages) const override {
				if (graph_.enable_native_node_ref_scans) {
					throw std::logic_error("direct Edge test must not use paged Node scans");
				}
				if (maximumRecords == 0U || maximumMetadataPages == 0U) {
					throw std::invalid_argument("direct Edge test Node page needs positive bounds");
				}
				LogicalGraphNodePage page;
				page.metadata_pages_visited = 1U;
				auto current = graph_.nodes.upper_bound(afterNodeId);
				while (current != graph_.nodes.end() && page.nodes.size() < maximumRecords) {
					page.nodes.push_back(current->second);
					++current;
				}
				page.may_have_more = current != graph_.nodes.end();
				return page;
			}

			[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdges(
					uint64_t nodeId, LogicalGraphReadDirection direction,
					std::optional<storage::LogicalGraphAdjacencyContinuation> continuation,
					size_t maximumRecords) const override {
				++graph_.adjacency_scans;
				if (graph_.enable_native_node_ref_scans) {
					throw std::logic_error("direct Edge test must not use adjacency pages");
				}
				const auto topology = scanAdjacentEdgeTopologies(nodeId, direction, continuation, maximumRecords);
				LogicalGraphAdjacencyPage page;
				page.continuation = topology.continuation;
				page.edges.reserve(topology.edges.size());
				for (const auto &edge: topology.edges) page.edges.push_back(readEdge(edge.edge_id));
				return page;
			}

			[[nodiscard]] bool supportsAdjacencyReadAdmission() const override { return graph_.enable_adjacency_read_admission; }
			[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdgesWithAdmission(
				uint64_t nodeId, LogicalGraphReadDirection direction,
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
				const std::function<void(size_t)> &propertyAdmission,
				const std::function<void(size_t)> &bufferAdmission) const override {
				bufferAdmission(4096U);
				propertyAdmission(4096U);
				bufferAdmission(1024U);
				return scanAdjacentEdges(nodeId, direction, continuation, maximumRecords);
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

			[[nodiscard]] bool supportsNativeAdjacentEdgeTopologyPages() const override { return graph_.enable_native_topology_pages; }
			[[nodiscard]] bool supportsNativeAdjacentEdgeRefScans() const override {
				return graph_.enable_adjacency_reference_pages;
			}

			[[nodiscard]] LogicalGraphAdjacencyTopologyPage scanAdjacentEdgeTopologies(
					uint64_t nodeId, LogicalGraphReadDirection direction,
					std::optional<storage::LogicalGraphAdjacencyContinuation> continuation,
					size_t maximumRecords) const override {
				++graph_.topology_scans;
				if (maximumRecords == 0U) {
					throw std::invalid_argument("direct Edge test topology page size is zero");
				}
				std::vector<const LogicalGraphEdge *> candidates;
				for (const LogicalGraphEdge &edge: graph_.edges) {
					if ((direction == LogicalGraphReadDirection::LGRD_OUTGOING && edge.source_node_id == nodeId) ||
						(direction == LogicalGraphReadDirection::LGRD_INCOMING && edge.target_node_id == nodeId)) {
						candidates.push_back(&edge);
					}
				}
				std::ranges::sort(candidates, [](const LogicalGraphEdge *left, const LogicalGraphEdge *right) {
					return left->edge_id < right->edge_id;
				});
				size_t begin = 0U;
				if (continuation.has_value()) {
					while (begin < candidates.size() && candidates[begin]->edge_id != continuation->next_edge_id) {
						++begin;
					}
					if (begin == 0U || begin == candidates.size() ||
						candidates[begin - 1U]->edge_id != continuation->previous_edge_id) {
						throw std::invalid_argument("direct Edge test topology continuation is invalid");
					}
				}
				LogicalGraphAdjacencyTopologyPage result;
				for (size_t index = begin; index < candidates.size() && result.edges.size() < maximumRecords; ++index) {
					const LogicalGraphEdge &edge = *candidates[index];
					result.edges.push_back({edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type});
				}
				const size_t nextIndex = begin + result.edges.size();
				if (nextIndex < candidates.size() && !result.edges.empty()) {
					result.continuation = {result.edges.back().edge_id, candidates[nextIndex]->edge_id};
				}
				return result;
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

		[[nodiscard]] inline LogicalPlanReadRuntimeBounds directBounds() {
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

		[[nodiscard]] inline DirectEdgeGraph directGraph() {
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



		// Keep tight row-transfer tests independent of newly admitted public map
		// storage. Derive only the extra capacity charge of their actual outputs.
		inline size_t entityCacheMetadataDelta(const DirectEdgeGraph &graph, bool includeEdges = false) {
			MaterializedByteBudget budget(16U * 1024U * 1024U);
			detail::BudgetMemoryResource memory(budget);
			std::pmr::unordered_map<uint64_t, LogicalGraphNode> nodes(&memory);
			for (const auto &[id, node] : graph.nodes) nodes.emplace(id, LogicalGraphNode{});
			std::pmr::unordered_map<uint64_t, LogicalGraphEdge> edges(&memory);
			if (includeEdges)
				for (const auto &edge : graph.edges) edges.emplace(edge.edge_id, LogicalGraphEdge{});
			const size_t objectBytes = nodes.size() * sizeof(LogicalGraphNode) + edges.size() * sizeof(LogicalGraphEdge);
			// Retained metadata plus one bucket array for rehash coexistence.
			return budget.used() - objectBytes +
				(nodes.bucket_count() + (includeEdges ? edges.bucket_count() : 0U)) * sizeof(void *);
		}

		inline size_t resultEntityCapacityDelta(const DirectEdgeGraph &graph) {
			size_t delta = 0U;
			for (const auto &[id, node] : graph.nodes) {
				const std::vector<std::string> resultLabels = node.labels;
				delta += resultLabels.capacity() * sizeof(std::string);
				for (const auto &label : resultLabels) {
					const size_t retained = detail::dynamicStringBufferBytes(label);
					if (retained > label.size()) delta += retained - label.size();
				}
				PropertyValue::MapType map;
				map.reserve(node.properties.size());
				size_t previous = 0U;
				for (const auto &entry : node.properties) {
					map.emplace(entry.key, entry.value);
					previous += entry.key.size() + sizeof(PropertyValue) +
						detail::retainedPropertyValueBytes(entry.value);
				}
				const size_t retained = detail::propertyMapHeapBufferBytes(map);
				if (retained > previous) delta += retained - previous;
			}
			return delta;
		}
} // namespace graph::query::execution::direct_edge_test
