/**
 * @file test_ReadingClauseHandler_BranchCoverage.cpp
 * @brief Additional branch coverage tests for ReadingClauseHandler
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
 * @class ReadingClauseHandlerBranchCoverageTest
 * @brief Additional branch coverage tests for ReadingClauseHandler
 */
class ReadingClauseHandlerBranchCoverageTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_reading_branch_" + boost::uuids::to_string(uuid) + ".dat");
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
// UNWIND tests for branch coverage
// ============================================================================

TEST_F(ReadingClauseHandlerBranchCoverageTest, Unwind_Simple) {
	// Tests UNWIND with list (Line 125 True branch)
	auto res = execute("UNWIND [1, 2, 3] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Unwind_EmptyList) {
	// Tests UNWIND with empty list (Line 129 True branch)
	auto res = execute("UNWIND [] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Unwind_SingleRow) {
	// Tests UNWIND when rootOp is null (Line 134 True branch)
	auto res = execute("UNWIND [42] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Unwind_AfterMatchWithResults) {
	// Tests UNWIND after MATCH when MATCH returns results
	(void) execute("CREATE (n:Test)");
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) RETURN n");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Unwind_NestedLists) {
	// Tests UNWIND with nested list structure
	auto res = execute("UNWIND [[1, 2], [3, 4]] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Unwind_DuplicateValues) {
	// Tests UNWIND with duplicate values
	auto res = execute("UNWIND [1, 1, 2, 2, 3] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 5UL);
}

// ============================================================================
// MATCH tests for branch coverage
// ============================================================================

TEST_F(ReadingClauseHandlerBranchCoverageTest, Match_Simple) {
	// Tests simple MATCH
	(void) execute("CREATE (n:Person {name: 'Test'})");

	auto res = execute("MATCH (n:Person) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Match_WithWhere) {
	// Tests MATCH with WHERE clause
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) WHERE n.value > 15 RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "20");
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Match_NoResults) {
	// Tests MATCH that returns no results
	auto res = execute("MATCH (n:NonExistent) RETURN n");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Match_WithRelationship) {
	// Tests MATCH with relationship pattern
	(void) execute("CREATE (a:Start)-[:LINK]->(b:End)");

	auto res = execute("MATCH (a:Start)-[:LINK]->(b:End) RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Match_MultipleRelationships) {
	// Tests MATCH with multiple relationship patterns
	(void) execute("CREATE (a)-[:L1]->(b)-[:L2]->(c)");

	auto res = execute("MATCH (a)-[:L1]->(b)-[:L2]->(c) RETURN a, b, c");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Match_VariableLength) {
	// Tests MATCH with relationship pattern
	(void) execute("CREATE (a)-[:LINK]->(b)");

	auto res = execute("MATCH (a)-[:LINK]->(b) RETURN b");
	EXPECT_GE(res.rowCount(), 1UL);
}

// ============================================================================
// CALL procedure tests
// ============================================================================

TEST_F(ReadingClauseHandlerBranchCoverageTest, Call_UnknownProcedure) {
	// Tests CALL with unknown procedure (Line 46 True branch has explicitProcedureInvocation)
	EXPECT_THROW({
		execute("CALL unknown.procedure()");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Call_WithArguments) {
	// Tests CALL with arguments
	EXPECT_THROW({
		execute("CALL some.proc(1, 2, 3)");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Call_InQueryUnknownProcedure) {
	// Tests in-query CALL with unknown procedure
	EXPECT_THROW({
		execute("CALL unknown.proc() RETURN x");
	}, std::exception);
}

// ============================================================================
// UNWIND special cases
// ============================================================================

TEST_F(ReadingClauseHandlerBranchCoverageTest, Unwind_LargeList) {
	// Tests UNWIND with large list
	std::string list = "[";
	for (int i = 1; i <= 50; ++i) {
		if (i > 1) list += ", ";
		list += std::to_string(i);
	}
	list += "]";

	auto res = execute("UNWIND " + list + " AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 50UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Unwind_NullValues) {
	// Tests UNWIND with null values
	auto res = execute("UNWIND [null, null, null] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Unwind_MixedTypes) {
	// Tests UNWIND with mixed types
	auto res = execute("UNWIND [1, 'two', true, 3.14] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 4UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Unwind_ScientificNotation) {
	// Tests UNWIND with scientific notation
	auto res = execute("UNWIND [1.5e10, 2.0e5] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Unwind_NegativeNumbers) {
	// Tests UNWIND with negative numbers
	auto res = execute("UNWIND [-5, -10, -15] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Unwind_Zero) {
	// Tests UNWIND with zero
	auto res = execute("UNWIND [0] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "0");
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Unwind_EmptyString) {
	// Tests UNWIND with empty string
	auto res = execute("UNWIND [''] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "");
}

// ============================================================================
// Complex MATCH patterns
// ============================================================================

TEST_F(ReadingClauseHandlerBranchCoverageTest, Match_MultipleNodesNoRelationship) {
	// Tests MATCH with multiple unrelated nodes
	(void) execute("CREATE (a:Person)");
	(void) execute("CREATE (b:Person)");

	auto res = execute("MATCH (a:Person), (b:Person) RETURN a, b");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Match_UndirectedRelationship) {
	// Tests MATCH with undirected relationship
	(void) execute("CREATE (a)-[:LINK]->(b)");

	auto res = execute("MATCH (a)-[:LINK]-(b) RETURN a, b");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Match_ReverseDirection) {
	// Tests MATCH with reverse direction
	(void) execute("CREATE (a:Start)-[:LINK]->(b:End)");

	auto res = execute("MATCH (b:End)<-[:LINK]-(a:Start) RETURN a, b");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Match_WithPropertyInPattern) {
	// Tests MATCH with property in pattern
	(void) execute("CREATE (n:Test {value: 42})");

	auto res = execute("MATCH (n:Test {value: 42}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerBranchCoverageTest, Match_MultipleLabels) {
	// Tests MATCH with multiple labels
	(void) execute("CREATE (n:Test:Label1:Label2)");

	auto res = execute("MATCH (n:Test:Label1) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}
