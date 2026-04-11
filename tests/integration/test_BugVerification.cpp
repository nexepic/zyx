/**
 * @file test_BugVerification.cpp
 * @brief Temporary test to verify if remaining bugs still exist
 **/

#include "QueryTestFixture.hpp"
#include <set>

class BugVerificationTest : public QueryTestFixture {
protected:
	std::string val(const graph::query::QueryResult &r, const std::string &col, size_t row = 0) {
		const auto &rows = r.getRows();
		if (row >= rows.size()) return "<no_row>";
		auto it = rows[row].find(col);
		if (it == rows[row].end()) return "<no_col>";
		return it->second.toString();
	}
};

// Bug #5: count(DISTINCT expr) — does count(DISTINCT ...) deduplicate?
TEST_F(BugVerificationTest, CountDistinctFunction) {
	(void) execute("CREATE (a:BV5 {cat: 'A'})");
	(void) execute("CREATE (b:BV5 {cat: 'A'})");
	(void) execute("CREATE (c:BV5 {cat: 'B'})");

	auto r = execute("MATCH (n:BV5) RETURN count(DISTINCT n.cat) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "2");  // Should be 2 distinct categories
}

// Bug #10: GROUP BY — aggregation with grouping key
TEST_F(BugVerificationTest, GroupByWithKey) {
	(void) execute("CREATE (a:BV10 {cat: 'X', val: 10})");
	(void) execute("CREATE (b:BV10 {cat: 'X', val: 20})");
	(void) execute("CREATE (c:BV10 {cat: 'Y', val: 30})");

	auto r = execute("MATCH (n:BV10) RETURN n.cat AS cat, sum(n.val) AS total");
	ASSERT_EQ(r.rowCount(), 2UL);  // Should have 2 groups: X and Y
	// Verify both groups exist (order may vary)
	std::set<std::string> cats;
	cats.insert(val(r, "cat", 0));
	cats.insert(val(r, "cat", 1));
	EXPECT_TRUE(cats.count("X"));
	EXPECT_TRUE(cats.count("Y"));
	// Verify sums: X group = 10+20=30, Y group = 30
	for (size_t i = 0; i < 2; ++i) {
		if (val(r, "cat", i) == "X") EXPECT_EQ(val(r, "total", i), "30");
		if (val(r, "cat", i) == "Y") EXPECT_EQ(val(r, "total", i), "30");
	}
}

// Bug #15: Triple AND/OR without explicit parentheses
TEST_F(BugVerificationTest, TripleAndWithoutParens) {
	(void) execute("CREATE (a:BV15 {x: 1, y: 2, z: 3})");
	(void) execute("CREATE (b:BV15 {x: 1, y: 2, z: 99})");
	(void) execute("CREATE (c:BV15 {x: 1, y: 99, z: 3})");

	auto r = execute("MATCH (n:BV15) WHERE n.x = 1 AND n.y = 2 AND n.z = 3 RETURN n.x");
	ASSERT_EQ(r.rowCount(), 1UL);  // Only first node matches all 3 conditions
}

// Bug #16: >= operator
TEST_F(BugVerificationTest, GreaterThanOrEqual) {
	(void) execute("CREATE (a:BV16 {val: 5})");
	(void) execute("CREATE (b:BV16 {val: 10})");
	(void) execute("CREATE (c:BV16 {val: 15})");

	auto r = execute("MATCH (n:BV16) WHERE n.val >= 10 RETURN n.val ORDER BY n.val");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "n.val", 0), "10");
	EXPECT_EQ(val(r, "n.val", 1), "15");
}

// Bug #9: Quantifier functions — all(), any(), none(), single()
TEST_F(BugVerificationTest, QuantifierAll) {
	auto r = execute("RETURN all(x IN [2, 4, 6] WHERE x % 2 = 0) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "true");
}

TEST_F(BugVerificationTest, QuantifierAny) {
	auto r = execute("RETURN any(x IN [1, 2, 3] WHERE x > 2) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "true");
}

TEST_F(BugVerificationTest, QuantifierNone) {
	auto r = execute("RETURN none(x IN [1, 3, 5] WHERE x > 10) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "true");
}

TEST_F(BugVerificationTest, QuantifierSingle) {
	auto r = execute("RETURN single(x IN [1, 2, 3] WHERE x > 2) AS val");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "val"), "true");
}

// Bug: ORDER BY on aggregate alias — count(n) was looked up as scalar function
TEST_F(BugVerificationTest, AggregateOrderByAlias) {
	(void) execute("CREATE (a:AggOrd {city: 'Beijing'})");
	(void) execute("CREATE (b:AggOrd {city: 'Beijing'})");
	(void) execute("CREATE (c:AggOrd {city: 'Shanghai'})");
	(void) execute("CREATE (d:AggOrd {city: 'Shanghai'})");
	(void) execute("CREATE (e:AggOrd {city: 'Shanghai'})");

	auto r = execute("MATCH (n:AggOrd) RETURN n.city, count(n) AS cnt ORDER BY cnt DESC");
	ASSERT_EQ(r.rowCount(), 2UL);
	// Shanghai has 3 nodes, Beijing has 2 — DESC means Shanghai first
	EXPECT_EQ(val(r, "n.city", 0), "Shanghai");
	EXPECT_EQ(val(r, "cnt", 0), "3");
	EXPECT_EQ(val(r, "n.city", 1), "Beijing");
	EXPECT_EQ(val(r, "cnt", 1), "2");
}

// Bug: Aggregate without ORDER BY still works (regression check)
TEST_F(BugVerificationTest, AggregateWithoutOrderBy) {
	(void) execute("CREATE (a:AggNoOrd {val: 10})");
	(void) execute("CREATE (b:AggNoOrd {val: 20})");
	(void) execute("CREATE (c:AggNoOrd {val: 30})");

	auto r = execute("MATCH (n:AggNoOrd) RETURN count(n) AS cnt, sum(n.val) AS total");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "3");
	EXPECT_EQ(val(r, "total"), "60");
}

// Bug: ORDER BY ASC on aggregate alias
TEST_F(BugVerificationTest, AggregateOrderByAliasAsc) {
	(void) execute("CREATE (a:AggOrdAsc {cat: 'A', val: 1})");
	(void) execute("CREATE (b:AggOrdAsc {cat: 'B', val: 2})");
	(void) execute("CREATE (c:AggOrdAsc {cat: 'B', val: 3})");

	auto r = execute("MATCH (n:AggOrdAsc) RETURN n.cat AS cat, sum(n.val) AS total ORDER BY total");
	ASSERT_EQ(r.rowCount(), 2UL);
	// A has total=1, B has total=5 — ASC means A first
	EXPECT_EQ(val(r, "cat", 0), "A");
	EXPECT_EQ(val(r, "total", 0), "1");
	EXPECT_EQ(val(r, "cat", 1), "B");
	EXPECT_EQ(val(r, "total", 1), "5");
}
