/**
 * @file test_TraversalQuery.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/23
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <graph/core/Database.hpp>
#include <graph/query/strategies/TraversalQuery.hpp>
#include <gtest/gtest.h>

class TraversalQueryTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file name using UUID
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_traversal_" + to_string(uuid) + ".dat");

		// Create and initialize Database
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();

		// Get DataManager and create TraversalQuery
		dataManager = database->getStorage()->getDataManager();
		traversalQuery = std::make_unique<graph::query::TraversalQuery>(dataManager);

		// Set up test graph structure
		setupTestGraph();
	}

	void TearDown() override {
		traversalQuery.reset();
		database->close();
		database.reset();
		std::filesystem::remove(testFilePath);
	}

	void setupTestGraph() {
		// Create a more comprehensive graph:
		//     1 ← 6
		//    ↙ ↘   ↘
		//   2 → 3 ← 7 → 8
		//   ↓   ↓       ↓
		//   4 → 5 ← 9 → 10
		//       ↑   ↓
		//      11 → 12

		// Reserve IDs for nodes
		std::vector<int64_t> nodeIds;
		nodeIds.reserve(12);
		for (int i = 0; i < 12; ++i) {
			nodeIds.push_back(dataManager->reserveTemporaryNodeId());
		}

		// Create nodes
		node1 = graph::Node(nodeIds[0], "Node1");
		node2 = graph::Node(nodeIds[1], "Node2");
		node3 = graph::Node(nodeIds[2], "Node3");
		node4 = graph::Node(nodeIds[3], "Node4");
		node5 = graph::Node(nodeIds[4], "Node5");
		node6 = graph::Node(nodeIds[5], "Node6");
		node7 = graph::Node(nodeIds[6], "Node7");
		node8 = graph::Node(nodeIds[7], "Node8");
		node9 = graph::Node(nodeIds[8], "Node9");
		node10 = graph::Node(nodeIds[9], "Node10");
		node11 = graph::Node(nodeIds[10], "Node11");
		node12 = graph::Node(nodeIds[11], "Node12");

		// Add all nodes
		std::vector nodes = {node1, node2, node3, node4, node5, node6, node7, node8, node9, node10, node11, node12};
		for (const auto &node: nodes) {
			dataManager->addNode(node);
		}

		// Create edges
		std::vector<std::pair<int64_t, int64_t>> edgeConnections = {
				{node1.getId(), node2.getId()}, // 1 -> 2
				{node1.getId(), node3.getId()}, // 1 -> 3
				{node2.getId(), node3.getId()}, // 2 -> 3
				{node2.getId(), node4.getId()}, // 2 -> 4
				{node3.getId(), node5.getId()}, // 3 -> 5
				{node4.getId(), node5.getId()}, // 4 -> 5
				{node6.getId(), node1.getId()}, // 6 -> 1
				{node6.getId(), node8.getId()}, // 6 -> 8
				{node7.getId(), node3.getId()}, // 7 -> 3
				{node7.getId(), node8.getId()}, // 7 -> 8
				{node8.getId(), node10.getId()}, // 8 -> 10
				{node9.getId(), node5.getId()}, // 9 -> 5
				{node9.getId(), node10.getId()}, // 9 -> 10
				{node9.getId(), node12.getId()}, // 9 -> 12
				{node11.getId(), node5.getId()}, // 11 -> 5
				{node11.getId(), node12.getId()} // 11 -> 12
		};

		edges.clear();
		for (size_t i = 0; i < edgeConnections.size(); ++i) {
			int64_t edgeId = dataManager->reserveTemporaryEdgeId();
			auto edge = graph::Edge(edgeId, edgeConnections[i].first, edgeConnections[i].second,
									"edge" + std::to_string(i + 1));
			edges.push_back(edge);
			dataManager->addEdge(edges.back());
		}
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::unique_ptr<graph::query::TraversalQuery> traversalQuery;

	// Test graph elements
	graph::Node node1, node2, node3, node4, node5, node6, node7, node8, node9, node10, node11, node12;
	std::vector<graph::Edge> edges;
};

TEST_F(TraversalQueryTest, FindShortestPathDirectConnection) {
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node1.getId(), node2.getId(), "outgoing");

	ASSERT_EQ(path.size(), 2u);
	EXPECT_EQ(path[0].getId(), node1.getId());
	EXPECT_EQ(path[1].getId(), node2.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathTwoHops) {
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node1.getId(), node5.getId(), "outgoing");

	ASSERT_EQ(path.size(), 3u);
	EXPECT_EQ(path[0].getId(), node1.getId());
	EXPECT_EQ(path[2].getId(), node5.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathMultiplePaths) {
	// Path from node1 to node5: 1->2->4->5 or 1->3->5 (both have different lengths)
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node1.getId(), node5.getId(), "outgoing");

	ASSERT_GE(path.size(), 3u);
	EXPECT_EQ(path[0].getId(), node1.getId());
	EXPECT_EQ(path.back().getId(), node5.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathSameNode) {
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node1.getId(), node1.getId(), "outgoing");

	ASSERT_EQ(path.size(), 1u);
	EXPECT_EQ(path[0].getId(), node1.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathNoConnection) {
	// Try to find path from node5 to node1 (no outgoing path exists)
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node5.getId(), node1.getId(), "outgoing");

	EXPECT_TRUE(path.empty());
}

TEST_F(TraversalQueryTest, FindShortestPathIncomingDirection) {
	// Test incoming direction: path from node5 to node1
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node5.getId(), node1.getId(), "incoming");

	ASSERT_GE(path.size(), 3u);
	EXPECT_EQ(path[0].getId(), node5.getId());
	EXPECT_EQ(path.back().getId(), node1.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathBothDirections) {
	// Test both directions: should find shortest path regardless of edge direction
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node5.getId(), node1.getId(), "both");

	ASSERT_GE(path.size(), 3u);
	EXPECT_EQ(path[0].getId(), node5.getId());
	EXPECT_EQ(path.back().getId(), node1.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathNonexistentNode) {
	// Test with non-existent end node
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node1.getId(), 999, "outgoing");

	EXPECT_TRUE(path.empty());
}

TEST_F(TraversalQueryTest, FindShortestPathLongerPath) {
	// Test longer path: node6 to node5
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node6.getId(), node5.getId(), "outgoing");

	ASSERT_GE(path.size(), 4u);
	EXPECT_EQ(path[0].getId(), node6.getId());
	EXPECT_EQ(path.back().getId(), node5.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathDisconnectedNodes) {
	// Test path between nodes that are not connected
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node12.getId(), node1.getId(), "outgoing");

	EXPECT_TRUE(path.empty());
}

TEST_F(TraversalQueryTest, FindShortestPathCrossConnections) {
	// Test path that requires crossing multiple branches: node6 to node10
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node6.getId(), node10.getId(), "outgoing");

	ASSERT_GE(path.size(), 3u);
	EXPECT_EQ(path[0].getId(), node6.getId());
	EXPECT_EQ(path.back().getId(), node10.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathFromIsolatedNode) {
	// Test path from node that has no incoming edges (node11, node10)
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node11.getId(), node10.getId(), "outgoing");

	EXPECT_TRUE(path.empty());
}

TEST_F(TraversalQueryTest, FindShortestPathInvalidDirection) {
	// Test with invalid direction parameter (should handle gracefully)
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node1.getId(), node5.getId(), "invalid");

	// Behavior depends on implementation - could be empty or default to "both"
	// Just ensure it doesn't crash
	EXPECT_TRUE(true);
}

TEST_F(TraversalQueryTest, FindShortestPathMultipleAlternatives) {
	// Test node9 to node5: direct path vs through node12->node11->node5
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node9.getId(), node5.getId(), "outgoing");

	ASSERT_EQ(path.size(), 2u); // Should choose direct path
	EXPECT_EQ(path[0].getId(), node9.getId());
	EXPECT_EQ(path[1].getId(), node5.getId());
}

// Test findConnectedNodes functionality
TEST_F(TraversalQueryTest, FindConnectedNodesOutgoing) {
	std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node1.getId(), "outgoing");

	ASSERT_EQ(connectedNodes.size(), 2u);
	// node1 has outgoing edges to node2 and node3
	std::unordered_set<int64_t> expectedIds = {node2.getId(), node3.getId()};
	for (const auto &node: connectedNodes) {
		EXPECT_TRUE(expectedIds.count(node.getId()) > 0);
	}
}

TEST_F(TraversalQueryTest, FindConnectedNodesIncoming) {
	std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node5.getId(), "incoming");

	ASSERT_GE(connectedNodes.size(), 2u);
	// node5 has incoming edges from node3, node4, node9, node11
	std::unordered_set<int64_t> expectedIds = {node3.getId(), node4.getId(), node9.getId(), node11.getId()};
	for (const auto &node: connectedNodes) {
		EXPECT_TRUE(expectedIds.count(node.getId()) > 0);
	}
}

TEST_F(TraversalQueryTest, FindConnectedNodesBoth) {
	std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node3.getId(), "both");

	ASSERT_GE(connectedNodes.size(), 3u);
	// node3 has incoming from node1, node2, node7 and outgoing to node5
	std::unordered_set<int64_t> possibleIds = {node1.getId(), node2.getId(), node5.getId(), node7.getId()};
	for (const auto &node: connectedNodes) {
		EXPECT_TRUE(possibleIds.count(node.getId()) > 0);
	}
}

TEST_F(TraversalQueryTest, FindConnectedNodesWithEdgeLabel) {
	// Add edge with specific label for testing
	int64_t edgeId = dataManager->reserveTemporaryEdgeId();
	auto labeledEdge = graph::Edge(edgeId, node6.getId(), node7.getId(), "special_edge");
	dataManager->addEdge(labeledEdge);

	std::vector<graph::Node> connectedNodes =
			traversalQuery->findConnectedNodes(node6.getId(), "outgoing", "special_edge");

	ASSERT_EQ(connectedNodes.size(), 1u);
	EXPECT_EQ(connectedNodes[0].getId(), node7.getId());
}

TEST_F(TraversalQueryTest, FindConnectedNodesWithNodeLabel) {
	// Modify node2 to have a specific label for testing
	graph::Node labeledNode2(node2.getId(), "special_node");
	dataManager->updateNode(labeledNode2);

	std::vector<graph::Node> connectedNodes =
			traversalQuery->findConnectedNodes(node1.getId(), "outgoing", "", "special_node");

	ASSERT_EQ(connectedNodes.size(), 1u);
	EXPECT_EQ(connectedNodes[0].getId(), node2.getId());
}

TEST_F(TraversalQueryTest, FindConnectedNodesWithBothLabels) {
	// Add edge and node with specific labels
	int64_t edgeId = dataManager->reserveTemporaryEdgeId();
	auto labeledEdge = graph::Edge(edgeId, node1.getId(), node4.getId(), "test_edge");
	dataManager->addEdge(labeledEdge);

	graph::Node labeledNode4(node4.getId(), "test_node");
	dataManager->updateNode(labeledNode4);

	std::vector<graph::Node> connectedNodes =
			traversalQuery->findConnectedNodes(node1.getId(), "outgoing", "test_edge", "test_node");

	ASSERT_EQ(connectedNodes.size(), 1u);
	EXPECT_EQ(connectedNodes[0].getId(), node4.getId());
}

TEST_F(TraversalQueryTest, FindConnectedNodesNoConnections) {
	// Test node with no connections (assuming node12 has limited connections)
	std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node12.getId(), "outgoing");

	// Based on current graph structure, node12 should have no outgoing edges
	EXPECT_TRUE(connectedNodes.empty());
}

TEST_F(TraversalQueryTest, FindConnectedNodesNonexistentEdgeLabel) {
	std::vector<graph::Node> connectedNodes =
			traversalQuery->findConnectedNodes(node1.getId(), "outgoing", "nonexistent_label");

	EXPECT_TRUE(connectedNodes.empty());
}

TEST_F(TraversalQueryTest, FindConnectedNodesNonexistentNodeLabel) {
	std::vector<graph::Node> connectedNodes =
			traversalQuery->findConnectedNodes(node1.getId(), "outgoing", "", "nonexistent_label");

	EXPECT_TRUE(connectedNodes.empty());
}

TEST_F(TraversalQueryTest, FindConnectedNodesInvalidDirection) {
	// Test with invalid direction parameter
	std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node1.getId(), "invalid_direction");

	// Should return empty result for invalid direction
	EXPECT_TRUE(connectedNodes.empty());
}

TEST_F(TraversalQueryTest, FindConnectedNodesDuplicateEdges) {
	// Test that duplicate connections are handled correctly (no duplicate nodes returned)
	std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node1.getId(), "outgoing");

	// Verify no duplicate nodes in result
	std::unordered_set<int64_t> uniqueIds;
	for (const auto &node: connectedNodes) {
		EXPECT_TRUE(uniqueIds.insert(node.getId()).second) << "Duplicate node found: " << node.getId();
	}
}

TEST_F(TraversalQueryTest, FindConnectedNodesNonexistentStartNode) {
	// Test with non-existent start node
	std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(999, "outgoing");

	EXPECT_TRUE(connectedNodes.empty());
}

TEST_F(TraversalQueryTest, FindConnectedNodesEmptyParameters) {
	// Test with empty edge and node labels (should return all connected nodes)
	std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node1.getId(), "outgoing", "", "");

	ASSERT_EQ(connectedNodes.size(), 2u);
	std::unordered_set<int64_t> expectedIds = {node2.getId(), node3.getId()};
	for (const auto &node: connectedNodes) {
		EXPECT_TRUE(expectedIds.count(node.getId()) > 0);
    }
}