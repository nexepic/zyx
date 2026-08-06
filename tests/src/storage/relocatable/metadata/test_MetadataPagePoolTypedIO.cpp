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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <gtest/gtest.h>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "storage/relocatable/metadata/MetadataPagePoolTestFixture.hpp"

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
	EXPECT_EQ(stats.hits, 3U);
	EXPECT_EQ(stats.misses, 0U);
	EXPECT_EQ(stats.disk_read_bytes, 0U);
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
