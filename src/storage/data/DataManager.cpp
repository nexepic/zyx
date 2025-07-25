/**
 * @file DataManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/data/DataManager.hpp"
#include <algorithm>
#include "graph/core/BlobChainManager.hpp"
#include "graph/core/StateChainManager.hpp"
#include "graph/storage/DeletionManager.hpp"
#include "graph/storage/EntityReferenceUpdater.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/SegmentTracker.hpp"
#include "graph/storage/SpaceManager.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/data/EdgeManager.hpp"
#include "graph/storage/data/IndexEntityManager.hpp"
#include "graph/storage/data/NodeManager.hpp"
#include "graph/storage/data/PropertyEntityManager.hpp"
#include "graph/storage/data/PropertyManager.hpp"
#include "graph/storage/data/StateManager.hpp"
#include "graph/traversal/RelationshipTraversal.hpp"

namespace graph::storage {

	DataManager::DataManager(std::shared_ptr<std::fstream> file, size_t cacheSize, FileHeader &fileHeader,
							 std::shared_ptr<IDAllocator> idAllocator, std::shared_ptr<SegmentTracker> segmentTracker,
							 std::shared_ptr<SpaceManager> spaceManager) :
		file_(std::move(file)), fileHeader_(fileHeader), nodeCache_(cacheSize), edgeCache_(cacheSize),
		propertyCache_(cacheSize * 2), blobCache_(cacheSize / 4), indexCache_(cacheSize), stateCache_(cacheSize),
		idAllocator_(std::move(idAllocator)), segmentTracker_(std::move(segmentTracker)),
		spaceManager_(std::move(spaceManager)) {

		segmentIndexManager_ = std::make_shared<SegmentIndexManager>(segmentTracker_);
		segmentTracker_->setSegmentIndexManager(std::weak_ptr(segmentIndexManager_));
	}

	DataManager::~DataManager() {
		if (file_ && file_->is_open()) {
			file_->close();
		}
	}

	void DataManager::initialize() {
		// Initialize low-level components
		deletionManager_ = std::make_shared<DeletionManager>(shared_from_this(), spaceManager_);
		entityReferenceUpdater_ = std::make_shared<EntityReferenceUpdater>(file_, idAllocator_, segmentTracker_);
		spaceManager_->setEntityReferenceUpdater(entityReferenceUpdater_);
		relationshipTraversal_ = std::make_shared<traversal::RelationshipTraversal>(shared_from_this());

		// Initialize segment indexes
		initializeSegmentIndexes();

		// Create chain managers
		auto blobChainManager = std::make_shared<graph::BlobChainManager>(shared_from_this());
		auto stateChainManager = std::make_shared<graph::StateChainManager>(shared_from_this());

		// Initialize entity managers
		initializeManagers(blobChainManager, stateChainManager);
	}

	void DataManager::initializeSegmentIndexes() {
		// Initialize all segment indexes at once
		segmentIndexManager_->initialize(fileHeader_.node_segment_head, fileHeader_.edge_segment_head,
										 fileHeader_.property_segment_head, fileHeader_.blob_segment_head,
										 fileHeader_.index_segment_head, fileHeader_.state_segment_head);
	}

	void DataManager::initializeManagers(std::shared_ptr<graph::BlobChainManager> blobChainManager,
										 std::shared_ptr<graph::StateChainManager> stateChainManager) {
		// Create property manager first as others depend on it
		propertyManager_ = std::make_shared<PropertyManager>(shared_from_this());

		// Create entity managers
		nodeManager_ = std::make_shared<NodeManager>(shared_from_this(), propertyManager_, deletionManager_);
		edgeManager_ = std::make_shared<EdgeManager>(shared_from_this(), propertyManager_, deletionManager_);
		propertyEntityManager_ =
				std::make_shared<PropertyEntityManager>(shared_from_this(), propertyManager_, deletionManager_);
		blobEntityManager_ =
				std::make_shared<BlobManager>(shared_from_this(), propertyManager_, blobChainManager, deletionManager_);
		indexEntityManager_ = std::make_shared<IndexEntityManager>(shared_from_this(), propertyManager_, deletionManager_);
		stateEntityManager_ = std::make_shared<StateManager>(shared_from_this(), propertyManager_, stateChainManager,
															 deletionManager_);
	}

	// --- Node Operations (delegate to NodeManager) ---

	void DataManager::addNode(const Node &node) { nodeManager_->add(node); }

	void DataManager::updateNode(const Node &node) { nodeManager_->update(node); }

	void DataManager::deleteNode(Node &node) { nodeManager_->remove(node); }

	Node DataManager::getNode(int64_t id) { return nodeManager_->get(id); }

	std::vector<Node> DataManager::getNodeBatch(const std::vector<int64_t> &ids) { return nodeManager_->getBatch(ids); }

	std::vector<Node> DataManager::getNodesInRange(int64_t startId, int64_t endId, size_t limit) {
		return nodeManager_->getInRange(startId, endId, limit);
	}

	void DataManager::addNodeProperties(int64_t nodeId,
										const std::unordered_map<std::string, PropertyValue> &properties) {
		nodeManager_->addProperties(nodeId, properties);
	}

	void DataManager::removeNodeProperty(int64_t nodeId, const std::string &key) {
		nodeManager_->removeProperty(nodeId, key);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getNodeProperties(int64_t nodeId) {
		return nodeManager_->getProperties(nodeId);
	}

	int64_t DataManager::reserveTemporaryNodeId() { return idAllocator_->reserveTemporaryId(Node::typeId); }

	// --- Edge Operations (delegate to EdgeManager) ---

	void DataManager::addEdge(Edge &edge) { edgeManager_->add(edge); }

	void DataManager::updateEdge(const Edge &edge) { edgeManager_->update(edge); }

	void DataManager::deleteEdge(Edge &edge) { edgeManager_->remove(edge); }

	Edge DataManager::getEdge(int64_t id) { return edgeManager_->get(id); }

	std::vector<Edge> DataManager::getEdgeBatch(const std::vector<int64_t> &ids) { return edgeManager_->getBatch(ids); }

	std::vector<Edge> DataManager::getEdgesInRange(int64_t startId, int64_t endId, size_t limit) {
		return edgeManager_->getInRange(startId, endId, limit);
	}

	void DataManager::addEdgeProperties(int64_t edgeId,
										const std::unordered_map<std::string, PropertyValue> &properties) {
		edgeManager_->addProperties(edgeId, properties);
	}

	void DataManager::removeEdgeProperty(int64_t edgeId, const std::string &key) {
		edgeManager_->removeProperty(edgeId, key);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getEdgeProperties(int64_t edgeId) {
		return edgeManager_->getProperties(edgeId);
	}

	std::vector<Edge> DataManager::findEdgesByNode(int64_t nodeId, const std::string &direction) const {
		if (direction == "out") {
			return relationshipTraversal_->getOutgoingEdges(nodeId);
		} else if (direction == "in") {
			return relationshipTraversal_->getIncomingEdges(nodeId);
		} else { // "both" is the default
			return relationshipTraversal_->getAllConnectedEdges(nodeId);
		}
	}

	int64_t DataManager::reserveTemporaryEdgeId() { return idAllocator_->reserveTemporaryId(Edge::typeId); }

	// --- Property Entity Operations ---

	void DataManager::addPropertyEntity(const Property &property) { propertyEntityManager_->add(property); }

	void DataManager::updatePropertyEntity(const Property &property) { propertyEntityManager_->update(property); }

	void DataManager::deleteProperty(Property &property) { propertyEntityManager_->remove(property); }

	Property DataManager::getProperty(int64_t id) { return propertyEntityManager_->get(id); }

	int64_t DataManager::reserveTemporaryPropertyId() { return idAllocator_->reserveTemporaryId(Property::typeId); }

	// --- Blob Operations ---

	void DataManager::addBlobEntity(const Blob &blob) { blobEntityManager_->add(blob); }

	void DataManager::updateBlobEntity(const Blob &blob) { blobEntityManager_->update(blob); }

	void DataManager::deleteBlob(Blob &blob) { blobEntityManager_->remove(blob); }

	Blob DataManager::getBlob(int64_t id) { return blobEntityManager_->get(id); }

	int64_t DataManager::reserveTemporaryBlobId() { return idAllocator_->reserveTemporaryId(Blob::typeId); }

	// --- Index Operations ---

	void DataManager::addIndexEntity(const Index &index) { indexEntityManager_->add(index); }

	void DataManager::updateIndexEntity(const Index &index) { indexEntityManager_->update(index); }

	void DataManager::deleteIndex(Index &index) { indexEntityManager_->remove(index); }

	Index DataManager::getIndex(int64_t id) { return indexEntityManager_->get(id); }

	int64_t DataManager::reserveTemporaryIndexId() { return idAllocator_->reserveTemporaryId(Index::typeId); }

	// --- State Operations ---

	void DataManager::addStateEntity(const State &state) { stateEntityManager_->add(state); }

	void DataManager::updateStateEntity(const State &state) { stateEntityManager_->update(state); }

	void DataManager::deleteState(State &state) { stateEntityManager_->remove(state); }

	State DataManager::getState(int64_t id) { return stateEntityManager_->get(id); }

	int64_t DataManager::reserveTemporaryStateId() { return idAllocator_->reserveTemporaryId(State::typeId); }

	std::vector<State> DataManager::getAllStates() { return stateEntityManager_->getAllHeadStates(); }

	State DataManager::findStateByKey(const std::string &key) { return stateEntityManager_->findByKey(key); }

	void DataManager::addStateProperties(const std::string &stateKey,
										 const std::unordered_map<std::string, PropertyValue> &properties) {
		stateEntityManager_->addStateProperties(stateKey, properties);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getStateProperties(const std::string &stateKey) {
		return stateEntityManager_->getStateProperties(stateKey);
	}

	void DataManager::removeState(const std::string &stateKey) { stateEntityManager_->removeState(stateKey); }

	bool DataManager::isChainHeadState(const State &state) {
		return state.getId() != 0 && state.isActive() && state.getPrevStateId() == 0;
	}

	// --- Property Management Delegations ---

	uint32_t
	DataManager::calculateSerializedSize(const std::unordered_map<std::string, PropertyValue> &properties) const {
		return propertyManager_->calculateSerializedSize(properties);
	}

	void DataManager::serializeProperties(std::ostream &os,
										  const std::unordered_map<std::string, PropertyValue> &properties) const {
		propertyManager_->serializeProperties(os, properties);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::deserializeProperties(std::istream &is) const {
		return propertyManager_->deserializeProperties(is);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getPropertiesFromBlob(int64_t blobId) {
		return propertyManager_->getPropertiesFromBlob(blobId);
	}

	// --- Template Method Implementations ---

	template<typename EntityType>
	void DataManager::cleanupExternalProperties(EntityType &entity) {
		propertyManager_->cleanupExternalProperties(entity);
	}

	template<typename EntityType>
	void DataManager::addToCache(const EntityType &entity) {
		EntityTraits<EntityType>::addToCache(this, entity);
	}

	template<typename EntityType>
	EntityType DataManager::getEntity(int64_t id) {
		return EntityTraits<EntityType>::get(this, id);
	}

	template<typename EntityType>
	void DataManager::addEntity(const EntityType &entity) {
		if constexpr (std::is_same_v<EntityType, Node>) {
			addNode(entity);
		} else if constexpr (std::is_same_v<EntityType, Edge>) {
			// Need to make a copy as addEdge takes a non-const reference
			Edge edgeCopy = entity;
			addEdge(edgeCopy);
		} else if constexpr (std::is_same_v<EntityType, Property>) {
			addPropertyEntity(entity);
		} else if constexpr (std::is_same_v<EntityType, Blob>) {
			addBlobEntity(entity);
		} else if constexpr (std::is_same_v<EntityType, Index>) {
			addIndexEntity(entity);
		} else if constexpr (std::is_same_v<EntityType, State>) {
			addStateEntity(entity);
		}
	}

	template<typename EntityType>
	void DataManager::updateEntity(const EntityType &entity) {
		if constexpr (std::is_same_v<EntityType, Node>) {
			updateNode(entity);
		} else if constexpr (std::is_same_v<EntityType, Edge>) {
			updateEdge(entity);
		} else if constexpr (std::is_same_v<EntityType, Property>) {
			updatePropertyEntity(entity);
		} else if constexpr (std::is_same_v<EntityType, Blob>) {
			updateBlobEntity(entity);
		} else if constexpr (std::is_same_v<EntityType, Index>) {
			updateIndexEntity(entity);
		} else if constexpr (std::is_same_v<EntityType, State>) {
			updateStateEntity(entity);
		}
	}

	template<typename EntityType>
	void DataManager::deleteEntity(EntityType &entity) {
		if constexpr (std::is_same_v<EntityType, Node>) {
			deleteNode(entity);
		} else if constexpr (std::is_same_v<EntityType, Edge>) {
			deleteEdge(entity);
		} else if constexpr (std::is_same_v<EntityType, Property>) {
			deleteProperty(entity);
		} else if constexpr (std::is_same_v<EntityType, Blob>) {
			deleteBlob(entity);
		} else if constexpr (std::is_same_v<EntityType, Index>) {
			deleteIndex(entity);
		} else if constexpr (std::is_same_v<EntityType, State>) {
			deleteState(entity);
		}
	}

	template<typename EntityType>
	std::vector<EntityType> DataManager::getEntitiesInRange(int64_t startId, int64_t endId, size_t limit) {
		if constexpr (std::is_same_v<EntityType, Node>) {
			return getNodesInRange(startId, endId, limit);
		} else if constexpr (std::is_same_v<EntityType, Edge>) {
			return getEdgesInRange(startId, endId, limit);
		} else {
			// For other entity types, implement a generic version
			std::vector<EntityType> result;
			if (startId > endId || limit == 0) {
				return result;
			}

			// Find relevant segments
			std::vector<uint64_t> relevantSegments;
			auto entityType = EntityType::typeId;
			auto segments = segmentTracker_->getSegmentsByType(entityType);

			for (const auto &header: segments) {
				int64_t segmentStartId = header.start_id;
				int64_t segmentEndId = header.start_id + header.used - 1;

				if (segmentStartId <= endId && segmentEndId >= startId) {
					relevantSegments.push_back(header.file_offset);
				}
			}

			// Load entities from segments
			for (uint64_t segmentOffset: relevantSegments) {
				auto segmentEntities =
						loadEntitiesFromSegment<EntityType>(segmentOffset, startId, endId, limit - result.size());

				for (const EntityType &entity: segmentEntities) {
					result.push_back(entity);
					addToCache(entity);
				}

				if (result.size() >= limit) {
					break;
				}
			}

			return result;
		}
	}

	template<typename EntityType>
	size_t DataManager::calculateEntityTotalPropertySize(int64_t entityId) {
		return propertyManager_->calculateEntityTotalPropertySize<EntityType>(entityId);
	}

	template<typename EntityType>
	void DataManager::addEntityProperties(int64_t entityId,
										  const std::unordered_map<std::string, PropertyValue> &properties) {
		propertyManager_->addEntityProperties<EntityType>(entityId, properties);
	}

	template<typename EntityType>
	std::unordered_map<std::string, PropertyValue> DataManager::getEntityProperties(int64_t entityId) {
		return propertyManager_->getEntityProperties<EntityType>(entityId);
	}

	template<typename EntityType>
	uint64_t DataManager::findSegmentForEntityId(int64_t id) const {
		uint32_t type = EntityType::typeId;
		return segmentIndexManager_->findSegmentForId(type, id);
	}

	template<typename EntityType>
	std::vector<EntityType> DataManager::getDirtyEntitiesWithChangeTypes(const std::vector<EntityChangeType> &types) {
		auto &dirtyMap = EntityTraits<EntityType>::getDirtyMap(this);
		std::vector<EntityType> result;

		for (const auto &[id, info]: dirtyMap) {
			if (std::find(types.begin(), types.end(), info.changeType) != types.end()) {
				if (info.backup.has_value()) {
					result.push_back(*info.backup);
				} else {
					auto &cache = EntityTraits<EntityType>::getCache(this);
					if (cache.contains(id)) {
						result.push_back(cache.peek(id));
					}
				}
			}
		}

		return result;
	}

	std::vector<Node> DataManager::getDirtyNodesWithChangeTypes(const std::vector<EntityChangeType> &types) const {
		std::vector<Node> result;

		for (const auto &[id, info]: dirtyNodes_) {
			if (std::find(types.begin(), types.end(), info.changeType) != types.end()) {
				if (info.backup.has_value()) {
					result.push_back(*info.backup);
				} else if (nodeCache_.contains(id)) {
					result.push_back(nodeCache_.peek(id));
				}
			}
		}

		return result;
	}

	std::vector<Edge> DataManager::getDirtyEdgesWithChangeTypes(const std::vector<EntityChangeType> &types) const {
		std::vector<Edge> result;

		for (const auto &[id, info]: dirtyEdges_) {
			if (std::find(types.begin(), types.end(), info.changeType) != types.end()) {
				if (info.backup.has_value()) {
					result.push_back(*info.backup);
				} else if (edgeCache_.contains(id)) {
					result.push_back(edgeCache_.peek(id));
				}
			}
		}

		return result;
	}

	std::vector<Property>
	DataManager::getDirtyPropertiesWithChangeTypes(const std::vector<EntityChangeType> &types) const {
		std::vector<Property> result;

		for (const auto &[id, info]: dirtyProperties_) {
			if (std::find(types.begin(), types.end(), info.changeType) != types.end()) {
				if (info.backup.has_value()) {
					result.push_back(*info.backup);
				} else if (propertyCache_.contains(id)) {
					result.push_back(propertyCache_.peek(id));
				}
			}
		}

		return result;
	}

	std::vector<Blob> DataManager::getDirtyBlobsWithChangeTypes(const std::vector<EntityChangeType> &types) const {
		std::vector<Blob> result;

		for (const auto &[id, info]: dirtyBlobs_) {
			if (std::find(types.begin(), types.end(), info.changeType) != types.end()) {
				if (info.backup.has_value()) {
					result.push_back(*info.backup);
				} else if (blobCache_.contains(id)) {
					result.push_back(blobCache_.peek(id));
				}
			}
		}

		return result;
	}

	// --- Entity Loading from Disk ---

	template<typename EntityType>
	std::vector<EntityType> DataManager::loadEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
																 size_t limit) const {
		return readEntitiesFromSegment<EntityType>(segmentOffset, startId, endId, limit);
	}

	template<typename EntityType>
	std::optional<EntityType> DataManager::readEntityFromDisk(int64_t fileOffset) const {
		file_->seekg(static_cast<std::streamoff>(fileOffset));
		EntityType entity = EntityType::deserialize(*file_);

		// Check if the entity is marked as inactive
		if (!entity.isActive()) {
			return std::nullopt; // Return empty optional if marked as inactive
		}

		return entity;
	}

	template<typename EntityType>
	std::optional<EntityType> DataManager::findAndReadEntity(int64_t id) const {
		// Find segment for this entity type and ID
		uint64_t segmentOffset = findSegmentForEntityId<EntityType>(id);

		if (segmentOffset == 0) {
			return std::nullopt; // Segment not found
		}

		// Read segment header
		SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);

		// Calculate position of entity within segment
		uint64_t relativePosition = id - header.start_id;
		if (relativePosition >= header.used) {
			return std::nullopt; // ID is out of range for this segment
		}

		// Check if the entity is active
		if (!segmentTracker_->isEntityActive(segmentOffset, relativePosition)) {
			return std::nullopt; // Entity is marked as inactive
		}

		// Calculate file offset for this entity
		auto entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader) +
														relativePosition * EntityType::getTotalSize());

		return readEntityFromDisk<EntityType>(entityOffset);
	}

	template<typename EntityType>
	std::vector<EntityType> DataManager::readEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
																 size_t limit, bool filterDeleted) const {
		std::vector<EntityType> result;
		if (segmentOffset == 0 || limit == 0) {
			return result;
		}

		// Read segment header
		SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);

		// Calculate effective start and end positions
		uint64_t effectiveStartId = std::max(startId, header.start_id);
		uint64_t effectiveEndId = std::min(endId, header.start_id + header.used - 1);

		if (effectiveStartId > effectiveEndId) {
			return result;
		}

		// Calculate offsets
		uint64_t startOffset = effectiveStartId - header.start_id;
		uint64_t count = std::min(effectiveEndId - effectiveStartId + 1, static_cast<uint64_t>(limit));

		// Reserve space for the maximum possible entities
		result.reserve(count);

		// Seek to the first entity
		auto entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader) +
														startOffset * EntityType::getTotalSize());

		// Read entities
		for (uint64_t i = 0; i < count; ++i) {
			if (filterDeleted) {
				auto entityOpt = readEntityFromDisk<EntityType>(entityOffset);
				if (entityOpt.has_value()) {
					result.push_back(entityOpt.value());
				}
			} else {
				// If we're not filtering deleted entities, read directly
				file_->seekg(entityOffset);
				EntityType entity = EntityType::deserialize(*file_);
				result.push_back(entity);
			}

			// Move to next entity
			entityOffset += EntityType::getTotalSize();
		}

		return result;
	}

	// --- Entity Memory Operations ---

	template<typename EntityType>
	std::optional<EntityType> DataManager::getEntityFromDirty(int64_t id) {
		auto &dirtyMap = EntityTraits<EntityType>::getDirtyMap(this);
		auto it = dirtyMap.find(id);

		if (it != dirtyMap.end()) {
			const auto &info = it->second;
			if (info.changeType != EntityChangeType::DELETED) {
				if (info.backup.has_value()) {
					return info.backup;
				} else {
					auto &cache = EntityTraits<EntityType>::getCache(this);
					if (cache.contains(id)) {
						return cache.peek(id);
					}
				}
			}
		}
		return std::nullopt;
	}

	template<typename EntityType>
	EntityType DataManager::getEntityFromMemoryOrDisk(int64_t id) {
		// First check dirty collections
		auto dirtyEntity = getEntityFromDirty<EntityType>(id);
		if (dirtyEntity.has_value()) {
			return *dirtyEntity;
		}

		// Then check cache
		auto &cache = EntityTraits<EntityType>::getCache(this);
		if (cache.contains(id)) {
			EntityType entity = cache.get(id);
			// Check if entity is active
			if (!entity.isActive()) {
				return EntityType(); // Return empty entity if inactive
			}
			return entity;
		}

		// Finally, load from disk
		EntityType entity = EntityTraits<EntityType>::loadFromDisk(this, id);
		if (entity.getId() != 0 && entity.isActive()) {
			// Add to cache
			cache.put(id, entity);
			return entity;
		}

		// Return empty entity if not found
		return EntityType();
	}

	// --- Loading Entities from Disk ---

	Node DataManager::loadNodeFromDisk(int64_t id) const {
		auto nodeOpt = findAndReadEntity<Node>(id);
		return nodeOpt.value_or(Node()); // Return empty node if not found or deleted
	}

	Edge DataManager::loadEdgeFromDisk(int64_t id) const {
		auto edgeOpt = findAndReadEntity<Edge>(id);
		return edgeOpt.value_or(Edge()); // Return empty edge if not found or deleted
	}

	Property DataManager::loadPropertyFromDisk(int64_t id) const {
		auto propertyOpt = findAndReadEntity<Property>(id);
		return propertyOpt.value_or(Property()); // Return empty property if not found or deleted
	}

	Blob DataManager::loadBlobFromDisk(int64_t id) const {
		auto blobOpt = findAndReadEntity<Blob>(id);
		return blobOpt.value_or(Blob()); // Return empty blob if not found or deleted
	}

	Index DataManager::loadIndexFromDisk(int64_t id) const {
		auto indexOpt = findAndReadEntity<Index>(id);
		return indexOpt.value_or(Index()); // Return empty index if not found or deleted
	}

	State DataManager::loadStateFromDisk(int64_t id) const {
		auto stateOpt = findAndReadEntity<State>(id);
		return stateOpt.value_or(State()); // Return empty state if not found or deleted
	}

	// --- Cache and Transaction Management ---

	void DataManager::clearCache() {
		nodeCache_.clear();
		edgeCache_.clear();
		propertyCache_.clear();
		blobCache_.clear();
		indexCache_.clear();
		stateCache_.clear();
	}

	bool DataManager::hasUnsavedChanges() const {
		return !dirtyNodes_.empty() || !dirtyEdges_.empty() || !dirtyProperties_.empty() || !dirtyBlobs_.empty() ||
			   !dirtyIndexes_.empty() || !dirtyStates_.empty();
	}

	void DataManager::markAllSaved() {
		dirtyNodes_.clear();
		dirtyEdges_.clear();
		dirtyProperties_.clear();
		dirtyBlobs_.clear();
		dirtyIndexes_.clear();
		dirtyStates_.clear();
	}

	void DataManager::flushToDisk(std::fstream &file) {
		// This would contain the logic to write all dirty entities to disk
		// and update the file header/indexes
		// Implementation details depend on how you want to handle persistence
	}

	void DataManager::checkAndTriggerAutoFlush() const {
		if (needsAutoFlush() && autoFlushCallback_) {
			autoFlushCallback_();
		}
	}

	void DataManager::handleIdUpdate(int64_t tempId, int64_t permId, uint32_t entityType) {
		// Skip if temp and perm IDs are the same
		if (tempId == permId)
			return;

		// Update the appropriate cache and dirty collections based on entity type
		if (entityType == Node::typeId) {
			updateEntityId<Node>(tempId, permId);
		} else if (entityType == Edge::typeId) {
			updateEntityId<Edge>(tempId, permId);
		} else if (entityType == Property::typeId) {
			updateEntityId<Property>(tempId, permId);
		} else if (entityType == Blob::typeId) {
			updateEntityId<Blob>(tempId, permId);
		} else if (entityType == Index::typeId) {
			updateEntityId<Index>(tempId, permId);
		} else if (entityType == State::typeId) {
			updateEntityId<State>(tempId, permId);
		} else {
			throw std::runtime_error("Unknown entity type for ID update: " + std::to_string(entityType));
		}
	}

	template<typename EntityType>
	void DataManager::updateEntityId(int64_t tempId, int64_t permId) {
		// Update in cache if it exists
		auto &cache = EntityTraits<EntityType>::getCache(this);
		if (cache.contains(tempId)) {
			EntityType entity = cache.get(tempId);
			entity.setPermanentId(permId);
			cache.remove(tempId);
			cache.put(permId, entity);
		}

		// Update in dirty map
		auto &dirtyMap = EntityTraits<EntityType>::getDirtyMap(this);
		auto it = dirtyMap.find(tempId);
		if (it != dirtyMap.end() && it->second.changeType == EntityChangeType::ADDED) {
			if (it->second.backup) {
				EntityType entity = *it->second.backup;
				entity.setPermanentId(permId);
				dirtyMap[permId] = DirtyEntityInfo<EntityType>(EntityChangeType::ADDED, entity);
				dirtyMap.erase(tempId);
			}
		}
	}

	template<typename EntityType>
	void DataManager::markEntityDeleted(EntityType &entity) {
		// Update in-memory state without calling back to DeletionManager
		entity.markInactive();
		auto &dirtyMap = EntityTraits<EntityType>::getDirtyMap(this);
		dirtyMap[entity.getId()] = DirtyEntityInfo<EntityType>(EntityChangeType::DELETED, entity);
		EntityTraits<EntityType>::removeFromCache(this, entity.getId());
		checkAndTriggerAutoFlush();
	}

	template<typename EntityType>
	void DataManager::removeEntityProperty(int64_t entityId, const std::string &key) {
		propertyManager_->removeEntityProperty<EntityType>(entityId, key);
	}

	// --- Template Instantiations ---

	// Node-specific instantiations
	template void DataManager::cleanupExternalProperties<Node>(Node &entity);
	template void DataManager::addToCache<Node>(const Node &entity);
	template Node DataManager::getEntity<Node>(int64_t id);
	template void DataManager::addEntity<Node>(const Node &entity);
	template void DataManager::updateEntity<Node>(const Node &entity);
	template void DataManager::deleteEntity<Node>(Node &entity);
	template std::vector<Node> DataManager::getEntitiesInRange<Node>(int64_t, int64_t, size_t);
	template size_t DataManager::calculateEntityTotalPropertySize<Node>(int64_t entityId);
	template void DataManager::addEntityProperties<Node>(int64_t,
														 const std::unordered_map<std::string, PropertyValue> &);
	template std::unordered_map<std::string, PropertyValue> DataManager::getEntityProperties<Node>(int64_t entityId);
	template uint64_t DataManager::findSegmentForEntityId<Node>(int64_t id) const;
	template std::vector<Node>
	DataManager::getDirtyEntitiesWithChangeTypes<Node>(const std::vector<EntityChangeType> &);
	template std::optional<Node> DataManager::getEntityFromDirty<Node>(int64_t id);
	template Node DataManager::getEntityFromMemoryOrDisk<Node>(int64_t id);
	template void DataManager::removeEntityProperty<Node>(int64_t entityId, const std::string &key);
	template void DataManager::markEntityDeleted<Node>(Node &entity);
	template void DataManager::updateEntityId<Node>(int64_t tempId, int64_t permId);
	template std::vector<Node> DataManager::loadEntitiesFromSegment<Node>(uint64_t, int64_t, int64_t, size_t) const;
	template std::optional<Node> DataManager::readEntityFromDisk<Node>(int64_t fileOffset) const;
	template std::optional<Node> DataManager::findAndReadEntity<Node>(int64_t id) const;
	template std::vector<Node> DataManager::readEntitiesFromSegment<Node>(uint64_t, int64_t, int64_t, size_t,
																		  bool) const;

	// Edge-specific instantiations
	template void DataManager::cleanupExternalProperties<Edge>(Edge &entity);
	template void DataManager::addToCache<Edge>(const Edge &entity);
	template Edge DataManager::getEntity<Edge>(int64_t id);
	template void DataManager::addEntity<Edge>(const Edge &entity);
	template void DataManager::updateEntity<Edge>(const Edge &entity);
	template void DataManager::deleteEntity<Edge>(Edge &entity);
	template std::vector<Edge> DataManager::getEntitiesInRange<Edge>(int64_t, int64_t, size_t);
	template size_t DataManager::calculateEntityTotalPropertySize<Edge>(int64_t entityId);
	template void DataManager::addEntityProperties<Edge>(int64_t,
														 const std::unordered_map<std::string, PropertyValue> &);
	template std::unordered_map<std::string, PropertyValue> DataManager::getEntityProperties<Edge>(int64_t entityId);
	template uint64_t DataManager::findSegmentForEntityId<Edge>(int64_t id) const;
	template std::vector<Edge>
	DataManager::getDirtyEntitiesWithChangeTypes<Edge>(const std::vector<EntityChangeType> &);
	template std::optional<Edge> DataManager::getEntityFromDirty<Edge>(int64_t id);
	template Edge DataManager::getEntityFromMemoryOrDisk<Edge>(int64_t id);
	template void DataManager::removeEntityProperty<Edge>(int64_t entityId, const std::string &key);
	template void DataManager::markEntityDeleted<Edge>(Edge &entity);
	template void DataManager::updateEntityId<Edge>(int64_t tempId, int64_t permId);
	template std::vector<Edge> DataManager::loadEntitiesFromSegment<Edge>(uint64_t, int64_t, int64_t, size_t) const;
	template std::optional<Edge> DataManager::readEntityFromDisk<Edge>(int64_t fileOffset) const;
	template std::optional<Edge> DataManager::findAndReadEntity<Edge>(int64_t id) const;
	template std::vector<Edge> DataManager::readEntitiesFromSegment<Edge>(uint64_t, int64_t, int64_t, size_t,
																		  bool) const;

	// Property-specific instantiations
	template void DataManager::addToCache<Property>(const Property &entity);
	template Property DataManager::getEntityFromMemoryOrDisk<Property>(int64_t id);
	template std::vector<Property> DataManager::getEntitiesInRange<Property>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Property>(int64_t id) const;
	template std::vector<Property>
	DataManager::getDirtyEntitiesWithChangeTypes<Property>(const std::vector<EntityChangeType> &);
	template void DataManager::markEntityDeleted<Property>(Property &entity);

	// Blob-specific instantiations
	template Blob DataManager::getEntityFromMemoryOrDisk<Blob>(int64_t id);
	template std::vector<Blob> DataManager::getEntitiesInRange<Blob>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Blob>(int64_t id) const;
	template std::vector<Blob>
	DataManager::getDirtyEntitiesWithChangeTypes<Blob>(const std::vector<EntityChangeType> &);
	template void DataManager::markEntityDeleted<Blob>(Blob &entity);

	// Index-specific instantiations
	template Index DataManager::getEntityFromMemoryOrDisk<Index>(int64_t id);
	template std::vector<Index> DataManager::getEntitiesInRange<Index>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Index>(int64_t id) const;
	template std::vector<Index>
	DataManager::getDirtyEntitiesWithChangeTypes<Index>(const std::vector<EntityChangeType> &);
	template void DataManager::markEntityDeleted<Index>(Index &entity);

	// State-specific instantiations
	template State DataManager::getEntityFromMemoryOrDisk<State>(int64_t id);
	template std::vector<State> DataManager::getEntitiesInRange<State>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<State>(int64_t id) const;
	template std::vector<State>
	DataManager::getDirtyEntitiesWithChangeTypes<State>(const std::vector<EntityChangeType> &);
	template void DataManager::markEntityDeleted<State>(State &entity);


} // namespace graph::storage
