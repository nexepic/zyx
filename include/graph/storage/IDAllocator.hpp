/**
 * @file IDAllocator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/19
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <map>
#include <vector>

namespace graph::storage {

    class SegmentTracker;

    /**
     * @brief Manages entity ID allocation with OOM protection.
     */
    class IDAllocator {
    public:
        // L1 Cache Limit: Fast access for immediate reuse
        static constexpr size_t L1_CACHE_SIZE = 1024;

        // L2 Cache Limit: Number of INTERVALS (not IDs).
        // 10,000 intervals can represent billions of continuous IDs with negligible memory.
        static constexpr size_t L2_MAX_INTERVALS = 10000;

        // Batch size for reading from disk
        static constexpr size_t DISK_READ_BATCH = 1024;

        explicit IDAllocator(std::shared_ptr<std::fstream> file,
                             std::shared_ptr<SegmentTracker> segmentTracker,
                             int64_t& maxNodeId, int64_t& maxEdgeId,
                             int64_t& maxPropId, int64_t& maxBlobId,
                             int64_t& maxIndexId, int64_t& maxStateId);

        /**
         * @brief Allocates an ID from L1 -> L2 -> Disk -> New Sequence.
         */
        int64_t allocateId(uint32_t entityType);

        /**
         * @brief Frees an ID, promoting it to caches.
         * Handles interval merging to save memory.
         */
        void freeId(int64_t id, uint32_t entityType);

        /**
         * @brief Resets caches but preserves cursors for lazy loading.
         */
        void initialize();
        void clearAllCaches();
        void clearCache(uint32_t entityType);

        // Getters
        [[nodiscard]] int64_t getCurrentMaxNodeId() const { return currentMaxNodeId_; }
        [[nodiscard]] int64_t getCurrentMaxEdgeId() const { return currentMaxEdgeId_; }
        [[nodiscard]] int64_t getCurrentMaxPropId() const { return currentMaxPropId_; }
        [[nodiscard]] int64_t getCurrentMaxBlobId() const { return currentMaxBlobId_; }
        [[nodiscard]] int64_t getCurrentMaxIndexId() const { return currentMaxIndexId_; }
        [[nodiscard]] int64_t getCurrentMaxStateId() const { return currentMaxStateId_; }

    private:
        /**
         * @brief Helper class to store ID ranges compactly (e.g., [1-1000] is one entry).
         * Prevents OOM when deleting massive sequential data.
         */
        class IDIntervalSet {
        public:
            // Adds an ID, merging with existing intervals if possible
            void add(int64_t id);
        	void addRange(int64_t start, int64_t end);

            // Retrieves and removes the first available ID
            // Returns 0 if empty
            int64_t pop();

            // Returns total number of intervals (for memory capping)
            [[nodiscard]] size_t intervalCount() const { return intervals_.size(); }

            [[nodiscard]] bool empty() const { return intervals_.empty(); }
            void clear() { intervals_.clear(); }

        private:
            // Map: StartID -> EndID
            // Using map keeps ranges sorted, allowing efficient O(log N) merging.
            std::map<int64_t, int64_t> intervals_;
        };

        struct ScanCursor {
            uint64_t nextSegmentOffset = 0;
            bool diskExhausted = false;
            bool wrappedAround = false;
        };

        // Attempt to fetch IDs from disk into L1 cache
        bool fetchInactiveIdsFromDisk(uint32_t entityType);

        int64_t allocateNewSequentialId(uint32_t entityType) const;

    	void recoverGapIds(uint32_t entityType, int64_t currentMaxId);

        std::shared_ptr<std::fstream> file_;
        std::shared_ptr<SegmentTracker> segmentTracker_;

        // L1 Cache: Immediate reuse (Discrete IDs)
        std::unordered_map<uint32_t, std::deque<int64_t>> hotCache_;

        // L2 Cache: Compact storage (Intervals)
        std::unordered_map<uint32_t, IDIntervalSet> coldCache_;

    	std::unordered_map<uint32_t, std::deque<int64_t>> memoryOnlyCache_;

        std::unordered_map<uint32_t, ScanCursor> scanCursors_;

        int64_t& currentMaxNodeId_;
        int64_t& currentMaxEdgeId_;
        int64_t& currentMaxPropId_;
        int64_t& currentMaxBlobId_;
        int64_t& currentMaxIndexId_;
        int64_t& currentMaxStateId_;

        mutable std::mutex mutex_;
    };

} // namespace graph::storage