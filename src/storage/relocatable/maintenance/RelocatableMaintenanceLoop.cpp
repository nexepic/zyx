/**
 * @file RelocatableMaintenanceLoop.cpp
 * @brief Bounded lifecycle and admission control for relocatable maintenance work.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/RelocatableMaintenanceLoop.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace graph::storage {

	namespace {

		void saturatingAdd(uint64_t &target, uint64_t value) {
			if (value > std::numeric_limits<uint64_t>::max() - target) {
				target = std::numeric_limits<uint64_t>::max();
				return;
			}
			target += value;
		}

	} // namespace

	RelocatableMaintenanceLoop::RelocatableMaintenanceLoop(RelocatableMaintenanceLoopPolicy policy,
											   RelocatableMaintenanceFailureHandler failureHandler) :
		policy_(policy), failure_handler_(std::move(failureHandler)) {
		const uint8_t invalid = static_cast<uint8_t>(policy_.maximum_outstanding_tasks == 0U) |
								static_cast<uint8_t>(policy_.maximum_outstanding_rewrite_bytes == 0U) |
								static_cast<uint8_t>(policy_.maximum_tasks_per_cycle == 0U) |
								static_cast<uint8_t>(policy_.maximum_rewrite_bytes_per_cycle == 0U) |
								static_cast<uint8_t>(policy_.minimum_cycle_interval.count() <= 0);
		if (invalid != 0U) {
			throw std::invalid_argument(
					"relocatable maintenance loop requires positive queue, cycle, byte, and interval bounds");
		}
		metrics_.state = RelocatableMaintenanceLoopState::VMLS_STOPPED;
	}

	RelocatableMaintenanceLoop::~RelocatableMaintenanceLoop() {
		try {
			stop();
		} catch (...) {
			// Destruction cannot recover from a platform thread-join failure.
			std::terminate();
		}
	}

	void RelocatableMaintenanceLoop::start() {
		std::lock_guard lock(mutex_);
		if (metrics_.state == RelocatableMaintenanceLoopState::VMLS_RUNNING ||
			metrics_.state == RelocatableMaintenanceLoopState::VMLS_PAUSED) {
			return;
		}
		if (metrics_.state != RelocatableMaintenanceLoopState::VMLS_STOPPED) {
			throw std::logic_error("relocatable maintenance loop can start only from a clean stopped state");
		}
		stop_requested_ = false;
		last_failure_ = nullptr;
		metrics_.state = RelocatableMaintenanceLoopState::VMLS_RUNNING;
		try {
			worker_ = std::thread([this] { workerMain(); });
		} catch (...) {
			metrics_.state = RelocatableMaintenanceLoopState::VMLS_STOPPED;
			throw;
		}
	}

	void RelocatableMaintenanceLoop::pause() {
		std::unique_lock lock(mutex_);
		if (std::this_thread::get_id() == worker_id_) {
			throw std::logic_error("relocatable maintenance worker cannot pause and wait for itself");
		}
		if (metrics_.state == RelocatableMaintenanceLoopState::VMLS_PAUSED) {
			idle_condition_.wait(lock, [this] { return metrics_.in_flight_tasks == 0U; });
			return;
		}
		if (metrics_.state != RelocatableMaintenanceLoopState::VMLS_RUNNING) {
			throw std::logic_error("relocatable maintenance loop can pause only while running");
		}
		metrics_.state = RelocatableMaintenanceLoopState::VMLS_PAUSED;
		condition_.notify_all();
		idle_condition_.wait(lock, [this] { return metrics_.in_flight_tasks == 0U; });
	}

	void RelocatableMaintenanceLoop::resume() {
		std::lock_guard lock(mutex_);
		if (metrics_.state == RelocatableMaintenanceLoopState::VMLS_RUNNING) {
			return;
		}
		if (metrics_.state != RelocatableMaintenanceLoopState::VMLS_PAUSED) {
			throw std::logic_error("relocatable maintenance loop can resume only from an operator pause");
		}
		metrics_.state = RelocatableMaintenanceLoopState::VMLS_RUNNING;
		condition_.notify_all();
	}

	void RelocatableMaintenanceLoop::stop() {
		{
			std::lock_guard lock(mutex_);
			if (std::this_thread::get_id() == worker_id_) {
				throw std::logic_error("relocatable maintenance worker cannot join itself");
			}
			if (metrics_.state == RelocatableMaintenanceLoopState::VMLS_STOPPED) {
				cancelQueuedTasksLocked();
				return;
			}
			stop_requested_ = true;
			metrics_.state = RelocatableMaintenanceLoopState::VMLS_STOPPING;
			condition_.notify_all();
		}
		// Every non-stopped state is published only after a worker is created;
		// a failed thread construction restores STOPPED before it can escape.
		worker_.join();
		std::lock_guard lock(mutex_);
		cancelQueuedTasksLocked();
		worker_id_ = {};
		stop_requested_ = false;
		metrics_.state = RelocatableMaintenanceLoopState::VMLS_STOPPED;
		idle_condition_.notify_all();
	}

	size_t RelocatableMaintenanceLoop::cancelQueuedTasks() {
		std::lock_guard lock(mutex_);
		const size_t cancelled = queue_.size();
		cancelQueuedTasksLocked();
		return cancelled;
	}

	uint64_t RelocatableMaintenanceLoop::submit(uint64_t maximumRewriteBytes,
														 RelocatableMaintenanceTaskExecutor executor,
														 uint64_t deduplicationKey) {
		if (!executor) {
			throw std::invalid_argument("relocatable maintenance loop requires a task executor");
		}
		std::lock_guard lock(mutex_);
		if (metrics_.state == RelocatableMaintenanceLoopState::VMLS_FAULTED ||
			metrics_.state == RelocatableMaintenanceLoopState::VMLS_STOPPING) {
			throw std::runtime_error("relocatable maintenance loop does not accept work after a failure or during shutdown");
		}
		if (deduplicationKey != 0U && active_deduplication_keys_.contains(deduplicationKey)) {
			return 0U;
		}
		if (maximumRewriteBytes > policy_.maximum_rewrite_bytes_per_cycle ||
			maximumRewriteBytes > policy_.maximum_outstanding_rewrite_bytes ||
			metrics_.in_flight_tasks >= policy_.maximum_outstanding_tasks ||
			queue_.size() >= policy_.maximum_outstanding_tasks - metrics_.in_flight_tasks ||
			maximumRewriteBytes > policy_.maximum_outstanding_rewrite_bytes - outstanding_rewrite_bytes_) {
			throw std::length_error("relocatable maintenance task exceeds the bounded queue or cycle budget");
		}
		if (metrics_.last_submitted_task_id == std::numeric_limits<uint64_t>::max()) {
			throw std::overflow_error("relocatable maintenance task identity space is exhausted");
		}
		const uint64_t taskId = ++metrics_.last_submitted_task_id;
		queue_.push_back({taskId, maximumRewriteBytes, deduplicationKey, std::move(executor)});
		if (deduplicationKey != 0U) {
			active_deduplication_keys_.insert(deduplicationKey);
		}
		outstanding_rewrite_bytes_ += maximumRewriteBytes;
		saturatingAdd(metrics_.submitted_tasks, 1U);
		metrics_.queued_tasks = queue_.size();
		metrics_.outstanding_rewrite_bytes = outstanding_rewrite_bytes_;
		condition_.notify_all();
		return taskId;
	}

	RelocatableMaintenanceLoopMetrics RelocatableMaintenanceLoop::metrics() const {
		std::lock_guard lock(mutex_);
		return metrics_;
	}

	std::exception_ptr RelocatableMaintenanceLoop::lastFailure() const {
		std::lock_guard lock(mutex_);
		return last_failure_;
	}

	void RelocatableMaintenanceLoop::workerMain() noexcept {
		{
			std::lock_guard lock(mutex_);
			worker_id_ = std::this_thread::get_id();
		}
		while (true) {
			{
				std::unique_lock lock(mutex_);
				condition_.wait(lock, [this] {
					return stop_requested_ ||
						   (metrics_.state == RelocatableMaintenanceLoopState::VMLS_RUNNING && !queue_.empty());
				});
				if (stop_requested_) {
					break;
				}
			}

			const bool attempted = runCycle();
			if (!attempted) {
				continue;
			}
			std::unique_lock lock(mutex_);
			condition_.wait_for(lock, policy_.minimum_cycle_interval, [this] {
				return stop_requested_ || metrics_.state != RelocatableMaintenanceLoopState::VMLS_RUNNING;
			});
			if (stop_requested_) {
				break;
			}
		}
		std::lock_guard lock(mutex_);
		worker_id_ = {};
		idle_condition_.notify_all();
	}

	bool RelocatableMaintenanceLoop::runCycle() {
		uint32_t attemptedTasks = 0U;
		uint64_t admittedBytes = 0U;
		bool attempted = false;
		while (attemptedTasks < policy_.maximum_tasks_per_cycle) {
			QueuedTask task;
			{
				std::lock_guard lock(mutex_);
				if (stop_requested_ || metrics_.state != RelocatableMaintenanceLoopState::VMLS_RUNNING || queue_.empty()) {
					break;
				}
				const QueuedTask &next = queue_.front();
				if (next.maximum_rewrite_bytes > policy_.maximum_rewrite_bytes_per_cycle - admittedBytes) {
					break;
				}
				task = std::move(queue_.front());
				queue_.pop_front();
				metrics_.queued_tasks = queue_.size();
				++metrics_.in_flight_tasks;
			}
			attempted = true;
			++attemptedTasks;
			admittedBytes += task.maximum_rewrite_bytes;

			try {
				const RelocatableMaintenanceTaskResult result = task.executor();
				if ((result.status != RelocatableMaintenanceTaskStatus::VMTS_COMPLETED &&
					 result.status != RelocatableMaintenanceTaskStatus::VMTS_DEFERRED) ||
					result.rewritten_bytes > task.maximum_rewrite_bytes) {
					throw std::logic_error("relocatable maintenance task exceeded its admitted rewrite reservation");
				}
				finishTask(task, result);
				if (result.status == RelocatableMaintenanceTaskStatus::VMTS_DEFERRED) {
					break;
				}
			} catch (...) {
				failTask(task, std::current_exception());
				break;
			}
		}
		if (attempted) {
			std::lock_guard lock(mutex_);
			saturatingAdd(metrics_.attempted_cycles, 1U);
		}
		return attempted;
	}

	void RelocatableMaintenanceLoop::finishTask(const QueuedTask &task, const RelocatableMaintenanceTaskResult &result) {
		std::lock_guard lock(mutex_);
		--metrics_.in_flight_tasks;
		saturatingAdd(metrics_.rewritten_bytes, result.rewritten_bytes);
		saturatingAdd(metrics_.reclaimed_physical_bytes, result.reclaimed_physical_bytes);
		if (result.status == RelocatableMaintenanceTaskStatus::VMTS_DEFERRED) {
			queue_.push_back(task);
			saturatingAdd(metrics_.deferred_attempts, 1U);
		} else {
			if (task.deduplication_key != 0U) {
				active_deduplication_keys_.erase(task.deduplication_key);
			}
			outstanding_rewrite_bytes_ -= task.maximum_rewrite_bytes;
			saturatingAdd(metrics_.completed_tasks, 1U);
		}
		metrics_.queued_tasks = queue_.size();
		metrics_.outstanding_rewrite_bytes = outstanding_rewrite_bytes_;
		idle_condition_.notify_all();
	}

	void RelocatableMaintenanceLoop::failTask(const QueuedTask &task, std::exception_ptr failure) noexcept {
		RelocatableMaintenanceFailureHandler handler;
		{
			std::lock_guard lock(mutex_);
			--metrics_.in_flight_tasks;
			if (task.deduplication_key != 0U) {
				active_deduplication_keys_.erase(task.deduplication_key);
			}
			outstanding_rewrite_bytes_ -= task.maximum_rewrite_bytes;
			saturatingAdd(metrics_.failed_tasks, 1U);
			metrics_.outstanding_rewrite_bytes = outstanding_rewrite_bytes_;
			metrics_.state = RelocatableMaintenanceLoopState::VMLS_FAULTED;
			last_failure_ = failure;
			handler = failure_handler_;
			idle_condition_.notify_all();
		}
		if (handler) {
			try {
				handler(failure);
			} catch (...) {
				// Failure reporting must never terminate the storage worker.
			}
		}
	}

	void RelocatableMaintenanceLoop::cancelQueuedTasksLocked() {
		for (const QueuedTask &task: queue_) {
			if (task.deduplication_key != 0U) {
				active_deduplication_keys_.erase(task.deduplication_key);
			}
			outstanding_rewrite_bytes_ -= task.maximum_rewrite_bytes;
		}
		saturatingAdd(metrics_.cancelled_tasks, static_cast<uint64_t>(queue_.size()));
		queue_.clear();
		metrics_.queued_tasks = 0U;
		metrics_.outstanding_rewrite_bytes = outstanding_rewrite_bytes_;
	}

} // namespace graph::storage
