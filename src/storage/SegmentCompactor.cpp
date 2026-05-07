/**
 * @file SegmentCompactor.cpp
 * @brief Segment compaction, merging, relocation, and max-ID recalculation
 *
 * @copyright Copyright (c) 2025 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/SegmentCompactor.hpp"
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include "graph/storage/SegmentType.hpp"

namespace graph::storage {

	SegmentCompactor::SegmentCompactor(std::shared_ptr<StorageIO> io, std::shared_ptr<SegmentTracker> tracker,
									   std::shared_ptr<SegmentAllocator> allocator,
									   std::shared_ptr<FileHeaderManager> fileHeaderManager) :
		io_(std::move(io)), tracker_(std::move(tracker)),
		allocator_(std::move(allocator)), fileHeaderManager_(std::move(fileHeaderManager)) {}

	SegmentCompactor::~SegmentCompactor() = default;

	// ── Compaction ──────────────────────────────────────────────────────────────

	void SegmentCompactor::compactSegments(uint32_t type, double threshold) {
		auto segments = tracker_->getSegmentsNeedingCompaction(type, threshold);

		if (segments.size() > 5) {
			std::ranges::sort(segments, [](const SegmentHeader &a, const SegmentHeader &b) {
				return a.getFragmentationRatio() > b.getFragmentationRatio();
			});
			segments.resize(5);
		}

		for (const auto &segment : segments) {
			dispatchByType(type, [&]<EntityType ET>() {
				compactSegmentImpl<ET>(segment.file_offset);
			});
		}
	}

	template<EntityType ET>
	bool SegmentCompactor::compactSegmentImpl(uint64_t offset) {
		using Entity = typename EntitySizeTraits<ET>::Type;
		constexpr size_t entitySize = EntitySizeTraits<ET>::entitySize;
		constexpr double COMPACTION_THRESHOLD = 0.3;

		SegmentHeader header = tracker_->getSegmentHeader(offset);

		if (header.used == header.inactive_count) {
			allocator_->deallocateSegment(offset);
			return true;
		}

		double fragmentationRatio = header.getFragmentationRatio();
		if (fragmentationRatio <= COMPACTION_THRESHOLD) {
			return true;
		}

		uint32_t nextFreeSpot = 0;
		uint8_t newBitmap[MAX_BITMAP_SIZE] = {};

		for (uint32_t i = 0; i < header.used; i++) {
			if (bitmap::getBit(header.activity_bitmap, i)) {
				int64_t oldId = header.start_id + i;
				int64_t newId = header.start_id + nextFreeSpot;

				auto entity = tracker_->readEntity<Entity>(offset, i, entitySize);
				entity.setId(newId);
				tracker_->writeEntity<Entity>(offset, nextFreeSpot, entity, entitySize);

				bitmap::setBit(newBitmap, nextFreeSpot, true);
				nextFreeSpot++;

				if (entityReferenceUpdater_) {
					entityReferenceUpdater_->updateEntityReferences(oldId, &entity, toUnderlying(ET));
				}
			}
		}

		header.used = nextFreeSpot;
		header.inactive_count = 0;
		header.bitmap_size = bitmap::calculateBitmapSize(nextFreeSpot);
		std::memcpy(header.activity_bitmap, newBitmap, header.bitmap_size);

		tracker_->writeSegmentHeader(offset, header);
		tracker_->updateSegmentUsage(offset, nextFreeSpot, 0);

		return true;
	}

	// Explicit template instantiations
	template bool SegmentCompactor::compactSegmentImpl<EntityType::Node>(uint64_t);
	template bool SegmentCompactor::compactSegmentImpl<EntityType::Edge>(uint64_t);
	template bool SegmentCompactor::compactSegmentImpl<EntityType::Property>(uint64_t);
	template bool SegmentCompactor::compactSegmentImpl<EntityType::Blob>(uint64_t);
	template bool SegmentCompactor::compactSegmentImpl<EntityType::Index>(uint64_t);
	template bool SegmentCompactor::compactSegmentImpl<EntityType::State>(uint64_t);

	// ── Merging ─────────────────────────────────────────────────────────────────

	std::vector<uint64_t> SegmentCompactor::findCandidatesForMerge(uint32_t type, double usageThreshold) const {
		std::vector<uint64_t> candidates;
		auto segments = tracker_->getSegmentsByType(type);

		for (const auto &header : segments) {
			double usageRatio = static_cast<double>(header.getActiveCount()) / header.capacity;
			if (usageRatio < usageThreshold) {
				candidates.push_back(header.file_offset);
			}
		}

		return candidates;
	}

	template<EntityType ET>
	void SegmentCompactor::processEntityImpl(uint64_t sourceOffset, uint64_t targetOffset, uint32_t i,
											 uint32_t &targetNextIndex, uint8_t *newBitmap,
											 const SegmentHeader &sourceHeader, const SegmentHeader &targetHeader) {
		using Entity = typename EntitySizeTraits<ET>::Type;
		constexpr size_t itemSize = EntitySizeTraits<ET>::entitySize;

		int64_t oldId = sourceHeader.start_id + i;
		int64_t newId = targetHeader.start_id + targetNextIndex;

		auto entity = tracker_->readEntity<Entity>(sourceOffset, i, itemSize);
		entity.setId(newId);
		tracker_->writeEntity(targetOffset, targetNextIndex, entity, itemSize);

		if (entityReferenceUpdater_) {
			entityReferenceUpdater_->updateEntityReferences(oldId, &entity, toUnderlying(ET));
		}

		bitmap::setBit(newBitmap, targetNextIndex, true);
		targetNextIndex++;
	}

	// Explicit template instantiations
	template void SegmentCompactor::processEntityImpl<EntityType::Node>(uint64_t, uint64_t, uint32_t, uint32_t &, uint8_t *, const SegmentHeader &, const SegmentHeader &);
	template void SegmentCompactor::processEntityImpl<EntityType::Edge>(uint64_t, uint64_t, uint32_t, uint32_t &, uint8_t *, const SegmentHeader &, const SegmentHeader &);
	template void SegmentCompactor::processEntityImpl<EntityType::Property>(uint64_t, uint64_t, uint32_t, uint32_t &, uint8_t *, const SegmentHeader &, const SegmentHeader &);
	template void SegmentCompactor::processEntityImpl<EntityType::Blob>(uint64_t, uint64_t, uint32_t, uint32_t &, uint8_t *, const SegmentHeader &, const SegmentHeader &);
	template void SegmentCompactor::processEntityImpl<EntityType::Index>(uint64_t, uint64_t, uint32_t, uint32_t &, uint8_t *, const SegmentHeader &, const SegmentHeader &);
	template void SegmentCompactor::processEntityImpl<EntityType::State>(uint64_t, uint64_t, uint32_t, uint32_t &, uint8_t *, const SegmentHeader &, const SegmentHeader &);

	bool SegmentCompactor::mergeIntoSegment(uint64_t targetOffset, uint64_t sourceOffset, uint32_t type) {
		SegmentHeader targetHeader = tracker_->getSegmentHeader(targetOffset);
		SegmentHeader sourceHeader = tracker_->getSegmentHeader(sourceOffset);

		if (targetHeader.data_type != type || sourceHeader.data_type != type) {
			return false;
		}

		uint32_t targetActiveItems = targetHeader.getActiveCount();
		uint32_t sourceActiveItems = sourceHeader.getActiveCount();

		if (targetActiveItems + sourceActiveItems > targetHeader.capacity) {
			return false;
		}

		uint32_t bitmapSize = bitmap::calculateBitmapSize(targetHeader.capacity);
		std::vector<uint8_t> newBitmap(bitmapSize, 0);
		std::memcpy(newBitmap.data(), targetHeader.activity_bitmap, std::min(bitmapSize, targetHeader.bitmap_size));

		uint32_t targetNextIndex = targetHeader.used;

		for (uint32_t i = 0; i < sourceHeader.used; i++) {
			if (bitmap::getBit(sourceHeader.activity_bitmap, i)) {
				dispatchByType(type, [&]<EntityType ET>() {
					processEntityImpl<ET>(sourceOffset, targetOffset, i, targetNextIndex, newBitmap.data(),
										  sourceHeader, targetHeader);
				});
			}
		}

		targetHeader.used = targetNextIndex;
		targetHeader.bitmap_size = bitmapSize;
		std::memcpy(targetHeader.activity_bitmap, newBitmap.data(), bitmapSize);
		tracker_->writeSegmentHeader(targetOffset, targetHeader);

		if (sourceHeader.prev_segment_offset != 0) {
			SegmentHeader prevHeader = tracker_->getSegmentHeader(sourceHeader.prev_segment_offset);
			tracker_->updateSegmentLinks(sourceHeader.prev_segment_offset, prevHeader.prev_segment_offset,
										 sourceHeader.next_segment_offset);
		} else {
			tracker_->updateChainHead(type, sourceHeader.next_segment_offset);
			allocator_->updateFileHeaderChainHeads();
		}

		if (sourceHeader.next_segment_offset != 0) {
			SegmentHeader nextHeader = tracker_->getSegmentHeader(sourceHeader.next_segment_offset);
			tracker_->updateSegmentLinks(sourceHeader.next_segment_offset, sourceHeader.prev_segment_offset,
										 nextHeader.next_segment_offset);
		}

		tracker_->markSegmentFree(sourceOffset);

		return true;
	}

	bool SegmentCompactor::mergeSegments(uint32_t type, double usageThreshold) {
		auto candidates = findCandidatesForMerge(type, usageThreshold);

		if (candidates.size() < 2) {
			return false;
		}

		uint64_t fileSize = calculateCurrentFileSize();
		uint64_t endThreshold = fileSize - (fileSize / 5);
		endThreshold = (endThreshold / TOTAL_SEGMENT_SIZE) * TOTAL_SEGMENT_SIZE;

		std::vector<uint64_t> endSegments;
		std::vector<uint64_t> frontSegments;

		for (uint64_t offset : candidates) {
			if (offset >= endThreshold) {
				endSegments.push_back(offset);
			} else {
				frontSegments.push_back(offset);
			}
		}

		std::ranges::sort(endSegments, [this](uint64_t a, uint64_t b) {
			SegmentHeader headerA = tracker_->getSegmentHeader(a);
			SegmentHeader headerB = tracker_->getSegmentHeader(b);
			double usageA = static_cast<double>(headerA.getActiveCount()) / headerA.capacity;
			double usageB = static_cast<double>(headerB.getActiveCount()) / headerB.capacity;
			return usageA < usageB;
		});

		std::ranges::sort(frontSegments);

		bool mergedAny = false;
		std::unordered_set<uint64_t> mergedSegments;

		// PHASE 1: Move data from end segments to front segments
		for (uint64_t sourceOffset : endSegments) {
			if (mergedSegments.contains(sourceOffset)) continue;

			SegmentHeader sourceHeader = tracker_->getSegmentHeader(sourceOffset);
			uint32_t activeItemsSource = sourceHeader.getActiveCount();

			if (activeItemsSource == 0) {
				tracker_->markSegmentFree(sourceOffset);
				mergedSegments.insert(sourceOffset);
				mergedAny = true;
				continue;
			}

			bool merged = false;
			for (uint64_t targetOffset : frontSegments) {
				if (mergedSegments.contains(targetOffset)) continue;

				if (mergeIntoSegment(targetOffset, sourceOffset, type)) {
					mergedAny = true;
					mergedSegments.insert(sourceOffset);
					merged = true;
					break;
				}
			}

			if (!merged) {
				std::vector<uint64_t> remainingEndSegments;
				for (uint64_t offset : endSegments) {
					if (offset != sourceOffset && !mergedSegments.contains(offset)) {
						remainingEndSegments.push_back(offset);
					}
				}
				std::ranges::sort(remainingEndSegments);

				for (uint64_t targetOffset : remainingEndSegments) {
					if (targetOffset >= sourceOffset) continue;

					if (mergeIntoSegment(targetOffset, sourceOffset, type)) {
						mergedAny = true;
						mergedSegments.insert(sourceOffset);
						break;
					}
				}
			}
		}

		// PHASE 2: Consolidate remaining front segments
		std::vector<uint64_t> remainingFrontSegments;
		for (uint64_t offset : frontSegments) {
			if (!mergedSegments.contains(offset)) {
				remainingFrontSegments.push_back(offset);
			}
		}

		std::ranges::sort(remainingFrontSegments, [this](uint64_t a, uint64_t b) {
			const SegmentHeader headerA = tracker_->getSegmentHeader(a);
			const SegmentHeader headerB = tracker_->getSegmentHeader(b);
			double usageA = static_cast<double>(headerA.getActiveCount()) / headerA.capacity;
			double usageB = static_cast<double>(headerB.getActiveCount()) / headerB.capacity;
			return usageA < usageB;
		});

		for (size_t i = 0; i < remainingFrontSegments.size(); i++) {
			uint64_t sourceOffset = remainingFrontSegments[i];

			if (mergedSegments.contains(sourceOffset)) continue;

			SegmentHeader sourceHeader = tracker_->getSegmentHeader(sourceOffset);
			uint32_t activeItemsSource = sourceHeader.getActiveCount();

			if (activeItemsSource == 0) {
				tracker_->markSegmentFree(sourceOffset);
				mergedSegments.insert(sourceOffset);
				mergedAny = true;
				continue;
			}

			for (size_t j = 0; j < remainingFrontSegments.size(); j++) {
				if (i == j) continue;

				uint64_t targetOffset = remainingFrontSegments[j];
				if (mergedSegments.contains(targetOffset)) continue;

				if (targetOffset > sourceOffset) continue;

				if (mergeIntoSegment(targetOffset, sourceOffset, type)) {
					mergedAny = true;
					mergedSegments.insert(sourceOffset);
					break;
				}
			}
		}

		return mergedAny;
	}

	// ── Empty segment processing ────────────────────────────────────────────────

	bool SegmentCompactor::processEmptySegments(uint32_t type) const {
		bool deallocatedAny = false;
		auto segments = tracker_->getSegmentsByType(type);

		std::vector<uint64_t> emptySegments;
		for (const auto &header : segments) {
			if (header.getActiveCount() == 0) {
				emptySegments.push_back(header.file_offset);
			}
		}

		for (uint64_t segmentOffset : emptySegments) {
			allocator_->deallocateSegment(segmentOffset);
			deallocatedAny = true;
		}

		return deallocatedAny;
	}

	void SegmentCompactor::processAllEmptySegments() const {
		for (uint32_t type = 0; type <= getMaxEntityType(); type++) {
			processEmptySegments(type);
		}
	}

	// ── Move & Relocate ─────────────────────────────────────────────────────────

	bool SegmentCompactor::moveSegment(uint64_t sourceOffset, uint64_t destinationOffset) const {
		if (sourceOffset == destinationOffset) {
			return true;
		}

		SegmentHeader sourceHeader = tracker_->getSegmentHeader(sourceOffset);
		tracker_->removeFromFreeList(destinationOffset);

		SegmentHeader newHeader = sourceHeader;
		newHeader.file_offset = destinationOffset;

		if (!copySegmentData(sourceOffset, destinationOffset)) {
			return false;
		}

		tracker_->writeSegmentHeader(destinationOffset, newHeader);
		updateSegmentChain(destinationOffset, sourceHeader);
		tracker_->markSegmentFree(sourceOffset);

		return true;
	}

	bool SegmentCompactor::copySegmentData(uint64_t sourceOffset, uint64_t destinationOffset) const {
		constexpr size_t COPY_BLOCK_SIZE = 64 * 1024;
		std::vector<char> buffer(COPY_BLOCK_SIZE);

		for (size_t offset = 0; offset < TOTAL_SEGMENT_SIZE; offset += COPY_BLOCK_SIZE) {
			size_t copySize = std::min(COPY_BLOCK_SIZE, static_cast<size_t>(TOTAL_SEGMENT_SIZE) - offset);

			size_t bytesRead = io_->readAt(sourceOffset + offset, buffer.data(), copySize);
			if (bytesRead != copySize) {
				return false;
			}

			io_->writeAt(destinationOffset + offset, buffer.data(), copySize);
		}

		return true;
	}

	void SegmentCompactor::updateSegmentChain(uint64_t newOffset, const SegmentHeader &header) const {
		bool isChainHead = false;
		uint64_t currentChainHead = tracker_->getChainHead(header.data_type);

		if (currentChainHead == header.file_offset) {
			tracker_->updateChainHead(header.data_type, newOffset);
			isChainHead = true;
		}

		if (header.prev_segment_offset != 0) {
			SegmentHeader prevHeader = tracker_->getSegmentHeader(header.prev_segment_offset);
			tracker_->updateSegmentLinks(prevHeader.file_offset, prevHeader.prev_segment_offset, newOffset);
		}

		if (header.next_segment_offset != 0) {
			SegmentHeader nextHeader = tracker_->getSegmentHeader(header.next_segment_offset);
			tracker_->updateSegmentLinks(nextHeader.file_offset, newOffset, nextHeader.next_segment_offset);
		}

		if (isChainHead) {
			allocator_->updateFileHeaderChainHeads();
		}
	}

	bool SegmentCompactor::relocateSegmentsFromEnd() const {
		auto freeSegments = tracker_->getFreeSegments();
		if (freeSegments.empty()) {
			return false;
		}

		std::ranges::sort(freeSegments);

		auto relocatableSegments = findRelocatableSegments();
		if (relocatableSegments.empty()) {
			return false;
		}

		std::ranges::sort(relocatableSegments, std::greater());

		bool anyMoved = false;

		for (uint64_t sourceOffset : relocatableSegments) {
			if (freeSegments.empty()) break;

			uint64_t targetOffset = freeSegments.front();
			freeSegments.erase(freeSegments.begin());

			if (sourceOffset <= targetOffset) continue;

			if (moveSegment(sourceOffset, targetOffset)) {
				anyMoved = true;
			}
		}

		return anyMoved;
	}

	// ── Utility ─────────────────────────────────────────────────────────────────

	bool SegmentCompactor::isSegmentAtEndOfFile(uint64_t offset) const {
		uint64_t fileSize = calculateCurrentFileSize();
		uint64_t segmentEnd = offset + TOTAL_SEGMENT_SIZE;
		return (fileSize - segmentEnd < TOTAL_SEGMENT_SIZE);
	}

	uint64_t SegmentCompactor::findFreeSegmentNotAtEnd() const {
		auto freeSegments = tracker_->getFreeSegments();
		if (freeSegments.empty()) {
			return 0;
		}

		uint64_t fileSize = calculateCurrentFileSize();
		uint64_t endThreshold = fileSize - TOTAL_SEGMENT_SIZE;

		std::ranges::sort(freeSegments);

		for (uint64_t offset : freeSegments) {
			if (offset < endThreshold) {
				return offset;
			}
		}

		return 0;
	}

	std::vector<uint64_t> SegmentCompactor::findRelocatableSegments() const {
		std::vector<uint64_t> result;
		uint64_t fileSize = calculateCurrentFileSize();

		uint64_t thresholdOffset = fileSize - (fileSize / 5);
		thresholdOffset = (thresholdOffset / TOTAL_SEGMENT_SIZE) * TOTAL_SEGMENT_SIZE;

		for (uint32_t type = 0; type <= getMaxEntityType(); type++) {
			auto segments = tracker_->getSegmentsByType(type);
			for (const auto &header : segments) {
				if (header.file_offset >= thresholdOffset) {
					result.push_back(header.file_offset);
				}
			}
		}
		return result;
	}

	uint64_t SegmentCompactor::calculateCurrentFileSize() const {
		uint64_t maxOffset = FILE_HEADER_SIZE;

		for (uint32_t type = 0; type <= getMaxEntityType(); type++) {
			auto segments = tracker_->getSegmentsByType(type);
			for (const auto &header : segments) {
				uint64_t endOffset = header.file_offset + TOTAL_SEGMENT_SIZE;
				maxOffset = std::max(maxOffset, endOffset);
			}
		}

		auto freeSegments = tracker_->getFreeSegments();
		for (uint64_t offset : freeSegments) {
			uint64_t endOffset = offset + TOTAL_SEGMENT_SIZE;
			maxOffset = std::max(maxOffset, endOffset);
		}

		return maxOffset;
	}

	// ── Max ID recalculation ────────────────────────────────────────────────────

	void SegmentCompactor::recalculateMaxIds() const {
		int64_t &maxNodeId = fileHeaderManager_->getMaxNodeIdRef();
		int64_t &maxEdgeId = fileHeaderManager_->getMaxEdgeIdRef();
		int64_t &maxPropId = fileHeaderManager_->getMaxPropIdRef();
		int64_t &maxBlobId = fileHeaderManager_->getMaxBlobIdRef();
		int64_t &maxIndexId = fileHeaderManager_->getMaxIndexIdRef();
		int64_t &maxStateId = fileHeaderManager_->getMaxStateIdRef();

		maxNodeId = 0;
		maxEdgeId = 0;
		maxPropId = 0;
		maxBlobId = 0;
		maxIndexId = 0;
		maxStateId = 0;

		for (uint32_t type = 0; type <= getMaxEntityType(); type++) {
			auto segments = tracker_->getSegmentsByType(type);

			for (const auto &header : segments) {
				if (header.getActiveCount() > 0) {
					int64_t lastUsedId = calculateLastUsedIdInSegment(header);

					switch (type) {
						case toUnderlying(SegmentType::Node):
							maxNodeId = std::max(maxNodeId, lastUsedId);
							break;
						case toUnderlying(SegmentType::Edge):
							maxEdgeId = std::max(maxEdgeId, lastUsedId);
							break;
						case toUnderlying(SegmentType::Property):
							maxPropId = std::max(maxPropId, lastUsedId);
							break;
						case toUnderlying(SegmentType::Blob):
							maxBlobId = std::max(maxBlobId, lastUsedId);
							break;
						case toUnderlying(SegmentType::Index):
							maxIndexId = std::max(maxIndexId, lastUsedId);
							break;
						case toUnderlying(SegmentType::State):
							maxStateId = std::max(maxStateId, lastUsedId);
							break;
					}
				}
			}
		}
	}

	int64_t SegmentCompactor::calculateLastUsedIdInSegment(const SegmentHeader &header) const {
		int64_t maxId = header.start_id - 1;
		auto segmentOffset = header.file_offset;

		for (int64_t i = static_cast<int64_t>(header.used) - 1; i >= 0; i--) {
			if (tracker_->isEntityActive(segmentOffset, i)) {
				maxId = header.start_id + i;
				break;
			}
		}

		return maxId;
	}

} // namespace graph::storage
