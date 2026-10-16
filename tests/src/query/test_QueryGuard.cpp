/**
 * @file test_QueryGuard.cpp
 * @brief Unit tests for QueryGuard timeout and memory limit enforcement.
 **/

#include <algorithm>
#include <array>

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
	// Let the deadline expire, then cross one periodic sampling boundary.
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	EXPECT_THROW({
		for (int i = 0; i < 1024; ++i) {
			guard.check();
		}
	}, QueryTimeoutException);
}

TEST(QueryGuardTest, CheckNowObservesAnExpiredDeadlineWithoutWaitingForThePeriodicSample) {
	QueryGuard guard(1, 0);
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	// The normal hot path intentionally samples only once per fixed interval.
	EXPECT_NO_THROW(guard.check());
	EXPECT_THROW(guard.checkNow(), QueryTimeoutException);
}

TEST(QueryGuardTest, NoMemoryLimit_CheckDoesNotThrow) {
	QueryGuard guard(0, 0); // No memory limit
	for (int i = 0; i < 2000; ++i) {
		EXPECT_NO_THROW(guard.check());
	}
}

TEST(QueryGuardTest, MemoryLimit_ThrowsWhenExceeded) {
	// The policy measures only memory growth caused after this query begins. A
	// deterministic sampler avoids depending on the test process's baseline RSS.
	std::array<size_t, 2U> samples{64U * 1024U * 1024U, 66U * 1024U * 1024U};
	size_t next = 0U;
	QueryGuard guard(0, 1, [&]() { return samples[std::min(next++, samples.size() - 1U)]; });
	EXPECT_THROW(guard.checkNow(), QueryMemoryExceededException);
}

TEST(QueryGuardTest, MemoryLimit_AllowsPreexistingProcessMemoryAndCanBeDisabledByLaterQuery) {
	std::array<size_t, 4U> samples{
			256U * 1024U * 1024U,
			256U * 1024U * 1024U,
			256U * 1024U * 1024U + 512U * 1024U,
			256U * 1024U * 1024U,
	};
	size_t next = 0U;
	QueryGuard limited(0, 1, [&]() { return samples[std::min(next++, samples.size() - 1U)]; });
	EXPECT_NO_THROW(limited.checkNow());
	EXPECT_NO_THROW(limited.checkNow());
	QueryGuard disabled(0, 0, [&]() { return samples.back(); });
	EXPECT_NO_THROW(disabled.checkNow());
}

TEST(QueryGuardTest, CheckIntervalSkipsMostChecks) {
	// Verify that checks below CHECK_MASK don't trigger slow path (timing test)
	QueryGuard guard(30000, 0); // 30s timeout — won't fire
	auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < 1000; ++i) {
		guard.check(); // Most iterations skip the full interval check
	}
	auto elapsed = std::chrono::steady_clock::now() - start;
	// 1000 fast-path checks should take well under 1ms
	EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 10);
}

TEST(QueryGuardTest, BothLimits_TimeoutFirst) {
	// Both enabled, timeout triggers first (1ms timeout, generous memory)
	QueryGuard guard(1, 10000);
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	EXPECT_THROW({
		for (int i = 0; i < 1024; ++i) {
			guard.check();
		}
	}, QueryTimeoutException);
}

TEST(QueryGuardTest, BothLimits_MemoryFirst) {
	std::array<size_t, 2U> samples{64U * 1024U * 1024U, 66U * 1024U * 1024U};
	size_t next = 0U;
	QueryGuard guard(60000, 1, [&]() { return samples[std::min(next++, samples.size() - 1U)]; });
	EXPECT_THROW(guard.checkNow(), QueryMemoryExceededException);
}

TEST(QueryGuardTest, GetCurrentRSSBytes_ReturnsNonZero) {
	size_t rss = QueryGuard::getCurrentRSSBytes();
	EXPECT_GT(rss, 0u) << "RSS should be non-zero for a running process";
}

TEST(QueryGuardTest, ExceptionMessages_ContainUsefulInfo) {
	try {
		QueryGuard guard(1, 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		for (int i = 0; i < 1024; ++i) {
			guard.check();
		}
		FAIL() << "Should have thrown";
	} catch (const QueryTimeoutException &e) {
		std::string msg = e.what();
		EXPECT_NE(msg.find("1ms"), std::string::npos) << "Message should contain timeout value";
	}

	try {
		std::array<size_t, 2U> samples{64U * 1024U * 1024U, 66U * 1024U * 1024U};
		size_t next = 0U;
		QueryGuard guard(0, 1, [&]() { return samples[std::min(next++, samples.size() - 1U)]; });
		guard.checkNow();
		FAIL() << "Should have thrown";
	} catch (const QueryMemoryExceededException &e) {
		std::string msg = e.what();
		EXPECT_NE(msg.find("1MB"), std::string::npos) << "Message should contain memory limit";
	}
}

TEST(QueryGuardTest, ManagedAdmissionMessageDoesNotPresentBudgetBytesAsResidentMemory) {
    const auto error = QueryMemoryExceededException::forResourceAdmission(1);
    EXPECT_NE(std::string(error.what()).find("memory limit of 1MB"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("account/database/process budget"), std::string::npos);
    EXPECT_EQ(std::string(error.what()).find("RSS"), std::string::npos);
}
