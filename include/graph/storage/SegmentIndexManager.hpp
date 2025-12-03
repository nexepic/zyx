/**
 * @file SegmentIndexManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include <shared_mutex> // Changed from mutex to shared_mutex for R/W locking
#include <vector>
#include "StorageHeaders.hpp"

namespace graph::storage {

    class SegmentTracker;

    /**
     * @brief Manages segment indexes for efficient entity ID lookups.
     *
     * Architecture Update:
     * This manager now reflects the Real-Time In-Memory state of segments.
     * It is updated immediately when SegmentTracker modifies segment metadata,
     * decoupling index validity from disk flush operations.
     */
    class SegmentIndexManager {
    public:
        struct SegmentIndex {
            int64_t startId;
            int64_t endId;
            uint64_t segmentOffset;
        };

        explicit SegmentIndexManager(std::shared_ptr<SegmentTracker> segmentTracker);

        void initialize(uint64_t &nodeHead, uint64_t &edgeHead, uint64_t &propertyHead, uint64_t &blobHead,
                        uint64_t &indexHead, uint64_t &stateHead);

        /**
         * @brief Find the segment containing an entity ID.
         * Thread-safe (Shared Lock).
         */
        uint64_t findSegmentForId(uint32_t type, int64_t id) const;

        // Accessors
        const std::vector<SegmentIndex> &getNodeSegmentIndex() const { return nodeSegmentIndex_; }
        const std::vector<SegmentIndex> &getEdgeSegmentIndex() const { return edgeSegmentIndex_; }
        const std::vector<SegmentIndex> &getPropertySegmentIndex() const { return propertySegmentIndex_; }
        const std::vector<SegmentIndex> &getBlobSegmentIndex() const { return blobSegmentIndex_; }
        const std::vector<SegmentIndex> &getIndexSegmentIndex() const { return indexSegmentIndex_; }
        const std::vector<SegmentIndex> &getStateSegmentIndex() const { return stateSegmentIndex_; }

        /**
         * @brief Full rebuild of indexes (Used during initialization/compaction).
         */
        void buildSegmentIndexes();

        /**
         * @brief Updates or Inserts a segment index entry based on the header.
         * Called immediately when a segment is created or its usage changes in memory.
         * Thread-safe (Unique Lock).
         *
         * @param header The current header of the segment.
         */
    	void updateSegmentIndex(const SegmentHeader &header, int64_t oldStartId);

        /**
         * @brief Removes a segment from the index.
         * Called immediately when a segment is marked free/deleted.
         * Thread-safe (Unique Lock).
         *
         * @param header The header of the segment being removed.
         */
        void removeSegmentIndex(const SegmentHeader &header);

    private:
        std::shared_ptr<SegmentTracker> segmentTracker_;

        std::vector<SegmentIndex> nodeSegmentIndex_;
        std::vector<SegmentIndex> edgeSegmentIndex_;
        std::vector<SegmentIndex> propertySegmentIndex_;
        std::vector<SegmentIndex> blobSegmentIndex_;
        std::vector<SegmentIndex> indexSegmentIndex_;
        std::vector<SegmentIndex> stateSegmentIndex_;

        uint64_t *nodeSegmentHead_ = nullptr;
        uint64_t *edgeSegmentHead_ = nullptr;
        uint64_t *propertySegmentHead_ = nullptr;
        uint64_t *blobSegmentHead_ = nullptr;
        uint64_t *indexSegmentHead_ = nullptr;
        uint64_t *stateSegmentHead_ = nullptr;

        // Use shared_mutex for high concurrent read performance
        mutable std::shared_mutex mutex_;

        void buildSegmentIndex(std::vector<SegmentIndex> &segmentIndex, uint64_t segmentHead) const;

        std::vector<SegmentIndex> &getSegmentIndexForType(uint32_t type);
        const std::vector<SegmentIndex> &getSegmentIndexForType(uint32_t type) const;

        static void sortSegmentIndex(std::vector<SegmentIndex> &segmentIndex);
    };

} // namespace graph::storage