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

namespace graph::storage {

	bool EntityReferenceUpdater::updateNodeReferencesToPermanent(Node &node,
																 const std::shared_ptr<IDAllocator> &idAllocator) {

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
					int64_t permanentId = idAllocator->getPermanentId(propId, type);
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
				int64_t permanentId = idAllocator->getPermanentId(edgeId, Edge::typeId);
				if (permanentId != 0) {
					edgeId = permanentId;
					inEdgesUpdated = true;
				}
			}
		}

		for (auto &edgeId: updatedOutEdges) {
			if (IDAllocator::isTemporaryId(edgeId)) {
				int64_t permanentId = idAllocator->getPermanentId(edgeId, Edge::typeId);
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

	bool EntityReferenceUpdater::updateEdgeReferencesToPermanent(Edge &edge,
																 const std::shared_ptr<IDAllocator> &idAllocator) {

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
					int64_t permanentId = idAllocator->getPermanentId(propId, type);
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
			int64_t permanentId = idAllocator->getPermanentId(sourceNodeId, Node::typeId);
			if (permanentId != 0) {
				edge.setSourceNodeId(permanentId);
				updated = true;
			}
		}

		// Update target node reference
		int64_t targetNodeId = edge.getTargetNodeId();
		if (IDAllocator::isTemporaryId(targetNodeId)) {
			int64_t permanentId = idAllocator->getPermanentId(targetNodeId, Node::typeId);
			if (permanentId != 0) {
				edge.setTargetNodeId(permanentId);
				updated = true;
			}
		}

		return updated;
	}

	bool EntityReferenceUpdater::updatePropertyReferencesToPermanent(Property &property,
																	 const std::shared_ptr<IDAllocator> &idAllocator) {

		bool updated = false;

		// Update entity ID reference
		int64_t entityId = property.getEntityId();
		uint32_t entityType = property.getEntityType();

		if (IDAllocator::isTemporaryId(entityId)) {
			int64_t permanentId = idAllocator->getPermanentId(entityId, entityType);
			if (permanentId != 0) {
				property.setEntityId(permanentId);
				updated = true;
			}
		}

		return updated;
	}

	bool EntityReferenceUpdater::updateBlobReferencesToPermanent(Blob &blob,
																 const std::shared_ptr<IDAllocator> &idAllocator) {

		bool updated = false;

		// Update entity ID reference
		int64_t entityId = blob.getEntityId();
		uint32_t entityType = blob.getEntityType();

		if (IDAllocator::isTemporaryId(entityId)) {
			int64_t permanentId = idAllocator->getPermanentId(entityId, entityType);
			if (permanentId != 0) {
				blob.setEntityId(permanentId);
				updated = true;
			}
		}

		// Update next blob ID reference in the chain
		int64_t nextBlobId = blob.getNextBlobId();
		if (nextBlobId != 0 && IDAllocator::isTemporaryId(nextBlobId)) {
			int64_t permanentId = idAllocator->getPermanentId(nextBlobId, Blob::typeId);
			if (permanentId != 0) {
				blob.setNextBlobId(permanentId);
				updated = true;
			}
		}

		// Update previous blob ID reference in the chain
		int64_t prevBlobId = blob.getPrevBlobId();
		if (prevBlobId != 0 && IDAllocator::isTemporaryId(prevBlobId)) {
			int64_t permanentId = idAllocator->getPermanentId(prevBlobId, Blob::typeId);
			if (permanentId != 0) {
				blob.setPrevBlobId(permanentId);
				updated = true;
			}
		}

		return updated;
	}

} // namespace graph::storage
