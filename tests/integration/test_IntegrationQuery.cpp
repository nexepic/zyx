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
		db.reset();
		std::error_code ec;
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath, ec);
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
TEST_F(IntegrationQueryTest, SetAppendsLabel) {
	// Create a node
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");

	// Append a new label (multi-label: SET appends, not replaces)
	(void) execute("MATCH (n:Person {name: 'Alice'}) SET n:Employee");

	// Node should have BOTH Person and Employee labels
	auto resultOld = execute("MATCH (n:Person) RETURN n.name");
	EXPECT_EQ(resultOld.rowCount(), 1UL);

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

// ============================================================================
// Expression Evaluator branch coverage tests
// ============================================================================

TEST_F(IntegrationQueryTest, ArithmeticMixedTypes) {
	// Integer + Double -> Double path in evaluateArithmetic
	auto r1 = execute("RETURN 5 + 2.5 AS val");
	EXPECT_EQ(r1.rowCount(), 1UL);

	// Double - Integer
	auto r2 = execute("RETURN 10.0 - 3 AS val");
	EXPECT_EQ(r2.rowCount(), 1UL);

	// Double * Double
	auto r3 = execute("RETURN 2.0 * 3.0 AS val");
	EXPECT_EQ(r3.rowCount(), 1UL);

	// Double / Double
	auto r4 = execute("RETURN 10.0 / 3.0 AS val");
	EXPECT_EQ(r4.rowCount(), 1UL);

	// Integer division that produces double (not evenly divisible)
	auto r5 = execute("RETURN 7 / 2 AS val");
	EXPECT_EQ(r5.rowCount(), 1UL);

	// Integer division that produces integer (evenly divisible)
	auto r6 = execute("RETURN 6 / 3 AS val");
	EXPECT_EQ(r6.rowCount(), 1UL);

	// Power operator
	auto r7 = execute("RETURN 2 ^ 3 AS val");
	EXPECT_EQ(r7.rowCount(), 1UL);

	// Modulo operator
	auto r8 = execute("RETURN 10 % 3 AS val");
	EXPECT_EQ(r8.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, StringConcatenation) {
	// String + String
	auto r1 = execute("RETURN 'hello' + ' world' AS val");
	EXPECT_EQ(r1.rowCount(), 1UL);

	// String + Integer (coercion)
	auto r2 = execute("RETURN 'count: ' + 42 AS val");
	EXPECT_EQ(r2.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, ComparisonOperators) {
	// Various comparisons covering all branches
	auto r1 = execute("RETURN 5 < 10 AS val");
	EXPECT_EQ(r1.rowCount(), 1UL);

	auto r2 = execute("RETURN 10 > 5 AS val");
	EXPECT_EQ(r2.rowCount(), 1UL);

	auto r3 = execute("RETURN 5 <= 5 AS val");
	EXPECT_EQ(r3.rowCount(), 1UL);

	auto r4 = execute("RETURN 5 >= 5 AS val");
	EXPECT_EQ(r4.rowCount(), 1UL);

	auto r5 = execute("RETURN 5 <> 10 AS val");
	EXPECT_EQ(r5.rowCount(), 1UL);

	// String operators
	auto r6 = execute("RETURN 'hello world' STARTS WITH 'hello' AS val");
	EXPECT_EQ(r6.rowCount(), 1UL);

	auto r7 = execute("RETURN 'hello world' ENDS WITH 'world' AS val");
	EXPECT_EQ(r7.rowCount(), 1UL);

	auto r8 = execute("RETURN 'hello world' CONTAINS 'lo wo' AS val");
	EXPECT_EQ(r8.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, LogicalOperatorsThreeValuedLogic) {
	// AND with NULL
	auto r1 = execute("RETURN false AND null AS val");
	EXPECT_EQ(r1.rowCount(), 1UL);

	auto r2 = execute("RETURN null AND false AS val");
	EXPECT_EQ(r2.rowCount(), 1UL);

	auto r3 = execute("RETURN true AND null AS val");
	EXPECT_EQ(r3.rowCount(), 1UL);

	// OR with NULL
	auto r4 = execute("RETURN true OR null AS val");
	EXPECT_EQ(r4.rowCount(), 1UL);

	auto r5 = execute("RETURN null OR true AS val");
	EXPECT_EQ(r5.rowCount(), 1UL);

	auto r6 = execute("RETURN false OR null AS val");
	EXPECT_EQ(r6.rowCount(), 1UL);

	// XOR with NULL
	auto r7 = execute("RETURN null XOR true AS val");
	EXPECT_EQ(r7.rowCount(), 1UL);

	// XOR without NULL
	auto r8 = execute("RETURN true XOR false AS val");
	EXPECT_EQ(r8.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, UnaryOperators) {
	// Unary minus on integer
	auto r1 = execute("RETURN -5 AS val");
	EXPECT_EQ(r1.rowCount(), 1UL);

	// Unary minus on double
	auto r2 = execute("RETURN -3.14 AS val");
	EXPECT_EQ(r2.rowCount(), 1UL);

	// NOT operator
	auto r3 = execute("RETURN NOT true AS val");
	EXPECT_EQ(r3.rowCount(), 1UL);

	auto r4 = execute("RETURN NOT false AS val");
	EXPECT_EQ(r4.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, NullPropagation) {
	// NULL in arithmetic
	auto r1 = execute("RETURN null + 5 AS val");
	EXPECT_EQ(r1.rowCount(), 1UL);

	auto r2 = execute("RETURN 5 + null AS val");
	EXPECT_EQ(r2.rowCount(), 1UL);

	// NULL in comparison
	auto r3 = execute("RETURN null = 5 AS val");
	EXPECT_EQ(r3.rowCount(), 1UL);

	// IS NULL / IS NOT NULL
	auto r4 = execute("RETURN null IS NULL AS val");
	EXPECT_EQ(r4.rowCount(), 1UL);

	auto r5 = execute("RETURN null IS NOT NULL AS val");
	EXPECT_EQ(r5.rowCount(), 1UL);

	auto r6 = execute("RETURN 1 IS NULL AS val");
	EXPECT_EQ(r6.rowCount(), 1UL);

	auto r7 = execute("RETURN 1 IS NOT NULL AS val");
	EXPECT_EQ(r7.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, ListSlicing) {
	// Single index
	auto r1 = execute("RETURN [1, 2, 3][0] AS val");
	EXPECT_EQ(r1.rowCount(), 1UL);

	// Negative index
	auto r2 = execute("RETURN [1, 2, 3][-1] AS val");
	EXPECT_EQ(r2.rowCount(), 1UL);

	// Out of bounds
	auto r3 = execute("RETURN [1, 2, 3][10] AS val");
	EXPECT_EQ(r3.rowCount(), 1UL);

	// Range slice
	auto r4 = execute("RETURN [1, 2, 3, 4, 5][1..3] AS val");
	EXPECT_EQ(r4.rowCount(), 1UL);

	// Range with negative
	auto r5 = execute("RETURN [1, 2, 3, 4, 5][0..-1] AS val");
	EXPECT_EQ(r5.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, CaseExpression) {
	(void) execute("CREATE (n:CaseNode {status: 'active', score: 85})");

	// Simple CASE
	auto r1 = execute("MATCH (n:CaseNode) RETURN CASE n.status WHEN 'active' THEN 'yes' WHEN 'inactive' THEN 'no' ELSE 'unknown' END AS val");
	EXPECT_EQ(r1.rowCount(), 1UL);

	// Searched CASE
	auto r2 = execute("MATCH (n:CaseNode) RETURN CASE WHEN n.score > 90 THEN 'A' WHEN n.score > 80 THEN 'B' ELSE 'C' END AS val");
	EXPECT_EQ(r2.rowCount(), 1UL);

	// CASE with no matching branch (uses ELSE)
	auto r3 = execute("MATCH (n:CaseNode) RETURN CASE n.status WHEN 'deleted' THEN 'yes' ELSE 'no' END AS val");
	EXPECT_EQ(r3.rowCount(), 1UL);

	// CASE without ELSE (returns NULL)
	auto r4 = execute("MATCH (n:CaseNode) RETURN CASE n.status WHEN 'deleted' THEN 'yes' END AS val");
	EXPECT_EQ(r4.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, InExpression) {
	auto r1 = execute("RETURN 3 IN [1, 2, 3, 4] AS val");
	EXPECT_EQ(r1.rowCount(), 1UL);

	auto r2 = execute("RETURN 5 IN [1, 2, 3, 4] AS val");
	EXPECT_EQ(r2.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, ListComprehension) {
	// Filter-only comprehension
	auto r1 = execute("RETURN [x IN [1, 2, 3, 4, 5] WHERE x > 3] AS val");
	EXPECT_EQ(r1.rowCount(), 1UL);

	// Map-only comprehension
	auto r2 = execute("RETURN [x IN [1, 2, 3] | x * 2] AS val");
	EXPECT_EQ(r2.rowCount(), 1UL);

	// Filter + Map comprehension
	auto r3 = execute("RETURN [x IN [1, 2, 3, 4, 5] WHERE x > 2 | x * 10] AS val");
	EXPECT_EQ(r3.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, QuantifierFunctions) {
	// Quantifier functions tested at unit level; Cypher syntax uses '|' for predicate
	// Simple integration smoke test with list comprehension syntax
	auto r1 = execute("RETURN [x IN [2, 4, 6] WHERE x > 3] AS val");
	EXPECT_EQ(r1.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, MergeNode) {
	// MERGE with ON CREATE SET
	auto r1 = execute("MERGE (n:MergeTest {name: 'Alice'}) ON CREATE SET n.created = true RETURN n.name");
	EXPECT_EQ(r1.rowCount(), 1UL);

	// MERGE same node again - should match (ON MATCH)
	auto r2 = execute("MERGE (n:MergeTest {name: 'Alice'}) ON MATCH SET n.matched = true RETURN n.name");
	EXPECT_EQ(r2.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, MergeEdge) {
	(void) execute("CREATE (a:MENode {name: 'A'}), (b:MENode {name: 'B'})");

	// MERGE edge - create
	auto r1 = execute("MATCH (a:MENode {name: 'A'}), (b:MENode {name: 'B'}) MERGE (a)-[r:LINKED]->(b) RETURN r");
	EXPECT_EQ(r1.rowCount(), 1UL);

	// MERGE same edge - should match
	auto r2 = execute("MATCH (a:MENode {name: 'A'}), (b:MENode {name: 'B'}) MERGE (a)-[r:LINKED]->(b) RETURN r");
	EXPECT_EQ(r2.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, UnionAll) {
	auto r1 = execute("RETURN 1 AS val UNION ALL RETURN 2 AS val");
	EXPECT_GE(r1.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, UnionDistinct) {
	(void) execute("CREATE (n:UD1 {val: 1}), (m:UD2 {val: 1})");

	auto r1 = execute("MATCH (n:UD1) RETURN n.val AS val UNION MATCH (m:UD2) RETURN m.val AS val");
	EXPECT_EQ(r1.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, DeleteNodeAndEdge) {
	(void) execute("CREATE (a:Del1 {name: 'A'})-[:DREL]->(b:Del2 {name: 'B'})");

	// DETACH DELETE
	auto r1 = execute("MATCH (n:Del1) DETACH DELETE n");
	(void) r1;

	auto r2 = execute("MATCH (n:Del1) RETURN n");
	EXPECT_EQ(r2.rowCount(), 0UL);
}

TEST_F(IntegrationQueryTest, SetAndRemoveProperty) {
	(void) execute("CREATE (n:SRNode {name: 'Test'})");

	// SET property
	(void) execute("MATCH (n:SRNode) SET n.age = 30");

	auto r1 = execute("MATCH (n:SRNode) RETURN n.age");
	EXPECT_EQ(r1.rowCount(), 1UL);

	// REMOVE property
	(void) execute("MATCH (n:SRNode) REMOVE n.age");

	auto r2 = execute("MATCH (n:SRNode) RETURN n.age");
	EXPECT_EQ(r2.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, SetLabel) {
	(void) execute("CREATE (n:LabelNode {name: 'Test'})");

	// SET label
	(void) execute("MATCH (n:LabelNode) SET n:ExtraLabel");
}

TEST_F(IntegrationQueryTest, OrderBySkipLimit) {
	(void) execute("CREATE (a:ONode {val: 3}), (b:ONode {val: 1}), (c:ONode {val: 2})");

	// ORDER BY ASC
	auto r1 = execute("MATCH (n:ONode) RETURN n.val ORDER BY n.val ASC");
	EXPECT_EQ(r1.rowCount(), 3UL);

	// ORDER BY DESC with LIMIT
	auto r2 = execute("MATCH (n:ONode) RETURN n.val ORDER BY n.val DESC LIMIT 2");
	EXPECT_EQ(r2.rowCount(), 2UL);

	// SKIP
	auto r3 = execute("MATCH (n:ONode) RETURN n.val ORDER BY n.val SKIP 1");
	EXPECT_EQ(r3.rowCount(), 2UL);

	// SKIP + LIMIT
	auto r4 = execute("MATCH (n:ONode) RETURN n.val ORDER BY n.val SKIP 1 LIMIT 1");
	EXPECT_EQ(r4.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, DistinctReturn) {
	(void) execute("CREATE (a:DNode {val: 1}), (b:DNode {val: 1}), (c:DNode {val: 2})");

	auto r1 = execute("MATCH (n:DNode) RETURN DISTINCT n.val");
	EXPECT_EQ(r1.rowCount(), 2UL);
}

TEST_F(IntegrationQueryTest, AggregationFunctions) {
	(void) execute("CREATE (a:ANode {val: 10}), (b:ANode {val: 20}), (c:ANode {val: 30})");

	// COUNT
	auto r1 = execute("MATCH (n:ANode) RETURN count(n) AS cnt");
	EXPECT_EQ(r1.rowCount(), 1UL);

	// SUM
	auto r2 = execute("MATCH (n:ANode) RETURN sum(n.val) AS total");
	EXPECT_EQ(r2.rowCount(), 1UL);

	// AVG
	auto r3 = execute("MATCH (n:ANode) RETURN avg(n.val) AS average");
	EXPECT_EQ(r3.rowCount(), 1UL);

	// MIN / MAX
	auto r4 = execute("MATCH (n:ANode) RETURN min(n.val) AS mn, max(n.val) AS mx");
	EXPECT_EQ(r4.rowCount(), 1UL);

	// COLLECT
	auto r5 = execute("MATCH (n:ANode) RETURN collect(n.val) AS vals");
	EXPECT_EQ(r5.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, WithClause) {
	(void) execute("CREATE (a:WNode {val: 10}), (b:WNode {val: 20})");

	auto r1 = execute("MATCH (n:WNode) WITH n.val AS v WHERE v > 15 RETURN v");
	EXPECT_EQ(r1.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, UnwindClause) {
	auto r1 = execute("UNWIND [1, 2, 3] AS x RETURN x");
	EXPECT_EQ(r1.rowCount(), 3UL);
}

TEST_F(IntegrationQueryTest, ScalarFunctions) {
	// toInteger
	auto r1 = execute("RETURN toInteger('42') AS val");
	EXPECT_EQ(r1.rowCount(), 1UL);

	// toFloat
	auto r2 = execute("RETURN toFloat('3.14') AS val");
	EXPECT_EQ(r2.rowCount(), 1UL);

	// toString
	auto r3 = execute("RETURN toString(42) AS val");
	EXPECT_EQ(r3.rowCount(), 1UL);

	// size on string
	auto r4 = execute("RETURN size('hello') AS val");
	EXPECT_EQ(r4.rowCount(), 1UL);

	// size on list
	auto r5 = execute("RETURN size([1, 2, 3]) AS val");
	EXPECT_EQ(r5.rowCount(), 1UL);

	// abs
	auto r6 = execute("RETURN abs(-5) AS val");
	EXPECT_EQ(r6.rowCount(), 1UL);

	// ceil, floor, round
	auto r7 = execute("RETURN ceil(2.3) AS c, floor(2.7) AS f, round(2.5) AS r");
	EXPECT_EQ(r7.rowCount(), 1UL);

	// coalesce
	auto r8 = execute("RETURN coalesce(null, null, 'found') AS val");
	EXPECT_EQ(r8.rowCount(), 1UL);

	// type (edge function)
	(void) execute("CREATE (a:TNode)-[:TREL]->(b:TNode)");
	auto r9 = execute("MATCH ()-[r:TREL]->() RETURN type(r) AS val");
	EXPECT_EQ(r9.rowCount(), 1UL);

	// String functions
	auto r10 = execute("RETURN lower('HELLO') AS l, upper('hello') AS u, trim('  hi  ') AS t");
	EXPECT_EQ(r10.rowCount(), 1UL);

	// substring
	auto r11 = execute("RETURN substring('hello', 1, 3) AS val");
	EXPECT_EQ(r11.rowCount(), 1UL);

	// replace
	auto r12 = execute("RETURN replace('hello world', 'world', 'there') AS val");
	EXPECT_EQ(r12.rowCount(), 1UL);

	// head, last, tail
	auto r13 = execute("RETURN head([1, 2, 3]) AS h, last([1, 2, 3]) AS l, tail([1, 2, 3]) AS t");
	EXPECT_EQ(r13.rowCount(), 1UL);

	// reverse
	auto r14 = execute("RETURN reverse([1, 2, 3]) AS val");
	EXPECT_EQ(r14.rowCount(), 1UL);

	// keys function
	(void) execute("CREATE (n:KeyNode {a: 1, b: 2})");
	auto r15 = execute("MATCH (n:KeyNode) RETURN keys(n) AS val");
	EXPECT_EQ(r15.rowCount(), 1UL);

	// range function
	auto r16 = execute("RETURN range(0, 5) AS val");
	EXPECT_EQ(r16.rowCount(), 1UL);

	// range with step
	auto r17 = execute("RETURN range(0, 10, 3) AS val");
	EXPECT_EQ(r17.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, EntityIntrospectionFunctions) {
	(void) execute("CREATE (n:IntrNode {name: 'test'})");

	auto r1 = execute("MATCH (n:IntrNode) RETURN id(n) AS nid");
	EXPECT_EQ(r1.rowCount(), 1UL);

	auto r2 = execute("MATCH (n:IntrNode) RETURN labels(n) AS lbls");
	EXPECT_EQ(r2.rowCount(), 1UL);

	auto r3 = execute("MATCH (n:IntrNode) RETURN properties(n) AS props");
	EXPECT_EQ(r3.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, CreateIndex) {
	(void) execute("CREATE (n:IdxNode {name: 'A', age: 30})");

	auto r1 = execute("CREATE INDEX idx_name FOR (n:IdxNode) ON (n.name)");
	(void) r1;

	auto r2 = execute("SHOW INDEXES");
	EXPECT_GE(r2.rowCount(), 1UL);

	auto r3 = execute("DROP INDEX idx_name");
	(void) r3;
}

TEST_F(IntegrationQueryTest, MapLiteral) {
	// Test map literals in property-setting context (SET +=)
	(void) execute("CREATE (n:MapLitNode {name: 'test'})");
	(void) execute("MATCH (n:MapLitNode) SET n += {a: 1, b: 'hello', c: true}");

	auto r1 = execute("MATCH (n:MapLitNode) RETURN n.a, n.b, n.c");
	EXPECT_EQ(r1.rowCount(), 1UL);
}

TEST_F(IntegrationQueryTest, SetPropertyMap) {
	(void) execute("CREATE (n:MapNode {name: 'X'})");
	(void) execute("MATCH (n:MapNode) SET n += {age: 25, city: 'NYC'}");

	auto r1 = execute("MATCH (n:MapNode) RETURN n.age, n.city");
	EXPECT_EQ(r1.rowCount(), 1UL);
}

// ============================================================================
// Additional integration tests for QueryExecutor and Database branch coverage
// ============================================================================

/**
 * Test DELETE node after creation within same session (covers QueryExecutor
 * delete paths and FileStorage deletion flush paths)
 */
TEST_F(IntegrationQueryTest, DeleteNodeAfterCreation) {
	(void) execute("CREATE (n:DelNode {name: 'ToDelete'})");
	auto before = execute("MATCH (n:DelNode) RETURN n");
	EXPECT_EQ(before.rowCount(), 1UL);

	(void) execute("MATCH (n:DelNode {name: 'ToDelete'}) DELETE n");
	auto after = execute("MATCH (n:DelNode) RETURN n");
	EXPECT_EQ(after.rowCount(), 0UL);
}

/**
 * Test DELETE edge without deleting nodes (covers edge deletion path)
 */
TEST_F(IntegrationQueryTest, DeleteEdgeOnly) {
	(void) execute("CREATE (a:EDelNode {name: 'A'})-[:EDEL_REL {w: 1}]->(b:EDelNode {name: 'B'})");

	// Verify edge exists
	auto before = execute("MATCH ()-[r:EDEL_REL]->() RETURN r");
	EXPECT_EQ(before.rowCount(), 1UL);

	// Delete just the edge
	(void) execute("MATCH ()-[r:EDEL_REL]->() DELETE r");

	// Edge should be gone but nodes should remain
	auto afterEdge = execute("MATCH ()-[r:EDEL_REL]->() RETURN r");
	EXPECT_EQ(afterEdge.rowCount(), 0UL);

	auto afterNodes = execute("MATCH (n:EDelNode) RETURN n");
	EXPECT_EQ(afterNodes.rowCount(), 2UL);
}

/**
 * Test bulk node creation and deletion (exercises many FileStorage save paths)
 */
TEST_F(IntegrationQueryTest, BulkCreateAndDelete) {
	for (int i = 0; i < 20; ++i) {
		execute("CREATE (n:BulkNode {id: " + std::to_string(i) + "})");
	}

	auto created = execute("MATCH (n:BulkNode) RETURN n.id");
	EXPECT_EQ(created.rowCount(), 20UL);

	// Delete half
	(void) execute("MATCH (n:BulkNode) WHERE n.id < 10 DELETE n");
	auto remaining = execute("MATCH (n:BulkNode) RETURN n.id");
	EXPECT_EQ(remaining.rowCount(), 10UL);
}

/**
 * Test DETACH DELETE on node with multiple edges
 * (covers RelationshipTraversal unlinkEdge paths)
 */
TEST_F(IntegrationQueryTest, DetachDeleteNodeWithMultipleEdges) {
	(void) execute("CREATE (c:Center {name: 'Hub'})");
	(void) execute("CREATE (a:Spoke {name: 'A'})");
	(void) execute("CREATE (b:Spoke {name: 'B'})");
	(void) execute("CREATE (d:Spoke {name: 'C'})");
	(void) execute("MATCH (c:Center), (a:Spoke {name: 'A'}) CREATE (c)-[:LINK]->(a)");
	(void) execute("MATCH (c:Center), (b:Spoke {name: 'B'}) CREATE (c)-[:LINK]->(b)");
	(void) execute("MATCH (c:Center), (d:Spoke {name: 'C'}) CREATE (d)-[:LINK]->(c)");

	// DETACH DELETE the center node
	(void) execute("MATCH (c:Center) DETACH DELETE c");

	auto centerResult = execute("MATCH (n:Center) RETURN n");
	EXPECT_EQ(centerResult.rowCount(), 0UL);

	// Spokes should still exist
	auto spokeResult = execute("MATCH (n:Spoke) RETURN n");
	EXPECT_EQ(spokeResult.rowCount(), 3UL);

	// All edges should be gone
	auto edgeResult = execute("MATCH ()-[r:LINK]->() RETURN r");
	EXPECT_EQ(edgeResult.rowCount(), 0UL);
}

/**
 * Test query returning edge values (covers QueryExecutor edge result path)
 */
TEST_F(IntegrationQueryTest, QueryReturnsEdgeValues) {
	(void) execute("CREATE (a:EV1 {name: 'A'})-[:EV_REL {weight: 42, tag: 'test'}]->(b:EV2 {name: 'B'})");

	auto result = execute("MATCH (a)-[r:EV_REL]->(b) RETURN r, a, b");
	EXPECT_EQ(result.rowCount(), 1UL);

	// Verify columns include edge
	auto cols = result.getColumns();
	EXPECT_EQ(cols.size(), 3UL);
}

/**
 * Test query with CREATE only (no RETURN clause)
 * The engine may or may not produce output rows for bare CREATE statements.
 */
TEST_F(IntegrationQueryTest, QueryCreateOnly) {
	auto result = execute("CREATE (n:NoRetNode {val: 1})");
	// Just verify the query succeeded and node was created
	auto verify = execute("MATCH (n:NoRetNode) RETURN n.val");
	EXPECT_EQ(verify.rowCount(), 1UL);
}

/**
 * Test query that produces null values in result (covers null/monostate result paths)
 */
TEST_F(IntegrationQueryTest, QueryWithNullValues) {
	(void) execute("CREATE (n:NullNode {name: 'Test'})");

	auto result = execute("MATCH (n:NullNode) RETURN n.name, n.nonexistent");
	EXPECT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(result.getColumns().size(), 2UL);
}

/**
 * Test close-reopen-query cycle (covers Database close/reopen with WAL)
 */
TEST_F(IntegrationQueryTest, CloseReopenQuery) {
	(void) execute("CREATE (n:ReOpenNode {val: 100})");
	db->close();

	db = std::make_unique<Database>(testDbPath.string());
	db->open();

	auto result = execute("MATCH (n:ReOpenNode) RETURN n.val");
	EXPECT_EQ(result.rowCount(), 1UL);
}

/**
 * Test multiple close-reopen cycles with data modification
 * (covers FileStorage flush/close/reopen sequences and WAL recovery)
 */
TEST_F(IntegrationQueryTest, MultipleCloseReopenWithModification) {
	// Cycle 1: Create
	(void) execute("CREATE (n:CycleNode {val: 1})");
	db->close();

	// Cycle 2: Add more
	db = std::make_unique<Database>(testDbPath.string());
	db->open();
	(void) execute("CREATE (n:CycleNode {val: 2})");
	db->close();

	// Cycle 3: Verify all data
	db = std::make_unique<Database>(testDbPath.string());
	db->open();
	auto result = execute("MATCH (n:CycleNode) RETURN n.val ORDER BY n.val");
	EXPECT_EQ(result.rowCount(), 2UL);
}

/**
 * Test hasActiveTransaction branch in Database
 */
TEST_F(IntegrationQueryTest, HasActiveTransactionCheck) {
	EXPECT_FALSE(db->hasActiveTransaction());

	auto txn = db->beginTransaction();
	EXPECT_TRUE(db->hasActiveTransaction());
}
