/**
 * @file BufferPool.cpp
 * @brief 2Q, generation-aware raw page cache for the native single-file store.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/cache/BufferPool.hpp"

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

	BufferPool::Pin::Pin(Pin &&other) noexcept : pool_(other.pool_), key_(other.key_), writable_(other.writable_) {
		other.pool_ = nullptr;
	}

	BufferPool::Pin &BufferPool::Pin::operator=(Pin &&other) noexcept {
		if (this != &other) {
			release();
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
	}

	BufferPool::BufferPool(StorageIO &io, size_t capacityBytes) :
		io_(io), capacity_bytes_(capacityBytes - capacityBytes % RSS_BUFFER_POOL_PAGE_BYTES) {
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

	BufferPool::Pin BufferPool::pin(BufferPoolPageKey key, BufferPoolReadHint hint) {
		if (!isAligned(key)) {
			throw std::invalid_argument("buffer pool page key is not 4 KiB aligned");
		}
		if (key.page_offset >= io_.fileSizeUncached()) {
			throw std::out_of_range("buffer pool cannot pin a page beyond the end of the database file");
		}
		std::lock_guard lock(mutex_);
		Entry &entry = loadLocked(key, hint);
		if (entry.private_cow) {
			throw std::logic_error("private buffer-pool COW page cannot be shared before root publication");
		}
		if (entry.pin_count == 0U) {
			++pinned_pages_;
		}
		++entry.pin_count;
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
		auto entry = std::make_unique<Entry>();
		entry->valid_bytes = RSS_BUFFER_POOL_PAGE_BYTES;
		entry->pin_count = 1U;
		entry->dirty = true;
		entry->private_cow = true;
		Entry &stored = *entry;
		entries_.emplace(key, std::move(entry));
		admitLocked(key, stored);
		resident_bytes_ += RSS_BUFFER_POOL_PAGE_BYTES;
		++pinned_pages_;
		++dirty_pages_;
		return Pin(this, key, true);
	}

	size_t BufferPool::readPage(BufferPoolPageKey key, std::span<std::byte, RSS_BUFFER_POOL_PAGE_BYTES> output,
								BufferPoolReadHint hint) {
		if (!isAligned(key) || key.generation == 0U) {
			throw std::invalid_argument("typed buffer-pool read needs an aligned nonzero generation key");
		}
		if (key.page_offset >= io_.fileSizeUncached()) {
			return 0U;
		}
		auto page = pin(key, hint);
		const size_t bytes = page.validBytes();
		std::copy_n(page.bytes().begin(), static_cast<std::ptrdiff_t>(bytes), output.begin());
		return bytes;
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
			auto fresh = std::make_unique<Entry>();
			Entry &stored = *fresh;
			entries_.emplace(key, std::move(fresh));
			admitLocked(key, stored);
			resident_bytes_ += RSS_BUFFER_POOL_PAGE_BYTES;
			entry = &stored;
		}

		std::copy(input.begin(), input.end(), entry->bytes.begin());
		entry->valid_bytes = RSS_BUFFER_POOL_PAGE_BYTES;
		if (!entry->dirty) {
			entry->dirty = true;
			++dirty_pages_;
		}
		promoteLocked(key, *entry);
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
			const size_t bytes = io_.readAtUncached(offset, output.data(), static_cast<size_t>(end - offset));
			std::lock_guard lock(mutex_);
			disk_read_bytes_ += bytes;
			return bytes;
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
				Entry &entry = loadLocked({pageOffset, 0U}, hint);
				std::copy_n(entry.bytes.begin() + static_cast<std::ptrdiff_t>(inPage), bytes,
							output.begin() + static_cast<std::ptrdiff_t>(copied));
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
				auto fresh = std::make_unique<Entry>();
				Entry &stored = *fresh;
				entries_.emplace(key, std::move(fresh));
				admitLocked(key, stored);
				resident_bytes_ += RSS_BUFFER_POOL_PAGE_BYTES;
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
			if (!entry->dirty) {
				entry->dirty = true;
				++dirty_pages_;
			}
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
		for (const auto &[key, entry]: entries_) {
			if (entry->dirty) {
				flushEntryLocked(*entry, key.page_offset);
			}
		}
	}

	void BufferPool::invalidateRange(uint64_t offset, size_t bytes) {
		if (bytes == 0U) {
			return;
		}
		const uint64_t end = checkedEnd(offset, bytes, "invalidation");
		std::lock_guard lock(mutex_);
		for (auto iterator = entries_.begin(); iterator != entries_.end();) {
			const uint64_t pageStart = iterator->first.page_offset;
			const uint64_t pageEnd = pageStart + RSS_BUFFER_POOL_PAGE_BYTES;
			Entry &entry = *iterator->second;
			if (pageEnd <= offset || pageStart >= end) {
				++iterator;
				continue;
			}
			if (entry.pin_count != 0U) {
				throw std::logic_error("buffer pool refuses to invalidate a pinned page");
			}
			if (entry.dirty) {
				flushEntryLocked(entry, pageStart);
			}
			const BufferPoolPageKey key = iterator->first;
			eraseLocked(key, entry);
			iterator = entries_.erase(iterator);
		}
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
		const auto existing = entries_.find(key);
		if (existing != entries_.end()) {
			++hits_;
			if (existing->second->prefetched) {
				++prefetch_hits_;
				existing->second->prefetched = false;
			}
			promoteLocked(key, *existing->second);
			return *existing->second;
		}
		++misses_;
		ensureCapacityLocked(RSS_BUFFER_POOL_PAGE_BYTES);
		auto entry = std::make_unique<Entry>();
		const size_t bytesRead = io_.readAtUncached(key.page_offset, entry->bytes.data(), entry->bytes.size());
		entry->valid_bytes = bytesRead;
		disk_read_bytes_ += bytesRead;
		entry->transient = hint == BufferPoolReadHint::BPRH_SEQUENTIAL;
		Entry &stored = *entry;
		entries_.emplace(key, std::move(entry));
		admitLocked(key, stored);
		resident_bytes_ += RSS_BUFFER_POOL_PAGE_BYTES;
		return stored;
	}

	void BufferPool::promoteLocked(BufferPoolPageKey key, Entry &entry) {
		if (entry.queue == QueueKind::BQK_RECENT) {
			recent_.erase(entry.recency);
			frequent_.push_front(key);
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

	void BufferPool::eraseLocked(BufferPoolPageKey, Entry &entry) {
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

	bool BufferPool::evictOneLocked() {
		const auto evictFrom = [this](std::list<BufferPoolPageKey> &queue) {
			for (auto iterator = queue.rbegin(); iterator != queue.rend(); ++iterator) {
				const auto found = entries_.find(*iterator);
				if (found == entries_.end()) { // ZYX_COV_EXCL_LINE: recency and entry maps are mutated under one lock.
					throw std::logic_error("buffer pool recency list references a missing page");
				}
				Entry &entry = *found->second;
				if (entry.pin_count != 0U) {
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
		entry.dirty = false;
		--dirty_pages_;
	}

	void BufferPool::discardStaleGenerationsLocked(BufferPoolPageKey key) {
		for (auto iterator = entries_.begin(); iterator != entries_.end();) {
			const BufferPoolPageKey residentKey = iterator->first;
			Entry &entry = *iterator->second;
			if (residentKey.page_offset != key.page_offset || residentKey.generation == key.generation) {
				++iterator;
				continue;
			}
			if (entry.pin_count != 0U || entry.private_cow) {
				throw std::logic_error("buffer pool refuses to recycle a pinned or private generation");
			}
			if (entry.dirty) {
				flushEntryLocked(entry, residentKey.page_offset);
			}
			eraseLocked(residentKey, entry);
			iterator = entries_.erase(iterator);
		}
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
		if (!found->second->dirty) {
			found->second->dirty = true;
			++dirty_pages_;
		}
	}

} // namespace graph::storage
