#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <limits>
#include <vector>

#include "graph/debug/PerfTrace.hpp"

using graph::debug::PerfTrace;
using graph::debug::ScopedPerfTimer;

class PerfTraceTest : public ::testing::Test {
protected:
	void TearDown() override {
		PerfTrace::reset();
		PerfTrace::setEnabled(false);
		PerfTrace::setValueCollectionEnabled(false);
	}
};

TEST_F(PerfTraceTest, DistinctPagePinsDeduplicateThreadsAndSeparateIoOffsetAndGeneration) {
	int first = 0, second = 0;
	PerfTrace::addPagePin(&first, 0U, 1U);
	EXPECT_TRUE(PerfTrace::snapshotAndReset().empty());
	PerfTrace::setValueCollectionEnabled(true);
	PerfTrace::addPagePin(&first, 0U, 1U);
	std::thread worker([&] { PerfTrace::addPagePin(&first, 0U, 1U); });
	worker.join();
	PerfTrace::addPagePin(&second, 0U, 1U);
	PerfTrace::addPagePin(&first, 4096U, 1U);
	PerfTrace::addPagePin(&first, 0U, 2U);
	auto snapshot = PerfTrace::snapshotAndReset();
	EXPECT_EQ(snapshot.at("native_graph.read.distinct_page_pins").totalValue, 4);
	EXPECT_EQ(snapshot.at("native_graph.read.distinct_page_pins_incomplete").totalValue, 0);
	EXPECT_TRUE(PerfTrace::snapshotAndReset().empty());
	PerfTrace::addPagePin(&first, 0U, 1U);
	PerfTrace::reset();
	EXPECT_TRUE(PerfTrace::snapshotAndReset().empty());
}

TEST_F(PerfTraceTest, DistinctPagePinsReportBoundWithoutThrowingOrCountingDuplicateAsOverflow) {
	PerfTrace::setValueCollectionEnabled(true);
	for (size_t i = 0; i < PerfTrace::MAXIMUM_DISTINCT_PAGE_PINS; ++i)
		PerfTrace::addPagePin(this, i, 1U);
	PerfTrace::addPagePin(this, 0U, 1U);
	auto exact = PerfTrace::snapshotAndReset();
	EXPECT_EQ(exact.at("native_graph.read.distinct_page_pins_incomplete").totalValue, 0);
	for (size_t i = 0; i <= PerfTrace::MAXIMUM_DISTINCT_PAGE_PINS; ++i)
		PerfTrace::addPagePin(this, i, 1U);
	auto capped = PerfTrace::snapshotAndReset();
	EXPECT_EQ(capped.at("native_graph.read.distinct_page_pins").totalValue, PerfTrace::MAXIMUM_DISTINCT_PAGE_PINS);
	EXPECT_EQ(capped.at("native_graph.read.distinct_page_pins_incomplete").totalValue, 1);
}

TEST_F(PerfTraceTest, DefaultDisabled) {
	EXPECT_FALSE(PerfTrace::isEnabled());
	EXPECT_FALSE(PerfTrace::areValuesEnabled());
}

TEST_F(PerfTraceTest, SetEnabledToggle) {
	PerfTrace::setEnabled(true);
	EXPECT_TRUE(PerfTrace::isEnabled());
	EXPECT_TRUE(PerfTrace::areValuesEnabled());
	PerfTrace::setEnabled(false);
	EXPECT_FALSE(PerfTrace::isEnabled());
	EXPECT_FALSE(PerfTrace::areValuesEnabled());
}

TEST_F(PerfTraceTest, AddDurationWhenDisabled) {
	// Should not record anything when disabled
	PerfTrace::addDuration("op", 500);
	PerfTrace::addDurationBatch("batch", 500, 1);
	PerfTrace::setEnabled(true);
	auto snapshot = PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.empty());
}

TEST_F(PerfTraceTest, AddDurationWithEmptyKey) {
	// Exercise the key.empty() == true branch while enabled
	PerfTrace::setEnabled(true);
	PerfTrace::addDuration("", 100);
	auto snapshot = PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.empty());
}

TEST_F(PerfTraceTest, AddDurationWithValidKey) {
	PerfTrace::setEnabled(true);
	PerfTrace::addDuration("query", 1000);
	PerfTrace::addDuration("query", 2000);
	PerfTrace::addDuration("commit", 500);

	auto snapshot = PerfTrace::snapshotAndReset();
	ASSERT_EQ(snapshot.size(), 2u);

	EXPECT_EQ(snapshot["query"].totalNs, 3000u);
	EXPECT_EQ(snapshot["query"].calls, 2u);
	EXPECT_EQ(snapshot["commit"].totalNs, 500u);
	EXPECT_EQ(snapshot["commit"].calls, 1u);
}

TEST_F(PerfTraceTest, AddDurationBatchRecordsTotalWithExplicitCallCount) {
	PerfTrace::setEnabled(true);
	PerfTrace::addDurationBatch("operator.task", 9000, 3);
	PerfTrace::addDurationBatch("operator.task", 4000, 2);
	PerfTrace::addDurationBatch("ignored", 1, 0);
	PerfTrace::addDurationBatch("", 1, 1);

	auto snapshot = PerfTrace::snapshotAndReset();
	ASSERT_EQ(snapshot.size(), 1u);
	EXPECT_EQ(snapshot["operator.task"].totalNs, 13000u);
	EXPECT_EQ(snapshot["operator.task"].calls, 5u);
}

TEST_F(PerfTraceTest, AddValueWithValidKey) {
	PerfTrace::setEnabled(true);
	PerfTrace::addValue("workers", 4);
	PerfTrace::addValue("workers", 2);
	PerfTrace::addValue("", 8);

	auto snapshot = PerfTrace::snapshotAndReset();
	ASSERT_EQ(snapshot.size(), 1u);
	EXPECT_EQ(snapshot["workers"].totalNs, 0u);
	EXPECT_EQ(snapshot["workers"].calls, 0u);
	EXPECT_EQ(snapshot["workers"].totalValue, 6);
	EXPECT_EQ(snapshot["workers"].valueCalls, 2u);
}

TEST_F(PerfTraceTest, ValueOnlyCollectionSkipsDurationTimers) {
	PerfTrace::setValueCollectionEnabled(true);
	EXPECT_FALSE(PerfTrace::isEnabled());
	EXPECT_TRUE(PerfTrace::areValuesEnabled());
	PerfTrace::addDuration("duration", 100);
	{
		ScopedPerfTimer timer("scoped-duration");
	}
	PerfTrace::addValue("scratch", 7);

	const auto snapshot = PerfTrace::snapshotAndReset();
	ASSERT_EQ(snapshot.size(), 1U);
	EXPECT_EQ(snapshot.at("scratch").totalValue, 7);
	EXPECT_EQ(snapshot.at("scratch").valueCalls, 1U);
	EXPECT_EQ(snapshot.at("scratch").calls, 0U);
}

TEST_F(PerfTraceTest, AddValueWhenValueCollectionDisabledIsIgnored) {
	PerfTrace::addValue("workers", 4);
	PerfTrace::setEnabled(true);
	PerfTrace::setValueCollectionEnabled(false);
	PerfTrace::addValue("workers", 2);

	EXPECT_TRUE(PerfTrace::snapshotAndReset().empty());
}

TEST_F(PerfTraceTest, ResetClearsData) {
	PerfTrace::setEnabled(true);
	PerfTrace::addDuration("op", 100);
	PerfTrace::reset();

	auto snapshot = PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.empty());
}

TEST_F(PerfTraceTest, SnapshotAndResetClearsData) {
	PerfTrace::setEnabled(true);
	PerfTrace::addDuration("op", 100);

	auto first = PerfTrace::snapshotAndReset();
	EXPECT_EQ(first.size(), 1u);

	auto second = PerfTrace::snapshotAndReset();
	EXPECT_TRUE(second.empty());
}

TEST_F(PerfTraceTest, ScopedPerfTimerRecordsWhenEnabled) {
	PerfTrace::setEnabled(true);
	{
		ScopedPerfTimer timer("scoped");
		std::this_thread::sleep_for(std::chrono::microseconds(1));
	}

	auto snapshot = PerfTrace::snapshotAndReset();
	ASSERT_EQ(snapshot.size(), 1u);
	EXPECT_EQ(snapshot["scoped"].calls, 1u);
	EXPECT_GT(snapshot["scoped"].totalNs, 0u);
}

TEST_F(PerfTraceTest, ScopedPerfTimerOwnsDynamicKey) {
	PerfTrace::setEnabled(true);
	{
		ScopedPerfTimer timer(std::string("dynamic-key"));
	}

	auto snapshot = PerfTrace::snapshotAndReset();
	ASSERT_EQ(snapshot.size(), 1u);
	EXPECT_EQ(snapshot["dynamic-key"].calls, 1u);
}

TEST_F(PerfTraceTest, ScopedPerfTimerSkipsWhenDisabledOrEmpty) {
	{
		ScopedPerfTimer disabled("disabled");
	}

	PerfTrace::setEnabled(true);
	{
		ScopedPerfTimer empty("");
	}

	auto snapshot = PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.empty());
}

TEST_F(PerfTraceTest, ConcurrentAddsAreMergedAcrossWorkerShards) {
	PerfTrace::setEnabled(true);

	std::vector<std::thread> workers;
	for (int worker = 0; worker < 8; ++worker) {
		workers.emplace_back([] {
			for (int iteration = 0; iteration < 100; ++iteration) {
				PerfTrace::addDuration("parallel-op", 10);
				PerfTrace::addValue("parallel-workers", 1);
			}
		});
	}
	for (auto &worker: workers) {
		worker.join();
	}

	auto snapshot = PerfTrace::snapshotAndReset();
	ASSERT_EQ(snapshot.size(), 2u);
	EXPECT_EQ(snapshot["parallel-op"].calls, 800u);
	EXPECT_EQ(snapshot["parallel-op"].totalNs, 8000u);
	EXPECT_EQ(snapshot["parallel-workers"].valueCalls, 800u);
	EXPECT_EQ(snapshot["parallel-workers"].totalValue, 800);
	EXPECT_TRUE(PerfTrace::snapshotAndReset().empty());
}

TEST_F(PerfTraceTest, SaturatesDurationsAndBatchCountsInsteadOfWrapping) {
	PerfTrace::setEnabled(true);
	const auto maximum = std::numeric_limits<uint64_t>::max();
	PerfTrace::addDurationBatch("batch", maximum - 1U, maximum - 1U);
	PerfTrace::addDurationBatch("batch", 1U, 1U);
	PerfTrace::addDurationBatch("batch", 1U, 1U);
	PerfTrace::addDuration("batch", maximum);
	const auto snapshot = PerfTrace::snapshotAndReset();
	EXPECT_EQ(snapshot.at("batch").totalNs, maximum);
	EXPECT_EQ(snapshot.at("batch").calls, maximum);
	EXPECT_TRUE(PerfTrace::snapshotAndReset().empty());
}

TEST_F(PerfTraceTest, SaturatesSignedValuesAtBothBoundsAndAllowsOpposingUpdates) {
	PerfTrace::setEnabled(true);
	const auto maximum = std::numeric_limits<int64_t>::max();
	const auto minimum = std::numeric_limits<int64_t>::min();
	PerfTrace::addValue("positive", maximum);
	PerfTrace::addValue("positive", 1);
	PerfTrace::addValue("negative", minimum);
	PerfTrace::addValue("negative", -1);
	const auto saturated = PerfTrace::snapshotAndReset();
	EXPECT_EQ(saturated.at("positive").totalValue, maximum);
	EXPECT_EQ(saturated.at("negative").totalValue, minimum);
	PerfTrace::addValue("mixed", maximum);
	PerfTrace::addValue("mixed", minimum);
	PerfTrace::addValue("mixed", 1);
	EXPECT_EQ(PerfTrace::snapshotAndReset().at("mixed").totalValue, 0);
	PerfTrace::addValue("recover", minimum);
	PerfTrace::addValue("recover", -1);
	PerfTrace::addValue("recover", 1);
	EXPECT_EQ(PerfTrace::snapshotAndReset().at("recover").totalValue, minimum + 1);
}

TEST_F(PerfTraceTest, SaturatesWorkerShardAggregation) {
	PerfTrace::setEnabled(true);
	std::vector<std::thread> workers;
	for (size_t worker = 0U; worker < 8U; ++worker) {
		workers.emplace_back([] {
			PerfTrace::addDurationBatch("duration", std::numeric_limits<uint64_t>::max(),
					std::numeric_limits<uint64_t>::max());
			PerfTrace::addValue("positive", std::numeric_limits<int64_t>::max());
			PerfTrace::addValue("negative", std::numeric_limits<int64_t>::min());
		});
	}
	for (auto &worker: workers) worker.join();
	const auto snapshot = PerfTrace::snapshotAndReset();
	EXPECT_EQ(snapshot.at("duration").totalNs, std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(snapshot.at("duration").calls, std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(snapshot.at("positive").totalValue, std::numeric_limits<int64_t>::max());
	EXPECT_EQ(snapshot.at("negative").totalValue, std::numeric_limits<int64_t>::min());
	EXPECT_EQ(snapshot.at("positive").valueCalls, 8U);
}

TEST_F(PerfTraceTest, MaximumValuePreservesNegativeValuesMergesThreadsAndResets) {
 PerfTrace::setEnabled(true);
 PerfTrace::setValueCollectionEnabled(true);
 PerfTrace::addValue("negative", -9);
 PerfTrace::addValue("negative", -3);
 PerfTrace::addDuration("negative", 5);
 std::vector<std::thread> workers;
 for (int value : {-7, -1, -5}) {
  workers.emplace_back([value] { PerfTrace::addValue("negative", value); });
 }
 for (auto &worker : workers) worker.join();
 auto snapshot = PerfTrace::snapshotAndReset();
 ASSERT_EQ(snapshot.at("negative").valueCalls, 5U);
 EXPECT_EQ(snapshot.at("negative").maximumValue, -1);
 EXPECT_EQ(snapshot.at("negative").totalValue, -25);
 EXPECT_EQ(snapshot.at("negative").calls, 1U);
 EXPECT_TRUE(PerfTrace::snapshotAndReset().empty());
 PerfTrace::addValue("negative", -42);
 EXPECT_EQ(PerfTrace::snapshotAndReset().at("negative").maximumValue, -42);
 PerfTrace::addValue("extreme", std::numeric_limits<int64_t>::min());
 PerfTrace::addValue("extreme", std::numeric_limits<int64_t>::max());
 EXPECT_EQ(PerfTrace::snapshotAndReset().at("extreme").maximumValue,
           std::numeric_limits<int64_t>::max());
}
