/**
 * @file EntityReferenceUpdater.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/5/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/EntityReferenceUpdater.hpp"
#include <graph/storage/SegmentTracker.hpp>
#include <graph/storage/SegmentType.hpp>
#include <utility>

namespace graph::storage {

	EntityReferenceUpdater::EntityReferenceUpdater(std::shared_ptr<std::fstream> file,
												   std::shared_ptr<SegmentTracker> tracker) :
		file_(std::move(file)), segmentTracker_(std::move(tracker)) {}

	void EntityReferenceUpdater::updateEntityReferences(int64_t oldEntityId, const void *newEntity,
														uint32_t entityType) {
		// Dispatch to appropriate handler based on entity type
		switch (entityType) {
			case toUnderlying(SegmentType::Node):
				updateNodeReferences(oldEntityId, *static_cast<const Node *>(newEntity));
				break;
			case toUnderlying(SegmentType::Edge):
				updateEdgeReferences(oldEntityId, *static_cast<const Edge *>(newEntity));
				break;
			case toUnderlying(SegmentType::Property):
				updatePropertyReferences(oldEntityId, *static_cast<const Property *>(newEntity));
				break;
			case toUnderlying(SegmentType::Blob):
				updateBlobReferences(oldEntityId, *static_cast<const Blob *>(newEntity));
				break;
			default:;
		}
	}

	void EntityReferenceUpdater::updateNodeReferences(int64_t oldNodeId, const Node &newNode) {
		int64_t newNodeId = newNode.getId();

		// Skip if IDs are the same (no reference update needed)
		if (oldNodeId == newNodeId) {
			return;
		}

		// We need to find all edges that reference this node (either as source or target)
		// and update their references

		// First, get the first outgoing edge from the node
		int64_t firstOutEdgeId = newNode.getFirstOutEdgeId();
		if (firstOutEdgeId != 0) {
			// Follow the outgoing edge chain
			int64_t currentEdgeId = firstOutEdgeId;
			while (currentEdgeId != 0) {
				// Get the segment containing this edge
				uint64_t edgeSegmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(currentEdgeId);
				if (edgeSegmentOffset == 0)
					break;

				// Get the edge segment header
				SegmentHeader edgeHeader = segmentTracker_->getSegmentHeader(edgeSegmentOffset);

				// Calculate the edge's index in the segment
				auto edgeIndex = static_cast<uint32_t>(currentEdgeId - edgeHeader.start_id);

				// Check if the edge exists and is active
				if (edgeIndex >= edgeHeader.used || !segmentTracker_->isEntityActive(edgeSegmentOffset, edgeIndex))
					break;

				// Read the edge
				size_t edgeSize = Edge::getTotalSize();
				Edge edge = segmentTracker_->readEntity<Edge>(edgeSegmentOffset, edgeIndex, edgeSize);

				// Update the edge's source node ID
				if (edge.getSourceNodeId() == oldNodeId) {
					edge.setSourceNodeId(newNodeId);
					segmentTracker_->writeEntity(edgeSegmentOffset, edgeIndex, edge, edgeSize);
				}

				// Move to the next edge in the chain
				currentEdgeId = edge.getNextOutEdgeId();
			}
		}

		// Next, get the first incoming edge to the node
		int64_t firstInEdgeId = newNode.getFirstInEdgeId();
		if (firstInEdgeId != 0) {
			// Follow the incoming edge chain
			int64_t currentEdgeId = firstInEdgeId;
			while (currentEdgeId != 0) {
				// Get the segment containing this edge
				uint64_t edgeSegmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(currentEdgeId);
				if (edgeSegmentOffset == 0)
					break;

				// Get the edge segment header
				SegmentHeader edgeHeader = segmentTracker_->getSegmentHeader(edgeSegmentOffset);

				// Calculate the edge's index in the segment
				auto edgeIndex = static_cast<uint32_t>(currentEdgeId - edgeHeader.start_id);

				// Check if the edge exists and is active
				if (edgeIndex >= edgeHeader.used || !segmentTracker_->isEntityActive(edgeSegmentOffset, edgeIndex))
					break;

				// Read the edge
				size_t edgeSize = Edge::getTotalSize();
				Edge edge = segmentTracker_->readEntity<Edge>(edgeSegmentOffset, edgeIndex, edgeSize);

				// Update the edge's target node ID
				if (edge.getTargetNodeId() == oldNodeId) {
					edge.setTargetNodeId(newNodeId);
					segmentTracker_->writeEntity(edgeSegmentOffset, edgeIndex, edge, edgeSize);
				}

				// Move to the next edge in the chain
				currentEdgeId = edge.getNextInEdgeId();
			}
		}

		// Update properties that reference this node
		updatePropertiesPointingToEntity(oldNodeId, newNode);
	}

	void EntityReferenceUpdater::updateEdgeReferences(int64_t oldEdgeId, const Edge &newEdge) {
		int64_t newEdgeId = newEdge.getId();

		// Skip if IDs are the same
		if (oldEdgeId == newEdgeId) {
			return;
		}

		// Get source and target node IDs from the edge
		int64_t sourceNodeId = newEdge.getSourceNodeId();
		int64_t targetNodeId = newEdge.getTargetNodeId();

		// Update the source node's first outgoing edge reference if it points to this edge
		uint64_t sourceNodeSegmentOffset = segmentTracker_->getSegmentOffsetForNodeId(sourceNodeId);
		if (sourceNodeSegmentOffset != 0) {
			SegmentHeader sourceNodeHeader = segmentTracker_->getSegmentHeader(sourceNodeSegmentOffset);
			auto sourceNodeIndex = static_cast<uint32_t>(sourceNodeId - sourceNodeHeader.start_id);

			if (sourceNodeIndex < sourceNodeHeader.used &&
				segmentTracker_->isEntityActive(sourceNodeSegmentOffset, sourceNodeIndex)) {

				size_t nodeSize = Node::getTotalSize();
				Node sourceNode = segmentTracker_->readEntity<Node>(sourceNodeSegmentOffset, sourceNodeIndex, nodeSize);

				if (sourceNode.getFirstOutEdgeId() == oldEdgeId) {
					sourceNode.setFirstOutEdgeId(newEdgeId);
					segmentTracker_->writeEntity(sourceNodeSegmentOffset, sourceNodeIndex, sourceNode, nodeSize);
				}
			}
		}

		// Update the target node's first incoming edge reference if it points to this edge
		uint64_t targetNodeSegmentOffset = segmentTracker_->getSegmentOffsetForNodeId(targetNodeId);
		if (targetNodeSegmentOffset != 0) {
			SegmentHeader targetNodeHeader = segmentTracker_->getSegmentHeader(targetNodeSegmentOffset);
			auto targetNodeIndex = static_cast<uint32_t>(targetNodeId - targetNodeHeader.start_id);

			if (targetNodeIndex < targetNodeHeader.used &&
				segmentTracker_->isEntityActive(targetNodeSegmentOffset, targetNodeIndex)) {

				size_t nodeSize = Node::getTotalSize();
				Node targetNode = segmentTracker_->readEntity<Node>(targetNodeSegmentOffset, targetNodeIndex, nodeSize);

				if (targetNode.getFirstInEdgeId() == oldEdgeId) {
					targetNode.setFirstInEdgeId(newEdgeId);
					segmentTracker_->writeEntity(targetNodeSegmentOffset, targetNodeIndex, targetNode, nodeSize);
				}
			}
		}

		// Update references in adjacent edges in the linked list
		// Get previous and next edges in the outgoing chain
		int64_t prevOutEdgeId = newEdge.getPrevOutEdgeId();
		int64_t nextOutEdgeId = newEdge.getNextOutEdgeId();

		// Update the previous edge's next pointer if it exists
		if (prevOutEdgeId != 0) {
			uint64_t prevEdgeSegmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(prevOutEdgeId);
			if (prevEdgeSegmentOffset != 0) {
				SegmentHeader prevEdgeHeader = segmentTracker_->getSegmentHeader(prevEdgeSegmentOffset);
				auto prevEdgeIndex = static_cast<uint32_t>(prevOutEdgeId - prevEdgeHeader.start_id);

				if (prevEdgeIndex < prevEdgeHeader.used &&
					segmentTracker_->isEntityActive(prevEdgeSegmentOffset, prevEdgeIndex)) {

					size_t edgeSize = Edge::getTotalSize();
					Edge prevEdge = segmentTracker_->readEntity<Edge>(prevEdgeSegmentOffset, prevEdgeIndex, edgeSize);

					if (prevEdge.getNextOutEdgeId() == oldEdgeId) {
						prevEdge.setNextOutEdgeId(newEdgeId);
						segmentTracker_->writeEntity(prevEdgeSegmentOffset, prevEdgeIndex, prevEdge, edgeSize);
					}
				}
			}
		}

		// Update the next edge's previous pointer if it exists
		if (nextOutEdgeId != 0) {
			uint64_t nextEdgeSegmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(nextOutEdgeId);
			if (nextEdgeSegmentOffset != 0) {
				SegmentHeader nextEdgeHeader = segmentTracker_->getSegmentHeader(nextEdgeSegmentOffset);
				auto nextEdgeIndex = static_cast<uint32_t>(nextOutEdgeId - nextEdgeHeader.start_id);

				if (nextEdgeIndex < nextEdgeHeader.used &&
					segmentTracker_->isEntityActive(nextEdgeSegmentOffset, nextEdgeIndex)) {

					size_t edgeSize = Edge::getTotalSize();
					Edge nextEdge = segmentTracker_->readEntity<Edge>(nextEdgeSegmentOffset, nextEdgeIndex, edgeSize);

					if (nextEdge.getPrevOutEdgeId() == oldEdgeId) {
						nextEdge.setPrevOutEdgeId(newEdgeId);
						segmentTracker_->writeEntity(nextEdgeSegmentOffset, nextEdgeIndex, nextEdge, edgeSize);
					}
				}
			}
		}

		// Get previous and next edges in the incoming chain
		int64_t prevInEdgeId = newEdge.getPrevInEdgeId();
		int64_t nextInEdgeId = newEdge.getNextInEdgeId();

		// Update the previous edge's next pointer if it exists
		if (prevInEdgeId != 0) {
			uint64_t prevEdgeSegmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(prevInEdgeId);
			if (prevEdgeSegmentOffset != 0) {
				SegmentHeader prevEdgeHeader = segmentTracker_->getSegmentHeader(prevEdgeSegmentOffset);
				auto prevEdgeIndex = static_cast<uint32_t>(prevInEdgeId - prevEdgeHeader.start_id);

				if (prevEdgeIndex < prevEdgeHeader.used &&
					segmentTracker_->isEntityActive(prevEdgeSegmentOffset, prevEdgeIndex)) {

					size_t edgeSize = Edge::getTotalSize();
					Edge prevEdge = segmentTracker_->readEntity<Edge>(prevEdgeSegmentOffset, prevEdgeIndex, edgeSize);

					if (prevEdge.getNextInEdgeId() == oldEdgeId) {
						prevEdge.setNextInEdgeId(newEdgeId);
						segmentTracker_->writeEntity(prevEdgeSegmentOffset, prevEdgeIndex, prevEdge, edgeSize);
					}
				}
			}
		}

		// Update the next edge's previous pointer if it exists
		if (nextInEdgeId != 0) {
			uint64_t nextEdgeSegmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(nextInEdgeId);
			if (nextEdgeSegmentOffset != 0) {
				SegmentHeader nextEdgeHeader = segmentTracker_->getSegmentHeader(nextEdgeSegmentOffset);
				auto nextEdgeIndex = static_cast<uint32_t>(nextInEdgeId - nextEdgeHeader.start_id);

				if (nextEdgeIndex < nextEdgeHeader.used &&
					segmentTracker_->isEntityActive(nextEdgeSegmentOffset, nextEdgeIndex)) {

					size_t edgeSize = Edge::getTotalSize();
					Edge nextEdge = segmentTracker_->readEntity<Edge>(nextEdgeSegmentOffset, nextEdgeIndex, edgeSize);

					if (nextEdge.getPrevInEdgeId() == oldEdgeId) {
						nextEdge.setPrevInEdgeId(newEdgeId);
						segmentTracker_->writeEntity(nextEdgeSegmentOffset, nextEdgeIndex, nextEdge, edgeSize);
					}
				}
			}
		}

		// Update properties that reference this edge
		updatePropertiesPointingToEntity(oldEdgeId, newEdge);
	}

	void EntityReferenceUpdater::updatePropertyReferences(int64_t oldPropId, const Property &newProperty) const {
		int64_t newPropId = newProperty.getId();

		// Skip if IDs are the same
		if (oldPropId == newPropId) {
			return;
		}

		// Get entity ID and type directly from the property object
		int64_t entityId = newProperty.getEntityId();
		uint32_t entityType = newProperty.getEntityType();

		if (entityType == toUnderlying(SegmentType::Node)) {
			uint64_t nodeSegmentOffset = segmentTracker_->getSegmentOffsetForNodeId(entityId);
			if (nodeSegmentOffset != 0) {
				SegmentHeader nodeHeader = segmentTracker_->getSegmentHeader(nodeSegmentOffset);
				auto nodeIndex = static_cast<uint32_t>(entityId - nodeHeader.start_id);

				if (nodeIndex < nodeHeader.used && segmentTracker_->isEntityActive(nodeSegmentOffset, nodeIndex)) {

					size_t nodeSize = Node::getTotalSize();
					Node node = segmentTracker_->readEntity<Node>(nodeSegmentOffset, nodeIndex, nodeSize);

					if (node.getPropertyEntityId() == oldPropId &&
						node.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {

						node.setPropertyEntityId(newPropId, PropertyStorageType::PROPERTY_ENTITY);
						segmentTracker_->writeEntity(nodeSegmentOffset, nodeIndex, node, nodeSize);
					}
				}
			}
		} else if (entityType == toUnderlying(SegmentType::Edge)) {
			uint64_t edgeSegmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(entityId);
			if (edgeSegmentOffset != 0) {
				SegmentHeader edgeHeader = segmentTracker_->getSegmentHeader(edgeSegmentOffset);
				auto edgeIndex = static_cast<uint32_t>(entityId - edgeHeader.start_id);

				if (edgeIndex < edgeHeader.used && segmentTracker_->isEntityActive(edgeSegmentOffset, edgeIndex)) {

					size_t edgeSize = Edge::getTotalSize();
					Edge edge = segmentTracker_->readEntity<Edge>(edgeSegmentOffset, edgeIndex, edgeSize);

					if (edge.getPropertyEntityId() == oldPropId &&
						edge.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {

						edge.setPropertyEntityId(newPropId, PropertyStorageType::PROPERTY_ENTITY);
						segmentTracker_->writeEntity(edgeSegmentOffset, edgeIndex, edge, edgeSize);
					}
				}
			}
		}
	}

	void EntityReferenceUpdater::updateBlobReferences(int64_t oldBlobId, const Blob &newBlob) const {
		int64_t newBlobId = newBlob.getId();

		// Skip if IDs are the same
		if (oldBlobId == newBlobId) {
			return;
		}

		// Get entity ID and type directly from the blob object
		int64_t entityId = newBlob.getEntityId();
		uint32_t entityType = newBlob.getEntityType();

		// Update the owner entity's reference to this blob
		if (entityType == toUnderlying(SegmentType::Node)) {
			uint64_t nodeSegmentOffset = segmentTracker_->getSegmentOffsetForNodeId(entityId);
			if (nodeSegmentOffset != 0) {
				SegmentHeader nodeHeader = segmentTracker_->getSegmentHeader(nodeSegmentOffset);
				auto nodeIndex = static_cast<uint32_t>(entityId - nodeHeader.start_id);

				if (nodeIndex < nodeHeader.used && segmentTracker_->isEntityActive(nodeSegmentOffset, nodeIndex)) {
					size_t nodeSize = Node::getTotalSize();
					Node node = segmentTracker_->readEntity<Node>(nodeSegmentOffset, nodeIndex, nodeSize);

					if (node.getPropertyEntityId() == oldBlobId &&
						node.getPropertyStorageType() == PropertyStorageType::BLOB_ENTITY) {

						node.setPropertyEntityId(newBlobId, PropertyStorageType::BLOB_ENTITY);
						segmentTracker_->writeEntity(nodeSegmentOffset, nodeIndex, node, nodeSize);
					}
				}
			}
		} else if (entityType == toUnderlying(SegmentType::Edge)) {
			uint64_t edgeSegmentOffset = segmentTracker_->getSegmentOffsetForEdgeId(entityId);
			if (edgeSegmentOffset != 0) {
				SegmentHeader edgeHeader = segmentTracker_->getSegmentHeader(edgeSegmentOffset);
				auto edgeIndex = static_cast<uint32_t>(entityId - edgeHeader.start_id);

				if (edgeIndex < edgeHeader.used && segmentTracker_->isEntityActive(edgeSegmentOffset, edgeIndex)) {
					size_t edgeSize = Edge::getTotalSize();
					Edge edge = segmentTracker_->readEntity<Edge>(edgeSegmentOffset, edgeIndex, edgeSize);

					if (edge.getPropertyEntityId() == oldBlobId &&
						edge.getPropertyStorageType() == PropertyStorageType::BLOB_ENTITY) {

						edge.setPropertyEntityId(newBlobId, PropertyStorageType::BLOB_ENTITY);
						segmentTracker_->writeEntity(edgeSegmentOffset, edgeIndex, edge, edgeSize);
					}
				}
			}
		}

		// Update references in the blob chain
		// 1. Update the next blob in the chain (if any)
		int64_t nextBlobId = newBlob.getNextBlobId();
		if (nextBlobId != 0) {
			updateBlobChainReference(oldBlobId, newBlobId, nextBlobId, true, newBlob);
		}

		// 2. Update the previous blob in the chain (if any)
		int64_t prevBlobId = newBlob.getPrevBlobId();
		if (prevBlobId != 0) {
			updateBlobChainReference(oldBlobId, newBlobId, prevBlobId, false, newBlob);
		}
	}

	void EntityReferenceUpdater::updateBlobChainReference(int64_t oldBlobId, int64_t newBlobId, int64_t linkedBlobId,
														  bool isNextBlob, const Blob &sourceBlob) const {
		uint64_t linkedBlobSegmentOffset = segmentTracker_->getSegmentOffsetForBlobId(linkedBlobId);
		if (linkedBlobSegmentOffset == 0) {
			return;
		}

		SegmentHeader linkedBlobHeader = segmentTracker_->getSegmentHeader(linkedBlobSegmentOffset);
		auto linkedBlobIndex = static_cast<uint32_t>(linkedBlobId - linkedBlobHeader.start_id);

		// if (linkedBlobIndex >= linkedBlobHeader.used ||
		//     !segmentTracker_->isEntityActive(linkedBlobSegmentOffset, linkedBlobIndex)) {
		//     return;
		// }

		size_t blobSize = Blob::getTotalSize();
		Blob linkedBlob = segmentTracker_->readEntity<Blob>(linkedBlobSegmentOffset, linkedBlobIndex, blobSize);
		bool updated = false;

		// Update the appropriate reference based on whether this is next or previous in the chain
		if (isNextBlob) {
			// We're updating the next blob's prevBlobId field
			if (linkedBlob.getPrevBlobId() == oldBlobId) {
				linkedBlob.setPrevBlobId(newBlobId);
				updated = true;
			}
		} else {
			// We're updating the previous blob's nextBlobId field
			if (linkedBlob.getNextBlobId() == oldBlobId) {
				linkedBlob.setNextBlobId(newBlobId);
				updated = true;
			}
		}

		// Update entity ID and type to match the source blob
		int64_t sourceEntityId = sourceBlob.getEntityId();
		uint32_t sourceEntityType = sourceBlob.getEntityType();

		if (linkedBlob.getEntityId() != sourceEntityId || linkedBlob.getEntityType() != sourceEntityType) {
			linkedBlob.setEntityInfo(sourceEntityId, sourceEntityType);
			updated = true;
		}

		// Only write back if something was updated
		if (updated) {
			segmentTracker_->writeEntity(linkedBlobSegmentOffset, linkedBlobIndex, linkedBlob, blobSize);
		}
	}

	Property EntityReferenceUpdater::getPropertyById(int64_t propertyId) const {
		// Find the segment containing this property
		uint64_t propSegmentOffset = segmentTracker_->getSegmentOffsetForPropId(propertyId);
		if (propSegmentOffset == 0) {
			throw std::runtime_error("Property not found for ID: " + std::to_string(propertyId));
		}

		// Get the segment header
		SegmentHeader propHeader = segmentTracker_->getSegmentHeader(propSegmentOffset);

		// Calculate the property's index in the segment
		auto propIndex = static_cast<uint32_t>(propertyId - propHeader.start_id);

		// Verify the property exists and is active
		if (propIndex >= propHeader.used || !segmentTracker_->isEntityActive(propSegmentOffset, propIndex)) {
			throw std::runtime_error("Property not active or index out of bounds for ID: " +
									 std::to_string(propertyId));
		}

		// Read and return the property
		size_t propertySize = Property::getTotalSize();
		return segmentTracker_->readEntity<Property>(propSegmentOffset, propIndex, propertySize);
	}

	// Get a specific blob by its ID
	Blob EntityReferenceUpdater::getBlobById(int64_t blobId) const {
		// Find the segment containing this blob
		uint64_t blobSegmentOffset = segmentTracker_->getSegmentOffsetForBlobId(blobId);
		if (blobSegmentOffset == 0) {
			throw std::runtime_error("Blob not found for ID: " + std::to_string(blobId));
		}

		// Get the segment header
		SegmentHeader blobHeader = segmentTracker_->getSegmentHeader(blobSegmentOffset);

		// Calculate the blob's index in the segment
		auto blobIndex = static_cast<uint32_t>(blobId - blobHeader.start_id);

		// Verify the blob exists and is active
		if (blobIndex >= blobHeader.used || !segmentTracker_->isEntityActive(blobSegmentOffset, blobIndex)) {
			throw std::runtime_error("Blob not active or index out of bounds for ID: " + std::to_string(blobId));
		}

		// Read and return the blob
		size_t blobSize = Blob::getTotalSize();
		return segmentTracker_->readEntity<Blob>(blobSegmentOffset, blobIndex, blobSize);
	}

	template<typename T>
	EntityReferenceUpdater::EntityPropertyInfo
	EntityReferenceUpdater::getEntityPropertyInfoFromEntity(const T &entity) {
		EntityPropertyInfo result = {PropertyStorageType::NONE, 0};

		// Extract property information directly from the entity
		result.storageType = entity.getPropertyStorageType();
		result.propertyEntityId = entity.getPropertyEntityId();

		return result;
	}

	template<typename T>
	void EntityReferenceUpdater::updatePropertiesPointingToEntity(int64_t oldEntityId, const T &newEntity) {
		// Get entity type from the entity object
		uint32_t entityType;
		if constexpr (std::is_same_v<T, Node>) {
			entityType = toUnderlying(SegmentType::Node);
		} else if constexpr (std::is_same_v<T, Edge>) {
			entityType = toUnderlying(SegmentType::Edge);
		} else {
			// Default fallback or error handling
			return;
		}

		// Extract property information directly from the entity
		EntityPropertyInfo propInfo = getEntityPropertyInfoFromEntity(newEntity);
		int64_t newEntityId = newEntity.getId();

		if (propInfo.storageType == PropertyStorageType::NONE || propInfo.propertyEntityId == 0) {
			// No property entity associated, nothing to update
			return;
		}

		if (propInfo.storageType == PropertyStorageType::PROPERTY_ENTITY) {
			try {
				// Get the property entity directly
				Property property = getPropertyById(propInfo.propertyEntityId);

				// Only update if it references the old entity ID
				if (property.getEntityId() == oldEntityId && property.getEntityType() == entityType) {
					// Update the entity reference
					Property updatedProperty = property;
					updatedProperty.setEntityInfo(newEntityId, entityType);

					// Get the segment info
					uint64_t propSegmentOffset = segmentTracker_->getSegmentOffsetForPropId(propInfo.propertyEntityId);
					SegmentHeader propHeader = segmentTracker_->getSegmentHeader(propSegmentOffset);
					auto propIndex = static_cast<uint32_t>(propInfo.propertyEntityId - propHeader.start_id);

					// Write back the updated property
					size_t propertySize = Property::getTotalSize();
					segmentTracker_->writeEntity(propSegmentOffset, propIndex, updatedProperty, propertySize);
				}
			} catch (const std::runtime_error &e) {
				// Log the error but continue processing
				std::cerr << "Error updating property reference: " << e.what() << std::endl;
			}
		} else if (propInfo.storageType == PropertyStorageType::BLOB_ENTITY) {
			try {
				// Get the blob entity directly
				Blob blob = getBlobById(propInfo.propertyEntityId);

				// Only update if it references the old entity ID
				if (blob.getEntityId() == oldEntityId && blob.getEntityType() == entityType) {
					// Update the entity reference
					Blob updatedBlob = blob;
					updatedBlob.setEntityInfo(newEntityId, entityType);

					// Get the segment info
					uint64_t blobSegmentOffset = segmentTracker_->getSegmentOffsetForBlobId(propInfo.propertyEntityId);
					SegmentHeader blobHeader = segmentTracker_->getSegmentHeader(blobSegmentOffset);
					auto blobIndex = static_cast<uint32_t>(propInfo.propertyEntityId - blobHeader.start_id);

					// Write back the updated blob
					size_t blobSize = Blob::getTotalSize();
					segmentTracker_->writeEntity(blobSegmentOffset, blobIndex, updatedBlob, blobSize);
				}
			} catch (const std::runtime_error &e) {
				// Log the error but continue processing
				std::cerr << "Error updating blob reference: " << e.what() << std::endl;
			}
		}
	}

	// Explicit template instantiations
	template void EntityReferenceUpdater::updatePropertiesPointingToEntity<Node>(int64_t oldEntityId,
																				 const Node &newEntity);
	template void EntityReferenceUpdater::updatePropertiesPointingToEntity<Edge>(int64_t oldEntityId,
																				 const Edge &newEntity);

} // namespace graph::storage
