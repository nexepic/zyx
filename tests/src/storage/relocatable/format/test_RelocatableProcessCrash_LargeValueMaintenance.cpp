/**
 * @file test_RelocatableProcessCrash_LargeValueMaintenance.cpp
 * @brief Real process-crash recovery for LargeValue maintenance.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "storage/relocatable/format/RelocatableProcessCrashSupport.hpp"

using namespace graph::storage;

namespace {

	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_PUBLICATION =
			"large-value-maintenance-before-publication";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_PUBLICATION =
			"large-value-maintenance-after-publication";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_JOURNAL_PUBLICATION =
			"large-value-maintenance-before-journal-publication";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_JOURNAL_PUBLICATION =
			"large-value-maintenance-after-journal-publication";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_LATER_JOURNAL_PUBLICATION =
			"large-value-maintenance-before-later-journal-publication";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_LATER_JOURNAL_PUBLICATION =
			"large-value-maintenance-after-later-journal-publication";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_STATE_OWNER_JOURNAL_PUBLICATION =
			"large-value-maintenance-before-state-owner-journal-publication";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_STATE_OWNER_JOURNAL_PUBLICATION =
			"large-value-maintenance-after-state-owner-journal-publication";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_JOURNAL_RESET =
			"large-value-maintenance-before-journal-reset";
	constexpr std::string_view CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_JOURNAL_RESET =
			"large-value-maintenance-after-journal-reset";

	using graph::test::relocatable_crash::CrashChild;
	using graph::test::relocatable_crash::waitToBeKilled;
	using graph::test::relocatable_crash::writeFailureMarker;
	using graph::test::relocatable_crash::writeMarker;

	[[nodiscard]] RelocatableMaintenanceOptions maintenanceOptions() {
		RelocatableMaintenanceOptions options;
		// Opening may enqueue independent recovery work. Leave enough bounded
		// admission for the explicit LargeValue task without changing its per-cycle cap.
		options.loop.maximum_outstanding_tasks = 4U;
		options.loop.maximum_outstanding_rewrite_bytes = 4U * RSS_SEGMENT_SIZE;
		options.loop.minimum_cycle_interval = std::chrono::milliseconds(1);
		return options;
	}

	[[nodiscard]] LargeValueReachabilityReclaimOptions reclaimOptions() {
		LargeValueReachabilityReclaimOptions options;
		options.maximum_manifest_locations = 16U;
		options.maximum_manifest_metadata_pages = 16U;
		options.maximum_manifest_nodes = 16U;
		options.maximum_owner_locations = 16U;
		options.maximum_owner_metadata_pages = 16U;
		options.maximum_data_pages = 16U;
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

	[[nodiscard]] bool isSuperblockWrite(pwrite_off_t offset) {
		return offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U);
	}

} // namespace

namespace graph::test {

	int runRelocatableLargeValueMaintenanceCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
													  const std::filesystem::path &markerPath) {
		if (mode != CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_PUBLICATION &&
			mode != CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_PUBLICATION &&
			mode != CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_JOURNAL_PUBLICATION &&
			mode != CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_JOURNAL_PUBLICATION &&
			mode != CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_LATER_JOURNAL_PUBLICATION &&
			mode != CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_LATER_JOURNAL_PUBLICATION &&
			mode != CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_STATE_OWNER_JOURNAL_PUBLICATION &&
			mode != CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_STATE_OWNER_JOURNAL_PUBLICATION &&
			mode != CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_JOURNAL_RESET &&
			mode != CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_JOURNAL_RESET) {
			return -1;
		}
		try {
			RelocatableStorageBackendOptions options;
			options.data_path = databasePath;
			options.open_mode = OpenMode::OPEN_EXISTING_FILE;
			options.maintenance = maintenanceOptions();
			RelocatableStorageBackend backend(std::move(options));
			backend.open();
			backend.pauseMaintenance();
			PhysicalPageRef priorManifestDirectoryRoot;
			PhysicalPageRef priorStateDirectoryRoot;
			{
				auto data =
						std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
				if (!data->is_open())
					return 108;
				auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
				const Superblock current = SuperblockFile(io).recover();
				priorManifestDirectoryRoot = current.large_value_manifest_directory_root;
				priorStateDirectoryRoot = current.state_directory_root;
			}

			const bool afterRetirementPublication = mode == CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_PUBLICATION;
			const bool afterJournalPublication = mode == CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_JOURNAL_PUBLICATION;
			const bool beforeLaterJournalPublication =
					mode == CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_LATER_JOURNAL_PUBLICATION;
			const bool afterLaterJournalPublication =
					mode == CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_LATER_JOURNAL_PUBLICATION;
			const bool beforeStateOwnerJournalPublication =
					mode == CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_STATE_OWNER_JOURNAL_PUBLICATION;
			const bool afterStateOwnerJournalPublication =
					mode == CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_STATE_OWNER_JOURNAL_PUBLICATION;
			const bool beforeJournalPublication = mode == CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_JOURNAL_PUBLICATION;
			const bool beforeJournalReset = mode == CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_JOURNAL_RESET;
			const bool afterJournalReset = mode == CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_JOURNAL_RESET;
			const bool journalPhaseCrash = beforeJournalPublication || afterJournalPublication ||
					beforeLaterJournalPublication || afterLaterJournalPublication ||
					beforeStateOwnerJournalPublication || afterStateOwnerJournalPublication || beforeJournalReset ||
					afterJournalReset;
			std::atomic<bool> observedBoundary{false};
			std::atomic<bool> retirementPublished{false};
			std::atomic<uint32_t> journalPublications{0U};
			StorageIO::ScopedAllThreadNativeWriteHookForTesting pauseAtMaintenanceBoundary(
					[afterRetirementPublication, afterJournalPublication, beforeLaterJournalPublication,
					 afterLaterJournalPublication, beforeStateOwnerJournalPublication, afterStateOwnerJournalPublication,
					 beforeJournalPublication, beforeJournalReset, afterJournalReset, journalPhaseCrash,
					 priorManifestDirectoryRoot, priorStateDirectoryRoot, &observedBoundary, &retirementPublished,
					 &journalPublications,
					 &markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (byteCount == RSS_PAGE_SIZE && isSuperblockWrite(offset) &&
							!observedBoundary.load(std::memory_order_relaxed)) {
							try {
								const Superblock candidate = SuperblockCodec::decode(
										std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
								const bool retirement =
										candidate.large_value_manifest_directory_root != priorManifestDirectoryRoot;
								const bool journalPublication =
										candidate.state_directory_root != priorStateDirectoryRoot && !retirementPublished.load(std::memory_order_relaxed);
								const bool journalReset = retirementPublished.load(std::memory_order_relaxed) &&
										candidate.state_directory_root != priorStateDirectoryRoot;
								if (retirement && !journalPhaseCrash) {
									observedBoundary.store(true, std::memory_order_relaxed);
									if (!afterRetirementPublication) {
										writeMarker(markerPath);
										waitToBeKilled();
									}
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount))
										return written;
									writeMarker(markerPath);
									waitToBeKilled();
								}
								if (retirement) {
									retirementPublished.store(true, std::memory_order_relaxed);
								}
								const uint32_t journalPublicationCount =
										journalPublication
												? journalPublications.fetch_add(1U, std::memory_order_relaxed) + 1U
												: 0U;
								const bool initialJournalBoundary = journalPublicationCount == 1U &&
										(beforeJournalPublication || afterJournalPublication);
								const bool laterJournalBoundary = journalPublicationCount == 2U &&
										(beforeLaterJournalPublication || afterLaterJournalPublication);
								const bool stateOwnerJournalBoundary = journalPublicationCount == 3U &&
										(beforeStateOwnerJournalPublication || afterStateOwnerJournalPublication);
								const bool journalResetBoundary = journalReset && (beforeJournalReset || afterJournalReset);
								if (initialJournalBoundary || laterJournalBoundary || stateOwnerJournalBoundary ||
									journalResetBoundary) {
									observedBoundary.store(true, std::memory_order_relaxed);
									if (beforeJournalPublication || beforeLaterJournalPublication ||
										beforeStateOwnerJournalPublication || beforeJournalReset) {
										writeMarker(markerPath);
										waitToBeKilled();
									}
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount))
										return written;
									writeMarker(markerPath);
									waitToBeKilled();
								}
							} catch (const FormatException &) {
								// Other file writes are outside this typed maintenance boundary.
							}
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			if (backend.enqueueLargeValueReachabilityReclamation(reclaimOptions()) == 0U)
				return 109;
			backend.resumeMaintenance();
			for (size_t attempt = 0U; attempt < 10'000U && !observedBoundary.load(std::memory_order_relaxed);
				 ++attempt) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return 102;
		} catch (const std::exception &error) {
			writeFailureMarker(markerPath, error.what());
			return 101;
		}
	}

} // namespace graph::test

class RelocatableProcessCrashLargeValueMaintenanceTest : public ::testing::Test {
protected:
	std::filesystem::path databasePath;
	std::filesystem::path markerPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		databasePath = std::filesystem::temp_directory_path() /
					   ("relocatable_large_value_maintenance_" + to_string(uuid) + ".db");
		markerPath = databasePath.string() + ".marker";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove(databasePath, error);
		std::filesystem::remove(markerPath, error);
		std::filesystem::remove(markerPath.string() + ".error", error);
	}

	void createDatabaseWithUnreachableLargeValue() const {
		RelocatableStorageBackendOptions options;
		options.data_path = databasePath;
		options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
		auto created = RelocatableStorageBackend::initializeNew(std::move(options));
		if (created == nullptr)
			throw std::runtime_error("failed to initialize LargeValue maintenance crash database");
		created->open();
		const std::string payload(8U * 1024U, 'm');
		const std::vector<uint64_t> nodes =
				created->createLogicalNodes({{{"MaintenanceDocument"}, {{"body", graph::PropertyValue(payload)}}}});
		if (nodes.size() != 1U)
			throw std::runtime_error("failed to create LargeValue maintenance owner");
		created->deleteLogicalNodes({{nodes.front(), false, 0U}});
		created->close();
	}

	[[nodiscard]] LargeValueReachabilityReclaimReport reclaimAfterCrash() const {
		RelocatableStorageBackendOptions options;
		options.data_path = databasePath;
		options.open_mode = OpenMode::OPEN_EXISTING_FILE;
		RelocatableStorageBackend reopened(std::move(options));
		reopened.open();
		EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
		const LargeValueReachabilityReclaimReport report = reopened.reclaimUnreachableLargeValues(reclaimOptions());
		EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
		reopened.close();
		return report;
	}

	void resumeMaintenanceAfterCrash() const {
		RelocatableStorageBackendOptions options;
		options.data_path = databasePath;
		options.open_mode = OpenMode::OPEN_EXISTING_FILE;
		options.maintenance = maintenanceOptions();
		RelocatableStorageBackend reopened(std::move(options));
		reopened.open();
		reopened.pauseMaintenance();
		ASSERT_GT(reopened.enqueueLargeValueReachabilityReclamation(reclaimOptions()), 0U);
		reopened.resumeMaintenance();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline) {
			const RelocatableMaintenanceMetrics metrics = reopened.maintenanceMetrics();
			if (metrics.loop.reclaimed_physical_bytes >= RSS_PAGE_SIZE ||
				metrics.loop.state == RelocatableMaintenanceLoopState::VMLS_FAULTED) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (const std::exception_ptr failure = reopened.maintenanceFailure(); failure) {
			try {
				std::rethrow_exception(failure);
			} catch (const std::exception &error) {
				FAIL() << "reopened LargeValue maintenance journal failed: " << error.what();
			}
		}
		EXPECT_GE(reopened.maintenanceMetrics().loop.reclaimed_physical_bytes, RSS_PAGE_SIZE);
		EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
		reopened.close();
	}
};

TEST_F(RelocatableProcessCrashLargeValueMaintenanceTest,
	   KillBeforeLargeValueMaintenancePublicationPreservesOldGenerationAndAllowsIdempotentRetry) {
	createDatabaseWithUnreachableLargeValue();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	const LargeValueReachabilityReclaimReport report = reclaimAfterCrash();
	EXPECT_EQ(report.retired_values, 1U);
	EXPECT_GT(report.retired_manifest_pages, 0U);
	EXPECT_GT(report.retired_data_pages, 0U);
}

TEST_F(RelocatableProcessCrashLargeValueMaintenanceTest,
	   KillAfterLargeValueMaintenancePublicationRecoversNewGenerationAndRemainsIdempotent) {
	createDatabaseWithUnreachableLargeValue();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	const LargeValueReachabilityReclaimReport report = reclaimAfterCrash();
	EXPECT_EQ(report.retired_values, 0U);
}

TEST_F(RelocatableProcessCrashLargeValueMaintenanceTest,
	   KillBeforeInitialJournalPublicationLeavesNoEvidenceAndAllowsDirectRetry) {
	createDatabaseWithUnreachableLargeValue();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_JOURNAL_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	const LargeValueReachabilityReclaimReport report = reclaimAfterCrash();
	EXPECT_EQ(report.retired_values, 1U);
}

TEST_F(RelocatableProcessCrashLargeValueMaintenanceTest,
	   KillAfterInitialJournalPublicationResumesFromDurableEvidence) {
	createDatabaseWithUnreachableLargeValue();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_JOURNAL_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	resumeMaintenanceAfterCrash();
	EXPECT_EQ(reclaimAfterCrash().retired_values, 0U);
}

TEST_F(RelocatableProcessCrashLargeValueMaintenanceTest,
	   KillBeforeLaterJournalPublicationResumesFromPriorDurableOwnerScanEvidence) {
	createDatabaseWithUnreachableLargeValue();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_LATER_JOURNAL_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	resumeMaintenanceAfterCrash();
	EXPECT_EQ(reclaimAfterCrash().retired_values, 0U);
}

TEST_F(RelocatableProcessCrashLargeValueMaintenanceTest,
	   KillAfterLaterJournalPublicationResumesFromDurableOwnerScanEvidence) {
	createDatabaseWithUnreachableLargeValue();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_LATER_JOURNAL_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	resumeMaintenanceAfterCrash();
	EXPECT_EQ(reclaimAfterCrash().retired_values, 0U);
}

TEST_F(RelocatableProcessCrashLargeValueMaintenanceTest,
	   KillBeforeStateOwnerJournalPublicationResumesFromPriorOwnerScanEvidence) {
	createDatabaseWithUnreachableLargeValue();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_STATE_OWNER_JOURNAL_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	resumeMaintenanceAfterCrash();
	EXPECT_EQ(reclaimAfterCrash().retired_values, 0U);
}

TEST_F(RelocatableProcessCrashLargeValueMaintenanceTest,
	   KillAfterStateOwnerJournalPublicationResumesFromDurableReclaimEvidence) {
	createDatabaseWithUnreachableLargeValue();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_STATE_OWNER_JOURNAL_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	resumeMaintenanceAfterCrash();
	EXPECT_EQ(reclaimAfterCrash().retired_values, 0U);
}

TEST_F(RelocatableProcessCrashLargeValueMaintenanceTest,
	   KillBeforeJournalResetAfterRetirementRecoversThePublishedGeneration) {
	createDatabaseWithUnreachableLargeValue();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_MAINTENANCE_BEFORE_JOURNAL_RESET);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	const LargeValueReachabilityReclaimReport report = reclaimAfterCrash();
	EXPECT_EQ(report.retired_values, 0U);
}

TEST_F(RelocatableProcessCrashLargeValueMaintenanceTest,
	   KillAfterJournalResetAfterRetirementRecoversThePublishedGeneration) {
	createDatabaseWithUnreachableLargeValue();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_LARGE_VALUE_MAINTENANCE_AFTER_JOURNAL_RESET);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	const LargeValueReachabilityReclaimReport report = reclaimAfterCrash();
	EXPECT_EQ(report.retired_values, 0U);
}
