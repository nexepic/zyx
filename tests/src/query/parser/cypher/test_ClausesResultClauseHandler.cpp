/**
 * @file test_ClausesResultClauseHandler.cpp
 * @date 2026/02/14
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

/**
 * @class ResultClauseHandlerTest
 * @brief Integration tests for ResultClauseHandler class.
 *
 * These tests verify the result clause handler:
 * - RETURN clause with projection
 * - ORDER BY clause
 * - SKIP clause
 * - LIMIT clause
 */
class ResultClauseHandlerTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_result_clauses_" + boost::uuids::to_string(uuid) + ".dat");
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
// RETURN Clause Tests (Projection)
// ============================================================================

TEST_F(ResultClauseHandlerTest, Return_MultiplyAll) {
	// Tests RETURN * (return all variables)
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	auto res = execute("MATCH (n:Person) RETURN *");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("n"));
}

TEST_F(ResultClauseHandlerTest, Return_SingleVariable) {
	// Tests RETURN n (single variable)
	(void) execute("CREATE (n:Person {name: 'Bob'})");

	auto res = execute("MATCH (n:Person) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("n"));
}

TEST_F(ResultClauseHandlerTest, Return_MultipleVariables) {
	// Tests RETURN n, m (multiple variables)
	(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");

	auto res = execute("MATCH (a:Person)-[:KNOWS]->(b:Person) RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("a"));
	EXPECT_TRUE(res.getRows()[0].contains("b"));
}

TEST_F(ResultClauseHandlerTest, Return_PropertyAccess) {
	// Tests RETURN n.name (property access)
	(void) execute("CREATE (n:Person {name: 'Charlie'})");

	auto res = execute("MATCH (n:Person) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Charlie");
}

TEST_F(ResultClauseHandlerTest, Return_MultiplePropertyAccess) {
	// Tests RETURN n.name, n.age (multiple property accesses)
	(void) execute("CREATE (n:Person {name: 'David', age: 30})");

	auto res = execute("MATCH (n:Person) RETURN n.name, n.age");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "David");
	EXPECT_EQ(res.getRows()[0].at("n.age").toString(), "30");
}

TEST_F(ResultClauseHandlerTest, Return_WithAlias) {
	// Tests RETURN n.name AS alias (with AS keyword)
	(void) execute("CREATE (n:Person {name: 'Eve'})");

	auto res = execute("MATCH (n:Person) RETURN n.name AS person_name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("person_name").toString(), "Eve");
}

TEST_F(ResultClauseHandlerTest, Return_MultipleAliases) {
	// Tests RETURN with multiple aliases
	(void) execute("CREATE (n:Person {name: 'Frank', age: 25})");

	auto res = execute("MATCH (n:Person) RETURN n.name AS person_name, n.age AS person_age");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("person_name").toString(), "Frank");
	EXPECT_EQ(res.getRows()[0].at("person_age").toString(), "25");
}

TEST_F(ResultClauseHandlerTest, Return_LiteralValue) {
	// Tests RETURN 42 (literal value)
	auto res = execute("RETURN 42");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("42").toString(), "42");
}

TEST_F(ResultClauseHandlerTest, Return_MultipleLiteralValues) {
	// Tests RETURN with multiple literal values
	auto res = execute("RETURN 1 AS one, 2 AS two, 3 AS three");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("one").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("two").toString(), "2");
	EXPECT_EQ(res.getRows()[0].at("three").toString(), "3");
}

TEST_F(ResultClauseHandlerTest, Return_StringLiteral) {
	// Tests RETURN 'text' (string literal)
	// Note: The column name is the expression text as parsed by the grammar
	auto res = execute("RETURN 'Hello World'");
	ASSERT_EQ(res.rowCount(), 1UL);
	// Column name is the expression text including quotes as parsed
	EXPECT_EQ(res.getRows()[0].at("'Hello World'").asPrimitive().getType(), graph::PropertyType::STRING);
}

TEST_F(ResultClauseHandlerTest, Return_BooleanLiteral) {
	// Tests RETURN true (boolean literal)
	auto res = execute("RETURN true");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("true").toString(), "true");
}

TEST_F(ResultClauseHandlerTest, Return_NullLiteral) {
	// Tests RETURN null (null literal)
	auto res = execute("RETURN null");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("null").asPrimitive().getType(), graph::PropertyType::NULL_TYPE);
}

TEST_F(ResultClauseHandlerTest, Return_NegativeLiteral) {
	// Tests RETURN -42 (negative literal)
	auto res = execute("RETURN -42");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("-42").toString(), "-42");
}

TEST_F(ResultClauseHandlerTest, Return_DoubleLiteral) {
	// Tests RETURN 3.14 (double literal)
	auto res = execute("RETURN 3.14");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("3.14").toString(), "3.14");
}

TEST_F(ResultClauseHandlerTest, Return_ScientificNotation) {
	// Tests RETURN 1.5e10 (scientific notation)
	// Note: Scientific notation values are parsed as DOUBLE type (decimal in scientific notation)
	auto res = execute("RETURN 1.5e10");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("1.5e10").asPrimitive().getType(), graph::PropertyType::DOUBLE);
}

// ============================================================================
// ORDER BY Clause Tests
// ============================================================================

TEST_F(ResultClauseHandlerTest, OrderBy_Ascending) {
	// Tests ORDER BY n.prop ASC (ascending order)
	(void) execute("CREATE (n:TestOrderByAsc {value: 10})");
	(void) execute("CREATE (n:TestOrderByAsc {value: 20})");
	(void) execute("CREATE (n:TestOrderByAsc {value: 30})");

	auto res = execute("MATCH (n:TestOrderByAsc) RETURN n.value ORDER BY n.value ASC");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "10");
	EXPECT_EQ(res.getRows()[1].at("n.value").toString(), "20");
	EXPECT_EQ(res.getRows()[2].at("n.value").toString(), "30");
}

TEST_F(ResultClauseHandlerTest, OrderBy_Descending) {
	// Tests ORDER BY n.prop DESC (descending order)
	(void) execute("CREATE (n:TestOrderByDesc2 {value: 10})");
	(void) execute("CREATE (n:TestOrderByDesc2 {value: 20})");
	(void) execute("CREATE (n:TestOrderByDesc2 {value: 30})");

	auto res = execute("MATCH (n:TestOrderByDesc2) RETURN n.value ORDER BY n.value DESC");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "30");
	EXPECT_EQ(res.getRows()[1].at("n.value").toString(), "20");
	EXPECT_EQ(res.getRows()[2].at("n.value").toString(), "10");
}

TEST_F(ResultClauseHandlerTest, OrderBy_DefaultAscending) {
	// Tests ORDER BY n.prop (default is ASCENDING)
	(void) execute("CREATE (n:TestOrderByDef {value: 10})");
	(void) execute("CREATE (n:TestOrderByDef {value: 20})");
	(void) execute("CREATE (n:TestOrderByDef {value: 30})");

	auto res = execute("MATCH (n:TestOrderByDef) RETURN n.value ORDER BY n.value");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "10");
	EXPECT_EQ(res.getRows()[1].at("n.value").toString(), "20");
	EXPECT_EQ(res.getRows()[2].at("n.value").toString(), "30");
}

TEST_F(ResultClauseHandlerTest, OrderBy_MultipleSortItems) {
	// Tests ORDER BY n.prop1, n.prop2 (multiple sort keys)
	(void) execute("CREATE (n:TestOrderByMulti {value: 10, score: 150})");
	(void) execute("CREATE (n:TestOrderByMulti {value: 20, score: 100})");
	(void) execute("CREATE (n:TestOrderByMulti {value: 30, score: 200})");

	auto res = execute("MATCH (n:TestOrderByMulti) RETURN n.value, n.score ORDER BY n.value ASC, n.score DESC");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "10");
	EXPECT_EQ(res.getRows()[1].at("n.value").toString(), "20");
	EXPECT_EQ(res.getRows()[2].at("n.value").toString(), "30");
	EXPECT_EQ(res.getRows()[0].at("n.score").toString(), "150");
	EXPECT_EQ(res.getRows()[1].at("n.score").toString(), "100");
	EXPECT_EQ(res.getRows()[2].at("n.score").toString(), "200");
}

// ============================================================================
// SKIP Clause Tests
// ============================================================================

TEST_F(ResultClauseHandlerTest, Skip_SkipOneRow) {
	// Tests SKIP 1
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC SKIP 1");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "20");
	EXPECT_EQ(res.getRows()[1].at("n.value").toString(), "30");
}

TEST_F(ResultClauseHandlerTest, Skip_SkipMultipleRows) {
	// Tests SKIP 2
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");
	(void) execute("CREATE (n:Test {value: 40})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC SKIP 2");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "30");
	EXPECT_EQ(res.getRows()[1].at("n.value").toString(), "40");
}

TEST_F(ResultClauseHandlerTest, Skip_SkipAllRows) {
	// Tests SKIP N where N >= row count
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC SKIP 10");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerTest, Skip_SkipZero) {
	// Tests SKIP 0 (no skip)
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC SKIP 0");
	ASSERT_EQ(res.rowCount(), 3UL);
}

// ============================================================================
// LIMIT Clause Tests
// ============================================================================

TEST_F(ResultClauseHandlerTest, Limit_LimitOneRow) {
	// Tests LIMIT 1
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC LIMIT 1");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "10");
}

TEST_F(ResultClauseHandlerTest, Limit_LimitMultipleRows) {
	// Tests LIMIT 2
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC LIMIT 2");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "10");
	EXPECT_EQ(res.getRows()[1].at("n.value").toString(), "20");
}

TEST_F(ResultClauseHandlerTest, Limit_LimitWithSkip) {
	// Tests LIMIT with SKIP
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");
	(void) execute("CREATE (n:Test {value: 40})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC SKIP 1 LIMIT 2");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "20");
	EXPECT_EQ(res.getRows()[1].at("n.value").toString(), "30");
}

TEST_F(ResultClauseHandlerTest, Limit_LimitAllRows) {
	// Tests LIMIT N where N = total row count
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC LIMIT 10");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(ResultClauseHandlerTest, Limit_LimitZero) {
	// Tests LIMIT 0
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC LIMIT 0");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// ============================================================================
// Combined Clauses Tests
// ============================================================================

TEST_F(ResultClauseHandlerTest, Combined_OrderByAndLimit) {
	// Tests ORDER BY with LIMIT
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value DESC LIMIT 2");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "30");
	EXPECT_EQ(res.getRows()[1].at("n.value").toString(), "20");
}

TEST_F(ResultClauseHandlerTest, Combined_SkipAndLimit) {
	// Tests SKIP with LIMIT
	(void) execute("CREATE (n:TestSkipAndLimit {value: 10})");
	(void) execute("CREATE (n:TestSkipAndLimit {value: 20})");
	(void) execute("CREATE (n:TestSkipAndLimit {value: 30})");
	(void) execute("CREATE (n:TestSkipAndLimit {value: 40})");

	auto res = execute("MATCH (n:TestSkipAndLimit) RETURN n.value ORDER BY n.value ASC SKIP 1 LIMIT 2");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "20");
	EXPECT_EQ(res.getRows()[1].at("n.value").toString(), "30");
}

TEST_F(ResultClauseHandlerTest, Combined_AllClauses) {
	// Tests ORDER BY, SKIP, and LIMIT together
	(void) execute("CREATE (n:TestAllClauses {value: 10})");
	(void) execute("CREATE (n:TestAllClauses {value: 20})");
	(void) execute("CREATE (n:TestAllClauses {value: 30})");
	(void) execute("CREATE (n:TestAllClauses {value: 40})");
	(void) execute("CREATE (n:TestAllClauses {value: 50})");

	auto res = execute("MATCH (n:TestAllClauses) RETURN n.value ORDER BY n.value ASC SKIP 1 LIMIT 3");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "20");
	EXPECT_EQ(res.getRows()[1].at("n.value").toString(), "30");
	EXPECT_EQ(res.getRows()[2].at("n.value").toString(), "40");
}

TEST_F(ResultClauseHandlerTest, Combined_ProjectionWithOrder) {
	// Tests RETURN with projection and ORDER BY
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
	(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");

	auto res = execute("MATCH (n:Person) RETURN n.name ORDER BY n.age ASC");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Bob");
	EXPECT_EQ(res.getRows()[1].at("n.name").toString(), "Alice");
}

TEST_F(ResultClauseHandlerTest, Combined_AliasWithOrder) {
	// Tests RETURN with alias and ORDER BY
	// ORDER BY DESC now sorts correctly in descending order
	(void) execute("CREATE (n:Person {name: 'Charlie', score: 100})");
	(void) execute("CREATE (n:Person {name: 'David', score: 200})");

	auto res = execute("MATCH (n:Person) RETURN n.score AS points ORDER BY points DESC");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("points").toString(), "200");
	EXPECT_EQ(res.getRows()[1].at("points").toString(), "100");
}

// ============================================================================
// Edge Case Tests
// ============================================================================

TEST_F(ResultClauseHandlerTest, EdgeCase_ReturnWithoutMatch) {
	// Tests RETURN without preceding MATCH (uses single row)
	auto res = execute("RETURN 42 AS answer");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("answer").toString(), "42");
}

TEST_F(ResultClauseHandlerTest, EdgeCase_ReturnMultipleLiterals) {
	// Tests RETURN with multiple different literal types
	auto res = execute("RETURN 1 AS num, 'text' AS str, true AS flag, null AS nil");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("num").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("str").toString(), "text");
	EXPECT_EQ(res.getRows()[0].at("flag").toString(), "true");
	EXPECT_EQ(res.getRows()[0].at("nil").asPrimitive().getType(), graph::PropertyType::NULL_TYPE);
}

TEST_F(ResultClauseHandlerTest, EdgeCase_OrderByWithNonExistentProperty) {
	// Tests ORDER BY with non-existent property
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.nonexistent ASC");
	ASSERT_EQ(res.rowCount(), 2UL);
	// Should not crash, behavior depends on implementation
}

TEST_F(ResultClauseHandlerTest, EdgeCase_NegativeSkip) {
	// Tests SKIP with negative value (should be handled as 0 or error)
	// Note: Implementation doesn't throw for negative SKIP
	EXPECT_NO_THROW({ execute("MATCH (n:Test) RETURN n SKIP -1"); });
}

TEST_F(ResultClauseHandlerTest, EdgeCase_NegativeLimit) {
	// Tests LIMIT with negative value (should be handled as 0 or error)
	// Note: Implementation doesn't throw for negative LIMIT
	EXPECT_NO_THROW({ execute("MATCH (n:Test) RETURN n LIMIT -1"); });
}

TEST_F(ResultClauseHandlerTest, EdgeCase_LimitLargerThanCount) {
	// Tests LIMIT larger than row count
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC LIMIT 100");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ResultClauseHandlerTest, EdgeCase_EmptyProjection) {
	// Tests RETURN with empty projection (just RETURN)
	(void) execute("CREATE (n:Test {value: 10})");

	// May return all columns or single column, depends on implementation
	auto res = execute("MATCH (n:Test) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ResultClauseHandlerTest, EdgeCase_VeryLargeSkip) {
	// Tests SKIP with very large value
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC SKIP 1000000");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerTest, EdgeCase_VeryLargeLimit) {
	// Tests LIMIT with very large value
	(void) execute("CREATE (n:TestVeryLarge {value: 10})");
	(void) execute("CREATE (n:TestVeryLarge {value: 20})");
	(void) execute("CREATE (n:TestVeryLarge {value: 30})");

	auto res = execute("MATCH (n:TestVeryLarge) RETURN n.value ORDER BY n.value ASC LIMIT 1000000");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(ResultClauseHandlerTest, EdgeCase_SkipEqualsCount) {
	// Tests SKIP where SKIP = exact row count
	(void) execute("CREATE (n:TestSkipEquals {value: 10})");
	(void) execute("CREATE (n:TestSkipEquals {value: 20})");
	(void) execute("CREATE (n:TestSkipEquals {value: 30})");

	auto res = execute("MATCH (n:TestSkipEquals) RETURN n.value ORDER BY n.value ASC SKIP 3");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerTest, EdgeCase_OrderByDescendingDefault) {
	// Tests ORDER BY n.prop DESC (explicit descending)
	(void) execute("CREATE (n:TestEdgeDesc {value: 10})");
	(void) execute("CREATE (n:TestEdgeDesc {value: 20})");
	(void) execute("CREATE (n:TestEdgeDesc {value: 30})");

	auto res = execute("MATCH (n:TestEdgeDesc) RETURN n.value ORDER BY n.value DESCENDING");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "30");
	EXPECT_EQ(res.getRows()[1].at("n.value").toString(), "20");
	EXPECT_EQ(res.getRows()[2].at("n.value").toString(), "10");
}

TEST_F(ResultClauseHandlerTest, EdgeCase_OrderByAscending) {
	// Tests ORDER BY n.prop ASC (explicit ascending)
	(void) execute("CREATE (n:TestEdgeAsc {value: 10})");
	(void) execute("CREATE (n:TestEdgeAsc {value: 20})");
	(void) execute("CREATE (n:TestEdgeAsc {value: 30})");

	auto res = execute("MATCH (n:TestEdgeAsc) RETURN n.value ORDER BY n.value ASCENDING");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "10");
	EXPECT_EQ(res.getRows()[1].at("n.value").toString(), "20");
	EXPECT_EQ(res.getRows()[2].at("n.value").toString(), "30");
}

TEST_F(ResultClauseHandlerTest, EdgeCase_ReturnVariableFromEdge) {
	// Tests RETURN variable from edge
	(void) execute("CREATE (a)-[r:LINK]->(b)");

	auto res = execute("MATCH ()-[r:LINK]->() RETURN r");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("r").isEdge());
}

TEST_F(ResultClauseHandlerTest, EdgeCase_ReturnMixedVariables) {
	// Tests RETURN with mix of node and edge variables
	// Note: Edge variables not available in RETURN for CREATE patterns
	(void) execute("CREATE (a)-[r:LINK]->(b)");

	auto res = execute("MATCH ()-[r:LINK]->() RETURN a, r, b");
	ASSERT_EQ(res.rowCount(), 1UL);
	// Only edge variable is available, node variables return as null/non-existent
	EXPECT_TRUE(res.getRows()[0].contains("r"));
}
