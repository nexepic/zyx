/**
 * @file test_GraphAlgorithm.cpp
 * @author Nexepic
 * @date 2025/7/23
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/query/algorithm/GraphAlgorithm.hpp"
#include "graph/query/algorithm/GraphProjection.hpp"

namespace fs = std::filesystem;

class GraphAlgorithmTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Unique file path
		auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_algo_" + to_string(uuid) + ".dat");

		// Initialize DB
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();

		initPointers();
		setupTestGraph();
	}

	void TearDown() override {
		if (database)
			database->close();
		database.reset();
		std::error_code ec;
		if (fs::exists(testFilePath))
			fs::remove(testFilePath, ec);
	}

	void initPointers() {
		dataManager = database->getStorage()->getDataManager();
		algo = std::make_unique<graph::query::algorithm::GraphAlgorithm>(dataManager);
	}

	void setupTestGraph() {
		// Topology:
		//     1 ← 6
		//    ↙ ↘   ↘
		//   2 → 3 ← 7 → 8
		//   ↓   ↓       ↓
		//   4 → 5 ← 9 → 10
		//       ↑   ↓
		//      11 → 12

		// Create Nodes (IDs 1-12)
		for (int i = 1; i <= 12; ++i) {
			graph::Node n(i, dataManager->getOrCreateLabelId("Node" + std::to_string(i)));
			dataManager->addNode(n);
		}

		// Ensure IDs match what we expect for readable tests
		// Note: ID allocation is usually sequential, but let's assume 1-12 mapped correctly.
		node1Id = 1;
		node2Id = 2;
		node3Id = 3;
		node4Id = 4;
		node5Id = 5;
		node6Id = 6;
		node7Id = 7;
		node8Id = 8;
		node9Id = 9;
		node10Id = 10;
		node11Id = 11;
		node12Id = 12;

		// Create Edges
		std::vector<std::pair<int64_t, int64_t>> connections = {
				{node1Id, node2Id},	 {node1Id, node3Id},  {node2Id, node3Id},  {node2Id, node4Id},
				{node3Id, node5Id},	 {node4Id, node5Id},  {node6Id, node1Id},  {node6Id, node8Id},
				{node7Id, node3Id},	 {node7Id, node8Id},  {node8Id, node10Id}, {node9Id, node5Id},
				{node9Id, node10Id}, {node9Id, node12Id}, {node11Id, node5Id}, {node11Id, node12Id}};

		for (size_t i = 0; i < connections.size(); ++i) {
			int64_t edgeId = static_cast<int64_t>(100) + static_cast<int64_t>(i);
			graph::Edge edge(edgeId, connections[i].first, connections[i].second,
							 dataManager->getOrCreateLabelId("LINK"));
			dataManager->addEdge(edge);
			// Note: DataManager::addEdge MUST call RelationshipTraversal::linkEdge internally!
		}

		// [CRITICAL] Persist and Reload to ensure graph topology is visible to all subsystems
		database->getStorage()->flush();
		database->close();
		database->open();
		initPointers();
	}

	fs::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::unique_ptr<graph::query::algorithm::GraphAlgorithm> algo;

	int64_t node1Id = 0, node2Id = 0, node3Id = 0, node4Id = 0, node5Id = 0, node6Id = 0, node7Id = 0, node8Id = 0,
			node9Id = 0, node10Id = 0, node11Id = 0, node12Id = 0;
};

// ============================================================================
// Shortest Path Tests
// ============================================================================

TEST_F(GraphAlgorithmTest, FindShortestPathDirect) {
	auto path = algo->findShortestPath(node1Id, node2Id, "out");
	ASSERT_EQ(path.size(), 2u);
	EXPECT_EQ(path[0].getId(), node1Id);
	EXPECT_EQ(path[1].getId(), node2Id);
}

TEST_F(GraphAlgorithmTest, FindShortestPathTwoHops) {
	// 1 -> 3 -> 5 (Length 3)
	// 1 -> 2 -> 4 -> 5 (Length 4) - BFS guarantees shortest
	auto path = algo->findShortestPath(node1Id, node5Id, "out");
	ASSERT_EQ(path.size(), 3u);
	EXPECT_EQ(path[0].getId(), node1Id);
	EXPECT_EQ(path[2].getId(), node5Id);
}

TEST_F(GraphAlgorithmTest, FindShortestPathReverse) {
	// 5 <- 3 <- 1
	auto path = algo->findShortestPath(node5Id, node1Id, "in");
	ASSERT_EQ(path.size(), 3u);
	EXPECT_EQ(path[0].getId(), node5Id);
	EXPECT_EQ(path[2].getId(), node1Id);
}

TEST_F(GraphAlgorithmTest, FindShortestPathNoConnection) {
	// 5 -> 1 (No outgoing path)
	auto path = algo->findShortestPath(node5Id, node1Id, "out");
	EXPECT_TRUE(path.empty());
}

TEST_F(GraphAlgorithmTest, FindShortestPathDisconnected) {
	// 12 has no outgoing edges, so it cannot reach 1 in 'out' mode.
	// Changing "both" -> "out" makes the expectation of empty path correct.
	auto path = algo->findShortestPath(node12Id, node1Id, "out");
	EXPECT_TRUE(path.empty());
}

TEST_F(GraphAlgorithmTest, FindShortestPathTrulyIsolated) {
	// Create a node that is not connected to the main component
	graph::Node node13(0, dataManager->getOrCreateLabelId("Island"));
	dataManager->addNode(node13);

	// Now this is guaranteed to be empty even with "both"
	auto path = algo->findShortestPath(node13.getId(), node1Id, "both");
	EXPECT_TRUE(path.empty());
}

// ============================================================================
// Variable Length Path Tests (Replaces findConnectedNodes)
// ============================================================================

TEST_F(GraphAlgorithmTest, FindAllPaths_1Hop) {
	// Equivalent to findConnectedNodes(1, out)
	// 1 -> 2, 1 -> 3
	auto nodes = algo->findAllPaths(node1Id, 1, 1, "", "out");
	ASSERT_EQ(nodes.size(), 2u);

	std::unordered_set<int64_t> ids;
	for (const auto &n: nodes)
		ids.insert(n.getId());
	EXPECT_TRUE(ids.contains(node2Id));
	EXPECT_TRUE(ids.contains(node3Id));
}

TEST_F(GraphAlgorithmTest, FindAllPaths_Range) {
	// 1 -> [1..2] hops
	// 1 hop: 2, 3
	// 2 hops: 3(via 2), 4(via 2), 5(via 3)
	auto nodes = algo->findAllPaths(node1Id, 1, 2, "", "out");

	std::unordered_set<int64_t> ids;
	for (const auto &n: nodes)
		ids.insert(n.getId());

	EXPECT_TRUE(ids.contains(node2Id));
	EXPECT_TRUE(ids.contains(node3Id));
	EXPECT_TRUE(ids.contains(node4Id));
	EXPECT_TRUE(ids.contains(node5Id));
}

// ============================================================================
// Breadth First Traversal Tests
// ============================================================================

TEST_F(GraphAlgorithmTest, BFS_Basic) {
	std::vector<int64_t> visited;

	// Logic: The algorithm runs until queue empty or visitor returns false.
	// If you want to limit depth, check 'depth' in the lambda.
	algo->breadthFirstTraversal(
			node1Id,
			[&](int64_t id, int depth) {
				if (depth > 3)
					return false; // Stop if too deep
				visited.push_back(id);
				return true;
			},
			"out");

	ASSERT_GE(visited.size(), 3u);
	EXPECT_EQ(visited[0], node1Id);
}

TEST_F(GraphAlgorithmTest, BFS_MaxDepth) {
	std::vector<int64_t> visited;

	// Depth 1 (Neighbors only)
	algo->breadthFirstTraversal(
			node1Id,
			[&](int64_t id, int depth) {
				if (depth > 1)
					return false; // [FIXED] Limit depth inside lambda
				visited.push_back(id);
				return true;
			},
			"out");

	// 1, 2, 3
	ASSERT_EQ(visited.size(), 3u);
	EXPECT_EQ(visited[0], node1Id);
}

TEST_F(GraphAlgorithmTest, BFS_EarlyTermination) {
	int count = 0;
	// Removed the integer argument '5'.
	algo->breadthFirstTraversal(
			node1Id,
			[&](int64_t, int) {
				count++;
				return false; // Stop immediately
			},
			"out");

	EXPECT_EQ(count, 1);
}

TEST_F(GraphAlgorithmTest, BFS_CycleDetection) {
	// 1 -> 2 -> 3
	// Add cycle: 3 -> 1
	graph::Edge cycle(200, node3Id, node1Id, dataManager->getOrCreateLabelId("CYCLE"));
	dataManager->addEdge(cycle);
	// Flush/Reload usually not needed for in-memory traversal updates if DM is live,
	// but good practice if tests are flaky.

	std::vector<int64_t> visited;
	// Removed the integer argument '10', moved depth check inside lambda.
	algo->breadthFirstTraversal(
			node1Id,
			[&](int64_t id, int depth) {
				if (depth > 10)
					return false;
				visited.push_back(id);
				return true;
			},
			"out");

	// Count occurrences
	std::unordered_map<int64_t, int> counts;
	for (auto id: visited)
		counts[id]++;

	// Should visit 1 only once despite cycle
	EXPECT_EQ(counts[node1Id], 1);
}

// ============================================================================
// Helper Verification (Hydration)
// ============================================================================

TEST_F(GraphAlgorithmTest, BFS_HydrationOnDemand) {
	// Verify we can load properties inside the callback
	dataManager->addNodeProperties(node1Id, {{"prop", std::string("val")}});

	bool propFound = false;
	algo->breadthFirstTraversal(node1Id, [&](int64_t id, int) {
		if (id == node1Id) {
			auto props = dataManager->getNodeProperties(id);
			if (props.contains("prop") && props.at("prop").toString() == "val") {
				propFound = true;
			}
		}
		return true;
	});

	EXPECT_TRUE(propFound);
}

// ============================================================================
// Edge Label Filtering Tests
// ============================================================================

TEST_F(GraphAlgorithmTest, FindAllPathsWithEdgeLabelFilter) {
	// Test findAllPaths with edge label filter
	// Create some edges with a specific label
	graph::Edge labeled1(300, node1Id, node2Id, dataManager->getOrCreateLabelId("SPECIAL"));
	graph::Edge labeled2(301, node2Id, node4Id, dataManager->getOrCreateLabelId("SPECIAL"));
	dataManager->addEdge(labeled1);
	dataManager->addEdge(labeled2);

	database->getStorage()->flush();
	database->close();
	database->open();
	initPointers();

	// Find paths using only SPECIAL labeled edges
	auto nodes = algo->findAllPaths(node1Id, 1, 2, "SPECIAL", "out");

	std::unordered_set<int64_t> ids;
	for (const auto &n: nodes)
		ids.insert(n.getId());

	// Should find node2 (direct SPECIAL edge) and node4 (via SPECIAL edges)
	EXPECT_TRUE(ids.contains(node2Id));
	EXPECT_TRUE(ids.contains(node4Id));
}

TEST_F(GraphAlgorithmTest, FindShortestPathWithSameNode) {
	// Test the trivial case: start == end
	auto path = algo->findShortestPath(node1Id, node1Id, "out");

	// Should return a path with just the starting node
	ASSERT_EQ(path.size(), 1u);
	EXPECT_EQ(path[0].getId(), node1Id);
}

TEST_F(GraphAlgorithmTest, FindShortestPathWithMaxDepth) {
	// Test maxDepth limit
	// Path from 1 to 5 is: 1 -> 3 -> 5 (2 hops)
	// With maxDepth=1, should not find the path

	auto path = algo->findShortestPath(node1Id, node5Id, "out", 1);
	EXPECT_TRUE(path.empty());
}

TEST_F(GraphAlgorithmTest, FindShortestPathWithMaxDepthSufficient) {
	// Test with sufficient maxDepth
	// Path from 1 to 5 is: 1 -> 3 -> 5 (2 hops)
	// With maxDepth=3, should find the path

	auto path = algo->findShortestPath(node1Id, node5Id, "out", 3);
	EXPECT_FALSE(path.empty());
	EXPECT_EQ(path[0].getId(), node1Id);
	EXPECT_EQ(path.back().getId(), node5Id);
}

// ============================================================================
// Depth First Traversal Tests
// ============================================================================

TEST_F(GraphAlgorithmTest, DFS_Basic) {
	std::vector<int64_t> visited;
	std::vector<int> depths;

	algo->depthFirstTraversal(
		node1Id,
		[&](int64_t id, int depth) {
			visited.push_back(id);
			depths.push_back(depth);
			return true;
		},
		"out");

	EXPECT_GE(visited.size(), 3u);
	EXPECT_EQ(visited[0], node1Id);
	EXPECT_EQ(depths[0], 0);
}

TEST_F(GraphAlgorithmTest, DFS_MaxDepth) {
	std::vector<int64_t> visited;

	algo->depthFirstTraversal(
		node1Id,
		[&](int64_t id, int depth) {
			if (depth > 1)
				return false; // Stop at depth 1
			visited.push_back(id);
			return true;
		},
		"out");

	// Should visit node1 at depth 0, and its direct neighbors at depth 1
	EXPECT_GE(visited.size(), 1u);
	EXPECT_EQ(visited[0], node1Id);
}

TEST_F(GraphAlgorithmTest, DFS_EarlyTermination) {
	int count = 0;

	algo->depthFirstTraversal(
		node1Id,
		[&](int64_t, int) {
			count++;
			return false; // Stop immediately
		},
		"out");

	EXPECT_EQ(count, 1);
}

TEST_F(GraphAlgorithmTest, DFS_CycleDetection) {
	// Add a cycle: 3 -> 1
	graph::Edge cycle(201, node3Id, node1Id, dataManager->getOrCreateLabelId("CYCLE"));
	dataManager->addEdge(cycle);

	std::vector<int64_t> visited;
	algo->depthFirstTraversal(
		node1Id,
		[&](int64_t id, int) {
			visited.push_back(id);
			return true;
		},
		"out");

	// Count occurrences
	std::unordered_map<int64_t, int> counts;
	for (auto id: visited)
		counts[id]++;

	// Should visit node1 only once despite cycle
	EXPECT_EQ(counts[node1Id], 1);
}

// ============================================================================
// Branch Coverage Improvement Tests
// ============================================================================

// Tests DFS traversal with invalid (non-existent) start node
// Covers: Branch (209:7) True path - dm_->getNode(startNodeId).getId() == 0
TEST_F(GraphAlgorithmTest, DFS_InvalidStartNode) {
	int count = 0;
	algo->depthFirstTraversal(
		999999, // Non-existent node
		[&](int64_t, int) {
			count++;
			return true;
		},
		"out");

	// Should not visit any nodes since start node doesn't exist
	EXPECT_EQ(count, 0);
}

// Tests DFS variable length path with a cycle
// Covers: Branch (151:8) True path - cycle detection in dfsVariableLength
TEST_F(GraphAlgorithmTest, FindAllPaths_CycleInVariableLength) {
	// Add a cycle: 3 -> 1 to create a cycle 1->3->1
	graph::Edge cycle(202, node3Id, node1Id, dataManager->getOrCreateLabelId("LINK"));
	dataManager->addEdge(cycle);

	database->getStorage()->flush();
	database->close();
	database->open();
	initPointers();

	// findAllPaths with enough depth to encounter the cycle
	auto nodes = algo->findAllPaths(node1Id, 0, 5, "", "out");

	// Should not contain duplicates due to cycle detection
	std::unordered_map<int64_t, int> counts;
	for (const auto &n : nodes)
		counts[n.getId()]++;

	// Node 1 should appear at most once
	EXPECT_LE(counts[node1Id], 1);
}

// Tests findShortestPath trivial case where the start node does not exist
// Covers: Branch (68:8) False path - n.getId() == 0 when startNodeId == endNodeId
TEST_F(GraphAlgorithmTest, FindShortestPathSameNodeNonExistent) {
	// Use a node ID that doesn't exist
	auto path = algo->findShortestPath(999999, 999999, "out");

	// Should return a single node with id 0 (empty/invalid node)
	ASSERT_EQ(path.size(), 1u);
	EXPECT_EQ(path[0].getId(), 0);
}

// Tests DFS traversal where stack may push duplicates
// Covers: Branch (218:8) False path - visited.contains(currId) is true
// This happens when a node is pushed to the stack from multiple parents
// before it gets popped and marked as visited
TEST_F(GraphAlgorithmTest, DFS_DuplicateInStack) {
	// Node 3 is reachable from both node1 and node2
	// When doing DFS from node1:
	// - Stack: [(1,0)]
	// - Pop 1, push neighbors 2,3: Stack: [(2,1),(3,1)]
	// - Pop 2 (or 3 depending on order), push its neighbors including 3
	// - When we pop node 3 from the stack, it may already be visited
	// Using "both" direction increases chance of duplicates in stack
	std::vector<int64_t> visited;
	algo->depthFirstTraversal(
		node1Id,
		[&](int64_t id, int) {
			visited.push_back(id);
			return true;
		},
		"both"); // "both" direction gives more edges, increasing duplicate pushes

	// Count occurrences - each node should appear exactly once
	std::unordered_map<int64_t, int> counts;
	for (auto id : visited)
		counts[id]++;

	for (const auto &[id, count] : counts) {
		EXPECT_EQ(count, 1) << "Node " << id << " should appear exactly once in DFS";
	}
}

// ============================================================================
// GDS Algorithm Tests (GraphProjection-based)
// ============================================================================

// --- Dijkstra Tests ---

TEST_F(GraphAlgorithmTest, Dijkstra_BasicPath) {
	// Build unweighted projection (all edges weight 1.0)
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);

	auto result = algo->dijkstra(proj, node1Id, node5Id);
	ASSERT_FALSE(result.nodes.empty());
	EXPECT_EQ(result.nodes.front().getId(), node1Id);
	EXPECT_EQ(result.nodes.back().getId(), node5Id);
	// Shortest path: 1->3->5 (2 hops, weight 2.0)
	EXPECT_DOUBLE_EQ(result.totalWeight, 2.0);
	EXPECT_EQ(result.nodes.size(), 3u);
}

TEST_F(GraphAlgorithmTest, Dijkstra_SameNode) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);

	auto result = algo->dijkstra(proj, node1Id, node1Id);
	ASSERT_EQ(result.nodes.size(), 1u);
	EXPECT_EQ(result.nodes[0].getId(), node1Id);
	EXPECT_DOUBLE_EQ(result.totalWeight, 0.0);
}

TEST_F(GraphAlgorithmTest, Dijkstra_NoPath) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);

	// Node 5 has no outgoing edges, so no path from 5 to 1 via out-edges
	auto result = algo->dijkstra(proj, node5Id, node1Id);
	EXPECT_TRUE(result.nodes.empty());
}

TEST_F(GraphAlgorithmTest, Dijkstra_NodeNotInProjection) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);

	auto result = algo->dijkstra(proj, 999, node1Id);
	EXPECT_TRUE(result.nodes.empty());
}

TEST_F(GraphAlgorithmTest, Dijkstra_EndNodeNotInProjection) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);

	// startId valid, endId invalid (covers !contains(endId) branch)
	auto result = algo->dijkstra(proj, node1Id, 999);
	EXPECT_TRUE(result.nodes.empty());
}

TEST_F(GraphAlgorithmTest, Dijkstra_WeightedEdges) {
	// Create a small weighted graph: A-[5]->B-[1]->C and A-[3]->C
	// Dijkstra should pick A->C (weight 3) over A->B->C (weight 6)
	// This also triggers the stale-entry branch (cost > dist[current])
	// and the shorter-path update branch (newCost < it->second)
	graph::Node nA(0, dataManager->getOrCreateLabelId("W"));
	graph::Node nB(0, dataManager->getOrCreateLabelId("W"));
	graph::Node nC(0, dataManager->getOrCreateLabelId("W"));
	dataManager->addNode(nA);
	dataManager->addNode(nB);
	dataManager->addNode(nC);

	graph::Edge eAB(0, nA.getId(), nB.getId(), dataManager->getOrCreateLabelId("R"));
	graph::Edge eBC(0, nB.getId(), nC.getId(), dataManager->getOrCreateLabelId("R"));
	graph::Edge eAC(0, nA.getId(), nC.getId(), dataManager->getOrCreateLabelId("R"));
	dataManager->addEdge(eAB);
	dataManager->addEdge(eBC);
	dataManager->addEdge(eAC);

	dataManager->addEdgeProperties(eAB.getId(), {{"cost", graph::PropertyValue(5.0)}});
	dataManager->addEdgeProperties(eBC.getId(), {{"cost", graph::PropertyValue(1.0)}});
	dataManager->addEdgeProperties(eAC.getId(), {{"cost", graph::PropertyValue(3.0)}});

	database->getStorage()->flush();
	database->close();
	database->open();
	initPointers();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "W", "R", "cost");

	auto result = algo->dijkstra(proj, nA.getId(), nC.getId());
	ASSERT_FALSE(result.nodes.empty());
	// Direct path A->C with weight 3 is shorter than A->B->C with weight 6
	EXPECT_DOUBLE_EQ(result.totalWeight, 3.0);
	EXPECT_EQ(result.nodes.size(), 2u);
}

TEST_F(GraphAlgorithmTest, Dijkstra_StalePriorityQueueEntries) {
	// Graph: A-[10]->X, A-[1]->Y, Y-[1]->X, X-[100]->Z. Target = Z.
	// Processing order: A(0), Y(1), X(2), stale X(10) popped & skipped, Z(102)
	// The stale X entry (cost=10 > dist[X]=2) triggers the skip branch
	graph::Node nA(0, dataManager->getOrCreateLabelId("SQ"));
	graph::Node nX(0, dataManager->getOrCreateLabelId("SQ"));
	graph::Node nY(0, dataManager->getOrCreateLabelId("SQ"));
	graph::Node nZ(0, dataManager->getOrCreateLabelId("SQ"));
	dataManager->addNode(nA);
	dataManager->addNode(nX);
	dataManager->addNode(nY);
	dataManager->addNode(nZ);

	graph::Edge eAX(0, nA.getId(), nX.getId(), dataManager->getOrCreateLabelId("SQR"));
	graph::Edge eAY(0, nA.getId(), nY.getId(), dataManager->getOrCreateLabelId("SQR"));
	graph::Edge eYX(0, nY.getId(), nX.getId(), dataManager->getOrCreateLabelId("SQR"));
	graph::Edge eXZ(0, nX.getId(), nZ.getId(), dataManager->getOrCreateLabelId("SQR"));
	dataManager->addEdge(eAX);
	dataManager->addEdge(eAY);
	dataManager->addEdge(eYX);
	dataManager->addEdge(eXZ);

	dataManager->addEdgeProperties(eAX.getId(), {{"w", graph::PropertyValue(10.0)}});
	dataManager->addEdgeProperties(eAY.getId(), {{"w", graph::PropertyValue(1.0)}});
	dataManager->addEdgeProperties(eYX.getId(), {{"w", graph::PropertyValue(1.0)}});
	dataManager->addEdgeProperties(eXZ.getId(), {{"w", graph::PropertyValue(100.0)}});

	database->getStorage()->flush();
	database->close();
	database->open();
	initPointers();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "SQ", "SQR", "w");

	// Path: A->Y->X->Z = cost 102 (shorter than A->X->Z = cost 110)
	// Stale X(10) entry will be popped and skipped after X(2) was already processed
	auto result = algo->dijkstra(proj, nA.getId(), nZ.getId());
	ASSERT_FALSE(result.nodes.empty());
	EXPECT_DOUBLE_EQ(result.totalWeight, 102.0);
	EXPECT_EQ(result.nodes.size(), 4u); // A->Y->X->Z
}

TEST_F(GraphAlgorithmTest, Betweenness_SamplingSizeExceedsNodeCount) {
	// Create a small graph and set samplingSize larger than node count
	// This covers the branch where samplingSize >= n
	graph::Node x(0, dataManager->getOrCreateLabelId("Tiny"));
	graph::Node y(0, dataManager->getOrCreateLabelId("Tiny"));
	dataManager->addNode(x);
	dataManager->addNode(y);

	graph::Edge exy(0, x.getId(), y.getId(), dataManager->getOrCreateLabelId("T"));
	dataManager->addEdge(exy);

	database->getStorage()->flush();
	database->close();
	database->open();
	initPointers();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "Tiny", "T");
	// samplingSize=100 but only 2 nodes: should use all nodes (no sampling)
	auto scores = algo->betweennessCentrality(proj, 100);
	ASSERT_EQ(scores.size(), 2u);
}

// --- PageRank Tests ---

TEST_F(GraphAlgorithmTest, PageRank_BasicScores) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);

	auto scores = algo->pageRank(proj, 20, 0.85);
	ASSERT_EQ(scores.size(), 12u);

	// All scores should be positive
	for (const auto &ns : scores) {
		EXPECT_GT(ns.score, 0.0);
	}

	// Scores should sum to approximately 1.0
	double total = 0.0;
	for (const auto &ns : scores) {
		total += ns.score;
	}
	EXPECT_NEAR(total, 1.0, 0.01);

	// Results should be sorted descending
	for (size_t i = 1; i < scores.size(); ++i) {
		EXPECT_GE(scores[i - 1].score, scores[i].score);
	}
}

TEST_F(GraphAlgorithmTest, PageRank_SingleNode) {
	graph::Node solo(0, dataManager->getOrCreateLabelId("Solo"));
	dataManager->addNode(solo);
	database->getStorage()->flush();
	database->close();
	database->open();
	initPointers();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "Solo");
	auto scores = algo->pageRank(proj);
	ASSERT_EQ(scores.size(), 1u);
	EXPECT_NEAR(scores[0].score, 1.0, 0.01);
}

TEST_F(GraphAlgorithmTest, PageRank_EmptyGraph) {
	// Build a projection with a label that no nodes have
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "NonExistent");
	auto scores = algo->pageRank(proj);
	EXPECT_TRUE(scores.empty());
}

TEST_F(GraphAlgorithmTest, PageRank_Convergence) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);

	// With enough iterations, should converge (no crash or divergence)
	auto scores = algo->pageRank(proj, 100, 0.85, 1e-10);
	ASSERT_EQ(scores.size(), 12u);
}

// --- Connected Components Tests ---

TEST_F(GraphAlgorithmTest, WCC_SingleComponent) {
	// The test graph topology is connected via undirected edges
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);

	auto components = algo->connectedComponents(proj);
	ASSERT_EQ(components.size(), 12u);

	// All nodes should be in the same component
	std::unordered_set<int64_t> componentIds;
	for (const auto &nc : components) {
		componentIds.insert(nc.componentId);
	}
	EXPECT_EQ(componentIds.size(), 1u);
}

TEST_F(GraphAlgorithmTest, WCC_MultipleComponents) {
	// Create two isolated nodes (separate from main graph)
	graph::Node iso1(0, dataManager->getOrCreateLabelId("Island"));
	graph::Node iso2(0, dataManager->getOrCreateLabelId("Island"));
	dataManager->addNode(iso1);
	dataManager->addNode(iso2);

	// Connect the two islands to each other but not to the main graph
	graph::Edge eIsland(0, iso1.getId(), iso2.getId(), dataManager->getOrCreateLabelId("BRIDGE"));
	dataManager->addEdge(eIsland);

	database->getStorage()->flush();
	database->close();
	database->open();
	initPointers();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
	auto components = algo->connectedComponents(proj);
	EXPECT_EQ(components.size(), 14u);

	std::unordered_set<int64_t> componentIds;
	for (const auto &nc : components) {
		componentIds.insert(nc.componentId);
	}
	// Should have 2 components: main graph + island pair
	EXPECT_EQ(componentIds.size(), 2u);
}

TEST_F(GraphAlgorithmTest, WCC_IsolatedNodes) {
	// Create 3 completely isolated nodes
	graph::Node i1(0, dataManager->getOrCreateLabelId("Iso"));
	graph::Node i2(0, dataManager->getOrCreateLabelId("Iso"));
	graph::Node i3(0, dataManager->getOrCreateLabelId("Iso"));
	dataManager->addNode(i1);
	dataManager->addNode(i2);
	dataManager->addNode(i3);

	database->getStorage()->flush();
	database->close();
	database->open();
	initPointers();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "Iso");
	auto components = algo->connectedComponents(proj);
	ASSERT_EQ(components.size(), 3u);

	// Each node should be its own component
	std::unordered_set<int64_t> componentIds;
	for (const auto &nc : components) {
		componentIds.insert(nc.componentId);
	}
	EXPECT_EQ(componentIds.size(), 3u);
}

TEST_F(GraphAlgorithmTest, WCC_EmptyGraph) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "NonExistent");
	auto components = algo->connectedComponents(proj);
	EXPECT_TRUE(components.empty());
}

// --- Betweenness Centrality Tests ---

TEST_F(GraphAlgorithmTest, Betweenness_BasicScores) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);

	auto scores = algo->betweennessCentrality(proj);
	ASSERT_EQ(scores.size(), 12u);

	// All scores should be non-negative
	for (const auto &ns : scores) {
		EXPECT_GE(ns.score, 0.0);
	}

	// Results should be sorted descending
	for (size_t i = 1; i < scores.size(); ++i) {
		EXPECT_GE(scores[i - 1].score, scores[i].score);
	}
}

TEST_F(GraphAlgorithmTest, Betweenness_WithSampling) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);

	auto scores = algo->betweennessCentrality(proj, 4);
	ASSERT_EQ(scores.size(), 12u);

	// With sampling, scores should still be non-negative
	for (const auto &ns : scores) {
		EXPECT_GE(ns.score, 0.0);
	}
}

TEST_F(GraphAlgorithmTest, Betweenness_EmptyGraph) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "NonExistent");
	auto scores = algo->betweennessCentrality(proj);
	EXPECT_TRUE(scores.empty());
}

TEST_F(GraphAlgorithmTest, Betweenness_LinearGraph) {
	// A -> B -> C: B should have highest betweenness
	graph::Node a(0, dataManager->getOrCreateLabelId("Lin"));
	graph::Node b(0, dataManager->getOrCreateLabelId("Lin"));
	graph::Node c(0, dataManager->getOrCreateLabelId("Lin"));
	dataManager->addNode(a);
	dataManager->addNode(b);
	dataManager->addNode(c);

	graph::Edge e1(0, a.getId(), b.getId(), dataManager->getOrCreateLabelId("SEQ"));
	graph::Edge e2(0, b.getId(), c.getId(), dataManager->getOrCreateLabelId("SEQ"));
	dataManager->addEdge(e1);
	dataManager->addEdge(e2);

	database->getStorage()->flush();
	database->close();
	database->open();
	initPointers();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "Lin", "SEQ");
	auto scores = algo->betweennessCentrality(proj);
	ASSERT_EQ(scores.size(), 3u);

	// B (middle node) should have the highest betweenness
	EXPECT_EQ(scores[0].nodeId, b.getId());
	EXPECT_GT(scores[0].score, 0.0);
}

// --- Closeness Centrality Tests ---

TEST_F(GraphAlgorithmTest, Closeness_BasicScores) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager);

	auto scores = algo->closenessCentrality(proj);
	ASSERT_EQ(scores.size(), 12u);

	// All scores should be non-negative
	for (const auto &ns : scores) {
		EXPECT_GE(ns.score, 0.0);
	}

	// Results should be sorted descending
	for (size_t i = 1; i < scores.size(); ++i) {
		EXPECT_GE(scores[i - 1].score, scores[i].score);
	}
}

TEST_F(GraphAlgorithmTest, Closeness_EmptyGraph) {
	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "NonExistent");
	auto scores = algo->closenessCentrality(proj);
	EXPECT_TRUE(scores.empty());
}

TEST_F(GraphAlgorithmTest, Closeness_SingleNode) {
	graph::Node solo(0, dataManager->getOrCreateLabelId("Solo2"));
	dataManager->addNode(solo);
	database->getStorage()->flush();
	database->close();
	database->open();
	initPointers();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "Solo2");
	auto scores = algo->closenessCentrality(proj);
	ASSERT_EQ(scores.size(), 1u);
	// Single node has closeness 0 (no neighbors reachable)
	EXPECT_DOUBLE_EQ(scores[0].score, 0.0);
}

TEST_F(GraphAlgorithmTest, Closeness_StarGraph) {
	// Center node connected to 3 leaves: center should have highest closeness
	graph::Node center(0, dataManager->getOrCreateLabelId("Star"));
	graph::Node leaf1(0, dataManager->getOrCreateLabelId("Star"));
	graph::Node leaf2(0, dataManager->getOrCreateLabelId("Star"));
	graph::Node leaf3(0, dataManager->getOrCreateLabelId("Star"));
	dataManager->addNode(center);
	dataManager->addNode(leaf1);
	dataManager->addNode(leaf2);
	dataManager->addNode(leaf3);

	graph::Edge e1(0, center.getId(), leaf1.getId(), dataManager->getOrCreateLabelId("RAY"));
	graph::Edge e2(0, center.getId(), leaf2.getId(), dataManager->getOrCreateLabelId("RAY"));
	graph::Edge e3(0, center.getId(), leaf3.getId(), dataManager->getOrCreateLabelId("RAY"));
	dataManager->addEdge(e1);
	dataManager->addEdge(e2);
	dataManager->addEdge(e3);

	database->getStorage()->flush();
	database->close();
	database->open();
	initPointers();

	auto proj = graph::query::algorithm::GraphProjection::build(dataManager, "Star", "RAY");
	auto scores = algo->closenessCentrality(proj);
	ASSERT_EQ(scores.size(), 4u);

	// Center node should have highest closeness
	EXPECT_EQ(scores[0].nodeId, center.getId());
}
