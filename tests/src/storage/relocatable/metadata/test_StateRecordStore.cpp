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
#include <system_error>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueMaintenanceJournal.hpp"
#include "graph/storage/relocatable/metadata/StateRecordStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#endif

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
	file_handle_t native_handle = INVALID_FILE_HANDLE;

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
		if (native_handle != INVALID_FILE_HANDLE) {
			EXPECT_EQ(portable_close_rw(native_handle), 0);
			native_handle = INVALID_FILE_HANDLE;
		}
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

	void switchToNativeIo() {
		io.reset();
		data_file->flush();
		native_handle = portable_open_rw(data_path.string().c_str());
		ASSERT_NE(native_handle, INVALID_FILE_HANDLE);
		io = std::make_shared<StorageIO>(data_file, native_handle, native_handle);
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
		first = store.upsert(firstRecord, options());
		MetadataSnapshotPin afterFirstPublication = metadata->openSnapshot();
		EXPECT_EQ(afterFirstPublication.superblock().generation, Generation(2U));
		afterFirstPublication.release();
		EXPECT_EQ(store.upsert(firstRecord, options()), first);
		MetadataSnapshotPin afterIdempotentUpsert = metadata->openSnapshot();
		EXPECT_EQ(afterIdempotentUpsert.superblock().generation, Generation(2U));
		afterIdempotentUpsert.release();
		StateRecord replacement = firstRecord;
		replacement.inline_bytes = {std::byte{0x33}};
		EXPECT_EQ(store.upsert(replacement, options()), first);
		MetadataSnapshotPin afterReplacement = metadata->openSnapshot();
		EXPECT_EQ(afterReplacement.superblock().generation, Generation(3U));
		afterReplacement.release();
		second = store.upsert(secondRecord, options());
		MetadataSnapshotPin afterSecondPublication = metadata->openSnapshot();
		EXPECT_EQ(afterSecondPublication.superblock().generation, Generation(4U));
		afterSecondPublication.release();
		EXPECT_EQ(first.page_id, second.page_id);
		EXPECT_NE(first.slot_id, second.slot_id);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_EQ(store.read(snapshot, 1), replacement);
		EXPECT_EQ(store.read(snapshot, 2), secondRecord);
		snapshot.release();
	}
	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopenedMetadata);
	StateRecordStore reopenedStore(reopenedPages, *reopenedMetadata);
	MetadataSnapshotPin snapshot = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedStore.read(snapshot, first.state_id),
			  (StateRecord{1, "state.1", StateRecordStorageKind::SRS_INLINE, LargeValueId{}, {std::byte{0x33}}}));
	EXPECT_EQ(reopenedStore.read(snapshot, second.state_id),
			  (StateRecord{2, "state.large", StateRecordStorageKind::SRS_LARGE_VALUE, LargeValueId(17), {}}));
}

TEST_F(RelocatableStateRecordStoreTest, ReplacesStableRecordsAndRejectsInvalidOptionsAndOversizedRecords) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	StateRecordStore store(pages, *metadata);
	const StateRecord stored = record(1);
	const StateLocation location = store.upsert(stored, options());
	StateRecord replacement = stored;
	replacement.inline_bytes = {std::byte{0x33}};
	EXPECT_EQ(store.upsert(replacement, options()), location);
	MetadataSnapshotPin replaced = metadata->openSnapshot();
	EXPECT_EQ(replaced.superblock().generation, Generation(3U));
	EXPECT_EQ(store.read(replaced, replacement.state_id), replacement);
	replaced.release();
	StateRecord larger = stored;
	larger.inline_bytes.assign(3U, std::byte{0x44});
	expectFormatError([&] { static_cast<void>(store.upsert(larger, options())); }, FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin unchanged = metadata->openSnapshot();
	EXPECT_EQ(store.read(unchanged, replacement.state_id), replacement);
	unchanged.release();
	for (StateRecordStoreOptions invalid:
		 {StateRecordStoreOptions{{}, PhysicalSlotId(43), 707}, StateRecordStoreOptions{PhysicalSlotId(40), {}, 707},
		  StateRecordStoreOptions{PhysicalSlotId(40), PhysicalSlotId(39), 707},
		  StateRecordStoreOptions{PhysicalSlotId(40), PhysicalSlotId(43), 0}}) {
		expectFormatError([&] { static_cast<void>(store.upsert(record(2), invalid)); },
						  FormatError::FERR_INVALID_HEADER);
	}
	StateRecord oversized = record(3);
	oversized.inline_bytes.assign(4000, std::byte{0x44});
	expectFormatError([&] { static_cast<void>(store.upsert(oversized, options())); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableStateRecordStoreTest, FailsClosedForMismatchedDirectoryAndExhaustedDestinations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	StateRecordStore store(pages, *metadata);
	const StateLocation location = store.upsert(record(1), options());
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
	const StateLocation first = store.upsert(full, options());
	const StateLocation second = store.upsert(record(2), options());
	EXPECT_NE(first.page_id, second.page_id);
}

TEST_F(RelocatableStateRecordStoreTest, RejectsCheckpointLsnExhaustion) {
	Superblock exhaustedLsn = superblock();
	exhaustedLsn.checkpoint_lsn = LogSequenceNumber(std::numeric_limits<uint64_t>::max());
	auto metadata = RelocatableMetadataStore::initializeNew(io, exhaustedLsn);
	SlottedPagePool pages(io, *metadata);
	StateRecordStore store(pages, *metadata);
	expectFormatError([&] { static_cast<void>(store.upsert(record(1), options())); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableStateRecordStoreTest, RejectsLogicalSegmentIdentityExhaustion) {
	Superblock exhaustedSegment = superblock();
	exhaustedSegment.next_logical_segment_id = 0;
	auto metadata = RelocatableMetadataStore::initializeNew(io, exhaustedSegment);
	SlottedPagePool segmentPages(io, *metadata);
	StateRecordStore segmentStore(segmentPages, *metadata);
	expectFormatError([&] { static_cast<void>(segmentStore.upsert(record(1), options())); },
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
	expectFormatError([&] { static_cast<void>(store.upsert(record(1), exhausted)); }, FormatError::FERR_INVALID_STATE);
	auto otherMetadata = RelocatableMetadataStore::open(io);
	EXPECT_THROW(static_cast<void>(StateRecordStore(pages, *otherMetadata)), std::invalid_argument);
}

TEST_F(RelocatableStateRecordStoreTest, CandidateDiskFullLeavesNoStateRouteAndTheSameRecordCanRetryAfterReopen) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	metadata.reset();
	switchToNativeIo();
	metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin before = metadata->openSnapshot();
	const uint64_t candidateOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(before.superblock().data_region_offset, PhysicalSlotId(40U));
	before.release();
	{
		SlottedPagePool pages(io, *metadata);
		StateRecordStore store(pages, *metadata);
		StorageIO::ScopedNativeWriteHookForTesting failStateCandidate(
				[candidateOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (static_cast<uint64_t>(offset) >= candidateOffset &&
						static_cast<uint64_t>(offset) < candidateOffset + RSS_SEGMENT_SIZE) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(store.upsert(record(1), options())), std::system_error);
	}
	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopened);
	StateRecordStore reopenedStore(reopenedPages, *reopened);
	MetadataSnapshotPin absent = reopened->openSnapshot();
	EXPECT_FALSE(reopened->findStateLocationOptional(absent, 1U).has_value());
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(absent, PhysicalSlotId(40U)));
	absent.release();
	const StateLocation location = reopenedStore.upsert(record(1), options());
	MetadataSnapshotPin published = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.read(published, location.state_id), record(1));
}

TEST_F(RelocatableStateRecordStoreTest, PersistsFixedCapacityLargeValueMaintenanceJournalAcrossReplacementAndReopen) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	LargeValueMaintenanceJournal expected;
	expected.scan_generation = Generation(1U);
	expected.phase = LargeValueMaintenanceJournalPhase::LVMJP_SCAN_STATES;
	expected.manifest_after_value_id = 19U;
	expected.entity_property_after_directory_id = 27U;
	expected.candidate_value_ids = {LargeValueId(3U), LargeValueId(9U), LargeValueId(18U)};
	expected.reachable_value_ids = {LargeValueId(9U)};
	LargeValueMaintenanceJournalStoreOptions journalOptions{options(), 83U};
	StateLocation initial;
	{
		SlottedPagePool pages(io, *metadata);
		StateRecordStore states(pages, *metadata);
		LargeValueMaintenanceJournalStore journal(states, *metadata, journalOptions);
		MetadataSnapshotPin beforeInitial = metadata->openSnapshot();
		const LargeValueMaintenanceJournalSave initialSave = journal.saveAfterScan(beforeInitial, expected);
		beforeInitial.release();
		initial = initialSave.location;
		expected = initialSave.journal;
		MetadataSnapshotPin afterInitial = metadata->openSnapshot();
		EXPECT_EQ(afterInitial.superblock().generation, expected.scan_generation);
		EXPECT_EQ(journal.load(afterInitial), expected);
		afterInitial.release();
		expected.phase = LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM;
		expected.state_after_id = 42U;
		expected.reachable_value_ids = {LargeValueId(3U), LargeValueId(9U)};
		MetadataSnapshotPin beforeReplacement = metadata->openSnapshot();
		const LargeValueMaintenanceJournalSave replacementSave = journal.saveAfterScan(beforeReplacement, expected);
		beforeReplacement.release();
		EXPECT_EQ(replacementSave.location, initial);
		expected = replacementSave.journal;
		MetadataSnapshotPin afterReplacement = metadata->openSnapshot();
		EXPECT_EQ(afterReplacement.superblock().generation, Generation(3U));
		EXPECT_EQ(afterReplacement.superblock().generation, expected.scan_generation);
		EXPECT_EQ(journal.load(afterReplacement), expected);
		afterReplacement.release();
	}
	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopened);
	StateRecordStore reopenedStates(reopenedPages, *reopened);
	LargeValueMaintenanceJournalStore reopenedJournal(reopenedStates, *reopened, journalOptions);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedJournal.load(snapshot), expected);
}

TEST_F(RelocatableStateRecordStoreTest,
	   LargeValueMaintenanceJournalReplacementShortWriteRetainsPriorEvidenceAndCanRetryAfterReopen) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	LargeValueMaintenanceJournal initial;
	initial.scan_generation = Generation(1U);
	initial.phase = LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES;
	initial.manifest_after_value_id = 19U;
	initial.candidate_value_ids = {LargeValueId(3U), LargeValueId(9U)};
	LargeValueMaintenanceJournalStoreOptions journalOptions{options(), 83U};
	StateLocation journalLocation;
	{
		SlottedPagePool pages(io, *metadata);
		StateRecordStore states(pages, *metadata);
		LargeValueMaintenanceJournalStore journal(states, *metadata, journalOptions);
		MetadataSnapshotPin beforeInitial = metadata->openSnapshot();
		const LargeValueMaintenanceJournalSave save = journal.saveAfterScan(beforeInitial, initial);
		beforeInitial.release();
		initial = save.journal;
		journalLocation = save.location;
	}

	metadata.reset();
	switchToNativeIo();
	metadata = RelocatableMetadataStore::open(io);
	uint64_t replacementOffset = 0U;
	{
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const PageLocation source = metadata->findPageLocation(snapshot, journalLocation.page_id);
		const CatalogEntry pool = metadata->findCatalog(snapshot, source.segment_id);
		ASSERT_LT(source.page_index, RSS_PAGE_POOL_DATA_FRAME_COUNT);
		replacementOffset = PhysicalSegmentAllocator::checkedSlotOffset(
				snapshot.superblock().data_region_offset, pool.physical_slot) +
				static_cast<uint64_t>(source.page_index + 1U) * RSS_PAGE_SIZE;
		snapshot.release();
	}

	LargeValueMaintenanceJournal replacement = initial;
	replacement.phase = LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM;
	replacement.state_after_id = 42U;
	replacement.reachable_value_ids = {LargeValueId(3U)};
	bool shortWriteObserved = false;
	{
		SlottedPagePool pages(io, *metadata);
		StateRecordStore states(pages, *metadata);
		LargeValueMaintenanceJournalStore journal(states, *metadata, journalOptions);
		StorageIO::ScopedNativeWriteHookForTesting shortReplacement(
				[replacementOffset, &shortWriteObserved](file_handle_t fd, const void *bytes, size_t byteCount,
																		 pwrite_off_t offset) {
					if (static_cast<uint64_t>(offset) == replacementOffset && byteCount == RSS_PAGE_SIZE) {
						shortWriteObserved = true;
						return portable_pwrite(fd, bytes, RSS_PAGE_OWNER_HEADER_BYTES - 1U, offset);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		MetadataSnapshotPin beforeReplacement = metadata->openSnapshot();
		EXPECT_THROW(static_cast<void>(journal.saveAfterScan(beforeReplacement, replacement)), std::system_error);
		beforeReplacement.release();
	}
	EXPECT_TRUE(shortWriteObserved);

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopened);
	StateRecordStore reopenedStates(reopenedPages, *reopened);
	LargeValueMaintenanceJournalStore reopenedJournal(reopenedStates, *reopened, journalOptions);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(reopenedJournal.load(recovered), initial);
	recovered.release();

	MetadataSnapshotPin retrySnapshot = reopened->openSnapshot();
	const LargeValueMaintenanceJournalSave retried = reopenedJournal.saveAfterScan(retrySnapshot, replacement);
	retrySnapshot.release();
	MetadataSnapshotPin published = reopened->openSnapshot();
	EXPECT_EQ(reopenedJournal.load(published), retried.journal);
}

TEST_F(RelocatableStateRecordStoreTest,
	   LargeValueMaintenanceJournalReplacementDiskFullRetainsPriorEvidenceAndCanRetryAfterReopen) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	LargeValueMaintenanceJournal initial;
	initial.scan_generation = Generation(1U);
	initial.phase = LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES;
	initial.manifest_after_value_id = 19U;
	initial.candidate_value_ids = {LargeValueId(3U), LargeValueId(9U)};
	LargeValueMaintenanceJournalStoreOptions journalOptions{options(), 83U};
	StateLocation journalLocation;
	{
		SlottedPagePool pages(io, *metadata);
		StateRecordStore states(pages, *metadata);
		LargeValueMaintenanceJournalStore journal(states, *metadata, journalOptions);
		MetadataSnapshotPin beforeInitial = metadata->openSnapshot();
		const LargeValueMaintenanceJournalSave save = journal.saveAfterScan(beforeInitial, initial);
		beforeInitial.release();
		initial = save.journal;
		journalLocation = save.location;
	}

	metadata.reset();
	switchToNativeIo();
	metadata = RelocatableMetadataStore::open(io);
	uint64_t replacementOffset = 0U;
	{
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const PageLocation source = metadata->findPageLocation(snapshot, journalLocation.page_id);
		const CatalogEntry pool = metadata->findCatalog(snapshot, source.segment_id);
		ASSERT_LT(source.page_index, RSS_PAGE_POOL_DATA_FRAME_COUNT);
		replacementOffset = PhysicalSegmentAllocator::checkedSlotOffset(
				snapshot.superblock().data_region_offset, pool.physical_slot) +
				static_cast<uint64_t>(source.page_index + 1U) * RSS_PAGE_SIZE;
		snapshot.release();
	}

	LargeValueMaintenanceJournal replacement = initial;
	replacement.phase = LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM;
	replacement.state_after_id = 42U;
	replacement.reachable_value_ids = {LargeValueId(3U)};
	bool diskFullObserved = false;
	{
		SlottedPagePool pages(io, *metadata);
		StateRecordStore states(pages, *metadata);
		LargeValueMaintenanceJournalStore journal(states, *metadata, journalOptions);
		StorageIO::ScopedNativeWriteHookForTesting failReplacement(
				[replacementOffset, &diskFullObserved](file_handle_t fd, const void *bytes, size_t byteCount,
																		 pwrite_off_t offset) {
					if (static_cast<uint64_t>(offset) == replacementOffset && byteCount == RSS_PAGE_SIZE) {
						diskFullObserved = true;
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		MetadataSnapshotPin beforeReplacement = metadata->openSnapshot();
		EXPECT_THROW(static_cast<void>(journal.saveAfterScan(beforeReplacement, replacement)), std::system_error);
		beforeReplacement.release();
	}
	EXPECT_TRUE(diskFullObserved);

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopened);
	StateRecordStore reopenedStates(reopenedPages, *reopened);
	LargeValueMaintenanceJournalStore reopenedJournal(reopenedStates, *reopened, journalOptions);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(reopenedJournal.load(recovered), initial);
	recovered.release();

	MetadataSnapshotPin retrySnapshot = reopened->openSnapshot();
	const LargeValueMaintenanceJournalSave retried = reopenedJournal.saveAfterScan(retrySnapshot, replacement);
	retrySnapshot.release();
	MetadataSnapshotPin published = reopened->openSnapshot();
	EXPECT_EQ(reopenedJournal.load(published), retried.journal);
}

TEST_F(RelocatableStateRecordStoreTest, RejectsCorruptedLargeValueMaintenanceJournalThroughPersistedStateRecord) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	LargeValueMaintenanceJournal journal;
	journal.scan_generation = Generation(1U);
	journal.phase = LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES;
	journal.candidate_value_ids = {LargeValueId(3U)};
	StateRecord record;
	record.state_id = 83U;
	record.key = std::string(RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY);
	record.storage_kind = StateRecordStorageKind::SRS_INLINE;
	record.inline_bytes = LargeValueMaintenanceJournalCodec::encode(journal);
	// Keep the enclosing State record valid so load() reaches the journal's
	// independent checksum boundary after a durable State-directory lookup.
	record.inline_bytes[0] ^= std::byte{0x01};
	LargeValueMaintenanceJournalStoreOptions journalOptions{options(), record.state_id};
	{
		SlottedPagePool pages(io, *metadata);
		StateRecordStore states(pages, *metadata);
		LargeValueMaintenanceJournalStore persistedJournal(states, *metadata, journalOptions);
		static_cast<void>(states.upsert(record, options()));
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		expectFormatError([&] { static_cast<void>(persistedJournal.load(snapshot)); }, FormatError::FERR_CHECKSUM_MISMATCH);
		snapshot.release();
	}

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopened);
	StateRecordStore reopenedStates(reopenedPages, *reopened);
	LargeValueMaintenanceJournalStore reopenedJournal(reopenedStates, *reopened, journalOptions);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	expectFormatError([&] { static_cast<void>(reopenedJournal.load(snapshot)); }, FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST(LargeValueMaintenanceJournalCodecTest, RejectsNoncanonicalOrInconsistentEvidence) {
	LargeValueMaintenanceJournal journal;
	journal.scan_generation = Generation(9U);
	journal.phase = LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES;
	journal.manifest_after_value_id = 7U;
	journal.candidate_value_ids = {LargeValueId(3U), LargeValueId(5U)};
	journal.reachable_value_ids = {LargeValueId(5U)};
	const std::vector<std::byte> encoded = LargeValueMaintenanceJournalCodec::encode(journal);
	EXPECT_EQ(LargeValueMaintenanceJournalCodec::decode(encoded), journal);

	LargeValueMaintenanceJournal invalid = journal;
	invalid.reachable_value_ids = {LargeValueId(4U)};
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> noncanonical = encoded;
	noncanonical.back() = std::byte{0x55};
	const uint32_t payloadCrc = graph::utils::calculateCrc(
			noncanonical.data() + RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES,
			noncanonical.size() - RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES);
	LittleEndianCodec::writeU32(noncanonical, 52U, payloadCrc);
	LittleEndianCodec::writeU32(noncanonical, 56U, 0U);
	const uint32_t headerCrc =
			graph::utils::calculateCrc(noncanonical.data(), RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_HEADER_BYTES);
	LittleEndianCodec::writeU32(noncanonical, 56U, headerCrc);
	expectFormatError([&] { static_cast<void>(LargeValueMaintenanceJournalCodec::decode(noncanonical)); },
					  FormatError::FERR_INVALID_HEADER);
}
