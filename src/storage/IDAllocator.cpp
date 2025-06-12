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
#include "graph/storage/SegmentTracker.hpp"

namespace graph::storage {

	IDAllocator::IDAllocator(std::shared_ptr<std::fstream> file, std::shared_ptr<SegmentTracker> segmentTracker,
							 int64_t &maxNodeId, int64_t &maxEdgeId, int64_t &maxPropId, int64_t &maxBlobId) :
		file_(std::move(file)), segmentTracker_(std::move(segmentTracker)), currentMaxNodeId_(maxNodeId),
		currentMaxEdgeId_(maxEdgeId), currentMaxPropId_(maxPropId), currentMaxBlobId_(maxBlobId) {}

	void IDAllocator::initialize() {
		std::lock_guard<std::mutex> lock(mutex_);

		// Initialize the cache of inactive IDs
		refreshInactiveIdsCache(Node::typeId);
		refreshInactiveIdsCache(Edge::typeId);
		refreshInactiveIdsCache(Property::typeId);
		refreshInactiveIdsCache(Blob::typeId);
	}

	int64_t IDAllocator::reserveTemporaryId(uint32_t entityType) {
		std::lock_guard<std::mutex> lock(mutex_);

		if (entityType == Node::typeId) {
			return nextTempNodeId_--;
		} else if (entityType == Edge::typeId) {
			return nextTempEdgeId_--;
		} else if (entityType == Property::typeId) {
			return nextTempPropId_--;
		} else if (entityType == Blob::typeId) {
			return nextTempBlobId_--;
		}

		throw std::runtime_error("Invalid entity type for temporary ID reservation");
	}

	int64_t IDAllocator::allocatePermanentId(int64_t tempId, uint32_t entityType) {
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
		} else if (entityType == Property::typeId) {
			auto it = tempToPermPropIds_.find(tempId);
			if (it != tempToPermPropIds_.end()) {
				return it->second;
			}
		} else if (entityType == Blob::typeId) {
			auto it = tempToPermBlobIds_.find(tempId);
			if (it != tempToPermBlobIds_.end()) {
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
		} else if (entityType == Property::typeId) {
			tempToPermPropIds_[tempId] = permId;
		} else if (entityType == Blob::typeId) {
			tempToPermBlobIds_[tempId] = permId;
		}

		// Notify the callback if it's set
		if (idUpdateCallback_) {
			idUpdateCallback_(tempId, permId, entityType);
		}

		return permId;
	}

	void IDAllocator::freeId(uint64_t id, uint32_t entityType) {
		std::lock_guard<std::mutex> lock(mutex_);

		// Find the segment containing this ID
		uint64_t segmentOffset = 0;
		if (entityType == Node::typeId) {
			segmentOffset = segmentTracker_->getSegmentOffsetForNodeId(id);
		} else if (entityType == Edge::typeId) {
			segmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(id);
		} else if (entityType == Property::typeId) {
			segmentOffset = segmentTracker_->getSegmentOffsetForPropId(id);
		} else if (entityType == Blob::typeId) {
			segmentOffset = segmentTracker_->getSegmentOffsetForBlobId(id);
		}

		if (segmentOffset == 0) {
			// ID not found in any segment, nothing to do
			return;
		}

		// Read segment header
		SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);

		// Calculate index in segment
		auto index = static_cast<uint32_t>(id - header.start_id);

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
			cache.emplace_back(segmentOffset, std::vector{index});
		}
	}

	int64_t IDAllocator::findInactiveId(uint32_t entityType) {
		auto &cache = inactiveIdsCache_[entityType];

		// If cache is empty, refresh it
		if (cache.empty()) {
			refreshInactiveIdsCache(entityType);

			// If still empty after refresh, no inactive IDs are available
			if (cache.empty()) {
				return 0;
			}
		}

		// Get first segment with inactive IDs
		uint64_t segmentOffset = cache.front().first;
		auto &inactiveIndices = cache.front().second;

		// Get an inactive index
		uint32_t index = inactiveIndices.back();
		inactiveIndices.pop_back();

		// If this segment has no more inactive indices, remove it from cache
		if (inactiveIndices.empty()) {
			cache.erase(cache.begin());
		}

		// Read segment header to get start_id
		SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);
		int64_t id = header.start_id + index;

		// Mark the ID as active in the segment tracker to prevent reuse
		segmentTracker_->setEntityActive(segmentOffset, index, true);

		return id;
	}

	int64_t IDAllocator::allocateNewSequentialId(uint32_t entityType) {
		int64_t id;

		if (entityType == Node::typeId) {
			id = ++currentMaxNodeId_;
		} else if (entityType == Edge::typeId) {
			id = ++currentMaxEdgeId_;
		} else if (entityType == Property::typeId) {
			id = ++currentMaxPropId_;
		} else if (entityType == Blob::typeId) {
			id = ++currentMaxBlobId_;
		} else {
			throw std::runtime_error("Invalid entity type for ID allocation");
		}

		return id;
	}

	void IDAllocator::markIdActive(uint64_t id, uint32_t entityType) {
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
		SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);

		// Calculate index in segment
		auto index = static_cast<uint32_t>(id - header.start_id);

		// Update bitmap to mark ID as active
		segmentTracker_->setEntityActive(segmentOffset, index, true);
	}

	void IDAllocator::refreshInactiveIdsCache(uint32_t entityType) {
		// Clear the current cache for this entity type
		inactiveIdsCache_[entityType].clear();

		// Get all segments of this type
		auto segments = segmentTracker_->getSegmentsByType(entityType);

		// Process each segment
		for (const auto &header: segments) {
			uint64_t segmentOffset = header.file_offset;

			// Skip segments with no inactive elements
			if (header.inactive_count == 0) {
				continue;
			}

			// Read the activity bitmap
			std::vector<bool> activityMap = segmentTracker_->getActivityBitmap(segmentOffset);

			// Find all inactive indices
			std::vector<uint32_t> inactiveIndices;
			inactiveIndices.reserve(header.inactive_count); // Pre-allocate for efficiency

			// Only check up to the 'used' count (not capacity)
			for (uint32_t i = 0; i < header.used; ++i) {
				if (!activityMap[i]) {
					inactiveIndices.push_back(i);
					if (inactiveIndices.size() >= header.inactive_count) {
						break; // Found all inactive slots
					}
				}
			}

			// If we found inactive indices, add to cache
			if (!inactiveIndices.empty()) {
				inactiveIdsCache_[entityType].emplace_back(segmentOffset, std::move(inactiveIndices));
			}
		}
	}

	int64_t IDAllocator::getPermanentId(int64_t tempId, uint32_t entityType) const {
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
		} else if (entityType == Property::typeId) {
			auto it = tempToPermPropIds_.find(tempId);
			if (it != tempToPermPropIds_.end()) {
				return it->second;
			}
		} else if (entityType == Blob::typeId) {
			auto it = tempToPermBlobIds_.find(tempId);
			if (it != tempToPermBlobIds_.end()) {
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

	void IDAllocator::clearTempIdMapping(int64_t tempId, uint32_t entityType) {
		std::lock_guard<std::mutex> lock(mutex_);

		if (entityType == Node::typeId) {
			tempToPermNodeIds_.erase(tempId);
		} else if (entityType == Edge::typeId) {
			tempToPermEdgeIds_.erase(tempId);
		}
	}

} // namespace graph::storage
