/**
 * @file test_IntegrationCypherCombinations.cpp
 * @brief Integration tests for Cypher clause interaction combinations.
 *
 * Covers: OPTIONAL MATCH + aggregation, UNWIND + aggregation, WITH DISTINCT,
 * XOR in WHERE, BETWEEN, multiple MATCH + aggregation, CASE + aggregation.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "QueryTestFixture.hpp"
#include <set>

class CypherCombinationsTest : public QueryTestFixture {
protected:
	std::string val(const graph::query::QueryResult &r, const std::string &col, size_t row = 0) {
		const auto &rows = r.getRows();
		if (row >= rows.size()) return "<no_row>";
		auto it = rows[row].find(col);
		if (it == rows[row].end()) return "<no_col>";
		return it->second.toString();
	}
};

// ============================================================================
// OPTIONAL MATCH + Aggregation
// ============================================================================

TEST_F(CypherCombinationsTest, OptionalMatch_CountStarIncludesNulls) {
	(void) execute("CREATE (a:OMA {name: 'Alice'})-[:KNOWS]->(b:OMA {name: 'Bob'})");
	(void) execute("CREATE (c:OMA {name: 'Charlie'})");

	// count(*) counts all rows including those with NULLs from OPTIONAL MATCH
	auto r = execute(
		"MATCH (p:OMA) OPTIONAL MATCH (p)-[:KNOWS]->(f:OMA) "
		"RETURN count(*) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	// Alice->Bob, Bob->null, Charlie->null = 3 rows
	EXPECT_EQ(val(r, "cnt"), "3");
}

TEST_F(CypherCombinationsTest, OptionalMatch_CountExprExcludesNulls) {
	(void) execute("CREATE (a:OMB {name: 'Alice'})-[:KNOWS]->(b:OMB {name: 'Bob'})");
	(void) execute("CREATE (c:OMB {name: 'Charlie'})");

	// count(f) only counts non-NULL values
	auto r = execute(
		"MATCH (p:OMB) OPTIONAL MATCH (p)-[:KNOWS]->(f:OMB) "
		"RETURN count(f) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "1");
}

TEST_F(CypherCombinationsTest, OptionalMatch_SumWithNulls) {
	(void) execute("CREATE (a:OMC {name: 'Alice'})-[:RATED {score: 5}]->(m:OMCMovie {title: 'X'})");
	(void) execute("CREATE (b:OMC {name: 'Bob'})");

	auto r = execute(
		"MATCH (p:OMC) OPTIONAL MATCH (p)-[r:RATED]->(m:OMCMovie) "
		"RETURN sum(r.score) AS total");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "total"), "5");
}

TEST_F(CypherCombinationsTest, OptionalMatch_CollectSkipsNulls) {
	(void) execute("CREATE (a:OMD {name: 'Alice'})-[:KNOWS]->(b:OMD {name: 'Bob'})");
	(void) execute("CREATE (c:OMD {name: 'Charlie'})");

	auto r = execute(
		"MATCH (p:OMD) OPTIONAL MATCH (p)-[:KNOWS]->(f:OMD) "
		"RETURN collect(f.name) AS friends");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto friends = val(r, "friends");
	EXPECT_NE(friends.find("Bob"), std::string::npos);
}

TEST_F(CypherCombinationsTest, OptionalMatch_GroupByWithNullGroups) {
	(void) execute("CREATE (a:OME {name: 'Alice', dept: 'Eng'})-[:KNOWS]->(b:OME {name: 'Bob', dept: 'Eng'})");
	(void) execute("CREATE (c:OME {name: 'Charlie', dept: 'Sales'})");

	auto r = execute(
		"MATCH (p:OME) OPTIONAL MATCH (p)-[:KNOWS]->(f:OME) "
		"RETURN p.dept AS dept, count(f) AS friendCount "
		"ORDER BY dept");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "dept", 0), "Eng");
	EXPECT_EQ(val(r, "dept", 1), "Sales");
	EXPECT_EQ(val(r, "friendCount", 1), "0");
}

TEST_F(CypherCombinationsTest, OptionalMatch_WithOrderByLimit) {
	(void) execute("CREATE (a:OMF {name: 'Alice'})-[:KNOWS]->(b:OMF {name: 'Bob'})");
	(void) execute("CREATE (a2:OMF {name: 'Alice'})-[:KNOWS]->(c:OMF {name: 'Charlie'})");
	(void) execute("CREATE (d:OMF {name: 'Dave'})");

	auto r = execute(
		"MATCH (p:OMF) OPTIONAL MATCH (p)-[:KNOWS]->(f:OMF) "
		"RETURN p.name AS person, count(f) AS friendCount "
		"ORDER BY friendCount DESC LIMIT 2");
	ASSERT_EQ(r.rowCount(), 2UL);
}

TEST_F(CypherCombinationsTest, OptionalMatch_PipedThroughWith) {
	(void) execute("CREATE (a:OMG {name: 'Alice'})-[:KNOWS]->(b:OMG {name: 'Bob'})");
	(void) execute("CREATE (c:OMG {name: 'Charlie'})");

	auto r = execute(
		"MATCH (p:OMG) OPTIONAL MATCH (p)-[:KNOWS]->(f:OMG) "
		"WITH p.name AS person, count(f) AS cnt "
		"WHERE cnt > 0 "
		"RETURN person");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "person"), "Alice");
}

TEST_F(CypherCombinationsTest, OptionalMatch_CountStarVsCountExpr) {
	(void) execute("CREATE (a:OMH {name: 'Alice'})-[:KNOWS]->(b:OMH {name: 'Bob'})");
	(void) execute("CREATE (c:OMH {name: 'Charlie'})");

	auto r = execute(
		"MATCH (p:OMH) OPTIONAL MATCH (p)-[:KNOWS]->(f:OMH) "
		"RETURN count(*) AS starCnt, count(f) AS exprCnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	// count(*) >= count(f) always
	auto starCnt = std::stoi(val(r, "starCnt"));
	auto exprCnt = std::stoi(val(r, "exprCnt"));
	EXPECT_GE(starCnt, exprCnt);
}

// ============================================================================
// UNWIND + Aggregation
// ============================================================================

TEST_F(CypherCombinationsTest, Unwind_CountAfterUnwind) {
	auto r = execute("UNWIND [1, 2, 3, 4, 5] AS x RETURN count(x) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "5");
}

TEST_F(CypherCombinationsTest, Unwind_SumAfterUnwind) {
	auto r = execute("UNWIND [10, 20, 30] AS x RETURN sum(x) AS total");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "total"), "60");
}

TEST_F(CypherCombinationsTest, Unwind_GroupByAfterUnwind) {
	auto r = execute(
		"UNWIND ['A', 'B', 'A', 'C', 'B', 'A'] AS x "
		"RETURN x, count(x) AS cnt ORDER BY cnt DESC");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "x", 0), "A");
	EXPECT_EQ(val(r, "cnt", 0), "3");
}

TEST_F(CypherCombinationsTest, Unwind_CollectAfterUnwind) {
	auto r = execute(
		"UNWIND [3, 1, 2] AS x "
		"RETURN collect(x) AS collected");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto collected = val(r, "collected");
	// collect preserves insertion order from UNWIND
	EXPECT_NE(collected.find("1"), std::string::npos);
	EXPECT_NE(collected.find("2"), std::string::npos);
	EXPECT_NE(collected.find("3"), std::string::npos);
}

TEST_F(CypherCombinationsTest, Unwind_OrderByOnAggregate) {
	auto r = execute(
		"UNWIND ['B', 'A', 'B', 'C'] AS x "
		"RETURN x AS label, count(x) AS cnt ORDER BY cnt DESC");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "label", 0), "B");
	EXPECT_EQ(val(r, "cnt", 0), "2");
}

TEST_F(CypherCombinationsTest, Unwind_NullList) {
	auto r = execute("UNWIND null AS x RETURN count(x) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "0");
}

TEST_F(CypherCombinationsTest, Unwind_NullElementsInList) {
	auto r = execute("UNWIND [1, null, 3] AS x RETURN count(x) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	// count(x) should skip nulls
	EXPECT_EQ(val(r, "cnt"), "2");
}

// ============================================================================
// WITH DISTINCT
// ============================================================================

TEST_F(CypherCombinationsTest, WithDistinct_MidPipelineDedup) {
	(void) execute("CREATE (a:WD1 {cat: 'A', val: 1})");
	(void) execute("CREATE (b:WD1 {cat: 'A', val: 2})");
	(void) execute("CREATE (c:WD1 {cat: 'B', val: 3})");

	auto r = execute(
		"MATCH (n:WD1) WITH DISTINCT n.cat AS cat "
		"RETURN cat ORDER BY cat");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "cat", 0), "A");
	EXPECT_EQ(val(r, "cat", 1), "B");
}

TEST_F(CypherCombinationsTest, WithDistinct_ThenOrderBy) {
	(void) execute("CREATE (a:WD2 {val: 3})");
	(void) execute("CREATE (b:WD2 {val: 1})");
	(void) execute("CREATE (c:WD2 {val: 3})");
	(void) execute("CREATE (d:WD2 {val: 2})");

	auto r = execute(
		"MATCH (n:WD2) WITH DISTINCT n.val AS v "
		"RETURN v ORDER BY v");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "v", 0), "1");
	EXPECT_EQ(val(r, "v", 1), "2");
	EXPECT_EQ(val(r, "v", 2), "3");
}

TEST_F(CypherCombinationsTest, WithDistinct_ThenAggregate) {
	(void) execute("CREATE (a:WD3 {cat: 'A'})");
	(void) execute("CREATE (b:WD3 {cat: 'A'})");
	(void) execute("CREATE (c:WD3 {cat: 'B'})");

	auto r = execute(
		"MATCH (n:WD3) WITH DISTINCT n.cat AS cat "
		"RETURN count(cat) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "2");
}

TEST_F(CypherCombinationsTest, WithDistinct_MultiColumn) {
	(void) execute("CREATE (a:WD4 {x: 1, y: 'A'})");
	(void) execute("CREATE (b:WD4 {x: 1, y: 'A'})");
	(void) execute("CREATE (c:WD4 {x: 1, y: 'B'})");
	(void) execute("CREATE (d:WD4 {x: 2, y: 'A'})");

	auto r = execute(
		"MATCH (n:WD4) WITH DISTINCT n.x AS x, n.y AS y "
		"RETURN x, y ORDER BY x, y");
	ASSERT_EQ(r.rowCount(), 3UL);
}

TEST_F(CypherCombinationsTest, WithDistinct_ChainedWith) {
	(void) execute("CREATE (a:WD5 {val: 1})");
	(void) execute("CREATE (b:WD5 {val: 1})");
	(void) execute("CREATE (c:WD5 {val: 2})");
	(void) execute("CREATE (d:WD5 {val: 2})");
	(void) execute("CREATE (e:WD5 {val: 3})");

	auto r = execute(
		"MATCH (n:WD5) WITH DISTINCT n.val AS v "
		"WITH v WHERE v > 1 "
		"RETURN v ORDER BY v");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "v", 0), "2");
	EXPECT_EQ(val(r, "v", 1), "3");
}

TEST_F(CypherCombinationsTest, WithDistinct_WhereAfterDistinct) {
	(void) execute("CREATE (a:WD6 {cat: 'A', val: 10})");
	(void) execute("CREATE (b:WD6 {cat: 'A', val: 20})");
	(void) execute("CREATE (c:WD6 {cat: 'B', val: 30})");
	(void) execute("CREATE (d:WD6 {cat: 'C', val: 40})");

	auto r = execute(
		"MATCH (n:WD6) WITH DISTINCT n.cat AS cat "
		"WHERE cat <> 'A' "
		"RETURN cat ORDER BY cat");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "cat", 0), "B");
	EXPECT_EQ(val(r, "cat", 1), "C");
}

// ============================================================================
// XOR in WHERE
// ============================================================================

TEST_F(CypherCombinationsTest, Xor_BasicFiltering) {
	(void) execute("CREATE (a:XOR1 {x: true, y: false})");
	(void) execute("CREATE (b:XOR1 {x: false, y: true})");
	(void) execute("CREATE (c:XOR1 {x: true, y: true})");
	(void) execute("CREATE (d:XOR1 {x: false, y: false})");

	// XOR: true when exactly one is true
	auto r = execute(
		"MATCH (n:XOR1) WHERE n.x XOR n.y "
		"RETURN n.x AS x ORDER BY n.x");
	ASSERT_EQ(r.rowCount(), 2UL);
}

TEST_F(CypherCombinationsTest, Xor_BothTrueYieldsFalse) {
	(void) execute("CREATE (a:XOR2 {active: true, premium: true, name: 'Alice'})");
	(void) execute("CREATE (b:XOR2 {active: true, premium: false, name: 'Bob'})");
	(void) execute("CREATE (c:XOR2 {active: false, premium: true, name: 'Charlie'})");

	auto r = execute(
		"MATCH (n:XOR2) WHERE n.active XOR n.premium "
		"RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Bob");
	EXPECT_EQ(val(r, "n.name", 1), "Charlie");
}

TEST_F(CypherCombinationsTest, Xor_NullPropagation) {
	// XOR with NULL should propagate NULL (not match in WHERE)
	auto r = execute("RETURN null XOR true AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "null");
}

// ============================================================================
// BETWEEN
// ============================================================================

TEST_F(CypherCombinationsTest, Between_BasicRange) {
	(void) execute("CREATE (a:BET1 {val: 1})");
	(void) execute("CREATE (b:BET1 {val: 5})");
	(void) execute("CREATE (c:BET1 {val: 10})");
	(void) execute("CREATE (d:BET1 {val: 15})");

	auto r = execute(
		"MATCH (n:BET1) WHERE n.val BETWEEN 5 AND 10 "
		"RETURN n.val ORDER BY n.val");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.val", 0), "5");
	EXPECT_EQ(val(r, "n.val", 1), "10");
}

TEST_F(CypherCombinationsTest, Between_InclusiveBoundaries) {
	(void) execute("CREATE (a:BET2 {val: 4})");
	(void) execute("CREATE (b:BET2 {val: 5})");
	(void) execute("CREATE (c:BET2 {val: 6})");

	// BETWEEN is inclusive on both ends
	auto r = execute(
		"MATCH (n:BET2) WHERE n.val BETWEEN 5 AND 5 "
		"RETURN n.val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.val"), "5");
}

TEST_F(CypherCombinationsTest, Between_WithOrderBy) {
	(void) execute("CREATE (a:BET3 {name: 'Alice', score: 75})");
	(void) execute("CREATE (b:BET3 {name: 'Bob', score: 85})");
	(void) execute("CREATE (c:BET3 {name: 'Charlie', score: 95})");
	(void) execute("CREATE (d:BET3 {name: 'Dave', score: 65})");

	auto r = execute(
		"MATCH (n:BET3) WHERE n.score BETWEEN 70 AND 90 "
		"RETURN n.name ORDER BY n.score DESC");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Bob");
	EXPECT_EQ(val(r, "n.name", 1), "Alice");
}

TEST_F(CypherCombinationsTest, Between_FloatValues) {
	(void) execute("CREATE (a:BET4 {val: 1.5})");
	(void) execute("CREATE (b:BET4 {val: 2.5})");
	(void) execute("CREATE (c:BET4 {val: 3.5})");

	auto r = execute(
		"MATCH (n:BET4) WHERE n.val BETWEEN 2.0 AND 3.0 "
		"RETURN n.val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.val"), "2.5");
}

// ============================================================================
// Multiple MATCH + Aggregation
// ============================================================================

TEST_F(CypherCombinationsTest, MultiMatch_TwoMatchesWithAggregate) {
	(void) execute("CREATE (a:MM1 {name: 'Alice', dept: 'Eng'})");
	(void) execute("CREATE (b:MM1 {name: 'Bob', dept: 'Eng'})");
	(void) execute("CREATE (c:MM1 {name: 'Charlie', dept: 'Sales'})");
	(void) execute("CREATE (p:MM1Project {name: 'P1', dept: 'Eng'})");
	(void) execute("CREATE (q:MM1Project {name: 'P2', dept: 'Eng'})");

	auto r = execute(
		"MATCH (n:MM1) WHERE n.dept = 'Eng' "
		"RETURN n.dept AS dept, count(n) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "2");
}

TEST_F(CypherCombinationsTest, MultiMatch_FilteredByRelationship) {
	(void) execute("CREATE (a:MM2 {name: 'Alice'})-[:WORKS_ON]->(p:MM2Proj {name: 'P1'})");
	(void) execute("CREATE (b:MM2 {name: 'Bob'})-[:WORKS_ON]->(p2:MM2Proj {name: 'P1'})");
	(void) execute("CREATE (c:MM2 {name: 'Charlie'})-[:WORKS_ON]->(q:MM2Proj {name: 'P2'})");

	auto r = execute(
		"MATCH (n:MM2)-[:WORKS_ON]->(p:MM2Proj) "
		"RETURN p.name AS project, count(n) AS workers "
		"ORDER BY workers DESC");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "project", 0), "P1");
	EXPECT_EQ(val(r, "workers", 0), "2");
}

TEST_F(CypherCombinationsTest, MultiMatch_OrderByAggregateAlias) {
	(void) execute("CREATE (a:MM3 {cat: 'A', val: 10})");
	(void) execute("CREATE (b:MM3 {cat: 'A', val: 20})");
	(void) execute("CREATE (c:MM3 {cat: 'B', val: 30})");
	(void) execute("CREATE (d:MM3 {cat: 'B', val: 40})");
	(void) execute("CREATE (e:MM3 {cat: 'B', val: 50})");

	auto r = execute(
		"MATCH (n:MM3) "
		"RETURN n.cat AS cat, sum(n.val) AS total "
		"ORDER BY total DESC");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "cat", 0), "B");
	EXPECT_EQ(val(r, "total", 0), "120");
	EXPECT_EQ(val(r, "cat", 1), "A");
	EXPECT_EQ(val(r, "total", 1), "30");
}

TEST_F(CypherCombinationsTest, MultiMatch_AggregateWithLimit) {
	(void) execute("CREATE (a:MM4 {cat: 'A'})");
	(void) execute("CREATE (b:MM4 {cat: 'A'})");
	(void) execute("CREATE (c:MM4 {cat: 'B'})");
	(void) execute("CREATE (d:MM4 {cat: 'C'})");
	(void) execute("CREATE (e:MM4 {cat: 'C'})");
	(void) execute("CREATE (f:MM4 {cat: 'C'})");

	auto r = execute(
		"MATCH (n:MM4) "
		"RETURN n.cat AS cat, count(n) AS cnt "
		"ORDER BY cnt DESC LIMIT 1");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cat"), "C");
	EXPECT_EQ(val(r, "cnt"), "3");
}

// ============================================================================
// CASE + Aggregation Interactions
// ============================================================================

TEST_F(CypherCombinationsTest, Case_SumWithCaseWhen) {
	(void) execute("CREATE (a:CA1 {name: 'Alice', status: 'active', val: 10})");
	(void) execute("CREATE (b:CA1 {name: 'Bob', status: 'inactive', val: 20})");
	(void) execute("CREATE (c:CA1 {name: 'Charlie', status: 'active', val: 30})");

	auto r = execute(
		"MATCH (n:CA1) "
		"RETURN sum(CASE WHEN n.status = 'active' THEN n.val ELSE 0 END) AS activeTotal");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "activeTotal"), "40");
}

TEST_F(CypherCombinationsTest, Case_CaseAroundAggregate) {
	(void) execute("CREATE (a:CA2 {val: 10})");
	(void) execute("CREATE (b:CA2 {val: 20})");
	(void) execute("CREATE (c:CA2 {val: 30})");

	auto r = execute(
		"MATCH (n:CA2) "
		"RETURN CASE WHEN count(n) > 2 THEN 'many' ELSE 'few' END AS label");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "label"), "many");
}

TEST_F(CypherCombinationsTest, Case_CountDistinctCase) {
	(void) execute("CREATE (a:CA3 {val: 10})");
	(void) execute("CREATE (b:CA3 {val: 20})");
	(void) execute("CREATE (c:CA3 {val: 30})");
	(void) execute("CREATE (d:CA3 {val: 5})");

	auto r = execute(
		"MATCH (n:CA3) "
		"RETURN count(DISTINCT CASE WHEN n.val >= 10 THEN 'high' ELSE 'low' END) AS categories");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "categories"), "2");
}

TEST_F(CypherCombinationsTest, Case_ArithmeticOnAggregates) {
	(void) execute("CREATE (a:CA4 {val: 10})");
	(void) execute("CREATE (b:CA4 {val: 20})");
	(void) execute("CREATE (c:CA4 {val: 30})");

	// sum/count should equal avg
	auto r = execute(
		"MATCH (n:CA4) "
		"RETURN sum(n.val) AS total, count(n) AS cnt, avg(n.val) AS average");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "total"), "60");
	EXPECT_EQ(val(r, "cnt"), "3");
	EXPECT_EQ(val(r, "average"), "20");
}
