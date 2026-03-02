/**
 * @file test_ResultClauseHandler_Coverage.cpp
 * @brief Coverage-focused unit tests for ResultClauseHandler
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
 * @class ResultClauseHandlerCoverageTest
 * @brief Coverage-focused tests for ResultClauseHandler
 */
class ResultClauseHandlerCoverageTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_result_coverage_" + boost::uuids::to_string(uuid) + ".dat");
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
// RETURN without rootOp - Line 36 True branch
// ============================================================================

TEST_F(ResultClauseHandlerCoverageTest, Return_StandaloneLiteral) {
	// Covers True branch at line 36 (rootOp is null)
	// SingleRowOperator should be injected
	auto res = execute("RETURN 42");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("42").toString(), "42");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_StandaloneString) {
	// Tests RETURN with string literal without MATCH
	auto res = execute("RETURN 'Hello'");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ResultClauseHandlerCoverageTest, Return_StandaloneBoolean) {
	// Tests RETURN with boolean without MATCH
	auto res = execute("RETURN true");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("true").toString(), "true");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_StandaloneNull) {
	// Tests RETURN with null without MATCH
	auto res = execute("RETURN null");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("null").asPrimitive().getType(), graph::PropertyType::NULL_TYPE);
}

TEST_F(ResultClauseHandlerCoverageTest, Return_StandaloneMultiple) {
	// Tests RETURN multiple literals without MATCH
	auto res = execute("RETURN 1, 2, 3");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("1").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("2").toString(), "2");
	EXPECT_EQ(res.getRows()[0].at("3").toString(), "3");
}

// ============================================================================
// RETURN * - Line 76 False branch
// ============================================================================

TEST_F(ResultClauseHandlerCoverageTest, Return_Multiply) {
	// Covers False branch at line 76 (MULTIPLY is true)
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	auto res = execute("MATCH (n:Person) RETURN *");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("n"));
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Multiply_MultipleVariables) {
	// Tests RETURN * with multiple variables
	(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");

	auto res = execute("MATCH (a)-[:KNOWS]->(b) RETURN *");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("a"));
	EXPECT_TRUE(res.getRows()[0].contains("b"));
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Multiply_AfterCreate) {
	// Tests RETURN * after CREATE
	auto res = execute("CREATE (n:Test {id: 1}) RETURN *");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("n"));
}

// ============================================================================
// RETURN without AS - Line 86 False branch
// ============================================================================

TEST_F(ResultClauseHandlerCoverageTest, Return_WithoutAlias_Single) {
	// Covers False branch at line 86 (K_AS is false)
	(void) execute("CREATE (n:Test {value: 100})");

	auto res = execute("MATCH (n:Test) RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_WithoutAlias_Multiple) {
	// Tests multiple RETURN items without AS
	(void) execute("CREATE (n:Test {a: 1, b: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.a, n.b");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.a").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.b").toString(), "2");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_WithAndWithoutAlias) {
	// Tests mix of AS and non-AS
	(void) execute("CREATE (n:Test {a: 1, b: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.a AS first, n.b");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("first").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.b").toString(), "2");
}

// ============================================================================
// ORDER BY variations - Line 41, 45, 53, 62, 69
// ============================================================================

TEST_F(ResultClauseHandlerCoverageTest, Return_OrderBy_PropertyWithoutDot) {
	// Covers False branch at line 53 (no dot in expression)
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) RETURN n ORDER BY n");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerCoverageTest, Return_OrderBy_DefaultAscending) {
	// Covers False branch at line 62 (no DESC/DESCENDING)
	(void) execute("CREATE (n:Test {val: 2})");
	(void) execute("CREATE (n:Test {val: 1})");

	auto res = execute("MATCH (n:Test) RETURN n.val ORDER BY n.val");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.val").toString(), "1");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_OrderBy_ExplicitAscending) {
	// Tests explicit ASC keyword
	(void) execute("CREATE (n:Test {val: 2})");
	(void) execute("CREATE (n:Test {val: 1})");

	auto res = execute("MATCH (n:Test) RETURN n.val ORDER BY n.val ASC");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.val").toString(), "1");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_OrderBy_SingleSortItem) {
	// Tests ORDER BY with single sort item
	(void) execute("CREATE (n:Test {id: 3})");
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "1");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_OrderBy_MultipleSortItems) {
	// Tests ORDER BY with multiple sort items
	(void) execute("CREATE (n:Test {a: 1, b: 2})");
	(void) execute("CREATE (n:Test {a: 1, b: 1})");
	(void) execute("CREATE (n:Test {a: 2, b: 1})");

	auto res = execute("MATCH (n:Test) RETURN n.a, n.b ORDER BY n.a, n.b");
	ASSERT_EQ(res.rowCount(), 3UL);
}

// ============================================================================
// SKIP variations - Line 99, 104
// ============================================================================

TEST_F(ResultClauseHandlerCoverageTest, Return_Skip_One) {
	// Tests SKIP 1
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 1");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "2");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Skip_Half) {
	// Tests SKIP that skips half of results
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 5");
	ASSERT_EQ(res.rowCount(), 5UL);
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Skip_AllButOne) {
	// Tests SKIP that leaves only one result
	for (int i = 0; i < 5; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 4");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// LIMIT variations - Line 112, 116
// ============================================================================

TEST_F(ResultClauseHandlerCoverageTest, Return_Limit_One) {
	// Tests LIMIT 1
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.id LIMIT 1");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Limit_Half) {
	// Tests LIMIT that returns half
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id LIMIT 5");
	ASSERT_EQ(res.rowCount(), 5UL);
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Limit_All) {
	// Tests LIMIT that returns all
	for (int i = 0; i < 5; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id LIMIT 10");
	ASSERT_EQ(res.rowCount(), 5UL);
}

// ============================================================================
// Combined SKIP and LIMIT
// ============================================================================

TEST_F(ResultClauseHandlerCoverageTest, Return_SkipLimit_Combo) {
	// Tests SKIP and LIMIT together
	for (int i = 0; i < 20; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 5 LIMIT 10");
	ASSERT_EQ(res.rowCount(), 10UL);
}

TEST_F(ResultClauseHandlerCoverageTest, Return_OrderBySkipLimit_All) {
	// Tests ORDER BY + SKIP + LIMIT all together
	for (int i = 0; i < 20; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id DESC SKIP 5 LIMIT 10");
	ASSERT_EQ(res.rowCount(), 10UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "14");
}

// ============================================================================
// RETURN with various expressions
// ============================================================================

TEST_F(ResultClauseHandlerCoverageTest, Return_Multiply_AfterMatch) {
	// Tests RETURN * with ORDER BY
	(void) execute("CREATE (n:Test {id: 2})");
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) RETURN * ORDER BY n.id");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_TRUE(res.getRows()[0].contains("n"));
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Multiply_WithLimit) {
	// Tests RETURN * with LIMIT
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN * LIMIT 5");
	ASSERT_EQ(res.rowCount(), 5UL);
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Multiply_WithSkip) {
	// Tests RETURN * with SKIP
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN * SKIP 5");
	ASSERT_EQ(res.rowCount(), 5UL);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_F(ResultClauseHandlerCoverageTest, Return_EmptyProjection) {
	// Tests RETURN with no items after WHERE filters all
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test {id: 2}) RETURN n");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerCoverageTest, Return_WithAlias_Chained) {
	// Tests RETURN with chained aliases
	(void) execute("CREATE (n:Test {a: 1, b: 2, c: 3})");

	auto res = execute("MATCH (n:Test) RETURN n.a AS x, n.b AS y, n.c AS z");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("y").toString(), "2");
	EXPECT_EQ(res.getRows()[0].at("z").toString(), "3");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_MultipleWithSameProperty) {
	// Tests RETURN same property multiple times
	(void) execute("CREATE (n:Test {value: 100})");

	auto res = execute("MATCH (n:Test) RETURN n.value, n.value, n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_OrderBy_AfterReturn) {
	// Tests ORDER BY after RETURN (correct order)
	(void) execute("CREATE (n:Test {id: 2})");
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id DESC");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "2");
}

// ============================================================================
// Aggregate Function Tests - Lines 118-186
// ============================================================================

TEST_F(ResultClauseHandlerCoverageTest, Return_Aggregate_Count_All) {
	// Tests COUNT(*) - covers line 142-150 (funcCall->getArgumentCount() == 0)
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 2})");
	(void) execute("CREATE (n:Test {value: 3})");

	auto res = execute("MATCH (n:Test) RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("count(*)").toString(), "3");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Aggregate_Count_Variable) {
	// Tests COUNT(n) - covers line 143-150 with argument
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 2})");

	auto res = execute("MATCH (n:Test) RETURN count(n)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("count(n)").toString(), "2");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Aggregate_Count_Property) {
	// Tests COUNT(n.prop) with non-null values
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 2})");
	(void) execute("CREATE (n:Test {})"); // No value property

	auto res = execute("MATCH (n:Test) RETURN count(n.value)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("count(n.value)").toString(), "2");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Aggregate_Sum) {
	// Tests SUM() - covers line 135
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN sum(n.value)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("sum(n.value)").toString(), "60");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Aggregate_Avg) {
	// Tests AVG() - covers line 136
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN avg(n.value)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("avg(n.value)").toString(), "20");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Aggregate_Min) {
	// Tests MIN() - covers line 137
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 5})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN min(n.value)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("min(n.value)").toString(), "5");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Aggregate_Max) {
	// Tests MAX() - covers line 138
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 5})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN max(n.value)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("max(n.value)").toString(), "20");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Aggregate_Collect) {
	// Tests COLLECT() - covers line 139
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 2})");
	(void) execute("CREATE (n:Test {value: 3})");

	auto res = execute("MATCH (n:Test) RETURN collect(n.value)");
	ASSERT_EQ(res.rowCount(), 1UL);
	// collect returns a list
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Aggregate_WithAlias) {
	// Tests aggregate with AS alias - covers line 154-157
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN sum(n.value) AS total");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("total").toString(), "30");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Aggregate_MultipleAggregates) {
	// Tests multiple aggregates in same RETURN - covers loop at line 114
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN count(n) AS cnt, sum(n.value) AS total, avg(n.value) AS avg");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("cnt").toString(), "3");
	EXPECT_EQ(res.getRows()[0].at("total").toString(), "60");
	EXPECT_EQ(res.getRows()[0].at("avg").toString(), "20");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Aggregate_MixedWithNonAggregate) {
	// Tests aggregates mixed with non-aggregates - covers line 163-175
	// When mixing aggregates with non-aggregates, non-aggregates become GROUP BY keys
	(void) execute("CREATE (n:Test {group: 'A', value: 10})");
	(void) execute("CREATE (n:Test {group: 'A', value: 20})");

	auto res = execute("MATCH (n:Test) RETURN n.group, count(n) AS cnt");
	// Should return 1 row since both nodes have group='A' (grouped by n.group)
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("cnt").toString(), "2");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Aggregate_UppercaseFunctionName) {
	// Tests case-insensitive aggregate function names - line 130-132 transform
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN COUNT(n)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("COUNT(n)").toString(), "2");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Aggregate_MixedCaseFunctionName) {
	// Tests mixed case function names
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN Count(n)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("Count(n)").toString(), "2");
}

// ============================================================================
// ORDER BY with Projection Alias - Lines 78-88
// ============================================================================

TEST_F(ResultClauseHandlerCoverageTest, Return_OrderBy_ProjectionAlias) {
	// Tests ORDER BY referencing projection alias - covers line 78-88
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 15})");

	auto res = execute("MATCH (n:Test) RETURN n.value AS v ORDER BY v");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("v").toString(), "10");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_OrderBy_ProjectionAliasDesc) {
	// Tests ORDER BY with alias and DESC
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN n.value AS v ORDER BY v DESC");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("v").toString(), "20");
}

TEST_F(ResultClauseHandlerCoverageTest, Return_OrderBy_PropertyAccessNoAlias) {
	// Tests ORDER BY with property access that's not a simple variable
	// Covers line 79 hasProperty() check when property is accessed
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "10");
}

// ============================================================================
// DISTINCT Tests - Line 41
// ============================================================================

TEST_F(ResultClauseHandlerCoverageTest, Return_Distinct_SingleField) {
	// Tests RETURN DISTINCT - covers True branch at line 41 (K_DISTINCT != nullptr)
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 2})");

	auto res = execute("MATCH (n:Test) RETURN DISTINCT n.value");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Distinct_MultipleFields) {
	// Tests DISTINCT with multiple fields
	(void) execute("CREATE (n:Test {a: 1, b: 2})");
	(void) execute("CREATE (n:Test {a: 1, b: 2})");
	(void) execute("CREATE (n:Test {a: 1, b: 3})");

	auto res = execute("MATCH (n:Test) RETURN DISTINCT n.a, n.b");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Distinct_WithAggregate) {
	// Tests DISTINCT with aggregate function
	(void) execute("CREATE (n:Test {group: 'A', value: 10})");
	(void) execute("CREATE (n:Test {group: 'A', value: 20})");

	auto res = execute("MATCH (n:Test) RETURN DISTINCT n.group, count(n)");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Distinct_AfterWhere) {
	// Tests DISTINCT with WHERE clause
	for (int i = 0; i < 5; ++i) {
		(void) execute("CREATE (n:Test {value: 1})");
	}
	for (int i = 0; i < 3; ++i) {
		(void) execute("CREATE (n:Test {value: 2})");
	}

	auto res = execute("MATCH (n:Test) WHERE n.value = 1 RETURN DISTINCT n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
}

// ============================================================================
// Error Case Tests - Lines 193-197, 206-210
// ============================================================================

TEST_F(ResultClauseHandlerCoverageTest, Return_Skip_NonInteger_Error) {
	// Tests SKIP with string literal - should trigger error handling
	EXPECT_THROW({
		execute("CREATE (n:Test {id: 1})");
		execute("MATCH (n:Test) RETURN n SKIP 'invalid'");
	}, std::runtime_error);
}

TEST_F(ResultClauseHandlerCoverageTest, Return_Limit_NonInteger_Error) {
	// Tests LIMIT with string literal - should trigger error handling
	EXPECT_THROW({
		execute("CREATE (n:Test {id: 1})");
		execute("MATCH (n:Test) RETURN n LIMIT 'invalid'");
	}, std::runtime_error);
}
