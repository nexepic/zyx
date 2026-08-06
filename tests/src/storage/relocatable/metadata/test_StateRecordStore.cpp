/**
 * @file test_StateRecordStore.cpp
 * @brief Durable State record placement tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/metadata/StateRecordStore.hpp"

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	Superblock superblock() {
		Superblock result;
		result.database_id = DatabaseId(0xBADC0FFEE0DDF00DULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	StateRecordStoreOptions options() { return {PhysicalSlotId(40), PhysicalSlotId(43), 707}; }

	StateRecord record(uint64_t stateId) {
		return {stateId,
				"state." + std::to_string(stateId),
				StateRecordStorageKind::SRS_INLINE,
				LargeValueId{},
				{std::byte{0x11}, std::byte{0x22}}};
	}

	LogSequenceNumber nextCheckpointLsn(RelocatableMetadataStore &metadata) {
		const MetadataSnapshotPin snapshot = metadata.openSnapshot();
		return LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
	}

} // namespace

class RelocatableStateRecordStoreTest : public ::testing::Test {
protected:
	std::filesystem::path data_path;
	std::filesystem::path wal_path;
	std::shared_ptr<std::fstream> data_file;
	std::shared_ptr<std::fstream> wal_file;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> wal_io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		data_path = std::filesystem::temp_directory_path() / ("relocatable_state_record_" + to_string(uuid) + ".dat");
		wal_path = data_path.string() + ".foreign";
		data_file = std::make_shared<std::fstream>(data_path,
												   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		wal_file = std::make_shared<std::fstream>(wal_path,
												  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(data_file->is_open());
		ASSERT_TRUE(wal_file->is_open());
		io = std::make_shared<StorageIO>(data_file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		wal_io = std::make_shared<StorageIO>(wal_file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		wal_io.reset();
		if (data_file && data_file->is_open()) {
			data_file->close();
		}
		if (wal_file && wal_file->is_open()) {
			wal_file->close();
		}
		data_file.reset();
		wal_file.reset();
		std::error_code error;
		std::filesystem::remove(data_path, error);
		std::filesystem::remove(wal_path, error);
	}
};

TEST_F(RelocatableStateRecordStoreTest, MaterializesIdempotentRecordsAndReopensTheirLocations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	StateLocation first;
	StateLocation second;
	{
		SlottedPagePool pages(io, *metadata);
		StateRecordStore store(pages, *metadata);
		const StateRecord firstRecord = record(1);
		const StateRecord secondRecord{2, "state.large", StateRecordStorageKind::SRS_LARGE_VALUE, LargeValueId(17), {}};
		first = store.ensure(firstRecord, options());
		EXPECT_EQ(store.ensure(firstRecord, options()), first);
		second = store.ensure(secondRecord, options());
		EXPECT_EQ(first.page_id, second.page_id);
		EXPECT_NE(first.slot_id, second.slot_id);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_EQ(store.read(snapshot, 1), firstRecord);
		EXPECT_EQ(store.read(snapshot, 2), secondRecord);
		snapshot.release();
	}
	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopenedMetadata);
	StateRecordStore reopenedStore(reopenedPages, *reopenedMetadata);
	MetadataSnapshotPin snapshot = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedStore.read(snapshot, first.state_id), record(1));
	EXPECT_EQ(reopenedStore.read(snapshot, second.state_id),
			  (StateRecord{2, "state.large", StateRecordStorageKind::SRS_LARGE_VALUE, LargeValueId(17), {}}));
}

TEST_F(RelocatableStateRecordStoreTest, RejectsConflictsInvalidOptionsAndOversizedRecords) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	StateRecordStore store(pages, *metadata);
	const StateRecord stored = record(1);
	static_cast<void>(store.ensure(stored, options()));
	StateRecord conflicting = stored;
	conflicting.inline_bytes = {std::byte{0x33}};
	expectFormatError([&] { static_cast<void>(store.ensure(conflicting, options())); },
					  FormatError::FERR_INVALID_STATE);
	for (StateRecordStoreOptions invalid:
		 {StateRecordStoreOptions{{}, PhysicalSlotId(43), 707}, StateRecordStoreOptions{PhysicalSlotId(40), {}, 707},
		  StateRecordStoreOptions{PhysicalSlotId(40), PhysicalSlotId(39), 707},
		  StateRecordStoreOptions{PhysicalSlotId(40), PhysicalSlotId(43), 0}}) {
		expectFormatError([&] { static_cast<void>(store.ensure(record(2), invalid)); },
						  FormatError::FERR_INVALID_HEADER);
	}
	StateRecord oversized = record(3);
	oversized.inline_bytes.assign(4000, std::byte{0x44});
	expectFormatError([&] { static_cast<void>(store.ensure(oversized, options())); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableStateRecordStoreTest, FailsClosedForMismatchedDirectoryAndExhaustedDestinations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	StateRecordStore store(pages, *metadata);
	const StateLocation location = store.ensure(record(1), options());
	const StateLocation mismatched{99, location.page_id, location.slot_id, location.slot_generation};
	static_cast<void>(metadata->upsertStateLocations(std::span(&mismatched, 1), {}, nextCheckpointLsn(*metadata)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.read(snapshot, 99)); }, FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(RelocatableStateRecordStoreTest, AllocatesAnotherPoolWhenTheCurrentPageCannotFit) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	StateRecordStore store(pages, *metadata);
	StateRecord full = record(1);
	full.inline_bytes.assign(3890, std::byte{0x55});
	const StateLocation first = store.ensure(full, options());
	const StateLocation second = store.ensure(record(2), options());
	EXPECT_NE(first.page_id, second.page_id);
}

TEST_F(RelocatableStateRecordStoreTest, RejectsCheckpointLsnExhaustion) {
	Superblock exhaustedLsn = superblock();
	exhaustedLsn.checkpoint_lsn = LogSequenceNumber(std::numeric_limits<uint64_t>::max());
	auto metadata = RelocatableMetadataStore::initializeNew(io, exhaustedLsn);
	SlottedPagePool pages(io, *metadata);
	StateRecordStore store(pages, *metadata);
	expectFormatError([&] { static_cast<void>(store.ensure(record(1), options())); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableStateRecordStoreTest, RejectsLogicalSegmentIdentityExhaustion) {
	Superblock exhaustedSegment = superblock();
	exhaustedSegment.next_logical_segment_id = 0;
	auto metadata = RelocatableMetadataStore::initializeNew(io, exhaustedSegment);
	SlottedPagePool segmentPages(io, *metadata);
	StateRecordStore segmentStore(segmentPages, *metadata);
	expectFormatError([&] { static_cast<void>(segmentStore.ensure(record(1), options())); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableStateRecordStoreTest, RejectsInitialMaterializationWithoutAFreePhysicalSlotAndForeignMetadata) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	SlottedPagePoolAllocationRequest occupied;
	occupied.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
	occupied.pool_kind = SegmentKind::SGK_STATE_PAGE_POOL;
	occupied.pool_owner_id = 12;
	occupied.generation = Generation(1);
	occupied.page_id = PageId(100);
	occupied.page_owner_id = 12;
	occupied.page_owner_kind = PageOwnerKind::POK_STATE;
	ASSERT_TRUE(pages.allocateEmptyPage(occupied, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	StateRecordStore store(pages, *metadata);
	StateRecordStoreOptions exhausted = options();
	exhausted.last_slot = PhysicalSlotId(40);
	expectFormatError([&] { static_cast<void>(store.ensure(record(1), exhausted)); }, FormatError::FERR_INVALID_STATE);
	auto otherMetadata = RelocatableMetadataStore::open(io);
	EXPECT_THROW(static_cast<void>(StateRecordStore(pages, *otherMetadata)), std::invalid_argument);
}
