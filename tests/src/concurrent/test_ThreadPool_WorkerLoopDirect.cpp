/**
 * @file test_ThreadPool_WorkerLoopDirect.cpp
 * @brief Direct worker-loop tests for header branch/line coverage.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <mutex>

// Pre-include STL headers used by ThreadPool.hpp to avoid macro side effects.
#include <condition_variable>
#include <functional>
#include <queue>
#include <thread>
#include <type_traits>

// The `#define private public` hack does not work on Windows because MSVC
// includes access specifiers in mangled names.
#ifndef _WIN32
#define private public
#endif
#include "graph/concurrent/ThreadPool.hpp"
#ifndef _WIN32
#undef private
#endif

using graph::concurrent::ThreadPool;

#ifndef _WIN32
TEST(ThreadPoolWorkerLoopDirectTest, ResolveThreadCountCoversFallbackAndSingleThreadNormalization) {
	EXPECT_EQ(ThreadPool::resolveThreadCount(0, 0), 2UL);
	EXPECT_EQ(ThreadPool::resolveThreadCount(0, 6), 6UL);
	EXPECT_EQ(ThreadPool::resolveThreadCount(1, 32), 1UL);
	EXPECT_EQ(ThreadPool::resolveThreadCount(4, 0), 4UL);
}

TEST(ThreadPoolWorkerLoopDirectTest, WorkerLoopExecutesQueuedTaskThenStops) {
	ThreadPool pool(1);
	std::atomic<int> executed{0};

	{
		std::unique_lock lock(pool.mutex_);
		pool.tasks_.emplace([&executed]() {
			executed.fetch_add(1, std::memory_order_relaxed);
		});
		pool.stop_ = true;
	}

	pool.workerLoop();

	EXPECT_EQ(executed.load(std::memory_order_relaxed), 1);
	EXPECT_TRUE(pool.tasks_.empty());
}
#endif

TEST(ThreadPoolWorkerLoopDirectTest, MultiThreadSubmitPathQueuesAndRunsTask) {
	ThreadPool pool(2);
	auto future = pool.submit([]() { return 123; });
	EXPECT_EQ(future.get(), 123);
}
