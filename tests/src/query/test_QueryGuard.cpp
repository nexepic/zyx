/**
 * @file test_QueryGuard.cpp
 * @brief Unit tests for QueryGuard timeout and memory limit enforcement.
 **/

#include <gtest/gtest.h>
#include <thread>
#include "graph/query/QueryGuard.hpp"

using namespace graph::query;

TEST(QueryGuardTest, NoTimeout_CheckDoesNotThrow) {
	QueryGuard guard(0, 0); // No limits
	for (int i = 0; i < 10000; ++i) {
		EXPECT_NO_THROW(guard.check());
	}
}

TEST(QueryGuardTest, Timeout_ThrowsAfterDeadline) {
	QueryGuard guard(1, 0); // 1ms timeout
	// Spin until the guard detects timeout
	EXPECT_THROW({
		for (int i = 0; i < 10000000; ++i) {
			guard.check();
		}
	}, QueryTimeoutException);
}

TEST(QueryGuardTest, NoMemoryLimit_CheckDoesNotThrow) {
	QueryGuard guard(0, 0); // No memory limit
	for (int i = 0; i < 2000; ++i) {
		EXPECT_NO_THROW(guard.check());
	}
}

TEST(QueryGuardTest, MemoryLimit_ThrowsWhenExceeded) {
	// Set an extremely low memory limit (1 MB) — any real process exceeds this
	QueryGuard guard(0, 1);
	EXPECT_THROW({
		for (int i = 0; i < 10000; ++i) {
			guard.check();
		}
	}, QueryMemoryExceededException);
}

TEST(QueryGuardTest, CheckInterval_FastPath) {
	// Verify that checks below CHECK_MASK don't trigger slow path (timing test)
	QueryGuard guard(30000, 0); // 30s timeout — won't fire
	auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < 1000; ++i) {
		guard.check(); // All fast path (counter & 1023 != 0 for most)
	}
	auto elapsed = std::chrono::steady_clock::now() - start;
	// 1000 fast-path checks should take well under 1ms
	EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 10);
}

TEST(QueryGuardTest, BothLimits_TimeoutFirst) {
	// Both enabled, timeout triggers first (1ms timeout, generous memory)
	QueryGuard guard(1, 10000);
	EXPECT_THROW({
		for (int i = 0; i < 10000000; ++i) {
			guard.check();
		}
	}, QueryTimeoutException);
}

TEST(QueryGuardTest, BothLimits_MemoryFirst) {
	// Both enabled, memory triggers first (1MB limit, long timeout)
	QueryGuard guard(60000, 1);
	EXPECT_THROW({
		for (int i = 0; i < 10000; ++i) {
			guard.check();
		}
	}, QueryMemoryExceededException);
}

TEST(QueryGuardTest, GetCurrentRSSBytes_ReturnsNonZero) {
	size_t rss = QueryGuard::getCurrentRSSBytes();
	EXPECT_GT(rss, 0u) << "RSS should be non-zero for a running process";
}

TEST(QueryGuardTest, ExceptionMessages_ContainUsefulInfo) {
	try {
		QueryGuard guard(1, 0);
		for (int i = 0; i < 10000000; ++i) {
			guard.check();
		}
		FAIL() << "Should have thrown";
	} catch (const QueryTimeoutException &e) {
		std::string msg = e.what();
		EXPECT_NE(msg.find("1ms"), std::string::npos) << "Message should contain timeout value";
	}

	try {
		QueryGuard guard(0, 1);
		for (int i = 0; i < 10000; ++i) {
			guard.check();
		}
		FAIL() << "Should have thrown";
	} catch (const QueryMemoryExceededException &e) {
		std::string msg = e.what();
		EXPECT_NE(msg.find("1MB"), std::string::npos) << "Message should contain memory limit";
	}
}
