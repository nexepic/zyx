/**
 * @file test_BTreePageStore_AtomicTree.cpp
 * @brief Atomic materialization and recovery tests for relocatable B+Tree index pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <atomic>
#include <cerrno>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "tests/src/storage/relocatable/index/RelocatableBTreePageStoreTestUtils.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;
using namespace relocatable_btree_test;

TEST_F(RelocatableBTreePageStoreTest, AtomicallyMaterializesNewTreePagesAndSwitchesTheIndexRoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation oldRoot{17, PageId(101), Generation(1)};
	static_cast<void>(store.publishRoots(std::span(&oldRoot, 1), {}, LogSequenceNumber(3)));
	MetadataSnapshotPin before = metadata->openSnapshot();
	static_cast<void>(
			store.materializeAndPublishTree(request.segment_id, 17, 2, 0, LogSequenceNumber(4),
											[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
												return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4));
											}));
	EXPECT_EQ(metadata->findIndexRoot(before, 17), oldRoot);
	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation newRoot = metadata->findIndexRoot(current, 17);
	EXPECT_EQ(newRoot.root_page_id, PageId(102));
	EXPECT_EQ(newRoot.root_generation, Generation(1));
	BTreeIndex index(17, store, *metadata);
	EXPECT_EQ(index.find(current, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1));
	EXPECT_EQ(metadata->findPageLocation(current, PageId(102)).segment_id, request.segment_id);
	EXPECT_EQ(metadata->findPageLocation(current, PageId(103)).segment_id, request.segment_id);
	before.release();
	current.release();

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	BTreeIndex reopenedIndex(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(reopened, 17), newRoot);
	EXPECT_EQ(reopenedIndex.find(reopened, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1));
	reopened.release();
	EXPECT_EQ(reopenedMetadata->allocateLogicalPageId(LogSequenceNumber(5)), PageId(104));
}

TEST_F(RelocatableBTreePageStoreTest, NativeTreeMaterializationDiskFullRetainsOldRootAndCanRetry) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, superblock());
		SlottedPagePool pool(nativeIo, *metadata);
		BTreePageStore store(pool, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
																				 LogSequenceNumber(2U))
						.has_value());
		const IndexRootLocation oldRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&oldRoot, 1U), {}, LogSequenceNumber(3U)));

		MetadataSnapshotPin before = metadata->openSnapshot();
		const CatalogEntry indexPool = metadata->findCatalog(before, request.segment_id);
		const uint64_t poolOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(before.superblock().data_region_offset, indexPool.physical_slot);
		const uint64_t candidateOffset =
				poolOffset + static_cast<uint64_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U) * RSS_PAGE_SIZE;
		{
			StorageIO::ScopedNativeWriteHookForTesting failFirstCandidate(
					[candidateOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(store.materializeAndPublishTree(
						 request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
						 [](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
							 return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
						 })),
					 std::system_error);
		}
		before.release();
		metadata.reset();

		auto recoveredMetadata = RelocatableMetadataStore::open(nativeIo);
		SlottedPagePool recoveredPool(nativeIo, *recoveredMetadata);
		BTreePageStore recoveredStore(recoveredPool, *recoveredMetadata);
		MetadataSnapshotPin recovered = recoveredMetadata->openSnapshot();
		EXPECT_EQ(recoveredMetadata->findIndexRoot(recovered, 17U), oldRoot);
		EXPECT_FALSE(recoveredMetadata->findPageLocationOptional(recovered, PageId(102U)).has_value());
		recovered.release();

		static_cast<void>(recoveredStore.materializeAndPublishTree(
				request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
				[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
					return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
				}));
		MetadataSnapshotPin published = recoveredMetadata->openSnapshot();
		EXPECT_EQ(recoveredMetadata->findIndexRoot(published, 17U).root_page_id, PageId(102U));
		BTreeIndex index(17U, recoveredStore, *recoveredMetadata);
		EXPECT_EQ(index.find(published, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1U));
		published.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableBTreePageStoreTest, NativeTreeMaterializationShortWriteRetainsOldRootAndCanRetry) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, superblock());
		SlottedPagePool pool(nativeIo, *metadata);
		BTreePageStore store(pool, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
																							 LogSequenceNumber(2U))
						.has_value());
		const IndexRootLocation oldRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&oldRoot, 1U), {}, LogSequenceNumber(3U)));

		MetadataSnapshotPin before = metadata->openSnapshot();
		const CatalogEntry indexPool = metadata->findCatalog(before, request.segment_id);
		const uint64_t poolOffset =
				PhysicalSegmentAllocator::checkedSlotOffset(before.superblock().data_region_offset, indexPool.physical_slot);
		const uint64_t candidateOffset =
				poolOffset + static_cast<uint64_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U) * RSS_PAGE_SIZE;
		bool shortWriteObserved = false;
		{
			StorageIO::ScopedNativeWriteHookForTesting shortFirstCandidate(
					[candidateOffset, &shortWriteObserved](file_handle_t fd, const void *bytes, size_t byteCount,
																						pwrite_off_t offset) {
						if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
							shortWriteObserved = true;
							return portable_pwrite(fd, bytes, RSS_PAGE_OWNER_HEADER_BYTES - 1U, offset);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(store.materializeAndPublishTree(
						 request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
						 [](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
							 return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
						 })),
					 std::system_error);
		}
		EXPECT_TRUE(shortWriteObserved);
		before.release();
		metadata.reset();

		auto recoveredMetadata = RelocatableMetadataStore::open(nativeIo);
		SlottedPagePool recoveredPool(nativeIo, *recoveredMetadata);
		BTreePageStore recoveredStore(recoveredPool, *recoveredMetadata);
		MetadataSnapshotPin recovered = recoveredMetadata->openSnapshot();
		EXPECT_EQ(recoveredMetadata->findIndexRoot(recovered, 17U), oldRoot);
		EXPECT_FALSE(recoveredMetadata->findPageLocationOptional(recovered, PageId(102U)).has_value());
		recovered.release();

		static_cast<void>(recoveredStore.materializeAndPublishTree(
				request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
				[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
					return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
				}));
		MetadataSnapshotPin published = recoveredMetadata->openSnapshot();
		EXPECT_EQ(recoveredMetadata->findIndexRoot(published, 17U).root_page_id, PageId(102U));
		BTreeIndex index(17U, recoveredStore, *recoveredMetadata);
		EXPECT_EQ(index.find(published, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1U));
		published.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableBTreePageStoreTest, NativeTreeMaterializationSyncFailureRetainsOldRootAndCanRetry) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, superblock());
		SlottedPagePool pool(nativeIo, *metadata);
		BTreePageStore store(pool, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
																				 LogSequenceNumber(2U))
						.has_value());
		const IndexRootLocation oldRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&oldRoot, 1U), {}, LogSequenceNumber(3U)));

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
			EXPECT_THROW(static_cast<void>(store.materializeAndPublishTree(
						 request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
						 [](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
							 return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
						 })),
					 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 1U);
		metadata.reset();

		auto recoveredMetadata = RelocatableMetadataStore::open(nativeIo);
		SlottedPagePool recoveredPool(nativeIo, *recoveredMetadata);
		BTreePageStore recoveredStore(recoveredPool, *recoveredMetadata);
		MetadataSnapshotPin recovered = recoveredMetadata->openSnapshot();
		EXPECT_EQ(recoveredMetadata->findIndexRoot(recovered, 17U), oldRoot);
		EXPECT_FALSE(recoveredMetadata->findPageLocationOptional(recovered, PageId(102U)).has_value());
		recovered.release();

		static_cast<void>(recoveredStore.materializeAndPublishTree(
				request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
				[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
					return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
				}));
		MetadataSnapshotPin published = recoveredMetadata->openSnapshot();
		EXPECT_EQ(recoveredMetadata->findIndexRoot(published, 17U).root_page_id, PageId(102U));
		published.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableBTreePageStoreTest,
	   NativeTreeMaterializationFinalSyncFailureReportsUncertainDurabilityAndReopensCompleteTree) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, superblock());
		SlottedPagePool pool(nativeIo, *metadata);
		BTreePageStore store(pool, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
															 LogSequenceNumber(2U))
						.has_value());
		const IndexRootLocation oldRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&oldRoot, 1U), {}, LogSequenceNumber(3U)));

		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence([&syncCalls](file_handle_t fd) {
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
			EXPECT_THROW(static_cast<void>(store.materializeAndPublishTree(
						 request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
						 [](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
							 return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
						 })),
					 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);

		// The inactive Superblock is already complete when its final fence fails.
		// The call reports uncertain durability, but recovery must select the whole
		// newly published tree rather than exposing a mixed root/page set.
		MetadataSnapshotPin current = metadata->openSnapshot();
		const IndexRootLocation publishedRoot = metadata->findIndexRoot(current, 17U);
		EXPECT_EQ(publishedRoot.root_page_id, PageId(102U));
		BTreeIndex currentIndex(17U, store, *metadata);
		EXPECT_EQ(currentIndex.find(current, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1U));
		current.release();

		metadata.reset();
		auto reopenedMetadata = RelocatableMetadataStore::open(nativeIo);
		SlottedPagePool reopenedPool(nativeIo, *reopenedMetadata);
		BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
		BTreeIndex reopenedIndex(17U, reopenedStore, *reopenedMetadata);
		MetadataSnapshotPin recovered = reopenedMetadata->openSnapshot();
		EXPECT_EQ(reopenedMetadata->findIndexRoot(recovered, 17U), publishedRoot);
		EXPECT_EQ(reopenedIndex.find(recovered, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1U));
		recovered.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableBTreePageStoreTest,
	   NativeTreeMaterializationMetadataDiskFullRetainsOldRootAndCanRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, superblock());
		SlottedPagePool pool(nativeIo, *metadata);
		BTreePageStore store(pool, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
															 LogSequenceNumber(2U))
						.has_value());
		const IndexRootLocation oldRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&oldRoot, 1U), {}, LogSequenceNumber(3U)));
		MetadataSnapshotPin before = metadata->openSnapshot();
		const uint64_t metadataPoolOffset = before.superblock().data_region_offset;
		before.release();

		{
			StorageIO::ScopedNativeWriteHookForTesting failMetadataCandidate(
					[metadataPoolOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
						if (absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
							absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE && byteCount == RSS_PAGE_SIZE) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(store.materializeAndPublishTree(
						 request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
						 [](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
							 return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
						 })),
					 std::runtime_error);
		}

		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(metadata->findIndexRoot(current, 17U), oldRoot);
		EXPECT_FALSE(metadata->findPageLocationOptional(current, PageId(102U)).has_value());
		current.release();
		metadata.reset();

		auto reopenedMetadata = RelocatableMetadataStore::open(nativeIo);
		SlottedPagePool reopenedPool(nativeIo, *reopenedMetadata);
		BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
		MetadataSnapshotPin recovered = reopenedMetadata->openSnapshot();
		EXPECT_EQ(reopenedMetadata->findIndexRoot(recovered, 17U), oldRoot);
		EXPECT_FALSE(reopenedMetadata->findPageLocationOptional(recovered, PageId(102U)).has_value());
		recovered.release();

		static_cast<void>(reopenedStore.materializeAndPublishTree(
				request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
				[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
					return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
				}));
		MetadataSnapshotPin published = reopenedMetadata->openSnapshot();
		BTreeIndex index(17U, reopenedStore, *reopenedMetadata);
		EXPECT_EQ(reopenedMetadata->findIndexRoot(published, 17U).root_page_id, PageId(102U));
		EXPECT_EQ(index.find(published, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1U));
		published.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableBTreePageStoreTest,
	   NativeTreeMaterializationMetadataShortWriteRetainsOldRootAndCanRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, superblock());
		SlottedPagePool pool(nativeIo, *metadata);
		BTreePageStore store(pool, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
																							 LogSequenceNumber(2U))
						.has_value());
		const IndexRootLocation oldRoot{17U, request.page_id, request.generation};
		static_cast<void>(store.publishRoots(std::span(&oldRoot, 1U), {}, LogSequenceNumber(3U)));
		MetadataSnapshotPin before = metadata->openSnapshot();
		const uint64_t metadataPoolOffset = before.superblock().data_region_offset;
		before.release();

		bool shortWriteObserved = false;
		{
			StorageIO::ScopedNativeWriteHookForTesting shortMetadataCandidate(
					[metadataPoolOffset, &shortWriteObserved](file_handle_t fd, const void *bytes, size_t byteCount,
																							pwrite_off_t offset) {
						const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
						if (absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
							absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE && byteCount == RSS_PAGE_SIZE) {
							shortWriteObserved = true;
							return portable_pwrite(fd, bytes, RSS_PAGE_SIZE - 1U, offset);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(store.materializeAndPublishTree(
						 request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
						 [](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
							 return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
						 })),
					 std::system_error);
		}
		EXPECT_TRUE(shortWriteObserved);

		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(metadata->findIndexRoot(current, 17U), oldRoot);
		EXPECT_FALSE(metadata->findPageLocationOptional(current, PageId(102U)).has_value());
		current.release();
		metadata.reset();

		auto reopenedMetadata = RelocatableMetadataStore::open(nativeIo);
		SlottedPagePool reopenedPool(nativeIo, *reopenedMetadata);
		BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
		MetadataSnapshotPin recovered = reopenedMetadata->openSnapshot();
		EXPECT_EQ(reopenedMetadata->findIndexRoot(recovered, 17U), oldRoot);
		EXPECT_FALSE(reopenedMetadata->findPageLocationOptional(recovered, PageId(102U)).has_value());
		recovered.release();

		static_cast<void>(reopenedStore.materializeAndPublishTree(
				request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
				[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
					return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
				}));
		MetadataSnapshotPin published = reopenedMetadata->openSnapshot();
		BTreeIndex index(17U, reopenedStore, *reopenedMetadata);
		EXPECT_EQ(reopenedMetadata->findIndexRoot(published, 17U).root_page_id, PageId(102U));
		EXPECT_EQ(index.find(published, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1U));
		published.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableBTreePageStoreTest, DoesNotPublishAnIncompleteTreeFactoryBatch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation oldRoot{17, PageId(101), Generation(1)};
	static_cast<void>(store.publishRoots(std::span(&oldRoot, 1), {}, LogSequenceNumber(3)));
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTree(
						request.segment_id, 17, 2, 0, LogSequenceNumber(4),
						[](const CatalogEntry &, std::span<const PageId>) { return std::vector<BTreePage>{}; }));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin afterFactoryFailure = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(afterFactoryFailure, 17), oldRoot);
	afterFactoryFailure.release();
	EXPECT_EQ(metadata->allocateLogicalPageId(LogSequenceNumber(4)), PageId(102));
}

TEST_F(RelocatableBTreePageStoreTest, RejectsInvalidTreeRequestsAndFactoryPageIdentities) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTree(
						SegmentId{}, 17, 1, 0, LogSequenceNumber(2),
						[](const CatalogEntry &, std::span<const PageId>) { return std::vector<BTreePage>{}; }));
			},
			FormatError::FERR_INVALID_HEADER);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation oldRoot{17, PageId(101), Generation(1)};
	static_cast<void>(store.publishRoots(std::span(&oldRoot, 1), {}, LogSequenceNumber(3)));
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTree(
						request.segment_id, 17, 2, 0, LogSequenceNumber(4),
						[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
							return twoPageTree(poolEntry, pageIds, LogSequenceNumber(3));
						}));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(snapshot, 17), oldRoot);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsOversizedBatchesAndNonIndexPoolsBeforeMaterialization) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTree(
						request.segment_id, 17, RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U, 0, LogSequenceNumber(3),
						[](const CatalogEntry &, std::span<const PageId>) { return std::vector<BTreePage>{}; }));
			},
			FormatError::FERR_INVALID_HEADER);

	request.segment_id = SegmentId(72);
	request.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.page_id = PageId(102);
	request.page_owner_id = 99;
	request.page_owner_kind = PageOwnerKind::POK_PROPERTY;
	ASSERT_TRUE(
			pool.allocateEmptyPage(request, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3)).has_value());
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTree(
						request.segment_id, 17, 1, 0, LogSequenceNumber(4),
						[](const CatalogEntry &, std::span<const PageId>) { return std::vector<BTreePage>{}; }));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsAtomicTreeBatchWhenThePoolLacksEnoughFrames) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest first = allocationRequest();
	ASSERT_TRUE(store.allocate(first, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	for (uint64_t page = 102; page <= 131; ++page) {
		const LogSequenceNumber lsn(page - 99U);
		SlottedPagePoolAppendRequest request;
		request.segment_id = first.segment_id;
		request.generation = first.generation;
		request.page_id = PageId(page);
		request.page_owner_id = first.page_owner_id;
		request.page_owner_kind = PageOwnerKind::POK_INDEX;
		PageOwnerHeader owner = pageOwner(lsn);
		owner.page_id = request.page_id;
		BTreePageHeader header;
		header.index_id = owner.owner_id;
		header.page_id = owner.page_id;
		header.generation = owner.generation;
		static_cast<void>(store.append(request, BTreePage::create(owner, header), lsn));
	}
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTree(
						first.segment_id, 17, 2, 0, LogSequenceNumber(33),
						[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
							return twoPageTree(poolEntry, pageIds, LogSequenceNumber(33));
						}));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, RecoversAnInterruptedAtomicTreePublicationFromTheWal) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation oldRoot{17, PageId(101), Generation(1)};
	static_cast<void>(store.publishRoots(std::span(&oldRoot, 1), {}, LogSequenceNumber(3)));
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.materializeAndPublishTree(
						 request.segment_id, 17, 2, 0, LogSequenceNumber(4),
						 [](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
							 return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4));
						 },
						 options)),
				 BootstrapPublishInterrupted);

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	BTreeIndex reopenedIndex(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(reopened, 17).root_page_id, PageId(102));
	EXPECT_EQ(reopenedIndex.find(reopened, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1));
}

TEST_F(RelocatableBTreePageStoreTest, DiscardsTreeFramesWhenTheWalPublicationHeaderWasNotDurable) {
	{
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pool(io, *metadata);
		BTreePageStore store(pool, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation oldRoot{17, PageId(101), Generation(1)};
		static_cast<void>(store.publishRoots(std::span(&oldRoot, 1), {}, LogSequenceNumber(3)));
		MetadataPublishOptions options;
		options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(store.materializeAndPublishTree(
							 request.segment_id, 17, 2, 0, LogSequenceNumber(4),
							 [](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
								 return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4));
							 },
							 options)),
					 BootstrapPublishInterrupted);
	}

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	MetadataSnapshotPin old = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(old, 17).root_page_id, PageId(101));
	old.release();
	static_cast<void>(
			reopenedStore.materializeAndPublishTree(allocationRequest().segment_id, 17, 2, 0, LogSequenceNumber(4),
													[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
														return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4));
													}));
	BTreeIndex index(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin current = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(current, 17).root_page_id, PageId(102));
	EXPECT_EQ(index.find(current, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1));
}

TEST_F(RelocatableBTreePageStoreTest, BindsInitialLeafValuesAfterTheMetadataBatchAssignsPageIds) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<BTreePageEntry, 2U> entries = {BTreePageEntry{{std::byte{'a'}}, 0U},
													BTreePageEntry{{std::byte{'b'}}, 0U}};
	bool binderCalled = false;
	std::vector<PageId> assignedPages;
	const BTreeInitialTreeMutationPlan plan = store.planInitialTreeMutation(
			before, 17U, entries,
			[&](std::span<BTreePageEntry> boundEntries, const BTreeInitialTreeValueBindingContext &context) {
				if (boundEntries.size() != entries.size() || context.pool_batches.empty() || context.page_ids.empty() ||
					context.page_ids.size() != context.generations.size()) {
					throw std::runtime_error("test initial-tree binder did not receive a complete metadata assignment");
				}
				binderCalled = true;
				assignedPages.assign(context.page_ids.begin(), context.page_ids.end());
				boundEntries[0].value_or_child = context.page_ids.front().value();
				boundEntries[1].value_or_child = context.page_ids.back().value() + 1U;
			});

	NativeGraphIndexTreeMutation treeMutation;
	treeMutation.index_id = plan.index_id;
	for (const BTreeNewIndexTreePoolMutation &newPool: plan.new_pool_mutations) {
		treeMutation.new_pool_requests.push_back(
				{PhysicalSlotId(40U), PhysicalSlotId(40U), newPool.owner_id, newPool.appended_page_count});
	}
	treeMutation.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal, true};
	NativeGraphDerivedStateMutation derived;
	derived.index_tree_mutations.push_back(std::move(treeMutation));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	static_cast<void>(metadata->mutateNativeGraphPages(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools, derived,
			LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization> replacements,
			   std::span<EntityPageAppendBatchMaterialization> appends,
			   std::span<EntityPageNewPoolBatchMaterialization> newPools,
			   std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
			   std::span<NativeGraphPropertyPageBatchMaterialization> newPropertyPools) {
				if (!replacements.empty() || !appends.empty() || !newPools.empty() || !properties.empty() ||
					!newPropertyPools.empty()) {
					throw std::runtime_error("test initial-tree publication unexpectedly materialized graph pages");
				}
			},
			[&plan](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				if (batches.size() != 1U) {
					throw std::runtime_error("test initial-tree publication has an invalid Index batch count");
				}
				plan.preparer(batches.front(), LogSequenceNumber(2U));
			}));

	EXPECT_TRUE(binderCalled);
	ASSERT_FALSE(assignedPages.empty());
	EXPECT_FALSE(metadata->findIndexRootOptional(before, 17U).has_value());
	MetadataSnapshotPin after = metadata->openSnapshot();
	BTreeIndex index(17U, store, *metadata);
	EXPECT_EQ(index.find(after, std::array<std::byte, 1U>{std::byte{'a'}}),
			  std::optional<uint64_t>(assignedPages.front().value()));
	EXPECT_EQ(index.find(after, std::array<std::byte, 1U>{std::byte{'b'}}),
			  std::optional<uint64_t>(assignedPages.back().value() + 1U));
	after.release();
	before.release();
}

TEST_F(RelocatableBTreePageStoreTest, PublishesAndReplacesKeyOnlyFreshTreesThroughTheNativeTransaction) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};

	const auto publish = [&](const BTreeInitialTreeMutationPlan &plan,
							std::optional<IndexRootLocation> expectedRoot, PhysicalSlotId slot,
							LogSequenceNumber checkpointLsn) {
		ASSERT_EQ(plan.new_pool_mutations.size(), 1U);
		NativeGraphIndexTreeMutation treeMutation;
		treeMutation.index_id = plan.index_id;
		treeMutation.expected_root = expectedRoot;
		for (const BTreeNewIndexTreePoolMutation &newPool: plan.new_pool_mutations) {
			treeMutation.new_pool_requests.push_back(
					{slot, slot, newPool.owner_id, newPool.appended_page_count});
		}
		treeMutation.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal, true};
		NativeGraphDerivedStateMutation derived;
		derived.index_tree_mutations.push_back(std::move(treeMutation));
		static_cast<void>(metadata->mutateNativeGraphPages(
				noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools, derived,
				checkpointLsn,
				[](std::span<EntityPageReplacementMaterialization> replacements,
				   std::span<EntityPageAppendBatchMaterialization> appends,
				   std::span<EntityPageNewPoolBatchMaterialization> newPools,
				   std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
				   std::span<NativeGraphPropertyPageBatchMaterialization> newPropertyPools) {
					if (!replacements.empty() || !appends.empty() || !newPools.empty() || !properties.empty() ||
						!newPropertyPools.empty()) {
						throw std::runtime_error("test key-only tree publication unexpectedly materialized graph pages");
					}
				},
				[&plan, checkpointLsn](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
					if (batches.size() != 1U) {
						throw std::runtime_error("test key-only tree publication has an invalid Index batch count");
					}
					plan.preparer(batches.front(), checkpointLsn);
				}));
	};

	const std::array<BTreePageEntry, 0U> noEntries{};
	MetadataSnapshotPin beforeCreate = metadata->openSnapshot();
	const BTreeInitialTreeMutationPlan emptyPlan =
			store.planInitialTreeMutation(beforeCreate, 17U, noEntries, {}, BTreeLeafValueLayout::BLVL_EMPTY);
	publish(emptyPlan, std::nullopt, PhysicalSlotId(40U), LogSequenceNumber(2U));
	EXPECT_FALSE(metadata->findIndexRootOptional(beforeCreate, 17U).has_value());
	beforeCreate.release();

	MetadataSnapshotPin afterCreate = metadata->openSnapshot();
	const IndexRootLocation emptyRoot = metadata->findIndexRoot(afterCreate, 17U);
	BTreeIndex index(17U, store, *metadata, BTreeLeafValueLayout::BLVL_EMPTY);
	EXPECT_TRUE(index.scan(afterCreate, std::nullopt, 1U).empty());
	EXPECT_EQ(store.read(afterCreate, emptyRoot.root_page_id).header().leaf_value_layout,
			  BTreeLeafValueLayout::BLVL_EMPTY);
	const std::array<BTreePageEntry, 2U> entries = {BTreePageEntry{{std::byte{'a'}}, 0U},
														 BTreePageEntry{{std::byte{'b'}}, 0U}};
	const BTreeInitialTreeMutationPlan replacementPlan =
			store.planReplacementTreeMutation(afterCreate, 17U, entries, {}, BTreeLeafValueLayout::BLVL_EMPTY);
	publish(replacementPlan, emptyRoot, PhysicalSlotId(41U), LogSequenceNumber(3U));
	EXPECT_EQ(metadata->findIndexRoot(afterCreate, 17U), emptyRoot);
	afterCreate.release();

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_NE(metadata->findIndexRoot(current, 17U), emptyRoot);
	EXPECT_EQ(index.scan(current, std::nullopt, entries.size()),
			  std::vector<BTreePageEntry>(entries.begin(), entries.end()));
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsNonemptyValuesInAKeyOnlyTransactionOverlayBeforePlanningMutation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	PageOwnerHeader owner = pageOwner(LogSequenceNumber(2U));
	BTreePageHeader header;
	header.index_id = owner.owner_id;
	header.page_id = owner.page_id;
	header.generation = owner.generation;
	header.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
	const BTreePage emptyKeyOnlyPage = BTreePage::create(owner, header);
	ASSERT_TRUE(store.allocate(request, emptyKeyOnlyPage, PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation root{17U, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::array operations = {BTreeScalarIndexOperation{
				BTreeScalarIndexOperationKind::BSIOP_UPSERT, {std::byte{'a'}}, 1U}};
	expectFormatError(
			[&] {
				static_cast<void>(store.planScalarIndexTransactionMutation(
						snapshot, 17U, operations, false, false, BTreeLeafValueLayout::BLVL_EMPTY));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, 17U), root);
	EXPECT_TRUE(store.read(snapshot, root.root_page_id).entries().empty());
}

TEST_F(RelocatableBTreePageStoreTest, RejectsInitialValueBindingThatChangesAKeyBeforeWritingFrames) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::array<BTreePageEntry, 1U> entries = {BTreePageEntry{{std::byte{'a'}}, 0U}};
	const BTreeInitialTreeMutationPlan plan = store.planInitialTreeMutation(
			snapshot, 17U, entries,
			[](std::span<BTreePageEntry> boundEntries, const BTreeInitialTreeValueBindingContext &) {
				boundEntries.front().key = {std::byte{'b'}};
				boundEntries.front().value_or_child = 1U;
			});
	IndexTreeMutationPoolBatch batch;
	batch.pool = {SegmentId(71U),			PhysicalSlotId(40U),  SegmentKind::SGK_INDEX_PAGE_POOL, 17U, Generation(1U),
				  SegmentState::SGS_SEALED, LogSequenceNumber(2U)};
	batch.newly_reserved = true;
	batch.appended_page_ids = {PageId(101U)};
	batch.appended_locations = {{PageId(101U), batch.pool.segment_id, 0U, batch.pool.generation, 0U}};
	std::vector<std::vector<IndexTreeMutationPoolBatch>> allBatches;
	allBatches.push_back({batch});
	expectFormatError([&] { plan.preparer(allBatches.front(), LogSequenceNumber(2U)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, 17U).has_value());
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsInitialValueBindingsThatChangeKeysBeforeWritingFrames) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const auto batchFor = [](const BTreeInitialTreeMutationPlan &plan) {
		EXPECT_EQ(plan.new_pool_mutations.size(), 1U);
		IndexTreeMutationPoolBatch batch;
		batch.pool = {SegmentId(71U), PhysicalSlotId(40U), SegmentKind::SGK_INDEX_PAGE_POOL, 17U, Generation(1U),
					  SegmentState::SGS_SEALED, LogSequenceNumber(2U)};
		batch.newly_reserved = true;
		const size_t pageCount = plan.new_pool_mutations.front().appended_page_count;
		for (size_t ordinal = 0U; ordinal < pageCount; ++ordinal) {
			const PageId pageId(101U + ordinal);
			batch.appended_page_ids.push_back(pageId);
			batch.appended_locations.push_back({pageId, batch.pool.segment_id, static_cast<uint16_t>(ordinal),
												batch.pool.generation, 0U});
		}
		return batch;
	};
	const auto expectRejection = [](const BTreeInitialTreeMutationPlan &plan,
										   std::vector<IndexTreeMutationPoolBatch> &batches, std::string_view message) {
		try {
			plan.preparer(batches, LogSequenceNumber(2U));
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
			EXPECT_NE(std::string_view(error.what()).find(message), std::string_view::npos)
					<< error.what();
		}
	};

	std::vector<BTreePageEntry> fixedShapeEntries;
	for (uint16_t ordinal = 100U; ordinal < 106U; ++ordinal) {
		fixedShapeEntries.push_back({wideKey(ordinal), 0U});
	}
	const BTreeInitialTreeMutationPlan changedFirstKey = store.planInitialTreeMutation(
			snapshot, 17U, fixedShapeEntries,
			[](std::span<BTreePageEntry> entries, const BTreeInitialTreeValueBindingContext &) {
				for (size_t ordinal = 0U; ordinal < entries.size(); ++ordinal) {
					entries[ordinal].value_or_child = ordinal + 1U;
				}
				entries.front().key = wideKey(99U);
			});
	std::vector<IndexTreeMutationPoolBatch> batches{batchFor(changedFirstKey)};
	expectRejection(changedFirstKey, batches, "changed a key");

	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, 17U).has_value());
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsFreshTreeStagingAndTamperedPreparationBeforeWritingPages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::array<BTreePageEntry, 1U> valueEntry = {BTreePageEntry{{std::byte{'a'}}, 0U}};
	const std::array<BTreePageEntry, 1U> keyOnlyEntry = {BTreePageEntry{{std::byte{'a'}}, 0U}};
	const std::array<BTreePageEntry, 1U> nonemptyKeyOnlyEntry = {BTreePageEntry{{std::byte{'a'}}, 1U}};
	const std::array<BTreePageEntry, 1U> alreadyBoundEntry = {BTreePageEntry{{std::byte{'b'}}, 7U}};
	const size_t stagedInputLimit =
			static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT) * RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	const std::vector<std::byte> oversizedKey(stagedInputLimit, std::byte{'x'});
	const std::array<BTreePageEntry, 1U> oversizedEntry = {BTreePageEntry{oversizedKey, 1U}};
	const std::vector<std::byte> halfLimitKey(stagedInputLimit / 2U, std::byte{'y'});
	const std::array<BTreePageEntry, 2U> cumulativelyOversizedEntries = {
			BTreePageEntry{halfLimitKey, 1U}, BTreePageEntry{halfLimitKey, 2U}};

	expectFormatError([&] { static_cast<void>(store.planInitialTreeMutation(snapshot, 17U, oversizedEntry)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store.planInitialTreeMutation(snapshot, 18U, cumulativelyOversizedEntries)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.planInitialTreeMutation(
						snapshot, 17U, keyOnlyEntry,
						[](std::span<BTreePageEntry>, const BTreeInitialTreeValueBindingContext &) {},
						BTreeLeafValueLayout::BLVL_EMPTY));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.planInitialTreeMutation(
						snapshot, 17U, nonemptyKeyOnlyEntry, {}, BTreeLeafValueLayout::BLVL_EMPTY));
			},
			FormatError::FERR_INVALID_HEADER);

	const BTreeInitialTreeMutationPlan plan = store.planInitialTreeMutation(
			snapshot, 17U, valueEntry,
			[](std::span<BTreePageEntry> boundEntries, const BTreeInitialTreeValueBindingContext &) {
				boundEntries.front().value_or_child = 0U;
			});
	EXPECT_NO_THROW(static_cast<void>(store.planInitialTreeMutation(
			snapshot, 19U, alreadyBoundEntry,
			[](std::span<BTreePageEntry> boundEntries, const BTreeInitialTreeValueBindingContext &) {
				boundEntries.front().value_or_child = 8U;
			})));
	IndexTreeMutationPoolBatch batch;
	batch.pool = {SegmentId(71U), PhysicalSlotId(40U), SegmentKind::SGK_INDEX_PAGE_POOL, 17U, Generation(1U),
				  SegmentState::SGS_SEALED, LogSequenceNumber(2U)};
	batch.newly_reserved = true;
	batch.appended_page_ids = {PageId(101U)};
	batch.appended_locations = {{PageId(101U), batch.pool.segment_id, 0U, batch.pool.generation, 0U}};
	const auto prepareSingleBatch = [&plan](IndexTreeMutationPoolBatch candidate, LogSequenceNumber checkpointLsn) {
		std::vector<IndexTreeMutationPoolBatch> batches;
		batches.push_back(std::move(candidate));
		plan.preparer(batches, checkpointLsn);
	};

	expectFormatError([&] { prepareSingleBatch(batch, LogSequenceNumber{}); }, FormatError::FERR_INVALID_STATE);
	const auto expectTamperedBatch = [&](const auto &tamper) {
		IndexTreeMutationPoolBatch candidate = batch;
		tamper(candidate);
		expectFormatError([&] { prepareSingleBatch(candidate, LogSequenceNumber(2U)); }, FormatError::FERR_INVALID_STATE);
	};
	expectTamperedBatch([](IndexTreeMutationPoolBatch &candidate) { candidate.newly_reserved = false; });
	expectTamperedBatch(
			[](IndexTreeMutationPoolBatch &candidate) { candidate.pool.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL; });
	expectTamperedBatch([](IndexTreeMutationPoolBatch &candidate) { candidate.pool.owner_id = 18U; });
	expectTamperedBatch([](IndexTreeMutationPoolBatch &candidate) { candidate.pool.generation = Generation(2U); });
	expectTamperedBatch(
			[](IndexTreeMutationPoolBatch &candidate) { candidate.pool.state = SegmentState::SGS_BUILDING; });
	expectTamperedBatch(
			[](IndexTreeMutationPoolBatch &candidate) { candidate.pool.committed_lsn = LogSequenceNumber(3U); });
	expectTamperedBatch([](IndexTreeMutationPoolBatch &candidate) { candidate.appended_page_ids.clear(); });
	expectTamperedBatch([](IndexTreeMutationPoolBatch &candidate) { candidate.appended_locations.clear(); });
	std::vector<IndexTreeMutationPoolBatch> missingBatches;
	expectFormatError([&] { plan.preparer(missingBatches, LogSequenceNumber(2U)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { prepareSingleBatch(batch, LogSequenceNumber(2U)); }, FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, 17U).has_value());
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsInvalidRetirementAndFreshTreePlanningContracts) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::array<BTreePageEntry, 1U> resolvedEntries = {BTreePageEntry{{std::byte{'a'}}, 1U}};
	const std::array<BTreePageEntry, 1U> unresolvedEntries = {BTreePageEntry{{std::byte{'a'}}, 0U}};

	expectFormatError([&] { static_cast<void>(store.planTreeRetirement(snapshot, 0U, 1U)); },
				  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.planTreeRetirement(snapshot, 17U, 0U)); },
				  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store.planInitialTreeMutation(snapshot, 0U, resolvedEntries)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.planInitialTreeMutation(
						snapshot, 17U, resolvedEntries, {}, static_cast<BTreeLeafValueLayout>(0xffU)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.planInitialTreeMutation(snapshot, 17U, unresolvedEntries)); },
				  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.planReplacementTreeMutation(snapshot, 17U, resolvedEntries)); },
				  FormatError::FERR_INVALID_STATE);
	snapshot.release();
	expectFormatError([&] { static_cast<void>(store.planTreeRetirement(snapshot, 17U, 1U)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableBTreePageStoreTest, InitialTreePlanRejectsAVisibleRootWithoutReservingCandidatePages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation publishedRoot{17U, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&publishedRoot, 1U), {}, LogSequenceNumber(3U)));

	const std::array<BTreePageEntry, 1U> entries = {BTreePageEntry{{std::byte{'a'}}, 1U}};
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.planInitialTreeMutation(snapshot, 17U, entries)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(metadata->findIndexRoot(snapshot, 17U), publishedRoot);
	EXPECT_EQ(metadata->findPageLocation(snapshot, request.page_id).page_id, request.page_id);
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsRootLeafWithPersistedSiblingLinkBeforeErasing) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation root{request.page_owner_id, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry currentPool = metadata->findCatalog(snapshot, request.segment_id);
	BTreePageHeader malformedHeader;
	malformedHeader.index_id = request.page_owner_id;
	malformedHeader.page_id = request.page_id;
	malformedHeader.generation = request.generation;
	malformedHeader.right_sibling = PageId(999U);
	const BTreePage malformed =
			BTreePage::create(treeOwner(currentPool, request.page_id, LogSequenceNumber(2U)), malformedHeader);
	overwriteCurrentBTreePage(*io, *metadata, request.page_id,
						  treeOwner(currentPool, request.page_id, LogSequenceNumber(2U)), malformed);

	const std::array<std::byte, 1U> key = {std::byte{'a'}};
	expectFormatError([&] { static_cast<void>(store.erase(request.page_owner_id, key, LogSequenceNumber(4U))); },
				  FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, PlansRootLeafRetirementAndRejectsTamperedPreparationBatches) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation root{request.page_owner_id, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_NE(metadata->findCatalog(snapshot, request.segment_id).owner_id, request.page_owner_id);
	const BTreeTreeRetirementPlan plan = store.planTreeRetirement(snapshot, request.page_owner_id, 1U);
	EXPECT_EQ(plan.expected_root, root);
	ASSERT_EQ(plan.pool_requests.size(), 1U);
	ASSERT_EQ(plan.pool_requests.front().expected_retirements.size(), 1U);
	EXPECT_EQ(plan.pool_requests.front().expected_retirements.front().location.page_id, request.page_id);
	std::vector<IndexTreeMutationPoolBatch> tamperedBatches;
	expectFormatError([&] { plan.preparer(tamperedBatches, LogSequenceNumber(4U)); }, FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, TraversesEveryInternalTreePageWhenPlanningRetirement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation initialRoot{request.page_owner_id, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1U), {}, LogSequenceNumber(3U)));

	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, request.page_owner_id, 3U, 0U, LogSequenceNumber(4U),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				BTreePageHeader rootHeader;
				rootHeader.index_id = 17U;
				rootHeader.page_id = pageIds[0];
				rootHeader.generation = poolEntry.generation;
				rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
				rootHeader.level = 1U;
				rootHeader.leftmost_child = pageIds[1];
				BTreePage root = BTreePage::create(treeOwner(poolEntry, pageIds[0], LogSequenceNumber(4U)), rootHeader);
				root.insert({{std::byte{'m'}}, pageIds[2].value()});

				std::vector<BTreePage> pages;
				pages.push_back(std::move(root));
				for (const PageId leafId: std::span(pageIds).subspan(1U)) {
					BTreePageHeader leafHeader;
					leafHeader.index_id = 17U;
					leafHeader.page_id = leafId;
					leafHeader.generation = poolEntry.generation;
					pages.push_back(BTreePage::create(treeOwner(poolEntry, leafId, LogSequenceNumber(4U)), leafHeader));
				}
				return pages;
			}));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, request.page_owner_id);
	const BTreeTreeRetirementPlan plan = store.planTreeRetirement(snapshot, request.page_owner_id, 3U);
	ASSERT_EQ(plan.pool_requests.size(), 1U);
	const std::vector<IndexTreePageRetirement> &retirements = plan.pool_requests.front().expected_retirements;
	ASSERT_EQ(retirements.size(), 3U);
	EXPECT_EQ(retirements[0].location.page_id, root.root_page_id);
	EXPECT_NE(retirements[1].location.page_id, root.root_page_id);
	EXPECT_NE(retirements[2].location.page_id, root.root_page_id);
	EXPECT_EQ(retirements[0].location.segment_id, request.segment_id);
	EXPECT_EQ(retirements[1].location.segment_id, request.segment_id);
	EXPECT_EQ(retirements[2].location.segment_id, request.segment_id);
	IndexTreeMutationPoolBatch batch;
	batch.pool = metadata->findCatalog(snapshot, request.segment_id);
	batch.expected_retirements = retirements;
	const auto expectTamperedRetirementBatch = [&](const auto &tamper) {
		IndexTreeMutationPoolBatch candidate = batch;
		tamper(candidate);
		std::vector<IndexTreeMutationPoolBatch> candidateBatches;
		candidateBatches.push_back(std::move(candidate));
		expectFormatError([&] { plan.preparer(candidateBatches, LogSequenceNumber(5U)); },
					  FormatError::FERR_INVALID_STATE);
	};
	std::vector<IndexTreeMutationPoolBatch> missingBatches;
	expectFormatError([&] { plan.preparer(missingBatches, LogSequenceNumber(5U)); }, FormatError::FERR_INVALID_STATE);
	expectTamperedRetirementBatch([](IndexTreeMutationPoolBatch &candidate) {
		candidate.pool.segment_id = SegmentId(99U);
	});
	expectTamperedRetirementBatch([](IndexTreeMutationPoolBatch &candidate) {
		candidate.expected_replacements.push_back({});
	});
	expectTamperedRetirementBatch([](IndexTreeMutationPoolBatch &candidate) {
		candidate.appended_page_ids.push_back(PageId(999U));
	});
	expectTamperedRetirementBatch([](IndexTreeMutationPoolBatch &candidate) { candidate.newly_reserved = true; });
	expectTamperedRetirementBatch(
			[](IndexTreeMutationPoolBatch &candidate) { candidate.expected_retirements.clear(); });
	std::vector<IndexTreeMutationPoolBatch> batches;
	batches.push_back(std::move(batch));
	EXPECT_NO_THROW(plan.preparer(batches, LogSequenceNumber(5U)));
	EXPECT_TRUE(batches.front().replacement_locations.empty());
	EXPECT_TRUE(batches.front().appended_locations.empty());
	std::vector<IndexTreeMutationPoolBatch> tamperedReserved = batches;
	tamperedReserved.front().newly_reserved = true;
	expectFormatError([&] { plan.preparer(tamperedReserved, LogSequenceNumber(5U)); },
				  FormatError::FERR_INVALID_STATE);
	std::vector<IndexTreeMutationPoolBatch> tamperedRetirements = batches;
	tamperedRetirements.front().expected_retirements.clear();
	expectFormatError([&] { plan.preparer(tamperedRetirements, LogSequenceNumber(5U)); },
				  FormatError::FERR_INVALID_STATE);

	const BTreePage originalRoot = store.read(snapshot, root.root_page_id);
	const CatalogEntry poolEntry = metadata->findCatalog(snapshot, request.segment_id);
	BTreePageHeader corruptedHeader = originalRoot.header();
	corruptedHeader.right_sibling = corruptedHeader.leftmost_child;
	const std::vector<BTreePageEntry> rootEntries = originalRoot.entries();
	snapshot.release();
	const BTreePage corruptedRoot = rebuildPage(poolEntry, corruptedHeader, LogSequenceNumber(4U), rootEntries);
	overwriteCurrentBTreePage(*io, *metadata, root.root_page_id,
						  treeOwner(poolEntry, root.root_page_id, LogSequenceNumber(4U)), corruptedRoot);
	snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.planTreeRetirement(snapshot, request.page_owner_id, 3U)); },
				  FormatError::FERR_INVALID_STATE);

	expectFormatError([&] { static_cast<void>(store.planTreeRetirement(snapshot, request.page_owner_id, 1U)); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsAnUnroutedInternalChildDuringTreeRetirementPlanning) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation initialRoot{request.page_owner_id, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1U), {}, LogSequenceNumber(3U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry currentPool = metadata->findCatalog(snapshot, request.segment_id);
	BTreePageHeader malformedHeader;
	malformedHeader.index_id = request.page_owner_id;
	malformedHeader.page_id = request.page_id;
	malformedHeader.generation = request.generation;
	malformedHeader.kind = BTreeNodeKind::BPN_INTERNAL;
	malformedHeader.level = 1U;
	malformedHeader.leftmost_child = PageId(999U);
	const BTreePage malformed = BTreePage::create(treeOwner(currentPool, request.page_id, LogSequenceNumber(2U)),
													malformedHeader);
	overwriteCurrentBTreePage(*io, *metadata, request.page_id,
						  treeOwner(currentPool, request.page_id, LogSequenceNumber(2U)), malformed);
	expectFormatError([&] { static_cast<void>(store.planTreeRetirement(snapshot, request.page_owner_id, 2U)); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsACyclicInternalTreeDuringRetirementPlanning) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation root{request.page_owner_id, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry currentPool = metadata->findCatalog(snapshot, request.segment_id);
	BTreePageHeader malformedHeader;
	malformedHeader.index_id = request.page_owner_id;
	malformedHeader.page_id = request.page_id;
	malformedHeader.generation = request.generation;
	malformedHeader.kind = BTreeNodeKind::BPN_INTERNAL;
	malformedHeader.level = 1U;
	malformedHeader.leftmost_child = request.page_id;
	const BTreePage malformed = BTreePage::create(treeOwner(currentPool, request.page_id, LogSequenceNumber(2U)),
															 malformedHeader);
	overwriteCurrentBTreePage(*io, *metadata, request.page_id,
						  treeOwner(currentPool, request.page_id, LogSequenceNumber(2U)), malformed);

	expectFormatError([&] { static_cast<void>(store.planTreeRetirement(snapshot, request.page_owner_id, 2U)); },
				  FormatError::FERR_INVALID_STATE);
	snapshot.release();
}
