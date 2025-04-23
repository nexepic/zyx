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
#include <iostream>
#include <unordered_set>

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

	uint64_t SpaceManager::allocateSegment(uint8_t type, uint32_t capacity) {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Calculate segment size based on type
		size_t segmentSize;
		switch (type) {
			case 0: // Node
				segmentSize = sizeof(SegmentHeader) + capacity * sizeof(Node);
				break;
			case 1: // Edge
				segmentSize = sizeof(SegmentHeader) + capacity * sizeof(Edge);
				break;
			case 2: // Property
				segmentSize = sizeof(PropertySegmentHeader) + capacity;
				break;
			case 3: // Blob
				segmentSize = BLOB_SECTION_SIZE;
				break;
			default:
				throw std::runtime_error("Invalid segment type");
		}

		// Round to next segment size if needed
		segmentSize = TOTAL_SEGMENT_SIZE;

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

		// Initialize the appropriate header
		if (type == 0 || type == 1 || type == 3) { // Node, Edge, or Blob
			SegmentHeader header;
			header.capacity = capacity;
			header.used = 0;
			header.data_type = type;
			header.next_segment_offset = 0;
			header.prev_segment_offset = 0;
			header.segment_crc = 0;

			// Start ID calculation for nodes and edges
			if (type == 0) { // Node
				// Find max node ID
				uint64_t maxId = 0;
				uint64_t nodeHead = tracker_->getChainHead(0);
				uint64_t current = nodeHead;
				while (current != 0) {
					SegmentInfo info = tracker_->getSegmentInfo(current);
					maxId = std::max(maxId, info.offset + info.capacity);
					current = info.nextSegment;
				}
				header.start_id = maxId + 1;
			} else if (type == 1) { // Edge
				// Find max edge ID
				uint64_t maxId = 0;
				uint64_t edgeHead = tracker_->getChainHead(1);
				uint64_t current = edgeHead;
				while (current != 0) {
					SegmentInfo info = tracker_->getSegmentInfo(current);
					maxId = std::max(maxId, info.offset + info.capacity);
					current = info.nextSegment;
				}
				header.start_id = maxId + 1;
			}

			// Write header
			file_->seekp(static_cast<std::streamoff>(offset));
			file_->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));
		} else if (type == 2) { // Property
			PropertySegmentHeader header;
			header.capacity = capacity;
			header.used = 0;
			header.segment_crc = 0;
			header.next_segment_offset = 0;
			header.prev_segment_offset = 0;

			// Write header
			file_->seekp(static_cast<std::streamoff>(offset));
			file_->write(reinterpret_cast<const char *>(&header), sizeof(PropertySegmentHeader));
		}

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
				SegmentInfo info = tracker_->getSegmentInfo(current);
				prev = current;
				current = info.nextSegment;
			}

			// Link the new segment to the end of chain
			if (prev != 0) {
				tracker_->updateSegmentLinks(offset, prev, 0);
				tracker_->updateSegmentLinks(prev, prev, offset);
			}
		}

		return offset;
	}

	void SpaceManager::deallocateSegment(uint64_t offset) {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Get segment info before it's removed
		SegmentInfo info = tracker_->getSegmentInfo(offset);

		// Update chain links
		uint64_t prevOffset = info.prevSegment;
		uint64_t nextOffset = info.nextSegment;

		if (prevOffset != 0) {
			tracker_->updateSegmentLinks(prevOffset, info.prevSegment, nextOffset);
		} else {
			// This is the head of the chain
			tracker_->updateChainHead(info.type, nextOffset);
		}

		if (nextOffset != 0) {
			tracker_->updateSegmentLinks(nextOffset, prevOffset, info.nextSegment);
		}

		// Mark segment as free
		tracker_->markSegmentFree(offset);
	}

	void SpaceManager::compactSegments(uint8_t type, double threshold) {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Get segments that need compaction based on total free space, not just inactive elements
		auto segments = tracker_->getSegmentsNeedingCompaction(type, threshold);

		// Limit to top 5 segments with highest fragmentation
		if (segments.size() > 5) {
			std::sort(segments.begin(), segments.end(), [](const SegmentInfo &a, const SegmentInfo &b) {
				return a.getFragmentationRatio() > b.getFragmentationRatio();
			});
			segments.resize(5);
		}

		for (const auto &segmentInfo: segments) {
			switch (type) {
				case 0: // Node
					compactNodeSegment(segmentInfo.offset);
					break;
				case 1: // Edge
					compactEdgeSegment(segmentInfo.offset);
					break;
				case 2: // Property
					compactPropertySegment(segmentInfo.offset);
					break;
				default:;
			}
		}
	}

	bool SpaceManager::moveSegment(uint64_t sourceOffset, uint64_t destinationOffset) {
		// std::lock_guard<std::mutex> lock(mutex_);

		if (sourceOffset == destinationOffset) {
			return true; // Nothing to do
		}

		try {
			// Get source segment info
			SegmentInfo sourceInfo = tracker_->getSegmentInfo(sourceOffset);

			// Copy the segment data
			if (!copySegmentData(sourceOffset, destinationOffset, sourceInfo)) {
				return false;
			}

			// Update chain links
			updateSegmentChain(destinationOffset, sourceInfo);

			// If this is the head of a chain, update the chain head
			if (sourceInfo.prevSegment == 0) {
				tracker_->updateChainHead(sourceInfo.type, destinationOffset);
			}

			// Update all references to this segment
			if (referenceUpdateCallback_) {
				referenceUpdateCallback_(sourceOffset, destinationOffset, sourceInfo.type);
			} else {
				// If no callback is set, do standard reference updates
				updateSegmentReferences(sourceOffset, destinationOffset, sourceInfo.type);
			}

			// Mark old segment as free
			tracker_->markSegmentFree(sourceOffset);

			return true;
		} catch (const std::exception &e) {
			std::cerr << "Error moving segment: " << e.what() << std::endl;
			return false;
		}
	}

	// Add this new method for standard reference updates
	void SpaceManager::updateSegmentReferences(uint64_t oldOffset, uint64_t newOffset, uint8_t type) {
		// Handle different segment types
		switch (type) {
			case 0: // Node segment
				updateNodeSegmentReferences(oldOffset, newOffset);
				break;
			case 1: // Edge segment
				updateEdgeSegmentReferences(oldOffset, newOffset);
				break;
			case 2: // Property segment
				updatePropertySegmentReferences(oldOffset, newOffset);
				break;
			case 3: // Blob segment
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
				updatePropertyReferencesToEntity(nodeId, 0, oldOffset, newOffset);
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
				updatePropertyReferencesToEntity(edgeId, 1, oldOffset, newOffset);
			}
		}
	}

	void SpaceManager::updatePropertySegmentReferences(uint64_t oldOffset, uint64_t newOffset) {
		// Read property segment header
		PropertySegmentHeader header;
		file_->seekg(static_cast<std::streamoff>(oldOffset));
		file_->read(reinterpret_cast<char *>(&header), sizeof(PropertySegmentHeader));

		// Scan through property entries and update references
		uint64_t current = oldOffset + sizeof(PropertySegmentHeader);
		uint32_t bytesProcessed = 0;

		while (bytesProcessed < header.used) {
			// Read property entry header
			PropertyEntryHeader entryHeader;
			file_->seekg(static_cast<std::streamoff>(current));
			file_->read(reinterpret_cast<char *>(&entryHeader), sizeof(PropertyEntryHeader));

			// Calculate relative offset in the segment
			uint64_t relativeOffset = current - oldOffset;
			uint64_t newEntryOffset = newOffset + relativeOffset;

			// Update entity's reference to this property
			uint64_t entityId = entryHeader.entity_id;
			uint8_t entityType = entryHeader.entity_type;

			updatePropertyReference(entityId, entityType, oldOffset, newOffset, current, newEntryOffset);

			// Move to next property entry
			uint32_t entrySize = sizeof(PropertyEntryHeader) + entryHeader.data_size;
			current += entrySize;
			bytesProcessed += entrySize;
		}
	}

	void SpaceManager::updateEdgeReferencesToNode(uint64_t nodeId, uint64_t oldNodeSegment, uint64_t newNodeSegment) {
		// This would scan edge segments and update any reference to this node
		// Simplified implementation - in reality would need to know edge structure

		// For each edge segment
		uint64_t edgeSegment = tracker_->getChainHead(1);
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
		// Scan property segments for entries referencing this entity
		uint64_t propSegment = tracker_->getChainHead(2);

		while (propSegment != 0) {
			PropertySegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(propSegment));
			file_->read(reinterpret_cast<char *>(&header), sizeof(PropertySegmentHeader));

			// Scan properties in this segment
			uint64_t current = propSegment + sizeof(PropertySegmentHeader);
			uint32_t bytesProcessed = 0;

			while (bytesProcessed < header.used) {
				// Read property entry header
				PropertyEntryHeader entryHeader;
				file_->seekg(static_cast<std::streamoff>(current));
				file_->read(reinterpret_cast<char *>(&entryHeader), sizeof(PropertyEntryHeader));

				// Check if this property references our entity
				if (entryHeader.entity_id == entityId && entryHeader.entity_type == entityType) {
					// Update entity segment reference
					if (entryHeader.entity_offset == oldSegment) {
						entryHeader.entity_offset = newSegment;

						// Write back updated header
						file_->seekp(static_cast<std::streamoff>(current));
						file_->write(reinterpret_cast<const char *>(&entryHeader), sizeof(PropertyEntryHeader));
					}
				}

				// Move to next property
				uint32_t entrySize = sizeof(PropertyEntryHeader) + entryHeader.data_size;
				current += entrySize;
				bytesProcessed += entrySize;
			}

			propSegment = header.next_segment_offset;
		}
	}

	void SpaceManager::setReferenceUpdateCallback(ReferenceUpdateCallback callback) {
		referenceUpdateCallback_ = std::move(callback);
	}

	double SpaceManager::getTotalFragmentationRatio() const {
		double nodeRatio = tracker_->calculateFragmentationRatio(0);
		double edgeRatio = tracker_->calculateFragmentationRatio(1);
		double propertyRatio = tracker_->calculateFragmentationRatio(2);

		// Weight by segment counts
		auto nodeSegments = tracker_->getSegmentsByType(0);
		auto edgeSegments = tracker_->getSegmentsByType(1);
		auto propSegments = tracker_->getSegmentsByType(2);

		double totalWeight = static_cast<double>(nodeSegments.size()) + static_cast<double>(edgeSegments.size()) +
							 static_cast<double>(propSegments.size());

		if (totalWeight == 0) {
			return 0.0;
		}

		auto ratio = (nodeRatio * static_cast<double>(nodeSegments.size()) +
					  edgeRatio * static_cast<double>(edgeSegments.size()) +
					  propertyRatio * static_cast<double>(propSegments.size())) /
					 totalWeight;

		return ratio;
	}

	void SpaceManager::compactSegments() {
		// Remove completely empty segments
		removeAllEmptySegments();

		// Compact individual segments with high fragmentation
		compactSegments(0, COMPACTION_THRESHOLD); // Nodes
		compactSegments(1, COMPACTION_THRESHOLD); // Edges
		compactSegments(2, COMPACTION_THRESHOLD); // Properties

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
		SegmentInfo info = tracker_->getSegmentInfo(offset);

		// If inactive count is zero, nothing to do
		if (info.getFragmentationRatio() == 0) {
			return false;
		}

		// Read the segment header
		SegmentHeader header = readSegmentHeader(offset);

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

		writeSegmentHeader(offset, header);

		// Update segment usage
		tracker_->updateSegmentUsage(offset, nextFreeSpot, 0);

		return true;
	}

	bool SpaceManager::compactEdgeSegment(uint64_t offset) {
		SegmentInfo info = tracker_->getSegmentInfo(offset);

		// If inactive count is zero, nothing to do
		if (info.getFragmentationRatio() == 0) {
			return false;
		}

		// Read the segment header
		SegmentHeader header = readSegmentHeader(offset);

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

		writeSegmentHeader(offset, header);

		// Update segment usage
		tracker_->updateSegmentUsage(offset, nextFreeSpot, 0);

		return true;
	}

	bool SpaceManager::compactPropertySegment(uint64_t offset) {
		SegmentInfo info = tracker_->getSegmentInfo(offset);

		// If inactive count is zero, nothing to do
		if (info.inactive == 0) {
			return true;
		}

		// Read the property segment header
		PropertySegmentHeader header;
		file_->seekg(static_cast<std::streamoff>(offset));
		file_->read(reinterpret_cast<char *>(&header), sizeof(PropertySegmentHeader));

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
		uint64_t srcPos = offset + sizeof(PropertySegmentHeader);
		uint32_t nextFreeSpot = 0;
		uint8_t newBitmap[MAX_BITMAP_SIZE] = {};
		std::vector<char> propertyBuffer;

		for (uint32_t i = 0; i < header.used; i++) {
			// Check if this property is active
			if (bitmap::getBit(header.activity_bitmap, i)) {
				if (i != nextFreeSpot) {
					uint32_t entrySize = sizeof(PropertyEntryHeader) + header.bitmap_size;
					propertyBuffer.resize(entrySize);

					// Read property
					file_->seekg(static_cast<std::streamoff>(srcPos + i * entrySize));
					file_->read(propertyBuffer.data(), entrySize);

					// Write to the free spot
					file_->seekp(static_cast<std::streamoff>(srcPos + nextFreeSpot * entrySize));
					file_->write(propertyBuffer.data(), entrySize);
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

		writeSegmentHeader(offset, header);

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

		// Initialize appropriate header
		if (type == 0 || type == 1 || type == 3) { // Node, Edge, or Blob
			SegmentHeader header;
			header.capacity = capacity;
			header.used = 0;
			header.data_type = type;
			header.next_segment_offset = 0;
			header.prev_segment_offset = 0;
			header.segment_crc = 0;
			header.inactive_count = 0;
			header.bitmap_size = bitmap::calculateBitmapSize(capacity);

			// Start ID calculation for nodes and edges is done elsewhere

			// Write header
			file_->seekp(static_cast<std::streamoff>(offset));
			file_->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));
		} else if (type == 2) { // Property
			PropertySegmentHeader header;
			header.capacity = capacity;
			header.used = 0;
			header.segment_crc = 0;
			header.next_segment_offset = 0;
			header.prev_segment_offset = 0;
			header.inactive_count = 0;
			header.bitmap_size = bitmap::calculateBitmapSize(capacity);

			// Write header
			file_->seekp(static_cast<std::streamoff>(offset));
			file_->write(reinterpret_cast<const char *>(&header), sizeof(PropertySegmentHeader));
		}

		// Register with tracker
		tracker_->registerSegment(offset, type, capacity);
	}

	uint64_t SpaceManager::findSegmentForNodeId(int64_t id) const {
		// Start from the node segment head
		uint64_t segmentOffset = tracker_->getChainHead(0);

		while (segmentOffset != 0) {
			SegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(segmentOffset));
			file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

			// Check if the ID falls within this segment's range
			if (id >= header.start_id && id < header.start_id + header.capacity) {
				return segmentOffset;
			}

			segmentOffset = header.next_segment_offset;
		}

		return 0; // Not found
	}

	uint64_t SpaceManager::findSegmentForEdgeId(int64_t id) const {
		// Start from the edge segment head
		uint64_t segmentOffset = tracker_->getChainHead(1);

		while (segmentOffset != 0) {
			SegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(segmentOffset));
			file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

			// Check if the ID falls within this segment's range
			if (id >= header.start_id && id < header.start_id + header.capacity) {
				return segmentOffset;
			}

			segmentOffset = header.next_segment_offset;
		}

		return 0; // Not found
	}

	void SpaceManager::updateMaxIds() {
		fileHeaderManager_->updateFileHeaderMaxIds(tracker_);
		// auto maxNodeId = fileHeaderManager_->getMaxNodeId();
		// auto maxEdgeId = fileHeaderManager_->getMaxEdgeId();
		// idAllocator_->updateMaxIds(maxNodeId, maxEdgeId);
	}

	SegmentHeader SpaceManager::readSegmentHeader(uint64_t offset) const {
		SegmentHeader header;
		file_->seekg(static_cast<std::streamoff>(offset));
		file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		if (!*file_) {
			throw std::runtime_error("Failed to read segment header at offset " + std::to_string(offset));
		}

		return header;
	}

	template<typename HeaderType>
	void SpaceManager::writeSegmentHeader(uint64_t offset, const HeaderType &header) {
		file_->seekp(static_cast<std::streamoff>(offset));
		file_->write(reinterpret_cast<const char *>(&header), sizeof(HeaderType));

		if (!*file_) {
			throw std::runtime_error("Failed to write segment header at offset " + std::to_string(offset));
		}
	}

	void SpaceManager::updatePropertyReference(uint64_t entityId, uint8_t entityType, uint64_t oldSegmentOffset,
											   uint64_t newSegmentOffset, uint64_t oldEntryOffset,
											   uint64_t newEntryOffset) {
		if (entityType == 0) { // Node
			// Find the segment containing this node
			uint64_t nodeSegmentOffset = tracker_->getSegmentOffsetForNodeId(entityId);
			if (nodeSegmentOffset != 0) {
				// Read segment header
				SegmentHeader header;
				file_->seekg(static_cast<std::streamoff>(nodeSegmentOffset));
				file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

				// Calculate node offset within segment
				uint32_t nodeIndex = static_cast<uint32_t>(entityId - header.start_id);
				uint64_t nodeOffset = nodeSegmentOffset + sizeof(SegmentHeader) + nodeIndex * sizeof(Node);

				// Read node
				Node node;
				file_->seekg(static_cast<std::streamoff>(nodeOffset));
				file_->read(reinterpret_cast<char *>(&node), sizeof(Node));

				// // Update property reference
				// PropertyReference &ref = node.getPropertyReferenceRef();
				// if (ref.segment_offset == oldSegmentOffset) {
				// 	ref.segment_offset = newSegmentOffset;
				// 	ref.property_offset = newEntryOffset;
				// }

				// Write updated node
				file_->seekp(static_cast<std::streamoff>(nodeOffset));
				file_->write(reinterpret_cast<const char *>(&node), sizeof(Node));
			}
		} else if (entityType == 1) { // Edge
			// Find the segment containing this edge
			uint64_t edgeSegmentOffset = tracker_->getSegmentOffsetForEdgeId(entityId);
			if (edgeSegmentOffset != 0) {
				// Read segment header
				SegmentHeader header;
				file_->seekg(static_cast<std::streamoff>(edgeSegmentOffset));
				file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

				// Calculate edge offset within segment
				uint32_t edgeIndex = static_cast<uint32_t>(entityId - header.start_id);
				uint64_t edgeOffset = edgeSegmentOffset + sizeof(SegmentHeader) + edgeIndex * sizeof(Edge);

				// Read edge
				Edge edge;
				file_->seekg(static_cast<std::streamoff>(edgeOffset));
				file_->read(reinterpret_cast<char *>(&edge), sizeof(Edge));

				// // Update property reference
				// PropertyReference &ref = edge.getPropertyReferenceRef();
				// if (ref.segment_offset == oldSegmentOffset) {
				// 	ref.segment_offset = newSegmentOffset;
				// 	ref.property_offset = newEntryOffset;
				// }

				// Write updated edge
				file_->seekp(static_cast<std::streamoff>(edgeOffset));
				file_->write(reinterpret_cast<const char *>(&edge), sizeof(Edge));
			}
		}
	}

	std::vector<uint64_t> SpaceManager::findCandidatesForMerge(uint8_t type, double usageThreshold) {
		std::vector<uint64_t> candidates;

		// Get all segments of the specified type
		auto segments = tracker_->getSegmentsByType(type);

		// Filter segments with low usage
		for (const auto &info: segments) {
			double usageRatio = static_cast<double>(info.used - info.inactive) / info.capacity;
			if (usageRatio < usageThreshold) {
				candidates.push_back(info.offset);
			}
		}

		return candidates;
	}

	bool SpaceManager::mergeSegments(uint8_t type, double usageThreshold) {
		// std::lock_guard<std::mutex> lock(mutex_);

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
			SegmentInfo infoA = tracker_->getSegmentInfo(a);
			SegmentInfo infoB = tracker_->getSegmentInfo(b);

			double usageA = static_cast<double>(infoA.used - infoA.inactive) / infoA.capacity;
			double usageB = static_cast<double>(infoB.used - infoB.inactive) / infoB.capacity;

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
			SegmentInfo infoA = tracker_->getSegmentInfo(a);
			SegmentInfo infoB = tracker_->getSegmentInfo(b);

			uint32_t freeSpaceA = infoA.capacity - (infoA.used - infoA.inactive);
			uint32_t freeSpaceB = infoB.capacity - (infoB.used - infoB.inactive);

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

			SegmentInfo sourceInfo = tracker_->getSegmentInfo(sourceOffset);
			uint32_t activeItemsSource = sourceInfo.used - sourceInfo.inactive;

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

				SegmentInfo targetInfo = tracker_->getSegmentInfo(targetOffset);
				uint32_t activeItemsTarget = targetInfo.used - targetInfo.inactive;

				// Check if source can fit into target
				if (activeItemsSource + activeItemsTarget <= targetInfo.capacity) {
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

					SegmentInfo targetInfo = tracker_->getSegmentInfo(targetOffset);
					uint32_t activeItemsTarget = targetInfo.used - targetInfo.inactive;

					if (activeItemsSource + activeItemsTarget <= targetInfo.capacity) {
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
			SegmentInfo infoA = tracker_->getSegmentInfo(a);
			SegmentInfo infoB = tracker_->getSegmentInfo(b);

			double usageA = static_cast<double>(infoA.used - infoA.inactive) / infoA.capacity;
			double usageB = static_cast<double>(infoB.used - infoB.inactive) / infoB.capacity;

			return usageA < usageB;
		});

		// Try to consolidate remaining front segments
		for (size_t i = 0; i < remainingFrontSegments.size(); i++) {
			uint64_t sourceOffset = remainingFrontSegments[i];

			if (mergedSegments.count(sourceOffset) > 0) {
				continue; // Skip if already processed
			}

			SegmentInfo sourceInfo = tracker_->getSegmentInfo(sourceOffset);
			uint32_t activeItemsSource = sourceInfo.used - sourceInfo.inactive;

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

				SegmentInfo targetInfo = tracker_->getSegmentInfo(targetOffset);
				uint32_t activeItemsTarget = targetInfo.used - targetInfo.inactive;

				if (activeItemsSource + activeItemsTarget <= targetInfo.capacity) {
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
		// Read the headers
		SegmentHeader targetHeader, sourceHeader;

		if (type == 0 || type == 1) { // Node or Edge
			file_->seekg(static_cast<std::streamoff>(targetOffset));
			file_->read(reinterpret_cast<char *>(&targetHeader), sizeof(SegmentHeader));

			file_->seekg(static_cast<std::streamoff>(sourceOffset));
			file_->read(reinterpret_cast<char *>(&sourceHeader), sizeof(SegmentHeader));
		} else if (type == 2) { // Property
			// Handle property segments differently
			return false; // Placeholder
		} else {
			return false; // Unsupported type
		}

		// Calculate active items and available space
		uint32_t targetActiveItems = targetHeader.used - targetHeader.inactive_count;
		uint32_t sourceActiveItems = sourceHeader.used - sourceHeader.inactive_count;

		// Ensure there's enough space
		if (targetActiveItems + sourceActiveItems > targetHeader.capacity) {
			return false;
		}

		// Copy active items from source to target
		size_t itemSize = (type == 0) ? sizeof(Node) : sizeof(Edge);
		uint32_t targetNextIndex = targetActiveItems; // Write after active items in target

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

				// Write to target
				file_->seekp(
						static_cast<std::streamoff>(targetOffset + sizeof(SegmentHeader) + targetNextIndex * itemSize));
				file_->write(buffer.data(), itemSize);

				// Mark as active in target bitmap
				bitmap::setBit(targetHeader.activity_bitmap, targetNextIndex, true);

				// Update index
				targetNextIndex++;
			}
		}

		// Update target header
		targetHeader.used = targetNextIndex;
		targetHeader.inactive_count = 0; // All items in the target are now active

		// Write updated header
		file_->seekp(static_cast<std::streamoff>(targetOffset));
		file_->write(reinterpret_cast<const char *>(&targetHeader), sizeof(SegmentHeader));

		// Update segment info in tracker
		tracker_->updateSegmentUsage(targetOffset, targetHeader.used, 0);

		// Remove source segment from the chain
		updateSegmentChain(targetOffset, tracker_->getSegmentInfo(sourceOffset));

		// Mark source segment as free
		tracker_->markSegmentFree(sourceOffset);

		return true;
	}

	bool SpaceManager::copySegmentData(uint64_t sourceOffset, uint64_t destinationOffset, const SegmentInfo &info) {
		size_t segmentSize;
		switch (info.type) {
			case 0: // Node
				segmentSize = sizeof(SegmentHeader) + info.capacity * sizeof(Node);
				break;
			case 1: // Edge
				segmentSize = sizeof(SegmentHeader) + info.capacity * sizeof(Edge);
				break;
			case 2: // Property
				segmentSize = sizeof(PropertySegmentHeader) + info.capacity;
				break;
			case 3: // Blob
				segmentSize = BLOB_SECTION_SIZE;
				break;
			default:
				return false;
		}

		// Round to segment size
		segmentSize = ((segmentSize + TOTAL_SEGMENT_SIZE - 1) / TOTAL_SEGMENT_SIZE) * TOTAL_SEGMENT_SIZE;

		// Copy data in chunks to avoid large memory allocations
		constexpr size_t COPY_BLOCK_SIZE = 64 * 1024; // 64KB
		std::vector<char> buffer(COPY_BLOCK_SIZE);

		for (size_t offset = 0; offset < segmentSize; offset += COPY_BLOCK_SIZE) {
			size_t copySize = std::min(COPY_BLOCK_SIZE, segmentSize - offset);

			// Read from source
			file_->seekg(static_cast<std::streamoff>(sourceOffset + offset));
			file_->read(buffer.data(), copySize);

			// Write to destination
			file_->seekp(static_cast<std::streamoff>(destinationOffset + offset));
			file_->write(buffer.data(), copySize);
		}

		// Update the segment header to reflect the new location
		if (info.type == 0 || info.type == 1 || info.type == 3) { // Node, Edge, or Blob
			SegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(destinationOffset));
			file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

			// No need to update header.start_id as it remains the same
			// Links will be updated by updateSegmentChain

			// Re-register with tracker
			tracker_->registerSegment(destinationOffset, info.type, header.capacity);
			tracker_->updateSegmentUsage(destinationOffset, header.used, info.inactive);
		} else if (info.type == 2) { // Property
			PropertySegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(destinationOffset));
			file_->read(reinterpret_cast<char *>(&header), sizeof(PropertySegmentHeader));

			// Links will be updated by updateSegmentChain

			// Re-register with tracker
			tracker_->registerSegment(destinationOffset, 2, header.capacity);
			tracker_->updateSegmentUsage(destinationOffset, header.used, info.inactive);
		}

		return true;
	}

	void SpaceManager::updateSegmentChain(uint64_t newOffset, const SegmentInfo &info) {
		// Update links in the new segment to point to the correct prev/next
		tracker_->updateSegmentLinks(newOffset, info.prevSegment, info.nextSegment);

		// Update prev segment to point to new segment
		if (info.prevSegment != 0) {
			SegmentInfo prevInfo = tracker_->getSegmentInfo(info.prevSegment);
			tracker_->updateSegmentLinks(info.prevSegment, prevInfo.prevSegment, newOffset);
		}

		// Update next segment to point to new segment
		if (info.nextSegment != 0) {
			SegmentInfo nextInfo = tracker_->getSegmentInfo(info.nextSegment);
			tracker_->updateSegmentLinks(info.nextSegment, newOffset, nextInfo.nextSegment);
		}
	}

	void SpaceManager::removeAllEmptySegments() {
		// Remove empty node segments
		removeEmptySegments(0);

		// Remove empty edge segments
		removeEmptySegments(1);

		// Remove empty property segments
		removeEmptySegments(2);
	}

	bool SpaceManager::removeEmptySegments(uint8_t type) {
		// std::lock_guard<std::mutex> lock(mutex_);
		bool removedAny = false;

		// Get all segments of the specified type
		auto segments = tracker_->getSegmentsByType(type);

		// Find segments with no active entities
		std::vector<uint64_t> emptySegments;
		for (const auto &info: segments) {
			uint32_t activeCount = info.used - info.inactive;
			if (activeCount == 0) {
				emptySegments.push_back(info.offset);
			}
		}

		// Remove each empty segment
		for (uint64_t segmentOffset: emptySegments) {
			// Get segment info before it's removed
			SegmentInfo info = tracker_->getSegmentInfo(segmentOffset);

			// Update chain links
			uint64_t prevOffset = info.prevSegment;
			uint64_t nextOffset = info.nextSegment;

			if (prevOffset != 0) {
				tracker_->updateSegmentLinks(prevOffset, tracker_->getSegmentInfo(prevOffset).prevSegment, nextOffset);
			} else {
				// This is the head of the chain
				tracker_->updateChainHead(type, nextOffset);
			}

			if (nextOffset != 0) {
				tracker_->updateSegmentLinks(nextOffset, prevOffset, tracker_->getSegmentInfo(nextOffset).nextSegment);
			}

			// Mark segment as free
			tracker_->markSegmentFree(segmentOffset);
			removedAny = true;
		}

		return removedAny;
	}

	bool SpaceManager::relocateSegmentsFromEnd() {
		// std::lock_guard<std::mutex> lock(mutex_);

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
			for (const auto &info: segments) {
				if (info.offset >= thresholdOffset) {
					result.push_back(info.offset);
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
			for (const auto &info: segments) {
				// Calculate the end of this segment
				uint64_t segmentSize;
				switch (info.type) {
					case 0: // Node
						segmentSize = sizeof(SegmentHeader) + info.capacity * sizeof(Node);
						break;
					case 1: // Edge
						segmentSize = sizeof(SegmentHeader) + info.capacity * sizeof(Edge);
						break;
					case 2: // Property
						segmentSize = sizeof(PropertySegmentHeader) + info.capacity;
						break;
					case 3: // Blob
						segmentSize = BLOB_SECTION_SIZE;
						break;
					default:
						segmentSize = SEGMENT_SIZE;
				}
				// Round up to segment size
				segmentSize = ((segmentSize + TOTAL_SEGMENT_SIZE - 1) / TOTAL_SEGMENT_SIZE) * TOTAL_SEGMENT_SIZE;

				uint64_t endOffset = info.offset + segmentSize;
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
			for (const auto &info: segments) {
				uint64_t segmentEnd = info.offset;
				switch (info.type) {
					case 0: // Node
						segmentEnd += sizeof(SegmentHeader) + info.capacity * sizeof(Node);
						break;
					case 1: // Edge
						segmentEnd += sizeof(SegmentHeader) + info.capacity * sizeof(Edge);
						break;
					case 2: // Property
						segmentEnd += sizeof(PropertySegmentHeader) + info.capacity;
						break;
					case 3: // Blob
						segmentEnd += BLOB_SECTION_SIZE;
						break;
				}
				highestActiveOffset = std::max(highestActiveOffset, segmentEnd);
			}
		}

		// Round to segment size
		highestActiveOffset =
				((highestActiveOffset + TOTAL_SEGMENT_SIZE - 1) / TOTAL_SEGMENT_SIZE) * TOTAL_SEGMENT_SIZE;

		// Find contiguous free segments at the end of file
		for (uint64_t offset: freeSegments) {
			if (offset >= highestActiveOffset) {
				result.push_back(offset);
			}
		}

		return result;
	}

} // namespace graph::storage
