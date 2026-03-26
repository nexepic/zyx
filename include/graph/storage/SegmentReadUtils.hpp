/**
 * @file SegmentReadUtils.hpp
 * @brief Utilities for coalescing consecutive segment reads into single large I/O calls.
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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "SegmentIndexManager.hpp"
#include "StorageHeaders.hpp"

namespace graph::storage {

	struct CoalescedGroup {
		uint64_t startOffset;                // File offset of first segment in group
		size_t segCount;                     // Number of consecutive segments
		std::vector<size_t> memberIndices;   // Original indices into the input segIndices vector
	};

	/**
	 * @brief Build groups of consecutive segments that can be read in a single pread call.
	 *
	 * Sorts the given segment indices by their file offset, then groups consecutive
	 * segments where next.offset == prev.offset + TOTAL_SEGMENT_SIZE.
	 *
	 * @param segIndices Indices into segIndex identifying relevant segments
	 * @param segIndex The segment index array
	 * @return Vector of CoalescedGroup, each representing segments readable in one I/O
	 */
	inline std::vector<CoalescedGroup> buildCoalescedGroups(
		const std::vector<size_t> &segIndices,
		const std::vector<SegmentIndexManager::SegmentIndex> &segIndex) {

		if (segIndices.empty())
			return {};

		// Create a sortable copy with original indices
		struct SortEntry {
			size_t originalIdx;  // index into segIndices
			uint64_t offset;     // segment file offset
		};
		std::vector<SortEntry> entries;
		entries.reserve(segIndices.size());
		for (size_t i = 0; i < segIndices.size(); ++i) {
			entries.push_back({i, segIndex[segIndices[i]].segmentOffset});
		}

		std::sort(entries.begin(), entries.end(),
				  [](const SortEntry &a, const SortEntry &b) { return a.offset < b.offset; });

		std::vector<CoalescedGroup> groups;
		CoalescedGroup current;
		current.startOffset = entries[0].offset;
		current.segCount = 1;
		current.memberIndices.push_back(entries[0].originalIdx);

		for (size_t i = 1; i < entries.size(); ++i) {
			uint64_t expectedNext = current.startOffset + current.segCount * TOTAL_SEGMENT_SIZE;
			if (entries[i].offset == expectedNext) {
				current.segCount++;
				current.memberIndices.push_back(entries[i].originalIdx);
			} else {
				groups.push_back(std::move(current));
				current = CoalescedGroup{};
				current.startOffset = entries[i].offset;
				current.segCount = 1;
				current.memberIndices.push_back(entries[i].originalIdx);
			}
		}
		groups.push_back(std::move(current));
		return groups;
	}

} // namespace graph::storage
