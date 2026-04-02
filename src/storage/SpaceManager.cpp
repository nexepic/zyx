/**
 * @file SpaceManager.cpp
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

#include "graph/storage/SpaceManager.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unordered_set>
#include <utility>
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/SegmentType.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::storage {

	SpaceManager::SpaceManager(std::shared_ptr<std::fstream> file, std::string fileName,
							   std::shared_ptr<SegmentTracker> tracker,
							   std::shared_ptr<FileHeaderManager> fileHeaderManager,
							   std::shared_ptr<IDAllocator> idAllocator) :
		file_(std::move(file)), fileName_(std::move(fileName)), segmentTracker_(std::move(tracker)),
		fileHeaderManager_(std::move(fileHeaderManager)), idAllocator_(std::move(idAllocator)) {}

	SpaceManager::~SpaceManager() = default;

	// TODO: optimize with IDAllocator
	uint64_t SpaceManager::findMaxId(uint32_t type, const std::shared_ptr<SegmentTracker> &tracker) {
		uint64_t maxId = 0;
		uint64_t head = tracker->getChainHead(type);
		uint64_t current = head;
		while (current != 0) {
			SegmentHeader header = tracker->getSegmentHeader(current);
			maxId = std::max(maxId, static_cast<uint64_t>(header.start_id + header.capacity));
			current = header.next_segment_offset;
		}
		return maxId;
	}

	uint64_t SpaceManager::allocateSegment(uint32_t type, uint32_t capacity) const {
		// Calculate segment size based on type
		size_t segmentSize = TOTAL_SEGMENT_SIZE; // Use a consistent segment size for all types

		// Check if we have free segments that can be reused
		auto freeSegments = segmentTracker_->getFreeSegments();
		uint64_t offset = 0;

		if (!freeSegments.empty()) {
			offset = freeSegments.front();
			// Remove from free list
			segmentTracker_->removeFromFreeList(offset); // This removes it from the free list
		} else {
			// Allocate at end of file
			file_->seekp(0, std::ios::end);
			offset = file_->tellp();

			// Initialize segment space with zeros
			std::vector<char> zeros(segmentSize, 0);
			file_->write(zeros.data(), static_cast<std::streamsize>(segmentSize));
		}

		// Initialize segment header
		SegmentHeader header;
		header.file_offset = offset;
		header.data_type = type;
		header.capacity = capacity;
		header.used = 0;
		header.inactive_count = 0;
		header.next_segment_offset = 0;
		header.prev_segment_offset = 0;

		// Start ID calculation for nodes, edges, properties, and blobs...
		if (type <= getMaxEntityType()) {
			header.start_id = static_cast<int64_t>(findMaxId(type, segmentTracker_)) + 1;
		} else {
			throw std::invalid_argument("Invalid segment type");
		}

		header.needs_compaction = 0;
		header.is_dirty = 0;
		header.bitmap_size = bitmap::calculateBitmapSize(capacity);
		std::memset(header.activity_bitmap, 0, sizeof(header.activity_bitmap));

		// Write header
		file_->seekp(static_cast<std::streamoff>(offset));
		file_->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));

		// Register with tracker
		segmentTracker_->registerSegment(header);

		// Link with chain
		uint64_t chainHead = segmentTracker_->getChainHead(type);
		if (chainHead == 0) {
			// This is the first segment of this type
			segmentTracker_->updateChainHead(type, offset);
		} else {
			// Find the last segment in chain
			uint64_t current = chainHead;
			uint64_t prev = 0;
			while (current != 0) {
				const SegmentHeader currentHeader = segmentTracker_->getSegmentHeader(current);
				prev = current;
				current = currentHeader.next_segment_offset;
			}

			// Link the new segment to the end of chain
			segmentTracker_->updateSegmentLinks(offset, prev, 0);
			SegmentHeader prevHeader = segmentTracker_->getSegmentHeader(prev);
			segmentTracker_->updateSegmentLinks(prev, prevHeader.prev_segment_offset, offset);
		}

		return offset;
	}

	void SpaceManager::deallocateSegment(uint64_t offset) const {
		// Get segment info before it's removed
		SegmentHeader header = segmentTracker_->getSegmentHeader(offset);

		// Check if this is a chain head
		bool isChainHead = (segmentTracker_->getChainHead(header.data_type) == offset);

		// Update chain links
		uint64_t prevOffset = header.prev_segment_offset;
		uint64_t nextOffset = header.next_segment_offset;

		if (prevOffset != 0) {
			SegmentHeader prevHeader = segmentTracker_->getSegmentHeader(prevOffset);
			segmentTracker_->updateSegmentLinks(prevOffset, prevHeader.prev_segment_offset, nextOffset);
		} else {
			// This is the head of the chain
			segmentTracker_->updateChainHead(header.data_type, nextOffset);
		}

		if (nextOffset != 0) {
			SegmentHeader nextHeader = segmentTracker_->getSegmentHeader(nextOffset);
			segmentTracker_->updateSegmentLinks(nextOffset, prevOffset, nextHeader.next_segment_offset);
		}

		// If this was a chain head, update the file header
		if (isChainHead) {
			updateFileHeaderChainHeads();
		}

		// Mark segment as free
		segmentTracker_->markSegmentFree(offset);
	}

	void SpaceManager::compactSegments(uint32_t type, double threshold) {
		// Get segments that need compaction based on total free space, not just inactive elements
		auto segments = segmentTracker_->getSegmentsNeedingCompaction(type, threshold);

		// Limit to top 5 segments with highest fragmentation
		if (segments.size() > 5) {
			std::ranges::sort(segments, [](const SegmentHeader &a, const SegmentHeader &b) {
				return a.getFragmentationRatio() > b.getFragmentationRatio();
			});
			segments.resize(5);
		}

		for (const auto &segment: segments) {
			switch (type) {
				case toUnderlying(SegmentType::Node):
					compactNodeSegment(segment.file_offset);
					break;
				case toUnderlying(SegmentType::Edge):
					compactEdgeSegment(segment.file_offset);
					break;
				case toUnderlying(SegmentType::Property):
					compactPropertySegment(segment.file_offset);
					break;
				case toUnderlying(SegmentType::Blob):
					compactBlobSegment(segment.file_offset);
					break;
				case toUnderlying(SegmentType::Index):
					compactIndexSegment(segment.file_offset);
					break;
				case toUnderlying(SegmentType::State):
					compactStateSegment(segment.file_offset);
					break;
			}
		}
	}

	bool SpaceManager::moveSegment(uint64_t sourceOffset, uint64_t destinationOffset) const {
		if (sourceOffset == destinationOffset) {
			return true; // Nothing to do
		}

		// Get source segment info from tracker
		SegmentHeader sourceHeader = segmentTracker_->getSegmentHeader(sourceOffset);

		// Remove destination from free list if applicable
		segmentTracker_->removeFromFreeList(destinationOffset);

		// Create a clean copy of the source header for the destination
		// Preserving all original metadata including start_id
		SegmentHeader newHeader = sourceHeader;
		newHeader.file_offset = destinationOffset;

		// Copy the segment data (content only, not header)
		if (!copySegmentData(sourceOffset, destinationOffset)) {
			return false;
		}

		// Write the copied header at the destination
		segmentTracker_->writeSegmentHeader(destinationOffset, newHeader);

		// Update chain links
		updateSegmentChain(destinationOffset, sourceHeader);

		// Mark old segment as free
		segmentTracker_->markSegmentFree(sourceOffset);

		return true;
	}

	double SpaceManager::getTotalFragmentationRatio() const {
		double nodeRatio = segmentTracker_->calculateFragmentationRatio(toUnderlying(SegmentType::Node));
		double edgeRatio = segmentTracker_->calculateFragmentationRatio(toUnderlying(SegmentType::Edge));
		double propertyRatio = segmentTracker_->calculateFragmentationRatio(toUnderlying(SegmentType::Property));
		double blobRatio = segmentTracker_->calculateFragmentationRatio(toUnderlying(SegmentType::Blob));
		double indexRatio = segmentTracker_->calculateFragmentationRatio(toUnderlying(SegmentType::Index));
		double stateRatio = segmentTracker_->calculateFragmentationRatio(toUnderlying(SegmentType::State));

		// Weight by segment counts
		auto nodeSegments = segmentTracker_->getSegmentsByType(toUnderlying(SegmentType::Node));
		auto edgeSegments = segmentTracker_->getSegmentsByType(toUnderlying(SegmentType::Edge));
		auto propSegments = segmentTracker_->getSegmentsByType(toUnderlying(SegmentType::Property));
		auto blobSegments = segmentTracker_->getSegmentsByType(toUnderlying(SegmentType::Blob));
		auto indexSegments = segmentTracker_->getSegmentsByType(toUnderlying(SegmentType::Index));
		auto stateSegments = segmentTracker_->getSegmentsByType(toUnderlying(SegmentType::State));

		double totalWeight = nodeSegments.size() + edgeSegments.size() +
							 propSegments.size() + blobSegments.size() +
							 indexSegments.size() + stateSegments.size();

		if (totalWeight == 0) {
			return 0.0;
		}

		auto ratio = (nodeRatio * nodeSegments.size() +
					  edgeRatio * edgeSegments.size() +
					  propertyRatio * propSegments.size() +
					  blobRatio * blobSegments.size() +
					  indexRatio * indexSegments.size() +
					  stateRatio * stateSegments.size()) /
					 totalWeight;

		return ratio;
	}

	void SpaceManager::recalculateMaxIds() const {
		// Reset all max IDs
		int64_t &maxNodeId = fileHeaderManager_->getMaxNodeIdRef();
		int64_t &maxEdgeId = fileHeaderManager_->getMaxEdgeIdRef();
		int64_t &maxPropId = fileHeaderManager_->getMaxPropIdRef();
		int64_t &maxBlobId = fileHeaderManager_->getMaxBlobIdRef();
		int64_t &maxIndexId = fileHeaderManager_->getMaxIndexIdRef();
		int64_t &maxStateId = fileHeaderManager_->getMaxStateIdRef();

		// Reset to initial values
		maxNodeId = 0;
		maxEdgeId = 0;
		maxPropId = 0;
		maxBlobId = 0;
		maxIndexId = 0;
		maxStateId = 0;

		// Scan all segments to determine true max IDs
		for (uint32_t type = 0; type <= getMaxEntityType(); type++) {
			auto segments = segmentTracker_->getSegmentsByType(type);

			for (const auto &header: segments) {
				// Only consider segments with active entities
				if (header.getActiveCount() > 0) {
					// Calculate max ID based on segment start_id and used values
					int64_t lastUsedId = calculateLastUsedIdInSegment(header);

					// Update appropriate max ID
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

	// Helper method to calculate the highest used ID in a segment considering the activity bitmap
	// Precondition: header.used > 0 (guaranteed by caller at line 313)
	int64_t SpaceManager::calculateLastUsedIdInSegment(const SegmentHeader &header) const {
		// Start with the segment's start_id
		int64_t maxId = header.start_id - 1; // Initialize to one less than start ID

		// Get the activity bitmap to check which entities are active
		auto segmentOffset = header.file_offset;

		// Find the highest active entity in the segment
		for (int64_t i = static_cast<int64_t>(header.used) - 1; i >= 0; i--) {
			if (segmentTracker_->isEntityActive(segmentOffset, i)) {
				// Found the highest active entity
				maxId = header.start_id + i;
				break;
			}
		}

		return maxId;
	}

	bool SpaceManager::safeCompactSegments() {
		// Try to acquire the compaction lock, return immediately if already locked
		if (!compactionMutex_.try_lock()) {
			return false; // Another thread is already performing compaction
		}

		// Perform the actual compaction
		compactSegments();

		// Always unlock when done
		compactionMutex_.unlock();
		return true;
	}

	void SpaceManager::compactSegments() {
		// Remove completely empty segments
		processAllEmptySegments();

		// Compact individual segments with high fragmentation
		compactSegments(toUnderlying(SegmentType::Node), COMPACTION_THRESHOLD);
		compactSegments(toUnderlying(SegmentType::Edge), COMPACTION_THRESHOLD);
		compactSegments(toUnderlying(SegmentType::Property), COMPACTION_THRESHOLD);
		compactSegments(toUnderlying(SegmentType::Blob), COMPACTION_THRESHOLD);
		compactSegments(toUnderlying(SegmentType::Index), COMPACTION_THRESHOLD);
		compactSegments(toUnderlying(SegmentType::State), COMPACTION_THRESHOLD);

		// Try to merge segments with low utilization
		mergeSegments(toUnderlying(SegmentType::Node), COMPACTION_THRESHOLD);
		mergeSegments(toUnderlying(SegmentType::Edge), COMPACTION_THRESHOLD);
		mergeSegments(toUnderlying(SegmentType::Property), COMPACTION_THRESHOLD);
		mergeSegments(toUnderlying(SegmentType::Blob), COMPACTION_THRESHOLD);
		mergeSegments(toUnderlying(SegmentType::Index), COMPACTION_THRESHOLD);
		mergeSegments(toUnderlying(SegmentType::State), COMPACTION_THRESHOLD);

		// Relocate segments from end of file to fill gaps
		static_cast<void>(relocateSegmentsFromEnd());

		// Update max IDs in file header
		updateMaxIds();

		// // Ensure chain heads are properly updated in file header
		// updateFileHeaderChainHeads();
	}

	bool SpaceManager::shouldCompact() const {
		// Only use fragmentation ratio based on COMPACTION_THRESHOLD
		double fragRatio = getTotalFragmentationRatio();
		return fragRatio > COMPACTION_THRESHOLD;
	}

	template<typename EntityType>
	bool SpaceManager::compactSegment(uint64_t offset, SegmentType segmentType, size_t entitySize) {
		SegmentHeader header = segmentTracker_->getSegmentHeader(offset);

		// If segment is empty, deallocate it
		if (header.used == header.inactive_count) {
			deallocateSegment(offset);
			return true;
		}

		// Calculate fragmentation ratio
		double fragmentationRatio = header.getFragmentationRatio();

		// Only perform compaction if fragmentation ratio exceeds threshold
		if (fragmentationRatio <= COMPACTION_THRESHOLD) {
			return true;
		}

		// Always compact in place
		uint32_t nextFreeSpot = 0;

		// Create a new activity bitmap
		uint8_t newBitmap[MAX_BITMAP_SIZE] = {};

		for (uint32_t i = 0; i < header.used; i++) {
			// Check if this entity is active
			if (bitmap::getBit(header.activity_bitmap, i)) {
				// Calculate IDs
				int64_t oldId = header.start_id + i;
				int64_t newId = header.start_id + nextFreeSpot;

				// Read entity using SegmentTracker
				auto entity = segmentTracker_->readEntity<EntityType>(offset, i, entitySize);

				// Update entity ID
				entity.setId(newId);

				// Write updated entity using SegmentTracker
				segmentTracker_->writeEntity<EntityType>(offset, nextFreeSpot, entity, entitySize);

				// Mark as active in new bitmap
				bitmap::setBit(newBitmap, nextFreeSpot, true);
				nextFreeSpot++;

				// Update references if entity reference updater is available
				if (entityReferenceUpdater_) {
					entityReferenceUpdater_->updateEntityReferences(oldId, &entity, toUnderlying(segmentType));
				}
			}
		}

		// Update header
		header.used = nextFreeSpot;
		header.inactive_count = 0;
		header.bitmap_size = bitmap::calculateBitmapSize(nextFreeSpot);
		std::memcpy(header.activity_bitmap, newBitmap, header.bitmap_size);

		segmentTracker_->writeSegmentHeader(offset, header);

		// Update segment usage
		segmentTracker_->updateSegmentUsage(offset, nextFreeSpot, 0);

		return true;
	}

	bool SpaceManager::compactNodeSegment(uint64_t offset) {
		return compactSegment<Node>(offset, SegmentType::Node, Node::getTotalSize());
	}

	bool SpaceManager::compactEdgeSegment(uint64_t offset) {
		return compactSegment<Edge>(offset, SegmentType::Edge, Edge::getTotalSize());
	}

	bool SpaceManager::compactPropertySegment(uint64_t offset) {
		return compactSegment<Property>(offset, SegmentType::Property, Property::getTotalSize());
	}

	bool SpaceManager::compactBlobSegment(uint64_t offset) {
		return compactSegment<Blob>(offset, SegmentType::Blob, Blob::getTotalSize());
	}

	bool SpaceManager::compactIndexSegment(uint64_t offset) {
		return compactSegment<Index>(offset, SegmentType::Index, Index::getTotalSize());
	}

	bool SpaceManager::compactStateSegment(uint64_t offset) {
		return compactSegment<State>(offset, SegmentType::State, State::getTotalSize());
	}

	// Check if segment is at the end of the file
	bool SpaceManager::isSegmentAtEndOfFile(uint64_t offset) const {
		uint64_t fileSize = calculateCurrentFileSize();
		uint64_t segmentEnd = offset + TOTAL_SEGMENT_SIZE;

		// Allow for small variations (within one segment size)
		return (fileSize - segmentEnd < TOTAL_SEGMENT_SIZE);
	}

	// Find a free segment that's not at the end of file
	uint64_t SpaceManager::findFreeSegmentNotAtEnd() const {
		auto freeSegments = segmentTracker_->getFreeSegments();
		if (freeSegments.empty()) {
			return 0; // No free segments
		}

		uint64_t fileSize = calculateCurrentFileSize();
		uint64_t endThreshold = fileSize - TOTAL_SEGMENT_SIZE;

		// Sort segments by offset (ascending)
		std::ranges::sort(freeSegments);

		// Find the first free segment that's not at the end
		for (uint64_t offset: freeSegments) {
			if (offset < endThreshold) {
				return offset;
			}
		}

		return 0; // No suitable segment found
	}

	uint64_t SpaceManager::findSegmentForNodeId(int64_t id) const {
		return segmentTracker_->getSegmentOffsetForNodeId(id);
	}

	uint64_t SpaceManager::findSegmentForEdgeId(int64_t id) const {
		// Use the tracker's built-in method for this operation
		return segmentTracker_->getSegmentOffsetForEdgeId(id);
	}

	void SpaceManager::updateMaxIds() const {
		recalculateMaxIds();
		fileHeaderManager_->updateFileHeaderMaxIds(segmentTracker_);
	}

	SegmentHeader SpaceManager::readSegmentHeader(uint64_t offset) const {
		// Directly use the tracker's method to read segment header
		return segmentTracker_->getSegmentHeader(offset);
	}

	void SpaceManager::writeSegmentHeader(uint64_t offset, const SegmentHeader &header) const {
		// Use tracker's method to write segment header
		segmentTracker_->writeSegmentHeader(offset, header);
	}

	std::vector<uint64_t> SpaceManager::findCandidatesForMerge(uint32_t type, double usageThreshold) const {
		std::vector<uint64_t> candidates;

		// Get all segments of the specified type
		auto segments = segmentTracker_->getSegmentsByType(type);

		// Filter segments with low usage
		for (const auto &header: segments) {
			double usageRatio = static_cast<double>(header.getActiveCount()) / header.capacity;
			if (usageRatio < usageThreshold) {
				candidates.push_back(header.file_offset);
			}
		}

		return candidates;
	}

	bool SpaceManager::mergeSegments(uint32_t type, double usageThreshold) {
		// Find candidate segments for merging
		auto candidates = findCandidatesForMerge(type, usageThreshold);

		// If fewer than 2 candidates, nothing to merge
		if (candidates.size() < 2) {
			return false;
		}

		// Calculate file size to identify segments near the end
		uint64_t fileSize = calculateCurrentFileSize();
		uint64_t endThreshold = fileSize - (fileSize / 5); // Last 20%

		// Align threshold to TOTAL_SEGMENT_SIZE boundary
		endThreshold = (endThreshold / TOTAL_SEGMENT_SIZE) * TOTAL_SEGMENT_SIZE;

		// First identify end segments and front segments separately
		std::vector<uint64_t> endSegments;
		std::vector<uint64_t> frontSegments;

		for (uint64_t offset: candidates) {
			if (offset >= endThreshold) {
				endSegments.push_back(offset);
			} else {
				frontSegments.push_back(offset);
			}
		}

		// Sort end segments by usage ratio (lowest first)
		std::ranges::sort(endSegments, [this](uint64_t a, uint64_t b) {
			SegmentHeader headerA = segmentTracker_->getSegmentHeader(a);
			SegmentHeader headerB = segmentTracker_->getSegmentHeader(b);

			double usageA = static_cast<double>(headerA.getActiveCount()) / headerA.capacity;
			double usageB = static_cast<double>(headerB.getActiveCount()) / headerB.capacity;

			return usageA < usageB;
		});

		// Sort front segments by position (ascending) and then by space available (descending)
		std::ranges::sort(frontSegments, [this](uint64_t a, uint64_t b) {
			// Primary sort by position (earlier first)
			if (a < b)
				return true;
			if (a > b)
				return false;

			// Secondary sort by free space (more free space first)
			SegmentHeader headerA = segmentTracker_->getSegmentHeader(a);
			SegmentHeader headerB = segmentTracker_->getSegmentHeader(b);

			uint32_t freeSpaceA = headerA.getTotalFreeSpace();
			uint32_t freeSpaceB = headerB.getTotalFreeSpace();

			return freeSpaceA > freeSpaceB;
		});

		bool mergedAny = false;

		// Track merged segments to avoid double processing
		std::unordered_set<uint64_t> mergedSegments;

		// PHASE 1: Try to move data from end segments to front segments
		for (uint64_t sourceOffset: endSegments) {
			if (mergedSegments.contains(sourceOffset)) {
				continue; // Already merged
			}

			SegmentHeader sourceHeader = segmentTracker_->getSegmentHeader(sourceOffset);
			uint32_t activeItemsSource = sourceHeader.getActiveCount();

			// Skip empty segments - they can just be marked as free later
			if (activeItemsSource == 0) {
				segmentTracker_->markSegmentFree(sourceOffset);
				mergedSegments.insert(sourceOffset);
				mergedAny = true;
				continue;
			}

			// Try each front segment as a potential target
			bool merged = false;
			for (uint64_t targetOffset: frontSegments) {
				if (mergedSegments.contains(targetOffset)) {
					continue; // Already used as a source
				}

				// Merge source into target
				if (mergeIntoSegment(targetOffset, sourceOffset, type)) {
					mergedAny = true;
					mergedSegments.insert(sourceOffset);
					merged = true;
					break;
				}
			}

			// If not merged with a front segment, try other end segments as fallback
			if (!merged) {
				// Sort remaining end segments by offset (ascending)
				std::vector<uint64_t> remainingEndSegments;
				for (uint64_t offset: endSegments) {
					if (offset != sourceOffset && !mergedSegments.contains(offset)) {
						remainingEndSegments.push_back(offset);
					}
				}

				std::ranges::sort(remainingEndSegments);

				for (uint64_t targetOffset: remainingEndSegments) {
					// Only merge with segments closer to the beginning of the file
					if (targetOffset >= sourceOffset) {
						continue;
					}

					if (mergeIntoSegment(targetOffset, sourceOffset, type)) {
						mergedAny = true;
						mergedSegments.insert(sourceOffset);
						merged = true;
						break;
					}
				}
			}
		}

		// PHASE 2: Now handle front segments (consolidate if they have low utilization)
		// Only process segments that weren't already used as targets
		std::vector<uint64_t> remainingFrontSegments;
		for (uint64_t offset: frontSegments) {
			if (!mergedSegments.contains(offset)) {
				remainingFrontSegments.push_back(offset);
			}
		}

		// Sort by usage ratio (lowest first)
		std::ranges::sort(remainingFrontSegments, [this](uint64_t a, uint64_t b) {
			const SegmentHeader headerA = segmentTracker_->getSegmentHeader(a);
			const SegmentHeader headerB = segmentTracker_->getSegmentHeader(b);

			double usageA = static_cast<double>(headerA.getActiveCount()) / headerA.capacity;
			double usageB = static_cast<double>(headerB.getActiveCount()) / headerB.capacity;

			return usageA < usageB;
		});

		// Try to consolidate remaining front segments
		for (size_t i = 0; i < remainingFrontSegments.size(); i++) {
			uint64_t sourceOffset = remainingFrontSegments[i];

			if (mergedSegments.contains(sourceOffset)) {
				continue; // Skip if already processed
			}

			SegmentHeader sourceHeader = segmentTracker_->getSegmentHeader(sourceOffset);
			uint32_t activeItemsSource = sourceHeader.getActiveCount();

			// Skip empty segments
			if (activeItemsSource == 0) {
				segmentTracker_->markSegmentFree(sourceOffset);
				mergedSegments.insert(sourceOffset);
				mergedAny = true;
				continue;
			}

			// Try to merge with another front segment
			for (size_t j = 0; j < remainingFrontSegments.size(); j++) {
				if (i == j)
					continue; // Skip self

				uint64_t targetOffset = remainingFrontSegments[j];
				if (mergedSegments.contains(targetOffset)) {
					continue; // Skip already processed
				}

				// Prefer merging into earlier segments
				if (targetOffset > sourceOffset) {
					continue;
				}

				if (mergeIntoSegment(targetOffset, sourceOffset, type)) {
					mergedAny = true;
					mergedSegments.insert(sourceOffset);
					break;
				}
			}
		}

		return mergedAny;
	}

	template<typename EntityType>
	void SpaceManager::processEntity(uint64_t sourceOffset, uint64_t targetOffset, uint32_t i,
									 uint32_t &targetNextIndex, uint8_t *newBitmap, size_t itemSize,
									 const SegmentHeader &sourceHeader, const SegmentHeader &targetHeader,
									 uint32_t type) {
		int64_t oldId = sourceHeader.start_id + i;
		int64_t newId = targetHeader.start_id + targetNextIndex;

		auto entity = segmentTracker_->readEntity<EntityType>(sourceOffset, i, itemSize);
		entity.setId(newId);
		segmentTracker_->writeEntity(targetOffset, targetNextIndex, entity, itemSize);

		if (entityReferenceUpdater_) {
			entityReferenceUpdater_->updateEntityReferences(oldId, &entity, type);
		}

		bitmap::setBit(newBitmap, targetNextIndex, true);
		targetNextIndex++;
	}

	bool SpaceManager::mergeIntoSegment(uint64_t targetOffset, uint64_t sourceOffset, uint32_t type) {
		SegmentHeader targetHeader = segmentTracker_->getSegmentHeader(targetOffset);
		SegmentHeader sourceHeader = segmentTracker_->getSegmentHeader(sourceOffset);

		if (targetHeader.data_type != type || sourceHeader.data_type != type) {
			return false;
		}

		uint32_t targetActiveItems = targetHeader.getActiveCount();
		uint32_t sourceActiveItems = sourceHeader.getActiveCount();

		if (targetActiveItems + sourceActiveItems > targetHeader.capacity) {
			return false;
		}

		size_t itemSize;
		switch (type) {
			case toUnderlying(SegmentType::Node):
				itemSize = Node::getTotalSize();
				break;
			case toUnderlying(SegmentType::Edge):
				itemSize = Edge::getTotalSize();
				break;
			case toUnderlying(SegmentType::Property):
				itemSize = Property::getTotalSize();
				break;
			case toUnderlying(SegmentType::Blob):
				itemSize = Blob::getTotalSize();
				break;
			case toUnderlying(SegmentType::Index):
				itemSize = Index::getTotalSize();
				break;
			case toUnderlying(SegmentType::State):
				itemSize = State::getTotalSize();
				break;
			default:
				return false;
		}

		uint32_t bitmapSize = bitmap::calculateBitmapSize(targetHeader.capacity);
		std::vector<uint8_t> newBitmap(bitmapSize, 0);
		std::memcpy(newBitmap.data(), targetHeader.activity_bitmap, std::min(bitmapSize, targetHeader.bitmap_size));

		uint32_t targetNextIndex = targetHeader.used;

		for (uint32_t i = 0; i < sourceHeader.used; i++) {
			if (bitmap::getBit(sourceHeader.activity_bitmap, i)) {
				switch (type) {
					case toUnderlying(SegmentType::Node):
						processEntity<Node>(sourceOffset, targetOffset, i, targetNextIndex, newBitmap.data(), itemSize,
											sourceHeader, targetHeader, type);
						break;
					case toUnderlying(SegmentType::Edge):
						processEntity<Edge>(sourceOffset, targetOffset, i, targetNextIndex, newBitmap.data(), itemSize,
											sourceHeader, targetHeader, type);
						break;
					case toUnderlying(SegmentType::Property):
						processEntity<Property>(sourceOffset, targetOffset, i, targetNextIndex, newBitmap.data(),
												itemSize, sourceHeader, targetHeader, type);
						break;
					case toUnderlying(SegmentType::Blob):
						processEntity<Blob>(sourceOffset, targetOffset, i, targetNextIndex, newBitmap.data(), itemSize,
											sourceHeader, targetHeader, type);
						break;
					case toUnderlying(SegmentType::Index):
						processEntity<Index>(sourceOffset, targetOffset, i, targetNextIndex, newBitmap.data(), itemSize,
											 sourceHeader, targetHeader, type);
						break;
					case toUnderlying(SegmentType::State):
						processEntity<State>(sourceOffset, targetOffset, i, targetNextIndex, newBitmap.data(), itemSize,
											 sourceHeader, targetHeader, type);
						break;
				}
			}
		}

		targetHeader.used = targetNextIndex;
		targetHeader.bitmap_size = bitmapSize;
		std::memcpy(targetHeader.activity_bitmap, newBitmap.data(), bitmapSize);
		segmentTracker_->writeSegmentHeader(targetOffset, targetHeader);

		if (sourceHeader.prev_segment_offset != 0) {
			SegmentHeader prevHeader = segmentTracker_->getSegmentHeader(sourceHeader.prev_segment_offset);
			segmentTracker_->updateSegmentLinks(sourceHeader.prev_segment_offset, prevHeader.prev_segment_offset,
												sourceHeader.next_segment_offset);
		} else {
			// Source was head
			segmentTracker_->updateChainHead(type, sourceHeader.next_segment_offset);
			// If we updated head, we must sync file header
			updateFileHeaderChainHeads();
		}

		if (sourceHeader.next_segment_offset != 0) {
			SegmentHeader nextHeader = segmentTracker_->getSegmentHeader(sourceHeader.next_segment_offset);
			segmentTracker_->updateSegmentLinks(sourceHeader.next_segment_offset, sourceHeader.prev_segment_offset,
												nextHeader.next_segment_offset);
		}

		segmentTracker_->markSegmentFree(sourceOffset);

		return true;
	}

	bool SpaceManager::copySegmentData(uint64_t sourceOffset, uint64_t destinationOffset) const {
		// Copy data in chunks to avoid large memory allocations
		constexpr size_t COPY_BLOCK_SIZE = 64 * 1024; // 64KB
		std::vector<char> buffer(COPY_BLOCK_SIZE);

		for (size_t offset = 0; offset < TOTAL_SEGMENT_SIZE; offset += COPY_BLOCK_SIZE) {
			size_t copySize = std::min(COPY_BLOCK_SIZE, TOTAL_SEGMENT_SIZE - offset);

			// Read from source
			file_->seekg(static_cast<std::streamoff>(sourceOffset + offset));
			file_->read(buffer.data(), static_cast<std::streamsize>(copySize));

			// Check for read errors
			if (file_->fail() || file_->gcount() != static_cast<std::streamsize>(copySize)) {
				return false;
			}

			// Write to destination
			file_->seekp(static_cast<std::streamoff>(destinationOffset + offset));
			file_->write(buffer.data(), static_cast<std::streamsize>(copySize));

			// Check for write errors
			if (file_->fail()) {
				return false;
			}
		}

		return true;
	}

	void SpaceManager::updateFileHeaderChainHeads() const {
		// Directly update the fileHeader_ member
		FileHeader &header = fileHeaderManager_->getFileHeader();

		// Update chain heads based on current tracker state
		header.node_segment_head = segmentTracker_->getChainHead(toUnderlying(SegmentType::Node));
		header.edge_segment_head = segmentTracker_->getChainHead(toUnderlying(SegmentType::Edge));
		header.property_segment_head = segmentTracker_->getChainHead(toUnderlying(SegmentType::Property));
		header.blob_segment_head = segmentTracker_->getChainHead(toUnderlying(SegmentType::Blob));
		header.index_segment_head = segmentTracker_->getChainHead(toUnderlying(SegmentType::Index));
		header.state_segment_head = segmentTracker_->getChainHead(toUnderlying(SegmentType::State));
	}

	void SpaceManager::updateSegmentChain(uint64_t newOffset, const SegmentHeader &header) const {
		// Check if this is a chain head
		bool isChainHead = false;
		uint64_t currentChainHead = segmentTracker_->getChainHead(header.data_type);

		if (currentChainHead == header.file_offset) {
			// This is the head of the chain, update chain head to point to new location
			segmentTracker_->updateChainHead(header.data_type, newOffset);
			isChainHead = true;
		}

		// Update prev segment to point to the new segment
		if (header.prev_segment_offset != 0) {
			SegmentHeader prevHeader = segmentTracker_->getSegmentHeader(header.prev_segment_offset);
			segmentTracker_->updateSegmentLinks(prevHeader.file_offset, prevHeader.prev_segment_offset, newOffset);
		}

		// Update next segment to point to the new segment
		if (header.next_segment_offset != 0) {
			SegmentHeader nextHeader = segmentTracker_->getSegmentHeader(header.next_segment_offset);
			segmentTracker_->updateSegmentLinks(nextHeader.file_offset, newOffset, nextHeader.next_segment_offset);
		}

		// If this was a chain head, update the file header as well
		if (isChainHead) {
			updateFileHeaderChainHeads();
		}
	}

	void SpaceManager::processAllEmptySegments() const {
		// Remove empty node segments
		processEmptySegments(toUnderlying(SegmentType::Node));

		// Remove empty edge segments
		processEmptySegments(toUnderlying(SegmentType::Edge));

		// Remove empty property segments
		processEmptySegments(toUnderlying(SegmentType::Property));

		// Remove empty blob segments
		processEmptySegments(toUnderlying(SegmentType::Blob));

		// Remove empty Index segments
		processEmptySegments(toUnderlying(SegmentType::Index));

		// Remove empty State segments
		processEmptySegments(toUnderlying(SegmentType::State));
	}

	bool SpaceManager::processEmptySegments(uint32_t type) const {
		bool deallocatedAny = false;

		// Get all segments of the specified type
		auto segments = segmentTracker_->getSegmentsByType(type);

		// Find segments with no active entities
		std::vector<uint64_t> emptySegments;
		for (const auto &header: segments) {
			if (header.getActiveCount() == 0) {
				emptySegments.push_back(header.file_offset);
			}
		}

		// deallocate each empty segment
		for (uint64_t segmentOffset: emptySegments) {
			// Deallocate using our existing method
			deallocateSegment(segmentOffset);
			deallocatedAny = true;
		}

		return deallocatedAny;
	}

	bool SpaceManager::relocateSegmentsFromEnd() const {
		// Get free segments that we can use as targets
		auto freeSegments = segmentTracker_->getFreeSegments();
		if (freeSegments.empty()) {
			return false; // No free slots to fill
		}

		// Sort free segments by offset (ascending)
		std::ranges::sort(freeSegments);

		// Find segments near the end that can be relocated
		auto relocatableSegments = findRelocatableSegments();
		if (relocatableSegments.empty()) {
			return false; // No segments to relocate
		}

		// Sort relocatable segments by offset (descending) to process from end of file
		std::ranges::sort(relocatableSegments, std::greater());

		bool anyMoved = false;

		// Match relocatable segments with free slots
		for (uint64_t sourceOffset: relocatableSegments) {
			if (freeSegments.empty())
				break;

			// Get the next free slot (from the beginning of the file)
			uint64_t targetOffset = freeSegments.front();
			freeSegments.erase(freeSegments.begin());

			// Skip if the source is before the target (would be counterproductive)
			if (sourceOffset <= targetOffset) {
				continue;
			}

			// Move the segment
			if (moveSegment(sourceOffset, targetOffset)) {
				anyMoved = true;
			}
		}

		return anyMoved;
	}

	std::vector<uint64_t> SpaceManager::findRelocatableSegments() const {
		std::vector<uint64_t> result;
		uint64_t fileSize = calculateCurrentFileSize();

		uint64_t thresholdOffset = fileSize - (fileSize / 5);
		thresholdOffset = (thresholdOffset / TOTAL_SEGMENT_SIZE) * TOTAL_SEGMENT_SIZE;

		for (uint32_t type = 0; type <= getMaxEntityType(); type++) {
			auto segments = segmentTracker_->getSegmentsByType(type);
			for (const auto &header: segments) {
				if (header.file_offset >= thresholdOffset) {
					result.push_back(header.file_offset);
				}
			}
		}
		return result;
	}

	uint64_t SpaceManager::calculateCurrentFileSize() const {
		// Start with FileHeader size as minimum file size
		uint64_t maxOffset = FILE_HEADER_SIZE;

		// Check all segments (active and free)
		for (uint32_t type = 0; type <= getMaxEntityType(); type++) {
			auto segments = segmentTracker_->getSegmentsByType(type);
			for (const auto &header: segments) {
				// Calculate the end of this segment
				uint64_t endOffset = header.file_offset + TOTAL_SEGMENT_SIZE;
				maxOffset = std::max(maxOffset, endOffset);
			}
		}

		// Also check free segments
		auto freeSegments = segmentTracker_->getFreeSegments();
		for (uint64_t offset: freeSegments) {
			// Assume free segments are a full SEGMENT_SIZE
			uint64_t endOffset = offset + TOTAL_SEGMENT_SIZE;
			maxOffset = std::max(maxOffset, endOffset);
		}

		return maxOffset;
	}

	bool SpaceManager::truncateFile(file_handle_t nativeFd) const {
		// Find segments at the end of file that can be truncated
		auto truncatableSegments = findTruncatableSegments();

		if (truncatableSegments.empty()) {
			return false; // Nothing to truncate
		}

		// Sort segments by offset (ascending)
		std::ranges::sort(truncatableSegments);

		// Find the new file size (start of the first truncatable segment)
		uint64_t newFileSize = truncatableSegments.front();

		// Ensure we never truncate below the FileHeader
		if (newFileSize < FILE_HEADER_SIZE) {
			newFileSize = FILE_HEADER_SIZE;
		}

		// Flush all pending writes from the fstream buffer
		file_->flush();

		if (nativeFd != INVALID_FILE_HANDLE) {
			// Preferred path: truncate via native file handle.
			// This avoids the close/reopen cycle that can leave leaked handles
			// (especially problematic on Windows where open handles prevent file
			// deletion) and is more efficient since no kernel-level open/close
			// overhead is incurred.
			if (portable_ftruncate(nativeFd, newFileSize) != 0) {
				return false;
			}

			// The fstream's internal HANDLE/fd still points to the same file
			// which has now been shortened underneath it. Reset its state so
			// subsequent I/O (e.g. updateFileCrc) works correctly.
			//
			// Precondition: the caller must have flushed all pending writes
			// and drained any cached reads BEFORE calling truncateFile().
			// This ensures the fstream has no stale buffered data that could
			// reference offsets beyond the new file size.
			file_->clear();
			file_->seekg(0, std::ios::beg);
			file_->seekp(0, std::ios::beg);
		} else {
			// Fallback: close, truncate via filesystem API, reopen.
			// Used when no native fd is available (e.g. unit tests that create
			// SpaceManager without a companion pwrite handle).
			file_->close();
			std::filesystem::resize_file(fileName_, newFileSize);
			file_->open(fileName_, std::ios::in | std::ios::out | std::ios::binary);
		}

		// Remove truncated segments from free list
		for (uint64_t offset: truncatableSegments) {
			segmentTracker_->removeFromFreeList(offset);
		}

		return true;
	}

	std::vector<uint64_t> SpaceManager::findTruncatableSegments() const {
		std::vector<uint64_t> result;

		// Get all free segments
		auto freeSegments = segmentTracker_->getFreeSegments();

		// Sort by offset (ascending)
		std::ranges::sort(freeSegments);

		// Find the highest used offset in any chain
		uint64_t highestActiveOffset = 0;

		for (uint32_t type = 0; type <= getMaxEntityType(); type++) {
			auto segments = segmentTracker_->getSegmentsByType(type);
			for (const auto &header: segments) {
				uint64_t endOffset = header.file_offset + TOTAL_SEGMENT_SIZE;
				highestActiveOffset = std::max(highestActiveOffset, endOffset);
			}
		}

		// Find contiguous free segments at the end of file
		for (uint64_t offset: freeSegments) {
			if (offset >= highestActiveOffset) {
				result.push_back(offset);
			}
		}

		return result;
	}

} // namespace graph::storage
