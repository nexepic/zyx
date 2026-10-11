/**
 * @file test_LogicalGdsProcedures.cpp
 * @brief Native Cypher integration tests for the process-local GDS catalog.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "QueryTestFixture.hpp"
#include "graph/debug/PerfTrace.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

class LogicalGdsProceduresTest : public QueryTestFixture {};

namespace {

	[[nodiscard]] int64_t integerValue(const graph::query::ResultValue &value) {
		return std::get<int64_t>(value.asPrimitive().getVariant());
	}

	[[nodiscard]] double doubleValue(const graph::query::ResultValue &value) {
		return std::get<double>(value.asPrimitive().getVariant());
	}

	[[nodiscard]] uint64_t nodeId(const graph::query::QueryResult &result, const std::string &column) {
		return result.getRows().at(0U).at(column).asLogicalNode().node_id;
	}

} // namespace

TEST_F(LogicalGdsProceduresTest, ProjectsListsDescribesAndDropsAGraphFromOnePinnedLogicalSnapshot) {
	(void) execute("CREATE (:Code {name: 'A'})-[:CALLS {cost: 2.0}]->(:Code {name: 'B'})");
	(void) execute("CREATE (:Other {name: 'ignored'})");

	const auto projected =
			execute("CALL gds.graph.project('code', {nodeLabels: ['Code'], relationships: {CALLS: {weight: 'cost'}}})");
	ASSERT_EQ(projected.getColumns(), (std::vector<std::string>{"name", "nodeCount", "edgeCount"}));
	ASSERT_EQ(projected.rowCount(), 1U);
	EXPECT_EQ(projected.getRows().front().at("name").toString(), "code");
	EXPECT_EQ(projected.getRows().front().at("nodeCount").toString(), "2");
	EXPECT_EQ(projected.getRows().front().at("edgeCount").toString(), "1");

	const auto listed = execute("CALL gds.graph.list('code')");
	ASSERT_EQ(listed.getColumns(),
			  (std::vector<std::string>{"name", "nodeCount", "edgeCount", "isWeighted", "createdAtMillis",
										"buildMillis", "memoryBytes", "hasCsr", "csrMemoryBytes", "sourceRevision",
										"currentRevision", "stale", "nodeLabels", "relationships"}));
	ASSERT_EQ(listed.rowCount(), 1U);
	EXPECT_EQ(listed.getRows().front().at("isWeighted").toString(), "true");
	EXPECT_GT(integerValue(listed.getRows().front().at("createdAtMillis")), 0);
	EXPECT_GE(integerValue(listed.getRows().front().at("buildMillis")), 0);
	EXPECT_EQ(listed.getRows().front().at("hasCsr").toString(), "false");
	EXPECT_EQ(integerValue(listed.getRows().front().at("csrMemoryBytes")), 0);
	EXPECT_EQ(listed.getRows().front().at("stale").toString(), "false");
	const int64_t sourceRevision = integerValue(listed.getRows().front().at("sourceRevision"));
	EXPECT_EQ(sourceRevision, integerValue(listed.getRows().front().at("currentRevision")));
	EXPECT_GT(sourceRevision, 0);
	EXPECT_EQ(listed.getRows().front().at("nodeLabels").toString(), "[Code]");

	// Catalog inspection must not eagerly construct the derived CSR cache; an
	// algorithm materializes it once and the descriptor then reports that fact.
	ASSERT_EQ(execute("CALL gds.pageRank.stream('code')").rowCount(), 2U);
	const auto listedAfterAlgorithm = execute("CALL gds.graph.list('code')");
	ASSERT_EQ(listedAfterAlgorithm.rowCount(), 1U);
	EXPECT_EQ(listedAfterAlgorithm.getRows().front().at("hasCsr").toString(), "true");
	EXPECT_GT(integerValue(listedAfterAlgorithm.getRows().front().at("csrMemoryBytes")), 0);

	const auto schema = execute("CALL gds.graph.schema('code')");
	ASSERT_EQ(schema.rowCount(), 1U);
	EXPECT_EQ(schema.getRows().front().at("relationships").asPrimitive().getType(), graph::PropertyType::LIST);
	const auto schemaRelationships = schema.getRows().front().at("relationships").asPrimitive().getList();
	ASSERT_EQ(schemaRelationships.size(), 1U);
	const auto &propertyRelationship = schemaRelationships.front().getMap();
	EXPECT_EQ(propertyRelationship.size(), 6U);
	EXPECT_EQ(propertyRelationship.at("type"), graph::PropertyValue("CALLS"));
	EXPECT_EQ(propertyRelationship.at("orientation"), graph::PropertyValue("NATURAL"));
	EXPECT_EQ(propertyRelationship.at("weightKind"), graph::PropertyValue("PROPERTY"));
	EXPECT_EQ(propertyRelationship.at("weightProperty"), graph::PropertyValue("cost"));
	EXPECT_EQ(propertyRelationship.at("constantWeight"), graph::PropertyValue(1.0));
	EXPECT_EQ(propertyRelationship.at("defaultWeight"), graph::PropertyValue(1.0));

	(void) execute("CREATE (:Code {name: 'C'})");
	const auto stale = execute("CALL gds.graph.list('code')");
	ASSERT_EQ(stale.rowCount(), 1U);
	EXPECT_EQ(stale.getRows().front().at("stale").toString(), "true");
	EXPECT_EQ(integerValue(stale.getRows().front().at("sourceRevision")), sourceRevision);
	EXPECT_GT(integerValue(stale.getRows().front().at("currentRevision")), sourceRevision);

	const auto exists = execute("CALL gds.graph.exists('code')");
	ASSERT_EQ(exists.rowCount(), 1U);
	EXPECT_EQ(exists.getRows().front().at("exists").toString(), "true");
	const auto dropped = execute("CALL gds.graph.drop('code')");
	ASSERT_EQ(dropped.rowCount(), 1U);
	EXPECT_EQ(dropped.getRows().front().at("name").toString(), "code");
	EXPECT_EQ(execute("CALL gds.graph.exists('code')").getRows().front().at("exists").toString(), "false");
	EXPECT_TRUE(execute("CALL gds.graph.list('code')").isEmpty());
	EXPECT_THROW(static_cast<void>(execute("CALL gds.graph.schema('code')")), std::out_of_range);
}

TEST_F(LogicalGdsProceduresTest, ListsProjectionDescriptorsInStableNameOrderWithoutMaterializingCsr) {
	(void) execute("CALL gds.graph.project('zeta', {})");
	(void) execute("CALL gds.graph.project('alpha', {})");
	(void) execute("CALL gds.graph.project('middle', {})");

	const auto listed = execute("CALL gds.graph.list()");
	ASSERT_EQ(listed.getColumns(),
			  (std::vector<std::string>{"name", "nodeCount", "edgeCount", "isWeighted", "createdAtMillis",
										"buildMillis", "memoryBytes", "hasCsr", "csrMemoryBytes", "sourceRevision",
										"currentRevision", "stale", "nodeLabels", "relationships"}));
	ASSERT_EQ(listed.rowCount(), 3U);
	EXPECT_EQ(listed.getRows()[0U].at("name").asPrimitive(), graph::PropertyValue("alpha"));
	EXPECT_EQ(listed.getRows()[1U].at("name").asPrimitive(), graph::PropertyValue("middle"));
	EXPECT_EQ(listed.getRows()[2U].at("name").asPrimitive(), graph::PropertyValue("zeta"));
	for (const auto &row: listed.getRows()) {
		EXPECT_EQ(row.at("hasCsr").asPrimitive(), graph::PropertyValue(false));
		EXPECT_EQ(row.at("csrMemoryBytes").asPrimitive(), graph::PropertyValue(int64_t{0}));
	}

	const auto filtered = execute("CALL gds.graph.list('middle')");
	ASSERT_EQ(filtered.rowCount(), 1U);
	EXPECT_EQ(filtered.getRows().front().at("name").asPrimitive(), graph::PropertyValue("middle"));
	EXPECT_TRUE(execute("CALL gds.graph.list('missing')").isEmpty());
}

TEST_F(LogicalGdsProceduresTest, PreservesCatalogProcedureResultColumnOrderAndLogicalValueTypes) {
	const auto primitiveType = [](const graph::query::ResultValue &value) { return value.asPrimitive().getType(); };
	const auto expectRowTypes = [&](const graph::query::QueryResult &result,
									const std::vector<graph::PropertyType> &expectedTypes) {
		ASSERT_EQ(result.rowCount(), 1U);
		ASSERT_EQ(result.getRows().front().size(), expectedTypes.size());
		for (size_t index = 0U; index < expectedTypes.size(); ++index) {
			EXPECT_EQ(primitiveType(result.getRows().front().at(result.getColumns()[index])), expectedTypes[index]);
		}
	};

	const auto projected = execute("CALL gds.graph.project('catalog_contract', {})");
	ASSERT_EQ(projected.getColumns(), (std::vector<std::string>{"name", "nodeCount", "edgeCount"}));
	expectRowTypes(projected, {graph::PropertyType::STRING, graph::PropertyType::INTEGER,
						   graph::PropertyType::INTEGER});

	const auto exists = execute("CALL gds.graph.exists('catalog_contract')");
	ASSERT_EQ(exists.getColumns(), (std::vector<std::string>{"name", "exists"}));
	expectRowTypes(exists, {graph::PropertyType::STRING, graph::PropertyType::BOOLEAN});

	const auto listed = execute("CALL gds.graph.list('catalog_contract')");
	ASSERT_EQ(listed.getColumns(),
			  (std::vector<std::string>{"name", "nodeCount", "edgeCount", "isWeighted", "createdAtMillis",
										"buildMillis", "memoryBytes", "hasCsr", "csrMemoryBytes", "sourceRevision",
										"currentRevision", "stale", "nodeLabels", "relationships"}));
	expectRowTypes(listed, {graph::PropertyType::STRING, graph::PropertyType::INTEGER, graph::PropertyType::INTEGER,
							graph::PropertyType::BOOLEAN, graph::PropertyType::INTEGER, graph::PropertyType::INTEGER,
							graph::PropertyType::INTEGER, graph::PropertyType::BOOLEAN, graph::PropertyType::INTEGER,
							graph::PropertyType::INTEGER, graph::PropertyType::INTEGER, graph::PropertyType::BOOLEAN,
							graph::PropertyType::LIST, graph::PropertyType::LIST});

	const auto schema = execute("CALL gds.graph.schema('catalog_contract')");
	ASSERT_EQ(schema.getColumns(), (std::vector<std::string>{"name", "nodeLabels", "relationships", "stale"}));
	expectRowTypes(schema, {graph::PropertyType::STRING, graph::PropertyType::LIST, graph::PropertyType::LIST,
						graph::PropertyType::BOOLEAN});

	const auto dropped = execute("CALL gds.graph.drop('catalog_contract')");
	ASSERT_EQ(dropped.getColumns(), (std::vector<std::string>{"name"}));
	expectRowTypes(dropped, {graph::PropertyType::STRING});

	(void) execute("CALL gds.graph.project('catalog_contract', {})");
	const auto droppedAll = execute("CALL gds.graph.dropAll()");
	ASSERT_EQ(droppedAll.getColumns(), (std::vector<std::string>{"droppedCount"}));
	expectRowTypes(droppedAll, {graph::PropertyType::INTEGER});
}

TEST_F(LogicalGdsProceduresTest, PreservesAlgorithmProcedureResultColumnOrderAndLogicalValueTypes) {
	(void) execute("CREATE (:AlgorithmContract)-[:LINK {cost: 1.0}]->(:AlgorithmContract)");
	const auto nodes = execute("MATCH (n:AlgorithmContract) RETURN n ORDER BY id(n)");
	ASSERT_EQ(nodes.rowCount(), 2U);
	const uint64_t source = nodes.getRows()[0U].at("n").asLogicalNode().node_id;
	const uint64_t target = nodes.getRows()[1U].at("n").asLogicalNode().node_id;
	(void) execute("CALL gds.graph.project('algorithm_contract', {nodeLabels: ['AlgorithmContract'], "
				   "relationships: {LINK: {weight: 'cost'}}})");

	const auto expectPrimitiveRowTypes = [](const graph::query::QueryResult &result,
													 const std::vector<graph::PropertyType> &expectedTypes) {
		ASSERT_FALSE(result.isEmpty());
		ASSERT_EQ(result.getRows().front().size(), expectedTypes.size());
		for (size_t index = 0U; index < expectedTypes.size(); ++index) {
			EXPECT_EQ(result.getRows().front().at(result.getColumns()[index]).asPrimitive().getType(), expectedTypes[index]);
		}
	};

	const auto pageRank = execute("CALL gds.pageRank.stream('algorithm_contract')");
	ASSERT_EQ(pageRank.getColumns(), (std::vector<std::string>{"nodeId", "score"}));
	expectPrimitiveRowTypes(pageRank, {graph::PropertyType::INTEGER, graph::PropertyType::DOUBLE});

	const auto wcc = execute("CALL gds.wcc.stream('algorithm_contract')");
	ASSERT_EQ(wcc.getColumns(), (std::vector<std::string>{"nodeId", "componentId"}));
	expectPrimitiveRowTypes(wcc, {graph::PropertyType::INTEGER, graph::PropertyType::INTEGER});

	const auto betweenness = execute("CALL gds.betweenness.stream('algorithm_contract')");
	ASSERT_EQ(betweenness.getColumns(), (std::vector<std::string>{"nodeId", "score"}));
	expectPrimitiveRowTypes(betweenness, {graph::PropertyType::INTEGER, graph::PropertyType::DOUBLE});

	const auto closeness = execute("CALL gds.closeness.stream('algorithm_contract')");
	ASSERT_EQ(closeness.getColumns(), (std::vector<std::string>{"nodeId", "score"}));
	expectPrimitiveRowTypes(closeness, {graph::PropertyType::INTEGER, graph::PropertyType::DOUBLE});

	const auto leiden = execute("CALL gds.leiden.stream('algorithm_contract')");
	ASSERT_EQ(leiden.getColumns(), (std::vector<std::string>{"nodeId", "communityId"}));
	expectPrimitiveRowTypes(leiden, {graph::PropertyType::INTEGER, graph::PropertyType::INTEGER});

	const auto dijkstra = execute("CALL gds.shortestPath.dijkstra.stream('algorithm_contract', " +
								  std::to_string(source) + ", " + std::to_string(target) + ")");
	ASSERT_EQ(dijkstra.getColumns(), (std::vector<std::string>{"nodeId", "node", "cost", "totalCost"}));
	ASSERT_FALSE(dijkstra.isEmpty());
	const auto &firstPathRow = dijkstra.getRows().front();
	EXPECT_EQ(firstPathRow.at("nodeId").asPrimitive().getType(), graph::PropertyType::INTEGER);
	EXPECT_TRUE(firstPathRow.at("node").isLogicalNode());
	EXPECT_EQ(firstPathRow.at("cost").asPrimitive().getType(), graph::PropertyType::DOUBLE);
	EXPECT_EQ(firstPathRow.at("totalCost").asPrimitive().getType(), graph::PropertyType::DOUBLE);
}

TEST_F(LogicalGdsProceduresTest, DescribesConstantWeightProjectionWithLegacyWeightKindFields) {
	(void) execute("CREATE (:WeightedCode)-[:CALLS]->(:WeightedCode)");
	(void) execute("CALL gds.graph.project('weighted_code', {nodeLabels: ['WeightedCode'], "
				   "relationships: {CALLS: {weight: 2.5}}, orientation: 'UNDIRECTED'})");

	const auto schema = execute("CALL gds.graph.schema('weighted_code')");
	ASSERT_EQ(schema.getColumns(), (std::vector<std::string>{"name", "nodeLabels", "relationships", "stale"}));
	ASSERT_EQ(schema.rowCount(), 1U);
	EXPECT_EQ(schema.getRows().front().at("name").asPrimitive(), graph::PropertyValue("weighted_code"));
	EXPECT_EQ(schema.getRows().front().at("nodeLabels").asPrimitive().getList(),
			  (std::vector<graph::PropertyValue>{graph::PropertyValue("WeightedCode")}));
	EXPECT_EQ(schema.getRows().front().at("stale").asPrimitive(), graph::PropertyValue(false));
	const auto relationships = schema.getRows().front().at("relationships").asPrimitive().getList();
	ASSERT_EQ(relationships.size(), 1U);
	const auto &relationship = relationships.front().getMap();
	EXPECT_EQ(relationship.size(), 6U);
	EXPECT_EQ(relationship.at("type"), graph::PropertyValue("CALLS"));
	EXPECT_EQ(relationship.at("orientation"), graph::PropertyValue("UNDIRECTED"));
	EXPECT_EQ(relationship.at("weightKind"), graph::PropertyValue("CONSTANT"));
	EXPECT_EQ(relationship.at("weightProperty"), graph::PropertyValue(""));
	EXPECT_EQ(relationship.at("constantWeight"), graph::PropertyValue(2.5));
	EXPECT_EQ(relationship.at("defaultWeight"), graph::PropertyValue(1.0));

	(void) execute("CREATE (:WeightedCode)");
	const auto staleSchema = execute("CALL gds.graph.schema('weighted_code')");
	ASSERT_EQ(staleSchema.rowCount(), 1U);
	EXPECT_EQ(staleSchema.getRows().front().at("stale").asPrimitive(), graph::PropertyValue(true));
	EXPECT_EQ(staleSchema.getRows().front().at("relationships").asPrimitive(),
			  schema.getRows().front().at("relationships").asPrimitive());
}

TEST_F(LogicalGdsProceduresTest, DescribesAndExecutesReversePropertyWeightedProjection) {
	(void) execute("CREATE (fallback:ReverseCode {name: 'fallback'})-[:CALLS]->"
				  "(source:ReverseCode {name: 'source'})-[:CALLS {cost: 3.25}]->"
				  "(target:ReverseCode {name: 'target'})");
	// Address the endpoints by their public properties; the contract must not
	// depend on the physical scan or logical-ID ordering of the two Nodes.
	const uint64_t source = nodeId(execute("MATCH (n:ReverseCode {name: 'source'}) RETURN n"), "n");
	const uint64_t target = nodeId(execute("MATCH (n:ReverseCode {name: 'target'}) RETURN n"), "n");
	const uint64_t fallback = nodeId(execute("MATCH (n:ReverseCode {name: 'fallback'}) RETURN n"), "n");

	const auto projected = execute(
			"CALL gds.graph.project('reverse_property', {nodeLabels: ['ReverseCode'], "
			"relationships: {CALLS: {orientation: 'REVERSE', weight: 'cost', defaultWeight: 0.75}}})");
	ASSERT_EQ(projected.rowCount(), 1U);
	EXPECT_EQ(projected.getRows().front().at("edgeCount").asPrimitive(), graph::PropertyValue(int64_t{2}));

	const auto schema = execute("CALL gds.graph.schema('reverse_property')");
	ASSERT_EQ(schema.rowCount(), 1U);
	const auto relationships = schema.getRows().front().at("relationships").asPrimitive().getList();
	ASSERT_EQ(relationships.size(), 1U);
	const auto &relationship = relationships.front().getMap();
	EXPECT_EQ(relationship.at("orientation"), graph::PropertyValue("REVERSE"));
	EXPECT_EQ(relationship.at("weightKind"), graph::PropertyValue("PROPERTY"));
	EXPECT_EQ(relationship.at("weightProperty"), graph::PropertyValue("cost"));
	EXPECT_EQ(relationship.at("constantWeight"), graph::PropertyValue(1.0));
	EXPECT_EQ(relationship.at("defaultWeight"), graph::PropertyValue(0.75));

	const auto reversePath = execute("CALL gds.shortestPath.dijkstra.stream('reverse_property', " +
									 std::to_string(target) + ", " + std::to_string(fallback) + ")");
	ASSERT_EQ(reversePath.rowCount(), 3U);
	EXPECT_DOUBLE_EQ(doubleValue(reversePath.getRows().back().at("totalCost")), 4.0);
	EXPECT_TRUE(execute("CALL gds.shortestPath.dijkstra.stream('reverse_property', " + std::to_string(source) +
							 " , " + std::to_string(target) + ")")
					 .isEmpty());
}

TEST_F(LogicalGdsProceduresTest, ValidatesCatalogProcedureArgumentsAndSupportsBoundedDropAll) {
	EXPECT_THROW(static_cast<void>(execute("CALL gds.graph.project('', {})")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.graph.project('bad', {unknown: true})")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.graph.project('bad_relation', {nodeLabels: ['Code'], "
										   "relationships: {CALLS: {unknown: 1}}})")),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.graph.project('bad_orientation', {nodeLabels: ['Code'], "
										   "relationships: {CALLS: {orientation: 'SIDEWAYS'}}})")),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.graph.dropAll('unexpected')")), std::invalid_argument);

	(void) execute("CALL gds.graph.project('first', {})");
	(void) execute("CALL gds.graph.project('second', {})");
	const auto dropped = execute("CALL gds.graph.dropAll()");
	ASSERT_EQ(dropped.rowCount(), 1U);
	EXPECT_EQ(dropped.getRows().front().at("droppedCount").toString(), "2");
	EXPECT_TRUE(execute("CALL gds.graph.list()").isEmpty());
}

TEST_F(LogicalGdsProceduresTest, PreservesLegacyCatalogErrorsAndProjectionSignatureVariants) {
	EXPECT_THROW(static_cast<void>(execute("CALL gds.graph.project('only-name', 'Code')")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.graph.exists()")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.graph.list('first', 'second')")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.graph.schema()")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.graph.schema('missing')")), std::out_of_range);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.graph.drop('missing')")), std::out_of_range);

	(void) execute("CREATE (a:Function)-[:CALLS {score: 5.0}]->(b:Method)");
	(void) execute("MATCH (b:Method) CREATE (b)-[:IMPORTS]->(:Class)");
	const uint64_t a = nodeId(execute("MATCH (n:Function) RETURN n"), "n");
	const uint64_t c = nodeId(execute("MATCH (n:Class) RETURN n"), "n");
	const auto projected =
			execute("CALL gds.graph.project('native_signature', ['Function', 'Method', 'Class'], "
					"{CALLS: {}, IMPORTS: {weight: 2.0}}, "
					"{orientation: 'UNDIRECTED', relationshipWeightProperty: 'score', defaultWeight: 1.0})");
	ASSERT_EQ(projected.rowCount(), 1U);
	EXPECT_EQ(projected.getRows().front().at("nodeCount").toString(), "3");
	EXPECT_EQ(projected.getRows().front().at("edgeCount").toString(), "4");

	const auto path = execute("CALL gds.shortestPath.dijkstra.stream('native_signature', " + std::to_string(a) + ", " +
							  std::to_string(c) + ")");
	ASSERT_EQ(path.rowCount(), 3U);
	EXPECT_DOUBLE_EQ(doubleValue(path.getRows().back().at("totalCost")), 7.0);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.graph.project('native_signature', {})")), std::invalid_argument);
}

TEST_F(LogicalGdsProceduresTest, ReturnsEmptyStreamsForAnEmptyProjection) {
	const auto projected =
			execute("CALL gds.graph.project('empty', {nodeLabels: ['AbsentLabel'], relationships: {ABSENT: {}}})");
	ASSERT_EQ(projected.rowCount(), 1U);
	EXPECT_EQ(projected.getRows().front().at("nodeCount").toString(), "0");
	EXPECT_TRUE(execute("CALL gds.pageRank.stream('empty')").isEmpty());
	EXPECT_TRUE(execute("CALL gds.wcc.stream('empty')").isEmpty());
	EXPECT_TRUE(execute("CALL gds.betweenness.stream('empty')").isEmpty());
	EXPECT_TRUE(execute("CALL gds.closeness.stream('empty')").isEmpty());
}

TEST_F(LogicalGdsProceduresTest, ProjectsMultipleLabelsRelationshipTypesAndConstantWeightsFromAConfigMap) {
	(void) execute("CREATE (a:Function)-[:CALLS]->(b:Method)");
	(void) execute("MATCH (b:Method) CREATE (b)-[:IMPORTS]->(c:Class)");
	(void) execute("MATCH (c:Class), (a:Function) CREATE (c)-[:DECLARES]->(a)");
	(void) execute("CREATE (:ExternalSymbol)-[:CALLS]->(:Package)");
	const uint64_t a = nodeId(execute("MATCH (n:Function) RETURN n"), "n");
	const uint64_t b = nodeId(execute("MATCH (n:Method) RETURN n"), "n");

	const auto projected =
			execute("CALL gds.graph.project('code_analysis', {nodeLabels: ['Function', 'Method', 'Class'], "
					"relationships: {CALLS: {weight: 8.0}, IMPORTS: {weight: 2.0}, DECLARES: {weight: 4.0}}, "
					"orientation: 'UNDIRECTED'})");
	ASSERT_EQ(projected.rowCount(), 1U);
	EXPECT_EQ(projected.getRows().front().at("nodeCount").toString(), "3");
	EXPECT_EQ(projected.getRows().front().at("edgeCount").toString(), "6");

	const auto path = execute("CALL gds.shortestPath.dijkstra.stream('code_analysis', " + std::to_string(b) + ", " +
							  std::to_string(a) + ")");
	ASSERT_EQ(path.rowCount(), 3U);
	EXPECT_DOUBLE_EQ(doubleValue(path.getRows().back().at("totalCost")), 6.0);
	const auto components = execute("CALL gds.wcc.stream('code_analysis')");
	ASSERT_EQ(components.rowCount(), 3U);
}

TEST_F(LogicalGdsProceduresTest, KeepsProjectionCatalogsIsolatedBetweenDatabaseInstances) {
	(void) execute("CALL gds.graph.project('local', {})");
	ASSERT_EQ(execute("CALL gds.graph.list()").rowCount(), 1U);

	const std::filesystem::path otherPath = testFilePath.string() + ".independent";
	graph::Database other(otherPath.string());
	other.open();
	const auto otherList = other.getQueryEngine()->execute("CALL gds.graph.list()");
	EXPECT_TRUE(otherList.isEmpty());
	(void) other.getQueryEngine()->execute("CALL gds.graph.project('local', {})");
	EXPECT_EQ(other.getQueryEngine()->execute("CALL gds.graph.list()").rowCount(), 1U);
	EXPECT_EQ(execute("CALL gds.graph.list()").rowCount(), 1U);
	other.close();
	std::error_code error;
	std::filesystem::remove(otherPath, error);
	EXPECT_FALSE(error);
}

TEST_F(LogicalGdsProceduresTest, KeepsProjectionCatalogProcessLocalAcrossDatabaseReopen) {
	(void) execute("CREATE (:Code)-[:CALLS]->(:Code)");
	(void) execute("CALL gds.graph.project('ephemeral', 'Code', 'CALLS')");
	ASSERT_EQ(execute("CALL gds.graph.list()").rowCount(), 1U);

	db->close();
	db->open();
	EXPECT_TRUE(execute("CALL gds.graph.list()").isEmpty());
	const auto projectedAfterReopen = execute("CALL gds.graph.project('fresh', 'Code', 'CALLS')");
	ASSERT_EQ(projectedAfterReopen.rowCount(), 1U);
	EXPECT_EQ(projectedAfterReopen.getRows().front().at("nodeCount").toString(), "2");
}

TEST_F(LogicalGdsProceduresTest, RunsNativePageRankWccAndWeightedDijkstraWithStableResultContracts) {
	(void) execute("CREATE (a:City {name: 'A'})-[:ROAD {cost: 1.5}]->(b:City {name: 'B'})");
	(void) execute("MATCH (b:City {name: 'B'}) CREATE (b)-[:ROAD {cost: 2.25}]->(:City {name: 'C'})");
	(void) execute("CREATE (:City {name: 'D'})");

	const uint64_t a = nodeId(execute("MATCH (n:City {name: 'A'}) RETURN n"), "n");
	const uint64_t b = nodeId(execute("MATCH (n:City {name: 'B'}) RETURN n"), "n");
	const uint64_t c = nodeId(execute("MATCH (n:City {name: 'C'}) RETURN n"), "n");
	const uint64_t d = nodeId(execute("MATCH (n:City {name: 'D'}) RETURN n"), "n");
	(void) execute("CALL gds.graph.project('cities', {nodeLabels: ['City'], relationships: {ROAD: {weight: 'cost'}}})");

	const auto pageRank = execute("CALL gds.pageRank.stream('cities', 20, 0.85)");
	ASSERT_EQ(pageRank.getColumns(), (std::vector<std::string>{"nodeId", "score"}));
	ASSERT_EQ(pageRank.rowCount(), 4U);
	double scoreSum = 0.0;
	double previousScore = std::numeric_limits<double>::infinity();
	for (const auto &row: pageRank.getRows()) {
		const double score = doubleValue(row.at("score"));
		EXPECT_GT(integerValue(row.at("nodeId")), 0);
		EXPECT_LE(score, previousScore);
		EXPECT_TRUE(std::isfinite(score));
		scoreSum += score;
		previousScore = score;
	}
	EXPECT_NEAR(scoreSum, 1.0, 1.0e-12);

	const auto betweenness = execute("CALL gds.betweenness.stream('cities')");
	ASSERT_EQ(betweenness.getColumns(), (std::vector<std::string>{"nodeId", "score"}));
	ASSERT_EQ(betweenness.rowCount(), 4U);
	const auto middleBetweenness = std::ranges::find_if(betweenness.getRows(), [b](const auto &row) {
		return static_cast<uint64_t>(integerValue(row.at("nodeId"))) == b;
	});
	ASSERT_NE(middleBetweenness, betweenness.getRows().end());
	EXPECT_DOUBLE_EQ(doubleValue(middleBetweenness->at("score")), 1.0);
	EXPECT_NO_THROW(static_cast<void>(execute("CALL gds.betweenness.stream('cities', 2)")));

	const auto closeness = execute("CALL gds.closeness.stream('cities')");
	ASSERT_EQ(closeness.getColumns(), (std::vector<std::string>{"nodeId", "score"}));
	ASSERT_EQ(closeness.rowCount(), 4U);
	const auto middleCloseness = std::ranges::find_if(closeness.getRows(), [b](const auto &row) {
		return static_cast<uint64_t>(integerValue(row.at("nodeId"))) == b;
	});
	ASSERT_NE(middleCloseness, closeness.getRows().end());
	EXPECT_DOUBLE_EQ(doubleValue(middleCloseness->at("score")), 1.0);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.betweenness.stream('cities', -1)")), std::invalid_argument);

	const auto wcc = execute(
			"CALL gds.wcc.stream('cities') YIELD nodeId, componentId RETURN nodeId, componentId ORDER BY nodeId");
	ASSERT_EQ(wcc.getColumns(), (std::vector<std::string>{"nodeId", "componentId"}));
	ASSERT_EQ(wcc.rowCount(), 4U);
	std::unordered_map<uint64_t, uint64_t> components;
	for (const auto &row: wcc.getRows()) {
		components.emplace(static_cast<uint64_t>(integerValue(row.at("nodeId"))),
						   static_cast<uint64_t>(integerValue(row.at("componentId"))));
	}
	ASSERT_EQ(components.size(), 4U);
	const uint64_t connectedComponent = std::min({a, b, c});
	EXPECT_EQ(components.at(a), connectedComponent);
	EXPECT_EQ(components.at(b), connectedComponent);
	EXPECT_EQ(components.at(c), connectedComponent);
	EXPECT_EQ(components.at(d), d);

	const auto path = execute("CALL gds.shortestPath.dijkstra.stream('cities', " + std::to_string(a) + ", " +
							  std::to_string(c) + ")");
	ASSERT_EQ(path.getColumns(), (std::vector<std::string>{"nodeId", "node", "cost", "totalCost"}));
	ASSERT_EQ(path.rowCount(), 3U);
	EXPECT_EQ(integerValue(path.getRows()[0U].at("nodeId")), static_cast<int64_t>(a));
	EXPECT_EQ(integerValue(path.getRows()[1U].at("nodeId")), static_cast<int64_t>(b));
	EXPECT_EQ(integerValue(path.getRows()[2U].at("nodeId")), static_cast<int64_t>(c));
	EXPECT_DOUBLE_EQ(doubleValue(path.getRows()[0U].at("cost")), 0.0);
	EXPECT_DOUBLE_EQ(doubleValue(path.getRows()[1U].at("cost")), 1.5);
	EXPECT_DOUBLE_EQ(doubleValue(path.getRows()[2U].at("cost")), 3.75);
	EXPECT_DOUBLE_EQ(doubleValue(path.getRows()[2U].at("totalCost")), 3.75);
	EXPECT_EQ(path.getRows()[1U].at("node").asLogicalNode().node_id, b);
	const auto stringIdPath = execute("CALL gds.shortestPath.dijkstra.stream('cities', '" + std::to_string(a) +
								"', '" + std::to_string(c) + "')");
	ASSERT_EQ(stringIdPath.rowCount(), 3U);
	EXPECT_EQ(integerValue(stringIdPath.getRows().front().at("nodeId")), static_cast<int64_t>(a));
	EXPECT_EQ(integerValue(stringIdPath.getRows().back().at("nodeId")), static_cast<int64_t>(c));
	const auto filteredPath = execute("CALL gds.shortestPath.dijkstra.stream('cities', " + std::to_string(a) +
								 ", " + std::to_string(c) +
								 ") YIELD nodeId, cost, totalCost WHERE cost >= 1.5 "
								 "RETURN nodeId, cost, totalCost ORDER BY cost DESC LIMIT 1");
	ASSERT_EQ(filteredPath.getColumns(), (std::vector<std::string>{"nodeId", "cost", "totalCost"}));
	ASSERT_EQ(filteredPath.rowCount(), 1U);
	EXPECT_EQ(integerValue(filteredPath.getRows().front().at("nodeId")), static_cast<int64_t>(c));
	EXPECT_DOUBLE_EQ(doubleValue(filteredPath.getRows().front().at("cost")), 3.75);
	EXPECT_DOUBLE_EQ(doubleValue(filteredPath.getRows().front().at("totalCost")), 3.75);
	EXPECT_TRUE(execute("CALL gds.shortestPath.dijkstra.stream('cities', " + std::to_string(d) + ", " +
						std::to_string(a) + ")")
						.isEmpty());

	EXPECT_THROW(static_cast<void>(execute("CALL gds.pageRank.stream('cities', 0)")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.pageRank.stream('missing')")), std::out_of_range);
}

TEST_F(LogicalGdsProceduresTest, RunsNativeLeidenWithValidatedOptionsAndStableCommunityIds) {
	(void) execute("CREATE (a:Person {name: 'A'})-[:KNOWS]->(b:Person {name: 'B'})");
	(void) execute("MATCH (b:Person {name: 'B'}) CREATE (b)-[:KNOWS]->(c:Person {name: 'C'})");
	(void) execute("MATCH (c:Person {name: 'C'}), (a:Person {name: 'A'}) CREATE (c)-[:KNOWS]->(a)");
	(void) execute("CREATE (d:Person {name: 'D'})-[:KNOWS]->(e:Person {name: 'E'})");
	(void) execute("MATCH (e:Person {name: 'E'}) CREATE (e)-[:KNOWS]->(f:Person {name: 'F'})");
	(void) execute("MATCH (f:Person {name: 'F'}), (d:Person {name: 'D'}) CREATE (f)-[:KNOWS]->(d)");
	(void) execute("MATCH (a:Person {name: 'A'}), (d:Person {name: 'D'}) CREATE (a)-[:KNOWS]->(d)");
	const uint64_t a = nodeId(execute("MATCH (n:Person {name: 'A'}) RETURN n"), "n");
	const uint64_t b = nodeId(execute("MATCH (n:Person {name: 'B'}) RETURN n"), "n");
	const uint64_t c = nodeId(execute("MATCH (n:Person {name: 'C'}) RETURN n"), "n");
	const uint64_t d = nodeId(execute("MATCH (n:Person {name: 'D'}) RETURN n"), "n");
	const uint64_t e = nodeId(execute("MATCH (n:Person {name: 'E'}) RETURN n"), "n");
	const uint64_t f = nodeId(execute("MATCH (n:Person {name: 'F'}) RETURN n"), "n");
	(void) execute("CALL gds.graph.project('leiden', 'Person', 'KNOWS')");

	const auto result = execute("CALL gds.leiden.stream('leiden', {maxIterations: 8, maxLevels: 4, resolution: 1.0, "
								"refinementThreshold: 0.01, concurrency: 1})");
	ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"nodeId", "communityId"}));
	ASSERT_EQ(result.rowCount(), 6U);
	std::unordered_map<uint64_t, uint64_t> communities;
	for (const auto &row: result.getRows()) {
		communities.emplace(static_cast<uint64_t>(integerValue(row.at("nodeId"))),
							static_cast<uint64_t>(integerValue(row.at("communityId"))));
	}
	ASSERT_EQ(communities.size(), 6U);
	EXPECT_EQ(communities.at(a), communities.at(b));
	EXPECT_EQ(communities.at(b), communities.at(c));
	EXPECT_EQ(communities.at(d), communities.at(e));
	EXPECT_EQ(communities.at(e), communities.at(f));
	EXPECT_NE(communities.at(a), communities.at(d));
	EXPECT_THROW(static_cast<void>(execute("CALL gds.leiden.stream('leiden', {maxIterations: 0})")),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.leiden.stream('leiden', {resolution: 0.0})")),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.leiden.stream('leiden', {maxIterations: 1.5})")),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.leiden.stream('leiden', {concurrency: -1})")),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL gds.leiden.stream('leiden', {unknown: 1})")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(
						 execute("CALL gds.leiden.stream('leiden', {maxIterations: 8, maxLevels: 4, resolution: 1.0, "
								 "refinementThreshold: 0.01, concurrency: 1, unsupported: 1})")),
				 std::invalid_argument);
	const auto serial = execute("CALL gds.leiden.stream('leiden', {maxIterations: 8, concurrency: 1})");
	const auto requestedParallel = execute("CALL gds.leiden.stream('leiden', {maxIterations: 8, concurrency: 4})");
	ASSERT_EQ(serial.rowCount(), requestedParallel.rowCount());
	for (size_t index = 0U; index < serial.rowCount(); ++index) {
		EXPECT_EQ(serial.getRows()[index].at("nodeId").toString(),
				  requestedParallel.getRows()[index].at("nodeId").toString());
		EXPECT_EQ(serial.getRows()[index].at("communityId").toString(),
				  requestedParallel.getRows()[index].at("communityId").toString());
	}
}

TEST_F(LogicalGdsProceduresTest, ScalarPathProcedureOutputsDoNotDecodeNodeProperties) {
	const std::string payload(8192U, 'x');
	(void) execute("CREATE (a:DeferredPath {name: 'A', payload: '" + payload +
		"'})-[:PATH_LINK {cost: 2.0}]->(b:DeferredPath {name: 'B', payload: '" + payload + "'})");
	const uint64_t a = static_cast<uint64_t>(integerValue(execute("MATCH (n:DeferredPath {name: 'A'}) RETURN id(n) AS id").getRows().front().at("id")));
	const uint64_t b = static_cast<uint64_t>(integerValue(execute("MATCH (n:DeferredPath {name: 'B'}) RETURN id(n) AS id").getRows().front().at("id")));
	(void) execute("CALL gds.graph.project('deferred_path', {nodeLabels: ['DeferredPath'], relationships: {PATH_LINK: {weight: 'cost'}}})");
	for (const bool weighted: {false, true}) {
		SCOPED_TRACE(weighted);
		const std::string query = weighted
			? "CALL gds.shortestPath.dijkstra.stream('deferred_path', " + std::to_string(a) + ", " + std::to_string(b) +
			  ") YIELD nodeId, cost, totalCost RETURN nodeId, cost, totalCost"
			: "CALL algo.shortestPath(" + std::to_string(a) + ", " + std::to_string(b) +
			  ") YIELD step, path_length RETURN step, path_length";
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setValueCollectionEnabled(true);
		const auto result = execute(query);
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		ASSERT_EQ(result.rowCount(), 2U);
		if (weighted) {
			EXPECT_EQ(integerValue(result.getRows()[0].at("nodeId")), static_cast<int64_t>(a));
			EXPECT_EQ(integerValue(result.getRows()[1].at("nodeId")), static_cast<int64_t>(b));
			EXPECT_DOUBLE_EQ(doubleValue(result.getRows()[1].at("cost")), 2.0);
		} else {
			EXPECT_EQ(integerValue(result.getRows()[1].at("step")), 1);
			EXPECT_EQ(integerValue(result.getRows()[1].at("path_length")), 2);
		}
		EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
		EXPECT_FALSE(trace.contains("native_graph.read.property_selected_decode"));
	}
	// The cached projection still refers to the removed Node. Scalar projection
	// must authenticate IDs even though it no longer hydrates Node Properties.
	(void) execute("MATCH (n:DeferredPath {name: 'B'}) DETACH DELETE n");
	EXPECT_THROW(static_cast<void>(execute("CALL gds.shortestPath.dijkstra.stream('deferred_path', " +
		std::to_string(a) + ", " + std::to_string(b) + ") YIELD cost RETURN cost")), std::runtime_error);

}
