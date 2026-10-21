/**
 * @file test_AllocatorDirectory.cpp
 * @brief Copy-on-write and reopen tests for the relocatable free-space directory.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

#ifdef _WIN32
#include <windows.h>
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

	Superblock superblock(uint64_t generation, uint64_t lsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(lsn);
		return result;
	}

	class TestAllocatorPageAllocator final : public MetadataPageAllocator {
	public:
		MetadataPageId allocateMetadataPageId() override { return invalid_page_id ? MetadataPageId{} : MetadataPageId(next_page_id++); }

		MetadataPageAllocation allocatePage(Generation generation, LogSequenceNumber,
											std::span<const PhysicalSlotId>) override {
			const std::optional<PhysicalSlotId> createdPool =
					allocation_count < created_pools.size() ? created_pools[allocation_count] : std::nullopt;
			++allocation_count;
			if (invalid_physical_reference) {
				return {PhysicalPageRef{}, createdPool};
			}
			if (overflow_physical_reference) {
				const PhysicalPageRef reference{PhysicalSlotId(std::numeric_limits<uint64_t>::max()), 1U, generation};
				returned_references.push_back(reference);
				return {reference, createdPool};
			}
			if (duplicate_physical_reference && !returned_references.empty()) {
				return {returned_references.front(), createdPool};
			}
			const uint64_t index = next_physical_page++;
			const PhysicalPageRef reference{PhysicalSlotId(index / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U),
											static_cast<uint8_t>(index % RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U),
											wrong_physical_generation ? Generation(generation.value() + 1U)
																	  : generation};
			returned_references.push_back(reference);
			return {reference, createdPool};
		}

		void abandonPage(PhysicalPageRef reference) noexcept override { abandoned_references.push_back(reference); }
		void abandonUnpublishedPool(PhysicalSlotId slot) noexcept override { abandoned_pools.push_back(slot); }

		bool invalid_page_id = false;
		bool invalid_physical_reference = false;
		bool overflow_physical_reference = false;
		bool duplicate_physical_reference = false;
		bool wrong_physical_generation = false;
		uint64_t next_page_id = 1;
		uint64_t next_physical_page = 0;
		uint64_t allocation_count = 0;
		std::vector<std::optional<PhysicalSlotId>> created_pools;
		std::vector<PhysicalPageRef> returned_references;
		std::vector<PhysicalPageRef> abandoned_references;
		std::vector<PhysicalSlotId> abandoned_pools;
	};

} // namespace

class RelocatableAllocatorDirectoryTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("relocatable_allocator_" + to_string(uuid) + ".dat");
		openFile(std::ios::trunc);
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

	void reopen() {
		io.reset();
		file->close();
		file.reset();
		openFile({});
	}

	void openFile(std::ios::openmode extraMode) {
		file = std::make_shared<std::fstream>(testFilePath,
											  std::ios::in | std::ios::out | std::ios::binary | extraMode);
		ASSERT_TRUE(file->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}
};

TEST_F(RelocatableAllocatorDirectoryTest, ResolvesAllocatorRootThroughPublishedMetadataBootstrapRoutes) {
	auto metadata = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	ASSERT_TRUE(snapshot.superblock().usesPageDirectoryBootstrap());
	MetadataPageFile pages(io, snapshot.superblock());
	MetadataPagePoolAllocator allocator(io, snapshot.superblock(), MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
	AllocatorDirectory directory(pages, allocator);
	const std::optional<PhysicalSlotId> free =
			directory.findFirstFree(snapshot.superblock(), PhysicalSlotId(1U), PhysicalSlotId(256U));
	EXPECT_TRUE(free.has_value());
	snapshot.release();
}

TEST_F(RelocatableAllocatorDirectoryTest, LogicalizesOwnedAllocatorRootBeforeBootstrapRoutePublication) {
	auto metadata = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();

	ASSERT_TRUE(current.usesPageDirectoryBootstrap());
	MetadataPageFile pages(io, current);
	MetadataPagePoolAllocator allocator(io, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
	AllocatorDirectory directory(pages, allocator);
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(current.checkpoint_lsn.value() + 1U));

	const std::optional<PhysicalSlotId> reserved =
			directory.prepareReserveFirstFree(batch, PhysicalSlotId(1U), PhysicalSlotId(256U));
	ASSERT_TRUE(reserved.has_value());
	ASSERT_TRUE(batch.ownsAllocatedPage(batch.candidate().allocator_root));
	EXPECT_FALSE(pages.readMetadataRadixUnresolved(batch.candidate().allocator_root).logical_children_enabled);

	directory.prepareLogicalRootChildren(batch);
	EXPECT_TRUE(pages.readMetadataRadixUnresolved(batch.candidate().allocator_root).logical_children_enabled);
}

TEST_F(RelocatableAllocatorDirectoryTest, PreparesFurtherReservationsThroughLogicalCandidateRootRoutes) {
	auto metadata = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();

	MetadataPageFile pages(io, current);
	MetadataPagePoolAllocator allocator(io, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
	AllocatorDirectory directory(pages, allocator);
	MetadataWriteBatch batch(allocator, current, LogSequenceNumber(current.checkpoint_lsn.value() + 1U));

	const std::optional<PhysicalSlotId> first =
			directory.prepareReserveFirstFree(batch, PhysicalSlotId(1U), PhysicalSlotId(256U));
	ASSERT_TRUE(first.has_value());
	directory.prepareLogicalRootChildren(batch);
	ASSERT_TRUE(pages.readMetadataRadixUnresolved(batch.candidate().allocator_root).logical_children_enabled);

	const std::optional<PhysicalSlotId> second =
			directory.prepareReserveFirstFree(batch, PhysicalSlotId(first->value() + 1U), PhysicalSlotId(256U));
	ASSERT_TRUE(second.has_value());
	EXPECT_GT(second->value(), first->value());
}

TEST_F(RelocatableAllocatorDirectoryTest, PublishesCopyOnWriteSlotTransitionsAndPreservesOldRoots) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);

	EXPECT_FALSE(directory.isAllocated(initial, PhysicalSlotId(1)));
	const Superblock allocated =
			directory.setAllocatedAndPublish(bootstrap, initial, PhysicalSlotId(1), true, LogSequenceNumber(2));
	EXPECT_TRUE(directory.isAllocated(allocated, PhysicalSlotId(1)));
	EXPECT_FALSE(directory.isAllocated(initial, PhysicalSlotId(1)));

	const Superblock released =
			directory.setAllocatedAndPublish(bootstrap, allocated, PhysicalSlotId(1), false, LogSequenceNumber(3));
	EXPECT_FALSE(directory.isAllocated(released, PhysicalSlotId(1)));
	EXPECT_TRUE(directory.isAllocated(allocated, PhysicalSlotId(1)));
	EXPECT_NE(allocated.allocator_root, released.allocator_root);
}

TEST_F(RelocatableAllocatorDirectoryTest, NativeCandidateShortWriteRetainsOldGenerationAndAllowsRetry) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const Superblock initial = superblock(1U, 1U);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(nativeIo, initial);
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		AllocatorDirectory directory(pages, allocator);
		bool shortWriteObserved = false;
		{
			StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
					[&shortWriteObserved](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (!shortWriteObserved && byteCount == RSS_PAGE_SIZE) {
							shortWriteObserved = true;
							return portable_pwrite(fd, bytes, RSS_PAGE_SIZE - 1U, offset);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(
						bootstrap, initial, PhysicalSlotId(1U), true, LogSequenceNumber(2U))), std::system_error);
		}
		EXPECT_TRUE(shortWriteObserved);
		EXPECT_EQ(bootstrap.recover().generation, initial.generation);
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	MetadataPageFile reopenedPages(io, recovered);
	MetadataPagePoolAllocator reopenedAllocator(io, recovered);
	AllocatorDirectory reopenedDirectory(reopenedPages, reopenedAllocator);
	const Superblock published = reopenedDirectory.setAllocatedAndPublish(
			reopenedBootstrap, recovered, PhysicalSlotId(1U), true, LogSequenceNumber(2U));
	EXPECT_EQ(published.generation, Generation(2U));
	EXPECT_TRUE(reopenedDirectory.isAllocated(published, PhysicalSlotId(1U)));
}

TEST_F(RelocatableAllocatorDirectoryTest,
		NativeReleaseCandidateShortWriteRetainsAllocatedGenerationAndAllowsRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		SuperblockFile bootstrap(nativeIo);
		const Superblock initial = superblock(1U, 1U);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(nativeIo, initial);
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		AllocatorDirectory directory(pages, allocator);
		const Superblock allocated = directory.setAllocatedAndPublish(
				bootstrap, initial, PhysicalSlotId(1U), true, LogSequenceNumber(2U));
		ASSERT_TRUE(directory.isAllocated(allocated, PhysicalSlotId(1U)));

		bool shortWriteObserved = false;
		{
			StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
					[&shortWriteObserved](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (!shortWriteObserved && byteCount == RSS_PAGE_SIZE) {
							shortWriteObserved = true;
							return portable_pwrite(fd, bytes, RSS_PAGE_SIZE - 1U, offset);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(
							bootstrap, allocated, PhysicalSlotId(1U), false, LogSequenceNumber(3U))), std::system_error);
		}
		EXPECT_TRUE(shortWriteObserved);
		EXPECT_EQ(bootstrap.recover().generation, allocated.generation);
		EXPECT_TRUE(directory.isAllocated(allocated, PhysicalSlotId(1U)));
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	EXPECT_EQ(recovered.generation, Generation(2U));
	MetadataPageFile reopenedPages(io, recovered);
	MetadataPagePoolAllocator reopenedAllocator(io, recovered);
	AllocatorDirectory reopenedDirectory(reopenedPages, reopenedAllocator);
	EXPECT_TRUE(reopenedDirectory.isAllocated(recovered, PhysicalSlotId(1U)));
	const Superblock released = reopenedDirectory.setAllocatedAndPublish(
			reopenedBootstrap, recovered, PhysicalSlotId(1U), false, LogSequenceNumber(3U));
	EXPECT_EQ(released.generation, Generation(3U));
	EXPECT_FALSE(reopenedDirectory.isAllocated(released, PhysicalSlotId(1U)));
}

TEST_F(RelocatableAllocatorDirectoryTest, NativeCandidateDiskFullRetainsOldGenerationAndAllowsRetry) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const Superblock initial = superblock(1U, 1U);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(nativeIo, initial);
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		AllocatorDirectory directory(pages, allocator);
		bool diskFullObserved = false;
		{
			StorageIO::ScopedNativeWriteHookForTesting diskFullCandidate(
					[&diskFullObserved](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (!diskFullObserved && byteCount == RSS_PAGE_SIZE) {
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
			EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(
						bootstrap, initial, PhysicalSlotId(1U), true, LogSequenceNumber(2U))), std::system_error);
		}
		EXPECT_TRUE(diskFullObserved);
		EXPECT_EQ(bootstrap.recover().generation, initial.generation);
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	MetadataPageFile reopenedPages(io, recovered);
	MetadataPagePoolAllocator reopenedAllocator(io, recovered);
	AllocatorDirectory reopenedDirectory(reopenedPages, reopenedAllocator);
	const Superblock published = reopenedDirectory.setAllocatedAndPublish(
			reopenedBootstrap, recovered, PhysicalSlotId(1U), true, LogSequenceNumber(2U));
	EXPECT_EQ(published.generation, Generation(2U));
	EXPECT_TRUE(reopenedDirectory.isAllocated(published, PhysicalSlotId(1U)));
}

TEST_F(RelocatableAllocatorDirectoryTest, NativeDataFenceFailureRetainsOldGenerationAndAllowsRetry) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const Superblock initial = superblock(1U, 1U);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(nativeIo, initial);
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		AllocatorDirectory directory(pages, allocator);
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
			EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(
						bootstrap, initial, PhysicalSlotId(1U), true, LogSequenceNumber(2U))), std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 1U);
		EXPECT_EQ(bootstrap.recover().generation, initial.generation);
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	MetadataPageFile reopenedPages(io, recovered);
	MetadataPagePoolAllocator reopenedAllocator(io, recovered);
	AllocatorDirectory reopenedDirectory(reopenedPages, reopenedAllocator);
	const Superblock published = reopenedDirectory.setAllocatedAndPublish(
			reopenedBootstrap, recovered, PhysicalSlotId(1U), true, LogSequenceNumber(2U));
	EXPECT_EQ(published.generation, Generation(2U));
	EXPECT_TRUE(reopenedDirectory.isAllocated(published, PhysicalSlotId(1U)));
}

TEST_F(RelocatableAllocatorDirectoryTest, NativeFinalFenceFailureReopensTheCompletePublishedGeneration) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		const Superblock initial = superblock(1U, 1U);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(initial);
		MetadataPageFile pages(nativeIo, initial);
		MetadataPagePoolAllocator allocator(nativeIo, initial);
		AllocatorDirectory directory(pages, allocator);
		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failFinalFence([&syncCalls](file_handle_t fd) {
				if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 1U) {
#ifdef _WIN32
					SetLastError(ERROR_DISK_FULL);
#else
					errno = ENOSPC;
#endif
					return -1;
				}
				return portable_fsync(fd);
			});
			EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(
						bootstrap, initial, PhysicalSlotId(1U), true, LogSequenceNumber(2U))), std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
		const Superblock published = bootstrap.recover();
		EXPECT_EQ(published.generation, Generation(2U));
		EXPECT_TRUE(directory.isAllocated(published, PhysicalSlotId(1U)));
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	EXPECT_EQ(recovered.generation, Generation(2U));
	MetadataPageFile reopenedPages(io, recovered);
	MetadataPagePoolAllocator reopenedAllocator(io, recovered);
	AllocatorDirectory reopenedDirectory(reopenedPages, reopenedAllocator);
	EXPECT_TRUE(reopenedDirectory.isAllocated(recovered, PhysicalSlotId(1U)));
}

TEST_F(RelocatableAllocatorDirectoryTest, SkipsAFullAllocatorLeafUsingThePersistedRadixSummary) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);

	const Superblock first =
			directory.setAllocatedAndPublish(bootstrap, initial, PhysicalSlotId(1), true, LogSequenceNumber(2));
	const Superblock current = directory.setAllocatedAndPublish(
			bootstrap, first, PhysicalSlotId(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY + 1U), true, LogSequenceNumber(3));
	MetadataRadixPage root = pages.readMetadataRadix(current.allocator_root);
	ASSERT_EQ(root.level, 0U);
	ASSERT_TRUE(root.children[0].isValid());
	ASSERT_TRUE(root.children[1].isValid());

	// A false summary must avoid reading the child entirely. Corrupting that
	// skipped child proves selection is driven by the durable hierarchy, not leaf scans.
	root.allocator_free_children.fill(std::byte{});
	root.allocator_free_children[0] = std::byte{0x02};
	pages.writeMetadataRadix(current.allocator_root, root);
	std::array<std::byte, RSS_PAGE_SIZE> corruptLeaf{};
	io->writeAt(pages.pageOffset(root.children[0]), corruptLeaf.data(), corruptLeaf.size());

	const std::optional<PhysicalSlotId> selected =
			directory.findFirstFree(current, PhysicalSlotId(1), PhysicalSlotId(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY * 2U));
	ASSERT_TRUE(selected.has_value());
	EXPECT_EQ(*selected, PhysicalSlotId(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY + 2U));
}

TEST_F(RelocatableAllocatorDirectoryTest, BuildsAndValidatesTheNewFileBootstrapOwnershipRoot) {
	const Superblock initial = superblock(1, 1);
	MetadataPageFile pages(io, initial);

	TestAllocatorPageAllocator allocator;
	allocator.created_pools = {PhysicalSlotId(1), std::nullopt};
	AllocatorDirectory directory(pages, allocator);
	const Superblock initialized = directory.initializeNew(initial, PhysicalSlotId(1));
	EXPECT_TRUE(initialized.allocator_root.isValid());
	EXPECT_EQ(allocator.returned_references.size(), 2U);
	EXPECT_TRUE(allocator.abandoned_references.empty());

	Superblock invalid = initial;
	invalid.catalog_root = PhysicalPageRef{PhysicalSlotId(1), 1, Generation(1)};
	expectFormatError([&] { static_cast<void>(directory.initializeNew(invalid, PhysicalSlotId(1))); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(directory.initializeNew(initial, PhysicalSlotId{})); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableAllocatorDirectoryTest, CleansUpFailedNewFileBootstrapAllocations) {
	const Superblock initial = superblock(1, 1);
	MetadataPageFile pages(io, initial);

	TestAllocatorPageAllocator noPoolRecord;
	AllocatorDirectory missingPoolDirectory(pages, noPoolRecord);
	expectFormatError([&] { static_cast<void>(missingPoolDirectory.initializeNew(initial, PhysicalSlotId(1))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(noPoolRecord.abandoned_references.size(), 2U);

	TestAllocatorPageAllocator duplicatePoolRecord;
	duplicatePoolRecord.created_pools = {PhysicalSlotId(1), PhysicalSlotId(1)};
	AllocatorDirectory duplicatePoolDirectory(pages, duplicatePoolRecord);
	expectFormatError([&] { static_cast<void>(duplicatePoolDirectory.initializeNew(initial, PhysicalSlotId(1))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(duplicatePoolRecord.abandoned_references.size(), 2U);
	EXPECT_EQ(duplicatePoolRecord.abandoned_pools, std::vector<PhysicalSlotId>({PhysicalSlotId(1), PhysicalSlotId(1)}));

	TestAllocatorPageAllocator wrongBootstrapPool;
	wrongBootstrapPool.created_pools = {PhysicalSlotId(1), std::nullopt};
	AllocatorDirectory wrongBootstrapPoolDirectory(pages, wrongBootstrapPool);
	expectFormatError([&] { static_cast<void>(wrongBootstrapPoolDirectory.initializeNew(initial, PhysicalSlotId(2))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(wrongBootstrapPool.abandoned_references.size(), 2U);
	EXPECT_EQ(wrongBootstrapPool.abandoned_pools, std::vector<PhysicalSlotId>({PhysicalSlotId(1)}));

	TestAllocatorPageAllocator invalidReference;
	invalidReference.invalid_physical_reference = true;
	AllocatorDirectory invalidReferenceDirectory(pages, invalidReference);
	expectFormatError([&] { static_cast<void>(invalidReferenceDirectory.initializeNew(initial, PhysicalSlotId(1))); },
					  FormatError::FERR_INVALID_STATE);

	TestAllocatorPageAllocator mismatchedPool;
	mismatchedPool.created_pools = {PhysicalSlotId(2)};
	AllocatorDirectory mismatchedPoolDirectory(pages, mismatchedPool);
	expectFormatError([&] { static_cast<void>(mismatchedPoolDirectory.initializeNew(initial, PhysicalSlotId(1))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableAllocatorDirectoryTest, ReservesLowestFreeSlotReusesReleasedSlotsAndReportsExhaustion) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);
	const PhysicalSlotId firstSlot(100);
	const PhysicalSlotId lastSlot(102);

	EXPECT_EQ(directory.findFirstFree(initial, firstSlot, lastSlot), firstSlot);
	const auto first =
			directory.reserveFirstFreeAndPublish(bootstrap, initial, firstSlot, lastSlot, LogSequenceNumber(2));
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(first->physical_slot, firstSlot);
	EXPECT_TRUE(directory.isAllocated(first->superblock, firstSlot));

	const auto second = directory.reserveFirstFreeAndPublish(bootstrap, first->superblock, firstSlot, lastSlot,
															 LogSequenceNumber(3));
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(second->physical_slot, PhysicalSlotId(101));
	const auto third = directory.reserveFirstFreeAndPublish(bootstrap, second->superblock, firstSlot, lastSlot,
															LogSequenceNumber(4));
	ASSERT_TRUE(third.has_value());
	EXPECT_EQ(third->physical_slot, lastSlot);

	EXPECT_FALSE(directory.findFirstFree(third->superblock, firstSlot, lastSlot).has_value());
	EXPECT_FALSE(
			directory
					.reserveFirstFreeAndPublish(bootstrap, third->superblock, firstSlot, lastSlot, LogSequenceNumber(5))
					.has_value());
	EXPECT_EQ(bootstrap.recover().generation, third->superblock.generation);

	const Superblock released = directory.setAllocatedAndPublish(bootstrap, third->superblock, PhysicalSlotId(101),
																 false, LogSequenceNumber(5));
	const auto reused =
			directory.reserveFirstFreeAndPublish(bootstrap, released, firstSlot, lastSlot, LogSequenceNumber(6));
	ASSERT_TRUE(reused.has_value());
	EXPECT_EQ(reused->physical_slot, PhysicalSlotId(101));
	EXPECT_THROW(static_cast<void>(directory.findFirstFree(reused->superblock, PhysicalSlotId{}, lastSlot)),
				 FormatException);
	EXPECT_THROW(static_cast<void>(directory.findFirstFree(reused->superblock, firstSlot, PhysicalSlotId{})),
				 FormatException);
	EXPECT_THROW(static_cast<void>(directory.findFirstFree(reused->superblock, lastSlot, firstSlot)), FormatException);
}

TEST_F(RelocatableAllocatorDirectoryTest, ScansAcrossBitmapBytesAndSkipsUnavailablePartialLeafTails) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);
	Superblock current = initial;
	for (uint64_t slot = 200; slot < 208; ++slot) {
		current = directory.setAllocatedAndPublish(bootstrap, current, PhysicalSlotId(slot), true,
												   LogSequenceNumber(slot - 198U));
	}
	EXPECT_EQ(directory.findFirstFree(current, PhysicalSlotId(200), PhysicalSlotId(208)), PhysicalSlotId(208));

	const AllocatorLeafPage originalLeaf =
			pages.readAllocatorLeaf(pages.readMetadataRadix(current.allocator_root).children[0]);
	AllocatorLeafPage shortLeaf = originalLeaf;
	shortLeaf.slot_count = 1;
	shortLeaf.allocated_slots.fill(std::byte{});
	shortLeaf.allocated_slots[0] = std::byte{1};
	pages.writeAllocatorLeaf(pages.readMetadataRadix(current.allocator_root).children[0], shortLeaf);
	EXPECT_EQ(directory.findFirstFree(current, PhysicalSlotId(201),
									  PhysicalSlotId(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY + 1U)),
			  PhysicalSlotId(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY + 1U));
}

TEST_F(RelocatableAllocatorDirectoryTest, HandlesPartiallySizedTerminalLeafAndClearsItsSummary) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);

	// The final physical slot falls in a leaf whose usable bitmap is shorter
	// than the normal 256-slot capacity. Reserve every slot in one batch so the
	// persisted summary must transition from free to full without a workload loop
	// of individual publications.
	const uint64_t lastValue = std::numeric_limits<uint64_t>::max();
	const uint64_t firstValue = lastValue - static_cast<uint64_t>((lastValue - 1U) % RSS_ALLOCATOR_PAGE_SLOT_CAPACITY);
	const PhysicalSlotId first(firstValue);
	const PhysicalSlotId last(lastValue);
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	for (uint64_t value = first.value(); value <= last.value(); ++value) {
		directory.prepareSetAllocated(batch, PhysicalSlotId(value), true);
		if (value == last.value()) {
			break;
		}
	}
	batch.publish(bootstrap);
	const Superblock full = bootstrap.recover();
	EXPECT_FALSE(directory.findFirstFree(full, first, last).has_value());

	const Superblock released = directory.setAllocatedAndPublish(
			bootstrap, full, last, false, LogSequenceNumber(3));
	EXPECT_EQ(directory.findFirstFree(released, first, last), last);
}

TEST_F(RelocatableAllocatorDirectoryTest, FindsSparseFreeRadixRoutesWithoutScanningEveryPhysicalSlot) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);
	const PhysicalSlotId highSlot(static_cast<uint64_t>(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY) * RSS_CATALOG_RADIX_FANOUT +
								  1U);
	const PhysicalSlotId sparseSlot(static_cast<uint64_t>(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY) * 5U + 1U);

	const Superblock low =
			directory.setAllocatedAndPublish(bootstrap, initial, PhysicalSlotId(1), true, LogSequenceNumber(2));
	const Superblock high = directory.setAllocatedAndPublish(bootstrap, low, highSlot, true, LogSequenceNumber(3));
	EXPECT_EQ(pages.readMetadataRadix(high.allocator_root).level, 1U);
	EXPECT_EQ(directory.findFirstFree(high, sparseSlot, PhysicalSlotId(sparseSlot.value() + 8U)), sparseSlot);

	const auto reserved = directory.reserveFirstFreeAndPublish(
			bootstrap, high, sparseSlot, PhysicalSlotId(sparseSlot.value() + 8U), LogSequenceNumber(4));
	ASSERT_TRUE(reserved.has_value());
	EXPECT_EQ(reserved->physical_slot, sparseSlot);
	EXPECT_EQ(directory.findFirstFree(reserved->superblock, sparseSlot, PhysicalSlotId(sparseSlot.value() + 8U)),
			  PhysicalSlotId(sparseSlot.value() + 1U));
}

TEST_F(RelocatableAllocatorDirectoryTest, GrowsTheRadixAndReopensThePublishedAllocatorRoot) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);
	const PhysicalSlotId highSlot(static_cast<uint64_t>(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY) * RSS_CATALOG_RADIX_FANOUT +
								  1U);
	SCOPED_TRACE("publish low slot");
	const Superblock lowPublished =
			directory.setAllocatedAndPublish(bootstrap, initial, PhysicalSlotId(1), true, LogSequenceNumber(2));
	SCOPED_TRACE("publish high slot");
	const Superblock published =
			directory.setAllocatedAndPublish(bootstrap, lowPublished, highSlot, true, LogSequenceNumber(3));
	SCOPED_TRACE("publish adjacent high slot");
	const Superblock updated = directory.setAllocatedAndPublish(
			bootstrap, published, PhysicalSlotId(highSlot.value() + 1U), true, LogSequenceNumber(4));
	EXPECT_TRUE(directory.isAllocated(updated, highSlot));
	EXPECT_TRUE(directory.isAllocated(updated, PhysicalSlotId(highSlot.value() + 1U)));
	EXPECT_FALSE(directory.isAllocated(
			updated, PhysicalSlotId(static_cast<uint64_t>(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY) * 2U + 1U)));
	EXPECT_EQ(pages.readMetadataRadix(updated.allocator_root).level, 1U);
	io->sync();

	reopen();
	SuperblockFile reopenedBootstrap(io);
	const Superblock recovered = reopenedBootstrap.recover();
	MetadataPageFile reopenedPages(io, recovered);
	MetadataPagePoolAllocator reopenedAllocator(io, recovered);
	AllocatorDirectory reopenedDirectory(reopenedPages, reopenedAllocator);
	EXPECT_EQ(recovered.allocator_root, updated.allocator_root);
	EXPECT_TRUE(reopenedDirectory.isAllocated(recovered, highSlot));
	EXPECT_TRUE(reopenedDirectory.isAllocated(recovered, PhysicalSlotId(1)));
}

TEST_F(RelocatableAllocatorDirectoryTest, RejectsNoopStaleAndMismatchedStoragePublications) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);
	const Superblock allocated =
			directory.setAllocatedAndPublish(bootstrap, initial, PhysicalSlotId(1), true, LogSequenceNumber(2));
	EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(bootstrap, allocated, PhysicalSlotId(1), true,
																	LogSequenceNumber(3))),
				 FormatException);
	const uint64_t allocatedPagesBeforeStalePublication = allocator.stats().allocated_pages;
	EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(bootstrap, initial, PhysicalSlotId(2), true,
																	LogSequenceNumber(3))),
				 FormatException);
	EXPECT_EQ(allocator.stats().allocated_pages, allocatedPagesBeforeStalePublication);
	EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(bootstrap, allocated, PhysicalSlotId(2), true,
																	LogSequenceNumber(2))),
				 FormatException);
	EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(bootstrap, allocated, PhysicalSlotId(2), true,
																	LogSequenceNumber{})),
				 FormatException);
	auto alternateIo = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	SuperblockFile alternateBootstrap(alternateIo);
	EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(alternateBootstrap, allocated, PhysicalSlotId(2),
																	true, LogSequenceNumber(3))),
				 FormatException);
	Superblock exhaustedGeneration = allocated;
	exhaustedGeneration.generation = Generation(std::numeric_limits<uint64_t>::max() >> 5U);
	EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(bootstrap, exhaustedGeneration, PhysicalSlotId(2),
																	true, LogSequenceNumber(3))),
				 FormatException);
	EXPECT_THROW(static_cast<void>(directory.isAllocated(allocated, PhysicalSlotId{})), FormatException);
}

TEST_F(RelocatableAllocatorDirectoryTest, ReleasesOnlyUnpublishedPagesAcrossBootstrapCrashBoundaries) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);

	EXPECT_THROW(static_cast<void>(directory.reserveFirstFreeAndPublish(
						 bootstrap, initial, PhysicalSlotId(100), PhysicalSlotId(100), LogSequenceNumber(2),
						 BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE)),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(allocator.stats().allocated_pages, 0U);
	EXPECT_EQ(bootstrap.recover().allocator_root, PhysicalPageRef{});

	EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(
						 bootstrap, initial, PhysicalSlotId(1), true, LogSequenceNumber(2),
						 BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE)),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(allocator.stats().allocated_pages, 0U);
	EXPECT_EQ(bootstrap.recover().allocator_root, PhysicalPageRef{});

	EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(
						 bootstrap, initial, PhysicalSlotId(1), true, LogSequenceNumber(2),
						 BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE)),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(allocator.stats().allocated_pages, 2U);
	const Superblock recovered = bootstrap.recover();
	EXPECT_EQ(recovered.generation, Generation(2));
	EXPECT_TRUE(directory.isAllocated(recovered, PhysicalSlotId(1)));
}

TEST_F(RelocatableAllocatorDirectoryTest, PreparedTransitionsRejectForeignAllocatorsAndRepeatMutations) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);

	TestAllocatorPageAllocator foreignAllocator;
	MetadataWriteBatch foreignBatch(foreignAllocator, initial, LogSequenceNumber(2));
	EXPECT_THROW(directory.prepareSetAllocated(foreignBatch, PhysicalSlotId(1), true), FormatException);

	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	directory.prepareSetAllocated(batch, PhysicalSlotId(1), true);
	EXPECT_THROW(directory.prepareSetAllocated(batch, PhysicalSlotId(1), true), FormatException);
	EXPECT_FALSE(directory.prepareReserveFirstFree(batch, PhysicalSlotId(1), PhysicalSlotId(1)).has_value());
}

TEST_F(RelocatableAllocatorDirectoryTest, ReservesFromAnUninitializedCandidateAllocatorRoot) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);
	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	const std::optional<PhysicalSlotId> free =
			directory.prepareReserveFirstFree(batch, PhysicalSlotId(1), PhysicalSlotId(1));
	ASSERT_TRUE(free.has_value());
	EXPECT_EQ(*free, PhysicalSlotId(1));
}

TEST_F(RelocatableAllocatorDirectoryTest, PreservesEveryDistinctPreparedSlotTransitionInOnePublication) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);

	MetadataWriteBatch batch(allocator, initial, LogSequenceNumber(2));
	directory.prepareSetAllocated(batch, PhysicalSlotId(6), true);
	directory.prepareSetAllocated(batch, PhysicalSlotId(8), true);
	directory.prepareSetAllocated(batch, PhysicalSlotId(9), true);
	batch.publish(bootstrap);

	const Superblock published = bootstrap.recover();
	EXPECT_TRUE(directory.isAllocated(published, PhysicalSlotId(6)));
	EXPECT_TRUE(directory.isAllocated(published, PhysicalSlotId(8)));
	EXPECT_TRUE(directory.isAllocated(published, PhysicalSlotId(9)));
}

TEST_F(RelocatableAllocatorDirectoryTest, RejectsBadAllocatorResultsAndCleansUpWrittenUnpublishedPages) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);

	TestAllocatorPageAllocator invalidPageId;
	invalidPageId.invalid_page_id = true;
	AllocatorDirectory invalidPageIdDirectory(pages, invalidPageId);
	EXPECT_THROW(static_cast<void>(invalidPageIdDirectory.setAllocatedAndPublish(bootstrap, initial, PhysicalSlotId(1),
																				 true, LogSequenceNumber(2))),
				 FormatException);
	EXPECT_TRUE(invalidPageId.abandoned_references.empty());

	TestAllocatorPageAllocator invalidPhysicalReference;
	invalidPhysicalReference.invalid_physical_reference = true;
	AllocatorDirectory invalidPhysicalReferenceDirectory(pages, invalidPhysicalReference);
	EXPECT_THROW(static_cast<void>(invalidPhysicalReferenceDirectory.setAllocatedAndPublish(
						 bootstrap, initial, PhysicalSlotId(1), true, LogSequenceNumber(2))),
				 FormatException);
	EXPECT_TRUE(invalidPhysicalReference.abandoned_references.empty());

	TestAllocatorPageAllocator wrongGeneration;
	wrongGeneration.wrong_physical_generation = true;
	AllocatorDirectory wrongGenerationDirectory(pages, wrongGeneration);
	EXPECT_THROW(static_cast<void>(wrongGenerationDirectory.setAllocatedAndPublish(
						 bootstrap, initial, PhysicalSlotId(1), true, LogSequenceNumber(2))),
				 FormatException);
	EXPECT_TRUE(wrongGeneration.abandoned_references.empty());

	TestAllocatorPageAllocator overflowReference;
	overflowReference.overflow_physical_reference = true;
	AllocatorDirectory overflowDirectory(pages, overflowReference);
	EXPECT_THROW(static_cast<void>(overflowDirectory.setAllocatedAndPublish(bootstrap, initial, PhysicalSlotId(1), true,
																			LogSequenceNumber(2))),
				 FormatException);
	ASSERT_EQ(overflowReference.returned_references.size(), 1U);
	ASSERT_EQ(overflowReference.abandoned_references.size(), 1U);
	EXPECT_EQ(overflowReference.abandoned_references.front(), overflowReference.returned_references.front());

	TestAllocatorPageAllocator duplicateReference;
	duplicateReference.duplicate_physical_reference = true;
	AllocatorDirectory duplicateDirectory(pages, duplicateReference);
	EXPECT_THROW(static_cast<void>(duplicateDirectory.setAllocatedAndPublish(bootstrap, initial, PhysicalSlotId(1),
																			 true, LogSequenceNumber(2))),
				 FormatException);
	ASSERT_EQ(duplicateReference.returned_references.size(), 1U);
	ASSERT_EQ(duplicateReference.abandoned_references.size(), 1U);
	EXPECT_EQ(duplicateReference.abandoned_references.front(), duplicateReference.returned_references.front());
	EXPECT_EQ(bootstrap.recover().allocator_root, PhysicalPageRef{});
}

TEST_F(RelocatableAllocatorDirectoryTest, RejectsCorruptDirectoryAndLeafRoutesBeforeUsingTheirBits) {
	const Superblock initial = superblock(1, 1);
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(initial);
	MetadataPageFile pages(io, initial);
	MetadataPagePoolAllocator allocator(io, initial);
	AllocatorDirectory directory(pages, allocator);
	const Superblock allocated =
			directory.setAllocatedAndPublish(bootstrap, initial, PhysicalSlotId(1), true, LogSequenceNumber(2));
	const MetadataRadixPage originalRoot = pages.readMetadataRadix(allocated.allocator_root);

	MetadataRadixPage wrongType = originalRoot;
	wrongType.directory_type = MetadataDirectoryType::MDT_CATALOG;
	wrongType.allocator_free_children.fill(std::byte{});
	pages.writeMetadataRadix(allocated.allocator_root, wrongType);
	EXPECT_THROW(static_cast<void>(directory.isAllocated(allocated, PhysicalSlotId(1))), FormatException);
	pages.writeMetadataRadix(allocated.allocator_root, originalRoot);

	MetadataRadixPage wrongRootKind = originalRoot;
	wrongRootKind.kind = CatalogRadixPageKind::CRPK_INTERNAL;
	pages.writeMetadataRadix(allocated.allocator_root, wrongRootKind);
	EXPECT_THROW(static_cast<void>(directory.isAllocated(allocated, PhysicalSlotId(1))), FormatException);
	pages.writeMetadataRadix(allocated.allocator_root, originalRoot);

	const PhysicalPageRef leafReference = originalRoot.children[0];
	const AllocatorLeafPage originalLeaf = pages.readAllocatorLeaf(leafReference);
	AllocatorLeafPage malformedLeaf = originalLeaf;
	malformedLeaf.first_slot = PhysicalSlotId(2);
	pages.writeAllocatorLeaf(leafReference, malformedLeaf);
	EXPECT_THROW(static_cast<void>(directory.isAllocated(allocated, PhysicalSlotId(1))), FormatException);
	pages.writeAllocatorLeaf(leafReference, originalLeaf);

	AllocatorLeafPage shortLeaf = originalLeaf;
	shortLeaf.slot_count = 1;
	pages.writeAllocatorLeaf(leafReference, shortLeaf);
	EXPECT_FALSE(directory.isAllocated(allocated, PhysicalSlotId(2)));
	EXPECT_THROW(static_cast<void>(directory.setAllocatedAndPublish(bootstrap, allocated, PhysicalSlotId(2), true,
																	LogSequenceNumber(3))),
				 FormatException);
}

TEST_F(RelocatableAllocatorDirectoryTest, BoundedAllocatedSlotWalkPreservesOldLogicalRootsAndNeverClaimsPartialCompletion) {
    auto metadata = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
    auto old = metadata->openSnapshot(); const PhysicalSlotId far(uint64_t(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY) * RSS_CATALOG_RADIX_FANOUT + 17U);
    (void)metadata->setPhysicalSlotAllocated(far, true, LogSequenceNumber(old.superblock().checkpoint_lsn.value() + 1U));
    auto current = metadata->openSnapshot(); MetadataPageFile pages(io, current.superblock());
    MetadataPagePoolAllocator pools(io, current.superblock(), MetadataPagePoolOpenMode::CPOM_DISCOVER_ONLY);
    AllocatorDirectory directory(pages, pools); std::vector<PhysicalSlotId> prior, latest; const auto epoch = io->dataMutationEpoch();
    ASSERT_TRUE(directory.visitAllocatedSlotsUntil(old.superblock(), 128, [&](PhysicalSlotId slot) { prior.push_back(slot); return true; }));
    ASSERT_TRUE(directory.visitAllocatedSlotsUntil(current.superblock(), 128, [&](PhysicalSlotId slot) { latest.push_back(slot); return true; }));
    EXPECT_EQ(std::count(prior.begin(), prior.end(), far), 0); EXPECT_EQ(std::count(latest.begin(), latest.end(), far), 1);
    EXPECT_TRUE(std::is_sorted(latest.begin(), latest.end())); EXPECT_EQ(std::adjacent_find(latest.begin(), latest.end()), latest.end());
    size_t pagesVisited = 0, delivered = 0;
    EXPECT_FALSE(directory.visitAllocatedSlotsUntil(current.superblock(), 1, [&](PhysicalSlotId) { ++delivered; return true; }, &pagesVisited));
    EXPECT_EQ(pagesVisited, 1U); EXPECT_EQ(delivered, 0U);
    EXPECT_FALSE(directory.visitAllocatedSlotsUntil(current.superblock(), 128, [&](PhysicalSlotId) { ++delivered; return false; }));
    EXPECT_EQ(delivered, 1U); EXPECT_EQ(io->dataMutationEpoch(), epoch);
}
