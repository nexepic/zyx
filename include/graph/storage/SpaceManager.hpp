/**
 * @file SpaceManager.hpp
 * @author Nexepic
 * @date 2025/4/11
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

#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <vector>
#include "EntityReferenceUpdater.hpp"
#include "FileHeaderManager.hpp"
#include "IDAllocator.hpp"
#include "PwriteHelper.hpp"
#include "SegmentTracker.hpp"
#include "SegmentType.hpp"
#include "StorageHeaders.hpp"

namespace graph::storage {

	class SpaceManager {
	public:
		SpaceManager(std::shared_ptr<std::fstream> file, std::string fileName, std::shared_ptr<SegmentTracker> tracker,
					 std::shared_ptr<FileHeaderManager> fileHeaderManager, std::shared_ptr<IDAllocator> idAllocator);
		~SpaceManager();

		static uint64_t findMaxId(uint32_t type, const std::shared_ptr<SegmentTracker> &tracker);
		uint64_t allocateSegment(uint32_t type, uint32_t capacity) const;
		void deallocateSegment(uint64_t offset) const;
		void compactSegments(uint32_t type, double threshold);
		bool moveSegment(uint64_t sourceOffset, uint64_t destinationOffset) const;

		bool shouldCompact() const;

		bool safeCompactSegments();
		void compactSegments();

		double getTotalFragmentationRatio() const;

		// Get the tracker
		std::shared_ptr<SegmentTracker> getTracker() const { return segmentTracker_; }

		// methods for segment merging
		std::vector<uint64_t> findCandidatesForMerge(uint32_t type, double usageThreshold) const;
		bool mergeSegments(uint32_t type, double usageThreshold);

		template<typename EntityType>
		void processEntity(uint64_t sourceOffset, uint64_t targetOffset, uint32_t i, uint32_t &targetNextIndex,
						   uint8_t *newBitmap, size_t itemSize, const SegmentHeader &sourceHeader,
						   const SegmentHeader &targetHeader, uint32_t type);

		bool mergeIntoSegment(uint64_t targetOffset, uint64_t sourceOffset, uint32_t type);

		// Remove empty segments
		bool processEmptySegments(uint32_t type) const;
		void processAllEmptySegments() const;

		// Truncates the file by removing unused trailing segments.
		// When @p nativeFd is a valid handle (from portable_open_rw), uses
		// portable_ftruncate() for an efficient in-place truncation that
		// avoids closing and reopening the fstream.
		// When INVALID_FILE_HANDLE (the default), falls back to the
		// close → resize_file → reopen pattern.
		bool truncateFile(file_handle_t nativeFd = INVALID_FILE_HANDLE) const;

		std::mutex &getMutex() { return mutex_; }

		bool isSegmentAtEndOfFile(uint64_t offset) const;
		uint64_t findFreeSegmentNotAtEnd() const;

		void setEntityReferenceUpdater(std::shared_ptr<EntityReferenceUpdater> entityReferenceUpdater) {
			entityReferenceUpdater_ = std::move(entityReferenceUpdater);
		}

		// Update FileHeader chain heads based on current SegmentTracker state
		void updateFileHeaderChainHeads() const;

		void recalculateMaxIds() const;

	private:
		std::mutex compactionMutex_;
		std::atomic<bool> compactionInProgress_{false};

		std::shared_ptr<std::fstream> file_;
		std::string fileName_;
		std::shared_ptr<SegmentTracker> segmentTracker_;
		std::mutex mutex_;

		std::shared_ptr<FileHeaderManager> fileHeaderManager_;

		std::shared_ptr<IDAllocator> idAllocator_;

		std::shared_ptr<EntityReferenceUpdater> entityReferenceUpdater_;

		// Update file header with current max IDs
		void updateMaxIds() const;

		// Compaction thresholds and counters
		static constexpr double COMPACTION_THRESHOLD = 0.3; // 30% inactive triggers compaction

		template<typename EntityType>
		bool compactSegment(uint64_t offset, SegmentType segmentType, size_t entitySize);

		bool compactNodeSegment(uint64_t offset);
		bool compactEdgeSegment(uint64_t offset);
		bool compactPropertySegment(uint64_t offset);
		bool compactBlobSegment(uint64_t offset);
		bool compactIndexSegment(uint64_t offset);
		bool compactStateSegment(uint64_t offset);
		bool copySegmentData(uint64_t sourceOffset, uint64_t destinationOffset) const;
		void updateSegmentChain(uint64_t newOffset, const SegmentHeader &info) const;

		uint64_t findSegmentForNodeId(int64_t id) const;
		uint64_t findSegmentForEdgeId(int64_t id) const;
		SegmentHeader readSegmentHeader(uint64_t offset) const;

		void writeSegmentHeader(uint64_t offset, const SegmentHeader &header) const;

		// Identifies segments at the end of file that can be truncated
		std::vector<uint64_t> findTruncatableSegments() const;

		// Relocate segments from the end of the file to fill gaps
		bool relocateSegmentsFromEnd() const;

		// Find segments near the end of the file that could be relocated
		std::vector<uint64_t> findRelocatableSegments() const;

		// Find the file size by examining segment positions
		uint64_t calculateCurrentFileSize() const;

		int64_t calculateLastUsedIdInSegment(const SegmentHeader &header) const;
	};

} // namespace graph::storage
