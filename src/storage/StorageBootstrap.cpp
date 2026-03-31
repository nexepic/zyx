/**
 * @file StorageBootstrap.cpp
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

#include "graph/storage/StorageBootstrap.hpp"
#include "graph/storage/SegmentTracker.hpp"

namespace graph::storage {

	StorageBootstrap::StorageBootstrap(std::shared_ptr<SegmentTracker> segmentTracker)
		: segmentTracker_(std::move(segmentTracker)) {}

	ChainScanResult StorageBootstrap::scanChain(uint64_t chainHead) const {
		ChainScanResult result;
		uint64_t currentOffset = chainHead;

		while (currentOffset != 0) {
			SegmentHeader header = segmentTracker_->getSegmentHeader(currentOffset);

			// Collect physical max ID (for IDAllocator)
			if (header.used > 0) {
				int64_t segmentUsedEndId = header.start_id + header.used - 1;
				if (segmentUsedEndId > result.physicalMaxId) {
					result.physicalMaxId = segmentUsedEndId;
				}
			}

			// Collect segment index entry (for SegmentIndexManager)
			SegmentIndexManager::SegmentIndex index{};
			index.startId = header.start_id;
			index.endId = header.start_id + (header.used > 0 ? header.used - 1 : 0);
			index.segmentOffset = currentOffset;
			result.segmentIndexEntries.push_back(index);

			currentOffset = header.next_segment_offset;
		}

		return result;
	}

} // namespace graph::storage
