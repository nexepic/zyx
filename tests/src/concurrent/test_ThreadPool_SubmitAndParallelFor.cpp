/**
 * @file test_ThreadPool_SubmitAndParallelFor.cpp
 * @brief Coverage tests targeting submit() template instantiation variants,
 *        parallelFor exception propagation, chunk division paths, and
 *        worker lifecycle edge cases.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// Expose private members for direct workerLoop/joinable branch testing.
#ifndef _WIN32
#define private public
#endif
#include "graph/concurrent/ThreadPool.hpp"
#ifndef _WIN32
#undef private
#endif

using graph::concurrent::ThreadPool;

// =============================================================================
// parallelFor exception propagation — fut.get() rethrows from worker
// =============================================================================

TEST(ThreadPoolSubmitAndParallelForTest, ParallelForExceptionFromWorkerPropagates) {
	ThreadPool pool(4);
	EXPECT_THROW(
		pool.parallelFor(0, 100, [](size_t i) {
			if (i == 50)
				throw std::runtime_error("parallel-boom");
		}),
		std::runtime_error);
}

TEST(ThreadPoolSubmitAndParallelForTest, ParallelForExceptionInFirstElement) {
	ThreadPool pool(2);
	EXPECT_THROW(
		pool.parallelFor(0, 10, [](size_t i) {
			if (i == 0)
				throw std::logic_error("first-element");
		}),
		std::logic_error);
}

TEST(ThreadPoolSubmitAndParallelForTest, ParallelForExceptionInLastElement) {
	ThreadPool pool(3);
	EXPECT_THROW(
		pool.parallelFor(0, 9, [](size_t i) {
			if (i == 8)
				throw std::runtime_error("last-element");
		}),
		std::runtime_error);
}

// =============================================================================
// submit() with multiple arguments — exercises Args&&... forwarding
// =============================================================================

TEST(ThreadPoolSubmitAndParallelForTest, MultiThreadSubmitTwoIntArgs) {
	ThreadPool pool(2);
	auto future = pool.submit([](int a, int b) { return a + b; }, 3, 7);
	EXPECT_EQ(future.get(), 10);
}

TEST(ThreadPoolSubmitAndParallelForTest, MultiThreadSubmitStringArg) {
	ThreadPool pool(2);
	auto future = pool.submit([](std::string s) { return s + "!"; }, std::string("hi"));
	EXPECT_EQ(future.get(), "hi!");
}

TEST(ThreadPoolSubmitAndParallelForTest, MultiThreadSubmitThreeArgs) {
	ThreadPool pool(2);
	auto future = pool.submit([](int a, double b, std::string c) {
		return c + std::to_string(a) + std::to_string(static_cast<int>(b));
	}, 1, 2.0, std::string("val"));
	EXPECT_EQ(future.get(), "val12");
}

TEST(ThreadPoolSubmitAndParallelForTest, SingleThreadSubmitTwoIntArgs) {
	ThreadPool pool(1);
	auto future = pool.submit([](int x, int y) { return x * y; }, 6, 7);
	EXPECT_EQ(future.get(), 42);
}

TEST(ThreadPoolSubmitAndParallelForTest, SingleThreadSubmitVoidWithArg) {
	ThreadPool pool(1);
	int result = 0;
	auto future = pool.submit([&result](int x) { result = x; }, 99);
	future.get();
	EXPECT_EQ(result, 99);
}

// =============================================================================
// submit() exception paths — multi-threaded packaged_task propagation
// =============================================================================

TEST(ThreadPoolSubmitAndParallelForTest, MultiThreadSubmitNonVoidExceptionPropagates) {
	ThreadPool pool(2);
	auto future = pool.submit([]() -> int {
		throw std::runtime_error("mt-nonvoid-boom");
	});
	EXPECT_THROW((void)future.get(), std::runtime_error);
}

TEST(ThreadPoolSubmitAndParallelForTest, MultiThreadSubmitVoidExceptionPropagates) {
	ThreadPool pool(2);
	auto future = pool.submit([]() -> void {
		throw std::logic_error("mt-void-boom");
	});
	EXPECT_THROW(future.get(), std::logic_error);
}

// =============================================================================
// parallelFor chunk division paths
// =============================================================================

TEST(ThreadPoolSubmitAndParallelForTest, ParallelForTotalLessThanThreadCount) {
	// 8 threads but only 3 items → numChunks = min(8, 3) = 3
	ThreadPool pool(8);
	std::atomic<size_t> hits{0};
	pool.parallelFor(0, 3, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 3UL);
}

TEST(ThreadPoolSubmitAndParallelForTest, ParallelForExactDivisionNoRemainder) {
	// 4 threads, 12 items → chunkSize = 3, remainder = 0
	ThreadPool pool(4);
	std::atomic<size_t> hits{0};
	pool.parallelFor(0, 12, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 12UL);
}

TEST(ThreadPoolSubmitAndParallelForTest, ParallelForWithRemainder) {
	// 4 threads, 10 items → chunkSize = 2, remainder = 2
	ThreadPool pool(4);
	std::atomic<size_t> hits{0};
	pool.parallelFor(0, 10, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 10UL);
}

TEST(ThreadPoolSubmitAndParallelForTest, ParallelForTotalEqualsThreadCount) {
	// 4 threads, 4 items → each thread gets 1 item
	ThreadPool pool(4);
	std::atomic<size_t> hits{0};
	pool.parallelFor(0, 4, [&hits](size_t) { hits.fetch_add(1); });
	EXPECT_EQ(hits.load(), 4UL);
}

TEST(ThreadPoolSubmitAndParallelForTest, ParallelForNonZeroBeginCorrectIndices) {
	ThreadPool pool(3);
	std::vector<std::atomic<bool>> visited(20);
	for (auto &v : visited) v.store(false);

	pool.parallelFor(5, 15, [&visited](size_t i) {
		visited[i].store(true);
	});

	for (size_t i = 0; i < 5; ++i) EXPECT_FALSE(visited[i].load());
	for (size_t i = 5; i < 15; ++i) EXPECT_TRUE(visited[i].load());
	for (size_t i = 15; i < 20; ++i) EXPECT_FALSE(visited[i].load());
}

TEST(ThreadPoolSubmitAndParallelForTest, ParallelForLargeRangeCorrectness) {
	ThreadPool pool(4);
	std::atomic<size_t> sum{0};
	pool.parallelFor(0, 1000, [&sum](size_t i) {
		sum.fetch_add(i, std::memory_order_relaxed);
	});
	EXPECT_EQ(sum.load(std::memory_order_relaxed), 499500UL);
}

// =============================================================================
// Worker lifecycle: submit then idle then submit again
// =============================================================================

TEST(ThreadPoolSubmitAndParallelForTest, SubmitAfterWorkersReturnToIdle) {
	ThreadPool pool(2);

	auto f1 = pool.submit([] { return 1; });
	EXPECT_EQ(f1.get(), 1);

	// Let workers go back to waiting on condition_variable
	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	auto f2 = pool.submit([] { return 2; });
	EXPECT_EQ(f2.get(), 2);

	// And again
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	auto f3 = pool.submit([] { return 3; });
	EXPECT_EQ(f3.get(), 3);
}

// =============================================================================
// Destructor joins after heavy load
// =============================================================================

TEST(ThreadPoolSubmitAndParallelForTest, DestructorJoinsAfterHeavyLoad) {
	auto pool = std::make_unique<ThreadPool>(4);
	std::atomic<int> completed{0};

	std::vector<std::future<void>> futures;
	for (int i = 0; i < 50; ++i) {
		futures.push_back(pool->submit([&completed]() {
			completed.fetch_add(1);
		}));
	}

	for (auto &f : futures) f.get();
	EXPECT_EQ(completed.load(), 50);

	pool.reset(); // destructor — all workers should be joinable and join cleanly
}

// =============================================================================
// Queue contention: many tasks flood the queue
// =============================================================================

TEST(ThreadPoolSubmitAndParallelForTest, ManySubmitsQueueContention) {
	ThreadPool pool(4);
	std::atomic<int> total{0};

	std::vector<std::future<void>> futures;
	futures.reserve(200);
	for (int i = 0; i < 200; ++i) {
		futures.push_back(pool.submit([&total]() {
			total.fetch_add(1, std::memory_order_relaxed);
		}));
	}

	for (auto &f : futures) f.get();
	EXPECT_EQ(total.load(std::memory_order_relaxed), 200);
}

// =============================================================================
// Worker joinable false branch (line 80)
// =============================================================================

#ifndef _WIN32
TEST(ThreadPoolSubmitAndParallelForTest, DestructorHandlesNonJoinableWorker) {
	// Construct a multi-thread pool, then manually move one worker out
	// to create a non-joinable (moved-from) thread in the workers_ vector.
	ThreadPool pool(2);

	// Submit a task to ensure workers are running
	auto f = pool.submit([] { return 42; });
	EXPECT_EQ(f.get(), 42);

	// Move one worker out → makes it non-joinable
	std::thread stolen = std::move(pool.workers_[0]);
	// stolen is now joinable, pool.workers_[0] is not

	// Set stop so when destructor runs, it tries to join
	{
		std::unique_lock lock(pool.mutex_);
		pool.stop_ = true;
	}
	pool.condition_.notify_all();

	// Join the stolen thread manually
	if (stolen.joinable())
		stolen.join();

	// Destructor will now encounter workers_[0].joinable() == false
	// and workers_[1].joinable() == true (or already exited).
	// This covers the `if (worker.joinable())` false branch.
}
#endif

// =============================================================================
// resolveThreadCount coverage via direct call
// =============================================================================

#ifndef _WIN32
TEST(ThreadPoolSubmitAndParallelForTest, ResolveThreadCountAllBranches) {
	// requestedThreadCount == 0, hardware > 0 → returns hardware
	EXPECT_EQ(ThreadPool::resolveThreadCount(0, 4), 4UL);
	// requestedThreadCount == 0, hardware == 0 → fallback to 2
	EXPECT_EQ(ThreadPool::resolveThreadCount(0, 0), 2UL);
	// requestedThreadCount == 1 → returns 1
	EXPECT_EQ(ThreadPool::resolveThreadCount(1, 8), 1UL);
	// requestedThreadCount > 1 → returns as-is
	EXPECT_EQ(ThreadPool::resolveThreadCount(6, 8), 6UL);
	// requestedThreadCount == 0, hardware == 1 → returns 1
	EXPECT_EQ(ThreadPool::resolveThreadCount(0, 1), 1UL);
}
#endif

// =============================================================================
// Accessors
// =============================================================================

TEST(ThreadPoolSubmitAndParallelForTest, GetThreadCountVarious) {
	{ ThreadPool p(1); EXPECT_EQ(p.getThreadCount(), 1UL); EXPECT_TRUE(p.isSingleThreaded()); }
	{ ThreadPool p(2); EXPECT_EQ(p.getThreadCount(), 2UL); EXPECT_FALSE(p.isSingleThreaded()); }
	{ ThreadPool p(8); EXPECT_EQ(p.getThreadCount(), 8UL); EXPECT_FALSE(p.isSingleThreaded()); }
}
