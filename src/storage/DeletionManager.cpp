/**
 * @file DeletionManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/1
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/DeletionManager.hpp"
#include <chrono>
#include <iostream>
#include <utility>
#include "graph/storage/DataManager.hpp"
#include "graph/storage/FileStorage.hpp"

namespace graph::storage {

	DeletionManager::DeletionManager(std::shared_ptr<DataManager> dataManager,
									 std::shared_ptr<SpaceManager> spaceManager) :
		dataManager_(std::move(dataManager)), spaceManager_(std::move(spaceManager)) {

		// Register callback for reference updates
		// spaceManager_->setReferenceUpdateCallback([this](uint64_t oldOffset, uint64_t newOffset, uint32_t type) {
		// 	this->handleReferenceUpdate(oldOffset, newOffset, type);
		// });
	}

	DeletionManager::~DeletionManager() = default;

	void DeletionManager::initialize() {
		// Refresh segment state to initialize tracking
		// refreshSegmentState();
	}

	void DeletionManager::deleteNode(Node &node) {
		if (node.hasPropertyEntity()) {
			deletePropertyEntity(node.getPropertyEntityId(), node.getPropertyStorageType());
		}

		deleteNodeConnectedEdges(node.getId());

		// Mark the node as inactive in the index
		markNodeInactive(node);
	}

	void DeletionManager::deleteEdge(Edge &edge) {
		if (edge.hasPropertyEntity()) {
			deletePropertyEntity(edge.getPropertyEntityId(), edge.getPropertyStorageType());
		}

		// Mark the edge as inactive in the index
		markEdgeInactive(edge);
	}

	void DeletionManager::deleteProperty(Property &property) {
		if (property.getId() == 0) return;

		// Mark the property as inactive in the index
		markPropertyInactive(property);
	}

	void DeletionManager::deleteBlob(Blob &blob) {
		if (blob.getId() == 0) return;

		// Mark the blob as inactive in the index
		markBlobInactive(blob);
	}

	uint64_t DeletionManager::findSegmentForNodeId(int64_t id) const {
		// Use the tracker's method instead of reimplementing the logic
		return spaceManager_->getTracker()->getSegmentOffsetForNodeId(id);
	}

	uint64_t DeletionManager::findSegmentForEdgeId(int64_t id) const {
		// Use the tracker's method instead of reimplementing the logic
		return spaceManager_->getTracker()->getSegmentOffsetForEdgeId(id);
	}

	uint64_t DeletionManager::findSegmentForPropertyId(int64_t id) const {
		// Use the tracker's method instead of reimplementing the logic
		return spaceManager_->getTracker()->getSegmentOffsetForPropId(id);
	}

	uint64_t DeletionManager::findSegmentForBlobId(int64_t id) const {
		// Use the tracker's method instead of reimplementing the logic
		return spaceManager_->getTracker()->getSegmentOffsetForBlobId(id);
	}

	bool DeletionManager::deleteBulkNodes(const std::vector<uint64_t> &nodeIds, bool cascadeEdges) {
		bool allSucceeded = true;

		// For bulk operations, we'll postpone compaction until the end
		for (uint64_t nodeId: nodeIds) {
			// Use a non-compacting version of node deletion
			bool success = performNodeDeletion(nodeId);

			if (success && cascadeEdges) {
				// Find and delete connected edges
				auto edges = dataManager_->findEdgesByNode(nodeId, "both");
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
			node = dataManager_->getNode(nodeId);
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
			edge = dataManager_->getEdge(edgeId);
		} catch (const std::exception &) {
			return false;
		}

		// Mark as inactive in index
		markEdgeInactive(edge);

		return true;
	}

	void DeletionManager::deletePropertyEntity(int64_t propertyId, PropertyStorageType storageType) {
		if (propertyId == 0) return;

		try {
			if (storageType == PropertyStorageType::PROPERTY_ENTITY) {
				Property property = dataManager_->getProperty(propertyId);
				if (property.getId() != 0 && property.isActive()) {
					markPropertyInactive(property);
				}
			} else if (storageType == PropertyStorageType::BLOB_ENTITY) {
				Blob blob = dataManager_->getBlob(propertyId);
				if (blob.getId() != 0 && blob.isActive()) {
					markBlobInactive(blob);
				}
			}
		} catch (const std::exception &e) {
			std::cerr << "Failed to delete property entity " << propertyId << ": " << e.what() << "\n";
		}
	}

	void DeletionManager::deleteNodeConnectedEdges(int64_t nodeId) {
		auto edges = dataManager_->findEdgesByNode(nodeId, "both");
		for (auto &edge : edges) {
			markEdgeInactive(edge);
		}
	}

	void DeletionManager::markNodeInactive(Node &node) {
		// Find the segment this node belongs to
		uint64_t segmentOffset = findSegmentForNodeId(node.getId());
		if (segmentOffset == 0) {
			// Node not found in segments, just mark it as deleted in memory
			dataManager_->markEntityDeleted(node);
			return;
		}

		// Get the node's index within the segment
		SegmentHeader header = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
		auto indexInSegment = static_cast<uint32_t>(node.getId() - header.start_id);

		// Mark the node as inactive in the segment bitmap
		spaceManager_->getTracker()->setEntityActive(segmentOffset, indexInSegment, false);

		// Update in-memory state
		dataManager_->markEntityDeleted(node);

		// Check if we need to mark segment for compaction using the actual fragmentation ratio
		SegmentHeader segmentHeader = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
		if (segmentHeader.getFragmentationRatio() >= COMPACTION_THRESHOLD) {
			spaceManager_->getTracker()->markForCompaction(segmentOffset, true);
		}
	}

	void DeletionManager::markEdgeInactive(Edge &edge) {
		// Find the segment this edge belongs to
		uint64_t segmentOffset = findSegmentForEdgeId(edge.getId());
		if (segmentOffset == 0) {
			// Edge not found in segments, just mark it as deleted in memory
			dataManager_->markEntityDeleted(edge);
			return;
		}

		// Get the edge's index within the segment
		SegmentHeader header = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
		auto indexInSegment = static_cast<uint32_t>(edge.getId() - header.start_id);

		// Mark the edge as inactive in the segment bitmap
		spaceManager_->getTracker()->setEntityActive(segmentOffset, indexInSegment, false);

		// Update in-memory state
		dataManager_->markEntityDeleted(edge);

		// Check if we need to mark segment for compaction
		SegmentHeader segmentHeader = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
		if (segmentHeader.getFragmentationRatio() >= COMPACTION_THRESHOLD) {
			spaceManager_->getTracker()->markForCompaction(segmentOffset, true);
		}
	}

	void DeletionManager::markPropertyInactive(Property &property) {
		// Find the segment this property belongs to
		uint64_t segmentOffset = findSegmentForPropertyId(property.getId());
		if (segmentOffset == 0) {
			// Property not found in segments, just mark it as deleted in memory
			dataManager_->markEntityDeleted(property);
			return;
		}

		// Get the property's index within the segment
		SegmentHeader header = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
		auto indexInSegment = static_cast<uint32_t>(property.getId() - header.start_id);

		// Mark the property as inactive in the segment bitmap
		spaceManager_->getTracker()->setEntityActive(segmentOffset, indexInSegment, false);

		// Update in-memory state
		dataManager_->markEntityDeleted(property);

		// Check if we need to mark segment for compaction
		SegmentHeader segmentHeader = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
		if (segmentHeader.getFragmentationRatio() >= COMPACTION_THRESHOLD) {
			spaceManager_->getTracker()->markForCompaction(segmentOffset, true);
		}
	}

	void DeletionManager::markBlobInactive(Blob &blob) {
		// Find the segment this blob belongs to
		uint64_t segmentOffset = findSegmentForBlobId(blob.getId());
		if (segmentOffset == 0) {
			// Blob not found in segments, just mark it as deleted in memory
			dataManager_->markEntityDeleted(blob);
			return;
		}

		// Get the blob's index within the segment
		SegmentHeader header = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
		auto indexInSegment = static_cast<uint32_t>(blob.getId() - header.start_id);

		// Mark the blob as inactive in the segment bitmap
		spaceManager_->getTracker()->setEntityActive(segmentOffset, indexInSegment, false);

		// Update in-memory state
		dataManager_->markEntityDeleted(blob);

		// Check if we need to mark segment for compaction
		SegmentHeader segmentHeader = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
		if (segmentHeader.getFragmentationRatio() >= COMPACTION_THRESHOLD) {
			spaceManager_->getTracker()->markForCompaction(segmentOffset, true);
		}
	}

	bool DeletionManager::isNodeActive(uint64_t nodeId) const {
		// Find the segment this node belongs to
		uint64_t segmentOffset = findSegmentForNodeId(nodeId);
		if (segmentOffset == 0) {
			return false; // Node not found
		}

		// Get segment header from tracker
		SegmentHeader header = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);

		// Calculate the index within the segment
		auto indexInSegment = static_cast<uint32_t>(nodeId - header.start_id);

		// Check if this node is active using the bitmap
		return spaceManager_->getTracker()->isEntityActive(segmentOffset, indexInSegment);
	}

	bool DeletionManager::isEdgeActive(uint64_t edgeId) const {
		// Find the segment this edge belongs to
		uint64_t segmentOffset = findSegmentForEdgeId(edgeId);
		if (segmentOffset == 0) {
			return false; // Edge not found
		}

		// Get segment header from tracker
		SegmentHeader header = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);

		// Calculate the index within the segment
		auto indexInSegment = static_cast<uint32_t>(edgeId - header.start_id);

		// Check if this edge is active using the bitmap
		return spaceManager_->getTracker()->isEntityActive(segmentOffset, indexInSegment);
	}

	std::unordered_map<uint64_t, double> DeletionManager::analyzeSegmentFragmentation(uint32_t entityType) const {
		std::unordered_map<uint64_t, double> result;

		// Get all segments of the specified type
		auto segments = spaceManager_->getTracker()->getSegmentsByType(entityType);

		// Calculate fragmentation ratio for each segment
		for (const auto &header: segments) {
			// Use the getFragmentationRatio method directly
			result[header.file_offset] = header.getFragmentationRatio();
		}

		return result;
	}

	// void DeletionManager::handleReferenceUpdate(uint64_t oldOffset, uint64_t newOffset, uint32_t type) {
	// 	// This callback is invoked after a segment has moved
	// 	// We need to update references to entities in the moved segment
	//
	// 	if (type == 0) { // Node segment moved
	// 		// Read segment header to get ID range
	// 		SegmentHeader header = spaceManager_->getTracker()->getSegmentHeader(newOffset);
	//
	// 		// ID range covered by this segment
	// 		uint64_t startId = header.start_id;
	// 		uint64_t endId = startId + header.capacity;
	//
	// 		// For each edge segment, check and update references to nodes in this segment
	// 		auto edgeSegments = spaceManager_->getTracker()->getSegmentsByType(1);
	// 		for (const auto &edgeHeader: edgeSegments) {
	// 			// Read each edge and update source/target references if needed
	// 			uint64_t edgeSegOffset = edgeHeader.file_offset;
	//
	// 			// Check edges in this segment
	// 			for (uint32_t i = 0; i < edgeHeader.used; ++i) {
	// 				// Only process active edges
	// 				if (spaceManager_->getTracker()->isEntityActive(edgeSegOffset, i)) {
	// 					uint64_t edgeId = edgeHeader.start_id + i;
	// 					uint64_t edgeOffset = edgeSegOffset + sizeof(SegmentHeader) + i * sizeof(Edge);
	//
	// 					// Read edge
	// 					Edge edge;
	// 					file_->seekg(static_cast<std::streamoff>(edgeOffset));
	// 					file_->read(reinterpret_cast<char *>(&edge), sizeof(Edge));
	//
	// 					// // Check if source or target node is in moved segment
	// 					// uint64_t sourceNode = edge.getSourceNodeId();
	// 					// uint64_t targetNode = edge.getTargetNodeId();
	// 					//
	// 					// bool updated = false;
	// 					// if (sourceNode >= startId && sourceNode < endId) {
	// 					//     // Update edge's reference to source node
	// 					//     updated = true;
	// 					// }
	// 					//
	// 					// if (targetNode >= startId && targetNode < endId) {
	// 					//     // Update edge's reference to target node
	// 					//     updated = true;
	// 					// }
	// 					//
	// 					// if (updated) {
	// 					//     // Write updated edge back
	// 					//     file_->seekp(static_cast<std::streamoff>(edgeOffset));
	// 					//     file_->write(reinterpret_cast<const char*>(&edge), sizeof(Edge));
	// 					// }
	// 				}
	// 			}
	// 		}
	// 	} else if (type == 1) { // Edge segment moved
	// 		// Read segment header to get ID range
	// 		SegmentHeader header = spaceManager_->getTracker()->getSegmentHeader(newOffset);
	//
	// 		// ID range covered by this segment
	// 		uint64_t startId = header.start_id;
	// 		uint64_t endId = startId + header.capacity;
	//
	// 		// Update any property references to edges in this segment
	// 		// (would require scanning property segments for references to these edges)
	//
	// 	} else if (type == 2) { // Property segment moved
	// 		// Update entity references to properties in this segment
	// 		SegmentHeader header = spaceManager_->getTracker()->getSegmentHeader(newOffset);
	//
	// 		// This is more complex as we need to scan through property entries
	// 		// to determine which entities reference this segment
	// 	}
	// }

} // namespace graph::storage
