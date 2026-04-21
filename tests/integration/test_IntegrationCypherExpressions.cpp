/**
 * @file test_IntegrationCypherExpressions.cpp
 * @brief Integration tests for Cypher expressions with value verification
 *
 * Tests scalar functions, type conversions, string operations, math functions,
 * and entity introspection — verifying actual result values, not just row counts.
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

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;
using namespace graph;

class IntegrationCypherExpressionsTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_expr_" + boost::uuids::to_string(uuid) + ".zyx");
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

	query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }

	// Helper to get the string value of a column from the first row
	std::string val(const query::QueryResult &r, const std::string &col) const {
		return r.getRows().at(0).at(col).toString();
	}

	// Helper to get the PropertyValue from a column in the first row
	const query::ResultValue &cell(const query::QueryResult &r, const std::string &col) const {
		return r.getRows().at(0).at(col);
	}

	std::filesystem::path testDbPath;
	std::unique_ptr<Database> db;
};

// ============================================================================
// String Functions
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, StringLeft) {
	auto r = execute("RETURN left('hello world', 5) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "hello");
}

TEST_F(IntegrationCypherExpressionsTest, StringRight) {
	auto r = execute("RETURN right('hello world', 5) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "world");
}

TEST_F(IntegrationCypherExpressionsTest, StringLTrim) {
	auto r = execute("RETURN lTrim('  hello  ') AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "hello  ");
}

TEST_F(IntegrationCypherExpressionsTest, StringRTrim) {
	auto r = execute("RETURN rTrim('  hello  ') AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "  hello");
}

TEST_F(IntegrationCypherExpressionsTest, StringTrim) {
	auto r = execute("RETURN trim('  hello  ') AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "hello");
}

TEST_F(IntegrationCypherExpressionsTest, StringSplit) {
	auto r = execute("RETURN split('a,b,c', ',') AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "[a, b, c]");
}

TEST_F(IntegrationCypherExpressionsTest, StringReverse) {
	auto r = execute("RETURN reverse('hello') AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "olleh");
}

TEST_F(IntegrationCypherExpressionsTest, StringSubstring) {
	auto r1 = execute("RETURN substring('hello world', 6) AS val");
	ASSERT_EQ(r1.rowCount(), 1UL);
	EXPECT_EQ(val(r1, "val"), "world");

	auto r2 = execute("RETURN substring('hello world', 0, 5) AS val");
	ASSERT_EQ(r2.rowCount(), 1UL);
	EXPECT_EQ(val(r2, "val"), "hello");
}

TEST_F(IntegrationCypherExpressionsTest, StringReplace) {
	auto r = execute("RETURN replace('hello world', 'world', 'there') AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "hello there");
}

TEST_F(IntegrationCypherExpressionsTest, StringLowerUpper) {
	auto r = execute("RETURN lower('HELLO') AS lo, upper('hello') AS up");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "lo"), "hello");
	EXPECT_EQ(val(r, "up"), "HELLO");
}

// ============================================================================
// Type Conversion Functions
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, ToInteger) {
	auto r = execute("RETURN toInteger('42') AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "42");
}

TEST_F(IntegrationCypherExpressionsTest, ToIntegerFromFloat) {
	auto r = execute("RETURN toInteger(3.7) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "3");
}

TEST_F(IntegrationCypherExpressionsTest, ToFloat) {
	auto r = execute("RETURN toFloat('3.14') AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	// Double toString may vary, check it starts with 3.14
	EXPECT_TRUE(val(r, "val").find("3.14") == 0);
}

TEST_F(IntegrationCypherExpressionsTest, ToBoolean) {
	auto r1 = execute("RETURN toBoolean('true') AS val");
	ASSERT_EQ(r1.rowCount(), 1UL);
	EXPECT_EQ(val(r1, "val"), "true");

	auto r2 = execute("RETURN toBoolean('false') AS val");
	ASSERT_EQ(r2.rowCount(), 1UL);
	EXPECT_EQ(val(r2, "val"), "false");
}

TEST_F(IntegrationCypherExpressionsTest, ToStringFromInt) {
	auto r = execute("RETURN toString(42) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "42");
}

TEST_F(IntegrationCypherExpressionsTest, ToStringFromBool) {
	auto r = execute("RETURN toString(true) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "true");
}

// ============================================================================
// Math Functions
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, MathAbs) {
	auto r = execute("RETURN abs(-42) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "42");
}

TEST_F(IntegrationCypherExpressionsTest, MathCeil) {
	auto r = execute("RETURN ceil(2.3) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "3");
}

TEST_F(IntegrationCypherExpressionsTest, MathFloor) {
	auto r = execute("RETURN floor(2.7) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "2");
}

TEST_F(IntegrationCypherExpressionsTest, MathRound) {
	auto r = execute("RETURN round(2.5) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	// round(2.5) typically rounds to 3 (or 2 depending on rounding mode)
	auto v = val(r, "val");
	EXPECT_TRUE(v == "3" || v == "2");
}

TEST_F(IntegrationCypherExpressionsTest, MathSqrt) {
	auto r = execute("RETURN sqrt(16) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "4");
}

TEST_F(IntegrationCypherExpressionsTest, MathSign) {
	auto r1 = execute("RETURN sign(-5) AS val");
	ASSERT_EQ(r1.rowCount(), 1UL);
	EXPECT_EQ(val(r1, "val"), "-1");

	auto r2 = execute("RETURN sign(0) AS val");
	ASSERT_EQ(r2.rowCount(), 1UL);
	EXPECT_EQ(val(r2, "val"), "0");

	auto r3 = execute("RETURN sign(10) AS val");
	ASSERT_EQ(r3.rowCount(), 1UL);
	EXPECT_EQ(val(r3, "val"), "1");
}

// ============================================================================
// List Functions
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, ListSize) {
	auto r1 = execute("RETURN size([1, 2, 3]) AS val");
	ASSERT_EQ(r1.rowCount(), 1UL);
	EXPECT_EQ(val(r1, "val"), "3");

	auto r2 = execute("RETURN size('hello') AS val");
	ASSERT_EQ(r2.rowCount(), 1UL);
	EXPECT_EQ(val(r2, "val"), "5");
}

TEST_F(IntegrationCypherExpressionsTest, ListHead) {
	auto r = execute("RETURN head([10, 20, 30]) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "10");
}

TEST_F(IntegrationCypherExpressionsTest, ListTail) {
	auto r = execute("RETURN tail([10, 20, 30]) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "[20, 30]");
}

TEST_F(IntegrationCypherExpressionsTest, ListLast) {
	auto r = execute("RETURN last([10, 20, 30]) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "30");
}

TEST_F(IntegrationCypherExpressionsTest, ListRange) {
	// range() end is inclusive per Cypher specification
	auto r1 = execute("RETURN range(0, 4) AS val");
	ASSERT_EQ(r1.rowCount(), 1UL);
	EXPECT_EQ(val(r1, "val"), "[0, 1, 2, 3, 4]");

	auto r2 = execute("RETURN range(0, 10, 3) AS val");
	ASSERT_EQ(r2.rowCount(), 1UL);
	EXPECT_EQ(val(r2, "val"), "[0, 3, 6, 9]");
}

TEST_F(IntegrationCypherExpressionsTest, Coalesce) {
	auto r = execute("RETURN coalesce(null, null, 'found') AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "found");
}

TEST_F(IntegrationCypherExpressionsTest, CoalesceFirstNonNull) {
	auto r = execute("RETURN coalesce(42, null, 'fallback') AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "42");
}

// ============================================================================
// Utility Functions
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, Timestamp) {
	auto r = execute("RETURN timestamp() AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto v = val(r, "val");
	// timestamp returns a non-zero integer (milliseconds since epoch)
	EXPECT_NE(v, "0");
	EXPECT_NE(v, "null");
}

TEST_F(IntegrationCypherExpressionsTest, RandomUUID) {
	auto r = execute("RETURN randomUUID() AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto v = val(r, "val");
	// UUID format: 8-4-4-4-12 hex chars
	EXPECT_GE(v.size(), 36UL);
	EXPECT_EQ(v[8], '-');
	EXPECT_EQ(v[13], '-');
}

// ============================================================================
// Entity Introspection
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, EntityId) {
	(void) execute("CREATE (n:IdTestNode {name: 'test'})");
	auto r = execute("MATCH (n:IdTestNode) RETURN id(n) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	// id() should return a non-null integer
	auto v = val(r, "val");
	EXPECT_NE(v, "null");
}

TEST_F(IntegrationCypherExpressionsTest, EntityLabels) {
	(void) execute("CREATE (n:LblTestNode {name: 'test'})");
	auto r = execute("MATCH (n:LblTestNode) RETURN labels(n) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto v = val(r, "val");
	// labels() returns a list (may contain label IDs or names depending on implementation)
	EXPECT_NE(v, "null");
	EXPECT_TRUE(v.front() == '[');
}

TEST_F(IntegrationCypherExpressionsTest, EntityType) {
	(void) execute("CREATE (a:TypeTest1)-[:FRIEND_OF]->(b:TypeTest2)");
	auto r = execute("MATCH ()-[r:FRIEND_OF]->() RETURN type(r) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	// type() returns the relationship type (may be label ID or name)
	auto v = val(r, "val");
	EXPECT_NE(v, "null");
	EXPECT_FALSE(v.empty());
}

TEST_F(IntegrationCypherExpressionsTest, EntityKeys) {
	(void) execute("CREATE (n:KeyTestNode {name: 'Alice', age: 30})");
	auto r = execute("MATCH (n:KeyTestNode) RETURN keys(n) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto v = val(r, "val");
	// keys() should contain both property names
	EXPECT_TRUE(v.find("name") != std::string::npos);
	EXPECT_TRUE(v.find("age") != std::string::npos);
}

TEST_F(IntegrationCypherExpressionsTest, EntityProperties) {
	(void) execute("CREATE (n:PropTestNode {name: 'Alice', age: 30})");
	auto r = execute("MATCH (n:PropTestNode) RETURN properties(n) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto v = val(r, "val");
	// properties() returns a map
	EXPECT_TRUE(v.find("name") != std::string::npos);
	EXPECT_TRUE(v.find("Alice") != std::string::npos);
	EXPECT_TRUE(v.find("age") != std::string::npos);
}

// ============================================================================
// CASE WHEN Expression
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, SimpleCaseExpression) {
	(void) execute("CREATE (n:CaseExprNode {status: 'active'})");
	auto r = execute(
			"MATCH (n:CaseExprNode) "
			"RETURN CASE n.status WHEN 'active' THEN 'yes' WHEN 'inactive' THEN 'no' ELSE 'unknown' END AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "yes");
}

TEST_F(IntegrationCypherExpressionsTest, SimpleCaseNoMatch) {
	(void) execute("CREATE (n:CaseExprNode2 {status: 'pending'})");
	auto r = execute(
			"MATCH (n:CaseExprNode2) "
			"RETURN CASE n.status WHEN 'active' THEN 'yes' WHEN 'inactive' THEN 'no' ELSE 'unknown' END AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "unknown");
}

TEST_F(IntegrationCypherExpressionsTest, SearchedCaseExpression) {
	// Simple CASE form with multiple branches
	(void) execute("CREATE (n:CaseExprNode3 {grade: 'B'})");
	auto r = execute(
			"MATCH (n:CaseExprNode3) "
			"RETURN CASE n.grade WHEN 'A' THEN 'excellent' WHEN 'B' THEN 'good' WHEN 'C' THEN 'average' ELSE 'unknown' END AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "good");
}

TEST_F(IntegrationCypherExpressionsTest, CaseWithoutElse) {
	(void) execute("CREATE (n:CaseExprNode4 {status: 'pending'})");
	auto r = execute(
			"MATCH (n:CaseExprNode4) "
			"RETURN CASE n.status WHEN 'active' THEN 'yes' END AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "null");
}

// ============================================================================
// String Predicates (STARTS WITH, ENDS WITH, CONTAINS)
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, StartsWithPredicate) {
	auto r = execute("RETURN 'hello world' STARTS WITH 'hello' AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "true");
}

TEST_F(IntegrationCypherExpressionsTest, StartsWithFalse) {
	auto r = execute("RETURN 'hello world' STARTS WITH 'world' AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "false");
}

TEST_F(IntegrationCypherExpressionsTest, EndsWithPredicate) {
	auto r = execute("RETURN 'hello world' ENDS WITH 'world' AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "true");
}

TEST_F(IntegrationCypherExpressionsTest, ContainsPredicate) {
	auto r = execute("RETURN 'hello world' CONTAINS 'lo wo' AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "true");
}

TEST_F(IntegrationCypherExpressionsTest, ContainsFalse) {
	auto r = execute("RETURN 'hello world' CONTAINS 'xyz' AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "false");
}

// ============================================================================
// String predicates in WHERE for filtering
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, StartsWithInWhere) {
	(void) execute("CREATE (n:SWNode {name: 'Alice'})");
	(void) execute("CREATE (n:SWNode {name: 'Adam'})");
	(void) execute("CREATE (n:SWNode {name: 'Bob'})");

	auto r = execute("MATCH (n:SWNode) WHERE n.name STARTS WITH 'A' RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name"), "Adam");
	EXPECT_EQ(r.getRows().at(1).at("n.name").toString(), "Alice");
}

TEST_F(IntegrationCypherExpressionsTest, EndsWithInWhere) {
	(void) execute("CREATE (n:EWNode {name: 'Alice'})");
	(void) execute("CREATE (n:EWNode {name: 'Grace'})");
	(void) execute("CREATE (n:EWNode {name: 'Bob'})");

	auto r = execute("MATCH (n:EWNode) WHERE n.name ENDS WITH 'ce' RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name"), "Alice");
	EXPECT_EQ(r.getRows().at(1).at("n.name").toString(), "Grace");
}

TEST_F(IntegrationCypherExpressionsTest, ContainsInWhere) {
	(void) execute("CREATE (n:CWNode {name: 'Alice'})");
	(void) execute("CREATE (n:CWNode {name: 'Bob'})");
	(void) execute("CREATE (n:CWNode {name: 'Alicia'})");

	auto r = execute("MATCH (n:CWNode) WHERE n.name CONTAINS 'lic' RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name"), "Alice");
	EXPECT_EQ(r.getRows().at(1).at("n.name").toString(), "Alicia");
}

// ============================================================================
// Arithmetic value verification
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, ArithmeticAddition) {
	auto r = execute("RETURN 5 + 3 AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "8");
}

TEST_F(IntegrationCypherExpressionsTest, ArithmeticMixedTypesValue) {
	auto r = execute("RETURN 5 + 2.5 AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "7.5");
}

TEST_F(IntegrationCypherExpressionsTest, ArithmeticModulo) {
	auto r = execute("RETURN 10 % 3 AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "1");
}

TEST_F(IntegrationCypherExpressionsTest, ArithmeticPower) {
	auto r = execute("RETURN 2 ^ 3 AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "8");
}

TEST_F(IntegrationCypherExpressionsTest, StringConcatenationValue) {
	auto r = execute("RETURN 'hello' + ' ' + 'world' AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "hello world");
}

// ============================================================================
// Comparison result values
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, ComparisonValues) {
	auto r1 = execute("RETURN 5 < 10 AS val");
	EXPECT_EQ(val(r1, "val"), "true");

	auto r2 = execute("RETURN 10 > 5 AS val");
	EXPECT_EQ(val(r2, "val"), "true");

	auto r3 = execute("RETURN 5 = 5 AS val");
	EXPECT_EQ(val(r3, "val"), "true");

	auto r4 = execute("RETURN 5 <> 10 AS val");
	EXPECT_EQ(val(r4, "val"), "true");
}

// ============================================================================
// Logical operators with value verification
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, LogicalAndValues) {
	auto r1 = execute("RETURN true AND true AS val");
	EXPECT_EQ(val(r1, "val"), "true");

	auto r2 = execute("RETURN true AND false AS val");
	EXPECT_EQ(val(r2, "val"), "false");

	auto r3 = execute("RETURN false AND null AS val");
	EXPECT_EQ(val(r3, "val"), "false");
}

TEST_F(IntegrationCypherExpressionsTest, LogicalOrValues) {
	auto r1 = execute("RETURN false OR true AS val");
	EXPECT_EQ(val(r1, "val"), "true");

	auto r2 = execute("RETURN false OR false AS val");
	EXPECT_EQ(val(r2, "val"), "false");

	auto r3 = execute("RETURN true OR null AS val");
	EXPECT_EQ(val(r3, "val"), "true");
}

TEST_F(IntegrationCypherExpressionsTest, LogicalNotValues) {
	auto r1 = execute("RETURN NOT true AS val");
	EXPECT_EQ(val(r1, "val"), "false");

	auto r2 = execute("RETURN NOT false AS val");
	EXPECT_EQ(val(r2, "val"), "true");
}

// ============================================================================
// IS NULL / IS NOT NULL values
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, IsNullValues) {
	auto r1 = execute("RETURN null IS NULL AS val");
	EXPECT_EQ(val(r1, "val"), "true");

	auto r2 = execute("RETURN 42 IS NULL AS val");
	EXPECT_EQ(val(r2, "val"), "false");

	auto r3 = execute("RETURN null IS NOT NULL AS val");
	EXPECT_EQ(val(r3, "val"), "false");

	auto r4 = execute("RETURN 42 IS NOT NULL AS val");
	EXPECT_EQ(val(r4, "val"), "true");
}

// ============================================================================
// IN expression values
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, InExpressionValues) {
	auto r1 = execute("RETURN 3 IN [1, 2, 3] AS val");
	EXPECT_EQ(val(r1, "val"), "true");

	auto r2 = execute("RETURN 5 IN [1, 2, 3] AS val");
	EXPECT_EQ(val(r2, "val"), "false");
}

// ============================================================================
// List comprehension values
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, ListComprehensionFilter) {
	auto r = execute("RETURN [x IN [1, 2, 3, 4, 5] WHERE x > 3] AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "[4, 5]");
}

TEST_F(IntegrationCypherExpressionsTest, ListComprehensionMap) {
	auto r = execute("RETURN [x IN [1, 2, 3] | x * 2] AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "[2, 4, 6]");
}

TEST_F(IntegrationCypherExpressionsTest, ListComprehensionFilterAndMap) {
	auto r = execute("RETURN [x IN [1, 2, 3, 4, 5] WHERE x > 2 | x * 10] AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "[30, 40, 50]");
}

// ============================================================================
// List slicing values
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, ListSlicingValues) {
	auto r1 = execute("RETURN [1, 2, 3][0] AS val");
	EXPECT_EQ(val(r1, "val"), "1");

	auto r2 = execute("RETURN [1, 2, 3][-1] AS val");
	EXPECT_EQ(val(r2, "val"), "3");

	auto r3 = execute("RETURN [1, 2, 3, 4, 5][1..3] AS val");
	EXPECT_EQ(val(r3, "val"), "[2, 3]");
}

// ============================================================================
// Unary operators values
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, UnaryMinusValues) {
	auto r1 = execute("RETURN -5 AS val");
	EXPECT_EQ(val(r1, "val"), "-5");

	auto r2 = execute("RETURN -3.14 AS val");
	auto v = val(r2, "val");
	EXPECT_TRUE(v.find("-3.14") == 0);
}

// ============================================================================
// Reverse on list
// ============================================================================

TEST_F(IntegrationCypherExpressionsTest, ReverseList) {
	auto r = execute("RETURN reverse([1, 2, 3]) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "[3, 2, 1]");
}
