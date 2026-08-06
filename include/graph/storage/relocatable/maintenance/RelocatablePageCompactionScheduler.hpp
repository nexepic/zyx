/**
 * @file RelocatablePageCompactionScheduler.hpp
 * @brief Admission-controlled scheduling around deterministic relocatable page compaction.
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

	// The policy is evaluated per scheduler tick. A zero page or byte allowance
	// pauses work without treating normal backpressure as a storage failure.
	struct RelocatablePageCompactionBudget {
		uint32_t maximum_page_moves = 0;
		uint64_t maximum_rewrite_bytes = 0;
		uint64_t maximum_active_snapshots = std::numeric_limits<uint64_t>::max();
		uint64_t maximum_retained_generations = std::numeric_limits<uint64_t>::max();

		constexpr bool operator==(const RelocatablePageCompactionBudget &) const = default;
	};

	// Prefixes prevent collisions with Windows platform macros.
	enum class RelocatablePageCompactionRunStatus : uint8_t {
		VPRS_COMPLETED = 1,
		VPRS_BUDGET_EXHAUSTED = 2,
		VPRS_SNAPSHOT_PRESSURE = 3,
		VPRS_RETIRED_GENERATION_PRESSURE = 4,
		VPRS_NO_DESTINATION_CAPACITY = 5,
		VPRS_SOURCE_RETIRED_AWAITING_SNAPSHOTS = 6,
	};

	struct RelocatablePageCompactionMetrics {
		uint64_t runs_started = 0;
		uint64_t budget_exhausted_runs = 0;
		uint64_t snapshot_pressure_rejections = 0;
		uint64_t retained_generation_rejections = 0;
		uint64_t destination_capacity_rejections = 0;
		uint64_t source_release_deferrals = 0;
		uint64_t moved_pages = 0;
		uint64_t rewritten_bytes = 0;
		uint64_t retired_source_segments = 0;
		uint64_t released_source_segments = 0;
		uint64_t reclaimed_physical_bytes = 0;
		uint64_t active_snapshot_count = 0;
		uint64_t tracked_retained_generations = 0;
		RelocatableCompactionObservation last_observation{};

		constexpr bool operator==(const RelocatablePageCompactionMetrics &) const = default;
	};

	struct RelocatablePageCompactionRunResult {
		RelocatablePageCompactionRunStatus status = RelocatablePageCompactionRunStatus::VPRS_COMPLETED;
		uint32_t admitted_page_budget = 0;
		uint64_t rewritten_bytes = 0;
		uint64_t reclaimed_physical_bytes = 0;
		RelocatableSlottedPoolCompactionResult compaction{};
		uint64_t active_snapshot_count_before = 0;
		uint64_t active_snapshot_count_after = 0;
		uint64_t tracked_retained_generations_before = 0;
		uint64_t tracked_retained_generations_after = 0;

		constexpr bool operator==(const RelocatablePageCompactionRunResult &) const = default;
	};

	using RelocatablePageCompactionExecutor = std::function<RelocatableSlottedPoolCompactionResult(
			const RelocatableSlottedPoolCompactionRequest &, const MetadataPublishOptions &)>;

	// This serializes policy decisions, not metadata itself. The executor selects
	// the typed coordinator method (Property, B+Tree, Vector, or Manifest), so
	// scheduling policy remains extensible without raw page-format dispatch.
	class RelocatablePageCompactionScheduler final {
	public:
		RelocatablePageCompactionScheduler(RelocatableMetadataStore &metadata, RelocatablePageCompactionExecutor executor);
		RelocatablePageCompactionScheduler(const RelocatablePageCompactionScheduler &) = delete;
		RelocatablePageCompactionScheduler &operator=(const RelocatablePageCompactionScheduler &) = delete;

		[[nodiscard]] RelocatablePageCompactionRunResult run(const RelocatableSlottedPoolCompactionRequest &request,
													   const RelocatablePageCompactionBudget &budget,
													   const MetadataPublishOptions &options = {});
		[[nodiscard]] RelocatablePageCompactionMetrics metrics() const;
		// A source released after snapshot drain did not pass through `run()` a
		// second time, so record its physical reclamation explicitly.
		void recordDeferredSourceRelease();
		// Catalog traversal is intentionally explicit and out of the hot per-tick
		// path. Call after any compaction lifecycle performed outside this scheduler.
		[[nodiscard]] RelocatableCompactionObservation refreshObservation();

	private:
		RelocatableMetadataStore &metadata_;
		RelocatablePageCompactionExecutor executor_;
		mutable std::mutex mutex_;
		RelocatablePageCompactionMetrics metrics_;
	};

} // namespace graph::storage
