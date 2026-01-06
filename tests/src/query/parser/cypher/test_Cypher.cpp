/**
 * @file test_Cypher.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/16
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <CLI/CLI.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/storage/FileStorage.hpp"

namespace fs = std::filesystem;

class CypherTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath =
				std::filesystem::temp_directory_path() / ("test_cypher_" + boost::uuids::to_string(uuid) + ".dat");

		if (fs::exists(testFilePath))
			fs::remove_all(testFilePath);

		db = std::make_unique<graph::Database>(testFilePath.string());
		db->open();
	}

	void TearDown() override {
		if (db)
			db->close();
		if (fs::exists(testFilePath))
			fs::remove_all(testFilePath);
	}

	graph::query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }
};

// ============================================================================
// 1. Basic CRUD Tests (Create & Read)
// ============================================================================

TEST_F(CypherTest, CreateAndMatchNode) {
	// 1. Create
	auto res1 = execute("CREATE (n:User {name: 'Alice', age: 30}) RETURN n");
	ASSERT_EQ(res1.rowCount(), 1UL);

	const auto &row1 = res1.getRows()[0];
	ASSERT_TRUE(row1.at("n").isNode());
	const auto &node1 = row1.at("n").asNode();

	EXPECT_EQ(node1.getLabel(), "User");
	EXPECT_EQ(node1.getProperties().at("name").toString(), "Alice");
	EXPECT_EQ(node1.getProperties().at("age").toString(), "30");

	// 2. Match (Scan)
	auto res2 = execute("MATCH (n:User) RETURN n");
	ASSERT_EQ(res2.rowCount(), 1UL);

	const auto &node2 = res2.getRows()[0].at("n").asNode();
	EXPECT_EQ(node2.getLabel(), "User");
}

TEST_F(CypherTest, CreateAndMatchChain) {
	// Test: (a)-[r]->(b)
	(void) execute("CREATE (a:Person {name: 'A'})-[r:KNOWS {since: 2020}]->(b:Person {name: 'B'})");

	auto res = execute("MATCH (n:Person)-[r]->(m) RETURN n, r, m");

	ASSERT_EQ(res.rowCount(), 1UL); // 1 Path found = 1 Row

	const auto &row = res.getRows()[0];
	ASSERT_TRUE(row.at("n").isNode());
	ASSERT_TRUE(row.at("m").isNode());
	ASSERT_TRUE(row.at("r").isEdge());

	const auto &edge = row.at("r").asEdge();
	EXPECT_EQ(edge.getLabel(), "KNOWS");
	EXPECT_EQ(edge.getProperties().at("since").toString(), "2020");
}

// ============================================================================
// 2. Update & Delete Tests (SET & DELETE)
// ============================================================================

TEST_F(CypherTest, UpdateProperties) {
	(void) execute("CREATE (n:UpdateTest {val: 1})");

	// Update property
	auto resSet = execute("MATCH (n:UpdateTest) SET n.val = 2 RETURN n");
	ASSERT_EQ(resSet.rowCount(), 1UL);

	// Immediate check
	const auto &nodeSet = resSet.getRows()[0].at("n").asNode();
	EXPECT_EQ(nodeSet.getProperties().at("val").toString(), "2");

	// Verify persistence
	auto resCheck = execute("MATCH (n:UpdateTest) RETURN n");
	const auto &nodeCheck = resCheck.getRows()[0].at("n").asNode();
	EXPECT_EQ(nodeCheck.getProperties().at("val").toString(), "2");
}

TEST_F(CypherTest, AddNewProperty) {
	(void) execute("CREATE (n:UpdateTest {val: 1})");

	// Add 'tag'
	(void) execute("MATCH (n:UpdateTest) SET n.tag = 'new'");

	auto res = execute("MATCH (n:UpdateTest) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);

	const auto &node = res.getRows()[0].at("n").asNode();
	const auto &props = node.getProperties();

	EXPECT_EQ(props.at("val").toString(), "1");
	EXPECT_EQ(props.at("tag").toString(), "new");
}

TEST_F(CypherTest, DeleteNodes) {
	(void) execute("CREATE (n:DeleteMe)");
	ASSERT_EQ(execute("MATCH (n:DeleteMe) RETURN n").rowCount(), 1UL);

	(void) execute("MATCH (n:DeleteMe) DELETE n");

	auto res = execute("MATCH (n:DeleteMe) RETURN n");
	EXPECT_EQ(res.rowCount(), 0UL);
}

TEST_F(CypherTest, DeleteNodeWithEdgesConstraint) {
	(void) execute("CREATE (a:ConstraintTest)-[:REL]->(b:ConstraintTest)");

	// Should fail (Standard DELETE on connected node)
	EXPECT_THROW({ execute("MATCH (n:ConstraintTest) DELETE n"); }, std::runtime_error);

	auto res = execute("MATCH (n:ConstraintTest) RETURN n");
	EXPECT_EQ(res.rowCount(), 2UL);
}

TEST_F(CypherTest, DetachDeleteNodes) {
	(void) execute("CREATE (a:DetachTest)-[r:REL]->(b:DetachTest)");

	// Should succeed (DETACH DELETE)
	(void) execute("MATCH (n:DetachTest) DETACH DELETE n");

	auto resNodes = execute("MATCH (n:DetachTest) RETURN n");
	EXPECT_EQ(resNodes.rowCount(), 0UL);

	auto resEdges = execute("MATCH ()-[r:REL]->() RETURN r");
	EXPECT_EQ(resEdges.rowCount(), 0UL);
}

TEST_F(CypherTest, DeleteEdgeOnly) {
	(void) execute("CREATE (a:EdgeDel)-[r:TO_BE_DELETED]->(b:EdgeDel)");

	(void) execute("MATCH (a:EdgeDel)-[r:TO_BE_DELETED]->(b) DELETE r");

	// Nodes exist
	auto resNodes = execute("MATCH (n:EdgeDel) RETURN n");
	EXPECT_EQ(resNodes.rowCount(), 2UL);

	// Edge gone
	auto resEdges = execute("MATCH (a:EdgeDel)-[r:TO_BE_DELETED]->(b) RETURN r");
	EXPECT_EQ(resEdges.rowCount(), 0UL);
}

// ============================================================================
// 3. Filtering Tests (WHERE & Inline)
// ============================================================================

TEST_F(CypherTest, FilterByInlineProperty) {
	(void) execute("CREATE (n:User {name: 'Alice'})");
	(void) execute("CREATE (n:User {name: 'Bob'})");

	auto res = execute("MATCH (n:User {name: 'Alice'}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("name").toString(), "Alice");
}

TEST_F(CypherTest, FilterByWhereClause) {
	(void) execute("CREATE (n:Item {price: 100})");
	(void) execute("CREATE (n:Item {price: 200})");
	(void) execute("CREATE (n:Item {price: 50})");

	// =
	auto res1 = execute("MATCH (n:Item) WHERE n.price = 200 RETURN n");
	ASSERT_EQ(res1.rowCount(), 1UL);
	EXPECT_EQ(res1.getRows()[0].at("n").asNode().getProperties().at("price").toString(), "200");

	// <>
	auto res2 = execute("MATCH (n:Item) WHERE n.price <> 100 RETURN n");
	ASSERT_EQ(res2.rowCount(), 2UL);

	// >
	auto res3 = execute("MATCH (n:Item) WHERE n.price > 90 RETURN n");
	ASSERT_EQ(res3.rowCount(), 2UL);
}

TEST_F(CypherTest, FilterTraversalTarget) {
	(void) execute("CREATE (a:Node)-[:LINK]->(b:Target {id: 1})");
	(void) execute("CREATE (a:Node)-[:LINK]->(c:Other {id: 2})");

	auto res = execute("MATCH (n:Node)-[r]->(t:Target) RETURN t");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("t").asNode().getLabel(), "Target");
}

// ============================================================================
// MERGE (Upsert) Tests
// ============================================================================

TEST_F(CypherTest, Merge_CreatesNewNode) {
	(void) execute("MERGE (n:MergeNew {key: 'unique_1'}) ON CREATE SET n.status = 'created'");

	auto res = execute("MATCH (n:MergeNew) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);

	const auto &props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_EQ(props.at("key").toString(), "unique_1");
	EXPECT_EQ(props.at("status").toString(), "created");
}

TEST_F(CypherTest, Merge_MatchesExistingNode) {
	(void) execute("CREATE (n:MergeExist {key: 'unique_2', count: 0})");

	(void) execute("MERGE (n:MergeExist {key: 'unique_2'}) "
				   "ON CREATE SET n.status = 'wrong' "
				   "ON MATCH SET n.count = 1, n.status = 'matched'");

	auto res = execute("MATCH (n:MergeExist) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);

	const auto &props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_EQ(props.at("count").toString(), "1");
	EXPECT_EQ(props.at("status").toString(), "matched");
}

TEST_F(CypherTest, Merge_WithIndexOptimization) {
	(void) execute("CREATE INDEX ON :MergeIdx(uid)");

	(void) execute("MERGE (n:MergeIdx {uid: 100}) ON CREATE SET n.step = 1");
	(void) execute("MERGE (n:MergeIdx {uid: 100}) ON MATCH SET n.step = 2");

	auto res = execute("MATCH (n:MergeIdx) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("step").toString(), "2");
}

// ============================================================================
// Extended Update Tests (REMOVE & SET Label)
// ============================================================================

TEST_F(CypherTest, RemoveProperty) {
	(void) execute("CREATE (n:RemProp {keep: 1, remove_me: 2})");
	(void) execute("MATCH (n:RemProp) REMOVE n.remove_me");

	auto res = execute("MATCH (n:RemProp) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);

	const auto &props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_TRUE(props.contains("keep"));
	EXPECT_FALSE(props.contains("remove_me"));
}

TEST_F(CypherTest, SetLabel) {
	(void) execute("CREATE (n:OldLabel {id: 1})");
	(void) execute("MATCH (n:OldLabel) SET n:NewLabel");

	auto resOld = execute("MATCH (n:OldLabel) RETURN n");
	EXPECT_EQ(resOld.rowCount(), 0UL);

	auto resNew = execute("MATCH (n:NewLabel) RETURN n");
	ASSERT_EQ(resNew.rowCount(), 1UL);
	EXPECT_EQ(resNew.getRows()[0].at("n").asNode().getProperties().at("id").toString(), "1");
}

TEST_F(CypherTest, RemoveLabel) {
	(void) execute("CREATE (n:TagToRemove {id: 99})");
	(void) execute("MATCH (n:TagToRemove) REMOVE n:TagToRemove");

	auto res = execute("MATCH (n:TagToRemove) RETURN n");
	EXPECT_EQ(res.rowCount(), 0UL);

	auto resAll = execute("MATCH (n) WHERE n.id = 99 RETURN n");
	ASSERT_EQ(resAll.rowCount(), 1UL);
	EXPECT_EQ(resAll.getRows()[0].at("n").asNode().getLabel(), "");
}

// ============================================================================
// 4. Indexing Tests (Admin & Optimization)
// ============================================================================

TEST_F(CypherTest, IndexCreationAndPushdown) {
	auto res1 = execute("CREATE INDEX ON :User(id)");
	ASSERT_GE(res1.rowCount(), 1UL);

	(void) execute("CREATE (u1:User {id: 1, name: 'One'})");
	(void) execute("CREATE (u2:User {id: 2, name: 'Two'})");
	(void) execute("CREATE (u3:User {id: 3, name: 'Three'})");

	auto resIndex = execute("SHOW INDEXES");
	ASSERT_GE(resIndex.rowCount(), 1UL);

	auto resQuery = execute("MATCH (n:User {id: 2}) RETURN n");
	ASSERT_EQ(resQuery.rowCount(), 1UL);
	EXPECT_EQ(resQuery.getRows()[0].at("n").asNode().getProperties().at("name").toString(), "Two");
}

TEST_F(CypherTest, DropIndex) {
	(void) execute("CREATE INDEX ON :Tag(name)");
	(void) execute("DROP INDEX ON :Tag(name)");

	auto resShow2 = execute("SHOW INDEXES");
	if (!resShow2.isEmpty() && resShow2.rowCount() > 0) {
		bool found = false;
		for (const auto &row: resShow2.getRows()) {
			if (row.at("properties").toString() == "name")
				found = true;
		}
		EXPECT_FALSE(found);
	}
}

TEST_F(CypherTest, IndexLifecycle_NamedIndex) {
	auto resCreate = execute("CREATE INDEX user_age_idx FOR (n:User) ON (n.age)");
	ASSERT_FALSE(resCreate.isEmpty());

	// Verify Metadata
	auto resShow = execute("SHOW INDEXES");
	bool nameFound = false;
	for (const auto &row: resShow.getRows()) {
		if (row.count("name") && row.at("name").toString() == "user_age_idx") {
			nameFound = true;
		}
	}
	EXPECT_TRUE(nameFound);

	(void) execute("CREATE (u:User {age: 25})");
	(void) execute("DROP INDEX user_age_idx");

	// Verify Removal
	auto resShowAfter = execute("SHOW INDEXES");
	bool stillExists = false;
	if (resShowAfter.rowCount() > 0) {
		for (const auto &row: resShowAfter.getRows()) {
			if (row.count("name") && row.at("name").toString() == "user_age_idx") {
				stillExists = true;
			}
		}
	}
	EXPECT_FALSE(stillExists);
}

TEST_F(CypherTest, IndexLifecycle_CreateAndDropByDefinition) {
	auto resCreate = execute("CREATE INDEX ON :Product(sku)");
	ASSERT_FALSE(resCreate.isEmpty());

	(void) execute("DROP INDEX ON :Product(sku)");

	auto resShow2 = execute("SHOW INDEXES");
	bool found = false;
	if (resShow2.rowCount() > 0) {
		for (const auto &row: resShow2.getRows()) {
			if (row.count("properties") && row.at("properties").toString() == "sku")
				found = true;
		}
	}
	EXPECT_FALSE(found);
}

TEST_F(CypherTest, IndexEffectiveness_ScanPushdown) {
	(void) execute("CREATE INDEX ON :User(uid)");
	(void) execute("CREATE (u:User {uid: 1001, name: 'A'})");
	(void) execute("CREATE (u:User {uid: 1002, name: 'B'})");

	auto res = execute("MATCH (n:User {uid: 1002}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("name").toString(), "B");
}

// ============================================================================
// 5. System Configuration Tests (CALL Procedures)
// ============================================================================

TEST_F(CypherTest, SystemConfigFlow) {
	(void) execute("CALL dbms.setConfig('test.key', 12345)");
	(void) execute("CALL dbms.setConfig('test.mode', 'ACTIVE')");

	auto res1 = execute("CALL dbms.getConfig('test.key')");
	ASSERT_EQ(res1.rowCount(), 1UL);
	EXPECT_EQ(res1.getRows()[0].at("value").toString(), "12345");

	auto res2 = execute("CALL dbms.listConfig()");
	ASSERT_GE(res2.rowCount(), 2UL);
}

// ============================================================================
// 6. Persistence & Recovery Test (Full Cycle)
// ============================================================================

TEST_F(CypherTest, DataPersistenceWithIndexAndConfig) {
	(void) execute("CREATE (n:SaveTest {val: 999})");
	(void) execute("CREATE INDEX ON :SaveTest(val)");
	(void) execute("CALL dbms.setConfig('persistent.cfg', 'true')");

	db->getStorage()->flush();
	db->close();
	db.reset();

	db = std::make_unique<graph::Database>(testFilePath.string());
	db->open();

	// Verify Data
	auto resData = execute("MATCH (n:SaveTest {val: 999}) RETURN n");
	ASSERT_EQ(resData.rowCount(), 1UL);
	EXPECT_EQ(resData.getRows()[0].at("n").asNode().getProperties().at("val").toString(), "999");

	// Verify Index
	auto resIdx = execute("SHOW INDEXES");
	bool indexFound = false;
	for (const auto &row: resIdx.getRows()) {
		if (row.count("properties") && row.at("properties").toString() == "val") {
			indexFound = true;
		}
	}
	EXPECT_TRUE(indexFound);

	// Verify Config
	auto resCfg = execute("CALL dbms.getConfig('persistent.cfg')");
	ASSERT_EQ(resCfg.rowCount(), 1UL);
	EXPECT_EQ(resCfg.getRows()[0].at("value").toString(), "true");
}

// ============================================================================
// 7. Graph Algorithm Tests (CALL algo.*)
// ============================================================================

TEST_F(CypherTest, AlgoShortestPath) {
	(void) execute("CREATE (a:City {name: 'A'})-[r1:ROAD]->(b:City {name: 'B'})");
	(void) execute("MATCH (b:City {name: 'B'}) CREATE (b)-[r2:ROAD]->(c:City {name: 'C'})");

	auto resA = execute("MATCH (n:City {name: 'A'}) RETURN n");
	int64_t idA = resA.getRows()[0].at("n").asNode().getId();

	auto resC = execute("MATCH (n:City {name: 'C'}) RETURN n");
	int64_t idC = resC.getRows()[0].at("n").asNode().getId();

	std::string query = "CALL algo.shortestPath(" + std::to_string(idA) + ", " + std::to_string(idC) + ")";
	auto resPath = execute(query);

	// algo.shortestPath currently returns list of Nodes as rows (compatibility mode)
	// We expect 3 rows (A, B, C)
	ASSERT_EQ(resPath.rowCount(), 3UL);

	// Nodes are in 'node' column (based on procedure impl)
	// Check props
	EXPECT_EQ(resPath.getRows()[0].at("node").asNode().getProperties().at("name").toString(), "A");
	EXPECT_EQ(resPath.getRows()[1].at("node").asNode().getProperties().at("name").toString(), "B");
	EXPECT_EQ(resPath.getRows()[2].at("node").asNode().getProperties().at("name").toString(), "C");
}

TEST_F(CypherTest, AlgoShortestPathNoPath) {
	(void) execute("CREATE (a:City {name: 'A'})");
	(void) execute("CREATE (b:City {name: 'B'})");

	auto resA = execute("MATCH (n:City {name: 'A'}) RETURN n");
	int64_t idA = resA.getRows()[0].at("n").asNode().getId();

	auto resB = execute("MATCH (n:City {name: 'B'}) RETURN n");
	int64_t idB = resB.getRows()[0].at("n").asNode().getId();

	std::string query = "CALL algo.shortestPath(" + std::to_string(idA) + ", " + std::to_string(idB) + ")";
	auto resPath = execute(query);

	EXPECT_TRUE(resPath.isEmpty());
}

// ============================================================================
// Update Consistency Tests (SET)
// ============================================================================

TEST_F(CypherTest, SetProperty_ReadModifyWrite_Consistency) {
	(void) execute("CREATE (n:Config {version: 1, mode: 'dev'})");
	(void) execute("MATCH (n:Config) SET n.version = 2");

	auto res = execute("MATCH (n:Config) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);

	auto props = res.getRows()[0].at("n").asNode().getProperties();
	EXPECT_EQ(props.at("version").toString(), "2");
	EXPECT_EQ(props.at("mode").toString(), "dev");
}

TEST_F(CypherTest, SetNewProperty) {
	(void) execute("CREATE (n:Node)");
	(void) execute("MATCH (n:Node) SET n.new_prop = 'hello'");

	auto res = execute("MATCH (n:Node) RETURN n");
	EXPECT_EQ(res.getRows()[0].at("n").asNode().getProperties().at("new_prop").toString(), "hello");
}

// ============================================================================
// Pagination Tests (LIMIT & SKIP)
// ============================================================================

TEST_F(CypherTest, LimitResults) {
	for (int i = 0; i < 5; ++i)
		(void) execute("CREATE (n:LimitTest {id: " + std::to_string(i) + "})");

	auto res = execute("MATCH (n:LimitTest) RETURN n LIMIT 3");
	ASSERT_EQ(res.rowCount(), 3UL);

	auto resAll = execute("MATCH (n:LimitTest) RETURN n LIMIT 10");
	ASSERT_EQ(resAll.rowCount(), 5UL);

	auto resZero = execute("MATCH (n:LimitTest) RETURN n LIMIT 0");
	ASSERT_EQ(resZero.rowCount(), 0UL);
}

TEST_F(CypherTest, SkipResults) {
	for (int i = 0; i < 5; ++i)
		(void) execute("CREATE (n:SkipTest {id: " + std::to_string(i) + "})");

	auto res = execute("MATCH (n:SkipTest) RETURN n SKIP 2");
	ASSERT_EQ(res.rowCount(), 3UL);

	auto resEmpty = execute("MATCH (n:SkipTest) RETURN n SKIP 5");
	ASSERT_EQ(resEmpty.rowCount(), 0UL);
}

TEST_F(CypherTest, SkipAndLimitCombined) {
	for (int i = 0; i < 10; ++i)
		(void) execute("CREATE (n:PageTest {val: " + std::to_string(i) + "})");

	auto res = execute("MATCH (n:PageTest) RETURN n SKIP 3 LIMIT 4");
	ASSERT_EQ(res.rowCount(), 4UL);

	// Verify values
	bool found3 = false, found6 = false;
	for (const auto &row: res.getRows()) {
		std::string val = row.at("n").asNode().getProperties().at("val").toString();
		if (val == "3")
			found3 = true;
		if (val == "6")
			found6 = true;
		EXPECT_NE(val, "0");
		EXPECT_NE(val, "1");
		EXPECT_NE(val, "2");
	}
	EXPECT_TRUE(found3);
	EXPECT_TRUE(found6);
}

// ============================================================================
// Sorting Tests (ORDER BY)
// ============================================================================

TEST_F(CypherTest, OrderByResults) {
	(void) execute("CREATE (n:SortTest {val: 3})");
	(void) execute("CREATE (n:SortTest {val: 1})");
	(void) execute("CREATE (n:SortTest {val: 2})");

	auto resAsc = execute("MATCH (n:SortTest) RETURN n ORDER BY n.val ASC");
	ASSERT_EQ(resAsc.rowCount(), 3UL);
	EXPECT_EQ(resAsc.getRows()[0].at("n").asNode().getProperties().at("val").toString(), "1");
	EXPECT_EQ(resAsc.getRows()[1].at("n").asNode().getProperties().at("val").toString(), "2");
	EXPECT_EQ(resAsc.getRows()[2].at("n").asNode().getProperties().at("val").toString(), "3");

	auto resDesc = execute("MATCH (n:SortTest) RETURN n ORDER BY n.val DESC");
	EXPECT_EQ(resDesc.getRows()[0].at("n").asNode().getProperties().at("val").toString(), "3");
}

// ============================================================================
// Hops Tests (Variable Length Paths)
// ============================================================================

TEST_F(CypherTest, VarLengthTraversal) {
	(void) execute("CREATE (a:HopNode {name:'A'})-[r1:NEXT]->(b:HopNode {name:'B'})");
	(void) execute("MATCH (b:HopNode {name:'B'}) CREATE (b)-[r2:NEXT]->(c:HopNode {name:'C'})");
	(void) execute("MATCH (c:HopNode {name:'C'}) CREATE (c)-[r3:NEXT]->(d:HopNode {name:'D'})");

	// 1..2 hops
	auto res1 = execute("MATCH (a:HopNode {name:'A'})-[*1..2]->(x) RETURN x");
	ASSERT_EQ(res1.rowCount(), 2UL);

	bool foundB = false, foundC = false;
	for (const auto &row: res1.getRows()) {
		std::string name = row.at("x").asNode().getProperties().at("name").toString();
		if (name == "B")
			foundB = true;
		if (name == "C")
			foundC = true;
	}
	EXPECT_TRUE(foundB);
	EXPECT_TRUE(foundC);

	// Exact 3 hops
	auto res2 = execute("MATCH (a:HopNode {name:'A'})-[*3]->(x) RETURN x");
	ASSERT_EQ(res2.rowCount(), 1UL);
	EXPECT_EQ(res2.getRows()[0].at("x").asNode().getProperties().at("name").toString(), "D");
}

// ============================================================================
// Cartesian Product Tests (Cross Joins)
// ============================================================================

TEST_F(CypherTest, CartesianProduct_Basic) {
	(void) execute("CREATE (:GroupA {id: 1})");
	(void) execute("CREATE (:GroupA {id: 2})");
	(void) execute("CREATE (:GroupB {val: 10})");
	(void) execute("CREATE (:GroupB {val: 20})");
	(void) execute("CREATE (:GroupB {val: 30})");

	auto res1 = execute("MATCH (a:GroupA), (b:GroupB) RETURN a.id, b.val");
	ASSERT_EQ(res1.rowCount(), 6UL);
}

TEST_F(CypherTest, CartesianProduct_WithRelationships) {
	(void) execute("CREATE (u1:User {name:'U1'})-[r1:BUY]->(p1:Product {name:'P1'})");
	(void) execute("CREATE (u2:User {name:'U2'})-[r2:BUY]->(p2:Product {name:'P2'})");
	(void) execute("CREATE (c:Category {type:'Tech'})");

	auto res = execute("MATCH (u)-[r:BUY]->(p), (c:Category) RETURN u.name, c.type");
	ASSERT_EQ(res.rowCount(), 2UL);

	for (const auto &row: res.getRows()) {
		EXPECT_EQ(row.at("c.type").toString(), "Tech");
		std::string name = row.at("u.name").toString();
		EXPECT_TRUE(name == "U1" || name == "U2");
	}
}

TEST_F(CypherTest, CartesianProduct_EmptySide) {
	(void) execute("CREATE (:GroupA)");
	(void) execute("CREATE (:GroupA)");

	// 2 * 0 = 0 rows
	auto res = execute("MATCH (a:GroupA), (b:GroupB) RETURN a, b");
	EXPECT_TRUE(res.isEmpty());
}

TEST_F(CypherTest, CartesianProduct_ReturnStar) {
	(void) execute("CREATE (:A {val: 1})");
	(void) execute("CREATE (:B {val: 2})");

	auto res = execute("MATCH (a:A), (b:B) RETURN *");

	// 1 * 1 = 1 row
	ASSERT_EQ(res.rowCount(), 1UL);

	const auto &row = res.getRows()[0];
	ASSERT_TRUE(row.at("a").isNode());
	ASSERT_TRUE(row.at("b").isNode());
}

// ============================================================================
// UNWIND & Batch Insert Tests
// ============================================================================

TEST_F(CypherTest, Unwind_BasicValues) {
	auto res = execute("UNWIND [1, 2, 3] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);

	EXPECT_EQ(res.getRows()[0].at("x").asPrimitive().toString(), "1");
	EXPECT_EQ(res.getRows()[1].at("x").asPrimitive().toString(), "2");
	EXPECT_EQ(res.getRows()[2].at("x").asPrimitive().toString(), "3");
}

TEST_F(CypherTest, BatchInsert_Nodes) {
	(void) execute("UNWIND [1, 2, 3] AS x CREATE (n:BatchNode)");

	auto res = execute("MATCH (n:BatchNode) RETURN n");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(CypherTest, Unwind_Cartesian) {
	(void) execute("CREATE (:A)");
	(void) execute("CREATE (:A)");

	// 2 Nodes * 3 Items = 6 Rows
	auto res = execute("MATCH (n:A) UNWIND [10, 20, 30] AS val RETURN n, val");
	ASSERT_EQ(res.rowCount(), 6UL);

	bool found10 = false;
	for (const auto &row: res.getRows()) {
		if (row.at("val").asPrimitive().toString() == "10")
			found10 = true;
	}
	EXPECT_TRUE(found10);
}
