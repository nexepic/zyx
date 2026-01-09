/**
 * @file EntityReferenceUpdater.cpp
 * @author Nexepic
 * @date 2025/5/20
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

#include "graph/storage/EntityReferenceUpdater.hpp"
#include <iostream>
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/SegmentType.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::storage {

	EntityReferenceUpdater::EntityReferenceUpdater(const std::shared_ptr<DataManager> &dataManager) :
		dataManager_(dataManager) {}

	void EntityReferenceUpdater::updateEntityReferences(int64_t oldEntityId, const void *newEntity,
														uint32_t entityType) {
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
			case toUnderlying(SegmentType::Index):
				updateIndexReferences(oldEntityId, *static_cast<const Index *>(newEntity));
				break;
			case toUnderlying(SegmentType::State):
				updateStateReferences(oldEntityId, *static_cast<const State *>(newEntity));
				break;
			default:;
		}
	}

	void EntityReferenceUpdater::updateNodeReferences(int64_t oldNodeId, const Node &newNode) {
		int64_t newNodeId = newNode.getId();
		if (oldNodeId == newNodeId)
			return;

		auto dm = dataManager_.lock();
		if (!dm)
			return;

		// 1. Update Outgoing Edges
		// We traverse the linked list of edges using DataManager
		int64_t currentEdgeId = newNode.getFirstOutEdgeId();
		while (currentEdgeId != 0) {
			Edge edge = dm->getEdge(currentEdgeId);
			if (edge.getId() == 0 || !edge.isActive())
				break;

			if (edge.getSourceNodeId() == oldNodeId) {
				edge.setSourceNodeId(newNodeId);
				dm->updateEdge(edge); // This handles dirty marking and caching
			}
			currentEdgeId = edge.getNextOutEdgeId();
		}

		// 2. Update Incoming Edges
		currentEdgeId = newNode.getFirstInEdgeId();
		while (currentEdgeId != 0) {
			Edge edge = dm->getEdge(currentEdgeId);
			if (edge.getId() == 0 || !edge.isActive())
				break;

			if (edge.getTargetNodeId() == oldNodeId) {
				edge.setTargetNodeId(newNodeId);
				dm->updateEdge(edge);
			}
			currentEdgeId = edge.getNextInEdgeId();
		}

		// 3. Update Properties
		updatePropertiesPointingToEntity(oldNodeId, newNode);
	}

	void EntityReferenceUpdater::updateEdgeReferences(int64_t oldEdgeId, const Edge &newEdge) {
		int64_t newEdgeId = newEdge.getId();
		if (oldEdgeId == newEdgeId)
			return;

		auto dm = dataManager_.lock();
		if (!dm)
			return;

		// 1. Update Source Node
		int64_t srcId = newEdge.getSourceNodeId();
		if (srcId != 0) {
			Node srcNode = dm->getNode(srcId);
			if (srcNode.getId() != 0 && srcNode.isActive() && srcNode.getFirstOutEdgeId() == oldEdgeId) {
				srcNode.setFirstOutEdgeId(newEdgeId);
				dm->updateNode(srcNode);
			}
		}

		// 2. Update Target Node
		int64_t dstId = newEdge.getTargetNodeId();
		if (dstId != 0) {
			Node dstNode = dm->getNode(dstId);
			if (dstNode.getId() != 0 && dstNode.isActive() && dstNode.getFirstInEdgeId() == oldEdgeId) {
				dstNode.setFirstInEdgeId(newEdgeId);
				dm->updateNode(dstNode);
			}
		}

		// 3. Update Linked Edges (Prev/Next)
		auto updateNeighbor = [&](int64_t neighborId, bool isPrev, bool isOutChain) {
			if (neighborId == 0)
				return;
			Edge neighbor = dm->getEdge(neighborId);
			if (neighbor.getId() == 0 || !neighbor.isActive())
				return;

			bool changed = false;
			if (isOutChain) {
				if (isPrev && neighbor.getNextOutEdgeId() == oldEdgeId) {
					neighbor.setNextOutEdgeId(newEdgeId);
					changed = true;
				} else if (!isPrev && neighbor.getPrevOutEdgeId() == oldEdgeId) {
					neighbor.setPrevOutEdgeId(newEdgeId);
					changed = true;
				}
			} else { // InChain
				if (isPrev && neighbor.getNextInEdgeId() == oldEdgeId) {
					neighbor.setNextInEdgeId(newEdgeId);
					changed = true;
				} else if (!isPrev && neighbor.getPrevInEdgeId() == oldEdgeId) {
					neighbor.setPrevInEdgeId(newEdgeId);
					changed = true;
				}
			}

			if (changed)
				dm->updateEdge(neighbor);
		};

		updateNeighbor(newEdge.getPrevOutEdgeId(), true, true);
		updateNeighbor(newEdge.getNextOutEdgeId(), false, true);
		updateNeighbor(newEdge.getPrevInEdgeId(), true, false);
		updateNeighbor(newEdge.getNextInEdgeId(), false, false);

		// 4. Update Properties
		updatePropertiesPointingToEntity(oldEdgeId, newEdge);
	}

	void EntityReferenceUpdater::updatePropertyReferences(int64_t oldPropId, const Property &newProperty) const {
		int64_t newPropId = newProperty.getId();
		if (oldPropId == newPropId)
			return;

		auto dm = dataManager_.lock();
		if (!dm)
			return;

		int64_t entityId = newProperty.getEntityId();
		uint32_t entityType = newProperty.getEntityType();

		if (entityType == toUnderlying(SegmentType::Node)) {
			Node node = dm->getNode(entityId);
			if (node.getId() != 0 && node.isActive() && node.getPropertyEntityId() == oldPropId &&
				node.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {

				node.setPropertyEntityId(newPropId, PropertyStorageType::PROPERTY_ENTITY);
				dm->updateNode(node);
			}
		} else if (entityType == toUnderlying(SegmentType::Edge)) {
			Edge edge = dm->getEdge(entityId);
			if (edge.getId() != 0 && edge.isActive() && edge.getPropertyEntityId() == oldPropId &&
				edge.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {

				edge.setPropertyEntityId(newPropId, PropertyStorageType::PROPERTY_ENTITY);
				dm->updateEdge(edge);
			}
		}
	}

	void EntityReferenceUpdater::updateBlobReferences(int64_t oldBlobId, const Blob &newBlob) const {
		int64_t newBlobId = newBlob.getId();
		if (oldBlobId == newBlobId)
			return;

		auto dm = dataManager_.lock();
		if (!dm)
			return;

		// 1. Update Owner
		int64_t entityId = newBlob.getEntityId();
		uint32_t entityType = newBlob.getEntityType();

		if (entityType == toUnderlying(SegmentType::Node)) {
			Node node = dm->getNode(entityId);
			if (node.getId() != 0 && node.isActive() && node.getPropertyEntityId() == oldBlobId &&
				node.getPropertyStorageType() == PropertyStorageType::BLOB_ENTITY) {

				node.setPropertyEntityId(newBlobId, PropertyStorageType::BLOB_ENTITY);
				dm->updateNode(node);
			}
		} else if (entityType == toUnderlying(SegmentType::Edge)) {
			Edge edge = dm->getEdge(entityId);
			if (edge.getId() != 0 && edge.isActive() && edge.getPropertyEntityId() == oldBlobId &&
				edge.getPropertyStorageType() == PropertyStorageType::BLOB_ENTITY) {

				edge.setPropertyEntityId(newBlobId, PropertyStorageType::BLOB_ENTITY);
				dm->updateEdge(edge);
			}
		}

		// 2. Update Chain
		int64_t nextBlobId = newBlob.getNextBlobId();
		if (nextBlobId != 0)
			updateBlobChainReference(oldBlobId, newBlobId, nextBlobId, true, newBlob);

		int64_t prevBlobId = newBlob.getPrevBlobId();
		if (prevBlobId != 0)
			updateBlobChainReference(oldBlobId, newBlobId, prevBlobId, false, newBlob);
	}

	void EntityReferenceUpdater::updateIndexReferences(int64_t oldIndexId, const Index &newIndex) {
		int64_t newIndexId = newIndex.getId();
		if (oldIndexId == newIndexId)
			return;

		// 1. Update Parent
		int64_t parentId = newIndex.getParentId();
		if (parentId != 0) {
			updateIndexChildId(parentId, oldIndexId, newIndexId);
		}

		// 2. Update Siblings (Leaf only)
		if (newIndex.isLeaf()) {
			int64_t prevLeafId = newIndex.getPrevLeafId();
			if (prevLeafId != 0) {
				updateIndexSiblingPtr(prevLeafId, oldIndexId, newIndexId, true);
			}

			int64_t nextLeafId = newIndex.getNextLeafId();
			if (nextLeafId != 0) {
				updateIndexSiblingPtr(nextLeafId, oldIndexId, newIndexId, false);
			}
		} else {
			// 3. Update Children (Internal only)
			// Use the new helper method in Index class
			// We iterate over the children IDs and update their parent pointer
			std::vector<int64_t> childIds = newIndex.getChildIds();
			for (int64_t childId: childIds) {
				updateIndexParentPtr(childId, newIndexId);
			}
		}
	}

	void EntityReferenceUpdater::updateStateReferences(int64_t oldStateId, const State &newState) {
		int64_t newStateId = newState.getId();
		if (oldStateId == newStateId)
			return;

		int64_t prevStateId = newState.getPrevStateId();
		if (prevStateId != 0) {
			updateStateChainReference(prevStateId, oldStateId, newStateId, true);
		}

		int64_t nextStateId = newState.getNextStateId();
		if (nextStateId != 0) {
			updateStateChainReference(nextStateId, oldStateId, newStateId, false);
		}
	}

	// ========================================================================
	// Helpers
	// ========================================================================

	void EntityReferenceUpdater::updateBlobChainReference(int64_t oldBlobId, int64_t newBlobId, int64_t linkedBlobId,
														  bool isNextBlob, const Blob &sourceBlob) const {
		auto dm = dataManager_.lock();
		if (!dm)
			return;

		Blob linkedBlob = dm->getBlob(linkedBlobId);
		if (linkedBlob.getId() == 0 || !linkedBlob.isActive())
			return;

		bool updated = false;
		if (isNextBlob) {
			if (linkedBlob.getPrevBlobId() == oldBlobId) {
				linkedBlob.setPrevBlobId(newBlobId);
				updated = true;
			}
		} else {
			if (linkedBlob.getNextBlobId() == oldBlobId) {
				linkedBlob.setNextBlobId(newBlobId);
				updated = true;
			}
		}

		// Sync entity info
		if (linkedBlob.getEntityId() != sourceBlob.getEntityId() ||
			linkedBlob.getEntityType() != sourceBlob.getEntityType()) {
			linkedBlob.setEntityInfo(sourceBlob.getEntityId(), sourceBlob.getEntityType());
			updated = true;
		}

		if (updated) {
			dm->updateBlobEntity(linkedBlob);
		}
	}

	void EntityReferenceUpdater::updateIndexParentPtr(int64_t childId, int64_t newParentId) const {
		auto dm = dataManager_.lock();
		if (!dm)
			return;

		Index child = dm->getIndex(childId);
		if (child.getId() != 0 && child.isActive()) {
			// Only update if parent matches old parent?
			// Here we know the parent MOVED, so child's parent pointer MUST be updated.
			if (child.getParentId() != newParentId) {
				child.setParentId(newParentId);
				dm->updateIndexEntity(child);
			}
		}
	}

	void EntityReferenceUpdater::updateIndexSiblingPtr(int64_t siblingId, int64_t oldId, int64_t newId,
													   bool updateNext) const {
		auto dm = dataManager_.lock();
		if (!dm)
			return;

		Index sibling = dm->getIndex(siblingId);
		if (sibling.getId() != 0 && sibling.isActive()) {
			bool changed = false;
			if (updateNext) {
				if (sibling.getNextLeafId() == oldId) {
					sibling.setNextLeafId(newId);
					changed = true;
				}
			} else {
				if (sibling.getPrevLeafId() == oldId) {
					sibling.setPrevLeafId(newId);
					changed = true;
				}
			}
			if (changed)
				dm->updateIndexEntity(sibling);
		}
	}

	void EntityReferenceUpdater::updateIndexChildId(int64_t parentId, int64_t oldId, int64_t newId) const {
		auto dm = dataManager_.lock();
		if (!dm)
			return;

		Index parent = dm->getIndex(parentId);
		if (parent.getId() != 0 && parent.isActive()) {
			// Uses the safe method we added to Index class
			if (parent.updateChildId(oldId, newId)) {
				dm->updateIndexEntity(parent);
			}
		}
	}

	void EntityReferenceUpdater::updateStateChainReference(int64_t targetStateId, int64_t oldId, int64_t newId,
														   bool updateNext) const {
		auto dm = dataManager_.lock();
		if (!dm)
			return;

		State state = dm->getState(targetStateId);
		if (state.getId() != 0 && state.isActive()) {
			bool changed = false;
			if (updateNext) {
				if (state.getNextStateId() == oldId) {
					state.setNextStateId(newId);
					changed = true;
				}
			} else {
				if (state.getPrevStateId() == oldId) {
					state.setPrevStateId(newId);
					changed = true;
				}
			}
			if (changed)
				dm->updateStateEntity(state);
		}
	}

	template<typename T>
	EntityReferenceUpdater::EntityPropertyInfo
	EntityReferenceUpdater::getEntityPropertyInfoFromEntity(const T &entity) {
		return {entity.getPropertyStorageType(), entity.getPropertyEntityId()};
	}

	template<typename T>
	void EntityReferenceUpdater::updatePropertiesPointingToEntity(int64_t oldEntityId, const T &newEntity) {
		uint32_t entityType;
		if constexpr (std::is_same_v<T, Node>) {
			entityType = toUnderlying(SegmentType::Node);
		} else if constexpr (std::is_same_v<T, Edge>) {
			entityType = toUnderlying(SegmentType::Edge);
		} else {
			return;
		}

		auto info = getEntityPropertyInfoFromEntity(newEntity);
		if (info.storageType == PropertyStorageType::NONE || info.propertyEntityId == 0)
			return;

		auto dm = dataManager_.lock();
		if (!dm)
			return;

		int64_t newEntityId = newEntity.getId();

		if (info.storageType == PropertyStorageType::PROPERTY_ENTITY) {
			Property prop = dm->getProperty(info.propertyEntityId);
			if (prop.getId() != 0 && prop.isActive() && prop.getEntityId() == oldEntityId &&
				prop.getEntityType() == entityType) {

				prop.setEntityInfo(newEntityId, entityType);
				dm->updatePropertyEntity(prop);
			}
		} else if (info.storageType == PropertyStorageType::BLOB_ENTITY) {
			Blob blob = dm->getBlob(info.propertyEntityId);
			if (blob.getId() != 0 && blob.isActive() && blob.getEntityId() == oldEntityId &&
				blob.getEntityType() == entityType) {

				blob.setEntityInfo(newEntityId, entityType);
				dm->updateBlobEntity(blob);
			}
		}
	}

	// Template instantiations
	template void EntityReferenceUpdater::updatePropertiesPointingToEntity<Node>(int64_t, const Node &);
	template void EntityReferenceUpdater::updatePropertiesPointingToEntity<Edge>(int64_t, const Edge &);

} // namespace graph::storage
