/**
 * @file PreadHelper.hpp
 * @brief Cross-platform pread() wrapper for position-independent file reads
 *
 * @copyright Copyright (c) 2025
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <string>

#ifdef _WIN32
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <windows.h>
#else
	#include <fcntl.h>
	#include <unistd.h>
#endif

namespace graph::storage {

/**
 * @brief Platform-specific type definitions
 */
#ifdef _WIN32
	// Windows doesn't have ssize_t, define it
	using ssize_t = intptr_t;
	// Use int64_t directly instead of redefining off_t to avoid conflicts with Windows SDK
	// Windows SDK defines off_t as long (32-bit) in sys/types.h
	// Constants for Windows compatibility
	constexpr int O_RDONLY = 0x0000;
#endif

/**
 * @brief Cross-platform file open function
 *
 * Opens a file for reading in a platform-independent manner.
 * On Unix/Linux, uses standard open() system call.
 * On Windows, uses CreateFile() with read-only access.
 *
 * @param filePath Path to the file
 * @return File descriptor (Unix) or HANDLE cast to int (Windows), or -1 on error
 */
int portable_open(const char* filePath, int flags);

/**
 * @brief Cross-platform file close function
 *
 * Closes a file descriptor opened by portable_open().
 *
 * @param fd File descriptor to close
 * @return 0 on success, -1 on error
 */
int portable_close(int fd);

/**
 * @brief Cross-platform pread wrapper function
 *
 * Performs an atomic position-independent read from a file.
 * This function does not affect the file position indicator,
 * allowing multiple threads to read concurrently without synchronization.
 *
 * Platform behavior:
 * - Unix/Linux: Uses native pread() system call
 * - Windows: Uses ReadFile() with OVERLAPPED structure
 *
 * @param fd File descriptor/handle
 *   - Unix/Linux: File descriptor (int)
 *   - Windows: HANDLE (cast to int)
 * @param buf Buffer to read data into
 * @param count Number of bytes to read
 * @param offset File offset to read from (use int64_t to avoid Windows SDK conflicts)
 * @return Number of bytes read, or -1 on error
 */
ssize_t portable_pread(int fd, void* buf, size_t count, int64_t offset);

// ============================================================================
// Platform-specific implementations
// ============================================================================

#ifdef _WIN32

inline int portable_open(const char* filePath, int flags) {
	if (!filePath) return -1;
	(void)flags;  // Unused on Windows, always opens read-only

	DWORD desiredAccess = GENERIC_READ;
	DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
	DWORD creationDisposition = OPEN_EXISTING;
	DWORD flagsAndAttributes = FILE_ATTRIBUTE_NORMAL;

	HANDLE hFile = ::CreateFileA(
		filePath,
		desiredAccess,
		shareMode,
		nullptr,
		creationDisposition,
		flagsAndAttributes,
		nullptr
	);

	if (hFile == INVALID_HANDLE_VALUE) {
		return -1;
	}

	// Cast HANDLE to int for storage (safe for x64)
	return static_cast<int>(reinterpret_cast<uintptr_t>(hFile));
}

inline int portable_close(int fd) {
	if (fd < 0) return -1;

	HANDLE hFile = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fd));
	return ::CloseHandle(hFile) ? 0 : -1;
}

inline ssize_t portable_pread(int fd, void* buf, size_t count, int64_t offset) {
	if (fd < 0 || !buf || count == 0) {
		return -1;
	}

	HANDLE hFile = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fd));

	// OVERLAPPED structure for position-independent read
	OVERLAPPED overlapped = {};
	overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
	overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);

	DWORD bytesRead = 0;
	BOOL result = ::ReadFile(
		hFile,
		buf,
		static_cast<DWORD>(count),
		&bytesRead,
		&overlapped
	);

	if (!result) {
		return -1;
	}

	return static_cast<ssize_t>(bytesRead);
}

#else // Unix/Linux

inline int portable_open(const char* filePath, int flags) {
	return ::open(filePath, flags);
}

inline int portable_close(int fd) {
	return ::close(fd);
}

inline ssize_t portable_pread(int fd, void* buf, size_t count, int64_t offset) {
	if (fd < 0 || !buf || count == 0) {
		return -1;
	}

	return ::pread(fd, buf, count, static_cast<off_t>(offset));
}

#endif

} // namespace graph::storage
