/**
 * @file test_ThreadPool.cpp
 * @brief Focused coverage tests for ThreadPool inline branches.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <stdexcept>

#include "graph/concurrent/ThreadPool.hpp"

using graph::concurrent::ThreadPool;

TEST(ThreadPoolTest, DefaultConstructorUsesAutoThreadCountPath) {
	ThreadPool pool;
	EXPECT_GE(pool.getThreadCount(), 1UL);
}

TEST(ThreadPoolTest, SingleThreadSubmitCoversValueVoidAndExceptionPaths) {
	ThreadPool pool(1);
	EXPECT_TRUE(pool.isSingleThreaded());

	auto valueFuture = pool.submit([] { return 7; });
	EXPECT_EQ(valueFuture.get(), 7);

	int sideEffect = 0;
	auto voidFuture = pool.submit([&sideEffect] { sideEffect += 1; });
	EXPECT_NO_THROW(voidFuture.get());
	EXPECT_EQ(sideEffect, 1);

	auto exceptionFuture = pool.submit([]() -> int {
		throw std::runtime_error("boom");
	});
	EXPECT_THROW((void)exceptionFuture.get(), std::runtime_error);
}

TEST(ThreadPoolTest, ParallelForSequentialFallbackAndNoOpRange) {
	ThreadPool pool(1);
	std::atomic<size_t> hits{0};

	pool.parallelFor(0, 1, [&hits](size_t) { hits.fetch_add(1, std::memory_order_relaxed); });
	EXPECT_EQ(hits.load(std::memory_order_relaxed), 1UL);

	pool.parallelFor(5, 5, [&hits](size_t) { hits.fetch_add(1, std::memory_order_relaxed); });
	EXPECT_EQ(hits.load(std::memory_order_relaxed), 1UL);
}

TEST(ThreadPoolTest, MultiThreadSubmitAndParallelForExecuteWorkerTasks) {
	ThreadPool pool(2);
	EXPECT_FALSE(pool.isSingleThreaded());

	auto future = pool.submit([] { return 42; });
	EXPECT_EQ(future.get(), 42);

	std::atomic<size_t> hits{0};
	pool.parallelFor(0, 64, [&hits](size_t) { hits.fetch_add(1, std::memory_order_relaxed); });
	EXPECT_EQ(hits.load(std::memory_order_relaxed), 64UL);
}
