/**
 * @file test_LogicalPlanVarLengthRuntime.cpp
 * @brief Bounded logical variable-length traversal regression tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/logical/operators/LogicalNamedPath.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalVarLengthTraversal.hpp"

using namespace graph;
using namespace graph::query::execution;
using namespace graph::storage;
namespace logical = graph::query::logical;

namespace {

	class PathSnapshot final : public LogicalGraphReadSnapshot {
	public:
		PathSnapshot() {
			nodes_.emplace(1U, LogicalGraphNode{1U, {"Source"}});
			nodes_.emplace(2U, LogicalGraphNode{2U, {"Middle"}});
			nodes_.emplace(3U, LogicalGraphNode{3U, {"Target"}});
			edges_.emplace(10U, LogicalGraphEdge{10U, 1U, 2U, "KNOWS"});
			edges_.emplace(11U, LogicalGraphEdge{11U, 2U, 3U, "KNOWS"});
		}

		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override { return nodes_.at(nodeId); }
		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override { return edges_.at(edgeId); }
		void visitNodes(size_t, const std::function<bool(const LogicalGraphNode &)> &) const override {
			throw std::logic_error("unexpected legacy Node traversal");
		}
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords, size_t) const override {
			LogicalGraphNodePage page;
			page.metadata_pages_visited = 1U;
			for (const auto &[id, node]: nodes_) {
				if (id <= afterNodeId) {
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
				std::optional<LogicalGraphAdjacencyContinuation>, size_t maximumRecords) const override {
			LogicalGraphAdjacencyPage page;
			for (const auto &[_, edge]: edges_) {
				const bool matches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
									 ? edge.source_node_id == nodeId
									 : edge.target_node_id == nodeId;
				if (!matches) {
					continue;
				}
				if (page.edges.size() == maximumRecords) {
					throw std::logic_error("test graph unexpectedly needs an adjacency continuation");
				}
				page.edges.push_back(edge);
			}
			return page;
		}
		void visitEdges(size_t, const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("unexpected legacy Edge traversal");
		}
		void visitAdjacentEdges(uint64_t, LogicalGraphReadDirection, size_t,
								const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("unexpected legacy adjacency traversal");
		}

	private:
		std::map<uint64_t, LogicalGraphNode> nodes_;
		std::map<uint64_t, LogicalGraphEdge> edges_;
	};

	class PathBackend final : public LogicalGraphReadBackend {
	public:
		[[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
			return std::make_unique<PathSnapshot>();
		}
	};

	[[nodiscard]] LogicalPlanReadRuntimeBounds bounds() {
		return {
				.maximum_node_records = 8U,
				.maximum_node_page_records = 2U,
				.maximum_metadata_pages_per_node_page = 2U,
				.maximum_edges_per_node = 4U,
				.maximum_adjacency_page_records = 2U,
				.maximum_property_entries = 4U,
				.maximum_rows = 8U,
				.maximum_distinct_rows = 8U,
		};
	}

} // namespace

TEST(LogicalPlanVarLengthRuntimeTest, ExecutesBoundedTargetTraversalAndRejectsUnrepresentableRelationshipLists) {
	PathBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalVarLengthTraversal targetOnly(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 1, 2, std::vector<std::string>{"Target"});
	const graph::query::QueryResult result = runtime.executeToQueryResult(targetOnly, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows()[0].at("source").asLogicalNode().node_id, 1U);
	EXPECT_EQ(result.getRows()[0].at("target").asLogicalNode().node_id, 3U);

	logical::LogicalVarLengthTraversal namedRelationships(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "edges",
			"target", "KNOWS", "out", 1, 2);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(namedRelationships, bounds())), std::invalid_argument);
}

TEST(LogicalPlanVarLengthRuntimeTest, BuildsAValidatedNamedPathUsingTheExistingPathListContract) {
	PathBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source",
			"relationship", "target", "KNOWS", "out");
	logical::LogicalNamedPath path(std::move(traversal), "p", std::vector<std::string>{"source", "target"},
								  std::vector<std::string>{"relationship"});

	const graph::query::QueryResult result = runtime.executeToQueryResult(path, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const std::vector<PropertyValue> &values = result.getRows()[0].at("p").asPrimitive().getList();
	ASSERT_EQ(values.size(), 3U);
	EXPECT_EQ(values[0].getMap().at("_type"), PropertyValue("node"));
	EXPECT_EQ(values[0].getMap().at("_id"), PropertyValue(1));
	EXPECT_EQ(values[1].getMap().at("_type"), PropertyValue("relationship"));
	EXPECT_EQ(values[1].getMap().at("_id"), PropertyValue(10));
	EXPECT_EQ(values[2].getMap().at("_type"), PropertyValue("node"));
	EXPECT_EQ(values[2].getMap().at("_id"), PropertyValue(2));
}
