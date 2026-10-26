/**
 * @file BufferPool.cpp
 * @brief 2Q, generation-aware raw page cache for the native single-file store.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/cache/BufferPool.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/ResourceCoordinator.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>

#include "graph/storage/StorageIO.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] uint64_t checkedEnd(uint64_t offset, size_t bytes, const char *operation) {
			if (bytes > std::numeric_limits<uint64_t>::max() - offset) {
				throw std::invalid_argument(std::string("buffer pool ") + operation + " range overflows uint64_t");
			}
			return offset + static_cast<uint64_t>(bytes);
		}

	} // namespace

	BufferPool::Pin::~Pin() { release(); }

	BufferPool::Pin::Pin(Pin &&other) noexcept : pool_owner_(std::move(other.pool_owner_)), pool_(other.pool_), key_(other.key_), writable_(other.writable_) {
		other.pool_ = nullptr;
	}

	BufferPool::Pin &BufferPool::Pin::operator=(Pin &&other) noexcept {
		if (this != &other) {
			release();
			pool_owner_ = std::move(other.pool_owner_);
			pool_ = other.pool_;
			key_ = other.key_;
			writable_ = other.writable_;
			other.pool_ = nullptr;
		}
		return *this;
	}

	std::span<const std::byte, RSS_BUFFER_POOL_PAGE_BYTES> BufferPool::Pin::bytes() const {
		if (pool_ == nullptr) {
			throw std::logic_error("buffer pool page pin is no longer valid");
		}
		return pool_->bytesForPin(key_);
	}

	size_t BufferPool::Pin::validBytes() const {
		if (pool_ == nullptr) {
			throw std::logic_error("buffer pool page pin is no longer valid");
		}
		return pool_->validBytesForPin(key_);
	}

	std::span<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> BufferPool::Pin::mutableBytes() {
		if (pool_ == nullptr || !writable_) {
			throw std::logic_error("buffer pool page pin is not a private writable COW page");
		}
		return pool_->mutableBytesForPin(key_);
	}

	void BufferPool::Pin::markDirty() {
		if (pool_ == nullptr || !writable_) {
			throw std::logic_error("buffer pool page pin cannot dirty a shared page");
		}
		pool_->markDirtyForPin(key_);
	}

	void BufferPool::Pin::release() noexcept {
		if (pool_ != nullptr) {
			pool_->unpin(key_);
			pool_ = nullptr;
		}
        pool_owner_.reset(); // unpin before releasing the last pool owner
	}

	BufferPool::BufferPool(StorageIO &io, size_t capacityBytes, std::shared_ptr<std::pmr::memory_resource> memoryOwner) :
		io_(io), capacity_bytes_(capacityBytes - capacityBytes % RSS_BUFFER_POOL_PAGE_BYTES),
		memory_owner_(std::move(memoryOwner)), memory_(memory_owner_ ? memory_owner_.get() : std::pmr::get_default_resource()),
		entries_(memory_), offsets_(OffsetOrder{}, memory_), recent_(memory_), frequent_(memory_) {
		if (capacity_bytes_ == 0U) {
			throw std::invalid_argument("buffer pool capacity must contain at least one 4 KiB page");
		}
	}

	size_t BufferPool::KeyHash::operator()(const BufferPoolPageKey &key) const noexcept {
		const uint64_t mixed = key.page_offset ^ (key.generation + UINT64_C(0x9E3779B97F4A7C15) +
												  (key.page_offset << 6U) + (key.page_offset >> 2U));
		return static_cast<size_t>(mixed ^ (mixed >> 32U));
	}

	bool BufferPool::isAligned(BufferPoolPageKey key) noexcept {
		return key.page_offset % RSS_BUFFER_POOL_PAGE_BYTES == 0U &&
			   key.page_offset <= std::numeric_limits<uint64_t>::max() - RSS_BUFFER_POOL_PAGE_BYTES;
	}

	void BufferPool::requireTypedPageRange(BufferPoolPageKey firstKey, size_t bytes, const char *operation) {
		if (firstKey.generation == 0U || !isAligned(firstKey) || bytes == 0U ||
			bytes % RSS_BUFFER_POOL_PAGE_BYTES != 0U ||
			bytes > std::numeric_limits<uint64_t>::max() - firstKey.page_offset) {
			throw std::invalid_argument(std::string("buffer pool ") + operation +
								" needs a nonempty aligned complete typed-page range");
		}
	}

	BufferPool::Pin BufferPool::pin(BufferPoolPageKey key, BufferPoolReadHint hint) {
		if (!isAligned(key)) {
			throw std::invalid_argument("buffer pool page key is not 4 KiB aligned");
		}
		if (key.page_offset >= io_.fileSizeUncached()) {
			throw std::out_of_range("buffer pool cannot pin a page beyond the end of the database file");
		}
		std::unique_lock lock(mutex_, std::defer_lock);
		{ debug::ScopedPerfTimer waitTimer("storage.buffer_pool.pin_lock_wait"); lock.lock(); }
		Entry &entry = loadLocked(key, hint);
		if (entry.private_cow) {
			throw std::logic_error("private buffer-pool COW page cannot be shared before root publication");
		}
		if (entry.pin_count == 0U) {
			++pinned_pages_;
		}
		++entry.pin_count;
		debug::PerfTrace::addPagePin(&io_, key.page_offset, key.generation);
		return Pin(this, key, false);
	}

	BufferPool::Pin BufferPool::allocatePrivate(BufferPoolPageKey key) {
		if (!isAligned(key) || key.generation == 0U) {
			throw std::invalid_argument("private buffer-pool COW page needs an aligned nonzero generation key");
		}
		io_.requireWritable("BufferPool::allocatePrivate");
		io_.ensureFileSizeUncached(key.page_offset + RSS_BUFFER_POOL_PAGE_BYTES);
		std::lock_guard lock(mutex_);
		if (entries_.contains(key)) {
			throw std::logic_error("private buffer-pool COW page key is already resident");
		}
		ensureCapacityLocked(RSS_BUFFER_POOL_PAGE_BYTES);
		auto entry = allocateEntryLocked();
		entry->valid_bytes = RSS_BUFFER_POOL_PAGE_BYTES;
		entry->pin_count = 1U;
		entry->private_cow = true;
		Entry &stored = storeEntryLocked(key, std::move(entry));
		markDirtyLocked(stored);
		++pinned_pages_;
		return Pin(this, key, true);
	}

	size_t BufferPool::readPage(BufferPoolPageKey key, std::span<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> output,
								BufferPoolReadHint hint) {
		if (!isAligned(key) || key.generation == 0U) {
			throw std::invalid_argument("typed buffer-pool read needs an aligned nonzero generation key");
		}
        // Use the same allocation-free miss path as multi-page scan reads.
        // Existing dirty/private entries still pass through readPages' checks.
        if (hint == BufferPoolReadHint::BPRH_SEQUENTIAL) {
            return readPages(key, output, hint);
        }
		if (key.page_offset >= io_.fileSizeUncached()) {
			return 0U;
		}
        // A copy read needs no externally visible pin. Keep the cache lock
        // through the copy so a concurrent control-record invalidation waits
        // for these few bytes instead of rejecting an otherwise valid writer.
        std::lock_guard lock(mutex_);
        try {
        Entry &entry = loadLocked(key, hint);
        if (entry.private_cow) {
            throw std::logic_error("private buffer-pool COW page cannot be shared before root publication");
        }
        const size_t bytes = entry.valid_bytes;
        std::copy_n(entry.bytes.begin(), static_cast<std::ptrdiff_t>(bytes), output.begin());
        if (entry.transient && entry.pin_count == 0U) {
            eraseLocked(key, entry);
            entries_.erase(key);
        }
        return bytes;
        } catch (const query::execution::detail::ResourceAdmissionDenied &) {
            const size_t bytes = io_.readAtUncached(key.page_offset, output.data(), output.size());
            disk_read_bytes_ += bytes;
            debug::PerfTrace::addValue("storage.buffer_pool.physical_read_bytes", static_cast<int64_t>(bytes));
            return bytes;
        }
	}

	void BufferPool::writePage(BufferPoolPageKey key, std::span<const std::byte, RSS_BUFFER_POOL_PAGE_BYTES> input) {
		if (!isAligned(key) || key.generation == 0U) {
			throw std::invalid_argument("typed buffer-pool write needs an aligned nonzero generation key");
		}
		io_.requireWritable("BufferPool::writePage");
		io_.ensureFileSizeUncached(key.page_offset + RSS_BUFFER_POOL_PAGE_BYTES);

		std::lock_guard lock(mutex_);
		discardStaleGenerationsLocked(key);
		Entry *entry = nullptr;
		if (const auto found = entries_.find(key); found != entries_.end()) {
			if (found->second->pin_count != 0U || found->second->private_cow) {
				throw std::logic_error("buffer pool refuses to overwrite a pinned or private typed page");
			}
			++hits_;
			entry = found->second.get();
		} else {
			++misses_;
			ensureCapacityLocked(RSS_BUFFER_POOL_PAGE_BYTES);
			auto fresh = allocateEntryLocked();
			Entry &stored = *fresh;
			static_cast<void>(storeEntryLocked(key, std::move(fresh)));
			entry = &stored;
		}

		std::copy(input.begin(), input.end(), entry->bytes.begin());
		entry->valid_bytes = RSS_BUFFER_POOL_PAGE_BYTES;
		markDirtyLocked(*entry);
		promoteLocked(key, *entry);
	}

	size_t BufferPool::readPages(BufferPoolPageKey firstKey, std::span<std::byte> output, BufferPoolReadHint hint) {
		requireTypedPageRange(firstKey, output.size(), "typed read");
		const uint64_t fileSize = io_.fileSizeUncached();
		if (firstKey.page_offset >= fileSize) {
			return 0U;
		}
		const uint64_t readableBytes = std::min<uint64_t>(output.size(), fileSize - firstKey.page_offset);
		const size_t pageCount = output.size() / RSS_BUFFER_POOL_PAGE_BYTES;
		size_t copied = 0U;
		std::lock_guard lock(mutex_);
		for (size_t page = 0U; page < pageCount; ++page) {
			const uint64_t pageOffset = firstKey.page_offset + page * RSS_BUFFER_POOL_PAGE_BYTES;
			if (pageOffset >= fileSize) {
				return copied;
			}
			const BufferPoolPageKey key{pageOffset, firstKey.generation};
			const auto existing = entries_.find(key);
			if (existing != entries_.end()) {
				Entry &entry = *existing->second;
				if (entry.private_cow) {
					throw std::logic_error("private buffer-pool COW page cannot be shared before root publication");
				}
				++hits_;
				if (entry.prefetched) {
					++prefetch_hits_;
					entry.prefetched = false;
				}
				promoteLocked(key, entry);
				const size_t bytes = std::min<size_t>(
						entry.valid_bytes, static_cast<size_t>(std::min<uint64_t>(RSS_BUFFER_POOL_PAGE_BYTES,
																		fileSize - pageOffset)));
				std::copy_n(entry.bytes.begin(), static_cast<std::ptrdiff_t>(bytes),
							output.begin() + static_cast<std::ptrdiff_t>(copied));
				copied += bytes;
				if (bytes != RSS_BUFFER_POOL_PAGE_BYTES) {
					return copied;
				}
				continue;
			}
			if (hint != BufferPoolReadHint::BPRH_SEQUENTIAL) {
                try {
				Entry &entry = loadLocked(key, hint);
				const size_t bytes = std::min<size_t>(
						entry.valid_bytes, static_cast<size_t>(std::min<uint64_t>(RSS_BUFFER_POOL_PAGE_BYTES,
																		fileSize - pageOffset)));
				std::copy_n(entry.bytes.begin(), static_cast<std::ptrdiff_t>(bytes),
							output.begin() + static_cast<std::ptrdiff_t>(copied));
				copied += bytes;
				if (bytes != RSS_BUFFER_POOL_PAGE_BYTES) {
					return copied;
				}
				continue;
                } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                    const size_t bytes = io_.readAtUncached(pageOffset, output.data() + copied,
                        static_cast<size_t>(std::min<uint64_t>(RSS_BUFFER_POOL_PAGE_BYTES, fileSize - pageOffset)));
                    disk_read_bytes_ += bytes;
                    debug::PerfTrace::addValue("storage.buffer_pool.physical_read_bytes", static_cast<int64_t>(bytes));
                    copied += bytes;
                    if (bytes != RSS_BUFFER_POOL_PAGE_BYTES) return copied;
                    continue;
                }
			}

			// Sequential typed scans have no useful working-set residency. Coalesce
			// one missing run into a positioned read, as repeated readPage() calls
			// would otherwise issue one native read and cache lock per 4 KiB page.
			size_t runPages = 1U;
			while (page + runPages < pageCount &&
				   !entries_.contains({firstKey.page_offset + (page + runPages) * RSS_BUFFER_POOL_PAGE_BYTES,
											  firstKey.generation})) {
				++runPages;
			}
			const size_t requested = static_cast<size_t>(std::min<uint64_t>(
					static_cast<uint64_t>(runPages) * RSS_BUFFER_POOL_PAGE_BYTES, readableBytes - copied));
			const size_t bytesRead = io_.readAtUncached(pageOffset,
					output.data() + static_cast<std::ptrdiff_t>(copied), requested);
			disk_read_bytes_ += bytesRead;
		debug::PerfTrace::addValue("storage.buffer_pool.physical_read_bytes", static_cast<int64_t>(bytesRead));
			misses_ += runPages;
			copied += bytesRead;
			if (bytesRead != requested) {
				return copied;
			}
			if (requested != runPages * RSS_BUFFER_POOL_PAGE_BYTES) {
				return copied;
			}
			page += runPages - 1U;
		}
		return copied;
	}

	void BufferPool::writePages(BufferPoolPageKey firstKey, std::span<const std::byte> input) {
		requireTypedPageRange(firstKey, input.size(), "typed write");
		io_.requireWritable("BufferPool::writePages");
		io_.ensureFileSizeUncached(firstKey.page_offset + input.size());
		const size_t pageCount = input.size() / RSS_BUFFER_POOL_PAGE_BYTES;
		std::lock_guard lock(mutex_);
		const uint64_t end = firstKey.page_offset + input.size();
		// Check the entire range before flushing any stale entry. A rejected batch
		// must not partly invalidate a live snapshot or modify dirty accounting.
		requireUnpinnedRangeLocked(firstKey.page_offset, end, true);
		discardStaleGenerationsInRangeLocked(firstKey, pageCount);
		for (size_t page = 0U; page < pageCount; ++page) {
			const BufferPoolPageKey key{firstKey.page_offset + page * RSS_BUFFER_POOL_PAGE_BYTES,
										firstKey.generation};
			Entry *entry = nullptr;
			if (const auto found = entries_.find(key); found != entries_.end()) {
				++hits_;
				entry = found->second.get();
			} else {
				++misses_;
				ensureCapacityLocked(RSS_BUFFER_POOL_PAGE_BYTES);
				auto fresh = allocateEntryLocked();
				Entry &stored = *fresh;
				static_cast<void>(storeEntryLocked(key, std::move(fresh)));
				entry = &stored;
			}
			std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(page * RSS_BUFFER_POOL_PAGE_BYTES),
						RSS_BUFFER_POOL_PAGE_BYTES, entry->bytes.begin());
			entry->valid_bytes = RSS_BUFFER_POOL_PAGE_BYTES;
			markDirtyLocked(*entry);
			promoteLocked(key, *entry);
		}
	}

	void BufferPool::writePagesThrough(BufferPoolPageKey firstKey, std::span<const std::byte> input) {
        requireTypedPageRange(firstKey, input.size(), "typed write-through");
        io_.requireWritable("BufferPool::writePagesThrough");
        io_.ensureFileSizeUncached(firstKey.page_offset + input.size());
        const uint64_t end = firstKey.page_offset + input.size();
        std::lock_guard lock(mutex_);
        requireUnpinnedRangeLocked(firstKey.page_offset, end, true);
        eraseRangeLocked(firstKey.page_offset, end);
        // Invalidate before I/O so a short write cannot be overwritten by a
        // surviving dirty image. Unrelated offsets are never visited.
        io_.writeAtUncached(firstKey.page_offset, input.data(), input.size());
        disk_write_bytes_ += input.size();
    }

    void BufferPool::writeControlRecord(BufferPoolPageKey key, size_t byteOffset, std::span<const std::byte> input) {
        if (!isAligned(key) || key.generation == 0U || input.empty() ||
            byteOffset > RSS_BUFFER_POOL_PAGE_BYTES || input.size() > RSS_BUFFER_POOL_PAGE_BYTES - byteOffset)
            throw std::invalid_argument("buffer pool control record requires a valid typed-page range");
        const uint64_t offset = checkedEnd(key.page_offset, byteOffset, "control-record offset");
        io_.requireWritableRange(offset, input.size(), "BufferPool::writeControlRecord");
        std::lock_guard lock(mutex_);
        requireUnpinnedRangeLocked(key.page_offset, key.page_offset + RSS_BUFFER_POOL_PAGE_BYTES, false);
        eraseRangeLocked(key.page_offset, key.page_offset + RSS_BUFFER_POOL_PAGE_BYTES);
        io_.writeAtUncached(offset, input.data(), input.size());
        disk_write_bytes_ += input.size();
    }

	size_t BufferPool::read(uint64_t offset, std::span<std::byte> output, BufferPoolReadHint hint) {
		if (output.empty()) {
			return 0U;
		}
		const uint64_t requestedEnd = checkedEnd(offset, output.size(), "read");
		const uint64_t fileSize = io_.fileSizeUncached();
		if (offset >= fileSize) {
			return 0U;
		}
		const uint64_t end = std::min(requestedEnd, fileSize);
        if (hint == BufferPoolReadHint::BPRH_SEQUENTIAL) {
            // Keep dirty untyped hits coherent while coalescing uncached gaps.
            // No residency allocation or eviction is needed, even with all
            // cache pages pinned. Serialize reads with cache writes/invalidations.
            std::lock_guard lock(mutex_);
            size_t copied = 0U;
            while (offset + copied < end) {
                const uint64_t current = offset + copied;
                const uint64_t pageOffset = current - current % RSS_BUFFER_POOL_PAGE_BYTES;
                const size_t inPage = static_cast<size_t>(current - pageOffset);
                const auto found = entries_.find({pageOffset, 0U});
                if (found != entries_.end()) {
                    Entry &entry = *found->second;
                    if (inPage >= entry.valid_bytes) return copied;
                    const size_t bytes = (std::min)(entry.valid_bytes - inPage, static_cast<size_t>(end - current));
                    std::copy_n(entry.bytes.begin() + static_cast<std::ptrdiff_t>(inPage), bytes,
                        output.begin() + static_cast<std::ptrdiff_t>(copied));
                    ++hits_;
                    copied += bytes;
                    continue;
                }
                uint64_t gapEnd = (std::min)(end, pageOffset + RSS_BUFFER_POOL_PAGE_BYTES);
                while (gapEnd < end && !entries_.contains({gapEnd, 0U})) {
                    gapEnd += (std::min)(uint64_t{RSS_BUFFER_POOL_PAGE_BYTES}, end - gapEnd);
                }
                const size_t requested = static_cast<size_t>(gapEnd - current);
                const size_t bytes = io_.readAtUncached(current, output.data() + copied, requested);
                disk_read_bytes_ += bytes;
                copied += bytes;
                if (bytes != requested) return copied;
            }
            return copied;
        }
		size_t copied = 0U;
		uint64_t current = offset;
		while (current < end) {
			const uint64_t pageOffset = current - current % RSS_BUFFER_POOL_PAGE_BYTES;
			const size_t inPage = static_cast<size_t>(current - pageOffset);
			const size_t available = static_cast<size_t>(end - current);
			const size_t bytes = std::min(RSS_BUFFER_POOL_PAGE_BYTES - inPage, available);
			{
				// The cache lock protects this page from eviction while its bytes are
				// copied; a short logical read does not need an externally visible pin.
				std::lock_guard lock(mutex_);
                try {
                    Entry &entry = loadLocked({pageOffset, 0U}, hint);
                    std::copy_n(entry.bytes.begin() + static_cast<std::ptrdiff_t>(inPage), bytes,
                        output.begin() + static_cast<std::ptrdiff_t>(copied));
                } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                    // A normal byte read can bypass optional residency when
                    // another alias/pin owns the entire shared cache grant.
                    const size_t observed = io_.readAtUncached(current, output.data() + copied, bytes);
                    disk_read_bytes_ += observed;
                    debug::PerfTrace::addValue("storage.buffer_pool.physical_read_bytes", static_cast<int64_t>(observed));
                    if (observed != bytes) return copied + observed;
                }
			}
			copied += bytes;
			current += bytes;
		}
		return copied;
	}

	void BufferPool::write(uint64_t offset, std::span<const std::byte> input) {
		if (input.empty()) {
			return;
		}
		io_.requireWritable("BufferPool::write");
		const uint64_t end = checkedEnd(offset, input.size(), "write");
		io_.ensureFileSizeUncached(end);
		std::lock_guard lock(mutex_);
		size_t copied = 0U;
		uint64_t current = offset;
		while (copied < input.size()) {
			const uint64_t pageOffset = current - current % RSS_BUFFER_POOL_PAGE_BYTES;
			const size_t inPage = static_cast<size_t>(current - pageOffset);
			const size_t bytes = std::min(RSS_BUFFER_POOL_PAGE_BYTES - inPage, input.size() - copied);
			const BufferPoolPageKey key{pageOffset, 0U};
			// Generic byte I/O has no durable generation. It must evict any typed
			// entry at this physical address before changing the bytes underneath it.
			discardStaleGenerationsLocked(key);
			Entry *entry = nullptr;
			if (const auto found = entries_.find(key); found != entries_.end()) {
				++hits_;
				if (found->second->prefetched) {
					++prefetch_hits_;
					found->second->prefetched = false;
				}
				entry = found->second.get();
			} else if (inPage == 0U && bytes == RSS_BUFFER_POOL_PAGE_BYTES) {
				// A complete COW page replacement has no bytes to preserve. Avoid a
				// read-before-write round trip for bulk page and segment publication.
				++misses_;
				ensureCapacityLocked(RSS_BUFFER_POOL_PAGE_BYTES);
				auto fresh = allocateEntryLocked();
				Entry &stored = *fresh;
				static_cast<void>(storeEntryLocked(key, std::move(fresh)));
				entry = &stored;
			} else {
				entry = &loadLocked(key, BufferPoolReadHint::BPRH_NORMAL);
			}
			if (entry->pin_count != 0U) {
				throw std::logic_error("buffer pool refuses to overwrite a pinned page; use a private COW page");
			}
			std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(copied), bytes,
						entry->bytes.begin() + static_cast<std::ptrdiff_t>(inPage));
			entry->valid_bytes = std::max(entry->valid_bytes, inPage + bytes);
            markDirtyLocked(*entry);
			promoteLocked(key, *entry);
			copied += bytes;
			current += bytes;
		}
	}

	void BufferPool::prefetch(std::span<const BufferPoolPageKey> keys, BufferPoolReadHint admissionHint) {
		const uint64_t fileSize = io_.fileSizeUncached();
		for (const BufferPoolPageKey key: keys) {
			if (!isAligned(key)) {
				throw std::invalid_argument("buffer pool prefetch key is not 4 KiB aligned");
			}
			if (key.page_offset >= fileSize) {
				throw std::out_of_range("buffer pool cannot prefetch a page beyond the end of the database file");
			}
		}
		std::lock_guard lock(mutex_);
		for (const BufferPoolPageKey key: keys) {
			++prefetch_requests_;
			const auto existing = entries_.find(key);
			if (existing != entries_.end()) {
				continue;
			}
			Entry &entry = loadLocked(key, admissionHint);
			entry.prefetched = true;
		}
	}

	void BufferPool::flush() {
        std::lock_guard lock(mutex_);
        while (dirty_head_) {
            debug::PerfTrace::addValue("storage.buffer_pool.flush_visited_entries", 1);
            flushEntryLocked(*dirty_head_, dirty_head_->key.page_offset);
        }
    }

    void BufferPool::invalidateRange(uint64_t offset, size_t bytes) {
        if (!bytes) return;
        const uint64_t end = checkedEnd(offset, bytes, "invalidation");
        const uint64_t begin = offset - offset % RSS_BUFFER_POOL_PAGE_BYTES;
        std::lock_guard lock(mutex_);
        requireUnpinnedRangeLocked(begin, end, false);
        eraseRangeLocked(begin, end);
    }

	void BufferPool::discardForExternalRebase() {
		std::lock_guard lock(mutex_);
		for (const auto &[key, entry]: entries_) {
			static_cast<void>(key);
			if (entry->pin_count != 0U || entry->dirty) {
				throw std::logic_error(
						"buffer pool cannot discard pinned or dirty pages during an external metadata rebase");
			}
		}
		offsets_.clear();
		entries_.clear();
        dirty_head_ = nullptr;
		recent_.clear();
		frequent_.clear();
		resident_bytes_ = 0U;
		pinned_pages_ = 0U;
		dirty_pages_ = 0U;
	}

	BufferPoolStats BufferPool::stats() const {
		std::lock_guard lock(mutex_);
		return {capacity_bytes_,
				resident_bytes_,
				pinned_pages_ * RSS_BUFFER_POOL_PAGE_BYTES,
				dirty_pages_ * RSS_BUFFER_POOL_PAGE_BYTES,
				hits_,
				misses_,
				evictions_,
				prefetch_requests_,
				prefetch_hits_,
				disk_read_bytes_,
				disk_write_bytes_};
	}

	void BufferPool::resetStats() {
		std::lock_guard lock(mutex_);
		hits_ = 0U;
		misses_ = 0U;
		evictions_ = 0U;
		prefetch_requests_ = 0U;
		prefetch_hits_ = 0U;
		disk_read_bytes_ = 0U;
		disk_write_bytes_ = 0U;
	}

	BufferPool::Entry &BufferPool::loadLocked(BufferPoolPageKey key, BufferPoolReadHint hint) {
		debug::ScopedPerfTimer lookupTimer("storage.buffer_pool.load_locked");
		const auto existing = entries_.find(key);
		if (existing != entries_.end()) {
			++hits_;
			debug::PerfTrace::addValue("storage.buffer_pool.hits", 1);
			if (existing->second->prefetched) {
				++prefetch_hits_;
				existing->second->prefetched = false;
			}
			promoteLocked(key, *existing->second);
			return *existing->second;
		}
		++misses_;
		debug::PerfTrace::addValue("storage.buffer_pool.misses", 1);
		ensureCapacityLocked(RSS_BUFFER_POOL_PAGE_BYTES);
		auto entry = allocateEntryLocked();
		const size_t bytesRead = io_.readAtUncached(key.page_offset, entry->bytes.data(), entry->bytes.size());
		entry->valid_bytes = bytesRead;
		disk_read_bytes_ += bytesRead;
		debug::PerfTrace::addValue("storage.buffer_pool.physical_read_bytes", static_cast<int64_t>(bytesRead));
		entry->transient = hint == BufferPoolReadHint::BPRH_SEQUENTIAL;
		Entry &stored = *entry;
		static_cast<void>(storeEntryLocked(key, std::move(entry)));
		return stored;
	}

    std::shared_ptr<BufferPool::Entry> BufferPool::allocateEntryLocked() {
        for (;;) {
            try {
                return std::allocate_shared<Entry>(std::pmr::polymorphic_allocator<Entry>(memory_));
            } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                // Metadata counts against the same grant as page arrays, so the
                // grant can fill before the payload-only resident ceiling.
                if (!evictOneLocked()) throw;
            }
        }
    }

    BufferPool::Entry &BufferPool::storeEntryLocked(BufferPoolPageKey key, std::shared_ptr<Entry> entry) {
        for (;;) {
            try {
                // Retain the candidate locally while map/queue admission runs.
                // A denied node can be retried after evicting an unpinned page.
                auto [stored, inserted] = entries_.emplace(key, entry);
                if (!inserted) throw std::logic_error("buffer pool entry is already resident");
                try {
                    const auto [offset, added] = offsets_.emplace(key, stored->second.get());
                    if (!added) throw std::logic_error("buffer pool offset is already indexed");
                    try { admitLocked(key, *stored->second); }
                    catch (...) { offsets_.erase(offset); throw; }
                } catch (...) { entries_.erase(stored); throw; }
                stored->second->key = key;
                resident_bytes_ += RSS_BUFFER_POOL_PAGE_BYTES;
                return *stored->second;
            } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                if (!evictOneLocked()) throw;
            }
        }
    }

	void BufferPool::promoteLocked(BufferPoolPageKey, Entry &entry) {
		if (entry.queue == QueueKind::BQK_RECENT) {
			// Both queues use the same allocator. Splicing cannot fail after
			// removing the recent node, unlike erase followed by allocating push.
			frequent_.splice(frequent_.begin(), recent_, entry.recency);
			entry.recency = frequent_.begin();
			entry.queue = QueueKind::BQK_FREQUENT;
			return;
		}
		frequent_.splice(frequent_.begin(), frequent_, entry.recency);
	}

	void BufferPool::admitLocked(BufferPoolPageKey key, Entry &entry) {
		entry.queue = QueueKind::BQK_RECENT;
		recent_.push_front(key);
		entry.recency = recent_.begin();
	}

	void BufferPool::eraseLocked(BufferPoolPageKey key, Entry &entry) {
        offsets_.erase(key);
		if (entry.queue == QueueKind::BQK_RECENT) {
			recent_.erase(entry.recency);
		} else {
			frequent_.erase(entry.recency);
		}
		resident_bytes_ -= RSS_BUFFER_POOL_PAGE_BYTES;
		// Precondition: every erase caller flushes a dirty page first. This keeps
		// dirty-page accounting and durable writes at the publication boundary.
	}

	void BufferPool::ensureCapacityLocked(size_t requiredBytes) {
		while (resident_bytes_ > capacity_bytes_ - requiredBytes) {
			if (!evictOneLocked()) {
				throw std::runtime_error("buffer pool has no unpinned page available for eviction");
			}
		}
	}

	bool BufferPool::tryReleaseCachedPage(std::pmr::memory_resource *expectedOwner) {
		if (expectedOwner == nullptr || expectedOwner != memory_) return false;
		std::lock_guard lock(mutex_);
		return evictOneLocked(true);
	}

	bool BufferPool::evictOneLocked(bool cleanOnly) {
		const auto evictFrom = [this, cleanOnly](PageQueue &queue) {
			for (auto iterator = queue.rbegin(); iterator != queue.rend(); ++iterator) {
				const auto found = entries_.find(*iterator);
				if (found == entries_.end()) { // ZYX_COV_EXCL_LINE: recency and entry maps are mutated under one lock.
					throw std::logic_error("buffer pool recency list references a missing page");
				}
				Entry &entry = *found->second;
				if (entry.pin_count != 0U || (cleanOnly && (entry.dirty || entry.private_cow))) {
					continue;
				}
				if (entry.dirty) {
					flushEntryLocked(entry, found->first.page_offset);
				}
				const BufferPoolPageKey key = found->first;
				eraseLocked(key, entry);
				entries_.erase(found);
				++evictions_;
				return true;
			}
			return false;
		};
		return evictFrom(recent_) || evictFrom(frequent_);
	}

	void BufferPool::flushEntryLocked(Entry &entry, uint64_t pageOffset) {
		// A page becomes dirty only after a non-empty write, or as a full private
		// COW page. Both paths establish the exact durable byte range first.
		const size_t bytes = entry.valid_bytes;
		io_.writeAtUncached(pageOffset, entry.bytes.data(), bytes);
		disk_write_bytes_ += bytes;
        if (entry.dirty_previous) entry.dirty_previous->dirty_next = entry.dirty_next;
        else dirty_head_ = entry.dirty_next;
        if (entry.dirty_next) entry.dirty_next->dirty_previous = entry.dirty_previous;
        entry.dirty_previous = entry.dirty_next = nullptr;
		entry.dirty = false;
		--dirty_pages_;
	}

    void BufferPool::markDirtyLocked(Entry &entry) noexcept {
        if (entry.dirty) return;
        entry.dirty = true;
        entry.dirty_next = dirty_head_;
        entry.dirty_previous = nullptr;
        if (dirty_head_) dirty_head_->dirty_previous = &entry;
        dirty_head_ = &entry;
        ++dirty_pages_;
    }

    void BufferPool::requireUnpinnedRangeLocked(uint64_t begin, uint64_t end, bool rejectPrivate) const {
        for (auto it = offsets_.lower_bound({begin, 0U}); it != offsets_.end() && it->first.page_offset < end; ++it) {
            debug::PerfTrace::addValue("storage.buffer_pool.range_checked_entries", 1);
            if (it->second->pin_count || (rejectPrivate && it->second->private_cow))
                throw std::logic_error("buffer pool refuses to invalidate a pinned or private page");
        }
    }

    void BufferPool::eraseRangeLocked(uint64_t begin, uint64_t end, std::optional<uint64_t> keepGeneration) {
        auto it = offsets_.lower_bound({begin, 0U});
        while (it != offsets_.end() && it->first.page_offset < end) {
            const auto key = it->first;
            Entry &entry = *it->second;
            ++it; // eraseLocked removes this entry from both indexing structures.
            debug::PerfTrace::addValue("storage.buffer_pool.range_erase_visited_entries", 1);
            if (keepGeneration && key.generation == *keepGeneration) continue;
            if (entry.dirty) flushEntryLocked(entry, key.page_offset);
            eraseLocked(key, entry);
            entries_.erase(key);
        }
    }

    void BufferPool::discardStaleGenerationsLocked(BufferPoolPageKey key) {
        const uint64_t end = key.page_offset + RSS_BUFFER_POOL_PAGE_BYTES;
        for (auto it = offsets_.lower_bound({key.page_offset, 0U}); it != offsets_.end() && it->first.page_offset < end; ++it) {
            debug::PerfTrace::addValue("storage.buffer_pool.generation_checked_entries", 1);
            if (it->first.generation != key.generation && (it->second->pin_count || it->second->private_cow))
                throw std::logic_error("buffer pool refuses to recycle a pinned or private generation");
        }
        eraseRangeLocked(key.page_offset, end, key.generation);
    }

    void BufferPool::discardStaleGenerationsInRangeLocked(BufferPoolPageKey firstKey, size_t pageCount) {
        const uint64_t end = firstKey.page_offset + pageCount * RSS_BUFFER_POOL_PAGE_BYTES;
        for (auto it = offsets_.lower_bound({firstKey.page_offset, 0U}); it != offsets_.end() && it->first.page_offset < end; ++it) {
            debug::PerfTrace::addValue("storage.buffer_pool.generation_checked_entries", 1);
            if (it->first.generation != firstKey.generation && (it->second->pin_count || it->second->private_cow))
                throw std::logic_error("buffer pool refuses to recycle a pinned or private generation");
        }
        eraseRangeLocked(firstKey.page_offset, end, firstKey.generation);
    }

	void BufferPool::unpin(BufferPoolPageKey key) noexcept {
		try {
			std::lock_guard lock(mutex_);
			const auto found = entries_.find(key);
			if (found == entries_.end() ||
				found->second->pin_count ==
						0U) { // ZYX_COV_EXCL_LINE: only a broken Pin invariant can reach this guard.
				throw std::logic_error("buffer pool unpin does not match a live pin");
			}
			Entry &entry = *found->second;
			--entry.pin_count;
			if (entry.pin_count != 0U) {
				return;
			}
			--pinned_pages_;
			if (entry.transient) {
				// Transient pages are only created by read-only sequential pins. A
				// mutable write cannot overlap such a pin, so they are always clean.
				eraseLocked(key, entry);
				entries_.erase(found);
			}
		} catch (...) { // ZYX_COV_EXCL_LINE: only an internal pin-accounting defect can reach this fail-closed path.
			// A mismatched RAII pin corrupts the cache's snapshot-safety invariant.
			// Destruction cannot report that error, so fail closed rather than reuse
			// a page with an untrustworthy pin count.
			std::terminate();
		}
	}

	std::span<const std::byte, RSS_BUFFER_POOL_PAGE_BYTES> BufferPool::bytesForPin(BufferPoolPageKey key) const {
		std::lock_guard lock(mutex_);
		const auto found = entries_.find(key);
		if (found == entries_.end() ||
			found->second->pin_count == 0U) { // ZYX_COV_EXCL_LINE: only a broken Pin invariant can reach this guard.
			throw std::logic_error("buffer pool pin does not reference a resident page");
		}
		return found->second->bytes;
	}

	size_t BufferPool::validBytesForPin(BufferPoolPageKey key) const {
		std::lock_guard lock(mutex_);
		const auto found = entries_.find(key);
		if (found == entries_.end() ||
			found->second->pin_count == 0U) { // ZYX_COV_EXCL_LINE: only a broken Pin invariant can reach this guard.
			throw std::logic_error("buffer pool pin does not reference a resident page");
		}
		return found->second->valid_bytes;
	}

	std::span<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> BufferPool::mutableBytesForPin(BufferPoolPageKey key) {
		std::lock_guard lock(mutex_);
		const auto found = entries_.find(key);
		if (found == entries_.end() || found->second->pin_count != 1U || !found->second->private_cow ||
			!found->second
					 ->dirty) { // ZYX_COV_EXCL_LINE: the private Pin API enforces exclusive ownership before this call.
			throw std::logic_error("buffer pool mutable pin lost exclusive private-page ownership");
		}
		return found->second->bytes;
	}

	void BufferPool::markDirtyForPin(BufferPoolPageKey key) {
		std::lock_guard lock(mutex_);
		const auto found = entries_.find(key);
		if (found == entries_.end() || found->second->pin_count != 1U ||
			!found->second->private_cow) { // ZYX_COV_EXCL_LINE: the private Pin API enforces exclusive ownership before
										   // this call.
			throw std::logic_error("buffer pool mutable pin lost exclusive private-page ownership");
		}
        markDirtyLocked(*found->second);
	}

} // namespace graph::storage
