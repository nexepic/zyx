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
#include <iostream>
#include <stdexcept>

namespace graph::storage {

	SegmentTracker::SegmentTracker(std::shared_ptr<std::fstream> file) :
		file_(std::move(file)) {}

	SegmentTracker::~SegmentTracker() = default;

	void SegmentTracker::initialize(const FileHeader &header) {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Initialize chain heads
		nodeSegmentHead_ = header.node_segment_head;
		edgeSegmentHead_ = header.edge_segment_head;
		propertySegmentHead_ = header.property_segment_head;
		blobSegmentHead_ = header.blob_section_head;

		// Clear existing cache
		segments_.clear();
		freeSegments_.clear();

		// Scan all chain heads and cache segment info
		uint64_t offset = nodeSegmentHead_;
		while (offset != 0) {
			SegmentHeader segmentHeader = readSegmentHeader(offset);
			SegmentInfo info;
			info.offset = offset;
			info.type = 0; // Node
			info.capacity = segmentHeader.capacity;
			info.used = segmentHeader.used;
			info.inactive = segmentHeader.inactive_count;
			info.nextSegment = segmentHeader.next_segment_offset;
			info.prevSegment = segmentHeader.prev_segment_offset;

			segments_[offset] = info;
			offset = segmentHeader.next_segment_offset;
		}

		offset = edgeSegmentHead_;
		while (offset != 0) {
			SegmentHeader segmentHeader = readSegmentHeader(offset);
			SegmentInfo info;
			info.offset = offset;
			info.type = 1; // Edge
			info.capacity = segmentHeader.capacity;
			info.used = segmentHeader.used;
			info.inactive = segmentHeader.inactive_count;
			info.nextSegment = segmentHeader.next_segment_offset;
			info.prevSegment = segmentHeader.prev_segment_offset;

			segments_[offset] = info;
			offset = segmentHeader.next_segment_offset;
		}

		offset = propertySegmentHead_;
		while (offset != 0) {
			PropertySegmentHeader propertySegmentHeader;
			file_->seekg(static_cast<std::streamoff>(offset));
			file_->read(reinterpret_cast<char *>(&propertySegmentHeader), sizeof(PropertySegmentHeader));

			SegmentInfo info;
			info.offset = offset;
			info.type = 2; // Property
			info.capacity = propertySegmentHeader.capacity;
			info.used = propertySegmentHeader.used;
			info.inactive = propertySegmentHeader.inactive_count;
			info.nextSegment = propertySegmentHeader.next_segment_offset;
			info.prevSegment = propertySegmentHeader.prev_segment_offset;

			segments_[offset] = info;
			offset = propertySegmentHeader.next_segment_offset;
		}

		// Cache blob segments as well
		offset = blobSegmentHead_;
		while (offset != 0) {
			SegmentHeader blobSegmentHeader = readSegmentHeader(offset);
			SegmentInfo info;
			info.offset = offset;
			info.type = 3; // Blob
			info.capacity = blobSegmentHeader.capacity;
			info.used = blobSegmentHeader.used;
			info.inactive = blobSegmentHeader.inactive_count;
			info.nextSegment = blobSegmentHeader.next_segment_offset;
			info.prevSegment = blobSegmentHeader.prev_segment_offset;

			segments_[offset] = info;
			offset = blobSegmentHeader.next_segment_offset;
		}
	}

	void SegmentTracker::registerSegment(uint64_t offset, uint8_t type, uint32_t capacity) {
		// std::lock_guard<std::mutex> lock(mutex_);

		SegmentInfo info;
		info.offset = offset;
		info.type = type;
		info.capacity = capacity;
		info.used = 0;
		info.inactive = 0;
		info.nextSegment = 0;
		info.prevSegment = 0;
		info.needsCompaction = false;

		segments_[offset] = info;
	}

	void SegmentTracker::updateSegmentUsage(uint64_t offset, uint32_t used, uint32_t inactive) {
		// std::lock_guard<std::mutex> lock(mutex_);

		ensureSegmentCached(offset);

		SegmentInfo &info = segments_[offset];
		info.used = used;
		info.inactive = inactive;

		// Update the on-disk segment header
		if (info.type == 0 || info.type == 1 || info.type == 3) { // Node, Edge, or Blob
			SegmentHeader header = readSegmentHeader(offset);
			header.used = used;
			writeSegmentHeader(offset, header);
		} else if (info.type == 2) { // Property
			PropertySegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(offset));
			file_->read(reinterpret_cast<char *>(&header), sizeof(PropertySegmentHeader));

			header.used = used;

			file_->seekp(static_cast<std::streamoff>(offset));
			file_->write(reinterpret_cast<const char *>(&header), sizeof(PropertySegmentHeader));
		}
	}

	void SegmentTracker::markForCompaction(uint64_t offset, bool needsCompaction) {
		// std::lock_guard<std::mutex> lock(mutex_);

		ensureSegmentCached(offset);

		segments_[offset].needsCompaction = needsCompaction;
	}

	SegmentInfo &SegmentTracker::getSegmentInfo(uint64_t offset) {
		// std::lock_guard<std::mutex> lock(mutex_);

		ensureSegmentCached(offset);

		return segments_.at(offset);
	}

	std::vector<SegmentInfo> SegmentTracker::getSegmentsByType(uint8_t type) const {
		// std::lock_guard<std::mutex> lock(mutex_);

		std::vector<SegmentInfo> result;
		for (const auto &[offset, info]: segments_) {
			if (info.type == type) {
				result.push_back(info);
			}
		}

		return result;
	}

	std::vector<SegmentInfo> SegmentTracker::getSegmentsNeedingCompaction(uint8_t type, double threshold) const {
		// std::lock_guard<std::mutex> lock(mutex_);

		std::vector<SegmentInfo> result;
		for (const auto& [offset, info] : segments_) {
			if (info.type == type &&
				(info.needsCompaction || info.getFragmentationRatio() >= threshold)) {
				result.push_back(info);
				}
		}

		// Sort by fragmentation ratio (highest first)
		std::sort(result.begin(), result.end(),
			[](const SegmentInfo& a, const SegmentInfo& b) {
				return a.getFragmentationRatio() > b.getFragmentationRatio();
			});

		return result;
	}

	double SegmentTracker::calculateFragmentationRatio(uint8_t type) const {
		// std::lock_guard<std::mutex> lock(mutex_);

		uint64_t totalCapacity = 0;
		uint64_t totalUtilizedSpace = 0;

		for (const auto &[offset, info]: segments_) {
			if (info.type == type) {
				totalCapacity += info.capacity;
				// Only count active elements (used minus inactive)
				totalUtilizedSpace += (info.used - info.inactive);
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
		// std::lock_guard<std::mutex> lock(mutex_);

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
		// std::lock_guard<std::mutex> lock(mutex_);

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
		// std::lock_guard<std::mutex> lock(mutex_);

		ensureSegmentCached(offset);

		SegmentInfo &info = segments_[offset];
		info.prevSegment = prevOffset;
		info.nextSegment = nextOffset;

		// Update on-disk header
		if (info.type == 0 || info.type == 1 || info.type == 3) { // Node, Edge, or Blob
			SegmentHeader header = readSegmentHeader(offset);
			header.prev_segment_offset = prevOffset;
			header.next_segment_offset = nextOffset;
			writeSegmentHeader(offset, header);
		} else if (info.type == 2) { // Property
			PropertySegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(offset));
			file_->read(reinterpret_cast<char *>(&header), sizeof(PropertySegmentHeader));

			header.prev_segment_offset = prevOffset;
			header.next_segment_offset = nextOffset;

			file_->seekp(static_cast<std::streamoff>(offset));
			file_->write(reinterpret_cast<const char *>(&header), sizeof(PropertySegmentHeader));
		}

		// Update links in adjacent segments
		if (prevOffset != 0 && segments_.find(prevOffset) != segments_.end()) {
			SegmentInfo &prevInfo = segments_[prevOffset];
			prevInfo.nextSegment = offset;

			// Update on-disk header for prev
			if (prevInfo.type == 0 || prevInfo.type == 1 || prevInfo.type == 3) {
				SegmentHeader header = readSegmentHeader(prevOffset);
				header.next_segment_offset = offset;
				writeSegmentHeader(prevOffset, header);
			} else if (prevInfo.type == 2) {
				PropertySegmentHeader header;
				file_->seekg(static_cast<std::streamoff>(prevOffset));
				file_->read(reinterpret_cast<char *>(&header), sizeof(PropertySegmentHeader));

				header.next_segment_offset = offset;

				file_->seekp(static_cast<std::streamoff>(prevOffset));
				file_->write(reinterpret_cast<const char *>(&header), sizeof(PropertySegmentHeader));
			}
		}

		if (nextOffset != 0 && segments_.find(nextOffset) != segments_.end()) {
			SegmentInfo &nextInfo = segments_[nextOffset];
			nextInfo.prevSegment = offset;

			// Update on-disk header for next
			if (nextInfo.type == 0 || nextInfo.type == 1 || nextInfo.type == 3) {
				SegmentHeader header = readSegmentHeader(nextOffset);
				header.prev_segment_offset = offset;
				writeSegmentHeader(nextOffset, header);
			} else if (nextInfo.type == 2) {
				PropertySegmentHeader header;
				file_->seekg(static_cast<std::streamoff>(nextOffset));
				file_->read(reinterpret_cast<char *>(&header), sizeof(PropertySegmentHeader));

				header.prev_segment_offset = offset;

				file_->seekp(static_cast<std::streamoff>(nextOffset));
				file_->write(reinterpret_cast<const char *>(&header), sizeof(PropertySegmentHeader));
			}
		}
	}

	void SegmentTracker::markSegmentFree(uint64_t offset) {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Ensure offset is aligned to TOTAL_SEGMENT_SIZE
		if ((offset - FILE_HEADER_SIZE) % TOTAL_SEGMENT_SIZE != 0) {
			throw std::runtime_error("Attempting to free segment at non-aligned offset: " + std::to_string(offset));
		}

		// Remove from segments map
		if (segments_.find(offset) != segments_.end()) {
			segments_.erase(offset);
		}

		// Add to free segments list
		freeSegments_.push_back(offset);
	}

	std::vector<uint64_t> SegmentTracker::getFreeSegments() const {
		// std::lock_guard<std::mutex> lock(mutex_);
		return freeSegments_;
	}

	void SegmentTracker::removeFromFreeList(uint64_t offset) {
		// std::lock_guard<std::mutex> lock(mutex_);
		freeSegments_.erase(
			std::remove(freeSegments_.begin(), freeSegments_.end(), offset),
			freeSegments_.end());
	}

	void SegmentTracker::refreshSegmentInfo(uint64_t offset) {
		// Read segment header from disk
		SegmentHeader header = readSegmentHeader(offset);

		// Update cache
		SegmentInfo &info = segments_[offset];
		info.capacity = header.capacity;
		info.used = header.used;
		info.nextSegment = header.next_segment_offset;
		info.prevSegment = header.prev_segment_offset;
	}

	SegmentHeader SegmentTracker::readSegmentHeader(uint64_t offset) const {
		SegmentHeader header;
		file_->seekg(static_cast<std::streamoff>(offset));
		file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		if (!*file_) {
			throw std::runtime_error("Failed to read segment header at offset " + std::to_string(offset));
		}

		return header;
	}

	void SegmentTracker::writeSegmentHeader(uint64_t offset, const SegmentHeader &header) {
		file_->seekp(static_cast<std::streamoff>(offset));
		file_->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));

		if (!*file_) {
			throw std::runtime_error("Failed to write segment header at offset " + std::to_string(offset));
		}
	}

	void SegmentTracker::ensureSegmentCached(uint64_t offset) {
		if (segments_.find(offset) == segments_.end()) {
			// Read from disk and cache
			SegmentHeader header = readSegmentHeader(offset);

			SegmentInfo info;
			info.offset = offset;
			info.type = header.data_type;
			info.capacity = header.capacity;
			info.used = header.used;
			info.inactive = 0; // Default until updated
			info.nextSegment = header.next_segment_offset;
			info.prevSegment = header.prev_segment_offset;

			segments_[offset] = info;
		}
	}

	uint64_t SegmentTracker::getSegmentOffsetForNodeId(uint64_t nodeId) const {
		// std::lock_guard<std::mutex> lock(mutex_);

		uint64_t offset = nodeSegmentHead_;

		while (offset != 0) {
			// Read segment header
			SegmentHeader header = readSegmentHeader(offset);

			// Check if the ID falls within this segment's range
			if (nodeId >= header.start_id && nodeId < header.start_id + header.capacity) {
				return offset;
			}

			offset = header.next_segment_offset;
		}

		return 0; // Not found
	}

	uint64_t SegmentTracker::getSegmentOffsetForEdgeId(uint64_t edgeId) const {
		// std::lock_guard<std::mutex> lock(mutex_);

		uint64_t offset = edgeSegmentHead_;

		while (offset != 0) {
			// Read segment header
			SegmentHeader header = readSegmentHeader(offset);

			// Check if the ID falls within this segment's range
			if (edgeId >= header.start_id && edgeId < header.start_id + header.capacity) {
				return offset;
			}

			offset = header.next_segment_offset;
		}

		return 0; // Not found
	}

	void SegmentTracker::setEntityActive(uint64_t offset, uint32_t index, bool active) {
		// std::lock_guard<std::mutex> lock(mutex_);

		ensureSegmentCached(offset);

		// Read header
		SegmentHeader header = readSegmentHeader(offset);

		// Get current status
		bool wasActive = bitmap::getBit(header.activity_bitmap, index);

		// If status is changing
		if (wasActive != active) {
			// Update bitmap
			bitmap::setBit(header.activity_bitmap, index, active);

			// Update inactive count
			if (active) {
				header.inactive_count = (header.inactive_count > 0) ? header.inactive_count - 1 : 0;
			} else {
				header.inactive_count++;
			}

			// Write updated header
			writeSegmentHeader(offset, header);

			// Update in-memory segment info
			if (segments_.find(offset) != segments_.end()) {
				segments_[offset].inactive = header.inactive_count;
			}
		}
	}

	bool SegmentTracker::isEntityActive(uint64_t offset, uint32_t index) const {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Read header
		SegmentHeader header = readSegmentHeader(offset);

		// Check bitmap
		return bitmap::getBit(header.activity_bitmap, index);
	}

	uint32_t SegmentTracker::countActiveEntities(uint64_t offset) const {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Read header
		SegmentHeader header = readSegmentHeader(offset);

		// Return active count
		return header.used - header.inactive_count;
	}

	void SegmentTracker::initializeActivityBitmap(uint64_t offset, uint32_t capacity) {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Read header
		SegmentHeader header = readSegmentHeader(offset);

		// Calculate bitmap size
		header.bitmap_size = bitmap::calculateBitmapSize(capacity);

		// Initialize bitmap with all active
		memset(header.activity_bitmap, 0xFF, header.bitmap_size);

		// Write updated header
		writeSegmentHeader(offset, header);
	}

	void SegmentTracker::updateActivityBitmap(uint64_t offset, const std::vector<bool> &activityMap) {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Read header
		SegmentHeader header = readSegmentHeader(offset);

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

		// Write updated header
		writeSegmentHeader(offset, header);

		// Update in-memory segment info
		if (segments_.find(offset) != segments_.end()) {
			segments_[offset].inactive = inactiveCount;
		}
	}

	std::vector<bool> SegmentTracker::getActivityBitmap(uint64_t offset) const {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Read header
		SegmentHeader header = readSegmentHeader(offset);

		// Create activity map
		std::vector<bool> activityMap(header.used);

		// Fill from bitmap
		for (uint32_t i = 0; i < header.used; i++) {
			activityMap[i] = bitmap::getBit(header.activity_bitmap, i);
		}

		return activityMap;
	}

} // namespace graph::storage
