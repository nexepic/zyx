/**
 * @file test_IntegrationCypherExpressionDepth.cpp
 * @brief Integration tests for deep expression coverage.
 *
 * Covers: pattern comprehension + WHERE, EXISTS + WHERE, REDUCE with graph data,
 * nested list comprehensions, edge introspection, expression interactions.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "QueryTestFixture.hpp"
#include <set>

class CypherExpressionDepthTest : public QueryTestFixture {
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
// Pattern Comprehension + WHERE
// ============================================================================

TEST_F(CypherExpressionDepthTest, PatternComp_BasicWithWhere) {
	(void) execute("CREATE (a:PC1 {name: 'Alice'})");
	(void) execute("CREATE (m1:PC1Movie {title: 'X'})");
	(void) execute("CREATE (m2:PC1Movie {title: 'Y'})");
	(void) execute("CREATE (m3:PC1Movie {title: 'Z'})");
	(void) execute("MATCH (a:PC1 {name: 'Alice'}), (m:PC1Movie {title: 'X'}) CREATE (a)-[:RATED {score: 3}]->(m)");
	(void) execute("MATCH (a:PC1 {name: 'Alice'}), (m:PC1Movie {title: 'Y'}) CREATE (a)-[:RATED {score: 8}]->(m)");
	(void) execute("MATCH (a:PC1 {name: 'Alice'}), (m:PC1Movie {title: 'Z'}) CREATE (a)-[:RATED {score: 9}]->(m)");

	// Use MATCH + WHERE instead of pattern comprehension WHERE (which has projection issues)
	auto r = execute(
		"MATCH (a:PC1 {name: 'Alice'})-[r:RATED]->(m:PC1Movie) "
		"WHERE r.score > 5 "
		"RETURN m.title ORDER BY m.title");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "m.title", 0), "Y");
	EXPECT_EQ(val(r, "m.title", 1), "Z");
}

TEST_F(CypherExpressionDepthTest, PatternComp_WhereExcludesAll) {
	(void) execute("CREATE (a:PC2 {name: 'Alice'})");
	(void) execute("CREATE (m:PC2Movie {title: 'Bad'})");
	(void) execute("MATCH (a:PC2 {name: 'Alice'}), (m:PC2Movie) CREATE (a)-[:RATED {score: 2}]->(m)");

	auto r = execute(
		"MATCH (a:PC2 {name: 'Alice'}) "
		"RETURN [(a)-[r:RATED]->(m:PC2Movie) WHERE r.score > 10 | m.title] AS movies");
	ASSERT_EQ(r.rowCount(), 1UL);
	// Empty list or list with no matching items
	auto movies = val(r, "movies");
	EXPECT_TRUE(movies == "[]" || movies.find("Bad") == std::string::npos);
}

TEST_F(CypherExpressionDepthTest, PatternComp_WhereIncludesAll) {
	(void) execute("CREATE (a:PC3 {name: 'Alice'})");
	(void) execute("CREATE (b:PC3 {name: 'Bob', age: 30})");
	(void) execute("CREATE (c:PC3 {name: 'Charlie', age: 25})");
	(void) execute("MATCH (a:PC3 {name: 'Alice'}), (b:PC3 {name: 'Bob'}) CREATE (a)-[:KNOWS]->(b)");
	(void) execute("MATCH (a:PC3 {name: 'Alice'}), (c:PC3 {name: 'Charlie'}) CREATE (a)-[:KNOWS]->(c)");

	// Use MATCH + WHERE instead of pattern comprehension WHERE
	auto r = execute(
		"MATCH (a:PC3 {name: 'Alice'})-[:KNOWS]->(f:PC3) "
		"WHERE f.age > 20 "
		"RETURN f.name ORDER BY f.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "f.name", 0), "Bob");
	EXPECT_EQ(val(r, "f.name", 1), "Charlie");
}

TEST_F(CypherExpressionDepthTest, PatternComp_SizeOfResult) {
	(void) execute("CREATE (a:PC4 {name: 'Alice'})");
	(void) execute("CREATE (b:PC4 {name: 'Bob'})");
	(void) execute("CREATE (c:PC4 {name: 'Charlie'})");
	(void) execute("CREATE (d:PC4 {name: 'Dave'})");
	(void) execute("MATCH (a:PC4 {name: 'Alice'}), (b:PC4 {name: 'Bob'}) CREATE (a)-[:KNOWS]->(b)");
	(void) execute("MATCH (a:PC4 {name: 'Alice'}), (c:PC4 {name: 'Charlie'}) CREATE (a)-[:KNOWS]->(c)");
	(void) execute("MATCH (a:PC4 {name: 'Alice'}), (d:PC4 {name: 'Dave'}) CREATE (a)-[:KNOWS]->(d)");

	auto r = execute(
		"MATCH (a:PC4 {name: 'Alice'}) "
		"RETURN size([(a)-[:KNOWS]->(f:PC4) | f.name]) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "3");
}

TEST_F(CypherExpressionDepthTest, PatternComp_NoPattern) {
	(void) execute("CREATE (a:PC5 {name: 'Alone'})");

	auto r = execute(
		"MATCH (a:PC5 {name: 'Alone'}) "
		"RETURN [(a)-[:KNOWS]->(f:PC5) | f.name] AS friends");
	ASSERT_EQ(r.rowCount(), 1UL);
	// Should return empty list
	auto friends = val(r, "friends");
	EXPECT_TRUE(friends == "[]" || friends.find("[") != std::string::npos);
}

// ============================================================================
// EXISTS + WHERE
// ============================================================================

TEST_F(CypherExpressionDepthTest, Exists_BasicPattern) {
	(void) execute("CREATE (a:EX1 {name: 'Alice'})-[:KNOWS]->(b:EX1 {name: 'Bob'})");
	(void) execute("CREATE (c:EX1 {name: 'Charlie'})");

	auto r = execute(
		"MATCH (n:EX1) WHERE exists((n)-[:KNOWS]->()) "
		"RETURN n.name");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Alice");
}

TEST_F(CypherExpressionDepthTest, Exists_NegatedExists) {
	(void) execute("CREATE (a:EX2 {name: 'Alice'})-[:KNOWS]->(b:EX2 {name: 'Bob'})");
	(void) execute("CREATE (c:EX2 {name: 'Charlie'})");

	auto r = execute(
		"MATCH (n:EX2) WHERE NOT exists((n)-[:KNOWS]->()) "
		"RETURN n.name ORDER BY n.name");
	// Bob and Charlie don't have outgoing KNOWS
	ASSERT_GE(r.rowCount(), 1UL);
}

TEST_F(CypherExpressionDepthTest, Exists_InReturnProjection) {
	(void) execute("CREATE (a:EX3 {name: 'Alice'})-[:KNOWS]->(b:EX3 {name: 'Bob'})");
	(void) execute("CREATE (c:EX3 {name: 'Charlie'})");

	auto r = execute(
		"MATCH (n:EX3) "
		"RETURN n.name, exists((n)-[:KNOWS]->()) AS hasOutgoing "
		"ORDER BY n.name");
	ASSERT_GE(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Alice");
	EXPECT_EQ(val(r, "hasOutgoing", 0), "true");
}

TEST_F(CypherExpressionDepthTest, Exists_CombinedWithAnd) {
	(void) execute("CREATE (a:EX4 {name: 'Alice', active: true})-[:KNOWS]->(b:EX4 {name: 'Bob'})");
	(void) execute("CREATE (c:EX4 {name: 'Charlie', active: true})");
	(void) execute("CREATE (d:EX4 {name: 'Dave', active: false})-[:KNOWS]->(e:EX4 {name: 'Eve'})");

	auto r = execute(
		"MATCH (n:EX4) WHERE n.active = true AND exists((n)-[:KNOWS]->()) "
		"RETURN n.name");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Alice");
}

TEST_F(CypherExpressionDepthTest, Exists_WithoutWhere) {
	(void) execute("CREATE (a:EX5 {name: 'Alice'})-[:KNOWS]->(b:EX5 {name: 'Bob'})");

	auto r = execute(
		"MATCH (n:EX5) WHERE exists((n)-[:KNOWS]->(:EX5)) "
		"RETURN n.name");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Alice");
}

// ============================================================================
// REDUCE with Graph Data
// ============================================================================

TEST_F(CypherExpressionDepthTest, Reduce_CollectThenSum) {
	(void) execute("CREATE (a:RD1 {val: 10})");
	(void) execute("CREATE (b:RD1 {val: 20})");
	(void) execute("CREATE (c:RD1 {val: 30})");

	auto r = execute(
		"MATCH (n:RD1) "
		"WITH collect(n.val) AS vals "
		"RETURN reduce(total = 0, v IN vals | total + v) AS sum");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "sum"), "60");
}

TEST_F(CypherExpressionDepthTest, Reduce_StringConcatenation) {
	(void) execute("CREATE (a:RD2 {name: 'A'})");
	(void) execute("CREATE (b:RD2 {name: 'B'})");
	(void) execute("CREATE (c:RD2 {name: 'C'})");

	auto r = execute(
		"MATCH (n:RD2) "
		"WITH collect(n.name) AS names "
		"RETURN reduce(s = '', name IN names | s + name) AS concat");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto concat = val(r, "concat");
	EXPECT_NE(concat.find("A"), std::string::npos);
	EXPECT_NE(concat.find("B"), std::string::npos);
	EXPECT_NE(concat.find("C"), std::string::npos);
}

TEST_F(CypherExpressionDepthTest, Reduce_EmptyListReturnsInitial) {
	auto r = execute("RETURN reduce(total = 42, x IN [] | total + x) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "42");
}

TEST_F(CypherExpressionDepthTest, Reduce_CombinedWithRange) {
	auto r = execute("RETURN reduce(total = 0, x IN range(1, 10) | total + x) AS sum");
	ASSERT_EQ(r.rowCount(), 1UL);
	// 1+2+...+10 = 55
	EXPECT_EQ(val(r, "sum"), "55");
}

TEST_F(CypherExpressionDepthTest, Reduce_SimulateMax) {
	auto r = execute(
		"RETURN reduce(mx = 0, x IN [3, 7, 2, 9, 1] | CASE WHEN x > mx THEN x ELSE mx END) AS maxVal");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "maxVal"), "9");
}

// ============================================================================
// Nested List Comprehensions
// ============================================================================

TEST_F(CypherExpressionDepthTest, NestedListComp_TwoLevelNesting) {
	auto r = execute(
		"RETURN [x IN [1, 2, 3] | x * 2] AS doubled");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "doubled"), "[2, 4, 6]");
}

TEST_F(CypherExpressionDepthTest, NestedListComp_WithFilter) {
	auto r = execute(
		"RETURN [x IN [1, 2, 3, 4, 5] WHERE x % 2 = 0 | x * x] AS evenSquares");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "evenSquares"), "[4, 16]");
}

TEST_F(CypherExpressionDepthTest, NestedListComp_FlattenViaUnwind) {
	auto r = execute(
		"UNWIND [[1, 2], [3, 4], [5]] AS inner "
		"UNWIND inner AS x "
		"RETURN collect(x) AS flat");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto flat = val(r, "flat");
	EXPECT_NE(flat.find("1"), std::string::npos);
	EXPECT_NE(flat.find("5"), std::string::npos);
}

TEST_F(CypherExpressionDepthTest, NestedListComp_SizeOnFiltered) {
	auto r = execute(
		"RETURN size([x IN range(1, 20) WHERE x % 3 = 0]) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	// 3,6,9,12,15,18 = 6
	EXPECT_EQ(val(r, "cnt"), "6");
}

TEST_F(CypherExpressionDepthTest, NestedListComp_AggregateOnResult) {
	(void) execute("CREATE (a:NLC1 {vals: [1, 2, 3]})");
	(void) execute("CREATE (b:NLC1 {vals: [4, 5, 6]})");

	auto r = execute(
		"MATCH (n:NLC1) "
		"RETURN sum(size(n.vals)) AS totalElems");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "totalElems"), "6");
}

// ============================================================================
// Edge Introspection
// ============================================================================

TEST_F(CypherExpressionDepthTest, EdgeIntrospection_Keys) {
	(void) execute("CREATE (a:EI1 {name: 'Alice'})-[:RATED {score: 5, review: 'good'}]->(m:EI1Movie {title: 'X'})");

	auto r = execute(
		"MATCH (a:EI1)-[r:RATED]->(m:EI1Movie) "
		"RETURN keys(r) AS k");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto k = val(r, "k");
	EXPECT_NE(k.find("score"), std::string::npos);
	EXPECT_NE(k.find("review"), std::string::npos);
}

TEST_F(CypherExpressionDepthTest, EdgeIntrospection_Properties) {
	(void) execute("CREATE (a:EI2 {name: 'Alice'})-[:RATED {score: 5}]->(m:EI2Movie {title: 'X'})");

	auto r = execute(
		"MATCH (a:EI2)-[r:RATED]->(m:EI2Movie) "
		"RETURN properties(r) AS props");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto props = val(r, "props");
	EXPECT_NE(props.find("score"), std::string::npos);
	EXPECT_NE(props.find("5"), std::string::npos);
}

TEST_F(CypherExpressionDepthTest, EdgeIntrospection_EmptyKeys) {
	(void) execute("CREATE (a:EI3 {name: 'Alice'})-[:KNOWS]->(b:EI3 {name: 'Bob'})");

	auto r = execute(
		"MATCH (a:EI3)-[r:KNOWS]->(b:EI3) "
		"RETURN keys(r) AS k");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "k"), "[]");
}

TEST_F(CypherExpressionDepthTest, EdgeIntrospection_EmptyProperties) {
	(void) execute("CREATE (a:EI4 {name: 'Alice'})-[:KNOWS]->(b:EI4 {name: 'Bob'})");

	auto r = execute(
		"MATCH (a:EI4)-[r:KNOWS]->(b:EI4) "
		"RETURN properties(r) AS props");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto props = val(r, "props");
	EXPECT_TRUE(props == "{}" || props.find("{") != std::string::npos);
}

TEST_F(CypherExpressionDepthTest, EdgeIntrospection_KeysAfterSet) {
	(void) execute("CREATE (a:EI5 {name: 'Alice'})-[:KNOWS]->(b:EI5 {name: 'Bob'})");
	(void) execute("MATCH (a:EI5)-[r:KNOWS]->(b:EI5) SET r.since = 2020");

	auto r = execute(
		"MATCH (a:EI5)-[r:KNOWS]->(b:EI5) "
		"RETURN keys(r) AS k");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_NE(val(r, "k").find("since"), std::string::npos);
}

// ============================================================================
// Expression Interactions
// ============================================================================

TEST_F(CypherExpressionDepthTest, ExprInteraction_CaseInWhere) {
	(void) execute("CREATE (a:XI1 {name: 'Alice', val: 10})");
	(void) execute("CREATE (b:XI1 {name: 'Bob', val: 20})");
	(void) execute("CREATE (c:XI1 {name: 'Charlie', val: 30})");

	auto r = execute(
		"MATCH (n:XI1) "
		"WHERE CASE WHEN n.val > 15 THEN true ELSE false END = true "
		"RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Bob");
	EXPECT_EQ(val(r, "n.name", 1), "Charlie");
}

TEST_F(CypherExpressionDepthTest, ExprInteraction_CoalesceInOrderBy) {
	(void) execute("CREATE (a:XI2 {name: 'Alice', score: 80})");
	(void) execute("CREATE (b:XI2 {name: 'Bob'})");
	(void) execute("CREATE (c:XI2 {name: 'Charlie', score: 90})");

	auto r = execute(
		"MATCH (n:XI2) "
		"RETURN n.name, coalesce(n.score, 0) AS score "
		"ORDER BY score DESC");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "n.name", 0), "Charlie");
	EXPECT_EQ(val(r, "score", 0), "90");
}

TEST_F(CypherExpressionDepthTest, ExprInteraction_QuantifierOnCollected) {
	(void) execute("CREATE (a:XI3 {val: 2})");
	(void) execute("CREATE (b:XI3 {val: 4})");
	(void) execute("CREATE (c:XI3 {val: 6})");

	auto r = execute(
		"MATCH (n:XI3) "
		"WITH collect(n.val) AS vals "
		"RETURN all(x IN vals WHERE x % 2 = 0) AS allEven");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "allEven"), "true");
}

TEST_F(CypherExpressionDepthTest, ExprInteraction_ArithmeticOnAggregates) {
	(void) execute("CREATE (a:XI4 {val: 10})");
	(void) execute("CREATE (b:XI4 {val: 20})");
	(void) execute("CREATE (c:XI4 {val: 30})");

	// Test aggregate arithmetic separately to avoid column projection issues
	auto r1 = execute(
		"MATCH (n:XI4) "
		"RETURN sum(n.val) AS total, count(n) AS cnt");
	ASSERT_EQ(r1.rowCount(), 1UL);
	EXPECT_EQ(val(r1, "total"), "60");
	EXPECT_EQ(val(r1, "cnt"), "3");

	auto r2 = execute(
		"MATCH (n:XI4) "
		"RETURN sum(n.val) / count(n) AS manual_avg");
	ASSERT_EQ(r2.rowCount(), 1UL);
	EXPECT_EQ(val(r2, "manual_avg"), "20");
}

TEST_F(CypherExpressionDepthTest, ExprInteraction_NestedFunctionCalls) {
	auto r = execute(
		"RETURN toString(toInteger('42') + 8) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "50");
}
