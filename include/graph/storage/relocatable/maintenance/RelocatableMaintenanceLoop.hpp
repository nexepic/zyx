/**
 * @file RelocatableMaintenanceLoop.hpp
 * @brief Bounded lifecycle and admission control for relocatable maintenance work.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace graph::storage {

	struct RelocatableMaintenanceLoopPolicy {
		size_t maximum_outstanding_tasks = 256U;
		uint64_t maximum_outstanding_rewrite_bytes = 32U * 1024U * 1024U;
		uint32_t maximum_tasks_per_cycle = 1U;
		uint64_t maximum_rewrite_bytes_per_cycle = 128U * 1024U;
		std::chrono::milliseconds minimum_cycle_interval{100};

		constexpr bool operator==(const RelocatableMaintenanceLoopPolicy &) const = default;
	};

	// Prefixes prevent collisions with Windows platform macros.
	enum class RelocatableMaintenanceLoopState : uint8_t {
		VMLS_STOPPED = 1,
		VMLS_RUNNING = 2,
		VMLS_PAUSED = 3,
		VMLS_FAULTED = 4,
		VMLS_STOPPING = 5,
	};

	enum class RelocatableMaintenanceTaskStatus : uint8_t {
		VMTS_COMPLETED = 1,
		VMTS_DEFERRED = 2,
	};

	struct RelocatableMaintenanceTaskResult {
		RelocatableMaintenanceTaskStatus status = RelocatableMaintenanceTaskStatus::VMTS_COMPLETED;
		uint64_t rewritten_bytes = 0;
		uint64_t reclaimed_physical_bytes = 0;

		constexpr bool operator==(const RelocatableMaintenanceTaskResult &) const = default;
	};

	using RelocatableMaintenanceTaskExecutor = std::function<RelocatableMaintenanceTaskResult()>;
	using RelocatableMaintenanceFailureHandler = std::function<void(std::exception_ptr)>;

	struct RelocatableMaintenanceLoopMetrics {
		RelocatableMaintenanceLoopState state = RelocatableMaintenanceLoopState::VMLS_STOPPED;
		uint64_t submitted_tasks = 0;
		uint64_t completed_tasks = 0;
		uint64_t deferred_attempts = 0;
		uint64_t failed_tasks = 0;
		uint64_t cancelled_tasks = 0;
		uint64_t attempted_cycles = 0;
		uint64_t rewritten_bytes = 0;
		uint64_t reclaimed_physical_bytes = 0;
		uint64_t outstanding_rewrite_bytes = 0;
		size_t queued_tasks = 0;
		size_t in_flight_tasks = 0;
		uint64_t last_submitted_task_id = 0;

		constexpr bool operator==(const RelocatableMaintenanceLoopMetrics &) const = default;
	};

	// The loop owns only control-plane closures created by typed storage adapters.
	// It never parses persistent payloads or invents compaction candidates.
	class RelocatableMaintenanceLoop final {
	public:
		explicit RelocatableMaintenanceLoop(RelocatableMaintenanceLoopPolicy policy,
									  RelocatableMaintenanceFailureHandler failureHandler = {});
		~RelocatableMaintenanceLoop();
		RelocatableMaintenanceLoop(const RelocatableMaintenanceLoop &) = delete;
		RelocatableMaintenanceLoop &operator=(const RelocatableMaintenanceLoop &) = delete;

		void start();
		// Pause waits for the current task to leave the execution boundary. Queued
		// work remains bounded and can be inspected or cancelled by the operator.
		void pause();
		void resume();
		void stop();
		[[nodiscard]] size_t cancelQueuedTasks();

		// `deduplicationKey == 0` disables deduplication. A nonzero key may have
		// only one queued or in-flight task at a time; duplicate admission returns
		// zero without consuming queue or rewrite budget.
		[[nodiscard]] uint64_t submit(uint64_t maximumRewriteBytes, RelocatableMaintenanceTaskExecutor executor,
									 uint64_t deduplicationKey = 0U);
		[[nodiscard]] RelocatableMaintenanceLoopMetrics metrics() const;
		[[nodiscard]] std::exception_ptr lastFailure() const;

	private:
		struct QueuedTask {
			uint64_t id = 0;
			uint64_t maximum_rewrite_bytes = 0;
			uint64_t deduplication_key = 0;
			RelocatableMaintenanceTaskExecutor executor;
		};

		void workerMain() noexcept;
		[[nodiscard]] bool runCycle();
		void finishTask(const QueuedTask &task, const RelocatableMaintenanceTaskResult &result);
		void failTask(const QueuedTask &task, std::exception_ptr failure) noexcept;
		void cancelQueuedTasksLocked();

		RelocatableMaintenanceLoopPolicy policy_;
		RelocatableMaintenanceFailureHandler failure_handler_;
		mutable std::mutex mutex_;
		std::condition_variable condition_;
		std::condition_variable idle_condition_;
		std::deque<QueuedTask> queue_;
		std::thread worker_;
		std::thread::id worker_id_{};
		RelocatableMaintenanceLoopMetrics metrics_{};
		std::exception_ptr last_failure_;
		std::unordered_set<uint64_t> active_deduplication_keys_;
		uint64_t outstanding_rewrite_bytes_ = 0;
		bool stop_requested_ = false;
	};

} // namespace graph::storage
