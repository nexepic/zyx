/**
 * @file IDAllocator.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/19
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/IDAllocator.h"
#include <algorithm>
#include <stdexcept>
#include "graph/storage/SegmentTracker.h"

namespace graph::storage {

	IDAllocator::IDAllocator(std::shared_ptr<std::fstream> file, std::shared_ptr<SegmentTracker> segmentTracker,
							 int64_t &maxNodeId, int64_t &maxEdgeId) :
		file_(std::move(file)), segmentTracker_(std::move(segmentTracker)), currentMaxNodeId_(maxNodeId),
		currentMaxEdgeId_(maxEdgeId) {}

	void IDAllocator::initialize() {
		std::lock_guard<std::mutex> lock(mutex_);

		// Initialize the cache of inactive IDs
		refreshInactiveIdsCache(Node::typeId);
		refreshInactiveIdsCache(Edge::typeId);
	}

	int64_t IDAllocator::reserveTemporaryId(uint8_t entityType) {
		std::lock_guard<std::mutex> lock(mutex_);

		if (entityType == Node::typeId) {
			return nextTempNodeId_--;
		} else if (entityType == Edge::typeId) {
			return nextTempEdgeId_--;
		}

		throw std::runtime_error("Invalid entity type for temporary ID reservation");
	}

	uint64_t IDAllocator::allocatePermanentId(int64_t tempId, uint8_t entityType) {
		std::lock_guard<std::mutex> lock(mutex_);

		// Check if we've already allocated a permanent ID for this temp ID
		if (entityType == Node::typeId) {
			auto it = tempToPermNodeIds_.find(tempId);
			if (it != tempToPermNodeIds_.end()) {
				return it->second;
			}
		} else if (entityType == Edge::typeId) {
			auto it = tempToPermEdgeIds_.find(tempId);
			if (it != tempToPermEdgeIds_.end()) {
				return it->second;
			}
		}

		// First try to reuse an inactive ID
		int64_t permId = findInactiveId(entityType);

		// If no inactive ID is available, allocate a new sequential ID
		if (permId == 0) {
			permId = allocateNewSequentialId(entityType);
		}

		// Store the mapping from temporary to permanent ID
		if (entityType == Node::typeId) {
			tempToPermNodeIds_[tempId] = permId;
		} else if (entityType == Edge::typeId) {
			tempToPermEdgeIds_[tempId] = permId;
		}

		return permId;
	}

	void IDAllocator::freeId(uint64_t id, uint8_t entityType) {
		std::lock_guard<std::mutex> lock(mutex_);

		// Find the segment containing this ID
		uint64_t segmentOffset = 0;
		if (entityType == Node::typeId) {
			segmentOffset = segmentTracker_->getSegmentOffsetForNodeId(id);
		} else if (entityType == Edge::typeId) {
			segmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(id);
		}

		if (segmentOffset == 0) {
			// ID not found in any segment, nothing to do
			return;
		}

		// Read segment header
		SegmentHeader header = segmentTracker_->readSegmentHeader(segmentOffset);

		// Calculate index in segment
		uint32_t index = static_cast<uint32_t>(id - header.start_id);

		// Update bitmap to mark ID as inactive
		segmentTracker_->setEntityActive(segmentOffset, index, false);

		// Update our cache of inactive IDs
		auto &cache = inactiveIdsCache_[entityType];

		// Find this segment in the cache
		auto it = std::find_if(cache.begin(), cache.end(),
							   [segmentOffset](const auto &entry) { return entry.first == segmentOffset; });

		// Add this index to the list of inactive indices
		if (it != cache.end()) {
			it->second.push_back(index);
		} else {
			cache.emplace_back(segmentOffset, std::vector<uint32_t>{index});
		}
	}

	int64_t IDAllocator::findInactiveId(uint8_t entityType) {
		auto &cache = inactiveIdsCache_[entityType];

		// If cache is empty, refresh it
		if (cache.empty()) {
			refreshInactiveIdsCache(entityType);
		}

		// If still empty after refresh, no inactive IDs are available
		if (cache.empty()) {
			return 0;
		}

		// Get the first segment with inactive IDs
		auto &entry = cache.front();
		uint64_t segmentOffset = entry.first;
		auto &inactiveIndices = entry.second;

		// If no inactive indices in this segment, remove it and try again
		if (inactiveIndices.empty()) {
			cache.erase(cache.begin());
			return findInactiveId(entityType);
		}

		// Get the first inactive index
		uint32_t index = inactiveIndices.back();
		inactiveIndices.pop_back();

		// If no more inactive indices in this segment, remove it from cache
		if (inactiveIndices.empty()) {
			cache.erase(cache.begin());
		}

		// Read segment header to get start_id
		SegmentHeader header = segmentTracker_->readSegmentHeader(segmentOffset);
		int64_t id = header.start_id + index;

		// Mark the ID as active
		markIdActive(id, entityType);

		return id;
	}

	void IDAllocator::updateMaxIds(int64_t maxNodeId, int64_t maxEdgeId) {
		std::lock_guard<std::mutex> lock(mutex_);
		currentMaxNodeId_ = maxNodeId;
		currentMaxEdgeId_ = maxEdgeId;
	}

	int64_t IDAllocator::allocateNewSequentialId(uint8_t entityType) {
		int64_t id;

		if (entityType == Node::typeId) {
			id = ++currentMaxNodeId_;
		} else if (entityType == Edge::typeId) {
			id = ++currentMaxEdgeId_;
		} else {
			throw std::runtime_error("Invalid entity type for ID allocation");
		}

		return id;
	}

	void IDAllocator::markIdActive(uint64_t id, uint8_t entityType) {
		// Find the segment containing this ID
		uint64_t segmentOffset = 0;
		if (entityType == Node::typeId) {
			segmentOffset = segmentTracker_->getSegmentOffsetForNodeId(id);
		} else if (entityType == Edge::typeId) {
			segmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(id);
		}

		if (segmentOffset == 0) {
			// This is a new ID that doesn't exist in any segment yet
			// It will be marked as active when it's written to disk
			return;
		}

		// Read segment header
		SegmentHeader header = segmentTracker_->readSegmentHeader(segmentOffset);

		// Calculate index in segment
		uint32_t index = static_cast<uint32_t>(id - header.start_id);

		// Update bitmap to mark ID as active
		segmentTracker_->setEntityActive(segmentOffset, index, true);
	}

	void IDAllocator::refreshInactiveIdsCache(uint8_t entityType) {
		// Clear the current cache for this entity type
		inactiveIdsCache_[entityType].clear();

		// Get all segments of this type
		auto segments = segmentTracker_->getSegmentsByType(entityType);

		// Process each segment
		for (const auto &segmentInfo: segments) {
			uint64_t segmentOffset = segmentInfo.offset;

			// Read the activity bitmap
			std::vector<bool> activityMap = segmentTracker_->getActivityBitmap(segmentOffset);

			// Find all inactive indices
			std::vector<uint32_t> inactiveIndices;
			for (uint32_t i = 0; i < activityMap.size(); ++i) {
				if (!activityMap[i]) {
					inactiveIndices.push_back(i);
				}
			}

			// If we found inactive indices, add to cache
			if (!inactiveIndices.empty()) {
				inactiveIdsCache_[entityType].emplace_back(segmentOffset, std::move(inactiveIndices));
			}
		}
	}

	int64_t IDAllocator::getPermanentId(int64_t tempId, uint8_t entityType) const {
		std::lock_guard<std::mutex> lock(mutex_);

		if (entityType == Node::typeId) {
			auto it = tempToPermNodeIds_.find(tempId);
			if (it != tempToPermNodeIds_.end()) {
				return it->second;
			}
		} else if (entityType == Edge::typeId) {
			auto it = tempToPermEdgeIds_.find(tempId);
			if (it != tempToPermEdgeIds_.end()) {
				return it->second;
			}
		}

		return 0; // Not found
	}


	void IDAllocator::clearTempIdMappings() {
		std::lock_guard<std::mutex> lock(mutex_);
		tempToPermNodeIds_.clear();
		tempToPermEdgeIds_.clear();
	}

	void IDAllocator::clearTempIdMapping(int64_t tempId, uint8_t entityType) {
		std::lock_guard<std::mutex> lock(mutex_);

		if (entityType == Node::typeId) {
			tempToPermNodeIds_.erase(tempId);
		} else if (entityType == Edge::typeId) {
			tempToPermEdgeIds_.erase(tempId);
		}
	}

} // namespace graph::storage
