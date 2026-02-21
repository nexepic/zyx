/**
 * @file test_WritingClauseHandler_Unit.cpp
 * @brief Unit tests for WritingClauseHandler coverage gaps
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
 * @class WritingClauseHandlerUnitTest
 * @brief Unit tests for WritingClauseHandler edge cases
 */
class WritingClauseHandlerUnitTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_writing_unit_" + boost::uuids::to_string(uuid) + ".dat");
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
// SET Clause Edge Cases
// ============================================================================

TEST_F(WritingClauseHandlerUnitTest, Set_WithoutPrecedingClause) {
	// Tests SET clause without preceding MATCH or CREATE
	// This should throw an error
	EXPECT_THROW({
		execute("SET n.value = 100");
	}, std::runtime_error);
}

TEST_F(WritingClauseHandlerUnitTest, Set_MultipleChainedSets) {
	// Tests multiple SET clauses in sequence
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) SET n.value = 10 SET n.value = 20 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "20");
}

TEST_F(WritingClauseHandlerUnitTest, Set_WithLiteralValues) {
	// Tests SET with literal values (not expressions)
	(void) execute("CREATE (n:Test {a: true})");

	auto res = execute("MATCH (n:Test) SET n.result = false RETURN n.result");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.result").toString(), "false");
}

TEST_F(WritingClauseHandlerUnitTest, Set_WithIntegerLiteral) {
	// Tests SET with integer literal
	(void) execute("CREATE (n:Test {a: 10})");

	auto res = execute("MATCH (n:Test) SET n.sum = 15 RETURN n.sum");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.sum").toString(), "15");
}

TEST_F(WritingClauseHandlerUnitTest, Set_WithDoubleLiteral) {
	// Tests SET with double literal
	(void) execute("CREATE (n:Test {a: 10})");

	auto res = execute("MATCH (n:Test) SET n.result = 30.5 RETURN n.result");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.result").toString(), "30.5");
}

// ============================================================================
// DELETE Clause Edge Cases
// ============================================================================

TEST_F(WritingClauseHandlerUnitTest, Delete_WithoutPrecedingMatch) {
	// Tests DELETE without preceding MATCH clause
	// This should throw an error
	EXPECT_THROW({
		execute("DELETE n");
	}, std::runtime_error);
}

TEST_F(WritingClauseHandlerUnitTest, Delete_DetachVsNonDetach) {
	// Tests difference between DELETE and DETACH DELETE
	// First test non-detach (should fail if relationships exist)
	(void) execute("CREATE (a:Test)-[:LINK]->(b:Test)");

	EXPECT_THROW({
		execute("MATCH (a:Test) DELETE a");
	}, std::runtime_error);
}

TEST_F(WritingClauseHandlerUnitTest, Delete_DetachWithRelationships) {
	// Tests DETACH DELETE with relationships
	(void) execute("CREATE (a:Test)-[:LINK]->(b:Test)");

	auto res = execute("MATCH (a:Test) DETACH DELETE a");
	EXPECT_GE(res.rowCount(), 1UL);

	// Verify nodes are deleted - count(*) may return 0 rows if no nodes exist
	auto res2 = execute("MATCH (n:Test) RETURN n");
	EXPECT_EQ(res2.rowCount(), 0UL);
}

TEST_F(WritingClauseHandlerUnitTest, Delete_MultipleVariables) {
	// Tests DELETE with multiple variables
	(void) execute("CREATE (a:Test1)");
	(void) execute("CREATE (b:Test2)");

	auto res = execute("MATCH (a:Test1), (b:Test2) DELETE a, b");
	EXPECT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// REMOVE Clause Edge Cases
// ============================================================================

TEST_F(WritingClauseHandlerUnitTest, Remove_WithoutPrecedingClause) {
	// Tests REMOVE without preceding MATCH or CREATE
	// This should throw an error
	EXPECT_THROW({
		execute("REMOVE n:Label");
	}, std::runtime_error);
}

TEST_F(WritingClauseHandlerUnitTest, Remove_PropertyFromNonExistentProperty) {
	// Tests REMOVE property that doesn't exist
	(void) execute("CREATE (n:Test {id: 1})");

	// Removing non-existent property should not error
	auto res = execute("MATCH (n:Test) REMOVE n.nonexistent RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerUnitTest, Remove_LabelFromNonExistentLabel) {
	// Tests REMOVE label that node doesn't have
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) REMOVE n:NonExistent RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerUnitTest, Remove_MultipleProperties) {
	// Tests REMOVE multiple properties
	(void) execute("CREATE (n:Test {a: 1, b: 2, c: 3})");

	auto res = execute("MATCH (n:Test) REMOVE n.a, n.b RETURN n.c");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.c").toString(), "3");
}

TEST_F(WritingClauseHandlerUnitTest, Remove_PropertyAndLabel) {
	// Tests REMOVE both property and label
	(void) execute("CREATE (n:Test:Label {value: 100})");

	auto res = execute("MATCH (n:Test) REMOVE n.value, n:Label RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// MERGE Clause Edge Cases
// ============================================================================

TEST_F(WritingClauseHandlerUnitTest, Merge_OnlyOnCreate) {
	// Tests MERGE with only ON CREATE clause
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	// First run - node exists, ON CREATE not triggered
	auto res1 = execute("MERGE (n:Person {name: 'Alice'}) ON CREATE SET n.created = true RETURN n.created");
	EXPECT_EQ(res1.rowCount(), 1UL);

	// Second run - new node, ON CREATE triggered
	auto res2 = execute("MERGE (n:Person {name: 'Bob'}) ON CREATE SET n.created = true RETURN n.created");
	ASSERT_EQ(res2.rowCount(), 1UL);
	EXPECT_EQ(res2.getRows()[0].at("n.created").toString(), "true");
}

TEST_F(WritingClauseHandlerUnitTest, Merge_OnlyOnMatch) {
	// Tests MERGE with only ON MATCH clause
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	// Node exists, ON MATCH should trigger
	auto res = execute("MERGE (n:Person {name: 'Alice'}) ON MATCH SET n.updated = true RETURN n.updated");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.updated").toString(), "true");

	// New node, ON MATCH not triggered
	auto res2 = execute("MERGE (n:Person {name: 'Bob'}) ON MATCH SET n.updated = true RETURN n.updated");
	EXPECT_EQ(res2.rowCount(), 1UL);
	// n.updated should not exist or be null
}

TEST_F(WritingClauseHandlerUnitTest, Merge_BothOnCreateAndOnMatch) {
	// Tests MERGE with both ON CREATE and ON MATCH
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	// Existing node - ON MATCH triggers
	auto res1 = execute("MERGE (n:Person {name: 'Alice'}) ON CREATE SET n.new = true ON MATCH SET n.existing = true RETURN n.new, n.existing");
	ASSERT_EQ(res1.rowCount(), 1UL);
	EXPECT_EQ(res1.getRows()[0].at("n.existing").toString(), "true");

	// New node - ON CREATE triggers
	auto res2 = execute("MERGE (n:Person {name: 'Bob'}) ON CREATE SET n.new = true ON MATCH SET n.existing = true RETURN n.new, n.existing");
	ASSERT_EQ(res2.rowCount(), 1UL);
	EXPECT_EQ(res2.getRows()[0].at("n.new").toString(), "true");
}

TEST_F(WritingClauseHandlerUnitTest, Merge_MultipleSetItemsInOnCreate) {
	// Tests MERGE with multiple SET items in ON CREATE
	auto res = execute("MERGE (n:Person {name: 'Charlie'}) ON CREATE SET n.created = true, n.timestamp = 123 RETURN n.created, n.timestamp");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.created").toString(), "true");
	EXPECT_EQ(res.getRows()[0].at("n.timestamp").toString(), "123");
}

TEST_F(WritingClauseHandlerUnitTest, Merge_MultipleSetItemsInOnMatch) {
	// Tests MERGE with multiple SET items in ON MATCH
	(void) execute("CREATE (n:Person {name: 'David'})");

	auto res = execute("MERGE (n:Person {name: 'David'}) ON MATCH SET n.updated = true, n.count = 1 RETURN n.updated, n.count");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.updated").toString(), "true");
	EXPECT_EQ(res.getRows()[0].at("n.count").toString(), "1");
}

// ============================================================================
// CREATE Clause Edge Cases
// ============================================================================

TEST_F(WritingClauseHandlerUnitTest, Create_WithNullProperties) {
	// Tests CREATE with null property values
	auto res = execute("CREATE (n:Test {value: null}) RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").asPrimitive().getType(), graph::PropertyType::NULL_TYPE);
}

TEST_F(WritingClauseHandlerUnitTest, Create_MultipleNodesInSinglePattern) {
	// Tests CREATE with multiple nodes connected by edges
	auto res = execute("CREATE (a:Test)-[:LINK]->(b:Test)-[:LINK]->(c:Test) RETURN a, b, c");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerUnitTest, Create_WithDuplicateLabels) {
	// Tests CREATE with duplicate labels (same label specified multiple times)
	// This might be handled as single label or error
	auto res = execute("CREATE (n:Test:Test) RETURN n");
	EXPECT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// Combined Clause Tests
// ============================================================================

TEST_F(WritingClauseHandlerUnitTest, CreateThenSet) {
	// Tests CREATE followed by SET
	auto res = execute("CREATE (n:Test) SET n.value = 100 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
}

TEST_F(WritingClauseHandlerUnitTest, CreateThenRemove) {
	// Tests CREATE followed by REMOVE
	(void) execute("CREATE (n:Test:Temp {value: 100})");
	auto res = execute("MATCH (n:Test) REMOVE n:Temp RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(WritingClauseHandlerUnitTest, MatchThenDelete) {
	// Tests MATCH followed by DELETE
	(void) execute("CREATE (n:Test {id: 1})");
	auto res = execute("MATCH (n:Test) DELETE n");
	EXPECT_EQ(res.rowCount(), 1UL);

	// Verify deletion - check directly without count(*)
	auto res2 = execute("MATCH (n:Test) RETURN n");
	EXPECT_EQ(res2.rowCount(), 0UL);
}

TEST_F(WritingClauseHandlerUnitTest, MergeThenSet) {
	// Tests MERGE followed by SET
	(void) execute("CREATE (n:Person {name: 'Eve'})");
	auto res = execute("MERGE (n:Person {name: 'Eve'}) SET n.age = 30 RETURN n.age");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.age").toString(), "30");
}
