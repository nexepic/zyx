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
#include <iostream>
#include <unordered_set>
#include "graph/storage/SegmentType.h"

namespace graph::storage {

	SpaceManager::SpaceManager(std::shared_ptr<std::fstream> file, std::string fileName,
							   std::shared_ptr<SegmentTracker> tracker,
							   std::shared_ptr<FileHeaderManager> fileHeaderManager,
							   std::shared_ptr<IDAllocator> idAllocator) :
		file_(file), fileName_(fileName), tracker_(tracker), fileHeaderManager_(fileHeaderManager),
		idAllocator_(idAllocator) {}

	SpaceManager::~SpaceManager() = default;

	void SpaceManager::initialize(FileHeader &header) {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Initialize tracker with header
		tracker_->initialize(header);
	}

	uint64_t SpaceManager::findMaxId(uint8_t type, std::shared_ptr<SegmentTracker> &tracker) {
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

	uint64_t SpaceManager::allocateSegment(uint8_t type, uint32_t capacity) {
		// Calculate segment size based on type
		size_t segmentSize = TOTAL_SEGMENT_SIZE; // Use a consistent segment size for all types

		// Check if we have free segments that can be reused
		auto freeSegments = tracker_->getFreeSegments();
		uint64_t offset = 0;

		if (!freeSegments.empty()) {
			offset = freeSegments.front();
			// Remove from free list
			tracker_->removeFromFreeList(offset); // This removes it from the free list
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
			header.start_id = findMaxId(type, tracker_) + 1;
		} else {
			throw std::invalid_argument("Invalid segment type");
		}

		// Write header
		file_->seekp(static_cast<std::streamoff>(offset));
		file_->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));

		// Register with tracker
		tracker_->registerSegment(offset, type, capacity);

		// Link with chain
		uint64_t chainHead = tracker_->getChainHead(type);
		if (chainHead == 0) {
			// This is the first segment of this type
			tracker_->updateChainHead(type, offset);
		} else {
			// Find the last segment in chain
			uint64_t current = chainHead;
			uint64_t prev = 0;
			while (current != 0) {
				SegmentHeader header = tracker_->getSegmentHeader(current);
				prev = current;
				current = header.next_segment_offset;
			}

			// Link the new segment to the end of chain
			if (prev != 0) {
				tracker_->updateSegmentLinks(offset, prev, 0);
				SegmentHeader prevHeader = tracker_->getSegmentHeader(prev);
				tracker_->updateSegmentLinks(prev, prevHeader.prev_segment_offset, offset);
			}
		}

		return offset;
	}

	void SpaceManager::deallocateSegment(uint64_t offset) {
		// Get segment info before it's removed
		SegmentHeader header = tracker_->getSegmentHeader(offset);

		// Update chain links
		uint64_t prevOffset = header.prev_segment_offset;
		uint64_t nextOffset = header.next_segment_offset;

		if (prevOffset != 0) {
			SegmentHeader prevHeader = tracker_->getSegmentHeader(prevOffset);
			tracker_->updateSegmentLinks(prevOffset, prevHeader.prev_segment_offset, nextOffset);
		} else {
			// This is the head of the chain
			tracker_->updateChainHead(header.data_type, nextOffset);
		}

		if (nextOffset != 0) {
			SegmentHeader nextHeader = tracker_->getSegmentHeader(nextOffset);
			tracker_->updateSegmentLinks(nextOffset, prevOffset, nextHeader.next_segment_offset);
		}

		// Mark segment as free
		tracker_->markSegmentFree(offset);
	}

	void SpaceManager::compactSegments(uint8_t type, double threshold) {
		// Get segments that need compaction based on total free space, not just inactive elements
		auto segments = tracker_->getSegmentsNeedingCompaction(type, threshold);

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
			// Get source segment info
			SegmentHeader sourceHeader = tracker_->getSegmentHeader(sourceOffset);

			// Copy the segment data
			if (!copySegmentData(sourceOffset, destinationOffset, sourceHeader)) {
				return false;
			}

			// Update chain links
			updateSegmentChain(destinationOffset, sourceHeader);

			// If this is the head of a chain, update the chain head
			if (sourceHeader.prev_segment_offset == 0) {
				tracker_->updateChainHead(sourceHeader.data_type, destinationOffset);
			}

			// Update all references to this segment
			if (referenceUpdateCallback_) {
				referenceUpdateCallback_(sourceOffset, destinationOffset, sourceHeader.data_type);
			} else {
				// If no callback is set, do standard reference updates
				updateSegmentReferences(sourceOffset, destinationOffset, sourceHeader.data_type);
			}

			// Mark old segment as free
			tracker_->markSegmentFree(sourceOffset);

			return true;
		} catch (const std::exception &e) {
			std::cerr << "Error moving segment: " << e.what() << std::endl;
			return false;
		}
	}

	void SpaceManager::updateSegmentReferences(uint64_t oldOffset, uint64_t newOffset, uint8_t type) {
		// Handle different segment types
		switch (type) {
			case toUnderlying(SegmentType::Node):
				updateNodeSegmentReferences(oldOffset, newOffset);
				break;
			case toUnderlying(SegmentType::Edge):
				updateEdgeSegmentReferences(oldOffset, newOffset);
				break;
			case toUnderlying(SegmentType::Property):
				updatePropertySegmentReferences(oldOffset, newOffset);
				break;
			case toUnderlying(SegmentType::Blob):
				// Special handling for blob references if needed
				break;
		}
	}

	void SpaceManager::updateNodeSegmentReferences(uint64_t oldOffset, uint64_t newOffset) {
		// Read segment headers to get ID ranges
		SegmentHeader oldHeader = readSegmentHeader(oldOffset);
		SegmentHeader newHeader = readSegmentHeader(newOffset);

		uint64_t startId = oldHeader.start_id;
		uint64_t endId = startId + oldHeader.capacity;

		// For each active node in the segment, update references in edges and properties
		for (uint32_t i = 0; i < oldHeader.used; i++) {
			if (bitmap::getBit(oldHeader.activity_bitmap, i)) {
				uint64_t nodeId = startId + i;

				// Update references in edges (would need to scan edge segments)
				updateEdgeReferencesToNode(nodeId, oldOffset, newOffset);

				// Update references in properties
				updatePropertyReferencesToEntity(nodeId, toUnderlying(SegmentType::Node), oldOffset, newOffset);
			}
		}
	}

	void SpaceManager::updateEdgeSegmentReferences(uint64_t oldOffset, uint64_t newOffset) {
		// Read segment headers to get ID ranges
		SegmentHeader oldHeader = readSegmentHeader(oldOffset);
		SegmentHeader newHeader = readSegmentHeader(newOffset);

		uint64_t startId = oldHeader.start_id;
		uint64_t endId = startId + oldHeader.capacity;

		// For each active edge in the segment, update references in properties
		for (uint32_t i = 0; i < oldHeader.used; i++) {
			if (bitmap::getBit(oldHeader.activity_bitmap, i)) {
				uint64_t edgeId = startId + i;

				// Update references in properties
				updatePropertyReferencesToEntity(edgeId, toUnderlying(SegmentType::Edge), oldOffset, newOffset);
			}
		}
	}

	void SpaceManager::updatePropertySegmentReferences(uint64_t oldOffset, uint64_t newOffset) {
		// Read property segment header
		SegmentHeader header = tracker_->getSegmentHeader(oldOffset);

		// Since properties are now treated as entities, we handle them like nodes/edges
		// Each property has a fixed size within the segment
		size_t propertySize = sizeof(Property); // Assuming Property is your property entity class
		uint32_t activeCount = header.getActiveCount();

		// Process all active properties in the segment
		for (uint32_t i = 0; i < header.used; i++) {
			if (bitmap::getBit(header.activity_bitmap, i)) {
				// Calculate property offset
				uint64_t propertyOffset = oldOffset + sizeof(SegmentHeader) + i * propertySize;
				uint64_t newPropertyOffset = newOffset + sizeof(SegmentHeader) + i * propertySize;

				// Read property entity
				Property property;
				file_->seekg(static_cast<std::streamoff>(propertyOffset));
				file_->read(reinterpret_cast<char *>(&property), propertySize);

				// Get the entity this property belongs to
				int64_t entityId = property.getEntityId();
				uint8_t entityType = property.getEntityType();

				// Update references to this property in the owning entity
				updatePropertyReference(entityId, entityType, oldOffset, newOffset, propertyOffset, newPropertyOffset);
			}
		}
	}

	void SpaceManager::updateEdgeReferencesToNode(uint64_t nodeId, uint64_t oldNodeSegment, uint64_t newNodeSegment) {
		// This would scan edge segments and update any reference to this node
		// Simplified implementation - in reality would need to know edge structure

		// For each edge segment
		uint64_t edgeSegment = tracker_->getChainHead(toUnderlying(SegmentType::Edge));
		while (edgeSegment != 0) {
			SegmentHeader header = readSegmentHeader(edgeSegment);

			// For each edge in the segment
			for (uint32_t i = 0; i < header.used; i++) {
				if (bitmap::getBit(header.activity_bitmap, i)) {
					// Read edge
					Edge edge;
					file_->seekg(static_cast<std::streamoff>(edgeSegment + sizeof(SegmentHeader) + i * sizeof(Edge)));
					file_->read(reinterpret_cast<char *>(&edge), sizeof(Edge));

					// Assuming Edge has source_id and target_id fields:
					bool updated = false;

					// Check if edge references this node
					// Note: This is pseudo-code - adjust to your Edge structure
					/*
					if (edge.source_id == nodeId || edge.target_id == nodeId) {
						// Update edge segment reference if needed
						if (edge.node_segment_reference == oldNodeSegment) {
							edge.node_segment_reference = newNodeSegment;
							updated = true;
						}
					}

					// Write back if updated
					if (updated) {
						file_->seekp(static_cast<std::streamoff>(edgeSegment + sizeof(SegmentHeader) + i *
					sizeof(Edge))); file_->write(reinterpret_cast<const char *>(&edge), sizeof(Edge));
					}
					*/
				}
			}

			edgeSegment = header.next_segment_offset;
		}
	}

	void SpaceManager::updatePropertyReferencesToEntity(int64_t entityId, uint8_t entityType, uint64_t oldSegment,
														uint64_t newSegment) {
		// Scan property segments for properties referencing this entity
		uint64_t propSegment = tracker_->getChainHead(toUnderlying(SegmentType::Property));
		size_t propertySize = sizeof(Property); // Assuming Property is your property entity class

		while (propSegment != 0) {
			SegmentHeader header = tracker_->getSegmentHeader(propSegment);

			// For each property in the segment
			for (uint32_t i = 0; i < header.used; i++) {
				if (bitmap::getBit(header.activity_bitmap, i)) {
					// Calculate property offset
					uint64_t propertyOffset = propSegment + sizeof(SegmentHeader) + i * propertySize;

					// Read property
					Property property;
					file_->seekg(static_cast<std::streamoff>(propertyOffset));
					file_->read(reinterpret_cast<char *>(&property), propertySize);

					// Check if this property references our entity
					if (property.getEntityId() == entityId && property.getEntityType() == entityType) {
						// Get the segment offset for this entity
						uint64_t entitySegmentOffset = 0;
						if (entityType == toUnderlying(SegmentType::Node)) {
							entitySegmentOffset = tracker_->getSegmentOffsetForNodeId(entityId);
						} else if (entityType == toUnderlying(SegmentType::Edge)) {
							entitySegmentOffset = tracker_->getSegmentOffsetForEdgeId(entityId);
						}

						// Check if the entity segment offset matches the old segment
						if (entitySegmentOffset == oldSegment) {
							// Update the property to point to the new segment
							property.setEntityInfo(entityId, entityType);

							// Write back the updated property
							file_->seekp(static_cast<std::streamoff>(propertyOffset));
							file_->write(reinterpret_cast<const char *>(&property), propertySize);
						}
					}
				}
			}

			propSegment = header.next_segment_offset;
		}
	}

	void SpaceManager::setReferenceUpdateCallback(ReferenceUpdateCallback callback) {
		referenceUpdateCallback_ = std::move(callback);
	}

	double SpaceManager::getTotalFragmentationRatio() const {
		double nodeRatio = tracker_->calculateFragmentationRatio(toUnderlying(SegmentType::Node));
		double edgeRatio = tracker_->calculateFragmentationRatio(toUnderlying(SegmentType::Edge));
		double propertyRatio = tracker_->calculateFragmentationRatio(toUnderlying(SegmentType::Property));
		double blobRatio = tracker_->calculateFragmentationRatio(toUnderlying(SegmentType::Blob));

		// Weight by segment counts
		auto nodeSegments = tracker_->getSegmentsByType(toUnderlying(SegmentType::Node));
		auto edgeSegments = tracker_->getSegmentsByType(toUnderlying(SegmentType::Edge));
		auto propSegments = tracker_->getSegmentsByType(toUnderlying(SegmentType::Property));
		auto blobSegments = tracker_->getSegmentsByType(toUnderlying(SegmentType::Blob));

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
		removeAllEmptySegments();

		// Compact individual segments with high fragmentation
		compactSegments(toUnderlying(SegmentType::Node), COMPACTION_THRESHOLD);
		compactSegments(toUnderlying(SegmentType::Edge), COMPACTION_THRESHOLD);
		compactSegments(toUnderlying(SegmentType::Property), COMPACTION_THRESHOLD);
		compactSegments(toUnderlying(SegmentType::Blob), COMPACTION_THRESHOLD);

		// Try to merge segments with low utilization
		// mergeSegments(0, COMPACTION_THRESHOLD); // Nodes
		// mergeSegments(1, COMPACTION_THRESHOLD); // Edges

		// Relocate segments from end of file to fill gaps
		// relocateSegmentsFromEnd();

		updateMaxIds();
	}

	bool SpaceManager::shouldCompact() const {
		// Only use fragmentation ratio based on COMPACTION_THRESHOLD
		double fragRatio = getTotalFragmentationRatio();
		return fragRatio > COMPACTION_THRESHOLD;
	}

	bool SpaceManager::compactNodeSegment(uint64_t offset) {
		SegmentHeader header = tracker_->getSegmentHeader(offset);

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
		size_t nodeSize = sizeof(Node);
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

		tracker_->writeSegmentHeader(offset, header);

		// Update segment usage
		tracker_->updateSegmentUsage(offset, nextFreeSpot, 0);

		return true;
	}

	bool SpaceManager::compactEdgeSegment(uint64_t offset) {
		SegmentHeader header = tracker_->getSegmentHeader(offset);

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
		size_t edgeSize = sizeof(Edge);
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

		tracker_->writeSegmentHeader(offset, header);

		// Update segment usage
		tracker_->updateSegmentUsage(offset, nextFreeSpot, 0);

		return true;
	}

	bool SpaceManager::compactPropertySegment(uint64_t offset) {
		SegmentHeader header = tracker_->getSegmentHeader(offset);

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
		size_t propertySize = sizeof(Property); // Use the entity-based Property size
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

		tracker_->writeSegmentHeader(offset, header);

		// Update segment usage
		tracker_->updateSegmentUsage(offset, nextFreeSpot, 0);

		return true;
	}

	bool SpaceManager::compactBlobSegment(uint64_t offset) {
		SegmentHeader header = tracker_->getSegmentHeader(offset);

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
		size_t blobSize = sizeof(Blob); // Assuming Blob is the structure for blobs
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

		tracker_->writeSegmentHeader(offset, header);

		// Update segment usage
		tracker_->updateSegmentUsage(offset, nextFreeSpot, 0);

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
		auto freeSegments = tracker_->getFreeSegments();
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
	void SpaceManager::initializeSegmentHeader(uint64_t offset, uint8_t type, uint32_t capacity) {
		// Remove from free list if it's there
		tracker_->removeFromFreeList(offset);

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
		tracker_->registerSegment(offset, type, capacity);
	}

	uint64_t SpaceManager::findSegmentForNodeId(int64_t id) const { return tracker_->getSegmentOffsetForNodeId(id); }

	uint64_t SpaceManager::findSegmentForEdgeId(int64_t id) const {
		// Use the tracker's built-in method for this operation
		return tracker_->getSegmentOffsetForEdgeId(id);
	}

	void SpaceManager::updateMaxIds() {
		fileHeaderManager_->updateFileHeaderMaxIds(tracker_);
		// auto maxNodeId = fileHeaderManager_->getMaxNodeId();
		// auto maxEdgeId = fileHeaderManager_->getMaxEdgeId();
		// idAllocator_->updateMaxIds(maxNodeId, maxEdgeId);
	}

	SegmentHeader SpaceManager::readSegmentHeader(uint64_t offset) const {
		// Directly use the tracker's method to read segment header
		return tracker_->getSegmentHeader(offset);
	}

	void SpaceManager::writeSegmentHeader(uint64_t offset, const SegmentHeader &header) {
		// Use tracker's method to write segment header
		tracker_->writeSegmentHeader(offset, header);
	}

	void SpaceManager::updatePropertyReference(uint64_t entityId, uint8_t entityType, uint64_t oldSegmentOffset,
											   uint64_t newSegmentOffset, uint64_t oldPropertyOffset,
											   uint64_t newPropertyOffset) {
		if (entityType == toUnderlying(SegmentType::Node)) {
			// Find the segment containing this node
			uint64_t nodeSegmentOffset = tracker_->getSegmentOffsetForNodeId(entityId);
			if (nodeSegmentOffset != 0) {
				// Read segment header directly from tracker
				SegmentHeader header = tracker_->getSegmentHeader(nodeSegmentOffset);

				// Calculate node offset within segment
				uint32_t nodeIndex = static_cast<uint32_t>(entityId - header.start_id);
				uint64_t nodeOffset = nodeSegmentOffset + sizeof(SegmentHeader) + nodeIndex * sizeof(Node);

				// Read node
				Node node;
				file_->seekg(static_cast<std::streamoff>(nodeOffset));
				file_->read(reinterpret_cast<char *>(&node), sizeof(Node));

				// Update property reference in the node
				if (node.getPropertyEntityId() == oldPropertyOffset) {
					node.setPropertyEntityId(newPropertyOffset);
				}

				// Write updated node
				file_->seekp(static_cast<std::streamoff>(nodeOffset));
				file_->write(reinterpret_cast<const char *>(&node), sizeof(Node));
			}
		} else if (entityType == toUnderlying(SegmentType::Edge)) {
			// Find the segment containing this edge
			uint64_t edgeSegmentOffset = tracker_->getSegmentOffsetForEdgeId(entityId);
			if (edgeSegmentOffset != 0) {
				// Read segment header directly from tracker
				SegmentHeader header = tracker_->getSegmentHeader(edgeSegmentOffset);

				// Calculate edge offset within segment
				uint32_t edgeIndex = static_cast<uint32_t>(entityId - header.start_id);
				uint64_t edgeOffset = edgeSegmentOffset + sizeof(SegmentHeader) + edgeIndex * sizeof(Edge);

				// Read edge
				Edge edge;
				file_->seekg(static_cast<std::streamoff>(edgeOffset));
				file_->read(reinterpret_cast<char *>(&edge), sizeof(Edge));

				// Update property reference in the edge
				if (edge.getPropertyEntityId() == oldPropertyOffset) {
					edge.setPropertyEntityId(newPropertyOffset);
				}

				// Write updated edge
				file_->seekp(static_cast<std::streamoff>(edgeOffset));
				file_->write(reinterpret_cast<const char *>(&edge), sizeof(Edge));
			}
		}
		// Handling for other entity types if needed
	}

	std::vector<uint64_t> SpaceManager::findCandidatesForMerge(uint8_t type, double usageThreshold) {
		std::vector<uint64_t> candidates;

		// Get all segments of the specified type
		auto segments = tracker_->getSegmentsByType(type);

		// Filter segments with low usage
		for (const auto &header: segments) {
			double usageRatio = static_cast<double>(header.getActiveCount()) / header.capacity;
			if (usageRatio < usageThreshold) {
				candidates.push_back(header.file_offset);
			}
		}

		return candidates;
	}

	bool SpaceManager::mergeSegments(uint8_t type, double usageThreshold) {
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
			SegmentHeader headerA = tracker_->getSegmentHeader(a);
			SegmentHeader headerB = tracker_->getSegmentHeader(b);

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
			SegmentHeader headerA = tracker_->getSegmentHeader(a);
			SegmentHeader headerB = tracker_->getSegmentHeader(b);

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

			SegmentHeader sourceHeader = tracker_->getSegmentHeader(sourceOffset);
			uint32_t activeItemsSource = sourceHeader.getActiveCount();

			// Skip empty segments - they can just be marked as free later
			if (activeItemsSource == 0) {
				tracker_->markSegmentFree(sourceOffset);
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

				SegmentHeader targetHeader = tracker_->getSegmentHeader(targetOffset);
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

					SegmentHeader targetHeader = tracker_->getSegmentHeader(targetOffset);
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
			SegmentHeader headerA = tracker_->getSegmentHeader(a);
			SegmentHeader headerB = tracker_->getSegmentHeader(b);

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

			SegmentHeader sourceHeader = tracker_->getSegmentHeader(sourceOffset);
			uint32_t activeItemsSource = sourceHeader.getActiveCount();

			// Skip empty segments
			if (activeItemsSource == 0) {
				tracker_->markSegmentFree(sourceOffset);
				mergedSegments.insert(sourceOffset);
				mergedAny = true;
				continue;
			}

			// Try to merge with another front segment
			bool merged = false;
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

				SegmentHeader targetHeader = tracker_->getSegmentHeader(targetOffset);
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

		// Clean up any merged segments that weren't properly freed
		for (uint64_t offset: mergedSegments) {
			// Verify segment is actually in the free list
			auto freeSegments = tracker_->getFreeSegments();
			if (std::find(freeSegments.begin(), freeSegments.end(), offset) == freeSegments.end()) {
				tracker_->markSegmentFree(offset);
			}
		}

		return mergedAny;
	}

	bool SpaceManager::mergeIntoSegment(uint64_t targetOffset, uint64_t sourceOffset, uint8_t type) {
		// Get headers directly from tracker
		SegmentHeader targetHeader = tracker_->getSegmentHeader(targetOffset);
		SegmentHeader sourceHeader = tracker_->getSegmentHeader(sourceOffset);

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
				itemSize = sizeof(Node);
				break;
			case toUnderlying(SegmentType::Edge):
				itemSize = sizeof(Edge);
				break;
			case toUnderlying(SegmentType::Property):
				itemSize = sizeof(Property);
				break;
			case toUnderlying(SegmentType::Blob):
				itemSize = sizeof(Blob);
				break;
			default:
				return false;
		}

		uint32_t targetNextIndex = targetHeader.used; // Write after the last used item in target

		// Buffer for copying items
		std::vector<char> buffer(itemSize);

		// Copy active items
		for (uint32_t i = 0; i < sourceHeader.used; i++) {
			// Check if this item is active
			bool isActive = bitmap::getBit(sourceHeader.activity_bitmap, i);

			if (isActive) {
				// Read from source
				file_->seekg(static_cast<std::streamoff>(sourceOffset + sizeof(SegmentHeader) + i * itemSize));
				file_->read(buffer.data(), itemSize);

				// Update ID for the item being moved if needed (implementation depends on Node/Edge structure)
				if (type == toUnderlying(SegmentType::Node)) {
					Node *node = reinterpret_cast<Node *>(buffer.data());
					node->setId(targetHeader.start_id + targetNextIndex);
				} else if (type == toUnderlying(SegmentType::Edge)) {
					Edge *edge = reinterpret_cast<Edge *>(buffer.data());
					edge->setId(targetHeader.start_id + targetNextIndex);
				}
				// TODO: Handle property references if needed
				// For property segments, might need additional updates (property offset, etc.)

				// Write to target
				file_->seekp(
						static_cast<std::streamoff>(targetOffset + sizeof(SegmentHeader) + targetNextIndex * itemSize));
				file_->write(buffer.data(), itemSize);

				// Mark as active in target bitmap
				tracker_->setEntityActive(targetOffset, targetNextIndex, true);

				// Update index
				targetNextIndex++;
			}
		}

		// Update target header
		targetHeader.used = targetNextIndex;

		// Write updated header
		tracker_->writeSegmentHeader(targetOffset, targetHeader);

		// Remove source segment from the chain
		updateSegmentChain(targetOffset, sourceHeader);

		// Mark source segment as free
		tracker_->markSegmentFree(sourceOffset);

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
		tracker_->registerSegment(destinationOffset, header.data_type, header.capacity);
		tracker_->updateSegmentUsage(destinationOffset, header.used, header.inactive_count);

		// Copy the bitmap
		std::vector<bool> activityBitmap = tracker_->getActivityBitmap(sourceOffset);
		tracker_->updateActivityBitmap(destinationOffset, activityBitmap);

		return true;
	}

	void SpaceManager::updateSegmentChain(uint64_t newOffset, const SegmentHeader &header) {
		// Update links in the new segment to point to the correct prev/next
		tracker_->updateSegmentLinks(newOffset, header.prev_segment_offset, header.next_segment_offset);

		// Update prev segment to point to new segment
		if (header.prev_segment_offset != 0) {
			tracker_->updateSegmentLinks(header.prev_segment_offset,
										 tracker_->getSegmentHeader(header.prev_segment_offset).prev_segment_offset,
										 newOffset);
		}

		// Update next segment to point to new segment
		if (header.next_segment_offset != 0) {
			tracker_->updateSegmentLinks(header.next_segment_offset, newOffset,
										 tracker_->getSegmentHeader(header.next_segment_offset).next_segment_offset);
		}
	}

	void SpaceManager::removeAllEmptySegments() {
		// Remove empty node segments
		removeEmptySegments(toUnderlying(SegmentType::Node));

		// Remove empty edge segments
		removeEmptySegments(toUnderlying(SegmentType::Edge));

		// Remove empty property segments
		removeEmptySegments(toUnderlying(SegmentType::Property));

		// Remove empty blob segments
		removeEmptySegments(toUnderlying(SegmentType::Blob));
	}

	bool SpaceManager::removeEmptySegments(uint8_t type) {
		bool removedAny = false;

		// Get all segments of the specified type
		auto segments = tracker_->getSegmentsByType(type);

		// Find segments with no active entities
		std::vector<uint64_t> emptySegments;
		for (const auto &header: segments) {
			if (header.getActiveCount() == 0) {
				emptySegments.push_back(header.file_offset);
			}
		}

		// Remove each empty segment
		for (uint64_t segmentOffset: emptySegments) {
			// Deallocate using our existing method
			deallocateSegment(segmentOffset);
			removedAny = true;
		}

		return removedAny;
	}

	bool SpaceManager::relocateSegmentsFromEnd() {
		// Get free segments that we can use as targets
		auto freeSegments = tracker_->getFreeSegments();
		if (freeSegments.empty()) {
			return false; // No free slots to fill
		}

		// Sort free segments by offset (ascending)
		std::sort(freeSegments.begin(), freeSegments.end());

		// Find segments near the end that can be relocated
		auto relocatableSegments = findRelocatableSegments();
		if (relocatableSegments.empty()) {
			return false; // No segments to relocate
		}

		// Sort relocatable segments by offset (descending) to process from end of file
		std::sort(relocatableSegments.begin(), relocatableSegments.end(), std::greater<uint64_t>());

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
			auto segments = tracker_->getSegmentsByType(type);
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
			auto segments = tracker_->getSegmentsByType(type);
			for (const auto &header: segments) {
				// Calculate the end of this segment
				uint64_t endOffset = header.file_offset + TOTAL_SEGMENT_SIZE;
				maxOffset = std::max(maxOffset, endOffset);
			}
		}

		// Also check free segments
		auto freeSegments = tracker_->getFreeSegments();
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
			// This requires storing the filename when opening the file
			// file_->close();

			// Truncate using filesystem library (C++17)
			std::filesystem::resize_file(fileName_, newFileSize);

			// Remove truncated segments from free list
			for (uint64_t offset: truncatableSegments) {
				tracker_->removeFromFreeList(offset);
			}

			return true;

			// // Reopen the file
			// file_->open(fileName_, std::ios::in | std::ios::out | std::ios::binary);
			// if (!file_->is_open()) {
			// 	return false; // Failed to reopen
			// }
			//
			// // Update file size tracking
			// fileSize_ = newFileSize;
			//
			// // Remove truncated segments from free list
			// for (uint64_t offset: truncatableSegments) {
			// 	auto it = std::find(tracker_->getFreeSegments().begin(), tracker_->getFreeSegments().end(), offset);
			// 	if (it != tracker_->getFreeSegments().end()) {
			// 		// Use a method in tracker to remove from free list
			// 		tracker_->removeFromFreeList(offset);
			// 	}
			// }
			//
			// return true;
		} catch (const std::exception &e) {
			std::cerr << "Error truncating file: " << e.what() << std::endl;
			return false;
		}
	}

	std::vector<uint64_t> SpaceManager::findTruncatableSegments() const {
		std::vector<uint64_t> result;

		// Get all free segments
		auto freeSegments = tracker_->getFreeSegments();

		// Sort by offset (ascending)
		std::sort(freeSegments.begin(), freeSegments.end());

		// Calculate current file size
		uint64_t fileSize = calculateCurrentFileSize();

		// Find the highest used offset in any chain
		uint64_t highestActiveOffset = 0;

		for (int type = 0; type <= 3; type++) {
			auto segments = tracker_->getSegmentsByType(type);
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
