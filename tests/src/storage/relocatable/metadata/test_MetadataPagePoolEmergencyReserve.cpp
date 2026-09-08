/**
 * @file test_MetadataPagePoolEmergencyReserve.cpp
 * @brief Emergency-reserve rotation lifecycle tests for metadata page pools.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <cerrno>
#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "storage/relocatable/metadata/MetadataPagePoolTestFixture.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;
using graph::storage::test_support::RelocatableMetadataPagePoolTest;

TEST_F(RelocatableMetadataPagePoolTest, RotatesAnEmergencyReserveIntoAnExistingEmptyTailSlot) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	Superblock initial;
	initial.database_id = DatabaseId(0x1020304050607080ULL);
	initial.generation = Generation(1U);
	initial.checkpoint_lsn = LogSequenceNumber(1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);

	{
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		for (uint32_t frame = 0U; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
			static_cast<void>(allocator.allocatePage(Generation(1U)));
		}
		const MetadataEmergencyReserveDescriptor reserve{
			PhysicalSlotId(2U), SegmentId(73U), Generation(1U), LogSequenceNumber(1U)};
		allocator.createEmergencyReserve(reserve);

		const uint64_t blockedPoolOffset = RSS_FILE_BOOTSTRAP_BYTES + 2U * RSS_SEGMENT_SIZE;
		{
			StorageIO::ScopedNativeWriteHookForTesting diskFullOnNextNormalPool(
					[](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (offset >= static_cast<pwrite_off_t>(blockedPoolOffset) &&
							offset < static_cast<pwrite_off_t>(blockedPoolOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_EQ(allocator.allocatePage(Generation(1U)).physical_slot, reserve.physical_slot);
		}
		ASSERT_TRUE(allocator.emergencyReserveHasAllocatedFrames());

		// A complete unregistered tail slot can remain after an unpublished batch.
		// Rotation must reuse it instead of extending the metadata file again.
		nativeIo->sync();
		const uint64_t originalTail = nativeIo->fileSize();
		ASSERT_EQ((originalTail - RSS_FILE_BOOTSTRAP_BYTES) % RSS_SEGMENT_SIZE, 0U);
		const uint64_t reusableTailEnd = originalTail + RSS_SEGMENT_SIZE;
		ASSERT_EQ(portable_ftruncate(writeHandle, reusableTailEnd), 0);
		const PhysicalSlotId reusableTailSlot(
				(originalTail - RSS_FILE_BOOTSTRAP_BYTES) / RSS_SEGMENT_SIZE + 1U);

		const MetadataEmergencyReserveDescriptor successor = allocator.rotateEmergencyReserve(
				SegmentId(74U), Generation(2U), LogSequenceNumber(2U), reusableTailSlot);
		EXPECT_EQ(successor.physical_slot, reusableTailSlot);
		EXPECT_EQ(successor.pool_token, SegmentId(74U));
		EXPECT_EQ(allocator.emergencyReserve(), successor);
		EXPECT_EQ(nativeIo->fileSize(), reusableTailEnd);
	}

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsEmergencyReserveRotationIntoANonTailSlot) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	Superblock initial;
	initial.database_id = DatabaseId(0x1020304050607080ULL);
	initial.generation = Generation(1U);
	initial.checkpoint_lsn = LogSequenceNumber(1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);

	{
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		for (uint32_t frame = 0U; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
			static_cast<void>(allocator.allocatePage(Generation(1U)));
		}
		const MetadataEmergencyReserveDescriptor reserve{
				PhysicalSlotId(2U), SegmentId(73U), Generation(1U), LogSequenceNumber(1U)};
		allocator.createEmergencyReserve(reserve);

		const uint64_t blockedPoolOffset = RSS_FILE_BOOTSTRAP_BYTES + 2U * RSS_SEGMENT_SIZE;
		{
			StorageIO::ScopedNativeWriteHookForTesting diskFullOnNextNormalPool(
					[](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (offset >= static_cast<pwrite_off_t>(blockedPoolOffset) &&
							offset < static_cast<pwrite_off_t>(blockedPoolOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_EQ(allocator.allocatePage(Generation(1U)).physical_slot, reserve.physical_slot);
		}
		ASSERT_TRUE(allocator.emergencyReserveHasAllocatedFrames());

		// Slot 1 is a live pool, not the only reusable physical tail slot.
		EXPECT_THROW(static_cast<void>(allocator.rotateEmergencyReserve(
					 SegmentId(74U), Generation(2U), LogSequenceNumber(2U), PhysicalSlotId(1U))),
				 FormatException);
		EXPECT_EQ(allocator.emergencyReserve(), reserve);
	}

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}

TEST_F(RelocatableMetadataPagePoolTest, RejectsRotationThatReusesTheConsumedReserveSlot) {
	const file_handle_t writeHandle = portable_open_rw(testFilePath.string().c_str());
	const file_handle_t readHandle = portable_open(testFilePath.string().c_str(), O_RDONLY);
	ASSERT_NE(writeHandle, INVALID_FILE_HANDLE);
	ASSERT_NE(readHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, writeHandle, readHandle);
	Superblock initial;
	initial.database_id = DatabaseId(0x1020304050607080ULL);
	initial.generation = Generation(1U);
	initial.checkpoint_lsn = LogSequenceNumber(1U);
	SuperblockFile bootstrap(nativeIo);
	bootstrap.initializeNew(initial);

	{
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		for (uint32_t frame = 0U; frame < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
			static_cast<void>(allocator.allocatePage(Generation(1U)));
		}
		const MetadataEmergencyReserveDescriptor reserve{
				PhysicalSlotId(2U), SegmentId(73U), Generation(1U), LogSequenceNumber(1U)};
		allocator.createEmergencyReserve(reserve);

		const uint64_t blockedPoolOffset = RSS_FILE_BOOTSTRAP_BYTES + 2U * RSS_SEGMENT_SIZE;
		{
			StorageIO::ScopedNativeWriteHookForTesting diskFullOnNextNormalPool(
					[](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (offset >= static_cast<pwrite_off_t>(blockedPoolOffset) &&
							offset < static_cast<pwrite_off_t>(blockedPoolOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_EQ(allocator.allocatePage(Generation(1U)).physical_slot, reserve.physical_slot);
		}
		ASSERT_TRUE(allocator.emergencyReserveHasAllocatedFrames());

		// The consumed reserve is the physical tail, but it remains registered and
		// cannot be overwritten as the successor reserve.
		EXPECT_THROW(static_cast<void>(allocator.rotateEmergencyReserve(
					 SegmentId(74U), Generation(2U), LogSequenceNumber(2U), reserve.physical_slot)),
				 FormatException);
		EXPECT_EQ(allocator.emergencyReserve(), reserve);
	}

	nativeIo.reset();
	EXPECT_EQ(portable_close_rw(writeHandle), 0);
	EXPECT_EQ(portable_close(readHandle), 0);
}
