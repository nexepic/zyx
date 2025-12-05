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
#include <algorithm>
#include <stdexcept>
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/log/Log.hpp"
#include "graph/storage/SegmentTracker.hpp"

using graph::log::Log;

namespace graph::storage {

	// =========================================================
	// IDIntervalSet Implementation
	// =========================================================

	void IDAllocator::IDIntervalSet::add(int64_t id) { addRange(id, id); }

	void IDAllocator::IDIntervalSet::addRange(int64_t start, int64_t end) {
		if (start > end)
			return;

		if (intervals_.empty()) {
			intervals_[start] = end;
			return;
		}

		if (start == end) {
			auto it = intervals_.upper_bound(start);
			bool mergeNext = (it != intervals_.end() && it->first == start + 1);

			bool mergePrev = false;
			decltype(it) prev;
			if (it != intervals_.begin()) {
				prev = std::prev(it);
				if (prev->second == start - 1) {
					mergePrev = true;
				} else if (prev->second >= start) {
					return; // Already exists
				}
			}

			if (mergePrev && mergeNext) {
				prev->second = it->second;
				intervals_.erase(it);
			} else if (mergePrev) {
				prev->second = start;
			} else if (mergeNext) {
				int64_t nextEnd = it->second;
				intervals_.erase(it);
				intervals_[start] = nextEnd;
			} else {
				intervals_[start] = start;
			}
		} else {
			intervals_[start] = end;
		}
	}

	int64_t IDAllocator::IDIntervalSet::pop() {
		if (intervals_.empty())
			return 0;

		auto it = intervals_.begin();
		int64_t start = it->first;
		int64_t end = it->second;
		int64_t idToReturn = start;

		if (start == end) {
			intervals_.erase(it);
		} else {
			intervals_.erase(it);
			intervals_[start + 1] = end;
		}

		return idToReturn;
	}

	// =========================================================
	// IDAllocator Implementation
	// =========================================================

	IDAllocator::IDAllocator(std::shared_ptr<std::fstream> file, std::shared_ptr<SegmentTracker> segmentTracker,
							 int64_t &maxNodeId, int64_t &maxEdgeId, int64_t &maxPropId, int64_t &maxBlobId,
							 int64_t &maxIndexId, int64_t &maxStateId) :
		file_(std::move(file)), segmentTracker_(std::move(segmentTracker)), currentMaxNodeId_(maxNodeId),
		currentMaxEdgeId_(maxEdgeId), currentMaxPropId_(maxPropId), currentMaxBlobId_(maxBlobId),
		currentMaxIndexId_(maxIndexId), currentMaxStateId_(maxStateId) {}

	void IDAllocator::initialize() {
		clearAllCaches(); // This keeps memoryOnlyCache_

		// [Robustness] Recover "Ghost Gaps"
		recoverGapIds(Node::typeId, currentMaxNodeId_);
		recoverGapIds(Edge::typeId, currentMaxEdgeId_);
		recoverGapIds(Property::typeId, currentMaxPropId_);
		recoverGapIds(Blob::typeId, currentMaxBlobId_);
		recoverGapIds(Index::typeId, currentMaxIndexId_);
		recoverGapIds(State::typeId, currentMaxStateId_);
	}

	void IDAllocator::recoverGapIds(uint32_t entityType, int64_t logicalMaxId) {
		if (logicalMaxId == 0)
			return;

		int64_t physicalMaxId = 0;
		uint64_t currentOffset = segmentTracker_->getChainHead(entityType);

		while (currentOffset != 0) {
			SegmentHeader header = segmentTracker_->getSegmentHeader(currentOffset);
			int64_t segmentEndId = header.start_id + header.capacity - 1;
			if (segmentEndId > physicalMaxId) {
				physicalMaxId = segmentEndId;
			}
			currentOffset = header.next_segment_offset;
		}

		if (logicalMaxId > physicalMaxId) {
			int64_t gapStart = physicalMaxId + 1;
			int64_t gapEnd = logicalMaxId;
			Log::debug("Recovering Gaps for Type ", entityType, ": [", gapStart, ", ", gapEnd, "]");
			coldCache_[entityType].addRange(gapStart, gapEnd);
		}
	}

	void IDAllocator::clearAllCaches() {
		std::lock_guard<std::mutex> lock(mutex_);
		hotCache_.clear();
		coldCache_.clear();
		scanCursors_.clear();
		// NOTE: memoryOnlyCache_ is intentionally preserved!
	}

	void IDAllocator::clearCache(uint32_t entityType) {
		std::lock_guard<std::mutex> lock(mutex_);
		hotCache_[entityType].clear();
		coldCache_[entityType].clear();
		scanCursors_[entityType] = ScanCursor{0, false, false};
		// NOTE: memoryOnlyCache_ is intentionally preserved!
	}

	int64_t IDAllocator::allocateId(const uint32_t entityType) {
		std::lock_guard<std::mutex> lock(mutex_);

		auto &memCache = memoryOnlyCache_[entityType];
		auto &l1 = hotCache_[entityType];
		auto &l2 = coldCache_[entityType];

		int64_t recycledId = 0;

		// 1. Check Memory Only Cache (Highest Priority)
		if (!memCache.empty()) {
			recycledId = memCache.front();
			memCache.pop_front();
			Log::debug("allocateId: Returning ID ", recycledId, " from MemoryOnlyCache");
			return recycledId;
		}

		// 2. Check L1 (Hot)
		if (!l1.empty()) {
			recycledId = l1.front();
			l1.pop_front();
			Log::debug("allocateId: Found ID ", recycledId, " in L1 Cache");
		}
		// 3. Check L2 (Cold Intervals)
		else if (!l2.empty()) {
			recycledId = l2.pop();
			Log::debug("allocateId: Found ID ", recycledId, " in L2 Cache");
		}
		// 4. Check Disk (Lazy Load)
		else {
			auto &cursor = scanCursors_[entityType];
			if (!cursor.diskExhausted) {
				if (fetchInactiveIdsFromDisk(entityType)) {
					recycledId = l1.front();
					l1.pop_front();
					Log::debug("allocateId: Found ID ", recycledId, " via Disk Scan");
				}
			}
		}

		// 5. Validate and Return
		if (recycledId != 0) {
			uint64_t segmentOffset = 0;
			if (entityType == Node::typeId)
				segmentOffset = segmentTracker_->getSegmentOffsetForNodeId(recycledId);
			else if (entityType == Edge::typeId)
				segmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(recycledId);
			else if (entityType == Property::typeId)
				segmentOffset = segmentTracker_->getSegmentOffsetForPropId(recycledId);
			else if (entityType == Blob::typeId)
				segmentOffset = segmentTracker_->getSegmentOffsetForBlobId(recycledId);
			else if (entityType == Index::typeId)
				segmentOffset = segmentTracker_->getSegmentOffsetForIndexId(recycledId);
			else if (entityType == State::typeId)
				segmentOffset = segmentTracker_->getSegmentOffsetForStateId(recycledId);

			if (segmentOffset != 0) {
				// CASE A: Backed by disk
				SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);
				if (recycledId >= header.start_id && recycledId < header.start_id + header.capacity) {
					auto index = static_cast<uint32_t>(recycledId - header.start_id);
					segmentTracker_->setEntityActive(segmentOffset, index, true);
					Log::debug("allocateId: Reused persisted ID ", recycledId, " (Offset: ", segmentOffset, ")");
					return recycledId;
				}
			} else {
				// CASE B: Memory Only / Gap Recovered
				// If we reached here, recycledId is valid but has no offset.
				// This includes IDs from Gap Recovery (L2) or double-check of memory logic.
				Log::debug("allocateId: Returning ID ", recycledId, " (No Offset / Gap Recovered)");
				return recycledId;
			}
		}

		// 6. Fallback: New Sequential ID
		return allocateNewSequentialId(entityType);
	}

	void IDAllocator::freeId(const int64_t id, const uint32_t entityType) {
		std::lock_guard<std::mutex> lock(mutex_);

		// 1. Attempt to update Disk Bitmap
		bool isAlreadyFreedOnDisk = false;
		uint64_t segmentOffset = 0;

		if (entityType == Node::typeId)
			segmentOffset = segmentTracker_->getSegmentOffsetForNodeId(id);
		else if (entityType == Edge::typeId)
			segmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(id);
		else if (entityType == Property::typeId)
			segmentOffset = segmentTracker_->getSegmentOffsetForPropId(id);
		else if (entityType == Blob::typeId)
			segmentOffset = segmentTracker_->getSegmentOffsetForBlobId(id);
		else if (entityType == Index::typeId)
			segmentOffset = segmentTracker_->getSegmentOffsetForIndexId(id);
		else if (entityType == State::typeId)
			segmentOffset = segmentTracker_->getSegmentOffsetForStateId(id);

		bool isDirtyInPreAlloc = false;

		if (segmentOffset != 0) {
			SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);
			if (id >= header.start_id && id < header.start_id + header.capacity) {
				auto index = static_cast<uint32_t>(id - header.start_id);

				// [Optimization] Use the helper method from SegmentTracker
				bool isWithinUsedRange = segmentTracker_->isIdInUsedRange(segmentOffset, id);

				if (segmentTracker_->isEntityActive(segmentOffset, index)) {
					// Normal case: Active on disk -> Set Inactive
					segmentTracker_->setEntityActive(segmentOffset, index, false);
					Log::debug("freeId: Set disk bitmap to Inactive for ID ", id);
				} else {
					// Already inactive on disk
					if (isWithinUsedRange) {
						// True Double Free (was previously used and freed)
						isAlreadyFreedOnDisk = true;
						Log::debug("freeId: ID ", id, " is ALREADY Inactive on disk (Used Range).");
					} else {
						// Dirty Data (Pre-allocated space, never written)
						// We treat this as a valid free for a memory-only ID.
						// Flag it so we put it in the correct cache later.
						isDirtyInPreAlloc = true;
						Log::debug("freeId: ID ", id, " is Inactive but outside Used range (Dirty). Recycling.");
					}
				}
			}
		}

		if (isAlreadyFreedOnDisk) {
			Log::debug("freeId: Skipping cache add for ID ", id, " (Double Free protection)");
			return;
		}

		// 2. Update Cache
		// Case A: No Segment (Pure Memory) OR Case B: Segment exists but ID is outside used range (Pre-alloc)
		if (segmentOffset == 0 || isDirtyInPreAlloc) {
			// [Dirty ID] -> Memory Only Cache
			// Protects against Compaction/Flush clearing it.
			memoryOnlyCache_[entityType].push_front(id);
			Log::debug("freeId: Pushed ID ", id, " to MemoryOnlyCache");
		} else {
			// [Persisted ID] -> L1/L2 Cache
			auto &l1 = hotCache_[entityType];
			auto &l2 = coldCache_[entityType];

			if (l1.size() < L1_CACHE_SIZE) {
				l1.push_front(id);
				Log::debug("freeId: Pushed ID ", id, " to L1 Cache");
			} else {
				if (l2.intervalCount() < L2_MAX_INTERVALS) {
					l2.add(id);
					Log::debug("freeId: Pushed ID ", id, " to L2 Cache");
				}
			}
		}
	}

	bool IDAllocator::fetchInactiveIdsFromDisk(uint32_t entityType) {
		auto &cursor = scanCursors_[entityType];
		auto &l1 = hotCache_[entityType];

		if (cursor.diskExhausted)
			return false;

		uint64_t chainHead = segmentTracker_->getChainHead(entityType);

		if (cursor.nextSegmentOffset == 0) {
			cursor.nextSegmentOffset = chainHead;
			cursor.wrappedAround = (chainHead == 0);
		}

		if (cursor.nextSegmentOffset == 0) {
			cursor.diskExhausted = true;
			return false;
		}

		uint64_t currentOffset = cursor.nextSegmentOffset;
		int scannedCount = 0;
		constexpr int MAX_SCANS_PER_CALL = 20;
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
		if (entityType == Node::typeId)
			return ++currentMaxNodeId_;
		if (entityType == Edge::typeId)
			return ++currentMaxEdgeId_;
		if (entityType == Property::typeId)
			return ++currentMaxPropId_;
		if (entityType == Blob::typeId)
			return ++currentMaxBlobId_;
		if (entityType == Index::typeId)
			return ++currentMaxIndexId_;
		if (entityType == State::typeId)
			return ++currentMaxStateId_;
		throw std::runtime_error("Invalid entity type");
	}

} // namespace graph::storage
