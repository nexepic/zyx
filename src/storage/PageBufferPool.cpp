/**
 * @file PageBufferPool.cpp
 * @date 2026/3/31
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/storage/PageBufferPool.hpp"
#include <cstring>
#include <mutex>

namespace graph::storage {

	PageBufferPool::PageBufferPool(size_t capacityPages) : capacity_(capacityPages) {}

	const Page *PageBufferPool::getPage(uint64_t segmentOffset) const {
		std::unique_lock lock(mutex_);
		auto it = pageMap_.find(segmentOffset);
		if (it == pageMap_.end()) {
			misses_.fetch_add(1, std::memory_order_relaxed);
			return nullptr;
		}
		hits_.fetch_add(1, std::memory_order_relaxed);
		// Move to front (most recently used) — requires exclusive lock since splice mutates the list
		pages_.splice(pages_.begin(), pages_, it->second);
		return &(*it->second);
	}

	bool PageBufferPool::copyPage(uint64_t segmentOffset, void *dest, size_t size) const {
		if (dest == nullptr) {
			return false;
		}
		std::unique_lock lock(mutex_);
		auto it = pageMap_.find(segmentOffset);
		if (it == pageMap_.end() || it->second->data.size() < size) {
			misses_.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		hits_.fetch_add(1, std::memory_order_relaxed);
		std::memcpy(dest, it->second->data.data(), size);
		pages_.splice(pages_.begin(), pages_, it->second);
		return true;
	}

	bool PageBufferPool::copyContiguousPages(uint64_t startSegmentOffset,
	                                         size_t pageCount,
	                                         void *dest,
	                                         size_t pageSize) const {
		if (dest == nullptr) {
			return false;
		}
		if (pageCount == 0) {
			return true;
		}

		std::unique_lock lock(mutex_);
		std::vector<std::list<Page>::iterator> pages;
		pages.reserve(pageCount);
		for (size_t page = 0; page < pageCount; ++page) {
			const uint64_t segmentOffset = startSegmentOffset + page * pageSize;
			auto it = pageMap_.find(segmentOffset);
			if (it == pageMap_.end() || it->second->data.size() < pageSize) {
				misses_.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			pages.push_back(it->second);
		}

		auto *out = static_cast<uint8_t *>(dest);
		for (size_t page = 0; page < pages.size(); ++page) {
			std::memcpy(out + page * pageSize, pages[page]->data.data(), pageSize);
			pages_.splice(pages_.begin(), pages_, pages[page]);
		}
		hits_.fetch_add(static_cast<uint64_t>(pageCount), std::memory_order_relaxed);
		return true;
	}

	void PageBufferPool::putPage(uint64_t segmentOffset, std::vector<uint8_t> &&data) {
		std::unique_lock lock(mutex_);
		if (capacity_ == 0) {
			return;
		}
		putPageLocked(segmentOffset, std::move(data));
	}

	void PageBufferPool::putContiguousPages(uint64_t startSegmentOffset,
	                                        size_t pageCount,
	                                        const void *src,
	                                        size_t pageSize) {
		if (src == nullptr || pageCount == 0) {
			return;
		}
		std::unique_lock lock(mutex_);
		if (capacity_ == 0) {
			return;
		}

		const auto *bytes = static_cast<const uint8_t *>(src);
		for (size_t page = 0; page < pageCount; ++page) {
			const uint64_t segmentOffset = startSegmentOffset + page * pageSize;
			const auto *begin = bytes + page * pageSize;
			putPageLocked(segmentOffset, std::vector<uint8_t>(begin, begin + pageSize));
		}
	}

	void PageBufferPool::putPageLocked(uint64_t segmentOffset, std::vector<uint8_t> &&data) {
		auto it = pageMap_.find(segmentOffset);
		if (it != pageMap_.end()) {
			// Update existing page
			it->second->data = std::move(data);
			pages_.splice(pages_.begin(), pages_, it->second);
			return;
		}

		// Evict LRU page if full
		if (pages_.size() >= capacity_) {
			auto &last = pages_.back();
			pageMap_.erase(last.segmentOffset);
			pages_.pop_back();
		}

		// Insert new page at front
		pages_.emplace_front(Page{segmentOffset, std::move(data)});
		pageMap_[segmentOffset] = pages_.begin();
	}

	void PageBufferPool::invalidate(uint64_t segmentOffset) {
		std::unique_lock lock(mutex_);
		auto it = pageMap_.find(segmentOffset);
		if (it != pageMap_.end()) {
			pages_.erase(it->second);
			pageMap_.erase(it);
		}
	}

	void PageBufferPool::clear() {
		std::unique_lock lock(mutex_);
		pageMap_.clear();
		pages_.clear();
	}

	size_t PageBufferPool::size() const {
		std::shared_lock lock(mutex_);
		return pages_.size();
	}

} // namespace graph::storage
