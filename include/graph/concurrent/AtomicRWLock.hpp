/**
 * @file AtomicRWLock.hpp
 * @date 2026/5/9
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include <atomic>
#include <cstdint>

namespace graph::concurrent {

	/**
	 * Atomic-based reader-writer lock with no thread-ownership requirement.
	 *
	 * State encoding (int32_t):
	 *   -1  = one writer holds the lock
	 *    0  = free
	 *   >0  = number of active readers
	 *
	 * Unlike std::shared_mutex, unlock can be called from a different thread
	 * than the one that acquired the lock. This is essential for Node.js addon
	 * usage where locks are acquired on the main thread and released on libuv
	 * worker threads.
	 */
	class AtomicRWLock {
	public:
		AtomicRWLock() = default;
		~AtomicRWLock() = default;

		AtomicRWLock(const AtomicRWLock &) = delete;
		AtomicRWLock &operator=(const AtomicRWLock &) = delete;
		AtomicRWLock(AtomicRWLock &&) = delete;
		AtomicRWLock &operator=(AtomicRWLock &&) = delete;

		/**
		 * Try to acquire the write lock (exclusive).
		 * Succeeds only if state is 0 (free). Sets state to -1.
		 */
		bool try_lock_write() noexcept {
			int32_t expected = 0;
			return state_.compare_exchange_strong(expected, -1, std::memory_order_acquire, std::memory_order_relaxed);
		}

		/**
		 * Release the write lock.
		 * Caller must ensure the lock is held for writing.
		 */
		void unlock_write() noexcept { state_.store(0, std::memory_order_release); }

		/**
		 * Try to acquire a read lock (shared).
		 * Succeeds if state >= 0 (no writer). Increments reader count.
		 */
		bool try_lock_read() noexcept {
			int32_t current = state_.load(std::memory_order_relaxed);
			while (current >= 0) {
				if (state_.compare_exchange_weak(current, current + 1, std::memory_order_acquire,
												 std::memory_order_relaxed)) {
					return true;
				}
				// current is updated by compare_exchange_weak on failure
			}
			return false; // Writer is active
		}

		/**
		 * Release a read lock. Decrements reader count.
		 * Caller must ensure the lock is held for reading.
		 */
		void unlock_read() noexcept { state_.fetch_sub(1, std::memory_order_release); }

	private:
		std::atomic<int32_t> state_{0};
	};

	/**
	 * Movable RAII guard for write (exclusive) lock.
	 */
	class WriteLockGuard {
	public:
		WriteLockGuard() noexcept = default;

		explicit WriteLockGuard(AtomicRWLock &lock) noexcept : lock_(&lock) {}

		~WriteLockGuard() { reset(); }

		WriteLockGuard(WriteLockGuard &&other) noexcept : lock_(other.lock_) { other.lock_ = nullptr; }

		WriteLockGuard &operator=(WriteLockGuard &&other) noexcept {
			if (this != &other) {
				reset();
				lock_ = other.lock_;
				other.lock_ = nullptr;
			}
			return *this;
		}

		WriteLockGuard(const WriteLockGuard &) = delete;
		WriteLockGuard &operator=(const WriteLockGuard &) = delete;

		void unlock() noexcept { reset(); }

		void reset() noexcept {
			if (lock_) {
				lock_->unlock_write();
				lock_ = nullptr;
			}
		}

		[[nodiscard]] bool owns_lock() const noexcept { return lock_ != nullptr; }

	private:
		AtomicRWLock *lock_ = nullptr;
	};

	/**
	 * Movable RAII guard for read (shared) lock.
	 */
	class ReadLockGuard {
	public:
		ReadLockGuard() noexcept = default;

		explicit ReadLockGuard(AtomicRWLock &lock) noexcept : lock_(&lock) {}

		~ReadLockGuard() { reset(); }

		ReadLockGuard(ReadLockGuard &&other) noexcept : lock_(other.lock_) { other.lock_ = nullptr; }

		ReadLockGuard &operator=(ReadLockGuard &&other) noexcept {
			if (this != &other) {
				reset();
				lock_ = other.lock_;
				other.lock_ = nullptr;
			}
			return *this;
		}

		ReadLockGuard(const ReadLockGuard &) = delete;
		ReadLockGuard &operator=(const ReadLockGuard &) = delete;

		void reset() noexcept {
			if (lock_) {
				lock_->unlock_read();
				lock_ = nullptr;
			}
		}

		[[nodiscard]] bool owns_lock() const noexcept { return lock_ != nullptr; }

	private:
		AtomicRWLock *lock_ = nullptr;
	};

} // namespace graph::concurrent
