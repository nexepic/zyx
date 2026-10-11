/**
 * @file test_RelocatableMetadataStoreEvacuationNativeFaults.cpp
 * @brief Native I/O fault boundaries for routed metadata-pool evacuation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <cerrno>
#include <system_error>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/relocatable/metadata/CatalogRadix.hpp"
#include "graph/storage/relocatable/metadata/PageDirectoryBootstrap.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

	bool isCandidateBootstrapHeader(const void *bytes, size_t byteCount) {
		if (byteCount != RSS_PAGE_SIZE) {
			return false;
		}
		try {
			const PageDirectoryBootstrap bootstrap = PageDirectoryBootstrapCodec::decode(
					std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
			return bootstrap.generation == Generation(10U) && bootstrap.metadata_page_directory_root.isValid();
		} catch (const FormatException &) {
			return false;
		}
	}

	bool isCandidateRouteRadixPage(const void *bytes, size_t byteCount) {
		if (byteCount != RSS_PAGE_SIZE) {
			return false;
		}
		try {
			const MetadataRadixPage page = MetadataRadixPageCodec::decode(
					std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
			return page.directory_type == MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE &&
				   page.generation == Generation(10U) && !page.logical_children_enabled;
		} catch (const FormatException &) {
			return false;
		}
	}

	bool isCandidateSuperblock(const void *bytes, size_t byteCount, pwrite_off_t offset) {
		static_cast<void>(bytes);
		return byteCount == RSS_SUPERBLOCK_BYTES && offset >= 0 &&
			   static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES;
	}

	void expectOldRoutedGeneration(const RelocatableMetadataStore &store) {
		MetadataSnapshotPin snapshot = store.openSnapshot();
		EXPECT_EQ(snapshot.superblock().generation, Generation(9U));
		EXPECT_EQ(store.findCatalog(snapshot, SegmentId(200U)).physical_slot, PhysicalSlotId(2U));
		EXPECT_EQ(store.findExtentLocation(snapshot, ExtentId(1U)),
				  (ExtentLocation{ExtentId(1U), SegmentId(20U), RSS_PAGE_POOL_FIRST_DATA_FRAME, 1U, Generation(1U)}));
	}

	void expectPublishedRoutedGeneration(const RelocatableMetadataStore &store) {
		MetadataSnapshotPin snapshot = store.openSnapshot();
		EXPECT_EQ(snapshot.superblock().generation, Generation(10U));
		EXPECT_EQ(store.findCatalog(snapshot, SegmentId(200U)).physical_slot, PhysicalSlotId(2U));
		EXPECT_EQ(store.findExtentLocation(snapshot, ExtentId(1U)),
				  (ExtentLocation{ExtentId(1U), SegmentId(20U), RSS_PAGE_POOL_FIRST_DATA_FRAME, 1U, Generation(1U)}));
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest,
	   NativeRoutedMetadataPoolRelocationBootstrapHeaderShortWriteKeepsTheOldGenerationAndCanRetry) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	prepareRoutedMetadataPoolForEvacuation(*store);
	store.reset();

	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
	store = RelocatableMetadataStore::open(io);
	bool interrupted = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting shortBootstrapHeader(
				[&interrupted](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (!interrupted && offset >= 0 && static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
						isCandidateBootstrapHeader(bytes, byteCount)) {
						interrupted = true;
						return portable_pwrite(fd, bytes, byteCount - 1U, offset);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(store->evacuateSparseMetadataPagePool(
						 LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT)),
				 std::system_error);
	}
	EXPECT_TRUE(interrupted);
	expectOldRoutedGeneration(*store);

	const MetadataPagePoolEvacuation retried =
			store->evacuateSparseMetadataPagePool(LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	EXPECT_EQ(retried.superblock.generation, Generation(10U));
	EXPECT_GT(retried.relocated_metadata_pages, 0U);
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	expectPublishedRoutedGeneration(*reopened);
	io.reset();
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableMetadataStoreTest,
	   NativeRoutedMetadataPoolRelocationBootstrapHeaderEnospcKeepsTheOldGenerationAndCanRetry) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	prepareRoutedMetadataPoolForEvacuation(*store);
	store.reset();

	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
	store = RelocatableMetadataStore::open(io);
	bool interrupted = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting failBootstrapHeader(
				[&interrupted](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (!interrupted && offset >= 0 && static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
						isCandidateBootstrapHeader(bytes, byteCount)) {
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
		EXPECT_THROW(static_cast<void>(store->evacuateSparseMetadataPagePool(
						 LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT)),
				 std::system_error);
	}
	EXPECT_TRUE(interrupted);
	expectOldRoutedGeneration(*store);

	const MetadataPagePoolEvacuation retried =
			store->evacuateSparseMetadataPagePool(LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	EXPECT_EQ(retried.superblock.generation, Generation(10U));
	EXPECT_GT(retried.relocated_metadata_pages, 0U);
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	expectPublishedRoutedGeneration(*reopened);
	io.reset();
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableMetadataStoreTest,
	   NativeRoutedMetadataPoolRelocationRouteRadixShortWriteKeepsTheOldGenerationAndCanRetry) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	prepareRoutedMetadataPoolForEvacuation(*store);
	store.reset();

	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
	store = RelocatableMetadataStore::open(io);
	bool interrupted = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting shortRouteRadix(
				[&interrupted](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (!interrupted && offset >= 0 && static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
						isCandidateRouteRadixPage(bytes, byteCount)) {
						interrupted = true;
						return portable_pwrite(fd, bytes, byteCount - 1U, offset);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(store->evacuateSparseMetadataPagePool(
						 LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT)),
				 std::system_error);
	}
	EXPECT_TRUE(interrupted);
	expectOldRoutedGeneration(*store);

	const MetadataPagePoolEvacuation retried =
			store->evacuateSparseMetadataPagePool(LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	EXPECT_EQ(retried.superblock.generation, Generation(10U));
	EXPECT_GT(retried.relocated_metadata_pages, 0U);
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	expectPublishedRoutedGeneration(*reopened);
	io.reset();
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableMetadataStoreTest,
	   NativeRoutedMetadataPoolRelocationRouteRadixEnospcKeepsTheOldGenerationAndCanRetry) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	prepareRoutedMetadataPoolForEvacuation(*store);
	store.reset();

	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
	store = RelocatableMetadataStore::open(io);
	bool interrupted = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting failRouteRadix(
				[&interrupted](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (!interrupted && offset >= 0 && static_cast<uint64_t>(offset) >= RSS_FILE_BOOTSTRAP_BYTES &&
						isCandidateRouteRadixPage(bytes, byteCount)) {
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
		EXPECT_THROW(static_cast<void>(store->evacuateSparseMetadataPagePool(
						 LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT)),
				 std::system_error);
	}
	EXPECT_TRUE(interrupted);
	expectOldRoutedGeneration(*store);

	const MetadataPagePoolEvacuation retried =
			store->evacuateSparseMetadataPagePool(LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	EXPECT_EQ(retried.superblock.generation, Generation(10U));
	EXPECT_GT(retried.relocated_metadata_pages, 0U);
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	expectPublishedRoutedGeneration(*reopened);
	io.reset();
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableMetadataStoreTest,
	   NativeRoutedMetadataPoolRelocationSuperblockShortWriteKeepsTheOldGenerationAndCanRetry) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	prepareRoutedMetadataPoolForEvacuation(*store);
	store.reset();

	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
	store = RelocatableMetadataStore::open(io);
	bool interrupted = false;
	{
			StorageIO::ScopedNativeWriteHookForTesting shortSuperblock(
					[&interrupted](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (!interrupted && isCandidateSuperblock(bytes, byteCount, offset)) {
							interrupted = true;
							// Stop before CRC so the inactive slot cannot validate as a new generation.
							return portable_pwrite(fd, bytes, RSS_SUPERBLOCK_HEADER_CRC_OFFSET - 1U, offset);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(store->evacuateSparseMetadataPagePool(
						 LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT)),
				 std::system_error);
	}
	EXPECT_TRUE(interrupted);
	expectOldRoutedGeneration(*store);

	const MetadataPagePoolEvacuation retried =
			store->evacuateSparseMetadataPagePool(LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	EXPECT_EQ(retried.superblock.generation, Generation(10U));
	EXPECT_GT(retried.relocated_metadata_pages, 0U);
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	expectPublishedRoutedGeneration(*reopened);
	io.reset();
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableMetadataStoreTest,
	   NativeRoutedMetadataPoolRelocationSuperblockEnospcKeepsTheOldGenerationAndCanRetry) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	prepareRoutedMetadataPoolForEvacuation(*store);
	store.reset();

	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
	store = RelocatableMetadataStore::open(io);
	bool interrupted = false;
	{
		StorageIO::ScopedNativeWriteHookForTesting failSuperblock(
				[&interrupted](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (!interrupted && isCandidateSuperblock(bytes, byteCount, offset)) {
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
		EXPECT_THROW(static_cast<void>(store->evacuateSparseMetadataPagePool(
						 LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT)),
				 std::system_error);
	}
	EXPECT_TRUE(interrupted);
	expectOldRoutedGeneration(*store);

	const MetadataPagePoolEvacuation retried =
			store->evacuateSparseMetadataPagePool(LogSequenceNumber(10U), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	EXPECT_EQ(retried.superblock.generation, Generation(10U));
	EXPECT_GT(retried.relocated_metadata_pages, 0U);
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	expectPublishedRoutedGeneration(*reopened);
	io.reset();
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}
