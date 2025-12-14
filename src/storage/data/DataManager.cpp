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
#include <map>
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
#include "graph/storage/data/EntityTraits.hpp"
#include "graph/storage/data/IndexEntityManager.hpp"
#include "graph/storage/data/NodeManager.hpp"
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

		persistenceManager_ = std::make_shared<PersistenceManager>();
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
		deletionManager_ = std::make_shared<DeletionManager>(shared_from_this(), spaceManager_, idAllocator_);
		entityReferenceUpdater_ = std::make_shared<EntityReferenceUpdater>(shared_from_this());
		spaceManager_->setEntityReferenceUpdater(entityReferenceUpdater_);
		relationshipTraversal_ = std::make_shared<traversal::RelationshipTraversal>(shared_from_this());

		// Initialize segment indexes
		initializeSegmentIndexes();

		// Initialize entity managers
		initializeManagers();
	}

	void DataManager::initializeSegmentIndexes() const {
		// Initialize all segment indexes at once
		segmentIndexManager_->initialize(fileHeader_.node_segment_head, fileHeader_.edge_segment_head,
										 fileHeader_.property_segment_head, fileHeader_.blob_segment_head,
										 fileHeader_.index_segment_head, fileHeader_.state_segment_head);
	}

	void DataManager::initializeManagers() {
		// Initialize chain managers
		blobChainManager_ = std::make_shared<BlobChainManager>(shared_from_this());
		stateChainManager_ = std::make_shared<StateChainManager>(shared_from_this());

		// Create property manager first as others depend on it
		propertyManager_ = std::make_shared<PropertyManager>(shared_from_this(), deletionManager_);

		// Create entity managers
		nodeManager_ = std::make_shared<NodeManager>(shared_from_this(), deletionManager_);
		edgeManager_ = std::make_shared<EdgeManager>(shared_from_this(), deletionManager_);
		blobManager_ = std::make_shared<BlobManager>(shared_from_this(), blobChainManager_, deletionManager_);
		indexEntityManager_ = std::make_shared<IndexEntityManager>(shared_from_this(), deletionManager_);
		stateManager_ = std::make_shared<StateManager>(shared_from_this(), stateChainManager_, deletionManager_);
	}

	void DataManager::registerObserver(std::shared_ptr<IEntityObserver> observer) {
		std::lock_guard<std::mutex> lock(observer_mutex_);
		observers_.push_back(std::move(observer));
	}

	// --- Notification Helper Implementations ---

	void DataManager::notifyNodeAdded(const Node &node) const {
		std::lock_guard<std::mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			observer->onNodeAdded(node);
		}
	}

	void DataManager::notifyNodeUpdated(const Node &oldNode, const Node &newNode) const {
		std::lock_guard<std::mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			observer->onNodeUpdated(oldNode, newNode);
		}
	}

	void DataManager::notifyNodeDeleted(const Node &node) const {
		std::lock_guard<std::mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			observer->onNodeDeleted(node);
		}
	}

	void DataManager::notifyEdgeAdded(const Edge &edge) const {
		std::lock_guard<std::mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			observer->onEdgeAdded(edge);
		}
	}

	void DataManager::notifyEdgeUpdated(const Edge &oldEdge, const Edge &newEdge) const {
		std::lock_guard<std::mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			observer->onEdgeUpdated(oldEdge, newEdge);
		}
	}

	void DataManager::notifyEdgeDeleted(const Edge &edge) const {
		std::lock_guard<std::mutex> lock(observer_mutex_);
		for (const auto &observer: observers_) {
			observer->onEdgeDeleted(edge);
		}
	}

	template<typename T>
	void DataManager::updateEntityImpl(const T &entity, std::function<T(int64_t)> getOldFunc,
									   std::function<void(T)> internalUpdateFunc,
									   std::function<void(const T &, const T &)> notifyFunc) {
		// Check dirty info via persistence manager
		auto dirtyInfo = persistenceManager_->getDirtyInfo<T>(entity.getId());

		// If entity is marked as ADDED, this update is part of creation.
		// Keep changeType as ADDED, do not trigger 'Updated' notification.
		if (dirtyInfo.has_value() && dirtyInfo->changeType == EntityChangeType::ADDED) {
			internalUpdateFunc(entity);
			return;
		}

		// Get old state
		T oldEntity = getOldFunc(entity.getId());

		// Perform update
		internalUpdateFunc(entity);

		// Notify
		if (notifyFunc)
			notifyFunc(oldEntity, entity);
	}

	// --- Node Operations (delegate to NodeManager) ---

	void DataManager::addNode(Node &node) const {
		nodeManager_->add(node);
		notifyNodeAdded(node);
	}

	void DataManager::updateNode(const Node &node) {
		updateEntityImpl<Node>(
				node, [this](int64_t id) { return nodeManager_->get(id); },
				[this](const Node &n) { nodeManager_->update(n); },
				[this](const Node &o, const Node &n) { notifyNodeUpdated(o, n); });
	}

	void DataManager::deleteNode(Node &node) const {
		nodeManager_->remove(node);
		notifyNodeDeleted(node);
	}

	Node DataManager::getNode(int64_t id) const { return nodeManager_->get(id); }

	std::vector<Node> DataManager::getNodeBatch(const std::vector<int64_t> &ids) const {
		return nodeManager_->getBatch(ids);
	}

	std::vector<Node> DataManager::getNodesInRange(int64_t startId, int64_t endId, size_t limit) const {
		return nodeManager_->getInRange(startId, endId, limit);
	}

	void DataManager::addNodeProperties(int64_t nodeId,
										const std::unordered_map<std::string, PropertyValue> &properties) const {
		// 1. Snapshot OLD state
		Node oldNode = nodeManager_->get(nodeId);
		// Manually fetch and bake the existing properties into the oldNode object.
		// This ensures oldNode.getProperties() returns the state BEFORE the modification.
		auto existingProps = nodeManager_->getProperties(nodeId);
		oldNode.setProperties(existingProps);

		// 2. Perform the modification
		nodeManager_->addProperties(nodeId, properties);

		// 3. Snapshot NEW state
		Node newNode = nodeManager_->get(nodeId);
		auto newProps = nodeManager_->getProperties(nodeId);
		newNode.setProperties(newProps);

		// 4. Notify with valid snapshots
		notifyNodeUpdated(oldNode, newNode);
	}

	void DataManager::removeNodeProperty(int64_t nodeId, const std::string &key) const {
		// 1. Snapshot OLD state
		Node oldNode = nodeManager_->get(nodeId);
		// Freeze old properties
		auto existingProps = nodeManager_->getProperties(nodeId);
		oldNode.setProperties(existingProps);

		// 2. Perform removal
		nodeManager_->removeProperty(nodeId, key);

		// 3. Snapshot NEW state
		Node newNode = nodeManager_->get(nodeId);
		auto newProps = nodeManager_->getProperties(nodeId);
		newNode.setProperties(newProps);

		// 4. Notify
		notifyNodeUpdated(oldNode, newNode);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getNodeProperties(int64_t nodeId) const {
		return nodeManager_->getProperties(nodeId);
	}

	// --- Edge Operations (delegate to EdgeManager) ---

	void DataManager::addEdge(Edge &edge) const {
		edgeManager_->add(edge);
		notifyEdgeAdded(edge);
	}

	void DataManager::updateEdge(const Edge &edge) {
		updateEntityImpl<Edge>(
				edge, [this](int64_t id) { return edgeManager_->get(id); },
				[this](const Edge &e) { edgeManager_->update(e); },
				[this](const Edge &o, const Edge &n) { notifyEdgeUpdated(o, n); });
	}

	void DataManager::deleteEdge(Edge &edge) const {
		edgeManager_->remove(edge);
		notifyEdgeDeleted(edge);
	}

	Edge DataManager::getEdge(int64_t id) const { return edgeManager_->get(id); }

	std::vector<Edge> DataManager::getEdgeBatch(const std::vector<int64_t> &ids) const {
		return edgeManager_->getBatch(ids);
	}

	std::vector<Edge> DataManager::getEdgesInRange(int64_t startId, int64_t endId, size_t limit) const {
		return edgeManager_->getInRange(startId, endId, limit);
	}

	void DataManager::addEdgeProperties(int64_t edgeId,
										const std::unordered_map<std::string, PropertyValue> &properties) const {
		// 1. Snapshot OLD state
		Edge oldEdge = edgeManager_->get(edgeId);
		// Freeze old properties
		auto existingProps = edgeManager_->getProperties(edgeId);
		oldEdge.setProperties(existingProps);

		// 2. Perform modification
		edgeManager_->addProperties(edgeId, properties);

		// 3. Snapshot NEW state
		Edge newEdge = edgeManager_->get(edgeId);
		auto newProps = edgeManager_->getProperties(edgeId);
		newEdge.setProperties(newProps);

		notifyEdgeUpdated(oldEdge, newEdge);
	}

	void DataManager::removeEdgeProperty(int64_t edgeId, const std::string &key) const {
		// 1. Snapshot OLD state
		Edge oldEdge = edgeManager_->get(edgeId);
		// Freeze old properties
		auto existingProps = edgeManager_->getProperties(edgeId);
		oldEdge.setProperties(existingProps);

		// 2. Perform removal
		edgeManager_->removeProperty(edgeId, key);

		// 3. Snapshot NEW state
		Edge newEdge = edgeManager_->get(edgeId);
		auto newProps = edgeManager_->getProperties(edgeId);
		newEdge.setProperties(newProps);

		notifyEdgeUpdated(oldEdge, newEdge);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getEdgeProperties(int64_t edgeId) const {
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

	// --- Property Entity Operations ---

	void DataManager::addPropertyEntity(Property &property) const { propertyManager_->add(property); }

	void DataManager::updatePropertyEntity(const Property &property) const { propertyManager_->update(property); }

	void DataManager::deleteProperty(Property &property) const { propertyManager_->remove(property); }

	Property DataManager::getProperty(int64_t id) const { return propertyManager_->get(id); }

	// --- Blob Operations ---

	void DataManager::addBlobEntity(Blob &blob) const { blobManager_->add(blob); }

	void DataManager::updateBlobEntity(const Blob &blob) const { blobManager_->update(blob); }

	void DataManager::deleteBlob(Blob &blob) const { blobManager_->remove(blob); }

	Blob DataManager::getBlob(int64_t id) const { return blobManager_->get(id); }

	// --- Index Operations ---

	void DataManager::addIndexEntity(Index &index) const { indexEntityManager_->add(index); }

	void DataManager::updateIndexEntity(const Index &index) const { indexEntityManager_->update(index); }

	void DataManager::deleteIndex(Index &index) const { indexEntityManager_->remove(index); }

	Index DataManager::getIndex(int64_t id) const { return indexEntityManager_->get(id); }

	// --- State Operations ---

	void DataManager::addStateEntity(State &state) const { stateManager_->add(state); }

	void DataManager::updateStateEntity(const State &state) const { stateManager_->update(state); }

	void DataManager::deleteState(State &state) const { stateManager_->remove(state); }

	State DataManager::getState(int64_t id) const { return stateManager_->get(id); }

	State DataManager::findStateByKey(const std::string &key) const { return stateManager_->findByKey(key); }

	void DataManager::addStateProperties(const std::string &stateKey,
										 const std::unordered_map<std::string, PropertyValue> &properties) const {
		stateManager_->addStateProperties(stateKey, properties);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getStateProperties(const std::string &stateKey) const {
		return stateManager_->getStateProperties(stateKey);
	}

	void DataManager::removeState(const std::string &stateKey) const { stateManager_->removeState(stateKey); }

	bool DataManager::isChainHeadState(const State &state) {
		return state.getId() != 0 && state.isActive() && state.getPrevStateId() == 0;
	}

	// --- Template Method Implementations ---

	template<typename EntityType>
	void DataManager::addToCache(const EntityType &entity) {
		EntityTraits<EntityType>::addToCache(this, entity);
	}

	template<typename EntityType>
	EntityType DataManager::getEntity(int64_t id) {
		return EntityTraits<EntityType>::get(this, id);
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
	std::vector<EntityType> DataManager::getEntitiesInRange(int64_t startId, int64_t endId, size_t limit) {
		std::vector<EntityType> result;
		if (startId > endId || limit == 0) {
			return result;
		}

		// Reserve memory to avoid reallocations.
		result.reserve(std::min(static_cast<size_t>(endId - startId + 1), limit));

		// This set will keep track of IDs that have already been processed (from memory)
		// to avoid adding them again from the disk-based load.
		std::unordered_set<int64_t> processedIds;

		// --- PASS 1: Populate from Memory (PersistenceManager & Cache) ---
		// This pass ensures data consistency by prioritizing in-memory changes over stale disk data.

		// Get reference to cache via EntityTraits (Adapter)
		auto &cache = EntityTraits<EntityType>::getCache(this);

		for (int64_t currentId = startId; currentId <= endId; ++currentId) {
			if (result.size() >= limit) {
				break;
			}

			// 1. Check PersistenceManager first (Highest Priority)
			// It checks both the Active Layer (new writes) and Flushing Layer (currently saving).
			auto dirtyInfo = persistenceManager_->getDirtyInfo<EntityType>(currentId);

			if (dirtyInfo.has_value()) {
				// Mark this ID as processed so we don't fetch it from cache or disk.
				processedIds.insert(currentId);

				// Only add if it's not marked for deletion.
				if (dirtyInfo->changeType != EntityChangeType::DELETED && dirtyInfo->backup.has_value()) {
					result.push_back(*dirtyInfo->backup);
				}
				continue; // Move to the next ID
			}

			// 2. If not dirty, check the LRU Cache.
			if (cache.contains(currentId)) {
				EntityType entity = cache.peek(currentId); // Use peek to avoid changing LRU order
				if (entity.isActive()) {
					result.push_back(entity);
				}
				// Mark as processed so we don't fetch it from disk.
				processedIds.insert(currentId);
			}
		}

		// If we have already reached the limit just from memory, we can return early.
		if (result.size() >= limit) {
			return result;
		}

		// --- PASS 2: Load remaining entities from Disk using Segment-based reads ---
		// This pass leverages data locality for performance by reading from segments in bulk.
		// It will skip any entities that were already loaded from memory in Pass 1.

		// Find all segments that overlap with the requested ID range.
		auto entityType = EntityType::typeId;
		auto segments = segmentTracker_->getSegmentsByType(entityType);

		for (const auto &header: segments) {
			// Calculate the intersection between the segment's ID range and the user's requested range.
			int64_t segmentStartId = header.start_id;
			int64_t segmentEndId = header.start_id + header.used - 1;

			int64_t intersectStart = std::max(startId, segmentStartId);
			int64_t intersectEnd = std::min(endId, segmentEndId);

			if (intersectStart > intersectEnd) {
				continue; // No overlap with this segment.
			}

			// Load a batch of entities from this segment within the intersecting range.
			size_t needed = limit - result.size();
			std::vector<EntityType> segmentEntities =
					loadEntitiesFromSegment<EntityType>(header.file_offset, intersectStart, intersectEnd, needed);

			for (const EntityType &entity: segmentEntities) {
				// IMPORTANT: Only add the entity if it was not already processed from memory.
				if (!processedIds.contains(entity.getId())) {
					result.push_back(entity);

					// Add the newly loaded entity to the cache for future queries.
					addToCache(entity);

					if (result.size() >= limit) {
						break;
					}
				}
			}

			if (result.size() >= limit) {
				break; // Stop iterating through segments if limit is reached.
			}
		}

		return result;
	}

	template<typename EntityType>
	uint64_t DataManager::findSegmentForEntityId(int64_t id) const {
		uint32_t type = EntityType::typeId;
		return segmentIndexManager_->findSegmentForId(type, id);
	}

	// --- Entity Loading from Disk ---

	template<typename EntityType>
	std::vector<EntityType> DataManager::loadEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
																 size_t limit) const {
		return readEntitiesFromSegment<EntityType>(segmentOffset, startId, endId, limit);
	}

	template<typename EntityType>
	std::optional<EntityType> DataManager::readEntityFromDisk(const int64_t fileOffset) const {
		file_->seekg(fileOffset);
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
	void DataManager::setEntityDirty(const DirtyEntityInfo<EntityType> &info) {
		if (info.backup.has_value() && info.backup->getId() == 0) {
			return;
		}
		persistenceManager_->upsert(info);
	}

	template<typename EntityType>
	std::optional<DirtyEntityInfo<EntityType>> DataManager::getDirtyInfo(int64_t id) {
		return persistenceManager_->getDirtyInfo<EntityType>(id);
	}

	bool DataManager::hasUnsavedChanges() const { return persistenceManager_->hasUnsavedChanges(); }

	FlushSnapshot DataManager::prepareFlushSnapshot() const { return persistenceManager_->createSnapshot(); }

	void DataManager::commitFlushSnapshot() const { persistenceManager_->commitSnapshot(); }

	void DataManager::setMaxDirtyEntities(size_t max) const { persistenceManager_->setMaxDirtyEntities(max); }

	void DataManager::setAutoFlushCallback(std::function<void()> cb) const {
		persistenceManager_->setAutoFlushCallback(std::move(cb));
	}

	void DataManager::checkAndTriggerAutoFlush() const { persistenceManager_->checkAndTriggerAutoFlush(); }

	template<typename EntityType>
	std::vector<DirtyEntityInfo<EntityType>>
	DataManager::getDirtyEntityInfos(const std::vector<EntityChangeType> &types) {
		// 1. Get all dirty entities from PersistenceManager (Active + Flushing)
		auto allInfos = persistenceManager_->getAllDirtyInfos<EntityType>();

		// 2. Filter by requested types
		// Optimization: If types contains ALL types, return directly
		if (types.size() == 3) { // Assuming ADDED, MODIFIED, DELETED are the only main ones
			return allInfos;
		}

		std::vector<DirtyEntityInfo<EntityType>> result;
		result.reserve(allInfos.size());

		for (const auto &info: allInfos) {
			bool typeMatch = false;
			for (auto type: types) {
				if (info.changeType == type) {
					typeMatch = true;
					break;
				}
			}
			if (typeMatch) {
				result.push_back(info);
			}
		}
		return result;
	}

	template<typename EntityType>
	EntityType DataManager::getEntityFromMemoryOrDisk(int64_t id) {
		// 1. Check dirty info via PersistenceManager
		auto dirtyInfo = getDirtyInfo<EntityType>(id);

		if (dirtyInfo.has_value()) {
			// CASE A: Entity is marked as DELETED in memory
			if (dirtyInfo->changeType == EntityChangeType::DELETED) {
				return EntityType();
			}

			// CASE B: Entity is ADDED or MODIFIED
			if (dirtyInfo->backup.has_value()) {
				return *dirtyInfo->backup;
			}
		}

		// 2. Check Cache
		auto &cache = EntityTraits<EntityType>::getCache(this);
		if (cache.contains(id)) {
			EntityType entity = cache.get(id);
			if (!entity.isActive()) {
				return EntityType(); // Cache hit but inactive -> return empty
			}
			return entity;
		}

		// 3. Load from Disk
		EntityType entity = EntityTraits<EntityType>::loadFromDisk(this, id);
		if (entity.getId() != 0 && entity.isActive()) {
			cache.put(id, entity);
			return entity;
		}

		return EntityType();
	}

	// --- Loading Entities from Disk ---

	Node DataManager::loadNodeFromDisk(const int64_t id) const {
		auto nodeOpt = findAndReadEntity<Node>(id);
		return nodeOpt.value_or(Node()); // Return empty node if not found or deleted
	}

	Edge DataManager::loadEdgeFromDisk(const int64_t id) const {
		auto edgeOpt = findAndReadEntity<Edge>(id);
		return edgeOpt.value_or(Edge()); // Return empty edge if not found or deleted
	}

	Property DataManager::loadPropertyFromDisk(const int64_t id) const {
		auto propertyOpt = findAndReadEntity<Property>(id);
		return propertyOpt.value_or(Property()); // Return empty property if not found or deleted
	}

	Blob DataManager::loadBlobFromDisk(const int64_t id) const {
		auto blobOpt = findAndReadEntity<Blob>(id);
		return blobOpt.value_or(Blob()); // Return empty blob if not found or deleted
	}

	Index DataManager::loadIndexFromDisk(const int64_t id) const {
		auto indexOpt = findAndReadEntity<Index>(id);
		return indexOpt.value_or(Index()); // Return empty index if not found or deleted
	}

	State DataManager::loadStateFromDisk(const int64_t id) const {
		auto stateOpt = findAndReadEntity<State>(id);
		return stateOpt.value_or(State()); // Return empty state if not found or deleted
	}

	// --- Cache and Transaction Management ---

	void DataManager::clearCache() const {
		nodeCache_.clear();
		edgeCache_.clear();
		propertyCache_.clear();
		blobCache_.clear();
		indexCache_.clear();
		stateCache_.clear();
	}

	template<typename EntityType>
	void DataManager::markEntityDeleted(EntityType &entity) {
		auto dirtyInfo = persistenceManager_->getDirtyInfo<EntityType>(entity.getId());

		if (dirtyInfo.has_value() && dirtyInfo->changeType == EntityChangeType::ADDED) {
			// Revert ADD by overwriting with explicit DELETED (or just removing, but DELETED is safer for log)
			// Actually, if it was just added in memory and never saved, we can technically ignore it,
			// BUT to be safe in the registry logic, we mark it DELETED.
			// Better yet, update registry to remove it?
			// Simpler: Mark DELETED.
			// persistenceManager_->upsert(DirtyEntityInfo<EntityType>(EntityChangeType::DELETED, entity));

			// Remove from persistence manager completely (Undoes the ADD).
			// This prevents an unnecessary DELETE record from being written to the WAL/Disk.
			const int64_t id = entity.getId();
			persistenceManager_->remove<EntityType>(id);
		} else {
			entity.markInactive();
			persistenceManager_->upsert(DirtyEntityInfo<EntityType>(EntityChangeType::DELETED, entity));
		}

		EntityTraits<EntityType>::removeFromCache(this, entity.getId());
		checkAndTriggerAutoFlush();
	}

	template<typename EntityType>
	void DataManager::removeEntityProperty(int64_t entityId, const std::string &key) {
		propertyManager_->removeEntityProperty<EntityType>(entityId, key);
	}

	// --- Template Instantiations ---

	// Node-specific instantiations
	template void DataManager::addToCache<Node>(const Node &entity);
	template Node DataManager::getEntity<Node>(int64_t id);
	template void DataManager::updateEntity<Node>(const Node &entity);
	template std::optional<DirtyEntityInfo<Node>> DataManager::getDirtyInfo<Node>(int64_t);
	template std::vector<Node> DataManager::getEntitiesInRange<Node>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Node>(int64_t id) const;
	template Node DataManager::getEntityFromMemoryOrDisk<Node>(int64_t id);
	template void DataManager::removeEntityProperty<Node>(int64_t entityId, const std::string &key);
	template void DataManager::markEntityDeleted<Node>(Node &entity);
	template std::vector<Node> DataManager::loadEntitiesFromSegment<Node>(uint64_t, int64_t, int64_t, size_t) const;
	template std::optional<Node> DataManager::readEntityFromDisk<Node>(int64_t fileOffset) const;
	template std::optional<Node> DataManager::findAndReadEntity<Node>(int64_t id) const;
	template std::vector<Node> DataManager::readEntitiesFromSegment<Node>(uint64_t, int64_t, int64_t, size_t,
																		  bool) const;
	template std::vector<DirtyEntityInfo<Node>>
	DataManager::getDirtyEntityInfos<Node>(const std::vector<EntityChangeType> &);
	template void DataManager::setEntityDirty<Node>(const DirtyEntityInfo<Node> &);

	// Edge-specific instantiations
	template void DataManager::addToCache<Edge>(const Edge &entity);
	template Edge DataManager::getEntity<Edge>(int64_t id);
	template void DataManager::updateEntity<Edge>(const Edge &entity);
	template std::optional<DirtyEntityInfo<Edge>> DataManager::getDirtyInfo<Edge>(int64_t);
	template std::vector<Edge> DataManager::getEntitiesInRange<Edge>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Edge>(int64_t id) const;
	template Edge DataManager::getEntityFromMemoryOrDisk<Edge>(int64_t id);
	template void DataManager::removeEntityProperty<Edge>(int64_t entityId, const std::string &key);
	template void DataManager::markEntityDeleted<Edge>(Edge &entity);
	template std::vector<Edge> DataManager::loadEntitiesFromSegment<Edge>(uint64_t, int64_t, int64_t, size_t) const;
	template std::optional<Edge> DataManager::readEntityFromDisk<Edge>(int64_t fileOffset) const;
	template std::optional<Edge> DataManager::findAndReadEntity<Edge>(int64_t id) const;
	template std::vector<Edge> DataManager::readEntitiesFromSegment<Edge>(uint64_t, int64_t, int64_t, size_t,
																		  bool) const;
	template std::vector<DirtyEntityInfo<Edge>>
	DataManager::getDirtyEntityInfos<Edge>(const std::vector<EntityChangeType> &);
	template void DataManager::setEntityDirty<Edge>(const DirtyEntityInfo<Edge> &);

	// Property-specific instantiations
	template void DataManager::addToCache<Property>(const Property &entity);
	template std::optional<DirtyEntityInfo<Property>> DataManager::getDirtyInfo<Property>(int64_t);
	template Property DataManager::getEntityFromMemoryOrDisk<Property>(int64_t id);
	template std::vector<Property> DataManager::getEntitiesInRange<Property>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Property>(int64_t id) const;
	template void DataManager::markEntityDeleted<Property>(Property &entity);
	template std::vector<DirtyEntityInfo<Property>>
	DataManager::getDirtyEntityInfos<Property>(const std::vector<EntityChangeType> &);
	template void DataManager::setEntityDirty<Property>(const DirtyEntityInfo<Property> &);

	// Blob-specific instantiations
	template Blob DataManager::getEntityFromMemoryOrDisk<Blob>(int64_t id);
	template std::optional<DirtyEntityInfo<Blob>> DataManager::getDirtyInfo<Blob>(int64_t);
	template std::vector<Blob> DataManager::getEntitiesInRange<Blob>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Blob>(int64_t id) const;
	template void DataManager::markEntityDeleted<Blob>(Blob &entity);
	template std::vector<DirtyEntityInfo<Blob>>
	DataManager::getDirtyEntityInfos<Blob>(const std::vector<EntityChangeType> &);
	template void DataManager::setEntityDirty<Blob>(const DirtyEntityInfo<Blob> &);

	// Index-specific instantiations
	template Index DataManager::getEntityFromMemoryOrDisk<Index>(int64_t id);
	template std::optional<DirtyEntityInfo<Index>> DataManager::getDirtyInfo<Index>(int64_t);
	template std::vector<Index> DataManager::getEntitiesInRange<Index>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<Index>(int64_t id) const;
	template void DataManager::markEntityDeleted<Index>(Index &entity);
	template std::vector<DirtyEntityInfo<Index>>
	DataManager::getDirtyEntityInfos<Index>(const std::vector<EntityChangeType> &);
	template void DataManager::setEntityDirty<Index>(const DirtyEntityInfo<Index> &);

	// State-specific instantiations
	template State DataManager::getEntityFromMemoryOrDisk<State>(int64_t id);
	template std::optional<DirtyEntityInfo<State>> DataManager::getDirtyInfo<State>(int64_t);
	template std::vector<State> DataManager::getEntitiesInRange<State>(int64_t, int64_t, size_t);
	template uint64_t DataManager::findSegmentForEntityId<State>(int64_t id) const;
	template void DataManager::markEntityDeleted<State>(State &entity);
	template std::vector<DirtyEntityInfo<State>>
	DataManager::getDirtyEntityInfos<State>(const std::vector<EntityChangeType> &);
	template void DataManager::setEntityDirty<State>(const DirtyEntityInfo<State> &);
} // namespace graph::storage
