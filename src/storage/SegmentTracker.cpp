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
#include <iostream>
#include <stdexcept>
#include <vector>
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentTypeRegistry.hpp"
#include "graph/utils/FixedSizeSerializer.hpp"

namespace graph::storage {

	// Helper to cast enum class to underlying type
	template<typename E>
	constexpr auto to_underlying(E e) noexcept {
		return static_cast<std::underlying_type_t<E>>(e);
	}

	SegmentTracker::SegmentTracker(std::shared_ptr<std::fstream> file, const FileHeader &fileHeader) :
		file_(std::move(file)) {
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
		std::lock_guard<std::recursive_mutex> lock(mutex_);

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
		while (offset != 0) {
			SegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(offset));
			if (!file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader))) {
				break;
			}

			if (header.data_type != expectedType) {
				std::cerr << "Warning: Segment type mismatch at " << offset << std::endl;
				break;
			}

			header.file_offset = offset;
			segments_[offset] = header;
			offset = header.next_segment_offset;
		}
	}

	void SegmentTracker::registerSegment(const SegmentHeader &header) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		segments_[header.file_offset] = header;

		if (auto indexMgr = segmentIndexManager_.lock()) {
			indexMgr->updateSegmentIndex(header, -1);
		}
	}

	void SegmentTracker::updateSegmentUsage(uint64_t offset, uint32_t used, uint32_t inactive) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		ensureSegmentCached(offset);
		SegmentHeader &header = segments_[offset];

		// Capture old start_id (though usage update rarely changes start_id, it keeps API consistent)
		int64_t oldStartId = header.start_id;

		if (header.used != used || header.inactive_count != inactive) {
			header.used = used;
			header.inactive_count = inactive;
			header.is_dirty = 1;
			markSegmentDirty(offset);

			// Pass oldStartId
			if (auto indexMgr = segmentIndexManager_.lock()) {
				indexMgr->updateSegmentIndex(header, oldStartId);
			}
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
		// std::lock_guard<std::recursive_mutex> lock(mutex_);
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
		for (const auto &val: segments_ | std::views::values) {
			if (val.data_type == type) {
				result.push_back(val);
			}
		}
		return result;
	}

	std::vector<SegmentHeader> SegmentTracker::getSegmentsNeedingCompaction(uint32_t type, double threshold) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
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
		std::lock_guard<std::recursive_mutex> lock(mutex_);
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
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		return registry_.getChainHead(static_cast<EntityType>(type));
	}

	void SegmentTracker::updateChainHead(uint32_t type, uint64_t newHead) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		registry_.setChainHead(static_cast<EntityType>(type), newHead);
	}

	void SegmentTracker::updateSegmentLinks(uint64_t offset, uint64_t prevOffset, uint64_t nextOffset) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		ensureSegmentCached(offset);
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
		}
	}

	void SegmentTracker::markSegmentFree(uint64_t offset) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if ((offset - FILE_HEADER_SIZE) % TOTAL_SEGMENT_SIZE != 0) {
			throw std::runtime_error("Invalid segment offset alignment");
		}

		if (segments_.contains(offset)) {
			if (auto indexMgr = segmentIndexManager_.lock()) {
				indexMgr->removeSegmentIndex(segments_[offset]);
			}
		}

		freeSegments_.insert(offset);
		segments_.erase(offset);

		dirtySegments_.erase(offset);

		SegmentHeader emptyHeader{};
		emptyHeader.data_type = 0xFF;
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
		SegmentHeader headerToWrite = header;
		file_->seekp(static_cast<std::streamoff>(offset));
		file_->write(reinterpret_cast<const char *>(&headerToWrite), sizeof(SegmentHeader));
		if (!*file_)
			throw std::runtime_error("Failed to write segment header");

		segments_[offset] = headerToWrite;
		segments_[offset].file_offset = offset;
		segments_[offset].is_dirty = 0;

		dirtySegments_.erase(offset);
	}

	void SegmentTracker::updateSegmentHeader(uint64_t offset, const std::function<void(SegmentHeader &)> &updateFn) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		ensureSegmentCached(offset);

		SegmentHeader &header = segments_[offset];

		int64_t oldStartId = header.start_id; // <--- CRITICAL

		updateFn(header);

		header.is_dirty = 1;
		markSegmentDirty(offset);

		if (auto indexMgr = segmentIndexManager_.lock()) {
			indexMgr->updateSegmentIndex(header, oldStartId);
		}
	}

	void SegmentTracker::setBitmapBit(uint64_t offset, uint32_t index, bool value) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		ensureSegmentCached(offset);
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
		std::lock_guard<std::recursive_mutex> lock(mutex_);
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

	bool SegmentTracker::isEntityActive(uint64_t offset, uint32_t index) const { return getBitmapBit(offset, index); }

	uint32_t SegmentTracker::countActiveEntities(uint64_t offset) const {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		auto it = segments_.find(offset);
		if (it == segments_.end())
			throw std::runtime_error("Segment not found");
		return it->second.used - it->second.inactive_count;
	}

	bool SegmentTracker::isIdInUsedRange(uint64_t segmentOffset, int64_t entityId) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		ensureSegmentCached(segmentOffset);
		const SegmentHeader &header = segments_[segmentOffset];

		return (entityId >= header.start_id && entityId < header.start_id + header.used);
	}

	// =========================================================================
	// OPTIMIZED LOOKUP: Uses SegmentIndexManager for O(log N) performance
	// =========================================================================
	uint64_t SegmentTracker::getSegmentOffsetForEntityId(EntityType type, int64_t entityId) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		// 1. Fast Path: Use Index Manager
		if (auto indexMgr = segmentIndexManager_.lock()) {
			uint64_t offset = indexMgr->findSegmentForId(static_cast<uint32_t>(type), entityId);
			if (offset != 0) {
				return offset;
			}
		}

		// 2. Slow Path: Linear Scan (Fallback if index is not ready)
		uint64_t offset = registry_.getChainHead(type);
		while (offset != 0) {
			SegmentHeader &header = getSegmentHeader(offset);
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

	void SegmentTracker::ensureSegmentCached(uint64_t offset) {
		if (!segments_.contains(offset)) {
			SegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(offset));
			if (!file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader))) {
				throw std::runtime_error("Failed to read segment header at offset " + std::to_string(offset));
			}
			header.file_offset = offset;
			header.needs_compaction = 0;
			header.is_dirty = 0;
			segments_[offset] = header;
		}
	}

	void SegmentTracker::markSegmentDirty(uint64_t offset) { dirtySegments_.insert(offset); }

	void SegmentTracker::flushDirtySegments() {
		std::lock_guard<std::recursive_mutex> lock(mutex_);

		if (dirtySegments_.empty())
			return;

		// Sort segments by offset to optimize disk seek times (Sequential Write)
		std::vector sortedSegments(dirtySegments_.begin(), dirtySegments_.end());
		std::ranges::sort(sortedSegments);

		for (uint64_t offset: sortedSegments) {
			auto it = segments_.find(offset);
			// Check valid and dirty flag (double check)
			if (it != segments_.end() && it->second.is_dirty) {
				SegmentHeader &header = it->second;
				SegmentHeader headerToWrite = header; // Copy to ensure alignment/padding if needed

				file_->seekp(static_cast<std::streamoff>(offset));
				file_->write(reinterpret_cast<const char *>(&headerToWrite), sizeof(SegmentHeader));

				it->second.is_dirty = 0;
			}
		}

		dirtySegments_.clear();
		file_->flush();
	}

	// Template Instantiations
	template<typename T>
	T SegmentTracker::readEntity(uint64_t segmentOffset, uint32_t itemIndex, size_t itemSize) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		ensureSegmentCached(segmentOffset);
		uint64_t itemOffset = segmentOffset + sizeof(SegmentHeader) + itemIndex * itemSize;
		file_->seekg(static_cast<std::streamoff>(itemOffset));
		return utils::FixedSizeSerializer::deserializeWithFixedSize<T>(*file_, itemSize);
	}

	template<typename T>
	void SegmentTracker::writeEntity(uint64_t segmentOffset, uint32_t itemIndex, const T &entity, size_t itemSize) {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		ensureSegmentCached(segmentOffset);
		uint64_t itemOffset = segmentOffset + sizeof(SegmentHeader) + itemIndex * itemSize;
		file_->seekp(static_cast<std::streamoff>(itemOffset));
		utils::FixedSizeSerializer::serializeWithFixedSize(*file_, entity, itemSize);

		auto it = segments_.find(segmentOffset);
		if (it != segments_.end()) {
			it->second.is_dirty = 1;
			markSegmentDirty(segmentOffset);
		}
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

} // namespace graph::storage
