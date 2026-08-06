/**
 * @file test_MetadataSnapshotRegistry.cpp
 * @brief Runtime snapshot registration and COW metadata-reclamation tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <utility>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataSnapshotRegistry.hpp"

using namespace graph::storage;
using namespace std::chrono_literals;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		return result;
	}

	void expectSameSuperblock(const Superblock &actual, const Superblock &expected) {
		EXPECT_EQ(actual.generation, expected.generation);
		EXPECT_EQ(actual.checkpoint_lsn, expected.checkpoint_lsn);
		EXPECT_EQ(actual.data_region_offset, expected.data_region_offset);
		EXPECT_EQ(actual.catalog_root, expected.catalog_root);
		EXPECT_EQ(actual.page_directory_root, expected.page_directory_root);
		EXPECT_EQ(actual.extent_directory_root, expected.extent_directory_root);
		EXPECT_EQ(actual.allocator_root, expected.allocator_root);
	}

	void moveAssign(MetadataSnapshotPin &target, MetadataSnapshotPin &&source) { target = std::move(source); }

	CatalogEntry entry(uint64_t physicalSlot, uint64_t generation, uint64_t lsn) {
		CatalogEntry result;
		result.segment_id = SegmentId(1);
		result.physical_slot = PhysicalSlotId(physicalSlot);
		result.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
		result.owner_id = 19;
		result.generation = Generation(generation);
		result.state = SegmentState::SGS_SEALED;
		result.committed_lsn = LogSequenceNumber(lsn);
		return result;
	}

} // namespace

class RelocatableMetadataSnapshotRegistryTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("relocatable_snapshot_registry_" + to_string(uuid) + ".dat");
		file = std::make_shared<std::fstream>(testFilePath,
											  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		if (file && file->is_open()) {
			file->close();
		}
		file.reset();
		std::error_code error;
		std::filesystem::remove(testFilePath, error);
	}
};

TEST_F(RelocatableMetadataSnapshotRegistryTest, PinsDurableRootAndDefersOnlineReclamationUntilRelease) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	CatalogTree tree(pages, allocator);
	MetadataSnapshotRegistry snapshots(bootstrap);

	Superblock generationTwo;
	{
		MetadataWriteGuard writer = snapshots.beginWrite();
		generationTwo = tree.upsertAndPublish(bootstrap, bootstrap.recover(), std::vector{entry(100, 2, 2)},
											  LogSequenceNumber(2));
		snapshots.reclaimAfterPublication(writer, allocator, generationTwo);
	}
	ASSERT_EQ(allocator.stats().allocated_pages, 2U);

	MetadataSnapshotPin snapshot = snapshots.openSnapshot();
	ASSERT_TRUE(snapshot.isValid());
	expectSameSuperblock(snapshot.superblock(), generationTwo);
	EXPECT_EQ(snapshots.activeSnapshotCount(), 1U);

	Superblock generationThree;
	{
		MetadataWriteGuard writer = snapshots.beginWrite();
		generationThree = tree.upsertAndPublish(bootstrap, bootstrap.recover(), std::vector{entry(101, 3, 3)},
												LogSequenceNumber(3));
		snapshots.reclaimAfterPublication(writer, allocator, generationThree);
	}
	EXPECT_EQ(allocator.stats().allocated_pages, 4U);
	EXPECT_EQ(tree.find(snapshot.superblock(), SegmentId(1)).physical_slot, PhysicalSlotId(100));
	EXPECT_EQ(tree.find(generationThree, SegmentId(1)).physical_slot, PhysicalSlotId(101));

	snapshot.release();
	EXPECT_EQ(snapshots.activeSnapshotCount(), 0U);
	{
		MetadataWriteGuard writer = snapshots.beginWrite();
		snapshots.reclaimAfterPublication(writer, allocator, bootstrap.recover());
	}
	EXPECT_EQ(allocator.stats().allocated_pages, 2U);
	EXPECT_EQ(tree.find(generationThree, SegmentId(1)).physical_slot, PhysicalSlotId(101));
}

TEST_F(RelocatableMetadataSnapshotRegistryTest, PinsSupportMoveAssignmentAndCountEveryActiveReader) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataSnapshotRegistry snapshots(bootstrap);

	MetadataSnapshotPin first = snapshots.openSnapshot();
	MetadataSnapshotPin second = snapshots.openSnapshot();
	EXPECT_EQ(snapshots.activeSnapshotCount(), 2U);
	first = std::move(second);
	EXPECT_TRUE(first.isValid());
	EXPECT_FALSE(second.isValid());
	EXPECT_EQ(snapshots.activeSnapshotCount(), 1U);
	first.release();
	first.release();
	EXPECT_EQ(snapshots.activeSnapshotCount(), 0U);
	moveAssign(first, std::move(first));
	EXPECT_FALSE(first.isValid());
}

TEST_F(RelocatableMetadataSnapshotRegistryTest, RejectsAnInvalidOrExhaustedPinLimit) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	EXPECT_THROW(static_cast<void>(MetadataSnapshotRegistry(
						 bootstrap, MetadataSnapshotRegistryLimits{.maximum_pins_per_generation = 0})),
				 std::invalid_argument);

	MetadataSnapshotRegistry snapshots(bootstrap, MetadataSnapshotRegistryLimits{.maximum_pins_per_generation = 1});
	MetadataSnapshotPin first = snapshots.openSnapshot();
	expectFormatError([&] { static_cast<void>(snapshots.openSnapshot()); }, FormatError::FERR_ARITHMETIC_OVERFLOW);
	EXPECT_EQ(snapshots.activeSnapshotCount(), 1U);
}

TEST_F(RelocatableMetadataSnapshotRegistryTest, PinsTheCurrentDurableRootOnlyUnderItsOwningWriter) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataSnapshotRegistry snapshots(bootstrap);
	MetadataSnapshotRegistry other(bootstrap);

	MetadataWriteGuard inactive;
	expectFormatError([&] { static_cast<void>(snapshots.pinCurrentUnderWrite(inactive, initial)); },
					  FormatError::FERR_INVALID_STATE);
	MetadataWriteGuard foreignWriter = other.beginWrite();
	expectFormatError([&] { static_cast<void>(snapshots.pinCurrentUnderWrite(foreignWriter, initial)); },
					  FormatError::FERR_INVALID_STATE);
	foreignWriter.release();

	MetadataWriteGuard writer = snapshots.beginWrite();
	expectFormatError([&] { static_cast<void>(snapshots.pinCurrentUnderWrite(writer, superblock(2, 2))); },
					  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin pinned = snapshots.pinCurrentUnderWrite(writer, initial);
	EXPECT_TRUE(pinned.isValid());
	MetadataSnapshotPin moved(std::move(pinned));
	EXPECT_FALSE(pinned.isValid());
	EXPECT_TRUE(moved.isValid());
	EXPECT_EQ(snapshots.activeSnapshotCount(), 1U);
	moved.release();
	writer.release();
	writer.release();
	inactive.release();
	EXPECT_EQ(snapshots.activeSnapshotCount(), 0U);

	MetadataSnapshotRegistry limited(bootstrap, MetadataSnapshotRegistryLimits{.maximum_pins_per_generation = 1U});
	MetadataWriteGuard limitedWriter = limited.beginWrite();
	MetadataSnapshotPin onlyPin = limited.pinCurrentUnderWrite(limitedWriter, initial);
	expectFormatError([&] { static_cast<void>(limited.pinCurrentUnderWrite(limitedWriter, initial)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	onlyPin.release();
}

TEST_F(RelocatableMetadataSnapshotRegistryTest, WriterBarrierPreventsSnapshotRegistrationUntilPublicationCompletes) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataSnapshotRegistry snapshots(bootstrap);

	MetadataWriteGuard writer = snapshots.beginWrite();
	std::promise<void> started;
	std::future<void> startedFuture = started.get_future();
	std::future<MetadataSnapshotPin> reader = std::async(std::launch::async, [&] {
		started.set_value();
		return snapshots.openSnapshot();
	});
	startedFuture.wait();
	EXPECT_EQ(reader.wait_for(20ms), std::future_status::timeout);
	writer.release();
	ASSERT_EQ(reader.wait_for(1s), std::future_status::ready);
	MetadataSnapshotPin snapshot = reader.get();
	EXPECT_TRUE(snapshot.isValid());
	expectSameSuperblock(snapshot.superblock(), initial);
	EXPECT_EQ(snapshots.activeSnapshotCount(), 1U);
}

TEST_F(RelocatableMetadataSnapshotRegistryTest, ReclamationRequiresThisRegistrysActiveWriterAndCurrentRoot) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	CatalogTree tree(pages, allocator);
	MetadataSnapshotRegistry snapshots(bootstrap);
	MetadataSnapshotRegistry otherRegistry(bootstrap);

	MetadataWriteGuard writer = snapshots.beginWrite();
	const Superblock current =
			tree.upsertAndPublish(bootstrap, bootstrap.recover(), std::vector{entry(100, 2, 2)}, LogSequenceNumber(2));
	MetadataWriteGuard foreignWriter = otherRegistry.beginWrite();
	MetadataWriteGuard inactiveWriter;
	expectFormatError([&] { snapshots.reclaimAfterPublication(inactiveWriter, allocator, current); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { snapshots.reclaimAfterPublication(foreignWriter, allocator, current); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { snapshots.reclaimAfterPublication(writer, allocator, initial); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(allocator.stats().poisoned);

	snapshots.reclaimAfterPublication(writer, allocator, current);
	EXPECT_EQ(allocator.stats().allocated_pages, 2U);
}

TEST_F(RelocatableMetadataSnapshotRegistryTest, InspectsPinnedSnapshotsOnlyUnderItsOwningWriter) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataSnapshotRegistry snapshots(bootstrap);
	MetadataSnapshotRegistry other(bootstrap);
	MetadataSnapshotPin pinned = snapshots.openSnapshot();
	MetadataWriteGuard writer = snapshots.beginWrite();

	EXPECT_TRUE(snapshots.anyActiveSnapshot(
			writer, [&](const Superblock &candidate) { return candidate.generation == pinned.superblock().generation; }));
	EXPECT_FALSE(snapshots.anyActiveSnapshot(
			writer, [](const Superblock &candidate) { return candidate.generation == Generation(2U); }));
	expectFormatError(
			[&] {
				static_cast<void>(snapshots.anyActiveSnapshot(writer, std::function<bool(const Superblock &)>{}));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataWriteGuard foreignWriter = other.beginWrite();
	expectFormatError(
			[&] {
				static_cast<void>(snapshots.anyActiveSnapshot(foreignWriter, [](const Superblock &) { return false; }));
			},
			FormatError::FERR_INVALID_STATE);
	foreignWriter.release();
	writer.release();
	pinned.release();
	EXPECT_EQ(snapshots.activeSnapshotCount(), 0U);
}
