/**
 * @file SegmentTracker.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "StorageHeaders.hpp"

namespace graph::storage {

	class SegmentIndexManager;

	class SegmentTracker {
	public:
		explicit SegmentTracker(std::shared_ptr<std::fstream> file);
		~SegmentTracker();

		void initialize(const FileHeader &header);
		void registerSegment(uint64_t offset, uint32_t type, uint32_t capacity);
		void updateSegmentUsage(uint64_t offset, uint32_t used, uint32_t inactive);
		void markForCompaction(uint64_t offset, bool needsCompaction);

		SegmentHeader &getSegmentHeader(uint64_t offset);
		std::vector<SegmentHeader> getSegmentsByType(uint32_t type) const;
		std::vector<SegmentHeader> getSegmentsNeedingCompaction(uint32_t type, double threshold) const;
		double calculateFragmentationRatio(uint32_t type) const;

		uint64_t getChainHead(uint32_t type) const;
		void updateChainHead(uint32_t type, uint64_t newHead) const;
		void updateSegmentLinks(uint64_t offset, uint64_t prevOffset, uint64_t nextOffset);

		void markSegmentFree(uint64_t offset);
		std::vector<uint64_t> getFreeSegments() const;
		void removeFromFreeList(uint64_t offset);

		void flushDirtySegments();

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

	private:
		std::shared_ptr<std::fstream> file_;
		mutable std::recursive_mutex mutex_;

		// Segment cache - stores actual segment headers
		std::unordered_map<uint64_t, SegmentHeader> segments_;

		std::unordered_set<uint64_t> freeSegments_;
		std::vector<uint64_t> dirtySegments_;
		std::weak_ptr<SegmentIndexManager> segmentIndexManager_;

		static void initializeRegistry();

		void loadSegments();

		// Load a chain of segments
		void loadSegmentChain(uint64_t headOffset, uint32_t type);

		// Ensure a segment is in cache
		void ensureSegmentCached(uint64_t offset);

		// Mark a segment as dirty
		void markSegmentDirty(uint64_t offset);
	};

} // namespace graph::storage
