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
#include "graph/query/expressions/ShortestPathExpression.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
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
			if (faults_.wrong_node_identity && id != nodes_.begin()->first) {
				return nodes_.begin()->second;
			}
			return nodes_.at(id);
		}
		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t id) const override { return edges_.at(id); }
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
