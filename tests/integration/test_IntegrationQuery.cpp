/**
 * @file test_IntegrationQuery.cpp
 * @date 2026/02/03
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
 * Integration test fixture for query execution
 */
class IntegrationQueryTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_query_" + boost::uuids::to_string(uuid) + ".graph");
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

	void setupSocialGraph() const {
		(void) execute("CREATE (a:Person {name: 'Alice', age: 30})");
		(void) execute("CREATE (b:Person {name: 'Bob', age: 25})");
		(void) execute("CREATE (c:Person {name: 'Charlie', age: 35})");
		(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");
		(void) execute("CREATE (b:Person {name: 'Bob'})-[:KNOWS]->(c:Person {name: 'Charlie'})");
	}

	query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }

	std::filesystem::path testDbPath;
	std::unique_ptr<Database> db;
};

/**
 * Test simple MATCH query
 */
TEST_F(IntegrationQueryTest, SimpleMatchQuery) {
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");

	auto result = execute("MATCH (n:Person) RETURN n.name, n.age");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test MATCH with WHERE clause
 */
TEST_F(IntegrationQueryTest, MatchWithWhere) {
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30, active: true})");
	(void) execute("CREATE (n:Person {name: 'Bob', age: 25, active: false})");

	auto result = execute("MATCH (n:Person) WHERE n.active = true RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test MATCH with relationships
 */
TEST_F(IntegrationQueryTest, MatchWithRelationships) {
	(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");

	auto result = execute("MATCH (a:Person)-[:KNOWS]->(b:Person) RETURN a.name, b.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test variable length traversal
 */
TEST_F(IntegrationQueryTest, VariableLengthTraversal) {
	setupSocialGraph();

	auto result = execute("MATCH (a:Person {name: 'Alice'})-[:KNOWS*1..2]->(b:Person) RETURN DISTINCT b.name");
	EXPECT_GT(result.rowCount(), 0UL);
}

/**
 * Test ORDER BY clause
 */
TEST_F(IntegrationQueryTest, OrderByQuery) {
	(void) execute("CREATE (n:Person {name: 'Charlie', age: 35})");
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
	(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");

	auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.age");
	EXPECT_EQ(result.rowCount(), 3UL);
}

/**
 * Test LIMIT clause
 */
TEST_F(IntegrationQueryTest, LimitQuery) {
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
	(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");
	(void) execute("CREATE (n:Person {name: 'Charlie', age: 35})");

	auto result = execute("MATCH (n:Person) RETURN n.name ORDER BY n.name LIMIT 2");
	EXPECT_EQ(result.rowCount(), 2UL);
}

/**
 * Test combined query with multiple clauses
 */
TEST_F(IntegrationQueryTest, CombinedQuery) {
	setupSocialGraph();

	auto result = execute("MATCH (p:Person) "
						  "WHERE p.age > 25 "
						  "RETURN p.name, p.age "
						  "ORDER BY p.age DESC "
						  "LIMIT 2");
	EXPECT_GT(result.rowCount(), 0UL);
	EXPECT_LE(result.rowCount(), 2UL);
}

/**
 * Test CREATE and RETURN
 */
TEST_F(IntegrationQueryTest, CreateAndReturn) {
	auto result = execute("CREATE (n:Person {name: 'Alice'}) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test SET clause
 */
TEST_F(IntegrationQueryTest, SetClause) {
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
	(void) execute("MATCH (n:Person {name: 'Alice'}) SET n.age = 31");

	auto result = execute("MATCH (n:Person {name: 'Alice'}) RETURN n.age");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test DELETE clause
 */
TEST_F(IntegrationQueryTest, DeleteClause) {
	(void) execute("CREATE (n:Person {name: 'Alice'})");
	(void) execute("CREATE (n:Person {name: 'Bob'})");
	(void) execute("MATCH (n:Person {name: 'Alice'}) DELETE n");

	auto result = execute("MATCH (n:Person) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test MERGE query
 */
TEST_F(IntegrationQueryTest, MergeQuery) {
	(void) execute("MERGE (n:Person {name: 'Alice'}) ON CREATE SET n.age = 30");
	(void) execute("MERGE (n:Person {name: 'Alice'}) ON MATCH SET n.age = 31");

	auto result = execute("MATCH (n:Person {name: 'Alice'}) RETURN n.age");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test query with multiple MATCH patterns
 */
TEST_F(IntegrationQueryTest, MultipleMatchPatterns) {
	setupSocialGraph();

	auto result = execute("MATCH (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person), "
						  "(b:Person)-[:KNOWS]->(c:Person) "
						  "RETURN a.name, b.name, c.name");
	EXPECT_GT(result.rowCount(), 0UL);
}

/**
 * Test IN operator with integer list
 */
TEST_F(IntegrationQueryTest, InOperatorIntegerList) {
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
	(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");
	(void) execute("CREATE (n:Person {name: 'Charlie', age: 35})");
	(void) execute("CREATE (n:Person {name: 'David', age: 40})");

	auto result = execute("MATCH (n:Person) WHERE n.age IN [25, 30, 35] RETURN n.name");
	EXPECT_EQ(result.rowCount(), 3UL);
}

/**
 * Test IN operator with string list
 */
TEST_F(IntegrationQueryTest, InOperatorStringList) {
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
	(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");
	(void) execute("CREATE (n:Person {name: 'Charlie', age: 35})");

	auto result = execute("MATCH (n:Person) WHERE n.name IN ['Alice', 'Bob'] RETURN n.name");
	EXPECT_EQ(result.rowCount(), 2UL);
}

/**
 * Test IN operator with empty result set
 */
TEST_F(IntegrationQueryTest, InOperatorEmptyResultSet) {
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
	(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");

	auto result = execute("MATCH (n:Person) WHERE n.age IN [40, 50, 60] RETURN n.name");
	EXPECT_EQ(result.rowCount(), 0UL);
}

/**
 * Test IN operator with single value in list
 */
TEST_F(IntegrationQueryTest, InOperatorSingleValue) {
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
	(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");

	auto result = execute("MATCH (n:Person) WHERE n.age IN [30] RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test SET label syntax
 */
TEST_F(IntegrationQueryTest, SetLabelSyntax) {
	// Create a node without label
	(void) execute("CREATE (n {name: 'Alice', age: 30})");

	// Add label to existing node
	(void) execute("MATCH (n {name: 'Alice'}) SET n:Person");

	// Verify the label was added
	auto result = execute("MATCH (n:Person) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test SET replaces existing label
 */
TEST_F(IntegrationQueryTest, SetReplacesLabel) {
	// Create a node
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");

	// Replace label with new one
	(void) execute("MATCH (n:Person {name: 'Alice'}) SET n:Employee");

	// Verify node no longer has Person label but has Employee label
	auto resultOld = execute("MATCH (n:Person) RETURN n.name");
	EXPECT_EQ(resultOld.rowCount(), 0UL);

	auto resultNew = execute("MATCH (n:Employee) RETURN n.name");
	EXPECT_EQ(resultNew.rowCount(), 1UL);
}

/**
 * Test DISTINCT operator
 */
TEST_F(IntegrationQueryTest, DistinctOperator) {
	// Create nodes with duplicate countries
	(void) execute("CREATE (n:Person {name: 'Alice', country: 'USA'})");
	(void) execute("CREATE (n:Person {name: 'Bob', country: 'USA'})");
	(void) execute("CREATE (n:Person {name: 'Charlie', country: 'UK'})");

	// Without DISTINCT - should return 3 rows
	auto resultAll = execute("MATCH (n:Person) RETURN n.country");
	EXPECT_EQ(resultAll.rowCount(), 3UL);

	// With DISTINCT - should return 2 unique countries
	auto resultDistinct = execute("MATCH (n:Person) RETURN DISTINCT n.country");
	EXPECT_EQ(resultDistinct.rowCount(), 2UL);
}

/**
 * Test DISTINCT with single value
 */
TEST_F(IntegrationQueryTest, DistinctSingleValue) {
	// Create nodes with same property value
	(void) execute("CREATE (n:Person {name: 'Alice', status: 'active'})");
	(void) execute("CREATE (n:Person {name: 'Bob', status: 'active'})");

	// DISTINCT should return 1 row
	auto result = execute("MATCH (n:Person) RETURN DISTINCT n.status");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test OPTIONAL MATCH with no matches returns NULL
 */
TEST_F(IntegrationQueryTest, OptionalMatchNoMatch) {
	// Create Alice who knows no one
	(void) execute("CREATE (a:Person {name: 'Alice'})");

	// OPTIONAL MATCH should return Alice with NULL for b
	auto result = execute("MATCH (a:Person {name: 'Alice'}) OPTIONAL MATCH (a)-[:KNOWS]->(b:Person) RETURN a.name, b.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test OPTIONAL MATCH with matches returns matched data
 */
TEST_F(IntegrationQueryTest, OptionalMatchWithMatch) {
	// Create Alice who knows Bob
	(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");

	// OPTIONAL MATCH should return the matched data
	auto result = execute("MATCH (a:Person {name: 'Alice'}) OPTIONAL MATCH (a)-[:KNOWS]->(b:Person) RETURN a.name, b.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test OPTIONAL MATCH with WHERE clause
 */
TEST_F(IntegrationQueryTest, OptionalMatchWithWhere) {
	// Create Alice who knows both Bob (age 25) and Charlie (age 35)
	(void) execute("CREATE (a:Person {name: 'Alice'})");
	(void) execute("CREATE (b:Person {name: 'Bob', age: 25})");
	(void) execute("CREATE (c:Person {name: 'Charlie', age: 35})");
	(void) execute("MATCH (a:Person {name: 'Alice'}), (b:Person {name: 'Bob'}) CREATE (a)-[:KNOWS]->(b)");
	(void) execute("MATCH (a:Person {name: 'Alice'}), (c:Person {name: 'Charlie'}) CREATE (a)-[:KNOWS]->(c)");

	// OPTIONAL MATCH with WHERE should only match Bob
	auto result = execute("MATCH (a:Person {name: 'Alice'}) OPTIONAL MATCH (a)-[:KNOWS]->(b:Person) WHERE b.age < 30 RETURN a.name, b.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test OPTIONAL MATCH with multiple people
 */
TEST_F(IntegrationQueryTest, OptionalMatchMultiplePeople) {
	// Create Alice who knows Bob, and Charlie who knows no one
	(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");
	(void) execute("CREATE (c:Person {name: 'Charlie'})");

	// All people should be returned, Bob should have NULL for friend
	auto result = execute("MATCH (p:Person) OPTIONAL MATCH (p)-[:KNOWS]->(f:Person) RETURN p.name, f.name ORDER BY p.name");
	EXPECT_EQ(result.rowCount(), 3UL);
}

/**
 * Test OPTIONAL MATCH after regular MATCH
 * NOTE: Current implementation doesn't fully support context binding.
 * This test documents the current behavior and will be updated when
 * proper variable binding is implemented.
 */
TEST_F(IntegrationQueryTest, OptionalMatchAfterRegularMatch) {
	// Create simple graph: Alice knows Bob
	(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");

	// Regular MATCH finds Alice->Bob
	// OPTIONAL MATCH with no matches for b->c should still return row with NULL c
	auto result = execute("MATCH (a:Person)-[:KNOWS]->(b:Person) OPTIONAL MATCH (b)-[:KNOWS]->(c:Person) RETURN a.name, b.name, c.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test OPTIONAL MATCH with no input
 */
TEST_F(IntegrationQueryTest, OptionalMatchNoInput) {
	// OPTIONAL MATCH as the first clause should work like regular MATCH
	(void) execute("CREATE (a:Person {name: 'Alice'})");

	auto result = execute("OPTIONAL MATCH (a:Person) RETURN a.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test OPTIONAL MATCH relationship in reverse direction
 */
TEST_F(IntegrationQueryTest, OptionalMatchReverseDirection) {
	// Create Bob who is known by Alice
	(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");

	// OPTIONAL MATCH with incoming relationship
	auto result = execute("MATCH (b:Person {name: 'Bob'}) OPTIONAL MATCH (a:Person)-[:KNOWS]->(b) RETURN b.name, a.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}
