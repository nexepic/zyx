/**
 * @file test_RelocatableMaintenanceLoop.cpp
 * @brief Lifecycle, backpressure, and fault-containment tests for relocatable maintenance.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/maintenance/RelocatableMaintenanceLoop.hpp"

using namespace graph::storage;

namespace {

	RelocatableMaintenanceLoopPolicy testPolicy() {
		RelocatableMaintenanceLoopPolicy policy;
		policy.maximum_outstanding_tasks = 4U;
		policy.maximum_outstanding_rewrite_bytes = 4096U;
		policy.maximum_tasks_per_cycle = 2U;
		policy.maximum_rewrite_bytes_per_cycle = 2048U;
		policy.minimum_cycle_interval = std::chrono::milliseconds(1);
		return policy;
	}

	template<typename Predicate>
	bool waitUntil(Predicate predicate) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline) {
			if (predicate()) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return predicate();
	}

} // namespace

TEST(RelocatableMaintenanceLoopTest, RunsBoundedWorkAndRetriesOnlyDeferredTasks) {
	RelocatableMaintenanceLoop loop(testPolicy());
	std::atomic_uint32_t attempts{0U};
	loop.start();
	const uint64_t taskId = loop.submit(1024U, [&] {
		const uint32_t attempt = ++attempts;
		if (attempt == 1U) {
			return RelocatableMaintenanceTaskResult{RelocatableMaintenanceTaskStatus::VMTS_DEFERRED, 512U, 0U};
		}
		return RelocatableMaintenanceTaskResult{RelocatableMaintenanceTaskStatus::VMTS_COMPLETED, 256U, 128U};
	});
	EXPECT_EQ(taskId, 1U);
	ASSERT_TRUE(waitUntil([&] { return loop.metrics().completed_tasks == 1U; }));
	const RelocatableMaintenanceLoopMetrics metrics = loop.metrics();
	EXPECT_EQ(attempts.load(), 2U);
	EXPECT_EQ(metrics.deferred_attempts, 1U);
	EXPECT_EQ(metrics.rewritten_bytes, 768U);
	EXPECT_EQ(metrics.reclaimed_physical_bytes, 128U);
	EXPECT_EQ(metrics.queued_tasks, 0U);
	EXPECT_EQ(metrics.outstanding_rewrite_bytes, 0U);
	loop.stop();
	EXPECT_EQ(loop.metrics().state, RelocatableMaintenanceLoopState::VMLS_STOPPED);
}

TEST(RelocatableMaintenanceLoopTest, PausePreventsExecutionAndResumePreservesQueuedWork) {
	RelocatableMaintenanceLoop loop(testPolicy());
	std::atomic_uint32_t executions{0U};
	loop.start();
	loop.pause();
	EXPECT_EQ(loop.metrics().state, RelocatableMaintenanceLoopState::VMLS_PAUSED);
	static_cast<void>(loop.submit(1024U, [&] {
		++executions;
		return RelocatableMaintenanceTaskResult{};
	}));
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	EXPECT_EQ(executions.load(), 0U);
	loop.resume();
	ASSERT_TRUE(waitUntil([&] { return loop.metrics().completed_tasks == 1U; }));
	EXPECT_EQ(executions.load(), 1U);
	loop.stop();
}

TEST(RelocatableMaintenanceLoopTest, FaultsClosedAndReportsTheOriginalFailure) {
	std::atomic_bool failureReported{false};
	RelocatableMaintenanceLoop loop(testPolicy(),
							  [&](std::exception_ptr failure) { failureReported.store(failure != nullptr); });
	loop.start();
	static_cast<void>(loop.submit(
			1024U, []() -> RelocatableMaintenanceTaskResult { throw std::runtime_error("injected maintenance failure"); }));
	ASSERT_TRUE(waitUntil([&] { return loop.metrics().state == RelocatableMaintenanceLoopState::VMLS_FAULTED; }));
	EXPECT_TRUE(failureReported.load());
	EXPECT_EQ(loop.metrics().failed_tasks, 1U);
	EXPECT_THROW(static_cast<void>(loop.submit(1U, [] { return RelocatableMaintenanceTaskResult{}; })), std::runtime_error);
	ASSERT_NE(loop.lastFailure(), nullptr);
	try {
		std::rethrow_exception(loop.lastFailure());
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "injected maintenance failure");
	}
	loop.stop();
}

TEST(RelocatableMaintenanceLoopTest, EnforcesQueueAndCycleReservationsAndCancelsPausedTasks) {
	RelocatableMaintenanceLoopPolicy policy = testPolicy();
	policy.maximum_outstanding_tasks = 2U;
	policy.maximum_outstanding_rewrite_bytes = 2048U;
	policy.maximum_rewrite_bytes_per_cycle = 1024U;
	RelocatableMaintenanceLoop loop(policy);
	loop.start();
	loop.pause();
	EXPECT_THROW(static_cast<void>(loop.submit(1025U, [] { return RelocatableMaintenanceTaskResult{}; })), std::length_error);
	static_cast<void>(loop.submit(1024U, [] { return RelocatableMaintenanceTaskResult{}; }));
	static_cast<void>(loop.submit(1024U, [] { return RelocatableMaintenanceTaskResult{}; }));
	EXPECT_THROW(static_cast<void>(loop.submit(1U, [] { return RelocatableMaintenanceTaskResult{}; })), std::length_error);
	EXPECT_EQ(loop.cancelQueuedTasks(), 2U);
	const RelocatableMaintenanceLoopMetrics metrics = loop.metrics();
	EXPECT_EQ(metrics.cancelled_tasks, 2U);
	EXPECT_EQ(metrics.queued_tasks, 0U);
	EXPECT_EQ(metrics.outstanding_rewrite_bytes, 0U);
	loop.stop();
}

TEST(RelocatableMaintenanceLoopTest, CountsAnInFlightTaskAgainstTheOutstandingLimit) {
	RelocatableMaintenanceLoopPolicy policy = testPolicy();
	policy.maximum_outstanding_tasks = 1U;
	RelocatableMaintenanceLoop loop(policy);
	std::atomic_bool started{false};
	std::atomic_bool release{false};
	loop.start();
	static_cast<void>(loop.submit(1024U, [&] {
		started.store(true);
		while (!release.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return RelocatableMaintenanceTaskResult{};
	}));
	if (!waitUntil([&] { return started.load(); })) {
		release.store(true);
		loop.stop();
		FAIL() << "maintenance worker did not start the admitted task";
		return;
	}
	EXPECT_THROW(static_cast<void>(loop.submit(1U, [] { return RelocatableMaintenanceTaskResult{}; })), std::length_error);
	release.store(true);
	ASSERT_TRUE(waitUntil([&] { return loop.metrics().completed_tasks == 1U; }));
	loop.stop();
}

TEST(RelocatableMaintenanceLoopTest, RejectsInvalidPoliciesAndExecutors) {
	RelocatableMaintenanceLoopPolicy invalid = testPolicy();
	invalid.minimum_cycle_interval = std::chrono::milliseconds(0);
	EXPECT_THROW(static_cast<void>(RelocatableMaintenanceLoop(invalid)), std::invalid_argument);
	invalid = testPolicy();
	invalid.maximum_outstanding_tasks = 0U;
	EXPECT_THROW(static_cast<void>(RelocatableMaintenanceLoop(invalid)), std::invalid_argument);
	invalid = testPolicy();
	invalid.maximum_outstanding_rewrite_bytes = 0U;
	EXPECT_THROW(static_cast<void>(RelocatableMaintenanceLoop(invalid)), std::invalid_argument);
	invalid = testPolicy();
	invalid.maximum_tasks_per_cycle = 0U;
	EXPECT_THROW(static_cast<void>(RelocatableMaintenanceLoop(invalid)), std::invalid_argument);
	invalid = testPolicy();
	invalid.maximum_rewrite_bytes_per_cycle = 0U;
	EXPECT_THROW(static_cast<void>(RelocatableMaintenanceLoop(invalid)), std::invalid_argument);
	RelocatableMaintenanceLoop loop(testPolicy());
	EXPECT_THROW(static_cast<void>(loop.submit(1U, {})), std::invalid_argument);
	EXPECT_THROW(loop.pause(), std::logic_error);
	EXPECT_THROW(loop.resume(), std::logic_error);
}

TEST(RelocatableMaintenanceLoopTest, TreatsRepeatedLifecycleRequestsAsIdempotent) {
	RelocatableMaintenanceLoop loop(testPolicy());
	loop.start();
	loop.start();
	loop.resume();
	loop.pause();
	loop.pause();
	loop.start();
	EXPECT_EQ(loop.metrics().state, RelocatableMaintenanceLoopState::VMLS_PAUSED);
	loop.resume();
	loop.resume();
	EXPECT_EQ(loop.metrics().state, RelocatableMaintenanceLoopState::VMLS_RUNNING);
	loop.stop();
	loop.stop();
	EXPECT_EQ(loop.metrics().state, RelocatableMaintenanceLoopState::VMLS_STOPPED);
}

TEST(RelocatableMaintenanceLoopTest, SplitsTasksAcrossCyclesWhenTheCombinedReservationWouldOverflowTheCycle) {
	RelocatableMaintenanceLoop loop(testPolicy());
	std::atomic_uint32_t executions{0U};
	loop.start();
	loop.pause();
	static_cast<void>(loop.submit(1500U, [&] {
		++executions;
		return RelocatableMaintenanceTaskResult{};
	}));
	static_cast<void>(loop.submit(1500U, [&] {
		++executions;
		return RelocatableMaintenanceTaskResult{};
	}));
	loop.resume();
	ASSERT_TRUE(waitUntil([&] { return loop.metrics().completed_tasks == 2U; }));
	EXPECT_EQ(executions.load(), 2U);
	EXPECT_GE(loop.metrics().attempted_cycles, 2U);
	loop.stop();
}

TEST(RelocatableMaintenanceLoopTest, ContainsInvalidTaskResultsAndThrowingFailureHandlers) {
	std::atomic_bool failureHandlerCalled{false};
	RelocatableMaintenanceLoop loop(testPolicy(), [&](std::exception_ptr failure) {
		failureHandlerCalled.store(failure != nullptr);
		throw std::runtime_error("failure observer must not escape the maintenance worker");
	});
	loop.start();
	static_cast<void>(loop.submit(1024U, [] {
		return RelocatableMaintenanceTaskResult{static_cast<RelocatableMaintenanceTaskStatus>(99U), 0U, 0U};
	}));
	ASSERT_TRUE(waitUntil([&] { return loop.metrics().state == RelocatableMaintenanceLoopState::VMLS_FAULTED; }));
	EXPECT_TRUE(failureHandlerCalled.load());
	EXPECT_THROW(loop.start(), std::logic_error);
	loop.stop();
}

TEST(RelocatableMaintenanceLoopTest, RejectsLifecycleCallsFromTheWorkerThread) {
	RelocatableMaintenanceLoop loop(testPolicy());
	std::atomic_bool pauseRejected{false};
	std::atomic_bool stopRejected{false};
	loop.start();
	static_cast<void>(loop.submit(1024U, [&] {
		try {
			loop.pause();
		} catch (const std::logic_error &) {
			pauseRejected.store(true);
		}
		try {
			loop.stop();
		} catch (const std::logic_error &) {
			stopRejected.store(true);
		}
		return RelocatableMaintenanceTaskResult{};
	}));
	ASSERT_TRUE(waitUntil([&] { return loop.metrics().completed_tasks == 1U; }));
	EXPECT_TRUE(pauseRejected.load());
	EXPECT_TRUE(stopRejected.load());
	loop.stop();
}

TEST(RelocatableMaintenanceLoopTest, EnforcesTheRemainingQueueBoundsAndRejectsSubmissionDuringStop) {
	RelocatableMaintenanceLoopPolicy aggregateBound = testPolicy();
	aggregateBound.maximum_rewrite_bytes_per_cycle = 4096U;
	aggregateBound.maximum_outstanding_rewrite_bytes = 4096U;
	RelocatableMaintenanceLoop aggregateLoop(aggregateBound);
	aggregateLoop.start();
	aggregateLoop.pause();
	static_cast<void>(aggregateLoop.submit(1024U, [] { return RelocatableMaintenanceTaskResult{}; }));
	EXPECT_THROW(static_cast<void>(aggregateLoop.submit(3500U, [] { return RelocatableMaintenanceTaskResult{}; })),
			 std::length_error);
	aggregateLoop.stop();

	RelocatableMaintenanceLoopPolicy outstandingBound = testPolicy();
	outstandingBound.maximum_rewrite_bytes_per_cycle = 4096U;
	outstandingBound.maximum_outstanding_rewrite_bytes = 1024U;
	RelocatableMaintenanceLoop outstandingLoop(outstandingBound);
	outstandingLoop.start();
	outstandingLoop.pause();
	EXPECT_THROW(static_cast<void>(outstandingLoop.submit(1500U, [] { return RelocatableMaintenanceTaskResult{}; })),
			 std::length_error);
	outstandingLoop.stop();

	RelocatableMaintenanceLoop stoppingLoop(testPolicy());
	std::atomic_bool taskStarted{false};
	std::atomic_bool releaseTask{false};
	stoppingLoop.start();
	static_cast<void>(stoppingLoop.submit(1024U, [&] {
		taskStarted.store(true);
		while (!releaseTask.load()) {
			std::this_thread::yield();
		}
		return RelocatableMaintenanceTaskResult{};
	}));
	if (!waitUntil([&] { return taskStarted.load(); })) {
		releaseTask.store(true);
		stoppingLoop.stop();
		FAIL() << "maintenance worker did not begin the blocking task";
		return;
	}
	std::thread stopper([&] { stoppingLoop.stop(); });
	ASSERT_TRUE(waitUntil([&] { return stoppingLoop.metrics().state == RelocatableMaintenanceLoopState::VMLS_STOPPING; }));
	EXPECT_THROW(static_cast<void>(stoppingLoop.submit(1U, [] { return RelocatableMaintenanceTaskResult{}; })),
			 std::runtime_error);
	releaseTask.store(true);
	stopper.join();
}

TEST(RelocatableMaintenanceLoopTest, ContainsOversizedResultsAndSaturatesReportedReclamation) {
	RelocatableMaintenanceLoop invalidResultLoop(testPolicy());
	invalidResultLoop.start();
	static_cast<void>(invalidResultLoop.submit(1024U, [] {
		return RelocatableMaintenanceTaskResult{RelocatableMaintenanceTaskStatus::VMTS_COMPLETED, 1025U, 0U};
	}));
	ASSERT_TRUE(waitUntil(
			[&] { return invalidResultLoop.metrics().state == RelocatableMaintenanceLoopState::VMLS_FAULTED; }));
	EXPECT_NE(invalidResultLoop.lastFailure(), nullptr);
	invalidResultLoop.stop();

	RelocatableMaintenanceLoop saturationLoop(testPolicy());
	saturationLoop.start();
	static_cast<void>(saturationLoop.submit(1U, [] {
		return RelocatableMaintenanceTaskResult{RelocatableMaintenanceTaskStatus::VMTS_COMPLETED, 0U,
																	std::numeric_limits<uint64_t>::max() - 1U};
	}));
	ASSERT_TRUE(waitUntil([&] { return saturationLoop.metrics().completed_tasks == 1U; }));
	static_cast<void>(saturationLoop.submit(1U, [] {
		return RelocatableMaintenanceTaskResult{RelocatableMaintenanceTaskStatus::VMTS_COMPLETED, 0U, 2U};
	}));
	ASSERT_TRUE(waitUntil([&] { return saturationLoop.metrics().completed_tasks == 2U; }));
	EXPECT_EQ(saturationLoop.metrics().reclaimed_physical_bytes, std::numeric_limits<uint64_t>::max());
	saturationLoop.stop();
}

TEST(RelocatableMaintenanceLoopTest, ExecutesTheCycleTaskLimitBeforeStartingAnotherCycle) {
	RelocatableMaintenanceLoop loop(testPolicy());
	std::atomic_uint32_t executions{0U};
	loop.start();
	loop.pause();
	static_cast<void>(loop.submit(512U, [&] {
		++executions;
		return RelocatableMaintenanceTaskResult{};
	}));
	static_cast<void>(loop.submit(512U, [&] {
		++executions;
		return RelocatableMaintenanceTaskResult{};
	}));
	loop.resume();
	ASSERT_TRUE(waitUntil([&] { return loop.metrics().completed_tasks == 2U; }));
	EXPECT_EQ(executions.load(), 2U);
	EXPECT_EQ(loop.metrics().attempted_cycles, 1U);
	loop.stop();
}
