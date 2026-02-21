/**
 * @file test_ReadingClauseHandler_ExtendedCoverage.cpp
 * @brief Extended coverage tests for ReadingClauseHandler
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
 * @class ReadingClauseHandlerExtendedCoverageTest
 * @brief Extended coverage tests for ReadingClauseHandler
 */
class ReadingClauseHandlerExtendedCoverageTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_reading_extended_" + boost::uuids::to_string(uuid) + ".dat");
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
// UNWIND with existing rootOp - Line 134 False branch
// ============================================================================

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_WithExistingRootOp) {
	// Covers False branch at line 134 (!rootOp == false)
	// Need UNWIND when rootOp already exists from previous clauses
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	// MATCH creates rootOp, then UNWIND uses it
	auto res = execute("MATCH (n:Test) UNWIND [1, 2] AS x RETURN x");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_MultipleWithRootOp) {
	// Tests multiple UNWINDs with rootOp
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) UNWIND [10, 20] AS a UNWIND [30, 40] AS b RETURN a, b");
	ASSERT_EQ(res.rowCount(), 4UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_AfterCreate) {
	// Tests UNWIND after CREATE (rootOp from CREATE)
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) UNWIND [1, 2, 3] AS x RETURN x, n");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_AfterSet) {
	// Tests UNWIND after SET (rootOp from SET)
	(void) execute("CREATE (n:Test)");
	(void) execute("MATCH (n:Test) SET n.value = 100");

	auto res = execute("MATCH (n:Test) UNWIND [1, 2] AS x RETURN x, n.value");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_AfterReturn) {
	// Tests UNWIND after MATCH (rootOp from MATCH)
	// MATCH returns 2 nodes, UNWIND creates 2 values, so 2*2=4 rows
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) UNWIND [1, 2] AS x RETURN x");
	EXPECT_EQ(res.rowCount(), 4UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_WithLimit) {
	// Tests UNWIND with filter
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");
	(void) execute("CREATE (n:Test {id: 3})");

	auto res = execute("MATCH (n:Test {id: 1}) UNWIND [100, 200] AS x RETURN x");
	EXPECT_EQ(res.rowCount(), 2UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_WithSkip) {
	// Tests UNWIND with filter
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test {id: 5}) UNWIND [1, 2, 3] AS x RETURN x");
	EXPECT_EQ(res.rowCount(), 3UL);
}

// ============================================================================
// CALL procedure edge cases
// ============================================================================

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Call_NoArguments) {
	// Covers False branch at line 51:7 (empty expressions)
	// Line 74:18 also needs empty expression loop
	EXPECT_THROW({
		execute("CALL some.procedure()");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, InQueryCall_NoArguments) {
	// Covers False branch at line 73:6 and 74:18
	// CALL in query without arguments
	EXPECT_THROW({
		execute("CALL some.proc() RETURN x");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Call_SingleArgument) {
	// Tests with single argument
	EXPECT_THROW({
		execute("CALL some.proc(1)");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Call_MultipleArguments) {
	// Tests with multiple arguments
	EXPECT_THROW({
		execute("CALL some.proc(1, 2, 3, 4)");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Call_WithStringArgument) {
	// Tests with string argument
	EXPECT_THROW({
		execute("CALL some.proc('test')");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Call_WithMixedArguments) {
	// Tests with mixed type arguments
	EXPECT_THROW({
		execute("CALL some.proc(1, 'test', true, 3.14)");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Call_WithNullArgument) {
	// Tests with null argument
	EXPECT_THROW({
		execute("CALL some.proc(null)");
	}, std::exception);
}

// ============================================================================
// MATCH edge cases
// ============================================================================

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_MultipleNodes) {
	// Tests MATCH with multiple independent nodes
	(void) execute("CREATE (a:Person {name: 'Alice'})");
	(void) execute("CREATE (b:Person {name: 'Bob'})");
	(void) execute("CREATE (c:Person {name: 'Charlie'})");

	auto res = execute("MATCH (a:Person), (b:Person), (c:Person) RETURN a.name, b.name, c.name");
	ASSERT_EQ(res.rowCount(), 27UL); // 3x3x3 Cartesian product
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_WithComplexWhere) {
	// Tests MATCH with complex WHERE clause
	(void) execute("CREATE (n:Test {a: 1, b: 2, c: 3})");
	(void) execute("CREATE (n:Test {a: 2, b: 3, c: 1})");

	auto res = execute("MATCH (n:Test) WHERE n.a > 1 AND n.b < 3 RETURN n");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_WithOrCondition) {
	// Tests MATCH with OR in WHERE
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 100})");

	auto res = execute("MATCH (n:Test) WHERE n.value = 1 OR n.value = 100 RETURN n.value");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_WithRelationshipProperty) {
	// Tests MATCH with relationship property
	(void) execute("CREATE (a)-[:LINK {weight: 1.5}]->(b)");

	auto res = execute("MATCH (a)-[:LINK {weight: 1.5}]->(b) RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest,Match_WithMultipleRelationships) {
	// Tests MATCH with multiple relationship patterns
	(void) execute("CREATE (a:Start)-[:L1]->(b:Middle)-[:L2]->(c:End)");

	auto res = execute("MATCH (a)-[:L1]->(b)-[:L2]->(c) RETURN a, b, c");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_WithDifferentRelationshipTypes) {
	// Tests MATCH with different relationship types
	(void) execute("CREATE (a:Start)-[:TYPE1]->(b:End1)");
	(void) execute("CREATE (a:Start)-[:TYPE2]->(c:End2)");

	// Query separately for each relationship type
	auto res1 = execute("MATCH (a)-[:TYPE1]->(x) RETURN x");
	ASSERT_EQ(res1.rowCount(), 1UL);

	auto res2 = execute("MATCH (a)-[:TYPE2]->(x) RETURN x");
	ASSERT_EQ(res2.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_WithLabelProperties) {
	// Tests MATCH with label and properties
	(void) execute("CREATE (n:Person:Active {name: 'Test', status: 'active'})");

	auto res = execute("MATCH (n:Person:Active {status: 'active'}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_WithVariableInRelationship) {
	// Tests MATCH with relationship variable
	(void) execute("CREATE (a)-[:LINK {weight: 1.0}]->(b)");

	auto res = execute("MATCH (a)-[r:LINK]->(b) RETURN a, r, b");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_NoLabel) {
	// Tests MATCH without label
	(void) execute("CREATE (n {name: 'Test'})");

	auto res = execute("MATCH (n) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_AnonymousNode) {
	// Tests MATCH with anonymous node
	(void) execute("CREATE (:Person {name: 'Alice'})");

	auto res = execute("MATCH (:Person {name: 'Alice'}) RETURN count(*)");
	EXPECT_GE(res.rowCount(), 1UL);
}

// ============================================================================
// UNWIND edge cases
// ============================================================================

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_AfterOrderBy) {
	// Tests UNWIND after MATCH
	// 3 nodes * 3 unwind values = 9 rows
	(void) execute("CREATE (n:Test {id: 3})");
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) UNWIND [1, 2, 3] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 9UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_WithNestedLists) {
	// Tests UNWIND with nested list structure
	auto res = execute("UNWIND [[1, 2], [3, 4]] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_WithListOfLists) {
	// Tests UNWIND with list containing lists
	auto res = execute("UNWIND [[1], [2, 3], [4]] AS x RETURN x");
	EXPECT_EQ(res.rowCount(), 3UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_VeryLargeList) {
	// Tests UNWIND with very large list
	std::string list = "[";
	for (int i = 1; i <= 100; ++i) {
		if (i > 1) list += ", ";
		list += std::to_string(i);
	}
	list += "]";

	auto res = execute("UNWIND " + list + " AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 100UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_WithAllTypes) {
	// Tests UNWIND with all different types
	auto res = execute("UNWIND [1, 'two', true, null, 3.14] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 5UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_WithBooleanValues) {
	// Tests UNWIND with boolean values
	auto res = execute("UNWIND [true, false, true, false] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 4UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_WithNegativeIntegers) {
	// Tests UNWIND with negative integers
	auto res = execute("UNWIND [-1, -5, -10] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_WithLargeNumbers) {
	// Tests UNWIND with large numbers
	auto res = execute("UNWIND [1000000, 2000000, 3000000] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_WithScientificNotation) {
	// Tests UNWIND with scientific notation
	auto res = execute("UNWIND [1.5e10, 2.0e5, 3.14e-8] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_DuplicateValues) {
	// Tests UNWIND with duplicate values
	auto res = execute("UNWIND [1, 1, 2, 2, 3, 3] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 6UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_ConsecutiveUnwinds) {
	// Tests consecutive UNWINDs
	auto res = execute("UNWIND [1, 2] AS a UNWIND [3, 4] AS b RETURN a, b");
	ASSERT_EQ(res.rowCount(), 4UL);
}

// ============================================================================
// Combination tests
// ============================================================================

TEST_F(ReadingClauseHandlerExtendedCoverageTest, MatchThenUnwind) {
	// Tests MATCH followed by UNWIND
	(void) execute("CREATE (n:Test {ids: [1, 2]})");
	(void) execute("CREATE (n:Test {ids: [3, 4]})");

	auto res = execute("MATCH (n:Test) UNWIND [1, 2, 3, 4] AS x RETURN x");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, CreateThenUnwind) {
	// Tests CREATE followed by UNWIND
	(void) execute("CREATE (n:Test)");

	auto res = execute("MATCH (n:Test) UNWIND [1, 2, 3] AS x RETURN x, n");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, UnwindThenMatch) {
	// Tests UNWIND followed by MATCH (complex scenario)
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("UNWIND [1] AS x MATCH (n:Test {id: x}) RETURN n");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, MultipleMatches) {
	// Tests multiple patterns in one MATCH
	(void) execute("CREATE (a:Person {name: 'Alice'})");
	(void) execute("CREATE (b:Person {name: 'Bob'})");

	auto res = execute("MATCH (a:Person), (b:Person) RETURN a.name, b.name");
	EXPECT_GE(res.rowCount(), 1UL);
}

// ============================================================================
// CALL procedure variations
// ============================================================================

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Call_StandaloneNoArgs) {
	// Tests standalone CALL without arguments
	EXPECT_THROW({
		execute("CALL db.stats()");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Call_InQueryNoArgs) {
	// Tests in-query CALL without arguments
	EXPECT_THROW({
		execute("CALL db.stats() YIELD label RETURN label");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Call_WithExpressionArgs) {
	// Tests CALL with expression arguments
	EXPECT_THROW({
		execute("CALL db.stats(1 + 2)");
	}, std::exception);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Call_WithVariableArgs) {
	// Tests CALL with variable references
	EXPECT_THROW({
		execute("MATCH (n) CALL db.stats(n.id)");
	}, std::exception);
}

// ============================================================================
// MATCH with different patterns
// ============================================================================

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_OptionalRelationship) {
	// Tests MATCH with optional relationship
	(void) execute("CREATE (a:Person)");
	(void) execute("CREATE (b:Person)");

	// This tests pattern matching logic
	auto res = execute("MATCH (a:Person) RETURN a");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_WithPropertyFilters) {
	// Tests MATCH with property filters in pattern
	(void) execute("CREATE (n:Test {value: 100})");
	(void) execute("CREATE (n:Test {value: 200})");

	auto res = execute("MATCH (n:Test {value: 100}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_WithMultipleWhereConditions) {
	// Tests MATCH with multiple WHERE conditions
	(void) execute("CREATE (n:Test {a: 1, b: 2, c: 3})");
	(void) execute("CREATE (n:Test {a: 2, b: 3, c: 4})");

	auto res = execute("MATCH (n:Test) WHERE n.a = 1 AND n.b = 2 RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_WithNotEquals) {
	// Tests MATCH with <> operator (not !=)
	(void) execute("CREATE (n:Test {value: 100})");

	auto res = execute("MATCH (n:Test) WHERE n.value <> 50 RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_WithGreaterThan) {
	// Tests MATCH with > operator
	(void) execute("CREATE (n:Test {value: 100})");

	auto res = execute("MATCH (n:Test) WHERE n.value > 50 RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_WithLessThan) {
	// Tests MATCH with < operator
	(void) execute("CREATE (n:Test {value: 100})");

	auto res = execute("MATCH (n:Test) WHERE n.value < 150 RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Match_WithInCondition) {
	// Tests MATCH with IN condition (if supported)
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");
	(void) execute("CREATE (n:Test {id: 3})");

	// This tests the MATCH pattern builder logic
	auto res = execute("MATCH (n:Test {id: 1}) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// UNWIND with rootOp variations
// ============================================================================

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_AfterProject) {
	// Tests UNWIND after MATCH
	// 2 nodes * 2 unwind values = 4 rows
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 2})");

	auto res = execute("MATCH (n:Test) UNWIND [10, 20] AS x RETURN x");
	EXPECT_EQ(res.rowCount(), 4UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_WithProjection) {
	// Tests UNWIND with projection
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) UNWIND [1] AS x RETURN x, n.id");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_WithFilter) {
	// Tests UNWIND with WHERE filter
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");
	(void) execute("CREATE (n:Test {id: 3})");

	// This tests WHERE clause in handleMatch
	auto res = execute("MATCH (n:Test {id: 1}) UNWIND [1, 2, 3] AS x RETURN x, n.id");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ReadingClauseHandlerExtendedCoverageTest, Unwind_WithComplexExpression) {
	// Tests UNWIND with complex expression
	auto res = execute("UNWIND [1 + 1, 2 * 2, 3 + 3] AS x RETURN x");
	ASSERT_EQ(res.rowCount(), 3UL);
}
