/**
 * @file RelocatablePageCompactionScheduler.cpp
 * @brief Admission-controlled scheduling around deterministic relocatable page compaction.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/RelocatablePageCompactionScheduler.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace graph::storage {

	namespace {

		void saturatingAdd(uint64_t &target, uint64_t value) {
			if (value > std::numeric_limits<uint64_t>::max() - target) {
				target = std::numeric_limits<uint64_t>::max();
				return;
			}
			target += value;
		}

		[[nodiscard]] RelocatablePageCompactionRunStatus mapStatus(RelocatableCompactionStatus status) {
			switch (status) {
				case RelocatableCompactionStatus::VCS_COMPLETED:
					return RelocatablePageCompactionRunStatus::VPRS_COMPLETED;
				case RelocatableCompactionStatus::VCS_NO_DESTINATION_CAPACITY:
					return RelocatablePageCompactionRunStatus::VPRS_NO_DESTINATION_CAPACITY;
				case RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS:
					return RelocatablePageCompactionRunStatus::VPRS_SOURCE_RETIRED_AWAITING_SNAPSHOTS;
			}
			throw FormatException(FormatError::FERR_INVALID_ENUM,
								  "relocatable page compaction executor returned an unsupported status");
		}

	} // namespace

	RelocatablePageCompactionScheduler::RelocatablePageCompactionScheduler(RelocatableMetadataStore &metadata,
															   RelocatablePageCompactionExecutor executor) :
		metadata_(metadata), executor_(std::move(executor)) {
		if (!executor_) {
			throw std::invalid_argument("relocatable page compaction scheduler requires a typed executor");
		}
		metrics_.last_observation = metadata_.observeCompactionState();
		metrics_.active_snapshot_count = metrics_.last_observation.active_snapshot_count;
		metrics_.tracked_retained_generations = metrics_.last_observation.retainedGenerationCount();
	}

	RelocatablePageCompactionRunResult RelocatablePageCompactionScheduler::run(const RelocatableSlottedPoolCompactionRequest &request,
																   const RelocatablePageCompactionBudget &budget,
																   const MetadataPublishOptions &options) {
		std::lock_guard<std::mutex> guard(mutex_);
		const uint8_t invalidRequest =
				static_cast<uint8_t>(!request.source_segment_id.isValid()) |
				static_cast<uint8_t>(!request.destination_segment_id.isValid()) |
				static_cast<uint8_t>(request.source_segment_id == request.destination_segment_id) |
				static_cast<uint8_t>(request.page_budget == 0U) | static_cast<uint8_t>(!request.first_lsn.isValid());
		if (invalidRequest != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable page compaction scheduler requires a bounded valid pool request");
		}
		RelocatablePageCompactionRunResult run;
		run.active_snapshot_count_before = metadata_.activeSnapshotCount();
		run.tracked_retained_generations_before = metrics_.tracked_retained_generations;
		if (run.active_snapshot_count_before > budget.maximum_active_snapshots) {
			run.status = RelocatablePageCompactionRunStatus::VPRS_SNAPSHOT_PRESSURE;
			++metrics_.snapshot_pressure_rejections;
			run.active_snapshot_count_after = run.active_snapshot_count_before;
			run.tracked_retained_generations_after = run.tracked_retained_generations_before;
			metrics_.active_snapshot_count = run.active_snapshot_count_after;
			return run;
		}
		if (run.tracked_retained_generations_before >= budget.maximum_retained_generations) {
			run.status = RelocatablePageCompactionRunStatus::VPRS_RETIRED_GENERATION_PRESSURE;
			++metrics_.retained_generation_rejections;
			run.active_snapshot_count_after = run.active_snapshot_count_before;
			run.tracked_retained_generations_after = run.tracked_retained_generations_before;
			metrics_.active_snapshot_count = run.active_snapshot_count_after;
			return run;
		}

		const uint64_t byteBoundedPages = budget.maximum_rewrite_bytes / RSS_PAGE_SIZE;
		const uint64_t admittedPages = std::min<uint64_t>({request.page_budget, budget.maximum_page_moves,
														   byteBoundedPages, std::numeric_limits<uint32_t>::max()});
		if (admittedPages == 0U) {
			run.status = RelocatablePageCompactionRunStatus::VPRS_BUDGET_EXHAUSTED;
			++metrics_.budget_exhausted_runs;
			run.active_snapshot_count_after = run.active_snapshot_count_before;
			run.tracked_retained_generations_after = run.tracked_retained_generations_before;
			metrics_.active_snapshot_count = run.active_snapshot_count_after;
			return run;
		}

		RelocatableSlottedPoolCompactionRequest admittedRequest = request;
		admittedRequest.page_budget = static_cast<uint32_t>(admittedPages);
		run.admitted_page_budget = admittedRequest.page_budget;
		++metrics_.runs_started;
		run.compaction = executor_(admittedRequest, options);
		run.status = mapStatus(run.compaction.status);
		run.rewritten_bytes = static_cast<uint64_t>(run.compaction.moved_pages) * RSS_PAGE_SIZE;
		saturatingAdd(metrics_.moved_pages, run.compaction.moved_pages);
		saturatingAdd(metrics_.rewritten_bytes, run.rewritten_bytes);
		if (run.compaction.source_retired) {
			++metrics_.retired_source_segments;
		}
		if (run.compaction.source_released) {
			run.reclaimed_physical_bytes = RSS_SEGMENT_SIZE;
			++metrics_.released_source_segments;
			saturatingAdd(metrics_.reclaimed_physical_bytes, run.reclaimed_physical_bytes);
		}
		if (run.compaction.source_retired && !run.compaction.source_released) {
			saturatingAdd(metrics_.tracked_retained_generations, 1U);
		}
		if (run.status == RelocatablePageCompactionRunStatus::VPRS_NO_DESTINATION_CAPACITY) {
			++metrics_.destination_capacity_rejections;
		} else if (run.status == RelocatablePageCompactionRunStatus::VPRS_SOURCE_RETIRED_AWAITING_SNAPSHOTS) {
			++metrics_.source_release_deferrals;
		}
		run.active_snapshot_count_after = metadata_.activeSnapshotCount();
		run.tracked_retained_generations_after = metrics_.tracked_retained_generations;
		metrics_.active_snapshot_count = run.active_snapshot_count_after;
		return run;
	}

	RelocatablePageCompactionMetrics RelocatablePageCompactionScheduler::metrics() const {
		std::lock_guard<std::mutex> guard(mutex_);
		return metrics_;
	}

	void RelocatablePageCompactionScheduler::recordDeferredSourceRelease() {
		std::lock_guard<std::mutex> guard(mutex_);
		saturatingAdd(metrics_.released_source_segments, 1U);
		saturatingAdd(metrics_.reclaimed_physical_bytes, RSS_SEGMENT_SIZE);
		metrics_.last_observation = metadata_.observeCompactionState();
		metrics_.active_snapshot_count = metrics_.last_observation.active_snapshot_count;
		metrics_.tracked_retained_generations = metrics_.last_observation.retainedGenerationCount();
	}

	RelocatableCompactionObservation RelocatablePageCompactionScheduler::refreshObservation() {
		std::lock_guard<std::mutex> guard(mutex_);
		metrics_.last_observation = metadata_.observeCompactionState();
		metrics_.active_snapshot_count = metrics_.last_observation.active_snapshot_count;
		metrics_.tracked_retained_generations = metrics_.last_observation.retainedGenerationCount();
		return metrics_.last_observation;
	}

} // namespace graph::storage
