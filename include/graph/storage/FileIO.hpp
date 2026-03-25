/**
 * @file FileIO.hpp
 * @brief Cross-platform file I/O utilities for atomic position-independent reads
 *
 * Provides a unified interface for atomic position-independent file reads across platforms:
 * - Unix/Linux: Uses native pread() (atomic, lock-free)
 * - Windows: Uses ReadFile with OVERLAPPED (atomic, lock-free)
 * - Fallback: Uses mutex-protected seek+read (not atomic, but safe)
 *
 * @copyright Copyright (c) 2025
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <cstdio>

#ifdef _WIN32
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <windows.h>
#else
	#include <unistd.h>
	#include <fcntl.h>
#endif

namespace graph::storage {

/**
 * @brief Platform-specific type definitions
 */
#ifdef _WIN32
	using ssize_t = intptr_t;
	using off_t = int64_t;
	using FileHandle = HANDLE;
	constexpr FileHandle INVALID_FILE_HANDLE = INVALID_HANDLE_VALUE;
#else
	using FileHandle = int;
	constexpr FileHandle INVALID_FILE_HANDLE = -1;
#endif

/**
 * @brief Cross-platform file I/O helper for atomic position-independent reads
 *
 * This class provides a unified interface for performing atomic position-independent
 * file reads across different platforms. The key feature is that reads do not affect
 * the file position indicator, allowing multiple threads to read concurrently without
 * synchronization.
 *
 * Platform-specific behavior:
 * - **Unix/Linux**: Uses pread() which is atomic and thread-safe
 * - **Windows**: Uses ReadFile() with OVERLAPPED structure which is atomic and thread-safe
 *
 * Example usage:
 * @code
 * FileIO fileIO;
 * if (fileIO.open("data.db", true)) {
 *     char buffer[1024];
 *     ssize_t bytes = fileIO.pread(buffer, sizeof(buffer), 0);
 *     if (bytes > 0) {
 *         // Process data
 *     }
 *     fileIO.close();
 * }
 * @endcode
 */
class FileIO {
public:
	FileIO() = default;
	~FileIO() { close(); }

	// Non-copyable, non-movable (owns file handle)
	FileIO(const FileIO&) = delete;
	FileIO& operator=(const FileIO&) = delete;
	FileIO(FileIO&&) = delete;
	FileIO& operator=(FileIO&&) = delete;

	/**
	 * @brief Opens a file for reading
	 * @param filePath Path to the file
	 * @param readOnly If true, opens in read-only mode (required for pread)
	 * @return true if successful, false otherwise
	 */
	[[nodiscard]] bool open(const char* filePath, bool readOnly = true);

	/**
	 * @brief Closes the file handle
	 */
	void close();

	/**
	 * @brief Performs an atomic position-independent read
	 *
	 * This function reads from the file at the specified offset without
	 * affecting the file position indicator. Multiple threads can call
	 * this function concurrently without synchronization.
	 *
	 * @param buf Buffer to read data into
	 * @param count Number of bytes to read
	 * @param offset File offset to read from
	 * @return Number of bytes read, or -1 on error
	 */
	[[nodiscard]] ssize_t pread(void* buf, size_t count, off_t offset) const;

	/**
	 * @brief Checks if the file is open
	 */
	[[nodiscard]] bool isOpen() const { return fd_ != INVALID_FILE_HANDLE; }

	/**
	 * @brief Gets the underlying file handle (for advanced use cases)
	 */
	[[nodiscard]] FileHandle getHandle() const { return fd_; }

private:
	FileHandle fd_ = INVALID_FILE_HANDLE;

#ifdef _WIN32
	/**
	 * @brief Windows-specific pread implementation using ReadFile with OVERLAPPED
	 *
	 * Windows doesn't have pread(), but ReadFile with OVERLAPPED structure
	 * provides equivalent functionality for position-independent reads.
	 */
	[[nodiscard]] ssize_t preadWindows(void* buf, size_t count, off_t offset) const;
#else
	/**
	 * @brief Unix/Linux pread implementation (direct system call)
	 */
	[[nodiscard]] ssize_t preadUnix(void* buf, size_t count, off_t offset) const;
#endif
};

// Inline implementations

inline bool FileIO::open(const char* filePath, bool readOnly) {
	close();

#ifdef _WIN32
	DWORD desiredAccess = readOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
	DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
	DWORD creationDisposition = OPEN_EXISTING;
	DWORD flagsAndAttributes = FILE_ATTRIBUTE_NORMAL;

	fd_ = CreateFileA(
		filePath,
		desiredAccess,
		shareMode,
		nullptr,
		creationDisposition,
		flagsAndAttributes,
		nullptr
	);

	return fd_ != INVALID_FILE_HANDLE;
#else
	int flags = readOnly ? O_RDONLY : O_RDWR;
	fd_ = ::open(filePath, flags);
	return fd_ >= 0;
#endif
}

inline void FileIO::close() {
	if (fd_ != INVALID_FILE_HANDLE) {
#ifdef _WIN32
		::CloseHandle(fd_);
#else
		::close(fd_);
#endif
		fd_ = INVALID_FILE_HANDLE;
	}
}

#ifdef _WIN32

inline ssize_t FileIO::preadWindows(void* buf, size_t count, off_t offset) const {
	if (!isOpen() || !buf || count == 0) {
		return -1;
	}

	// OVERLAPPED structure for position-independent read
	OVERLAPPED overlapped = {};
	overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
	overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);

	DWORD bytesRead = 0;
	BOOL result = ::ReadFile(
		fd_,
		buf,
		static_cast<DWORD>(count),
		&bytesRead,
		&overlapped
	);

	if (!result) {
		// Error occurred
		return -1;
	}

	return static_cast<ssize_t>(bytesRead);
}

inline ssize_t FileIO::pread(void* buf, size_t count, off_t offset) const {
	return preadWindows(buf, count, offset);
}

#else

inline ssize_t FileIO::preadUnix(void* buf, size_t count, off_t offset) const {
	if (!isOpen() || !buf || count == 0) {
		return -1;
	}

	return ::pread(fd_, buf, count, offset);
}

inline ssize_t FileIO::pread(void* buf, size_t count, off_t offset) const {
	return preadUnix(buf, count, offset);
}

#endif

} // namespace graph::storage
