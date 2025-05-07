/**
 * @file SegmentTracker.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/SegmentTracker.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <graph/storage/SegmentType.h>
#include <graph/utils/ChecksumUtils.h>
#include <iostream>
#include <stdexcept>

namespace graph::storage {

	SegmentTracker::SegmentTracker(std::shared_ptr<std::fstream> file) : file_(std::move(file)) {}

	SegmentTracker::~SegmentTracker() = default;

	void SegmentTracker::initialize(const FileHeader &header) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Initialize chain heads
		nodeSegmentHead_ = header.node_segment_head;
		edgeSegmentHead_ = header.edge_segment_head;
		propertySegmentHead_ = header.property_segment_head;
		blobSegmentHead_ = header.blob_segment_head;

		// Clear existing cache
		segments_.clear();
		freeSegments_.clear();
		dirtySegments_.clear();

		// Load and cache all segments
		loadSegments();
	}

	void SegmentTracker::loadSegments() {
		// Scan node segments
		loadSegmentChain(nodeSegmentHead_, toUnderlying(SegmentType::Node));

		// Scan edge segments
		loadSegmentChain(edgeSegmentHead_, toUnderlying(SegmentType::Edge));

		// Scan property segments
		loadSegmentChain(propertySegmentHead_, toUnderlying(SegmentType::Property));

		// Scan blob segments
		loadSegmentChain(blobSegmentHead_, toUnderlying(SegmentType::Blob));
	}

	void SegmentTracker::loadSegmentChain(uint64_t headOffset, uint8_t expectedType) {
		uint64_t offset = headOffset;
		while (offset != 0) {
			// Read segment header
			SegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(offset));
			file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

			if (!*file_) {
				throw std::runtime_error("Failed to read segment header at offset " + std::to_string(offset));
			}

			// Validate data type
			if (header.data_type != expectedType) {
				throw std::runtime_error("Segment type mismatch: expected " + std::to_string(expectedType) +
										 ", found " + std::to_string(header.data_type));
			}

			// // Store file offset and initialize tracking flags
			// header.file_offset = offset;
			// header.is_dirty = 0;
			// header.needs_compaction = 0;


			// Add to segment map
			segments_[offset] = header;

			// Move to next segment
			offset = header.next_segment_offset;
		}
	}

	void SegmentTracker::registerSegment(uint64_t offset, uint8_t type, uint32_t capacity) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		SegmentHeader header;
		header.file_offset = offset;
		header.data_type = type;
		header.capacity = capacity;
		header.used = 0;
		header.inactive_count = 0;
		header.next_segment_offset = 0;
		header.prev_segment_offset = 0;
		header.start_id = 0;
		header.needs_compaction = 0;
		header.is_dirty = 0;
		header.bitmap_size = bitmap::calculateBitmapSize(capacity);

		segments_[offset] = header;
	}

	void SegmentTracker::updateSegmentUsage(uint64_t offset, uint32_t used, uint32_t inactive) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		ensureSegmentCached(offset);

		SegmentHeader &header = segments_[offset];
		if (header.used != used || header.inactive_count != inactive) {
			header.used = used;
			header.inactive_count = inactive;
			header.is_dirty = 1;
			markSegmentDirty(offset);
		}
	}

	void SegmentTracker::markForCompaction(uint64_t offset, bool needsCompaction) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		ensureSegmentCached(offset);

		SegmentHeader &header = segments_[offset];
		if ((header.needs_compaction == 1) != needsCompaction) {
			header.needs_compaction = needsCompaction ? 1 : 0;
			header.is_dirty = 1;
			markSegmentDirty(offset);
		}
	}

	SegmentHeader& SegmentTracker::getSegmentHeader(uint64_t offset) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Ensure the segment is cached
		ensureSegmentCached(offset);

		auto it = segments_.find(offset);
		if (it == segments_.end()) {
			throw std::runtime_error("Segment not found at offset " + std::to_string(offset));
		}
		return it->second;
	}

	std::vector<SegmentHeader> SegmentTracker::getSegmentsByType(uint8_t type) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		std::vector<SegmentHeader> result;
		for (const auto &[offset, header]: segments_) {
			if (header.data_type == type) {
				result.push_back(header);
			}
		}
		return result;
	}

	std::vector<SegmentHeader> SegmentTracker::getSegmentsNeedingCompaction(uint8_t type, double threshold) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		std::vector<SegmentHeader> result;
		for (const auto &[offset, header]: segments_) {
			if (header.data_type == type && (header.needs_compaction || header.getFragmentationRatio() >= threshold)) {
				result.push_back(header);
			}
		}

		// Sort by fragmentation ratio (highest first)
		std::sort(result.begin(), result.end(), [](const SegmentHeader &a, const SegmentHeader &b) {
			return a.getFragmentationRatio() > b.getFragmentationRatio();
		});

		return result;
	}

	double SegmentTracker::calculateFragmentationRatio(uint8_t type) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		uint64_t totalCapacity = 0;
		uint64_t totalUtilizedSpace = 0;

		for (const auto &[offset, header]: segments_) {
			if (header.data_type == type) {
				totalCapacity += header.capacity;
				// Only count active elements (used minus inactive)
				totalUtilizedSpace += (header.used - header.inactive_count);
			}
		}

		if (totalCapacity == 0) {
			return 0.0;
		}

		// Fragmentation ratio is (1 - utilization rate)
		double utilizationRate = static_cast<double>(totalUtilizedSpace) / totalCapacity;
		return 1.0 - utilizationRate;
	}

	uint64_t SegmentTracker::getChainHead(uint8_t type) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		switch (type) {
			case 0:
				return nodeSegmentHead_;
			case 1:
				return edgeSegmentHead_;
			case 2:
				return propertySegmentHead_;
			case 3:
				return blobSegmentHead_;
			default:
				return 0;
		}
	}

	void SegmentTracker::updateChainHead(uint8_t type, uint64_t newHead) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		switch (type) {
			case 0:
				nodeSegmentHead_ = newHead;
				break;
			case 1:
				edgeSegmentHead_ = newHead;
				break;
			case 2:
				propertySegmentHead_ = newHead;
				break;
			case 3:
				blobSegmentHead_ = newHead;
				break;
		}
	}

	void SegmentTracker::updateSegmentLinks(uint64_t offset, uint64_t prevOffset, uint64_t nextOffset) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Ensure the current segment is cached
		ensureSegmentCached(offset);

		SegmentHeader &header = segments_[offset];
		bool changed = false;

		// Update the current segment's links
		if (header.prev_segment_offset != prevOffset) {
			if (prevOffset == offset) {
				throw std::runtime_error("Self-loop detected: prevOffset cannot be the same as offset");
			}
			header.prev_segment_offset = prevOffset;
			changed = true;
		}

		if (header.next_segment_offset != nextOffset) {
			if (nextOffset == offset) {
				throw std::runtime_error("Self-loop detected: nextOffset cannot be the same as offset");
			}
			header.next_segment_offset = nextOffset;
			changed = true;
		}

		if (changed) {
			header.is_dirty = 1;
			markSegmentDirty(offset);
		}
	}

	void SegmentTracker::markSegmentFree(uint64_t offset) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Ensure offset is aligned to TOTAL_SEGMENT_SIZE
		if ((offset - FILE_HEADER_SIZE) % TOTAL_SEGMENT_SIZE != 0) {
			throw std::runtime_error("Attempting to free segment at non-aligned offset: " + std::to_string(offset));
		}

		// Remove from dirty segments list if present
		auto it = std::find(dirtySegments_.begin(), dirtySegments_.end(), offset);
		if (it != dirtySegments_.end()) {
			dirtySegments_.erase(it);
		}

		// Remove from segments map
		if (segments_.find(offset) != segments_.end()) {
			segments_.erase(offset);
		}

		// Add to free segments list
		freeSegments_.push_back(offset);
	}

	std::vector<uint64_t> SegmentTracker::getFreeSegments() const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		return freeSegments_;
	}

	void SegmentTracker::removeFromFreeList(uint64_t offset) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		freeSegments_.erase(std::remove(freeSegments_.begin(), freeSegments_.end(), offset), freeSegments_.end());
	}

	void SegmentTracker::writeSegmentHeader(uint64_t offset, const SegmentHeader &header) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Create a copy for writing to disk
		SegmentHeader headerToWrite = header;

		// Calculate CRC before writing
		headerToWrite.segment_crc = utils::calculateCrc(&headerToWrite, offsetof(SegmentHeader, segment_crc));

		// Write to disk
		file_->seekp(static_cast<std::streamoff>(offset));
		file_->write(reinterpret_cast<const char *>(&headerToWrite), sizeof(SegmentHeader));

		if (!*file_) {
			throw std::runtime_error("Failed to write segment header at offset " + std::to_string(offset));
		}

		// Update in-memory cache
		auto it = segments_.find(offset);
		if (it != segments_.end()) {
			it->second = headerToWrite;
			it->second.file_offset = offset; // Ensure offset is preserved
			it->second.is_dirty = 0; // Mark as clean after writing

			// Remove from dirty list
			auto dirtyIt = std::find(dirtySegments_.begin(), dirtySegments_.end(), offset);
			if (dirtyIt != dirtySegments_.end()) {
				dirtySegments_.erase(dirtyIt);
			}
		} else {
			// If not in cache yet, add it
			segments_[offset] = headerToWrite;
			segments_[offset].file_offset = offset;
			segments_[offset].is_dirty = 0;
		}
	}

	void SegmentTracker::updateSegmentHeader(uint64_t offset, const std::function<void(SegmentHeader &)> &updateFn) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		ensureSegmentCached(offset);
		SegmentHeader &header = segments_[offset];

		// Apply update function
		updateFn(header);

		// Mark as dirty
		header.is_dirty = 1;
		markSegmentDirty(offset);
	}

	void SegmentTracker::setBitmapBit(uint64_t offset, uint32_t index, bool value) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		ensureSegmentCached(offset);
		SegmentHeader &header = segments_[offset];

		bool oldValue = bitmap::getBit(header.activity_bitmap, index);

		// Only update if the value is changing
		if (oldValue != value) {
			bitmap::setBit(header.activity_bitmap, index, value);

			// Update inactive count
			if (value) { // Activating
				if (header.inactive_count > 0) {
					header.inactive_count--;
				}
			} else { // Deactivating
				header.inactive_count++;
			}

			header.is_dirty = 1;
			markSegmentDirty(offset);
		}
	}

	bool SegmentTracker::getBitmapBit(uint64_t offset, uint32_t index) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		auto it = segments_.find(offset);
		if (it != segments_.end()) {
			return bitmap::getBit(it->second.activity_bitmap, index);
		}

		throw std::runtime_error("Segment not found at offset " + std::to_string(offset));
	}

	void SegmentTracker::updateActivityBitmap(uint64_t offset, const std::vector<bool> &activityMap) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		ensureSegmentCached(offset);
		SegmentHeader &header = segments_[offset];

		// Calculate bitmap size
		header.bitmap_size = bitmap::calculateBitmapSize(activityMap.size());

		// Reset bitmap
		memset(header.activity_bitmap, 0, header.bitmap_size);

		// Count inactive entities
		uint32_t inactiveCount = 0;

		// Set bits according to activity map
		for (size_t i = 0; i < activityMap.size(); i++) {
			bitmap::setBit(header.activity_bitmap, i, activityMap[i]);
			if (!activityMap[i]) {
				inactiveCount++;
			}
		}

		// Update inactive count
		header.inactive_count = inactiveCount;

		// Mark as dirty
		header.is_dirty = 1;
		markSegmentDirty(offset);
	}

	std::vector<bool> SegmentTracker::getActivityBitmap(uint64_t offset) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		auto it = segments_.find(offset);
		if (it == segments_.end()) {
			throw std::runtime_error("Segment not found at offset " + std::to_string(offset));
		}

		const SegmentHeader &header = it->second;

		// Create activity map
		std::vector<bool> activityMap(header.used);

		// Fill from bitmap
		for (uint32_t i = 0; i < header.used; i++) {
			activityMap[i] = bitmap::getBit(header.activity_bitmap, i);
		}

		return activityMap;
	}

	void SegmentTracker::setEntityActive(uint64_t offset, uint32_t index, bool active) {
		setBitmapBit(offset, index, active);
	}

	bool SegmentTracker::isEntityActive(uint64_t offset, uint32_t index) const { return getBitmapBit(offset, index); }

	uint32_t SegmentTracker::countActiveEntities(uint64_t offset) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		auto it = segments_.find(offset);
		if (it == segments_.end()) {
			throw std::runtime_error("Segment not found at offset " + std::to_string(offset));
		}

		return it->second.used - it->second.inactive_count;
	}

	uint64_t SegmentTracker::getSegmentOffsetForNodeId(int64_t nodeId) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		uint64_t offset = nodeSegmentHead_;

		while (offset != 0) {
			SegmentHeader &header = getSegmentHeader(offset);

			// Check if the ID falls within this segment's range
			if (nodeId >= header.start_id && nodeId < header.start_id + header.capacity) {
				return offset;
			}

			offset = header.next_segment_offset;
		}

		return 0; // Not found
	}

	uint64_t SegmentTracker::getSegmentOffsetForEdgeId(int64_t edgeId) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		uint64_t offset = edgeSegmentHead_;

		while (offset != 0) {
			SegmentHeader &header = getSegmentHeader(offset);

			// Check if the ID falls within this segment's range
			if (edgeId >= header.start_id && edgeId < header.start_id + header.capacity) {
				return offset;
			}

			offset = header.next_segment_offset;
		}

		return 0; // Not found
	}

	uint64_t SegmentTracker::getSegmentOffsetForPropId(int64_t propId) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		uint64_t offset = propertySegmentHead_;

		while (offset != 0) {
			SegmentHeader &header = getSegmentHeader(offset);

			// Check if the ID falls within this segment's range
			if (propId >= header.start_id && propId < header.start_id + header.capacity) {
				return offset;
			}

			offset = header.next_segment_offset;
		}

		return 0; // Not found
	}

	uint64_t SegmentTracker::getSegmentOffsetForBlobId(int64_t blobId) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		uint64_t offset = blobSegmentHead_;

		while (offset != 0) {
			SegmentHeader &header = getSegmentHeader(offset);

			// Check if the ID falls within this segment's range
			if (blobId >= header.start_id && blobId < header.start_id + header.capacity) {
				return offset;
			}

			offset = header.next_segment_offset;
		}

		return 0; // Not found
	}

	void SegmentTracker::ensureSegmentCached(uint64_t offset) {
		if (segments_.find(offset) == segments_.end()) {
			// Read from disk and add to cache
			SegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(offset));
			file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

			if (!*file_) {
				throw std::runtime_error("Failed to read segment header at offset " + std::to_string(offset));
			}

			// Set memory-only fields
			header.file_offset = offset;
			header.needs_compaction = 0;
			header.is_dirty = 0;

			// Add to cache
			segments_[offset] = header;
		}
	}

	void SegmentTracker::markSegmentDirty(uint64_t offset) {
		// Add to dirty segments list if not already there
		if (std::find(dirtySegments_.begin(), dirtySegments_.end(), offset) == dirtySegments_.end()) {
			dirtySegments_.push_back(offset);
		}
	}

	void SegmentTracker::flushDirtySegments() {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Make a copy to avoid issues if the list is modified during iteration
		auto dirtySegmentsCopy = dirtySegments_;

		for (uint64_t offset: dirtySegmentsCopy) {
			auto it = segments_.find(offset);
			if (it != segments_.end() && it->second.is_dirty) {
				SegmentHeader &header = it->second;

				// Create a copy for writing to disk, excluding memory-only fields
				SegmentHeader headerToWrite = header;

				// Calculate CRC
				headerToWrite.segment_crc =
						utils::calculateCrc(&headerToWrite, offsetof(SegmentHeader, segment_crc));

				// Write to disk
				file_->seekp(static_cast<std::streamoff>(offset));
				file_->write(reinterpret_cast<const char *>(&headerToWrite), sizeof(SegmentHeader));

				// Mark as clean
				it->second.is_dirty = 0;
			}
		}

		// Clear the dirty list
		dirtySegments_.clear();

		// Ensure all writes are flushed to disk
		file_->flush();
	}

} // namespace graph::storage
