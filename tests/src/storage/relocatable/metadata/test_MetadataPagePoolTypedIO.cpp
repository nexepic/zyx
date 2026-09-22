/**
 * @file test_MetadataPagePoolTypedIO.cpp
 * @brief Generation-keyed metadata pool control-page I/O tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <system_error>

#include <gtest/gtest.h>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "storage/relocatable/metadata/MetadataPagePoolTestFixture.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;
using graph::storage::test_support::RelocatableMetadataPagePoolTest;

namespace {

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		return result;
	}

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	uint64_t inactiveControlRecordOffset(std::span<const std::byte, RSS_PAGE_SIZE> controlPage,
										 uint64_t poolOffset) {
		const MetadataPagePoolHeader first = MetadataPagePoolHeaderCodec::decode(
				controlPage.subspan(RSS_ENVELOPE_BYTES, RSS_METADATA_PAGE_POOL_HEADER_BYTES));
		const MetadataPagePoolHeader second = MetadataPagePoolHeaderCodec::decode(
				controlPage.subspan(RSS_ENVELOPE_BYTES + RSS_METADATA_PAGE_POOL_HEADER_BYTES,
										RSS_METADATA_PAGE_POOL_HEADER_BYTES));
		return poolOffset + (first.control_sequence >= second.control_sequence
								 ? RSS_ENVELOPE_BYTES + RSS_METADATA_PAGE_POOL_HEADER_BYTES
								 : RSS_ENVELOPE_BYTES);
	}

} // namespace

TEST_F(RelocatableMetadataPagePoolTest, KeepsControlPageUpdatesUnderTheDurablePoolGenerationKey) {
	io->enableBufferPool(64U * RSS_PAGE_SIZE);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(io, initial);
	const PhysicalPageRef first = allocator.allocatePage(Generation(2U));
	ASSERT_EQ(first.physical_slot, PhysicalSlotId(1U));
	io->sync();
	io->resetBufferPoolStats();

	const PhysicalPageRef second = allocator.allocatePage(Generation(2U));
	EXPECT_EQ(second.physical_slot, first.physical_slot);
	EXPECT_EQ(second.frame_index, first.frame_index + 1U);
	std::array<std::byte, RSS_PAGE_SIZE> controlPage{};
	EXPECT_EQ(io->readAt({RSS_FILE_BOOTSTRAP_BYTES, Generation(2U).value()}, controlPage.data(), controlPage.size()),
			  controlPage.size());
	std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES> headerBytes{};
	std::copy_n(controlPage.begin() + RSS_ENVELOPE_BYTES, headerBytes.size(), headerBytes.begin());
	EXPECT_EQ(std::popcount(MetadataPagePoolHeaderCodec::decode(headerBytes).allocated_frames), 2U);

	const BufferPoolStats stats = io->bufferPoolStats();
	// The inactive-record update invalidates the whole generation-tagged control
	// page so a failed partial write cannot later be overwritten by stale bytes.
	EXPECT_GE(stats.misses, 1U);
	EXPECT_EQ(stats.disk_read_bytes, stats.misses * RSS_PAGE_SIZE);
}

TEST_F(RelocatableMetadataPagePoolTest, BatchAllocationPersistsOneBitmapUpdatePerPool) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);

	size_t controlRecordWrites = 0U;
	{
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		StorageIO::ScopedNativeWriteHookForTesting countControlRecords(
				[&controlRecordWrites](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_METADATA_PAGE_POOL_HEADER_BYTES) {
						++controlRecordWrites;
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		const std::vector<MetadataPageAllocation> allocated = allocator.allocatePages(
				Generation(2U), LogSequenceNumber(2U), 8U, std::span<const PhysicalSlotId>{},
				std::span<const PhysicalSlotRange>{}, nullptr);
		ASSERT_EQ(allocated.size(), 8U);
		EXPECT_EQ(controlRecordWrites, 1U);
		EXPECT_EQ(allocator.stats().allocated_pages, 8U);
		EXPECT_EQ(std::popcount(allocator.poolHeader(PhysicalSlotId(1U)).allocated_frames), 8U);
	}

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, BatchAllocationRollsBackEveryPoolWhenLaterBitmapWriteFails) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);

	{
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		const PhysicalPageRef retained = allocator.allocatePage(Generation(1U));
		ASSERT_EQ(retained.physical_slot, PhysicalSlotId(1U));
		const uint64_t secondPoolControlRecord = RSS_FILE_BOOTSTRAP_BYTES + RSS_SEGMENT_SIZE +
												 RSS_ENVELOPE_BYTES + RSS_METADATA_PAGE_POOL_HEADER_BYTES;
		bool interrupted = false;
		{
			StorageIO::ScopedNativeWriteHookForTesting failSecondPoolBitmap(
					[secondPoolControlRecord, &interrupted](file_handle_t fd, const void *bytes, size_t byteCount,
															 pwrite_off_t offset) {
						if (!interrupted && static_cast<uint64_t>(offset) == secondPoolControlRecord &&
							byteCount == RSS_METADATA_PAGE_POOL_HEADER_BYTES) {
							interrupted = true;
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(
					static_cast<void>(allocator.allocatePages(
							Generation(2U), LogSequenceNumber(2U), RSS_PAGE_POOL_DATA_FRAME_COUNT,
							std::span<const PhysicalSlotId>{}, std::span<const PhysicalSlotRange>{}, nullptr)),
					std::system_error);
		}
		ASSERT_TRUE(interrupted);
		EXPECT_FALSE(allocator.stats().poisoned);
		EXPECT_EQ(allocator.stats().pools, 1U);
		EXPECT_EQ(allocator.stats().allocated_pages, 1U);
		const PhysicalPageRef retried = allocator.allocatePage(Generation(2U));
		EXPECT_EQ(retried.physical_slot, retained.physical_slot);
		EXPECT_EQ(retried.frame_index, retained.frame_index + 1U);
	}

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, ControlRecordShortWriteRetainsThePreviousAllocationBitmap) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(nativeIo, initial);
	const PhysicalPageRef first = allocator.allocatePage(Generation(1U));
	ASSERT_TRUE(first.isValid());
	nativeIo->sync();

	const uint64_t poolOffset = RSS_FILE_BOOTSTRAP_BYTES;
	std::array<std::byte, RSS_PAGE_SIZE> controlPage{};
	ASSERT_EQ(nativeIo->readAt(poolOffset, controlPage.data(), controlPage.size()), controlPage.size());
	const uint64_t targetOffset = inactiveControlRecordOffset(controlPage, poolOffset);
	bool interrupted = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting shortInactiveRecord(
				[targetOffset, &interrupted](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (static_cast<uint64_t>(offset) == targetOffset &&
						byteCount == RSS_METADATA_PAGE_POOL_HEADER_BYTES) {
						interrupted = true;
						return portable_pwrite(fd, bytes, 16U, offset);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(allocator.allocatePage(Generation(1U))), std::system_error);
	}
	ASSERT_TRUE(interrupted);

	MetadataPagePoolAllocator recovered(nativeIo, initial, MetadataPagePoolOpenMode::CPOM_DISCOVER_ONLY);
	EXPECT_EQ(recovered.stats().allocated_pages, 1U);
	const PhysicalPageRef retried = recovered.allocatePage(Generation(1U));
	EXPECT_EQ(retried.physical_slot, first.physical_slot);
	EXPECT_EQ(retried.frame_index, first.frame_index + 1U);

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, ControlRecordDiskFullRetainsThePreviousAllocationBitmap) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(nativeIo, initial);
	const PhysicalPageRef first = allocator.allocatePage(Generation(1U));
	ASSERT_TRUE(first.isValid());
	nativeIo->sync();

	const uint64_t poolOffset = RSS_FILE_BOOTSTRAP_BYTES;
	std::array<std::byte, RSS_PAGE_SIZE> controlPage{};
	ASSERT_EQ(nativeIo->readAt(poolOffset, controlPage.data(), controlPage.size()), controlPage.size());
	const uint64_t targetOffset = inactiveControlRecordOffset(controlPage, poolOffset);
	bool interrupted = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFull(
				[targetOffset, &interrupted](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (static_cast<uint64_t>(offset) == targetOffset &&
						byteCount == RSS_METADATA_PAGE_POOL_HEADER_BYTES) {
						interrupted = true;
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(allocator.allocatePage(Generation(1U))), std::system_error);
	}
	ASSERT_TRUE(interrupted);

	MetadataPagePoolAllocator recovered(nativeIo, initial, MetadataPagePoolOpenMode::CPOM_DISCOVER_ONLY);
	EXPECT_EQ(recovered.stats().allocated_pages, 1U);
	const PhysicalPageRef retried = recovered.allocatePage(Generation(1U));
	EXPECT_EQ(retried.physical_slot, first.physical_slot);
	EXPECT_EQ(retried.frame_index, first.frame_index + 1U);

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsShortRawControlPageReadsDuringPoolDiscovery) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(nativeIo, initial);
	static_cast<void>(allocator.allocatePage(Generation(1U)));
	nativeIo->sync();

	{
		StorageIO::ScopedNativeReadHookForTesting shortControlPageRead(
				[](file_handle_t fd, void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (offset == static_cast<pwrite_off_t>(RSS_FILE_BOOTSTRAP_BYTES) && byteCount == RSS_PAGE_SIZE) {
						return portable_pread(fd, bytes, byteCount - 1U, static_cast<int64_t>(offset));
					}
					return portable_pread(fd, bytes, byteCount, static_cast<int64_t>(offset));
				});
		expectFormatError([&] { static_cast<void>(MetadataPagePoolAllocator(nativeIo, initial)); },
						  FormatError::FERR_BUFFER_TOO_SMALL);
	}

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsShortTypedControlPageReadsDuringCatalogRegistration) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	const Superblock initial = superblock(1U, 1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);
	MetadataPagePoolAllocator allocator(nativeIo, initial);
	const PhysicalPageRef allocated = allocator.allocatePage(Generation(2U));
	allocator.bindCatalogPoolIdentity(allocated.physical_slot, SegmentId(71U), Generation(2U), LogSequenceNumber(1U));
	allocator.eraseRegisteredPoolForTesting(allocated.physical_slot);
	nativeIo->sync();

	{
		StorageIO::ScopedNativeReadHookForTesting shortControlPageRead(
				[](file_handle_t fd, void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (offset == static_cast<pwrite_off_t>(RSS_FILE_BOOTSTRAP_BYTES) && byteCount == RSS_PAGE_SIZE) {
						return portable_pread(fd, bytes, byteCount - 1U, static_cast<int64_t>(offset));
					}
					return portable_pread(fd, bytes, byteCount, static_cast<int64_t>(offset));
				});
		expectFormatError(
				[&] {
					allocator.registerCatalogPool(allocated.physical_slot, SegmentId(71U), Generation(2U),
												  LogSequenceNumber(1U));
				},
				FormatError::FERR_BUFFER_TOO_SMALL);
	}

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}
