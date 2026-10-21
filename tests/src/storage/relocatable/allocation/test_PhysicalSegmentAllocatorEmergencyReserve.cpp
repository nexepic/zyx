/**
 * @file test_PhysicalSegmentAllocatorEmergencyReserve.cpp
 * @brief Allocation, integrity, and publication-boundary tests for relocatable physical slots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/allocation/PhysicalSegmentAllocatorTestFixture.hpp"

TEST_F(RelocatablePhysicalSegmentAllocatorTest, NativeDiskFullFallsBackToTheEmergencyReserveThenRotatesItAtomically) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	Superblock rotated;
	{
		auto nativeIo = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto store = RelocatableMetadataStore::initializeNew(nativeIo, superblock(1U, 1U));
		MetadataSnapshotPin initial = store->openSnapshot();
		const Superblock initialSuperblock = initial.superblock();
		ASSERT_EQ(initialSuperblock.metadata_emergency_reserve.physical_slot, PhysicalSlotId(2U));
		initial.release();

		const uint64_t ordinaryPoolOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(initialSuperblock.data_region_offset, PhysicalSlotId(3U));
		{
			StorageIO::ScopedNativeWriteHookForTesting diskFullOnOrdinaryPool(
					[ordinaryPoolOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (offset >= static_cast<pwrite_off_t>(ordinaryPoolOffset) &&
							offset < static_cast<pwrite_off_t>(ordinaryPoolOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_NO_THROW(static_cast<void>(
					store->upsertCatalog(catalogEntriesThatExhaustOneNormalMetadataPool(), LogSequenceNumber(2U))));
		}

		MetadataSnapshotPin fallbackSnapshot = store->openSnapshot();
		const SegmentId expectedSuccessorToken(fallbackSnapshot.superblock().next_logical_segment_id);
		EXPECT_EQ(fallbackSnapshot.superblock().metadata_emergency_reserve.physical_slot, PhysicalSlotId(2U));
		EXPECT_TRUE(store->isPhysicalSlotAllocated(fallbackSnapshot, PhysicalSlotId(2U)));
		EXPECT_FALSE(store->isPhysicalSlotAllocated(fallbackSnapshot, PhysicalSlotId(3U)));
		EXPECT_EQ(store->pagePoolStats().pools, 2U);
		fallbackSnapshot.release();

		const MetadataEmergencyReserveRotation rotation = store->rotateMetadataEmergencyReserve(LogSequenceNumber(3U));
		ASSERT_TRUE(rotation.rotated);
		EXPECT_EQ(rotation.previous_reserve.physical_slot, PhysicalSlotId(2U));
		EXPECT_EQ(rotation.previous_reserve.pool_token, SegmentId(RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN));
		EXPECT_EQ(rotation.current_reserve.physical_slot, PhysicalSlotId(3U));
		EXPECT_EQ(rotation.current_reserve.pool_token, expectedSuccessorToken);
		EXPECT_EQ(store->pagePoolStats().pools, 3U);

		MetadataSnapshotPin current = store->openSnapshot();
		rotated = current.superblock();
		EXPECT_EQ(rotated.metadata_emergency_reserve, rotation.current_reserve);
		EXPECT_TRUE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(2U)));
		EXPECT_TRUE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(3U)));
		EXPECT_EQ(store->findCatalog(current, rotation.previous_reserve.pool_token).physical_slot, PhysicalSlotId(2U));
		current.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock(), rotated);
	EXPECT_EQ(recovered.superblock().metadata_emergency_reserve.physical_slot, PhysicalSlotId(3U));
	EXPECT_EQ(reopened->findCatalog(recovered, SegmentId(RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN))
					  .physical_slot,
			  PhysicalSlotId(2U));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(3U)));
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest,
	   EmergencyReserveRotationRetainsThePublishedDescriptorAcrossDiskFullAndPublicationBoundaries) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	Superblock publishedRotation;
	{
		auto nativeIo = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto store = RelocatableMetadataStore::initializeNew(nativeIo, superblock(1U, 1U));
		MetadataSnapshotPin initial = store->openSnapshot();
		const Superblock initialSuperblock = initial.superblock();
		initial.release();
		const uint64_t successorOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(initialSuperblock.data_region_offset, PhysicalSlotId(3U));

		{
			StorageIO::ScopedNativeWriteHookForTesting fallbackDiskFull(
					[successorOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (offset >= static_cast<pwrite_off_t>(successorOffset) &&
							offset < static_cast<pwrite_off_t>(successorOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			ASSERT_NO_THROW(static_cast<void>(
					store->upsertCatalog(catalogEntriesThatExhaustOneNormalMetadataPool(), LogSequenceNumber(2U))));
		}

		MetadataSnapshotPin fallback = store->openSnapshot();
		const Superblock fallbackSuperblock = fallback.superblock();
		ASSERT_EQ(fallbackSuperblock.metadata_emergency_reserve.physical_slot, PhysicalSlotId(2U));
		fallback.release();

		{
			StorageIO::ScopedNativeWriteHookForTesting rotationDiskFull(
					[successorOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (offset >= static_cast<pwrite_off_t>(successorOffset) &&
							offset < static_cast<pwrite_off_t>(successorOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(store->rotateMetadataEmergencyReserve(LogSequenceNumber(3U))),
						 std::system_error);
		}
		MetadataSnapshotPin afterDiskFull = store->openSnapshot();
		EXPECT_EQ(afterDiskFull.superblock(), fallbackSuperblock);
		EXPECT_FALSE(store->isPhysicalSlotAllocated(afterDiskFull, PhysicalSlotId(3U)));
		afterDiskFull.release();

		EXPECT_THROW(static_cast<void>(store->rotateMetadataEmergencyReserve(
							 LogSequenceNumber(3U),
							 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
					 BootstrapPublishInterrupted);
		MetadataSnapshotPin beforePublication = store->openSnapshot();
		EXPECT_EQ(beforePublication.superblock(), fallbackSuperblock);
		EXPECT_FALSE(store->isPhysicalSlotAllocated(beforePublication, PhysicalSlotId(3U)));
		beforePublication.release();
		EXPECT_EQ(nativeIo->fileSize(), successorOffset);

		EXPECT_THROW(static_cast<void>(store->rotateMetadataEmergencyReserve(
							 LogSequenceNumber(3U),
							 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
					 BootstrapPublishInterrupted);
		MetadataSnapshotPin afterPublication = store->openSnapshot();
		publishedRotation = afterPublication.superblock();
		EXPECT_EQ(publishedRotation.generation, Generation(3U));
		EXPECT_EQ(publishedRotation.metadata_emergency_reserve.physical_slot, PhysicalSlotId(3U));
		EXPECT_TRUE(store->isPhysicalSlotAllocated(afterPublication, PhysicalSlotId(3U)));
		EXPECT_EQ(store->findCatalog(afterPublication, SegmentId(RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN))
						  .physical_slot,
				  PhysicalSlotId(2U));
		afterPublication.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock(), publishedRotation);
	EXPECT_EQ(recovered.superblock().metadata_emergency_reserve.physical_slot, PhysicalSlotId(3U));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(3U)));
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest,
	   EmergencyReserveRotationPrePublicationSyncFailureRetainsTheDescriptorAndReopenCanRetry) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto store = RelocatableMetadataStore::initializeNew(nativeIo, superblock(1U, 1U));
		MetadataSnapshotPin initial = store->openSnapshot();
		const Superblock initialSuperblock = initial.superblock();
		initial.release();
		const uint64_t ordinaryPoolOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(initialSuperblock.data_region_offset, PhysicalSlotId(3U));
		{
			StorageIO::ScopedNativeWriteHookForTesting consumeReserve(
					[ordinaryPoolOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (offset >= static_cast<pwrite_off_t>(ordinaryPoolOffset) &&
								offset < static_cast<pwrite_off_t>(ordinaryPoolOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			ASSERT_NO_THROW(static_cast<void>(
					store->upsertCatalog(catalogEntriesThatExhaustOneNormalMetadataPool(), LogSequenceNumber(2U))));
		}

		MetadataSnapshotPin fallback = store->openSnapshot();
		const Superblock fallbackSuperblock = fallback.superblock();
		ASSERT_EQ(fallbackSuperblock.metadata_emergency_reserve.physical_slot, PhysicalSlotId(2U));
		fallback.release();
		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failDataFence([&syncCalls](file_handle_t fd) {
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
			EXPECT_THROW(static_cast<void>(store->rotateMetadataEmergencyReserve(LogSequenceNumber(3U))),
						 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
		MetadataSnapshotPin current = store->openSnapshot();
		EXPECT_EQ(current.superblock(), fallbackSuperblock);
		EXPECT_EQ(current.superblock().metadata_emergency_reserve.physical_slot, PhysicalSlotId(2U));
		EXPECT_TRUE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(2U)));
		EXPECT_FALSE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(3U)));
		current.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	auto reopened = RelocatableMetadataStore::open(io);
	const MetadataEmergencyReserveRotation retried = reopened->rotateMetadataEmergencyReserve(LogSequenceNumber(3U));
	ASSERT_TRUE(retried.rotated);
	EXPECT_EQ(retried.current_reserve.physical_slot, PhysicalSlotId(3U));
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock(), retried.superblock);
	EXPECT_EQ(recovered.superblock().metadata_emergency_reserve, retried.current_reserve);
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(2U)));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(3U)));
}
