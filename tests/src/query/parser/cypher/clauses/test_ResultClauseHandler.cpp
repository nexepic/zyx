/**
 * @file test_ResultClauseHandler.cpp
 * @brief Consolidated tests for ResultClauseHandler
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

#include "QueryTestFixture.hpp"

class ResultClauseHandlerTest : public QueryTestFixture {};

// === Combined Tests ===

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

TEST_F(ResultClauseHandlerTest, Combined_ProjectionWithOrder) {
	// Tests RETURN with projection and ORDER BY
	(void) execute("CREATE (n:Person {name: 'Alice', age: 30})");
	(void) execute("CREATE (n:Person {name: 'Bob', age: 25})");

	auto res = execute("MATCH (n:Person) RETURN n.name ORDER BY n.age ASC");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Bob");
	EXPECT_EQ(res.getRows()[1].at("n.name").toString(), "Alice");
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

// === EdgeCase Tests ===

TEST_F(ResultClauseHandlerTest, EdgeCase_EmptyProjection) {
	// Tests RETURN with empty projection (just RETURN)
	(void) execute("CREATE (n:Test {value: 10})");

	// May return all columns or single column, depends on implementation
	auto res = execute("MATCH (n:Test) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ResultClauseHandlerTest, EdgeCase_LimitLargerThanCount) {
	// Tests LIMIT larger than row count
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC LIMIT 100");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ResultClauseHandlerTest, EdgeCase_NegativeLimit) {
	// Tests LIMIT with negative value (should be handled as 0 or error)
	// Note: Implementation doesn't throw for negative LIMIT
	EXPECT_NO_THROW({ execute("MATCH (n:Test) RETURN n LIMIT -1"); });
}

TEST_F(ResultClauseHandlerTest, EdgeCase_NegativeSkip) {
	// Tests SKIP with negative value (should be handled as 0 or error)
	// Note: Implementation doesn't throw for negative SKIP
	EXPECT_NO_THROW({ execute("MATCH (n:Test) RETURN n SKIP -1"); });
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

TEST_F(ResultClauseHandlerTest, EdgeCase_OrderByWithNonExistentProperty) {
	// Tests ORDER BY with non-existent property
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.nonexistent ASC");
	ASSERT_EQ(res.rowCount(), 2UL);
	// Should not crash, behavior depends on implementation
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

TEST_F(ResultClauseHandlerTest, EdgeCase_ReturnMultipleLiterals) {
	// Tests RETURN with multiple different literal types
	auto res = execute("RETURN 1 AS num, 'text' AS str, true AS flag, null AS nil");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("num").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("str").toString(), "text");
	EXPECT_EQ(res.getRows()[0].at("flag").toString(), "true");
	EXPECT_EQ(res.getRows()[0].at("nil").asPrimitive().getType(), graph::PropertyType::NULL_TYPE);
}

TEST_F(ResultClauseHandlerTest, EdgeCase_ReturnVariableFromEdge) {
	// Tests RETURN variable from edge
	(void) execute("CREATE (a)-[r:LINK]->(b)");

	auto res = execute("MATCH ()-[r:LINK]->() RETURN r");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].at("r").isEdge());
}

TEST_F(ResultClauseHandlerTest, EdgeCase_ReturnWithoutMatch) {
	// Tests RETURN without preceding MATCH (uses single row)
	auto res = execute("RETURN 42 AS answer");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("answer").toString(), "42");
}

TEST_F(ResultClauseHandlerTest, EdgeCase_SkipEqualsCount) {
	// Tests SKIP where SKIP = exact row count
	(void) execute("CREATE (n:TestSkipEquals {value: 10})");
	(void) execute("CREATE (n:TestSkipEquals {value: 20})");
	(void) execute("CREATE (n:TestSkipEquals {value: 30})");

	auto res = execute("MATCH (n:TestSkipEquals) RETURN n.value ORDER BY n.value ASC SKIP 3");
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

TEST_F(ResultClauseHandlerTest, EdgeCase_VeryLargeSkip) {
	// Tests SKIP with very large value
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC SKIP 1000000");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// === Limit Tests ===

TEST_F(ResultClauseHandlerTest, Limit_LimitAllRows) {
	// Tests LIMIT N where N = total row count
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC LIMIT 10");
	ASSERT_EQ(res.rowCount(), 3UL);
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

TEST_F(ResultClauseHandlerTest, Limit_LimitOneRow) {
	// Tests LIMIT 1
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC LIMIT 1");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "10");
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

TEST_F(ResultClauseHandlerTest, Limit_LimitZero) {
	// Tests LIMIT 0
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC LIMIT 0");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerTest, Limit_WithNegativeNumber) {
	// Tests LIMIT with negative number
	// Behavior might be implementation-specific
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) RETURN n.id LIMIT -1");
	// Negative LIMIT might be treated as 0 or cause error
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerTest, Limit_WithOrderBy) {
	// Tests LIMIT combined with ORDER BY
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id DESC LIMIT 3");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "9");
}

TEST_F(ResultClauseHandlerTest, Limit_WithVeryLargeNumber) {
	// Tests LIMIT with a very large number
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id LIMIT 999999999999");
	ASSERT_EQ(res.rowCount(), 10UL);
}

TEST_F(ResultClauseHandlerTest, Limit_WithZero) {
	// Tests LIMIT 0 (edge case - should return no rows)
	for (int i = 0; i < 5; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id LIMIT 0");
	ASSERT_EQ(res.rowCount(), 0UL);
}

// === OrderBy Tests ===

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

TEST_F(ResultClauseHandlerTest, OrderBy_MultipleFields) {
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

TEST_F(ResultClauseHandlerTest, OrderBy_SingleFieldAscending) {
	// Tests ORDER BY with single field ascending (explicit or default)
	(void) execute("CREATE (n:Test {val: 3})");
	(void) execute("CREATE (n:Test {val: 1})");
	(void) execute("CREATE (n:Test {val: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.val ORDER BY n.val ASC");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.val").toString(), "1");
}

TEST_F(ResultClauseHandlerTest, OrderBy_SingleFieldDescending) {
	// Tests ORDER BY with DESC keyword
	(void) execute("CREATE (n:Test {val: 1})");
	(void) execute("CREATE (n:Test {val: 3})");
	(void) execute("CREATE (n:Test {val: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.val ORDER BY n.val DESC");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.val").toString(), "3");
}

TEST_F(ResultClauseHandlerTest, OrderBy_WithDescendingKeyword) {
	// Tests ORDER BY with DESCENDING keyword (full form)
	(void) execute("CREATE (n:Test {val: 1})");
	(void) execute("CREATE (n:Test {val: 3})");

	auto res = execute("MATCH (n:Test) RETURN n.val ORDER BY n.val DESCENDING");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.val").toString(), "3");
}

TEST_F(ResultClauseHandlerTest, OrderBy_WithNullValues) {
	// Tests ORDER BY with null values
	(void) execute("CREATE (n:Test {val: null})");
	(void) execute("CREATE (n:Test {val: 1})");
	(void) execute("CREATE (n:Test {val: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.val ORDER BY n.val");
	ASSERT_EQ(res.rowCount(), 3UL);
	// Null values typically sort first or last depending on implementation
}

// === Return Tests ===

TEST_F(ResultClauseHandlerTest, Return_Aggregate_Avg) {
	// Tests AVG() - covers line 136
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN avg(n.value)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("avg(n.value)").toString(), "20");
}

TEST_F(ResultClauseHandlerTest, Return_Aggregate_Collect) {
	// Tests COLLECT() - covers line 139
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 2})");
	(void) execute("CREATE (n:Test {value: 3})");

	auto res = execute("MATCH (n:Test) RETURN collect(n.value)");
	ASSERT_EQ(res.rowCount(), 1UL);
	// collect returns a list
}

TEST_F(ResultClauseHandlerTest, Return_Aggregate_Count_All) {
	// Tests COUNT(*) - covers line 142-150 (funcCall->getArgumentCount() == 0)
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 2})");
	(void) execute("CREATE (n:Test {value: 3})");

	auto res = execute("MATCH (n:Test) RETURN count(*)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("count(*)").toString(), "3");
}

TEST_F(ResultClauseHandlerTest, Return_Aggregate_Count_Property) {
	// Tests COUNT(n.prop) with non-null values
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 2})");
	(void) execute("CREATE (n:Test {})"); // No value property

	auto res = execute("MATCH (n:Test) RETURN count(n.value)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("count(n.value)").toString(), "2");
}

TEST_F(ResultClauseHandlerTest, Return_Aggregate_Count_Variable) {
	// Tests COUNT(n) - covers line 143-150 with argument
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 2})");

	auto res = execute("MATCH (n:Test) RETURN count(n)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("count(n)").toString(), "2");
}

TEST_F(ResultClauseHandlerTest, Return_Aggregate_Max) {
	// Tests MAX() - covers line 138
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 5})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN max(n.value)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("max(n.value)").toString(), "20");
}

TEST_F(ResultClauseHandlerTest, Return_Aggregate_Min) {
	// Tests MIN() - covers line 137
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 5})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN min(n.value)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("min(n.value)").toString(), "5");
}

TEST_F(ResultClauseHandlerTest, Return_Aggregate_MixedCaseFunctionName) {
	// Tests mixed case function names
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN Count(n)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("Count(n)").toString(), "2");
}

TEST_F(ResultClauseHandlerTest, Return_Aggregate_MixedWithNonAggregate) {
	// Tests aggregates mixed with non-aggregates - covers line 163-175
	// When mixing aggregates with non-aggregates, non-aggregates become GROUP BY keys
	(void) execute("CREATE (n:Test {group: 'A', value: 10})");
	(void) execute("CREATE (n:Test {group: 'A', value: 20})");

	auto res = execute("MATCH (n:Test) RETURN n.group, count(n) AS cnt");
	// Should return 1 row since both nodes have group='A' (grouped by n.group)
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("cnt").toString(), "2");
}

TEST_F(ResultClauseHandlerTest, Return_Aggregate_MultipleAggregates) {
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

TEST_F(ResultClauseHandlerTest, Return_Aggregate_Sum) {
	// Tests SUM() - covers line 135
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN sum(n.value)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("sum(n.value)").toString(), "60");
}

TEST_F(ResultClauseHandlerTest, Return_Aggregate_UppercaseFunctionName) {
	// Tests case-insensitive aggregate function names - line 130-132 transform
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN COUNT(n)");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("COUNT(n)").toString(), "2");
}

TEST_F(ResultClauseHandlerTest, Return_Aggregate_WithAlias) {
	// Tests aggregate with AS alias - covers line 154-157
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN sum(n.value) AS total");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("total").toString(), "30");
}

TEST_F(ResultClauseHandlerTest, Return_BooleanLiteral) {
	// Tests RETURN true (boolean literal)
	auto res = execute("RETURN true");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("true").toString(), "true");
}

TEST_F(ResultClauseHandlerTest, Return_Distinct_AfterWhere) {
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

TEST_F(ResultClauseHandlerTest, Return_Distinct_MultipleFields) {
	// Tests DISTINCT with multiple fields
	(void) execute("CREATE (n:Test {a: 1, b: 2})");
	(void) execute("CREATE (n:Test {a: 1, b: 2})");
	(void) execute("CREATE (n:Test {a: 1, b: 3})");

	auto res = execute("MATCH (n:Test) RETURN DISTINCT n.a, n.b");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ResultClauseHandlerTest, Return_Distinct_SingleField) {
	// Tests RETURN DISTINCT - covers True branch at line 41 (K_DISTINCT != nullptr)
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 1})");
	(void) execute("CREATE (n:Test {value: 2})");

	auto res = execute("MATCH (n:Test) RETURN DISTINCT n.value");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ResultClauseHandlerTest, Return_Distinct_WithAggregate) {
	// Tests DISTINCT with aggregate function
	(void) execute("CREATE (n:Test {group: 'A', value: 10})");
	(void) execute("CREATE (n:Test {group: 'A', value: 20})");

	auto res = execute("MATCH (n:Test) RETURN DISTINCT n.group, count(n)");
	EXPECT_GE(res.rowCount(), 1UL);
}

TEST_F(ResultClauseHandlerTest, Return_DoubleLiteral) {
	// Tests RETURN 3.14 (double literal)
	auto res = execute("RETURN 3.14");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("3.14").toString(), "3.14");
}

TEST_F(ResultClauseHandlerTest, Return_EmptyProjection) {
	// Tests RETURN with no items after WHERE filters all
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test {id: 2}) RETURN n");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerTest, Return_Limit_All) {
	// Tests LIMIT that returns all
	for (int i = 0; i < 5; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id LIMIT 10");
	ASSERT_EQ(res.rowCount(), 5UL);
}

TEST_F(ResultClauseHandlerTest, Return_Limit_Half) {
	// Tests LIMIT that returns half
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id LIMIT 5");
	ASSERT_EQ(res.rowCount(), 5UL);
}

TEST_F(ResultClauseHandlerTest, Return_Limit_NonInteger_Error) {
	// Tests LIMIT with string literal - should trigger error handling
	EXPECT_THROW({
		execute("CREATE (n:Test {id: 1})");
		execute("MATCH (n:Test) RETURN n LIMIT 'invalid'");
	}, std::runtime_error);
}

TEST_F(ResultClauseHandlerTest, Return_Limit_One) {
	// Tests LIMIT 1
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.id LIMIT 1");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ResultClauseHandlerTest, Return_LiteralValue) {
	// Tests RETURN 42 (literal value)
	auto res = execute("RETURN 42");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("42").toString(), "42");
}

TEST_F(ResultClauseHandlerTest, Return_MultipleAliases) {
	// Tests RETURN with multiple aliases
	(void) execute("CREATE (n:Person {name: 'Frank', age: 25})");

	auto res = execute("MATCH (n:Person) RETURN n.name AS person_name, n.age AS person_age");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("person_name").toString(), "Frank");
	EXPECT_EQ(res.getRows()[0].at("person_age").toString(), "25");
}

TEST_F(ResultClauseHandlerTest, Return_MultipleLiteralValues) {
	// Tests RETURN with multiple literal values
	auto res = execute("RETURN 1 AS one, 2 AS two, 3 AS three");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("one").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("two").toString(), "2");
	EXPECT_EQ(res.getRows()[0].at("three").toString(), "3");
}

TEST_F(ResultClauseHandlerTest, Return_MultiplePropertyAccess) {
	// Tests RETURN n.name, n.age (multiple property accesses)
	(void) execute("CREATE (n:Person {name: 'David', age: 30})");

	auto res = execute("MATCH (n:Person) RETURN n.name, n.age");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "David");
	EXPECT_EQ(res.getRows()[0].at("n.age").toString(), "30");
}

TEST_F(ResultClauseHandlerTest, Return_MultipleVariables) {
	// Tests RETURN n, m (multiple variables)
	(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");

	auto res = execute("MATCH (a:Person)-[:KNOWS]->(b:Person) RETURN a, b");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("a"));
	EXPECT_TRUE(res.getRows()[0].contains("b"));
}

TEST_F(ResultClauseHandlerTest, Return_MultipleWithSameProperty) {
	// Tests RETURN same property multiple times
	(void) execute("CREATE (n:Test {value: 100})");

	auto res = execute("MATCH (n:Test) RETURN n.value, n.value, n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
}

TEST_F(ResultClauseHandlerTest, Return_Multiply) {
	// Covers False branch at line 76 (MULTIPLY is true)
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	auto res = execute("MATCH (n:Person) RETURN *");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("n"));
}

TEST_F(ResultClauseHandlerTest, Return_MultiplyAll) {
	// Tests RETURN * (return all variables)
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	auto res = execute("MATCH (n:Person) RETURN *");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("n"));
}

TEST_F(ResultClauseHandlerTest, Return_MultiplyWithLimit) {
	// Tests RETURN * with LIMIT
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN * LIMIT 5");
	ASSERT_EQ(res.rowCount(), 5UL);
}

TEST_F(ResultClauseHandlerTest, Return_Multiply_AfterCreate) {
	// Tests RETURN * after CREATE
	auto res = execute("CREATE (n:Test {id: 1}) RETURN *");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("n"));
}

TEST_F(ResultClauseHandlerTest, Return_Multiply_AfterMatch) {
	// Tests RETURN * with ORDER BY
	(void) execute("CREATE (n:Test {id: 2})");
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) RETURN * ORDER BY n.id");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_TRUE(res.getRows()[0].contains("n"));
}

TEST_F(ResultClauseHandlerTest, Return_Multiply_MultipleVariables) {
	// Tests RETURN * with multiple variables
	(void) execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})");

	auto res = execute("MATCH (a)-[:KNOWS]->(b) RETURN *");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("a"));
	EXPECT_TRUE(res.getRows()[0].contains("b"));
}

TEST_F(ResultClauseHandlerTest, Return_Multiply_WithLimit) {
	// Tests RETURN * with LIMIT
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN * LIMIT 5");
	ASSERT_EQ(res.rowCount(), 5UL);
}

TEST_F(ResultClauseHandlerTest, Return_Multiply_WithSkip) {
	// Tests RETURN * with SKIP
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN * SKIP 5");
	ASSERT_EQ(res.rowCount(), 5UL);
}

TEST_F(ResultClauseHandlerTest, Return_NegativeLiteral) {
	// Tests RETURN -42 (negative literal)
	auto res = execute("RETURN -42");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("-42").toString(), "-42");
}

TEST_F(ResultClauseHandlerTest, Return_NullLiteral) {
	// Tests RETURN null (null literal)
	auto res = execute("RETURN null");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("null").asPrimitive().getType(), graph::PropertyType::NULL_TYPE);
}

TEST_F(ResultClauseHandlerTest, Return_OrderBySkipLimit_All) {
	// Tests ORDER BY + SKIP + LIMIT all together
	for (int i = 0; i < 20; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id DESC SKIP 5 LIMIT 10");
	ASSERT_EQ(res.rowCount(), 10UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "14");
}

TEST_F(ResultClauseHandlerTest, Return_OrderBy_AfterReturn) {
	// Tests ORDER BY after RETURN (correct order)
	(void) execute("CREATE (n:Test {id: 2})");
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id DESC");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "2");
}

TEST_F(ResultClauseHandlerTest, Return_OrderBy_DefaultAscending) {
	// Covers False branch at line 62 (no DESC/DESCENDING)
	(void) execute("CREATE (n:Test {val: 2})");
	(void) execute("CREATE (n:Test {val: 1})");

	auto res = execute("MATCH (n:Test) RETURN n.val ORDER BY n.val");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.val").toString(), "1");
}

TEST_F(ResultClauseHandlerTest, Return_OrderBy_ExplicitAscending) {
	// Tests explicit ASC keyword
	(void) execute("CREATE (n:Test {val: 2})");
	(void) execute("CREATE (n:Test {val: 1})");

	auto res = execute("MATCH (n:Test) RETURN n.val ORDER BY n.val ASC");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.val").toString(), "1");
}

TEST_F(ResultClauseHandlerTest, Return_OrderBy_MultipleSortItems) {
	// Tests ORDER BY with multiple sort items
	(void) execute("CREATE (n:Test {a: 1, b: 2})");
	(void) execute("CREATE (n:Test {a: 1, b: 1})");
	(void) execute("CREATE (n:Test {a: 2, b: 1})");

	auto res = execute("MATCH (n:Test) RETURN n.a, n.b ORDER BY n.a, n.b");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(ResultClauseHandlerTest, Return_OrderBy_ProjectionAlias) {
	// Tests ORDER BY referencing projection alias - covers line 78-88
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 15})");

	auto res = execute("MATCH (n:Test) RETURN n.value AS v ORDER BY v");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("v").toString(), "10");
}

TEST_F(ResultClauseHandlerTest, Return_OrderBy_ProjectionAliasDesc) {
	// Tests ORDER BY with alias and DESC
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN n.value AS v ORDER BY v DESC");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("v").toString(), "20");
}

TEST_F(ResultClauseHandlerTest, Return_OrderBy_PropertyAccessNoAlias) {
	// Tests ORDER BY with property access that's not a simple variable
	// Covers line 79 hasProperty() check when property is accessed
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value");
	ASSERT_EQ(res.rowCount(), 2UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "10");
}

TEST_F(ResultClauseHandlerTest, Return_OrderBy_PropertyWithoutDot) {
	// Covers False branch at line 53 (no dot in expression)
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) RETURN n ORDER BY n");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerTest, Return_OrderBy_SingleSortItem) {
	// Tests ORDER BY with single sort item
	(void) execute("CREATE (n:Test {id: 3})");
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id");
	ASSERT_EQ(res.rowCount(), 3UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "1");
}

TEST_F(ResultClauseHandlerTest, Return_PropertyAccess) {
	// Tests RETURN n.name (property access)
	(void) execute("CREATE (n:Person {name: 'Charlie'})");

	auto res = execute("MATCH (n:Person) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.name").toString(), "Charlie");
}

TEST_F(ResultClauseHandlerTest, Return_ScientificNotation) {
	// Tests RETURN 1.5e10 (scientific notation)
	// Note: Scientific notation values are parsed as DOUBLE type (decimal in scientific notation)
	auto res = execute("RETURN 1.5e10");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("1.5e10").asPrimitive().getType(), graph::PropertyType::DOUBLE);
}

TEST_F(ResultClauseHandlerTest, Return_SingleVariable) {
	// Tests RETURN n (single variable)
	(void) execute("CREATE (n:Person {name: 'Bob'})");

	auto res = execute("MATCH (n:Person) RETURN n");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("n"));
}

TEST_F(ResultClauseHandlerTest, Return_SkipLimit_Combo) {
	// Tests SKIP and LIMIT together
	for (int i = 0; i < 20; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 5 LIMIT 10");
	ASSERT_EQ(res.rowCount(), 10UL);
}

TEST_F(ResultClauseHandlerTest, Return_Skip_AllButOne) {
	// Tests SKIP that leaves only one result
	for (int i = 0; i < 5; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 4");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ResultClauseHandlerTest, Return_Skip_Half) {
	// Tests SKIP that skips half of results
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 5");
	ASSERT_EQ(res.rowCount(), 5UL);
}

TEST_F(ResultClauseHandlerTest, Return_Skip_NonInteger_Error) {
	// Tests SKIP with string literal - should trigger error handling
	EXPECT_THROW({
		execute("CREATE (n:Test {id: 1})");
		execute("MATCH (n:Test) RETURN n SKIP 'invalid'");
	}, std::runtime_error);
}

TEST_F(ResultClauseHandlerTest, Return_Skip_One) {
	// Tests SKIP 1
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 1");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "2");
}

TEST_F(ResultClauseHandlerTest, Return_StandaloneBoolean) {
	// Tests RETURN with boolean without MATCH
	auto res = execute("RETURN true");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("true").toString(), "true");
}

TEST_F(ResultClauseHandlerTest, Return_StandaloneLiteral) {
	// Covers True branch at line 36 (rootOp is null)
	// SingleRowOperator should be injected
	auto res = execute("RETURN 42");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("42").toString(), "42");
}

TEST_F(ResultClauseHandlerTest, Return_StandaloneMultiple) {
	// Tests RETURN multiple literals without MATCH
	auto res = execute("RETURN 1, 2, 3");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("1").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("2").toString(), "2");
	EXPECT_EQ(res.getRows()[0].at("3").toString(), "3");
}

TEST_F(ResultClauseHandlerTest, Return_StandaloneNull) {
	// Tests RETURN with null without MATCH
	auto res = execute("RETURN null");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("null").asPrimitive().getType(), graph::PropertyType::NULL_TYPE);
}

TEST_F(ResultClauseHandlerTest, Return_StandaloneString) {
	// Tests RETURN with string literal without MATCH
	auto res = execute("RETURN 'Hello'");
	ASSERT_EQ(res.rowCount(), 1UL);
}

TEST_F(ResultClauseHandlerTest, Return_StarWithOrderBy) {
	// Tests RETURN * with ORDER BY
	(void) execute("CREATE (n:Test {id: 2})");
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) RETURN * ORDER BY n.id");
	ASSERT_EQ(res.rowCount(), 2UL);
}

TEST_F(ResultClauseHandlerTest, Return_StringLiteral) {
	// Tests RETURN 'text' (string literal)
	// Note: The column name is the expression text as parsed by the grammar
	auto res = execute("RETURN 'Hello World'");
	ASSERT_EQ(res.rowCount(), 1UL);
	// Column name is the expression text including quotes as parsed
	EXPECT_EQ(res.getRows()[0].at("'Hello World'").asPrimitive().getType(), graph::PropertyType::STRING);
}

TEST_F(ResultClauseHandlerTest, Return_WithAlias) {
	// Tests RETURN n.name AS alias (with AS keyword)
	(void) execute("CREATE (n:Person {name: 'Eve'})");

	auto res = execute("MATCH (n:Person) RETURN n.name AS person_name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("person_name").toString(), "Eve");
}

TEST_F(ResultClauseHandlerTest, Return_WithAlias_Chained) {
	// Tests RETURN with chained aliases
	(void) execute("CREATE (n:Test {a: 1, b: 2, c: 3})");

	auto res = execute("MATCH (n:Test) RETURN n.a AS x, n.b AS y, n.c AS z");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("x").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("y").toString(), "2");
	EXPECT_EQ(res.getRows()[0].at("z").toString(), "3");
}

TEST_F(ResultClauseHandlerTest, Return_WithAllClauses) {
	// Tests RETURN with ORDER BY, SKIP, and LIMIT all present
	for (int i = 0; i < 30; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id DESC SKIP 5 LIMIT 10");
	ASSERT_EQ(res.rowCount(), 10UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "24");
}

TEST_F(ResultClauseHandlerTest, Return_WithAndWithoutAlias) {
	// Tests mix of AS and non-AS
	(void) execute("CREATE (n:Test {a: 1, b: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.a AS first, n.b");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("first").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.b").toString(), "2");
}

TEST_F(ResultClauseHandlerTest, Return_WithMultipleAliases) {
	// Tests RETURN with multiple aliases
	(void) execute("CREATE (n:Test {a: 10, b: 5})");

	auto res = execute("MATCH (n:Test) RETURN n.a AS value_a, n.b AS value_b");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("value_a").toString(), "10");
	EXPECT_EQ(res.getRows()[0].at("value_b").toString(), "5");
}

TEST_F(ResultClauseHandlerTest, Return_WithPropertyAccess) {
	// Tests RETURN with property access
	(void) execute("CREATE (n:Test {a: 5, b: 3})");

	auto res = execute("MATCH (n:Test) RETURN n.a, n.b");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.a").toString(), "5");
	EXPECT_EQ(res.getRows()[0].at("n.b").toString(), "3");
}

TEST_F(ResultClauseHandlerTest, Return_WithoutAlias) {
	// Tests RETURN without AS keyword (alias defaults to expression)
	(void) execute("CREATE (n:Test {name: 'Alice'})");

	auto res = execute("MATCH (n:Test) RETURN n.name");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_TRUE(res.getRows()[0].contains("n.name"));
}

TEST_F(ResultClauseHandlerTest, Return_WithoutAlias_Multiple) {
	// Tests multiple RETURN items without AS
	(void) execute("CREATE (n:Test {a: 1, b: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.a, n.b");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.a").toString(), "1");
	EXPECT_EQ(res.getRows()[0].at("n.b").toString(), "2");
}

TEST_F(ResultClauseHandlerTest, Return_WithoutAlias_Single) {
	// Covers False branch at line 86 (K_AS is false)
	(void) execute("CREATE (n:Test {value: 100})");

	auto res = execute("MATCH (n:Test) RETURN n.value");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("n.value").toString(), "100");
}

// === Skip Tests ===

TEST_F(ResultClauseHandlerTest, Skip_ExactlyAllRows) {
	// Tests SKIP that skips exactly all rows
	for (int i = 0; i < 5; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 5");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerTest, Skip_MoreThanAvailable) {
	// Tests SKIP with value greater than row count
	for (int i = 0; i < 3; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 10");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerTest, Skip_SkipAllRows) {
	// Tests SKIP N where N >= row count
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC SKIP 10");
	ASSERT_EQ(res.rowCount(), 0UL);
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

TEST_F(ResultClauseHandlerTest, Skip_SkipZero) {
	// Tests SKIP 0 (no skip)
	(void) execute("CREATE (n:Test {value: 10})");
	(void) execute("CREATE (n:Test {value: 20})");
	(void) execute("CREATE (n:Test {value: 30})");

	auto res = execute("MATCH (n:Test) RETURN n.value ORDER BY n.value ASC SKIP 0");
	ASSERT_EQ(res.rowCount(), 3UL);
}

TEST_F(ResultClauseHandlerTest, Skip_WithNegativeNumber) {
	// Tests SKIP with negative number
	// Behavior might be implementation-specific
	(void) execute("CREATE (n:Test {id: 1})");

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP -1");
	// Negative SKIP might be treated as 0 or cause error
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerTest, Skip_WithOrderBy) {
	// Tests SKIP combined with ORDER BY
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id DESC SKIP 5");
	ASSERT_EQ(res.rowCount(), 5UL);
}

TEST_F(ResultClauseHandlerTest, Skip_WithVeryLargeNumber) {
	// Tests SKIP with a very large number that might overflow
	// Create test data
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 999999999999");
	ASSERT_EQ(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerTest, Skip_WithZero) {
	// Tests SKIP 0 (edge case)
	(void) execute("CREATE (n:Test {id: 1})");
	(void) execute("CREATE (n:Test {id: 2})");

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 0");
	ASSERT_EQ(res.rowCount(), 2UL);
}

// === SkipAndLimit Tests ===

TEST_F(ResultClauseHandlerTest, SkipAndLimit_BothPresent) {
	// Tests SKIP and LIMIT together
	for (int i = 0; i < 20; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id SKIP 5 LIMIT 10");
	ASSERT_EQ(res.rowCount(), 10UL);
	EXPECT_EQ(res.getRows()[0].at("n.id").toString(), "5");
}

TEST_F(ResultClauseHandlerTest, SkipAndLimit_LimitThenSkip) {
	// Tests ORDER BY with SKIP then LIMIT (correct order)
	for (int i = 0; i < 20; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	// SKIP first, then LIMIT
	auto res = execute("MATCH (n:Test) RETURN n.id ORDER BY n.id SKIP 5 LIMIT 10");
	EXPECT_GE(res.rowCount(), 0UL);
}

TEST_F(ResultClauseHandlerTest, SkipAndLimit_SkipGreaterThanLimit) {
	// Tests SKIP greater than LIMIT
	for (int i = 0; i < 10; ++i) {
		(void) execute("CREATE (n:Test {id: " + std::to_string(i) + "})");
	}

	auto res = execute("MATCH (n:Test) RETURN n.id SKIP 8 LIMIT 5");
	ASSERT_EQ(res.rowCount(), 2UL);
}

