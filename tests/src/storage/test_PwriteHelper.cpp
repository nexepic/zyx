/**
 * @file test_PwriteHelper.cpp
 * @brief Unit tests for cross-platform pwrite functionality
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

using namespace graph::storage;

class PwriteHelperTest : public ::testing::Test {
protected:
	std::string testFile_ = "test_pwrite_data.bin";
	const size_t testSize_ = 4096;

	void SetUp() override {
		// Create test file with zeros
		std::ofstream file(testFile_, std::ios::binary);
		std::vector<char> data(testSize_, 0);
		file.write(data.data(), static_cast<std::streamsize>(testSize_));
		file.close();
	}

	void TearDown() override {
		// Use C++ filesystem remove with error_code for best-effort cleanup on Windows
		std::error_code ec;
		std::filesystem::remove(testFile_, ec);
	}
};

TEST_F(PwriteHelperTest, OpenRWAndClose) {
	file_handle_t fd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(fd, INVALID_FILE_HANDLE) << "Failed to open test file for read-write";

	int result = portable_close_rw(fd);
	EXPECT_EQ(result, 0) << "Failed to close file";
}

TEST_F(PwriteHelperTest, WriteAtBeginning) {
	file_handle_t fd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	const char data[] = "Hello, pwrite!";
	auto written = portable_pwrite(fd, data, sizeof(data), 0);
	EXPECT_EQ(written, static_cast<pwrite_ssize_t>(sizeof(data)));

	// Verify via pread
	file_handle_t rfd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(rfd, INVALID_FILE_HANDLE);

	char buffer[sizeof(data)];
	auto bytesRead = portable_pread(rfd, buffer, sizeof(buffer), 0);
	EXPECT_EQ(bytesRead, static_cast<ssize_t>(sizeof(data)));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);

	portable_close(rfd);
	portable_close_rw(fd);
}

TEST_F(PwriteHelperTest, WriteAtOffset) {
	file_handle_t fd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	const off_t offset = 1024;
	const char data[] = "offset_write";
	auto written = portable_pwrite(fd, data, sizeof(data), static_cast<pwrite_off_t>(offset));
	EXPECT_EQ(written, static_cast<pwrite_ssize_t>(sizeof(data)));

	// Verify
	file_handle_t rfd = portable_open(testFile_.c_str(), O_RDONLY);
	char buffer[sizeof(data)];
	portable_pread(rfd, buffer, sizeof(buffer), offset);
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);

	// Verify beginning is still zeros
	char zeroBuf[16] = {};
	portable_pread(rfd, zeroBuf, sizeof(zeroBuf), 0);
	char expected[16] = {};
	EXPECT_EQ(std::memcmp(zeroBuf, expected, sizeof(expected)), 0);

	portable_close(rfd);
	portable_close_rw(fd);
}

TEST_F(PwriteHelperTest, ConcurrentNonOverlappingWrites) {
	file_handle_t fd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	// Write two non-overlapping regions
	const char data1[] = "region_one";
	const char data2[] = "region_two";
	portable_pwrite(fd, data1, sizeof(data1), 0);
	portable_pwrite(fd, data2, sizeof(data2), 2048);

	// Verify both
	file_handle_t rfd = portable_open(testFile_.c_str(), O_RDONLY);
	char buf1[sizeof(data1)], buf2[sizeof(data2)];
	portable_pread(rfd, buf1, sizeof(buf1), 0);
	portable_pread(rfd, buf2, sizeof(buf2), 2048);
	EXPECT_EQ(std::memcmp(data1, buf1, sizeof(data1)), 0);
	EXPECT_EQ(std::memcmp(data2, buf2, sizeof(data2)), 0);

	portable_close(rfd);
	portable_close_rw(fd);
}

TEST_F(PwriteHelperTest, FsyncDoesNotFail) {
	file_handle_t fd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	const char data[] = "fsync_test";
	portable_pwrite(fd, data, sizeof(data), 0);

	int result = portable_fsync(fd);
	EXPECT_EQ(result, 0);

	portable_close_rw(fd);
}

TEST_F(PwriteHelperTest, InvalidFd) {
	auto result = portable_pwrite(INVALID_FILE_HANDLE, "data", 4, 0);
	EXPECT_EQ(result, -1);

	EXPECT_EQ(portable_fsync(INVALID_FILE_HANDLE), -1);
	EXPECT_EQ(portable_close_rw(INVALID_FILE_HANDLE), -1);
}

TEST_F(PwriteHelperTest, TruncateFile) {
	file_handle_t fd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	// Write some data first
	const char data[] = "truncate_test_data";
	portable_pwrite(fd, data, sizeof(data), 0);

	// Truncate to a smaller size
	constexpr uint64_t newSize = 512;
	int result = portable_ftruncate(fd, newSize);
	EXPECT_EQ(result, 0);

	portable_close_rw(fd);

	// Verify file size via filesystem
	auto fileSize = std::filesystem::file_size(testFile_);
	EXPECT_EQ(fileSize, newSize);
}

TEST_F(PwriteHelperTest, TruncateInvalidFd) {
	int result = portable_ftruncate(INVALID_FILE_HANDLE, 100);
	EXPECT_EQ(result, -1);
}

TEST_F(PwriteHelperTest, OpenRWNullPath) {
	file_handle_t fd = portable_open_rw(nullptr);
	EXPECT_EQ(fd, INVALID_FILE_HANDLE);
}

// Covers: portable_pwrite with null buffer returns -1
TEST_F(PwriteHelperTest, PwriteNullBuffer) {
	file_handle_t fd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	auto result = portable_pwrite(fd, nullptr, 10, 0);
	EXPECT_EQ(result, -1);

	portable_close_rw(fd);
}

// Covers: portable_pwrite with count=0 returns -1
TEST_F(PwriteHelperTest, PwriteZeroCount) {
	file_handle_t fd = portable_open_rw(testFile_.c_str());
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	const char data[] = "test";
	auto result = portable_pwrite(fd, data, 0, 0);
	EXPECT_EQ(result, -1);

	portable_close_rw(fd);
}
