/**
 * @file test_ThreadPool_ExceptionAndParallel.cpp
 * @brief Additional branch coverage tests for ThreadPool.hpp.
 *        Covers: single-thread void-returning exception path,
 *        parallelFor begin > end, and multi-thread total <= 1 sequential fallback.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>

#include "graph/concurrent/ThreadPool.hpp"

using graph::concurrent::ThreadPool;

// Single-thread void submit that throws — covers the catch path for void returns.
TEST(ThreadPoolExceptionAndParallelTest, SingleThreadVoidSubmitExceptionPropagates) {
	ThreadPool pool(1);
	auto future = pool.submit([]() -> void {
		throw std::logic_error("void-boom");
	});
	EXPECT_THROW(future.get(), std::logic_error);
}

// parallelFor with begin > end — immediate return.
TEST(ThreadPoolExceptionAndParallelTest, ParallelForBeginGreaterThanEnd) {
	ThreadPool pool(2);
	std::atomic<size_t> hits{0};
	pool.parallelFor(10, 5, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 0UL);
}

// Multi-thread parallelFor with exactly 1 element — total <= 1 sequential fallback.
TEST(ThreadPoolExceptionAndParallelTest, MultiThreadParallelForTotalOneSequentialFallback) {
	ThreadPool pool(4);
	EXPECT_FALSE(pool.isSingleThreaded());
	std::atomic<size_t> hits{0};
	pool.parallelFor(7, 8, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 1UL);
}

// Multi-thread parallelFor with remainder > 0 (covers c < remainder branch in chunk loop).
TEST(ThreadPoolExceptionAndParallelTest, MultiThreadParallelForWithRemainder) {
	ThreadPool pool(3);
	std::atomic<size_t> hits{0};
	// 10 items / 3 threads = 3 chunks + 1 remainder
	pool.parallelFor(0, 10, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 10UL);
}

// Single-thread non-void submit that throws — covers the catch path for non-void returns.
TEST(ThreadPoolExceptionAndParallelTest, SingleThreadNonVoidSubmitExceptionPropagates) {
	ThreadPool pool(1);
	auto future = pool.submit([]() -> int {
		throw std::runtime_error("nonvoid-boom");
	});
	EXPECT_THROW(future.get(), std::runtime_error);
}

// Single-thread non-void submit returning a value — covers the non-void inline path.
TEST(ThreadPoolExceptionAndParallelTest, SingleThreadNonVoidSubmitReturnsValue) {
	ThreadPool pool(1);
	auto future = pool.submit([]() -> int { return 42; });
	EXPECT_EQ(future.get(), 42);
}

// Single-thread parallelFor (sequential path, threadCount_ <= 1).
TEST(ThreadPoolExceptionAndParallelTest, SingleThreadParallelForSequential) {
	ThreadPool pool(1);
	std::atomic<size_t> hits{0};
	pool.parallelFor(0, 5, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 5UL);
}

// parallelFor with begin == end — immediate return.
TEST(ThreadPoolExceptionAndParallelTest, ParallelForBeginEqualsEnd) {
	ThreadPool pool(2);
	std::atomic<size_t> hits{0};
	pool.parallelFor(5, 5, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 0UL);
}

// Verify resolveThreadCount: request 0 when hardware_concurrency would be 0 → fallback to 2.
// This is tested indirectly but let's test the isSingleThreaded / getThreadCount accessors.
TEST(ThreadPoolExceptionAndParallelTest, SingleThreadedAccessors) {
	ThreadPool pool(1);
	EXPECT_TRUE(pool.isSingleThreaded());
	EXPECT_EQ(pool.getThreadCount(), 1UL);
}

// Multi-thread submit of non-void task — covers multi-thread packaged_task path.
TEST(ThreadPoolExceptionAndParallelTest, MultiThreadNonVoidSubmit) {
	ThreadPool pool(2);
	auto future = pool.submit([]() -> int { return 99; });
	EXPECT_EQ(future.get(), 99);
}

// Multi-thread submit of void task — covers multi-thread packaged_task void path.
TEST(ThreadPoolExceptionAndParallelTest, MultiThreadVoidSubmit) {
	ThreadPool pool(2);
	std::atomic<bool> done{false};
	auto future = pool.submit([&done]() { done.store(true); });
	future.get();
	EXPECT_TRUE(done.load());
}
