#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <vector>

#include "graph/debug/PerfTrace.hpp"

using graph::debug::PerfTrace;
using graph::debug::ScopedPerfTimer;

class PerfTraceTest : public ::testing::Test {
protected:
	void TearDown() override {
		PerfTrace::reset();
		PerfTrace::setEnabled(false);
	}
};

TEST_F(PerfTraceTest, DefaultDisabled) {
	EXPECT_FALSE(PerfTrace::isEnabled());
}

TEST_F(PerfTraceTest, SetEnabledToggle) {
	PerfTrace::setEnabled(true);
	EXPECT_TRUE(PerfTrace::isEnabled());
	PerfTrace::setEnabled(false);
	EXPECT_FALSE(PerfTrace::isEnabled());
}

TEST_F(PerfTraceTest, AddDurationWhenDisabled) {
	// Should not record anything when disabled
	PerfTrace::addDuration("op", 500);
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
