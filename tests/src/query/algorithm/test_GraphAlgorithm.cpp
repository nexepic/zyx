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
		if (fs::exists(testFilePath))
			fs::remove(testFilePath);
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
			graph::Node n(i, "Node" + std::to_string(i));
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
			graph::Edge edge(100 + i, connections[i].first, connections[i].second, "LINK");
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
	graph::Node node13(0, "Island");
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
	graph::Edge cycle(200, node3Id, node1Id, "CYCLE");
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
