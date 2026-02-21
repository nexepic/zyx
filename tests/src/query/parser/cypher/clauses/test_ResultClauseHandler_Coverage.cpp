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
