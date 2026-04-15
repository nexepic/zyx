/**
 * @file SegmentTracker.cpp
 * @author Nexepic
 * @date 2025/4/11
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

#include "graph/storage/SegmentTracker.hpp"
#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

namespace graph::storage {

	// Helper to cast enum class to underlying type
	template<typename E>
	constexpr auto to_underlying(E e) noexcept {
		return static_cast<std::underlying_type_t<E>>(e);
	}

	SegmentTracker::SegmentTracker(std::shared_ptr<StorageIO> io, const FileHeader &fileHeader) :
		io_(std::move(io)) {
		initialize(fileHeader);
	}

	SegmentTracker::~SegmentTracker() = default;

	void SegmentTracker::initializeRegistry() {
		registry_.clear();
		registry_.registerType(EntityType::Node);
		registry_.registerType(EntityType::Edge);
		registry_.registerType(EntityType::Property);
		registry_.registerType(EntityType::Blob);
		registry_.registerType(EntityType::Index);
		registry_.registerType(EntityType::State);
	}

	void SegmentTracker::initialize(const FileHeader &header) {
		std::unique_lock lock(mutex_);

		initializeRegistry();

		registry_.setChainHead(EntityType::Node, header.node_segment_head);
		registry_.setChainHead(EntityType::Edge, header.edge_segment_head);
		registry_.setChainHead(EntityType::Property, header.property_segment_head);
		registry_.setChainHead(EntityType::Blob, header.blob_segment_head);
		registry_.setChainHead(EntityType::Index, header.index_segment_head);
		registry_.setChainHead(EntityType::State, header.state_segment_head);

		segments_.clear();
		freeSegments_.clear();
		dirtySegments_.clear();

		loadSegments();
	}

	void SegmentTracker::loadSegments() {
		for (const auto &type: registry_.getAllTypes()) {
			uint64_t headOffset = registry_.getChainHead(type);
			loadSegmentChain(headOffset, to_underlying(type));
		}
	}

	void SegmentTracker::loadSegmentChain(uint64_t headOffset, uint32_t expectedType) {
		uint64_t offset = headOffset;
		uint64_t lastOffset = 0;
		while (offset != 0) {
			SegmentHeader header;
			size_t bytesRead = io_->readAt(offset, &header, sizeof(SegmentHeader));
			if (bytesRead < sizeof(SegmentHeader)) {
				break;
			}

			if (header.data_type != expectedType) {
				std::cerr << "Warning: Segment type mismatch at " << offset << std::endl;
				break;
			}

			header.file_offset = offset;
			segments_[offset] = header;
			lastOffset = offset;
			offset = header.next_segment_offset;
		}
		if (lastOffset != 0) {
			registry_.setChainTail(static_cast<EntityType>(expectedType), lastOffset);
		}
	}

	void SegmentTracker::registerSegment(const SegmentHeader &header) {
		std::unique_lock lock(mutex_);

		segments_[header.file_offset] = header;

		if (auto indexMgr = segmentIndexManager_.lock()) {
			indexMgr->updateSegmentIndex(header, -1);
		}
	}

	void SegmentTracker::updateSegmentUsage(uint64_t offset, uint32_t used, uint32_t inactive) {
		std::unique_lock lock(mutex_);
		ensureSegmentCached_locked(offset);
		SegmentHeader &header = segments_[offset];

		int64_t oldStartId = header.start_id;

		if (header.used != used || header.inactive_count != inactive) {
			header.used = used;
			header.inactive_count = inactive;
			header.is_dirty = 1;
			markSegmentDirty(offset);

			if (auto indexMgr = segmentIndexManager_.lock()) {
				indexMgr->updateSegmentIndex(header, oldStartId);
			}
		}
	}

	void SegmentTracker::markForCompaction(uint64_t offset, bool needsCompaction) {
		std::unique_lock lock(mutex_);
		ensureSegmentCached_locked(offset);
		SegmentHeader &header = segments_[offset];
		if ((header.needs_compaction == 1) != needsCompaction) {
			header.needs_compaction = needsCompaction ? 1 : 0;
			header.is_dirty = 1;
			markSegmentDirty(offset);
		}
	}

	SegmentHeader &SegmentTracker::getSegmentHeader(uint64_t offset) {
		std::unique_lock lock(mutex_);
		ensureSegmentCached_locked(offset);
		return getSegmentHeader_locked(offset);
	}

	SegmentHeader SegmentTracker::getSegmentHeaderCopy(uint64_t offset) const {
		// Fast path: shared_lock allows concurrent readers
		{
			std::shared_lock lock(mutex_);
			auto it = segments_.find(offset);
			if (it != segments_.end())
				return it->second;
		}
		// Slow path: segment not cached (should be rare after initialization)
		SegmentHeader header;
		size_t n = io_->readAt(offset, &header, sizeof(SegmentHeader));
		if (n >= sizeof(SegmentHeader)) {
			header.file_offset = offset;
			return header;
		}
		throw std::runtime_error("Segment not found at offset " + std::to_string(offset));
	}

	std::vector<SegmentHeader> SegmentTracker::getSegmentsByType(uint32_t type) const {
		std::shared_lock lock(mutex_);
		std::vector<SegmentHeader> result;
		for (const auto &val: segments_ | std::views::values) {
			if (val.data_type == type) {
				result.push_back(val);
			}
		}
		return result;
	}

	std::vector<SegmentHeader> SegmentTracker::getSegmentsNeedingCompaction(uint32_t type, double threshold) const {
		std::shared_lock lock(mutex_);
		std::vector<SegmentHeader> result;
		for (const auto &header: segments_ | std::views::values) {
			if (header.data_type == type && (header.needs_compaction || header.getFragmentationRatio() >= threshold)) {
				result.push_back(header);
			}
		}
		std::ranges::sort(result, [](const SegmentHeader &a, const SegmentHeader &b) {
			return a.getFragmentationRatio() > b.getFragmentationRatio();
		});
		return result;
	}

	double SegmentTracker::calculateFragmentationRatio(uint32_t type) const {
		std::shared_lock lock(mutex_);
		double totalCapacity = 0;
		double totalUtilizedSpace = 0;
		for (const auto &header: segments_ | std::views::values) {
			if (header.data_type == type) {
				totalCapacity += header.capacity;
				totalUtilizedSpace += (header.used - header.inactive_count);
			}
		}
		if (totalCapacity == 0)
			return 0.0;
		return 1.0 - (totalUtilizedSpace / totalCapacity);
	}

	uint64_t SegmentTracker::getChainHead(uint32_t type) const {
		std::shared_lock lock(mutex_);
		return registry_.getChainHead(static_cast<EntityType>(type));
	}

	void SegmentTracker::updateChainHead(uint32_t type, uint64_t newHead) {
		std::unique_lock lock(mutex_);
		registry_.setChainHead(static_cast<EntityType>(type), newHead);
	}

	uint64_t SegmentTracker::getChainTail(uint32_t type) const {
		std::shared_lock lock(mutex_);
		return registry_.getChainTail(static_cast<EntityType>(type));
	}

	void SegmentTracker::updateChainTail(uint32_t type, uint64_t newTail) {
		std::unique_lock lock(mutex_);
		registry_.setChainTail(static_cast<EntityType>(type), newTail);
	}

	void SegmentTracker::updateSegmentLinks(uint64_t offset, uint64_t prevOffset, uint64_t nextOffset) {
		std::unique_lock lock(mutex_);
		ensureSegmentCached_locked(offset);
		SegmentHeader &header = segments_[offset];

		bool changed = false;
		if (header.prev_segment_offset != prevOffset) {
			if (prevOffset == offset)
				throw std::runtime_error("Self-loop detected");
			header.prev_segment_offset = prevOffset;
			changed = true;
		}
		if (header.next_segment_offset != nextOffset) {
			if (nextOffset == offset)
				throw std::runtime_error("Self-loop detected");
			header.next_segment_offset = nextOffset;
			changed = true;
		}
		if (changed) {
			header.is_dirty = 1;
			markSegmentDirty(offset);

			if (nextOffset == 0) {
				registry_.setChainTail(static_cast<EntityType>(header.data_type), offset);
			}
		}
	}

	void SegmentTracker::markSegmentFree(uint64_t offset) {
		std::unique_lock lock(mutex_);
		if ((offset - FILE_HEADER_SIZE) % TOTAL_SEGMENT_SIZE != 0) {
			throw std::runtime_error("Invalid segment offset alignment");
		}

		if (segments_.contains(offset)) {
			const SegmentHeader &header = segments_[offset];

			auto type = static_cast<EntityType>(header.data_type);
			if (registry_.getChainTail(type) == offset) {
				registry_.setChainTail(type, header.prev_segment_offset);
			}

			if (auto indexMgr = segmentIndexManager_.lock()) {
				indexMgr->removeSegmentIndex(segments_[offset]);
			}
		}

		freeSegments_.insert(offset);
		segments_.erase(offset);

		dirtySegments_.erase(offset);
		validatedSegments_.erase(offset);

		SegmentHeader emptyHeader{};
		emptyHeader.data_type = 0xFF;
		io_->writeAt(offset, &emptyHeader, sizeof(SegmentHeader));
	}

	std::vector<uint64_t> SegmentTracker::getFreeSegments() const {
		std::shared_lock lock(mutex_);
		return {freeSegments_.begin(), freeSegments_.end()};
	}

	void SegmentTracker::removeFromFreeList(uint64_t offset) {
		std::unique_lock lock(mutex_);
		freeSegments_.erase(offset);
	}

	void SegmentTracker::writeSegmentHeader(uint64_t offset, const SegmentHeader &header) {
		std::unique_lock lock(mutex_);

		SegmentHeader headerToWrite = header;
		io_->flushStream();

		uint64_t dataOffset = offset + sizeof(SegmentHeader);
		std::vector<char> buffer(SEGMENT_SIZE, 0);

		size_t n = io_->readAt(dataOffset, buffer.data(), SEGMENT_SIZE);
		if (n > 0) {
			headerToWrite.segment_crc = utils::calculateCrc(buffer.data(), SEGMENT_SIZE);
		}

		io_->writeAt(offset, &headerToWrite, sizeof(SegmentHeader));

		segments_[offset] = headerToWrite;
		segments_[offset].file_offset = offset;
		segments_[offset].is_dirty = 0;

		dirtySegments_.erase(offset);
		validatedSegments_.erase(offset);
	}

	void SegmentTracker::updateSegmentHeader(uint64_t offset, const std::function<void(SegmentHeader &)> &updateFn) {
		std::unique_lock lock(mutex_);
		ensureSegmentCached_locked(offset);

		SegmentHeader &header = segments_[offset];

		int64_t oldStartId = header.start_id;

		updateFn(header);

		header.is_dirty = 1;
		markSegmentDirty(offset);

		if (auto indexMgr = segmentIndexManager_.lock()) {
			indexMgr->updateSegmentIndex(header, oldStartId);
		}
	}

	void SegmentTracker::setBitmapBit(uint64_t offset, uint32_t index, bool value) {
		std::unique_lock lock(mutex_);
		ensureSegmentCached_locked(offset);
		SegmentHeader &header = segments_[offset];

		bool oldValue = bitmap::getBit(header.activity_bitmap, index);
		if (oldValue != value) {
			bitmap::setBit(header.activity_bitmap, index, value);
			if (value) {
				if (header.inactive_count > 0)
					header.inactive_count--;
			} else {
				header.inactive_count++;
			}
			header.is_dirty = 1;
			markSegmentDirty(offset);
		}
	}

	bool SegmentTracker::getBitmapBit(uint64_t offset, uint32_t index) const {
		std::shared_lock lock(mutex_);
		auto it = segments_.find(offset);
		if (it != segments_.end()) {
			return bitmap::getBit(it->second.activity_bitmap, index);
		}
		throw std::runtime_error("Segment not found at offset " + std::to_string(offset));
	}

	void SegmentTracker::updateActivityBitmap(uint64_t offset, const std::vector<bool> &activityMap) {
		std::unique_lock lock(mutex_);
		ensureSegmentCached_locked(offset);
		SegmentHeader &header = segments_[offset];

		header.bitmap_size = bitmap::calculateBitmapSize(static_cast<uint32_t>(activityMap.size()));
		std::memset(header.activity_bitmap, 0, header.bitmap_size);

		uint32_t inactiveCount = 0;
		for (size_t i = 0; i < activityMap.size(); i++) {
			bitmap::setBit(header.activity_bitmap, static_cast<uint32_t>(i), activityMap[i]);
			if (!activityMap[i])
				inactiveCount++;
		}
		header.inactive_count = inactiveCount;
		header.is_dirty = 1;
		markSegmentDirty(offset);
	}

	std::vector<bool> SegmentTracker::getActivityBitmap(uint64_t offset) const {
		std::shared_lock lock(mutex_);
		auto it = segments_.find(offset);
		if (it == segments_.end())
			throw std::runtime_error("Segment not found");

		const SegmentHeader &header = it->second;
		std::vector<bool> activityMap(header.used);
		for (uint32_t i = 0; i < header.used; i++) {
			activityMap[i] = bitmap::getBit(header.activity_bitmap, i);
		}
		return activityMap;
	}

	void SegmentTracker::setEntityActive(uint64_t offset, uint32_t index, bool active) {
		setBitmapBit(offset, index, active);
	}

	void SegmentTracker::setEntityActiveRange(uint64_t offset, uint32_t startIndex, uint32_t count, bool active) {
		std::unique_lock lock(mutex_);
		ensureSegmentCached_locked(offset);
		SegmentHeader &header = segments_[offset];

		uint32_t changed = 0;
		for (uint32_t i = 0; i < count; i++) {
			uint32_t idx = startIndex + i;
			bool oldValue = bitmap::getBit(header.activity_bitmap, idx);
			if (oldValue != active) {
				bitmap::setBit(header.activity_bitmap, idx, active);
				changed++;
			}
		}

		if (changed > 0) {
			if (active) {
				header.inactive_count = (header.inactive_count >= changed) ? header.inactive_count - changed : 0;
			} else {
				header.inactive_count += changed;
			}
			header.is_dirty = 1;
			markSegmentDirty(offset);
		}
	}

	void SegmentTracker::batchSetEntityActive(uint64_t offset,
											  const std::vector<std::pair<uint32_t, bool>> &indexActiveList) {
		if (indexActiveList.empty()) return;

		std::unique_lock lock(mutex_);
		ensureSegmentCached_locked(offset);
		SegmentHeader &header = segments_[offset];

		uint32_t activated = 0;
		uint32_t deactivated = 0;
		for (const auto &[index, active] : indexActiveList) {
			bool oldValue = bitmap::getBit(header.activity_bitmap, index);
			if (oldValue != active) {
				bitmap::setBit(header.activity_bitmap, index, active);
				if (active) {
					activated++;
				} else {
					deactivated++;
				}
			}
		}

		if (activated > 0 || deactivated > 0) {
			header.inactive_count = (header.inactive_count >= activated)
				? header.inactive_count - activated + deactivated
				: deactivated;
			header.is_dirty = 1;
			markSegmentDirty(offset);
		}
	}

	bool SegmentTracker::isEntityActive(uint64_t offset, uint32_t index) const { return getBitmapBit(offset, index); }

	uint32_t SegmentTracker::countActiveEntities(uint64_t offset) const {
		std::shared_lock lock(mutex_);
		auto it = segments_.find(offset);
		if (it == segments_.end())
			throw std::runtime_error("Segment not found");
		return it->second.used - it->second.inactive_count;
	}

	bool SegmentTracker::isIdInUsedRange(uint64_t segmentOffset, int64_t entityId) {
		std::unique_lock lock(mutex_);
		ensureSegmentCached_locked(segmentOffset);
		const SegmentHeader &header = segments_[segmentOffset];

		return (entityId >= header.start_id && entityId < header.start_id + header.used);
	}

	// =========================================================================
	// OPTIMIZED LOOKUP: Uses SegmentIndexManager for O(log N) performance
	// =========================================================================
	uint64_t SegmentTracker::getSegmentOffsetForEntityId(EntityType type, int64_t entityId) {
		// 1. Fast Path: Use Index Manager (its own shared_lock protects the index)
		if (auto indexMgr = segmentIndexManager_.lock()) {
			uint64_t offset = indexMgr->findSegmentForId(static_cast<uint32_t>(type), entityId);
			if (offset != 0) {
				return offset;
			}
		}

		// 2. Slow Path: Linear Scan (Fallback if index is not ready)
		std::unique_lock lock(mutex_);
		uint64_t offset = registry_.getChainHead(type);
		while (offset != 0) {
			ensureSegmentCached_locked(offset);
			SegmentHeader &header = getSegmentHeader_locked(offset);
			if (entityId >= header.start_id && entityId < header.start_id + header.capacity) {
				return offset;
			}
			offset = header.next_segment_offset;
		}

		return 0; // Not found
	}

	uint64_t SegmentTracker::getSegmentOffsetForNodeId(int64_t nodeId) {
		return getSegmentOffsetForEntityId(EntityType::Node, nodeId);
	}
	uint64_t SegmentTracker::getSegmentOffsetForEdgeId(int64_t edgeId) {
		return getSegmentOffsetForEntityId(EntityType::Edge, edgeId);
	}
	uint64_t SegmentTracker::getSegmentOffsetForPropId(int64_t propId) {
		return getSegmentOffsetForEntityId(EntityType::Property, propId);
	}
	uint64_t SegmentTracker::getSegmentOffsetForBlobId(int64_t blobId) {
		return getSegmentOffsetForEntityId(EntityType::Blob, blobId);
	}
	uint64_t SegmentTracker::getSegmentOffsetForIndexId(int64_t indexId) {
		return getSegmentOffsetForEntityId(EntityType::Index, indexId);
	}
	uint64_t SegmentTracker::getSegmentOffsetForStateId(int64_t stateId) {
		return getSegmentOffsetForEntityId(EntityType::State, stateId);
	}

	// ── Internal helpers (caller must hold appropriate lock) ─────────────────

	void SegmentTracker::ensureSegmentCached_locked(uint64_t offset) {
		if (!segments_.contains(offset)) {
			SegmentHeader header;
			size_t n = io_->readAt(offset, &header, sizeof(SegmentHeader));
			if (n < sizeof(SegmentHeader)) {
				throw std::runtime_error("Failed to read segment header at offset " + std::to_string(offset));
			}
			header.file_offset = offset;
			header.needs_compaction = 0;
			header.is_dirty = 0;
			segments_[offset] = header;
		}
	}

	SegmentHeader &SegmentTracker::getSegmentHeader_locked(uint64_t offset) {
		auto it = segments_.find(offset);
		if (it == segments_.end()) {
			throw std::runtime_error("Segment not found at offset " + std::to_string(offset));
		}
		return it->second;
	}

	void SegmentTracker::markSegmentDirty(uint64_t offset) { dirtySegments_.insert(offset); }

	void SegmentTracker::flushDirtySegments() {
		std::unique_lock lock(mutex_);

		if (dirtySegments_.empty())
			return;

		// Sort segments by offset to optimize disk seek times (Sequential Write)
		std::vector sortedSegments(dirtySegments_.begin(), dirtySegments_.end());
		std::ranges::sort(sortedSegments);

		// Check which segments need a read-back vs already have a pending CRC
		bool needFlushForReadBack = false;
		for (uint64_t offset : sortedSegments) {
			auto it = segments_.find(offset);
			if (it != segments_.end() && it->second.is_dirty && !pendingCrcs_.contains(offset)) {
				needFlushForReadBack = true;
				break;
			}
		}

		// Only flush the stream if we need to read back data for CRC computation
		if (needFlushForReadBack) {
			io_->flushStream();
		}

		// Compute CRC for each dirty segment's data region before writing headers
		for (uint64_t offset : sortedSegments) {
			auto it = segments_.find(offset);
			if (it != segments_.end() && it->second.is_dirty) {
				// Use pre-computed CRC if available, otherwise fall back to read-back
				auto crcIt = pendingCrcs_.find(offset);
				if (crcIt != pendingCrcs_.end()) {
					it->second.segment_crc = crcIt->second;
				} else {
					calculateAndUpdateSegmentCrc(offset);
				}
			}
		}

		pendingCrcs_.clear();

		for (uint64_t offset: sortedSegments) {
			auto it = segments_.find(offset);
			if (it != segments_.end() && it->second.is_dirty) {
				SegmentHeader &header = it->second;
				SegmentHeader headerToWrite = header;

				io_->writeAt(offset, &headerToWrite, sizeof(SegmentHeader));

				it->second.is_dirty = 0;
			}
		}

		dirtySegments_.clear();
		io_->flushStream();
	}

	// Template Instantiations
	template<typename T>
	T SegmentTracker::readEntity(uint64_t segmentOffset, uint32_t itemIndex, size_t itemSize) {
		std::unique_lock lock(mutex_);
		ensureSegmentCached_locked(segmentOffset);

		// Lazy CRC validation: verify segment data on first read
		if (!validatedSegments_.contains(segmentOffset)) {
			validateSegmentCrc(segmentOffset);
		}

		uint64_t itemOffset = segmentOffset + sizeof(SegmentHeader) + itemIndex * itemSize;
		std::vector<char> buf(itemSize);
		size_t bytesRead = io_->readAt(itemOffset, buf.data(), itemSize);
		if (bytesRead < itemSize) {
			throw std::runtime_error("Failed to read entity at offset " + std::to_string(itemOffset));
		}
		std::string bufStr(buf.begin(), buf.end());
		std::istringstream iss(bufStr, std::ios::binary);
		return utils::FixedSizeSerializer::deserializeWithFixedSize<T>(iss, itemSize);
	}

	template<typename T>
	void SegmentTracker::writeEntity(uint64_t segmentOffset, uint32_t itemIndex, const T &entity, size_t itemSize) {
		std::unique_lock lock(mutex_);
		ensureSegmentCached_locked(segmentOffset);
		uint64_t itemOffset = segmentOffset + sizeof(SegmentHeader) + itemIndex * itemSize;
		auto buf = utils::FixedSizeSerializer::serializeToBuffer(entity, itemSize);
		io_->writeAt(itemOffset, buf.data(), buf.size());

		auto it = segments_.find(segmentOffset);
		if (it != segments_.end()) {
			it->second.is_dirty = 1;
			markSegmentDirty(segmentOffset);
		}

		// Invalidate CRC validation cache since data changed
		validatedSegments_.erase(segmentOffset);
	}

	template Node SegmentTracker::readEntity<Node>(uint64_t, uint32_t, size_t);
	template Edge SegmentTracker::readEntity<Edge>(uint64_t, uint32_t, size_t);
	template Property SegmentTracker::readEntity<Property>(uint64_t, uint32_t, size_t);
	template Blob SegmentTracker::readEntity<Blob>(uint64_t, uint32_t, size_t);
	template Index SegmentTracker::readEntity<Index>(uint64_t, uint32_t, size_t);
	template State SegmentTracker::readEntity<State>(uint64_t, uint32_t, size_t);

	template void SegmentTracker::writeEntity<Node>(uint64_t, uint32_t, const Node &, size_t);
	template void SegmentTracker::writeEntity<Edge>(uint64_t, uint32_t, const Edge &, size_t);
	template void SegmentTracker::writeEntity<Property>(uint64_t, uint32_t, const Property &, size_t);
	template void SegmentTracker::writeEntity<Blob>(uint64_t, uint32_t, const Blob &, size_t);
	template void SegmentTracker::writeEntity<Index>(uint64_t, uint32_t, const Index &, size_t);
	template void SegmentTracker::writeEntity<State>(uint64_t, uint32_t, const State &, size_t);

	void SegmentTracker::setPendingCrc(uint64_t segmentOffset, uint32_t crc) {
		std::unique_lock lock(mutex_);
		pendingCrcs_[segmentOffset] = crc;
	}

	void SegmentTracker::calculateAndUpdateSegmentCrc(uint64_t segmentOffset) {
		// Caller must hold unique lock and have flushed the file before calling.
		uint64_t dataOffset = segmentOffset + sizeof(SegmentHeader);
		std::vector<char> buffer(SEGMENT_SIZE, 0);

		size_t n = io_->readAt(dataOffset, buffer.data(), SEGMENT_SIZE);
		if (n == 0) return;

		uint32_t crc = utils::calculateCrc(buffer.data(), SEGMENT_SIZE);

		auto it = segments_.find(segmentOffset);
		if (it != segments_.end()) {
			it->second.segment_crc = crc;
		}
	}

	void SegmentTracker::validateSegmentCrc(uint64_t segmentOffset) {
		// Caller must hold unique lock.
		auto it = segments_.find(segmentOffset);
		if (it == segments_.end()) {
			return;
		}

		const SegmentHeader &header = it->second;

		// Skip validation for empty segments or dirty segments (CRC not yet computed)
		if (header.used == 0 || header.is_dirty || dirtySegments_.contains(segmentOffset)) {
			validatedSegments_.insert(segmentOffset);
			return;
		}

		// Read data region
		uint64_t dataOffset = segmentOffset + sizeof(SegmentHeader);
		std::vector<char> buffer(SEGMENT_SIZE, 0);

		io_->flushStream();
		size_t n = io_->readAt(dataOffset, buffer.data(), SEGMENT_SIZE);
		if (n == 0) {
			throw std::runtime_error("Failed to read segment data at offset " + std::to_string(segmentOffset));
		}

		uint32_t computed = utils::calculateCrc(buffer.data(), SEGMENT_SIZE);

		if (computed != header.segment_crc) {
			throw std::runtime_error(
				"Segment data corruption detected at offset " + std::to_string(segmentOffset)
				+ ", entity ID range [" + std::to_string(header.start_id)
				+ ", " + std::to_string(header.start_id + header.capacity - 1) + "]"
			);
		}

		validatedSegments_.insert(segmentOffset);
	}

	std::vector<uint32_t> SegmentTracker::collectSegmentCrcs() const {
		std::shared_lock lock(mutex_);
		std::vector<std::pair<uint64_t, uint32_t>> ordered;
		ordered.reserve(segments_.size());
		for (const auto &[offset, header] : segments_) {
			ordered.emplace_back(offset, header.segment_crc);
		}
		std::ranges::sort(ordered, {}, &std::pair<uint64_t, uint32_t>::first);
		std::vector<uint32_t> crcs;
		crcs.reserve(ordered.size());
		for (const auto &[offset, crc] : ordered) {
			crcs.push_back(crc);
		}
		return crcs;
	}

} // namespace graph::storage
