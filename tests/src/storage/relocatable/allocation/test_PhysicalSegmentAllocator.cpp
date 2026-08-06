/**
 * @file test_PhysicalSegmentAllocator.cpp
 * @brief Allocation, integrity, and publication-boundary tests for relocatable physical slots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <source_location>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"

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

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		return result;
	}

	// SegmentId 1 is the bootstrap pool and the following ID belongs to the
	// bootstrap emergency reserve. Test ordinals begin at one after both IDs.
	SegmentId testSegmentId(uint64_t ordinal) { return SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + ordinal - 1U); }

	PhysicalSegmentAllocationRequest request(uint64_t segmentId, std::span<const std::byte> payload = {}) {
		PhysicalSegmentAllocationRequest result;
		result.segment_id = testSegmentId(segmentId);
		result.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
		result.owner_id = 99;
		result.generation = Generation(1);
		result.payload.assign(payload.begin(), payload.end());
		return result;
	}

	MetadataPublishOptions withBootstrapFailpoint(BootstrapPublishFailpoint failpoint) {
		MetadataPublishOptions options;
		options.bootstrap_failpoint = failpoint;
		return options;
	}

	using NativeWriteHookFactoryForTesting = std::function<StorageIO::NativeWriteHookForTesting(const Superblock &)>;

	std::vector<CatalogEntry> catalogEntriesThatExhaustOneNormalMetadataPool() {
		std::vector<CatalogEntry> entries;
		entries.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U);
		for (uint64_t ordinal = 0U; ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U; ++ordinal) {
			CatalogEntry entry;
			entry.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + ordinal * RSS_CATALOG_PAGE_MAX_ENTRIES);
			entry.physical_slot = PhysicalSlotId(1000U + ordinal);
			entry.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
			entry.owner_id = 99U;
			entry.generation = Generation(1U);
			entry.state = SegmentState::SGS_SEALED;
			entry.committed_lsn = LogSequenceNumber(2U);
			entries.push_back(entry);
		}
		return entries;
	}

} // namespace

class RelocatablePhysicalSegmentAllocatorTest : public ::testing::Test {
protected:
	std::filesystem::path dataPath;
	std::shared_ptr<std::fstream> dataFile;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dataPath =
				std::filesystem::temp_directory_path() / ("relocatable_physical_segment_" + to_string(uuid) + ".dat");
		dataFile = std::make_shared<std::fstream>(dataPath,
												  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(dataFile->is_open());
		io = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		if (dataFile && dataFile->is_open()) {
			dataFile->close();
		}
		dataFile.reset();
		std::error_code error;
		std::filesystem::remove(dataPath, error);
	}
};

namespace {

	void expectNativeWriteFailureRetainsOnlyThePublishedGeneration(const std::filesystem::path &dataPath,
																   const std::shared_ptr<std::fstream> &dataFile,
																   const std::shared_ptr<StorageIO> &reopenIo,
																   const NativeWriteHookFactoryForTesting &makeHook) {
		const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
		ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
		{
			auto nativeIo = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
			auto store = RelocatableMetadataStore::initializeNew(nativeIo, superblock(1U, 1U));
			MetadataSnapshotPin before = store->openSnapshot();
			const Superblock initial = before.superblock();
			before.release();
			PhysicalSegmentAllocator allocator(nativeIo, *store);
			const std::array payload = {std::byte{0xE1}, std::byte{0xE2}, std::byte{0xE3}};
			{
				StorageIO::ScopedNativeWriteHookForTesting writeFailure(makeHook(initial));
				EXPECT_THROW(static_cast<void>(allocator.allocate(request(1U, payload), PhysicalSlotId(40U),
																  PhysicalSlotId(40U), LogSequenceNumber(2U))),
							 std::runtime_error);
			}

			MetadataSnapshotPin current = store->openSnapshot();
			EXPECT_EQ(current.superblock().generation, Generation(1U));
			EXPECT_FALSE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(40U)));
			EXPECT_THROW(static_cast<void>(store->findCatalog(current, testSegmentId(1U))), FormatException);
			current.release();
		}
		EXPECT_EQ(portable_close_rw(nativeHandle), 0);

		auto reopened = RelocatableMetadataStore::open(reopenIo);
		MetadataSnapshotPin recovered = reopened->openSnapshot();
		EXPECT_EQ(recovered.superblock().generation, Generation(1U));
		EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
		EXPECT_THROW(static_cast<void>(reopened->findCatalog(recovered, testSegmentId(1U))), FormatException);
		recovered.release();

		PhysicalSegmentAllocator recoveredAllocator(reopenIo, *reopened);
		const std::array retryPayload = {std::byte{0x7E}};
		EXPECT_TRUE(recoveredAllocator
							.allocate(request(1U, retryPayload), PhysicalSlotId(40U), PhysicalSlotId(40U),
									  LogSequenceNumber(2U))
							.has_value());
	}

} // namespace

TEST_F(RelocatablePhysicalSegmentAllocatorTest, AllocatesZeroesPublishesAndReadsOneFixedSlot) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	const std::array payload = {std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
	const std::optional<PhysicalSegmentAllocation> allocation =
			allocator.allocate(request(1, payload), PhysicalSlotId(40), PhysicalSlotId(42), LogSequenceNumber(2));
	ASSERT_TRUE(allocation.has_value());
	EXPECT_EQ(allocation->entry.physical_slot, PhysicalSlotId(40));
	EXPECT_EQ(allocation->entry.committed_lsn, LogSequenceNumber(2));

	MetadataSnapshotPin snapshot = store->openSnapshot();
	const PhysicalSegmentContents contents = allocator.read(snapshot, testSegmentId(1));
	EXPECT_EQ(contents.entry.segment_id, allocation->entry.segment_id);
	EXPECT_EQ(contents.entry.physical_slot, allocation->entry.physical_slot);
	EXPECT_EQ(contents.entry.generation, allocation->entry.generation);
	EXPECT_EQ(contents.envelope.header_bytes, RSS_ENVELOPE_BYTES);
	EXPECT_TRUE(contents.header_extension.empty());
	EXPECT_EQ(contents.envelope.payload_bytes, payload.size());
	EXPECT_EQ(contents.payload, std::vector<std::byte>(payload.begin(), payload.end()));
	EXPECT_TRUE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));

	const uint64_t staleSlotOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, PhysicalSlotId(41));
	const std::array staleBytes = {std::byte{0xA5}, std::byte{0xA5}, std::byte{0xA5}, std::byte{0xA5}};
	io->writeAt(staleSlotOffset + 100U, staleBytes.data(), staleBytes.size());
	const std::array replacementPayload = {std::byte{0x55}};
	const std::optional<PhysicalSegmentAllocation> replacement = allocator.allocate(
			request(2, replacementPayload), PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3));
	ASSERT_TRUE(replacement.has_value());
	std::array<std::byte, 4> clearedBytes{};
	ASSERT_EQ(io->readAt(staleSlotOffset + 100U, clearedBytes.data(), clearedBytes.size()), clearedBytes.size());
	EXPECT_EQ(clearedBytes, (std::array<std::byte, 4>{}));
	EXPECT_FALSE(
			allocator.allocate(request(3), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(4)).has_value());
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest,
	   NativeSuccessfulAllocationUsesExactlyOneDataFenceAndOneSuperblockFence) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	const std::array payload = {std::byte{0xF1}, std::byte{0xE2}};
	{
		auto nativeIo = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto store = RelocatableMetadataStore::initializeNew(nativeIo, superblock(1U, 1U));
		PhysicalSegmentAllocator allocator(nativeIo, *store);
		std::atomic<uint32_t> syncCalls{0U};
		std::optional<PhysicalSegmentAllocation> allocation;
		{
			StorageIO::ScopedNativeSyncHookForTesting countPublicationSyncs([&syncCalls](file_handle_t fd) {
				syncCalls.fetch_add(1U, std::memory_order_relaxed);
				return portable_fsync(fd);
			});
			allocation = allocator.allocate(request(1U, payload), PhysicalSlotId(40U), PhysicalSlotId(40U),
											LogSequenceNumber(2U));
		}

		EXPECT_TRUE(allocation.has_value());
		EXPECT_EQ(syncCalls.load(std::memory_order_relaxed), 2U);
		if (allocation.has_value()) {
			EXPECT_EQ(allocation->superblock.generation, Generation(2U));
		}
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	// Reopening must safely reconstruct the deferred post-publication frame-map reclamation.
	io.reset();
	dataFile->close();
	dataFile = std::make_shared<std::fstream>(dataPath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(dataFile->is_open());
	io = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(2U));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	PhysicalSegmentAllocator recoveredAllocator(io, *reopened);
	EXPECT_EQ(recoveredAllocator.read(recovered, testSegmentId(1U)).payload,
			  (std::vector<std::byte>(payload.begin(), payload.end())));
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest,
	   NativeShortDataSegmentWriteDoesNotPublishAndReopenRetainsOldGeneration) {
	expectNativeWriteFailureRetainsOnlyThePublishedGeneration(dataPath, dataFile, io, [](const Superblock &initial) {
		const uint64_t segmentOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(initial.data_region_offset, PhysicalSlotId(40U));
		return [segmentOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
			if (offset >= static_cast<pwrite_off_t>(segmentOffset) &&
				offset < static_cast<pwrite_off_t>(segmentOffset + RSS_SEGMENT_SIZE) && byteCount > 1U) {
				return portable_pwrite(fd, bytes, byteCount - 1U, offset);
			}
			return portable_pwrite(fd, bytes, byteCount, offset);
		};
	});
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, NativeDataSegmentDiskFullDoesNotPublishAndReopenRetainsOldGeneration) {
	expectNativeWriteFailureRetainsOnlyThePublishedGeneration(dataPath, dataFile, io, [](const Superblock &initial) {
		const uint64_t segmentOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(initial.data_region_offset, PhysicalSlotId(40U));
		return [segmentOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
			if (offset >= static_cast<pwrite_off_t>(segmentOffset) &&
				offset < static_cast<pwrite_off_t>(segmentOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
				SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
				return static_cast<pwrite_ssize_t>(-1);
			}
			return portable_pwrite(fd, bytes, byteCount, offset);
		};
	});
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest,
	   NativeShortMetadataPageWriteDoesNotPublishAndReopenRetainsOldGeneration) {
	expectNativeWriteFailureRetainsOnlyThePublishedGeneration(dataPath, dataFile, io, [](const Superblock &initial) {
		const uint64_t metadataPoolOffset = initial.data_region_offset;
		return [metadataPoolOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
			if (offset >= static_cast<pwrite_off_t>(metadataPoolOffset) &&
				offset < static_cast<pwrite_off_t>(metadataPoolOffset + RSS_SEGMENT_SIZE) && byteCount > 1U) {
				return portable_pwrite(fd, bytes, byteCount - 1U, offset);
			}
			return portable_pwrite(fd, bytes, byteCount, offset);
		};
	});
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, NativeMetadataPageDiskFullDoesNotPublishAndReopenRetainsOldGeneration) {
	expectNativeWriteFailureRetainsOnlyThePublishedGeneration(dataPath, dataFile, io, [](const Superblock &initial) {
		const uint64_t metadataPoolOffset = initial.data_region_offset;
		return [metadataPoolOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
			if (offset >= static_cast<pwrite_off_t>(metadataPoolOffset) &&
				offset < static_cast<pwrite_off_t>(metadataPoolOffset + RSS_SEGMENT_SIZE)) {
#ifdef _WIN32
				SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
				return static_cast<pwrite_ssize_t>(-1);
			}
			return portable_pwrite(fd, bytes, byteCount, offset);
		};
	});
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, NativePrePublicationSyncFailureReclaimsTailAndReopensPriorGeneration) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	const std::array payload = {std::byte{0xC4}, std::byte{0xA7}};
	{
		auto nativeIo = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto store = RelocatableMetadataStore::initializeNew(nativeIo, superblock(1U, 1U));
		const uint64_t initialFileSize = nativeIo->fileSize();
		PhysicalSegmentAllocator allocator(nativeIo, *store);
		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failFirstPublicationSync([&syncCalls](file_handle_t fd) {
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
			EXPECT_THROW(static_cast<void>(allocator.allocate(request(1U, payload), PhysicalSlotId(40U),
															  PhysicalSlotId(40U), LogSequenceNumber(2U))),
						 std::runtime_error);
		}

		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
		EXPECT_EQ(nativeIo->fileSize(), initialFileSize);
		EXPECT_FALSE(store->pagePoolStats().poisoned);
		MetadataSnapshotPin current = store->openSnapshot();
		EXPECT_EQ(current.superblock().generation, Generation(1U));
		EXPECT_FALSE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(40U)));
		EXPECT_THROW(static_cast<void>(store->findCatalog(current, testSegmentId(1U))), FormatException);
		current.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	io.reset();
	dataFile->close();
	dataFile = std::make_shared<std::fstream>(dataPath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(dataFile->is_open());
	io = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(1U));
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	recovered.release();

	PhysicalSegmentAllocator retryAllocator(io, *reopened);
	ASSERT_TRUE(retryAllocator
						.allocate(request(1U, payload), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
						.has_value());
	MetadataSnapshotPin retried = reopened->openSnapshot();
	EXPECT_EQ(retried.superblock().generation, Generation(2U));
	EXPECT_EQ(retryAllocator.read(retried, testSegmentId(1U)).payload,
			  (std::vector<std::byte>(payload.begin(), payload.end())));
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest,
	   PersistentNativeSyncFailurePoisonsWriterUntilReopenAndNeverPublishesTheCandidate) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	const std::array payload = {std::byte{0x5D}};
	uint64_t initialFileSize = 0U;
	{
		auto nativeIo = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto store = RelocatableMetadataStore::initializeNew(nativeIo, superblock(1U, 1U));
		initialFileSize = nativeIo->fileSize();
		PhysicalSegmentAllocator allocator(nativeIo, *store);
		{
			StorageIO::ScopedNativeSyncHookForTesting failEverySync([](file_handle_t) {
#ifdef _WIN32
				SetLastError(ERROR_DISK_FULL);
#else
				errno = ENOSPC;
#endif
				return -1;
			});
			EXPECT_THROW(static_cast<void>(allocator.allocate(request(1U, payload), PhysicalSlotId(40U),
															  PhysicalSlotId(40U), LogSequenceNumber(2U))),
						 std::runtime_error);
		}

		EXPECT_GT(nativeIo->fileSize(), initialFileSize);
		EXPECT_TRUE(store->pagePoolStats().poisoned);
		MetadataSnapshotPin current = store->openSnapshot();
		EXPECT_EQ(current.superblock().generation, Generation(1U));
		EXPECT_FALSE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(40U)));
		EXPECT_THROW(static_cast<void>(store->findCatalog(current, testSegmentId(1U))), FormatException);
		current.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	io.reset();
	dataFile->close();
	dataFile = std::make_shared<std::fstream>(dataPath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(dataFile->is_open());
	io = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto reopened = RelocatableMetadataStore::open(io);
	EXPECT_FALSE(reopened->pagePoolStats().poisoned);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(1U));
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	recovered.release();

	PhysicalSegmentAllocator retryAllocator(io, *reopened);
	ASSERT_TRUE(retryAllocator
						.allocate(request(1U, payload), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
						.has_value());
}

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

TEST_F(RelocatablePhysicalSegmentAllocatorTest, OwnsCodecBytesBeforeMetadataPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	const std::vector<std::byte> expectedHeader = {std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
												   std::byte{0x50}, std::byte{0x60}, std::byte{0x70}, std::byte{0x80}};
	const std::vector<std::byte> expectedPayload = {std::byte{0xA1}, std::byte{0xB2}, std::byte{0xC3}};
	std::vector<std::byte> callerHeader = expectedHeader;
	std::vector<std::byte> callerPayload = expectedPayload;
	PhysicalSegmentAllocationRequest allocationRequest{testSegmentId(1),
													   SegmentKind::SGK_ENTITY_PAGE_POOL,
													   99U,
													   Generation(1U),
													   static_cast<uint16_t>(RSS_ENVELOPE_BYTES + callerHeader.size()),
													   callerHeader,
													   callerPayload};
	callerHeader.assign(callerHeader.size(), std::byte{0xFF});
	callerPayload.assign(callerPayload.size(), std::byte{0xEE});

	ASSERT_TRUE(allocator
						.allocate(std::move(allocationRequest), PhysicalSlotId(40), PhysicalSlotId(40),
								  LogSequenceNumber(2))
						.has_value());
	const MetadataSnapshotPin snapshot = store->openSnapshot();
	const PhysicalSegmentContents contents = allocator.read(snapshot, testSegmentId(1));
	EXPECT_EQ(contents.header_extension, expectedHeader);
	EXPECT_EQ(contents.payload, expectedPayload);
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, PersistsAndReadsExplicitSubtypeHeaderExtensions) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	const std::array header = {std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
							   std::byte{0x50}, std::byte{0x60}, std::byte{0x70}, std::byte{0x80}};
	const std::array payload = {std::byte{0xAB}, std::byte{0xCD}};
	PhysicalSegmentAllocationRequest allocationRequest = request(1, payload);
	allocationRequest.header_bytes = RSS_ENVELOPE_BYTES + header.size();
	allocationRequest.header_extension.assign(header.begin(), header.end());
	ASSERT_TRUE(allocator.allocate(allocationRequest, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());

	const MetadataSnapshotPin snapshot = store->openSnapshot();
	const PhysicalSegmentContents contents = allocator.read(snapshot, testSegmentId(1));
	EXPECT_EQ(contents.envelope.header_bytes, RSS_ENVELOPE_BYTES + header.size());
	EXPECT_EQ(contents.header_extension, std::vector<std::byte>(header.begin(), header.end()));
	EXPECT_EQ(contents.payload, std::vector<std::byte>(payload.begin(), payload.end()));
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, AtomicallyAllocatesBatchesAndRejectsDuplicateLogicalIds) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	const std::array firstPayload = {std::byte{1}};
	const std::array secondPayload = {std::byte{2}, std::byte{3}};
	const std::vector<PhysicalSegmentAllocationRequest> requests = {request(1, firstPayload),
																	request(2, secondPayload)};
	const std::optional<PhysicalSegmentAllocations> allocations =
			allocator.allocate(requests, PhysicalSlotId(40), PhysicalSlotId(42), LogSequenceNumber(2));
	ASSERT_TRUE(allocations.has_value());
	ASSERT_EQ(allocations->entries.size(), 2U);
	EXPECT_EQ(allocations->entries[0].physical_slot, PhysicalSlotId(40));
	EXPECT_EQ(allocations->entries[1].physical_slot, PhysicalSlotId(41));

	expectFormatError(
			[&] {
				static_cast<void>(
						allocator.allocate(request(1), PhysicalSlotId(42), PhysicalSlotId(42), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(2));
	EXPECT_EQ(allocator.read(snapshot, testSegmentId(1)).payload,
			  std::vector<std::byte>(firstPayload.begin(), firstPayload.end()));
	EXPECT_EQ(allocator.read(snapshot, testSegmentId(2)).payload,
			  std::vector<std::byte>(secondPayload.begin(), secondPayload.end()));
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, PublishesFullSegmentExtentBindingsAndRejectsMalformedBindings) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	const PhysicalSegmentAllocationRequest first = request(1);
	const ExtentLocation firstExtent{ExtentId(101), testSegmentId(1), RSS_PAGE_POOL_FIRST_DATA_FRAME,
									 RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1)};
	ASSERT_TRUE(allocator
						.allocateExtentSegments(std::span(&first, 1), std::span(&firstExtent, 1), PhysicalSlotId(40),
												PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin published = store->openSnapshot();
	EXPECT_EQ(store->findExtentLocation(published, ExtentId(101)), firstExtent);
	published.release();

	const PhysicalSegmentAllocationRequest second = request(2);
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocateExtentSegments(std::span(&second, 1), {}, PhysicalSlotId(41),
																   PhysicalSlotId(41), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_HEADER);
	const ExtentLocation missingSegment{ExtentId(102), testSegmentId(3), RSS_PAGE_POOL_FIRST_DATA_FRAME,
										RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1)};
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocateExtentSegments(std::span(&second, 1), std::span(&missingSegment, 1),
																   PhysicalSlotId(41), PhysicalSlotId(41),
																   LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_HEADER);
	// A below-range ID reaches lower_bound but must still be rejected because it
	// is not one of the requested Segment IDs.
	const ExtentLocation lowerBoundMismatch{ExtentId(103), testSegmentId(1), RSS_PAGE_POOL_FIRST_DATA_FRAME,
											RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1)};
	expectFormatError(
			[&] {
				static_cast<void>(
						allocator.allocateExtentSegments(std::span(&second, 1), std::span(&lowerBoundMismatch, 1),
														 PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_HEADER);
	const ExtentLocation staleGeneration{ExtentId(102), testSegmentId(2), RSS_PAGE_POOL_FIRST_DATA_FRAME,
										 RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(2)};
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocateExtentSegments(std::span(&second, 1),
																   std::span(&staleGeneration, 1), PhysicalSlotId(41),
																   PhysicalSlotId(41), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_HEADER);
	ExtentLocation partialExtent{ExtentId(102), testSegmentId(2), RSS_PAGE_POOL_FIRST_DATA_FRAME, 1, Generation(1)};
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocateExtentSegments(std::span(&second, 1), std::span(&partialExtent, 1),
																   PhysicalSlotId(41), PhysicalSlotId(41),
																   LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_HEADER);

	const std::array batch = {request(2), request(3)};
	const std::array duplicateExtentIds = {
			ExtentLocation{ExtentId(102), testSegmentId(2), RSS_PAGE_POOL_FIRST_DATA_FRAME,
						   RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1)},
			ExtentLocation{ExtentId(102), testSegmentId(3), RSS_PAGE_POOL_FIRST_DATA_FRAME,
						   RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1)}};
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocateExtentSegments(batch, duplicateExtentIds, PhysicalSlotId(41),
																   PhysicalSlotId(42), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::array duplicateSegmentIds = {
			ExtentLocation{ExtentId(102), testSegmentId(2), RSS_PAGE_POOL_FIRST_DATA_FRAME,
						   RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1)},
			ExtentLocation{ExtentId(103), testSegmentId(2), RSS_PAGE_POOL_FIRST_DATA_FRAME,
						   RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1)}};
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocateExtentSegments(batch, duplicateSegmentIds, PhysicalSlotId(41),
																   PhysicalSlotId(42), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, RetiresAfterPublicationAndReusesOnlyAfterTheLastSnapshotReleases) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	const std::array payload = {std::byte{0x11}};
	ASSERT_TRUE(allocator.allocate(request(1, payload), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin oldSnapshot = store->openSnapshot();
	const Superblock retired = allocator.retire(testSegmentId(1), Generation(1), LogSequenceNumber(3));
	EXPECT_EQ(retired.generation, Generation(3));
	expectFormatError(
			[&] { static_cast<void>(allocator.retire(testSegmentId(1), Generation(1), LogSequenceNumber(4))); },
			FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(allocator.read(oldSnapshot, testSegmentId(1)).payload,
			  std::vector<std::byte>(payload.begin(), payload.end()));
	MetadataSnapshotPin retiredSnapshot = store->openSnapshot();
	expectFormatError([&] { static_cast<void>(allocator.read(retiredSnapshot, testSegmentId(1))); },
					  FormatError::FERR_INVALID_STATE);
	retiredSnapshot.release();
	expectFormatError(
			[&] { static_cast<void>(allocator.releaseRetired(testSegmentId(1), Generation(1), LogSequenceNumber(4))); },
			FormatError::FERR_INVALID_STATE);
	oldSnapshot.release();
	const Superblock released = allocator.releaseRetired(testSegmentId(1), Generation(1), LogSequenceNumber(4));
	EXPECT_EQ(released.generation, Generation(4));
	MetadataSnapshotPin freeSnapshot = store->openSnapshot();
	EXPECT_FALSE(store->isPhysicalSlotAllocated(freeSnapshot, PhysicalSlotId(40)));
	EXPECT_EQ(store->findCatalog(freeSnapshot, testSegmentId(1)).state, SegmentState::SGS_OBSOLETE);
	freeSnapshot.release();
	expectFormatError(
			[&] { static_cast<void>(allocator.releaseRetired(testSegmentId(1), Generation(1), LogSequenceNumber(5))); },
			FormatError::FERR_INVALID_STATE);

	const std::optional<PhysicalSegmentAllocation> reused =
			allocator.allocate(request(2), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(5));
	ASSERT_TRUE(reused.has_value());
	EXPECT_EQ(reused->entry.physical_slot, PhysicalSlotId(40));
	MetadataSnapshotPin reusedSnapshot = store->openSnapshot();
	EXPECT_EQ(allocator.read(reusedSnapshot, testSegmentId(2)).payload.size(), 0U);
	expectFormatError([&] { static_cast<void>(allocator.read(reusedSnapshot, testSegmentId(1))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, RelocatesWholeSegmentsAndDefersSourceReuseUntilSnapshotsRelease) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	const std::array payload = {std::byte{0x1A}, std::byte{0x2B}, std::byte{0x3C}};
	ASSERT_TRUE(allocator.allocate(request(1, payload), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin oldSnapshot = store->openSnapshot();
	EXPECT_FALSE(allocator
						 .relocate(testSegmentId(1), Generation(1), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(3))
						 .has_value());
	const std::optional<PhysicalSegmentRelocation> relocation = allocator.relocate(
			testSegmentId(1), Generation(1), PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3));
	ASSERT_TRUE(relocation.has_value());
	EXPECT_EQ(relocation->source.physical_slot, PhysicalSlotId(40));
	EXPECT_EQ(relocation->destination.physical_slot, PhysicalSlotId(41));
	EXPECT_EQ(relocation->destination.generation, Generation(2));
	MetadataSnapshotPin queuedSnapshot = store->openSnapshot();
	EXPECT_EQ(store->findRetireQueueEntryOptional(queuedSnapshot, PhysicalSlotId(40)),
			  std::optional<RetireQueueEntry>({PhysicalSlotId(40), testSegmentId(1), Generation(3),
											   RetireQueueDisposition::RQD_RELOCATED_SOURCE}));
	queuedSnapshot.release();
	EXPECT_EQ(allocator.read(oldSnapshot, testSegmentId(1)).payload,
			  std::vector<std::byte>(payload.begin(), payload.end()));
	expectFormatError(
			[&] {
				static_cast<void>(allocator.relocate(testSegmentId(1), Generation(2), PhysicalSlotId(42),
													 PhysicalSlotId(42), LogSequenceNumber(4)));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin relocatedSnapshot = store->openSnapshot();
	EXPECT_EQ(allocator.read(relocatedSnapshot, testSegmentId(1)).payload,
			  std::vector<std::byte>(payload.begin(), payload.end()));
	expectFormatError(
			[&] {
				static_cast<void>(
						allocator.releaseRelocatedSource(testSegmentId(1), Generation(2), LogSequenceNumber(4)));
			},
			FormatError::FERR_INVALID_STATE);
	relocatedSnapshot.release();
	oldSnapshot.release();
	const Superblock released = allocator.releaseRelocatedSource(testSegmentId(1), Generation(2), LogSequenceNumber(4));
	EXPECT_EQ(released.generation, Generation(4));
	MetadataSnapshotPin current = store->openSnapshot();
	const CatalogEntry currentEntry = store->findCatalog(current, testSegmentId(1));
	EXPECT_FALSE(store->findRetireQueueEntryOptional(current, PhysicalSlotId(40)).has_value());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(40)));
	EXPECT_TRUE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(41)));
	EXPECT_EQ(allocator.read(current, testSegmentId(1)).payload,
			  std::vector<std::byte>(payload.begin(), payload.end()));
	current.release();
	expectFormatError(
			[&] {
				static_cast<void>(
						allocator.releaseRelocatedSource(testSegmentId(1), Generation(2), LogSequenceNumber(5)));
			},
			FormatError::FERR_INVALID_STATE);
	const std::optional<PhysicalSegmentAllocation> reused =
			allocator.allocate(request(2), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(5));
	ASSERT_TRUE(reused.has_value());
	EXPECT_EQ(reused->entry.physical_slot, PhysicalSlotId(40));
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest,
	   PublishesRetireQueueReclamationAtomicallyAcrossCrashRecoveryBoundaries) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocate(request(1), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)).has_value());
	MetadataSnapshotPin beforeRelocation = store->openSnapshot();
	ASSERT_TRUE(allocator
						.relocate(testSegmentId(1), Generation(1), PhysicalSlotId(41), PhysicalSlotId(41),
								  LogSequenceNumber(3))
						.has_value());
	beforeRelocation.release();

	EXPECT_THROW(static_cast<void>(store->reclaimRetiredPhysicalSlots(
						 LogSequenceNumber(4),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	auto beforePublication = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin queued = beforePublication->openSnapshot();
	EXPECT_EQ(queued.superblock().generation, Generation(3));
	EXPECT_TRUE(beforePublication->findRetireQueueEntryOptional(queued, PhysicalSlotId(40)).has_value());
	EXPECT_TRUE(beforePublication->isPhysicalSlotAllocated(queued, PhysicalSlotId(40)));
	queued.release();

	EXPECT_THROW(
			static_cast<void>(beforePublication->reclaimRetiredPhysicalSlots(
					LogSequenceNumber(4), withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
			BootstrapPublishInterrupted);
	auto afterPublication = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin reclaimed = afterPublication->openSnapshot();
	EXPECT_EQ(reclaimed.superblock().generation, Generation(4));
	EXPECT_FALSE(afterPublication->findRetireQueueEntryOptional(reclaimed, PhysicalSlotId(40)).has_value());
	EXPECT_FALSE(afterPublication->isPhysicalSlotAllocated(reclaimed, PhysicalSlotId(40)));
	EXPECT_TRUE(afterPublication->isPhysicalSlotAllocated(reclaimed, PhysicalSlotId(41)));
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, ReclaimsRetiredWholeSegmentsAfterReopenAndLastSnapshotRelease) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	ASSERT_TRUE(
			allocator.allocate(request(1), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)).has_value());
	MetadataSnapshotPin beforeRetirement = store->openSnapshot();
	static_cast<void>(allocator.retire(testSegmentId(1), Generation(1), LogSequenceNumber(3)));
	EXPECT_EQ(store->reclaimRetiredPhysicalSlots(LogSequenceNumber(4)).reclaimed_physical_slots, 0U);
	beforeRetirement.release();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin queued = reopened->openSnapshot();
	EXPECT_EQ(reopened->findRetireQueueEntryOptional(queued, PhysicalSlotId(40)),
			  std::optional<RetireQueueEntry>({PhysicalSlotId(40), testSegmentId(1), Generation(3),
											   RetireQueueDisposition::RQD_RETIRED_SEGMENT}));
	queued.release();
	EXPECT_EQ(reopened->reclaimRetiredPhysicalSlots(LogSequenceNumber(4)).reclaimed_physical_slots, 1U);
	MetadataSnapshotPin reclaimed = reopened->openSnapshot();
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(reclaimed, PhysicalSlotId(40)));
	EXPECT_FALSE(reopened->findRetireQueueEntryOptional(reclaimed, PhysicalSlotId(40)).has_value());
	EXPECT_EQ(reopened->findCatalog(reclaimed, testSegmentId(1)).state, SegmentState::SGS_OBSOLETE);
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, RejectsCorruptRelocationSourceWithoutPublishingTheDestination) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	const std::array payload = {std::byte{0x91}, std::byte{0x92}};
	ASSERT_TRUE(allocator.allocate(request(1, payload), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin before = store->openSnapshot();
	const uint64_t sourceOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(before.superblock().data_region_offset, PhysicalSlotId(40));
	const std::byte corrupt{0xFF};
	io->writeAt(sourceOffset + RSS_ENVELOPE_BYTES, &corrupt, sizeof(corrupt));
	expectFormatError(
			[&] {
				static_cast<void>(allocator.relocate(testSegmentId(1), Generation(1), PhysicalSlotId(41),
													 PhysicalSlotId(41), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_STATE);
	before.release();
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(current.superblock().generation, Generation(2));
	EXPECT_EQ(store->findCatalog(current, testSegmentId(1)).physical_slot, PhysicalSlotId(40));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(41)));
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest,
	   RecoversPriorGenerationAfterInterruptedRelocationBeforeSuperblockPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const std::array payload = {std::byte{0x44}};
	{
		PhysicalSegmentAllocator allocator(io, *store);
		ASSERT_TRUE(
				allocator.allocate(request(1, payload), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
		MetadataPublishOptions options;
		options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(allocator.relocate(testSegmentId(1), Generation(1), PhysicalSlotId(41),
														  PhysicalSlotId(41), LogSequenceNumber(3), options)),
					 BootstrapPublishInterrupted);
	}
	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator allocator(io, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(2));
	EXPECT_EQ(allocator.read(snapshot, testSegmentId(1)).payload,
			  std::vector<std::byte>(payload.begin(), payload.end()));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(41)));
	snapshot.release();
	EXPECT_TRUE(allocator
						.relocate(testSegmentId(1), Generation(1), PhysicalSlotId(41), PhysicalSlotId(41),
								  LogSequenceNumber(3))
						.has_value());
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, DoesNotWriteOrPublishWhenTheRequestedRangeCannotFitTheBatch) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	const std::vector<PhysicalSegmentAllocationRequest> requests = {request(1), request(2), request(3)};
	EXPECT_FALSE(
			allocator.allocate(requests, PhysicalSlotId(40), PhysicalSlotId(41), LogSequenceNumber(2)).has_value());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(1));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(41)));
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, RejectsMalformedRequestsBeforeMetadataMutation) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocate(std::span<const PhysicalSegmentAllocationRequest>{},
													 PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);

	PhysicalSegmentAllocationRequest badKind = request(1);
	badKind.kind = SegmentKind::SGK_METADATA_PAGE_POOL;
	expectFormatError(
			[&] {
				static_cast<void>(
						allocator.allocate(badKind, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_ENUM);
	PhysicalSegmentAllocationRequest noKind = request(1);
	noKind.kind = SegmentKind::SGK_NONE;
	expectFormatError(
			[&] {
				static_cast<void>(
						allocator.allocate(noKind, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_ENUM);
	PhysicalSegmentAllocationRequest badId = request(1);
	badId.segment_id = SegmentId{};
	expectFormatError(
			[&] {
				static_cast<void>(
						allocator.allocate(badId, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	PhysicalSegmentAllocationRequest reservedBootstrapId = request(1);
	reservedBootstrapId.segment_id = SegmentId(RSS_BOOTSTRAP_METADATA_SEGMENT_ID);
	io->resetStats();
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocate(reservedBootstrapId, PhysicalSlotId(40), PhysicalSlotId(40),
													 LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(io->stats().write_calls, 0U);
	EXPECT_EQ(io->stats().write_bytes, 0U);
	PhysicalSegmentAllocationRequest shortHeader = request(1);
	shortHeader.header_bytes = RSS_ENVELOPE_BYTES - 8U;
	expectFormatError(
			[&] {
				static_cast<void>(
						allocator.allocate(shortHeader, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	PhysicalSegmentAllocationRequest badHeader = request(1);
	badHeader.header_bytes = RSS_ENVELOPE_BYTES + 1U;
	expectFormatError(
			[&] {
				static_cast<void>(
						allocator.allocate(badHeader, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	PhysicalSegmentAllocationRequest missingExtension = request(1);
	missingExtension.header_bytes = RSS_ENVELOPE_BYTES + 8U;
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocate(missingExtension, PhysicalSlotId(40), PhysicalSlotId(40),
													 LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::array shortExtension = {std::byte{1}};
	PhysicalSegmentAllocationRequest shortExtensionRequest = request(1);
	shortExtensionRequest.header_bytes = RSS_ENVELOPE_BYTES + 8U;
	shortExtensionRequest.header_extension.assign(shortExtension.begin(), shortExtension.end());
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocate(shortExtensionRequest, PhysicalSlotId(40), PhysicalSlotId(40),
													 LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	PhysicalSegmentAllocationRequest oversizedHeader = request(1);
	oversizedHeader.header_bytes = std::numeric_limits<uint16_t>::max();
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocate(oversizedHeader, PhysicalSlotId(40), PhysicalSlotId(40),
													 LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> oversizedPayload(RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES + 1U);
	PhysicalSegmentAllocationRequest oversized = request(1, oversizedPayload);
	expectFormatError(
			[&] {
				static_cast<void>(
						allocator.allocate(oversized, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	PhysicalSegmentAllocationRequest badGeneration = request(1);
	badGeneration.generation = Generation{};
	expectFormatError(
			[&] {
				static_cast<void>(allocator.allocate(badGeneration, PhysicalSlotId(40), PhysicalSlotId(40),
													 LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::vector<PhysicalSegmentAllocationRequest> unordered = {request(2), request(1)};
	expectFormatError(
			[&] {
				static_cast<void>(
						allocator.allocate(unordered, PhysicalSlotId(40), PhysicalSlotId(41), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(1));
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, RejectsCorruptPayloadEnvelopeAndFreeSpaceMappings) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	const std::array payload = {std::byte{0x11}, std::byte{0x22}};
	const std::optional<PhysicalSegmentAllocation> allocation =
			allocator.allocate(request(1, payload), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2));
	ASSERT_TRUE(allocation.has_value());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																		allocation->entry.physical_slot);
	const std::byte corrupt{0xFF};
	io->writeAt(offset + RSS_ENVELOPE_BYTES, &corrupt, sizeof(corrupt));
	expectFormatError([&] { static_cast<void>(allocator.read(snapshot, testSegmentId(1))); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
	snapshot.release();

	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40), false, LogSequenceNumber(3)));
	MetadataSnapshotPin released = store->openSnapshot();
	expectFormatError([&] { static_cast<void>(allocator.read(released, testSegmentId(1))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, ReopensAndValidatesTheCatalogEnvelopeIdentity) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const std::array payload = {std::byte{0xAA}};
	{
		PhysicalSegmentAllocator allocator(io, *store);
		ASSERT_TRUE(
				allocator.allocate(request(1, payload), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	}
	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator allocator(io, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(allocator.read(snapshot, testSegmentId(1)).payload,
			  std::vector<std::byte>(payload.begin(), payload.end()));

	const CatalogEntry entry = reopened->findCatalog(snapshot, testSegmentId(1));
	SegmentEnvelope badEnvelope;
	badEnvelope.kind = entry.kind;
	badEnvelope.state = SegmentState::SGS_SEALED;
	badEnvelope.segment_id = entry.segment_id;
	badEnvelope.owner_id = entry.owner_id + 1U;
	badEnvelope.generation = entry.generation;
	badEnvelope.committed_lsn = entry.committed_lsn;
	badEnvelope.payload_crc = 0;
	const auto encoded = SegmentEnvelopeCodec::encode(badEnvelope);
	io->writeAt(
			PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset, entry.physical_slot),
			encoded.data(), encoded.size());
	expectFormatError([&] { static_cast<void>(allocator.read(snapshot, testSegmentId(1))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, LeavesDataUnreachableWhenMetadataPublicationStopsBeforeTheWalRecord) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	PhysicalSegmentAllocator allocator(io, *store);
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(allocator.allocate(request(1), PhysicalSlotId(40), PhysicalSlotId(40),
													  LogSequenceNumber(2), options)),
				 BootstrapPublishInterrupted);
	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator reopenedAllocator(io, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(1));
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
	snapshot.release();
	EXPECT_TRUE(reopenedAllocator.allocate(request(1), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, FailsClosedWhenMetadataReferencesAnUnwrittenPhysicalSlot) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest catalogRequest;
	catalogRequest.segment_id = testSegmentId(1);
	catalogRequest.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	catalogRequest.owner_id = 99;
	catalogRequest.segment_generation = Generation(1);
	catalogRequest.state = SegmentState::SGS_SEALED;
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(catalogRequest, PhysicalSlotId(40), PhysicalSlotId(40),
														LogSequenceNumber(2))
						.has_value());
	PhysicalSegmentAllocator allocator(io, *store);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	expectFormatError([&] { static_cast<void>(allocator.read(snapshot, testSegmentId(1))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
}

TEST_F(RelocatablePhysicalSegmentAllocatorTest, ValidatesStorageIdentityAndCheckedOffsets) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	EXPECT_NO_THROW(PhysicalSegmentAllocator(io, *store));
	const auto foreignIo = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	EXPECT_THROW(PhysicalSegmentAllocator(foreignIo, *store), std::invalid_argument);
	EXPECT_THROW(PhysicalSegmentAllocator(nullptr, *store), std::invalid_argument);
	EXPECT_EQ(PhysicalSegmentAllocator::checkedSlotOffset(RSS_FILE_BOOTSTRAP_BYTES, PhysicalSlotId(1)),
			  static_cast<uint64_t>(RSS_FILE_BOOTSTRAP_BYTES));
	expectFormatError(
			[&] {
				static_cast<void>(
						PhysicalSegmentAllocator::checkedSlotOffset(RSS_FILE_BOOTSTRAP_BYTES, PhysicalSlotId{}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						PhysicalSegmentAllocator::checkedSlotOffset(RSS_FILE_BOOTSTRAP_BYTES + 1U, PhysicalSlotId(1)));
			},
			FormatError::FERR_INVALID_HEADER);
	const uint64_t capacity = (std::numeric_limits<uint64_t>::max() - RSS_FILE_BOOTSTRAP_BYTES) / RSS_SEGMENT_SIZE;
	expectFormatError(
			[&] {
				static_cast<void>(PhysicalSegmentAllocator::checkedSlotOffset(RSS_FILE_BOOTSTRAP_BYTES,
																			  PhysicalSlotId(capacity + 1U)));
			},
			FormatError::FERR_ARITHMETIC_OVERFLOW);
	const uint64_t terminalAlignedOffset =
			std::numeric_limits<uint64_t>::max() - std::numeric_limits<uint64_t>::max() % RSS_SEGMENT_SIZE;
	expectFormatError(
			[&] {
				static_cast<void>(
						PhysicalSegmentAllocator::checkedSlotOffset(terminalAlignedOffset, PhysicalSlotId(1)));
			},
			FormatError::FERR_ARITHMETIC_OVERFLOW);
}
