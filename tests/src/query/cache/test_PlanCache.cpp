/**
 * @file test_PlanCache.cpp
 * @brief Focused tests for PlanCache update/hit/miss branches.
 */

#include <gtest/gtest.h>

#include "graph/query/cache/PlanCache.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"

using namespace graph::query::cache;
using namespace graph::query::logical;

TEST(PlanCacheTest, MissThenHitAndUpdateExistingEntry) {
	PlanCache cache(2);

	auto miss = cache.get("MATCH (n) RETURN n");
	EXPECT_EQ(miss, nullptr);
	EXPECT_EQ(cache.misses(), 1UL);

	auto plan = std::make_unique<LogicalNodeScan>("n");
	cache.put("MATCH (n) RETURN n", plan.get());
	EXPECT_EQ(cache.size(), 1UL);

	auto hit = cache.get("MATCH (n) RETURN n");
	ASSERT_NE(hit, nullptr);
	EXPECT_EQ(cache.hits(), 1UL);

	cache.put("MATCH (n) RETURN n", plan.get());
	EXPECT_EQ(cache.size(), 1UL);
}

TEST(PlanCacheTest, EvictsLeastRecentlyUsedWhenAtCapacity) {
	PlanCache cache(2);
	auto plan = std::make_unique<LogicalNodeScan>("n");

	cache.put("q1", plan.get());
	cache.put("q2", plan.get());
	ASSERT_EQ(cache.size(), 2UL);

	(void)cache.get("q2");
	cache.put("q3", plan.get());

	EXPECT_EQ(cache.size(), 2UL);
	EXPECT_EQ(cache.get("q1"), nullptr);
}
