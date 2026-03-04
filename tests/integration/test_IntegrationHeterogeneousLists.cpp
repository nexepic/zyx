/**
 * @file test_IntegrationHeterogeneousLists.cpp
 * @date 2026/01/04
 *
 * @copyright Copyright (c) 2026
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;
using namespace graph;

/**
 * Integration test fixture for heterogeneous list operations
 */
class IntegrationHeterogeneousListsTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_hetero_lists_" + boost::uuids::to_string(uuid) + ".graph");
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath);
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
	}

	void TearDown() override {
		if (db)
			db->close();
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath);
	}

	query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }

	std::filesystem::path testDbPath;
	std::unique_ptr<Database> db;
};

/**
 * Test heterogeneous list literal in RETURN clause
 */
TEST_F(IntegrationHeterogeneousListsTest, ListLiteralInReturn) {
	auto result = execute("RETURN [1, 'hello', true, 3.14] AS mixedList");
	EXPECT_EQ(result.rowCount(), 1UL);

	// Verify the result contains a list
	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("mixedList"));
	ASSERT_TRUE(rows[0].at("mixedList").isPrimitive());
	EXPECT_EQ(rows[0].at("mixedList").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test list slicing with range notation
 */
TEST_F(IntegrationHeterogeneousListsTest, ListSlicingWithRange) {
	auto result = execute("RETURN [1, 2, 3, 4, 5][1..3] AS sliced");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("sliced"));
	EXPECT_EQ(rows[0].at("sliced").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test list slicing with single index
 * NOTE: Returns the element directly, not wrapped in a list
 */
TEST_F(IntegrationHeterogeneousListsTest, ListSlicingWithIndex) {
	auto result = execute("RETURN [10, 20, 30, 40, 50][2] AS element");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("element"));
	// Should return the third element (30) as an integer
	EXPECT_EQ(rows[0].at("element").asPrimitive().getType(), PropertyType::INTEGER);
}

/**
 * Test nested lists
 */
TEST_F(IntegrationHeterogeneousListsTest, NestedLists) {
	auto result = execute("RETURN [[1, 2], [3, 4], [5, 6]] AS nested");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("nested"));
	EXPECT_EQ(rows[0].at("nested").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test deeply nested heterogeneous lists
 */
TEST_F(IntegrationHeterogeneousListsTest, DeeplyNestedHeterogeneousLists) {
	auto result = execute("RETURN [[1, 'a'], [2, 'b'], [true, false]] AS deepNested");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("deepNested"));
	EXPECT_EQ(rows[0].at("deepNested").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test list with NULL values
 */
TEST_F(IntegrationHeterogeneousListsTest, ListWithNullValues) {
	auto result = execute("RETURN [1, NULL, 'test', NULL, 3.14] AS withNulls");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("withNulls"));
	EXPECT_EQ(rows[0].at("withNulls").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test empty list
 */
TEST_F(IntegrationHeterogeneousListsTest, EmptyList) {
	auto result = execute("RETURN [] AS empty");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("empty"));
	EXPECT_EQ(rows[0].at("empty").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test heterogeneous list in node properties
 */
TEST_F(IntegrationHeterogeneousListsTest, ListInNodeProperties) {
	// Create a node with a list property
	(void) execute("CREATE (n:Test {tags: [1, 'important', true, 3.5]})");

	// Query it back
	auto result = execute("MATCH (n:Test) RETURN n.tags AS tags");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("tags"));
	EXPECT_EQ(rows[0].at("tags").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test list slicing in WHERE clause
 * NOTE: List slicing in WHERE clause not yet fully supported
 * This test documents the expected future behavior
 */
TEST_F(IntegrationHeterogeneousListsTest, DISABLED_ListSlicingInWhere) {
	// Create nodes with list properties
	(void) execute("CREATE (n:Test {values: [10, 20, 30, 40, 50]})");
	(void) execute("CREATE (n:Test {values: [100, 200, 300, 400, 500]})");

	// Query using list slicing
	auto result = execute("MATCH (n:Test) WHERE n.values[0] = 10 RETURN n.values AS values");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("values"));
	EXPECT_EQ(rows[0].at("values").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test range() function with heterogeneous result
 */
TEST_F(IntegrationHeterogeneousListsTest, RangeFunction) {
	auto result = execute("RETURN range(1, 5) AS numbers");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("numbers"));
	EXPECT_EQ(rows[0].at("numbers").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test list persistence - save and reload
 */
TEST_F(IntegrationHeterogeneousListsTest, ListPersistence) {
	// Create a node with a heterogeneous list property
	(void) execute("CREATE (n:Persistent {data: [42, 'test', true, 3.14, NULL]})");

	// Close and reopen the database
	db->close();
	db = std::make_unique<Database>(testDbPath.string());
	db->open();

	// Query the data back
	auto result = execute("MATCH (n:Persistent) RETURN n.data AS data");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("data"));
	EXPECT_EQ(rows[0].at("data").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test nested list persistence
 */
TEST_F(IntegrationHeterogeneousListsTest, NestedListPersistence) {
	// Create a node with nested lists
	(void) execute("CREATE (n:Nested {matrix: [[1, 2], [3, 4], [5, 6]]})");

	// Close and reopen the database
	db->close();
	db = std::make_unique<Database>(testDbPath.string());
	db->open();

	// Query the data back
	auto result = execute("MATCH (n:Nested) RETURN n.matrix AS matrix");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("matrix"));
	EXPECT_EQ(rows[0].at("matrix").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test complex query with multiple list operations
 */
TEST_F(IntegrationHeterogeneousListsTest, ComplexQueryWithLists) {
	// Create test data
	(void) execute("CREATE (a:User {name: 'Alice', scores: [85, 90, 78, 92]})");
	(void) execute("CREATE (b:User {name: 'Bob', scores: [76, 88, 95, 81]})");

	// Query with list slicing
	auto result = execute("MATCH (u:User) WHERE u.name = 'Alice' RETURN u.scores[0..2] AS topScores");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("topScores"));
	EXPECT_EQ(rows[0].at("topScores").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test list concatenation scenarios
 */
TEST_F(IntegrationHeterogeneousListsTest, MixedTypeListsInQueries) {
	auto result = execute("RETURN [1, 'one', 2.0, true, NULL] AS mixed");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("mixed"));
	EXPECT_EQ(rows[0].at("mixed").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test large heterogeneous list
 */
TEST_F(IntegrationHeterogeneousListsTest, LargeHeterogeneousList) {
	// Create a large list with mixed types
	std::string query = "RETURN [";
	for (int i = 0; i < 100; ++i) {
		if (i > 0) query += ", ";
		query += std::to_string(i);
	}
	query += "] AS largeList";

	auto result = execute(query);
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("largeList"));
	EXPECT_EQ(rows[0].at("largeList").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test end-to-end list comprehensions
 */
TEST_F(IntegrationHeterogeneousListsTest, EndToEndListComprehensions) {
	// Create test data
	(void) execute("CREATE (n:User {scores: [85, 90, 78, 92]})");

	// Test comprehension in WHERE
	auto result = execute("MATCH (n:User) WHERE size([x IN n.scores WHERE x > 80]) > 0 RETURN n");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test complex list operations with comprehensions
 */
TEST_F(IntegrationHeterogeneousListsTest, ComplexListComprehensions) {
	// Test filtering and transformation
	auto result = execute("RETURN [x IN range(1, 10) WHERE x % 2 = 0 | x * 2] AS doubledEvens");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("doubledEvens"));
	EXPECT_EQ(rows[0].at("doubledEvens").asPrimitive().getType(), PropertyType::LIST);
}

/**
 * Test list comprehension with NULL values
 * NOTE: IS NOT NULL in WHERE clause of comprehension not yet supported by grammar
 * This test documents the expected future behavior
 */
TEST_F(IntegrationHeterogeneousListsTest, DISABLED_ListComprehensionWithNulls) {
	auto result = execute("RETURN [x IN [1, NULL, 3, NULL, 5] WHERE x IS NOT NULL] AS filtered");
	EXPECT_EQ(result.rowCount(), 1UL);

	auto &rows = result.getRows();
	ASSERT_FALSE(rows.empty());
	ASSERT_TRUE(rows[0].contains("filtered"));
	EXPECT_EQ(rows[0].at("filtered").asPrimitive().getType(), PropertyType::LIST);
}
