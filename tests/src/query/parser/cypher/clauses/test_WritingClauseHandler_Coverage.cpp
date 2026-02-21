/**
 * @file test_WritingClauseHandler_Coverage.cpp
 * @brief Coverage-focused unit tests for WritingClauseHandler
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
 * @class WritingClauseHandlerCoverageTest
 * @brief Coverage-focused tests for WritingClauseHandler
 */
class WritingClauseHandlerCoverageTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_writing_coverage_" + boost::uuids::to_string(uuid) + ".dat");
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
// CREATE with null pattern - Line 34 True branch
// ============================================================================

// Note: CREATE without pattern is grammatically invalid
// This branch may not be reachable through normal queries

// ============================================================================
// SET without rootOp - Line 52 True branch (error case)
// ============================================================================

TEST_F(WritingClauseHandlerCoverageTest, Set_WithoutMatchOrCreate) {
	// Covers True branch at line 52 (!rootOp check)
	// This should throw an error
	EXPECT_THROW({
		execute("SET n.value = 100");
	}, std::runtime_error);
}

// ============================================================================
// DELETE edge cases - Line 67, 70, 75
// ============================================================================

TEST_F(WritingClauseHandlerCoverageTest, Delete_WithDetach) {
	// Covers True branch at line 67 (K_DETACH != nullptr)
	(void) execute("CREATE (a:Test)-[:LINK]->(b:Test)");

	auto res = execute("MATCH (a:Test) DETACH DELETE a");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Delete_WithoutDetach) {
	// Covers False branch at line 67
	(void) execute("CREATE (a:Test)");

	auto res = execute("MATCH (a:Test) DELETE a");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Delete_SingleVariable) {
	// Covers line 70 with single expression
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) DELETE n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Delete_MultipleVariables) {
	// Covers line 70 with multiple expressions (loop)
	(void) execute("CREATE (a:Test {id: 1})");
	(void) execute("CREATE (b:Test {id: 2})");

	auto res = execute("MATCH (a:Test {id: 1}), (b:Test {id: 2}) DELETE a, b");
	EXPECT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// REMOVE edge cases - Line 91, 98
// ============================================================================

TEST_F(WritingClauseHandlerCoverageTest, Remove_Property) {
	// Covers True branch at line 91 (propertyExpression exists)
	(void) execute("CREATE (n:Test {prop: 'value'})");

	auto res = execute("MATCH (n:Test) REMOVE n.prop RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Remove_Label) {
	// Covers True branch at line 98 (variable and nodeLabels exist)
	(void) execute("CREATE (n:Test:Label1 {id: 1})");

	auto res = execute("MATCH (n:Test) REMOVE n:Label1 RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Remove_MultipleProperties) {
	// Covers loop at line 89 with multiple property removals
	(void) execute("CREATE (n:Test {a: 1, b: 2, c: 3})");

	auto res = execute("MATCH (n:Test) REMOVE n.a, n.b, n.c RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Remove_PropertyAndLabel) {
	// Tests both property and label removal in same query
	(void) execute("CREATE (n:Test:Label {value: 100})");

	auto res = execute("MATCH (n:Test) REMOVE n.value, n:Label RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Remove_MultipleLabels) {
	// Tests removing multiple labels
	(void) execute("CREATE (n:Test:Label1:Label2)");

	auto res = execute("MATCH (n:Test) REMOVE n:Label1, n:Label2 RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// MERGE edge cases - Line 125-139
// ============================================================================

TEST_F(WritingClauseHandlerCoverageTest, Merge_WithoutOnClauses) {
	// Covers False branches in loop (no ON MATCH/CREATE)
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	// MERGE with existing node (no ON clauses)
	auto res = execute("MERGE (n:Person {name: 'Alice'}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Merge_OnlyOnMatch) {
	// Covers True branch at line 133 (type == "MATCH")
	(void) execute("CREATE (n:Person {name: 'Bob'})");

	auto res = execute("MERGE (n:Person {name: 'Bob'}) ON MATCH SET n.updated = true RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Merge_OnlyOnCreate) {
	// Covers True branch at line 135 (type == "CREATE")
	auto res = execute("MERGE (n:Person {name: 'Charlie'}) ON CREATE SET n.created = true RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Merge_BothOnMatchAndOnCreate) {
	// Tests both ON MATCH and ON CREATE
	(void) execute("CREATE (n:Person {name: 'Dave'})");

	// Existing node - ON MATCH triggers
	auto res1 = execute("MERGE (n:Person {name: 'Dave'}) ON MATCH SET n.matched = true ON CREATE SET n.created = true RETURN n");
	ASSERT_EQ(res1.rowCount(), 1UL);

	// New node - ON CREATE triggers
	auto res2 = execute("MERGE (n:Person {name: 'Eve'}) ON MATCH SET n.matched = true ON CREATE SET n.created = true RETURN n");
	ASSERT_EQ(res2.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Merge_MultipleOnClauses) {
	// Tests multiple ON MATCH clauses
	(void) execute("CREATE (n:Person {name: 'Frank'})");

	auto res = execute("MERGE (n:Person {name: 'Frank'}) ON MATCH SET n.v1 = 1 ON MATCH SET n.v2 = 2 RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Merge_WithRelationship) {
	// Tests MERGE with relationship pattern
	(void) execute("CREATE (a:Person {name: 'Alice'})");

	auto res = execute("MERGE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'}) RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// SET clause variations
// ============================================================================

TEST_F(WritingClauseHandlerCoverageTest, Set_SingleProperty) {
	// Tests SET with single property
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) SET n.value = 100 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
}

TEST_F(WritingClauseHandlerCoverageTest, Set_MultipleProperties) {
	// Tests SET with multiple properties
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) SET n.a = 1, n.b = 2, n.c = 3 RETURN n.a, n.b, n.c");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.a").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.b").toString(), "2");
	EXPECT_EQ(res.getRows()[0].at("n.c").toString(), "3");
}

TEST_F(WritingClauseHandlerCoverageTest, Set_OverwriteProperty) {
	// Tests overwriting existing property
	(void) execute("CREATE (n:Test {value: 100})");

	auto res = execute("MATCH (n:Test) SET n.value = 200 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "200");
}

// ============================================================================
// CREATE clause variations
// ============================================================================

TEST_F(WritingClauseHandlerCoverageTest, Create_SingleNode) {
	// Tests simple CREATE
	auto res = execute("CREATE (n:Test {id: 1}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Create_MultipleNodes) {
	// Tests CREATE with multiple nodes
	auto res = execute("CREATE (a:Test {id: 1}), (b:Test {id: 2}) RETURN a");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Create_WithRelationship) {
	// Tests CREATE with relationship
	auto res = execute("CREATE (a)-[:LINK]->(b) RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// Combined clauses
// ============================================================================

TEST_F(WritingClauseHandlerCoverageTest, CreateThenSetThenReturn) {
	// Tests CREATE followed by SET
	auto res = execute("CREATE (n:Test) SET n.value = 100 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
}

TEST_F(WritingClauseHandlerCoverageTest, MatchThenSetThenReturn) {
	// Tests MATCH followed by SET
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) SET n.updated = true RETURN n.updated");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.updated").toString(), "true");
}

TEST_F(WritingClauseHandlerCoverageTest, MatchThenRemoveThenReturn) {
	// Tests MATCH followed by REMOVE
	(void) execute("CREATE (n:Test:Label {value: 100})");

	auto res = execute("MATCH (n:Test) REMOVE n:Label RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, MatchThenDelete) {
	// Tests MATCH followed by DELETE
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) DELETE n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// Additional branch coverage tests
// ============================================================================

TEST_F(WritingClauseHandlerCoverageTest, Delete_DetachMultiple) {
	// Tests DETACH DELETE with multiple relationships
	(void) execute("CREATE (a)-[:L1]->(b)-[:L2]->(c)");
	(void) execute("CREATE (a)-[:L3]->(d)");

	auto res = execute("MATCH (a) DETACH DELETE a");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Remove_AllPropertiesFromNode) {
	// Tests removing all properties from node
	(void) execute("CREATE (n:Test {a: 1, b: 2, c: 3})");

	auto res = execute("MATCH (n:Test) REMOVE n.a, n.b, n.c RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Merge_CreateWhenNotExists) {
	// Tests MERGE creates new node when not exists
	auto res = execute("MERGE (n:NewNode {name: 'test'}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerCoverageTest, Merge_MatchWhenExists) {
	// Tests MERGE matches existing node
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	auto res = execute("MERGE (n:Person {name: 'Alice'}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}
