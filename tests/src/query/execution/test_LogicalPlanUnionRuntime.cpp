/**
 * @file test_LogicalPlanUnionRuntime.cpp
 * @brief Logical UNION execution tests independent of physical graph storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

#include <gtest/gtest.h>

#include "CypherParserImpl.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/storage/LogicalGraphRead.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::execution;
using namespace graph::storage;

namespace {

	class NoGraphSnapshot final : public LogicalGraphReadSnapshot {
	public:
		[[nodiscard]] LogicalGraphNode readNode(uint64_t) const override { throw std::logic_error("unexpected Node read"); }
		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t) const override { throw std::logic_error("unexpected Edge read"); }
		void visitNodes(size_t, const std::function<bool(const LogicalGraphNode &)> &) const override {
			throw std::logic_error("unexpected Node traversal");
		}
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override {
			throw std::logic_error("unexpected Node page scan");
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdges(
				uint64_t, LogicalGraphReadDirection, std::optional<LogicalGraphAdjacencyContinuation>, size_t) const override {
			throw std::logic_error("unexpected adjacency scan");
		}
		void visitEdges(size_t, const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("unexpected Edge traversal");
		}
		void visitAdjacentEdges(uint64_t, LogicalGraphReadDirection, size_t,
								const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("unexpected adjacency traversal");
		}
	};

	class NoGraphBackend final : public LogicalGraphReadBackend {
	public:
		[[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
			return std::make_unique<NoGraphSnapshot>();
		}
	};

	[[nodiscard]] LogicalPlanReadRuntimeBounds bounds() {
		return {
				.maximum_node_records = 16U,
				.maximum_node_page_records = 4U,
				.maximum_metadata_pages_per_node_page = 4U,
				.maximum_edges_per_node = 16U,
				.maximum_adjacency_page_records = 4U,
				.maximum_property_entries = 16U,
				.maximum_rows = 16U,
				.maximum_distinct_rows = 16U,
		};
	}

} // namespace

TEST(LogicalPlanUnionRuntimeTest, ExecutesParserProducedUnionAndUnionAllWithCypherDistinctSemantics) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;

	QueryPlan distinctPlan = parser.parseToLogical("RETURN 1 AS value UNION RETURN 1 AS value UNION RETURN 2 AS value");
	ASSERT_NE(distinctPlan.root, nullptr);
	const QueryResult distinct = runtime.executeToQueryResult(*distinctPlan.root, bounds());
	ASSERT_EQ(distinct.rowCount(), 2U);
	EXPECT_EQ(distinct.getRows()[0].at("value").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(distinct.getRows()[1].at("value").asPrimitive(), PropertyValue(2));

	QueryPlan allPlan = parser.parseToLogical("RETURN 1 AS value UNION ALL RETURN 1 AS value UNION ALL RETURN 2 AS value");
	ASSERT_NE(allPlan.root, nullptr);
	const QueryResult all = runtime.executeToQueryResult(*allPlan.root, bounds());
	ASSERT_EQ(all.rowCount(), 3U);
	EXPECT_EQ(all.getRows()[0].at("value").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(all.getRows()[1].at("value").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(all.getRows()[2].at("value").asPrimitive(), PropertyValue(2));
}

TEST(LogicalPlanUnionRuntimeTest, RejectsMismatchedOutputColumnsBeforeExecutingEitherBranch) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("RETURN 1 AS left UNION RETURN 2 AS right");
	ASSERT_NE(plan.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds())), std::invalid_argument);
}
