/**
 * @file test_SuperblockFile.cpp
 * @brief Durable reopen and crash-boundary tests for relocatable A/B superblocks.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <algorithm>
#include <cerrno>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"

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

	void expectInterrupted(const std::function<void()> &operation) {
		EXPECT_THROW(operation(), BootstrapPublishInterrupted);
	}

	PhysicalPageRef pageRef(uint64_t slot, uint8_t frame, uint64_t generation) {
		PhysicalPageRef result;
		result.physical_slot = PhysicalSlotId(slot);
		result.frame_index = frame;
		result.generation = Generation(generation);
		return result;
	}

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn, uint64_t rootSlot) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		result.catalog_root = pageRef(rootSlot, 1, generation);
		result.page_directory_root = pageRef(rootSlot + 1U, 2, generation);
		result.extent_directory_root = pageRef(rootSlot + 2U, 3, generation);
		result.allocator_root = pageRef(rootSlot + 3U, 4, generation);
		return result;
	}

} // namespace

class RelocatableSuperblockFileTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("relocatable_superblock_" + to_string(uuid) + ".dat");
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

TEST_F(RelocatableSuperblockFileTest, InitializesFixedBootstrapAndPersistsBothCopies) {
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(superblock(1, 10, 100));

	EXPECT_EQ(std::filesystem::file_size(testFilePath), RSS_FILE_BOOTSTRAP_BYTES);
	EXPECT_EQ(bootstrap.recover().generation, Generation(1));
	EXPECT_EQ(SuperblockCodec::decode(bootstrap.readSlot(0)).generation, Generation(1));
	EXPECT_EQ(SuperblockCodec::decode(bootstrap.readSlot(1)).generation, Generation(1));
}

TEST_F(RelocatableSuperblockFileTest, PublishesThenReopensTheNewestMetadataGeneration) {
	{
		SuperblockFile bootstrap(io);
		bootstrap.initializeNew(superblock(1, 10, 100));
		bootstrap.publish(superblock(2, 11, 200));
		bootstrap.publish(superblock(3, 12, 300));
		bootstrap.publish(superblock(4, 13, 400));
		EXPECT_EQ(bootstrap.recover().catalog_root.physical_slot, PhysicalSlotId(400));
	}

	reopen();
	SuperblockFile reopened(io);
	EXPECT_EQ(reopened.recover().generation, Generation(4));
	EXPECT_EQ(reopened.recover().checkpoint_lsn, LogSequenceNumber(13));
	EXPECT_EQ(reopened.recover().catalog_root.physical_slot, PhysicalSlotId(400));
}

TEST_F(RelocatableSuperblockFileTest, RebuildsEitherCorruptPeerWithoutDiscardingTheValidCopy) {
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(superblock(1, 10, 100));
	bootstrap.publish(superblock(2, 11, 200));

	std::byte corrupt = std::byte{0};
	io->writeAt(SuperblockFile::slotOffset(0), &corrupt, sizeof(corrupt));
	io->sync();
	EXPECT_EQ(bootstrap.recover().generation, Generation(2));
	bootstrap.publish(superblock(3, 12, 300));
	EXPECT_EQ(SuperblockCodec::decode(bootstrap.readSlot(0)).generation, Generation(3));

	io->writeAt(SuperblockFile::slotOffset(1), &corrupt, sizeof(corrupt));
	io->sync();
	EXPECT_EQ(bootstrap.recover().generation, Generation(3));
	bootstrap.publish(superblock(4, 13, 400));
	EXPECT_EQ(SuperblockCodec::decode(bootstrap.readSlot(1)).generation, Generation(4));
}

TEST_F(RelocatableSuperblockFileTest, CrashBoundariesRetainOldOrPublishNewGenerationDeterministically) {
	SuperblockFile bootstrap(io);
	bootstrap.initializeNew(superblock(5, 30, 100));
	expectInterrupted(
			[&] { bootstrap.publish(superblock(6, 31, 200), BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE); });
	EXPECT_EQ(bootstrap.recover().generation, Generation(5));

	expectInterrupted([&] {
		bootstrap.publish(superblock(6, 31, 200), BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE);
	});
	EXPECT_EQ(bootstrap.recover().generation, Generation(5));

	expectInterrupted(
			[&] { bootstrap.publish(superblock(6, 31, 200), BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE); });
	EXPECT_EQ(bootstrap.recover().generation, Generation(6));
	EXPECT_EQ(bootstrap.recover().catalog_root.physical_slot, PhysicalSlotId(200));
}

TEST_F(RelocatableSuperblockFileTest, NativeShortWriteCannotPublishAPartialGeneration) {
	const file_handle_t nativeFd = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeFd, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeFd, nativeFd);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(superblock(5, 30, 100));
		{
			StorageIO::ScopedNativeWriteHookForTesting shortWrite(
					[](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						const size_t partialBytes = std::min(byteCount, static_cast<size_t>(RSS_SUPERBLOCK_HEADER_CRC_OFFSET));
						return portable_pwrite(fd, bytes, partialBytes, offset);
					});
			EXPECT_THROW(bootstrap.publish(superblock(6, 31, 200)), std::runtime_error);
		}
		EXPECT_EQ(bootstrap.recover().generation, Generation(5));
		EXPECT_EQ(bootstrap.recover().catalog_root.physical_slot, PhysicalSlotId(100));
	}
	EXPECT_EQ(portable_close_rw(nativeFd), 0);
	reopen();
	SuperblockFile reopened(io);
	EXPECT_EQ(reopened.recover().generation, Generation(5));
	EXPECT_EQ(reopened.recover().catalog_root.physical_slot, PhysicalSlotId(100));
}

TEST_F(RelocatableSuperblockFileTest, NativeDiskFullFailureCannotPublishANewGeneration) {
	const file_handle_t nativeFd = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeFd, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeFd, nativeFd);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(superblock(7, 40, 100));
		{
			StorageIO::ScopedNativeWriteHookForTesting diskFull(
					[](file_handle_t, const void *, size_t, pwrite_off_t) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					});
			EXPECT_THROW(bootstrap.publish(superblock(8, 41, 200)), std::runtime_error);
		}
		EXPECT_EQ(bootstrap.recover().generation, Generation(7));
		EXPECT_EQ(bootstrap.recover().catalog_root.physical_slot, PhysicalSlotId(100));
	}
	EXPECT_EQ(portable_close_rw(nativeFd), 0);
	reopen();
	SuperblockFile reopened(io);
	EXPECT_EQ(reopened.recover().generation, Generation(7));
	EXPECT_EQ(reopened.recover().catalog_root.physical_slot, PhysicalSlotId(100));
}

TEST_F(RelocatableSuperblockFileTest, NativeCandidateRootSyncFailureCannotPublishANewGeneration) {
	const file_handle_t nativeFd = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeFd, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeFd, nativeFd);
		SuperblockFile bootstrap(nativeIo);
		bootstrap.initializeNew(superblock(9, 50, 100));
		{
			StorageIO::ScopedNativeSyncHookForTesting syncFailure([](file_handle_t) {
#ifdef _WIN32
				SetLastError(ERROR_DISK_FULL);
#else
				errno = ENOSPC;
#endif
				return -1;
			});
			EXPECT_THROW(bootstrap.publish(superblock(10, 51, 200)), std::runtime_error);
		}
		EXPECT_EQ(bootstrap.recover().generation, Generation(9));
		EXPECT_EQ(bootstrap.recover().catalog_root.physical_slot, PhysicalSlotId(100));
	}
	EXPECT_EQ(portable_close_rw(nativeFd), 0);
	reopen();
	SuperblockFile reopened(io);
	EXPECT_EQ(reopened.recover().generation, Generation(9));
	EXPECT_EQ(reopened.recover().catalog_root.physical_slot, PhysicalSlotId(100));
}

TEST_F(RelocatableSuperblockFileTest, RejectsMalformedStaleAndTruncatedBootstrapStates) {
	EXPECT_THROW(static_cast<void>(SuperblockFile(nullptr)), std::invalid_argument);
	SuperblockFile bootstrap(io);
	expectFormatError([&] { static_cast<void>(bootstrap.recover()); }, FormatError::FERR_BUFFER_TOO_SMALL);
	expectFormatError([&] { static_cast<void>(bootstrap.readSlot(0)); }, FormatError::FERR_BUFFER_TOO_SMALL);

	bootstrap.initializeNew(superblock(7, 40, 100));
	expectFormatError([&] { bootstrap.publish(superblock(7, 41, 200)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { bootstrap.publish(superblock(8, 40, 200)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { bootstrap.publish(superblock(8, 41, 200), static_cast<BootstrapPublishFailpoint>(99)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(bootstrap.readSlot(2)); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(bootstrap.recover().generation, Generation(7));
}
