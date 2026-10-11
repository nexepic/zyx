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

#include <algorithm>
#include <functional>
#include <queue>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "ApiTestFixture.hpp"
#include "graph/debug/PerfTrace.hpp"

namespace {

	using LogicalNodeId = int64_t;
	using DirectedAdjacency = std::unordered_map<LogicalNodeId, std::vector<LogicalNodeId>>;

	[[nodiscard]] std::unordered_map<LogicalNodeId, size_t>
	calculateDirectedHopDistances(const DirectedAdjacency &adjacency, const LogicalNodeId source) {
		std::unordered_map<LogicalNodeId, size_t> distances;
		std::queue<LogicalNodeId> pending;
		distances.emplace(source, 0U);
		pending.push(source);
		while (!pending.empty()) {
			const LogicalNodeId current = pending.front();
			pending.pop();
			const auto adjacencyIt = adjacency.find(current);
			if (adjacencyIt == adjacency.end()) {
				continue;
			}
			for (const LogicalNodeId next: adjacencyIt->second) {
				if (distances.contains(next)) {
					continue;
				}
				distances.emplace(next, distances.at(current) + 1U);
				pending.push(next);
			}
		}
		return distances;
	}

	[[nodiscard]] bool
	isDirectedEdge(const DirectedAdjacency &adjacency, const LogicalNodeId source, const LogicalNodeId target) {
		const auto adjacencyIt = adjacency.find(source);
		return adjacencyIt != adjacency.end() &&
				std::ranges::find(adjacencyIt->second, target) != adjacencyIt->second.end();
	}

} // namespace

// ============================================================================
// Algorithms & Topology
// ============================================================================

TEST_F(CppApiTest, AlgorithmShortestPath) {
	int64_t a = db->createNode("A", {{"val", (int64_t) 1}});
	int64_t b = db->createNode("B", {{"val", (int64_t) 2}});
	int64_t c = db->createNode("C", {{"val", (int64_t) 3}});
	static_cast<void>(db->createEdge(a, b, "NEXT"));
	static_cast<void>(db->createEdge(b, c, "NEXT"));
	db->save();

	auto path = db->getShortestPath(a, c);
	ASSERT_EQ(path.size(), 3u);
	EXPECT_EQ(path[0].id, a);
	EXPECT_EQ(path[2].id, c);

	auto emptyPath = db->getShortestPath(c, a);
	EXPECT_TRUE(emptyPath.empty());
}

TEST_F(CppApiTest, PublicTraversalsReadEdgeTopologyWithoutDecodingUnrelatedProperties) {
	const auto a = db->createNode("A");
	const auto b = db->createNode("B");
	const auto c = db->createNode("C");
	const std::string payload(16U * 1024U, 'x');
	static_cast<void>(db->createEdge(a, b, "NEXT", {{"payload", payload}}));
	static_cast<void>(db->createEdge(a, b, "PARALLEL", {{"payload", payload}}));
	static_cast<void>(db->createEdge(b, b, "LOOP", {{"payload", payload}}));
	static_cast<void>(db->createEdge(b, c, "NEXT", {{"payload", payload}}));
	db->save();
	const bool traceWasEnabled = graph::debug::PerfTrace::isEnabled();
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	std::vector<int64_t> visited;
	db->bfs(a, [&](const zyx::Node &node) {
		visited.push_back(node.id);
		return true;
	});
	const auto path = db->getShortestPath(a, c);
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	graph::debug::PerfTrace::setEnabled(traceWasEnabled);
	EXPECT_EQ(visited, (std::vector<int64_t>{a, b, c}));
	ASSERT_EQ(path.size(), 3U);
	EXPECT_EQ(path[0].id, a);
	EXPECT_EQ(path[1].id, b);
	EXPECT_EQ(path[2].id, c);
	EXPECT_EQ(path[1].labels, (std::vector<std::string>{"B"}));
	ASSERT_TRUE(trace.contains("native_graph.read.adjacency_topology_scan_calls"));
	EXPECT_GT(trace.at("native_graph.read.adjacency_topology_scan_calls").totalValue, 0);
	EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
}

TEST_F(CppApiTest, ShortestPathDefersNodePropertiesAndScattersSortedPathBatches) {
	const std::string payload(16U * 1024U, 'n');
	const auto b = db->createNode("Middle", {{"payload", payload}, {"name", "B"}});
	const auto c = db->createNode("End", {{"payload", payload}, {"name", "C"}});
	const auto a = db->createNode("Start", {{"payload", payload}, {"name", "A"}});
	const auto dead = db->createNode("Dead", {{"payload", payload}});
	static_cast<void>(db->createEdge(a, dead, "FIRST"));
	static_cast<void>(db->createEdge(a, b, "NEXT"));
	static_cast<void>(db->createEdge(b, c, "NEXT"));
	db->save();
	const bool traceWasEnabled = graph::debug::PerfTrace::isEnabled();
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	const auto path = db->getShortestPath(a, c);
	const auto pathTrace = graph::debug::PerfTrace::snapshotAndReset();
	const auto missing = db->getShortestPath(c, a);
	const auto missingTrace = graph::debug::PerfTrace::snapshotAndReset();
	const auto depthLimited = db->getShortestPath(a, c, 0);
	const auto depthTrace = graph::debug::PerfTrace::snapshotAndReset();
	const auto same = db->getShortestPath(b, b);
	static_cast<void>(graph::debug::PerfTrace::snapshotAndReset());
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	graph::debug::PerfTrace::setEnabled(traceWasEnabled);
	ASSERT_EQ(path.size(), 3U);
	EXPECT_EQ(path[0].id, a);
	EXPECT_EQ(path[1].id, b);
	EXPECT_EQ(path[2].id, c);
	EXPECT_EQ(path[0].label, "Start");
	EXPECT_EQ(path[1].label, "Middle");
	EXPECT_EQ(path[2].label, "End");
	for (const auto &node: path) EXPECT_EQ(std::get<std::string>(node.properties.at("payload")), payload);
	ASSERT_TRUE(pathTrace.contains("native_graph.read.property_full_decode"));
	EXPECT_EQ(pathTrace.at("native_graph.read.property_full_decode").totalValue, 3);
	ASSERT_TRUE(pathTrace.contains("native_graph.read.node_sorted_batch_entries"));
	// This counter includes only fixed records absent from the snapshot session;
	// search has already cached some path identities before complete batching.
	EXPECT_GT(pathTrace.at("native_graph.read.node_sorted_batch_entries").totalValue, 0);
	EXPECT_LE(pathTrace.at("native_graph.read.node_sorted_batch_entries").totalValue, 3);
	EXPECT_TRUE(missing.empty());
	EXPECT_TRUE(depthLimited.empty());
	EXPECT_FALSE(missingTrace.contains("native_graph.read.property_full_decode"));
	EXPECT_FALSE(depthTrace.contains("native_graph.read.property_full_decode"));
	ASSERT_EQ(same.size(), 1U);
	EXPECT_EQ(same.front().id, b);
	EXPECT_EQ(same.front().label, "Middle");
	EXPECT_EQ(std::get<std::string>(same.front().properties.at("payload")), payload);
}

TEST_F(CppApiTest, ShortestPathPreservesDescendingLogicalOrderAcrossMaterializationBatches) {
	constexpr size_t count = 513U;
	std::vector<zyx::Value> ordinals;
	ordinals.reserve(count);
	for (size_t row = 0U; row < count; ++row) ordinals.emplace_back(int64_t(row));
	auto transaction = db->beginBulkTransaction();
	const auto ids = transaction.createNodesColumnar("Path", count, {{"ordinal", ordinals}});
	ASSERT_EQ(ids.size(), count);
	std::vector<int64_t> sources;
	std::vector<int64_t> targets;
	for (size_t row = count - 1U; row > 0U; --row) {
		sources.push_back(ids[row]);
		targets.push_back(ids[row - 1U]);
	}
	ASSERT_EQ(transaction.createEdgesColumnar("NEXT", sources, targets).size(), count - 1U);
	transaction.commit();
	const auto path = db->getShortestPath(ids.back(), ids.front(), int(count));
	ASSERT_EQ(path.size(), count);
	for (size_t row = 0U; row < count; ++row) {
		EXPECT_EQ(path[row].id, ids[count - 1U - row]);
		EXPECT_EQ(path[row].label, "Path");
		EXPECT_EQ(std::get<int64_t>(path[row].properties.at("ordinal")), int64_t(count - 1U - row));
	}
}

TEST_F(CppApiTest, AlgorithmBFS) {
	int64_t root = db->createNode("Root");
	int64_t child = db->createNode("Child");
	static_cast<void>(db->createEdge(root, child, "LINK"));
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

	static_cast<void>(db->createEdge(a, b, "NEXT"));
	static_cast<void>(db->createEdge(b, c, "NEXT"));
	static_cast<void>(db->createEdge(c, d, "NEXT"));
	db->save();

	auto path = db->getShortestPath(a, d, 2);
	EXPECT_LT(path.size(), 4u);
}

TEST_F(CppApiTest, BFSEarlyTermination) {
	int64_t root = db->createNode("Root");
	int64_t child1 = db->createNode("Child1");
	int64_t child2 = db->createNode("Child2");

	static_cast<void>(db->createEdge(root, child1, "LINK"));
	static_cast<void>(db->createEdge(root, child2, "LINK"));
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
	static_cast<void>(db->createEdge(a, b, "LINK"));
	static_cast<void>(db->createEdge(b, c, "LINK"));
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
	static_cast<void>(db->createEdge(a, b, "LINK"));
	static_cast<void>(db->createEdge(b, c, "LINK"));
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

TEST_F(CppApiTest, PublicAlgorithmsMatchIndependentDirectedBreadthFirstOracleAcrossFixedRandomGraphs) {
	// These graphs include cycles, parallel paths, and an unreachable Node. The
	// oracle is deliberately independent of DatabaseImpl's traversal helpers.
	constexpr size_t graphCount = 2U;
	constexpr size_t reachableNodeCount = 8U;
	std::mt19937 random(0x5A17U);

	for (size_t graphNumber = 0U; graphNumber < graphCount; ++graphNumber) {
		auto transaction = db->beginBulkTransaction();
		std::vector<LogicalNodeId> nodes;
		nodes.reserve(reachableNodeCount + 1U);
		for (size_t node = 0U; node < reachableNodeCount + 1U; ++node) {
			nodes.push_back(transaction.createNode("AlgorithmOracle" + std::to_string(graphNumber)));
		}

		DirectedAdjacency adjacency;
		for (size_t node = 0U; node + 1U < reachableNodeCount; ++node) {
			static_cast<void>(transaction.createEdge(nodes[node], nodes[node + 1U], "ORACLE_LINK"));
			adjacency[nodes[node]].push_back(nodes[node + 1U]);
		}
		for (size_t source = 0U; source < reachableNodeCount; ++source) {
			for (size_t target = 0U; target < reachableNodeCount; ++target) {
				if (source == target || (random() % 5U) != 0U) {
					continue;
				}
				static_cast<void>(transaction.createEdge(nodes[source], nodes[target], "ORACLE_LINK"));
				adjacency[nodes[source]].push_back(nodes[target]);
			}
		}
		transaction.commit();

		const LogicalNodeId source = nodes.front();
		const auto distances = calculateDirectedHopDistances(adjacency, source);
		ASSERT_EQ(distances.size(), reachableNodeCount);

		std::vector<LogicalNodeId> bfsOrder;
		db->bfs(source, [&](const zyx::Node &node) {
			bfsOrder.push_back(node.id);
			return true;
		});
		ASSERT_EQ(bfsOrder.size(), distances.size());
		std::unordered_set<LogicalNodeId> uniqueVisited(bfsOrder.begin(), bfsOrder.end());
		EXPECT_EQ(uniqueVisited.size(), bfsOrder.size());
		EXPECT_EQ(uniqueVisited.size(), distances.size());
		size_t previousDistance = 0U;
		for (const LogicalNodeId nodeId: bfsOrder) {
			const auto distance = distances.find(nodeId);
			ASSERT_NE(distance, distances.end());
			EXPECT_GE(distance->second, previousDistance);
			previousDistance = distance->second;
		}
		EXPECT_FALSE(uniqueVisited.contains(nodes.back()));

		for (size_t targetIndex = 1U; targetIndex < reachableNodeCount; ++targetIndex) {
			const LogicalNodeId target = nodes[targetIndex];
			const size_t expectedHops = distances.at(target);
			const auto path = db->getShortestPath(source, target, static_cast<int>(expectedHops));
			ASSERT_EQ(path.size(), expectedHops + 1U);
			EXPECT_EQ(path.front().id, source);
			EXPECT_EQ(path.back().id, target);
			for (size_t pathIndex = 1U; pathIndex < path.size(); ++pathIndex) {
				EXPECT_TRUE(isDirectedEdge(adjacency, path[pathIndex - 1U].id, path[pathIndex].id));
			}
			if (expectedHops > 0U) {
				EXPECT_TRUE(db->getShortestPath(source, target, static_cast<int>(expectedHops - 1U)).empty());
			}
		}

		EXPECT_TRUE(db->getShortestPath(source, nodes.back(), static_cast<int>(reachableNodeCount)).empty());
	}
}

TEST_F(CppApiTest, PublicTransactionAlgorithmsReadTheStagedDirectedSnapshotAndRollbackAtomically) {
	auto transaction = db->beginTransaction();
	std::vector<LogicalNodeId> nodes;
	nodes.reserve(7U);
	for (size_t node = 0U; node < 7U; ++node) {
		nodes.push_back(transaction.createNode("TransactionAlgorithmOracle"));
	}

	const std::vector<std::pair<size_t, size_t>> edgeIndexes = {
			{0U, 1U}, {1U, 2U}, {2U, 3U}, {3U, 4U}, {4U, 5U}, {5U, 6U},
			{0U, 3U}, {3U, 6U}, {2U, 0U}, {4U, 2U},
	};
	DirectedAdjacency adjacency;
	for (const auto &[sourceIndex, targetIndex]: edgeIndexes) {
		static_cast<void>(transaction.createEdge(nodes[sourceIndex], nodes[targetIndex], "TXN_ORACLE_LINK"));
		adjacency[nodes[sourceIndex]].push_back(nodes[targetIndex]);
	}

	const auto distances = calculateDirectedHopDistances(adjacency, nodes.front());
	ASSERT_EQ(distances.size(), nodes.size());
	std::vector<LogicalNodeId> bfsOrder;
	transaction.bfs(nodes.front(), [&](const zyx::Node &node) {
		bfsOrder.push_back(node.id);
		return true;
	});
	ASSERT_EQ(bfsOrder.size(), distances.size());
	std::unordered_set<LogicalNodeId> uniqueVisited(bfsOrder.begin(), bfsOrder.end());
	EXPECT_EQ(uniqueVisited.size(), bfsOrder.size());
	size_t previousDistance = 0U;
	for (const LogicalNodeId nodeId: bfsOrder) {
		const auto distance = distances.find(nodeId);
		ASSERT_NE(distance, distances.end());
		EXPECT_GE(distance->second, previousDistance);
		previousDistance = distance->second;
	}

	const LogicalNodeId target = nodes.back();
	const size_t expectedHops = distances.at(target);
	const auto path = transaction.getShortestPath(nodes.front(), target, static_cast<int>(expectedHops));
	ASSERT_EQ(path.size(), expectedHops + 1U);
	EXPECT_EQ(path.front().id, nodes.front());
	EXPECT_EQ(path.back().id, target);
	for (size_t pathIndex = 1U; pathIndex < path.size(); ++pathIndex) {
		EXPECT_TRUE(isDirectedEdge(adjacency, path[pathIndex - 1U].id, path[pathIndex].id));
	}
	EXPECT_TRUE(transaction.getShortestPath(nodes.front(), target, static_cast<int>(expectedHops - 1U)).empty());

	transaction.rollback();
	EXPECT_TRUE(db->getShortestPath(nodes.front(), target).empty());
}
