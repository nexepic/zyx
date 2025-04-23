/**
 * @file DeletionManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/1
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/DeletionManager.h"
#include <chrono>
#include <iostream>
#include "graph/storage/DataManager.h"
#include "graph/storage/FileStorage.h"

namespace graph::storage {

	DeletionManager::DeletionManager(std::shared_ptr<std::fstream> file, FileStorage &storage, DataManager &dataManager,
									 std::shared_ptr<SpaceManager> spaceManager) :
		file_(file), storage_(storage), dataManager_(dataManager), spaceManager_(spaceManager) {

		// Register callback for reference updates
		spaceManager_->setReferenceUpdateCallback([this](uint64_t oldOffset, uint64_t newOffset, uint8_t type) {
			this->handleReferenceUpdate(oldOffset, newOffset, type);
		});
	}

	DeletionManager::~DeletionManager() = default;

	void DeletionManager::initialize() {
		// Refresh segment state to initialize tracking
		refreshSegmentState();
	}

	void DeletionManager::refreshSegmentState() {
		// Update the inactive count for each segment based on actual data
		auto nodeSegments = spaceManager_->getTracker()->getSegmentsByType(0);
		for (const auto &info: nodeSegments) {
			// Read segment header
			SegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(info.offset));
			file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

			// Create activity map
			std::vector<bool> activityMap(header.used);

			// Scan nodes in segment to determine active/inactive status
			for (uint32_t i = 0; i < header.used; ++i) {
				uint64_t nodeId = header.start_id + i;
				try {
					Node node = dataManager_.getNode(nodeId);
					activityMap[i] = node.isActive();
				} catch (const std::exception &) {
					// Node not found or error - consider it inactive
					activityMap[i] = false;
				}
			}

			// Update segment bitmap
			spaceManager_->getTracker()->updateActivityBitmap(info.offset, activityMap);
		}

		// Do the same for edge segments
		auto edgeSegments = spaceManager_->getTracker()->getSegmentsByType(1);
		for (const auto &info: edgeSegments) {
			// Read segment header
			SegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(info.offset));
			file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

			// Create activity map
			std::vector<bool> activityMap(header.used);

			// Scan edges in segment to determine active/inactive status
			for (uint32_t i = 0; i < header.used; ++i) {
				uint64_t edgeId = header.start_id + i;
				try {
					Edge edge = dataManager_.getEdge(edgeId);
					activityMap[i] = edge.isActive();
				} catch (const std::exception &) {
					// Edge not found or error - consider it inactive
					activityMap[i] = false;
				}
			}

			// Update segment bitmap
			spaceManager_->getTracker()->updateActivityBitmap(info.offset, activityMap);
		}
	}

	bool DeletionManager::deleteNode(int64_t nodeId, bool cascadeEdges) {
		// Check if the node exists by trying to retrieve it
		Node node;
		try {
			node = dataManager_.getNode(nodeId);
		} catch (const std::exception &) {
			// Node not found
			return false;
		}

		// If cascading deletion is requested, delete all connected edges
		if (cascadeEdges) {
			// Find edges connected to this node
			auto edges = dataManager_.findEdgesByNode(nodeId, "both");
			for (const auto &edge: edges) {
				deleteEdge(edge.getId());
			}
		}

		// Delete the node properties
		auto properties = dataManager_.getNodeProperties(nodeId);
		if (!properties.empty()) {
			const PropertyReference &ref = node.getPropertyReference();
			if (ref.type != PropertyReference::StorageType::NONE) {
				// Property deletion is handled by marking the node as deleted
				// The property segment compaction will handle freeing space later
			}
		}

		// Mark the node as inactive in the index
		markNodeInactive(node);

		return true;
	}

	bool DeletionManager::deleteEdge(int64_t edgeId) {
		// Check if the edge exists
		Edge edge;
		try {
			edge = dataManager_.getEdge(edgeId);
		} catch (const std::exception &) {
			return false;
		}

		// Delete edge properties
		auto properties = dataManager_.getEdgeProperties(edgeId);
		if (!properties.empty()) {
			const PropertyReference &ref = edge.getPropertyReference();
			if (ref.type != PropertyReference::StorageType::NONE) {
				// Property deletion is handled by marking the edge as deleted
				// The property segment compaction will handle freeing space later
			}
		}

		// Mark as inactive in index
		markEdgeInactive(edge);

		return true;
	}

	uint64_t DeletionManager::findSegmentForNodeId(int64_t id) const {
		// Start from the node segment head
		uint64_t segmentOffset = dataManager_.getFileHeader().node_segment_head;

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

	uint64_t DeletionManager::findSegmentForEdgeId(int64_t id) const {
		// Start from the edge segment head
		uint64_t segmentOffset = dataManager_.getFileHeader().edge_segment_head;

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

	bool DeletionManager::deleteBulkNodes(const std::vector<uint64_t> &nodeIds, bool cascadeEdges) {
		bool allSucceeded = true;

		// For bulk operations, we'll postpone compaction until the end
		for (uint64_t nodeId: nodeIds) {
			// Use a non-compacting version of node deletion
			bool success = performNodeDeletion(nodeId);

			if (success && cascadeEdges) {
				// Find and delete connected edges
				auto edges = dataManager_.findEdgesByNode(nodeId, "both");
				for (const auto &edge: edges) {
					performEdgeDeletion(edge.getId());
				}
			}

			allSucceeded = allSucceeded && success;
		}

		return allSucceeded;
	}

	bool DeletionManager::deleteBulkEdges(const std::vector<uint64_t> &edgeIds) {
		bool allSucceeded = true;

		// For bulk operations, postpone compaction until the end
		for (uint64_t edgeId: edgeIds) {
			// Use non-compacting deletion
			allSucceeded = performEdgeDeletion(edgeId) && allSucceeded;
		}

		return allSucceeded;
	}

	bool DeletionManager::performNodeDeletion(uint64_t nodeId) {
		// Check if the node exists
		Node node;
		try {
			node = dataManager_.getNode(nodeId);
		} catch (const std::exception &) {
			return false;
		}

		// Mark as inactive in index
		markNodeInactive(node);

		return true;
	}

	bool DeletionManager::performEdgeDeletion(uint64_t edgeId) {
		// Check if the edge exists
		Edge edge;
		try {
			edge = dataManager_.getEdge(edgeId);
		} catch (const std::exception &) {
			return false;
		}

		// Mark as inactive in index
		markEdgeInactive(edge);

		return true;
	}

	void DeletionManager::markNodeInactive(Node node) {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Mark node as inactive
		node.markInactive();
		dataManager_.deleteNode(node);

		// Find the segment this node belongs to
		uint64_t segmentOffset = findSegmentForNodeId(node.getId());
		if (segmentOffset == 0) {
			return; // Node not found
		}

		// // Get segment info
		// SegmentInfo info = spaceManager_->getTracker()->getSegmentInfo(segmentOffset);
		//
		// // Read segment header
		// SegmentHeader header;
		// file_->seekg(static_cast<std::streamoff>(segmentOffset));
		// file_->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

		// // Calculate the index within the segment
		// uint32_t indexInSegment = static_cast<uint32_t>(nodeId - header.start_id);
		//
		// // Update the bitmap to mark this node as inactive
		// bitmap::setBit(header.activity_bitmap, indexInSegment, false);
		//
		// // Ensure bitmap size is updated if needed
		// if (header.bitmap_size < bitmap::calculateBitmapSize(indexInSegment + 1)) {
		// 	header.bitmap_size = bitmap::calculateBitmapSize(indexInSegment + 1);
		// }
		//
		// // Update inactive count
		// uint32_t inactiveCount = info.inactive + 1;
		// header.inactive_count = inactiveCount;
		//
		// // Write the updated header back
		// file_->seekp(static_cast<std::streamoff>(segmentOffset));
		// file_->write(reinterpret_cast<const char*>(&header), sizeof(SegmentHeader));

		// // Update segment info with new inactive count
		// spaceManager_->getTracker()->updateSegmentUsage(segmentOffset, header.used, inactiveCount);

		// Check if we need to mark segment for compaction using the actual fragmentation ratio
		SegmentInfo segmentInfo = spaceManager_->getTracker()->getSegmentInfo(segmentOffset);
		if (segmentInfo.getFragmentationRatio() >= COMPACTION_THRESHOLD) {
			spaceManager_->getTracker()->markForCompaction(segmentOffset, true);
		}
	}

	void DeletionManager::markEdgeInactive(Edge edge) {
		// std::lock_guard<std::mutex> lock(mutex_);

		// Mark edge as inactive
		edge.markInactive();
		dataManager_.deleteEdge(edge);

		// Find the segment this edge belongs to
		uint64_t segmentOffset = findSegmentForEdgeId(edge.getId());
		if (segmentOffset == 0) {
			return; // Edge not found
		}

		// // Get segment info
		// SegmentInfo info = spaceManager_->getTracker()->getSegmentInfo(segmentOffset);
		//
		// // Read segment header
		// SegmentHeader header;
		// file_->seekg(static_cast<std::streamoff>(segmentOffset));
		// file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));
		//
		// // Calculate the index within the segment
		// uint32_t indexInSegment = static_cast<uint32_t>(edgeId - header.start_id);
		//
		// // Update the bitmap to mark this edge as inactive
		// bitmap::setBit(header.activity_bitmap, indexInSegment, false);
		//
		// // Ensure bitmap size is updated if needed
		// if (header.bitmap_size < bitmap::calculateBitmapSize(indexInSegment + 1)) {
		// 	header.bitmap_size = bitmap::calculateBitmapSize(indexInSegment + 1);
		// }
		//
		// // Update inactive count
		// uint32_t inactiveCount = info.inactive + 1;
		// header.inactive_count = inactiveCount;
		//
		// // Write the updated header back
		// file_->seekp(static_cast<std::streamoff>(segmentOffset));
		// file_->write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));
		//
		// // Update segment info with new inactive count
		// spaceManager_->getTracker()->updateSegmentUsage(segmentOffset, header.used, inactiveCount);

		// Check if we need to mark segment for compaction
		SegmentInfo segmentInfo = spaceManager_->getTracker()->getSegmentInfo(segmentOffset);
		if (segmentInfo.getFragmentationRatio() >= COMPACTION_THRESHOLD) {
			spaceManager_->getTracker()->markForCompaction(segmentOffset, true);
		}
	}

	bool DeletionManager::isNodeActive(uint64_t nodeId) const {
		// Find the segment this node belongs to
		uint64_t segmentOffset = findSegmentForNodeId(nodeId);
		if (segmentOffset == 0) {
			return false; // Node not found
		}

		// Read segment header
		SegmentHeader header;
		file_->seekg(static_cast<std::streamoff>(segmentOffset));
		file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		// Calculate the index within the segment
		uint32_t indexInSegment = static_cast<uint32_t>(nodeId - header.start_id);

		// Check if this node is active using the bitmap
		return spaceManager_->getTracker()->isEntityActive(segmentOffset, indexInSegment);
	}

	bool DeletionManager::isEdgeActive(uint64_t edgeId) const {
		// Find the segment this edge belongs to
		uint64_t segmentOffset = findSegmentForEdgeId(edgeId);
		if (segmentOffset == 0) {
			return false; // Edge not found
		}

		// Read segment header
		SegmentHeader header;
		file_->seekg(static_cast<std::streamoff>(segmentOffset));
		file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		// Calculate the index within the segment
		uint32_t indexInSegment = static_cast<uint32_t>(edgeId - header.start_id);

		// Check if this edge is active using the bitmap
		return spaceManager_->getTracker()->isEntityActive(segmentOffset, indexInSegment);
	}

	std::unordered_map<uint64_t, double> DeletionManager::analyzeSegmentFragmentation(uint8_t entityType) const {
		std::unordered_map<uint64_t, double> result;

		// Get all segments of the specified type
		auto segments = spaceManager_->getTracker()->getSegmentsByType(entityType);

		// Calculate fragmentation ratio for each segment
		for (const auto &info: segments) {
			// Fragmentation ratio is inactive / capacity
			double fragRatio = (info.capacity > 0) ? static_cast<double>(info.inactive) / info.capacity : 0.0;

			result[info.offset] = fragRatio;
		}

		return result;
	}

	void DeletionManager::handleReferenceUpdate(uint64_t oldOffset, uint64_t newOffset, uint8_t type) {
		// This callback is invoked after a segment has moved
		// We need to update references to entities in the moved segment

		if (type == 0) { // Node segment moved
			// Read segment header to get ID range
			SegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(newOffset));
			file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

			// ID range covered by this segment
			uint64_t startId = header.start_id;
			uint64_t endId = startId + header.capacity;

			// For each edge segment, check and update references to nodes in this segment
			auto edgeSegments = spaceManager_->getTracker()->getSegmentsByType(1);
			for (const auto &edgeInfo: edgeSegments) {
				// Read each edge and update source/target references if needed
				uint64_t edgeSegOffset = edgeInfo.offset;
				SegmentHeader edgeHeader;
				file_->seekg(static_cast<std::streamoff>(edgeSegOffset));
				file_->read(reinterpret_cast<char *>(&edgeHeader), sizeof(SegmentHeader));

				// Check edges in this segment
				for (uint32_t i = 0; i < edgeHeader.used; ++i) {
					uint64_t edgeId = edgeHeader.start_id + i;
					uint64_t edgeOffset = edgeSegOffset + sizeof(SegmentHeader) + i * sizeof(Edge);

					// Read edge
					Edge edge;
					file_->seekg(static_cast<std::streamoff>(edgeOffset));
					file_->read(reinterpret_cast<char *>(&edge), sizeof(Edge));

					// // Check if source or target node is in moved segment
					// uint64_t sourceNode = edge.getSourceNodeId();
					// uint64_t targetNode = edge.getTargetNodeId();
					//
					// bool updated = false;
					// if (sourceNode >= startId && sourceNode < endId) {
					//     // Update edge's reference to source node
					//     updated = true;
					// }
					//
					// if (targetNode >= startId && targetNode < endId) {
					//     // Update edge's reference to target node
					//     updated = true;
					// }

					// if (updated) {
					//     // Write updated edge back
					//     file_->seekp(static_cast<std::streamoff>(edgeOffset));
					//     file_->write(reinterpret_cast<const char*>(&edge), sizeof(Edge));
					// }
				}
			}
		} else if (type == 1) { // Edge segment moved
			// Read segment header to get ID range
			SegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(newOffset));
			file_->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

			// ID range covered by this segment
			uint64_t startId = header.start_id;
			uint64_t endId = startId + header.capacity;

			// Update any property references to edges in this segment
			// (would require scanning property segments for references to these edges)
		} else if (type == 2) { // Property segment moved
			// Update entity references to properties in this segment
			PropertySegmentHeader header;
			file_->seekg(static_cast<std::streamoff>(newOffset));
			file_->read(reinterpret_cast<char *>(&header), sizeof(PropertySegmentHeader));

			// This is more complex as we need to scan through property entries
			// to determine which entities reference this segment
		}
	}

} // namespace graph::storage
