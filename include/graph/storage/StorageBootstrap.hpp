/**
 * @file StorageBootstrap.hpp
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
#include <memory>
#include <vector>
#include "graph/storage/SegmentIndexManager.hpp"

namespace graph::storage {

	class SegmentTracker;

	/**
	 * @brief Result of scanning a single entity type's segment chain.
	 *
	 * Contains both the physical max ID (for IDAllocator gap recovery)
	 * and the segment index entries (for SegmentIndexManager).
	 */
	struct ChainScanResult {
		int64_t physicalMaxId = 0;
		std::vector<SegmentIndexManager::SegmentIndex> segmentIndexEntries;
	};

	/**
	 * @brief Performs a unified scan of all segment chains during database open.
	 *
	 * Eliminates the duplicate traversal of 6 entity type chains that previously
	 * occurred separately in IDAllocator::initialize() and
	 * SegmentIndexManager::buildSegmentIndexes().
	 */
	class StorageBootstrap {
	public:
		explicit StorageBootstrap(std::shared_ptr<SegmentTracker> segmentTracker);

		/**
		 * @brief Scan a single entity type's segment chain once,
		 * collecting both gap data and segment index entries.
		 */
		ChainScanResult scanChain(uint64_t chainHead) const;

	private:
		std::shared_ptr<SegmentTracker> segmentTracker_;
	};

} // namespace graph::storage
