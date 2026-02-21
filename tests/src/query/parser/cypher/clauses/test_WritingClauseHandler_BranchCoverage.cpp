/**
 * @file test_WritingClauseHandler_BranchCoverage.cpp
 * @brief Additional branch coverage tests for WritingClauseHandler
 * @date 2026/02/17
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;

/**
 * @class WritingClauseHandlerBranchCoverageTest
 * @brief Additional branch coverage tests for WritingClauseHandler
 */
class WritingClauseHandlerBranchCoverageTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_writing_branch_" + boost::uuids::to_string(uuid) + ".dat");
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

	[[nodiscard]] graph::query::QueryResult execute(const std::string &query) const {
		return db->getQueryEngine()->execute(query);
	}
};

// ============================================================================
// SET tests for branch coverage
// ============================================================================

TEST_F(WritingClauseHandlerBranchCoverageTest, Set_AfterMatch) {
	// Covers False branch at line 52 (rootOp exists)
	(void) execute("CREATE (n:Test {value: 100})");

	auto res = execute("MATCH (n:Test) SET n.value = 200 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "200");
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Set_AfterCreate) {
	// Tests SET after CREATE
	auto res = execute("CREATE (n:Test) SET n.value = 100 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Set_AddNewProperty) {
	// Tests SET adding new property to node
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) SET n.newProp = 'value' RETURN n.newProp");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.newProp").toString(), "value");
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Set_OverwriteExistingProperty) {
	// Tests SET overwriting existing property
	(void) execute("CREATE (n:Test {value: 10})");

	auto res = execute("MATCH (n:Test) SET n.value = 20 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "20");
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Set_MultiplePropertiesAtOnce) {
	// Tests SET with multiple properties
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) SET n.a = 1, n.b = 2, n.c = 3 RETURN n.a, n.b, n.c");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.a").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.b").toString(), "2");
	EXPECT_EQ(res.getRows()[0].at("n.c").toString(), "3");
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Set_DifferentTypes) {
	// Tests SET with different value types
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) SET n.str = 'test', n.num = 42, n.bool = true RETURN n.str, n.num, n.bool");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.str").toString(), "test");
	EXPECT_EQ(res.getRows()[0].at("n.num").toString(), "42");
	EXPECT_EQ(res.getRows()[0].at("n.bool").toString(), "true");
}

// ============================================================================
// DELETE tests for branch coverage
// ============================================================================

TEST_F(WritingClauseHandlerBranchCoverageTest, Delete_SingleNode) {
	// Tests DELETE with single variable
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) DELETE n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Delete_MultipleNodes) {
	// Tests DELETE with multiple variables
	(void) execute("CREATE (a:Test {id: 1})");
	(void) execute("CREATE (b:Test {id: 2})");

	auto res = execute("MATCH (a:Test {id: 1}), (b:Test {id: 2}) DELETE a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Delete_DetachSingleNode) {
	// Tests DETACH DELETE
	(void) execute("CREATE (a)-[:LINK]->(b)");

	auto res = execute("MATCH (a) DETACH DELETE a");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Delete_DetachMultipleNodes) {
	// Tests DETACH DELETE with multiple targets
	(void) execute("CREATE (a)-[:L1]->(b)");
	(void) execute("CREATE (a)-[:L2]->(c)");

	auto res = execute("MATCH (a) DETACH DELETE a");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Delete_NoDetachMultiple) {
	// Tests DELETE without DETACH, multiple nodes
	(void) execute("CREATE (a:Test {id: 1})");
	(void) execute("CREATE (b:Test {id: 2})");

	auto res = execute("MATCH (a:Test {id: 1}), (b:Test {id: 2}) DELETE a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// REMOVE tests for branch coverage
// ============================================================================

TEST_F(WritingClauseHandlerBranchCoverageTest, Remove_Property) {
	// Tests REMOVE property (Line 91 True branch)
	(void) execute("CREATE (n:Test {prop: 'value'})");

	auto res = execute("MATCH (n:Test) REMOVE n.prop RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Remove_Label) {
	// Tests REMOVE label (Line 98 True branch)
	(void) execute("CREATE (n:Test:Label)");

	auto res = execute("MATCH (n:Test) REMOVE n:Label RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Remove_MultipleProperties) {
	// Tests REMOVE multiple properties (loop at line 89)
	(void) execute("CREATE (n:Test {a: 1, b: 2, c: 3})");

	auto res = execute("MATCH (n:Test) REMOVE n.a, n.b, n.c RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Remove_MultipleLabels) {
	// Tests REMOVE multiple labels
	(void) execute("CREATE (n:Test:Label1:Label2:Label3)");

	auto res = execute("MATCH (n:Test) REMOVE n:Label1, n:Label2, n:Label3 RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Remove_PropertyAndLabel) {
	// Tests REMOVE both property and label
	(void) execute("CREATE (n:Test:Label {value: 100})");

	auto res = execute("MATCH (n:Test) REMOVE n.value, n:Label RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Remove_AllProperties) {
	// Tests REMOVE all properties from node
	(void) execute("CREATE (n:Test {a: 1, b: 2, c: 3, d: 4})");

	auto res = execute("MATCH (n:Test) REMOVE n.a, n.b, n.c, n.d RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// MERGE tests for branch coverage
// ============================================================================

TEST_F(WritingClauseHandlerBranchCoverageTest, Merge_CreateNewNode) {
	// Tests MERGE creating new node (Line 125-134 False branches for no ON clauses)
	auto res = execute("MERGE (n:Person {name: 'Alice'}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Merge_MatchExistingNode) {
	// Tests MERGE matching existing node
	(void) execute("CREATE (n:Person {name: 'Bob'})");

	auto res = execute("MERGE (n:Person {name: 'Bob'}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Merge_OnCreateOnly) {
	// Tests MERGE with ON CREATE only (Line 133 True branch)
	(void) execute("CREATE (n:Person {name: 'Charlie'})");

	auto res = execute("MERGE (n:Person {name: 'Charlie'}) ON CREATE SET n.created = true RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Merge_OnMatchOnly) {
	// Tests MERGE with ON MATCH only (Line 135 True branch)
	(void) execute("CREATE (n:Person {name: 'David'})");

	auto res = execute("MERGE (n:Person {name: 'David'}) ON MATCH SET n.found = true RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Merge_BothOnCreateAndOnMatch) {
	// Tests MERGE with both ON CREATE and ON MATCH
	(void) execute("CREATE (n:Person {name: 'Eve'})");

	// Existing node - ON MATCH triggers
	auto res1 = execute("MERGE (n:Person {name: 'Eve'}) ON CREATE SET n.is_new = true ON MATCH SET n.is_existing = true RETURN n");
	ASSERT_EQ(res1.rowCount(), 1UL);

	// New node - ON CREATE triggers
	auto res2 = execute("MERGE (n:Person {name: 'Frank'}) ON CREATE SET n.is_new = true ON MATCH SET n.is_existing = true RETURN n");
	ASSERT_EQ(res2.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Merge_WithRelationship) {
	// Tests MERGE with relationship pattern
	(void) execute("CREATE (a:Person {name: 'Alice'})");

	auto res = execute("MERGE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'}) RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Merge_MultipleOnClauses) {
	// Tests MERGE with multiple ON clauses
	(void) execute("CREATE (n:Person {name: 'George'})");

	auto res = execute("MERGE (n:Person {name: 'George'}) ON MATCH SET n.v1 = 1 ON MATCH SET n.v2 = 2 RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// CREATE tests for branch coverage
// ============================================================================

TEST_F(WritingClauseHandlerBranchCoverageTest, Create_SingleNode) {
	// Tests CREATE with single node
	auto res = execute("CREATE (n:Test {id: 1}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Create_MultipleNodes) {
	// Tests CREATE with multiple nodes
	auto res = execute("CREATE (a:Test {id: 1}), (b:Test {id: 2}) RETURN a");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Create_WithRelationship) {
	// Tests CREATE with relationship
	auto res = execute("CREATE (a)-[:LINK]->(b) RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Create_ComplexPattern) {
	// Tests CREATE with complex pattern
	auto res = execute("CREATE (a:Start)-[:LINK1]->(b)-[:LINK2]->(c:End) RETURN a, b, c");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, Create_MultipleRelationships) {
	// Tests CREATE with multiple relationships from same node
	auto res = execute("CREATE (a)-[:L1]->(b), (a)-[:L2]->(c) RETURN a");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// Combined clause tests
// ============================================================================

TEST_F(WritingClauseHandlerBranchCoverageTest, CreateThenSetThenReturn) {
	// Tests CREATE followed by SET
	auto res = execute("CREATE (n:Test) SET n.value = 100 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
}

TEST_F(WritingClauseHandlerBranchCoverageTest, MatchThenSetThenReturn) {
	// Tests MATCH followed by SET
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) SET n.updated = true RETURN n.updated");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.updated").toString(), "true");
}

TEST_F(WritingClauseHandlerBranchCoverageTest, MatchThenRemoveThenReturn) {
	// Tests MATCH followed by REMOVE
	(void) execute("CREATE (n:Test:Label {value: 100})");

	auto res = execute("MATCH (n:Test) REMOVE n:Label RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerBranchCoverageTest, MatchThenDelete) {
	// Tests MATCH followed by DELETE
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) DELETE n");
	ASSERT_EQ(res.rowCount(), 1UL);
}
