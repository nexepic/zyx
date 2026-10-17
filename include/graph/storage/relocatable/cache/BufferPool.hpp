/**
 * @file BufferPool.hpp
 * @brief Generation-aware, byte-bounded 2Q buffer pool for native storage pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <span>
#include <unordered_map>

#include "graph/storage/relocatable/cache/BufferPoolTypes.hpp"

namespace graph::storage {

	class StorageIO;

	class BufferPool final {
	public:
		using PageBytes = std::array<std::byte, RSS_BUFFER_POOL_PAGE_BYTES>;

		class Pin final {
		public:
			Pin() = default;
			~Pin();
			Pin(const Pin &) = delete;
			Pin &operator=(const Pin &) = delete;
			Pin(Pin &&other) noexcept;
			Pin &operator=(Pin &&other) noexcept;

			[[nodiscard]] bool isValid() const { return pool_ != nullptr; }
			[[nodiscard]] BufferPoolPageKey key() const { return key_; }
			[[nodiscard]] std::span<const std::byte, RSS_BUFFER_POOL_PAGE_BYTES> bytes() const;
			[[nodiscard]] size_t validBytes() const;
			[[nodiscard]] std::span<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> mutableBytes();
			void markDirty();

		private:
			friend class BufferPool;
			Pin(BufferPool *pool, BufferPoolPageKey key, bool writable) : pool_(pool), key_(key), writable_(writable) {}
			void release() noexcept;

			BufferPool *pool_ = nullptr;
			BufferPoolPageKey key_{};
			bool writable_ = false;
		};

		BufferPool(StorageIO &io, size_t capacityBytes, std::shared_ptr<std::pmr::memory_resource> memoryOwner = {});
		~BufferPool() = default;
		BufferPool(const BufferPool &) = delete;
		BufferPool &operator=(const BufferPool &) = delete;

		[[nodiscard]] Pin pin(BufferPoolPageKey key, BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL);
		// COW writers create a fresh generation-specific physical page. The caller
		// owns the only mutable pin until it publishes the page through a new root.
		[[nodiscard]] Pin allocatePrivate(BufferPoolPageKey key);
		// Typed storage paths identify a complete physical page by both its address
		// and durable generation. They never share the generation-zero generic-I/O
		// cache entries, so a recycled frame cannot serve stale snapshot bytes.
		[[nodiscard]] size_t readPage(BufferPoolPageKey key, std::span<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> output,
									  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL);
		void writePage(BufferPoolPageKey key, std::span<const std::byte, RSS_BUFFER_POOL_PAGE_BYTES> input);
		// Bounded typed scans such as private scratch candidates use one lock and
		// one uncached read per missing contiguous run, while retaining the normal
		// generation, pin, cache-admission, and short-read rules for every page.
		[[nodiscard]] size_t readPages(BufferPoolPageKey firstKey, std::span<std::byte> output,
									   BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL);
		void writePages(BufferPoolPageKey firstKey, std::span<const std::byte> input);
		// A caller with a complete, self-authenticating replacement image can
		// write it once without retaining scan-only scratch bytes in the cache.
		// Overlapping cache views are flushed and removed before the direct write.
		void writePagesThrough(BufferPoolPageKey firstKey, std::span<const std::byte> input);
		// Keep invalidation and the exact-record write under one cache lock so
		// a concurrent miss cannot retain a pre-write control-page image.
		void writeControlRecord(BufferPoolPageKey key, size_t byteOffset, std::span<const std::byte> input);
		[[nodiscard]] size_t read(uint64_t offset, std::span<std::byte> output,
								  BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL);
		void write(uint64_t offset, std::span<const std::byte> input);
		// Sequential readahead remains transient after the matching demand read;
		// ordinary prefetch retains the warmed page in the working set.
		void prefetch(std::span<const BufferPoolPageKey> keys,
					  BufferPoolReadHint admissionHint = BufferPoolReadHint::BPRH_PREFETCH);
		void flush();
		void invalidateRange(uint64_t offset, size_t bytes);
		// Drop every clean, unpinned cached page when another handle has advanced
		// the durable root. Unlike invalidation, this never writes a stale image.
		void discardForExternalRebase();

		[[nodiscard]] BufferPoolStats stats() const;
		void resetStats();
		// Optional sibling caches can reclaim one clean, unpinned page only
		// when both consumers charge the exact same allocation authority.
		[[nodiscard]] bool tryReleaseCachedPage(std::pmr::memory_resource *expectedOwner);

	private:
		// Test-only access verifies fail-closed guards that public Pin ownership
		// intentionally makes unreachable during normal operation.
		friend class BufferPoolTestAccess;

		struct KeyHash {
			[[nodiscard]] size_t operator()(const BufferPoolPageKey &key) const noexcept;
		};

		enum class QueueKind : uint8_t {
			BQK_RECENT = 0,
			BQK_FREQUENT = 1,
		};

		using PageQueue = std::pmr::list<BufferPoolPageKey>;
		struct Entry {
			PageBytes bytes{};
			size_t valid_bytes = 0U;
			size_t pin_count = 0U;
			bool dirty = false;
			bool private_cow = false;
			bool transient = false;
			bool prefetched = false;
			QueueKind queue = QueueKind::BQK_RECENT;
			PageQueue::iterator recency;
		};

		[[nodiscard]] static bool isAligned(BufferPoolPageKey key) noexcept;
		static void requireTypedPageRange(BufferPoolPageKey firstKey, size_t bytes, const char *operation);
		[[nodiscard]] Entry &loadLocked(BufferPoolPageKey key, BufferPoolReadHint hint);
		std::shared_ptr<Entry> allocateEntryLocked();
		Entry &storeEntryLocked(BufferPoolPageKey key, std::shared_ptr<Entry> entry);
		void promoteLocked(BufferPoolPageKey key, Entry &entry);
		void admitLocked(BufferPoolPageKey key, Entry &entry);
		void eraseLocked(BufferPoolPageKey key, Entry &entry);
		void ensureCapacityLocked(size_t requiredBytes);
		bool evictOneLocked(bool cleanOnly = false);
		void flushEntryLocked(Entry &entry, uint64_t pageOffset);
		void discardStaleGenerationsLocked(BufferPoolPageKey key);
		void discardStaleGenerationsInRangeLocked(BufferPoolPageKey firstKey, size_t pageCount);
		void unpin(BufferPoolPageKey key) noexcept;
		[[nodiscard]] std::span<const std::byte, RSS_BUFFER_POOL_PAGE_BYTES> bytesForPin(BufferPoolPageKey key) const;
		[[nodiscard]] size_t validBytesForPin(BufferPoolPageKey key) const;
		[[nodiscard]] std::span<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> mutableBytesForPin(BufferPoolPageKey key);
		void markDirtyForPin(BufferPoolPageKey key);

		StorageIO &io_;
		const size_t capacity_bytes_;
		// The allocator owner outlives every entry, queue node and hash bucket.
		std::shared_ptr<std::pmr::memory_resource> memory_owner_;
		std::pmr::memory_resource *memory_;
		mutable std::mutex mutex_;
		std::pmr::unordered_map<BufferPoolPageKey, std::shared_ptr<Entry>, KeyHash> entries_;
		PageQueue recent_;
		PageQueue frequent_;
		size_t resident_bytes_ = 0U;
		size_t pinned_pages_ = 0U;
		size_t dirty_pages_ = 0U;
		uint64_t hits_ = 0U;
		uint64_t misses_ = 0U;
		uint64_t evictions_ = 0U;
		uint64_t prefetch_requests_ = 0U;
		uint64_t prefetch_hits_ = 0U;
		uint64_t disk_read_bytes_ = 0U;
		uint64_t disk_write_bytes_ = 0U;
	};

} // namespace graph::storage
