/**
 * @file test_IntegrationCypherFiltering.cpp
 * @brief Integration tests for Cypher WHERE clause filtering with correctness verification
 *
 * Tests AND/OR, NOT, IS NULL, IN, string predicates, NULL propagation,
 * SET with expressions, REMOVE label, and quantifier functions
 * — verifying actual result values, not just row counts.
 *
 * @copyright Copyright (c) 2026 Nexepic
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
#include <set>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;
using namespace graph;

class IntegrationCypherFilteringTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_filter_" + boost::uuids::to_string(uuid) + ".graph");
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

	std::string val(const query::QueryResult &r, const std::string &col, size_t row = 0) const {
		return r.getRows().at(row).at(col).toString();
	}

	std::filesystem::path testDbPath;
	std::unique_ptr<Database> db;
};

// ============================================================================
// AND / OR Operators
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, WhereAnd) {
	(void) execute("CREATE (a:FANode {name: 'Alice', age: 30, active: true})");
	(void) execute("CREATE (b:FANode {name: 'Bob', age: 25, active: true})");
	(void) execute("CREATE (c:FANode {name: 'Charlie', age: 35, active: false})");

	auto r = execute("MATCH (n:FANode) WHERE n.age > 25 AND n.active = true RETURN n.name");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Alice");
}

TEST_F(IntegrationCypherFilteringTest, WhereOr) {
	(void) execute("CREATE (a:FONode {name: 'Alice', val: 1})");
	(void) execute("CREATE (b:FONode {name: 'Bob', val: 2})");
	(void) execute("CREATE (c:FONode {name: 'Charlie', val: 3})");

	auto r = execute("MATCH (n:FONode) WHERE n.val = 1 OR n.val = 3 RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Alice");
	EXPECT_EQ(val(r, "n.name", 1), "Charlie");
}

TEST_F(IntegrationCypherFilteringTest, WhereAndOr) {
	(void) execute("CREATE (a:FAONode {name: 'Alice', age: 30, group: 'A'})");
	(void) execute("CREATE (b:FAONode {name: 'Bob', age: 25, group: 'B'})");
	(void) execute("CREATE (c:FAONode {name: 'Charlie', age: 35, group: 'A'})");
	(void) execute("CREATE (d:FAONode {name: 'Diana', age: 28, group: 'B'})");

	auto r = execute("MATCH (n:FAONode) WHERE (n.group = 'A' AND n.age > 30) OR (n.group = 'B' AND n.age < 27) RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Bob");
	EXPECT_EQ(val(r, "n.name", 1), "Charlie");
}

// ============================================================================
// NOT Operator
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, WhereNot) {
	(void) execute("CREATE (a:FNNode {name: 'Alice', active: true})");
	(void) execute("CREATE (b:FNNode {name: 'Bob', active: false})");
	(void) execute("CREATE (c:FNNode {name: 'Charlie', active: true})");

	auto r = execute("MATCH (n:FNNode) WHERE NOT n.active = true RETURN n.name");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Bob");
}

// ============================================================================
// IS NULL / IS NOT NULL
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, WhereIsNull) {
	(void) execute("CREATE (a:NNode {name: 'Alice', email: 'alice@test.com'})");
	(void) execute("CREATE (b:NNode {name: 'Bob'})");
	(void) execute("CREATE (c:NNode {name: 'Charlie', email: 'charlie@test.com'})");

	auto r = execute("MATCH (n:NNode) WHERE n.email IS NULL RETURN n.name");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Bob");
}

TEST_F(IntegrationCypherFilteringTest, WhereIsNotNull) {
	(void) execute("CREATE (a:NNNode {name: 'Alice', email: 'alice@test.com'})");
	(void) execute("CREATE (b:NNNode {name: 'Bob'})");
	(void) execute("CREATE (c:NNNode {name: 'Charlie', email: 'charlie@test.com'})");

	auto r = execute("MATCH (n:NNNode) WHERE n.email IS NOT NULL RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Alice");
	EXPECT_EQ(val(r, "n.name", 1), "Charlie");
}

// ============================================================================
// IN Operator
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, WhereInIntegers) {
	(void) execute("CREATE (a:INNode {name: 'Alice', age: 30})");
	(void) execute("CREATE (b:INNode {name: 'Bob', age: 25})");
	(void) execute("CREATE (c:INNode {name: 'Charlie', age: 35})");
	(void) execute("CREATE (d:INNode {name: 'Diana', age: 40})");

	auto r = execute("MATCH (n:INNode) WHERE n.age IN [25, 35] RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Bob");
	EXPECT_EQ(val(r, "n.name", 1), "Charlie");
}

TEST_F(IntegrationCypherFilteringTest, WhereInStrings) {
	(void) execute("CREATE (a:ISNode {name: 'Alice'})");
	(void) execute("CREATE (b:ISNode {name: 'Bob'})");
	(void) execute("CREATE (c:ISNode {name: 'Charlie'})");

	auto r = execute("MATCH (n:ISNode) WHERE n.name IN ['Alice', 'Charlie'] RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Alice");
	EXPECT_EQ(val(r, "n.name", 1), "Charlie");
}

TEST_F(IntegrationCypherFilteringTest, WhereInNoMatch) {
	(void) execute("CREATE (a:INMNode {val: 1})");
	(void) execute("CREATE (b:INMNode {val: 2})");

	auto r = execute("MATCH (n:INMNode) WHERE n.val IN [10, 20] RETURN n.val");
	EXPECT_EQ(r.rowCount(), 0UL);
}

// ============================================================================
// String Predicates in WHERE
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, WhereStartsWith) {
	(void) execute("CREATE (a:SWFNode {name: 'Alice'})");
	(void) execute("CREATE (b:SWFNode {name: 'Adam'})");
	(void) execute("CREATE (c:SWFNode {name: 'Bob'})");
	(void) execute("CREATE (d:SWFNode {name: 'Anna'})");

	auto r = execute("MATCH (n:SWFNode) WHERE n.name STARTS WITH 'A' RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "n.name", 0), "Adam");
	EXPECT_EQ(val(r, "n.name", 1), "Alice");
	EXPECT_EQ(val(r, "n.name", 2), "Anna");
}

TEST_F(IntegrationCypherFilteringTest, WhereEndsWith) {
	(void) execute("CREATE (a:EWFNode {name: 'Alice'})");
	(void) execute("CREATE (b:EWFNode {name: 'Bob'})");
	(void) execute("CREATE (c:EWFNode {name: 'Grace'})");

	auto r = execute("MATCH (n:EWFNode) WHERE n.name ENDS WITH 'e' RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Alice");
	EXPECT_EQ(val(r, "n.name", 1), "Grace");
}

TEST_F(IntegrationCypherFilteringTest, WhereContains) {
	(void) execute("CREATE (a:CnFNode {name: 'Alexander'})");
	(void) execute("CREATE (b:CnFNode {name: 'Bob'})");
	(void) execute("CREATE (c:CnFNode {name: 'Alexandra'})");

	auto r = execute("MATCH (n:CnFNode) WHERE n.name CONTAINS 'Alex' RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Alexander");
	EXPECT_EQ(val(r, "n.name", 1), "Alexandra");
}

// ============================================================================
// NULL Propagation in WHERE
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, NullAndTrue) {
	// null AND true = null, so the WHERE condition is not satisfied
	auto r = execute("RETURN null AND true AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "null");
}

TEST_F(IntegrationCypherFilteringTest, NullOrFalse) {
	// null OR false = null
	auto r = execute("RETURN null OR false AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "null");
}

TEST_F(IntegrationCypherFilteringTest, FalseAndNull) {
	// false AND null = false
	auto r = execute("RETURN false AND null AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "false");
}

TEST_F(IntegrationCypherFilteringTest, TrueOrNull) {
	// true OR null = true
	auto r = execute("RETURN true OR null AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "true");
}

TEST_F(IntegrationCypherFilteringTest, NullComparisonFiltering) {
	// Nodes with missing properties should be filtered out by comparison
	(void) execute("CREATE (a:NCNode {name: 'Alice', score: 80})");
	(void) execute("CREATE (b:NCNode {name: 'Bob'})"); // no score
	(void) execute("CREATE (c:NCNode {name: 'Charlie', score: 90})");

	auto r = execute("MATCH (n:NCNode) WHERE n.score > 70 RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Alice");
	EXPECT_EQ(val(r, "n.name", 1), "Charlie");
}

// ============================================================================
// SET with Expression
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, SetWithExpression) {
	(void) execute("CREATE (n:SExNode {val: 10})");
	(void) execute("MATCH (n:SExNode) SET n.val = n.val + 5");

	auto r = execute("MATCH (n:SExNode) RETURN n.val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.val"), "15");
}

TEST_F(IntegrationCypherFilteringTest, SetWithMultiplication) {
	(void) execute("CREATE (n:SMNode {val: 7})");
	(void) execute("MATCH (n:SMNode) SET n.val = n.val * 3");

	auto r = execute("MATCH (n:SMNode) RETURN n.val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.val"), "21");
}

TEST_F(IntegrationCypherFilteringTest, SetWithStringConcat) {
	(void) execute("CREATE (n:SSNode {first: 'Hello', last: 'World'})");
	(void) execute("MATCH (n:SSNode) SET n.full = n.first + ' ' + n.last");

	auto r = execute("MATCH (n:SSNode) RETURN n.full");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.full"), "Hello World");
}

// ============================================================================
// SET += map merge
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, SetMapMerge) {
	(void) execute("CREATE (n:MMNode {name: 'Alice', age: 30})");
	(void) execute("MATCH (n:MMNode) SET n += {city: 'NYC', active: true}");

	auto r = execute("MATCH (n:MMNode) RETURN n.name, n.age, n.city, n.active");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Alice");
	EXPECT_EQ(val(r, "n.age"), "30");
	EXPECT_EQ(val(r, "n.city"), "NYC");
	EXPECT_EQ(val(r, "n.active"), "true");
}

// ============================================================================
// REMOVE label
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, RemoveLabel) {
	(void) execute("CREATE (n:RLNode {name: 'Test'})");

	// SET appends a label (multi-label support)
	(void) execute("MATCH (n:RLNode) SET n:NewLabel");

	// Node should have BOTH labels now
	auto rOld = execute("MATCH (n:RLNode) RETURN n.name");
	EXPECT_EQ(rOld.rowCount(), 1UL);

	auto rNew = execute("MATCH (n:NewLabel) RETURN n.name");
	ASSERT_EQ(rNew.rowCount(), 1UL);
	EXPECT_EQ(val(rNew, "n.name"), "Test");

	// Now REMOVE the original label
	(void) execute("MATCH (n:RLNode) REMOVE n:RLNode");
	auto rRemoved = execute("MATCH (n:RLNode) RETURN n.name");
	EXPECT_EQ(rRemoved.rowCount(), 0UL);

	// NewLabel should still exist
	auto rStill = execute("MATCH (n:NewLabel) RETURN n.name");
	ASSERT_EQ(rStill.rowCount(), 1UL);
}

// ============================================================================
// REMOVE property
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, RemoveProperty) {
	(void) execute("CREATE (n:RPNode {name: 'Test', age: 30, city: 'NYC'})");
	(void) execute("MATCH (n:RPNode) REMOVE n.age");

	auto r = execute("MATCH (n:RPNode) RETURN n.name, n.age, n.city");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Test");
	EXPECT_EQ(val(r, "n.age"), "null");
	EXPECT_EQ(val(r, "n.city"), "NYC");
}

// ============================================================================
// Complex filtering scenarios
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, ComparisonWithMultipleConditions) {
	(void) execute("CREATE (a:CMNode {name: 'Alice', age: 30, score: 85})");
	(void) execute("CREATE (b:CMNode {name: 'Bob', age: 25, score: 90})");
	(void) execute("CREATE (c:CMNode {name: 'Charlie', age: 35, score: 70})");
	// Two-condition AND: age > 25 AND score > 80
	auto r = execute("MATCH (n:CMNode) WHERE n.age > 25 AND n.score > 80 RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name", 0), "Alice");
}

TEST_F(IntegrationCypherFilteringTest, NestedLogicalOperators) {
	(void) execute("CREATE (a:NLNode {name: 'Alice', x: 1, y: 1})");
	(void) execute("CREATE (b:NLNode {name: 'Bob', x: 1, y: 2})");
	(void) execute("CREATE (c:NLNode {name: 'Charlie', x: 2, y: 1})");
	(void) execute("CREATE (d:NLNode {name: 'Diana', x: 2, y: 2})");

	// Only nodes where x=1 AND y=1, OR x=2 AND y=2
	auto r = execute("MATCH (n:NLNode) WHERE (n.x = 1 AND n.y = 1) OR (n.x = 2 AND n.y = 2) RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Alice");
	EXPECT_EQ(val(r, "n.name", 1), "Diana");
}

// ============================================================================
// List comprehension for quantifier-like filtering
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, ListComprehensionFilterAll) {
	// Equivalent of "all even": filter elements that are NOT even, expect empty result
	auto r = execute("RETURN [x IN [2, 4, 6] WHERE x % 2 <> 0] AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "[]");
}

TEST_F(IntegrationCypherFilteringTest, ListComprehensionFilterSome) {
	// Filter even numbers from a mixed list
	auto r = execute("RETURN [x IN [1, 2, 3, 4, 5] WHERE x % 2 = 0] AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "[2, 4]");
}

TEST_F(IntegrationCypherFilteringTest, ListComprehensionFilterNone) {
	// Filter elements > 10, expect empty (no matches)
	auto r = execute("RETURN [x IN [1, 3, 5] WHERE x > 10] AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "[]");
}

TEST_F(IntegrationCypherFilteringTest, ListComprehensionFilterSingle) {
	// Filter elements > 2, expect exactly one match
	auto r = execute("RETURN size([x IN [1, 2, 3] WHERE x > 2]) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "1");
}

TEST_F(IntegrationCypherFilteringTest, ListComprehensionWithTransform) {
	// Filter and transform: double the even numbers
	auto r = execute("RETURN [x IN [1, 2, 3, 4] WHERE x % 2 = 0 | x * 2] AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "[4, 8]");
}

// ============================================================================
// Filtering on relationship properties
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, FilterOnRelationshipProperty) {
	(void) execute("CREATE (a:FRNode {name: 'Alice'})-[:RATED {score: 5}]->(m:FRMovie {title: 'Matrix'})");
	(void) execute("CREATE (a:FRNode {name: 'Alice'})-[:RATED {score: 3}]->(m:FRMovie {title: 'Avatar'})");

	auto r = execute("MATCH (n:FRNode)-[r:RATED]->(m:FRMovie) WHERE r.score >= 4 RETURN m.title");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "m.title"), "Matrix");
}

// ============================================================================
// Equality on boolean
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, BooleanEquality) {
	(void) execute("CREATE (a:BENode {name: 'Active', status: true})");
	(void) execute("CREATE (b:BENode {name: 'Inactive', status: false})");

	auto r = execute("MATCH (n:BENode) WHERE n.status = true RETURN n.name");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Active");
}

// ============================================================================
// Not equals
// ============================================================================

TEST_F(IntegrationCypherFilteringTest, NotEquals) {
	(void) execute("CREATE (a:NENode {name: 'Alice'})");
	(void) execute("CREATE (b:NENode {name: 'Bob'})");
	(void) execute("CREATE (c:NENode {name: 'Charlie'})");

	auto r = execute("MATCH (n:NENode) WHERE n.name <> 'Bob' RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Alice");
	EXPECT_EQ(val(r, "n.name", 1), "Charlie");
}
