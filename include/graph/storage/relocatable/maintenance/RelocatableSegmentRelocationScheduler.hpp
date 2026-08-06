/**
 * @file RelocatableSegmentRelocationScheduler.hpp
 * @brief Admission-controlled scheduling for one full relocatable segment relocation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>

#include "graph/storage/relocatable/maintenance/RelocatableCompactionCoordinator.hpp"

namespace graph::storage {

	struct RelocatableSegmentRelocationBudget {
		uint32_t maximum_segment_moves = 0;
		uint64_t maximum_rewrite_bytes = 0;
		uint64_t maximum_active_snapshots = std::numeric_limits<uint64_t>::max();
		uint64_t maximum_retained_generations = std::numeric_limits<uint64_t>::max();

		constexpr bool operator==(const RelocatableSegmentRelocationBudget &) const = default;
	};

	// Prefixes prevent collisions with Windows platform macros.
	enum class RelocatableSegmentRelocationRunStatus : uint8_t {
		VSRRS_COMPLETED = 1,
		VSRRS_BUDGET_EXHAUSTED = 2,
		VSRRS_SNAPSHOT_PRESSURE = 3,
		VSRRS_RETIRED_GENERATION_PRESSURE = 4,
		VSRRS_NO_DESTINATION_CAPACITY = 5,
		VSRRS_SOURCE_RETIRED_AWAITING_SNAPSHOTS = 6,
	};

	// The typed task factories normalize only control-plane results. They never
	// expose payload bytes or persistent references to the scheduler.
	struct RelocatableSegmentRelocationExecution {
		RelocatableCompactionStatus status = RelocatableCompactionStatus::VCS_COMPLETED;
		bool source_relocated = false;
		bool source_released = false;
		LogSequenceNumber next_lsn{};

		constexpr bool operator==(const RelocatableSegmentRelocationExecution &) const = default;
	};

	using RelocatableSegmentRelocationExecutor =
			std::function<RelocatableSegmentRelocationExecution(LogSequenceNumber, const MetadataPublishOptions &)>;

	// A task is built by a typed factory. `run()` supplies the recorded LSN to
	// the factory closure, so the scheduler and the durable coordinator cannot
	// diverge on the publication sequence.
	struct RelocatableSegmentRelocationTask {
		LogSequenceNumber first_lsn{};
		RelocatableSegmentRelocationExecutor executor;

		[[nodiscard]] static RelocatableSegmentRelocationTask forCodebookExtent(RelocatableCompactionCoordinator &coordinator,
																		  RelocatableCodebookExtentCompactionRequest request);
	};

	struct RelocatableSegmentRelocationMetrics {
		uint64_t runs_started = 0;
		uint64_t budget_exhausted_runs = 0;
		uint64_t snapshot_pressure_rejections = 0;
		uint64_t retained_generation_rejections = 0;
		uint64_t destination_capacity_rejections = 0;
		uint64_t source_release_deferrals = 0;
		uint64_t relocated_segments = 0;
		uint64_t rewritten_bytes = 0;
		uint64_t released_source_segments = 0;
		uint64_t reclaimed_physical_bytes = 0;
		uint64_t active_snapshot_count = 0;
		uint64_t tracked_retained_generations = 0;
		RelocatableCompactionObservation last_observation{};

		constexpr bool operator==(const RelocatableSegmentRelocationMetrics &) const = default;
	};

	struct RelocatableSegmentRelocationRunResult {
		RelocatableSegmentRelocationRunStatus status = RelocatableSegmentRelocationRunStatus::VSRRS_COMPLETED;
		uint32_t admitted_segment_moves = 0;
		uint64_t rewritten_bytes = 0;
		uint64_t reclaimed_physical_bytes = 0;
		RelocatableSegmentRelocationExecution compaction{};
		uint64_t active_snapshot_count_before = 0;
		uint64_t active_snapshot_count_after = 0;
		uint64_t tracked_retained_generations_before = 0;
		uint64_t tracked_retained_generations_after = 0;

		constexpr bool operator==(const RelocatableSegmentRelocationRunResult &) const = default;
	};

	// This serializes admission decisions, not metadata publication. Typed task
	// factories retain concrete Codebook requests until the
	// coordinator performs its own fail-closed validation.
	class RelocatableSegmentRelocationScheduler final {
	public:
		explicit RelocatableSegmentRelocationScheduler(RelocatableMetadataStore &metadata);
		RelocatableSegmentRelocationScheduler(const RelocatableSegmentRelocationScheduler &) = delete;
		RelocatableSegmentRelocationScheduler &operator=(const RelocatableSegmentRelocationScheduler &) = delete;

		[[nodiscard]] RelocatableSegmentRelocationRunResult run(const RelocatableSegmentRelocationTask &task,
														  const RelocatableSegmentRelocationBudget &budget,
														  const MetadataPublishOptions &options = {});
		[[nodiscard]] RelocatableSegmentRelocationMetrics metrics() const;
		// A deferred source is released by the service after snapshot drain, not by
		// another relocation run. Preserve exact reclamation accounting here.
		void recordDeferredSourceRelease();
		// Catalog traversal stays off the hot relocation tick. Call this after a
		// typed coordinator releases a deferred source outside this scheduler.
		[[nodiscard]] RelocatableCompactionObservation refreshObservation();

	private:
		[[nodiscard]] RelocatableSegmentRelocationRunResult finishRejectedRun(RelocatableSegmentRelocationRunResult run);

		RelocatableMetadataStore &metadata_;
		mutable std::mutex mutex_;
		RelocatableSegmentRelocationMetrics metrics_;
	};

} // namespace graph::storage
