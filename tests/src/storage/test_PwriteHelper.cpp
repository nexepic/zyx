/**
 * @file test_PwriteHelper.cpp
 * @brief Unit tests for cross-platform pwrite functionality
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <gtest/gtest.h>
#include <cstring>
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
		std::remove(testFile_.c_str());
	}
};

TEST_F(PwriteHelperTest, OpenRWAndClose) {
	int fd = portable_open_rw(testFile_.c_str());
	ASSERT_GE(fd, 0) << "Failed to open test file for read-write";

	int result = portable_close_rw(fd);
	EXPECT_EQ(result, 0) << "Failed to close file";
}

TEST_F(PwriteHelperTest, WriteAtBeginning) {
	int fd = portable_open_rw(testFile_.c_str());
	ASSERT_GE(fd, 0);

	const char data[] = "Hello, pwrite!";
	auto written = portable_pwrite(fd, data, sizeof(data), 0);
	EXPECT_EQ(written, static_cast<pwrite_ssize_t>(sizeof(data)));

	// Verify via pread
	int rfd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_GE(rfd, 0);

	char buffer[sizeof(data)];
	auto bytesRead = portable_pread(rfd, buffer, sizeof(buffer), 0);
	EXPECT_EQ(bytesRead, static_cast<ssize_t>(sizeof(data)));
	EXPECT_EQ(std::memcmp(data, buffer, sizeof(data)), 0);

	portable_close(rfd);
	portable_close_rw(fd);
}

TEST_F(PwriteHelperTest, WriteAtOffset) {
	int fd = portable_open_rw(testFile_.c_str());
	ASSERT_GE(fd, 0);

	const off_t offset = 1024;
	const char data[] = "offset_write";
	auto written = portable_pwrite(fd, data, sizeof(data), static_cast<pwrite_off_t>(offset));
	EXPECT_EQ(written, static_cast<pwrite_ssize_t>(sizeof(data)));

	// Verify
	int rfd = portable_open(testFile_.c_str(), O_RDONLY);
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
	int fd = portable_open_rw(testFile_.c_str());
	ASSERT_GE(fd, 0);

	// Write two non-overlapping regions
	const char data1[] = "region_one";
	const char data2[] = "region_two";
	portable_pwrite(fd, data1, sizeof(data1), 0);
	portable_pwrite(fd, data2, sizeof(data2), 2048);

	// Verify both
	int rfd = portable_open(testFile_.c_str(), O_RDONLY);
	char buf1[sizeof(data1)], buf2[sizeof(data2)];
	portable_pread(rfd, buf1, sizeof(buf1), 0);
	portable_pread(rfd, buf2, sizeof(buf2), 2048);
	EXPECT_EQ(std::memcmp(data1, buf1, sizeof(data1)), 0);
	EXPECT_EQ(std::memcmp(data2, buf2, sizeof(data2)), 0);

	portable_close(rfd);
	portable_close_rw(fd);
}

TEST_F(PwriteHelperTest, FsyncDoesNotFail) {
	int fd = portable_open_rw(testFile_.c_str());
	ASSERT_GE(fd, 0);

	const char data[] = "fsync_test";
	portable_pwrite(fd, data, sizeof(data), 0);

	int result = portable_fsync(fd);
	EXPECT_EQ(result, 0);

	portable_close_rw(fd);
}

TEST_F(PwriteHelperTest, InvalidFd) {
	auto result = portable_pwrite(-1, "data", 4, 0);
	EXPECT_EQ(result, -1);

	EXPECT_EQ(portable_fsync(-1), -1);
	EXPECT_EQ(portable_close_rw(-1), -1);
}
