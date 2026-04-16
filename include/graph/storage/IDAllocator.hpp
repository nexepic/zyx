/**
 * @file IDAllocator.hpp
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

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "IntervalSet.hpp"
#include "graph/core/Types.hpp"

namespace graph::storage {

	class SegmentTracker;

	/**
	 * @brief Per-type entity ID allocator with multi-layer caching.
	 *
	 * Cache priority: Volatile → L1 (hot) → L2 (cold) → Disk Scan → Sequential.
	 * - Volatile: IDs allocated but never persisted (rollback/transient).
	 * - L1: Recently freed persisted IDs (LIFO stack for temporal locality).
	 * - L2: Older freed persisted IDs (compact interval storage).
	 * - Disk Scan: Lazy recovery of inactive slots from physical storage.
	 *
	 * Each instance manages exactly ONE entity type. No internal type dispatch.
	 */
	class IDAllocator {
	public:
		static constexpr size_t ENTITY_TYPE_COUNT = static_cast<size_t>(getMaxEntityType()) + 1;

		explicit IDAllocator(EntityType entityType, std::shared_ptr<SegmentTracker> segmentTracker, int64_t &maxId);

		// ── Core Operations ──────────────────────────────────

		/// Allocates an ID: Volatile → L1 → L2 → Disk → Sequential.
		int64_t allocate();

		/// Allocates a contiguous range, bypassing caches for O(1) bulk.
		/// Returns the first ID; range is [return, return + count - 1].
		int64_t allocateBatch(size_t count);

		/// Frees an ID, routing to Volatile (unpersisted) or L1/L2 (persisted).
		void free(int64_t id);

		// ── Lifecycle ────────────────────────────────────────

		/// Full initialization: chain walk + gap recovery. Call at startup.
		void initialize();

		/// Gap recovery only (physicalMaxId provided by caller). Used by StorageBootstrap.
		void initializeFromScan(int64_t physicalMaxId);

		/// Clears L1/L2/cursor. Does NOT clear Volatile (prevents ID leaks).
		void clearPersistedCaches();

		/// Clears ALL caches including Volatile. Only after compaction.
		void resetAfterCompaction();

		/// Ensures maxId counter is at least `id`. Used by WAL replay.
		void ensureMaxId(int64_t id);

		// ── Query ────────────────────────────────────────────

		[[nodiscard]] int64_t getCurrentMaxId() const;
		[[nodiscard]] EntityType getEntityType() const { return entityType_; }

		// ── Testing Support ──────────────────────────────────

		void setCacheLimits(size_t l1Size, size_t l2Intervals, size_t volatileIntervals);

	private:
		void validateId(int64_t id) const;
		int64_t scanPhysicalMaxId() const;
		void recoverGaps(int64_t physicalMaxId);
		bool fetchInactiveIdsFromDisk();

		struct ScanCursor {
			uint64_t nextSegmentOffset = 0;
			bool diskExhausted = false;
			bool wrappedAround = false;
		};

		EntityType entityType_;
		std::shared_ptr<SegmentTracker> segmentTracker_;
		int64_t &maxId_;

		size_t l1CacheSize_ = 4096;
		size_t l2MaxIntervals_ = 10000;
		size_t volatileMaxIntervals_ = 50000;

		IntervalSet volatileCache_;
		std::vector<int64_t> hotCache_;
		IntervalSet coldCache_;
		ScanCursor scanCursor_{};

		mutable std::mutex mutex_;
	};

	/// Convenience alias for the full set of per-type allocators.
	using IDAllocators = std::array<std::shared_ptr<IDAllocator>, IDAllocator::ENTITY_TYPE_COUNT>;

} // namespace graph::storage
