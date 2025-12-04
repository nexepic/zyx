/**
 * @file IDAllocator.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/19
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include <algorithm>
#include <stdexcept>

namespace graph::storage {

    // =========================================================
    // IDIntervalSet Implementation (The Memory Optimizer)
    // =========================================================

    void IDAllocator::IDIntervalSet::add(int64_t id) {
        if (intervals_.empty()) {
            intervals_[id] = id;
            return;
        }

        // Find the first interval starting AFTER id
        auto it = intervals_.upper_bound(id);

        // Check merge with NEXT interval (id + 1 == next.start)
        bool mergeNext = (it != intervals_.end() && it->first == id + 1);

        // Check merge with PREV interval (prev.end + 1 == id)
        bool mergePrev = false;
        auto prev = it;
        if (it != intervals_.begin()) {
            prev = std::prev(it);
            if (prev->second == id - 1) {
                mergePrev = true;
            } else if (prev->second >= id) {
                // ID already exists in range (Duplicate free?)
                return;
            }
        }

        if (mergePrev && mergeNext) {
            // Merge all three: [prev] + id + [next]
            // Extend prev to cover next
            prev->second = it->second;
            intervals_.erase(it);
        } else if (mergePrev) {
            // Extend prev: [prev] + id
            prev->second = id;
        } else if (mergeNext) {
            // Extend next backwards: id + [next]
            int64_t end = it->second;
            intervals_.erase(it);
            intervals_[id] = end;
        } else {
            // No merge, new isolated interval
            intervals_[id] = id;
        }
    }

    int64_t IDAllocator::IDIntervalSet::pop() {
        if (intervals_.empty()) return 0;

        // Take the first available interval
        auto it = intervals_.begin();
        int64_t start = it->first;
        int64_t end = it->second;

        int64_t idToReturn = start;

        if (start == end) {
            // Interval exhausted, remove it
            intervals_.erase(it);
        } else {
            // Shrink interval from the left
            // Optimizing map key modification: remove and re-insert is necessary
            // because keys are const.
            intervals_.erase(it);
            intervals_[start + 1] = end;
        }

        return idToReturn;
    }

    // =========================================================
    // IDAllocator Implementation
    // =========================================================

    IDAllocator::IDAllocator(std::shared_ptr<std::fstream> file,
                             std::shared_ptr<SegmentTracker> segmentTracker,
                             int64_t& maxNodeId, int64_t& maxEdgeId,
                             int64_t& maxPropId, int64_t& maxBlobId,
                             int64_t& maxIndexId, int64_t& maxStateId)
        : file_(std::move(file)),
          segmentTracker_(std::move(segmentTracker)),
          currentMaxNodeId_(maxNodeId),
          currentMaxEdgeId_(maxEdgeId),
          currentMaxPropId_(maxPropId),
          currentMaxBlobId_(maxBlobId),
          currentMaxIndexId_(maxIndexId),
          currentMaxStateId_(maxStateId) {}

    void IDAllocator::initialize() {
        clearAllCaches();
    }

    void IDAllocator::clearAllCaches() {
        std::lock_guard<std::mutex> lock(mutex_);
        hotCache_.clear();
        coldCache_.clear();
        scanCursors_.clear();
    }

    void IDAllocator::clearCache(uint32_t entityType) {
        std::lock_guard<std::mutex> lock(mutex_);
        hotCache_[entityType].clear();
        coldCache_[entityType].clear();
        scanCursors_[entityType] = ScanCursor{0, false, false};
    }

    int64_t IDAllocator::allocateId(uint32_t entityType) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto& l1 = hotCache_[entityType];
        auto& l2 = coldCache_[entityType];

        int64_t recycledId = 0;

        // Strategy: L1 -> L2 -> Disk -> Sequence

        // 1. Check L1 (Hot)
        if (!l1.empty()) {
            recycledId = l1.front();
            l1.pop_front();
        }
        // 2. Check L2 (Cold Intervals)
        else if (!l2.empty()) {
            recycledId = l2.pop();
        }
        // 3. Check Disk (Lazy Load)
        else {
            auto& cursor = scanCursors_[entityType];
            if (!cursor.diskExhausted) {
                // Try to fill L1 from disk
                if (fetchInactiveIdsFromDisk(entityType)) {
                    // Fetch succeeded, take one from L1
                    recycledId = l1.front();
                    l1.pop_front();
                }
            }
        }

        // 4. Validate and Return
        if (recycledId != 0) {
            // Validate against SegmentTracker (Safety Check)
            uint64_t segmentOffset = 0;
            if (entityType == Node::typeId) segmentOffset = segmentTracker_->getSegmentOffsetForNodeId(recycledId);
            else if (entityType == Edge::typeId) segmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(recycledId);
            else if (entityType == Property::typeId) segmentOffset = segmentTracker_->getSegmentOffsetForPropId(recycledId);
            else if (entityType == Blob::typeId) segmentOffset = segmentTracker_->getSegmentOffsetForBlobId(recycledId);
            else if (entityType == Index::typeId) segmentOffset = segmentTracker_->getSegmentOffsetForIndexId(recycledId);
            else if (entityType == State::typeId) segmentOffset = segmentTracker_->getSegmentOffsetForStateId(recycledId);

            if (segmentOffset != 0) {
                SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);
                if (recycledId >= header.start_id && recycledId < header.start_id + header.capacity) {
                    auto index = static_cast<uint32_t>(recycledId - header.start_id);
                    segmentTracker_->setEntityActive(segmentOffset, index, true);
                    return recycledId;
                }
            }
            // If validation fails, fall through to allocate new
        }

        // 5. Fallback: New Sequential ID
        return allocateNewSequentialId(entityType);
    }

    void IDAllocator::freeId(int64_t id, uint32_t entityType) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 1. Mark in SegmentTracker
        uint64_t segmentOffset = 0;
        if (entityType == Node::typeId) segmentOffset = segmentTracker_->getSegmentOffsetForNodeId(id);
        else if (entityType == Edge::typeId) segmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(id);
        else if (entityType == Property::typeId) segmentOffset = segmentTracker_->getSegmentOffsetForPropId(id);
        else if (entityType == Blob::typeId) segmentOffset = segmentTracker_->getSegmentOffsetForBlobId(id);
        else if (entityType == Index::typeId) segmentOffset = segmentTracker_->getSegmentOffsetForIndexId(id);
        else if (entityType == State::typeId) segmentOffset = segmentTracker_->getSegmentOffsetForStateId(id);

        if (segmentOffset == 0) return;

        SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);
        if (id < header.start_id || id >= header.start_id + header.capacity) return;
        auto index = static_cast<uint32_t>(id - header.start_id);

        if (segmentTracker_->isEntityActive(segmentOffset, index)) {
            segmentTracker_->setEntityActive(segmentOffset, index, false);

            // 2. Add to Cache Hierarchy
            auto& l1 = hotCache_[entityType];
            auto& l2 = coldCache_[entityType];

            if (l1.size() < L1_CACHE_SIZE) {
                // Keep hot cache filled with most recently freed (LIFO for locality)
                l1.push_front(id);
            } else {
                // Overflow to L2 Cold Cache
                // Move the oldest from L1 to L2 to make room, or just push current to L2?
                // Strategy: Push current ID to L2. Since L2 merges intervals,
                // it is efficient for sequential deletes.

                if (l2.intervalCount() < L2_MAX_INTERVALS) {
                    l2.add(id);
                } else {
                    // L2 Full: Drop the ID.
                    // It is safely recorded in Disk Bitmap (SegmentTracker).
                    // fetchInactiveIdsFromDisk will find it later if needed.
                    // This guarantees we never OOM.
                }
            }
        }
    }

    bool IDAllocator::fetchInactiveIdsFromDisk(uint32_t entityType) {
        auto& cursor = scanCursors_[entityType];
        auto& l1 = hotCache_[entityType];

        if (cursor.diskExhausted) return false;

        uint64_t chainHead = segmentTracker_->getChainHead(entityType);

        // Initialization logic (Same as improved version)
        if (cursor.nextSegmentOffset == 0) {
            if (!cursor.wrappedAround) {
                cursor.nextSegmentOffset = chainHead;
                cursor.wrappedAround = true;
            } else {
                cursor.nextSegmentOffset = chainHead;
                cursor.wrappedAround = true;
            }
        }

        if (cursor.nextSegmentOffset == 0) {
            cursor.diskExhausted = true;
            return false;
        }

        uint64_t currentOffset = cursor.nextSegmentOffset;
        int scannedCount = 0;
		constexpr int MAX_SCANS_PER_CALL = 20; // Reduced scans per call to stay responsive
        bool foundAny = false;

        while (currentOffset != 0 && scannedCount < MAX_SCANS_PER_CALL) {
            SegmentHeader header = segmentTracker_->getSegmentHeader(currentOffset);

            if (header.inactive_count > 0) {
                std::vector<bool> bitmap = segmentTracker_->getActivityBitmap(currentOffset);
                for (uint32_t i = 0; i < header.used; ++i) {
                    if (!bitmap[i]) {
                        int64_t recoveredId = header.start_id + i;
                        l1.push_back(recoveredId);
                        foundAny = true;
                    }
                }
            }

            // Stop if L1 is full enough
            if (l1.size() >= L1_CACHE_SIZE) {
                cursor.nextSegmentOffset = header.next_segment_offset;
                return true;
            }

            currentOffset = header.next_segment_offset;
            scannedCount++;
        }

        if (currentOffset == 0) {
            if (cursor.wrappedAround) {
                cursor.diskExhausted = true;
                cursor.nextSegmentOffset = 0;
                cursor.wrappedAround = false;
            } else {
                cursor.wrappedAround = true;
                cursor.nextSegmentOffset = chainHead;
            }
        } else {
            cursor.nextSegmentOffset = currentOffset;
        }

        return foundAny;
    }

    int64_t IDAllocator::allocateNewSequentialId(uint32_t entityType) const {
        if (entityType == Node::typeId) return ++currentMaxNodeId_;
        if (entityType == Edge::typeId) return ++currentMaxEdgeId_;
        if (entityType == Property::typeId) return ++currentMaxPropId_;
        if (entityType == Blob::typeId) return ++currentMaxBlobId_;
        if (entityType == Index::typeId) return ++currentMaxIndexId_;
        if (entityType == State::typeId) return ++currentMaxStateId_;
        throw std::runtime_error("Invalid entity type");
    }

} // namespace graph::storage