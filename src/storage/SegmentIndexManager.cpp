/**
 * @file SegmentIndexManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/SegmentIndexManager.hpp"
#include <algorithm>
#include <graph/storage/SegmentType.hpp>

namespace graph::storage {

	SegmentIndexManager::SegmentIndexManager(std::shared_ptr<SegmentTracker> segmentTracker) :
		segmentTracker_(std::move(segmentTracker)) {}

	void SegmentIndexManager::initialize(uint64_t &nodeHead, uint64_t &edgeHead, uint64_t &propertyHead,
										 uint64_t &blobHead, uint64_t &indexHead, uint64_t &stateHead) {
		std::lock_guard<std::mutex> lock(mutex_);

		// Store pointers to the head references
		nodeSegmentHead_ = &nodeHead;
		edgeSegmentHead_ = &edgeHead;
		propertySegmentHead_ = &propertyHead;
		blobSegmentHead_ = &blobHead;
		indexSegmentHead_ = &indexHead;
		stateSegmentHead_ = &stateHead;

		// Build indexes using the current head values
		buildSegmentIndexes();
	}

	void SegmentIndexManager::buildSegmentIndexes() {
		// Ensure this method is called with the mutex already locked or lock it here
		// std::lock_guard<std::mutex> lock(mutex_);

		// Use the dereferenced pointers to get the current head values
		buildSegmentIndex(nodeSegmentIndex_, nodeSegmentHead_ ? *nodeSegmentHead_ : 0);
		buildSegmentIndex(edgeSegmentIndex_, edgeSegmentHead_ ? *edgeSegmentHead_ : 0);
		buildSegmentIndex(propertySegmentIndex_, propertySegmentHead_ ? *propertySegmentHead_ : 0);
		buildSegmentIndex(blobSegmentIndex_, blobSegmentHead_ ? *blobSegmentHead_ : 0);
		buildSegmentIndex(indexSegmentIndex_, indexSegmentHead_ ? *indexSegmentHead_ : 0);
		buildSegmentIndex(stateSegmentIndex_, stateSegmentHead_ ? *stateSegmentHead_ : 0);
	}

	void SegmentIndexManager::buildSegmentIndex(std::vector<SegmentIndex> &segmentIndex, uint64_t segmentHead) const {
		segmentIndex.clear();
		uint64_t currentOffset = segmentHead;

		while (currentOffset != 0) {
			SegmentHeader header = segmentTracker_->getSegmentHeader(currentOffset);

			SegmentIndex index{};
			index.startId = header.start_id;
			index.endId = header.start_id + header.used - 1;
			index.segmentOffset = currentOffset;

			segmentIndex.push_back(index);
			currentOffset = header.next_segment_offset;
		}

		sortSegmentIndex(segmentIndex);
	}

	uint64_t SegmentIndexManager::findSegmentForId(uint32_t type, int64_t id) const {
		std::lock_guard<std::mutex> lock(mutex_);

		const auto &segmentIndex = getSegmentIndexForType(type);

		// Binary search to find the segment containing this ID
		auto it = std::lower_bound(segmentIndex.begin(), segmentIndex.end(), id,
								   [](const SegmentIndex &index, const int64_t value) { return index.endId < value; });

		if (it != segmentIndex.end() && id >= it->startId && id <= it->endId) {
			return it->segmentOffset;
		}

		return 0; // Not found
	}

	std::vector<SegmentIndexManager::SegmentIndex> &SegmentIndexManager::getSegmentIndexForType(uint32_t type) {
		switch (type) {
			case toUnderlying(SegmentType::Node):
				return nodeSegmentIndex_;
			case toUnderlying(SegmentType::Edge):
				return edgeSegmentIndex_;
			case toUnderlying(SegmentType::Property):
				return propertySegmentIndex_;
			case toUnderlying(SegmentType::Blob):
				return blobSegmentIndex_;
			case toUnderlying(SegmentType::Index):
				return indexSegmentIndex_;
			case toUnderlying(SegmentType::State):
				return stateSegmentIndex_;
			default:
				throw std::invalid_argument("Invalid segment type");
		}
	}

	const std::vector<SegmentIndexManager::SegmentIndex> &
	SegmentIndexManager::getSegmentIndexForType(uint32_t type) const {
		switch (type) {
			case toUnderlying(SegmentType::Node):
				return nodeSegmentIndex_;
			case toUnderlying(SegmentType::Edge):
				return edgeSegmentIndex_;
			case toUnderlying(SegmentType::Property):
				return propertySegmentIndex_;
			case toUnderlying(SegmentType::Blob):
				return blobSegmentIndex_;
			case toUnderlying(SegmentType::Index):
				return indexSegmentIndex_;
			case toUnderlying(SegmentType::State):
				return stateSegmentIndex_;
			default:
				throw std::invalid_argument("Invalid segment type");
		}
	}

	void SegmentIndexManager::sortSegmentIndex(std::vector<SegmentIndex> &segmentIndex) {
		std::ranges::sort(segmentIndex,
						  [](const SegmentIndex &a, const SegmentIndex &b) { return a.startId < b.startId; });
	}

	bool SegmentIndexManager::updateSegmentIndexByOffset(uint64_t offset, const SegmentHeader &header) {
		std::lock_guard<std::mutex> lock(mutex_);

		uint32_t type = header.data_type;
		auto &segmentIndex = getSegmentIndexForType(type);

		// Try to find the segment by offset
		auto it = std::ranges::find_if(segmentIndex,
									   [offset](const SegmentIndex &index) { return index.segmentOffset == offset; });

		if (it != segmentIndex.end()) {
			// Update existing entry
			it->startId = header.start_id;
			it->endId = header.start_id + (header.used > 0 ? header.used - 1 : 0);
			return true;
		}

		// Entry not found, create a new one
		SegmentIndex index{};
		index.startId = header.start_id;
		index.endId = header.start_id + (header.used > 0 ? header.used - 1 : 0);
		index.segmentOffset = offset;

		// Find insertion position
		const auto insertPos =
				std::ranges::lower_bound(segmentIndex, index, [](const SegmentIndex &a, const SegmentIndex &b) {
					return a.startId < b.startId;
				});

		// Insert at the correct position
		segmentIndex.insert(insertPos, index);
		return false;
	}

} // namespace graph::storage
