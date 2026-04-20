#include <gtest/gtest.h>

#include "graph/debug/PerfTrace.hpp"

using graph::debug::PerfTrace;

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
