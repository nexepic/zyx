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
#include <optional>
#include <span>
#include <system_error>
#include "PwriteHelper.hpp"
#include "graph/storage/relocatable/cache/BufferPool.hpp"
#include "graph/storage/relocatable/cache/BufferPoolTypes.hpp"

namespace graph::storage {
    // This refusal happens before the rejected I/O mutates cache or disk.
    // Earlier writes in a larger transaction still require outcome inspection.
    class StorageFileLimitExceeded final : public std::system_error {
    public:
        StorageFileLimitExceeded() : std::system_error(std::make_error_code(std::errc::no_space_on_device),
            "StorageIO configured database file byte limit exceeded") {}
    };
	class FileTruncator;
	class RelocatableMetadataStore;

	// The capability is explicit because a readable fstream does not expose
	// whether it was opened with std::ios::out.  This keeps a read-only cache
	// from deferring an invalid write until its teardown flush.
	enum class StorageIOAccessMode : uint8_t {
		SIOAM_READ_WRITE = 1,
		SIOAM_READ_ONLY = 2,
	};

	// A durable sync can either be general I/O maintenance or one of the two
	// ordered publication fences that make a COW Superblock candidate visible.
	enum class StorageIOSyncKind : uint8_t {
		SIOSK_GENERAL = 1,
		SIOSK_DATA_FENCE = 2,
		SIOSK_SUPERBLOCK_FENCE = 3,
	};

	struct StorageIOStats {
		uint64_t read_bytes = 0U;
		uint64_t write_bytes = 0U;
		uint64_t reserved_bytes = 0U;
		uint64_t read_calls = 0U;
		uint64_t write_calls = 0U;
		uint64_t reserve_calls = 0U;
		uint64_t sync_calls = 0U;
		uint64_t data_sync_calls = 0U;
		uint64_t superblock_sync_calls = 0U;

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
        StorageIO(std::shared_ptr<std::fstream> stream, file_handle_t writeFd, file_handle_t readFd,
                  StorageIOAccessMode accessMode, uint64_t maximumFileBytes);
		~StorageIO() noexcept;

        // Thread-bound scan policy. Read hits remain coherent with dirty cache
        // entries; misses bypass residency and decoded metadata cache admission.
        class SequentialReadScope final {
        public:
            explicit SequentialReadScope(const StorageIO &io) noexcept
                : io_(&io), previous_(active_) { active_ = this; }
            ~SequentialReadScope() { active_ = previous_; }
            SequentialReadScope(const SequentialReadScope &) = delete;
            SequentialReadScope &operator=(const SequentialReadScope &) = delete;
            [[nodiscard]] static bool activeFor(const StorageIO &io) noexcept {
                for (auto scope = active_; scope; scope = scope->previous_)
                    if (scope->io_ == &io) return true;
                return false;
            }
        private:
            const StorageIO *io_;
            SequentialReadScope *previous_;
            inline static thread_local SequentialReadScope *active_ = nullptr;
        };

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

		// A process-wide hook is reserved for background-worker fault tests. The
		// existing thread-local hook remains the default for ordinary tests.
		class ScopedAllThreadNativeWriteHookForTesting final {
		public:
			explicit ScopedAllThreadNativeWriteHookForTesting(NativeWriteHookForTesting hook);
			~ScopedAllThreadNativeWriteHookForTesting();

			ScopedAllThreadNativeWriteHookForTesting(const ScopedAllThreadNativeWriteHookForTesting &) = delete;
			ScopedAllThreadNativeWriteHookForTesting &
			operator=(const ScopedAllThreadNativeWriteHookForTesting &) = delete;
		};

		class ScopedNativeSyncHookForTesting final {
		public:
			explicit ScopedNativeSyncHookForTesting(NativeSyncHookForTesting hook);
			~ScopedNativeSyncHookForTesting();

			ScopedNativeSyncHookForTesting(const ScopedNativeSyncHookForTesting &) = delete;
			ScopedNativeSyncHookForTesting &operator=(const ScopedNativeSyncHookForTesting &) = delete;
		};

		class ScopedAllThreadNativeSyncHookForTesting final {
		public:
			explicit ScopedAllThreadNativeSyncHookForTesting(NativeSyncHookForTesting hook);
			~ScopedAllThreadNativeSyncHookForTesting();

			ScopedAllThreadNativeSyncHookForTesting(const ScopedAllThreadNativeSyncHookForTesting &) = delete;
			ScopedAllThreadNativeSyncHookForTesting &
			operator=(const ScopedAllThreadNativeSyncHookForTesting &) = delete;
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
		 * @brief Write a nonempty contiguous range of complete generation-tagged pages.
		 *
		 * The range must begin at a 4 KiB boundary, use one nonzero durable
		 * generation, and contain an exact multiple of native pages.  It avoids a
		 * lock/cache round trip per page without weakening typed cache identity.
		 */
		void writePages(BufferPoolPageKey firstKey, const void *buf, size_t size);

		/**
		 * @brief Write a complete typed-page range directly after invalidating cached views.
		 *
		 * This is for bounded, self-authenticating scratch images only. It rejects
		 * pinned/private views and never leaves a stale dirty cache entry after a
		 * short native write or positioned-I/O failure.
		 */
		void writePagesThrough(BufferPoolPageKey firstKey, const void *buf, size_t size);

		/**
		 * @brief Directly update a bounded record within one generation-tagged page.
		 *
		 * This is intentionally narrower than generic byte I/O: format control
		 * records use it for an inactive, independently checksummed record.  The
		 * method invalidates every cached view of the page before issuing the
		 * positioned write, so a failed partial record write cannot later be
		 * flushed back as a stale complete page.
		 */
		void writeControlRecord(BufferPoolPageKey key, size_t byteOffset, std::span<const std::byte> bytes);

		/**
		 * @brief Position-independent read.
		 *
		 * Thread-safe through native pread or the serialized fallback fstream.
		 * @return Number of bytes actually read.
		 */
		size_t readAt(uint64_t offset, void *buf, size_t size) const;

		/**
		 * @brief Read one known sequential range without admitting it to the Buffer Pool.
		 *
		 * Callers use this only for bounded, whole-object scans that authenticate
		 * their own immutable identity after the read. Bypassing the cache avoids
		 * displacing hot typed pages with scan-only scratch or relocation images.
		 */
		size_t readAtSequential(uint64_t offset, void *buf, size_t size) const;

		/**
		 * @brief Read one complete generation-tagged native format page.
		 *
		 * This preserves cache identity across physical-frame reuse. The return
		 * value can be short only when the underlying file is truncated.
		 */
		size_t readAt(BufferPoolPageKey key, void *buf, size_t size,
					  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;

		/**
		 * @brief Read a nonempty contiguous range of complete generation-tagged pages.
		 *
		 * The returned byte count can be short only for a truncated underlying file.
		 */
		size_t readPages(BufferPoolPageKey firstKey, void *buf, size_t size,
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
		// Records a successful durable fence with its publication role. General
		// callers retain sync(); only the A/B Superblock publisher classifies its
		// ordered data and root fences.
		void sync(StorageIOSyncKind kind);

		/**
		 * @brief Flush the fstream internal buffer.
		 *
		 * Required before pread can see data written via fstream,
		 * or before pwrite data is visible to fstream reads.
		 */
		void flushStream();

		[[nodiscard]] bool hasPwriteSupport() const { return writeFd_ != INVALID_FILE_HANDLE; }
		[[nodiscard]] bool hasPreadSupport() const { return readFd_ != INVALID_FILE_HANDLE; }

		// The native storage backend configures this once at open. Capacity is in
		// bytes and must contain at least one 4 KiB format page.
		void enableBufferPool(size_t capacityBytes,
                              std::shared_ptr<std::pmr::memory_resource> memoryOwner = {});
		void disableBufferPool();
		// Holds a generation-qualified cached native page without copying its
		// bytes. Typed storage readers use this only while their decoded view is
		// alive; callers without an enabled pool keep the established copy path.
		[[nodiscard]] std::optional<BufferPool::Pin>
		pinTypedPage(BufferPoolPageKey key, BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const;
		// Discard clean, unpinned cached pages after another handle has published a
		// newer durable root. This deliberately never flushes local cache images.
		void discardBufferPoolForExternalRebase();
		// Best-effort cache warming for already validated native page routes. With
		// no Buffer Pool enabled this deliberately performs no speculative I/O.
		void prefetchPages(std::span<const BufferPoolPageKey> keys,
						   BufferPoolReadHint admissionHint = BufferPoolReadHint::BPRH_PREFETCH) const;
		[[nodiscard]] BufferPoolStats bufferPoolStats() const;
		[[nodiscard]] bool tryReleaseCachedPage(std::pmr::memory_resource *expectedOwner) const;
		// Keeps resident/pinned/dirty state intact and resets telemetry counters.
		void resetBufferPoolStats();
		// Counters measure bytes submitted through this abstraction, not filesystem
		// or device-level write amplification. They are transient and relaxed-atomic
		// so concurrent positioned I/O can be calibrated without serialization.
		[[nodiscard]] StorageIOStats stats() const;
		// Monotonic cache invalidation identity; diagnostic resets never reset it.
		[[nodiscard]] uint64_t dataMutationEpoch() const { return data_mutation_epoch_.load(std::memory_order_relaxed); }
		void resetStats();

	private:
		friend class FileTruncator;
		friend class RelocatableStorageBackend;
		friend class RelocatableMetadataStore;
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
		[[nodiscard]] std::shared_ptr<std::fstream> getStream() const { return stream_; }
		[[nodiscard]] file_handle_t getWriteFd() const { return writeFd_; }
		void setWriteFd(file_handle_t writeFd) { writeFd_ = writeFd; }
		[[nodiscard]] bool isWritable() const { return access_mode_ == StorageIOAccessMode::SIOAM_READ_WRITE; }
		void requireWritable(const char *operation) const;
        void requireWritableRange(uint64_t offset, size_t size, const char *operation) const;
		static void requireTypedPageKey(BufferPoolPageKey key, size_t size, const char *operation);
		static void requireTypedPageRange(BufferPoolPageKey firstKey, size_t size, const char *operation);
		[[nodiscard]] std::shared_ptr<BufferPool> bufferPool() const;

		std::shared_ptr<std::fstream> stream_;
		file_handle_t writeFd_;
		file_handle_t readFd_;
		StorageIOAccessMode access_mode_;
        const uint64_t maximum_file_bytes_;
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
		std::atomic<uint64_t> data_mutation_epoch_{0U};
		std::atomic<uint64_t> reserve_calls_{0U};
		std::atomic<uint64_t> sync_calls_{0U};
		std::atomic<uint64_t> data_sync_calls_{0U};
		std::atomic<uint64_t> superblock_sync_calls_{0U};
	};

} // namespace graph::storage
