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
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/SegmentType.hpp"

namespace graph::storage {

	SegmentIndexManager::SegmentIndexManager(std::shared_ptr<SegmentTracker> segmentTracker) :
		segmentTracker_(std::move(segmentTracker)) {}

	void SegmentIndexManager::initialize(uint64_t &nodeHead, uint64_t &edgeHead, uint64_t &propertyHead,
										 uint64_t &blobHead, uint64_t &indexHead, uint64_t &stateHead) {
		std::unique_lock<std::shared_mutex> lock(mutex_); // Write lock

		nodeSegmentHead_ = &nodeHead;
		edgeSegmentHead_ = &edgeHead;
		propertySegmentHead_ = &propertyHead;
		blobSegmentHead_ = &blobHead;
		indexSegmentHead_ = &indexHead;
		stateSegmentHead_ = &stateHead;

		// Internal call doesn't need lock (we already hold it)
		// But buildSegmentIndexes is public and locks, so we refactor logic or call unsafe internal.
		// For safety/simplicity, we release lock and call public method,
		// or just duplicate the build logic here since it's init.
		// Let's call the public buildSegmentIndexes which handles locking.
		lock.unlock();
		buildSegmentIndexes();
	}

	void SegmentIndexManager::buildSegmentIndexes() {
		std::unique_lock<std::shared_mutex> lock(mutex_); // Write lock

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
			index.endId = header.start_id + (header.used > 0 ? header.used - 1 : 0);
			index.segmentOffset = currentOffset;

			segmentIndex.push_back(index);
			currentOffset = header.next_segment_offset;
		}

		sortSegmentIndex(segmentIndex);
	}

	uint64_t SegmentIndexManager::findSegmentForId(uint32_t type, int64_t id) const {
		std::shared_lock<std::shared_mutex> lock(mutex_); // Read lock

		const auto &segmentIndex = getSegmentIndexForType(type);

		auto it = std::lower_bound(segmentIndex.begin(), segmentIndex.end(), id,
								   [](const SegmentIndex &index, const int64_t value) { return index.endId < value; });

		if (it != segmentIndex.end() && id >= it->startId && id <= it->endId) {
			return it->segmentOffset;
		}

		return 0; // Not found
	}

	void SegmentIndexManager::updateSegmentIndex(const SegmentHeader &header, int64_t oldStartId) {
		std::unique_lock<std::shared_mutex> lock(mutex_); // Write lock

		auto &segmentIndex = getSegmentIndexForType(header.data_type);

		// Calculate logical end ID
		int64_t endId = header.start_id + (header.used > 0 ? header.used - 1 : 0);

		// Check if Primary Key (Start ID) has changed
		if (oldStartId != -1 && oldStartId != header.start_id) {
			// CASE 1: Key Changed (Move Operation)
			// We must remove the old entry first
			SegmentIndex oldKey{};
			oldKey.startId = oldStartId;

			auto itOld = std::lower_bound(
					segmentIndex.begin(), segmentIndex.end(), oldKey,
					[](const SegmentIndex &a, const SegmentIndex &b) { return a.startId < b.startId; });

			// Verify and Remove
			if (itOld != segmentIndex.end() && itOld->startId == oldStartId &&
				itOld->segmentOffset == header.file_offset) {
				segmentIndex.erase(itOld);
			} else {
				// Fallback: If binary search failed (shouldn't happen), try linear scan by offset to clean up
				// This handles edge cases where index might be dirty
				std::erase_if(segmentIndex,
							  [&](const SegmentIndex &idx) { return idx.segmentOffset == header.file_offset; });
			}

			// Now insert the new entry (fall through to insert logic below)
			// Reset 'it' since we modified the vector
		} else {
			// CASE 2: Key Same (Update Operation)
			SegmentIndex searchKey{};
			searchKey.startId = header.start_id;

			auto it = std::lower_bound(
					segmentIndex.begin(), segmentIndex.end(), searchKey,
					[](const SegmentIndex &a, const SegmentIndex &b) { return a.startId < b.startId; });

			if (it != segmentIndex.end() && it->startId == header.start_id) {
				if (it->segmentOffset == header.file_offset) {
					it->endId = endId;
					return; // Done
				}
			}
		}

		// Insert New Entry (for Case 1 or if not found in Case 2)
		SegmentIndex newIndex{};
		newIndex.startId = header.start_id;
		newIndex.endId = endId;
		newIndex.segmentOffset = header.file_offset;

		auto itNew =
				std::lower_bound(segmentIndex.begin(), segmentIndex.end(), newIndex,
								 [](const SegmentIndex &a, const SegmentIndex &b) { return a.startId < b.startId; });

		segmentIndex.insert(itNew, newIndex);
	}

	void SegmentIndexManager::removeSegmentIndex(const SegmentHeader &header) {
		std::unique_lock<std::shared_mutex> lock(mutex_);

		auto &segmentIndex = getSegmentIndexForType(header.data_type);

		SegmentIndex searchKey{};
		searchKey.startId = header.start_id;

		auto it = std::lower_bound(segmentIndex.begin(), segmentIndex.end(), searchKey,
								   [](const SegmentIndex &a, const SegmentIndex &b) { return a.startId < b.startId; });

		if (it != segmentIndex.end() && it->startId == header.start_id && it->segmentOffset == header.file_offset) {
			segmentIndex.erase(it);
		}
	}

	// ... (getSegmentIndexForType implementations remain the same) ...
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
		std::sort(segmentIndex.begin(), segmentIndex.end(),
				  [](const SegmentIndex &a, const SegmentIndex &b) { return a.startId < b.startId; });
	}

} // namespace graph::storage
