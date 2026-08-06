/**
 * @file RelocatableSegmentRelocationScheduler.cpp
 * @brief Admission-controlled scheduling for one full relocatable segment relocation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/RelocatableSegmentRelocationScheduler.hpp"

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

		struct ExpectedLifecycle {
			RelocatableSegmentRelocationRunStatus run_status;
			bool source_relocated;
			bool source_released;
		};

		[[nodiscard]] ExpectedLifecycle mapExpectedLifecycle(RelocatableCompactionStatus status) {
			switch (status) {
				case RelocatableCompactionStatus::VCS_COMPLETED:
					return {RelocatableSegmentRelocationRunStatus::VSRRS_COMPLETED, true, true};
				case RelocatableCompactionStatus::VCS_NO_DESTINATION_CAPACITY:
					return {RelocatableSegmentRelocationRunStatus::VSRRS_NO_DESTINATION_CAPACITY, false, false};
				case RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS:
					return {RelocatableSegmentRelocationRunStatus::VSRRS_SOURCE_RETIRED_AWAITING_SNAPSHOTS, true, false};
			}
			throw FormatException(FormatError::FERR_INVALID_ENUM,
								  "relocatable segment relocation executor returned an unsupported status");
		}

		[[nodiscard]] RelocatableSegmentRelocationExecution normalize(const RelocatableExtentCompactionResult &result) {
			return {result.status, result.source_relocated, result.source_released, result.next_lsn};
		}

	} // namespace

	RelocatableSegmentRelocationTask
	RelocatableSegmentRelocationTask::forCodebookExtent(RelocatableCompactionCoordinator &coordinator,
												  RelocatableCodebookExtentCompactionRequest request) {
		return {request.first_lsn,
				[&coordinator, request](LogSequenceNumber firstLsn, const MetadataPublishOptions &options) {
					RelocatableCodebookExtentCompactionRequest submitted = request;
					submitted.first_lsn = firstLsn;
					return normalize(coordinator.compactCodebookExtent(submitted, options));
				}};
	}


	RelocatableSegmentRelocationScheduler::RelocatableSegmentRelocationScheduler(RelocatableMetadataStore &metadata) :
		metadata_(metadata) {
		metrics_.last_observation = metadata_.observeCompactionState();
		metrics_.active_snapshot_count = metrics_.last_observation.active_snapshot_count;
		metrics_.tracked_retained_generations = metrics_.last_observation.retainedGenerationCount();
	}

	RelocatableSegmentRelocationRunResult RelocatableSegmentRelocationScheduler::run(const RelocatableSegmentRelocationTask &task,
																		 const RelocatableSegmentRelocationBudget &budget,
																		 const MetadataPublishOptions &options) {
		std::lock_guard<std::mutex> guard(mutex_);
		if (!task.first_lsn.isValid() || !task.executor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable segment relocation scheduler requires a valid typed task and first LSN");
		}
		RelocatableSegmentRelocationRunResult run;
		run.active_snapshot_count_before = metadata_.activeSnapshotCount();
		run.tracked_retained_generations_before = metrics_.tracked_retained_generations;
		if (run.active_snapshot_count_before > budget.maximum_active_snapshots) {
			run.status = RelocatableSegmentRelocationRunStatus::VSRRS_SNAPSHOT_PRESSURE;
			++metrics_.snapshot_pressure_rejections;
			return finishRejectedRun(run);
		}
		if (run.tracked_retained_generations_before >= budget.maximum_retained_generations) {
			run.status = RelocatableSegmentRelocationRunStatus::VSRRS_RETIRED_GENERATION_PRESSURE;
			++metrics_.retained_generation_rejections;
			return finishRejectedRun(run);
		}
		if (budget.maximum_segment_moves == 0U || budget.maximum_rewrite_bytes < RSS_SEGMENT_SIZE) {
			run.status = RelocatableSegmentRelocationRunStatus::VSRRS_BUDGET_EXHAUSTED;
			++metrics_.budget_exhausted_runs;
			return finishRejectedRun(run);
		}

		run.admitted_segment_moves = 1;
		++metrics_.runs_started;
		run.compaction = task.executor(task.first_lsn, options);
		const ExpectedLifecycle expected = mapExpectedLifecycle(run.compaction.status);
		if (run.compaction.source_relocated != expected.source_relocated ||
			run.compaction.source_released != expected.source_released) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable segment relocation executor returned an inconsistent lifecycle result");
		}
		run.status = expected.run_status;
		if (run.compaction.source_relocated) {
			run.rewritten_bytes = RSS_SEGMENT_SIZE;
			saturatingAdd(metrics_.relocated_segments, 1U);
			saturatingAdd(metrics_.rewritten_bytes, run.rewritten_bytes);
		}
		if (run.compaction.source_released) {
			run.reclaimed_physical_bytes = RSS_SEGMENT_SIZE;
			saturatingAdd(metrics_.released_source_segments, 1U);
			saturatingAdd(metrics_.reclaimed_physical_bytes, run.reclaimed_physical_bytes);
		}
		if (run.compaction.source_relocated && !run.compaction.source_released) {
			saturatingAdd(metrics_.tracked_retained_generations, 1U);
		}
		if (run.status == RelocatableSegmentRelocationRunStatus::VSRRS_NO_DESTINATION_CAPACITY) {
			++metrics_.destination_capacity_rejections;
		} else if (run.status == RelocatableSegmentRelocationRunStatus::VSRRS_SOURCE_RETIRED_AWAITING_SNAPSHOTS) {
			++metrics_.source_release_deferrals;
		}
		run.active_snapshot_count_after = metadata_.activeSnapshotCount();
		run.tracked_retained_generations_after = metrics_.tracked_retained_generations;
		metrics_.active_snapshot_count = run.active_snapshot_count_after;
		return run;
	}

	RelocatableSegmentRelocationMetrics RelocatableSegmentRelocationScheduler::metrics() const {
		std::lock_guard<std::mutex> guard(mutex_);
		return metrics_;
	}

	void RelocatableSegmentRelocationScheduler::recordDeferredSourceRelease() {
		std::lock_guard<std::mutex> guard(mutex_);
		saturatingAdd(metrics_.released_source_segments, 1U);
		saturatingAdd(metrics_.reclaimed_physical_bytes, RSS_SEGMENT_SIZE);
		metrics_.last_observation = metadata_.observeCompactionState();
		metrics_.active_snapshot_count = metrics_.last_observation.active_snapshot_count;
		metrics_.tracked_retained_generations = metrics_.last_observation.retainedGenerationCount();
	}

	RelocatableCompactionObservation RelocatableSegmentRelocationScheduler::refreshObservation() {
		std::lock_guard<std::mutex> guard(mutex_);
		metrics_.last_observation = metadata_.observeCompactionState();
		metrics_.active_snapshot_count = metrics_.last_observation.active_snapshot_count;
		metrics_.tracked_retained_generations = metrics_.last_observation.retainedGenerationCount();
		return metrics_.last_observation;
	}

	RelocatableSegmentRelocationRunResult
	RelocatableSegmentRelocationScheduler::finishRejectedRun(RelocatableSegmentRelocationRunResult run) {
		run.active_snapshot_count_after = run.active_snapshot_count_before;
		run.tracked_retained_generations_after = run.tracked_retained_generations_before;
		metrics_.active_snapshot_count = run.active_snapshot_count_after;
		return run;
	}

} // namespace graph::storage
