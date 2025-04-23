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
#include <vector>
#include <unordered_map>
#include <fstream>
#include <mutex>
#include "StorageHeaders.h"

namespace graph::storage {

    struct SegmentInfo {
        uint64_t offset = 0;
        uint8_t type = 0;
        uint32_t capacity = 0;
        uint32_t used = 0;
        uint32_t inactive = 0;
        uint64_t nextSegment = 0;
        uint64_t prevSegment = 0;
        bool needsCompaction = false;

        // Helper method to get number of active elements
        uint32_t getActiveCount() const {
            return (used > inactive) ? (used - inactive) : 0;
        }

        // Helper method to get total free space (never used + inactive)
        uint32_t getTotalFreeSpace() const {
            return capacity - getActiveCount();
        }

        // Calculate true fragmentation ratio
        double getFragmentationRatio() const {
            return (capacity > 0) ? static_cast<double>(getTotalFreeSpace()) / capacity : 0.0;
        }
    };

    class SegmentTracker {
    public:
        explicit SegmentTracker(std::shared_ptr<std::fstream> file);
        ~SegmentTracker();

        void initialize(const FileHeader& header);
        void registerSegment(uint64_t offset, uint8_t type, uint32_t capacity);
        void updateSegmentUsage(uint64_t offset, uint32_t used, uint32_t inactive);
        void markForCompaction(uint64_t offset, bool needsCompaction);

        SegmentInfo &getSegmentInfo(uint64_t offset);
        std::vector<SegmentInfo> getSegmentsByType(uint8_t type) const;
        std::vector<SegmentInfo> getSegmentsNeedingCompaction(uint8_t type, double threshold) const;
        double calculateFragmentationRatio(uint8_t type) const;
        double calculateUtilizationRatio(uint64_t offset) const;
        std::vector<SegmentInfo> getLowUtilizationSegments(uint8_t type, double threshold) const;

        uint64_t getChainHead(uint8_t type) const;
        void updateChainHead(uint8_t type, uint64_t newHead);
        void updateSegmentLinks(uint64_t offset, uint64_t prevOffset, uint64_t nextOffset);
        void markSegmentFree(uint64_t offset);
        std::vector<uint64_t> getFreeSegments() const;

        void removeFromFreeList(uint64_t offset);

        void refreshSegmentInfo(uint64_t offset);

        // New methods for finding segments for specific entities
        uint64_t getSegmentOffsetForNodeId(uint64_t nodeId) const;
        uint64_t getSegmentOffsetForEdgeId(uint64_t edgeId) const;

        SegmentHeader readSegmentHeader(uint64_t offset) const;

        // Bitmap operations
        void setEntityActive(uint64_t offset, uint32_t index, bool active);
        bool isEntityActive(uint64_t offset, uint32_t index) const;
        uint32_t countActiveEntities(uint64_t offset) const;

        // Methods for activity bitmap management
        void initializeActivityBitmap(uint64_t offset, uint32_t capacity);
        void updateActivityBitmap(uint64_t offset, const std::vector<bool>& activityMap);
        std::vector<bool> getActivityBitmap(uint64_t offset) const;

        std::mutex& getMutex() { return mutex_; }

    private:
        std::shared_ptr<std::fstream> file_;
        mutable std::mutex mutex_;

        // Chain heads (cached)
        uint64_t nodeSegmentHead_ = 0;
        uint64_t edgeSegmentHead_ = 0;
        uint64_t propertySegmentHead_ = 0;
        uint64_t blobSegmentHead_ = 0;

        // Segment maps
        std::unordered_map<uint64_t, SegmentInfo> segments_;
        std::vector<uint64_t> freeSegments_;

        void writeSegmentHeader(uint64_t offset, const SegmentHeader& header);
        void ensureSegmentCached(uint64_t offset);
    };

} // namespace graph::storage