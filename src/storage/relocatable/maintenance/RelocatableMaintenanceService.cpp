/**
 * @file RelocatableMaintenanceService.cpp
 * @brief Typed production attachment for bounded relocatable maintenance schedulers.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/RelocatableMaintenanceService.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"

namespace graph::storage {

	namespace {

		struct PageTaskState {
			RelocatableSlottedPoolCompactionRequest request{};
			Generation retired_generation{};
			bool awaiting_release = false;
		};

		struct SegmentTaskState {
			SegmentId release_segment_id{};
			Generation relocated_generation{};
			bool awaiting_release = false;
		};

		struct VectorRebuildRecoveryState {
			uint64_t vector_after_index_id = 0U;
		};

		void saturatingIncrement(std::atomic<uint64_t> &counter) {
			uint64_t current = counter.load(std::memory_order_relaxed);
			while (current != std::numeric_limits<uint64_t>::max() &&
				   !counter.compare_exchange_weak(current, current + 1U, std::memory_order_relaxed,
																					 std::memory_order_relaxed)) {
			}
		}

		void saturatingAdd(std::atomic<uint64_t> &counter, uint64_t increment) {
			uint64_t current = counter.load(std::memory_order_relaxed);
			while (current != std::numeric_limits<uint64_t>::max()) {
				const uint64_t updated = increment > std::numeric_limits<uint64_t>::max() - current
													 ? std::numeric_limits<uint64_t>::max()
													 : current + increment;
				if (counter.compare_exchange_weak(current, updated, std::memory_order_relaxed,
												  std::memory_order_relaxed)) {
					return;
				}
			}
		}

		[[nodiscard]] RelocatableMaintenanceTaskResult pageResult(const RelocatablePageCompactionRunResult &run) {
			switch (run.status) {
				case RelocatablePageCompactionRunStatus::VPRS_COMPLETED:
					return {RelocatableMaintenanceTaskStatus::VMTS_COMPLETED, run.rewritten_bytes,
							run.reclaimed_physical_bytes};
				case RelocatablePageCompactionRunStatus::VPRS_SNAPSHOT_PRESSURE:
				case RelocatablePageCompactionRunStatus::VPRS_RETIRED_GENERATION_PRESSURE:
				case RelocatablePageCompactionRunStatus::VPRS_NO_DESTINATION_CAPACITY:
					return {RelocatableMaintenanceTaskStatus::VMTS_DEFERRED, 0U, 0U};
				case RelocatablePageCompactionRunStatus::VPRS_SOURCE_RETIRED_AWAITING_SNAPSHOTS:
					return {RelocatableMaintenanceTaskStatus::VMTS_DEFERRED, run.rewritten_bytes, 0U};
				case RelocatablePageCompactionRunStatus::VPRS_BUDGET_EXHAUSTED:
					throw std::logic_error("relocatable maintenance configured an unusable page-compaction budget");
			}
			throw std::logic_error("relocatable maintenance received an unknown page-compaction result");
		}

		[[nodiscard]] RelocatableMaintenanceTaskResult segmentResult(const RelocatableSegmentRelocationRunResult &run) {
			switch (run.status) {
				case RelocatableSegmentRelocationRunStatus::VSRRS_COMPLETED:
					return {RelocatableMaintenanceTaskStatus::VMTS_COMPLETED, run.rewritten_bytes,
							run.reclaimed_physical_bytes};
				case RelocatableSegmentRelocationRunStatus::VSRRS_SNAPSHOT_PRESSURE:
				case RelocatableSegmentRelocationRunStatus::VSRRS_RETIRED_GENERATION_PRESSURE:
				case RelocatableSegmentRelocationRunStatus::VSRRS_NO_DESTINATION_CAPACITY:
					return {RelocatableMaintenanceTaskStatus::VMTS_DEFERRED, 0U, 0U};
				case RelocatableSegmentRelocationRunStatus::VSRRS_SOURCE_RETIRED_AWAITING_SNAPSHOTS:
					return {RelocatableMaintenanceTaskStatus::VMTS_DEFERRED, run.rewritten_bytes, 0U};
				case RelocatableSegmentRelocationRunStatus::VSRRS_BUDGET_EXHAUSTED:
					throw std::logic_error("relocatable maintenance configured an unusable segment-relocation budget");
			}
			throw std::logic_error("relocatable maintenance received an unknown segment-relocation result");
		}

		void validateOptions(const RelocatableMaintenanceOptions &options) {
			const uint8_t invalidPage = static_cast<uint8_t>(options.page.maximum_page_moves == 0U) |
										static_cast<uint8_t>(options.page.maximum_rewrite_bytes < RSS_PAGE_SIZE);
			const uint8_t invalidSegment =
					static_cast<uint8_t>(options.segment.maximum_segment_moves == 0U) |
					static_cast<uint8_t>(options.segment.maximum_rewrite_bytes < RSS_SEGMENT_SIZE);
			if (invalidPage != 0U || invalidSegment != 0U ||
				options.loop.maximum_rewrite_bytes_per_cycle < RSS_SEGMENT_SIZE ||
				options.loop.maximum_outstanding_rewrite_bytes < RSS_SEGMENT_SIZE ||
				options.vector_rebuild_recovery_metadata_pages_per_cycle == 0U ||
				options.vector_rebuild_recovery_metadata_pages_per_cycle == std::numeric_limits<size_t>::max()) {
				throw std::invalid_argument(
					"relocatable maintenance service requires usable page, segment, cycle, and Vector recovery "
						"bounds");
			}
		}

		[[nodiscard]] RelocatableMaintenanceOptions validatedOptions(RelocatableMaintenanceOptions options) {
			validateOptions(options);
			return options;
		}

	} // namespace

	RelocatableMaintenanceService::RelocatableMaintenanceService(
		SlottedPagePool &pages, BTreePageStore &btreePages, VectorPageStore &vectorPages,
			VectorManifestStore &vectorManifests, EntityPageStore &entities, VectorCodebookStore &codebooks,
			LargeValueStore &largeValues, PhysicalSegmentAllocator &physical, RelocatableMetadataStore &metadata,
			std::mutex &publicationMutex, RelocatableMaintenanceOptions options,
			RelocatableMaintenanceFailureHandler failureHandler, std::function<bool()> writeAllowed,
		RelocatableVectorRebuildExecutor vectorRebuild,
		RelocatableVectorRebuildDebtScanner vectorRebuildDebtScanner) :
		options_(validatedOptions(std::move(options))), metadata_(metadata), entities_(entities), publication_mutex_(publicationMutex),
			write_allowed_(std::move(writeAllowed)), vector_rebuild_(std::move(vectorRebuild)),
			vector_rebuild_debt_scanner_(std::move(vectorRebuildDebtScanner)),
		property_coordinator_(pages, metadata),
		entity_coordinator_(pages, entities, metadata),
		btree_coordinator_(pages, btreePages, metadata), vector_coordinator_(pages, vectorPages, metadata),
		manifest_coordinator_(pages, vectorManifests, metadata),
		codebook_coordinator_(pages, codebooks, physical, metadata),
		large_value_coordinator_(pages, largeValues, metadata),
		property_scheduler_(
				metadata,
				[this](const RelocatableSlottedPoolCompactionRequest &request, const MetadataPublishOptions &publishOptions) {
					return property_coordinator_.compactSlottedPool(request, publishOptions);
				}),
		entity_scheduler_(
				metadata,
				[this](const RelocatableSlottedPoolCompactionRequest &request, const MetadataPublishOptions &publishOptions) {
					return entity_coordinator_.compactEntityPool(request, publishOptions);
				}),
		btree_scheduler_(
				metadata,
				[this](const RelocatableSlottedPoolCompactionRequest &request, const MetadataPublishOptions &publishOptions) {
					return btree_coordinator_.compactBTreePool(request, publishOptions);
				}),
		vector_scheduler_(
				metadata,
				[this](const RelocatableSlottedPoolCompactionRequest &request, const MetadataPublishOptions &publishOptions) {
					return vector_coordinator_.compactVectorPool(request, publishOptions);
				}),
		manifest_scheduler_(
				metadata,
				[this](const RelocatableSlottedPoolCompactionRequest &request, const MetadataPublishOptions &publishOptions) {
					return manifest_coordinator_.compactVectorManifestPool(request, publishOptions);
				}),
		large_value_scheduler_(
				metadata,
				[this](const RelocatableSlottedPoolCompactionRequest &request, const MetadataPublishOptions &publishOptions) {
					return large_value_coordinator_.compactLargeValuePool(request, publishOptions);
				}),
		segment_scheduler_(metadata), loop_(options_.loop, std::move(failureHandler)) {}

	void RelocatableMaintenanceService::start() { loop_.start(); }

	void RelocatableMaintenanceService::pause() { loop_.pause(); }

	void RelocatableMaintenanceService::resume() { loop_.resume(); }

	void RelocatableMaintenanceService::stop() { loop_.stop(); }

	size_t RelocatableMaintenanceService::cancelQueuedTasks() { return loop_.cancelQueuedTasks(); }

	uint64_t RelocatableMaintenanceService::enqueuePropertyPageCompaction(SegmentId source, SegmentId destination,
																uint32_t pageBudget) {
		return enqueuePageCompaction(property_scheduler_, property_coordinator_, source, destination, pageBudget);
	}

	uint64_t RelocatableMaintenanceService::enqueueEntityPageCompaction(SegmentId source, SegmentId destination,
																	  uint32_t pageBudget) {
		return enqueuePageCompaction(entity_scheduler_, entity_coordinator_, source, destination, pageBudget);
	}

	uint64_t RelocatableMaintenanceService::enqueueLargeValuePageCompaction(SegmentId source, SegmentId destination,
																		 uint32_t pageBudget) {
		return enqueuePageCompaction(large_value_scheduler_, large_value_coordinator_, source, destination, pageBudget);
	}

	uint64_t RelocatableMaintenanceService::enqueueBTreePageCompaction(SegmentId source, SegmentId destination,
																 uint32_t pageBudget) {
		return enqueuePageCompaction(btree_scheduler_, btree_coordinator_, source, destination, pageBudget);
	}

	uint64_t RelocatableMaintenanceService::enqueueVectorPageCompaction(SegmentId source, SegmentId destination,
																  uint32_t pageBudget) {
		return enqueuePageCompaction(vector_scheduler_, vector_coordinator_, source, destination, pageBudget);
	}

	uint64_t RelocatableMaintenanceService::enqueueVectorManifestCompaction(SegmentId source, SegmentId destination,
																	  uint32_t pageBudget) {
		return enqueuePageCompaction(manifest_scheduler_, manifest_coordinator_, source, destination, pageBudget);
	}

	uint64_t RelocatableMaintenanceService::enqueuePageCompaction(RelocatablePageCompactionScheduler &scheduler,
															RelocatableCompactionCoordinator &coordinator, SegmentId source,
															SegmentId destination, uint32_t pageBudget) {
		if (!source.isValid() || !destination.isValid() || source == destination || pageBudget == 0U) {
			throw std::invalid_argument("relocatable maintenance page compaction requires distinct pools and a page budget");
		}
		const uint64_t admittedPages = std::min<uint64_t>(
				{pageBudget, options_.page.maximum_page_moves, options_.page.maximum_rewrite_bytes / RSS_PAGE_SIZE,
				 options_.loop.maximum_rewrite_bytes_per_cycle / RSS_PAGE_SIZE});
		const uint64_t reservedBytes = admittedPages * RSS_PAGE_SIZE;
		RelocatablePageCompactionBudget taskBudget = options_.page;
		taskBudget.maximum_page_moves = static_cast<uint32_t>(admittedPages);
		taskBudget.maximum_rewrite_bytes = reservedBytes;
		auto state = std::make_shared<PageTaskState>();
		state->request = {source, destination, pageBudget, {}};
		return loop_.submit(reservedBytes, [this, state, taskBudget, &scheduler, &coordinator]() {
			RelocatableMaintenanceTaskResult result;
			ensureWritesAllowed();
			{
				std::lock_guard publicationLock(publication_mutex_);
				ensureWritesAllowed();
				if (state->awaiting_release) {
					const bool released = coordinator.releaseRetiredSlottedPool(
							state->request.source_segment_id, state->retired_generation, nextCheckpointLsn());
					if (released) {
						scheduler.recordDeferredSourceRelease();
						result = {RelocatableMaintenanceTaskStatus::VMTS_COMPLETED, 0U, RSS_SEGMENT_SIZE};
					} else {
						result = {RelocatableMaintenanceTaskStatus::VMTS_DEFERRED, 0U, 0U};
					}
					return result;
				}

				MetadataSnapshotPin snapshot = metadata_.openSnapshot();
				const CatalogEntry sourceEntry = metadata_.findCatalog(snapshot, state->request.source_segment_id);
				snapshot.release();
				state->request.first_lsn = nextCheckpointLsn();
				const RelocatablePageCompactionRunResult run = scheduler.run(state->request, taskBudget);
				result = pageResult(run);
				if (run.status == RelocatablePageCompactionRunStatus::VPRS_RETIRED_GENERATION_PRESSURE) {
					static_cast<void>(scheduler.refreshObservation());
				}
				if (run.status == RelocatablePageCompactionRunStatus::VPRS_SOURCE_RETIRED_AWAITING_SNAPSHOTS) {
					state->retired_generation = sourceEntry.generation;
					state->awaiting_release = true;
				}
			}
			return result;
		});
	}

	uint64_t RelocatableMaintenanceService::enqueueCodebookExtentRelocation(uint64_t codebookId, ExtentId extentId,
																	  PhysicalSlotId firstDestination,
																	  PhysicalSlotId lastDestination) {
		if (!VectorCodebookExtentIdCodec::canEncodeCodebookId(codebookId) || !extentId.isValid() ||
			!firstDestination.isValid() || !lastDestination.isValid() ||
			firstDestination > lastDestination) {
			throw std::invalid_argument(
					"relocatable maintenance Codebook relocation requires identities and a destination range");
		}
		auto state = std::make_shared<SegmentTaskState>();
		return loop_.submit(RSS_SEGMENT_SIZE, [this, state, codebookId, extentId, firstDestination,
														 lastDestination]() {
			RelocatableMaintenanceTaskResult result;
			ensureWritesAllowed();
			{
				std::lock_guard publicationLock(publication_mutex_);
				ensureWritesAllowed();
				if (state->awaiting_release) {
					const bool released = codebook_coordinator_.releaseRelocatedExtentSource(
							state->release_segment_id, state->relocated_generation, nextCheckpointLsn());
					if (released) {
						segment_scheduler_.recordDeferredSourceRelease();
						result = {RelocatableMaintenanceTaskStatus::VMTS_COMPLETED, 0U, RSS_SEGMENT_SIZE};
					} else {
						result = {RelocatableMaintenanceTaskStatus::VMTS_DEFERRED, 0U, 0U};
					}
					return result;
				}
				MetadataSnapshotPin snapshot = metadata_.openSnapshot();
				const ExtentLocation sourceLocation = metadata_.findExtentLocation(snapshot, extentId);
				snapshot.release();
				RelocatableCodebookExtentCompactionRequest request{codebookId, extentId, firstDestination, lastDestination,
															 nextCheckpointLsn()};
				const RelocatableSegmentRelocationRunResult run = segment_scheduler_.run(
						RelocatableSegmentRelocationTask::forCodebookExtent(codebook_coordinator_, request),
						options_.segment);
				result = segmentResult(run);
				if (run.status == RelocatableSegmentRelocationRunStatus::VSRRS_RETIRED_GENERATION_PRESSURE) {
					static_cast<void>(segment_scheduler_.refreshObservation());
				}
				if (run.status == RelocatableSegmentRelocationRunStatus::VSRRS_SOURCE_RETIRED_AWAITING_SNAPSHOTS) {
					MetadataSnapshotPin relocatedSnapshot = metadata_.openSnapshot();
					const CatalogEntry relocated = metadata_.findCatalog(relocatedSnapshot, sourceLocation.segment_id);
					relocatedSnapshot.release();
					state->release_segment_id = sourceLocation.segment_id;
					state->relocated_generation = relocated.generation;
					state->awaiting_release = true;
				}
			}
			return result;
		});
	}


	uint64_t RelocatableMaintenanceService::enqueueVectorBaseRebuild(uint64_t indexId) {
		if (indexId == 0U || !vector_rebuild_) {
			throw std::invalid_argument("relocatable Vector maintenance requires a valid rebuild executor and Index ID");
		}
		const uint64_t reservedBytes = std::max<uint64_t>(RSS_SEGMENT_SIZE,
				std::min(options_.loop.maximum_rewrite_bytes_per_cycle, options_.loop.maximum_outstanding_rewrite_bytes));
		return loop_.submit(reservedBytes, [this, indexId]() {
			ensureWritesAllowed();
			std::lock_guard publicationLock(publication_mutex_);
			ensureWritesAllowed();
			const RelocatableMaintenanceTaskResult result = vector_rebuild_(indexId);
			if (result.status != RelocatableMaintenanceTaskStatus::VMTS_COMPLETED &&
				result.status != RelocatableMaintenanceTaskStatus::VMTS_DEFERRED) {
				throw std::logic_error("relocatable Vector rebuild returned an unknown maintenance status");
			}
			return result;
		}, indexId);
	}

	uint64_t RelocatableMaintenanceService::enqueueVectorRebuildRecovery() {
		if (!vector_rebuild_ || !vector_rebuild_debt_scanner_) {
			throw std::invalid_argument(
					"relocatable Vector rebuild recovery requires its typed executor and durable debt scanner");
		}
		const uint64_t reservedBytes = std::max<uint64_t>(
				RSS_SEGMENT_SIZE,
				std::min(options_.loop.maximum_rewrite_bytes_per_cycle, options_.loop.maximum_outstanding_rewrite_bytes));
		auto state = std::make_shared<VectorRebuildRecoveryState>();
		return loop_.submit(reservedBytes, [this, state]() {
			ensureWritesAllowed();
			const RelocatableVectorRebuildDebtScan scan = vector_rebuild_debt_scanner_(
					state->vector_after_index_id, options_.vector_rebuild_recovery_metadata_pages_per_cycle);
			if (scan.scanned_metadata_pages > options_.vector_rebuild_recovery_metadata_pages_per_cycle ||
				(scan.last_scanned_index_id != 0U && scan.last_scanned_index_id <= state->vector_after_index_id) ||
				(scan.index_id.has_value() &&
					(*scan.index_id <= state->vector_after_index_id || scan.last_scanned_index_id != *scan.index_id))) {
				throw std::logic_error(
						"relocatable Vector rebuild recovery scanner returned a nonmonotonic or unbounded result");
			}
			saturatingAdd(vector_rebuild_recovery_scanned_metadata_pages_, scan.scanned_metadata_pages);
			if (!scan.index_id.has_value()) {
				state->vector_after_index_id = scan.exhausted ? 0U : scan.last_scanned_index_id;
				return RelocatableMaintenanceTaskResult{RelocatableMaintenanceTaskStatus::VMTS_DEFERRED, 0U, 0U};
			}

			std::lock_guard publicationLock(publication_mutex_);
			ensureWritesAllowed();
			const RelocatableMaintenanceTaskResult result = vector_rebuild_(*scan.index_id);
			if (result.status != RelocatableMaintenanceTaskStatus::VMTS_COMPLETED &&
				result.status != RelocatableMaintenanceTaskStatus::VMTS_DEFERRED) {
				throw std::logic_error("relocatable Vector rebuild recovery returned an unknown maintenance status");
			}
			if (result.status == RelocatableMaintenanceTaskStatus::VMTS_DEFERRED) {
				return result;
			}
			state->vector_after_index_id = scan.last_scanned_index_id;
			saturatingIncrement(vector_rebuild_recovery_completed_rebuild_attempts_);
			// This persistent scheduler remains queued after each completed rebuild so
			// newly committed durable debt does not rely on transient queue admission.
			return RelocatableMaintenanceTaskResult{
					RelocatableMaintenanceTaskStatus::VMTS_DEFERRED, result.rewritten_bytes, result.reclaimed_physical_bytes};
		});
	}

	RelocatableMaintenanceMetrics RelocatableMaintenanceService::metrics() const {
		return {loop_.metrics(),
				property_scheduler_.metrics(),
				entity_scheduler_.metrics(),
				btree_scheduler_.metrics(),
				vector_scheduler_.metrics(),
				manifest_scheduler_.metrics(),
				large_value_scheduler_.metrics(),
				segment_scheduler_.metrics(),
				vector_rebuild_recovery_scanned_metadata_pages_.load(std::memory_order_relaxed),
				vector_rebuild_recovery_completed_rebuild_attempts_.load(std::memory_order_relaxed)};
	}

	std::exception_ptr RelocatableMaintenanceService::lastFailure() const { return loop_.lastFailure(); }

	void RelocatableMaintenanceService::ensureWritesAllowed() const {
		if (write_allowed_ && !write_allowed_()) {
			throw std::logic_error("relocatable maintenance requires reopen after an indeterminate write");
		}
	}

	LogSequenceNumber RelocatableMaintenanceService::nextCheckpointLsn() const {
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const uint64_t current = snapshot.superblock().checkpoint_lsn.value();
		snapshot.release();
		if (current == std::numeric_limits<uint64_t>::max()) {
			throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "relocatable maintenance cannot advance an exhausted checkpoint LSN");
		}
		return LogSequenceNumber(current + 1U);
	}

} // namespace graph::storage
