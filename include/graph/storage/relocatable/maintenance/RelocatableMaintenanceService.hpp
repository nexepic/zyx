/**
 * @file RelocatableMaintenanceService.hpp
 * @brief Typed production attachment for bounded relocatable maintenance schedulers.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>

#include "graph/storage/relocatable/maintenance/RelocatableMaintenanceLoop.hpp"
#include "graph/storage/relocatable/maintenance/RelocatablePageCompactionScheduler.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableSegmentRelocationScheduler.hpp"
#include "graph/storage/relocatable/maintenance/SingleWriterSaturatingCounter.hpp"

namespace graph::storage {

	class BTreePageStore;
	class EntityPageStore;
	class LargeValueStore;
	class PhysicalSegmentAllocator;
	class SlottedPagePool;
	class VectorCodebookStore;
	class VectorManifestStore;
	class VectorPageStore;
	class RelocatableMetadataStore;
	struct LargeValueReachabilityReclaimOptions;

	struct RelocatableMaintenanceOptions {
		RelocatableMaintenanceLoopPolicy loop{};
		RelocatablePageCompactionBudget page{16U, 16U * RSS_PAGE_SIZE, 64U, 8U};
		RelocatableSegmentRelocationBudget segment{1U, RSS_SEGMENT_SIZE, 64U, 8U};
		// Vector rebuild recovery walks the routed Manifest Directory incrementally.
		// This is independent of the transient task queue, so one resident recovery
		// cursor can repair durable Vector debt even when the queue capacity is one.
		size_t vector_rebuild_recovery_metadata_pages_per_cycle = 16U;

		constexpr bool operator==(const RelocatableMaintenanceOptions &) const = default;
	};

	struct RelocatableMaintenanceMetrics {
		RelocatableMaintenanceLoopMetrics loop{};
		RelocatablePageCompactionMetrics property_pages{};
		RelocatablePageCompactionMetrics entity_pages{};
		RelocatablePageCompactionMetrics btree_pages{};
		RelocatablePageCompactionMetrics vector_pages{};
		RelocatablePageCompactionMetrics vector_manifests{};
		RelocatablePageCompactionMetrics large_value_pages{};
		RelocatableSegmentRelocationMetrics segments{};
		uint64_t vector_rebuild_recovery_scanned_metadata_pages = 0U;
		uint64_t vector_rebuild_recovery_completed_rebuild_attempts = 0U;
		uint64_t global_adjacency_pools_retired = 0U;
		uint64_t global_adjacency_pools_released = 0U;

		constexpr bool operator==(const RelocatableMaintenanceMetrics &) const = default;
	};

	// The backend owns semantic Vector source validation and derived-generation
	// publication. The maintenance loop only supplies bounded admission and
	// retry semantics for this typed request.
	using RelocatableVectorRebuildExecutor =
			std::function<RelocatableMaintenanceTaskResult(uint64_t)>;
	struct RelocatableVectorRebuildDebtScan {
		std::optional<uint64_t> index_id;
		uint64_t last_scanned_index_id = 0U;
		uint64_t scanned_metadata_pages = 0U;
		bool exhausted = false;
	};

	// Resolves at most one durable rebuild candidate after the supplied logical
	// Index ID. It must fail closed if a routed Manifest does not have a valid
	// Vector definition; the cursor must never silently skip persistent state.
	using RelocatableVectorRebuildDebtScanner =
			std::function<RelocatableVectorRebuildDebtScan(uint64_t afterIndexId, size_t maximumMetadataPages)>;

	// The graph backend owns the semantic proof that a pool belonged to the
	// retired global adjacency tree. The maintenance service owns only its
	// bounded retirement and snapshot-safe release lifecycle.
	// The scanner distinguishes new empty pools from a durable retirement whose
	// snapshot-safe physical release was interrupted by a prior process crash.
	enum class RelocatableGlobalAdjacencyPoolReclaimState {
		RGAPRS_SEALED,
		RGAPRS_RETIRED,
	};

	struct RelocatableGlobalAdjacencyPoolReclaimCandidate {
		SegmentId segment_id{};
		Generation generation{};
		RelocatableGlobalAdjacencyPoolReclaimState state =
				RelocatableGlobalAdjacencyPoolReclaimState::RGAPRS_SEALED;

		constexpr bool operator==(const RelocatableGlobalAdjacencyPoolReclaimCandidate &) const = default;
	};

	using RelocatableGlobalAdjacencyPoolReclaimCandidateScanner =
			std::function<std::optional<RelocatableGlobalAdjacencyPoolReclaimCandidate>()>;

	// Every public enqueue method captures only a typed logical request. Physical
	// payload dispatch stays in the matching coordinator, while the execution LSN
	// is selected under the graph publication lock rather than at queue time.
	class RelocatableMaintenanceService final {
	public:
		RelocatableMaintenanceService(SlottedPagePool &pages, BTreePageStore &btreePages, VectorPageStore &vectorPages,
								VectorManifestStore &vectorManifests, EntityPageStore &entities,
								VectorCodebookStore &codebooks, LargeValueStore &largeValues,
								   PhysicalSegmentAllocator &physical, RelocatableMetadataStore &metadata,
			   std::mutex &publicationMutex, RelocatableMaintenanceOptions options,
			   RelocatableMaintenanceFailureHandler failureHandler = {},
			   std::function<bool()> writeAllowed = {},
			   RelocatableVectorRebuildExecutor vectorRebuild = {},
			   RelocatableVectorRebuildDebtScanner vectorRebuildDebtScanner = {},
			   RelocatableGlobalAdjacencyPoolReclaimCandidateScanner globalAdjacencyPoolReclaimCandidateScanner = {});
		~RelocatableMaintenanceService() = default;
		RelocatableMaintenanceService(const RelocatableMaintenanceService &) = delete;
		RelocatableMaintenanceService &operator=(const RelocatableMaintenanceService &) = delete;

		void start();
		void pause();
		void resume();
		void stop();
		[[nodiscard]] size_t cancelQueuedTasks();

		[[nodiscard]] uint64_t enqueuePropertyPageCompaction(SegmentId source, SegmentId destination,
															 uint32_t pageBudget);
		[[nodiscard]] uint64_t enqueueBTreePageCompaction(SegmentId source, SegmentId destination, uint32_t pageBudget);
		[[nodiscard]] uint64_t enqueueVectorPageCompaction(SegmentId source, SegmentId destination,
														   uint32_t pageBudget);
		[[nodiscard]] uint64_t enqueueVectorManifestCompaction(SegmentId source, SegmentId destination,
																   uint32_t pageBudget);
		[[nodiscard]] uint64_t enqueueEntityPageCompaction(SegmentId source, SegmentId destination,
																 uint32_t pageBudget);
		[[nodiscard]] uint64_t enqueueLargeValuePageCompaction(SegmentId source, SegmentId destination, uint32_t pageBudget);
		// Runs one bounded reachability/reclamation pass under the same publication
		// fence as other maintenance tasks. Duplicate requests are coalesced.
		[[nodiscard]] uint64_t enqueueLargeValueReachabilityReclamation(
				const LargeValueReachabilityReclaimOptions &options);
		[[nodiscard]] uint64_t enqueueCodebookExtentRelocation(uint64_t codebookId, ExtentId extentId,
													   PhysicalSlotId firstDestination,
													   PhysicalSlotId lastDestination);
		[[nodiscard]] uint64_t enqueueVectorBaseRebuild(uint64_t indexId);
		// Runs indefinitely as one bounded, deferred task. It resumes after a
		// process restart from the routed Manifest Directory, so queue admission is
		// never the source of truth for durable Vector rebuild debt.
		[[nodiscard]] uint64_t enqueueVectorRebuildRecovery();
		// Retires every currently empty global-adjacency B+Tree pool. One queued
		// task walks candidates incrementally and defers final slot release until
		// every older metadata snapshot has drained.
		[[nodiscard]] uint64_t enqueueGlobalAdjacencyPoolReclamation();

		[[nodiscard]] RelocatableMaintenanceMetrics metrics() const;
		[[nodiscard]] std::exception_ptr lastFailure() const;

	private:
		[[nodiscard]] uint64_t enqueuePageCompaction(RelocatablePageCompactionScheduler &scheduler,
													 RelocatableCompactionCoordinator &coordinator, SegmentId source,
													 SegmentId destination, uint32_t pageBudget);
		void ensureWritesAllowed() const;
		[[nodiscard]] LogSequenceNumber nextCheckpointLsn() const;

		RelocatableMaintenanceOptions options_;
		SlottedPagePool &pages_;
		LargeValueStore &large_values_;
		RelocatableMetadataStore &metadata_;
		EntityPageStore &entities_;
		std::mutex &publication_mutex_;
		std::function<bool()> write_allowed_;
		RelocatableVectorRebuildExecutor vector_rebuild_;
		RelocatableVectorRebuildDebtScanner vector_rebuild_debt_scanner_;
		RelocatableGlobalAdjacencyPoolReclaimCandidateScanner global_adjacency_pool_reclaim_candidate_scanner_;
		RelocatableCompactionCoordinator property_coordinator_;
		RelocatableCompactionCoordinator entity_coordinator_;
		RelocatableCompactionCoordinator btree_coordinator_;
		RelocatableCompactionCoordinator vector_coordinator_;
		RelocatableCompactionCoordinator manifest_coordinator_;
		RelocatableCompactionCoordinator codebook_coordinator_;
		RelocatableCompactionCoordinator large_value_coordinator_;
		RelocatablePageCompactionScheduler property_scheduler_;
		RelocatablePageCompactionScheduler entity_scheduler_;
		RelocatablePageCompactionScheduler btree_scheduler_;
		RelocatablePageCompactionScheduler vector_scheduler_;
		RelocatablePageCompactionScheduler manifest_scheduler_;
		RelocatablePageCompactionScheduler large_value_scheduler_;
		RelocatableSegmentRelocationScheduler segment_scheduler_;
		RelocatableMaintenanceLoop loop_;
		SingleWriterSaturatingCounter vector_rebuild_recovery_scanned_metadata_pages_{};
		SingleWriterSaturatingCounter vector_rebuild_recovery_completed_rebuild_attempts_{};
		SingleWriterSaturatingCounter global_adjacency_pools_retired_{};
		SingleWriterSaturatingCounter global_adjacency_pools_released_{};
	};

} // namespace graph::storage
