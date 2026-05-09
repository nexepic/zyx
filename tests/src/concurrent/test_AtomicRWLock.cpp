/**
 * @file test_AtomicRWLock.cpp
 * @brief Tests for AtomicRWLock: basic lock/unlock, multi-reader concurrency,
 *        and cross-thread unlock regression test.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "graph/concurrent/AtomicRWLock.hpp"

using graph::concurrent::AtomicRWLock;
using graph::concurrent::ReadLockGuard;
using graph::concurrent::WriteLockGuard;

// =============================================================================
// Basic write lock tests
// =============================================================================

TEST(AtomicRWLockTest, WriteLockBasic) {
	AtomicRWLock lock;
	EXPECT_TRUE(lock.try_lock_write());
	// Cannot acquire again while held
	EXPECT_FALSE(lock.try_lock_write());
	EXPECT_FALSE(lock.try_lock_read());
	lock.unlock_write();
	// Can acquire again after release
	EXPECT_TRUE(lock.try_lock_write());
	lock.unlock_write();
}

TEST(AtomicRWLockTest, ReadLockBasic) {
	AtomicRWLock lock;
	EXPECT_TRUE(lock.try_lock_read());
	// Multiple readers allowed
	EXPECT_TRUE(lock.try_lock_read());
	// Writer blocked while readers hold
	EXPECT_FALSE(lock.try_lock_write());
	lock.unlock_read();
	lock.unlock_read();
	// Writer can acquire after all readers release
	EXPECT_TRUE(lock.try_lock_write());
	lock.unlock_write();
}

// =============================================================================
// WriteLockGuard tests
// =============================================================================

TEST(AtomicRWLockTest, WriteLockGuardRAII) {
	AtomicRWLock lock;
	{
		EXPECT_TRUE(lock.try_lock_write());
		WriteLockGuard guard(lock);
		// Lock is held by guard (we already locked it, guard just takes ownership)
		EXPECT_FALSE(lock.try_lock_read());
	}
	// Guard released the lock
	EXPECT_TRUE(lock.try_lock_read());
	lock.unlock_read();
}

TEST(AtomicRWLockTest, WriteLockGuardMove) {
	AtomicRWLock lock;
	lock.try_lock_write();
	WriteLockGuard guard1(lock);
	WriteLockGuard guard2(std::move(guard1));
	EXPECT_FALSE(guard1.owns_lock()); // NOLINT(bugprone-use-after-move)
	EXPECT_TRUE(guard2.owns_lock());
	guard2.reset();
	EXPECT_TRUE(lock.try_lock_write());
	lock.unlock_write();
}

TEST(AtomicRWLockTest, WriteLockGuardUnlock) {
	AtomicRWLock lock;
	lock.try_lock_write();
	WriteLockGuard guard(lock);
	guard.unlock();
	EXPECT_FALSE(guard.owns_lock());
	EXPECT_TRUE(lock.try_lock_read());
	lock.unlock_read();
}

// =============================================================================
// ReadLockGuard tests
// =============================================================================

TEST(AtomicRWLockTest, ReadLockGuardRAII) {
	AtomicRWLock lock;
	{
		lock.try_lock_read();
		ReadLockGuard guard(lock);
		EXPECT_FALSE(lock.try_lock_write());
	}
	EXPECT_TRUE(lock.try_lock_write());
	lock.unlock_write();
}

TEST(AtomicRWLockTest, ReadLockGuardMove) {
	AtomicRWLock lock;
	lock.try_lock_read();
	ReadLockGuard guard1(lock);
	ReadLockGuard guard2(std::move(guard1));
	EXPECT_FALSE(guard1.owns_lock()); // NOLINT(bugprone-use-after-move)
	EXPECT_TRUE(guard2.owns_lock());
	guard2.reset();
	EXPECT_TRUE(lock.try_lock_write());
	lock.unlock_write();
}

// =============================================================================
// Multi-reader concurrency test
// =============================================================================

TEST(AtomicRWLockTest, MultiReaderConcurrency) {
	AtomicRWLock lock;
	constexpr int kReaders = 8;
	std::atomic<int> readersInside{0};
	std::atomic<bool> allReadersStarted{false};
	std::vector<std::thread> threads;

	for (int i = 0; i < kReaders; ++i) {
		threads.emplace_back([&]() {
			while (!lock.try_lock_read()) {
				std::this_thread::yield();
			}
			readersInside.fetch_add(1);
			// Spin until all readers have entered
			while (readersInside.load() < kReaders) {
				std::this_thread::yield();
			}
			allReadersStarted.store(true);
			lock.unlock_read();
		});
	}

	for (auto &t : threads) {
		t.join();
	}
	EXPECT_TRUE(allReadersStarted.load());
	// All readers finished, writer should succeed
	EXPECT_TRUE(lock.try_lock_write());
	lock.unlock_write();
}

// =============================================================================
// Cross-thread unlock regression test (the core bug this fixes)
// =============================================================================

TEST(AtomicRWLockTest, CrossThreadWriteUnlock) {
	AtomicRWLock lock;
	std::atomic<bool> locked{false};
	std::atomic<bool> unlocked{false};

	// Thread A acquires the write lock
	std::thread threadA([&]() {
		while (!lock.try_lock_write()) {
			std::this_thread::yield();
		}
		locked.store(true);
		// Hold the lock — Thread B will release it
		while (!unlocked.load()) {
			std::this_thread::yield();
		}
	});

	// Wait for Thread A to acquire
	while (!locked.load()) {
		std::this_thread::yield();
	}

	// Thread B releases the write lock (cross-thread unlock)
	std::thread threadB([&]() {
		lock.unlock_write();
		unlocked.store(true);
	});

	threadA.join();
	threadB.join();

	// Lock should be free after cross-thread unlock
	EXPECT_TRUE(lock.try_lock_write());
	lock.unlock_write();
}

TEST(AtomicRWLockTest, CrossThreadReadUnlock) {
	AtomicRWLock lock;
	std::atomic<bool> locked{false};
	std::atomic<bool> unlocked{false};

	// Thread A acquires a read lock
	std::thread threadA([&]() {
		while (!lock.try_lock_read()) {
			std::this_thread::yield();
		}
		locked.store(true);
		while (!unlocked.load()) {
			std::this_thread::yield();
		}
	});

	while (!locked.load()) {
		std::this_thread::yield();
	}

	// Thread B releases the read lock (cross-thread unlock)
	std::thread threadB([&]() {
		lock.unlock_read();
		unlocked.store(true);
	});

	threadA.join();
	threadB.join();

	// Lock should be free — writer can acquire
	EXPECT_TRUE(lock.try_lock_write());
	lock.unlock_write();
}

// =============================================================================
// Default-constructed guard tests
// =============================================================================

TEST(AtomicRWLockTest, DefaultGuardsNoOp) {
	WriteLockGuard wguard;
	EXPECT_FALSE(wguard.owns_lock());
	wguard.reset(); // Should not crash

	ReadLockGuard rguard;
	EXPECT_FALSE(rguard.owns_lock());
	rguard.reset(); // Should not crash
}

TEST(AtomicRWLockTest, WriteLockGuardMoveAssignment) {
	AtomicRWLock lock;
	lock.try_lock_write();
	WriteLockGuard guard1(lock);
	WriteLockGuard guard2;
	guard2 = std::move(guard1);
	EXPECT_FALSE(guard1.owns_lock()); // NOLINT(bugprone-use-after-move)
	EXPECT_TRUE(guard2.owns_lock());
	guard2.reset();
	EXPECT_TRUE(lock.try_lock_write());
	lock.unlock_write();
}

TEST(AtomicRWLockTest, ReadLockGuardMoveAssignment) {
	AtomicRWLock lock;
	lock.try_lock_read();
	ReadLockGuard guard1(lock);
	ReadLockGuard guard2;
	guard2 = std::move(guard1);
	EXPECT_FALSE(guard1.owns_lock()); // NOLINT(bugprone-use-after-move)
	EXPECT_TRUE(guard2.owns_lock());
	guard2.reset();
	EXPECT_TRUE(lock.try_lock_write());
	lock.unlock_write();
}

// =============================================================================
// Self-assignment coverage (branch: this == &other)
// =============================================================================

TEST(AtomicRWLockTest, WriteLockGuardSelfAssignment) {
	AtomicRWLock lock;
	lock.try_lock_write();
	WriteLockGuard guard(lock);
	EXPECT_TRUE(guard.owns_lock());
	// Self move-assignment should be a no-op; lock remains held
	auto *ptr = &guard;
	guard = std::move(*ptr); // NOLINT(bugprone-use-after-move,clang-diagnostic-self-move)
	EXPECT_TRUE(guard.owns_lock());
	// Lock is still held by guard
	EXPECT_FALSE(lock.try_lock_read());
	guard.reset();
	EXPECT_TRUE(lock.try_lock_read());
	lock.unlock_read();
}

TEST(AtomicRWLockTest, ReadLockGuardSelfAssignment) {
	AtomicRWLock lock;
	lock.try_lock_read();
	ReadLockGuard guard(lock);
	EXPECT_TRUE(guard.owns_lock());
	// Self move-assignment should be a no-op; lock remains held
	auto *ptr = &guard;
	guard = std::move(*ptr); // NOLINT(bugprone-use-after-move,clang-diagnostic-self-move)
	EXPECT_TRUE(guard.owns_lock());
	// Lock is still held by guard
	EXPECT_FALSE(lock.try_lock_write());
	guard.reset();
	EXPECT_TRUE(lock.try_lock_write());
	lock.unlock_write();
}

// =============================================================================
// CAS retry coverage: force compare_exchange_weak failure in try_lock_read
// =============================================================================

TEST(AtomicRWLockTest, ReadLockCASRetryUnderContention) {
	AtomicRWLock lock;
	constexpr int kThreads = 16;
	constexpr int kIterations = 1000;
	std::atomic<int> totalAcquires{0};

	// Many threads rapidly acquire/release read locks to cause CAS contention
	std::vector<std::thread> threads;
	for (int i = 0; i < kThreads; ++i) {
		threads.emplace_back([&]() {
			for (int j = 0; j < kIterations; ++j) {
				while (!lock.try_lock_read()) {
					std::this_thread::yield();
				}
				totalAcquires.fetch_add(1, std::memory_order_relaxed);
				lock.unlock_read();
			}
		});
	}

	for (auto &t : threads) {
		t.join();
	}
	EXPECT_EQ(totalAcquires.load(), kThreads * kIterations);
	// Lock should be free
	EXPECT_TRUE(lock.try_lock_write());
	lock.unlock_write();
}
