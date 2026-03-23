/**
 * @file SegmentTracker.hpp
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

#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "SegmentTypeRegistry.hpp"
#include "StorageHeaders.hpp"

namespace graph::storage {

	class SegmentIndexManager;

	class SegmentTracker {
	public:
		explicit SegmentTracker(std::shared_ptr<std::fstream> file, const FileHeader &fileHeader);
		~SegmentTracker();

		void initialize(const FileHeader &header);
		void registerSegment(const SegmentHeader &header);
		void updateSegmentUsage(uint64_t offset, uint32_t used, uint32_t inactive);
		void markForCompaction(uint64_t offset, bool needsCompaction);

		SegmentHeader &getSegmentHeader(uint64_t offset);
		// Thread-safe read-only access for parallel scans. Returns a copy to avoid
		// lifetime issues with references into the mutable segments_ map.
		SegmentHeader getSegmentHeaderCopy(uint64_t offset) const;
		std::vector<SegmentHeader> getSegmentsByType(uint32_t type) const;
		std::vector<SegmentHeader> getSegmentsNeedingCompaction(uint32_t type, double threshold) const;
		double calculateFragmentationRatio(uint32_t type) const;

		uint64_t getChainHead(uint32_t type) const;
		void updateChainHead(uint32_t type, uint64_t newHead);
		void updateSegmentLinks(uint64_t offset, uint64_t prevOffset, uint64_t nextOffset);

		void markSegmentFree(uint64_t offset);
		std::vector<uint64_t> getFreeSegments() const;
		void removeFromFreeList(uint64_t offset);

		void flushDirtySegments();

		bool isIdInUsedRange(uint64_t segmentOffset, int64_t entityId);
		uint64_t getSegmentOffsetForEntityId(EntityType type, int64_t entityId);

		// New methods for finding segments for specific entities
		uint64_t getSegmentOffsetForNodeId(int64_t nodeId);
		uint64_t getSegmentOffsetForEdgeId(int64_t edgeId);
		uint64_t getSegmentOffsetForPropId(int64_t propId);
		uint64_t getSegmentOffsetForBlobId(int64_t blobId);
		uint64_t getSegmentOffsetForIndexId(int64_t indexId);
		uint64_t getSegmentOffsetForStateId(int64_t stateId);

		// Unified segment header update operations
		void updateSegmentHeader(uint64_t offset, const std::function<void(SegmentHeader &)> &updateFn);

		void writeSegmentHeader(uint64_t offset, const SegmentHeader &header);

		// Bitmap operations
		void setEntityActive(uint64_t offset, uint32_t index, bool active);
		bool isEntityActive(uint64_t offset, uint32_t index) const;
		uint32_t countActiveEntities(uint64_t offset) const;

		// Methods for activity bitmap management
		void setBitmapBit(uint64_t offset, uint32_t index, bool value);
		bool getBitmapBit(uint64_t offset, uint32_t index) const;
		void updateActivityBitmap(uint64_t offset, const std::vector<bool> &activityMap);
		std::vector<bool> getActivityBitmap(uint64_t offset) const;

		template<typename T>
		T readEntity(uint64_t segmentOffset, uint32_t itemIndex, size_t itemSize);

		template<typename T>
		void writeEntity(uint64_t segmentOffset, uint32_t itemIndex, const T &entity, size_t itemSize);

		void setSegmentIndexManager(std::weak_ptr<SegmentIndexManager> indexManager) {
			segmentIndexManager_ = std::move(indexManager);
		}

		void setReadFd(int fd) { readFd_ = fd; }

		// get segment type registry
		SegmentTypeRegistry getSegmentTypeRegistry() const { return registry_; }

	private:
		std::shared_ptr<std::fstream> file_;
		int readFd_ = -1; // pread fd for thread-safe reads (not owned, do not close)
		mutable std::shared_mutex rwMutex_;    // Read-write lock for concurrent read access
		mutable std::recursive_mutex mutex_;   // Legacy exclusive lock for write operations

		// Segment cache - stores actual segment headers
		std::unordered_map<uint64_t, SegmentHeader> segments_;

		std::unordered_set<uint64_t> freeSegments_;
		std::unordered_set<uint64_t> dirtySegments_;
		std::weak_ptr<SegmentIndexManager> segmentIndexManager_;

		SegmentTypeRegistry registry_;

		void initializeRegistry();

		void loadSegments();

		// Load a chain of segments
		void loadSegmentChain(uint64_t headOffset, uint32_t type);

		// Ensure a segment is in cache
		void ensureSegmentCached(uint64_t offset);

		// Mark a segment as dirty
		void markSegmentDirty(uint64_t offset);
	};

} // namespace graph::storage
