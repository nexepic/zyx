/**
 * @file IntervalSet.cpp
 * @author Nexepic
 * @date 2025/4/19
 *
 * @copyright Copyright (c) 2025 Nexepic
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

#include "graph/storage/IntervalSet.hpp"
#include <algorithm>

namespace graph::storage {

	void IntervalSet::add(int64_t id) { addRange(id, id); }

	void IntervalSet::addRange(int64_t start, int64_t end) {
		if (start > end)
			return;

		// Optimization: Check if we can simply append to the last interval
		if (!intervals_.empty()) {
			auto lastIt = intervals_.rbegin();
			if (lastIt->second == start - 1) {
				lastIt->second = end; // Extend last interval
				return;
			}
		}

		// General Insert & Merge Logic
		auto it = intervals_.upper_bound(start);

		// Check overlap/adjacency with previous interval
		if (it != intervals_.begin()) {
			auto prev = std::prev(it);
			if (prev->second >= start - 1) {
				start = prev->first;
				end = std::max(end, prev->second);
				intervals_.erase(prev);
			}
		}

		// Check overlap/adjacency with subsequent intervals
		while (it != intervals_.end() && it->first <= end + 1) {
			end = std::max(end, it->second);
			it = intervals_.erase(it);
		}

		intervals_[start] = end;
	}

	int64_t IntervalSet::pop() {
		if (intervals_.empty())
			return 0;

		auto it = intervals_.begin();
		int64_t start = it->first;
		int64_t end = it->second;
		int64_t idToReturn = start;

		if (start == end) {
			intervals_.erase(it);
		} else {
			intervals_.erase(it);
			intervals_.emplace_hint(intervals_.begin(), start + 1, end);
		}

		return idToReturn;
	}

} // namespace graph::storage
