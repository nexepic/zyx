#include <gtest/gtest.h>
#include "graph/debug/RssActivity.hpp"
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
TEST(RssActivityTest, SeparatesGrowthFromReleaseAndIgnoresUnavailableSnapshots) {
    graph::debug::RssActivityCounters counters;
    counters.record({true, 20, 100}, {true, 30, 90});
    counters.record({true, 30, 90}, {true, 10, 140});
    counters.record({}, {true, 999, 999});
    EXPECT_EQ(counters.calls.load(), 2U);
    EXPECT_EQ(counters.live_increase.load(), 10U);
    EXPECT_EQ(counters.live_decrease.load(), 20U);
    EXPECT_EQ(counters.heap_increase.load(), 50U);
    EXPECT_EQ(counters.heap_decrease.load(), 10U);
    counters.reset();
    EXPECT_EQ(counters.calls.load(), 0U);
    EXPECT_EQ(counters.heap_increase.load(), 0U);
}
TEST(RssActivityTest, SaturatesInsteadOfWrappingLongDiagnosticRuns) {
    graph::debug::RssActivityCounters counters;
    counters.record({true, 0, 0}, {true, SIZE_MAX, SIZE_MAX});
    counters.record({true, 0, 0}, {true, 1, 1});
    EXPECT_EQ(counters.live_increase.load(), SIZE_MAX);
    EXPECT_EQ(counters.heap_increase.load(), SIZE_MAX);
}
#endif
