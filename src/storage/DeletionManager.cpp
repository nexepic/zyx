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
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::storage {

	DeletionManager::DeletionManager(std::shared_ptr<DataManager> dataManager,
									 std::shared_ptr<SpaceManager> spaceManager,
									 std::shared_ptr<IDAllocator> idAllocator) :
	dataManager_(std::move(dataManager)),
	spaceManager_(std::move(spaceManager)),
	idAllocator_(std::move(idAllocator)) {}

	DeletionManager::~DeletionManager() = default;

	void DeletionManager::initialize() {
		// Refresh segment state to initialize tracking
		// refreshSegmentState();
	}

	void DeletionManager::deleteNode(Node &node) const {
		if (node.hasPropertyEntity()) {
			deletePropertyEntity(node.getPropertyEntityId(), node.getPropertyStorageType());
		}

		deleteNodeConnectedEdges(node.getId());

		// Mark the node as inactive in the index
		markNodeInactive(node);
	}

	void DeletionManager::deleteEdge(Edge &edge) const {
		if (edge.hasPropertyEntity()) {
			deletePropertyEntity(edge.getPropertyEntityId(), edge.getPropertyStorageType());
		}

		// Mark the edge as inactive in the index
		markEdgeInactive(edge);
	}

	void DeletionManager::deleteProperty(Property &property) const {
		if (property.getId() == 0)
			return;

		// Mark the property as inactive in the index
		markPropertyInactive(property);
	}

	void DeletionManager::deleteBlob(Blob &blob) const {
		if (blob.getId() == 0)
			return;

		// Mark the blob as inactive in the index
		markBlobInactive(blob);
	}

	void DeletionManager::deleteIndex(Index &index) const {
		if (index.getId() == 0)
			return;

		// Mark the index as inactive in the index
		markIndexInactive(index);
	}

	void DeletionManager::deleteState(State &state) const {
		if (state.getId() == 0)
			return;

		// Mark the state as inactive in the index
		markStateInactive(state);
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

	uint64_t DeletionManager::findSegmentForIndexId(int64_t id) const {
		// Use the tracker's method instead of reimplementing the logic
		return spaceManager_->getTracker()->getSegmentOffsetForIndexId(id);
	}

	uint64_t DeletionManager::findSegmentForStateId(int64_t id) const {
		// Use the tracker's method instead of reimplementing the logic
		return spaceManager_->getTracker()->getSegmentOffsetForStateId(id);
	}

	bool DeletionManager::deleteBulkNodes(const std::vector<int64_t> &nodeIds, bool cascadeEdges) const {
		bool allSucceeded = true;

		// For bulk operations, we'll postpone compaction until the end
		for (int64_t nodeId: nodeIds) {
			// Use a non-compacting version of node deletion
			bool success = performNodeDeletion(nodeId);

			if (success && cascadeEdges) {
				// Find and delete connected edges
				auto edges = dataManager_->findEdgesByNode(nodeId, "both");
				for (const auto &edge: edges) {
					return performEdgeDeletion(edge.getId());
				}
			}

			allSucceeded = allSucceeded && success;
		}

		return allSucceeded;
	}

	bool DeletionManager::deleteBulkEdges(const std::vector<int64_t> &edgeIds) const {
		bool allSucceeded = true;

		// For bulk operations, postpone compaction until the end
		for (int64_t edgeId: edgeIds) {
			// Use non-compacting deletion
			allSucceeded = performEdgeDeletion(edgeId) && allSucceeded;
		}

		return allSucceeded;
	}

	bool DeletionManager::performNodeDeletion(int64_t nodeId) const {
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

	bool DeletionManager::performEdgeDeletion(int64_t edgeId) const {
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

	void DeletionManager::deletePropertyEntity(int64_t propertyId, PropertyStorageType storageType) const {
		if (propertyId == 0)
			return;

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

	void DeletionManager::deleteNodeConnectedEdges(int64_t nodeId) const {
		auto edges = dataManager_->findEdgesByNode(nodeId, "both");
		for (auto &edge: edges) {
			markEdgeInactive(edge);
		}
	}

	void DeletionManager::markNodeInactive(Node &node) const {
		// 1. Eagerly free the ID
		// This updates the SegmentTracker bitmap to 0 AND puts the ID into the L1 Cache immediately.
		idAllocator_->freeId(node.getId(), Node::typeId);

		// 2. Mark logical deletion in DataManager (Persistence Layer)
		// This ensures the entity is treated as DELETED in transactions/queries.
		dataManager_->markEntityDeleted(node);

		// 3. Compaction Check
		// We still need to check compaction, but we use the IDAllocator's underlying tracker state which is now updated.
		// Optimization: freeId already updated the bitmap, so fragmentation ratio is current.
		uint64_t segmentOffset = findSegmentForNodeId(node.getId());
		if (segmentOffset != 0) {
			SegmentHeader segmentHeader = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
			if (segmentHeader.getFragmentationRatio() >= COMPACTION_THRESHOLD) {
				spaceManager_->getTracker()->markForCompaction(segmentOffset, true);
			}
		}
	}

	void DeletionManager::markEdgeInactive(Edge &edge) const {
		idAllocator_->freeId(edge.getId(), Edge::typeId);
		dataManager_->markEntityDeleted(edge);

		uint64_t segmentOffset = findSegmentForEdgeId(edge.getId());
		if (segmentOffset != 0) {
			SegmentHeader segmentHeader = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
			if (segmentHeader.getFragmentationRatio() >= COMPACTION_THRESHOLD) {
				spaceManager_->getTracker()->markForCompaction(segmentOffset, true);
			}
		}
	}

	void DeletionManager::markPropertyInactive(Property &property) const {
        idAllocator_->freeId(property.getId(), Property::typeId);
        dataManager_->markEntityDeleted(property);

        uint64_t segmentOffset = findSegmentForPropertyId(property.getId());
        if (segmentOffset != 0) {
            SegmentHeader segmentHeader = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
            if (segmentHeader.getFragmentationRatio() >= COMPACTION_THRESHOLD) {
                spaceManager_->getTracker()->markForCompaction(segmentOffset, true);
            }
        }
    }

    void DeletionManager::markBlobInactive(Blob &blob) const {
        idAllocator_->freeId(blob.getId(), Blob::typeId);
        dataManager_->markEntityDeleted(blob);

        uint64_t segmentOffset = findSegmentForBlobId(blob.getId());
        if (segmentOffset != 0) {
            SegmentHeader segmentHeader = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
            if (segmentHeader.getFragmentationRatio() >= COMPACTION_THRESHOLD) {
                spaceManager_->getTracker()->markForCompaction(segmentOffset, true);
            }
        }
    }

    void DeletionManager::markIndexInactive(Index &index) const {
        idAllocator_->freeId(index.getId(), Index::typeId);
        dataManager_->markEntityDeleted(index);

        uint64_t segmentOffset = findSegmentForIndexId(index.getId());
        if (segmentOffset != 0) {
            SegmentHeader segmentHeader = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
            if (segmentHeader.getFragmentationRatio() >= COMPACTION_THRESHOLD) {
                spaceManager_->getTracker()->markForCompaction(segmentOffset, true);
            }
        }
    }

    void DeletionManager::markStateInactive(State &state) const {
        idAllocator_->freeId(state.getId(), State::typeId);
        dataManager_->markEntityDeleted(state);

        uint64_t segmentOffset = findSegmentForStateId(state.getId());
        if (segmentOffset != 0) {
            SegmentHeader segmentHeader = spaceManager_->getTracker()->getSegmentHeader(segmentOffset);
            if (segmentHeader.getFragmentationRatio() >= COMPACTION_THRESHOLD) {
                spaceManager_->getTracker()->markForCompaction(segmentOffset, true);
            }
        }
    }

	bool DeletionManager::isNodeActive(int64_t nodeId) const {
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

	bool DeletionManager::isEdgeActive(int64_t edgeId) const {
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

} // namespace graph::storage
