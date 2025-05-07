/**
 * @file SegmentTracker.h
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
#include <mutex>
#include <unordered_map>
#include <vector>
#include "StorageHeaders.h"

#include <functional>

namespace graph::storage {

    class SegmentTracker {
    public:
        explicit SegmentTracker(std::shared_ptr<std::fstream> file);
        ~SegmentTracker();

        void initialize(const FileHeader& header);
        void registerSegment(uint64_t offset, uint8_t type, uint32_t capacity);
        void updateSegmentUsage(uint64_t offset, uint32_t used, uint32_t inactive);
        void markForCompaction(uint64_t offset, bool needsCompaction);

        SegmentHeader &getSegmentHeader(uint64_t offset);
        std::vector<SegmentHeader> getSegmentsByType(uint8_t type) const;
        std::vector<SegmentHeader> getSegmentsNeedingCompaction(uint8_t type, double threshold) const;
        double calculateFragmentationRatio(uint8_t type) const;

        uint64_t getChainHead(uint8_t type) const;
        void updateChainHead(uint8_t type, uint64_t newHead);
        void updateSegmentLinks(uint64_t offset, uint64_t prevOffset, uint64_t nextOffset);
        void markSegmentFree(uint64_t offset);
        std::vector<uint64_t> getFreeSegments() const;

        void removeFromFreeList(uint64_t offset);

        void flushDirtySegments();

        // New methods for finding segments for specific entities
        uint64_t getSegmentOffsetForNodeId(int64_t nodeId);
        uint64_t getSegmentOffsetForEdgeId(int64_t edgeId);
        uint64_t getSegmentOffsetForPropId(int64_t propId);
        uint64_t getSegmentOffsetForBlobId(int64_t blobId);

        // Unified segment header update operations
        void updateSegmentHeader(uint64_t offset, const std::function<void(SegmentHeader &)> & updateFn);

        void writeSegmentHeader(uint64_t offset, const SegmentHeader& header);

        // Bitmap operations
        void setEntityActive(uint64_t offset, uint32_t index, bool active);
        bool isEntityActive(uint64_t offset, uint32_t index) const;
        uint32_t countActiveEntities(uint64_t offset) const;

        // Methods for activity bitmap management
        void setBitmapBit(uint64_t offset, uint32_t index, bool value);
        bool getBitmapBit(uint64_t offset, uint32_t index) const;
        void updateActivityBitmap(uint64_t offset, const std::vector<bool>& activityMap);
        std::vector<bool> getActivityBitmap(uint64_t offset) const;

    private:
        std::shared_ptr<std::fstream> file_;
        mutable std::recursive_mutex mutex_;

        // Chain heads (cached)
        uint64_t nodeSegmentHead_ = 0;
        uint64_t edgeSegmentHead_ = 0;
        uint64_t propertySegmentHead_ = 0;
        uint64_t blobSegmentHead_ = 0;

        // Segment cache - stores actual segment headers
        std::unordered_map<uint64_t, SegmentHeader> segments_;

        std::vector<uint64_t> freeSegments_;
        std::vector<uint64_t> dirtySegments_;

        void loadSegments();

        // Load a chain of segments
        void loadSegmentChain(uint64_t headOffset, uint8_t type);

        // Ensure a segment is in cache
        void ensureSegmentCached(uint64_t offset);

        // Mark a segment as dirty
        void markSegmentDirty(uint64_t offset);
    };

} // namespace graph::storage