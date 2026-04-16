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

#include "graph/log/Log.hpp"
#include "graph/storage/SegmentTracker.hpp"

using graph::log::Log;

namespace graph::storage {

	// =========================================================
	// Construction
	// =========================================================

	IDAllocator::IDAllocator(EntityType entityType, std::shared_ptr<SegmentTracker> segmentTracker, int64_t &maxId) :
		entityType_(entityType), segmentTracker_(std::move(segmentTracker)), maxId_(maxId) {}

	// =========================================================
	// Core Operations
	// =========================================================

	int64_t IDAllocator::allocate() {
		std::lock_guard lock(mutex_);

		// 1. Volatile Cache
		if (!volatileCache_.empty()) {
			int64_t id = volatileCache_.pop();
			Log::debug("IDAllocator[{}]: Returning ID {} from VolatileCache", toUnderlying(entityType_), id);
			return id;
		}

		// 2. L1 Hot Cache (LIFO stack)
		if (!hotCache_.empty()) {
			int64_t id = hotCache_.back();
			hotCache_.pop_back();
			Log::debug("IDAllocator[{}]: Found ID {} in L1 Cache", toUnderlying(entityType_), id);
			return id;
		}

		// 3. L2 Cold Cache
		if (!coldCache_.empty()) {
			int64_t id = coldCache_.pop();
			Log::debug("IDAllocator[{}]: Found ID {} in L2 Cache", toUnderlying(entityType_), id);
			return id;
		}

		// 4. Disk Scan
		if (!scanCursor_.diskExhausted) {
			if (fetchInactiveIdsFromDisk()) {
				if (!hotCache_.empty()) {
					int64_t id = hotCache_.back();
					hotCache_.pop_back();
					Log::debug("IDAllocator[{}]: Found ID {} via Disk Scan", toUnderlying(entityType_), id);
					return id;
				}
			}
		}

		// 5. Sequential
		return ++maxId_;
	}

	int64_t IDAllocator::allocateBatch(size_t count) {
		if (count == 0)
			return 0;

		std::lock_guard lock(mutex_);

		int64_t startId = maxId_ + 1;
		maxId_ += static_cast<int64_t>(count);

		return startId;
	}

	void IDAllocator::free(int64_t id) {
		std::lock_guard lock(mutex_);
		validateId(id);

		uint64_t segmentOffset =
				segmentTracker_->getSegmentOffsetForEntityId(entityType_, id);

		bool isPersisted = false;
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
					Log::debug("IDAllocator[{}]: Deactivated ID {} on disk.", toUnderlying(entityType_), id);
				} else {
					if (isWithinUsedRange) {
						Log::debug("IDAllocator[{}]: ID {} is already inactive (Double Free). Skipping.",
								   toUnderlying(entityType_), id);
						return;
					}
					isDirtyInPreAlloc = true;
				}
			}
		}

		if (segmentOffset == 0 || isDirtyInPreAlloc) {
			if (volatileCache_.intervalCount() < volatileMaxIntervals_) {
				volatileCache_.add(id);
				Log::debug("IDAllocator[{}]: Added ID {} to VolatileCache", toUnderlying(entityType_), id);
			} else {
				Log::warn("IDAllocator[{}]: VolatileCache full, dropping ID {}. Gap created.",
						  toUnderlying(entityType_), id);
			}
		} else if (isPersisted) {
			if (hotCache_.size() < l1CacheSize_) {
				hotCache_.push_back(id);
			} else if (coldCache_.intervalCount() < l2MaxIntervals_) {
				coldCache_.add(id);
			}
		}
	}

	// =========================================================
	// Lifecycle
	// =========================================================

	void IDAllocator::initialize() {
		clearPersistedCaches();
		int64_t physicalMaxId = scanPhysicalMaxId();
		recoverGaps(physicalMaxId);
	}

	void IDAllocator::initializeFromScan(int64_t physicalMaxId) {
		recoverGaps(physicalMaxId);
	}

	void IDAllocator::clearPersistedCaches() {
		std::lock_guard lock(mutex_);
		hotCache_.clear();
		coldCache_.clear();
		scanCursor_ = ScanCursor{};
	}

	void IDAllocator::resetAfterCompaction() {
		std::lock_guard lock(mutex_);
		Log::info("IDAllocator[{}]: Resetting ALL caches after compaction.", toUnderlying(entityType_));
		hotCache_.clear();
		coldCache_.clear();
		volatileCache_.clear();
		scanCursor_ = ScanCursor{};
	}

	void IDAllocator::ensureMaxId(int64_t id) {
		std::lock_guard lock(mutex_);
		if (id > maxId_) {
			maxId_ = id;
		}
	}

	// =========================================================
	// Query
	// =========================================================

	int64_t IDAllocator::getCurrentMaxId() const {
		std::lock_guard lock(mutex_);
		return maxId_;
	}

	// =========================================================
	// Testing Support
	// =========================================================

	void IDAllocator::setCacheLimits(size_t l1Size, size_t l2Intervals, size_t volatileIntervals) {
		std::lock_guard lock(mutex_);
		l1CacheSize_ = l1Size;
		l2MaxIntervals_ = l2Intervals;
		volatileMaxIntervals_ = volatileIntervals;
	}

	// =========================================================
	// Private Helpers
	// =========================================================

	void IDAllocator::validateId(int64_t id) const {
		if (id <= 0) {
			throw std::runtime_error("IDAllocator: Invalid ID " + std::to_string(id));
		}
	}

	int64_t IDAllocator::scanPhysicalMaxId() const {
		int64_t physicalMax = 0;
		uint64_t offset = segmentTracker_->getChainHead(toUnderlying(entityType_));

		while (offset != 0) {
			SegmentHeader header = segmentTracker_->getSegmentHeader(offset);
			if (header.used > 0) {
				int64_t segmentUsedEndId = header.start_id + header.used - 1;
				physicalMax = std::max(physicalMax, segmentUsedEndId);
			}
			offset = header.next_segment_offset;
		}

		return physicalMax;
	}

	void IDAllocator::recoverGaps(int64_t physicalMaxId) {
		if (maxId_ > physicalMaxId) {
			int64_t gapStart = physicalMaxId + 1;
			int64_t gapEnd = maxId_;
			volatileCache_.addRange(gapStart, gapEnd);
			Log::info("IDAllocator[{}]: Recovered gaps [{}, {}]", toUnderlying(entityType_), gapStart, gapEnd);
		} else if (physicalMaxId > maxId_) {
			Log::warn("IDAllocator[{}]: Detected stale header. Logical: {}, Physical: {}. Correcting to Physical Max.",
					  toUnderlying(entityType_), maxId_, physicalMaxId);
			maxId_ = physicalMaxId;
		}
	}

	bool IDAllocator::fetchInactiveIdsFromDisk() {
		if (scanCursor_.diskExhausted)
			return false;

		uint64_t chainHead = segmentTracker_->getChainHead(toUnderlying(entityType_));

		if (scanCursor_.nextSegmentOffset == 0) {
			scanCursor_.nextSegmentOffset = chainHead;
			scanCursor_.wrappedAround = (chainHead == 0);
		}

		if (scanCursor_.nextSegmentOffset == 0) {
			scanCursor_.diskExhausted = true;
			return false;
		}

		uint64_t currentOffset = scanCursor_.nextSegmentOffset;
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
						hotCache_.push_back(recoveredId);
						foundAny = true;

						if (hotCache_.size() >= l1CacheSize_)
							break;
					}
				}
			}

			if (hotCache_.size() >= l1CacheSize_) {
				scanCursor_.nextSegmentOffset = header.next_segment_offset;
				return true;
			}

			currentOffset = header.next_segment_offset;
			scannedCount++;
		}

		if (currentOffset == 0) {
			if (scanCursor_.wrappedAround) {
				scanCursor_.diskExhausted = true;
				scanCursor_.nextSegmentOffset = 0;
				scanCursor_.wrappedAround = false;
			} else {
				scanCursor_.wrappedAround = true;
				scanCursor_.nextSegmentOffset = chainHead;
			}
		} else {
			scanCursor_.nextSegmentOffset = currentOffset;
		}

		return foundAny;
	}

} // namespace graph::storage
