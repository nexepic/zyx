/**
 * @file test_IntegrationAggregateProjection.cpp
 * @brief Comprehensive integration tests for aggregate/projection pipeline.
 *
 * Covers: aggregate + ORDER BY, nested aggregates, multi-clause scoping,
 * DISTINCT with aggregates, edge cases from the IR refactoring plan (Phase 5).
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "QueryTestFixture.hpp"
#include <set>

class AggregateProjectionTest : public QueryTestFixture {
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
		// Shared test data: cities with populations
		(void) execute("CREATE (:City {name: 'Beijing', country: 'CN', pop: 21000000})");
		(void) execute("CREATE (:City {name: 'Shanghai', country: 'CN', pop: 24000000})");
		(void) execute("CREATE (:City {name: 'Shenzhen', country: 'CN', pop: 13000000})");
		(void) execute("CREATE (:City {name: 'Tokyo', country: 'JP', pop: 14000000})");
		(void) execute("CREATE (:City {name: 'Osaka', country: 'JP', pop: 2700000})");
		(void) execute("CREATE (:City {name: 'Paris', country: 'FR', pop: 2100000})");
	}
};

// --- Aggregate + ORDER BY combinations ---

TEST_F(AggregateProjectionTest, OrderByAggregateAliasDesc) {
	auto r = execute(
		"MATCH (c:City) RETURN c.country AS country, count(c) AS cnt "
		"ORDER BY cnt DESC");
	ASSERT_EQ(r.rowCount(), 3UL);
	// CN=3, JP=2, FR=1
	EXPECT_EQ(val(r, "country", 0), "CN");
	EXPECT_EQ(val(r, "cnt", 0), "3");
	EXPECT_EQ(val(r, "country", 1), "JP");
	EXPECT_EQ(val(r, "cnt", 1), "2");
	EXPECT_EQ(val(r, "country", 2), "FR");
	EXPECT_EQ(val(r, "cnt", 2), "1");
}

TEST_F(AggregateProjectionTest, OrderByAggregateAliasAsc) {
	auto r = execute(
		"MATCH (c:City) RETURN c.country AS country, count(c) AS cnt "
		"ORDER BY cnt ASC");
	ASSERT_EQ(r.rowCount(), 3UL);
	// FR=1, JP=2, CN=3
	EXPECT_EQ(val(r, "country", 0), "FR");
	EXPECT_EQ(val(r, "cnt", 0), "1");
	EXPECT_EQ(val(r, "country", 2), "CN");
	EXPECT_EQ(val(r, "cnt", 2), "3");
}

TEST_F(AggregateProjectionTest, OrderBySumAlias) {
	auto r = execute(
		"MATCH (c:City) RETURN c.country AS country, sum(c.pop) AS totalPop "
		"ORDER BY totalPop DESC");
	ASSERT_EQ(r.rowCount(), 3UL);
	// CN has highest total pop
	EXPECT_EQ(val(r, "country", 0), "CN");
}

TEST_F(AggregateProjectionTest, OrderByGroupKeyAndAggregate) {
	auto r = execute(
		"MATCH (c:City) RETURN c.country AS country, count(c) AS cnt "
		"ORDER BY country");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "country", 0), "CN");
	EXPECT_EQ(val(r, "country", 1), "FR");
	EXPECT_EQ(val(r, "country", 2), "JP");
}

// --- Multiple aggregates ---

TEST_F(AggregateProjectionTest, MultipleAggregatesInReturn) {
	auto r = execute(
		"MATCH (c:City) RETURN c.country AS country, "
		"count(c) AS cnt, sum(c.pop) AS total, avg(c.pop) AS avgPop "
		"ORDER BY country");
	ASSERT_EQ(r.rowCount(), 3UL);
	// CN: count=3
	EXPECT_EQ(val(r, "country", 0), "CN");
	EXPECT_EQ(val(r, "cnt", 0), "3");
}

// --- Aggregate without GROUP BY (single-row result) ---

TEST_F(AggregateProjectionTest, AggregateWithoutGroupBy) {
	auto r = execute("MATCH (c:City) RETURN count(c) AS cnt, sum(c.pop) AS total");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "6");
}

// --- Nested aggregate expressions ---

TEST_F(AggregateProjectionTest, AggregateInArithmetic) {
	auto r = execute("MATCH (c:City) RETURN count(c) + 1 AS cntPlusOne");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cntPlusOne"), "7");
}

TEST_F(AggregateProjectionTest, AggregateMultiplication) {
	auto r = execute(
		"MATCH (c:City) RETURN c.country AS country, sum(c.pop) * 2 AS doubled "
		"ORDER BY country");
	ASSERT_EQ(r.rowCount(), 3UL);
}

// --- DISTINCT with aggregates ---

TEST_F(AggregateProjectionTest, CountDistinct) {
	auto r = execute("MATCH (c:City) RETURN count(DISTINCT c.country) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "3");
}

// --- WITH clause aggregation ---

TEST_F(AggregateProjectionTest, WithAggregateAndReturn) {
	auto r = execute(
		"MATCH (c:City) "
		"WITH c.country AS country, count(c) AS cnt "
		"RETURN country, cnt ORDER BY cnt DESC");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "country", 0), "CN");
	EXPECT_EQ(val(r, "cnt", 0), "3");
}

TEST_F(AggregateProjectionTest, WithAggregateWhereFilter) {
	auto r = execute(
		"MATCH (c:City) "
		"WITH c.country AS country, count(c) AS cnt "
		"WHERE cnt >= 2 "
		"RETURN country ORDER BY country");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "country", 0), "CN");
	EXPECT_EQ(val(r, "country", 1), "JP");
}

// --- Multi-clause scoping: MATCH → WITH agg → WHERE → RETURN ---

TEST_F(AggregateProjectionTest, MultiClauseAggScoping) {
	auto r = execute(
		"MATCH (c:City) "
		"WITH c.country AS country, sum(c.pop) AS totalPop "
		"WHERE totalPop > 10000000 "
		"RETURN country, totalPop ORDER BY totalPop DESC");
	ASSERT_EQ(r.rowCount(), 2UL);
	// CN and JP have total pop > 10M
	EXPECT_EQ(val(r, "country", 0), "CN");
	EXPECT_EQ(val(r, "country", 1), "JP");
}

// --- ORDER BY with complex expressions after aggregation ---

TEST_F(AggregateProjectionTest, OrderByAggregateExpression) {
	auto r = execute(
		"MATCH (c:City) RETURN c.country AS country, sum(c.pop) AS total "
		"ORDER BY total DESC");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "country", 0), "CN");
}

// --- Empty result sets ---

TEST_F(AggregateProjectionTest, AggregateOnEmptyMatch) {
	auto r = execute("MATCH (c:NonExistentLabel) RETURN count(c) AS cnt");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "cnt"), "0");
}

TEST_F(AggregateProjectionTest, AggregateGroupByOnEmptyMatch) {
	auto r = execute(
		"MATCH (c:NonExistentLabel) RETURN c.country AS country, count(c) AS cnt");
	EXPECT_EQ(r.rowCount(), 0UL);
}

// --- LIMIT with aggregation ---

TEST_F(AggregateProjectionTest, AggregateWithLimit) {
	auto r = execute(
		"MATCH (c:City) RETURN c.country AS country, count(c) AS cnt "
		"ORDER BY cnt DESC LIMIT 2");
	ASSERT_EQ(r.rowCount(), 2UL);
	EXPECT_EQ(val(r, "country", 0), "CN");
	EXPECT_EQ(val(r, "cnt", 0), "3");
}

// --- SKIP with aggregation ---

TEST_F(AggregateProjectionTest, AggregateWithSkip) {
	auto r = execute(
		"MATCH (c:City) RETURN c.country AS country, count(c) AS cnt "
		"ORDER BY cnt DESC SKIP 1");
	ASSERT_EQ(r.rowCount(), 2UL);
	// Skipped CN(3), should start from JP(2)
	EXPECT_EQ(val(r, "country", 0), "JP");
}

// --- SKIP + LIMIT ---

TEST_F(AggregateProjectionTest, AggregateWithSkipAndLimit) {
	auto r = execute(
		"MATCH (c:City) RETURN c.country AS country, count(c) AS cnt "
		"ORDER BY cnt DESC SKIP 1 LIMIT 1");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "country", 0), "JP");
	EXPECT_EQ(val(r, "cnt", 0), "2");
}

// --- Simple (non-aggregate) ORDER BY + LIMIT ---

TEST_F(AggregateProjectionTest, SimpleOrderByWithLimit) {
	auto r = execute(
		"MATCH (c:City) RETURN c.name AS name, c.pop AS pop "
		"ORDER BY c.pop DESC LIMIT 3");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "name", 0), "Shanghai");
}

// --- RETURN * ---

TEST_F(AggregateProjectionTest, ReturnStarWithOrderBy) {
	// Create small isolated data for this test
	(void) execute("CREATE (:Star {v: 3})");
	(void) execute("CREATE (:Star {v: 1})");
	(void) execute("CREATE (:Star {v: 2})");

	auto r = execute("MATCH (n:Star) RETURN n.v AS v ORDER BY v");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "v", 0), "1");
	EXPECT_EQ(val(r, "v", 1), "2");
	EXPECT_EQ(val(r, "v", 2), "3");
}

// --- Aggregate on expressions ---

TEST_F(AggregateProjectionTest, SumOfExpression) {
	auto r = execute(
		"MATCH (c:City) WHERE c.country = 'CN' "
		"RETURN sum(c.pop / 1000000) AS totalMillions");
	ASSERT_EQ(r.rowCount(), 1UL);
	// 21+24+13 = 58
	EXPECT_EQ(val(r, "totalMillions"), "58");
}

// --- collect() aggregate ---

TEST_F(AggregateProjectionTest, CollectAggregate) {
	auto r = execute(
		"MATCH (c:City) WHERE c.country = 'JP' "
		"RETURN collect(c.name) AS names");
	ASSERT_EQ(r.rowCount(), 1UL);
	std::string names = val(r, "names");
	// Should contain both Tokyo and Osaka
	EXPECT_NE(names.find("Tokyo"), std::string::npos);
	EXPECT_NE(names.find("Osaka"), std::string::npos);
}

// --- min/max aggregates with ORDER BY ---

TEST_F(AggregateProjectionTest, MinMaxWithGroupByAndOrderBy) {
	auto r = execute(
		"MATCH (c:City) RETURN c.country AS country, "
		"min(c.pop) AS minPop, max(c.pop) AS maxPop "
		"ORDER BY country");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "country", 0), "CN");
	EXPECT_EQ(val(r, "country", 1), "FR");
	EXPECT_EQ(val(r, "country", 2), "JP");
}

// --- WITH chaining: two aggregation stages ---

TEST_F(AggregateProjectionTest, ChainedWithAggregations) {
	auto r = execute(
		"MATCH (c:City) "
		"WITH c.country AS country, count(c) AS cnt "
		"WITH count(country) AS numCountries "
		"RETURN numCountries");
	ASSERT_EQ(r.rowCount(), 1UL);
	EXPECT_EQ(val(r, "numCountries"), "3");
}

// --- RETURN DISTINCT (non-aggregate) ---

TEST_F(AggregateProjectionTest, ReturnDistinct) {
	auto r = execute("MATCH (c:City) RETURN DISTINCT c.country AS country ORDER BY country");
	ASSERT_EQ(r.rowCount(), 3UL);
	EXPECT_EQ(val(r, "country", 0), "CN");
	EXPECT_EQ(val(r, "country", 1), "FR");
	EXPECT_EQ(val(r, "country", 2), "JP");
}
