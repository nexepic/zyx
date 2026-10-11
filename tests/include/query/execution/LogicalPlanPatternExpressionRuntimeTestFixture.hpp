#pragma once

/**
 * @file test_LogicalPlanPatternExpressionRuntime.cpp
 * @brief Regression tests for bounded logical EXISTS and pattern comprehensions.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "CypherParserImpl.hpp"
#include "graph/core/Types.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/expressions/ExistsExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/expressions/PatternComprehensionExpression.hpp"
#include "graph/query/expressions/ShortestPathExpression.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/storage/LogicalGraphRead.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::execution;
using namespace graph::storage;

namespace {

	using FixtureNodes = std::map<uint64_t, LogicalGraphNode>;
	using FixtureEdges = std::map<uint64_t, LogicalGraphEdge>;

	struct PatternExpressionGraph {
		FixtureNodes nodes;
		FixtureEdges edges;
	};

	struct PatternExpressionSnapshotFaults {
		std::shared_ptr<std::vector<size_t>> topology_admission_requests;
		bool malformed_adjacency_page = false;
		bool invalid_adjacency_endpoint = false;
		bool wrong_node_identity = false;
		bool discontinuous_adjacency_page = false;
		bool supports_node_existence_probes = false;
		bool unsupported_marker_presence = false;
		bool truncate_selected_property_column = false;
		bool supports_native_adjacency_refs = false;
		bool supports_native_node_ref_scans = false;
		bool supports_shaped_property_admission = false;
		std::shared_ptr<size_t> node_read_count;
		std::shared_ptr<size_t> edge_read_count;
		std::shared_ptr<size_t> node_existence_probe_count;
		std::shared_ptr<size_t> node_existence_batch_probe_count;
		std::shared_ptr<size_t> adjacency_topology_scan_count;
		std::shared_ptr<size_t> adjacency_reference_scan_count;
		std::shared_ptr<size_t> edge_shaped_batch_read_count;
		std::shared_ptr<size_t> edge_shaped_admission_read_count;
		std::shared_ptr<size_t> node_shaped_admission_read_count;
		std::shared_ptr<size_t> node_shaped_completed_read_count;
		std::shared_ptr<size_t> node_property_column_admission_count;
		std::shared_ptr<size_t> node_property_column_read_count;
		std::shared_ptr<size_t> edge_property_column_admission_count;
		std::shared_ptr<size_t> edge_property_column_read_count;
		std::shared_ptr<std::vector<uint64_t>> edge_shaped_batch_ids;
	};

	[[nodiscard]] PatternExpressionGraph makeDefaultPatternExpressionGraph() {
		PatternExpressionGraph graph;
		graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Person"}, {{"name", PropertyValue("Ada")}}});
		graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Person", "Friend"}, {{"age", PropertyValue(30)}}});
		graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Robot"}, {{"age", PropertyValue(7)}}});
		graph.edges.emplace(10U, LogicalGraphEdge{10U, 1U, 2U, "KNOWS"});
		graph.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 3U, "OTHER"});
		graph.edges.emplace(12U, LogicalGraphEdge{12U, 3U, 1U, "LIKES"});
		return graph;
	}

	class PatternExpressionSnapshot final : public LogicalGraphReadSnapshot {
	public:
		explicit PatternExpressionSnapshot(PatternExpressionGraph graph,
									   PatternExpressionSnapshotFaults faults = {})
		: nodes_(std::move(graph.nodes)), edges_(std::move(graph.edges)), faults_(faults) {}

		[[nodiscard]] LogicalGraphNode readNode(uint64_t id) const override {
			if (faults_.node_read_count) {
				++*faults_.node_read_count;
			}
			if (faults_.wrong_node_identity && id != nodes_.begin()->first) {
				return nodes_.begin()->second;
			}
			return nodes_.at(id);
		}
		[[nodiscard]] std::optional<bool> nodePropertyExists(uint64_t id, std::string_view key) const override {
			if (key != "_type") return LogicalGraphReadSnapshot::nodePropertyExists(id, key);
			if (faults_.unsupported_marker_presence) return std::nullopt;
			return std::ranges::any_of(nodes_.at(id).properties, [&](const auto &entry) { return entry.key == key; });
		}
		[[nodiscard]] std::optional<bool> edgePropertyExists(uint64_t id, std::string_view key) const override {
			if (key != "_type") return LogicalGraphReadSnapshot::edgePropertyExists(id, key);
			if (faults_.unsupported_marker_presence) return std::nullopt;
			return std::ranges::any_of(edges_.at(id).properties, [&](const auto &entry) { return entry.key == key; });
		}
		[[nodiscard]] PropertyValue readNodeProperty(uint64_t id, std::string_view key) const override {
			if (key != "_type") return LogicalGraphReadSnapshot::readNodeProperty(id, key);
			const auto &entries = nodes_.at(id).properties;
			const auto entry = std::ranges::find(entries, key, &LogicalGraphPropertyEntry::key);
			return entry == entries.end() ? PropertyValue() : entry->value;
		}
		[[nodiscard]] PropertyValue readEdgeProperty(uint64_t id, std::string_view key) const override {
			if (key != "_type") return LogicalGraphReadSnapshot::readEdgeProperty(id, key);
			const auto &entries = edges_.at(id).properties;
			const auto entry = std::ranges::find(entries, key, &LogicalGraphPropertyEntry::key);
			return entry == entries.end() ? PropertyValue() : entry->value;
		}
		[[nodiscard]] bool containsNode(uint64_t id) const override {
			if (faults_.node_existence_probe_count) {
				++*faults_.node_existence_probe_count;
			}
			return nodes_.contains(id);
		}
		[[nodiscard]] std::vector<bool> containsNodesSorted(std::span<const uint64_t> ids) const override {
			if (faults_.node_existence_batch_probe_count) {
				++*faults_.node_existence_batch_probe_count;
			}
			std::vector<bool> result;
			result.reserve(ids.size());
			uint64_t previous = 0U;
			for (const uint64_t id: ids) {
				if (id == 0U || id <= previous) {
					throw std::invalid_argument("pattern-expression fixture requires sorted Node probes");
				}
				previous = id;
				result.push_back(nodes_.contains(id));
			}
			return result;
		}
		[[nodiscard]] bool supportsNodeExistenceProbes() const override {
			return faults_.supports_node_existence_probes;
		}
		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t id) const override {
			if (faults_.edge_read_count) {
				++*faults_.edge_read_count;
			}
			return edges_.at(id);
		}
		[[nodiscard]] std::vector<LogicalGraphEdge> readEdgesShapedSorted(std::span<const uint64_t> ids,
																		  const EntityReadShape &shape) const override {
			if (faults_.edge_shaped_batch_read_count) {
				++*faults_.edge_shaped_batch_read_count;
			}
			if (faults_.edge_shaped_batch_ids) {
				faults_.edge_shaped_batch_ids->assign(ids.begin(), ids.end());
			}
			return LogicalGraphReadSnapshot::readEdgesShapedSorted(ids, shape);
		}
		[[nodiscard]] bool supportsShapedPropertyReadAdmission() const override {
			return faults_.supports_shaped_property_admission;
		}
		[[nodiscard]] std::vector<LogicalGraphNode>
		readNodesShapedSortedWithAdmission(std::span<const uint64_t> ids, const EntityReadShape &shape,
										   const std::function<void(size_t)> &admission) const override {
			if (faults_.node_shaped_admission_read_count)
				++*faults_.node_shaped_admission_read_count;
			for (const uint64_t id: ids) {
				for (const auto &entry: nodes_.at(id).properties) {
					if (shape.all_properties)
						admission(property_utils::getPropertyValueSize(entry.value) + sizeof(uint32_t));
				}
			}
			auto result = LogicalGraphReadSnapshot::readNodesShapedSorted(ids, shape);
			if (faults_.node_shaped_completed_read_count)
				++*faults_.node_shaped_completed_read_count;
			return result;
		}
		[[nodiscard]] std::vector<LogicalGraphEdge>
		readEdgesShapedSortedWithAdmission(std::span<const uint64_t> ids, const EntityReadShape &shape,
										   const std::function<void(size_t)> &admission) const override {
			if (faults_.edge_shaped_admission_read_count)
				++*faults_.edge_shaped_admission_read_count;
			for (const uint64_t id: ids) {
				for (const auto &entry: edges_.at(id).properties) {
					if (shape.all_properties)
						admission(property_utils::getPropertyValueSize(entry.value) + sizeof(uint32_t));
				}
			}
			return readEdgesShapedSorted(ids, shape);
		}
		[[nodiscard]] bool supportsPropertyReadAdmission() const override {
			return static_cast<bool>(faults_.node_property_column_admission_count) ||
				static_cast<bool>(faults_.edge_property_column_admission_count);
		}
		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSorted(std::span<const uint64_t> ids,
									  std::span<const std::string_view> keys) const override {
			if (faults_.node_property_column_read_count)
				++*faults_.node_property_column_read_count;
			return LogicalGraphReadSnapshot::readNodePropertyColumnsSorted(ids, keys);
		}
		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readNodePropertyColumnsSortedWithAdmission(std::span<const uint64_t> ids,
												   std::span<const std::string_view> keys,
												   const std::function<void(size_t)> &admission) const override {
			if (faults_.node_property_column_admission_count)
				++*faults_.node_property_column_admission_count;
			for (const uint64_t id: ids) {
				for (const std::string_view key: keys) {
					const auto &properties = nodes_.at(id).properties;
					const auto value = std::ranges::find(properties, key, &LogicalGraphPropertyEntry::key);
					if (value != properties.end()) {
						admission(sizeof(uint32_t) + property_utils::getPropertyValueSize(value->value));
					}
				}
			}
			auto columns = readNodePropertyColumnsSorted(ids, keys);
			if (faults_.truncate_selected_property_column && !columns.empty()) columns.front().clear();
			return columns;
		}
		[[nodiscard]] std::vector<std::vector<PropertyValue>>
		readEdgePropertyColumnsSortedWithAdmission(std::span<const uint64_t> ids,
			std::span<const std::string_view> keys, const std::function<void(size_t)> &admission) const override {
			if (faults_.edge_property_column_admission_count) ++*faults_.edge_property_column_admission_count;
			for (const uint64_t id: ids) {
				for (const auto key: keys) {
					const auto &properties = edges_.at(id).properties;
					const auto value = std::ranges::find(properties, key, &LogicalGraphPropertyEntry::key);
					if (value != properties.end())
						admission(sizeof(uint32_t) + property_utils::getPropertyValueSize(value->value));
				}
			}
			if (faults_.edge_property_column_read_count) ++*faults_.edge_property_column_read_count;
			auto columns = LogicalGraphReadSnapshot::readEdgePropertyColumnsSorted(ids, keys);
			if (faults_.truncate_selected_property_column && !columns.empty()) columns.front().clear();
			return columns;
		}

		void visitNodeRefs(size_t maximumRecords,
						   const std::function<bool(const EntityRef &)> &visitor) const override {
			if (!faults_.supports_native_node_ref_scans) {
				LogicalGraphReadSnapshot::visitNodeRefs(maximumRecords, visitor);
				return;
			}
			size_t visited = 0U;
			for (const auto &[id, node]: nodes_) {
				static_cast<void>(node);
				if (visited++ == maximumRecords || !visitor(EntityRef{id, false, 0U, 0U, 0U}))
					return;
			}
		}
		[[nodiscard]] bool supportsNativeNodeRefScans() const override {
			return faults_.supports_native_node_ref_scans;
		}
		void visitNodes(size_t, const std::function<bool(const LogicalGraphNode &)> &) const override {
			throw std::logic_error("pattern-expression runtime uses paged Node scans");
		}
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t after, size_t maximumRecords,
														  size_t maximumMetadataPages) const override {
			if (maximumRecords == 0U || maximumMetadataPages == 0U) {
				throw std::invalid_argument("pattern-expression fixture Node scan needs positive bounds");
			}
			LogicalGraphNodePage page;
			page.metadata_pages_visited = 1U;
			for (const auto &[id, node]: nodes_) {
				if (id <= after) {
					continue;
				}
				if (page.nodes.size() == maximumRecords) {
					page.may_have_more = true;
					break;
				}
				page.nodes.push_back(node);
			}
			return page;
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdges(
				uint64_t nodeId, LogicalGraphReadDirection direction,
				std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const override {
			if (maximumRecords == 0U) {
				throw std::invalid_argument("pattern-expression fixture adjacency scan needs a positive bound");
			}
			std::vector<LogicalGraphEdge> matches;
			for (const auto &[id, edge]: edges_) {
				static_cast<void>(id);
				if ((direction == LogicalGraphReadDirection::LGRD_OUTGOING && edge.source_node_id == nodeId) ||
					(direction == LogicalGraphReadDirection::LGRD_INCOMING && edge.target_node_id == nodeId)) {
					matches.push_back(edge);
				}
			}
			size_t begin = 0U;
			if (continuation.has_value()) {
				while (begin < matches.size() && matches[begin].edge_id != continuation->next_edge_id) {
					++begin;
				}
				if (begin == 0U || begin == matches.size() || matches[begin - 1U].edge_id != continuation->previous_edge_id) {
					throw std::runtime_error("pattern-expression fixture received an invalid adjacency continuation");
				}
			}
			const size_t end = std::min(matches.size(), begin + maximumRecords);
			LogicalGraphAdjacencyPage page;
			page.edges.insert(page.edges.end(), matches.begin() + static_cast<std::ptrdiff_t>(begin),
							  matches.begin() + static_cast<std::ptrdiff_t>(end));
			if (end < matches.size()) {
				page.continuation = {page.edges.back().edge_id, matches[end].edge_id};
			}
			if (faults_.malformed_adjacency_page) {
				return LogicalGraphAdjacencyPage{{}, LogicalGraphAdjacencyContinuation{1U, 2U}};
			}
			if (faults_.discontinuous_adjacency_page && continuation.has_value()) {
				return LogicalGraphAdjacencyPage{{matches.front()}, std::nullopt};
			}
			if (faults_.invalid_adjacency_endpoint && !page.edges.empty()) {
				page.edges.front().source_node_id = 999U;
			}
			return page;
		}
		[[nodiscard]] LogicalGraphAdjacencyTopologyPage scanAdjacentEdgeTopologies(
				uint64_t nodeId, LogicalGraphReadDirection direction,
				std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const override {
			if (faults_.adjacency_topology_scan_count) {
				++*faults_.adjacency_topology_scan_count;
			}
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
		bool supportsAdjacencyTopologyBufferAdmission() const override { return static_cast<bool>(faults_.topology_admission_requests); }
		LogicalGraphAdjacencyTopologyPage scanAdjacentEdgeTopologiesWithBufferAdmission(uint64_t nodeId,
			LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
			size_t maximumRecords, const std::function<void(size_t)> &admission) const override {
			faults_.topology_admission_requests->push_back(maximumRecords);
			const size_t scratchBytes = maximumRecords * 256U;
			admission(scratchBytes + maximumRecords * (sizeof(LogicalGraphEdge) + sizeof(LogicalGraphEdgeTopology) + 64U));
			std::vector<std::byte> scratch(scratchBytes);
			auto page = scanAdjacentEdgeTopologies(nodeId, direction, continuation, maximumRecords);
			std::vector<std::byte>().swap(scratch);
			size_t bytes = page.edges.capacity() * sizeof(LogicalGraphEdgeTopology);
			for (const auto &edge: page.edges) bytes += edge.type.capacity();
			admission(bytes);
			return page;
		}

		void visitAdjacentEdgeTopologies(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumRecords,
										 const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
			if (maximumRecords == 0U || !visitor) {
				throw std::invalid_argument("pattern-expression fixture topology visit needs a positive bound and visitor");
			}
			std::optional<LogicalGraphAdjacencyContinuation> continuation;
		size_t visited = 0U;
		for (;;) {
			const LogicalGraphAdjacencyTopologyPage page =
					scanAdjacentEdgeTopologies(nodeId, direction, continuation, maximumRecords - visited);
			for (const LogicalGraphEdgeTopology &edge: page.edges) {
				++visited;
				if (!visitor(edge) || visited == maximumRecords) {
					return;
				}
			}
			if (!page.continuation.has_value()) {
				return;
			}
			continuation = page.continuation;
		}
		}
		[[nodiscard]] LogicalGraphAdjacencyReferencePage scanAdjacentEdgeRefs(
				uint64_t nodeId, LogicalGraphReadDirection direction,
				std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const override {
			if (!faults_.supports_native_adjacency_refs) {
				throw std::logic_error("pattern-expression fixture has no native adjacency references");
			}
			if (faults_.adjacency_reference_scan_count) {
				++*faults_.adjacency_reference_scan_count;
			}
			const LogicalGraphAdjacencyPage full =
					scanAdjacentEdges(nodeId, direction, std::move(continuation), maximumRecords);
			LogicalGraphAdjacencyReferencePage result;
			result.edges.reserve(full.edges.size());
			for (const LogicalGraphEdge &edge: full.edges) {
				const uint64_t typeToken = edge.type == "R" ? 1U : 2U;
				result.edges.push_back({edge.edge_id, true, edge.source_node_id, edge.target_node_id, typeToken});
			}
			result.continuation = full.continuation;
			return result;
		}
		bool supportsAdjacencyReferenceBufferAdmission() const override {
			return faults_.supports_native_adjacency_refs && static_cast<bool>(faults_.topology_admission_requests);
		}
		LogicalGraphAdjacencyReferencePage scanAdjacentEdgeRefsWithBufferAdmission(uint64_t nodeId,
			LogicalGraphReadDirection direction, std::optional<LogicalGraphAdjacencyContinuation> continuation,
			size_t maximumRecords, const std::function<void(size_t)> &admission) const override {
			faults_.topology_admission_requests->push_back(maximumRecords);
			admission(maximumRecords * (256U + sizeof(EntityRef)));
			auto page = scanAdjacentEdgeRefs(nodeId, direction, continuation, maximumRecords);
			admission(page.edges.capacity() * sizeof(EntityRef));
			return page;
		}
		[[nodiscard]] bool supportsNativeAdjacentEdgeRefScans() const override {
			return faults_.supports_native_adjacency_refs;
		}
		[[nodiscard]] bool supportsNativeAdjacentEdgeTopologyPages() const override { return true; }
		[[nodiscard]] bool supportsNativeRelationshipTypeTokens() const override {
			return faults_.supports_native_adjacency_refs;
		}
		[[nodiscard]] std::optional<uint64_t> findRelationshipTypeToken(std::string_view name) const override {
			if (!faults_.supports_native_adjacency_refs) {
				return std::nullopt;
			}
			return name == "R" ? std::optional<uint64_t>{1U} : std::nullopt;
		}
		void visitEdges(size_t, const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("pattern-expression runtime does not use Edge scans");
		}
		void visitAdjacentEdges(uint64_t, LogicalGraphReadDirection, size_t,
								const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("pattern-expression runtime uses paged adjacency scans");
		}

	private:
		FixtureNodes nodes_;
		FixtureEdges edges_;
		PatternExpressionSnapshotFaults faults_;
	};

	class PatternExpressionBackend final : public LogicalGraphReadBackend {
	public:
		explicit PatternExpressionBackend(PatternExpressionGraph graph = makeDefaultPatternExpressionGraph(),
									  PatternExpressionSnapshotFaults faults = {})
			: graph_(std::move(graph)), faults_(faults) {}

		[[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
			return std::make_unique<PatternExpressionSnapshot>(graph_, faults_);
		}

	private:
		PatternExpressionGraph graph_;
		PatternExpressionSnapshotFaults faults_;
	};

	[[nodiscard]] LogicalPlanReadRuntimeBounds bounds() {
		return {.maximum_node_records = 16U,
				.maximum_node_page_records = 2U,
				.maximum_metadata_pages_per_node_page = 4U,
				.maximum_edges_per_node = 8U,
				.maximum_edge_records = 32U,
				.maximum_adjacency_page_records = 1U,
				.maximum_property_entries = 8U,
				.maximum_rows = 16U,
				.maximum_distinct_rows = 16U};
	}

	[[nodiscard]] QueryPlan parse(const std::string &query) {
		graph::parser::cypher::CypherParserImpl parser;
		return parser.parseToLogical(query);
	}

	[[nodiscard]] std::unique_ptr<graph::query::logical::LogicalProject>
	makeShortestPathRoot(std::string startLabel, std::string endLabel, std::string relType,
						 graph::query::expressions::PatternDirection direction, int minHops, int maxHops, bool all) {
		auto left = std::make_unique<graph::query::logical::LogicalNodeScan>(
				"a", std::vector<std::string>{std::move(startLabel)},
				std::vector<std::pair<std::string, PropertyValue>>{});
		auto right = std::make_unique<graph::query::logical::LogicalNodeScan>(
				"b", std::vector<std::string>{std::move(endLabel)},
				std::vector<std::pair<std::string, PropertyValue>>{});
		auto join = std::make_unique<graph::query::logical::LogicalJoin>(std::move(left), std::move(right));
		std::vector<graph::query::logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<graph::query::expressions::ShortestPathExpression>(
						   "a", "b", std::move(relType), direction, minHops, maxHops, all),
					   "path");
		return std::make_unique<graph::query::logical::LogicalProject>(std::move(join), std::move(items));
	}

	[[nodiscard]] int64_t entityId(const PropertyValue &entity) {
		return std::get<int64_t>(entity.getMap().at("_id").getVariant());
	}

	[[nodiscard]] PatternExpressionGraph makeDiamondGraph() {
		PatternExpressionGraph graph;
		graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
		graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
		graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Middle"}});
		graph.nodes.emplace(4U, LogicalGraphNode{4U, {"Middle"}});
		// Map order makes the route through Node 4 visible first during BFS. The
		// runtime must nevertheless materialize all paths in parent-ID order.
		graph.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 4U, "R"});
		graph.edges.emplace(12U, LogicalGraphEdge{12U, 4U, 2U, "R"});
		graph.edges.emplace(13U, LogicalGraphEdge{13U, 1U, 3U, "R"});
		graph.edges.emplace(14U, LogicalGraphEdge{14U, 3U, 2U, "R"});
		return graph;
	}

	[[nodiscard]] PatternExpressionGraph makeLayeredPathGraph() {
		PatternExpressionGraph graph;
		graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
		for (uint64_t nodeId = 2U; nodeId <= 11U; ++nodeId) {
			graph.nodes.emplace(nodeId, LogicalGraphNode{nodeId, {"Middle"}});
		}
		graph.nodes.emplace(12U, LogicalGraphNode{12U, {"End"}});
		uint64_t edgeId = 1U;
		for (uint64_t target = 2U; target <= 3U; ++target) {
			graph.edges.emplace(edgeId, LogicalGraphEdge{edgeId++, 1U, target, "R"});
		}
		for (uint64_t firstLayer = 2U; firstLayer <= 8U; firstLayer += 2U) {
			for (uint64_t source = firstLayer; source <= firstLayer + 1U; ++source) {
				for (uint64_t target = firstLayer + 2U; target <= firstLayer + 3U; ++target) {
					graph.edges.emplace(edgeId, LogicalGraphEdge{edgeId++, source, target, "R"});
				}
			}
		}
		for (uint64_t source = 10U; source <= 11U; ++source) {
			graph.edges.emplace(edgeId, LogicalGraphEdge{edgeId++, source, 12U, "R"});
		}
		return graph;
	}

} // namespace
