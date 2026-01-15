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

#include <cstdint>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace graph::storage {

	class SegmentTracker;

	/**
	 * @brief Manages entity ID allocation with OOM protection and Interval merging.
	 *
	 * Handles ID lifecycles through multiple cache layers:
	 * 1. Volatile Cache: For IDs allocated but never persisted (rollback/transient).
	 * 2. L1 (Hot) Cache: Recently freed persisted IDs (fast reuse).
	 * 3. L2 (Cold) Cache: Older freed persisted IDs (compact interval storage).
	 * 4. Disk Scan: Lazy recovery of gaps from physical storage.
	 */
	class IDAllocator {
	public:
		void setCacheLimits(size_t l1Size, size_t l2Intervals, size_t volatileIntervals) {
			l1CacheSize_ = l1Size;
			l2MaxIntervals_ = l2Intervals;
			volatileMaxIntervals_ = volatileIntervals;
		}

		explicit IDAllocator(std::shared_ptr<std::fstream> file, std::shared_ptr<SegmentTracker> segmentTracker,
							 int64_t &maxNodeId, int64_t &maxEdgeId, int64_t &maxPropId, int64_t &maxBlobId,
							 int64_t &maxIndexId, int64_t &maxStateId);

		/**
		 * @brief Allocates an ID, prioritizing caches in order: Volatile -> L1 -> L2 -> Disk.
		 * If all caches are empty, allocates a new sequential ID.
		 */
		int64_t allocateId(uint32_t entityType);

		/**
		 * @brief Allocates a contiguous range of IDs efficiently.
		 * Ignores cached/recycled IDs to ensure O(1) performance for bulk operations.
		 * @param entityType The type of entity.
		 * @param count Number of IDs to allocate.
		 * @return The first ID in the allocated range. The range is [return, return + count - 1].
		 */
		int64_t allocateIdBatch(uint32_t entityType, size_t count) const;

		/**
		 * @brief Frees an ID, routing it to the appropriate cache (Volatile vs Persisted).
		 * Detects if the ID was backed by disk storage to decide handling strategy.
		 */
		void freeId(int64_t id, uint32_t entityType);

		/**
		 * @brief Initializes the allocator, recovering logic/physical ID gaps from disk.
		 * Should be called on startup.
		 */
		void initialize();

		/**
		 * @brief Clears caches for persisted IDs (L1/L2).
		 * @note Does NOT clear Volatile cache to prevent ID leaks during normal operation.
		 */
		void clearAllCaches();
		void clearCache(uint32_t entityType);

		/**
		 * @brief Completely resets ALL caches, including Volatile.
		 * MUST ONLY be called after a full database compaction/truncation where
		 * all IDs have been re-mapped to a contiguous range [1..Max].
		 */
		void resetAfterCompaction();

		// Getters
		[[nodiscard]] int64_t getCurrentMaxNodeId() const { return currentMaxNodeId_; }
		[[nodiscard]] int64_t getCurrentMaxEdgeId() const { return currentMaxEdgeId_; }
		[[nodiscard]] int64_t getCurrentMaxPropId() const { return currentMaxPropId_; }
		[[nodiscard]] int64_t getCurrentMaxBlobId() const { return currentMaxBlobId_; }
		[[nodiscard]] int64_t getCurrentMaxIndexId() const { return currentMaxIndexId_; }
		[[nodiscard]] int64_t getCurrentMaxStateId() const { return currentMaxStateId_; }

	private:
		// L1 Cache Limit: Fast access for immediate reuse (Persisted IDs)
		size_t l1CacheSize_ = 4096;
		// L2 Cache Limit: Number of INTERVALS (Persisted IDs)
		// 10,000 intervals can represent millions of IDs efficiently.
		size_t l2MaxIntervals_ = 10000;
		// Volatile Cache Limit: Number of INTERVALS (Memory-only IDs)
		// Allowed to be larger to handle massive transaction rollbacks gracefully.
		size_t volatileMaxIntervals_ = 50000;
		/**
		 * @brief Helper class to store ID ranges compactly (e.g., [1-1000]).
		 * Replaces std::deque for bulk storage to reduce memory footprint by orders of magnitude.
		 */
		class IDIntervalSet {
		public:
			void add(int64_t id);
			void addRange(int64_t start, int64_t end);

			// Retrieves and removes the first available ID. Returns 0 if empty.
			int64_t pop();

			[[nodiscard]] size_t intervalCount() const { return intervals_.size(); }
			[[nodiscard]] bool empty() const { return intervals_.empty(); }
			void clear() { intervals_.clear(); }

		private:
			// Map: StartID -> EndID. Keeps ranges sorted and merged.
			std::map<int64_t, int64_t> intervals_;
		};

		struct ScanCursor {
			uint64_t nextSegmentOffset = 0;
			bool diskExhausted = false;
			bool wrappedAround = false;
		};

		bool fetchInactiveIdsFromDisk(uint32_t entityType);
		int64_t allocateNewSequentialId(uint32_t entityType) const;

		// Recovers gaps between logical MaxID and physical disk storage on startup.
		void recoverGapIds(uint32_t entityType, int64_t &logicalMaxId);

		std::shared_ptr<std::fstream> file_;
		std::shared_ptr<SegmentTracker> segmentTracker_;

		// --- Cache Layering ---

		// 1. Volatile Cache: IDs allocated but NEVER persisted (or dirty pre-allocs).
		// Stored as Intervals to handle massive transaction rollbacks efficiently.
		// Priority: HIGH (Reuse these first to avoid holes).
		std::unordered_map<uint32_t, IDIntervalSet> volatileCache_;

		// 2. L1 Hot Cache: Recently freed PERSISTED IDs.
		// Stored as Deque for O(1) push/pop without tree rebalancing overhead.
		// Priority: MEDIUM (Reuse active disk slots).
		std::unordered_map<uint32_t, std::deque<int64_t>> hotCache_;

		// 3. L2 Cold Cache: Older freed PERSISTED IDs.
		// Stored as Intervals to save memory.
		// Priority: LOW.
		std::unordered_map<uint32_t, IDIntervalSet> coldCache_;

		std::unordered_map<uint32_t, ScanCursor> scanCursors_;

		// References to global max counters managed by FileHeaderManager
		int64_t &currentMaxNodeId_;
		int64_t &currentMaxEdgeId_;
		int64_t &currentMaxPropId_;
		int64_t &currentMaxBlobId_;
		int64_t &currentMaxIndexId_;
		int64_t &currentMaxStateId_;

		mutable std::mutex mutex_;
	};

} // namespace graph::storage
