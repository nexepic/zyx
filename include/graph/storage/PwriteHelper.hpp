/**
 * @file PwriteHelper.hpp
 * @brief Cross-platform pwrite() wrapper for position-independent file writes
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>

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
 * @brief Cross-platform file handle type.
 *
 * On Windows, HANDLE is void* (8 bytes on 64-bit). Using intptr_t avoids
 * the undefined-behavior truncation that static_cast<int>(...) would cause
 * when a HANDLE value exceeds INT_MAX.
 * On POSIX, file descriptors are plain int.
 */
#ifdef _WIN32
using file_handle_t = intptr_t;
using pwrite_ssize_t = intptr_t;
using pwrite_off_t = int64_t;
#else
using file_handle_t = int;
using pwrite_ssize_t = ssize_t;
using pwrite_off_t = off_t;
#endif

/// Sentinel value: invalid / not-yet-opened handle.
/// On Windows this matches INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
/// after reinterpret_cast<intptr_t>; on POSIX it matches the -1
/// returned by open() on failure.
constexpr file_handle_t INVALID_FILE_HANDLE = -1;

/**
 * @brief Cross-platform file open for read-write access
 */
inline file_handle_t portable_open_rw(const char* filePath) {
	if (!filePath) return INVALID_FILE_HANDLE;

#ifdef _WIN32
	HANDLE hFile = ::CreateFileA(
		filePath,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);
	if (hFile == INVALID_HANDLE_VALUE) return INVALID_FILE_HANDLE;
	// intptr_t can hold a HANDLE on both 32-bit and 64-bit Windows
	return reinterpret_cast<file_handle_t>(hFile);
#else
	return ::open(filePath, O_RDWR);
#endif
}

/**
 * @brief Cross-platform pwrite wrapper
 *
 * Performs an atomic position-independent write to a file.
 * Thread-safe: multiple threads can call concurrently on the same fd
 * as long as they write to non-overlapping regions.
 */
inline pwrite_ssize_t portable_pwrite(file_handle_t fd, const void* buf, size_t count, pwrite_off_t offset) {
	if (fd == INVALID_FILE_HANDLE || !buf || count == 0) return -1;

#ifdef _WIN32
	HANDLE hFile = reinterpret_cast<HANDLE>(fd);
	OVERLAPPED overlapped = {};
	overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
	overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFF);
	DWORD bytesWritten = 0;
	BOOL result = ::WriteFile(hFile, buf, static_cast<DWORD>(count), &bytesWritten, &overlapped);
	return result ? static_cast<pwrite_ssize_t>(bytesWritten) : -1;
#else
	return ::pwrite(fd, buf, count, offset);
#endif
}

/**
 * @brief Cross-platform fsync/fdatasync wrapper
 */
inline int portable_fsync(file_handle_t fd) {
	if (fd == INVALID_FILE_HANDLE) return -1;

#ifdef _WIN32
	HANDLE hFile = reinterpret_cast<HANDLE>(fd);
	return ::FlushFileBuffers(hFile) ? 0 : -1;
#else
#ifdef __APPLE__
	return ::fcntl(fd, F_FULLFSYNC);
#else
	return ::fdatasync(fd);
#endif
#endif
}

/**
 * @brief Close a file descriptor opened by portable_open_rw
 */
inline int portable_close_rw(file_handle_t fd) {
	if (fd == INVALID_FILE_HANDLE) return -1;

#ifdef _WIN32
	HANDLE hFile = reinterpret_cast<HANDLE>(fd);
	return ::CloseHandle(hFile) ? 0 : -1;
#else
	return ::close(fd);
#endif
}

/**
 * @brief Cross-platform file truncation
 *
 * Truncates (or extends) the file to exactly @p newSize bytes.
 * The file handle remains open and valid after the call.
 *
 * @param fd  Handle previously returned by portable_open_rw()
 * @param newSize  Desired file size in bytes
 * @return 0 on success, -1 on error
 */
inline int portable_ftruncate(file_handle_t fd, uint64_t newSize) {
	if (fd == INVALID_FILE_HANDLE) return -1;

#ifdef _WIN32
	HANDLE hFile = reinterpret_cast<HANDLE>(fd);
	LARGE_INTEGER li;
	li.QuadPart = static_cast<LONGLONG>(newSize);
	if (!::SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN)) return -1;
	return ::SetEndOfFile(hFile) ? 0 : -1;
#else
	return ::ftruncate(fd, static_cast<off_t>(newSize));
#endif
}

} // namespace graph::storage
