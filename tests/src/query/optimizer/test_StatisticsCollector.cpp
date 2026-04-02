/**
 * @file test_StatisticsCollector.cpp
 * @brief Unit tests for StatisticsCollector — covers null-pointer early returns,
 *        caching, invalidation, and ensureLabelStats() methods.
 **/

#include <gtest/gtest.h>
#include <memory>

#include "graph/query/optimizer/StatisticsCollector.hpp"

using namespace graph::query::optimizer;

class StatisticsCollectorTest : public ::testing::Test {};

// ============================================================================
// collect() — null DataManager early-return (line 18)
// ============================================================================

TEST_F(StatisticsCollectorTest, Collect_NullDataManager_ReturnsEmpty) {
	StatisticsCollector sc(nullptr, nullptr);
	auto stats = sc.collect();
	EXPECT_EQ(stats.totalNodeCount, 0);
	EXPECT_EQ(stats.totalEdgeCount, 0);
}

// ============================================================================
// collectLabelStats() — null paths
// ============================================================================

TEST_F(StatisticsCollectorTest, CollectLabelStats_NullIndexManager_ReturnsEmpty) {
	// Line 35: early return when im_ is null
	StatisticsCollector sc(nullptr, nullptr);
	auto stats = sc.collectLabelStats("Person");
	EXPECT_EQ(stats.nodeCount, 0);
	EXPECT_EQ(stats.label, "Person");
	EXPECT_TRUE(stats.properties.empty());
}

// ============================================================================
// getCachedStatistics() + invalidate()
// ============================================================================

TEST_F(StatisticsCollectorTest, CachedStatistics_CollectsOnce) {
	StatisticsCollector sc(nullptr, nullptr);

	const auto &s1 = sc.getCachedStatistics();
	EXPECT_EQ(s1.totalNodeCount, 0);

	// Subsequent call returns same cached value (no re-collection)
	const auto &s2 = sc.getCachedStatistics();
	EXPECT_EQ(s2.totalNodeCount, 0);
}

TEST_F(StatisticsCollectorTest, Invalidate_ClearsCacheAndReCollects) {
	StatisticsCollector sc(nullptr, nullptr);

	(void)sc.getCachedStatistics(); // populate cache

	sc.invalidate();

	// After invalidation, next call re-collects
	const auto &s = sc.getCachedStatistics();
	EXPECT_EQ(s.totalNodeCount, 0);
}

// ============================================================================
// ensureLabelStats() — lazy population + idempotency
// ============================================================================

TEST_F(StatisticsCollectorTest, EnsureLabelStats_PopulatesWhenMissing) {
	StatisticsCollector sc(nullptr, nullptr); // null im → empty label stats

	Statistics stats;
	sc.ensureLabelStats(stats, {"X", "Y"});

	// Both labels should be present (even with 0 nodes)
	EXPECT_EQ(stats.labelStats.count("X"), 1u);
	EXPECT_EQ(stats.labelStats.count("Y"), 1u);
	EXPECT_EQ(stats.labelStats["X"].nodeCount, 0);
}

TEST_F(StatisticsCollectorTest, EnsureLabelStats_DoesNotOverwriteExisting) {
	StatisticsCollector sc(nullptr, nullptr);

	Statistics stats;
	// Pre-populate "X" with fake data
	LabelStatistics fake;
	fake.label = "X";
	fake.nodeCount = 999;
	stats.labelStats["X"] = fake;

	sc.ensureLabelStats(stats, {"X"});

	// Should NOT overwrite existing entry
	EXPECT_EQ(stats.labelStats["X"].nodeCount, 999);
}
