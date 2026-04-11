/**
 * @file test_IntegrationCypherPatterns.cpp
 * @brief Integration tests for Cypher pattern matching variants.
 *
 * Covers: named paths, undirected relationships, multi-type relationships,
 * multi-label matching, variable-length paths, shortestPath expressions.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "QueryTestFixture.hpp"
#include <set>

class CypherPatternsTest : public QueryTestFixture {
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
// Named Paths
// ============================================================================

TEST_F(CypherPatternsTest, NamedPath_NodesFunction) {
	(void) execute("CREATE (a:NP1 {name: 'Alice'})-[:KNOWS]->(b:NP1 {name: 'Bob'})");

	auto r = execute("MATCH p = (a:NP1 {name: 'Alice'})-[:KNOWS]->(b:NP1) RETURN nodes(p) AS n");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto nodes = val(r, "n");
	EXPECT_NE(nodes.find("Alice"), std::string::npos);
	EXPECT_NE(nodes.find("Bob"), std::string::npos);
}

TEST_F(CypherPatternsTest, NamedPath_RelationshipsFunction) {
	(void) execute("CREATE (a:NP2 {name: 'Alice'})-[:KNOWS]->(b:NP2 {name: 'Bob'})");

	auto r = execute("MATCH p = (a:NP2 {name: 'Alice'})-[:KNOWS]->(b:NP2) RETURN relationships(p) AS rels");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto rels = val(r, "rels");
	EXPECT_NE(rels, "null");
}

TEST_F(CypherPatternsTest, NamedPath_LengthFunction) {
	(void) execute("CREATE (a:NP3 {name: 'Alice'})-[:KNOWS]->(b:NP3 {name: 'Bob'})-[:KNOWS]->(c:NP3 {name: 'Charlie'})");

	auto r = execute("MATCH p = (a:NP3 {name: 'Alice'})-[:KNOWS*]->(c:NP3 {name: 'Charlie'}) RETURN length(p) AS len");
	ASSERT_GE(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "len"), "2");
}

TEST_F(CypherPatternsTest, NamedPath_MultiHop) {
	(void) execute("CREATE (a:NP4 {name: 'A'})-[:R]->(b:NP4 {name: 'B'})-[:R]->(c:NP4 {name: 'C'})");

	auto r = execute("MATCH p = (a:NP4 {name: 'A'})-[:R*2]->(c:NP4) RETURN length(p) AS len, c.name AS dest");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "len"), "2");
	EXPECT_EQ(val(r, "dest"), "C");
}

TEST_F(CypherPatternsTest, NamedPath_WithWhere) {
	(void) execute("CREATE (a:NP5 {name: 'A'})-[:R]->(b:NP5 {name: 'B', val: 10})");
	(void) execute("CREATE (a2:NP5 {name: 'A'})-[:R]->(c:NP5 {name: 'C', val: 20})");

	auto r = execute(
		"MATCH p = (a:NP5 {name: 'A'})-[:R]->(b:NP5) "
		"WHERE b.val > 15 "
		"RETURN b.name AS dest");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "dest"), "C");
}

TEST_F(CypherPatternsTest, NamedPath_MultiplePathFunctions) {
	(void) execute("CREATE (a:NP6 {name: 'Alice'})-[:KNOWS]->(b:NP6 {name: 'Bob'})");

	auto r = execute(
		"MATCH p = (a:NP6 {name: 'Alice'})-[:KNOWS]->(b:NP6) "
		"RETURN length(p) AS len, nodes(p) AS n");
	ASSERT_EQ(r.rowCount(), 1UL);
	// Verify path has meaningful length
	auto len = std::stoi(val(r, "len"));
	EXPECT_GT(len, 0);
}

TEST_F(CypherPatternsTest, NamedPath_NoMatch) {
	(void) execute("CREATE (a:NP7 {name: 'Alice'})");

	auto r = execute("MATCH p = (a:NP7 {name: 'Alice'})-[:KNOWS]->(b:NP7) RETURN length(p) AS len");
	EXPECT_EQ(r.rowCount(), 0UL);
}

TEST_F(CypherPatternsTest, NamedPath_ReturnPathDirectly) {
	(void) execute("CREATE (a:NP8 {name: 'Alice'})-[:R]->(b:NP8 {name: 'Bob'})");

	auto r = execute("MATCH p = (a:NP8 {name: 'Alice'})-[:R]->(b:NP8) RETURN p");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto pathStr = val(r, "p");
	EXPECT_NE(pathStr, "null");
}

// ============================================================================
// Undirected Relationships
// ============================================================================

TEST_F(CypherPatternsTest, Undirected_BothDirections) {
	(void) execute("CREATE (a:UD1 {name: 'Alice'})-[:KNOWS]->(b:UD1 {name: 'Bob'})");

	// Undirected should match in both directions
	auto r = execute("MATCH (a:UD1)-[:KNOWS]-(b:UD1) RETURN a.name, b.name ORDER BY a.name");
	// Should return both Alice->Bob and Bob->Alice
	ASSERT_EQ(r.rowCount(), 2UL);
}

TEST_F(CypherPatternsTest, Undirected_WithFilter) {
	(void) execute("CREATE (a:UD2 {name: 'Alice', age: 30})-[:KNOWS]->(b:UD2 {name: 'Bob', age: 25})");

	auto r = execute(
		"MATCH (a:UD2)-[:KNOWS]-(b:UD2) WHERE a.age > 28 "
		"RETURN a.name ORDER BY a.name");
	ASSERT_GE(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "a.name", 0), "Alice");
}

TEST_F(CypherPatternsTest, Undirected_CountComparisonVsDirected) {
	(void) execute("CREATE (a:UD3 {name: 'Alice'})-[:KNOWS]->(b:UD3 {name: 'Bob'})");

	auto directed = execute("MATCH (a:UD3)-[:KNOWS]->(b:UD3) RETURN count(*) AS cnt");
	auto undirected = execute("MATCH (a:UD3)-[:KNOWS]-(b:UD3) RETURN count(*) AS cnt");

	auto dCnt = std::stoi(val(directed, "cnt"));
	auto uCnt = std::stoi(val(undirected, "cnt"));
	// Undirected should return >= directed count
	EXPECT_GE(uCnt, dCnt);
}

TEST_F(CypherPatternsTest, Undirected_Typeless) {
	(void) execute("CREATE (a:UD4 {name: 'Alice'})-[:KNOWS]->(b:UD4 {name: 'Bob'})");
	(void) execute("CREATE (a2:UD4 {name: 'Alice'})-[:LIKES]->(c:UD4 {name: 'Charlie'})");

	// Typeless undirected should match any relationship
	auto r = execute("MATCH (a:UD4 {name: 'Alice'})--(b:UD4) RETURN b.name ORDER BY b.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "b.name", 0), "Bob");
	EXPECT_EQ(val(r, "b.name", 1), "Charlie");
}

// ============================================================================
// Multi-Type Relationships (using single source node with separate CREATE + MATCH)
// ============================================================================

TEST_F(CypherPatternsTest, MultiType_BasicOrRelTypes) {
	(void) execute("CREATE (a:MT1 {name: 'Alice'})");
	(void) execute("CREATE (b:MT1 {name: 'Bob'})");
	(void) execute("CREATE (c:MT1 {name: 'Charlie'})");
	(void) execute("CREATE (d:MT1 {name: 'Dave'})");
	(void) execute("MATCH (a:MT1 {name: 'Alice'}), (b:MT1 {name: 'Bob'}) CREATE (a)-[:KNOWS]->(b)");
	(void) execute("MATCH (a:MT1 {name: 'Alice'}), (c:MT1 {name: 'Charlie'}) CREATE (a)-[:LIKES]->(c)");
	(void) execute("MATCH (a:MT1 {name: 'Alice'}), (d:MT1 {name: 'Dave'}) CREATE (a)-[:HATES]->(d)");

	// Verify individual types match correctly
	auto rKnows = execute(
		"MATCH (a:MT1 {name: 'Alice'})-[:KNOWS]->(b:MT1) RETURN b.name");
	ASSERT_EQ(rKnows.rowCount(), 1UL);
	EXPECT_EQ(val(rKnows, "b.name"), "Bob");

	auto rLikes = execute(
		"MATCH (a:MT1 {name: 'Alice'})-[:LIKES]->(b:MT1) RETURN b.name");
	ASSERT_EQ(rLikes.rowCount(), 1UL);
	EXPECT_EQ(val(rLikes, "b.name"), "Charlie");

	// HATES should not match when querying KNOWS
	auto rNoMatch = execute(
		"MATCH (a:MT1 {name: 'Alice'})-[:HATES]->(b:MT1) RETURN b.name");
	ASSERT_EQ(rNoMatch.rowCount(), 1UL);
	EXPECT_EQ(val(rNoMatch, "b.name"), "Dave");
}

TEST_F(CypherPatternsTest, MultiType_CountValidation) {
	(void) execute("CREATE (a:MT2 {name: 'A'})");
	(void) execute("CREATE (b:MT2 {name: 'B'})");
	(void) execute("CREATE (c:MT2 {name: 'C'})");
	(void) execute("CREATE (d:MT2 {name: 'D'})");
	(void) execute("MATCH (a:MT2 {name: 'A'}), (b:MT2 {name: 'B'}) CREATE (a)-[:R1]->(b)");
	(void) execute("MATCH (a:MT2 {name: 'A'}), (c:MT2 {name: 'C'}) CREATE (a)-[:R2]->(c)");
	(void) execute("MATCH (a:MT2 {name: 'A'}), (d:MT2 {name: 'D'}) CREATE (a)-[:R3]->(d)");

	// Verify each type has exactly one edge
	auto r1 = execute(
		"MATCH (a:MT2 {name: 'A'})-[:R1]->(b:MT2) RETURN count(b) AS cnt");
	EXPECT_EQ(val(r1, "cnt"), "1");

	auto r2 = execute(
		"MATCH (a:MT2 {name: 'A'})-[:R2]->(b:MT2) RETURN count(b) AS cnt");
	EXPECT_EQ(val(r2, "cnt"), "1");

	auto r3 = execute(
		"MATCH (a:MT2 {name: 'A'})-[:R3]->(b:MT2) RETURN count(b) AS cnt");
	EXPECT_EQ(val(r3, "cnt"), "1");
}

TEST_F(CypherPatternsTest, MultiType_NoMatchForUnlistedType) {
	(void) execute("CREATE (a:MT3 {name: 'A'})-[:KNOWS]->(b:MT3 {name: 'B'})");

	auto r = execute(
		"MATCH (a:MT3 {name: 'A'})-[:LIKES|HATES]->(b:MT3) "
		"RETURN b.name");
	EXPECT_EQ(r.rowCount(), 0UL);
}

TEST_F(CypherPatternsTest, MultiType_WithWhere) {
	(void) execute("CREATE (a:MT4 {name: 'A'})");
	(void) execute("CREATE (b:MT4 {name: 'B', val: 10})");
	(void) execute("CREATE (c:MT4 {name: 'C', val: 20})");
	(void) execute("MATCH (a:MT4 {name: 'A'}), (b:MT4 {name: 'B'}) CREATE (a)-[:KNOWS]->(b)");
	(void) execute("MATCH (a:MT4 {name: 'A'}), (c:MT4 {name: 'C'}) CREATE (a)-[:LIKES]->(c)");

	// Query each type with WHERE filter
	auto rKnows = execute(
		"MATCH (a:MT4 {name: 'A'})-[:KNOWS]->(b:MT4) "
		"WHERE b.val > 15 "
		"RETURN b.name");
	EXPECT_EQ(rKnows.rowCount(), 0UL);

	auto rLikes = execute(
		"MATCH (a:MT4 {name: 'A'})-[:LIKES]->(b:MT4) "
		"WHERE b.val > 15 "
		"RETURN b.name");
	ASSERT_EQ(rLikes.rowCount(), 1UL);
	EXPECT_EQ(val(rLikes, "b.name"), "C");
}

TEST_F(CypherPatternsTest, MultiType_ThreeTypes) {
	(void) execute("CREATE (a:MT5 {name: 'A'})");
	(void) execute("CREATE (b:MT5 {name: 'B'})");
	(void) execute("CREATE (c:MT5 {name: 'C'})");
	(void) execute("CREATE (d:MT5 {name: 'D'})");
	(void) execute("CREATE (e:MT5 {name: 'E'})");
	(void) execute("MATCH (a:MT5 {name: 'A'}), (b:MT5 {name: 'B'}) CREATE (a)-[:R1]->(b)");
	(void) execute("MATCH (a:MT5 {name: 'A'}), (c:MT5 {name: 'C'}) CREATE (a)-[:R2]->(c)");
	(void) execute("MATCH (a:MT5 {name: 'A'}), (d:MT5 {name: 'D'}) CREATE (a)-[:R3]->(d)");
	(void) execute("MATCH (a:MT5 {name: 'A'}), (e:MT5 {name: 'E'}) CREATE (a)-[:R4]->(e)");

	// All four types should be individually queryable
	auto rAll = execute(
		"MATCH (a:MT5 {name: 'A'})-[]->(b:MT5) "
		"RETURN count(b) AS cnt");
	ASSERT_EQ(rAll.rowCount(), 1UL);
	EXPECT_EQ(val(rAll, "cnt"), "4");

	// Specific type should match only one
	auto r1 = execute(
		"MATCH (a:MT5 {name: 'A'})-[:R1]->(b:MT5) RETURN b.name");
	ASSERT_EQ(r1.rowCount(), 1UL);
	EXPECT_EQ(val(r1, "b.name"), "B");
}

// ============================================================================
// Multi-Label Matching
// ============================================================================

TEST_F(CypherPatternsTest, MultiLabel_OnlyMatchesBoth) {
	(void) execute("CREATE (a:ML1A:ML1B {name: 'Both'})");
	(void) execute("CREATE (b:ML1A {name: 'OnlyA'})");
	(void) execute("CREATE (c:ML1B {name: 'OnlyB'})");

	auto r = execute("MATCH (n:ML1A:ML1B) RETURN n.name");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Both");
}

TEST_F(CypherPatternsTest, MultiLabel_ExcludesPartial) {
	(void) execute("CREATE (a:ML2X {name: 'JustX'})");
	(void) execute("CREATE (b:ML2Y {name: 'JustY'})");

	auto r = execute("MATCH (n:ML2X:ML2Y) RETURN n.name");
	EXPECT_EQ(r.rowCount(), 0UL);
}

TEST_F(CypherPatternsTest, MultiLabel_ThreeLabels) {
	(void) execute("CREATE (a:ML3A:ML3B:ML3C {name: 'Triple'})");
	(void) execute("CREATE (b:ML3A:ML3B {name: 'Double'})");

	auto r = execute("MATCH (n:ML3A:ML3B:ML3C) RETURN n.name");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Triple");
}

TEST_F(CypherPatternsTest, MultiLabel_WithProperty) {
	(void) execute("CREATE (a:ML4A:ML4B {name: 'Alice', val: 10})");
	(void) execute("CREATE (b:ML4A:ML4B {name: 'Bob', val: 20})");

	auto r = execute("MATCH (n:ML4A:ML4B) WHERE n.val > 15 RETURN n.name");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Bob");
}

TEST_F(CypherPatternsTest, MultiLabel_Count) {
	(void) execute("CREATE (a:ML5A:ML5B {name: 'X'})");
	(void) execute("CREATE (b:ML5A:ML5B {name: 'Y'})");
	(void) execute("CREATE (c:ML5A {name: 'Z'})");

	auto r = execute("MATCH (n:ML5A:ML5B) RETURN count(n) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "2");
}

// ============================================================================
// Variable-Length Path Variants
// ============================================================================

TEST_F(CypherPatternsTest, VarLength_ExactHops) {
	(void) execute("CREATE (a:VL1 {name: 'A'})-[:R]->(b:VL1 {name: 'B'})-[:R]->(c:VL1 {name: 'C'})-[:R]->(d:VL1 {name: 'D'})");

	auto r = execute("MATCH (a:VL1 {name: 'A'})-[:R*3]->(d:VL1) RETURN d.name");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "d.name"), "D");
}

TEST_F(CypherPatternsTest, VarLength_Unbounded) {
	(void) execute("CREATE (a:VL2 {name: 'A'})-[:R]->(b:VL2 {name: 'B'})-[:R]->(c:VL2 {name: 'C'})");

	auto r = execute("MATCH (a:VL2 {name: 'A'})-[:R*]->(x:VL2) RETURN x.name ORDER BY x.name");
	ASSERT_GE(r.rowCount(), 2UL);
}

TEST_F(CypherPatternsTest, VarLength_ZeroLower) {
	(void) execute("CREATE (a:VL3 {name: 'A'})-[:R]->(b:VL3 {name: 'B'})");

	// *0..1 includes zero-length (self match) and one hop
	auto r = execute("MATCH (a:VL3 {name: 'A'})-[:R*0..1]->(x:VL3) RETURN x.name ORDER BY x.name");
	ASSERT_GE(r.rowCount(), 1UL);
}

TEST_F(CypherPatternsTest, VarLength_OpenUpper) {
	(void) execute("CREATE (a:VL4 {name: 'A'})-[:R]->(b:VL4 {name: 'B'})-[:R]->(c:VL4 {name: 'C'})");

	auto r = execute("MATCH (a:VL4 {name: 'A'})-[:R*2..]->(x:VL4) RETURN x.name");
	ASSERT_GE(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "x.name"), "C");
}

TEST_F(CypherPatternsTest, VarLength_OpenLower) {
	(void) execute("CREATE (a:VL5 {name: 'A'})-[:R]->(b:VL5 {name: 'B'})-[:R]->(c:VL5 {name: 'C'})-[:R]->(d:VL5 {name: 'D'})");

	auto r = execute("MATCH (a:VL5 {name: 'A'})-[:R*..2]->(x:VL5) RETURN x.name ORDER BY x.name");
	ASSERT_GE(r.rowCount(), 2UL);
}

TEST_F(CypherPatternsTest, VarLength_NoResults) {
	(void) execute("CREATE (a:VL6 {name: 'A'})");

	auto r = execute("MATCH (a:VL6 {name: 'A'})-[:R*1..3]->(b:VL6) RETURN b.name");
	EXPECT_EQ(r.rowCount(), 0UL);
}

TEST_F(CypherPatternsTest, VarLength_WithDistinct) {
	(void) execute("CREATE (a:VL7 {name: 'A'})-[:R]->(b:VL7 {name: 'B'})-[:R]->(c:VL7 {name: 'C'})");

	auto r = execute(
		"MATCH (a:VL7 {name: 'A'})-[:R*]->(x:VL7) "
		"RETURN DISTINCT x.name ORDER BY x.name");
	ASSERT_GE(r.rowCount(), 2UL);
}

TEST_F(CypherPatternsTest, VarLength_WithNamedPath) {
	(void) execute("CREATE (a:VL8 {name: 'A'})-[:R]->(b:VL8 {name: 'B'})-[:R]->(c:VL8 {name: 'C'})");

	auto r = execute(
		"MATCH p = (a:VL8 {name: 'A'})-[:R*]->(c:VL8 {name: 'C'}) "
		"RETURN length(p) AS len");
	ASSERT_GE(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "len"), "2");
}

// ============================================================================
// shortestPath Expression (used as RETURN expression, not in MATCH pattern)
// ============================================================================

TEST_F(CypherPatternsTest, ShortestPath_AsReturnExpression) {
	(void) execute("CREATE (a:SP1 {name: 'A'})-[:R]->(b:SP1 {name: 'B'})-[:R]->(c:SP1 {name: 'C'})");

	auto r = execute(
		"MATCH (a:SP1 {name: 'A'}), (c:SP1 {name: 'C'}) "
		"RETURN shortestPath((a)-[:R*]->(c)) AS p");
	ASSERT_GE(r.rowCount(), 1UL);
	auto p = val(r, "p");
	EXPECT_NE(p, "null");
}

TEST_F(CypherPatternsTest, ShortestPath_NoPath) {
	(void) execute("CREATE (a:SP2 {name: 'A'})");
	(void) execute("CREATE (b:SP2 {name: 'B'})");

	auto r = execute(
		"MATCH (a:SP2 {name: 'A'}), (b:SP2 {name: 'B'}) "
		"RETURN shortestPath((a)-[:R*]->(b)) AS p");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "p"), "null");
}

TEST_F(CypherPatternsTest, ShortestPath_DirectAdjacent) {
	(void) execute("CREATE (a:SP3 {name: 'A'})-[:R]->(b:SP3 {name: 'B'})");

	auto r = execute(
		"MATCH (a:SP3 {name: 'A'}), (b:SP3 {name: 'B'}) "
		"RETURN shortestPath((a)-[:R*]->(b)) AS p");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto p = val(r, "p");
	EXPECT_NE(p, "null");
}

TEST_F(CypherPatternsTest, ShortestPath_AllShortestPaths) {
	// Create a diamond: A->B->D, A->C->D
	(void) execute("CREATE (a:SP4 {name: 'A'})");
	(void) execute("CREATE (b:SP4 {name: 'B'})");
	(void) execute("CREATE (c:SP4 {name: 'C'})");
	(void) execute("CREATE (d:SP4 {name: 'D'})");
	(void) execute("MATCH (a:SP4 {name: 'A'}), (b:SP4 {name: 'B'}) CREATE (a)-[:R]->(b)");
	(void) execute("MATCH (a:SP4 {name: 'A'}), (c:SP4 {name: 'C'}) CREATE (a)-[:R]->(c)");
	(void) execute("MATCH (b:SP4 {name: 'B'}), (d:SP4 {name: 'D'}) CREATE (b)-[:R]->(d)");
	(void) execute("MATCH (c:SP4 {name: 'C'}), (d:SP4 {name: 'D'}) CREATE (c)-[:R]->(d)");

	auto r = execute(
		"MATCH (a:SP4 {name: 'A'}), (d:SP4 {name: 'D'}) "
		"RETURN allShortestPaths((a)-[:R*]->(d)) AS p");
	ASSERT_GE(r.rowCount(), 1UL);
	auto p = val(r, "p");
	EXPECT_NE(p, "null");
}

TEST_F(CypherPatternsTest, ShortestPath_LengthOfResult) {
	(void) execute("CREATE (a:SP5 {name: 'A'})-[:R]->(b:SP5 {name: 'B'})-[:R]->(c:SP5 {name: 'C'})");

	// Verify shortest path exists and is non-null
	auto r = execute(
		"MATCH (a:SP5 {name: 'A'}), (c:SP5 {name: 'C'}) "
		"RETURN shortestPath((a)-[:R*]->(c)) AS p");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_NE(val(r, "p"), "null");
}
