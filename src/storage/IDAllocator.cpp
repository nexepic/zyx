/**
 * @file IDAllocator.cpp
 * @author Nexepic
 * @date 2025/4/19
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

		// Optimization: Check if we can simply append to the last interval
		if (!intervals_.empty()) {
			auto lastIt = intervals_.rbegin();
			if (lastIt->second == start - 1) {
				lastIt->second = end; // Extend last interval
				return;
			}
		}

		// General Insert & Merge Logic
		auto it = intervals_.upper_bound(start);

		// Check overlap/adjacency with previous interval
		if (it != intervals_.begin()) {
			auto prev = std::prev(it);
			if (prev->second >= start - 1) {
				start = prev->first;
				end = std::max(end, prev->second);
				intervals_.erase(prev);
			}
		}

		// Check overlap/adjacency with subsequent intervals
		while (it != intervals_.end() && it->first <= end + 1) {
			end = std::max(end, it->second);
			it = intervals_.erase(it);
		}

		intervals_[start] = end;
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
			intervals_.emplace_hint(intervals_.begin(), start + 1, end);
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
		// Clear caches but keep memory structure ready
		clearAllCaches();

		// [Robustness] Recover "Ghost Gaps" AND Synchronize Counters
		recoverGapIds(Node::typeId, currentMaxNodeId_);
		recoverGapIds(Edge::typeId, currentMaxEdgeId_);
		recoverGapIds(Property::typeId, currentMaxPropId_);
		recoverGapIds(Blob::typeId, currentMaxBlobId_);
		recoverGapIds(Index::typeId, currentMaxIndexId_);
		recoverGapIds(State::typeId, currentMaxStateId_);
	}

	void IDAllocator::recoverGapIds(uint32_t entityType, int64_t &logicalMaxId) {
		int64_t physicalMaxId = 0;
		uint64_t currentOffset = segmentTracker_->getChainHead(entityType);

		// 1. Scan Disk to find the TRUE physical max ID
		while (currentOffset != 0) {
			SegmentHeader header = segmentTracker_->getSegmentHeader(currentOffset);

			if (header.used > 0) {
				int64_t segmentUsedEndId = header.start_id + header.used - 1;
				if (segmentUsedEndId > physicalMaxId) {
					physicalMaxId = segmentUsedEndId;
				}
			}

			currentOffset = header.next_segment_offset;
		}

		// 2. CASE A: Recover Gaps (Normal Restart)
		if (logicalMaxId > physicalMaxId) {
			int64_t gapStart = physicalMaxId + 1;
			int64_t gapEnd = logicalMaxId;

			volatileCache_[entityType].addRange(gapStart, gapEnd);

			// Log format updated to use {} placeholders
			Log::info("IDAllocator: Recovered gaps for Type {}: [{}, {}]", entityType, gapStart, gapEnd);
		}
		// 3. CASE B: Fix Stale Header (Crash Recovery)
		else if (physicalMaxId > logicalMaxId) {
			// Log format updated
			Log::warn("IDAllocator: Detected stale header for Type {}. Logical: {}, Physical: {}. Correcting to "
					  "Physical Max.",
					  entityType, logicalMaxId, physicalMaxId);
			logicalMaxId = physicalMaxId;
		}
	}

	void IDAllocator::initializeFromScan(uint32_t entityType, int64_t physicalMaxId) {
		int64_t &logicalMaxId = getMaxIdRef(entityType);

		// Same gap recovery logic as recoverGapIds, but without the chain walk
		if (logicalMaxId > physicalMaxId) {
			int64_t gapStart = physicalMaxId + 1;
			int64_t gapEnd = logicalMaxId;
			volatileCache_[entityType].addRange(gapStart, gapEnd);
			Log::info("IDAllocator: Recovered gaps for Type {}: [{}, {}]", entityType, gapStart, gapEnd);
		} else if (physicalMaxId > logicalMaxId) {
			Log::warn("IDAllocator: Detected stale header for Type {}. Logical: {}, Physical: {}. Correcting to "
					  "Physical Max.",
					  entityType, logicalMaxId, physicalMaxId);
			logicalMaxId = physicalMaxId;
		}
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

	void IDAllocator::resetAfterCompaction() {
		std::lock_guard<std::mutex> lock(mutex_);

		Log::info("IDAllocator: Resetting ALL caches after compaction.");

		hotCache_.clear();
		coldCache_.clear();
		volatileCache_.clear();
		scanCursors_.clear();
	}

	int64_t IDAllocator::allocateId(const uint32_t entityType) {
		std::lock_guard<std::mutex> lock(mutex_);

		auto &volCache = volatileCache_[entityType];
		auto &hot = hotCache_[entityType];
		auto &cold = coldCache_[entityType];

		int64_t recycledId = 0;

		// 1. Check Volatile Cache
		if (!volCache.empty()) {
			recycledId = volCache.pop();
			// Log format updated
			Log::debug("allocateId: Returning ID {} from VolatileCache", recycledId);
			return recycledId;
		}

		// 2. Check L1 Hot Cache
		if (!hot.empty()) {
			recycledId = hot.front();
			hot.pop_front();
			// Log format updated
			Log::debug("allocateId: Found ID {} in L1 Cache", recycledId);
		}
		// 3. Check L2 Cold Cache
		else if (!cold.empty()) {
			recycledId = cold.pop();
			// Log format updated
			Log::debug("allocateId: Found ID {} in L2 Cache", recycledId);
		}
		// 4. Check Disk
		else {
			auto &cursor = scanCursors_[entityType];
			if (!cursor.diskExhausted) {
				if (fetchInactiveIdsFromDisk(entityType)) {
					if (!hot.empty()) {
						recycledId = hot.front();
						hot.pop_front();
						// Log format updated
						Log::debug("allocateId: Found ID {} via Disk Scan", recycledId);
					}
				}
			}
		}

		// 5. Validate and Return
		if (recycledId != 0) {
			return recycledId;
		}

		// 6. Fallback: New Sequential ID
		return allocateNewSequentialId(entityType);
	}

	int64_t IDAllocator::allocateIdBatch(const uint32_t entityType, size_t count) {
		if (count == 0)
			return 0;

		std::lock_guard<std::mutex> lock(mutex_);

		int64_t &maxId = getMaxIdRef(entityType);
		int64_t startId = maxId + 1;
		maxId += static_cast<int64_t>(count);

		return startId;
	}

	void IDAllocator::freeId(const int64_t id, const uint32_t entityType) {
		std::lock_guard<std::mutex> lock(mutex_);

		bool isPersisted = false;
		uint64_t segmentOffset = 0;

		switch (entityType) {
			case Node::typeId:
				segmentOffset = segmentTracker_->getSegmentOffsetForNodeId(id);
				break;
			case Edge::typeId:
				segmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(id);
				break;
			case Property::typeId:
				segmentOffset = segmentTracker_->getSegmentOffsetForPropId(id);
				break;
			case Blob::typeId:
				segmentOffset = segmentTracker_->getSegmentOffsetForBlobId(id);
				break;
			case Index::typeId:
				segmentOffset = segmentTracker_->getSegmentOffsetForIndexId(id);
				break;
			case State::typeId:
				segmentOffset = segmentTracker_->getSegmentOffsetForStateId(id);
				break;
			default:;
		}

		bool isDirtyInPreAlloc = false;

		if (segmentOffset != 0) {
			SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);

			if (id >= header.start_id && id < header.start_id + header.capacity) {
				auto index = static_cast<uint32_t>(id - header.start_id);
				bool isActive = segmentTracker_->isEntityActive(segmentOffset, index);
				bool isWithinUsedRange = segmentTracker_->isIdInUsedRange(segmentOffset, id);

				if (isActive) {
					segmentTracker_->setEntityActive(segmentOffset, index, false);
					isPersisted = true;
					// Log format updated
					Log::debug("freeId: Deactivated ID {} on disk.", id);
				} else {
					if (isWithinUsedRange) {
						// Log format updated
						Log::debug("freeId: ID {} is already inactive (Double Free). Skipping.", id);
						return;
					} else {
						isDirtyInPreAlloc = true;
						isPersisted = false;
					}
				}
			}
		}

		if (segmentOffset == 0 || isDirtyInPreAlloc) {
			auto &volCache = volatileCache_[entityType];

			if (volCache.intervalCount() < volatileMaxIntervals_) {
				volCache.add(id);
				// Log format updated
				Log::debug("freeId: Added ID {} to VolatileCache", id);
			} else {
				// Log format updated
				Log::warn("freeId: VolatileCache full, dropping ID {}. Gap created.", id);
			}
		} else if (isPersisted) {
			auto &l1 = hotCache_[entityType];
			auto &l2 = coldCache_[entityType];

			if (l1.size() < l1CacheSize_) {
				l1.push_front(id);
			} else if (l2.intervalCount() < l2MaxIntervals_) {
				l2.add(id);
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

						if (l1.size() >= l1CacheSize_)
							break;
					}
				}
			}

			if (l1.size() >= l1CacheSize_) {
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

	int64_t IDAllocator::allocateNewSequentialId(uint32_t entityType) {
		return ++getMaxIdRef(entityType);
	}

	int64_t &IDAllocator::getMaxIdRef(uint32_t entityType) {
		switch (entityType) {
			case Node::typeId: return currentMaxNodeId_;
			case Edge::typeId: return currentMaxEdgeId_;
			case Property::typeId: return currentMaxPropId_;
			case Blob::typeId: return currentMaxBlobId_;
			case Index::typeId: return currentMaxIndexId_;
			case State::typeId: return currentMaxStateId_;
			default: throw std::runtime_error("Invalid entity type: " + std::to_string(entityType));
		}
	}

} // namespace graph::storage
