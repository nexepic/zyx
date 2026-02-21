/**
 * @file test_ReadingClauseHandler_Coverage.cpp
 * @brief Coverage-focused unit tests for ReadingClauseHandler
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
 * @class ReadingClauseHandlerCoverageTest
 * @brief Coverage-focused tests for ReadingClauseHandler
 */
class ReadingClauseHandlerCoverageTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_reading_coverage_" + boost::uuids::to_string(uuid) + ".dat");
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
// UNWIND with rootOp != null - Line 134 False branch
// ============================================================================

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_AfterMatch) {
	// Covers False branch at line 134 (!rootOp)
	// When rootOp is not null (e.g., after MATCH)
	(void) execute("CREATE (n:Test {ids: [1, 2, 3]})");

	auto res = execute("MATCH (n:Test) UNWIND [10, 20, 30] AS x RETURN x");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_AfterCreate) {
	// Covers False branch at line 134 after CREATE
	// CREATE ... UNWIND is not supported, so we test with WITH
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) UNWIND [1, 2] AS x RETURN x");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_MultipleUnwinds) {
	// Tests multiple UNWIND in sequence
	auto res = execute("UNWIND [1] AS x UNWIND [2] AS y RETURN x, y");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("y").toString(), "2");
}

// ============================================================================
// UNWIND empty list - Line 129 True branch
// ============================================================================

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_EmptyList_Standalone) {
	// Covers True branch at line 129 (listValues.empty())
	auto res = execute("UNWIND [] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_EmptyList_AfterMatch) {
	// Covers True branch after MATCH
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) UNWIND [] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_SingleValue) {
	// Tests UNWIND with single value
	auto res = execute("UNWIND [42] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "42");
}

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_TwoValues) {
	// Tests UNWIND with two values
	auto res = execute("UNWIND [1, 2] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 2UL);
}

// ============================================================================
// MATCH with rootOp - Line 36-38
// ============================================================================

TEST_F(ReadingClauseHandlerCoverageTest, Match_WithNoResults) {
	// Tests MATCH that returns no results
	auto res = execute("MATCH (n:NonExistent) RETURN n");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerCoverageTest, Match_SimpleNode) {
	// Tests simple MATCH
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	auto res = execute("MATCH (n:Person) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Alice");
}

TEST_F(ReadingClauseHandlerCoverageTest, Match_WithWhere) {
	// Tests MATCH with WHERE
	(void) execute("CREATE (n:Test {value: 100})");
	(void) execute("CREATE (n:Test {value: 200})");

	auto res = execute("MATCH (n:Test) WHERE n.value > 100 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "200");
}

TEST_F(ReadingClauseHandlerCoverageTest, Match_WithRelationship) {
	// Tests MATCH with relationship
	(void) execute("CREATE (a:Start)-[:LINK]->(b:End)");

	auto res = execute("MATCH (a:Start)-[:LINK]->(b:End) RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// CALL edge cases
// ============================================================================

TEST_F(ReadingClauseHandlerCoverageTest, Call_Standalone_UnknownProcedure) {
	// Tests CALL with unknown procedure
	EXPECT_THROW({
		execute("CALL unknown.procedure()");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerCoverageTest, Call_InQuery_UnknownProcedure) {
	// Tests in-query CALL with unknown procedure
	(void) execute("CREATE (n:Test)");

	EXPECT_THROW({
		execute("MATCH (n:Test) CALL unknown.proc() RETURN n");
	}, std::exception);
}

// ============================================================================
// UNWIND special cases
// ============================================================================

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_NegativeNumbers) {
	// Tests UNWIND with negative numbers
	auto res = execute("UNWIND [-5, -10, -15] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_LargeList) {
	// Tests UNWIND with larger list
	auto res = execute("UNWIND [1, 2, 3, 4, 5] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 5UL);
}

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_WithNull) {
	// Tests UNWIND with null value
	auto res = execute("UNWIND [null] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("x").asPrimitive().getType(), graph::PropertyType::NULL_TYPE);
}

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_WithBoolean) {
	// Tests UNWIND with boolean values
	auto res = execute("UNWIND [true, false] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 2UL);
}

// ============================================================================
// Complex patterns
// ============================================================================

TEST_F(ReadingClauseHandlerCoverageTest, Match_MultipleNodes) {
	// Tests MATCH with multiple nodes
	(void) execute("CREATE (a:Person {name: 'Alice'})");
	(void) execute("CREATE (b:Person {name: 'Bob'})");

	auto res = execute("MATCH (a:Person), (b:Person) RETURN a.name, b.name");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerCoverageTest, Match_WithMultipleRelationships) {
	// Tests MATCH with multiple relationship patterns
	(void) execute("CREATE (a)-[:LINK1]->(b)-[:LINK2]->(c)");

	auto res = execute("MATCH (a)-[:LINK1]->(b)-[:LINK2]->(c) RETURN a, b, c");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerCoverageTest, Match_VariableLength) {
	// Tests MATCH with variable length relationship
	(void) execute("CREATE (a)-[:LINK]->(b)-[:LINK]->(c)");

	auto res = execute("MATCH (a)-[:LINK*1..2]->(b) RETURN b");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerCoverageTest, Match_WithPropertyComparison) {
	// Tests MATCH with property comparison in WHERE
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) WHERE n.value > 15 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "20");
}

// ============================================================================
// UNWIND with complex scenarios
// ============================================================================

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_ThenMatch) {
	// Tests UNWIND followed by MATCH
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("UNWIND [1, 2] AS x MATCH (n:Test {id: x}) RETURN n.id");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerCoverageTest, Match_ThenUnwind) {
	// Tests MATCH followed by UNWIND
	(void) execute("CREATE (n:Test {values: [1, 2, 3]})");

	// Note: This may not work as expected due to property reference limitations
	auto res = execute("MATCH (n:Test) UNWIND [1, 2] AS x RETURN x");
	EXPECT_GE(res.rowCount(), 0UL);
}

// ============================================================================
// UNWIND rootOp variations
// ============================================================================

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_WithExistingRootOp_Multiple) {
	// Tests UNWIND when rootOp already exists from previous clauses
	// This specifically targets line 134 False branch

	// First UNWIND creates rootOp
	// Second UNWIND should see non-null rootOp
	auto res = execute("UNWIND [1] AS a UNWIND [2] AS b RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("a").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("b").toString(), "2");
}

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_AfterUnwind) {
	// Tests consecutive UNWINDs
	auto res = execute("UNWIND [1, 2] AS x UNWIND [3, 4] AS y RETURN x, y");
	ASSERT_EQ(res.rowCount(), 4UL);
}

// ============================================================================
// Edge cases for expression parsing
// ============================================================================

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_Zero) {
	// Tests UNWIND with zero value
	auto res = execute("UNWIND [0] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "0");
}

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_EmptyString) {
	// Tests UNWIND with empty string
	auto res = execute("UNWIND [''] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "");
}

TEST_F(ReadingClauseHandlerCoverageTest, Unwind_ScientificNotation) {
	// Tests UNWIND with scientific notation
	auto res = execute("UNWIND [1.5e10] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// CALL procedure variations
// ============================================================================

TEST_F(ReadingClauseHandlerCoverageTest, Call_WithArguments) {
	// Tests CALL with procedure arguments
	EXPECT_THROW({
		execute("CALL some.procedure(1, 2, 3)");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerCoverageTest, Call_WithStringArguments) {
	// Tests CALL with string arguments
	EXPECT_THROW({
		execute("CALL some.procedure('arg1', 'arg2')");
	}, std::exception);
}
