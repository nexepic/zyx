/**
 * @file IntervalSet.hpp
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

#pragma once

#include <cstdint>
#include <map>

namespace graph::storage {

	/// Compact storage for integer ID ranges with automatic merging.
	/// Stores intervals as sorted, non-overlapping [start, end] pairs.
	/// Example: add(1), add(2), add(3) → single interval [1,3].
	class IntervalSet {
	public:
		void add(int64_t id);
		void addRange(int64_t start, int64_t end);

		/// Removes and returns the smallest ID. Returns 0 if empty.
		int64_t pop();

		[[nodiscard]] size_t intervalCount() const { return intervals_.size(); }
		[[nodiscard]] bool empty() const { return intervals_.empty(); }
		void clear() { intervals_.clear(); }

	private:
		std::map<int64_t, int64_t> intervals_; // start → end, sorted, merged
	};

} // namespace graph::storage
