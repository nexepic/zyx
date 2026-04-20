/**
 * @file test_StorageIO.cpp
 * @brief Unit tests for StorageIO unified I/O abstraction
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"

using namespace graph::storage;

class StorageIOTest : public ::testing::Test {
protected:
	std::string testFile_ = "test_storage_io.bin";
	const size_t testSize_ = 4096;

	void SetUp() override {
		// Create test file with zeros
		std::ofstream file(testFile_, std::ios::binary);
		std::vector<char> data(testSize_, 0);
		file.write(data.data(), static_cast<std::streamsize>(testSize_));
		file.close();
	}

	void TearDown() override {
		std::error_code ec;
		std::filesystem::remove(testFile_, ec);
	}

	std::shared_ptr<StorageIO> createIO(bool withNativeFds = true) {
		auto stream = std::make_shared<std::fstream>(
				testFile_, std::ios::binary | std::ios::in | std::ios::out);
		EXPECT_TRUE(stream->good());

		file_handle_t wfd = INVALID_FILE_HANDLE;
		file_handle_t rfd = INVALID_FILE_HANDLE;
		if (withNativeFds) {
			wfd = portable_open_rw(testFile_.c_str());
			rfd = portable_open(testFile_.c_str(), O_RDONLY);
		}

		return std::make_shared<StorageIO>(stream, wfd, rfd);
	}
};

// ============================================================================
// writeAt / readAt roundtrip with native fds (pwrite/pread)
// ============================================================================

TEST_F(StorageIOTest, WriteAtReadAtRoundtrip_NativeFd) {
	auto io = createIO(/*withNativeFds=*/true);
	ASSERT_TRUE(io->hasPwriteSupport());
	ASSERT_TRUE(io->hasPreadSupport());

	const char data[] = "Hello StorageIO!";
	io->writeAt(100, data, sizeof(data));

	char buffer[sizeof(data)] = {};
	size_t bytesRead = io->readAt(100, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, sizeof(data));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);
}

// ============================================================================
// writeAt / readAt roundtrip with fstream fallback
// ============================================================================

TEST_F(StorageIOTest, WriteAtReadAtRoundtrip_FstreamFallback) {
	auto io = createIO(/*withNativeFds=*/false);
	ASSERT_FALSE(io->hasPwriteSupport());
	ASSERT_FALSE(io->hasPreadSupport());

	const char data[] = "Fstream fallback!";
	io->writeAt(200, data, sizeof(data));

	char buffer[sizeof(data)] = {};
	size_t bytesRead = io->readAt(200, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, sizeof(data));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);
}

// ============================================================================
// writeAt at non-zero offset
// ============================================================================

TEST_F(StorageIOTest, WriteAtMiddleOfFile) {
	auto io = createIO(true);

	const uint64_t offset = 2048;
	const char data[] = "middle write";
	io->writeAt(offset, data, sizeof(data));

	char buffer[sizeof(data)] = {};
	size_t bytesRead = io->readAt(offset, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, sizeof(data));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);

	// Verify beginning is still zeros
	char zeroBuf[16] = {};
	char expected[16] = {};
	std::memset(expected, 0, sizeof(expected));
	io->readAt(0, zeroBuf, sizeof(zeroBuf));
	EXPECT_EQ(std::memcmp(zeroBuf, expected, sizeof(expected)), 0);
}

// ============================================================================
// append returns correct offset
// ============================================================================

TEST_F(StorageIOTest, AppendReturnsCorrectOffset) {
	auto io = createIO(false);  // append uses fstream

	const char data1[] = "first append";
	uint64_t offset1 = io->append(data1, sizeof(data1));
	EXPECT_EQ(offset1, testSize_);  // Should be at end of existing file

	const char data2[] = "second append";
	uint64_t offset2 = io->append(data2, sizeof(data2));
	EXPECT_EQ(offset2, testSize_ + sizeof(data1));

	// Verify data can be read back
	io->flushStream();  // Ensure fstream writes are visible to reads
	char buffer[sizeof(data1)] = {};
	size_t bytesRead = io->readAt(offset1, buffer, sizeof(data1));
	EXPECT_EQ(bytesRead, sizeof(data1));
	EXPECT_EQ(std::memcmp(data1, buffer, sizeof(data1)), 0);
}

// ============================================================================
// writeAt with null buffer or zero size is a no-op
// ============================================================================

TEST_F(StorageIOTest, WriteAtNullBufferNoOp) {
	auto io = createIO(true);
	// Should not throw
	io->writeAt(0, nullptr, 10);
	io->writeAt(0, "data", 0);
}

// ============================================================================
// readAt with null buffer or zero size returns 0
// ============================================================================

TEST_F(StorageIOTest, ReadAtNullBufferReturnsZero) {
	auto io = createIO(true);
	EXPECT_EQ(io->readAt(0, nullptr, 10), 0u);
	char buf[1];
	EXPECT_EQ(io->readAt(0, buf, 0), 0u);
}

// ============================================================================
// append with null buffer throws
// ============================================================================

TEST_F(StorageIOTest, AppendNullBufferThrows) {
	auto io = createIO(false);
	EXPECT_THROW(io->append(nullptr, 10), std::invalid_argument);
	EXPECT_THROW(io->append("data", 0), std::invalid_argument);
}

// ============================================================================
// sync does not throw
// ============================================================================

TEST_F(StorageIOTest, SyncDoesNotThrow) {
	auto io = createIO(true);
	EXPECT_NO_THROW(io->sync());
}

// ============================================================================
// flushStream does not throw
// ============================================================================

TEST_F(StorageIOTest, FlushStreamDoesNotThrow) {
	auto io = createIO(true);
	EXPECT_NO_THROW(io->flushStream());
}

// ============================================================================
// getStream returns valid stream
// ============================================================================

TEST_F(StorageIOTest, GetStreamReturnsValidStream) {
	auto io = createIO(true);
	auto stream = io->getStream();
	ASSERT_NE(stream, nullptr);
	EXPECT_TRUE(stream->good());
}

// ============================================================================
// Accessor methods
// ============================================================================

TEST_F(StorageIOTest, GetWriteFdReturnsValidHandle) {
	auto io = createIO(true);
	EXPECT_NE(io->getWriteFd(), INVALID_FILE_HANDLE);
}

TEST_F(StorageIOTest, GetReadFdReturnsValidHandle) {
	auto io = createIO(true);
	EXPECT_NE(io->getReadFd(), INVALID_FILE_HANDLE);
}

TEST_F(StorageIOTest, GetFdsReturnInvalidWhenNotProvided) {
	auto io = createIO(false);
	EXPECT_EQ(io->getWriteFd(), INVALID_FILE_HANDLE);
	EXPECT_EQ(io->getReadFd(), INVALID_FILE_HANDLE);
}

// ============================================================================
// writeAt/readAt null buffer and zero size with fstream fallback
// ============================================================================

TEST_F(StorageIOTest, WriteAtNullBufferNoOp_FstreamFallback) {
	auto io = createIO(false);
	// Should not throw — early return before touching fstream
	io->writeAt(0, nullptr, 10);
	io->writeAt(0, "data", 0);
}

TEST_F(StorageIOTest, ReadAtNullBufferReturnsZero_FstreamFallback) {
	auto io = createIO(false);
	EXPECT_EQ(io->readAt(0, nullptr, 10), 0u);
	char buf[1];
	EXPECT_EQ(io->readAt(0, buf, 0), 0u);
}

// ============================================================================
// writeAt at offset 0 (both native fd and fstream)
// ============================================================================

TEST_F(StorageIOTest, WriteAtOffsetZero_NativeFd) {
	auto io = createIO(true);
	const char data[] = "AtZero";
	io->writeAt(0, data, sizeof(data));

	char buffer[sizeof(data)] = {};
	size_t bytesRead = io->readAt(0, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, sizeof(data));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);
}

TEST_F(StorageIOTest, WriteAtOffsetZero_FstreamFallback) {
	auto io = createIO(false);
	const char data[] = "AtZero";
	io->writeAt(0, data, sizeof(data));

	char buffer[sizeof(data)] = {};
	size_t bytesRead = io->readAt(0, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, sizeof(data));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);
}

// ============================================================================
// readAt beyond file end — pread returns short read (not error)
// ============================================================================

TEST_F(StorageIOTest, ReadAtBeyondFileEnd_NativeFd) {
	auto io = createIO(true);
	char buffer[64] = {};
	// File is 4096 bytes; reading at offset 4096 should return 0 bytes
	size_t bytesRead = io->readAt(testSize_, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, 0u);
}

TEST_F(StorageIOTest, ReadAtBeyondFileEnd_FstreamFallback) {
	auto io = createIO(false);
	char buffer[64] = {};
	// File is 4096 bytes; reading at offset 4096 should return 0 bytes
	size_t bytesRead = io->readAt(testSize_, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, 0u);
}

TEST_F(StorageIOTest, ReadAtPartialRead_NativeFd) {
	auto io = createIO(true);
	char buffer[64] = {};
	// Read starting 32 bytes before EOF — should get only 32 bytes
	size_t bytesRead = io->readAt(testSize_ - 32, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, 32u);
}

TEST_F(StorageIOTest, ReadAtPartialRead_FstreamFallback) {
	auto io = createIO(false);
	char buffer[64] = {};
	// Read starting 32 bytes before EOF — should get only 32 bytes
	size_t bytesRead = io->readAt(testSize_ - 32, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, 32u);
}

// ============================================================================
// pwrite/pread error paths via invalid (closed) file descriptors
// ============================================================================

TEST_F(StorageIOTest, WriteAtPwriteError_InvalidFd) {
	// Create IO with a valid fstream but a bogus write fd
	auto stream = std::make_shared<std::fstream>(
			testFile_, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream->good());

	// Open a real fd then close it to make it invalid
	file_handle_t wfd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(wfd, INVALID_FILE_HANDLE);
	portable_close_rw(wfd);  // Now wfd is closed/invalid

	file_handle_t rfd = portable_open(testFile_.c_str(), O_RDONLY);
	auto io = std::make_shared<StorageIO>(stream, wfd, rfd);

	// pwrite on a closed fd should fail and throw
	const char data[] = "fail";
	EXPECT_THROW(io->writeAt(0, data, sizeof(data)), std::runtime_error);

	if (rfd != INVALID_FILE_HANDLE) portable_close(rfd);
}

TEST_F(StorageIOTest, ReadAtPreadError_InvalidFd) {
	auto stream = std::make_shared<std::fstream>(
			testFile_, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream->good());

	file_handle_t wfd = portable_open_rw(testFile_.c_str());
	file_handle_t rfd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(rfd, INVALID_FILE_HANDLE);
	portable_close(rfd);  // Now rfd is closed/invalid

	auto io = std::make_shared<StorageIO>(stream, wfd, rfd);

	char buffer[16] = {};
	EXPECT_THROW(io->readAt(0, buffer, sizeof(buffer)), std::runtime_error);

	if (wfd != INVALID_FILE_HANDLE) portable_close_rw(wfd);
}

// ============================================================================
// sync with fstream fallback (no native write fd)
// ============================================================================

TEST_F(StorageIOTest, SyncFstreamFallback) {
	auto io = createIO(false);
	// With INVALID_FILE_HANDLE for writeFd, sync should flush the stream
	EXPECT_NO_THROW(io->sync());
}

// ============================================================================
// flushStream with fstream fallback
// ============================================================================

TEST_F(StorageIOTest, FlushStreamFstreamFallback) {
	auto io = createIO(false);
	EXPECT_NO_THROW(io->flushStream());
}

// ============================================================================
// append with native fd IO (append always uses fstream)
// ============================================================================

TEST_F(StorageIOTest, AppendWithNativeFds) {
	auto io = createIO(true);
	const char data[] = "append with fds";
	uint64_t offset = io->append(data, sizeof(data));
	EXPECT_EQ(offset, testSize_);

	// Flush so pread can see the appended data
	io->flushStream();

	char buffer[sizeof(data)] = {};
	size_t bytesRead = io->readAt(offset, buffer, sizeof(buffer));
	EXPECT_EQ(bytesRead, sizeof(data));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);
}

// ============================================================================
// append null buffer throws (with native fds)
// ============================================================================

TEST_F(StorageIOTest, AppendNullBufferThrows_NativeFds) {
	auto io = createIO(true);
	EXPECT_THROW(io->append(nullptr, 10), std::invalid_argument);
	EXPECT_THROW(io->append("data", 0), std::invalid_argument);
}

// ============================================================================
// Multiple sequential writes and reads at various offsets
// ============================================================================

// ============================================================================
// sync and flushStream with null stream
// ============================================================================

TEST_F(StorageIOTest, SyncWithNullStream) {
	// Create IO with valid write fd but null stream
	file_handle_t wfd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(wfd, INVALID_FILE_HANDLE);
	auto io = std::make_shared<StorageIO>(nullptr, wfd, INVALID_FILE_HANDLE);
	// sync should use portable_fsync (writeFd_ is valid), so null stream is fine
	EXPECT_NO_THROW(io->sync());
	portable_close_rw(wfd);
}

TEST_F(StorageIOTest, FlushStreamWithNullStream) {
	// Create IO with null stream
	file_handle_t wfd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(wfd, INVALID_FILE_HANDLE);
	auto io = std::make_shared<StorageIO>(nullptr, wfd, INVALID_FILE_HANDLE);
	// flushStream should check stream_ and skip if null
	EXPECT_NO_THROW(io->flushStream());
	portable_close_rw(wfd);
}

TEST_F(StorageIOTest, SyncWithNullStreamAndInvalidFd) {
	// Both writeFd_ == INVALID and stream_ == null — tests the else-if false branch
	auto io = std::make_shared<StorageIO>(nullptr, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	EXPECT_NO_THROW(io->sync());
}

// ============================================================================
// Multiple sequential writes and reads at various offsets
// ============================================================================

TEST_F(StorageIOTest, MultipleWriteReadRoundtrip_FstreamFallback) {
	auto io = createIO(false);

	const char data1[] = "first";
	const char data2[] = "second";
	io->writeAt(0, data1, sizeof(data1));
	io->writeAt(512, data2, sizeof(data2));

	char buf1[sizeof(data1)] = {};
	char buf2[sizeof(data2)] = {};
	EXPECT_EQ(io->readAt(0, buf1, sizeof(buf1)), sizeof(data1));
	EXPECT_EQ(io->readAt(512, buf2, sizeof(buf2)), sizeof(data2));
	EXPECT_EQ(std::memcmp(data1, buf1, sizeof(data1)), 0);
	EXPECT_EQ(std::memcmp(data2, buf2, sizeof(data2)), 0);
}
