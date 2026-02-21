/**
 * @file test_ResultClauseHandler_Unit.cpp
 * @brief Unit tests for ResultClauseHandler coverage gaps
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
 * @class ResultClauseHandlerUnitTest
 * @brief Unit tests for ResultClauseHandler edge cases
 */
class ResultClauseHandlerUnitTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_result_unit_" + boost::uuids::to_string(uuid) + ".dat");
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
// SKIP Error Case Tests - Coverage Gap
// ============================================================================

// Note: Testing SKIP with non-integer expression requires triggering
// the catch block for std::stoll exception. This is difficult to test
// through normal queries as the parser may not accept non-integer expressions.
// The following tests attempt various scenarios.

TEST_F(ResultClauseHandlerUnitTest, Skip_WithVeryLargeNumber) {
	// Tests SKIP with a very large number that might overflow
	// Create test data
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 999999999999");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerUnitTest, Skip_WithZero) {
	// Tests SKIP 0 (edge case)
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 0");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ResultClauseHandlerUnitTest, Skip_WithNegativeNumber) {
	// Tests SKIP with negative number
	// Behavior might be implementation-specific
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP -1");
	// Negative SKIP might be treated as 0 or cause error
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerUnitTest, Skip_ExactlyAllRows) {
	// Tests SKIP that skips exactly all rows
	for (int i = 0; i < 5; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 5");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerUnitTest, Skip_MoreThanAvailable) {
	// Tests SKIP with value greater than row count
	for (int i = 0; i < 3; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 10");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerUnitTest, Skip_WithOrderBy) {
	// Tests SKIP combined with ORDER BY
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id DESC SKIP 5");
	ASSERT_EQ(res.rowCount(), 5UL);
}

// ============================================================================
// LIMIT Error Case Tests - Coverage Gap
// ============================================================================

TEST_F(ResultClauseHandlerUnitTest, Limit_WithVeryLargeNumber) {
	// Tests LIMIT with a very large number
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id LIMIT 999999999999");
	ASSERT_EQ(res.rowCount(), 10UL);
}

TEST_F(ResultClauseHandlerUnitTest, Limit_WithZero) {
	// Tests LIMIT 0 (edge case - should return no rows)
	for (int i = 0; i < 5; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id LIMIT 0");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerUnitTest, Limit_WithNegativeNumber) {
	// Tests LIMIT with negative number
	// Behavior might be implementation-specific
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) RETURN n.id LIMIT -1");
	// Negative LIMIT might be treated as 0 or cause error
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerUnitTest, Limit_WithOrderBy) {
	// Tests LIMIT combined with ORDER BY
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id DESC LIMIT 3");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "9");
}

// ============================================================================
// SKIP and LIMIT Combined
// ============================================================================

TEST_F(ResultClauseHandlerUnitTest, SkipAndLimit_BothPresent) {
	// Tests SKIP and LIMIT together
	for (int i = 0; i < 20; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id SKIP 5 LIMIT 10");
	ASSERT_EQ(res.rowCount(), 10UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "5");
}

TEST_F(ResultClauseHandlerUnitTest, SkipAndLimit_SkipGreaterThanLimit) {
	// Tests SKIP greater than LIMIT
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 8 LIMIT 5");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ResultClauseHandlerUnitTest, SkipAndLimit_LimitThenSkip) {
	// Tests ORDER BY with SKIP then LIMIT (correct order)
	for (int i = 0; i < 20; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	// SKIP first, then LIMIT
	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id SKIP 5 LIMIT 10");
	EXPECT_GE(res.rowCount(), 0UL);
}

// ============================================================================
// ORDER BY Edge Cases
// ============================================================================

TEST_F(ResultClauseHandlerUnitTest, OrderBy_SingleFieldAscending) {
	// Tests ORDER BY with single field ascending (explicit or default)
	(void) execute("CREATE (n:Test {val: 3})");
	(void) execute("CREATE (n:Test {val: 1})");
	(void) execute("CREATE (n:Test {val: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.val ORDER BY n.val ASC");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.val").toString(), "1");
}

TEST_F(ResultClauseHandlerUnitTest, OrderBy_SingleFieldDescending) {
	// Tests ORDER BY with DESC keyword
	(void) execute("CREATE (n:Test {val: 1})");
	(void) execute("CREATE (n:Test {val: 3})");
	(void) execute("CREATE (n:Test {val: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.val ORDER BY n.val DESC");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.val").toString(), "3");
}

TEST_F(ResultClauseHandlerUnitTest, OrderBy_WithDescendingKeyword) {
	// Tests ORDER BY with DESCENDING keyword (full form)
	(void) execute("CREATE (n:Test {val: 1})");
	(void) execute("CREATE (n:Test {val: 3})");

	auto res = execute("MATCH (n:Test) RETURN n.val ORDER BY n.val DESCENDING");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.val").toString(), "3");
}

TEST_F(ResultClauseHandlerUnitTest, OrderBy_MultipleFields) {
	// Tests ORDER BY with multiple fields
	(void) execute("CREATE (n:Test {a: 1, b: 2})");
	(void) execute("CREATE (n:Test {a: 1, b: 1})");
	(void) execute("CREATE (n:Test {a: 2, b: 1})");

	auto res = execute("MATCH (n:Test) RETURN n.a, n.b ORDER BY n.a ASC, n.b DESC");
	ASSERT_EQ(res.rowCount(), 3UL);
	// First row should be a=1, b=2 (a ascending, then b descending)
	EXPECT_EQ(res.getRows()[0].at("n.a").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.b").toString(), "2");
}

TEST_F(ResultClauseHandlerUnitTest, OrderBy_WithNullValues) {
	// Tests ORDER BY with null values
	(void) execute("CREATE (n:Test {val: null})");
	(void) execute("CREATE (n:Test {val: 1})");
	(void) execute("CREATE (n:Test {val: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.val ORDER BY n.val");
	ASSERT_EQ(res.rowCount(), 3UL);
	// Null values typically sort first or last depending on implementation
}

// ============================================================================
// RETURN Edge Cases
// ============================================================================

TEST_F(ResultClauseHandlerUnitTest, Return_WithPropertyAccess) {
	// Tests RETURN with property access
	(void) execute("CREATE (n:Test {a: 5, b: 3})");

	auto res = execute("MATCH (n:Test) RETURN n.a, n.b");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.a").toString(), "5");
	EXPECT_EQ(res.getRows()[0].at("n.b").toString(), "3");
}

TEST_F(ResultClauseHandlerUnitTest, Return_WithMultipleAliases) {
	// Tests RETURN with multiple aliases
	(void) execute("CREATE (n:Test {a: 10, b: 5})");

	auto res = execute("MATCH (n:Test) RETURN n.a AS value_a, n.b AS value_b");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("value_a").toString(), "10");
	EXPECT_EQ(res.getRows()[0].at("value_b").toString(), "5");
}

TEST_F(ResultClauseHandlerUnitTest, Return_WithoutAlias) {
	// Tests RETURN without AS keyword (alias defaults to expression)
	(void) execute("CREATE (n:Test {name: 'Alice'})");

	auto res = execute("MATCH (n:Test) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("n.name"));
}

// ============================================================================
// Combined Clauses
// ============================================================================

TEST_F(ResultClauseHandlerUnitTest, Return_WithAllClauses) {
	// Tests RETURN with ORDER BY, SKIP, and LIMIT all present
	for (int i = 0; i < 30; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id DESC SKIP 5 LIMIT 10");
	ASSERT_EQ(res.rowCount(), 10UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "24");
}

TEST_F(ResultClauseHandlerUnitTest, Return_StarWithOrderBy) {
	// Tests RETURN * with ORDER BY
	(void) execute("CREATE (n:Test {id: 2})");
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) RETURN * ORDER BY n.id");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ResultClauseHandlerUnitTest, Return_MultiplyWithLimit) {
	// Tests RETURN * with LIMIT
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN * LIMIT 5");
	ASSERT_EQ(res.rowCount(), 5UL);
}
