/**
 * @file test_RelocatableStorageBackendSchema_Maintenance.cpp
 * @brief Durable Vector debt, maintenance lifecycle, and schema recreation tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendSchemaTestFixture.hpp"

#include <atomic>
#include <chrono>
#include <limits>

#ifndef _WIN32
#include <cerrno>
#endif

#include "graph/storage/relocatable/format/SuperblockFile.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

	[[nodiscard]] LargeValueReachabilityReclaimOptions largeValueReclaimOptions() {
		LargeValueReachabilityReclaimOptions options;
		options.maximum_manifest_locations = 16U;
		options.maximum_manifest_metadata_pages = 16U;
		options.maximum_manifest_nodes = 16U;
		options.maximum_owner_locations = 16U;
		options.maximum_owner_metadata_pages = 16U;
		options.maximum_data_pages = 16U;
		// One State-page journal publication shares the default 128 KiB worker
		// cycle with up to 31 LargeValue page retirements.
		options.maximum_retirements = 31U;
		options.maximum_page_locations = 128U;
		options.maximum_pools = 16U;
		options.first_index_slot = PhysicalSlotId(1U);
		options.last_index_slot = PhysicalSlotId(999U);
		options.first_journal_state_slot = PhysicalSlotId(1000U);
		options.last_journal_state_slot = PhysicalSlotId(1015U);
		options.journal_state_page_owner_id = 0x4C564D4AU;
		options.journal_state_id = 0x4C564D4AU;
		return options;
	}

} // namespace

TEST_F(RelocatableStorageBackendSchemaTest, ReclaimsUnreachableLargeValuePropertyStorageThroughThePublicationGate) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::string payload(8U * 1024U, 'x');
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	backend.deleteLogicalNodes({{nodes.front(), false, 0U}});

	const LargeValueReachabilityReclaimReport report =
			backend.reclaimUnreachableLargeValues(largeValueReclaimOptions());
	EXPECT_EQ(report.retired_values, 1U);
	EXPECT_GT(report.retired_manifest_pages, 0U);
	EXPECT_GT(report.retired_data_pages, 0U);
	EXPECT_GT(report.published_generation, report.scanned_generation);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_EQ(reopened.reclaimUnreachableLargeValues(largeValueReclaimOptions()).retired_values, 0U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   LargeValueReclamationRejectsAnotherPublicationAfterConsumingTheFinalCheckpoint) {
	RelocatableStorageBackend writer(options());
	writer.open();
	const std::string payload(8U * 1024U, 'z');
	const std::vector<uint64_t> nodes =
			writer.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	writer.deleteLogicalNodes({{nodes.front(), false, 0U}});
	writer.close();

	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		SuperblockFile superblocks(io);
		Superblock finalPublication = superblocks.recover();
		ASSERT_LT(finalPublication.generation.value(), std::numeric_limits<uint64_t>::max());
		finalPublication.generation = Generation(finalPublication.generation.value() + 1U);
		finalPublication.checkpoint_lsn = LogSequenceNumber(std::numeric_limits<uint64_t>::max() - 5U);
		superblocks.publish(finalPublication);
		io->sync();
		io.reset();
		data->close();
	}

	RelocatableStorageBackend exhausted(options());
	exhausted.open();
	EXPECT_EQ(exhausted.reclaimUnreachableLargeValues(largeValueReclaimOptions()).retired_values, 1U);
	EXPECT_THROW(static_cast<void>(exhausted.reclaimUnreachableLargeValues(largeValueReclaimOptions())),
			 FormatException);
	exhausted.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   EnqueuesLargeValueReachabilityReclamationThroughTheBoundedMaintenanceWorker) {
	RelocatableStorageBackendOptions configured = options();
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	configured.maintenance = maintenance;
	RelocatableStorageBackend backend(configured);
	backend.open();
	const std::string payload(8U * 1024U, 'w');
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	backend.deleteLogicalNodes({{nodes.front(), false, 0U}});

	backend.pauseMaintenance();
	const uint64_t taskId = backend.enqueueLargeValueReachabilityReclamation(largeValueReclaimOptions());
	EXPECT_EQ(backend.enqueueLargeValueReachabilityReclamation(largeValueReclaimOptions()), 0U);
	EXPECT_GT(taskId, 0U);
	backend.resumeMaintenance();
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceMetrics metrics = backend.maintenanceMetrics();
		return metrics.loop.reclaimed_physical_bytes >= RSS_PAGE_SIZE ||
			   metrics.loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
	}));
	if (const std::exception_ptr failure = backend.maintenanceFailure(); failure) {
		try {
			std::rethrow_exception(failure);
		} catch (const std::exception &error) {
			FAIL() << "LargeValue reachability maintenance failed: " << error.what();
		}
	}
	EXPECT_GE(backend.maintenanceMetrics().loop.reclaimed_physical_bytes, RSS_PAGE_SIZE);
	// Candidate enumeration, both durable owner scans, and candidate retirement
	// are separate persisted phases rather than one in-memory full scan.
	EXPECT_GE(backend.maintenanceMetrics().loop.deferred_attempts, 3U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_EQ(reopened.reclaimUnreachableLargeValues(largeValueReclaimOptions()).retired_values, 0U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   LargeValueMaintenanceWorkerContinuesPastOneDurableCandidateBatch) {
	RelocatableStorageBackendOptions configured = options();
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	configured.maintenance = maintenance;
	RelocatableStorageBackend backend(configured);
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Document"}, {{"body", graph::PropertyValue(std::string(8U * 1024U, 'a'))}}},
			 {{"Document"}, {{"body", graph::PropertyValue(std::string(8U * 1024U, 'b'))}}}});
	ASSERT_EQ(nodes.size(), 2U);
	backend.deleteLogicalNodes({{nodes[0U], false, 0U}, {nodes[1U], false, 0U}});

	LargeValueReachabilityReclaimOptions bounded = largeValueReclaimOptions();
	bounded.maximum_manifest_locations = 1U;
	backend.pauseMaintenance();
	ASSERT_GT(backend.enqueueLargeValueReachabilityReclamation(bounded), 0U);
	backend.resumeMaintenance();
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceMetrics metrics = backend.maintenanceMetrics();
		return metrics.loop.reclaimed_physical_bytes >= 2U * RSS_PAGE_SIZE ||
				   metrics.loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
	}));
	if (const std::exception_ptr failure = backend.maintenanceFailure(); failure) {
		try {
			std::rethrow_exception(failure);
		} catch (const std::exception &error) {
			FAIL() << "LargeValue continuation maintenance failed: " << error.what();
		}
	}
	EXPECT_GE(backend.maintenanceMetrics().loop.deferred_attempts, 7U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   LargeValueMaintenanceWorkerRestartsStaleJournalAfterAnExternalOwnerPublication) {
	RelocatableStorageBackendOptions configured = options();
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(25);
	configured.maintenance = maintenance;
	RelocatableStorageBackend backend(configured);
	backend.open();
	const std::string payload(8U * 1024U, 'r');
	const std::vector<uint64_t> deleted =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(deleted.size(), 1U);
	backend.deleteLogicalNodes({{deleted.front(), false, 0U}});

	backend.pauseMaintenance();
	ASSERT_GT(backend.enqueueLargeValueReachabilityReclamation(largeValueReclaimOptions()), 0U);
	backend.resumeMaintenance();
	ASSERT_TRUE(waitUntil([&] { return backend.maintenanceMetrics().loop.deferred_attempts >= 1U; }));
	backend.pauseMaintenance();
	// This reuses the previously orphaned content and publishes a new owner
	// generation after the first journal phase has been saved.
	const std::vector<uint64_t> retained =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(retained.size(), 1U);
	backend.resumeMaintenance();
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceMetrics metrics = backend.maintenanceMetrics();
		return (metrics.loop.deferred_attempts >= 4U && metrics.loop.completed_tasks >= 1U) ||
				   metrics.loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
	}));
	if (const std::exception_ptr failure = backend.maintenanceFailure(); failure) {
		try {
			std::rethrow_exception(failure);
		} catch (const std::exception &error) {
			FAIL() << "LargeValue stale-journal restart failed: " << error.what();
		}
	}
	EXPECT_EQ(backend.reclaimUnreachableLargeValues(largeValueReclaimOptions()).retired_values, 0U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   LargeValueMaintenanceJournalResumesAcrossReopenAtEveryDurablePhase) {
	RelocatableStorageBackendOptions initialOptions = options();
	RelocatableMaintenanceOptions maintenance;
	// Leave a wide interval after each deferred phase so the test can close the
	// process before the queued continuation begins its next durable phase.
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(100);
	initialOptions.maintenance = maintenance;
	{
		RelocatableStorageBackend backend(initialOptions);
		backend.open();
		const std::vector<uint64_t> nodes = backend.createLogicalNodes(
				{{{"Document"}, {{"body", graph::PropertyValue(std::string(8U * 1024U, 'p'))}}}});
		ASSERT_EQ(nodes.size(), 1U);
		backend.deleteLogicalNodes({{nodes.front(), false, 0U}});
		backend.close();
	}

	for (size_t phase = 0U; phase < 3U; ++phase) {
		RelocatableStorageBackendOptions resumedOptions = options();
		resumedOptions.maintenance = maintenance;
		RelocatableStorageBackend backend(resumedOptions);
		backend.open();
		backend.pauseMaintenance();
		ASSERT_GT(backend.enqueueLargeValueReachabilityReclamation(largeValueReclaimOptions()), 0U);
		backend.resumeMaintenance();
		ASSERT_TRUE(waitUntil([&] { return backend.maintenanceMetrics().loop.deferred_attempts >= 1U; }));
		backend.pauseMaintenance();
		EXPECT_EQ(backend.maintenanceMetrics().loop.completed_tasks, 0U);
		backend.close();
	}

	RelocatableStorageBackendOptions finalOptions = options();
	finalOptions.maintenance = maintenance;
	RelocatableStorageBackend backend(finalOptions);
	backend.open();
	backend.pauseMaintenance();
	ASSERT_GT(backend.enqueueLargeValueReachabilityReclamation(largeValueReclaimOptions()), 0U);
	backend.resumeMaintenance();
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceMetrics metrics = backend.maintenanceMetrics();
		return metrics.loop.reclaimed_physical_bytes >= RSS_PAGE_SIZE ||
				   metrics.loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
	}));
	if (const std::exception_ptr failure = backend.maintenanceFailure(); failure) {
		try {
			std::rethrow_exception(failure);
		} catch (const std::exception &error) {
			FAIL() << "LargeValue journal reopen recovery failed: " << error.what();
		}
	}
	EXPECT_EQ(backend.reclaimUnreachableLargeValues(largeValueReclaimOptions()).retired_values, 0U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsLargeValueReclamationThatExceedsOneMaintenanceCycleBudget) {
	RelocatableStorageBackendOptions configured = options();
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.maximum_rewrite_bytes_per_cycle = RSS_SEGMENT_SIZE;
	configured.maintenance = maintenance;
	RelocatableStorageBackend backend(configured);
	backend.open();
	backend.pauseMaintenance();

	LargeValueReachabilityReclaimOptions oversized = largeValueReclaimOptions();
	oversized.maximum_retirements = RSS_SEGMENT_SIZE / RSS_PAGE_SIZE + 1U;
	EXPECT_THROW(static_cast<void>(backend.enqueueLargeValueReachabilityReclamation(oversized)), std::length_error);

	backend.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, LargeValueMaintenanceFinalSyncFailureRequiresReopenAndCanResume) {
	RelocatableStorageBackendOptions configured = options();
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	configured.maintenance = maintenance;
	RelocatableStorageBackend backend(configured);
	backend.open();
	const std::string payload(8U * 1024U, 'f');
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	backend.deleteLogicalNodes({{nodes.front(), false, 0U}});
	backend.pauseMaintenance();

	std::atomic<bool> inactiveSuperblockWritten = false;
	std::atomic<uint32_t> syncCalls = 0U;
	{
		StorageIO::ScopedAllThreadNativeWriteHookForTesting observeInactiveSuperblock(
				[&inactiveSuperblockWritten](const file_handle_t fd, const void *bytes, const size_t byteCount,
											 const pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						inactiveSuperblockWritten.store(true, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedAllThreadNativeSyncHookForTesting failFinalPublicationFence(
				[&inactiveSuperblockWritten, &syncCalls](const file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					if (inactiveSuperblockWritten.load(std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		ASSERT_GT(backend.enqueueLargeValueReachabilityReclamation(largeValueReclaimOptions()), 0U);
		backend.resumeMaintenance();
		ASSERT_TRUE(waitUntil([&] {
			return backend.maintenanceMetrics().loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
		}));
	}
	EXPECT_TRUE(inactiveSuperblockWritten.load(std::memory_order_relaxed));
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	EXPECT_NE(backend.maintenanceFailure(), nullptr);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_NO_THROW(static_cast<void>(reopened.reclaimUnreachableLargeValues(largeValueReclaimOptions())));
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   LargeValueMaintenanceLaterJournalStatePublicationFinalSyncFailureRequiresReopenAndCanResume) {
	RelocatableStorageBackendOptions configured = options();
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	configured.maintenance = maintenance;
	RelocatableStorageBackend backend(configured);
	backend.open();
	const std::string payload(8U * 1024U, 'j');
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	backend.deleteLogicalNodes({{nodes.front(), false, 0U}});
	backend.pauseMaintenance();

	PhysicalPageRef initialStateDirectoryRoot;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		initialStateDirectoryRoot = SuperblockFile(io).recover().state_directory_root;
	}
	std::atomic<uint32_t> journalStatePublications = 0U;
	std::atomic<bool> laterJournalPublicationWritten = false;
	std::atomic<bool> failedFinalSync = false;
	{
		StorageIO::ScopedAllThreadNativeWriteHookForTesting observeJournalPublications(
				[initialStateDirectoryRoot, &journalStatePublications, &laterJournalPublicationWritten](
						file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						try {
							const Superblock candidate = SuperblockCodec::decode(
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
							if (candidate.state_directory_root != initialStateDirectoryRoot &&
								journalStatePublications.fetch_add(1U, std::memory_order_relaxed) + 1U == 2U) {
								laterJournalPublicationWritten.store(true, std::memory_order_relaxed);
							}
						} catch (const FormatException &) {
							// Only a valid State-directory publication can arm this fence fault.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedAllThreadNativeSyncHookForTesting failLaterJournalFinalSync(
				[&laterJournalPublicationWritten, &failedFinalSync](file_handle_t fd) {
					if (laterJournalPublicationWritten.load(std::memory_order_relaxed) &&
						!failedFinalSync.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		ASSERT_GT(backend.enqueueLargeValueReachabilityReclamation(largeValueReclaimOptions()), 0U);
		backend.resumeMaintenance();
		ASSERT_TRUE(waitUntil([&] {
			return backend.maintenanceMetrics().loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
		}));
	}
	EXPECT_GE(journalStatePublications.load(std::memory_order_relaxed), 2U);
	EXPECT_TRUE(laterJournalPublicationWritten.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedFinalSync.load(std::memory_order_relaxed));
	EXPECT_NE(backend.maintenanceFailure(), nullptr);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackendOptions reopenedOptions = options();
	reopenedOptions.maintenance = maintenance;
	RelocatableStorageBackend reopened(reopenedOptions);
	reopened.open();
	reopened.pauseMaintenance();
	ASSERT_GT(reopened.enqueueLargeValueReachabilityReclamation(largeValueReclaimOptions()), 0U);
	reopened.resumeMaintenance();
	ASSERT_TRUE(waitUntil([&] {
		const RelocatableMaintenanceMetrics metrics = reopened.maintenanceMetrics();
		return metrics.loop.reclaimed_physical_bytes >= RSS_PAGE_SIZE ||
				metrics.loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
	}));
	if (const std::exception_ptr failure = reopened.maintenanceFailure(); failure) {
		try {
			std::rethrow_exception(failure);
		} catch (const std::exception &error) {
			FAIL() << "later LargeValue journal final-sync recovery failed: " << error.what();
		}
	}
	EXPECT_EQ(reopened.reclaimUnreachableLargeValues(largeValueReclaimOptions()).retired_values, 0U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   LargeValueMaintenanceJournalResetCandidateShortWriteRequiresReopenAndCanResume) {
	RelocatableStorageBackendOptions configured = options();
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	configured.maintenance = maintenance;
	RelocatableStorageBackend backend(configured);
	backend.open();
	const std::string payload(8U * 1024U, 'r');
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	backend.deleteLogicalNodes({{nodes.front(), false, 0U}});
	backend.pauseMaintenance();

	PhysicalPageRef priorManifestDirectoryRoot;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		priorManifestDirectoryRoot = SuperblockFile(io).recover().large_value_manifest_directory_root;
	}
	std::atomic<bool> retirementPublished = false;
	std::atomic<bool> resetCandidateShortWritten = false;
	{
		StorageIO::ScopedAllThreadNativeWriteHookForTesting failResetCandidate(
				[priorManifestDirectoryRoot, &retirementPublished, &resetCandidateShortWritten](
						file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					const bool superblock = byteCount == RSS_PAGE_SIZE &&
							(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U));
					if (superblock && !retirementPublished.load(std::memory_order_relaxed)) {
						try {
							const Superblock candidate = SuperblockCodec::decode(
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
							if (candidate.large_value_manifest_directory_root != priorManifestDirectoryRoot)
								retirementPublished.store(true, std::memory_order_relaxed);
						} catch (const FormatException &) {
							// Ignore non-canonical writes outside the retirement publication boundary.
						}
					}
					if (retirementPublished.load(std::memory_order_relaxed) && !superblock && byteCount == RSS_PAGE_SIZE &&
							!resetCandidateShortWritten.exchange(true, std::memory_order_relaxed)) {
						return portable_pwrite(fd, bytes, RSS_PAGE_OWNER_HEADER_BYTES - 1U, offset);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		ASSERT_GT(backend.enqueueLargeValueReachabilityReclamation(largeValueReclaimOptions()), 0U);
		backend.resumeMaintenance();
		ASSERT_TRUE(waitUntil([&] {
			return backend.maintenanceMetrics().loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
		}));
	}
	EXPECT_TRUE(retirementPublished.load(std::memory_order_relaxed));
	EXPECT_TRUE(resetCandidateShortWritten.load(std::memory_order_relaxed));
	EXPECT_NE(backend.maintenanceFailure(), nullptr);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_NO_THROW(static_cast<void>(reopened.reclaimUnreachableLargeValues(largeValueReclaimOptions())));
	EXPECT_EQ(reopened.reclaimUnreachableLargeValues(largeValueReclaimOptions()).retired_values, 0U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   LargeValueMaintenanceJournalResetCandidateDiskFullRequiresReopenAndCanResume) {
	RelocatableStorageBackendOptions configured = options();
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	configured.maintenance = maintenance;
	RelocatableStorageBackend backend(configured);
	backend.open();
	const std::string payload(8U * 1024U, 'e');
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	backend.deleteLogicalNodes({{nodes.front(), false, 0U}});
	backend.pauseMaintenance();

	PhysicalPageRef priorManifestDirectoryRoot;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		priorManifestDirectoryRoot = SuperblockFile(io).recover().large_value_manifest_directory_root;
	}
	std::atomic<bool> retirementPublished = false;
	std::atomic<bool> resetCandidateFailed = false;
	{
		StorageIO::ScopedAllThreadNativeWriteHookForTesting failResetCandidate(
				[priorManifestDirectoryRoot, &retirementPublished, &resetCandidateFailed](
						file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					const bool superblock = byteCount == RSS_PAGE_SIZE &&
							(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U));
					if (superblock && !retirementPublished.load(std::memory_order_relaxed)) {
						try {
							const Superblock candidate = SuperblockCodec::decode(
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
							if (candidate.large_value_manifest_directory_root != priorManifestDirectoryRoot)
								retirementPublished.store(true, std::memory_order_relaxed);
						} catch (const FormatException &) {
							// Ignore non-canonical writes outside the retirement publication boundary.
						}
					}
					if (retirementPublished.load(std::memory_order_relaxed) && !superblock && byteCount == RSS_PAGE_SIZE &&
							!resetCandidateFailed.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		ASSERT_GT(backend.enqueueLargeValueReachabilityReclamation(largeValueReclaimOptions()), 0U);
		backend.resumeMaintenance();
		ASSERT_TRUE(waitUntil([&] {
			return backend.maintenanceMetrics().loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
		}));
	}
	EXPECT_TRUE(retirementPublished.load(std::memory_order_relaxed));
	EXPECT_TRUE(resetCandidateFailed.load(std::memory_order_relaxed));
	EXPECT_NE(backend.maintenanceFailure(), nullptr);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_NO_THROW(static_cast<void>(reopened.reclaimUnreachableLargeValues(largeValueReclaimOptions())));
	EXPECT_EQ(reopened.reclaimUnreachableLargeValues(largeValueReclaimOptions()).retired_values, 0U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   LargeValueMaintenanceJournalResetFinalSyncFailureRequiresReopenAndCanResume) {
	RelocatableStorageBackendOptions configured = options();
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	configured.maintenance = maintenance;
	RelocatableStorageBackend backend(configured);
	backend.open();
	const std::string payload(8U * 1024U, 'f');
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	backend.deleteLogicalNodes({{nodes.front(), false, 0U}});
	backend.pauseMaintenance();

	PhysicalPageRef priorManifestDirectoryRoot;
	PhysicalPageRef priorStateDirectoryRoot;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		const Superblock prior = SuperblockFile(io).recover();
		priorManifestDirectoryRoot = prior.large_value_manifest_directory_root;
		priorStateDirectoryRoot = prior.state_directory_root;
	}
	std::atomic<bool> retirementPublished = false;
	std::atomic<bool> resetPublicationWritten = false;
	std::atomic<bool> failedFinalSync = false;
	{
		StorageIO::ScopedAllThreadNativeWriteHookForTesting observeResetPublication(
				[priorManifestDirectoryRoot, priorStateDirectoryRoot, &retirementPublished,
				 &resetPublicationWritten](file_handle_t fd, const void *bytes, size_t byteCount,
															 pwrite_off_t offset) {
					const bool superblock = byteCount == RSS_PAGE_SIZE &&
							(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U));
					if (superblock && !retirementPublished.load(std::memory_order_relaxed)) {
						try {
							const Superblock candidate = SuperblockCodec::decode(
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
							if (candidate.large_value_manifest_directory_root != priorManifestDirectoryRoot)
								retirementPublished.store(true, std::memory_order_relaxed);
						} catch (const FormatException &) {
							// Ignore non-canonical writes outside the retirement boundary.
						}
					}
					if (superblock && retirementPublished.load(std::memory_order_relaxed) &&
							!resetPublicationWritten.load(std::memory_order_relaxed)) {
						try {
							const Superblock candidate = SuperblockCodec::decode(
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
							if (candidate.state_directory_root != priorStateDirectoryRoot) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written == static_cast<pwrite_ssize_t>(byteCount))
									resetPublicationWritten.store(true, std::memory_order_relaxed);
								return written;
							}
						} catch (const FormatException &) {
							// Ignore non-canonical writes outside the reset boundary.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedAllThreadNativeSyncHookForTesting failResetFinalSync(
				[&resetPublicationWritten, &failedFinalSync](file_handle_t fd) {
					if (resetPublicationWritten.load(std::memory_order_relaxed) &&
							!failedFinalSync.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		ASSERT_GT(backend.enqueueLargeValueReachabilityReclamation(largeValueReclaimOptions()), 0U);
		backend.resumeMaintenance();
		ASSERT_TRUE(waitUntil([&] {
			return backend.maintenanceMetrics().loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
		}));
	}
	EXPECT_TRUE(retirementPublished.load(std::memory_order_relaxed));
	EXPECT_TRUE(resetPublicationWritten.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedFinalSync.load(std::memory_order_relaxed));
	EXPECT_NE(backend.maintenanceFailure(), nullptr);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	const LargeValueReachabilityReclaimReport recovery =
			reopened.reclaimUnreachableLargeValues(largeValueReclaimOptions());
	EXPECT_EQ(recovery.retired_values, 0U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, LargeValueMaintenanceCandidateShortWriteRequiresReopenAndCanRetry) {
	RelocatableStorageBackendOptions configured = options();
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	configured.maintenance = maintenance;
	RelocatableStorageBackend backend(configured);
	backend.open();
	const std::string payload(8U * 1024U, 's');
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	backend.deleteLogicalNodes({{nodes.front(), false, 0U}});
	backend.pauseMaintenance();

	std::atomic<bool> candidateWriteObserved = false;
	{
		StorageIO::ScopedAllThreadNativeWriteHookForTesting shortCandidate(
				[&candidateWriteObserved](const file_handle_t fd, const void *bytes, const size_t byteCount,
										  const pwrite_off_t offset) {
					const bool isSuperblock =
							offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U);
					if (byteCount == RSS_PAGE_SIZE && !isSuperblock &&
						!candidateWriteObserved.exchange(true, std::memory_order_relaxed)) {
						return static_cast<pwrite_ssize_t>(byteCount - 1U);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		ASSERT_GT(backend.enqueueLargeValueReachabilityReclamation(largeValueReclaimOptions()), 0U);
		backend.resumeMaintenance();
		ASSERT_TRUE(waitUntil([&] {
			return backend.maintenanceMetrics().loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
		}));
	}
	EXPECT_TRUE(candidateWriteObserved.load(std::memory_order_relaxed));
	EXPECT_NE(backend.maintenanceFailure(), nullptr);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_NO_THROW(static_cast<void>(reopened.reclaimUnreachableLargeValues(largeValueReclaimOptions())));
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, LargeValueMaintenanceCandidateDiskFullRequiresReopenAndCanRetry) {
	RelocatableStorageBackendOptions configured = options();
	RelocatableMaintenanceOptions maintenance;
	maintenance.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
	configured.maintenance = maintenance;
	RelocatableStorageBackend backend(configured);
	backend.open();
	const std::string payload(8U * 1024U, 'd');
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(nodes.size(), 1U);
	backend.deleteLogicalNodes({{nodes.front(), false, 0U}});
	backend.pauseMaintenance();

	std::atomic<bool> candidateWriteObserved = false;
	{
		StorageIO::ScopedAllThreadNativeWriteHookForTesting diskFullCandidate(
				[&candidateWriteObserved](const file_handle_t fd, const void *bytes, const size_t byteCount,
										  const pwrite_off_t offset) {
					const bool isSuperblock =
							offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U);
					if (byteCount == RSS_PAGE_SIZE && !isSuperblock &&
						!candidateWriteObserved.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		ASSERT_GT(backend.enqueueLargeValueReachabilityReclamation(largeValueReclaimOptions()), 0U);
		backend.resumeMaintenance();
		ASSERT_TRUE(waitUntil([&] {
			return backend.maintenanceMetrics().loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED;
		}));
	}
	EXPECT_TRUE(candidateWriteObserved.load(std::memory_order_relaxed));
	EXPECT_NE(backend.maintenanceFailure(), nullptr);
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"BlockedUntilReopen"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_NO_THROW(static_cast<void>(reopened.reclaimUnreachableLargeValues(largeValueReclaimOptions())));
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   ReclaimingReplacedLargeValueKeepsThePinnedSnapshotReadableAndPersistsTheReplacement) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::string originalPayload(8U * 1024U, 'a');
	const std::string replacementPayload(8U * 1024U, 'b');
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(originalPayload)}}}});
	ASSERT_EQ(nodes.size(), 1U);

	std::unique_ptr<LogicalGraphReadSnapshot> oldSnapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(oldSnapshot, nullptr);
	EXPECT_EQ(oldSnapshot->readNode(nodes.front()).properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"body", graph::PropertyValue(originalPayload)}}));

	backend.replaceLogicalNodeProperties({{{nodes.front(), {{"body", graph::PropertyValue(replacementPayload)}}}}});
	const LargeValueReachabilityReclaimReport report =
			backend.reclaimUnreachableLargeValues(largeValueReclaimOptions());
	EXPECT_EQ(report.retired_values, 1U);
	EXPECT_GT(report.retired_manifest_pages, 0U);
	EXPECT_GT(report.retired_data_pages, 0U);

	// Retirement may publish while this old root is pinned, but its frames cannot
	// be reused until that snapshot is released.
	EXPECT_EQ(oldSnapshot->readNode(nodes.front()).properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"body", graph::PropertyValue(originalPayload)}}));
	std::unique_ptr<LogicalGraphReadSnapshot> currentSnapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(currentSnapshot->readNode(nodes.front()).properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"body", graph::PropertyValue(replacementPayload)}}));
	currentSnapshot.reset();
	oldSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> reopenedSnapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(reopenedSnapshot->readNode(nodes.front()).properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"body", graph::PropertyValue(replacementPayload)}}));
	reopenedSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest,
	   ReclaimingOneOfTwoDuplicateLargeValueOwnersRetainsContentUntilTheFinalOwnerIsDeleted) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const std::string payload(8U * 1024U, 'd');
	const std::vector<uint64_t> first =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	const std::vector<uint64_t> second =
			backend.createLogicalNodes({{{"Document"}, {{"body", graph::PropertyValue(payload)}}}});
	ASSERT_EQ(first.size(), 1U);
	ASSERT_EQ(second.size(), 1U);

	backend.deleteLogicalNodes({{first.front(), false, 0U}});
	const LargeValueReachabilityReclaimReport retained =
			backend.reclaimUnreachableLargeValues(largeValueReclaimOptions());
	EXPECT_EQ(retained.retired_values, 0U);
	EXPECT_EQ(retained.reachable_values, 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> retainedSnapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(retainedSnapshot->readNode(second.front()).properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"body", graph::PropertyValue(payload)}}));
	retainedSnapshot.reset();

	backend.deleteLogicalNodes({{second.front(), false, 0U}});
	const LargeValueReachabilityReclaimReport retired =
			backend.reclaimUnreachableLargeValues(largeValueReclaimOptions());
	EXPECT_EQ(retired.retired_values, 1U);
	EXPECT_GT(retired.retired_manifest_pages, 0U);
	EXPECT_GT(retired.retired_data_pages, 0U);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_EQ(reopened.reclaimUnreachableLargeValues(largeValueReclaimOptions()).retired_values, 0U);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, OwnsAnOptInTypedMaintenanceLifecycle) {
	RelocatableStorageBackendOptions enabled = options();
	enabled.maintenance = RelocatableMaintenanceOptions{};
	// This lifecycle case asserts the single recovery task admitted at open;
	// use the one-slot policy explicitly so the independent adjacency sweep is
	// not admitted by the production startup policy.
	enabled.maintenance->loop.maximum_outstanding_tasks = 1U;
	RelocatableStorageBackend backend(enabled);
	backend.open();
	ASSERT_TRUE(backend.maintenanceEnabled());
	EXPECT_EQ(backend.maintenanceMetrics().loop.state, RelocatableMaintenanceLoopState::VMLS_RUNNING);
	EXPECT_EQ(backend.maintenanceFailure(), nullptr);
	// The worker may legitimately issue metadata reads between resetIoStats() and
	// ioStats(). Pause first so this verifies reset semantics rather than racing
	// the opt-in maintenance loop.
	backend.pauseMaintenance();
	EXPECT_EQ(backend.maintenanceMetrics().loop.state, RelocatableMaintenanceLoopState::VMLS_PAUSED);
	const BufferPoolStats bufferBeforeReset = backend.ioStats().buffer_pool;
	backend.resetIoStats();
	const RelocatableStorageIOStats afterReset = backend.ioStats();
	EXPECT_EQ(afterReset.data, (StorageIOStats{}));
	EXPECT_EQ(afterReset.buffer_pool.capacity_bytes, bufferBeforeReset.capacity_bytes);
	EXPECT_EQ(afterReset.buffer_pool.resident_bytes, bufferBeforeReset.resident_bytes);
	EXPECT_EQ(afterReset.buffer_pool.pinned_bytes, bufferBeforeReset.pinned_bytes);
	EXPECT_EQ(afterReset.buffer_pool.dirty_bytes, bufferBeforeReset.dirty_bytes);
	EXPECT_EQ(afterReset.buffer_pool.hits, 0U);
	EXPECT_EQ(afterReset.buffer_pool.misses, 0U);
	EXPECT_EQ(afterReset.buffer_pool.evictions, 0U);
	EXPECT_EQ(afterReset.buffer_pool.prefetch_requests, 0U);
	EXPECT_EQ(afterReset.buffer_pool.prefetch_hits, 0U);
	EXPECT_EQ(afterReset.buffer_pool.disk_read_bytes, 0U);
	EXPECT_EQ(afterReset.buffer_pool.disk_write_bytes, 0U);
	backend.resumeMaintenance();
	EXPECT_EQ(backend.maintenanceMetrics().loop.state, RelocatableMaintenanceLoopState::VMLS_RUNNING);
	RelocatableReadSnapshot pinned = backend.openSnapshot();
	EXPECT_THROW(backend.close(), std::logic_error);
	EXPECT_TRUE(backend.isOpen());
	EXPECT_EQ(backend.maintenanceMetrics().loop.state, RelocatableMaintenanceLoopState::VMLS_RUNNING);
	pinned.release();
	backend.pauseMaintenance();
	EXPECT_EQ(backend.maintenanceMetrics().loop.state, RelocatableMaintenanceLoopState::VMLS_PAUSED);
	EXPECT_THROW(static_cast<void>(backend.enqueuePropertyPageCompaction({}, SegmentId(2U), 1U)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.enqueueBTreePageCompaction({}, SegmentId(2U), 1U)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.enqueueVectorPageCompaction({}, SegmentId(2U), 1U)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.enqueueVectorManifestCompaction({}, SegmentId(2U), 1U)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.enqueueEntityPageCompaction({}, SegmentId(2U), 1U)), std::invalid_argument);
	EXPECT_THROW(
			static_cast<void>(backend.enqueueCodebookExtentRelocation(0U, {}, PhysicalSlotId(2U), PhysicalSlotId(2U))),
			std::invalid_argument);
	// Open recovery owns one queued maintenance slot until the caller resumes it.
	EXPECT_EQ(backend.cancelQueuedMaintenance(), 1U);
	backend.resumeMaintenance();
	EXPECT_EQ(backend.maintenanceMetrics().loop.state, RelocatableMaintenanceLoopState::VMLS_RUNNING);
	backend.close();
	EXPECT_FALSE(backend.maintenanceEnabled());
}

TEST_F(RelocatableStorageBackendSchemaTest, NativeMaintenanceControllerReportsAndChangesTheActualLifecycle) {
	RelocatableStorageBackendOptions enabled = options();
	enabled.maintenance = RelocatableMaintenanceOptions{};
	RelocatableStorageBackend backend(enabled);
	backend.open();
	EXPECT_TRUE(backend.nativeMaintenanceAvailable());
	EXPECT_TRUE(backend.nativeMaintenanceEnabled());

	backend.setNativeMaintenanceEnabled(true);
	EXPECT_TRUE(backend.nativeMaintenanceEnabled());
	backend.setNativeMaintenanceEnabled(false);
	EXPECT_FALSE(backend.nativeMaintenanceEnabled());
	EXPECT_EQ(backend.maintenanceMetrics().loop.state, RelocatableMaintenanceLoopState::VMLS_PAUSED);
	backend.setNativeMaintenanceEnabled(false);
	backend.setNativeMaintenanceEnabled(true);
	EXPECT_TRUE(backend.nativeMaintenanceEnabled());
	backend.close();

	RelocatableStorageBackend withoutMaintenance(options());
	withoutMaintenance.open();
	EXPECT_FALSE(withoutMaintenance.nativeMaintenanceAvailable());
	EXPECT_FALSE(withoutMaintenance.nativeMaintenanceEnabled());
	EXPECT_THROW(withoutMaintenance.setNativeMaintenanceEnabled(true), std::logic_error);
	withoutMaintenance.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, AppliesAndRecoversDurableCompactionEnablementThroughNativeConfiguration) {
	RelocatableStorageBackendOptions enabled = options();
	enabled.maintenance = RelocatableMaintenanceOptions{};
	RelocatableStorageBackend backend(enabled);
	backend.open();
	graph::query::QueryEngine engine(
			std::shared_ptr<RelocatableStorageBackend>(&backend, [](RelocatableStorageBackend *) {}),
			std::shared_ptr<RelocatableStorageBackend>(&backend, [](RelocatableStorageBackend *) {}));

	static_cast<void>(engine.execute("CALL dbms.setConfig('storage.compaction.enabled', false)"));
	EXPECT_EQ(backend.maintenanceMetrics().loop.state, RelocatableMaintenanceLoopState::VMLS_PAUSED);
	EXPECT_EQ(engine.execute("CALL dbms.getConfig('storage.compaction.enabled')")
					  .getRows()
					  .front()
					  .at("value")
					  .asPrimitive(),
			  graph::PropertyValue(false));
	backend.close();

	RelocatableStorageBackend reopened(enabled);
	reopened.open();
	EXPECT_EQ(reopened.maintenanceMetrics().loop.state, RelocatableMaintenanceLoopState::VMLS_PAUSED);
	graph::query::QueryEngine reopenedEngine(
			std::shared_ptr<RelocatableStorageBackend>(&reopened, [](RelocatableStorageBackend *) {}),
			std::shared_ptr<RelocatableStorageBackend>(&reopened, [](RelocatableStorageBackend *) {}));
	static_cast<void>(reopenedEngine.execute("CALL dbms.setConfig('storage.compaction.enabled', true)"));
	EXPECT_EQ(reopened.maintenanceMetrics().loop.state, RelocatableMaintenanceLoopState::VMLS_RUNNING);
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsNonBooleanDurableCompactionConfigurationBeforeStartingMaintenance) {
	RelocatableStorageBackendOptions enabled = options();
	enabled.maintenance = RelocatableMaintenanceOptions{};
	RelocatableStorageBackend backend(enabled);
	backend.open();

	// The direct backend configuration API is public, so reopen must protect the
	// maintenance lifecycle from a persisted value with the wrong logical type.
	backend.setConfiguration("storage.compaction.enabled", graph::PropertyValue(int64_t{1}));
	backend.close();

	RelocatableStorageBackend reopened(enabled);
	EXPECT_THROW(reopened.open(), FormatException);
	EXPECT_FALSE(reopened.isOpen());
}

TEST_F(RelocatableStorageBackendSchemaTest, RejectsBackgroundMaintenanceOnAReadOnlyOpen) {
	RelocatableStorageBackendOptions readOnly = options();
	readOnly.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
	readOnly.maintenance = RelocatableMaintenanceOptions{};
	RelocatableStorageBackend backend(readOnly);
	EXPECT_THROW(backend.open(), std::invalid_argument);
	EXPECT_FALSE(backend.isOpen());
}

TEST_F(RelocatableStorageBackendSchemaTest, ReopensMaintenanceWithMixedScalarAndVectorIndexes) {
	RelocatableStorageBackend writer(options());
	writer.open();
	const LogicalGraphIndexDefinition scalar =
			writer.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	const LogicalGraphIndexDefinition vector =
			writer.createLogicalNodeVectorIndex({"person_embedding", "Person", "embedding", 2U, "l2"});
	writer.close();

	RelocatableStorageBackendOptions maintained = options();
	maintained.maintenance = RelocatableMaintenanceOptions{};
	RelocatableStorageBackend reopened(maintained);
	ASSERT_NO_THROW(reopened.open());
	EXPECT_TRUE(reopened.maintenanceEnabled());
	EXPECT_EQ(reopened.listLogicalIndexes(8U), (std::vector<LogicalGraphIndexDefinition>{scalar, vector}));
	reopened.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, EnforcesTheVectorDdlTypedPageBudgetBeforePublication) {
	const graph::PropertyValue vector(
			std::vector<graph::PropertyValue>{graph::PropertyValue(1.0), graph::PropertyValue(2.0)});
	RelocatableStorageBackend backend(options());
	backend.open();
	static_cast<void>(backend.createLogicalNodes({{{"Person"}, {{"embedding", vector}}}}));
	backend.close();

	RelocatableStorageBackendOptions constrainedOptions = options();
	constrainedOptions.maximum_online_index_pages = 2U;
	RelocatableStorageBackend constrained(constrainedOptions);
	constrained.open();
	EXPECT_THROW(static_cast<void>(constrained.createLogicalNodeVectorIndex(
						 {"person_embedding", "Person", "embedding", 2U, "l2"})),
				 FormatException);
	EXPECT_TRUE(constrained.listLogicalIndexes(8U).empty());
	constrained.close();
}

TEST_F(RelocatableStorageBackendSchemaTest, DropsCanonicalIndexAndRecreatesItsLogicalSchema) {
	RelocatableStorageBackend backend(options());
	backend.open();
	const LogicalGraphIndexDefinition created = backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	EXPECT_EQ(backend.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{created});
	EXPECT_TRUE(backend.dropLogicalIndexByName(created.name));
	EXPECT_FALSE(backend.dropLogicalIndexByName(created.name));
	EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());
	const ScalarIndexTreeReclaimReport reclamation =
			backend.reclaimObsoleteScalarIndexTrees({8U, 8U, 16U, 16U, 16U, 16U});
	EXPECT_EQ(reclamation.dropped_index_roots_retired, 0U);
	EXPECT_EQ(reclamation.index_tree_pages_retired, 0U);
	backend.close();

	RelocatableStorageBackend reopened(options());
	reopened.open();
	EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
	const LogicalGraphIndexDefinition recreated =
			reopened.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	EXPECT_NE(recreated.index_id, created.index_id);
	EXPECT_EQ(recreated.name, created.name);
	EXPECT_EQ(recreated.label, created.label);
	EXPECT_EQ(recreated.property_keys, created.property_keys);
	EXPECT_EQ(reopened.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{recreated});
	reopened.close();
}
