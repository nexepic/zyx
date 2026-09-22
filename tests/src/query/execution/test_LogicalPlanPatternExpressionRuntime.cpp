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
		bool malformed_adjacency_page = false;
		bool invalid_adjacency_endpoint = false;
		bool wrong_node_identity = false;
		bool discontinuous_adjacency_page = false;
		bool supports_node_existence_probes = false;
		bool supports_native_adjacency_refs = false;
		std::shared_ptr<size_t> node_read_count;
		std::shared_ptr<size_t> edge_read_count;
		std::shared_ptr<size_t> node_existence_probe_count;
		std::shared_ptr<size_t> node_existence_batch_probe_count;
		std::shared_ptr<size_t> adjacency_topology_scan_count;
		std::shared_ptr<size_t> adjacency_reference_scan_count;
		std::shared_ptr<size_t> edge_shaped_batch_read_count;
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
		[[nodiscard]] std::vector<LogicalGraphEdge> readEdgesShapedSorted(
				std::span<const uint64_t> ids, const EntityReadShape &shape) const override {
			if (faults_.edge_shaped_batch_read_count) {
				++*faults_.edge_shaped_batch_read_count;
			}
			if (faults_.edge_shaped_batch_ids) {
				faults_.edge_shaped_batch_ids->assign(ids.begin(), ids.end());
			}
			return LogicalGraphReadSnapshot::readEdgesShapedSorted(ids, shape);
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

TEST(LogicalPlanPatternExpressionRuntimeTest, ExecutesExistsWithTypedTargetAndScopedWherePredicate) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) WHERE EXISTS((n)-[:KNOWS]->(m:Friend) WHERE m.age > 21) RETURN n.name AS name");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("name").asPrimitive(), PropertyValue("Ada"));
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ExcludesExistingRelationshipsThatFailTheExistsWherePredicate) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse(
			"MATCH (n:Person) WHERE EXISTS((n)-[:KNOWS]->(m:Friend) WHERE m.age > 30) RETURN n.name AS name");
	ASSERT_NE(plan.root, nullptr);

	EXPECT_EQ(runtime.executeToQueryResult(*plan.root, bounds()).rowCount(), 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, UsesNativeAdjacencyReferencesForTypedExistsPatterns) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Person"}, {{"name", PropertyValue("Ada")}}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Friend"}, {{"age", PropertyValue(30)}}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.adjacency_reference_scan_count = std::make_shared<size_t>(0U);
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
		faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) WHERE EXISTS((n)-[:R]->(m:Friend) WHERE m.age > 21) RETURN n.name AS name");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("name").asPrimitive(), PropertyValue("Ada"));
	EXPECT_GT(*faults.adjacency_reference_scan_count, 0U);
	EXPECT_EQ(*faults.adjacency_topology_scan_count, 0U);
		EXPECT_EQ(*faults.node_existence_probe_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, NativeReferenceExistsStopsBeforeMaterializingLaterMatches) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Person"}, {{"name", PropertyValue("Ada")}}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Friend"}, {{"age", PropertyValue(30)}}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Friend"}, {{"age", PropertyValue(40)}}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.adjacency_reference_scan_count = std::make_shared<size_t>(0U);
	faults.edge_read_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse(
			"MATCH (n:Person) WHERE EXISTS((n)-[:R]->(m:Friend) WHERE m.age > 21) RETURN n.name AS name");
	ASSERT_NE(plan.root, nullptr);
	LogicalPlanReadRuntimeBounds queryBounds = bounds();
	queryBounds.maximum_adjacency_page_records = 2U;

	const QueryResult result = runtime.executeToQueryResult(*plan.root, queryBounds);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("name").asPrimitive(), PropertyValue("Ada"));
	EXPECT_EQ(*faults.adjacency_reference_scan_count, 1U);
	// EXISTS can stop after the first matching candidate; later full Edge payloads
	// must not be prefetched merely because they share one topology page.
	EXPECT_EQ(*faults.edge_read_count, 1U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, BatchesEndpointExistenceForTopologyOnlyExistsPatterns) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Person"}, {{"name", PropertyValue("Ada")}}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Friend"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Other"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 3U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 2U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.supports_node_existence_probes = true;
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	PatternExpressionSnapshot snapshot(std::move(graph), faults);
	LogicalPlanReadRow row;
	row.values.emplace("n", EntityRef{1U, false, 0U, 0U, 0U});
	graph::query::expressions::ExistsExpression expression(
			"(n)-[:R]->()", "n", "R", "", graph::query::expressions::PatternDirection::PAT_OUTGOING);
	QueryContext context;
	LogicalPlanReadRuntimeBounds batchBounds = bounds();
	batchBounds.maximum_adjacency_page_records = 2U;
	const LogicalPlanReadValue result = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, expression, row, batchBounds, context);
	ASSERT_TRUE(std::holds_alternative<PropertyValue>(result));
	EXPECT_EQ(std::get<PropertyValue>(result), PropertyValue(true));
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	EXPECT_GT(*faults.node_existence_batch_probe_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest,
		GenericTraversalPrefiltersRelationshipTypeBeforeCompleteEdgeRead) {
	PatternExpressionGraph graph = makeDefaultPatternExpressionGraph();
	graph.edges.at(10U).properties.push_back({"weight", PropertyValue(int64_t{7})});
	graph.edges.at(11U).properties.push_back({"weight", PropertyValue(int64_t{7})});
	PatternExpressionSnapshotFaults faults;
	faults.edge_shaped_batch_read_count = std::make_shared<size_t>(0U);
	faults.edge_shaped_batch_ids = std::make_shared<std::vector<uint64_t>>();
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	// The projection deliberately prevents the direct-traversal shortcut, so this
	// exercises the ordinary adjacency-page consumer rather than a query-specific
	// global/reference fast path.
	auto source = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "n");
	auto projectedSource = std::make_unique<logical::LogicalProject>(std::move(source), std::move(items));
	logical::LogicalTraversal traversal(
			std::move(projectedSource), "n", "r", "m", "KNOWS", "out", {}, {},
			{{"weight", PropertyValue(int64_t{7})}});

	std::vector<uint64_t> edgeIds;
	LogicalPlanReadRuntimeBounds traversalBounds = bounds();
	traversalBounds.maximum_adjacency_page_records = 2U;
	static_cast<void>(runtime.execute(traversal, traversalBounds, [&edgeIds](const LogicalPlanReadRow &row) {
		edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
		return true;
	}));

	EXPECT_EQ(edgeIds, std::vector<uint64_t>{10U});
	ASSERT_EQ(*faults.edge_shaped_batch_read_count, 1U);
	EXPECT_EQ(*faults.edge_shaped_batch_ids, std::vector<uint64_t>{10U});
}

TEST(LogicalPlanPatternExpressionRuntimeTest, DoesNotMaterializeTopologyEdgesRejectedByRelationshipType) {
	PatternExpressionGraph graph = makeDefaultPatternExpressionGraph();
	PatternExpressionSnapshotFaults faults;
	faults.edge_shaped_batch_read_count = std::make_shared<size_t>(0U);
	faults.edge_shaped_batch_ids = std::make_shared<std::vector<uint64_t>>();
	PatternExpressionSnapshot snapshot(std::move(graph), faults);
	LogicalPlanReadRow row;
	row.values.emplace("n", EntityRef{1U, false, 0U, 0U, 0U});
	graph::query::expressions::ExistsExpression expression(
			"(n)-[:KNOWS]->(m)", "n", "KNOWS", "", graph::query::expressions::PatternDirection::PAT_OUTGOING);
	QueryContext context;
	LogicalPlanReadRuntimeBounds expressionBounds = bounds();
	expressionBounds.maximum_adjacency_page_records = 2U;
	const LogicalPlanReadValue result = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, expression, row, expressionBounds, context);
	ASSERT_TRUE(std::holds_alternative<PropertyValue>(result));
	EXPECT_EQ(std::get<PropertyValue>(result), PropertyValue(true));
	ASSERT_EQ(*faults.edge_shaped_batch_read_count, 1U);
	ASSERT_EQ(*faults.edge_shaped_batch_ids, std::vector<uint64_t>({10U}));
}

TEST(LogicalPlanPatternExpressionRuntimeTest, EvaluatesLengthForANamedSingleRelationshipPath) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse(
			"MATCH p=(a:Person)-[:KNOWS]->(b:Friend) "
			"RETURN length(p) AS relationship_count, size(nodes(p)) AS node_count, "
			"size(relationships(p)) AS relationship_list_count");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("relationship_count").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(row.at("node_count").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("relationship_list_count").asPrimitive(), PropertyValue(1));
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ResolvesParameterizedNodePropertyPredicatesAtExecutionTime) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);

	logical::LogicalNodeScan scan("n", {"Person"});
	scan.setParameterPropertyPredicates({{"name", "name"}});
	QueryContext context;
	context.parameters.emplace("name", PropertyValue("Ada"));
	const QueryResult matching = runtime.executeToQueryResult(scan, bounds(), context);
	ASSERT_EQ(matching.rowCount(), 1U);
	EXPECT_EQ(matching.getRows().front().at("n").asLogicalNode().node_id, 1U);

	context.parameters.insert_or_assign("name", PropertyValue());
	const QueryResult nullPredicate = runtime.executeToQueryResult(scan, bounds(), context);
	EXPECT_EQ(nullPredicate.rowCount(), 0U);

	QueryContext missingParameter;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(scan, bounds(), missingParameter)), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, EvaluatesEntityIntrospectionFunctionsFromParserProducedPlans) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse(
			"MATCH (n:Person)-[r:KNOWS]->(m:Friend) "
			"RETURN id(n) AS node_id, labels(n) AS node_labels, id(r) AS edge_id, type(r) AS edge_type, "
			"keys(n) AS node_keys, properties(n) AS node_properties, keys(r) AS edge_keys, properties(r) AS edge_properties");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("node_id").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(row.at("node_labels").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("Person")}));
	EXPECT_EQ(row.at("edge_id").asPrimitive(), PropertyValue(10));
	EXPECT_EQ(row.at("edge_type").asPrimitive(), PropertyValue("KNOWS"));
	EXPECT_EQ(row.at("node_keys").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("name")}));
	EXPECT_EQ(row.at("node_properties").asPrimitive(),
			  PropertyValue(PropertyValue::MapType{{"name", PropertyValue("Ada")}}));
	EXPECT_EQ(row.at("edge_keys").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
	EXPECT_EQ(row.at("edge_properties").asPrimitive(), PropertyValue(PropertyValue::MapType{}));
}

TEST(LogicalPlanPatternExpressionRuntimeTest, EvaluatesMapProjectionThroughTheLogicalStorageBoundary) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse(
			"MATCH (n:Person) WHERE n.name = 'Ada' "
			"RETURN n {.*, .name, missing: null, greeting: 'hello'} AS projection");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const PropertyValue projection = result.getRows().front().at("projection").asPrimitive();
	ASSERT_EQ(projection.getType(), PropertyType::MAP);
	EXPECT_EQ(projection.getMap().at("name"), PropertyValue("Ada"));
	EXPECT_EQ(projection.getMap().at("missing"), PropertyValue());
	EXPECT_EQ(projection.getMap().at("greeting"), PropertyValue("hello"));
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ProducesCompletePatternComprehensionAndPreservesOuterBindings) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) RETURN n.name AS name, [(n)-[:KNOWS]->(m) WHERE m.age > 21 | m.age] AS ages");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows().front().at("name").asPrimitive(), PropertyValue("Ada"));
	EXPECT_EQ(result.getRows().front().at("ages").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(30)}));
	EXPECT_EQ(result.getRows().back().at("ages").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
}

TEST(LogicalPlanPatternExpressionRuntimeTest, BoundsPatternComprehensionContainerOverhead) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) RETURN [(n)-[:KNOWS]->(m) | m.age] AS ages");
	ASSERT_NE(plan.root, nullptr);
	LogicalPlanReadRuntimeBounds limited = bounds();
	// The mapped integer fits by itself, but the returned list also needs its
	// collection envelope and must be rejected under the tighter bound.
	limited.maximum_materialized_bytes = 8U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limited)), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, BatchesEndpointExistenceForCompleteTopologyPages) {
	PatternExpressionSnapshotFaults faults;
	faults.supports_node_existence_probes = true;
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(makeDefaultPatternExpressionGraph(), faults);
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) RETURN [(n)-[:KNOWS]->(m) | m.age] AS ages");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows().front().at("ages").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(30)}));
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	EXPECT_GT(*faults.node_existence_batch_probe_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest,
	 BindsTheRelationshipWhenAPatternComprehensionUsesAnAnonymousTarget) {
	PatternExpressionGraph graph = makeDefaultPatternExpressionGraph();
	graph.edges.at(10U).properties.push_back({"weight", PropertyValue(int64_t{7})});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse(
			"MATCH (n:Person) RETURN n.name AS name, [(n)-[r:KNOWS]->() | r.weight] AS weights");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows().front().at("name").asPrimitive(), PropertyValue("Ada"));
	EXPECT_EQ(result.getRows().front().at("weights").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t{7})}));
	EXPECT_EQ(result.getRows().back().at("weights").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
}

TEST(LogicalPlanPatternExpressionRuntimeTest,
	 PreservesUndirectedPatternSemanticsAcrossSelfLoopsTypesAndTargetLabels) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Seed"}, {}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Friend"}, {{"age", PropertyValue(30)}}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Other"}, {}});
	// A correct undirected traversal observes the self-loop in both directions,
	// but returns it only once. The other two edges exercise type and label
	// filtering without requiring a larger graph fixture.
	graph.edges.emplace(10U, LogicalGraphEdge{10U, 1U, 1U, "KNOWS"});
	graph.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, "KNOWS"});
	graph.edges.emplace(12U, LogicalGraphEdge{12U, 3U, 1U, "OTHER"});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Seed) RETURN [(n)-[:KNOWS]-(m:Friend) | m.age] AS ages");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("ages").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(30)}));
}

TEST(LogicalPlanPatternExpressionRuntimeTest, DoesNotRebindPreexistingPatternTargets) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);

	QueryPlan matchingExistsPlan = parse(
			"MATCH (n:Person), (m:Friend) WHERE EXISTS((n)-[:KNOWS]->(m)) RETURN n.name AS name");
	ASSERT_NE(matchingExistsPlan.root, nullptr);
	const QueryResult matching = runtime.executeToQueryResult(*matchingExistsPlan.root, bounds());
	ASSERT_EQ(matching.rowCount(), 1U);
	EXPECT_EQ(matching.getRows().front().at("name").asPrimitive(), PropertyValue("Ada"));

	QueryPlan existsPlan = parse(
			"MATCH (n:Person), (m:Robot) WHERE EXISTS((n)-[:KNOWS]->(m)) RETURN n.name AS name");
	ASSERT_NE(existsPlan.root, nullptr);
	EXPECT_EQ(runtime.executeToQueryResult(*existsPlan.root, bounds()).rowCount(), 0U);

	QueryPlan comprehensionPlan = parse(
			"MATCH (n:Person), (m:Robot) RETURN n.name AS name, [(n)-[:KNOWS]->(m) | m.age] AS ages");
	ASSERT_NE(comprehensionPlan.root, nullptr);
	const QueryResult result = runtime.executeToQueryResult(*comprehensionPlan.root, bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	for (const auto &row: result.getRows()) {
		EXPECT_EQ(row.at("ages").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
	}
}

TEST(LogicalPlanPatternExpressionRuntimeTest, EvaluatesStructuredNodeOnlyPatternsAndMissingSourceBindings) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);

	std::vector<logical::LogicalProjectItem> missingExistsItems;
	missingExistsItems.emplace_back(std::make_shared<expressions::ExistsExpression>(
														"(n)", "n", "", "", expressions::PatternDirection::PAT_OUTGOING),
														"exists");
	logical::LogicalProject missingExists(std::make_unique<logical::LogicalSingleRow>(), std::move(missingExistsItems));
	const QueryResult missingExistsResult = runtime.executeToQueryResult(missingExists, bounds());
	ASSERT_EQ(missingExistsResult.rowCount(), 1U);
	EXPECT_EQ(missingExistsResult.getRows().front().at("exists").asPrimitive(), PropertyValue(false));

	std::vector<logical::LogicalProjectItem> unstructuredExistsItems;
	unstructuredExistsItems.emplace_back(std::make_shared<expressions::ExistsExpression>("(n)"), "exists");
	logical::LogicalProject unstructuredExists(std::make_unique<logical::LogicalSingleRow>(),
				std::move(unstructuredExistsItems));
	const QueryResult unstructuredExistsResult = runtime.executeToQueryResult(unstructuredExists, bounds());
	ASSERT_EQ(unstructuredExistsResult.rowCount(), 1U);
	EXPECT_EQ(unstructuredExistsResult.getRows().front().at("exists").asPrimitive(), PropertyValue(false));

	std::vector<logical::LogicalProjectItem> nodeOnlyExistsItems;
	nodeOnlyExistsItems.emplace_back(std::make_shared<expressions::ExistsExpression>(
														"(n)", "n", "", "", expressions::PatternDirection::PAT_OUTGOING),
														"exists");
	logical::LogicalProject nodeOnlyExists(
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}),
				std::move(nodeOnlyExistsItems));
	const QueryResult nodeOnlyExistsResult = runtime.executeToQueryResult(nodeOnlyExists, bounds());
	ASSERT_EQ(nodeOnlyExistsResult.rowCount(), 2U);
	for (const auto &row: nodeOnlyExistsResult.getRows()) {
		EXPECT_EQ(row.at("exists").asPrimitive(), PropertyValue(true));
	}

	auto mapValue = std::make_unique<expressions::LiteralExpression>(std::string("mapped"));
	std::vector<logical::LogicalProjectItem> missingComprehensionItems;
	missingComprehensionItems.emplace_back(std::make_shared<expressions::PatternComprehensionExpression>(
														"(n)", "n", "target", "", "", expressions::PatternDirection::PAT_OUTGOING,
														std::move(mapValue)),
														"values");
	logical::LogicalProject missingComprehension(std::make_unique<logical::LogicalSingleRow>(),
				std::move(missingComprehensionItems));
	const QueryResult missingComprehensionResult = runtime.executeToQueryResult(missingComprehension, bounds());
	ASSERT_EQ(missingComprehensionResult.rowCount(), 1U);
	EXPECT_EQ(missingComprehensionResult.getRows().front().at("values").asPrimitive(),
				  PropertyValue(std::vector<PropertyValue>{}));

	std::vector<logical::LogicalProjectItem> unstructuredComprehensionItems;
	unstructuredComprehensionItems.emplace_back(std::make_shared<expressions::PatternComprehensionExpression>(
														"(n)", "target",
														std::make_unique<expressions::LiteralExpression>(std::string("mapped"))),
														"values");
	logical::LogicalProject unstructuredComprehension(std::make_unique<logical::LogicalSingleRow>(),
				std::move(unstructuredComprehensionItems));
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(unstructuredComprehension, bounds())), std::logic_error);

	std::vector<logical::LogicalProjectItem> nodeOnlyComprehensionItems;
	nodeOnlyComprehensionItems.emplace_back(std::make_shared<expressions::PatternComprehensionExpression>(
														"(n)", "n", "target", "", "", expressions::PatternDirection::PAT_OUTGOING,
														std::make_unique<expressions::LiteralExpression>(std::string("mapped"))),
														"values");
	logical::LogicalProject nodeOnlyComprehension(
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}),
				std::move(nodeOnlyComprehensionItems));
	const QueryResult nodeOnlyComprehensionResult = runtime.executeToQueryResult(nodeOnlyComprehension, bounds());
	ASSERT_EQ(nodeOnlyComprehensionResult.rowCount(), 2U);
	for (const auto &row: nodeOnlyComprehensionResult.getRows()) {
		const PropertyValue values = row.at("values").asPrimitive();
		ASSERT_EQ(values.getType(), PropertyType::LIST);
		ASSERT_EQ(values.getList().size(), 1U);
		EXPECT_EQ(values.getList().front(), PropertyValue("mapped"));
	}
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsTruncatedPatternComprehensionInsteadOfReturningAPrefix) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) RETURN [(n)-->(m) | m] AS targets");
	ASSERT_NE(plan.root, nullptr);
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_edges_per_node = 1U;
	limited.maximum_adjacency_page_records = 1U;

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limited)), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, MaterializesBoundedShortestPathAndAllShortestPaths) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const auto makeRoot = [](bool all) {
		auto left = std::make_unique<graph::query::logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"name", PropertyValue("Ada")}});
		auto right = std::make_unique<graph::query::logical::LogicalNodeScan>("b", std::vector<std::string>{"Friend"});
		auto join = std::make_unique<graph::query::logical::LogicalJoin>(std::move(left), std::move(right));
		std::vector<graph::query::logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<graph::query::expressions::ShortestPathExpression>(
						   "a", "b", "KNOWS", graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 3, all),
						   "path");
		return graph::query::logical::LogicalProject(std::move(join), std::move(items));
	};

	auto shortest = makeRoot(false);
	const QueryResult result = runtime.executeToQueryResult(shortest, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &path = result.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(path.size(), 3U);
	EXPECT_EQ(std::get<std::string>(path[0].getMap().at("_type").getVariant()), "node");
	EXPECT_EQ(std::get<int64_t>(path[1].getMap().at("_id").getVariant()), 10);

	auto allShortest = makeRoot(true);
	const QueryResult allResult = runtime.executeToQueryResult(allShortest, bounds());
	const auto &paths = allResult.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(paths.size(), 1U);
	EXPECT_EQ(paths.front().getType(), PropertyType::LIST);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathUsesNativeEndpointProbesBeforePathMaterialization) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Detour"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_node_existence_probes = true;
	faults.node_read_count = std::make_shared<size_t>(0U);
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 1, false);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(entityId(result.getRows().front().at("path").asPrimitive().getList()[1]), 1);
	// Node-scan bindings are already authenticated; the topology-only BFS does
	// not need a second point probe before materializing the returned path.
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	// The start and end records are materialized only while building the returned
	// path. The non-path frontier Node is never fully decoded.
	EXPECT_EQ(*faults.node_read_count, 2U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathSeedsBfsFromEntityIdsWithoutReadingEndpoints) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}, {{"payload", PropertyValue("large")}}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}, {{"payload", PropertyValue("large")}}});
	PatternExpressionSnapshotFaults faults;
	faults.node_read_count = std::make_shared<size_t>(0U);
	faults.supports_node_existence_probes = true;
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	PatternExpressionSnapshot snapshot(std::move(graph), faults);
	LogicalPlanReadRow row;
	row.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
	row.values.emplace("b", EntityRef{2U, false, 0U, 0U, 0U});
	graph::query::expressions::ShortestPathExpression expression(
		"a", "b", "R", graph::query::expressions::PatternDirection::PAT_OUTGOING, 0, 0, false);
	graph::query::QueryContext context;
	const LogicalPlanReadValue result = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, expression, row, bounds(), context);
	ASSERT_TRUE(std::holds_alternative<PropertyValue>(result));
	EXPECT_EQ(std::get<PropertyValue>(result).getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(*faults.node_read_count, 0U);
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	EXPECT_EQ(*faults.node_existence_batch_probe_count, 1U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathUsesTypedNativeAdjacencyReferences) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	faults.adjacency_reference_scan_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 1, false);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(entityId(result.getRows().front().at("path").asPrimitive().getList()[1]), 1);
	EXPECT_EQ(*faults.adjacency_reference_scan_count, 1U);
	EXPECT_EQ(*faults.adjacency_topology_scan_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathUsesUntypedNativeAdjacencyReferences) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "OTHER"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	faults.adjacency_reference_scan_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 1, false);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(entityId(result.getRows().front().at("path").asPrimitive().getList()[1]), 1);
	EXPECT_EQ(*faults.adjacency_reference_scan_count, 1U);
	EXPECT_EQ(*faults.adjacency_topology_scan_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, AnchoredRelationshipCountUsesTypedNativeAdjacencyReferences) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Other"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "OTHER"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.supports_node_existence_probes = true;
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	faults.adjacency_reference_scan_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto source = std::make_unique<graph::query::logical::LogicalNodeScan>("a", std::vector<std::string>{"Start"});
	auto traversal = std::make_unique<graph::query::logical::LogicalTraversal>(
			std::move(source), "a", "r", "b", "R", "out");
	graph::query::logical::LogicalAggregate aggregate(
			std::move(traversal), {},
			{{"count", std::make_shared<graph::query::expressions::VariableReferenceExpression>("b"), "count", false}});

	std::vector<PropertyValue> results;
	static_cast<void>(runtime.execute(aggregate, bounds(), [&results](const LogicalPlanReadRow &row) {
		results.push_back(std::get<PropertyValue>(row.values.at("count")));
		return true;
	}));

	EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{1})});
	EXPECT_GT(*faults.adjacency_reference_scan_count, 0U);
	EXPECT_EQ(*faults.adjacency_topology_scan_count, 0U);
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	EXPECT_GT(*faults.node_existence_batch_probe_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, AnchoredRelationshipCountUsesUntypedNativeAdjacencyReferences) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Other"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "OTHER"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.supports_node_existence_probes = true;
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	faults.adjacency_reference_scan_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto source = std::make_unique<graph::query::logical::LogicalNodeScan>("a", std::vector<std::string>{"Start"});
	auto traversal = std::make_unique<graph::query::logical::LogicalTraversal>(
			std::move(source), "a", "r", "b", "", "out");
	graph::query::logical::LogicalAggregate aggregate(
			std::move(traversal), {},
			{{"count", std::make_shared<graph::query::expressions::VariableReferenceExpression>("b"), "count", false}});

	std::vector<PropertyValue> results;
	static_cast<void>(runtime.execute(aggregate, bounds(), [&results](const LogicalPlanReadRow &row) {
		results.push_back(std::get<PropertyValue>(row.values.at("count")));
		return true;
	}));

	EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{2})});
	EXPECT_GT(*faults.adjacency_reference_scan_count, 0U);
	EXPECT_EQ(*faults.adjacency_topology_scan_count, 0U);
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	EXPECT_GT(*faults.node_existence_batch_probe_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, AnchoredRelationshipCountBatchesTopologyPageEndpointProbes) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Other"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_node_existence_probes = true;
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto source = std::make_unique<graph::query::logical::LogicalNodeScan>("a", std::vector<std::string>{"Start"});
	auto traversal = std::make_unique<graph::query::logical::LogicalTraversal>(
			std::move(source), "a", "r", "b", "", "out");
	graph::query::logical::LogicalAggregate aggregate(
			std::move(traversal), {},
			{{"count", std::make_shared<graph::query::expressions::VariableReferenceExpression>("b"), "count", false}});

	std::vector<PropertyValue> results;
	static_cast<void>(runtime.execute(aggregate, bounds(), [&results](const LogicalPlanReadRow &row) {
		results.push_back(std::get<PropertyValue>(row.values.at("count")));
		return true;
	}));

	EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{2})});
	EXPECT_GT(*faults.adjacency_topology_scan_count, 0U);
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	EXPECT_GT(*faults.node_existence_batch_probe_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, MaterializesAllEqualShortestPathsInDeterministicParentOrder) {
	PatternExpressionBackend backend(makeDiamondGraph());
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 3, true);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &paths = result.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(paths.size(), 2U);
	ASSERT_EQ(paths[0].getType(), PropertyType::LIST);
	ASSERT_EQ(paths[1].getType(), PropertyType::LIST);
	const auto &first = paths[0].getList();
	const auto &second = paths[1].getList();
	ASSERT_EQ(first.size(), 5U);
	ASSERT_EQ(second.size(), 5U);
	EXPECT_EQ(entityId(first[1]), 13);
	EXPECT_EQ(entityId(first[2]), 3);
	EXPECT_EQ(entityId(first[3]), 14);
	EXPECT_EQ(entityId(second[1]), 11);
	EXPECT_EQ(entityId(second[2]), 4);
	EXPECT_EQ(entityId(second[3]), 12);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ReturnsNullWhenNoTypedShortestPathExists) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "OTHER"});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 3, false);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("path").asPrimitive().getType(), PropertyType::NULL_TYPE);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, HonorsTheMinimumHopBoundInsteadOfReturningATooShortRoute) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Middle"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "R"});
	graph.edges.emplace(3U, LogicalGraphEdge{3U, 3U, 2U, "R"});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 2, 2, false);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &path = result.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(path.size(), 5U);
	EXPECT_EQ(entityId(path[1]), 2);
	EXPECT_EQ(entityId(path[3]), 3);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, MaterializesZeroHopPathWhenStartAndEndAreTheSameNode) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Anchor"}});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Anchor", "Anchor", "",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 0, 0, false);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &path = result.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(path.size(), 1U);
	EXPECT_EQ(entityId(path.front()), 1);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, SupportsIncomingAndUndirectedTypedShortestPaths) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.edges.emplace(7U, LogicalGraphEdge{7U, 2U, 1U, "BACK"});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);

	auto incoming = makeShortestPathRoot("Start", "End", "BACK",
			graph::query::expressions::PatternDirection::PAT_INCOMING, 1, 1, false);
	const QueryResult incomingResult = runtime.executeToQueryResult(*incoming, bounds());
	ASSERT_EQ(incomingResult.rowCount(), 1U);
	EXPECT_EQ(entityId(incomingResult.getRows().front().at("path").asPrimitive().getList()[1]), 7);

	auto undirected = makeShortestPathRoot("Start", "End", "BACK",
			graph::query::expressions::PatternDirection::PAT_BOTH, 1, 1, false);
	const QueryResult undirectedResult = runtime.executeToQueryResult(*undirected, bounds());
	ASSERT_EQ(undirectedResult.rowCount(), 1U);
	EXPECT_EQ(entityId(undirectedResult.getRows().front().at("path").asPrimitive().getList()[1]), 7);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsShortestPathWhenAnAdjacencyPageExceedsThePerNodeBound) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Other"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "R"});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 2, false);
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_edges_per_node = 1U;
	limited.maximum_adjacency_page_records = 1U;

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*root, limited)), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsShortestPathWhenTheParentDagWouldExceedItsBound) {
	PatternExpressionBackend backend(makeDiamondGraph());
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 3, true);
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_rows = 3U;

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*root, limited)), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsShortestPathWhenTraversalStateExceedsByteBound) {
	PatternExpressionBackend backend(makeLayeredPathGraph());
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 6, false);
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_node_records = 12U;
	limited.maximum_rows = 20U;
	limited.maximum_edges_per_node = 2U;
	limited.maximum_adjacency_page_records = 1U;
	limited.maximum_materialized_bytes = 128U;

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*root, limited)), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsShortestPathWhenAllPathMaterializationWouldExceedItsBound) {
	PatternExpressionBackend backend(makeLayeredPathGraph());
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 6, true);
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_node_records = 12U;
	limited.maximum_rows = 20U;
	limited.maximum_edges_per_node = 2U;
	limited.maximum_adjacency_page_records = 1U;

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*root, limited)), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsMalformedShortestPathAdjacencyPagesAndEndpoints) {
	const auto executeFaultyPath = [](PatternExpressionSnapshotFaults faults) {
		PatternExpressionBackend backend(makeDefaultPatternExpressionGraph(), faults);
		LogicalPlanReadRuntime runtime(backend);
		auto root = makeShortestPathRoot("Person", "Friend", "KNOWS",
				graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 2, false);
		return runtime.executeToQueryResult(*root, bounds());
	};

	EXPECT_THROW(static_cast<void>(executeFaultyPath({.malformed_adjacency_page = true})), std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeFaultyPath({.invalid_adjacency_endpoint = true})), std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeFaultyPath({.wrong_node_identity = true})), std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeFaultyPath({.discontinuous_adjacency_page = true})), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsADiscontinuousPatternExpressionAdjacencyPage) {
	PatternExpressionBackend backend(makeDefaultPatternExpressionGraph(), {.discontinuous_adjacency_page = true});
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) WHERE EXISTS((n)-[:OTHER]->(m)) RETURN n");
	ASSERT_NE(plan.root, nullptr);

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds())), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsAPatternExpressionTargetWithTheWrongLogicalIdentity) {
	PatternExpressionBackend backend(makeDefaultPatternExpressionGraph(), {.wrong_node_identity = true});
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) WHERE EXISTS((n)-[:KNOWS]->(m:Friend)) RETURN n");
	ASSERT_NE(plan.root, nullptr);

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds())), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsShortestPathWithMalformedNodeMetadata) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start", "Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 1, false);

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*root, bounds())), std::runtime_error);
}
