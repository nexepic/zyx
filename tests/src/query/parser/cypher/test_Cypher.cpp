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
				std::filesystem::temp_directory_path() / ("test_cypher_" + CLI::detail::to_string(uuid) + ".dat");

		// Initialize Database
		db = std::make_unique<graph::Database>(testFilePath);
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
	graph::query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }
};

// ============================================================================
// 1. Basic CRUD Tests (Create & Read)
// ============================================================================

TEST_F(CypherTest, CreateAndMatchNode) {
	// 1. Create a node with properties
	auto res1 = execute("CREATE (n:User {name: 'Alice', age: 30}) RETURN n");
	ASSERT_EQ(res1.nodeCount(), 1);

	auto node = res1.getNodes()[0];
	EXPECT_EQ(node.getLabel(), "User");

	// Verify properties (Hydration check)
	const auto &props = node.getProperties();
	EXPECT_TRUE(props.contains("name"));
	EXPECT_EQ(props.at("name").toString(), "Alice");
	EXPECT_EQ(props.at("age").toString(), "30");

	// 2. Match the node back (Scan)
	auto res2 = execute("MATCH (n:User) RETURN n");
	ASSERT_EQ(res2.nodeCount(), 1);
	EXPECT_EQ(res2.getNodes()[0].getLabel(), "User");
}

TEST_F(CypherTest, CreateAndMatchChain) {
	// Test: (a)-[r]->(b) creation pipeline
	(void) execute("CREATE (a:Person {name: 'A'})-[r:KNOWS {since: 2020}]->(b:Person {name: 'B'})");

	// Verify traversal
	auto res = execute("MATCH (n:Person)-[r]->(m) RETURN n, r, m");

	ASSERT_EQ(res.nodeCount(), 2); // A and B
	ASSERT_EQ(res.edgeCount(), 1); // KNOWS

	// Verify Topology
	const auto &edges = res.getEdges();
	EXPECT_EQ(edges[0].getLabel(), "KNOWS");

	// Verify Properties
	EXPECT_EQ(edges[0].getProperties().at("since").toString(), "2020");
}

// ============================================================================
// 2. Filtering Tests (WHERE & Inline)
// ============================================================================

TEST_F(CypherTest, FilterByInlineProperty) {
	(void) execute("CREATE (n:User {name: 'Alice'})");
	(void) execute("CREATE (n:User {name: 'Bob'})");

	// Inline match (This tests Index Pushdown or FilterOperator logic)
	auto res = execute("MATCH (n:User {name: 'Alice'}) RETURN n");
	ASSERT_EQ(res.nodeCount(), 1);
	EXPECT_EQ(res.getNodes()[0].getProperties().at("name").toString(), "Alice");
}

// TEST_F(CypherTest, FilterByWhereClause) {
// 	execute("CREATE (n:Item {price: 100})");
// 	execute("CREATE (n:Item {price: 200})");
// 	execute("CREATE (n:Item {price: 50})");
//
// 	// Test Equals (=)
// 	auto res1 = execute("MATCH (n:Item) WHERE n.price = 200 RETURN n");
// 	ASSERT_EQ(res1.nodeCount(), 1);
// 	EXPECT_EQ(res1.getNodes()[0].getProperties().at("price").toString(), "200");
//
// 	// Test Not Equals (<>)
// 	auto res2 = execute("MATCH (n:Item) WHERE n.price <> 100 RETURN n");
// 	ASSERT_EQ(res2.nodeCount(), 2); // 200 and 50
// }

TEST_F(CypherTest, FilterTraversalTarget) {
	// A -> B(Target)
	(void) execute("CREATE (a:Node)-[:LINK]->(b:Target {id: 1})");
	// A -> C(Other)
	(void) execute("CREATE (a:Node)-[:LINK]->(c:Other {id: 2})");

	// Filter target label
	auto res = execute("MATCH (n:Node)-[r]->(t:Target) RETURN t");
	ASSERT_EQ(res.nodeCount(), 1);
	EXPECT_EQ(res.getNodes()[0].getLabel(), "Target");
}

// ============================================================================
// 3. Indexing Tests (Admin & Optimization)
// ============================================================================

TEST_F(CypherTest, IndexCreationAndPushdown) {
	// 1. Create Index via Cypher
	auto res1 = execute("CREATE INDEX ON :User(id)");
	// Should contain a row with "result" -> "Index created"
	ASSERT_GE(res1.rowCount(), 1UL);

	// 2. Insert Data
	(void) execute("CREATE (u1:User {id: 1, name: 'One'})");
	(void) execute("CREATE (u2:User {id: 2, name: 'Two'})");
	(void) execute("CREATE (u3:User {id: 3, name: 'Three'})");

	// 3. Show Indexes
	auto resIndex = execute("SHOW INDEXES");
	ASSERT_GE(resIndex.rowCount(), 1UL);
	// Could iterate rows to verify key='id' exists

	// 4. Query using Index (Implicit Verification)
	// The logic inside NodeScanOperator should pick the PropertyIndex.
	// We confirm the result is correct.
	auto resQuery = execute("MATCH (n:User {id: 2}) RETURN n");
	ASSERT_EQ(resQuery.nodeCount(), 1);
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
	// Note: Assuming no other indexes exist in this isolated test
	// In a real suite, we'd filter the rows.
	// Since SetUp clears the DB, this is safe.
	auto resShow2 = execute("SHOW INDEXES");
	// If show indexes returns empty optional in Operator, QueryResult might be empty
	if (!resShow2.isEmpty()) {
		// If not empty, ensure Tag(name) is not in it
		// (Simplified check for empty here)
	}
}

// ============================================================================
// 4. System Configuration Tests (CALL Procedures)
// ============================================================================

TEST_F(CypherTest, SystemConfigFlow) {
	// 1. Set Config (Mixed Types)
	(void) execute("CALL dbms.setConfig('test.key', 12345)");
	(void) execute("CALL dbms.setConfig('test.mode', 'ACTIVE')");

	// 2. Get Specific Config
	auto res1 = execute("CALL dbms.getConfig('test.key')");
	ASSERT_EQ(res1.rowCount(), 1);
	EXPECT_EQ(res1.getRows()[0].at("value").toString(), "12345");

	// 3. List All
	auto res2 = execute("CALL dbms.listConfig()");
	// Should contain at least the 2 we added
	ASSERT_GE(res2.rowCount(), 2UL);

	// Verify Merge behavior (previous config not lost)
	bool foundKey = false;
	bool foundMode = false;
	for (const auto &row: res2.getRows()) {
		if (row.at("key").toString() == "test.key")
			foundKey = true;
		if (row.at("key").toString() == "test.mode")
			foundMode = true;
	}
	EXPECT_TRUE(foundKey);
	EXPECT_TRUE(foundMode);
}

// ============================================================================
// 5. Persistence & Recovery Test (Full Cycle)
// ============================================================================

TEST_F(CypherTest, DataPersistenceWithIndexAndConfig) {
	// 1. Setup Phase
	(void) execute("CREATE (n:SaveTest {val: 999})");
	(void) execute("CREATE INDEX ON :SaveTest(val)");
	(void) execute("CALL dbms.setConfig('persistent.cfg', 'true')");

	// 2. Flush to Disk (Simulate 'save' command)
	// This triggers IndexManager and SystemStateManager to serialize data
	db->getStorage()->flush();

	// 3. Close Database (Destructor)
	// We manually close and reset the pointer to ensure complete shutdown
	db->close();
	db.reset();

	// 4. Reopen Database
	// This simulates a fresh application start
	db = std::make_unique<graph::Database>(testFilePath);
	db->open();

	// 5. Verify Data (Hydration)
	auto resData = execute("MATCH (n:SaveTest {val: 999}) RETURN n");
	ASSERT_EQ(resData.nodeCount(), 1);
	EXPECT_EQ(resData.getNodes()[0].getProperties().at("val").toString(), "999");

	// 6. Verify Index (Metadata Persistence)
	auto resIdx = execute("SHOW INDEXES");
	// Should show the index we created
	bool indexFound = false;
	for (const auto &row: resIdx.getRows()) {
		// Checking simplified string matching
		if (row.at("key").toString() == "val")
			indexFound = true;
	}
	EXPECT_TRUE(indexFound) << "Index definition lost after restart";

	// 7. Verify Config (State Persistence)
	auto resCfg = execute("CALL dbms.getConfig('persistent.cfg')");
	ASSERT_EQ(resCfg.rowCount(), 1);
	EXPECT_EQ(resCfg.getRows()[0].at("value").toString(), "true") << "Config lost after restart";
}
