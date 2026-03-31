/**
 * @file PageBufferPool.hpp
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

#pragma once

#include <cstdint>
#include <list>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace graph::storage {

	struct Page {
		uint64_t segmentOffset;
		std::vector<uint8_t> data;
	};

	class PageBufferPool {
	public:
		explicit PageBufferPool(size_t capacityPages);

		const Page *getPage(uint64_t segmentOffset) const;

		void putPage(uint64_t segmentOffset, std::vector<uint8_t> &&data);

		void invalidate(uint64_t segmentOffset);

		void clear();

		[[nodiscard]] size_t size() const;

		[[nodiscard]] size_t capacity() const { return capacity_; }

	private:
		size_t capacity_;
		mutable std::list<Page> pages_;
		mutable std::unordered_map<uint64_t, std::list<Page>::iterator> pageMap_;
		mutable std::shared_mutex mutex_;
	};

} // namespace graph::storage
