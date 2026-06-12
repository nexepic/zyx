/**
 * @file ThreadPool.hpp
 * @author Nexepic
 * @date 2026/3/22
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

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "graph/concurrent/ParallelAdaptivePolicy.hpp"

namespace graph::concurrent {

	/**
	 * @class ThreadPool
	 * @brief A lightweight, general-purpose thread pool for parallelizing work.
	 *
	 * Configuration:
	 *   - threadCount = 0: auto-detect via std::thread::hardware_concurrency()
	 *   - threadCount = 1: single-threaded mode (no worker threads, tasks execute inline)
	 *   - threadCount > 1: multi-threaded mode with N worker threads
	 *
	 * When threadCount == 1, submit() executes the task synchronously on the calling
	 * thread and parallelFor() runs sequentially. This ensures zero overhead when
	 * concurrency is disabled.
	 */
	class ThreadPool {
	public:
		/**
		 * @brief Construct a thread pool.
		 * @param threadCount Number of threads. 0 = auto-detect, 1 = single-threaded (inline).
		 */
		explicit ThreadPool(size_t threadCount = 0) {
#ifdef __EMSCRIPTEN__
			threadCount = 1; // WASM: single-threaded mode only
#endif
			hardwareInfo_ = resolveThreadPoolHardwareInfo(threadCount, std::thread::hardware_concurrency());
			threadCount_ = hardwareInfo_.resolvedThreadCount;

			if (threadCount_ <= 1) {
				// Single-threaded mode: no worker threads needed
				return;
			}

			// Multi-threaded mode: launch worker threads
			workers_.reserve(threadCount_);
			for (size_t i = 0; i < threadCount_; ++i) {
				workers_.emplace_back([this] { workerLoop(); });
			}
		}

		~ThreadPool() {
			{
				std::unique_lock lock(mutex_);
				stop_ = true;
			}
			condition_.notify_all();
			for (auto &worker: workers_) {
				if (worker.joinable())
					worker.join();
			}
		}

		// Non-copyable, non-movable
		ThreadPool(const ThreadPool &) = delete;
		ThreadPool &operator=(const ThreadPool &) = delete;
		ThreadPool(ThreadPool &&) = delete;
		ThreadPool &operator=(ThreadPool &&) = delete;

		/**
		 * @brief Submit a callable task and get a future for the result.
		 *
		 * In single-threaded mode (threadCount == 1), the task is executed
		 * immediately on the calling thread.
		 */
		template<typename F, typename... Args>
		auto submit(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
			using ReturnType = std::invoke_result_t<F, Args...>;

			if (threadCount_ <= 1) {
				// Single-threaded: execute inline
				std::promise<ReturnType> promise;
				auto future = promise.get_future();
				try {
					if constexpr (std::is_void_v<ReturnType>) {
						std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
						promise.set_value();
					} else {
						promise.set_value(std::invoke(std::forward<F>(f), std::forward<Args>(args)...));
					}
				} catch (...) {
					promise.set_exception(std::current_exception());
				}
				return future;
			}

			auto task = std::make_shared<std::packaged_task<ReturnType()>>(
					std::bind(std::forward<F>(f), std::forward<Args>(args)...));

			auto future = task->get_future();
			enqueue([task]() { (*task)(); });
			return future;
		}

		/**
		 * @brief Execute a function in parallel over a range [begin, end).
		 *
		 * Divides the range into chunks and distributes across threads.
		 * In single-threaded mode, executes sequentially with no overhead.
		 *
		 * @param begin Start index (inclusive)
		 * @param end End index (exclusive)
		 * @param func Function taking (size_t index) called for each element
		 */
		template<typename Func>
		void parallelFor(size_t begin, size_t end, Func &&func) {
			parallelForWithMaxWorkers(begin, end, threadCount_, std::forward<Func>(func));
		}

		/**
		 * @brief Execute a function over a range using at most maxWorkers chunks.
		 *
		 * This is useful for memory-bandwidth-bound scans where the database can
		 * benefit from parallelism but should not blindly use every worker thread.
		 */
		template<typename Func>
		void parallelFor(size_t begin, size_t end, size_t maxWorkers, Func &&func) {
			parallelForWithMaxWorkers(begin, end, maxWorkers, std::forward<Func>(func));
		}

		/**
		 * @brief Returns the configured thread count.
		 */
		[[nodiscard]] size_t getThreadCount() const { return threadCount_; }

		/**
		 * @brief Returns true if the pool is in single-threaded (inline) mode.
		 */
		[[nodiscard]] bool isSingleThreaded() const { return threadCount_ <= 1; }

		/**
		 * @brief Returns portable hardware/thread-pool resolution metadata.
		 */
		[[nodiscard]] const ThreadPoolHardwareInfo &getHardwareInfo() const { return hardwareInfo_; }

		[[nodiscard]] AdaptiveParallelPolicyState &adaptivePolicyState() { return adaptivePolicyState_; }

		[[nodiscard]] const AdaptiveParallelPolicyState &adaptivePolicyState() const { return adaptivePolicyState_; }

		void setParallelPolicyMode(ParallelPolicyMode mode) { adaptivePolicyState_.setMode(mode); }

		[[nodiscard]] ParallelPolicyMode getParallelPolicyMode() const { return adaptivePolicyState_.mode(); }

		void resetAdaptiveParallelPolicy() { adaptivePolicyState_.reset(); }

	private:
		template<typename Func>
		void parallelForWithMaxWorkers(size_t begin, size_t end, size_t maxWorkers, Func &&func) {
			if (begin >= end)
				return;

			size_t total = end - begin;
			const size_t effectiveThreadCount = std::min(threadCount_, std::max<size_t>(1, maxWorkers));

			if (effectiveThreadCount <= 1 || total <= 1) {
				// Sequential execution
				for (size_t i = begin; i < end; ++i)
					func(i);
				return;
			}

			// Determine number of chunks
			size_t numChunks = std::min(effectiveThreadCount, total);
			size_t chunkSize = total / numChunks;
			size_t remainder = total % numChunks;

			std::mutex completionMutex;
			std::condition_variable completionCv;
			size_t remainingWorkerChunks = numChunks - 1;
			std::exception_ptr firstException;

			auto rememberException = [&](std::exception_ptr exception) {
				std::lock_guard lock(completionMutex);
				if (!firstException) {
					firstException = std::move(exception);
				}
			};

			auto completeWorkerChunk = [&]() {
				std::lock_guard lock(completionMutex);
				if (--remainingWorkerChunks == 0) {
					completionCv.notify_one();
				}
			};

			auto runChunk = [&](size_t chunkBegin, size_t chunkEnd) {
				try {
					for (size_t i = chunkBegin; i < chunkEnd; ++i) {
						func(i);
					}
				} catch (...) {
					rememberException(std::current_exception());
				}
			};

			size_t start = begin;
			size_t inlineBegin = begin;
			size_t inlineEnd = begin;
			for (size_t c = 0; c < numChunks; ++c) {
				const size_t thisChunk = chunkSize + static_cast<size_t>(c < remainder);
				const size_t chunkEnd = start + thisChunk;
				if (c + 1 == numChunks) {
					inlineBegin = start;
					inlineEnd = chunkEnd;
				} else {
					try {
						enqueue([start, chunkEnd, &runChunk, &completeWorkerChunk]() {
							runChunk(start, chunkEnd);
							completeWorkerChunk();
						});
					} catch (...) {
						rememberException(std::current_exception());
						completeWorkerChunk();
					}
				}
				start = chunkEnd;
			}

			// Let the caller contribute one chunk instead of idling while workers run.
			runChunk(inlineBegin, inlineEnd);

			{
				std::unique_lock lock(completionMutex);
				completionCv.wait(lock, [&] { return remainingWorkerChunks == 0; });
				if (firstException) {
					std::rethrow_exception(firstException);
				}
			}
		}

		void enqueue(std::function<void()> task) {
			{
				std::unique_lock lock(mutex_);
				if (stop_)
					throw std::runtime_error("Cannot submit to stopped ThreadPool");
				tasks_.emplace(std::move(task));
			}
			condition_.notify_one();
		}

		static size_t resolveThreadCount(size_t requestedThreadCount, unsigned int hardwareThreadCount) {
			return resolveThreadPoolHardwareInfo(requestedThreadCount, hardwareThreadCount).resolvedThreadCount;
		}

		static ThreadPoolHardwareInfo resolveThreadPoolHardwareInfo(size_t requestedThreadCount,
																	 unsigned int hardwareThreadCount) {
			ThreadPoolHardwareInfo info{.requestedThreadCount = requestedThreadCount,
										.hardwareThreadCount = hardwareThreadCount,
										.resolvedThreadCount = 1,
										.source = ThreadPoolSizeSource::TPSS_MANUAL};
			if (requestedThreadCount == 0) {
				requestedThreadCount = static_cast<size_t>(hardwareThreadCount);
				if (requestedThreadCount == 0) {
					info.resolvedThreadCount = 2; // Fallback if hardware_concurrency returns 0
					info.source = ThreadPoolSizeSource::TPSS_AUTO_FALLBACK;
					return info;
				}
				info.resolvedThreadCount = requestedThreadCount;
				info.source = ThreadPoolSizeSource::TPSS_AUTO_DETECTED;
				return info;
			}
			if (requestedThreadCount <= 1) {
				info.resolvedThreadCount = 1;
				info.source = ThreadPoolSizeSource::TPSS_FORCED_SINGLE_THREADED;
				return info;
			}
			info.resolvedThreadCount = requestedThreadCount;
			info.source = ThreadPoolSizeSource::TPSS_MANUAL;
			return info;
		}

		void workerLoop() {
			for (;;) {
				std::function<void()> task;
				std::unique_lock lock(mutex_);
				condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
				if (stop_ && tasks_.empty())
					return;
				task = std::move(tasks_.front());
				tasks_.pop();
				lock.unlock();
				task();
			}
		}

		size_t threadCount_ = 1;
		ThreadPoolHardwareInfo hardwareInfo_{};
		AdaptiveParallelPolicyState adaptivePolicyState_;
		std::vector<std::thread> workers_;
		std::queue<std::function<void()>> tasks_;
		std::mutex mutex_;
		std::condition_variable condition_;
		bool stop_ = false;
	};

} // namespace graph::concurrent
