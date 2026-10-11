#include <gtest/gtest.h>
#include <stdexcept>
#include "query/execution/GuardedIdGather.hpp"

using graph::query::execution::detail::sortUniqueIdsGuarded;

TEST(GuardedIdGatherTest, PreservesCapacityAndSortedUniqueIds) {
	std::vector<uint64_t> ids{9U, 3U, 9U, 1U, 3U, 8U};
	const size_t capacity = ids.capacity();
	size_t checks = 0U;
	sortUniqueIdsGuarded(ids, [&] { ++checks; });
	EXPECT_EQ(ids, (std::vector<uint64_t>{1U, 3U, 8U, 9U}));
	EXPECT_EQ(ids.capacity(), capacity);
	EXPECT_GE(checks, 3U);
	ids.clear();
	sortUniqueIdsGuarded(ids, [] {});
	EXPECT_TRUE(ids.empty());
	EXPECT_EQ(ids.capacity(), capacity);
}

TEST(GuardedIdGatherTest, CancellationDuringSortAndDedupAllowsFreshGatherRetry) {
	std::vector<uint64_t> original;
	for (size_t index = 0U; index < 1024U; ++index) original.push_back((1023U - index) % 101U);
	auto baseline = original;
	size_t totalChecks = 0U;
	sortUniqueIdsGuarded(baseline, [&] { ++totalChecks; });
	ASSERT_EQ(baseline.size(), 101U);
	ASSERT_GT(totalChecks, 5U);
	// Checks at the start, in sort comparisons, and near the end of dedup.
	for (size_t rejectAt: {size_t{1U}, size_t{3U}, totalChecks - 2U}) {
		auto attempt = original;
		const size_t capacity = attempt.capacity();
		size_t checks = 0U;
		EXPECT_THROW(sortUniqueIdsGuarded(attempt, [&] {
			if (++checks == rejectAt) throw std::runtime_error("cancelled gather");
		}), std::runtime_error);
		EXPECT_EQ(checks, rejectAt);
		EXPECT_EQ(attempt.capacity(), capacity);
		EXPECT_EQ(attempt.size(), original.size());
		attempt = original; // Runtime retries rebuild its private gather from the snapshot.
		sortUniqueIdsGuarded(attempt, [] {});
		EXPECT_EQ(attempt, baseline);
	}
}
