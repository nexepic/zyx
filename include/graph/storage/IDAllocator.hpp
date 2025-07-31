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

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "StorageHeaders.hpp"

namespace graph::storage {

	class SegmentTracker;

	class IDAllocator {
	public:
		explicit IDAllocator(std::shared_ptr<std::fstream> file, std::shared_ptr<SegmentTracker> segmentTracker,
							 int64_t &maxNodeId, int64_t &maxEdgeId, int64_t &maxPropId, int64_t &maxBlobId,
							 int64_t &maxIndexId, int64_t &maxStateId);

		int64_t allocateId(uint32_t entityType);

		// Free an ID (mark as inactive for future reuse)
		void freeId(int64_t id, uint32_t entityType);

		// Initialize with current maximum IDs
		void initialize();

		// Get current max IDs for file header updates
		int64_t getCurrentMaxNodeId() const { return currentMaxNodeId_; }
		int64_t getCurrentMaxEdgeId() const { return currentMaxEdgeId_; }
		int64_t getCurrentMaxPropId() const { return currentMaxPropId_; }
		int64_t getCurrentMaxBlobId() const { return currentMaxBlobId_; }
		int64_t getCurrentMaxIndexId() const { return currentMaxIndexId_; }
		int64_t getCurrentMaxStateId() const { return currentMaxStateId_; }

		// Refresh the inactive IDs cache for a given entity type
		void refreshInactiveIdsCache(uint32_t entityType);

	private:
		// Find an inactive ID in segments
		int64_t findInactiveId(uint32_t entityType);

		// Allocate a new sequential ID
		int64_t allocateNewSequentialId(uint32_t entityType) const;

		std::shared_ptr<std::fstream> file_;
		std::shared_ptr<SegmentTracker> segmentTracker_;

		// Cache of segments with inactive IDs for quick lookup
		std::unordered_map<uint32_t, std::vector<std::pair<uint64_t, std::vector<uint32_t>>>> inactiveIdsCache_;

		// References to max IDs
		int64_t &currentMaxNodeId_;
		int64_t &currentMaxEdgeId_;
		int64_t &currentMaxPropId_;
		int64_t &currentMaxBlobId_;
		int64_t &currentMaxIndexId_;
		int64_t &currentMaxStateId_;

		// Mutex for thread safety
		mutable std::mutex mutex_;
	};

} // namespace graph::storage
