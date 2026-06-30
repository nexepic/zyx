/**
 * @file test_EntityObserverColumnarMaterialization.cpp
 * @brief Tests default columnar observer materialization for node and edge batches.
 */

#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "graph/storage/indexes/IEntityObserver.hpp"

namespace {

class CapturingEntityObserver final : public graph::IEntityObserver {
public:
	void onNodesAdded(const std::vector<graph::Node> &nodes) override { observedNodes = nodes; }
	void onEdgesAdded(const std::vector<graph::Edge> &edges) override { observedEdges = edges; }

	std::vector<graph::Node> observedNodes;
	std::vector<graph::Edge> observedEdges;
};

graph::Node makeNode(int64_t id) {
	graph::Node node;
	node.setId(id);
	return node;
}

graph::Edge makeEdge(int64_t id) {
	graph::Edge edge;
	edge.setId(id);
	return edge;
}

} // namespace

TEST(EntityObserverColumnarMaterializationTest, NodeColumnsPopulateRowsAndSkipShortColumnValues) {
	CapturingEntityObserver observer;
	std::vector<graph::Node> nodes{makeNode(1), makeNode(2)};
	const std::vector<graph::storage::BulkPropertyColumn> columns{
			{.key = "name", .values = {graph::PropertyValue("Ada"), graph::PropertyValue("Grace")}},
			{.key = "rank", .values = {graph::PropertyValue(int64_t{1})}}};

	observer.onNodesAddedColumnar(nodes, columns);

	ASSERT_EQ(observer.observedNodes.size(), 2UL);
	EXPECT_EQ(observer.observedNodes[0].getProperty("name"), graph::PropertyValue("Ada"));
	EXPECT_EQ(observer.observedNodes[0].getProperty("rank"), graph::PropertyValue(int64_t{1}));
	EXPECT_EQ(observer.observedNodes[1].getProperty("name"), graph::PropertyValue("Grace"));
	EXPECT_FALSE(observer.observedNodes[1].hasProperty("rank"));
}

TEST(EntityObserverColumnarMaterializationTest, EmptyNodeInputAndEmptyColumnsForwardOriginalRows) {
	CapturingEntityObserver observer;

	observer.onNodesAddedColumnar({}, {{.key = "ignored", .values = {graph::PropertyValue("x")}}});
	EXPECT_TRUE(observer.observedNodes.empty());

	std::vector<graph::Node> nodes{makeNode(7)};
	observer.onNodesAddedColumnar(nodes, {});
	ASSERT_EQ(observer.observedNodes.size(), 1UL);
	EXPECT_EQ(observer.observedNodes.front().getId(), 7);
	EXPECT_TRUE(observer.observedNodes.front().getProperties().empty());
}

TEST(EntityObserverColumnarMaterializationTest, EdgeColumnsUseSameMaterializationContract) {
	CapturingEntityObserver observer;
	std::vector<graph::Edge> edges{makeEdge(11), makeEdge(12)};
	const std::vector<graph::storage::BulkPropertyColumn> columns{
			{.key = "weight", .values = {graph::PropertyValue(3.5), graph::PropertyValue(7.0)}},
			{.key = "tag", .values = {graph::PropertyValue("hot")}}};

	observer.onEdgesAddedColumnar(edges, columns);

	ASSERT_EQ(observer.observedEdges.size(), 2UL);
	EXPECT_EQ(observer.observedEdges[0].getProperty("weight"), graph::PropertyValue(3.5));
	EXPECT_EQ(observer.observedEdges[0].getProperty("tag"), graph::PropertyValue("hot"));
	EXPECT_EQ(observer.observedEdges[1].getProperty("weight"), graph::PropertyValue(7.0));
	EXPECT_FALSE(observer.observedEdges[1].hasProperty("tag"));
}

TEST(EntityObserverColumnarMaterializationTest, EmptyEdgeInputAndEmptyColumnsForwardOriginalRows) {
	CapturingEntityObserver observer;

	observer.onEdgesAddedColumnar({}, {{.key = "ignored", .values = {graph::PropertyValue("x")}}});
	EXPECT_TRUE(observer.observedEdges.empty());

	std::vector<graph::Edge> edges{makeEdge(17)};
	observer.onEdgesAddedColumnar(edges, {});
	ASSERT_EQ(observer.observedEdges.size(), 1UL);
	EXPECT_EQ(observer.observedEdges.front().getId(), 17);
	EXPECT_TRUE(observer.observedEdges.front().getProperties().empty());
}
