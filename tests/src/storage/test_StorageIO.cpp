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
