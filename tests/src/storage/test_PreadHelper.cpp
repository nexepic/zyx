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
#include <filesystem>
#include "graph/storage/PreadHelper.hpp"
#ifdef _WIN32
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/detail/WindowsLeaseReadBridge.hpp"
#include <atomic>
#include <thread>
#endif

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
		// Use C++ filesystem remove with error_code for best-effort cleanup on Windows
		std::error_code ec;
		std::filesystem::remove(testFile_, ec);
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

TEST_F(PreadHelperTest, CloseInvalidHandle) {
	EXPECT_EQ(portable_close(INVALID_FILE_HANDLE), -1);
}

TEST_F(PreadHelperTest, NullBuffer) {
	file_handle_t fd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	ssize_t bytesRead = portable_pread(fd, nullptr, 256, 0);

	EXPECT_EQ(bytesRead, -1);

	portable_close(fd);
}

TEST_F(PreadHelperTest, ZeroCountReadReturnsError) {
	file_handle_t fd = portable_open(testFile_.c_str(), O_RDONLY);
	ASSERT_NE(fd, INVALID_FILE_HANDLE);

	char buffer[16];
	ssize_t bytesRead = portable_pread(fd, buffer, 0, 0);
	EXPECT_EQ(bytesRead, -1);

	portable_close(fd);
}

#ifdef _WIN32
TEST_F(PreadHelperTest, ReadOnlyStorageBorrowsLeaseWithoutEscapingWriterHandle) {
    struct Handles {
        file_handle_t writer = INVALID_FILE_HANDLE;
        file_handle_t reader = INVALID_FILE_HANDLE;
        ~Handles() {
            if (reader != INVALID_FILE_HANDLE) portable_close(reader);
            if (writer != INVALID_FILE_HANDLE) portable_close_rw(writer);
        }
    } handles;
    // Open the read-only handle first, then independently open the writer.
    handles.reader = portable_open(testFile_.c_str(), O_RDONLY);
    ASSERT_NE(handles.reader, INVALID_FILE_HANDLE);
    handles.writer = portable_open_rw(testFile_.c_str());
    ASSERT_NE(handles.writer, INVALID_FILE_HANDLE);
    StorageIO io(nullptr, INVALID_FILE_HANDLE, handles.reader, StorageIOAccessMode::SIOAM_READ_ONLY, uint64_t{1} << 30U);
    char before = 0, during = 0, after = 0;
    ASSERT_EQ(io.readAt(0U, &before, 1U), 1U);
    const file_handle_t token = detail::WindowsLeaseReadBridge::tryLock(handles.writer);
    ASSERT_NE(token, INVALID_FILE_HANDLE);
    struct Lease {
        file_handle_t handle;
        bool held = true;
        ~Lease() { if (held) detail::WindowsLeaseReadBridge::unlock(handle); }
    } lease{token};
    EXPECT_EQ(io.readAt(0U, &during, 1U), 1U);
    EXPECT_EQ(before, during);
    std::atomic<bool> failed{false};
    std::thread reader([&] {
        try {
            for (size_t i = 0; i < 100U; ++i) {
                char value = 0;
                if (io.readAt(0U, &value, 1U) != 1U || value != before) failed = true;
            }
        } catch (...) { failed = true; }
    });
    const int closeResult = portable_close_rw(handles.writer);
    EXPECT_EQ(closeResult, 0);
    if (closeResult == 0) handles.writer = INVALID_FILE_HANDLE;
    const file_handle_t contender = portable_open_rw(testFile_.c_str());
    EXPECT_NE(contender, INVALID_FILE_HANDLE);
    if (contender != INVALID_FILE_HANDLE) {
        OVERLAPPED overlap = {};
        const BOOL acquired = ::LockFileEx(reinterpret_cast<HANDLE>(contender),
            LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0U, 1U, 0U, &overlap);
        const DWORD error = acquired ? ERROR_SUCCESS : ::GetLastError();
        EXPECT_FALSE(acquired);
        if (!acquired) EXPECT_EQ(error, ERROR_LOCK_VIOLATION);
        else ::UnlockFileEx(reinterpret_cast<HANDLE>(contender), 0U, 1U, 0U, &overlap);
        portable_close_rw(contender);
    }
    detail::WindowsLeaseReadBridge::unlock(token);
    lease.held = false;
    reader.join();
    EXPECT_FALSE(failed.load());
    EXPECT_EQ(io.readAt(0U, &after, 1U), 1U);
    EXPECT_EQ(before, after);
}
#endif

#ifdef _WIN32
TEST_F(PreadHelperTest, ReadOnlyLeaseBorrowResolvesHardLinkIdentity) {
    const std::string alias = testFile_ + ".hardlink";
    struct RemoveAlias { std::string path; ~RemoveAlias() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{alias};
    ASSERT_TRUE(::CreateHardLinkA(alias.c_str(), testFile_.c_str(), nullptr));
    struct Handles {
        file_handle_t writer = INVALID_FILE_HANDLE, reader = INVALID_FILE_HANDLE;
        ~Handles() {
            if (reader != INVALID_FILE_HANDLE) portable_close(reader);
            if (writer != INVALID_FILE_HANDLE) portable_close_rw(writer);
        }
    } handles;
    handles.reader = portable_open(alias.c_str(), O_RDONLY);
    handles.writer = portable_open_rw(testFile_.c_str());
    ASSERT_NE(handles.reader, INVALID_FILE_HANDLE);
    ASSERT_NE(handles.writer, INVALID_FILE_HANDLE);
    const file_handle_t token = detail::WindowsLeaseReadBridge::tryLock(handles.writer);
    ASSERT_NE(token, INVALID_FILE_HANDLE);
    struct Lease { file_handle_t handle; ~Lease() { detail::WindowsLeaseReadBridge::unlock(handle); } } lease{token};
    StorageIO io(nullptr, INVALID_FILE_HANDLE, handles.reader, StorageIOAccessMode::SIOAM_READ_ONLY, uint64_t{1} << 30U);
    char value = 0;
    EXPECT_EQ(io.readAt(0U, &value, 1U), 1U);
}
#endif

#ifdef _WIN32
TEST_F(PreadHelperTest, WindowsExclusiveByteLockUsesTheLockingHandle) {
    struct Handles {
        file_handle_t writer = INVALID_FILE_HANDLE;
        file_handle_t reader = INVALID_FILE_HANDLE;
        ~Handles() {
            if (reader != INVALID_FILE_HANDLE) portable_close(reader);
            if (writer != INVALID_FILE_HANDLE) portable_close_rw(writer);
        }
    } handles;
    handles.writer = portable_open_rw(testFile_.c_str());
    ASSERT_NE(handles.writer, INVALID_FILE_HANDLE);
    handles.reader = portable_open(testFile_.c_str(), O_RDONLY);
    ASSERT_NE(handles.reader, INVALID_FILE_HANDLE);
    const file_handle_t writer = handles.writer, reader = handles.reader;
    OVERLAPPED lock = {};
    ASSERT_TRUE(::LockFileEx(reinterpret_cast<HANDLE>(writer),
                             LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                             0U, 1U, 0U, &lock));
    char value = 0;
    EXPECT_EQ(portable_pread(writer, &value, 1U, 0), 1);
    EXPECT_EQ(portable_pread(reader, &value, 1U, 0), -1);
    EXPECT_EQ(::GetLastError(), ERROR_LOCK_VIOLATION);
    EXPECT_TRUE(::UnlockFileEx(reinterpret_cast<HANDLE>(writer), 0U, 1U, 0U, &lock));
    EXPECT_EQ(portable_pread(reader, &value, 1U, 0), 1);
}
#endif

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
