/**
 * @file StorageIO.hpp
 * @brief Unified cross-platform I/O abstraction for position-independent file operations
 *
 * Wraps pwrite/pread (native fd) with fstream fallback into a single interface,
 * eliminating scattered if/else branching throughout the storage layer.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include "PwriteHelper.hpp"

namespace graph::storage {

	class StorageIO {
	public:
		/**
		 * @param stream   Shared fstream (fallback for platforms without pwrite/pread)
		 * @param writeFd  Native file handle for pwrite (INVALID_FILE_HANDLE to use fstream)
		 * @param readFd   Native file handle for pread  (INVALID_FILE_HANDLE to use fstream)
		 */
		StorageIO(std::shared_ptr<std::fstream> stream, file_handle_t writeFd, file_handle_t readFd);
		~StorageIO() = default;

		// Non-copyable, non-movable (shared ownership via shared_ptr)
		StorageIO(const StorageIO &) = delete;
		StorageIO &operator=(const StorageIO &) = delete;

		/**
		 * @brief Position-independent write.
		 *
		 * Thread-safe when writing to non-overlapping regions (uses pwrite).
		 * Falls back to fstream seekp+write when pwrite is unavailable.
		 */
		void writeAt(uint64_t offset, const void *buf, size_t size);

		/**
		 * @brief Position-independent read.
		 *
		 * Thread-safe (uses pread). Falls back to fstream seekg+read.
		 * @return Number of bytes actually read.
		 */
		size_t readAt(uint64_t offset, void *buf, size_t size) const;

		/**
		 * @brief Append data to the end of the file.
		 *
		 * NOT thread-safe — caller must serialize append calls.
		 * @return File offset where the data was written.
		 */
		uint64_t append(const void *buf, size_t size);

		/**
		 * @brief Sync all pending writes to durable storage.
		 */
		void sync();

		/**
		 * @brief Flush the fstream internal buffer.
		 *
		 * Required before pread can see data written via fstream,
		 * or before pwrite data is visible to fstream reads.
		 */
		void flushStream();

		[[nodiscard]] bool hasPwriteSupport() const { return writeFd_ != INVALID_FILE_HANDLE; }
		[[nodiscard]] bool hasPreadSupport() const { return readFd_ != INVALID_FILE_HANDLE; }

		/**
		 * @brief Access the underlying fstream.
		 *
		 * For legacy code paths that still need direct stream access
		 * (e.g., SegmentTracker chain loading during initialization).
		 */
		[[nodiscard]] std::shared_ptr<std::fstream> getStream() const { return stream_; }

		[[nodiscard]] file_handle_t getWriteFd() const { return writeFd_; }
		[[nodiscard]] file_handle_t getReadFd() const { return readFd_; }

	private:
		std::shared_ptr<std::fstream> stream_;
		file_handle_t writeFd_;
		file_handle_t readFd_;
	};

} // namespace graph::storage
