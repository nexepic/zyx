/**
 * @file SegmentAllocator.cpp
 * @brief Segment allocation, deallocation, and chain management
 *
 * @copyright Copyright (c) 2025 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/SegmentAllocator.hpp"
#include <cstring>
#include <stdexcept>

namespace graph::storage {

	SegmentAllocator::SegmentAllocator(std::shared_ptr<std::fstream> file, std::shared_ptr<SegmentTracker> tracker,
									   std::shared_ptr<FileHeaderManager> fileHeaderManager,
									   std::shared_ptr<IDAllocator> idAllocator) :
		file_(std::move(file)), segmentTracker_(std::move(tracker)),
		fileHeaderManager_(std::move(fileHeaderManager)), idAllocator_(std::move(idAllocator)) {}

	SegmentAllocator::~SegmentAllocator() = default;

	uint64_t SegmentAllocator::findMaxId(uint32_t type, const std::shared_ptr<SegmentTracker> &tracker) {
		uint64_t maxId = 0;
		uint64_t current = tracker->getChainHead(type);
		while (current != 0) {
			SegmentHeader header = tracker->getSegmentHeader(current);
			maxId = std::max(maxId, static_cast<uint64_t>(header.start_id + header.capacity));
			current = header.next_segment_offset;
		}
		return maxId;
	}

	uint64_t SegmentAllocator::allocateSegment(uint32_t type, uint32_t capacity) const {
		size_t segmentSize = TOTAL_SEGMENT_SIZE;

		auto freeSegments = segmentTracker_->getFreeSegments();
		uint64_t offset = 0;

		if (!freeSegments.empty()) {
			offset = freeSegments.front();
			segmentTracker_->removeFromFreeList(offset);
		} else {
			file_->seekp(0, std::ios::end);
			offset = file_->tellp();

			std::vector<char> zeros(segmentSize, 0);
			file_->write(zeros.data(), static_cast<std::streamsize>(segmentSize));
		}

		SegmentHeader header;
		header.file_offset = offset;
		header.data_type = type;
		header.capacity = capacity;
		header.used = 0;
		header.inactive_count = 0;
		header.next_segment_offset = 0;
		header.prev_segment_offset = 0;

		if (type <= getMaxEntityType()) {
			header.start_id = static_cast<int64_t>(findMaxId(type, segmentTracker_)) + 1;
		} else {
			throw std::invalid_argument("Invalid segment type");
		}

		header.needs_compaction = 0;
		header.is_dirty = 0;
		header.bitmap_size = bitmap::calculateBitmapSize(capacity);
		std::memset(header.activity_bitmap, 0, sizeof(header.activity_bitmap));

		file_->seekp(static_cast<std::streamoff>(offset));
		file_->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));

		segmentTracker_->registerSegment(header);

		uint64_t chainHead = segmentTracker_->getChainHead(type);
		if (chainHead == 0) {
			segmentTracker_->updateChainHead(type, offset);
			segmentTracker_->updateChainTail(type, offset);
		} else {
			// O(1) tail access — append directly to the chain tail
			uint64_t tail = segmentTracker_->getChainTail(type);
			if (tail != 0) {
				SegmentHeader tailHeader = segmentTracker_->getSegmentHeader(tail);
				segmentTracker_->updateSegmentLinks(offset, tail, 0);
				segmentTracker_->updateSegmentLinks(tail, tailHeader.prev_segment_offset, offset);
			} else {
				// Fallback: tail not cached (should not happen after init)
				uint64_t current = chainHead;
				uint64_t prev = 0;
				while (current != 0) {
					const SegmentHeader currentHeader = segmentTracker_->getSegmentHeader(current);
					prev = current;
					current = currentHeader.next_segment_offset;
				}
				segmentTracker_->updateSegmentLinks(offset, prev, 0);
				SegmentHeader prevHeader = segmentTracker_->getSegmentHeader(prev);
				segmentTracker_->updateSegmentLinks(prev, prevHeader.prev_segment_offset, offset);
			}
			segmentTracker_->updateChainTail(type, offset);
		}

		return offset;
	}

	void SegmentAllocator::deallocateSegment(uint64_t offset) const {
		SegmentHeader header = segmentTracker_->getSegmentHeader(offset);

		bool isChainHead = (segmentTracker_->getChainHead(header.data_type) == offset);

		uint64_t prevOffset = header.prev_segment_offset;
		uint64_t nextOffset = header.next_segment_offset;

		if (prevOffset != 0) {
			SegmentHeader prevHeader = segmentTracker_->getSegmentHeader(prevOffset);
			segmentTracker_->updateSegmentLinks(prevOffset, prevHeader.prev_segment_offset, nextOffset);
		} else {
			segmentTracker_->updateChainHead(header.data_type, nextOffset);
		}

		if (nextOffset != 0) {
			SegmentHeader nextHeader = segmentTracker_->getSegmentHeader(nextOffset);
			segmentTracker_->updateSegmentLinks(nextOffset, prevOffset, nextHeader.next_segment_offset);
		}

		if (isChainHead) {
			updateFileHeaderChainHeads();
		}

		segmentTracker_->markSegmentFree(offset);
	}

	void SegmentAllocator::updateFileHeaderChainHeads() const {
		FileHeader &header = fileHeaderManager_->getFileHeader();
		header.node_segment_head = segmentTracker_->getChainHead(toUnderlying(SegmentType::Node));
		header.edge_segment_head = segmentTracker_->getChainHead(toUnderlying(SegmentType::Edge));
		header.property_segment_head = segmentTracker_->getChainHead(toUnderlying(SegmentType::Property));
		header.blob_segment_head = segmentTracker_->getChainHead(toUnderlying(SegmentType::Blob));
		header.index_segment_head = segmentTracker_->getChainHead(toUnderlying(SegmentType::Index));
		header.state_segment_head = segmentTracker_->getChainHead(toUnderlying(SegmentType::State));
	}

} // namespace graph::storage
