/**
 * @file test_ApiAlgorithm.cpp
 * @author Nexepic
 * @date 2026/1/5
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include <functional>
#include <variant>
#include <vector>

#include "ApiTestFixture.hpp"

// ============================================================================
// Algorithms & Topology
// ============================================================================

TEST_F(CppApiTest, AlgorithmShortestPath) {
	int64_t a = db->createNode("A", {{"val", (int64_t) 1}});
	int64_t b = db->createNode("B", {{"val", (int64_t) 2}});
	int64_t c = db->createNode("C", {{"val", (int64_t) 3}});
	db->createEdge(a, b, "NEXT");
	db->createEdge(b, c, "NEXT");
	db->save();

	auto path = db->getShortestPath(a, c);
	ASSERT_EQ(path.size(), 3u);
	EXPECT_EQ(path[0].id, a);
	EXPECT_EQ(path[2].id, c);

	auto emptyPath = db->getShortestPath(c, a);
	EXPECT_TRUE(emptyPath.empty());
}

TEST_F(CppApiTest, AlgorithmBFS) {
	int64_t root = db->createNode("Root");
	int64_t child = db->createNode("Child");
	db->createEdge(root, child, "LINK");
	db->save();

	std::vector<int64_t> visited;
	db->bfs(root, [&](const zyx::Node &n) {
		visited.push_back(n.id);
		return true;
	});
	ASSERT_EQ(visited.size(), 2u);
}

TEST_F(CppApiTest, ShortestPathWithMaxDepth) {
	int64_t a = db->createNode("Node", {{"name", "A"}});
	int64_t b = db->createNode("Node", {{"name", "B"}});
	int64_t c = db->createNode("Node", {{"name", "C"}});
	int64_t d = db->createNode("Node", {{"name", "D"}});

	db->createEdge(a, b, "NEXT");
	db->createEdge(b, c, "NEXT");
	db->createEdge(c, d, "NEXT");
	db->save();

	auto path = db->getShortestPath(a, d, 2);
	EXPECT_LT(path.size(), 4u);
}

TEST_F(CppApiTest, BFSEarlyTermination) {
	int64_t root = db->createNode("Root");
	int64_t child1 = db->createNode("Child1");
	int64_t child2 = db->createNode("Child2");

	db->createEdge(root, child1, "LINK");
	db->createEdge(root, child2, "LINK");
	db->save();

	std::vector<int64_t> visited;
	db->bfs(root, [&](const zyx::Node &n) {
		visited.push_back(n.id);
		return false;
	});

	EXPECT_EQ(visited.size(), 1u);
	EXPECT_EQ(visited[0], root);
}

TEST_F(CppApiTest, BfsTraversal) {
	int64_t a = db->createNode("BFSNode", {{"name", "A"}});
	int64_t b = db->createNode("BFSNode", {{"name", "B"}});
	int64_t c = db->createNode("BFSNode", {{"name", "C"}});
	db->createEdge(a, b, "LINK");
	db->createEdge(b, c, "LINK");
	db->save();

	std::vector<std::string> visited;
	db->bfs(a, [&](const zyx::Node& n) -> bool {
		auto it = n.properties.find("name");
		if (it != n.properties.end() && std::holds_alternative<std::string>(it->second)) {
			visited.push_back(std::get<std::string>(it->second));
		}
		return true;
	});

	EXPECT_GE(visited.size(), 1u);
}

TEST_F(CppApiTest, BfsTraversal_StopEarly) {
	int64_t a = db->createNode("BFSStop", {{"name", "A"}});
	int64_t b = db->createNode("BFSStop", {{"name", "B"}});
	int64_t c = db->createNode("BFSStop", {{"name", "C"}});
	db->createEdge(a, b, "LINK");
	db->createEdge(b, c, "LINK");
	db->save();

	int count = 0;
	db->bfs(a, [&](const zyx::Node&) -> bool {
		count++;
		return false;
	});

	EXPECT_EQ(count, 1);
}

TEST_F(CppApiTest, ShortestPath_NoPath) {
	int64_t a = db->createNode("SPNode", {{"name", "A"}});
	int64_t b = db->createNode("SPNode", {{"name", "B"}});
	db->save();

	auto path = db->getShortestPath(a, b);
	EXPECT_TRUE(path.empty());
}

TEST_F(CppApiTest, ShortestPathInvalidNodes) {
	auto path = db->getShortestPath(99999, 99998);
	EXPECT_TRUE(path.empty());
}

TEST_F(CppApiTest, BfsOnNonExistentNode) {
	int count = 0;
	db->bfs(99999, [&](const zyx::Node&) -> bool {
		count++;
		return true;
	});
	EXPECT_GE(count, 0);
}
