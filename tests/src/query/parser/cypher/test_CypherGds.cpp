/**
 * @file test_CypherGds.cpp
 * @author Nexepic
 * @date 2026/4/9
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

#include "QueryTestFixture.hpp"

class CypherGdsTest : public QueryTestFixture {
protected:
	void createSocialGraph() {
		(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");
		(void) execute("MATCH (b:Person {name: 'Bob'}) CREATE (b)-[:KNOWS]->(c:Person {name: 'Carol'})");
		(void) execute("MATCH (a:Person {name: 'Alice'}), (c:Person {name: 'Carol'}) CREATE (a)-[:KNOWS]->(c)");
		(void) execute("MATCH (c:Person {name: 'Carol'}) CREATE (c)-[:KNOWS]->(d:Person {name: 'Dave'})");
	}

	static int64_t getInt(const graph::query::ResultValue &rv) {
		return std::get<int64_t>(rv.asPrimitive().getVariant());
	}

	static double getDouble(const graph::query::ResultValue &rv) {
		return std::get<double>(rv.asPrimitive().getVariant());
	}
};

// ============================================================================
// gds.graph.project / gds.graph.drop
// ============================================================================

TEST_F(CypherGdsTest, ProjectAndDropGraph) {
	createSocialGraph();

	auto res = execute("CALL gds.graph.project('social', 'Person', 'KNOWS')");
	ASSERT_EQ(res.rowCount(), 1UL);

	auto row = res.getRows()[0];
	EXPECT_EQ(row.at("name").toString(), "social");
	EXPECT_GE(getInt(row.at("nodeCount")), 4);
	EXPECT_GE(getInt(row.at("edgeCount")), 4);

	// Drop
	auto dropRes = execute("CALL gds.graph.drop('social')");
	ASSERT_EQ(dropRes.rowCount(), 1UL);
}

TEST_F(CypherGdsTest, ProjectDuplicateNameThrows) {
	createSocialGraph();
	(void) execute("CALL gds.graph.project('dup', 'Person', 'KNOWS')");
	EXPECT_THROW(execute("CALL gds.graph.project('dup', 'Person', 'KNOWS')"), std::exception);
	(void) execute("CALL gds.graph.drop('dup')");
}

TEST_F(CypherGdsTest, DropNonExistentThrows) {
	EXPECT_THROW(execute("CALL gds.graph.drop('missing')"), std::exception);
}

// ============================================================================
// gds.pageRank.stream
// ============================================================================

TEST_F(CypherGdsTest, PageRankStream) {
	createSocialGraph();
	(void) execute("CALL gds.graph.project('social', 'Person', 'KNOWS')");

	auto res = execute("CALL gds.pageRank.stream('social')");
	ASSERT_EQ(res.rowCount(), 4UL);

	for (const auto &row : res.getRows()) {
		EXPECT_GT(getInt(row.at("nodeId")), 0);
		EXPECT_GT(getDouble(row.at("score")), 0.0);
	}

	(void) execute("CALL gds.graph.drop('social')");
}

TEST_F(CypherGdsTest, PageRankStreamWithParams) {
	createSocialGraph();
	(void) execute("CALL gds.graph.project('social', 'Person', 'KNOWS')");

	auto res = execute("CALL gds.pageRank.stream('social', 10, 0.85)");
	ASSERT_EQ(res.rowCount(), 4UL);

	(void) execute("CALL gds.graph.drop('social')");
}

// ============================================================================
// gds.wcc.stream
// ============================================================================

TEST_F(CypherGdsTest, WccStream) {
	createSocialGraph();
	(void) execute("CREATE (:Person {name: 'Eve'})");
	(void) execute("CALL gds.graph.project('social', 'Person', 'KNOWS')");

	auto res = execute("CALL gds.wcc.stream('social')");
	ASSERT_EQ(res.rowCount(), 5UL);

	std::unordered_set<int64_t> components;
	for (const auto &row : res.getRows()) {
		components.insert(getInt(row.at("componentId")));
	}
	EXPECT_GE(components.size(), 2u);

	(void) execute("CALL gds.graph.drop('social')");
}

// ============================================================================
// gds.betweenness.stream
// ============================================================================

TEST_F(CypherGdsTest, BetweennessStream) {
	createSocialGraph();
	(void) execute("CALL gds.graph.project('social', 'Person', 'KNOWS')");

	auto res = execute("CALL gds.betweenness.stream('social')");
	ASSERT_EQ(res.rowCount(), 4UL);

	for (const auto &row : res.getRows()) {
		EXPECT_GE(getDouble(row.at("score")), 0.0);
	}

	(void) execute("CALL gds.graph.drop('social')");
}

TEST_F(CypherGdsTest, BetweennessStreamWithSampling) {
	createSocialGraph();
	(void) execute("CALL gds.graph.project('social', 'Person', 'KNOWS')");

	auto res = execute("CALL gds.betweenness.stream('social', 2)");
	ASSERT_EQ(res.rowCount(), 4UL);

	(void) execute("CALL gds.graph.drop('social')");
}

// ============================================================================
// gds.closeness.stream
// ============================================================================

TEST_F(CypherGdsTest, ClosenessStream) {
	createSocialGraph();
	(void) execute("CALL gds.graph.project('social', 'Person', 'KNOWS')");

	auto res = execute("CALL gds.closeness.stream('social')");
	ASSERT_EQ(res.rowCount(), 4UL);

	for (const auto &row : res.getRows()) {
		EXPECT_GE(getDouble(row.at("score")), 0.0);
	}

	(void) execute("CALL gds.graph.drop('social')");
}

// ============================================================================
// gds.shortestPath.dijkstra.stream
// ============================================================================

TEST_F(CypherGdsTest, DijkstraStream) {
	(void) execute("CREATE (a:City {name: 'A'})-[:ROAD]->(b:City {name: 'B'})");
	(void) execute("MATCH (b:City {name: 'B'}) CREATE (b)-[:ROAD]->(c:City {name: 'C'})");

	(void) execute("CALL gds.graph.project('roads', 'City', 'ROAD')");

	int64_t idA = execute("MATCH (n:City {name: 'A'}) RETURN n").getRows()[0].at("n").asNode().getId();
	int64_t idC = execute("MATCH (n:City {name: 'C'}) RETURN n").getRows()[0].at("n").asNode().getId();

	auto res = execute("CALL gds.shortestPath.dijkstra.stream('roads', " +
					   std::to_string(idA) + ", " + std::to_string(idC) + ")");
	ASSERT_EQ(res.rowCount(), 3UL); // A -> B -> C

	(void) execute("CALL gds.graph.drop('roads')");
}

TEST_F(CypherGdsTest, DijkstraStreamNoPath) {
	(void) execute("CREATE (:City {name: 'X'})");
	(void) execute("CREATE (:City {name: 'Y'})");

	(void) execute("CALL gds.graph.project('cities', 'City', 'ROAD')");

	int64_t idX = execute("MATCH (n:City {name: 'X'}) RETURN n").getRows()[0].at("n").asNode().getId();
	int64_t idY = execute("MATCH (n:City {name: 'Y'}) RETURN n").getRows()[0].at("n").asNode().getId();

	auto res = execute("CALL gds.shortestPath.dijkstra.stream('cities', " +
					   std::to_string(idX) + ", " + std::to_string(idY) + ")");
	EXPECT_TRUE(res.isEmpty());

	(void) execute("CALL gds.graph.drop('cities')");
}

// ============================================================================
// Error Handling
// ============================================================================

TEST_F(CypherGdsTest, AlgorithmOnMissingProjectionThrows) {
	EXPECT_THROW(execute("CALL gds.pageRank.stream('nonexistent')"), std::exception);
}

// ============================================================================
// Empty Projection (covers empty-result branches in operators)
// ============================================================================

TEST_F(CypherGdsTest, PageRankOnEmptyProjection) {
	// Project with a label that has no nodes
	auto res = execute("CALL gds.graph.project('empty', 'NoSuchLabel', 'NoSuchEdge')");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(getInt(res.getRows()[0].at("nodeCount")), 0);

	auto prRes = execute("CALL gds.pageRank.stream('empty')");
	EXPECT_TRUE(prRes.isEmpty());

	(void) execute("CALL gds.graph.drop('empty')");
}

TEST_F(CypherGdsTest, WccOnEmptyProjection) {
	(void) execute("CALL gds.graph.project('empty2', 'NoSuchLabel', 'NoSuchEdge')");

	auto res = execute("CALL gds.wcc.stream('empty2')");
	EXPECT_TRUE(res.isEmpty());

	(void) execute("CALL gds.graph.drop('empty2')");
}

TEST_F(CypherGdsTest, BetweennessOnEmptyProjection) {
	(void) execute("CALL gds.graph.project('empty3', 'NoSuchLabel', 'NoSuchEdge')");

	auto res = execute("CALL gds.betweenness.stream('empty3')");
	EXPECT_TRUE(res.isEmpty());

	(void) execute("CALL gds.graph.drop('empty3')");
}

TEST_F(CypherGdsTest, ClosenessOnEmptyProjection) {
	(void) execute("CALL gds.graph.project('empty4', 'NoSuchLabel', 'NoSuchEdge')");

	auto res = execute("CALL gds.closeness.stream('empty4')");
	EXPECT_TRUE(res.isEmpty());

	(void) execute("CALL gds.graph.drop('empty4')");
}

// ============================================================================
// Argument Validation (covers error-throwing branches in ProcedureRegistry)
// ============================================================================

TEST_F(CypherGdsTest, ProjectWithTooFewArgsThrows) {
	EXPECT_THROW(execute("CALL gds.graph.project('onlyname', 'Person')"), std::exception);
}

TEST_F(CypherGdsTest, ProjectWithWeightProperty) {
	(void) execute("CREATE (a:WCity {name: 'X'})-[:WROAD {dist: 5.0}]->(b:WCity {name: 'Y'})");
	auto res = execute("CALL gds.graph.project('weighted', 'WCity', 'WROAD', 'dist')");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_GE(getInt(res.getRows()[0].at("nodeCount")), 2);

	(void) execute("CALL gds.graph.drop('weighted')");
}
