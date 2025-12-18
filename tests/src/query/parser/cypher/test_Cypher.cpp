/**
 * @file test_Cypher.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/16
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "graph/cli/CLI11.hpp"
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

		// Clean up previous artifacts if any
		if (fs::exists(testFilePath)) fs::remove_all(testFilePath);

		// Initialize Database
		db = std::make_unique<graph::Database>(testFilePath.string());
		db->open();
	}

	void TearDown() override {
		if (db) {
			db->close();
		}
		// Cleanup file system
		if (fs::exists(testFilePath)) {
			fs::remove_all(testFilePath);
		}
	}

	// Helper to execute Cypher and return result
	graph::query::QueryResult execute(const std::string &query) const {
        return db->getQueryEngine()->execute(query);
    }
};

// ============================================================================
// 1. Basic CRUD Tests (Create & Read)
// ============================================================================

TEST_F(CypherTest, CreateAndMatchNode) {
	// 1. Create a node with properties
	auto res1 = execute("CREATE (n:User {name: 'Alice', age: 30}) RETURN n");
	ASSERT_EQ(res1.nodeCount(), 1UL);

	auto node = res1.getNodes()[0];
	EXPECT_EQ(node.getLabel(), "User");

	// Verify properties (Hydration check)
	const auto &props = node.getProperties();
	EXPECT_TRUE(props.contains("name"));
	EXPECT_EQ(props.at("name").toString(), "Alice");
	EXPECT_EQ(props.at("age").toString(), "30");

	// 2. Match the node back (Scan)
	auto res2 = execute("MATCH (n:User) RETURN n");
	ASSERT_EQ(res2.nodeCount(), 1UL);
	EXPECT_EQ(res2.getNodes()[0].getLabel(), "User");
}

TEST_F(CypherTest, CreateAndMatchChain) {
	// Test: (a)-[r]->(b) creation pipeline
    // We expect no return data, so void cast to ignore result
	(void) execute("CREATE (a:Person {name: 'A'})-[r:KNOWS {since: 2020}]->(b:Person {name: 'B'})");

	// Verify traversal
	auto res = execute("MATCH (n:Person)-[r]->(m) RETURN n, r, m");

	ASSERT_EQ(res.nodeCount(), 2UL); // A and B
	ASSERT_EQ(res.edgeCount(), 1UL); // KNOWS

	// Verify Topology
	const auto &edges = res.getEdges();
	EXPECT_EQ(edges[0].getLabel(), "KNOWS");

	// Verify Properties
	EXPECT_EQ(edges[0].getProperties().at("since").toString(), "2020");
}

// ============================================================================
// 2. Update & Delete Tests (SET & DELETE)
// ============================================================================

TEST_F(CypherTest, UpdateProperties) {
    (void) execute("CREATE (n:UpdateTest {val: 1})");

    // Update property
    auto resSet = execute("MATCH (n:UpdateTest) SET n.val = 2 RETURN n");
    ASSERT_EQ(resSet.nodeCount(), 1UL);
    // Should reflect immediate change in returned record
    EXPECT_EQ(resSet.getNodes()[0].getProperties().at("val").toString(), "2");

    // Verify persistence
    auto resCheck = execute("MATCH (n:UpdateTest) RETURN n");
    EXPECT_EQ(resCheck.getNodes()[0].getProperties().at("val").toString(), "2");
}

TEST_F(CypherTest, AddNewProperty) {
    (void) execute("CREATE (n:UpdateTest {val: 1})");

    // Add new property 'tag'
    (void) execute("MATCH (n:UpdateTest) SET n.tag = 'new'");

    auto res = execute("MATCH (n:UpdateTest) RETURN n");
    const auto& props = res.getNodes()[0].getProperties();
    EXPECT_EQ(props.at("val").toString(), "1");
    EXPECT_EQ(props.at("tag").toString(), "new");
}

TEST_F(CypherTest, DeleteNodes) {
    (void) execute("CREATE (n:DeleteMe)");

    // Ensure exists
    ASSERT_EQ(execute("MATCH (n:DeleteMe) RETURN n").nodeCount(), 1UL);

    // Delete
    (void) execute("MATCH (n:DeleteMe) DELETE n");

    // Verify gone
    auto res = execute("MATCH (n:DeleteMe) RETURN n");
    EXPECT_EQ(res.nodeCount(), 0UL);
}

TEST_F(CypherTest, DeleteNodeWithEdgesConstraint) {
	// 1. Setup: Create two nodes connected by an edge
	(void) execute("CREATE (a:ConstraintTest)-[:REL]->(b:ConstraintTest)");

	// 2. Attempt Standard DELETE (Should Fail)
	// Cypher semantics: Cannot delete a node that still has relationships
	EXPECT_THROW({
		execute("MATCH (n:ConstraintTest) DELETE n");
	}, std::runtime_error);

	// 3. Verify Data remains untouched
	auto res = execute("MATCH (n:ConstraintTest) RETURN n");
	EXPECT_EQ(res.nodeCount(), 2UL);
}

TEST_F(CypherTest, DetachDeleteNodes) {
	// 1. Setup: Create nodes with relationships
	(void) execute("CREATE (a:DetachTest)-[r:REL]->(b:DetachTest)");

	// 2. Execute DETACH DELETE
	// Should remove both nodes AND the relationship 'r'
	(void) execute("MATCH (n:DetachTest) DETACH DELETE n");

	// 3. Verify Nodes are gone
	auto resNodes = execute("MATCH (n:DetachTest) RETURN n");
	EXPECT_EQ(resNodes.nodeCount(), 0UL);

	// 4. Verify Relationships are gone
	// (Trying to match the pattern again should yield nothing)
	auto resEdges = execute("MATCH ()-[r:REL]->() RETURN r");
	EXPECT_EQ(resEdges.edgeCount(), 0UL);
}

TEST_F(CypherTest, DeleteEdgeOnly) {
	// 1. Setup
	(void) execute("CREATE (a:EdgeDel)-[r:TO_BE_DELETED]->(b:EdgeDel)");

	// 2. Delete only the relationship 'r'
	(void) execute("MATCH (a:EdgeDel)-[r:TO_BE_DELETED]->(b) DELETE r");

	// 3. Verify Nodes still exist
	auto resNodes = execute("MATCH (n:EdgeDel) RETURN n");
	EXPECT_EQ(resNodes.nodeCount(), 2UL);

	// 4. Verify Edge is gone
	auto resEdges = execute("MATCH (a:EdgeDel)-[r:TO_BE_DELETED]->(b) RETURN r");
	EXPECT_EQ(resEdges.edgeCount(), 0UL);
}

// ============================================================================
// 3. Filtering Tests (WHERE & Inline)
// ============================================================================

TEST_F(CypherTest, FilterByInlineProperty) {
	(void) execute("CREATE (n:User {name: 'Alice'})");
	(void) execute("CREATE (n:User {name: 'Bob'})");

	// Inline match (Tests Index Pushdown or FilterOperator)
	auto res = execute("MATCH (n:User {name: 'Alice'}) RETURN n");
	ASSERT_EQ(res.nodeCount(), 1UL);
	EXPECT_EQ(res.getNodes()[0].getProperties().at("name").toString(), "Alice");
}

TEST_F(CypherTest, FilterByWhereClause) {
	(void) execute("CREATE (n:Item {price: 100})");
	(void) execute("CREATE (n:Item {price: 200})");
	(void) execute("CREATE (n:Item {price: 50})");

	// Test Equals (=)
	auto res1 = execute("MATCH (n:Item) WHERE n.price = 200 RETURN n");
	ASSERT_EQ(res1.nodeCount(), 1UL);
	EXPECT_EQ(res1.getNodes()[0].getProperties().at("price").toString(), "200");

	// Test Not Equals (<>)
	auto res2 = execute("MATCH (n:Item) WHERE n.price <> 100 RETURN n");
	ASSERT_EQ(res2.nodeCount(), 2UL); // 200 and 50

    // Test Greater Than (>)
    auto res3 = execute("MATCH (n:Item) WHERE n.price > 90 RETURN n");
    ASSERT_EQ(res3.nodeCount(), 2UL); // 100 and 200
}

TEST_F(CypherTest, FilterTraversalTarget) {
	// A -> B(Target)
	(void) execute("CREATE (a:Node)-[:LINK]->(b:Target {id: 1})");
	// A -> C(Other)
	(void) execute("CREATE (a:Node)-[:LINK]->(c:Other {id: 2})");

	// Filter target label
	auto res = execute("MATCH (n:Node)-[r]->(t:Target) RETURN t");
	ASSERT_EQ(res.nodeCount(), 1UL);
	EXPECT_EQ(res.getNodes()[0].getLabel(), "Target");
}

// ============================================================================
// 4. Indexing Tests (Admin & Optimization)
// ============================================================================

TEST_F(CypherTest, IndexCreationAndPushdown) {
	// 1. Create Index via Cypher
	auto res1 = execute("CREATE INDEX ON :User(id)");
	ASSERT_GE(res1.rowCount(), 1UL);

	// 2. Insert Data (Should trigger IndexBuilder)
	(void) execute("CREATE (u1:User {id: 1, name: 'One'})");
	(void) execute("CREATE (u2:User {id: 2, name: 'Two'})");
	(void) execute("CREATE (u3:User {id: 3, name: 'Three'})");

	// 3. Show Indexes
	auto resIndex = execute("SHOW INDEXES");
	ASSERT_GE(resIndex.rowCount(), 1UL);

	// 4. Query using Index (Implicit Verification of Scan Logic)
	auto resQuery = execute("MATCH (n:User {id: 2}) RETURN n");
	ASSERT_EQ(resQuery.nodeCount(), 1UL);
	EXPECT_EQ(resQuery.getNodes()[0].getProperties().at("name").toString(), "Two");
}

TEST_F(CypherTest, DropIndex) {
	(void) execute("CREATE INDEX ON :Tag(name)");

	// Ensure it exists
	auto resShow1 = execute("SHOW INDEXES");
	ASSERT_GE(resShow1.rowCount(), 1UL);

	// Drop it
	(void) execute("DROP INDEX ON :Tag(name)");

	// Ensure it's gone
	auto resShow2 = execute("SHOW INDEXES");
    // Depending on implementation, might return empty set or set without this index
    if (!resShow2.isEmpty() && resShow2.rowCount() > 0) {
        bool found = false;
        for(const auto& row : resShow2.getRows()) {
            if (row.at("key").toString() == "name") found = true;
        }
        EXPECT_FALSE(found);
    }
}

// ============================================================================
// 5. System Configuration Tests (CALL Procedures)
// ============================================================================

TEST_F(CypherTest, SystemConfigFlow) {
	// 1. Set Config (Mixed Types)
	(void) execute("CALL dbms.setConfig('test.key', 12345)");
	(void) execute("CALL dbms.setConfig('test.mode', 'ACTIVE')");

	// 2. Get Specific Config
	auto res1 = execute("CALL dbms.getConfig('test.key')");
	ASSERT_EQ(res1.rowCount(), 1UL);
	EXPECT_EQ(res1.getRows()[0].at("value").toString(), "12345");

	// 3. List All
	auto res2 = execute("CALL dbms.listConfig()");
	ASSERT_GE(res2.rowCount(), 2UL);

	// Verify Merge behavior
	bool foundKey = false;
	bool foundMode = false;
	for (const auto &row: res2.getRows()) {
		if (row.at("key").toString() == "test.key") foundKey = true;
		if (row.at("key").toString() == "test.mode") foundMode = true;
	}
	EXPECT_TRUE(foundKey);
	EXPECT_TRUE(foundMode);
}

// ============================================================================
// 6. Persistence & Recovery Test (Full Cycle)
// ============================================================================

TEST_F(CypherTest, DataPersistenceWithIndexAndConfig) {
	// 1. Setup Phase
	(void) execute("CREATE (n:SaveTest {val: 999})");
	(void) execute("CREATE INDEX ON :SaveTest(val)");
	(void) execute("CALL dbms.setConfig('persistent.cfg', 'true')");

	// 2. Flush to Disk
	db->getStorage()->flush();

	// 3. Close Database
	db->close();
	db.reset();

	// 4. Reopen Database
	db = std::make_unique<graph::Database>(testFilePath.string());
	db->open();

	// 5. Verify Data (Hydration)
	auto resData = execute("MATCH (n:SaveTest {val: 999}) RETURN n");
	ASSERT_EQ(resData.nodeCount(), 1UL);
	EXPECT_EQ(resData.getNodes()[0].getProperties().at("val").toString(), "999");

	// 6. Verify Index (Metadata Persistence)
	auto resIdx = execute("SHOW INDEXES");
	bool indexFound = false;
	for (const auto &row: resIdx.getRows()) {
		if (row.at("key").toString() == "val") indexFound = true;
	}
	EXPECT_TRUE(indexFound) << "Index definition lost after restart";

	// 7. Verify Config (State Persistence)
	auto resCfg = execute("CALL dbms.getConfig('persistent.cfg')");
	ASSERT_EQ(resCfg.rowCount(), 1UL);
	EXPECT_EQ(resCfg.getRows()[0].at("value").toString(), "true") << "Config lost after restart";
}

// ============================================================================
// 7. Graph Algorithm Tests (CALL algo.*)
// ============================================================================

TEST_F(CypherTest, AlgoShortestPath) {
	// 1. Setup Graph Topology: A -> B -> C
	(void) execute("CREATE (a:City {name: 'A'})-[r1:ROAD]->(b:City {name: 'B'})");
	// Match B to extend the chain to C
	(void) execute("MATCH (b:City {name: 'B'}) CREATE (b)-[r2:ROAD]->(c:City {name: 'C'})");

	// 2. Fetch IDs dynamically
	auto resA = execute("MATCH (n:City {name: 'A'}) RETURN n");
	ASSERT_EQ(resA.nodeCount(), 1UL);
	int64_t idA = resA.getNodes()[0].getId();

	auto resC = execute("MATCH (n:City {name: 'C'}) RETURN n");
	ASSERT_EQ(resC.nodeCount(), 1UL);
	int64_t idC = resC.getNodes()[0].getId();

	// 3. Execute Shortest Path
	std::string query = "CALL algo.shortestPath(" + std::to_string(idA) + ", " + std::to_string(idC) + ")";
	auto resPath = execute(query);

	// 4. Verify Result
	// Expecting 3 nodes in the path: A -> B -> C
	ASSERT_EQ(resPath.nodeCount(), 3UL);

	const auto &nodes = resPath.getNodes();
	// Verify Order and Properties
	EXPECT_EQ(nodes[0].getProperties().at("name").toString(), "A");
	EXPECT_EQ(nodes[1].getProperties().at("name").toString(), "B");
	EXPECT_EQ(nodes[2].getProperties().at("name").toString(), "C");

	// Verify Steps metadata (returned as Rows)
	ASSERT_GE(resPath.rowCount(), 3UL);
	EXPECT_EQ(resPath.getRows()[0].at("step").toString(), "0");
	EXPECT_EQ(resPath.getRows()[2].at("step").toString(), "2");
}

TEST_F(CypherTest, AlgoShortestPathNoPath) {
	// 1. Create Disconnected Graph: A   B
	(void) execute("CREATE (a:City {name: 'A'})");
	(void) execute("CREATE (b:City {name: 'B'})");

	auto resA = execute("MATCH (n:City {name: 'A'}) RETURN n");
	int64_t idA = resA.getNodes()[0].getId();

	auto resB = execute("MATCH (n:City {name: 'B'}) RETURN n");
	int64_t idB = resB.getNodes()[0].getId();

	// 2. Execute Shortest Path
	std::string query = "CALL algo.shortestPath(" + std::to_string(idA) + ", " + std::to_string(idB) + ")";
	auto resPath = execute(query);

	// 3. Verify Empty Result
	EXPECT_TRUE(resPath.isEmpty()) << "Should return empty result for disconnected nodes";
}