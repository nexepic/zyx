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

#ifdef _WIN32
using pwrite_ssize_t = intptr_t;
using pwrite_off_t = int64_t;
#else
using pwrite_ssize_t = ssize_t;
using pwrite_off_t = off_t;
#endif

/**
 * @brief Cross-platform file open for read-write access
 */
inline int portable_open_rw(const char* filePath) {
	if (!filePath) return -1;

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
	if (hFile == INVALID_HANDLE_VALUE) return -1;
	// Cast HANDLE (void*) to int using pointer-to-int conversion
	// On 64-bit Windows, this truncates from 64-bit to 32-bit, which is safe
	// as we're just storing a handle identifier
	return static_cast<int>(reinterpret_cast<uintptr_t>(hFile));
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
inline pwrite_ssize_t portable_pwrite(int fd, const void* buf, size_t count, pwrite_off_t offset) {
	if (fd < 0 || !buf || count == 0) return -1;

#ifdef _WIN32
	// Cast int back to HANDLE
	HANDLE hFile = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fd));
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
inline int portable_fsync(int fd) {
	if (fd < 0) return -1;

#ifdef _WIN32
	HANDLE hFile = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fd));
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
inline int portable_close_rw(int fd) {
	if (fd < 0) return -1;

#ifdef _WIN32
	HANDLE hFile = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fd));
	return ::CloseHandle(hFile) ? 0 : -1;
#else
	return ::close(fd);
#endif
}

} // namespace graph::storage
