/**
 * @file test_RelocatablePageCompactionScheduler.cpp
 * @brief Admission, accounting, and pressure tests for relocatable page compaction scheduling.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <fstream>
#include <memory>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/maintenance/RelocatablePageCompactionScheduler.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	PageOwnerHeader propertyOwner(PageId pageId, LogSequenceNumber lsn) {
		return {pageId, 99, PageOwnerKind::POK_PROPERTY, 0, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1), lsn};
	}

	SlottedPagePoolAllocationRequest propertyRequest(SegmentId segmentId, PageId pageId) {
		SlottedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
		result.pool_owner_id = 99;
		result.generation = Generation(1);
		result.page_id = pageId;
		result.page_owner_id = 99;
		result.page_owner_kind = PageOwnerKind::POK_PROPERTY;
		return result;
	}

	SlottedPagePoolAppendRequest propertyAppendRequest(PageId pageId) {
		SlottedPagePoolAppendRequest result;
		result.segment_id = SegmentId(71);
		result.generation = Generation(1);
		result.page_id = pageId;
		result.page_owner_id = 99;
		result.page_owner_kind = PageOwnerKind::POK_PROPERTY;
		return result;
	}

	RelocatablePageCompactionBudget onePageBudget() {
		RelocatablePageCompactionBudget result;
		result.maximum_page_moves = 1;
		result.maximum_rewrite_bytes = RSS_PAGE_SIZE;
		return result;
	}

} // namespace

class RelocatablePageCompactionSchedulerTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_page_compaction_scheduler_" + to_string(uuid));
		walPath = path.string() + ".foreign";
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		walFile = std::make_shared<std::fstream>(walPath,
												 std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		ASSERT_TRUE(walFile->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		walIo = std::make_shared<StorageIO>(walFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		walIo.reset();
		if (file && file->is_open()) {
			file->close();
		}
		if (walFile && walFile->is_open()) {
			walFile->close();
		}
		std::error_code error;
		std::filesystem::remove(path, error);
		std::filesystem::remove(walPath, error);
	}
};

TEST_F(RelocatablePageCompactionSchedulerTest, AdmitsOnePagePerTickAndAccountsForSourceRelease) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	SlottedPage first = SlottedPage::create(propertyOwner(PageId(101), LogSequenceNumber(2)));
	SlottedPage second = SlottedPage::create(propertyOwner(PageId(102), LogSequenceNumber(3)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(71), PageId(101)), first, PhysicalSlotId(40),
								   PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	static_cast<void>(pages.appendPage(propertyAppendRequest(PageId(102)), second, LogSequenceNumber(3)));
	SlottedPage destination = SlottedPage::create(propertyOwner(PageId(200), LogSequenceNumber(4)));
	ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(72), PageId(200)), destination, PhysicalSlotId(41),
								   PhysicalSlotId(41), LogSequenceNumber(4))
						.has_value());
	RelocatableCompactionCoordinator coordinator(pages, *metadata);
	RelocatablePageCompactionScheduler scheduler(*metadata, [&coordinator](const RelocatableSlottedPoolCompactionRequest &request,
																	 const MetadataPublishOptions &options) {
		return coordinator.compactSlottedPool(request, options);
	});
	const RelocatablePageCompactionBudget budget = onePageBudget();
	const RelocatablePageCompactionRunResult firstRun =
			scheduler.run({SegmentId(71), SegmentId(72), 2, LogSequenceNumber(5)}, budget);
	EXPECT_EQ(firstRun.status, RelocatablePageCompactionRunStatus::VPRS_COMPLETED);
	EXPECT_EQ(firstRun.admitted_page_budget, 1U);
	EXPECT_EQ(firstRun.compaction.moved_pages, 1U);
	EXPECT_FALSE(firstRun.compaction.source_empty);
	EXPECT_EQ(firstRun.rewritten_bytes, static_cast<uint64_t>(RSS_PAGE_SIZE));
	EXPECT_EQ(firstRun.reclaimed_physical_bytes, 0U);

	const RelocatablePageCompactionRunResult secondRun =
			scheduler.run({SegmentId(71), SegmentId(72), 2, LogSequenceNumber(6)}, budget);
	EXPECT_EQ(secondRun.status, RelocatablePageCompactionRunStatus::VPRS_COMPLETED);
	EXPECT_TRUE(secondRun.compaction.source_empty);
	EXPECT_TRUE(secondRun.compaction.source_retired);
	EXPECT_TRUE(secondRun.compaction.source_released);
	EXPECT_EQ(secondRun.reclaimed_physical_bytes, static_cast<uint64_t>(RSS_SEGMENT_SIZE));
	const RelocatablePageCompactionMetrics metrics = scheduler.metrics();
	EXPECT_EQ(metrics.runs_started, 2U);
	EXPECT_EQ(metrics.moved_pages, 2U);
	EXPECT_EQ(metrics.rewritten_bytes, static_cast<uint64_t>(2U * RSS_PAGE_SIZE));
	EXPECT_EQ(metrics.retired_source_segments, 1U);
	EXPECT_EQ(metrics.released_source_segments, 1U);
	EXPECT_EQ(metrics.reclaimed_physical_bytes, static_cast<uint64_t>(RSS_SEGMENT_SIZE));
	EXPECT_EQ(metrics.last_observation.retainedGenerationCount(), 0U);
	const RelocatableCompactionObservation refreshed = scheduler.refreshObservation();
	EXPECT_EQ(refreshed.retainedGenerationCount(), 0U);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCatalog(snapshot, SegmentId(71)).state, SegmentState::SGS_OBSOLETE);
}

TEST_F(RelocatablePageCompactionSchedulerTest, RejectsSnapshotAndRetiredGenerationPressureWithoutExecutingWork) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	uint32_t executions = 0;
	RelocatablePageCompactionScheduler scheduler(
			*metadata, [&executions](const RelocatableSlottedPoolCompactionRequest &, const MetadataPublishOptions &) {
				++executions;
				return RelocatableSlottedPoolCompactionResult{};
			});
	MetadataSnapshotPin pinned = metadata->openSnapshot();
	RelocatablePageCompactionBudget snapshotBudget = onePageBudget();
	snapshotBudget.maximum_active_snapshots = 0;
	const RelocatablePageCompactionRunResult snapshotRejected =
			scheduler.run({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(2)}, snapshotBudget);
	EXPECT_EQ(snapshotRejected.status, RelocatablePageCompactionRunStatus::VPRS_SNAPSHOT_PRESSURE);
	EXPECT_EQ(executions, 0U);
	pinned.release();

	RelocatablePageCompactionBudget retainedBudget = onePageBudget();
	retainedBudget.maximum_retained_generations = 0;
	const RelocatablePageCompactionRunResult retainedRejected =
			scheduler.run({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(2)}, retainedBudget);
	EXPECT_EQ(retainedRejected.status, RelocatablePageCompactionRunStatus::VPRS_RETIRED_GENERATION_PRESSURE);
	EXPECT_EQ(executions, 0U);
	const RelocatablePageCompactionMetrics metrics = scheduler.metrics();
	EXPECT_EQ(metrics.snapshot_pressure_rejections, 1U);
	EXPECT_EQ(metrics.retained_generation_rejections, 1U);
}

TEST_F(RelocatablePageCompactionSchedulerTest, ReportsBudgetAndExecutorBackpressureStatuses) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EXPECT_THROW(static_cast<void>(RelocatablePageCompactionScheduler(*metadata, {})), std::invalid_argument);
	RelocatablePageCompactionScheduler scheduler(
			*metadata, [](const RelocatableSlottedPoolCompactionRequest &, const MetadataPublishOptions &) {
				return RelocatableSlottedPoolCompactionResult{RelocatableCompactionStatus::VCS_NO_DESTINATION_CAPACITY};
			});
	EXPECT_THROW(static_cast<void>(scheduler.run({}, onePageBudget())), FormatException);
	RelocatablePageCompactionBudget paused;
	const RelocatablePageCompactionRunResult pausedRun =
			scheduler.run({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(2)}, paused);
	EXPECT_EQ(pausedRun.status, RelocatablePageCompactionRunStatus::VPRS_BUDGET_EXHAUSTED);
	EXPECT_EQ(scheduler.metrics().budget_exhausted_runs, 1U);
	const RelocatablePageCompactionRunResult capacityRun =
			scheduler.run({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(2)}, onePageBudget());
	EXPECT_EQ(capacityRun.status, RelocatablePageCompactionRunStatus::VPRS_NO_DESTINATION_CAPACITY);
	EXPECT_EQ(scheduler.metrics().destination_capacity_rejections, 1U);

	RelocatablePageCompactionScheduler deferredScheduler(
			*metadata, [](const RelocatableSlottedPoolCompactionRequest &, const MetadataPublishOptions &) {
				return RelocatableSlottedPoolCompactionResult{RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS,
														0,
														false,
														true,
														false,
														LogSequenceNumber(2)};
			});
	const RelocatablePageCompactionRunResult deferredRun =
			deferredScheduler.run({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(2)}, onePageBudget());
	EXPECT_EQ(deferredRun.status, RelocatablePageCompactionRunStatus::VPRS_SOURCE_RETIRED_AWAITING_SNAPSHOTS);
	EXPECT_EQ(deferredScheduler.metrics().source_release_deferrals, 1U);
	EXPECT_EQ(deferredScheduler.metrics().tracked_retained_generations, 1U);
	EXPECT_EQ(deferredScheduler.refreshObservation().retainedGenerationCount(), 0U);

	RelocatablePageCompactionScheduler invalidScheduler(
			*metadata, [](const RelocatableSlottedPoolCompactionRequest &, const MetadataPublishOptions &) {
				RelocatableSlottedPoolCompactionResult invalid;
				invalid.status = static_cast<RelocatableCompactionStatus>(99);
				return invalid;
			});
	EXPECT_THROW(static_cast<void>(invalidScheduler.run({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(2)},
														onePageBudget())),
				 FormatException);
}
