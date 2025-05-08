/**
 * @file IDAllocator.h
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
#include "StorageHeaders.h"

namespace graph::storage {

	class SegmentTracker;

	class IDAllocator {
	public:
		explicit IDAllocator(std::shared_ptr<std::fstream> file, std::shared_ptr<SegmentTracker> segmentTracker,
							 int64_t &maxNodeId, int64_t &maxEdgeId, int64_t &maxPropId, int64_t &maxBlobId);

		// Reserve a temporary ID (negative number)
		int64_t reserveTemporaryId(uint8_t entityType);

		// Allocate a permanent ID for a temporary ID
		int64_t allocatePermanentId(int64_t tempId, uint8_t entityType);

		// Check if an ID is temporary
		static bool isTemporaryId(int64_t id) { return id < 0; }

		// Free an ID (mark as inactive for future reuse)
		void freeId(uint64_t id, uint8_t entityType);

		// Initialize with current maximum IDs
		void initialize();

		// Get current max IDs for file header updates
		int64_t getCurrentMaxNodeId() const { return currentMaxNodeId_; }
		int64_t getCurrentMaxEdgeId() const { return currentMaxEdgeId_; }
		int64_t getCurrentMaxPropId() const { return currentMaxPropId_; }
		int64_t getCurrentMaxBlobId() const { return currentMaxBlobId_; }

		// Clear temporary to permanent ID mappings
		void clearTempIdMappings();

		// Clear a specific temporary ID mapping
		void clearTempIdMapping(int64_t tempId, uint8_t entityType);

		// Get permanent ID for a temporary ID (returns 0 if not found)
		int64_t getPermanentId(int64_t tempId, uint8_t entityType) const;

		void updateMaxIds(int64_t maxNodeId, int64_t maxEdgeId);

		// Refresh the inactive IDs cache for a given entity type
		void refreshInactiveIdsCache(uint8_t entityType);

		using IdUpdateCallback = std::function<void(int64_t tempId, int64_t permId, uint8_t entityType)>;

		// Set the callback for ID updates
		void setIdUpdateCallback(IdUpdateCallback callback) { idUpdateCallback_ = std::move(callback); }

	private:
		// Find an inactive ID in segments
		int64_t findInactiveId(uint8_t entityType);

		// Allocate a new sequential ID
		int64_t allocateNewSequentialId(uint8_t entityType);

		// Mark an ID as active in its segment
		void markIdActive(uint64_t id, uint8_t entityType);

		std::shared_ptr<std::fstream> file_;
		std::shared_ptr<SegmentTracker> segmentTracker_;

		// Cache of segments with inactive IDs for quick lookup
		std::unordered_map<uint8_t, std::vector<std::pair<uint64_t, std::vector<uint32_t>>>> inactiveIdsCache_;

		// References to max IDs
		int64_t &currentMaxNodeId_;
		int64_t &currentMaxEdgeId_;
		int64_t &currentMaxPropId_;
		int64_t &currentMaxBlobId_;

		// Temporary ID counters (negative values)
		int64_t nextTempNodeId_ = -1;
		int64_t nextTempEdgeId_ = -1;
		int64_t nextTempPropId_ = -1;
		int64_t nextTempBlobId_ = -1;

		// Maps from temporary IDs to permanent IDs
		std::unordered_map<int64_t, int64_t> tempToPermNodeIds_;
		std::unordered_map<int64_t, int64_t> tempToPermEdgeIds_;
		std::unordered_map<int64_t, int64_t> tempToPermPropIds_;
		std::unordered_map<int64_t, int64_t> tempToPermBlobIds_;

		// Mutex for thread safety
		mutable std::mutex mutex_;

		// Callback to notify when IDs are updated
		IdUpdateCallback idUpdateCallback_;
	};

} // namespace graph::storage
