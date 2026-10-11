/**
 * @file test_MetadataEmergencyReserveExhaustion.cpp
 * @brief Publication-boundary tests for exhausted metadata emergency reserves.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <cerrno>
#include <atomic>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <source_location>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected,
						   const std::source_location location = std::source_location::current()) {
		try {
			operation();
			ADD_FAILURE_AT(location.file_name(), static_cast<int>(location.line())) << "Expected FormatException";
		} catch (const FormatException &error) {
			if (error.error() != expected) {
				ADD_FAILURE_AT(location.file_name(), static_cast<int>(location.line()))
						<< "Unexpected FormatError: " << error.what();
			}
		}
	}

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	std::vector<CatalogEntry> entriesThatExhaustBothMetadataPools() {
		std::vector<CatalogEntry> entries;
		entries.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT * 3U);
		for (uint64_t ordinal = 0U; ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT * 3U; ++ordinal) {
			entries.push_back({SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + ordinal * RSS_CATALOG_PAGE_MAX_ENTRIES),
							   PhysicalSlotId(1000U + ordinal), SegmentKind::SGK_ENTITY_PAGE_POOL, 77U,
							   Generation(1U), SegmentState::SGS_SEALED, LogSequenceNumber(2U)});
		}
		return entries;
	}

	std::vector<CatalogEntry> entriesThatRequireOneAdditionalMetadataPool() {
		std::vector<CatalogEntry> entries;
		entries.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);
		for (uint64_t ordinal = 0U; ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U; ++ordinal) {
			entries.push_back({SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + ordinal * RSS_CATALOG_PAGE_MAX_ENTRIES),
							   PhysicalSlotId(1000U + ordinal), SegmentKind::SGK_ENTITY_PAGE_POOL, 77U,
							   Generation(1U), SegmentState::SGS_SEALED, LogSequenceNumber(2U)});
		}
		return entries;
	}

} // namespace

class RelocatableMetadataEmergencyReserveExhaustionTest : public ::testing::Test {
protected:
	std::filesystem::path dataPath;
	std::shared_ptr<std::fstream> dataFile;

	void SetUp() override {
		dataPath = std::filesystem::temp_directory_path() /
				   ("relocatable_metadata_reserve_exhaustion_" + boost::uuids::to_string(boost::uuids::random_generator()()) +
					".dat");
		dataFile = std::make_shared<std::fstream>(dataPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(dataFile->is_open());
	}

	void TearDown() override {
		dataFile.reset();
		std::error_code error;
		std::filesystem::remove(dataPath, error);
	}
};

TEST_F(RelocatableMetadataEmergencyReserveExhaustionTest,
	   NativeDiskFullWithAnExhaustedReserveRetainsThePublishedGenerationAndCanRetry) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	const std::vector<CatalogEntry> entries = entriesThatExhaustBothMetadataPools();
	Superblock published;
	{
		auto io = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		MetadataSnapshotPin before = metadata->openSnapshot();
		published = before.superblock();
		ASSERT_EQ(published.metadata_emergency_reserve.physical_slot, PhysicalSlotId(2U));
		const uint64_t initialFileSize = io->fileSize();
		const uint64_t firstOrdinaryPoolOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(published.data_region_offset, PhysicalSlotId(3U));
		before.release();

		{
			StorageIO::ScopedNativeWriteHookForTesting diskFullOnEveryOrdinaryPool(
					[firstOrdinaryPoolOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (static_cast<uint64_t>(offset) >= firstOrdinaryPoolOffset) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			expectFormatError([&] { static_cast<void>(metadata->upsertCatalog(entries, LogSequenceNumber(2U))); },
						  FormatError::FERR_INVALID_STATE);
		}

		MetadataSnapshotPin afterFailure = metadata->openSnapshot();
		EXPECT_EQ(afterFailure.superblock(), published);
		EXPECT_TRUE(metadata->isPhysicalSlotAllocated(afterFailure, PhysicalSlotId(2U)));
		EXPECT_FALSE(metadata->isPhysicalSlotAllocated(afterFailure, PhysicalSlotId(3U)));
		afterFailure.release();
		EXPECT_EQ(io->fileSize(), initialFileSize);
		EXPECT_FALSE(metadata->pagePoolStats().poisoned);

		EXPECT_NO_THROW(static_cast<void>(metadata->upsertCatalog(entries, LogSequenceNumber(2U))));
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	auto reopenFile = std::make_shared<std::fstream>(dataPath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(reopenFile->is_open());
	auto reopenedIo = std::make_shared<StorageIO>(reopenFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto reopened = RelocatableMetadataStore::open(reopenedIo);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(2U));
	const CatalogEntry restored = reopened->findCatalog(recovered, entries.front().segment_id);
	EXPECT_EQ(restored.segment_id, entries.front().segment_id);
	EXPECT_EQ(restored.physical_slot, entries.front().physical_slot);
	EXPECT_EQ(restored.kind, entries.front().kind);
	EXPECT_EQ(restored.owner_id, entries.front().owner_id);
	EXPECT_EQ(restored.generation, entries.front().generation);
	EXPECT_EQ(restored.state, entries.front().state);
	EXPECT_EQ(restored.committed_lsn, entries.front().committed_lsn);
}

TEST_F(RelocatableMetadataEmergencyReserveExhaustionTest,
	   MetadataControlRecordPrePublicationSyncFailureRetainsPublishedBitmapAndCanRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	const CatalogEntry entry{SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), PhysicalSlotId(40U),
							 SegmentKind::SGK_ENTITY_PAGE_POOL, 77U, Generation(1U), SegmentState::SGS_SEALED,
							 LogSequenceNumber(2U)};
	Superblock published;
	MetadataPagePoolStats publishedStats;
	{
		auto io = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		MetadataSnapshotPin before = metadata->openSnapshot();
		published = before.superblock();
		before.release();
		publishedStats = metadata->pagePoolStats();

		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failFirstDataFence([&syncCalls](file_handle_t fd) {
				if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
#ifdef _WIN32
					SetLastError(ERROR_DISK_FULL);
#else
					errno = ENOSPC;
#endif
					return -1;
				}
				return portable_fsync(fd);
			});
			EXPECT_THROW(static_cast<void>(metadata->upsertCatalog(std::span(&entry, 1U), LogSequenceNumber(2U))),
						 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);

		MetadataSnapshotPin afterFailure = metadata->openSnapshot();
		EXPECT_EQ(afterFailure.superblock(), published);
		afterFailure.release();
		const MetadataPagePoolStats afterFailureStats = metadata->pagePoolStats();
		EXPECT_EQ(afterFailureStats.pools, publishedStats.pools);
		EXPECT_EQ(afterFailureStats.allocated_pages, publishedStats.allocated_pages);
		EXPECT_FALSE(afterFailureStats.poisoned);

		EXPECT_NO_THROW(static_cast<void>(metadata->upsertCatalog(std::span(&entry, 1U), LogSequenceNumber(2U))));
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	auto reopenFile = std::make_shared<std::fstream>(dataPath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(reopenFile->is_open());
	auto reopenedIo = std::make_shared<StorageIO>(reopenFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto reopened = RelocatableMetadataStore::open(reopenedIo);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(2U));
	EXPECT_EQ(reopened->findCatalog(recovered, entry.segment_id).physical_slot, entry.physical_slot);
}

TEST_F(RelocatableMetadataEmergencyReserveExhaustionTest,
	   ReopenReusesOnlyTheRootFreeUnpublishedMetadataTailBeforeAppendingAnotherPool) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	const std::vector<CatalogEntry> entries = entriesThatRequireOneAdditionalMetadataPool();
	{
		auto io = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		MetadataSnapshotPin before = metadata->openSnapshot();
		const Superblock published = before.superblock();
		before.release();
		const uint64_t candidateOffset = io->reserveAppendSpace(RSS_SEGMENT_SIZE);
		ASSERT_EQ(candidateOffset, PhysicalSegmentAllocator::checkedSlotOffset(published.data_region_offset, PhysicalSlotId(3U)));
		const std::array<std::byte, 16U> interruptedCandidate = {
				std::byte{0xA1}, std::byte{0xB2}, std::byte{0xC3}, std::byte{0xD4}};
		io->writeAt(candidateOffset, interruptedCandidate.data(), interruptedCandidate.size());
		io->sync();
		metadata.reset();

		metadata = RelocatableMetadataStore::open(io);
		MetadataSnapshotPin recovered = metadata->openSnapshot();
		EXPECT_EQ(recovered.superblock(), published);
		EXPECT_FALSE(metadata->isPhysicalSlotAllocated(recovered, PhysicalSlotId(3U)));
		recovered.release();

		EXPECT_NO_THROW(static_cast<void>(metadata->upsertCatalog(entries, LogSequenceNumber(2U))));
		EXPECT_EQ(io->fileSize(), candidateOffset + RSS_SEGMENT_SIZE);
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}
