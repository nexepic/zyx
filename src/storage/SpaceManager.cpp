/**
 * @file SpaceManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/SpaceManager.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unordered_set>
#include "graph/storage/SegmentType.h"

namespace graph::storage {

	SpaceManager::SpaceManager(std::shared_ptr<std::fstream> file, std::string fileName,
							   std::shared_ptr<SegmentTracker> tracker,
							   std::shared_ptr<FileHeaderManager> fileHeaderManager,
							   std::shared_ptr<IDAllocator> idAllocator,
							   std::shared_ptr<EntityReferenceUpdater> entityReferenceUpdater) :
		file_(file), fileName_(fileName), segmentTracker_(tracker), fileHeaderManager_(fileHeaderManager),
		idAllocator_(idAllocator), entityReferenceUpdater_(entityReferenceUpdater) {}

	SpaceManager::~SpaceManager() = default;

	void SpaceManager::initialize(FileHeader &header) {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Initialize tracker with header
		segmentTracker_->initialize(header);
	}

	uint64_t SpaceManager::findMaxId(uint32_t type, std::shared_ptr<SegmentTracker> &tracker) {
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

	uint64_t SpaceManager::allocateSegment(uint32_t type, uint32_t capacity) {
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
			file_->write(zeros.data(), segmentSize);
		}

		// Initialize segment header
		SegmentHeader header;
		header.capacity = capacity;
		header.used = 0;
		header.data_type = type;
		header.next_segment_offset = 0;
		header.prev_segment_offset = 0;
		header.segment_crc = 0;
		header.inactive_count = 0;
		header.bitmap_size = bitmap::calculateBitmapSize(capacity);

		// Start ID calculation for nodes, edges, properties, and blobs
		if (type >= 0 && type <= 3) {
			header.start_id = findMaxId(type, segmentTracker_) + 1;
		} else {
			throw std::invalid_argument("Invalid segment type");
		}

		// Write header
		file_->seekp(static_cast<std::streamoff>(offset));
		file_->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));

		// Register with tracker
		segmentTracker_->registerSegment(offset, type, capacity);

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
				SegmentHeader header = segmentTracker_->getSegmentHeader(current);
				prev = current;
				current = header.next_segment_offset;
			}

			// Link the new segment to the end of chain
			if (prev != 0) {
				segmentTracker_->updateSegmentLinks(offset, prev, 0);
				SegmentHeader prevHeader = segmentTracker_->getSegmentHeader(prev);
				segmentTracker_->updateSegmentLinks(prev, prevHeader.prev_segment_offset, offset);
			}
		}

		return offset;
	}

	void SpaceManager::deallocateSegment(uint64_t offset) {
		// Get segment info before it's removed
		SegmentHeader header = segmentTracker_->getSegmentHeader(offset);

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

		// Mark segment as free
		segmentTracker_->markSegmentFree(offset);
	}

	void SpaceManager::compactSegments(uint32_t type, double threshold) {
		// Get segments that need compaction based on total free space, not just inactive elements
		auto segments = segmentTracker_->getSegmentsNeedingCompaction(type, threshold);

		// Limit to top 5 segments with highest fragmentation
		if (segments.size() > 5) {
			std::sort(segments.begin(), segments.end(), [](const SegmentHeader &a, const SegmentHeader &b) {
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
				default:;
			}
		}
	}

	bool SpaceManager::moveSegment(uint64_t sourceOffset, uint64_t destinationOffset) {
		if (sourceOffset == destinationOffset) {
			return true; // Nothing to do
		}

		try {
			// Get source segment info from tracker
			SegmentHeader sourceHeader = segmentTracker_->getSegmentHeader(sourceOffset);
			uint32_t segmentType = sourceHeader.data_type;

			// Get destination segment info (if it's a free segment)
			SegmentHeader destHeader;
			bool isFreeSegment = false;

			try {
				// Try to get header from free list first
				destHeader = segmentTracker_->getFreeSegmentHeader(destinationOffset);
				isFreeSegment = true;
			} catch (const std::exception &) {
				// If not in free list, try to read directly (this is a fallback)
				try {
					destHeader = segmentTracker_->getSegmentHeader(destinationOffset);
				} catch (const std::exception &) {
					// If we can't get header info at all, initialize a new one
					destHeader = sourceHeader;
					destHeader.file_offset = destinationOffset;
				}
			}

			// Create ID mapping if the start_id values differ and we're moving to a previously
			// used segment location
			std::vector<EntityIdMapping> idMappings;

			if (isFreeSegment && destHeader.start_id != 0 && destHeader.start_id != sourceHeader.start_id) {
				idMappings = mapEntityIds(segmentType, sourceOffset, destinationOffset, sourceHeader, destHeader);
			}

			// Save active entities information for reference updates before copying data
			std::vector<int64_t> activeEntityIds;
			std::vector<bool> activityMap = segmentTracker_->getActivityBitmap(sourceOffset);
			for (uint32_t i = 0; i < sourceHeader.used && i < activityMap.size(); i++) {
				if (activityMap[i]) {
					activeEntityIds.push_back(sourceHeader.start_id + i);
				}
			}

			// Copy the segment data (both header and content)
			copySegmentData(sourceOffset, destinationOffset, sourceHeader);

			// Update entity IDs if we have mappings
			if (!idMappings.empty()) {
				updateEntitiesWithNewIds(segmentType, destinationOffset, idMappings, destHeader);

				// Update references for each entity with ID mapping
				for (const auto &mapping: idMappings) {
					entityReferenceUpdater_->updateEntityReferences(mapping.oldId, segmentType, sourceOffset,
																	destinationOffset);
				}
			} else {
				// Directly update references for each active entity
				for (int64_t entityId: activeEntityIds) {
					entityReferenceUpdater_->updateEntityReferences(entityId, segmentType, sourceOffset,
																	destinationOffset);
				}
			}

			// Prepare updated header for the destination
			SegmentHeader newHeader = sourceHeader;

			// If moving to a previously used segment, preserve its start_id to maintain ID sequence
			if (isFreeSegment && destHeader.start_id != 0) {
				newHeader.start_id = destHeader.start_id;
			}

			newHeader.file_offset = destinationOffset;

			// Write the updated header at the destination
			segmentTracker_->writeSegmentHeader(destinationOffset, newHeader);

			// Update chain links
			updateSegmentChain(destinationOffset, sourceHeader);

			// If this is the head of a chain, update the chain head
			if (sourceHeader.prev_segment_offset == 0) {
				segmentTracker_->updateChainHead(sourceHeader.data_type, destinationOffset);
			}

			// Mark old segment as free
			segmentTracker_->markSegmentFree(sourceOffset);

			return true;
		} catch (const std::exception &e) {
			std::cerr << "Error moving segment: " << e.what() << std::endl;
			return false;
		}
	}

	std::vector<SpaceManager::EntityIdMapping> SpaceManager::mapEntityIds(uint32_t type, uint64_t sourceOffset,
																		  uint64_t destOffset,
																		  const SegmentHeader &sourceHeader,
																		  const SegmentHeader &destHeader) {

		std::vector<EntityIdMapping> idMappings;

		// Get information about active entities in the source segment
		std::vector<bool> activityMap = segmentTracker_->getActivityBitmap(sourceOffset);

		// Calculate ID offset difference
		int64_t idDiff = destHeader.start_id - sourceHeader.start_id;

		// Create mapping for each active entity
		for (uint32_t i = 0; i < sourceHeader.used && i < activityMap.size(); i++) {
			if (activityMap[i]) {
				EntityIdMapping mapping;
				mapping.oldId = sourceHeader.start_id + i;
				mapping.newId = mapping.oldId + idDiff;
				mapping.index = i;
				idMappings.push_back(mapping);
			}
		}

		return idMappings;
	}

	void SpaceManager::updateEntitiesWithNewIds(uint32_t type, uint64_t destOffset,
												const std::vector<EntityIdMapping> &idMappings,
												const SegmentHeader &header) {

		// Determine entity size based on type
		size_t entitySize;
		switch (type) {
			case toUnderlying(SegmentType::Node):
				entitySize = Node::getTotalSize();
				break;
			case toUnderlying(SegmentType::Edge):
				entitySize = Edge::getTotalSize();
				break;
			case toUnderlying(SegmentType::Property):
				entitySize = Property::getTotalSize();
				break;
			case toUnderlying(SegmentType::Blob):
				entitySize = Blob::getTotalSize();
				break;
			default:
				throw std::invalid_argument("Invalid segment type");
		}

		// Update each entity with its new ID
		for (const auto &mapping: idMappings) {
			// Calculate relative index in the segment
			uint32_t index = mapping.index;

			// Update ID based on entity type
			switch (type) {
				case toUnderlying(SegmentType::Node): {
					// Read entity using serialization
					auto node = segmentTracker_->readEntity<Node>(destOffset, index, entitySize);
					node.setId(mapping.newId);
					// Write updated entity back
					segmentTracker_->writeEntity(destOffset, index, node, entitySize);
					break;
				}
				case toUnderlying(SegmentType::Edge): {
					auto edge = segmentTracker_->readEntity<Edge>(destOffset, index, entitySize);
					edge.setId(mapping.newId);
					segmentTracker_->writeEntity(destOffset, index, edge, entitySize);
					break;
				}
				case toUnderlying(SegmentType::Property): {
					auto property = segmentTracker_->readEntity<Property>(destOffset, index, entitySize);
					property.setId(mapping.newId);
					segmentTracker_->writeEntity(destOffset, index, property, entitySize);
					break;
				}
				case toUnderlying(SegmentType::Blob): {
					auto blob = segmentTracker_->readEntity<Blob>(destOffset, index, entitySize);
					blob.setId(mapping.newId);
					segmentTracker_->writeEntity(destOffset, index, blob, entitySize);
					break;
				}
				default:;
			}
		}
	}

	double SpaceManager::getTotalFragmentationRatio() const {
		double nodeRatio = segmentTracker_->calculateFragmentationRatio(toUnderlying(SegmentType::Node));
		double edgeRatio = segmentTracker_->calculateFragmentationRatio(toUnderlying(SegmentType::Edge));
		double propertyRatio = segmentTracker_->calculateFragmentationRatio(toUnderlying(SegmentType::Property));
		double blobRatio = segmentTracker_->calculateFragmentationRatio(toUnderlying(SegmentType::Blob));

		// Weight by segment counts
		auto nodeSegments = segmentTracker_->getSegmentsByType(toUnderlying(SegmentType::Node));
		auto edgeSegments = segmentTracker_->getSegmentsByType(toUnderlying(SegmentType::Edge));
		auto propSegments = segmentTracker_->getSegmentsByType(toUnderlying(SegmentType::Property));
		auto blobSegments = segmentTracker_->getSegmentsByType(toUnderlying(SegmentType::Blob));

		double totalWeight = static_cast<double>(nodeSegments.size()) + static_cast<double>(edgeSegments.size()) +
							 static_cast<double>(propSegments.size()) + static_cast<double>(blobSegments.size());

		if (totalWeight == 0) {
			return 0.0;
		}

		auto ratio = (nodeRatio * static_cast<double>(nodeSegments.size()) +
					  edgeRatio * static_cast<double>(edgeSegments.size()) +
					  propertyRatio * static_cast<double>(propSegments.size()) +
					  blobRatio * static_cast<double>(blobSegments.size())) /
					 totalWeight;

		return ratio;
	}

	void SpaceManager::compactSegments() {
		// Remove completely empty segments
		processAllEmptySegments();

		// Compact individual segments with high fragmentation
		compactSegments(toUnderlying(SegmentType::Node), COMPACTION_THRESHOLD);
		compactSegments(toUnderlying(SegmentType::Edge), COMPACTION_THRESHOLD);
		compactSegments(toUnderlying(SegmentType::Property), COMPACTION_THRESHOLD);
		compactSegments(toUnderlying(SegmentType::Blob), COMPACTION_THRESHOLD);

		// Try to merge segments with low utilization
		mergeSegments(toUnderlying(SegmentType::Node), COMPACTION_THRESHOLD);
		mergeSegments(toUnderlying(SegmentType::Edge), COMPACTION_THRESHOLD);
		mergeSegments(toUnderlying(SegmentType::Property), COMPACTION_THRESHOLD);
		mergeSegments(toUnderlying(SegmentType::Blob), COMPACTION_THRESHOLD);

		// Relocate segments from end of file to fill gaps
		relocateSegmentsFromEnd();

		updateMaxIds();
	}

	bool SpaceManager::shouldCompact() const {
		// Only use fragmentation ratio based on COMPACTION_THRESHOLD
		double fragRatio = getTotalFragmentationRatio();
		return fragRatio > COMPACTION_THRESHOLD;
	}

	bool SpaceManager::compactNodeSegment(uint64_t offset) {
		SegmentHeader header = segmentTracker_->getSegmentHeader(offset);

		// If inactive count is zero, nothing to do
		if (header.getFragmentationRatio() == 0) {
			return false;
		}

		// If segment is empty, deallocate it
		if (header.used == header.inactive_count) {
			deallocateSegment(offset);
			return true;
		}

		// Calculate fragmentation ratio
		double fragmentationRatio = header.getFragmentationRatio();

		// Only perform compaction if fragmentation ratio exceeds threshold
		if (fragmentationRatio <= COMPACTION_THRESHOLD) {
			// No need to compact yet
			return true;
		}

		// Always compact in place
		size_t nodeSize = Node::getTotalSize();
		uint32_t nextFreeSpot = 0;

		// Create a new activity bitmap
		uint8_t newBitmap[MAX_BITMAP_SIZE] = {};

		std::vector<char> nodeBuffer(nodeSize);
		for (uint32_t i = 0; i < header.used; i++) {
			// Check if this node is active
			if (bitmap::getBit(header.activity_bitmap, i)) {
				// Read node
				file_->seekg(static_cast<std::streamoff>(offset + sizeof(SegmentHeader) +
														 static_cast<std::streamoff>(i * nodeSize)));
				file_->read(nodeBuffer.data(), static_cast<std::streamsize>(nodeSize));

				// Update node ID
				Node *node = reinterpret_cast<Node *>(nodeBuffer.data());
				node->setId(header.start_id + nextFreeSpot);

				// Write to the free spot
				file_->seekp(static_cast<std::streamoff>(offset + sizeof(SegmentHeader) +
														 static_cast<std::streamoff>(nextFreeSpot * nodeSize)));
				file_->write(nodeBuffer.data(), static_cast<std::streamsize>(nodeSize));

				// Mark as active in new bitmap
				bitmap::setBit(newBitmap, nextFreeSpot, true);
				nextFreeSpot++;
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

	bool SpaceManager::compactEdgeSegment(uint64_t offset) {
		SegmentHeader header = segmentTracker_->getSegmentHeader(offset);

		// If inactive count is zero, nothing to do
		if (header.getFragmentationRatio() == 0) {
			return false;
		}

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
		size_t edgeSize = Edge::getTotalSize();
		uint32_t nextFreeSpot = 0;

		// Create a new activity bitmap
		uint8_t newBitmap[MAX_BITMAP_SIZE] = {};

		std::vector<char> edgeBuffer(edgeSize);
		for (uint32_t i = 0; i < header.used; i++) {
			// Check if this edge is active
			if (bitmap::getBit(header.activity_bitmap, i)) {
				if (i != nextFreeSpot) {
					// Read edge
					file_->seekg(static_cast<std::streamoff>(offset + sizeof(SegmentHeader) + i * edgeSize));
					file_->read(edgeBuffer.data(), static_cast<std::streamsize>(edgeSize));

					// Update edge ID
					Edge *edge = reinterpret_cast<Edge *>(edgeBuffer.data());
					edge->setId(header.start_id + nextFreeSpot);

					// Write to the free spot
					file_->seekp(static_cast<std::streamoff>(offset + sizeof(SegmentHeader) + nextFreeSpot * edgeSize));
					file_->write(edgeBuffer.data(), static_cast<std::streamsize>(edgeSize));
				}

				// Mark as active in new bitmap
				bitmap::setBit(newBitmap, nextFreeSpot, true);
				nextFreeSpot++;
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

	bool SpaceManager::compactPropertySegment(uint64_t offset) {
		SegmentHeader header = segmentTracker_->getSegmentHeader(offset);

		// If inactive count is zero, nothing to do
		if (header.inactive_count == 0) {
			return true;
		}

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
		size_t propertySize = Property::getTotalSize(); // Use the entity-based Property size
		uint32_t nextFreeSpot = 0;

		// Create a new activity bitmap
		uint8_t newBitmap[MAX_BITMAP_SIZE] = {};

		std::vector<char> propertyBuffer(propertySize);
		for (uint32_t i = 0; i < header.used; i++) {
			// Check if this property is active
			if (bitmap::getBit(header.activity_bitmap, i)) {
				if (i != nextFreeSpot) {
					// Read property
					file_->seekg(static_cast<std::streamoff>(offset + sizeof(SegmentHeader) + i * propertySize));
					file_->read(propertyBuffer.data(), static_cast<std::streamsize>(propertySize));

					// Update property ID if needed
					Property *property = reinterpret_cast<Property *>(propertyBuffer.data());
					property->setId(header.start_id + nextFreeSpot);

					// Write to the free spot
					file_->seekp(
							static_cast<std::streamoff>(offset + sizeof(SegmentHeader) + nextFreeSpot * propertySize));
					file_->write(propertyBuffer.data(), static_cast<std::streamsize>(propertySize));
				}

				// Mark as active in new bitmap
				bitmap::setBit(newBitmap, nextFreeSpot, true);
				nextFreeSpot++;
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

	bool SpaceManager::compactBlobSegment(uint64_t offset) {
		SegmentHeader header = segmentTracker_->getSegmentHeader(offset);

		// If inactive count is zero, nothing to do
		if (header.inactive_count == 0) {
			return true;
		}

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
		size_t blobSize = Blob::getTotalSize(); // Assuming Blob is the structure for blobs
		uint32_t nextFreeSpot = 0;

		// Create a new activity bitmap
		uint8_t newBitmap[MAX_BITMAP_SIZE] = {};

		std::vector<char> blobBuffer(blobSize);
		for (uint32_t i = 0; i < header.used; i++) {
			// Check if this blob is active
			if (bitmap::getBit(header.activity_bitmap, i)) {
				if (i != nextFreeSpot) {
					// Read blob
					file_->seekg(static_cast<std::streamoff>(offset + sizeof(SegmentHeader) + i * blobSize));
					file_->read(blobBuffer.data(), static_cast<std::streamsize>(blobSize));

					// Update blob ID if needed
					Blob *blob = reinterpret_cast<Blob *>(blobBuffer.data());
					blob->setId(header.start_id + nextFreeSpot);

					// Write to the free spot
					file_->seekp(static_cast<std::streamoff>(offset + sizeof(SegmentHeader) + nextFreeSpot * blobSize));
					file_->write(blobBuffer.data(), static_cast<std::streamsize>(blobSize));
				}

				// Mark as active in new bitmap
				bitmap::setBit(newBitmap, nextFreeSpot, true);
				nextFreeSpot++;
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
		std::sort(freeSegments.begin(), freeSegments.end());

		// Find the first free segment that's not at the end
		for (uint64_t offset: freeSegments) {
			if (offset < endThreshold) {
				return offset;
			}
		}

		return 0; // No suitable segment found
	}

	// Initialize segment header without allocating new space
	void SpaceManager::initializeSegmentHeader(uint64_t offset, uint32_t type, uint32_t capacity) {
		// Remove from free list if it's there
		segmentTracker_->removeFromFreeList(offset);

		// Initialize segment header for all types
		SegmentHeader header;
		header.capacity = capacity;
		header.used = 0;
		header.data_type = type;
		header.next_segment_offset = 0;
		header.prev_segment_offset = 0;
		header.segment_crc = 0;
		header.inactive_count = 0;
		header.bitmap_size = bitmap::calculateBitmapSize(capacity);

		// Write header
		file_->seekp(static_cast<std::streamoff>(offset));
		file_->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));

		// Register with tracker
		segmentTracker_->registerSegment(offset, type, capacity);
	}

	uint64_t SpaceManager::findSegmentForNodeId(int64_t id) const {
		return segmentTracker_->getSegmentOffsetForNodeId(id);
	}

	uint64_t SpaceManager::findSegmentForEdgeId(int64_t id) const {
		// Use the tracker's built-in method for this operation
		return segmentTracker_->getSegmentOffsetForEdgeId(id);
	}

	void SpaceManager::updateMaxIds() {
		fileHeaderManager_->updateFileHeaderMaxIds(segmentTracker_);
		// auto maxNodeId = fileHeaderManager_->getMaxNodeId();
		// auto maxEdgeId = fileHeaderManager_->getMaxEdgeId();
		// idAllocator_->updateMaxIds(maxNodeId, maxEdgeId);
	}

	SegmentHeader SpaceManager::readSegmentHeader(uint64_t offset) const {
		// Directly use the tracker's method to read segment header
		return segmentTracker_->getSegmentHeader(offset);
	}

	void SpaceManager::writeSegmentHeader(uint64_t offset, const SegmentHeader &header) {
		// Use tracker's method to write segment header
		segmentTracker_->writeSegmentHeader(offset, header);
	}

	std::vector<uint64_t> SpaceManager::findCandidatesForMerge(uint32_t type, double usageThreshold) {
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
		std::sort(endSegments.begin(), endSegments.end(), [this](uint64_t a, uint64_t b) {
			SegmentHeader headerA = segmentTracker_->getSegmentHeader(a);
			SegmentHeader headerB = segmentTracker_->getSegmentHeader(b);

			double usageA = static_cast<double>(headerA.getActiveCount()) / headerA.capacity;
			double usageB = static_cast<double>(headerB.getActiveCount()) / headerB.capacity;

			return usageA < usageB;
		});

		// Sort front segments by position (ascending) and then by space available (descending)
		std::sort(frontSegments.begin(), frontSegments.end(), [this](uint64_t a, uint64_t b) {
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
			if (mergedSegments.count(sourceOffset) > 0) {
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
				if (mergedSegments.count(targetOffset) > 0) {
					continue; // Already used as a source
				}

				SegmentHeader targetHeader = segmentTracker_->getSegmentHeader(targetOffset);
				uint32_t activeItemsTarget = targetHeader.getActiveCount();

				// Check if source can fit into target
				if (activeItemsSource + activeItemsTarget <= targetHeader.capacity) {
					// Merge source into target
					if (mergeIntoSegment(targetOffset, sourceOffset, type)) {
						mergedAny = true;
						mergedSegments.insert(sourceOffset);
						merged = true;
						break;
					}
				}
			}

			// If not merged with a front segment, try other end segments as fallback
			if (!merged) {
				// Sort remaining end segments by offset (ascending)
				std::vector<uint64_t> remainingEndSegments;
				for (uint64_t offset: endSegments) {
					if (offset != sourceOffset && mergedSegments.count(offset) == 0) {
						remainingEndSegments.push_back(offset);
					}
				}

				std::sort(remainingEndSegments.begin(), remainingEndSegments.end());

				for (uint64_t targetOffset: remainingEndSegments) {
					// Only merge with segments closer to the beginning of the file
					if (targetOffset >= sourceOffset) {
						continue;
					}

					SegmentHeader targetHeader = segmentTracker_->getSegmentHeader(targetOffset);
					uint32_t activeItemsTarget = targetHeader.getActiveCount();

					if (activeItemsSource + activeItemsTarget <= targetHeader.capacity) {
						if (mergeIntoSegment(targetOffset, sourceOffset, type)) {
							mergedAny = true;
							mergedSegments.insert(sourceOffset);
							merged = true;
							break;
						}
					}
				}
			}
		}

		// PHASE 2: Now handle front segments (consolidate if they have low utilization)
		// Only process segments that weren't already used as targets
		std::vector<uint64_t> remainingFrontSegments;
		for (uint64_t offset: frontSegments) {
			if (mergedSegments.count(offset) == 0) {
				remainingFrontSegments.push_back(offset);
			}
		}

		// Sort by usage ratio (lowest first)
		std::sort(remainingFrontSegments.begin(), remainingFrontSegments.end(), [this](uint64_t a, uint64_t b) {
			SegmentHeader headerA = segmentTracker_->getSegmentHeader(a);
			SegmentHeader headerB = segmentTracker_->getSegmentHeader(b);

			double usageA = static_cast<double>(headerA.getActiveCount()) / headerA.capacity;
			double usageB = static_cast<double>(headerB.getActiveCount()) / headerB.capacity;

			return usageA < usageB;
		});

		// Try to consolidate remaining front segments
		for (size_t i = 0; i < remainingFrontSegments.size(); i++) {
			uint64_t sourceOffset = remainingFrontSegments[i];

			if (mergedSegments.count(sourceOffset) > 0) {
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
				if (mergedSegments.count(targetOffset) > 0) {
					continue; // Skip already processed
				}

				// Prefer merging into earlier segments
				if (targetOffset > sourceOffset) {
					continue;
				}

				SegmentHeader targetHeader = segmentTracker_->getSegmentHeader(targetOffset);
				uint32_t activeItemsTarget = targetHeader.getActiveCount();

				if (activeItemsSource + activeItemsTarget <= targetHeader.capacity) {
					if (mergeIntoSegment(targetOffset, sourceOffset, type)) {
						mergedAny = true;
						mergedSegments.insert(sourceOffset);
						break;
					}
				}
			}
		}

		// Clean up any merged segments that weren't properly freed
		for (uint64_t offset: mergedSegments) {
			// Verify segment is actually in the free list
			auto freeSegments = segmentTracker_->getFreeSegments();
			if (std::find(freeSegments.begin(), freeSegments.end(), offset) == freeSegments.end()) {
				segmentTracker_->markSegmentFree(offset);
			}
		}

		return mergedAny;
	}

	bool SpaceManager::mergeIntoSegment(uint64_t targetOffset, uint64_t sourceOffset, uint32_t type) {
		// Get headers directly from tracker
		SegmentHeader targetHeader = segmentTracker_->getSegmentHeader(targetOffset);
		SegmentHeader sourceHeader = segmentTracker_->getSegmentHeader(sourceOffset);

		// Ensure the types match
		if (targetHeader.data_type != type || sourceHeader.data_type != type) {
			return false;
		}

		// Calculate active items and available space
		uint32_t targetActiveItems = targetHeader.getActiveCount();
		uint32_t sourceActiveItems = sourceHeader.getActiveCount();

		// Ensure there's enough space
		if (targetActiveItems + sourceActiveItems > targetHeader.capacity) {
			return false;
		}

		// Get appropriate item size based on type
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
			default:
				return false;
		}

		// Create a copy of the target bitmap to work with
		uint32_t bitmapSize = bitmap::calculateBitmapSize(targetHeader.capacity);
		std::vector<uint8_t> newBitmap(bitmapSize, 0);

		// Copy the existing target bitmap
		std::memcpy(newBitmap.data(), targetHeader.activity_bitmap,
					std::min(bitmapSize, static_cast<uint32_t>(targetHeader.bitmap_size)));

		uint32_t targetNextIndex = targetHeader.used;
		uint32_t newInactiveCount = targetHeader.inactive_count;

		// Track entities being moved for reference updates
		std::vector<std::pair<uint64_t, uint64_t>> movedEntities; // <oldId, newId>

		// Copy active items
		for (uint32_t i = 0; i < sourceHeader.used; i++) {
			// Check if this item is active
			bool isActive = bitmap::getBit(sourceHeader.activity_bitmap, i);

			if (isActive) {
				uint64_t oldId = 0;
				uint64_t newId = targetHeader.start_id + targetNextIndex;

				// Read, update ID, and write entity based on type
				if (type == toUnderlying(SegmentType::Node)) {
					Node node = segmentTracker_->readEntity<Node>(sourceOffset, i, itemSize);
					oldId = node.getId();
					node.setId(newId);
					segmentTracker_->writeEntity(targetOffset, targetNextIndex, node, itemSize);

					// Track for reference updates
					movedEntities.emplace_back(oldId, newId);
				} else if (type == toUnderlying(SegmentType::Edge)) {
					Edge edge = segmentTracker_->readEntity<Edge>(sourceOffset, i, itemSize);
					oldId = edge.getId();
					edge.setId(newId);
					segmentTracker_->writeEntity(targetOffset, targetNextIndex, edge, itemSize);

					// Track for reference updates
					movedEntities.emplace_back(oldId, newId);
				} else if (type == toUnderlying(SegmentType::Property)) {
					Property property = segmentTracker_->readEntity<Property>(sourceOffset, i, itemSize);
					oldId = property.getId();
					property.setId(newId);
					segmentTracker_->writeEntity(targetOffset, targetNextIndex, property, itemSize);

					// Track for reference updates
					movedEntities.emplace_back(oldId, newId);
				} else if (type == toUnderlying(SegmentType::Blob)) {
					Blob blob = segmentTracker_->readEntity<Blob>(sourceOffset, i, itemSize);
					oldId = blob.getId();
					blob.setId(newId);
					segmentTracker_->writeEntity(targetOffset, targetNextIndex, blob, itemSize);

					// Track for reference updates
					movedEntities.emplace_back(oldId, newId);
				}

				// Mark as active in the new bitmap
				bitmap::setBit(newBitmap.data(), targetNextIndex, true);

				// Update index
				targetNextIndex++;
			}
		}

		// Update target header with new values
		targetHeader.used = targetNextIndex;
		targetHeader.inactive_count = newInactiveCount;
		targetHeader.bitmap_size = bitmapSize;

		// Copy the new bitmap to the header
		std::memcpy(targetHeader.activity_bitmap, newBitmap.data(), bitmapSize);

		// Write updated header to disk
		segmentTracker_->writeSegmentHeader(targetOffset, targetHeader);

		// Update segment chain references
		updateSegmentChain(targetOffset, sourceHeader);

		// Update entity references based on type
		for (const auto &[oldId, newId]: movedEntities) {
			if (type == toUnderlying(SegmentType::Node)) {
				// Logic to update node references would go here
			} else if (type == toUnderlying(SegmentType::Edge)) {
				// Logic to update edge references would go here
			} else if (type == toUnderlying(SegmentType::Property)) {
				// Logic to update property references would go here
			}
		}

		// Mark source segment as free
		segmentTracker_->markSegmentFree(sourceOffset);

		return true;
	}

	bool SpaceManager::copySegmentData(uint64_t sourceOffset, uint64_t destinationOffset, const SegmentHeader &header) {
		// Copy data in chunks to avoid large memory allocations
		constexpr size_t COPY_BLOCK_SIZE = 64 * 1024; // 64KB
		std::vector<char> buffer(COPY_BLOCK_SIZE);

		for (size_t offset = 0; offset < TOTAL_SEGMENT_SIZE; offset += COPY_BLOCK_SIZE) {
			size_t copySize = std::min(COPY_BLOCK_SIZE, TOTAL_SEGMENT_SIZE - offset);

			// Read from source
			file_->seekg(static_cast<std::streamoff>(sourceOffset + offset));
			file_->read(buffer.data(), copySize);

			// Write to destination
			file_->seekp(static_cast<std::streamoff>(destinationOffset + offset));
			file_->write(buffer.data(), copySize);
		}

		// Update the segment header at the destination
		// Copy the header but update its location information
		SegmentHeader newHeader = header;
		newHeader.file_offset = destinationOffset; // Update memory-only field

		// Re-register with tracker at the new location
		segmentTracker_->registerSegment(destinationOffset, header.data_type, header.capacity);
		segmentTracker_->updateSegmentUsage(destinationOffset, header.used, header.inactive_count);

		// Copy the bitmap
		std::vector<bool> activityBitmap = segmentTracker_->getActivityBitmap(sourceOffset);
		segmentTracker_->updateActivityBitmap(destinationOffset, activityBitmap);

		return true;
	}

	void SpaceManager::updateSegmentChain(uint64_t newOffset, const SegmentHeader &header) {
		// Update prev segment to point to the new segment
		if (header.prev_segment_offset != 0) {
			SegmentHeader prevHeader = segmentTracker_->getSegmentHeader(header.prev_segment_offset);
			if (prevHeader.file_offset != newOffset) {
				segmentTracker_->updateSegmentLinks(prevHeader.file_offset, prevHeader.prev_segment_offset, newOffset);
			} else {
				segmentTracker_->updateSegmentLinks(prevHeader.file_offset, prevHeader.prev_segment_offset,
													header.next_segment_offset);
			}
		} else {
			// This is the head of the chain
			segmentTracker_->updateChainHead(header.data_type, newOffset);
		}

		// Update next segment to point to the new segment
		if (header.next_segment_offset != 0) {
			SegmentHeader nextHeader = segmentTracker_->getSegmentHeader(header.next_segment_offset);
			if (nextHeader.file_offset != newOffset) {
				segmentTracker_->updateSegmentLinks(nextHeader.file_offset, newOffset, nextHeader.next_segment_offset);
			} else {
				segmentTracker_->updateSegmentLinks(nextHeader.file_offset, header.prev_segment_offset,
													nextHeader.next_segment_offset);
			}
		}
	}

	void SpaceManager::processAllEmptySegments() {
		// Remove empty node segments
		processEmptySegments(toUnderlying(SegmentType::Node));

		// Remove empty edge segments
		processEmptySegments(toUnderlying(SegmentType::Edge));

		// Remove empty property segments
		processEmptySegments(toUnderlying(SegmentType::Property));

		// Remove empty blob segments
		processEmptySegments(toUnderlying(SegmentType::Blob));
	}

	bool SpaceManager::processEmptySegments(uint32_t type) {
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

	bool SpaceManager::relocateSegmentsFromEnd() {
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

		// Consider segments in the last 20% of the file as candidates
		uint64_t thresholdOffset = fileSize - (fileSize / 5);

		thresholdOffset = (thresholdOffset / TOTAL_SEGMENT_SIZE) * TOTAL_SEGMENT_SIZE;

		// Check all segment types
		for (int type = 0; type <= 3; type++) { // Node, Edge, Property, Blob
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
		for (int type = 0; type <= 3; type++) {
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

	bool SpaceManager::truncateFile() {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Find segments at the end of file that can be truncated
		auto truncatableSegments = findTruncatableSegments();

		if (truncatableSegments.empty()) {
			return false; // Nothing to truncate
		}

		// Sort segments by offset (ascending)
		std::sort(truncatableSegments.begin(), truncatableSegments.end());

		// Find the new file size (start of the first truncatable segment)
		uint64_t newFileSize = truncatableSegments.front();

		// Ensure we never truncate below the FileHeader
		if (newFileSize < FILE_HEADER_SIZE) {
			newFileSize = FILE_HEADER_SIZE;
		}

		// Close and reopen the file to ensure all writes are flushed
		file_->flush();

		// Use standard resize method to truncate the file
		try {
			// Must close file before truncating on some platforms
			// Get the filename from the file stream

			// Truncate using filesystem library (C++17)
			std::filesystem::resize_file(fileName_, newFileSize);

			// Remove truncated segments from free list
			for (uint64_t offset: truncatableSegments) {
				segmentTracker_->removeFromFreeList(offset);
			}

			return true;
		} catch (const std::exception &e) {
			std::cerr << "Error truncating file: " << e.what() << std::endl;
			return false;
		}
	}

	std::vector<uint64_t> SpaceManager::findTruncatableSegments() const {
		std::vector<uint64_t> result;

		// Get all free segments
		auto freeSegments = segmentTracker_->getFreeSegments();

		// Sort by offset (ascending)
		std::sort(freeSegments.begin(), freeSegments.end());

		// Calculate current file size
		uint64_t fileSize = calculateCurrentFileSize();

		// Find the highest used offset in any chain
		uint64_t highestActiveOffset = 0;

		for (int type = 0; type <= 3; type++) {
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
