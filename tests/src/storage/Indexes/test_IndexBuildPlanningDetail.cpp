/**
 * @file test_IndexBuildPlanningDetail.cpp
 * @brief Tests pure planning helpers used by index rebuild paths.
 */

#include "src/storage/indexes/IndexBuildPlanningDetail.hpp"

#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace detail = graph::query::indexes::index_build_detail;

TEST(IndexBuildPlanningDetailTest, AppendsOnlyUniqueNonEmptyPropertyKeys) {
	std::vector<std::string> keys{"name"};
	detail::appendUnique(keys, "");
	detail::appendUnique(keys, "name");
	detail::appendUnique(keys, "age");

	ASSERT_EQ(keys.size(), 2U);
	EXPECT_EQ(keys[0], "name");
	EXPECT_EQ(keys[1], "age");
}

TEST(IndexBuildPlanningDetailTest, AppendsMappedPhysicalKeysWhenPresent) {
	const std::unordered_map<std::string, std::vector<std::string>> physicalKeysByProperty{
			{"name", {"name", "Person.name"}},
			{"age", {"age"}},
	};
	std::vector<std::string> physicalKeys{"existing"};

	detail::appendMappedPhysicalKeys(physicalKeysByProperty, "name", physicalKeys);
	detail::appendMappedPhysicalKeys(physicalKeysByProperty, "missing", physicalKeys);

	EXPECT_EQ(physicalKeys, (std::vector<std::string>{"existing", "name", "Person.name"}));
}

TEST(IndexBuildPlanningDetailTest, DetectsCompleteScopedPropertyBuildInputs) {
	EXPECT_FALSE(detail::shouldBuildScopedNodePropertyEntries(0, ""));
	EXPECT_FALSE(detail::shouldBuildScopedNodePropertyEntries(7, ""));
	EXPECT_FALSE(detail::shouldBuildScopedNodePropertyEntries(0, "User.name"));
	EXPECT_TRUE(detail::shouldBuildScopedNodePropertyEntries(7, "User.name"));
}

TEST(IndexBuildPlanningDetailTest, DetectsWhetherOwnerScanHasWork) {
	EXPECT_FALSE(detail::hasOwnerScanWork(0, 0));
	EXPECT_FALSE(detail::hasOwnerScanWork(3, 0));
	EXPECT_FALSE(detail::hasOwnerScanWork(0, 2));
	EXPECT_TRUE(detail::hasOwnerScanWork(3, 2));
}

TEST(IndexBuildPlanningDetailTest, DetectsWhenPropertyMapFallbackIsNeeded) {
	EXPECT_FALSE(detail::needsPropertyMapFallbackScan(true));
	EXPECT_TRUE(detail::needsPropertyMapFallbackScan(false));
}

TEST(IndexBuildPlanningDetailTest, SortsAndDeduplicatesEntityIds) {
	const auto ids = detail::sortedUniqueIds({4, 1, 4, 2, 1, 3});
	EXPECT_EQ(ids, (std::vector<int64_t>{1, 2, 3, 4}));
}

TEST(IndexBuildPlanningDetailTest, AppendsUncheckpointedTailAfterCoveredRanges) {
	std::vector<std::pair<int64_t, int64_t>> ranges{{1, 3}, {9, 7}, {4, 5}};
	detail::appendUncheckpointedTailRange(ranges, 8);
	ASSERT_EQ(ranges.size(), 4U);
	EXPECT_EQ(ranges.back(), (std::pair<int64_t, int64_t>{6, 8}));

	detail::appendUncheckpointedTailRange(ranges, 0);
	detail::appendUncheckpointedTailRange(ranges, 8);
	EXPECT_EQ(ranges.size(), 4U);
}

TEST(IndexBuildPlanningDetailTest, DetectsIdsInsideInclusiveRanges) {
	const std::vector<std::pair<int64_t, int64_t>> ranges{{3, 5}, {10, 12}};
	EXPECT_FALSE(detail::rangesContainId(ranges, 2));
	EXPECT_TRUE(detail::rangesContainId(ranges, 3));
	EXPECT_TRUE(detail::rangesContainId(ranges, 11));
	EXPECT_FALSE(detail::rangesContainId(ranges, 13));
}

TEST(IndexBuildPlanningDetailTest, AppendsOnlyPositiveUnseenIds) {
	std::vector<int64_t> ids;
	std::unordered_set<int64_t> seen;
	detail::appendUniqueId(ids, seen, 0);
	detail::appendUniqueId(ids, seen, -2);
	detail::appendUniqueId(ids, seen, 7);
	detail::appendUniqueId(ids, seen, 7);
	detail::appendUniqueId(ids, seen, 8);

	EXPECT_EQ(ids, (std::vector<int64_t>{7, 8}));
}

TEST(IndexBuildPlanningDetailTest, BuildsNormalizedActiveIdRangeTasks) {
	const auto tasks = detail::buildActiveIdRangeTasks({{1, 5}, {9, 7}, {10, 12}}, 2);
	ASSERT_EQ(tasks.size(), 5U);
	EXPECT_EQ(tasks[0].startId, 1);
	EXPECT_EQ(tasks[0].endId, 2);
	EXPECT_EQ(tasks[1].startId, 3);
	EXPECT_EQ(tasks[1].endId, 4);
	EXPECT_EQ(tasks[2].startId, 5);
	EXPECT_EQ(tasks[2].endId, 5);
	EXPECT_EQ(tasks[3].startId, 10);
	EXPECT_EQ(tasks[3].endId, 11);
	EXPECT_EQ(tasks[4].startId, 12);
	EXPECT_EQ(tasks[4].endId, 12);
}

TEST(IndexBuildPlanningDetailTest, HandlesDegenerateTaskSizeAndMaxIdRange) {
	const auto singleStep = detail::buildActiveIdRangeTasks({{3, 4}}, 0);
	ASSERT_EQ(singleStep.size(), 2U);
	EXPECT_EQ(singleStep[0].startId, 3);
	EXPECT_EQ(singleStep[0].endId, 3);
	EXPECT_EQ(singleStep[1].startId, 4);
	EXPECT_EQ(singleStep[1].endId, 4);

	const int64_t maxId = std::numeric_limits<int64_t>::max();
	const auto maxRange = detail::buildActiveIdRangeTasks({{maxId, maxId}}, 128);
	ASSERT_EQ(maxRange.size(), 1U);
	EXPECT_EQ(maxRange[0].startId, maxId);
	EXPECT_EQ(maxRange[0].endId, maxId);
}
