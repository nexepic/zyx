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
#include "PwriteHelper.hpp" // for file_handle_t, INVALID_FILE_HANDLE

#ifdef _WIN32
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <windows.h>
	#include <fcntl.h>   // O_RDONLY
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
#endif

/**
 * @brief Cross-platform file open function (read-only)
 *
 * Opens a file for reading in a platform-independent manner.
 * On Unix/Linux, uses standard open() system call.
 * On Windows, uses CreateFile() with read-only access.
 *
 * @param filePath Path to the file
 * @param flags Open flags (O_RDONLY on POSIX; ignored on Windows)
 * @return Native file handle, or INVALID_FILE_HANDLE on error
 */
inline file_handle_t portable_open(const char* filePath, int flags) {
#ifdef _WIN32
	if (!filePath) return INVALID_FILE_HANDLE;
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
		return INVALID_FILE_HANDLE;
	}

	// intptr_t can hold a HANDLE on both 32-bit and 64-bit Windows
	return reinterpret_cast<file_handle_t>(hFile);
#else
	return ::open(filePath, flags);
#endif
}

/**
 * @brief Cross-platform file close function
 *
 * Closes a file handle opened by portable_open().
 *
 * @param fd File handle to close
 * @return 0 on success, -1 on error
 */
inline int portable_close(file_handle_t fd) {
	if (fd == INVALID_FILE_HANDLE) return -1;

#ifdef _WIN32
	HANDLE hFile = reinterpret_cast<HANDLE>(fd);
	return ::CloseHandle(hFile) ? 0 : -1;
#else
	return ::close(fd);
#endif
}

/**
 * @brief Cross-platform pread wrapper function
 *
 * Performs an atomic position-independent read from a file.
 * This function does not affect the file position indicator,
 * allowing multiple threads to read concurrently without synchronization.
 *
 * @param fd File handle from portable_open()
 * @param buf Buffer to read data into
 * @param count Number of bytes to read
 * @param offset File offset to read from
 * @return Number of bytes read, or -1 on error
 */
inline ssize_t portable_pread(file_handle_t fd, void* buf, size_t count, int64_t offset) {
	if (fd == INVALID_FILE_HANDLE || !buf || count == 0) {
		return -1;
	}

#ifdef _WIN32
	HANDLE hFile = reinterpret_cast<HANDLE>(fd);

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
#else
	return ::pread(fd, buf, count, static_cast<off_t>(offset));
#endif
}

} // namespace graph::storage
