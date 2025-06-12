/**
 * @file SegmentTracker.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/SegmentTracker.hpp"
#include <algorithm>
#include <cstring>
#include <functional>
#include <graph/storage/SegmentIndexManager.hpp>
#include <graph/storage/SegmentType.hpp>
#include <graph/utils/ChecksumUtils.hpp>
#include <iostream>
#include <stdexcept>
#include "graph/utils/FixedSizeSerializer.hpp"

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

	void SegmentTracker::loadSegmentChain(uint64_t headOffset, uint32_t expectedType) {
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

			// Add to segment map
			segments_[offset] = header;

			// Move to next segment
			offset = header.next_segment_offset;
		}
	}

	void SegmentTracker::registerSegment(uint64_t offset, uint32_t type, uint32_t capacity) {
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

	void SegmentTracker::validateSegmentChains() {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Validate node segment chain
		validateChain(nodeSegmentHead_, toUnderlying(SegmentType::Node));

		// Validate edge segment chain
		validateChain(edgeSegmentHead_, toUnderlying(SegmentType::Edge));

		// Validate property segment chain
		validateChain(propertySegmentHead_, toUnderlying(SegmentType::Property));

		// Validate blob segment chain
		validateChain(blobSegmentHead_, toUnderlying(SegmentType::Blob));
	}

	void SegmentTracker::validateChain(uint64_t headOffset, uint32_t type) {
		if (headOffset == 0) {
			return; // Empty chain, nothing to validate
		}

		uint64_t current = headOffset;
		uint64_t prev = 0;

		while (current != 0) {
			auto it = segments_.find(current);
			if (it == segments_.end()) {
				// Segment not in cache, try to read from disk
				ensureSegmentCached(current);
				it = segments_.find(current);

				if (it == segments_.end()) {
					// Still not found, chain is broken
					if (prev == 0) {
						// This was the head, update chain head to 0
						updateChainHead(type, 0);
					} else {
						// Update previous segment to point to 0
						updateSegmentLinks(prev, segments_[prev].prev_segment_offset, 0);
					}
					break;
				}
			}

			SegmentHeader &header = it->second;

			// Verify type matches
			if (header.data_type != type) {
				// Type mismatch, fix the chain
				if (prev == 0) {
					// This was the head, update chain head to next segment
					updateChainHead(type, header.next_segment_offset);
				} else {
					// Update previous segment to point to next segment
					updateSegmentLinks(prev, segments_[prev].prev_segment_offset, header.next_segment_offset);
				}

				// Remove this segment from the chain
				if (header.next_segment_offset != 0) {
					updateSegmentLinks(header.next_segment_offset, prev,
									   segments_[header.next_segment_offset].next_segment_offset);
				}

				// Skip to next segment
				current = header.next_segment_offset;
				continue;
			}

			// Verify prev_segment_offset matches
			if (header.prev_segment_offset != prev) {
				// Fix prev_segment_offset
				updateSegmentLinks(current, prev, header.next_segment_offset);
			}

			// Move to next segment
			prev = current;
			current = header.next_segment_offset;
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

	SegmentHeader &SegmentTracker::getSegmentHeader(uint64_t offset) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Ensure the segment is cached
		ensureSegmentCached(offset);

		auto it = segments_.find(offset);
		if (it == segments_.end()) {
			throw std::runtime_error("Segment not found at offset " + std::to_string(offset));
		}
		return it->second;
	}

	std::vector<SegmentHeader> SegmentTracker::getSegmentsByType(uint32_t type) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		std::vector<SegmentHeader> result;
		for (const auto &[offset, header]: segments_) {
			if (header.data_type == type) {
				result.push_back(header);
			}
		}
		return result;
	}

	std::vector<SegmentHeader> SegmentTracker::getSegmentsNeedingCompaction(uint32_t type, double threshold) const {
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

	double SegmentTracker::calculateFragmentationRatio(uint32_t type) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		double totalCapacity = 0;
		double totalUtilizedSpace = 0;

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
		double utilizationRate = totalUtilizedSpace / totalCapacity;
		return 1.0 - utilizationRate;
	}

	uint64_t SegmentTracker::getChainHead(uint32_t type) const {
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

	void SegmentTracker::updateChainHead(uint32_t type, uint64_t newHead) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		switch (type) {
			case toUnderlying(SegmentType::Node):
				nodeSegmentHead_ = newHead;
				break;
			case toUnderlying(SegmentType::Edge):
				edgeSegmentHead_ = newHead;
				break;
			case toUnderlying(SegmentType::Property):
				propertySegmentHead_ = newHead;
				break;
			case toUnderlying(SegmentType::Blob):
				blobSegmentHead_ = newHead;
				break;
			default:;
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

		// Add to free segments set
		freeSegments_.insert(offset);

		// Remove from segments map if present
		segments_.erase(offset);

		// Remove from dirty segments list if present
		auto dirtyIt = std::find(dirtySegments_.begin(), dirtySegments_.end(), offset);
		if (dirtyIt != dirtySegments_.end()) {
			dirtySegments_.erase(dirtyIt);
		}

		// Mark the segment as free in the file by zeroing out the first few bytes
		// This is just for safety and diagnostics - we don't actually need the segment header anymore
		SegmentHeader emptyHeader{};
		emptyHeader.data_type = 0xFF; // Special marker for free segments

		file_->seekp(static_cast<std::streamoff>(offset));
		file_->write(reinterpret_cast<const char *>(&emptyHeader), sizeof(SegmentHeader));
	}

	std::vector<uint64_t> SegmentTracker::getFreeSegments() const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		return {freeSegments_.begin(), freeSegments_.end()};
	}

	void SegmentTracker::removeFromFreeList(uint64_t offset) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		freeSegments_.erase(offset);
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

		// TODO: Executing multiple times while performing deleting and saving

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
				headerToWrite.segment_crc = utils::calculateCrc(&headerToWrite, offsetof(SegmentHeader, segment_crc));

				// Write to disk
				file_->seekp(static_cast<std::streamoff>(offset));
				file_->write(reinterpret_cast<const char *>(&headerToWrite), sizeof(SegmentHeader));

				// Mark as clean
				it->second.is_dirty = 0;

				// Update segment index after successful flush
				if (auto segmentIndexManager = segmentIndexManager_.lock()) {
				    segmentIndexManager->updateSegmentIndexByOffset(offset, header);
				} else {
					throw std::runtime_error("SegmentIndexManager is not available");
				}
			}
		}

		// Clear the dirty list
		dirtySegments_.clear();

		// Ensure all writes are flushed to disk
		file_->flush();
	}

	template<typename T>
	T SegmentTracker::readEntity(uint64_t segmentOffset, uint32_t itemIndex, size_t itemSize) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Ensure segment is cached
		ensureSegmentCached(segmentOffset);

		// Calculate item offset within segment
		uint64_t itemOffset = segmentOffset + sizeof(SegmentHeader) + itemIndex * itemSize;

		// Position the file at the correct offset
		file_->seekg(static_cast<std::streamoff>(itemOffset));

		// Use the FixedSizeSerializer to deserialize the entity
		return utils::FixedSizeSerializer::deserializeWithFixedSize<T>(*file_, itemSize);
	}

	template<typename T>
	void SegmentTracker::writeEntity(uint64_t segmentOffset, uint32_t itemIndex, const T &entity, size_t itemSize) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// Ensure segment is cached
		ensureSegmentCached(segmentOffset);

		// Calculate item offset within segment
		uint64_t itemOffset = segmentOffset + sizeof(SegmentHeader) + itemIndex * itemSize;

		// Position the file at the correct offset
		file_->seekp(static_cast<std::streamoff>(itemOffset));

		// Use the FixedSizeSerializer to serialize the entity
		utils::FixedSizeSerializer::serializeWithFixedSize(*file_, entity, itemSize);

		// Mark segment as dirty since we've modified its content
		auto it = segments_.find(segmentOffset);
		if (it != segments_.end()) {
			it->second.is_dirty = 1;
			markSegmentDirty(segmentOffset);
		}
	}

	template Node SegmentTracker::readEntity<Node>(uint64_t segmentOffset, uint32_t itemIndex, size_t itemSize);
	template Edge SegmentTracker::readEntity<Edge>(uint64_t segmentOffset, uint32_t itemIndex, size_t itemSize);
	template Property SegmentTracker::readEntity<Property>(uint64_t segmentOffset, uint32_t itemIndex, size_t itemSize);
	template Blob SegmentTracker::readEntity<Blob>(uint64_t segmentOffset, uint32_t itemIndex, size_t itemSize);

	template void SegmentTracker::writeEntity<Node>(uint64_t segmentOffset, uint32_t itemIndex, const Node &entity,
													size_t itemSize);
	template void SegmentTracker::writeEntity<Edge>(uint64_t segmentOffset, uint32_t itemIndex, const Edge &entity,
													size_t itemSize);
	template void SegmentTracker::writeEntity<Property>(uint64_t segmentOffset, uint32_t itemIndex,
														const Property &entity, size_t itemSize);
	template void SegmentTracker::writeEntity<Blob>(uint64_t segmentOffset, uint32_t itemIndex, const Blob &entity,
													size_t itemSize);

} // namespace graph::storage
