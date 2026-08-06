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

#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include "PwriteHelper.hpp"
#include "graph/storage/relocatable/cache/BufferPoolTypes.hpp"

namespace graph::storage {
	class BufferPool;

	// The capability is explicit because a readable fstream does not expose
	// whether it was opened with std::ios::out.  This keeps a read-only cache
	// from deferring an invalid write until its teardown flush.
	enum class StorageIOAccessMode : uint8_t {
		SIOAM_READ_WRITE = 1,
		SIOAM_READ_ONLY = 2,
	};

	struct StorageIOStats {
		uint64_t read_bytes = 0U;
		uint64_t write_bytes = 0U;
		uint64_t reserved_bytes = 0U;
		uint64_t read_calls = 0U;
		uint64_t write_calls = 0U;
		uint64_t reserve_calls = 0U;
		uint64_t sync_calls = 0U;

		constexpr bool operator==(const StorageIOStats &) const = default;
	};

	class StorageIO {
	public:
		/**
		 * @param stream   Shared fstream (fallback for platforms without pwrite/pread)
		 * @param writeFd  Native file handle for pwrite (INVALID_FILE_HANDLE to use fstream)
		 * @param readFd   Native file handle for pread  (INVALID_FILE_HANDLE to use fstream)
		 */
		// Retain the established writable construction form. This also keeps an
		// incremental rebuild from linking a preexisting caller against a removed
		// constructor symbol when the explicit access-mode overload was introduced.
		StorageIO(std::shared_ptr<std::fstream> stream, file_handle_t writeFd, file_handle_t readFd);
		StorageIO(std::shared_ptr<std::fstream> stream, file_handle_t writeFd, file_handle_t readFd,
				  StorageIOAccessMode accessMode);
		~StorageIO() noexcept;

		// Non-copyable, non-movable (shared ownership via shared_ptr)
		StorageIO(const StorageIO &) = delete;
		StorageIO &operator=(const StorageIO &) = delete;

#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
		// This hook exists only in test-instrumented builds. Production builds keep
		// the native write path direct and free of fault-injection dispatch.
		using NativeWriteHookForTesting =
				std::function<pwrite_ssize_t(file_handle_t, const void *, size_t, pwrite_off_t)>;
		using NativeReadHookForTesting = std::function<pwrite_ssize_t(file_handle_t, void *, size_t, pwrite_off_t)>;
		using NativeSyncHookForTesting = std::function<int(file_handle_t)>;
		using NativeTruncateHookForTesting = std::function<int(file_handle_t, uint64_t)>;

		class ScopedNativeWriteHookForTesting final {
		public:
			explicit ScopedNativeWriteHookForTesting(NativeWriteHookForTesting hook);
			~ScopedNativeWriteHookForTesting();

			ScopedNativeWriteHookForTesting(const ScopedNativeWriteHookForTesting &) = delete;
			ScopedNativeWriteHookForTesting &operator=(const ScopedNativeWriteHookForTesting &) = delete;
		};

		class ScopedNativeSyncHookForTesting final {
		public:
			explicit ScopedNativeSyncHookForTesting(NativeSyncHookForTesting hook);
			~ScopedNativeSyncHookForTesting();

			ScopedNativeSyncHookForTesting(const ScopedNativeSyncHookForTesting &) = delete;
			ScopedNativeSyncHookForTesting &operator=(const ScopedNativeSyncHookForTesting &) = delete;
		};

		class ScopedNativeReadHookForTesting final {
		public:
			explicit ScopedNativeReadHookForTesting(NativeReadHookForTesting hook);
			~ScopedNativeReadHookForTesting();

			ScopedNativeReadHookForTesting(const ScopedNativeReadHookForTesting &) = delete;
			ScopedNativeReadHookForTesting &operator=(const ScopedNativeReadHookForTesting &) = delete;
		};

		class ScopedNativeTruncateHookForTesting final {
		public:
			explicit ScopedNativeTruncateHookForTesting(NativeTruncateHookForTesting hook);
			~ScopedNativeTruncateHookForTesting();

			ScopedNativeTruncateHookForTesting(const ScopedNativeTruncateHookForTesting &) = delete;
			ScopedNativeTruncateHookForTesting &operator=(const ScopedNativeTruncateHookForTesting &) = delete;
		};
#endif

		/**
		 * @brief Position-independent write.
		 *
		 * Thread-safe for non-overlapping native writes. The fallback serializes its
		 * shared fstream seek position around each positioned operation.
		 */
		void writeAt(uint64_t offset, const void *buf, size_t size);

		/**
		 * @brief Write one complete generation-tagged native format page.
		 *
		 * Typed COW stores use this overload rather than the generation-zero byte
		 * I/O path. The key must describe one aligned 4 KiB physical page and carry
		 * a nonzero durable generation.
		 */
		void writeAt(BufferPoolPageKey key, const void *buf, size_t size);

		/**
		 * @brief Position-independent read.
		 *
		 * Thread-safe through native pread or the serialized fallback fstream.
		 * @return Number of bytes actually read.
		 */
		size_t readAt(uint64_t offset, void *buf, size_t size) const;

		/**
		 * @brief Read one complete generation-tagged native format page.
		 *
		 * This preserves cache identity across physical-frame reuse. The return
		 * value can be short only when the underlying file is truncated.
		 */
		size_t readAt(BufferPoolPageKey key, void *buf, size_t size,
					  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

		/**
		 * @brief Append data to the end of the file.
		 *
		 * Native append remains caller-serialized because size discovery and write
		 * form one operation; the fallback serializes its stream state.
		 * @return File offset where the data was written.
		 */
		uint64_t append(const void *buf, size_t size);

		/**
		 * @brief Reserve a logical append range without materializing zero bytes.
		 *
		 * Extends the file by writing a single zero byte at the end of the reserved
		 * range. Unwritten bytes in the gap read back as zero on supported desktop
		 * platforms, which matches the storage segment initialization contract.
		 *
		 * Native append/reserve calls remain caller-serialized; the fallback
		 * serializes its stream state.
		 * @return File offset where the reserved range starts.
		 */
		uint64_t reserveAppendSpace(size_t size);

		/**
		 * @brief Return the current file length without changing the logical append position.
		 */
		[[nodiscard]] uint64_t fileSize() const;

		/**
		 * @brief Shrink a native-backed file and invalidate any cached tail pages.
		 *
		 * The native single-file format uses this only to discard a contiguous,
		 * unpublished append suffix after a failed metadata publication.  fstream
		 * alone cannot portably truncate an already-open file, so callers must use
		 * a native write handle for this operation.
		 */
		void truncate(uint64_t size);

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
		[[nodiscard]] bool isWritable() const { return access_mode_ == StorageIOAccessMode::SIOAM_READ_WRITE; }

		/**
		 * @brief Access the underlying fstream.
		 *
		 * For legacy code paths that still need direct stream access
		 * (e.g., SegmentTracker chain loading during initialization).
		 */
		[[nodiscard]] std::shared_ptr<std::fstream> getStream() const { return stream_; }

		[[nodiscard]] file_handle_t getWriteFd() const { return writeFd_; }
		[[nodiscard]] file_handle_t getReadFd() const { return readFd_; }
		void setWriteFd(file_handle_t writeFd) { writeFd_ = writeFd; }

		// The native storage backend configures this once at open. Capacity is in
		// bytes and must contain at least one 4 KiB format page.
		void enableBufferPool(size_t capacityBytes);
		void disableBufferPool();
		// Best-effort cache warming for already validated native page routes. With
		// no Buffer Pool enabled this deliberately performs no speculative I/O.
		void prefetchPages(std::span<const BufferPoolPageKey> keys,
						   BufferPoolReadHint admissionHint = BufferPoolReadHint::BPRH_PREFETCH) const;
		[[nodiscard]] BufferPoolStats bufferPoolStats() const;
		// Keeps resident/pinned/dirty state intact and resets telemetry counters.
		void resetBufferPoolStats();
		// Counters measure bytes submitted through this abstraction, not filesystem
		// or device-level write amplification. They are transient and relaxed-atomic
		// so concurrent positioned I/O can be calibrated without serialization.
		[[nodiscard]] StorageIOStats stats() const;
		void resetStats();

	private:
		friend class BufferPool;
		// Saturation must be verified without issuing impractically large I/O.
		friend class StorageIOTestAccess;
		static void saturatingAdd(std::atomic<uint64_t> &counter, uint64_t value) noexcept;
		void recordRead(size_t bytes) const noexcept;
		void recordWrite(size_t bytes) noexcept;
		void writeAtUncached(uint64_t offset, const void *buf, size_t size);
		[[nodiscard]] size_t readAtUncached(uint64_t offset, void *buf, size_t size) const;
		[[nodiscard]] uint64_t fileSizeUncached() const;
		void ensureFileSizeUncached(uint64_t minimumSize);
		void syncUncached();
		void flushStreamUncached();
		void requireWritable(const char *operation) const;
		static void requireTypedPageKey(BufferPoolPageKey key, size_t size, const char *operation);
		[[nodiscard]] std::shared_ptr<BufferPool> bufferPool() const;

		std::shared_ptr<std::fstream> stream_;
		file_handle_t writeFd_;
		file_handle_t readFd_;
		StorageIOAccessMode access_mode_;
		// The fallback stream has one shared seek position. Native positioned I/O
		// remains lock-free; fallback operations serialize only that stream state.
		mutable std::mutex stream_mutex_;
		mutable std::mutex buffer_pool_mutex_;
		std::shared_ptr<BufferPool> buffer_pool_;
		mutable std::atomic<uint64_t> read_bytes_{0U};
		std::atomic<uint64_t> write_bytes_{0U};
		std::atomic<uint64_t> reserved_bytes_{0U};
		mutable std::atomic<uint64_t> read_calls_{0U};
		std::atomic<uint64_t> write_calls_{0U};
		std::atomic<uint64_t> reserve_calls_{0U};
		std::atomic<uint64_t> sync_calls_{0U};
	};

} // namespace graph::storage
