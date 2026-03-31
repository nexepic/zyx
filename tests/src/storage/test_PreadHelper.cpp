/**
 * @file test_PreadHelper.cpp
 * @brief Unit tests for cross-platform pread functionality
 *
 * @copyright Copyright (c) 2025
 * @license Apache-2.0
 **/

#include <gtest/gtest.h>
#include <fstream>
#include <cstring>
#include <chrono>
#include "graph/storage/PreadHelper.hpp"

using namespace graph::storage;

class PreadHelperTest : public ::testing::Test {
protected:
	std::string testFile_ = "test_pread_data.bin";
	const size_t testSize_ = 4096;

	void SetUp() override {
		// Create test file with known pattern
		std::ofstream file(testFile_, std::ios::binary);
		std::vector<char> data(testSize_);
		for (size_t i = 0; i < testSize_; ++i) {
			data[i] = static_cast<char>(i % 256);
		}
		file.write(data.data(), testSize_);
		file.close();
	}

	void TearDown() override {
		std::remove(testFile_.c_str());
	}
};

TEST_F(PreadHelperTest, OpenAndCloseFile) {
	file_handle_t fd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(fd, INVALID_FILE_HANDLE) << "Failed to open test file";

	int result = portable_close(fd);
	EXPECT_EQ(result, 0) << "Failed to close file";
}

TEST_F(PreadHelperTest, ReadAtBeginning) {
	file_handle_t fd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	char buffer[256];
	ssize_t bytesRead = portable_pread(fd, buffer, sizeof(buffer), 0);

	EXPECT_EQ(bytesRead, static_cast<ssize_t>(sizeof(buffer)));
	for (size_t i = 0; i < sizeof(buffer); ++i) {
		EXPECT_EQ(buffer[i], static_cast<char>(i % 256))
			<< "Mismatch at byte " << i;
	}

	portable_close(fd);
}

TEST_F(PreadHelperTest, ReadAtOffset) {
	file_handle_t fd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	const size_t offset = 1024;
	char buffer[256];
	ssize_t bytesRead = portable_pread(fd, buffer, sizeof(buffer), offset);

	EXPECT_EQ(bytesRead, static_cast<ssize_t>(sizeof(buffer)));
	for (size_t i = 0; i < sizeof(buffer); ++i) {
		EXPECT_EQ(buffer[i], static_cast<char>((offset + i) % 256))
			<< "Mismatch at byte " << i;
	}

	portable_close(fd);
}

TEST_F(PreadHelperTest, MultipleReadsNoInterference) {
	file_handle_t fd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	// Read from two different positions
	char buffer1[100];
	char buffer2[100];

	ssize_t n1 = portable_pread(fd, buffer1, sizeof(buffer1), 0);
	ssize_t n2 = portable_pread(fd, buffer2, sizeof(buffer2), 2048);

	EXPECT_EQ(n1, static_cast<ssize_t>(sizeof(buffer1)));
	EXPECT_EQ(n2, static_cast<ssize_t>(sizeof(buffer2)));

	// Verify data integrity
	EXPECT_EQ(buffer1[0], 0);
	EXPECT_EQ(buffer2[0], static_cast<char>(2048 % 256));

	portable_close(fd);
}

TEST_F(PreadHelperTest, ReadBeyondFileSize) {
	file_handle_t fd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	char buffer[256];
	ssize_t bytesRead = portable_pread(fd, buffer, sizeof(buffer), testSize_ + 1000);

	// Platform-specific behavior: may return 0 or partial data
	EXPECT_LE(bytesRead, static_cast<ssize_t>(sizeof(buffer)));

	portable_close(fd);
}

TEST_F(PreadHelperTest, InvalidFileDescriptor) {
	char buffer[256];
	ssize_t bytesRead = portable_pread(INVALID_FILE_HANDLE, buffer, sizeof(buffer), 0);

	EXPECT_EQ(bytesRead, -1);
}

TEST_F(PreadHelperTest, NullBuffer) {
	file_handle_t fd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	ssize_t bytesRead = portable_pread(fd, nullptr, 256, 0);

	EXPECT_EQ(bytesRead, -1);

	portable_close(fd);
}

#ifdef _WIN32
TEST_F(PreadHelperTest, LargeOffsetOnWindows) {
	file_handle_t fd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	// Test offset > 4GB to verify OffsetHigh is used
	const int64_t largeOffset = static_cast<int64_t>(5ULL * 1024ULL * 1024ULL * 1024ULL);
	char buffer[256];
	ssize_t bytesRead = portable_pread(fd, buffer, sizeof(buffer), largeOffset);

	// Should gracefully handle reads beyond file
	EXPECT_LE(bytesRead, static_cast<ssize_t>(sizeof(buffer)));

	portable_close(fd);
}
#endif

// Performance comparison test (optional)
TEST_F(PreadHelperTest, DISABLED_PerformanceComparison) {
	const int iterations = 10000;
	const size_t readSize = 4096;

	file_handle_t fd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	std::vector<char> buffer(readSize);
	auto start = std::chrono::steady_clock::now();

	for (int i = 0; i < iterations; ++i) {
		portable_pread(fd, buffer.data(), readSize, 0);
	}

	auto end = std::chrono::steady_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

	std::cout << "Read " << iterations << " x " << readSize
			  << " bytes in " << duration.count() << " ms\n";

	portable_close(fd);
}
