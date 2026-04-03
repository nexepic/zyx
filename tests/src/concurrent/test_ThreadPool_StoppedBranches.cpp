/**
 * @file test_ThreadPool_StoppedBranches.cpp
 * @brief Branch-focused tests for stop-state behavior in ThreadPool.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

// Pre-include STL headers used by ThreadPool.hpp to avoid macro side effects.
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <type_traits>

#define private public
#include "graph/concurrent/ThreadPool.hpp"
#undef private

using graph::concurrent::ThreadPool;

TEST(ThreadPoolStoppedBranchTest, SubmitThrowsWhenPoolHasBeenStopped) {
	ThreadPool pool(2);
	{
		std::unique_lock lock(pool.mutex_);
		pool.stop_ = true;
	}

	EXPECT_THROW((void)pool.submit([] { return 1; }), std::runtime_error);
}

TEST(ThreadPoolStoppedBranchTest, WorkerLoopDrainsQueuedTasksAfterStopSignal) {
	auto pool = std::make_unique<ThreadPool>(2);
	std::atomic<int> blockersStarted{0};
	std::atomic<bool> releaseBlockers{false};
	std::atomic<int> queuedExecuted{0};

	auto blocker = [&]() {
		blockersStarted.fetch_add(1, std::memory_order_relaxed);
		while (!releaseBlockers.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
	};

	auto f1 = pool->submit(blocker);
	auto f2 = pool->submit(blocker);

	while (blockersStarted.load(std::memory_order_relaxed) < 2) {
		std::this_thread::yield();
	}

	std::vector<std::future<void>> queued;
	queued.reserve(8);
	for (int i = 0; i < 8; ++i) {
		queued.push_back(pool->submit([&queuedExecuted]() {
			queuedExecuted.fetch_add(1, std::memory_order_relaxed);
		}));
	}

	std::thread destroyer([&pool]() {
		pool.reset();
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	releaseBlockers.store(true, std::memory_order_release);

	f1.get();
	f2.get();
	for (auto &future : queued) {
		future.get();
	}
	destroyer.join();

	EXPECT_EQ(queuedExecuted.load(std::memory_order_relaxed), 8);
}
