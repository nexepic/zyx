/**
 * @file EntityReferenceUpdater.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/5/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/EntityReferenceUpdater.h"
#include <graph/storage/SegmentTracker.h>
#include <graph/storage/SegmentType.h>
#include <utility>

namespace graph::storage {

	EntityReferenceUpdater::EntityReferenceUpdater(std::shared_ptr<std::fstream> file,
												   std::shared_ptr<IDAllocator> idAllocator,
												   std::shared_ptr<SegmentTracker> tracker) :
		file_(std::move(file)), idAllocator_(std::move(idAllocator)), segmentTracker_(std::move(tracker)) {}

	bool EntityReferenceUpdater::updateNodeReferencesToPermanent(Node &node) {

		bool updated = false;

		// Update property entity reference if it exists
		if (node.hasPropertyEntity()) {
			int64_t propId = node.getPropertyEntityId();
			if (IDAllocator::isTemporaryId(propId)) {
				// Determine the type based on the property storage type
				auto storageType = node.getPropertyStorageType();
				uint32_t type = (storageType == PropertyStorageType::PROPERTY_ENTITY) ? Property::typeId
								: (storageType == PropertyStorageType::BLOB_ENTITY)	  ? Blob::typeId
																					  : 0;

				if (type != 0) {
					int64_t permanentId = idAllocator_->getPermanentId(propId, type);
					if (permanentId != 0) {
						node.setPropertyEntityId(permanentId, storageType);
						updated = true;
					}
				}
			}
		}

		// Update edge references in inEdges and outEdges
		auto inEdges = node.getInEdges();
		auto outEdges = node.getOutEdges();
		bool inEdgesUpdated = false;
		bool outEdgesUpdated = false;

		std::vector<uint64_t> updatedInEdges = inEdges;
		std::vector<uint64_t> updatedOutEdges = outEdges;

		for (auto &edgeId: updatedInEdges) {
			if (IDAllocator::isTemporaryId(edgeId)) {
				int64_t permanentId = idAllocator_->getPermanentId(edgeId, Edge::typeId);
				if (permanentId != 0) {
					edgeId = permanentId;
					inEdgesUpdated = true;
				}
			}
		}

		for (auto &edgeId: updatedOutEdges) {
			if (IDAllocator::isTemporaryId(edgeId)) {
				int64_t permanentId = idAllocator_->getPermanentId(edgeId, Edge::typeId);
				if (permanentId != 0) {
					edgeId = permanentId;
					outEdgesUpdated = true;
				}
			}
		}

		if (inEdgesUpdated) {
			node.setInEdges(updatedInEdges);
			updated = true;
		}

		if (outEdgesUpdated) {
			node.setOutEdges(updatedOutEdges);
			updated = true;
		}

		return updated;
	}

	bool EntityReferenceUpdater::updateEdgeReferencesToPermanent(Edge &edge) {

		bool updated = false;

		// Update property entity reference if it exists
		if (edge.hasPropertyEntity()) {
			int64_t propId = edge.getPropertyEntityId();
			if (IDAllocator::isTemporaryId(propId)) {
				// Determine the type based on the property storage type
				auto storageType = edge.getPropertyStorageType();
				uint32_t type = (storageType == PropertyStorageType::PROPERTY_ENTITY) ? Property::typeId
								: (storageType == PropertyStorageType::BLOB_ENTITY)	  ? Blob::typeId
																					  : 0;

				if (type != 0) {
					int64_t permanentId = idAllocator_->getPermanentId(propId, type);
					if (permanentId != 0) {
						edge.setPropertyEntityId(permanentId, storageType);
						updated = true;
					}
				}
			}
		}

		// Update source node reference
		int64_t sourceNodeId = edge.getSourceNodeId();
		if (IDAllocator::isTemporaryId(sourceNodeId)) {
			int64_t permanentId = idAllocator_->getPermanentId(sourceNodeId, Node::typeId);
			if (permanentId != 0) {
				edge.setSourceNodeId(permanentId);
				updated = true;
			}
		}

		// Update target node reference
		int64_t targetNodeId = edge.getTargetNodeId();
		if (IDAllocator::isTemporaryId(targetNodeId)) {
			int64_t permanentId = idAllocator_->getPermanentId(targetNodeId, Node::typeId);
			if (permanentId != 0) {
				edge.setTargetNodeId(permanentId);
				updated = true;
			}
		}

		return updated;
	}

	bool EntityReferenceUpdater::updatePropertyReferencesToPermanent(Property &property) {

		bool updated = false;

		// Update entity ID reference
		int64_t entityId = property.getEntityId();
		uint32_t entityType = property.getEntityType();

		if (IDAllocator::isTemporaryId(entityId)) {
			int64_t permanentId = idAllocator_->getPermanentId(entityId, entityType);
			if (permanentId != 0) {
				property.setEntityId(permanentId);
				updated = true;
			}
		}

		return updated;
	}

	bool EntityReferenceUpdater::updateBlobReferencesToPermanent(Blob &blob) {

		bool updated = false;

		// Update entity ID reference
		int64_t entityId = blob.getEntityId();
		uint32_t entityType = blob.getEntityType();

		if (IDAllocator::isTemporaryId(entityId)) {
			int64_t permanentId = idAllocator_->getPermanentId(entityId, entityType);
			if (permanentId != 0) {
				blob.setEntityId(permanentId);
				updated = true;
			}
		}

		// Update next blob ID reference in the chain
		int64_t nextBlobId = blob.getNextBlobId();
		if (nextBlobId != 0 && IDAllocator::isTemporaryId(nextBlobId)) {
			int64_t permanentId = idAllocator_->getPermanentId(nextBlobId, Blob::typeId);
			if (permanentId != 0) {
				blob.setNextBlobId(permanentId);
				updated = true;
			}
		}

		// Update previous blob ID reference in the chain
		int64_t prevBlobId = blob.getPrevBlobId();
		if (prevBlobId != 0 && IDAllocator::isTemporaryId(prevBlobId)) {
			int64_t permanentId = idAllocator_->getPermanentId(prevBlobId, Blob::typeId);
			if (permanentId != 0) {
				blob.setPrevBlobId(permanentId);
				updated = true;
			}
		}

		return updated;
	}

	void EntityReferenceUpdater::updateEntityReferences(int64_t entityId, uint32_t entityType,
														uint64_t oldSegmentOffset, uint64_t newSegmentOffset) {
		// Get the original segment header to determine if the ID might have changed
		SegmentHeader oldHeader;
		SegmentHeader newHeader;
		int64_t newId = entityId;

		try {
			// Try to get headers from both locations
			oldHeader = segmentTracker_->getFreeSegmentHeader(oldSegmentOffset);
			newHeader = segmentTracker_->getSegmentHeader(newSegmentOffset);

			// Calculate the ID offset if start_ids differ
			if (oldHeader.start_id != newHeader.start_id) {
				int64_t idOffset = newHeader.start_id - oldHeader.start_id;
				// Calculate the entity's relative position in the segment
				int64_t relativePos = entityId - oldHeader.start_id;
				// Calculate the new entity ID after relocation
				newId = newHeader.start_id + relativePos;
			}
		} catch (const std::exception &) {
			// If we can't get headers, assume ID hasn't changed
		}

		// Dispatch to appropriate handler based on entity type
		switch (entityType) {
			case toUnderlying(SegmentType::Node):
				updateNodeReferences(entityId, newId, oldSegmentOffset, newSegmentOffset);
				break;
			case toUnderlying(SegmentType::Edge):
				updateEdgeReferences(entityId, newId, oldSegmentOffset, newSegmentOffset);
				break;
			case toUnderlying(SegmentType::Property):
				updatePropertyReferences(entityId, newId, oldSegmentOffset, newSegmentOffset);
				break;
			case toUnderlying(SegmentType::Blob):
				updateBlobReferences(entityId, newId, oldSegmentOffset, newSegmentOffset);
				break;
			default:;
		}
	}

	void EntityReferenceUpdater::updateNodeReferences(int64_t oldNodeId, int64_t newNodeId, uint64_t oldOffset,
													  uint64_t newOffset) {
		// Update edges that reference this node
		updateEdgesPointingToNode(oldNodeId, newNodeId, oldOffset, newOffset);

		// Update properties that reference this node
		updatePropertiesPointingToEntity(oldNodeId, newNodeId, toUnderlying(SegmentType::Node), oldOffset, newOffset);
	}

	void EntityReferenceUpdater::updateEdgeReferences(int64_t oldEdgeId, int64_t newEdgeId, uint64_t oldOffset,
													  uint64_t newOffset) {
		// Update properties that reference this edge
		updatePropertiesPointingToEntity(oldEdgeId, newEdgeId, toUnderlying(SegmentType::Edge), oldOffset, newOffset);
	}

	void EntityReferenceUpdater::updatePropertyReferences(int64_t oldPropId, int64_t newPropId, uint64_t oldOffset,
														  uint64_t newOffset) {
		// Find entities that reference this property
		// Scan node segments
		uint64_t nodeSegment = segmentTracker_->getChainHead(toUnderlying(SegmentType::Node));
		size_t nodeSize = Node::getTotalSize();

		while (nodeSegment != 0) {
			SegmentHeader header = segmentTracker_->getSegmentHeader(nodeSegment);

			// Check each node in the segment
			for (uint32_t i = 0; i < header.used; i++) {
				if (segmentTracker_->getBitmapBit(nodeSegment, i)) {
					// Read node using entity serialization
					Node node = segmentTracker_->readEntity<Node>(nodeSegment, i, nodeSize);

					// Calculate old property physical location
					uint64_t oldPropPhysicalLoc = calculatePropertyPhysicalLocation(oldPropId, oldOffset);

					// Check if this node references the property by physical location
					if (node.getPropertyEntityId() == oldPropPhysicalLoc) {
						// Calculate new property physical location
						uint64_t newPropPhysicalLoc = calculatePropertyPhysicalLocation(newPropId, newOffset);

						// Update property reference
						node.setPropertyEntityId(newPropPhysicalLoc);

						// Write updated node using entity serialization
						segmentTracker_->writeEntity(nodeSegment, i, node, nodeSize);
					}
				}
			}
			nodeSegment = header.next_segment_offset;
		}

		// Scan edge segments (similar logic as nodes)
		uint64_t edgeSegment = segmentTracker_->getChainHead(toUnderlying(SegmentType::Edge));
		size_t edgeSize = Edge::getTotalSize();

		while (edgeSegment != 0) {
			SegmentHeader header = segmentTracker_->getSegmentHeader(edgeSegment);

			for (uint32_t i = 0; i < header.used; i++) {
				if (segmentTracker_->getBitmapBit(edgeSegment, i)) {
					// Read edge using entity serialization
					Edge edge = segmentTracker_->readEntity<Edge>(edgeSegment, i, edgeSize);

					// Calculate old property physical location
					uint64_t oldPropPhysicalLoc = calculatePropertyPhysicalLocation(oldPropId, oldOffset);

					// Check if this edge references the property by physical location
					if (edge.getPropertyEntityId() == oldPropPhysicalLoc) {
						// Calculate new property physical location
						uint64_t newPropPhysicalLoc = calculatePropertyPhysicalLocation(newPropId, newOffset);

						// Update property reference
						edge.setPropertyEntityId(newPropPhysicalLoc, PropertyStorageType::PROPERTY_ENTITY);

						// Write updated edge using entity serialization
						segmentTracker_->writeEntity(edgeSegment, i, edge, edgeSize);
					}
				}
			}
			edgeSegment = header.next_segment_offset;
		}
	}

	void EntityReferenceUpdater::updateBlobReferences(int64_t oldBlobId, int64_t newBlobId, uint64_t oldOffset,
	   uint64_t newOffset) {
		// Implementation would follow the same pattern as the other reference update methods
		// Read entities using readEntity<T>, update references, and write back with writeEntity
	}

	void EntityReferenceUpdater::updatePropertiesPointingToEntity(int64_t oldEntityId, int64_t newEntityId,
																  uint32_t entityType, uint64_t oldOffset,
																  uint64_t newOffset) {
		// Scan property segments for properties referencing this entity
		uint64_t propSegment = segmentTracker_->getChainHead(toUnderlying(SegmentType::Property));
		size_t propertySize = Property::getTotalSize();

		while (propSegment != 0) {
			SegmentHeader header = segmentTracker_->getSegmentHeader(propSegment);

			// Check each property in the segment
			for (uint32_t i = 0; i < header.used; i++) {
				if (segmentTracker_->getBitmapBit(propSegment, i)) {
					// Read property using entity serialization
					Property property = segmentTracker_->readEntity<Property>(propSegment, i, propertySize);

					// Check if this property references our entity
					if (property.getEntityId() == oldEntityId && property.getEntityType() == entityType) {
						// Update entity ID if it changed
						if (oldEntityId != newEntityId) {
							property.setEntityInfo(newEntityId, entityType);

							// Write updated property using entity serialization
							segmentTracker_->writeEntity(propSegment, i, property, propertySize);
						}
					}
				}
			}
			propSegment = header.next_segment_offset;
		}
	}

	void EntityReferenceUpdater::updateEdgesPointingToNode(int64_t oldNodeId, int64_t newNodeId, uint64_t oldOffset,
														   uint64_t newOffset) {
		// Scan all edge segments for edges referencing this node
		uint64_t edgeSegment = segmentTracker_->getChainHead(toUnderlying(SegmentType::Edge));
		size_t edgeSize = Edge::getTotalSize();

		while (edgeSegment != 0) {
			SegmentHeader header = segmentTracker_->getSegmentHeader(edgeSegment);

			// Check each edge in the segment
			for (uint32_t i = 0; i < header.used; i++) {
				if (segmentTracker_->getBitmapBit(edgeSegment, i)) {
					// Read edge using entity serialization
					Edge edge = segmentTracker_->readEntity<Edge>(edgeSegment, i, edgeSize);

					// Check if this edge references our node as source or target
					bool updated = false;

					// Update source node reference if needed
					if (edge.getSourceNodeId() == oldNodeId) {
						edge.setSourceNodeId(newNodeId);
						updated = true;
					}

					// Update target node reference if needed
					if (edge.getTargetNodeId() == oldNodeId) {
						edge.setTargetNodeId(newNodeId);
						updated = true;
					}

					if (updated) {
						// Write updated edge using entity serialization
						segmentTracker_->writeEntity(edgeSegment, i, edge, edgeSize);
					}
				}
			}
			edgeSegment = header.next_segment_offset;
		}
	}

	uint64_t EntityReferenceUpdater::calculatePropertyPhysicalLocation(int64_t propId, uint64_t segmentOffset) {
		// Get the segment start_id
		int64_t startId = getSegmentStartId(segmentOffset, toUnderlying(SegmentType::Property));

		// Calculate physical location
		return segmentOffset + sizeof(SegmentHeader) + (propId - startId) * Property::getTotalSize();
	}

	// Helper method to get the start_id for a segment
	int64_t EntityReferenceUpdater::getSegmentStartId(uint64_t segmentOffset, uint32_t segmentType) {
		try {
			// Try to get from active segments first
			SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);
			return header.start_id;
		} catch (const std::exception &) {
			// If not active, try free segments
			try {
				SegmentHeader header = segmentTracker_->getFreeSegmentHeader(segmentOffset);
				return header.start_id;
			} catch (const std::exception &) {
				// If we can't get the start_id, return 0 as fallback
				return 0;
			}
		}
	}

} // namespace graph::storage
