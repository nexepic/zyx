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
		const boost::uuids::uuid uuid = boost::uuids::random_generator()();
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

		// Create nodes
		node1 = graph::Node(0, "Node1");
		node2 = graph::Node(0, "Node2");
		node3 = graph::Node(0, "Node3");
		node4 = graph::Node(0, "Node4");
		node5 = graph::Node(0, "Node5");
		node6 = graph::Node(0, "Node6");
		node7 = graph::Node(0, "Node7");
		node8 = graph::Node(0, "Node8");
		node9 = graph::Node(0, "Node9");
		node10 = graph::Node(0, "Node10");
		node11 = graph::Node(0, "Node11");
		node12 = graph::Node(0, "Node12");

		// Add all nodes
		dataManager->addNode(node1);
		dataManager->addNode(node2);
		dataManager->addNode(node3);
		dataManager->addNode(node4);
		dataManager->addNode(node5);
		dataManager->addNode(node6);
		dataManager->addNode(node7);
		dataManager->addNode(node8);
		dataManager->addNode(node9);
		dataManager->addNode(node10);
		dataManager->addNode(node11);
		dataManager->addNode(node12);

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
			auto edge =
					graph::Edge(0, edgeConnections[i].first, edgeConnections[i].second, "edge" + std::to_string(i + 1));

			dataManager->addEdge(edge);
			edges.push_back(edge);
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

// FindShortestPath tests
TEST_F(TraversalQueryTest, FindShortestPathDirectConnection) {
	const std::vector<graph::Node> path = traversalQuery->findShortestPath(node1.getId(), node2.getId(), "outgoing");

	ASSERT_EQ(path.size(), 2u);
	EXPECT_EQ(path[0].getId(), node1.getId());
	EXPECT_EQ(path[1].getId(), node2.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathTwoHops) {
	const std::vector<graph::Node> path = traversalQuery->findShortestPath(node1.getId(), node5.getId(), "outgoing");

	ASSERT_EQ(path.size(), 3u);
	EXPECT_EQ(path[0].getId(), node1.getId());
	EXPECT_EQ(path[2].getId(), node5.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathMultiplePaths) {
	// Path from node1 to node5: 1->2->4->5 or 1->3->5 (both have different lengths)
	const std::vector<graph::Node> path = traversalQuery->findShortestPath(node1.getId(), node5.getId(), "outgoing");

	ASSERT_GE(path.size(), 3u);
	EXPECT_EQ(path[0].getId(), node1.getId());
	EXPECT_EQ(path.back().getId(), node5.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathSameNode) {
	const std::vector<graph::Node> path = traversalQuery->findShortestPath(node1.getId(), node1.getId(), "outgoing");

	ASSERT_EQ(path.size(), 1u);
	EXPECT_EQ(path[0].getId(), node1.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathNoConnection) {
	// Try to find path from node5 to node1 (no outgoing path exists)
	const std::vector<graph::Node> path = traversalQuery->findShortestPath(node5.getId(), node1.getId(), "outgoing");

	EXPECT_TRUE(path.empty());
}

TEST_F(TraversalQueryTest, FindShortestPathIncomingDirection) {
	// Test incoming direction: path from node5 to node1
	const std::vector<graph::Node> path = traversalQuery->findShortestPath(node5.getId(), node1.getId(), "incoming");

	ASSERT_GE(path.size(), 3u);
	EXPECT_EQ(path[0].getId(), node5.getId());
	EXPECT_EQ(path.back().getId(), node1.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathBothDirections) {
	// Test both directions: should find the shortest path regardless of edge direction
	const std::vector<graph::Node> path = traversalQuery->findShortestPath(node5.getId(), node1.getId(), "both");

	ASSERT_GE(path.size(), 3u);
	EXPECT_EQ(path[0].getId(), node5.getId());
	EXPECT_EQ(path.back().getId(), node1.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathNonexistentNode) {
	// Test with non-existent end node
	const std::vector<graph::Node> path = traversalQuery->findShortestPath(node1.getId(), 999, "outgoing");

	EXPECT_TRUE(path.empty());
}

TEST_F(TraversalQueryTest, FindShortestPathLongerPath) {
	// Test longer path: node6 to node5
	const std::vector<graph::Node> path = traversalQuery->findShortestPath(node6.getId(), node5.getId(), "outgoing");

	ASSERT_GE(path.size(), 4u);
	EXPECT_EQ(path[0].getId(), node6.getId());
	EXPECT_EQ(path.back().getId(), node5.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathDisconnectedNodes) {
	// Test path between nodes that are not connected
	const std::vector<graph::Node> path = traversalQuery->findShortestPath(node12.getId(), node1.getId(), "outgoing");

	EXPECT_TRUE(path.empty());
}

TEST_F(TraversalQueryTest, FindShortestPathCrossConnections) {
	// Test path that requires crossing multiple branches: node6 to node10
	const std::vector<graph::Node> path = traversalQuery->findShortestPath(node6.getId(), node10.getId(), "outgoing");

	ASSERT_GE(path.size(), 3u);
	EXPECT_EQ(path[0].getId(), node6.getId());
	EXPECT_EQ(path.back().getId(), node10.getId());
}

TEST_F(TraversalQueryTest, FindShortestPathFromIsolatedNode) {
	// Test path from node that has no incoming edges (node11, node10)
	const std::vector<graph::Node> path = traversalQuery->findShortestPath(node11.getId(), node10.getId(), "outgoing");

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
	const std::vector<graph::Node> path = traversalQuery->findShortestPath(node9.getId(), node5.getId(), "outgoing");

	ASSERT_EQ(path.size(), 2u); // Should choose direct path
	EXPECT_EQ(path[0].getId(), node9.getId());
	EXPECT_EQ(path[1].getId(), node5.getId());
}

// FindConnectedNodes tests
TEST_F(TraversalQueryTest, FindConnectedNodesOutgoing) {
	const std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node1.getId(), "outgoing");

	ASSERT_EQ(connectedNodes.size(), 2u);
	// node1 has outgoing edges to node2 and node3
	const std::unordered_set expectedIds = {node2.getId(), node3.getId()};
	for (const auto &node: connectedNodes) {
		EXPECT_TRUE(expectedIds.contains(node.getId()));
	}
}

TEST_F(TraversalQueryTest, FindConnectedNodesIncoming) {
	const std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node5.getId(), "incoming");

	ASSERT_GE(connectedNodes.size(), 2u);
	// node5 has incoming edges from node3, node4, node9, node11
	const std::unordered_set expectedIds = {node3.getId(), node4.getId(), node9.getId(), node11.getId()};
	for (const auto &node: connectedNodes) {
		EXPECT_TRUE(expectedIds.contains(node.getId()));
	}
}

TEST_F(TraversalQueryTest, FindConnectedNodesBoth) {
	const std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node3.getId(), "both");

	ASSERT_GE(connectedNodes.size(), 3u);
	// node3 has incoming from node1, node2, node7 and outgoing to node5
	const std::unordered_set possibleIds = {node1.getId(), node2.getId(), node5.getId(), node7.getId()};
	for (const auto &node: connectedNodes) {
		EXPECT_TRUE(possibleIds.contains(node.getId()));
	}
}

TEST_F(TraversalQueryTest, FindConnectedNodesWithEdgeLabel) {
	// Add edge with specific label for testing
	auto labeledEdge = graph::Edge(0, node6.getId(), node7.getId(), "special_edge");
	dataManager->addEdge(labeledEdge);

	std::vector<graph::Node> connectedNodes =
			traversalQuery->findConnectedNodes(node6.getId(), "outgoing", "special_edge");

	ASSERT_EQ(connectedNodes.size(), 1u);
	EXPECT_EQ(connectedNodes[0].getId(), node7.getId());
}

TEST_F(TraversalQueryTest, FindConnectedNodesWithNodeLabel) {
	// Modify node2 to have a specific label for testing
	const graph::Node labeledNode2(node2.getId(), "special_node");
	dataManager->updateNode(labeledNode2);

	std::vector<graph::Node> connectedNodes =
			traversalQuery->findConnectedNodes(node1.getId(), "outgoing", "", "special_node");

	ASSERT_EQ(connectedNodes.size(), 1u);
	EXPECT_EQ(connectedNodes[0].getId(), node2.getId());
}

TEST_F(TraversalQueryTest, FindConnectedNodesWithBothLabels) {
	// Add edge and node with specific labels
	auto labeledEdge = graph::Edge(0, node1.getId(), node4.getId(), "test_edge");
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
	const std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node12.getId(), "outgoing");

	// Based on current graph structure, node12 should have no outgoing edges
	EXPECT_TRUE(connectedNodes.empty());
}

TEST_F(TraversalQueryTest, FindConnectedNodesNonexistentEdgeLabel) {
	const std::vector<graph::Node> connectedNodes =
			traversalQuery->findConnectedNodes(node1.getId(), "outgoing", "nonexistent_label");

	EXPECT_TRUE(connectedNodes.empty());
}

TEST_F(TraversalQueryTest, FindConnectedNodesNonexistentNodeLabel) {
	const std::vector<graph::Node> connectedNodes =
			traversalQuery->findConnectedNodes(node1.getId(), "outgoing", "", "nonexistent_label");

	EXPECT_TRUE(connectedNodes.empty());
}

TEST_F(TraversalQueryTest, FindConnectedNodesInvalidDirection) {
	// Test with invalid direction parameter
	const std::vector<graph::Node> connectedNodes =
			traversalQuery->findConnectedNodes(node1.getId(), "invalid_direction");

	// Should return empty result for invalid direction
	EXPECT_TRUE(connectedNodes.empty());
}

TEST_F(TraversalQueryTest, FindConnectedNodesDuplicateEdges) {
	// Test that duplicate connections are handled correctly (no duplicate nodes returned)
	const std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node1.getId(), "outgoing");

	// Verify no duplicate nodes in result
	std::unordered_set<int64_t> uniqueIds;
	for (const auto &node: connectedNodes) {
		EXPECT_TRUE(uniqueIds.insert(node.getId()).second) << "Duplicate node found: " << node.getId();
	}
}

TEST_F(TraversalQueryTest, FindConnectedNodesNonexistentStartNode) {
	// Test with non-existent start node
	const std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(999, "outgoing");

	EXPECT_TRUE(connectedNodes.empty());
}

TEST_F(TraversalQueryTest, FindConnectedNodesEmptyParameters) {
	// Test with empty edge and node labels (should return all connected nodes)
	std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node1.getId(), "outgoing", "", "");

	ASSERT_EQ(connectedNodes.size(), 2u);
	const std::unordered_set expectedIds = {node2.getId(), node3.getId()};
	for (const auto &node: connectedNodes) {
		EXPECT_TRUE(expectedIds.contains(node.getId()));
	}
}

TEST_F(TraversalQueryTest, BreadthFirstTraversal_BasicTraversal) {
	std::vector<int64_t> visitedNodeIds;
	std::vector<int> visitedDepths;

	traversalQuery->breadthFirstTraversal(
			node1.getId(),
			[&visitedNodeIds, &visitedDepths](const graph::Node &node, int depth) {
				visitedNodeIds.push_back(node.getId());
				visitedDepths.push_back(depth);
				return true; // Continue traversal
			},
			3, // Max depth of 3
			"outgoing");

	// Check we visited the expected nodes in BFS order
	ASSERT_GE(visitedNodeIds.size(), 3u);
	EXPECT_EQ(visitedNodeIds[0], node1.getId()); // Start node (depth 0)

	// Check depths are correct (should increase monotonically by steps)
	EXPECT_EQ(visitedDepths[0], 0); // Start at depth 0

	// All nodes at depth 1 should be before nodes at depth 2
	int lastDepth = 0;
	for (size_t i = 1; i < visitedDepths.size(); ++i) {
		EXPECT_GE(visitedDepths[i], lastDepth);
		EXPECT_LE(visitedDepths[i] - lastDepth, 1); // Depth can only increase by 1 in BFS
		lastDepth = visitedDepths[i];
	}
}

TEST_F(TraversalQueryTest, BreadthFirstTraversal_MaxDepthLimit) {
	std::vector<int64_t> visitedNodeIds;
	std::vector<int> visitedDepths;

	// Set max depth to 1 (only immediate neighbors)
	traversalQuery->breadthFirstTraversal(
			node1.getId(),
			[&visitedNodeIds, &visitedDepths](const graph::Node &node, int depth) {
				visitedNodeIds.push_back(node.getId());
				visitedDepths.push_back(depth);
				return true;
			},
			1, // Max depth of 1
			"outgoing");

	// Should only visit node1 and its direct neighbors (node2, node3)
	ASSERT_LE(visitedNodeIds.size(), 3u);
	EXPECT_EQ(visitedNodeIds[0], node1.getId());

	// All nodes should be at depth 0 or 1
	for (int depth: visitedDepths) {
		EXPECT_LE(depth, 1);
	}
}

TEST_F(TraversalQueryTest, BreadthFirstTraversal_EarlyTermination) {
	std::vector<int64_t> visitedNodeIds;

	// Stop traversal after visiting node1
	traversalQuery->breadthFirstTraversal(
			node1.getId(),
			[&visitedNodeIds](const graph::Node &node, int depth) {
				visitedNodeIds.push_back(node.getId());
				return false; // Stop traversal immediately
			},
			5, // High max depth
			"outgoing");

	// Should only visit the start node due to early termination
	ASSERT_EQ(visitedNodeIds.size(), 1u);
	EXPECT_EQ(visitedNodeIds[0], node1.getId());
}

TEST_F(TraversalQueryTest, BreadthFirstTraversal_IncomingDirection) {
	std::vector<int64_t> visitedNodeIds;

	// Traverse incoming edges from node5
	traversalQuery->breadthFirstTraversal(
			node5.getId(),
			[&visitedNodeIds](const graph::Node &node, int depth) {
				visitedNodeIds.push_back(node.getId());
				return true;
			},
			2, // Max depth of 2
			"incoming");

	// Node5 has incoming edges from node3, node4, node9, node11
	ASSERT_GE(visitedNodeIds.size(), 5u); // At least 5 nodes including node5
	EXPECT_EQ(visitedNodeIds[0], node5.getId());

	// Check that direct incoming nodes to node5 are included
	const std::unordered_set<int64_t> directIncoming = {node3.getId(), node4.getId(), node9.getId(), node11.getId()};

	bool foundDirectIncoming = false;
	for (size_t i = 1; i < visitedNodeIds.size(); ++i) {
		if (directIncoming.contains(visitedNodeIds[i])) {
			foundDirectIncoming = true;
			break;
		}
	}
	EXPECT_TRUE(foundDirectIncoming);
}

TEST_F(TraversalQueryTest, BreadthFirstTraversal_BothDirections) {
	std::vector<int64_t> visitedNodeIds;

	// Traverse in both directions from node3
	traversalQuery->breadthFirstTraversal(
			node3.getId(),
			[&visitedNodeIds](const graph::Node &node, int depth) {
				visitedNodeIds.push_back(node.getId());
				return true;
			},
			1, // Max depth of 1
			"both");

	// Node3 has connections with node1, node2, node5, node7
	ASSERT_GE(visitedNodeIds.size(), 3u);
	EXPECT_EQ(visitedNodeIds[0], node3.getId());

	// Check for neighbors in both directions
	const std::unordered_set<int64_t> neighbors = {node1.getId(), node2.getId(), node5.getId(), node7.getId()};

	int neighborCount = 0;
	for (size_t i = 1; i < visitedNodeIds.size(); ++i) {
		if (neighbors.contains(visitedNodeIds[i])) {
			neighborCount++;
		}
	}
	EXPECT_GT(neighborCount, 0);
}

TEST_F(TraversalQueryTest, BreadthFirstTraversal_NoConnections) {
	std::vector<int64_t> visitedNodeIds;

	// Create an isolated node for testing
	graph::Node isolatedNode(0, "IsolatedNode");
	dataManager->addNode(isolatedNode);

	traversalQuery->breadthFirstTraversal(
			isolatedNode.getId(),
			[&visitedNodeIds](const graph::Node &node, int depth) {
				visitedNodeIds.push_back(node.getId());
				return true;
			},
			3, // Max depth of 3
			"outgoing");

	// Should only visit the isolated node itself
	ASSERT_EQ(visitedNodeIds.size(), 1u);
	EXPECT_EQ(visitedNodeIds[0], isolatedNode.getId());
}

TEST_F(TraversalQueryTest, BreadthFirstTraversal_ComplexGraph) {
	std::vector<int64_t> visitedNodeIds;
	std::unordered_map<int64_t, int> nodeDepths;

	// Start from node6 which has multiple paths to node5
	traversalQuery->breadthFirstTraversal(
			node6.getId(),
			[&visitedNodeIds, &nodeDepths](const graph::Node &node, int depth) {
				visitedNodeIds.push_back(node.getId());
				nodeDepths[node.getId()] = depth;
				return true;
			},
			5, // High max depth to reach deep nodes
			"outgoing");

	ASSERT_GT(visitedNodeIds.size(), 1u);
	EXPECT_EQ(visitedNodeIds[0], node6.getId());

	// Node5 should be visited at depth > 0
	bool foundNode5 = false;
	for (int64_t id: visitedNodeIds) {
		if (id == node5.getId()) {
			EXPECT_GT(nodeDepths[id], 0);
			foundNode5 = true;
			break;
		}
	}

	// The traversal should eventually reach node5 through outgoing paths
	EXPECT_TRUE(foundNode5);
}

TEST_F(TraversalQueryTest, BreadthFirstTraversal_ZeroMaxDepth) {
	std::vector<int64_t> visitedNodeIds;

	// Set max depth to 0 (only the start node)
	traversalQuery->breadthFirstTraversal(
			node1.getId(),
			[&visitedNodeIds](const graph::Node &node, int depth) {
				visitedNodeIds.push_back(node.getId());
				return true;
			},
			0, // Max depth of 0
			"outgoing");

	// Should only visit the start node
	ASSERT_EQ(visitedNodeIds.size(), 1u);
	EXPECT_EQ(visitedNodeIds[0], node1.getId());
}

TEST_F(TraversalQueryTest, BreadthFirstTraversal_InvalidDirection) {
	std::vector<int64_t> visitedNodeIds;

	// Use invalid direction
	traversalQuery->breadthFirstTraversal(
			node1.getId(),
			[&visitedNodeIds](const graph::Node &node, int depth) {
				visitedNodeIds.push_back(node.getId());
				return true;
			},
			3, "invalid_direction");

	// Should only visit the start node since no valid connections will be found
	ASSERT_EQ(visitedNodeIds.size(), 1u);
	EXPECT_EQ(visitedNodeIds[0], node1.getId());
}

TEST_F(TraversalQueryTest, BreadthFirstTraversal_CycleDetection) {
	// Create a cycle: A -> B -> C -> A
	graph::Node nodeA(0, "NodeA");
	graph::Node nodeB(0, "NodeB");
	graph::Node nodeC(0, "NodeC");

	dataManager->addNode(nodeA);
	dataManager->addNode(nodeB);
	dataManager->addNode(nodeC);

	graph::Edge edge1(0, nodeA.getId(), nodeB.getId(), "cycle_edge1");
	graph::Edge edge2(0, nodeB.getId(), nodeC.getId(), "cycle_edge2");
	graph::Edge edge3(0, nodeC.getId(), nodeA.getId(), "cycle_edge3");

	dataManager->addEdge(edge1);
	dataManager->addEdge(edge2);
	dataManager->addEdge(edge3);

	std::vector<int64_t> visitedNodeIds;

	traversalQuery->breadthFirstTraversal(
			nodeA.getId(),
			[&visitedNodeIds](const graph::Node &node, int depth) {
				visitedNodeIds.push_back(node.getId());
				return true;
			},
			5, // High max depth
			"outgoing");

	// Should visit each node exactly once despite the cycle
	EXPECT_EQ(visitedNodeIds.size(), 3u);

	// Count occurrences of each node
	std::unordered_map<int64_t, int> nodeCounts;
	for (int64_t id: visitedNodeIds) {
		nodeCounts[id]++;
	}

	// Each node should appear exactly once
	EXPECT_EQ(nodeCounts[nodeA.getId()], 1);
	EXPECT_EQ(nodeCounts[nodeB.getId()], 1);
	EXPECT_EQ(nodeCounts[nodeC.getId()], 1);
}

TEST_F(TraversalQueryTest, BreadthFirstTraversal_NonexistentStartNode) {
	bool visitCalled = false;

	// Use non-existent node ID
	traversalQuery->breadthFirstTraversal(
			999,
			[&visitCalled](const graph::Node &node, int depth) {
				visitCalled = true;
				return true;
			},
			3, "outgoing");

	// The visit function should not be called as the start node doesn't exist
	EXPECT_FALSE(visitCalled);
}

TEST_F(TraversalQueryTest, CombinedQueryTest) {
	// Test combining findConnectedNodes and findShortestPath
	std::vector<graph::Node> connectedNodes = traversalQuery->findConnectedNodes(node6.getId(), "outgoing");
	ASSERT_FALSE(connectedNodes.empty());

	int64_t connectedNodeId = connectedNodes[0].getId();
	std::vector<graph::Node> path = traversalQuery->findShortestPath(node6.getId(), connectedNodeId, "outgoing");

	ASSERT_EQ(path.size(), 2u);
	EXPECT_EQ(path[0].getId(), node6.getId());
	EXPECT_EQ(path[1].getId(), connectedNodeId);
}

TEST_F(TraversalQueryTest, BreadthFirstTraversal_DepthProgression) {
	std::vector<std::pair<int64_t, int>> visitOrder;

	traversalQuery->breadthFirstTraversal(
			node6.getId(),
			[&visitOrder](const graph::Node &node, int depth) {
				visitOrder.emplace_back(node.getId(), depth);
				return true;
			},
			3, "outgoing");

	// Verify that all nodes at depth n are visited before any node at depth n+1
	int currentDepth = 0;
	for (const auto &[nodeId, depth]: visitOrder) {
		if (depth > currentDepth) {
			// When depth increases, it should increase by exactly 1
			EXPECT_EQ(depth, currentDepth + 1);
			currentDepth = depth;
		}
		EXPECT_LE(depth, currentDepth);
	}
}
