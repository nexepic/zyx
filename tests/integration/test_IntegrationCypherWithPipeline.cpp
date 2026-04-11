/**
 * @file test_IntegrationCypherWithPipeline.cpp
 * @brief Integration tests for deep WITH pipeline chains.
 *
 * Covers: three-stage WITH chains, WITH + subsequent MATCH,
 * RETURN DISTINCT + ORDER BY + LIMIT, chained aggregation edge cases.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "QueryTestFixture.hpp"
#include <set>

class CypherWithPipelineTest : public QueryTestFixture {
protected:
	std::string val(const graph::query::QueryResult &r, const std::string &col, size_t row = 0) {
		const auto &rows = r.getRows();
		if (row >= rows.size()) return "<no_row>";
		auto it = rows[row].find(col);
		if (it == rows[row].end()) return "<no_col>";
		return it->second.toString();
	}

	void SetUp() override {
		QueryTestFixture::SetUp();
		// Shared social graph: 6 persons in 3 departments with relationships
		(void) execute("CREATE (a:WP {name: 'Alice', dept: 'Eng', salary: 100})");
		(void) execute("CREATE (b:WP {name: 'Bob', dept: 'Eng', salary: 120})");
		(void) execute("CREATE (c:WP {name: 'Charlie', dept: 'Sales', salary: 90})");
		(void) execute("CREATE (d:WP {name: 'Dave', dept: 'Sales', salary: 85})");
		(void) execute("CREATE (e:WP {name: 'Eve', dept: 'HR', salary: 95})");
		(void) execute("CREATE (f:WP {name: 'Frank', dept: 'HR', salary: 110})");
		// KNOWS relationships
		(void) execute("MATCH (a:WP {name: 'Alice'}), (b:WP {name: 'Bob'}) CREATE (a)-[:KNOWS]->(b)");
		(void) execute("MATCH (a:WP {name: 'Alice'}), (c:WP {name: 'Charlie'}) CREATE (a)-[:KNOWS]->(c)");
		(void) execute("MATCH (b:WP {name: 'Bob'}), (d:WP {name: 'Dave'}) CREATE (b)-[:KNOWS]->(d)");
		(void) execute("MATCH (e:WP {name: 'Eve'}), (f:WP {name: 'Frank'}) CREATE (e)-[:KNOWS]->(f)");
	}
};

// ============================================================================
// Three-Stage WITH Chains
// ============================================================================

TEST_F(CypherWithPipelineTest, ThreeStage_AggFilterReturn) {
	auto r = execute(
		"MATCH (n:WP) "
		"WITH n.dept AS dept, sum(n.salary) AS totalSal "
		"WITH dept, totalSal WHERE totalSal > 180 "
		"RETURN dept, totalSal ORDER BY totalSal DESC");
	ASSERT_GE(r.rowCount(), 1UL);
	// Eng: 100+120=220, HR: 95+110=205, Sales: 90+85=175
	EXPECT_EQ(val(r, "dept", 0), "Eng");
}

TEST_F(CypherWithPipelineTest, ThreeStage_AggToAgg) {
	auto r = execute(
		"MATCH (n:WP) "
		"WITH n.dept AS dept, count(n) AS cnt "
		"WITH count(dept) AS numDepts "
		"RETURN numDepts");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "numDepts"), "3");
}

TEST_F(CypherWithPipelineTest, ThreeStage_ScalarToAgg) {
	auto r = execute(
		"MATCH (n:WP) "
		"WITH n.salary AS sal "
		"WITH sum(sal) AS totalSal "
		"RETURN totalSal");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "totalSal"), "600");
}

TEST_F(CypherWithPipelineTest, ThreeStage_FilterToAgg) {
	auto r = execute(
		"MATCH (n:WP) "
		"WITH n WHERE n.salary > 95 "
		"WITH count(n) AS cnt "
		"RETURN cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	// Alice(100), Bob(120), Frank(110) = 3
	EXPECT_EQ(val(r, "cnt"), "3");
}

TEST_F(CypherWithPipelineTest, ThreeStage_RenameToAgg) {
	auto r = execute(
		"MATCH (n:WP) "
		"WITH n.salary AS s "
		"WITH avg(s) AS avgSal "
		"RETURN avgSal");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "avgSal"), "100");
}

TEST_F(CypherWithPipelineTest, ThreeStage_FilterChain) {
	auto r = execute(
		"MATCH (n:WP) "
		"WITH n WHERE n.salary > 90 "
		"WITH n WHERE n.dept = 'Eng' "
		"RETURN n.name ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.name", 0), "Alice");
	EXPECT_EQ(val(r, "n.name", 1), "Bob");
}

TEST_F(CypherWithPipelineTest, ThreeStage_AggWithWhere) {
	auto r = execute(
		"MATCH (n:WP) "
		"WITH n.dept AS dept, sum(n.salary) AS total "
		"WHERE total >= 200 "
		"RETURN dept ORDER BY dept");
	ASSERT_GE(r.rowCount(), 1UL);
	// Eng=220, HR=205 both >= 200
}

// ============================================================================
// WITH + Subsequent MATCH
// ============================================================================

TEST_F(CypherWithPipelineTest, WithSubsequentMatch_AggResultAsFilter) {
	auto r = execute(
		"MATCH (n:WP) "
		"WITH n.dept AS dept, avg(n.salary) AS avgSal "
		"WHERE avgSal > 100 "
		"RETURN dept ORDER BY dept");
	ASSERT_GE(r.rowCount(), 1UL);
	// Eng: avg=110, HR: avg=102.5, Sales: avg=87.5
	EXPECT_EQ(val(r, "dept", 0), "Eng");
}

TEST_F(CypherWithPipelineTest, WithSubsequentMatch_TopSalary) {
	auto r = execute(
		"MATCH (n:WP) "
		"RETURN n.name, n.salary ORDER BY n.salary DESC LIMIT 1");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "n.name"), "Bob");
	EXPECT_EQ(val(r, "n.salary"), "120");
}

TEST_F(CypherWithPipelineTest, WithSubsequentMatch_EngFriends) {
	auto r = execute(
		"MATCH (n:WP)-[:KNOWS]->(m:WP) "
		"WHERE n.dept = 'Eng' "
		"RETURN n.name AS person, m.name AS friend ORDER BY person, friend");
	ASSERT_GE(r.rowCount(), 2UL);
}

TEST_F(CypherWithPipelineTest, WithSubsequentMatch_CountFriends) {
	auto r = execute(
		"MATCH (n:WP)-[:KNOWS]->(m:WP) "
		"RETURN n.name AS person, count(m) AS friends "
		"ORDER BY friends DESC");
	ASSERT_GE(r.rowCount(), 1UL);
	// Alice knows Bob and Charlie = 2 friends
	EXPECT_EQ(val(r, "person", 0), "Alice");
	EXPECT_EQ(val(r, "friends", 0), "2");
}

TEST_F(CypherWithPipelineTest, WithSubsequentMatch_OptionalWithCount) {
	auto r = execute(
		"MATCH (n:WP) "
		"OPTIONAL MATCH (n)-[:KNOWS]->(m:WP) "
		"RETURN n.name, count(m) AS friends ORDER BY n.name");
	ASSERT_EQ(r.rowCount(), 6UL);
}

// ============================================================================
// RETURN DISTINCT + ORDER BY + LIMIT
// ============================================================================

TEST_F(CypherWithPipelineTest, ReturnDistinct_CombinedPipeline) {
	auto r = execute(
		"MATCH (n:WP) "
		"RETURN DISTINCT n.dept AS dept ORDER BY dept");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "dept", 0), "Eng");
	EXPECT_EQ(val(r, "dept", 1), "HR");
	EXPECT_EQ(val(r, "dept", 2), "Sales");
}

TEST_F(CypherWithPipelineTest, ReturnDistinct_SkipLimit) {
	auto r = execute(
		"MATCH (n:WP) "
		"RETURN DISTINCT n.dept AS dept ORDER BY dept SKIP 1 LIMIT 1");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "dept"), "HR");
}

TEST_F(CypherWithPipelineTest, ReturnDistinct_AfterWith) {
	auto r = execute(
		"MATCH (n:WP) "
		"WITH n.dept AS dept, n.salary AS sal "
		"RETURN DISTINCT dept ORDER BY dept");
	ASSERT_EQ(r.rowCount(), 3UL);
}

TEST_F(CypherWithPipelineTest, ReturnDistinct_MultiColumn) {
	(void) execute("CREATE (x:WPMC {a: 1, b: 'X'})");
	(void) execute("CREATE (y:WPMC {a: 1, b: 'X'})");
	(void) execute("CREATE (z:WPMC {a: 1, b: 'Y'})");

	auto r = execute(
		"MATCH (n:WPMC) "
		"RETURN DISTINCT n.a AS a, n.b AS b ORDER BY b");
	ASSERT_EQ(r.rowCount(), 2UL);
}

TEST_F(CypherWithPipelineTest, ReturnDistinct_WithAggregate) {
	auto r = execute(
		"MATCH (n:WP) "
		"RETURN DISTINCT n.dept AS dept, count(n) AS cnt "
		"ORDER BY cnt DESC");
	// Each dept is already distinct after GROUP BY
	ASSERT_EQ(r.rowCount(), 3UL);
}

// ============================================================================
// ORDER BY + SKIP + LIMIT in RETURN
// ============================================================================

TEST_F(CypherWithPipelineTest, ReturnSkipLimit_CorrectOffset) {
	auto r = execute(
		"MATCH (n:WP) "
		"RETURN n.name AS name ORDER BY name SKIP 2 LIMIT 2");
	ASSERT_EQ(r.rowCount(), 2UL);
	// Alphabetical: Alice, Bob, Charlie, Dave, Eve, Frank
	// Skip 2 = Charlie, Dave
	EXPECT_EQ(val(r, "name", 0), "Charlie");
	EXPECT_EQ(val(r, "name", 1), "Dave");
}

TEST_F(CypherWithPipelineTest, ReturnSkipLimit_MultiColumnSort) {
	auto r = execute(
		"MATCH (n:WP) "
		"RETURN n.dept AS dept, n.name AS name ORDER BY dept, name");
	ASSERT_EQ(r.rowCount(), 6UL);
	// Eng: Alice, Bob
	EXPECT_EQ(val(r, "dept", 0), "Eng");
	EXPECT_EQ(val(r, "name", 0), "Alice");
}

TEST_F(CypherWithPipelineTest, ReturnSkipLimit_TopN) {
	auto r = execute(
		"MATCH (n:WP) "
		"RETURN n.name, n.salary ORDER BY n.salary DESC LIMIT 3");
	ASSERT_EQ(r.rowCount(), 3UL);
	// Bob(120), Frank(110), Alice(100)
	EXPECT_EQ(val(r, "n.name", 0), "Bob");
}

TEST_F(CypherWithPipelineTest, ReturnSkipLimit_BottomN) {
	auto r = execute(
		"MATCH (n:WP) "
		"RETURN n.name, n.salary ORDER BY n.salary ASC LIMIT 2");
	ASSERT_EQ(r.rowCount(), 2UL);
	// Dave(85), Charlie(90)
	EXPECT_EQ(val(r, "n.name", 0), "Dave");
}

// ============================================================================
// Chained Aggregation Edge Cases
// ============================================================================

TEST_F(CypherWithPipelineTest, ChainedAgg_CountOfCount) {
	auto r = execute(
		"MATCH (n:WP) "
		"WITH n.dept AS dept, count(n) AS cnt "
		"WITH count(cnt) AS numGroups "
		"RETURN numGroups");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "numGroups"), "3");
}

TEST_F(CypherWithPipelineTest, ChainedAgg_SumOfSums) {
	auto r = execute(
		"MATCH (n:WP) "
		"WITH n.dept AS dept, sum(n.salary) AS deptTotal "
		"WITH sum(deptTotal) AS grandTotal "
		"RETURN grandTotal");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "grandTotal"), "600");
}

TEST_F(CypherWithPipelineTest, ChainedAgg_CollectThenSize) {
	auto r = execute(
		"MATCH (n:WP) WHERE n.dept = 'Eng' "
		"WITH collect(n.name) AS names "
		"RETURN size(names) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "2");
}

TEST_F(CypherWithPipelineTest, ChainedAgg_CollectUnwindRoundTrip) {
	auto r = execute(
		"MATCH (n:WP) WHERE n.dept = 'Eng' "
		"WITH collect(n.name) AS names "
		"UNWIND names AS name "
		"RETURN name ORDER BY name");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "name", 0), "Alice");
	EXPECT_EQ(val(r, "name", 1), "Bob");
}

TEST_F(CypherWithPipelineTest, ChainedAgg_ReduceOnCollected) {
	auto r = execute(
		"MATCH (n:WP) WHERE n.dept = 'Eng' "
		"WITH collect(n.salary) AS salaries "
		"RETURN reduce(total = 0, s IN salaries | total + s) AS sum");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "sum"), "220");
}

TEST_F(CypherWithPipelineTest, ChainedAgg_OrderBySizeCollected) {
	auto r = execute(
		"MATCH (n:WP) "
		"WITH n.dept AS dept, collect(n.name) AS members "
		"RETURN dept, size(members) AS cnt ORDER BY cnt DESC");
	ASSERT_EQ(r.rowCount(), 3UL);
	// All depts have 2 members, so cnt should be 2
	EXPECT_EQ(val(r, "cnt", 0), "2");
}

TEST_F(CypherWithPipelineTest, ChainedAgg_EmptyGroupPropagation) {
	auto r = execute(
		"MATCH (n:WPNonExist) "
		"WITH n.dept AS dept, count(n) AS cnt "
		"RETURN count(dept) AS numGroups");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "numGroups"), "0");
}

TEST_F(CypherWithPipelineTest, ChainedAgg_CollectReduce) {
	auto r = execute(
		"MATCH (n:WP) WHERE n.dept = 'HR' "
		"WITH collect(n.name) AS names "
		"RETURN reduce(s = '', name IN names | s + name) AS concat");
	ASSERT_EQ(r.rowCount(), 1UL);
	auto concat = val(r, "concat");
	EXPECT_NE(concat.find("Eve"), std::string::npos);
	EXPECT_NE(concat.find("Frank"), std::string::npos);
}

TEST_F(CypherWithPipelineTest, ChainedAgg_NullGroupKey) {
	(void) execute("CREATE (x:WPNull {name: 'Test'})");  // no dept

	auto r = execute(
		"MATCH (n:WPNull) "
		"WITH n.dept AS dept, count(n) AS cnt "
		"RETURN dept, cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "dept"), "null");
	EXPECT_EQ(val(r, "cnt"), "1");
}
