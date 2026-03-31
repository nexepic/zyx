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

namespace graph::storage {

	PageBufferPool::PageBufferPool(size_t capacityPages) : capacity_(capacityPages) {}

	const Page *PageBufferPool::getPage(uint64_t segmentOffset) const {
		std::shared_lock lock(mutex_);
		auto it = pageMap_.find(segmentOffset);
		if (it == pageMap_.end()) {
			return nullptr;
		}
		// Move to front (most recently used)
		pages_.splice(pages_.begin(), pages_, it->second);
		return &(*it->second);
	}

	void PageBufferPool::putPage(uint64_t segmentOffset, std::vector<uint8_t> &&data) {
		std::unique_lock lock(mutex_);
		if (capacity_ == 0) {
			return;
		}

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
