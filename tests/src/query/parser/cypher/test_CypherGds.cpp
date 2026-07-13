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

	static bool getBool(const graph::query::ResultValue &rv) {
		return std::get<bool>(rv.asPrimitive().getVariant());
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

TEST_F(CypherGdsTest, ProjectionCatalogExistsListSchemaAndDropAll) {
	createSocialGraph();

	auto missing = execute("CALL gds.graph.exists('catalog')");
	ASSERT_EQ(missing.rowCount(), 1UL);
	EXPECT_FALSE(getBool(missing.getRows()[0].at("exists")));

	(void) execute(
		"CALL gds.graph.project('catalog', {"
		"nodeLabels: ['Person'], "
		"relationships: {KNOWS: {weight: 1.5}}, "
		"orientation: 'UNDIRECTED'"
		"})");

	auto exists = execute("CALL gds.graph.exists('catalog')");
	ASSERT_EQ(exists.rowCount(), 1UL);
	EXPECT_TRUE(getBool(exists.getRows()[0].at("exists")));

	auto listOne = execute("CALL gds.graph.list('catalog')");
	ASSERT_EQ(listOne.rowCount(), 1UL);
	const auto &catalogRow = listOne.getRows()[0];
	EXPECT_EQ(catalogRow.at("name").toString(), "catalog");
	EXPECT_EQ(getInt(catalogRow.at("nodeCount")), 4);
	EXPECT_EQ(getInt(catalogRow.at("edgeCount")), 8);
	EXPECT_TRUE(getBool(catalogRow.at("isWeighted")));
	EXPECT_FALSE(getBool(catalogRow.at("stale")));
	EXPECT_GT(getInt(catalogRow.at("memoryBytes")), 0);
	EXPECT_EQ(catalogRow.at("nodeLabels").asPrimitive().getList().size(), 1UL);
	ASSERT_EQ(catalogRow.at("relationships").asPrimitive().getList().size(), 1UL);

	auto schema = execute("CALL gds.graph.schema('catalog')");
	ASSERT_EQ(schema.rowCount(), 1UL);
	const auto relationships = schema.getRows()[0].at("relationships").asPrimitive().getList();
	ASSERT_EQ(relationships.size(), 1UL);
	const auto &relationshipMap = relationships[0].getMap();
	EXPECT_EQ(relationshipMap.at("type").toString(), "KNOWS");
	EXPECT_EQ(relationshipMap.at("orientation").toString(), "UNDIRECTED");
	EXPECT_EQ(relationshipMap.at("weightKind").toString(), "CONSTANT");

	auto listAll = execute("CALL gds.graph.list()");
	ASSERT_EQ(listAll.rowCount(), 1UL);

	auto dropAll = execute("CALL gds.graph.dropAll()");
	ASSERT_EQ(dropAll.rowCount(), 1UL);
	EXPECT_EQ(getInt(dropAll.getRows()[0].at("droppedCount")), 1);
	EXPECT_FALSE(getBool(execute("CALL gds.graph.exists('catalog')").getRows()[0].at("exists")));
	EXPECT_EQ(execute("CALL gds.graph.list('catalog')").rowCount(), 0UL);
}

TEST_F(CypherGdsTest, ProjectionCatalogReportsStaleAfterGraphMutation) {
	createSocialGraph();
	(void) execute("CALL gds.graph.project('stale_graph', 'Person', 'KNOWS')");

	auto fresh = execute("CALL gds.graph.list('stale_graph')");
	ASSERT_EQ(fresh.rowCount(), 1UL);
	EXPECT_FALSE(getBool(fresh.getRows()[0].at("stale")));

	(void) execute("CREATE (:Person {name: 'Erin'})");
	auto stale = execute("CALL gds.graph.list('stale_graph')");
	ASSERT_EQ(stale.rowCount(), 1UL);
	EXPECT_TRUE(getBool(stale.getRows()[0].at("stale")));
	EXPECT_LT(getInt(stale.getRows()[0].at("sourceRevision")),
			  getInt(stale.getRows()[0].at("currentRevision")));

	(void) execute("CALL gds.graph.drop('stale_graph')");
}

TEST_F(CypherGdsTest, ProjectionCatalogRejectsInvalidIntrospectionArguments) {
	EXPECT_THROW(execute("CALL gds.graph.exists()"), std::exception);
	EXPECT_THROW(execute("CALL gds.graph.list('a', 'b')"), std::exception);
	EXPECT_THROW(execute("CALL gds.graph.dropAll('x')"), std::exception);
	EXPECT_THROW(execute("CALL gds.graph.schema()"), std::exception);
	EXPECT_THROW(execute("CALL gds.graph.schema('missing')"), std::exception);
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

TEST_F(CypherGdsTest, ProjectWithConfigMapSupportsMultiLabelsTypesWeightsAndOrientation) {
	(void) execute("CREATE (a:Function {name: 'A'})-[:CALLS]->(b:Method {name: 'B'})");
	(void) execute("MATCH (b:Method {name: 'B'}) CREATE (b)-[:IMPORTS]->(c:Class {name: 'C'})");
	(void) execute("MATCH (c:Class {name: 'C'}), (a:Function {name: 'A'}) CREATE (c)-[:DECLARES]->(a)");
	(void) execute("CREATE (:ExternalSymbol {name: 'X'})-[:CALLS]->(:Package {name: 'P'})");

	auto project = execute(
		"CALL gds.graph.project('code_analysis', {"
		"nodeLabels: ['Function', 'Method', 'Class'], "
		"relationships: {"
		"CALLS: {weight: 8.0}, "
		"IMPORTS: {weight: 2.0}, "
		"DECLARES: {weight: 4.0}"
		"}, "
		"orientation: 'UNDIRECTED'"
		"})");
	ASSERT_EQ(project.rowCount(), 1UL);
	EXPECT_EQ(getInt(project.getRows()[0].at("nodeCount")), 3);
	EXPECT_EQ(getInt(project.getRows()[0].at("edgeCount")), 6);

	int64_t idA = execute("MATCH (n:Function {name: 'A'}) RETURN n").getRows()[0].at("n").asNode().getId();
	int64_t idB = execute("MATCH (n:Method {name: 'B'}) RETURN n").getRows()[0].at("n").asNode().getId();
	auto path = execute("CALL gds.shortestPath.dijkstra.stream('code_analysis', " +
						std::to_string(idB) + ", " + std::to_string(idA) + ")");
	ASSERT_EQ(path.rowCount(), 3UL);
	EXPECT_DOUBLE_EQ(getDouble(path.getRows()[0].at("totalCost")), 6.0);

	auto wcc = execute("CALL gds.wcc.stream('code_analysis')");
	EXPECT_EQ(wcc.rowCount(), 3UL);

	(void) execute("CALL gds.graph.drop('code_analysis')");
}

TEST_F(CypherGdsTest, ProjectWithNeo4jStyleNativeSignature) {
	(void) execute("CREATE (a:Function {name: 'A'})-[:CALLS {score: 5.0}]->(b:Method {name: 'B'})");
	(void) execute("MATCH (b:Method {name: 'B'}) CREATE (b)-[:IMPORTS]->(c:Class {name: 'C'})");

	auto project = execute(
		"CALL gds.graph.project("
		"'native_code', "
		"['Function', 'Method', 'Class'], "
		"{CALLS: {}, IMPORTS: {weight: 2.0}}, "
		"{orientation: 'UNDIRECTED', relationshipWeightProperty: 'score', defaultWeight: 1.0}"
		")");
	ASSERT_EQ(project.rowCount(), 1UL);
	EXPECT_EQ(getInt(project.getRows()[0].at("nodeCount")), 3);
	EXPECT_EQ(getInt(project.getRows()[0].at("edgeCount")), 4);

	int64_t idA = execute("MATCH (n:Function {name: 'A'}) RETURN n").getRows()[0].at("n").asNode().getId();
	int64_t idC = execute("MATCH (n:Class {name: 'C'}) RETURN n").getRows()[0].at("n").asNode().getId();
	auto path = execute("CALL gds.shortestPath.dijkstra.stream('native_code', " +
						std::to_string(idA) + ", " + std::to_string(idC) + ")");
	ASSERT_EQ(path.rowCount(), 3UL);
	EXPECT_DOUBLE_EQ(getDouble(path.getRows()[0].at("totalCost")), 7.0);

	(void) execute("CALL gds.graph.drop('native_code')");
}

TEST_F(CypherGdsTest, ProjectWithConfigMapRejectsInvalidOptions) {
	EXPECT_THROW(
		execute("CALL gds.graph.project('bad', {nodeLabels: ['Person'], relationships: {KNOWS: {badOption: 1}}})"),
		std::exception);
	EXPECT_THROW(
		execute("CALL gds.graph.project('bad2', {nodeLabels: ['Person'], relationships: {KNOWS: {orientation: 'SIDEWAYS'}}})"),
		std::exception);
}

// ============================================================================
// gds.leiden.stream
// ============================================================================

TEST_F(CypherGdsTest, LeidenStreamEndToEnd) {
	// Two triangles bridged by one edge → 2 communities.
	(void) execute("CREATE (a:Person {name: 'A'})-[:KNOWS]->(b:Person {name: 'B'})");
	(void) execute("MATCH (b:Person {name: 'B'}) CREATE (b)-[:KNOWS]->(c:Person {name: 'C'})");
	(void) execute("MATCH (a:Person {name: 'A'}), (c:Person {name: 'C'}) CREATE (a)-[:KNOWS]->(c)");
	(void) execute("CREATE (d:Person {name: 'D'})-[:KNOWS]->(e:Person {name: 'E'})");
	(void) execute("MATCH (e:Person {name: 'E'}) CREATE (e)-[:KNOWS]->(f:Person {name: 'F'})");
	(void) execute("MATCH (d:Person {name: 'D'}), (f:Person {name: 'F'}) CREATE (d)-[:KNOWS]->(f)");
	(void) execute("MATCH (a:Person {name: 'A'}), (d:Person {name: 'D'}) CREATE (a)-[:KNOWS]->(d)");

	(void) execute("CALL gds.graph.project('lc', 'Person', 'KNOWS')");
	auto res = execute("CALL gds.leiden.stream('lc')");
	ASSERT_GE(res.rowCount(), 6UL);

	std::set<int64_t> communities;
	for (const auto &row : res.getRows()) communities.insert(getInt(row.at("communityId")));
	EXPECT_EQ(communities.size(), 2UL);

	(void) execute("CALL gds.graph.drop('lc')");
}

TEST_F(CypherGdsTest, LeidenStreamWithOptionsMap) {
	// Same topology as LeidenStreamEndToEnd, but using the documented map API.
	(void) execute("CREATE (a:Person {name: 'A'})-[:KNOWS]->(b:Person {name: 'B'})");
	(void) execute("MATCH (b:Person {name: 'B'}) CREATE (b)-[:KNOWS]->(c:Person {name: 'C'})");
	(void) execute("MATCH (a:Person {name: 'A'}), (c:Person {name: 'C'}) CREATE (a)-[:KNOWS]->(c)");
	(void) execute("CREATE (d:Person {name: 'D'})-[:KNOWS]->(e:Person {name: 'E'})");
	(void) execute("MATCH (e:Person {name: 'E'}) CREATE (e)-[:KNOWS]->(f:Person {name: 'F'})");
	(void) execute("MATCH (d:Person {name: 'D'}), (f:Person {name: 'F'}) CREATE (d)-[:KNOWS]->(f)");
	(void) execute("MATCH (a:Person {name: 'A'}), (d:Person {name: 'D'}) CREATE (a)-[:KNOWS]->(d)");

	(void) execute("CALL gds.graph.project('lc_opts', 'Person', 'KNOWS')");
	auto res = execute("CALL gds.leiden.stream('lc_opts', {maxIterations: 12, maxLevels: 4, resolution: 1.0, refinementThreshold: 0.01, concurrency: 1})");
	ASSERT_GE(res.rowCount(), 6UL);

	std::set<int64_t> communities;
	for (const auto &row : res.getRows()) communities.insert(getInt(row.at("communityId")));
	EXPECT_EQ(communities.size(), 2UL);

	(void) execute("CALL gds.graph.drop('lc_opts')");
}

TEST_F(CypherGdsTest, LeidenStreamRejectsInvalidOptions) {
	(void) execute("CREATE (:Person {name: 'A'})-[:KNOWS]->(:Person {name: 'B'})");
	(void) execute("CALL gds.graph.project('lc_invalid', 'Person', 'KNOWS')");
	EXPECT_THROW(execute("CALL gds.leiden.stream('lc_invalid', {maxIterations: 0})"), std::exception);
	EXPECT_THROW(execute("CALL gds.leiden.stream('lc_invalid', {maxIterations: 1.5})"), std::exception);
	EXPECT_THROW(execute("CALL gds.leiden.stream('lc_invalid', {resolution: 0.0})"), std::exception);
	EXPECT_THROW(execute("CALL gds.leiden.stream('lc_invalid', {concurrency: -1})"), std::exception);
	(void) execute("CALL gds.graph.drop('lc_invalid')");
}

TEST_F(CypherGdsTest, LeidenStreamTooFewArgsThrows) {
	EXPECT_THROW(execute("CALL gds.leiden.stream()"), std::exception);
}
