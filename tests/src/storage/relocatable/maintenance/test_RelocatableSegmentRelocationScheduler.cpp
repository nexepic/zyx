/**
 * @file test_RelocatableSegmentRelocationScheduler.cpp
 * @brief Admission, accounting, and typed-executor tests for relocatable relocation scheduling.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableSegmentRelocationScheduler.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	RelocatableSegmentRelocationBudget oneSegmentBudget() {
		RelocatableSegmentRelocationBudget result;
		result.maximum_segment_moves = 1;
		result.maximum_rewrite_bytes = RSS_SEGMENT_SIZE;
		return result;
	}

} // namespace

class RelocatableSegmentRelocationSchedulerTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_segment_relocation_scheduler_" + to_string(uuid));
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

TEST_F(RelocatableSegmentRelocationSchedulerTest, SchedulesCodebookExtentRelocationWithoutPayloadDispatch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	PhysicalSegmentAllocator physical(io, *metadata);
	VectorCodebookStore codebooks(physical, *metadata);
	VectorCodebookWriteRequest codebook;
	codebook.codebook_id = 17;
	codebook.manifest_segment_id = SegmentId(71);
	codebook.generation = Generation(1);
	codebook.element_type = VectorCodebookElementType::VCET_FLOAT32;
	codebook.dimensions = 2;
	codebook.subspace_count = 1;
	codebook.centroid_count = 3;
	const std::vector<std::byte> codebookPayload(24U, std::byte{0x31});
	static_cast<void>(codebooks.writeImmutable(codebook, codebookPayload, PhysicalSlotId(40), PhysicalSlotId(40),
											   LogSequenceNumber(2)));
	RelocatableCompactionCoordinator codebookCoordinator(pages, codebooks, physical, *metadata);
	RelocatableSegmentRelocationScheduler codebookScheduler(*metadata);
	const RelocatableSegmentRelocationTask codebookTask = RelocatableSegmentRelocationTask::forCodebookExtent(
			codebookCoordinator, {17, PhysicalSlotId(42), PhysicalSlotId(42), LogSequenceNumber(3)});
	const RelocatableSegmentRelocationRunResult codebookResult = codebookScheduler.run(codebookTask, oneSegmentBudget());
	EXPECT_EQ(codebookResult.status, RelocatableSegmentRelocationRunStatus::VSRRS_COMPLETED);
	EXPECT_TRUE(codebookResult.compaction.source_released);
	MetadataSnapshotPin codebookSnapshot = metadata->openSnapshot();
	EXPECT_EQ(codebooks.read(codebookSnapshot, 17).payload, codebookPayload);
	EXPECT_EQ(metadata
				  ->findExtentLocation(codebookSnapshot, VectorCodebookExtentIdCodec::manifestExtentId(17U))
				  .generation,
			  Generation(2));
	codebookSnapshot.release();

}

TEST_F(RelocatableSegmentRelocationSchedulerTest, RejectsPressureAndTracksDeferredSourceReleaseWithoutExecutingAgain) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	uint32_t executions = 0;
	RelocatableSegmentRelocationScheduler scheduler(*metadata);
	const RelocatableSegmentRelocationTask task{
			LogSequenceNumber(2), [&executions](LogSequenceNumber, const MetadataPublishOptions &) {
				++executions;
				return RelocatableSegmentRelocationExecution{RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS,
													   true, false, LogSequenceNumber(3)};
			}};
	MetadataSnapshotPin pinned = metadata->openSnapshot();
	RelocatableSegmentRelocationBudget snapshotBudget = oneSegmentBudget();
	snapshotBudget.maximum_active_snapshots = 0;
	EXPECT_EQ(scheduler.run(task, snapshotBudget).status, RelocatableSegmentRelocationRunStatus::VSRRS_SNAPSHOT_PRESSURE);
	EXPECT_EQ(executions, 0U);
	pinned.release();

	RelocatableSegmentRelocationBudget retainedBudget = oneSegmentBudget();
	retainedBudget.maximum_retained_generations = 0;
	EXPECT_EQ(scheduler.run(task, retainedBudget).status,
			  RelocatableSegmentRelocationRunStatus::VSRRS_RETIRED_GENERATION_PRESSURE);
	EXPECT_EQ(executions, 0U);

	RelocatableSegmentRelocationBudget pausedBudget = oneSegmentBudget();
	pausedBudget.maximum_rewrite_bytes = RSS_SEGMENT_SIZE - 1U;
	EXPECT_EQ(scheduler.run(task, pausedBudget).status, RelocatableSegmentRelocationRunStatus::VSRRS_BUDGET_EXHAUSTED);
	EXPECT_EQ(executions, 0U);
	RelocatableSegmentRelocationBudget zeroMoveBudget = oneSegmentBudget();
	zeroMoveBudget.maximum_segment_moves = 0;
	EXPECT_EQ(scheduler.run(task, zeroMoveBudget).status, RelocatableSegmentRelocationRunStatus::VSRRS_BUDGET_EXHAUSTED);
	EXPECT_EQ(executions, 0U);

	const RelocatableSegmentRelocationRunResult deferred = scheduler.run(task, oneSegmentBudget());
	EXPECT_EQ(deferred.status, RelocatableSegmentRelocationRunStatus::VSRRS_SOURCE_RETIRED_AWAITING_SNAPSHOTS);
	EXPECT_EQ(executions, 1U);
	EXPECT_EQ(scheduler.metrics().tracked_retained_generations, 1U);
	EXPECT_EQ(scheduler.refreshObservation().retainedGenerationCount(), 0U);
	EXPECT_EQ(scheduler.metrics().tracked_retained_generations, 0U);
	const RelocatableSegmentRelocationTask capacityTask{
			LogSequenceNumber(4), [](LogSequenceNumber, const MetadataPublishOptions &) {
				return RelocatableSegmentRelocationExecution{RelocatableCompactionStatus::VCS_NO_DESTINATION_CAPACITY};
			}};
	EXPECT_EQ(scheduler.run(capacityTask, oneSegmentBudget()).status,
			  RelocatableSegmentRelocationRunStatus::VSRRS_NO_DESTINATION_CAPACITY);
	EXPECT_EQ(scheduler.metrics().destination_capacity_rejections, 1U);
}

TEST_F(RelocatableSegmentRelocationSchedulerTest, RejectsInvalidExecutorsRequestsAndResults) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	RelocatableSegmentRelocationScheduler scheduler(*metadata);
	EXPECT_THROW(static_cast<void>(scheduler.run({}, oneSegmentBudget())), FormatException);
	const RelocatableSegmentRelocationTask missingExecutor{LogSequenceNumber(2), {}};
	EXPECT_THROW(static_cast<void>(scheduler.run(missingExecutor, oneSegmentBudget())), FormatException);
	const RelocatableSegmentRelocationTask inconsistentResult{
			LogSequenceNumber(2), [](LogSequenceNumber, const MetadataPublishOptions &) {
				return RelocatableSegmentRelocationExecution{RelocatableCompactionStatus::VCS_COMPLETED, false, false,
													   LogSequenceNumber(3)};
			}};
	EXPECT_THROW(static_cast<void>(scheduler.run(inconsistentResult, oneSegmentBudget())), FormatException);
	const RelocatableSegmentRelocationTask unreleasedResult{
			LogSequenceNumber(2), [](LogSequenceNumber, const MetadataPublishOptions &) {
				return RelocatableSegmentRelocationExecution{RelocatableCompactionStatus::VCS_COMPLETED, true, false,
													   LogSequenceNumber(3)};
			}};
	EXPECT_THROW(static_cast<void>(scheduler.run(unreleasedResult, oneSegmentBudget())), FormatException);
	const RelocatableSegmentRelocationTask invalidStatus{
			LogSequenceNumber(2), [](LogSequenceNumber, const MetadataPublishOptions &) {
				return RelocatableSegmentRelocationExecution{static_cast<RelocatableCompactionStatus>(99)};
			}};
	EXPECT_THROW(static_cast<void>(scheduler.run(invalidStatus, oneSegmentBudget())), FormatException);
}
