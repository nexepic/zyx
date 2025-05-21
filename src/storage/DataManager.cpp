/**
 * @file DataManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/DataManager.h"
#include <algorithm>
#include <graph/storage/SegmentTracker.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include "graph/core/BlobChainManager.h"

namespace graph::storage {

	DataManager::DataManager(const std::string &dbFilePath, size_t cacheSize, FileHeader &fileHeader,
							 std::shared_ptr<IDAllocator> idAllocator, std::shared_ptr<SegmentTracker> segmentTracker,
							 std::shared_ptr<SpaceManager> spaceManager) :
		dbFilePath_(dbFilePath), nodeCache_(cacheSize), edgeCache_(cacheSize), propertyCache_(cacheSize * 2),
		blobCache_(cacheSize / 4), fileHeader_(fileHeader), idAllocator_(std::move(idAllocator)),
		segmentTracker_(std::move(segmentTracker)), spaceManager_(std::move(spaceManager)) {
		file_ = std::make_shared<std::ifstream>(dbFilePath, std::ios::binary);
		if (!file_->good()) {
			throw std::runtime_error("Cannot open database file");
		}
	}

	DataManager::~DataManager() {
		if (file_ && file_->is_open()) {
			file_->close();
		}
	}

	void DataManager::initialize() {
		blobManager_ = std::make_unique<BlobChainManager>(shared_from_this());
		deletionManager_ = std::make_unique<DeletionManager>(shared_from_this(), spaceManager_);

		// Build segment indexes for efficient lookups
		buildNodeSegmentIndex();
		buildEdgeSegmentIndex();
		buildPropertySegmentIndex();
		buildBlobSegmentIndex();
	}

	// Generic method to build segment index
	void DataManager::buildSegmentIndex(std::vector<SegmentIndex> &segmentIndex, uint64_t segmentHead) const {
		segmentIndex.clear();
		uint64_t currentOffset = segmentHead;

		while (currentOffset != 0) {
			// Use the segment tracker to read the header if available
			SegmentHeader header = segmentTracker_->getSegmentHeader(currentOffset);

			SegmentIndex index{};
			index.startId = header.start_id;
			index.endId = header.start_id + header.used - 1;
			index.segmentOffset = currentOffset;

			segmentIndex.push_back(index);
			currentOffset = header.next_segment_offset;
		}

		// Sort by startId to enable binary search
		std::sort(segmentIndex.begin(), segmentIndex.end(),
				  [](const SegmentIndex &a, const SegmentIndex &b) { return a.startId < b.startId; });
	}

	void DataManager::buildNodeSegmentIndex() { buildSegmentIndex(nodeSegmentIndex_, fileHeader_.node_segment_head); }

	void DataManager::buildEdgeSegmentIndex() { buildSegmentIndex(edgeSegmentIndex_, fileHeader_.edge_segment_head); }

	void DataManager::buildPropertySegmentIndex() {
		buildSegmentIndex(propertySegmentIndex_, fileHeader_.property_segment_head);
	}

	void DataManager::buildBlobSegmentIndex() { buildSegmentIndex(blobSegmentIndex_, fileHeader_.blob_segment_head); }

	// Generic method to find segment for ID
	uint64_t DataManager::findSegmentForId(const std::vector<SegmentIndex> &segmentIndex, int64_t id) {
		// Binary search to find the segment containing this ID
		auto it = std::lower_bound(segmentIndex.begin(), segmentIndex.end(), id,
								   [](const SegmentIndex &index, const int64_t value) { return index.endId < value; });

		if (it != segmentIndex.end() && id >= it->startId && id <= it->endId) {
			return it->segmentOffset;
		}

		return 0; // Not found
	}

	template<typename EntityType>
	uint64_t DataManager::findSegmentForEntityId(int64_t id) const {
		const auto &segmentIndex = EntityTraits<EntityType>::getSegmentIndex(this);
		return findSegmentForId(segmentIndex, id);
	}

	int64_t DataManager::reserveTemporaryNodeId() { return idAllocator_->reserveTemporaryId(Node::typeId); }

	int64_t DataManager::reserveTemporaryEdgeId() { return idAllocator_->reserveTemporaryId(Edge::typeId); }

	int64_t DataManager::reserveTemporaryPropertyId() { return idAllocator_->reserveTemporaryId(Property::typeId); }

	int64_t DataManager::reserveTemporaryBlobId() { return idAllocator_->reserveTemporaryId(Blob::typeId); }

	void DataManager::addNode(const Node &node) { addEntity(node); }
	void DataManager::addEdge(const Edge &edge) { addEntity(edge); }
	void DataManager::addPropertyEntity(const Property &property) { addEntity(property); }
	void DataManager::addBlobEntity(const Blob &blob) { addEntity(blob); }

	template<typename EntityType>
	std::unordered_map<std::string, PropertyValue> DataManager::getEntityProperties(int64_t entityId) {
		// Get the entity
		auto entity = getEntity<EntityType>(entityId);

		// If entity doesn't exist or is inactive, return empty map
		if (entity.getId() == 0 || !entity.isActive()) {
			return {};
		}

		// Start with inline properties
		std::unordered_map<std::string, PropertyValue> allProperties = entity.getProperties();

		// Check if the entity has external properties
		if (entity.hasPropertyEntity()) {
			if (entity.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
				// Load from Property entity
				Property property = getProperty(entity.getPropertyEntityId());
				if (property.getId() != 0 && property.isActive()) {
					// Verify ownership
					if (property.getEntityId() == entityId &&
						property.getEntityType() == EntityTraits<EntityType>::typeId) {
						// Merge properties
						const auto &externalProps = property.getPropertyValues();
						for (const auto &[key, value]: externalProps) {
							allProperties[key] = value;
						}

						// Add property to cache
						addToCache(property);
					}
				}
			} else if (entity.getPropertyStorageType() == PropertyStorageType::BLOB_ENTITY) {
				// Load from Blob chain
				auto blobProperties = getPropertiesFromBlob(entity.getPropertyEntityId());

				// Merge properties
				for (const auto &[key, value]: blobProperties) {
					allProperties[key] = value;
				}
			}
		}

		return allProperties;
	}

	template<typename EntityType>
	size_t DataManager::calculateEntityTotalPropertySize(int64_t entityId) {
		EntityType entity = getEntity<EntityType>(entityId);
		if (entity.getId() == 0 || !entity.isActive()) {
			return 0;
		}

		// Start with inline properties size
		size_t totalSize = 0;
		for (const auto &[key, value]: entity.getProperties()) {
			totalSize += key.size();
			totalSize += property_utils::getPropertyValueSize(value);
		}

		// If we have external properties, we need to account for them too
		if (entity.hasPropertyEntity()) {
			if (entity.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
				// Get property entity and calculate its size
				Property property = getProperty(entity.getPropertyEntityId());
				if (property.getId() != 0 && property.isActive()) {
					for (const auto &[key, value]: property.getPropertyValues()) {
						totalSize += key.size();
						totalSize += property_utils::getPropertyValueSize(value);
					}
				}
			} else if (entity.getPropertyStorageType() == PropertyStorageType::BLOB_ENTITY) {
				// Get blob entity and calculate size from its deserialized properties
				Blob blob = getBlob(entity.getPropertyEntityId());
				if (blob.getId() != 0 && blob.isActive()) {
					std::string serializedData = blob.getData();
					std::stringstream ss(serializedData);
					auto properties = deserializeProperties(ss);

					for (const auto &[key, value]: properties) {
						totalSize += key.size();
						totalSize += property_utils::getPropertyValueSize(value);
					}
				}
			}
		}

		return totalSize;
	}

	// Implementation of addEntityProperties
	template<typename EntityType>
	void DataManager::addEntityProperties(int64_t entityId,
										  const std::unordered_map<std::string, PropertyValue> &properties) {
		// Get the entity
		auto entity = getEntity<EntityType>(entityId);

		// Check if entity exists and is active
		if (entity.getId() == 0 || !entity.isActive()) {
			throw std::runtime_error("Cannot add properties to non-existent or inactive: " + std::to_string(entityId));
		}

		// Add properties to the entity
		for (const auto &[key, value]: properties) {
			entity.addProperty(key, value);
		}

		// Store properties using the appropriate mechanism
		storeProperties<EntityType>(entity);

		// Update the entity
		updateEntity(entity);
	}


	void DataManager::addNodeProperties(int64_t nodeId,
										const std::unordered_map<std::string, PropertyValue> &properties) {
		addEntityProperties<Node>(nodeId, properties);
	}

	template<typename EntityType>
	void DataManager::removeEntityProperty(int64_t entityId, const std::string &key) {
		// Get the entity
		auto entity = getEntity<EntityType>(entityId);

		// Check if the entity exists and is active
		if (entity.getId() == 0 || !entity.isActive()) {
			throw std::runtime_error("Cannot remove property from non-existent or inactive: " +
									 std::to_string(entityId));
		}

		// Check if the entity has this property
		bool propertyRemoved = false;

		// Try to remove from inline properties
		if (entity.hasProperty(key)) {
			entity.removeProperty(key);
			propertyRemoved = true;
		}

		// Special handling for properties stored externally
		if (entity.hasPropertyEntity() && !propertyRemoved) {
			if (entity.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
				Property property = getProperty(entity.getPropertyEntityId());
				if (property.getId() != 0 && property.isActive()) {
					auto properties = property.getPropertyValues();
					auto it = properties.find(key);
					if (it != properties.end()) {
						properties.erase(it);
						property.setProperties(properties);
						propertyRemoved = true;

						// If no properties left, delete the property entity
						if (properties.empty()) {
							deleteProperty(property);
							entity.setPropertyEntityId(0, PropertyStorageType::NONE);
						} else {
							updatePropertyEntity(property);
						}
					}
				}
			} else if (entity.getPropertyStorageType() == PropertyStorageType::BLOB_ENTITY) {
				Blob blob = getBlob(entity.getPropertyEntityId());
				if (blob.getId() != 0 && blob.isActive()) {
					std::string serializedData = blob.getData();
					std::stringstream ss(serializedData);
					auto properties = deserializeProperties(ss);

					auto it = properties.find(key);
					if (it != properties.end()) {
						properties.erase(it);
						propertyRemoved = true;

						// If no properties left, delete the blob
						if (properties.empty()) {
							deleteBlob(blob);
							entity.setPropertyEntityId(0, PropertyStorageType::NONE);
						} else {
							// Re-serialize and update
							std::stringstream outSs;
							serializeProperties(outSs, properties);
							blob.setData(outSs.str());
							updateBlobEntity(blob);
						}
					}
				}
			}
		}

		if (!propertyRemoved) {
			throw std::runtime_error("Property not found: " + key);
		}

		// Store the properties using the appropriate mechanism - in case size thresholds changed
		storeProperties(entity);

		// Update the entity in cache and mark it as dirty
		updateEntity(entity);
	}

	void DataManager::removeNodeProperty(int64_t nodeId, const std::string &key) {
		removeEntityProperty<Node>(nodeId, key);
	}

	void DataManager::addEdgeProperties(int64_t edgeId,
										const std::unordered_map<std::string, PropertyValue> &properties) {
		addEntityProperties<Edge>(edgeId, properties);
	}

	void DataManager::removeEdgeProperty(int64_t edgeId, const std::string &key) {
		removeEntityProperty<Edge>(edgeId, key);
	}

	void DataManager::updateNode(const Node &node) { updateEntity(node); }
	void DataManager::updateEdge(const Edge &edge) { updateEntity(edge); }
	void DataManager::updatePropertyEntity(const Property &property) { updateEntity(property); }
	void DataManager::updateBlobEntity(const Blob &blob) { updateEntity(blob); }

	template<typename EntityType>
	bool DataManager::hasExternalProperty(const EntityType &entity, const std::string &key) {
		if (!entity.hasPropertyEntity()) {
			return false;
		}

		if (entity.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
			Property property = getProperty(entity.getPropertyEntityId());
			if (property.getId() != 0 && property.isActive()) {
				return property.hasPropertyValue(key);
			}
		} else if (entity.getPropertyStorageType() == PropertyStorageType::BLOB_ENTITY) {
			Blob blob = getBlob(entity.getPropertyEntityId());
			if (blob.getId() != 0 && blob.isActive()) {
				std::string serializedData = blob.getData();
				std::stringstream ss(serializedData);
				auto properties = deserializeProperties(ss);
				return properties.find(key) != properties.end();
			}
		}

		return false;
	}

	void DataManager::deleteNode(Node &node) { deleteEntity(node); }
	void DataManager::deleteEdge(Edge &edge) { deleteEntity(edge); }
	void DataManager::deleteProperty(Property &property) { deleteEntity(property); }
	void DataManager::deleteBlob(Blob &blob) { deleteEntity(blob); }

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

	uint32_t
	DataManager::calculateSerializedSize(const std::unordered_map<std::string, PropertyValue> &properties) const {
		// Calculate the size of the serialized property set
		uint32_t size = sizeof(uint32_t); // Number of properties

		for (const auto &[key, value]: properties) {
			size += sizeof(uint32_t) + key.size(); // Key length + key content
			size += property_utils::getPropertyValueSize(value); // Value size
		}

		return size;
	}

	void DataManager::serializeProperties(std::ostream &os,
										  const std::unordered_map<std::string, PropertyValue> &properties) const {
		// Write the number of properties
		utils::Serializer::writePOD(os, static_cast<uint32_t>(properties.size()));

		// Write each property key-value pair
		for (const auto &[key, value]: properties) {
			utils::Serializer::writeString(os, key);
			utils::Serializer::writePropertyValue(os, value);
		}
	}

	std::unordered_map<std::string, PropertyValue> DataManager::deserializeProperties(std::istream &is) const {
		std::unordered_map<std::string, PropertyValue> properties;

		// Read the number of properties
		auto propertyCount = utils::Serializer::readPOD<uint32_t>(is);

		// Read each property key-value pair
		for (uint32_t i = 0; i < propertyCount; ++i) {
			std::string key = utils::Serializer::readString(is);
			PropertyValue value = utils::Serializer::readPropertyValue(is);
			properties[key] = value;
		}

		return properties;
	}

	// Clean up existing external properties
	template<typename EntityType>
	void DataManager::cleanupExternalProperties(EntityType &entity) {
		if (!entity.hasPropertyEntity()) {
			return;
		}

		int64_t propertyId = entity.getPropertyEntityId();

		if (entity.getPropertyStorageType() == PropertyStorageType::BLOB_ENTITY) {
			// Delete the entire blob chain
			blobManager_->deleteBlobChain(propertyId);
		} else if (entity.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
			Property property = getProperty(propertyId);
			if (property.getId() != 0 && property.isActive()) {
				deleteProperty(property);
			}
		}
	}

	// Store properties in a Property entity
	template<typename EntityType>
	void
	DataManager::storePropertiesInPropertyEntity(EntityType &entity,
												 const std::unordered_map<std::string, PropertyValue> &properties) {
		// Check if there's an existing property entity
		if (entity.hasPropertyEntity() && entity.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
			// Update existing property entity
			Property property = getProperty(entity.getPropertyEntityId());
			if (property.getId() != 0 && property.isActive()) {
				property.setProperties(properties);
				updatePropertyEntity(property);
				// Clear properties from entity as they're now stored externally
				entity.clearProperties();
				return;
			}
		}

		// Clean up any existing blob
		if (entity.hasPropertyEntity() && entity.getPropertyStorageType() == PropertyStorageType::BLOB_ENTITY) {
			Blob blob = getBlob(entity.getPropertyEntityId());
			if (blob.getId() != 0 && blob.isActive()) {
				deleteBlob(blob);
			}
		}

		// Create new property entity
		Property property;
		property.setId(reserveTemporaryPropertyId());
		property.setProperties(properties);

		// Set entity reference on the property
		if constexpr (std::is_same_v<EntityType, Node>) {
			property.setEntityInfo(entity.getId(), Node::typeId);
		} else if constexpr (std::is_same_v<EntityType, Edge>) {
			property.setEntityInfo(entity.getId(), Edge::typeId);
		}

		// Add property to storage
		addPropertyEntity(property);

		// Update entity with property reference
		entity.setPropertyEntityId(property.getId(), PropertyStorageType::PROPERTY_ENTITY);

		// Clear properties from entity as they're now stored externally
		entity.clearProperties();
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getPropertiesFromBlob(int64_t blobId) {
		try {
			// Read data from blob chain
			std::string serializedData = blobManager_->readBlobChain(blobId);

			// Deserialize properties
			std::stringstream ss(serializedData);
			return deserializeProperties(ss);

		} catch (const std::exception &e) {
			// Log error but return empty map to avoid crashing
			std::cerr << "Error reading blob chain: " << e.what() << std::endl;
			return {};
		}
	}

	// Store properties in a Blob entity
	template<typename EntityType>
	void DataManager::storePropertiesInBlob(EntityType &entity,
											const std::unordered_map<std::string, PropertyValue> &properties) {
		// Serialize properties
		std::stringstream ss;
		serializeProperties(ss, properties);
		std::string serializedData = ss.str();

		// Check if there's an existing blob
		if (entity.hasPropertyEntity() && entity.getPropertyStorageType() == PropertyStorageType::BLOB_ENTITY) {
			try {
				// Update existing blob chain
				// blobManager_->updateBlobChain(entity.getPropertyEntityId(), serializedData);
				blobManager_->deleteBlobChain(entity.getPropertyEntityId());

				// Clear properties from entity as they're now stored externally
				entity.clearProperties();
				return;
			} catch (const std::exception &e) {
				// Handle update failure (e.g., blob not found)
				// Clean up any existing blob chain
				// blobManager_->deleteBlobChain(entity.getPropertyEntityId());
				std::cerr << "Error deleting blob chain: " << e.what() << std::endl;
			}
		}

		// Clean up any existing property entity
		if (entity.hasPropertyEntity() && entity.getPropertyStorageType() == PropertyStorageType::PROPERTY_ENTITY) {
			Property property = getProperty(entity.getPropertyEntityId());
			if (property.getId() != 0 && property.isActive()) {
				deleteProperty(property);
			}
		}

		try {
			// Create new blob chain
			int64_t entityId = entity.getId();
			uint32_t entityType = 0;

			if constexpr (std::is_same_v<EntityType, Node>) {
				entityType = Node::typeId;
			} else if constexpr (std::is_same_v<EntityType, Edge>) {
				entityType = Edge::typeId;
			}

			auto blobChain = blobManager_->createBlobChain(entityId, entityType, serializedData);

			if (blobChain.empty()) {
				throw std::runtime_error("Failed to create blob chain");
			}

			// Add all blobs to storage
			for (auto &blob: blobChain) {
				addBlobEntity(blob);
			}

			// Update entity with head blob reference
			entity.setPropertyEntityId(blobChain[0].getId(), PropertyStorageType::BLOB_ENTITY);

			// Clear properties from entity as they're now stored externally
			entity.clearProperties();

		} catch (const std::runtime_error &e) {
			// Handle errors during blob chain creation
			throw std::runtime_error(std::string("Failed to store properties in blob: ") + e.what());
		}
	}

	template<typename EntityType>
	void DataManager::storeProperties(EntityType &entity) {
		auto properties = entity.getProperties();

		// Calculate total size of properties
		uint32_t dataSize = calculateSerializedSize(properties);

		if (entity.hasPropertyEntity()) {
			cleanupExternalProperties(entity);
			entity.setPropertyEntityId(0, PropertyStorageType::NONE);
		}

		// Choose storage type based on size
		if (dataSize + PROPERTY_ENTITY_OVERHEAD > MAX_SEGMENT_PROPERTY_SIZE) {
			// Large properties - use Blob
			storePropertiesInBlob(entity, properties);
		} else {
			// Medium properties - use Property entity
			storePropertiesInPropertyEntity(entity, properties);
		}
	}

	bool DataManager::hasUnsavedChanges() const {
		return !dirtyNodes_.empty() || !dirtyEdges_.empty() || !dirtyProperties_.empty() || !dirtyBlobs_.empty();
	}

	void DataManager::markAllSaved() {
		dirtyNodes_.clear();
		dirtyEdges_.clear();
		dirtyProperties_.clear();
		dirtyBlobs_.clear();
	}

	void DataManager::checkAndTriggerAutoFlush() const {
		if (needsAutoFlush() && autoFlushCallback_) {
			autoFlushCallback_();
		}
	}

	Node DataManager::getNode(int64_t id) { return getEntity<Node>(id); }

	Edge DataManager::getEdge(int64_t id) { return getEntity<Edge>(id); }

	Property DataManager::getProperty(int64_t id) { return getEntity<Property>(id); }

	Blob DataManager::getBlob(int64_t id) { return getEntity<Blob>(id); }

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

	std::vector<Node> DataManager::getNodeBatch(const std::vector<int64_t> &ids) {
		std::vector<Node> result;
		result.reserve(ids.size());
		std::vector<int64_t> missedIds;

		// First check dirty collections and cache
		for (int64_t id: ids) {
			// Check dirty collections
			auto dirtyNode = getEntityFromDirty<Node>(id);
			if (dirtyNode.has_value()) {
				result.push_back(*dirtyNode);
				continue;
			}

			// Check cache
			if (nodeCache_.contains(id)) {
				result.push_back(nodeCache_.get(id));
			} else {
				missedIds.push_back(id);
			}
		}

		// Group missed IDs by segment for efficient loading
		std::unordered_map<uint64_t, std::vector<int64_t>> segmentToIds;
		for (int64_t id: missedIds) {
			uint64_t segmentOffset = findSegmentForEntityId<Node>(id);
			if (segmentOffset != 0) {
				segmentToIds[segmentOffset].push_back(id);
			}
		}

		// Load nodes segment by segment
		for (const auto &[segmentOffset, segmentIds]: segmentToIds) {
			for (int64_t id: segmentIds) {
				auto nodeOpt = findAndReadEntity<Node>(id);
				if (nodeOpt.has_value()) {
					const Node &node = nodeOpt.value();
					result.push_back(node);
					// Add to cache
					nodeCache_.put(id, node);
				}
			}
		}

		return result;
	}

	std::vector<Edge> DataManager::getEdgeBatch(const std::vector<int64_t> &ids) {
		std::vector<Edge> result;
		result.reserve(ids.size());
		std::vector<int64_t> missedIds;

		// First check dirty collections and cache
		for (int64_t id: ids) {
			// Check dirty collections
			auto dirtyEdge = getEntityFromDirty<Edge>(id);
			if (dirtyEdge.has_value()) {
				result.push_back(*dirtyEdge);
				continue;
			}

			// Check cache
			if (edgeCache_.contains(id)) {
				result.push_back(edgeCache_.get(id));
			} else {
				missedIds.push_back(id);
			}
		}

		// Group missed IDs by segment for efficient loading
		std::unordered_map<uint64_t, std::vector<int64_t>> segmentToIds;
		for (int64_t id: missedIds) {
			uint64_t segmentOffset = findSegmentForEntityId<Edge>(id);
			if (segmentOffset != 0) {
				segmentToIds[segmentOffset].push_back(id);
			}
		}

		// Load edges segment by segment
		for (const auto &[segmentOffset, segmentIds]: segmentToIds) {
			for (int64_t id: segmentIds) {
				auto edgeOpt = findAndReadEntity<Edge>(id);
				if (edgeOpt.has_value()) {
					const Edge &edge = edgeOpt.value();
					result.push_back(edge);
					// Add to cache
					edgeCache_.put(id, edge);
				}
			}
		}

		return result;
	}

	std::vector<Node> DataManager::getNodesInRange(int64_t startId, int64_t endId, size_t limit) {
		return getEntitiesInRange<Node>(startId, endId, limit);
	}

	std::vector<Edge> DataManager::getEdgesInRange(int64_t startId, int64_t endId, size_t limit) {
		return getEntitiesInRange<Edge>(startId, endId, limit);
	}

	// Core method to read a single entity from a specific file offset
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

	// Core method to find and read a single entity by ID
	template<typename EntityType>
	std::optional<EntityType> DataManager::findAndReadEntity(int64_t id) const {
		// Find segment for this entity type and ID using EntityTraits
		uint64_t segmentOffset = findSegmentForEntityId<EntityType>(id);

		if (segmentOffset == 0) {
			return std::nullopt; // Segment not found
		}

		// Read segment header using segment tracker if available
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
														relativePosition * sizeof(EntityType));

		auto entity = readEntityFromDisk<EntityType>(entityOffset);

		// Add to cache if found
		if (entity.has_value()) {
			// We need to use const_cast here because findAndReadEntity is const but we want to add to cache
			const_cast<DataManager *>(this)->addToCache(*entity);
		}

		return entity;
	}

	// Core method to read multiple entities from a segment with filtering
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
		auto entityOffset =
				static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader) + startOffset * sizeof(EntityType));

		// Read entities
		for (uint64_t i = 0; i < count; ++i) {
			auto entityOpt = readEntityFromDisk<EntityType>(entityOffset);
			if (entityOpt.has_value()) {
				result.push_back(entityOpt.value());
			} else if (!filterDeleted) {
				// If we're not filtering deleted entities, read directly
				file_->seekg(entityOffset);
				EntityType entity = EntityType::deserialize(*file_);
				result.push_back(entity);
			}

			// Move to next entity
			entityOffset += sizeof(EntityType);
		}

		return result;
	}

	template<typename EntityType>
	std::vector<EntityType> DataManager::loadEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
																 size_t limit) const {
		return readEntitiesFromSegment<EntityType>(segmentOffset, startId, endId, limit);
	}

	std::vector<Edge> DataManager::findEdgesByNode(int64_t nodeId, const std::string &direction) {
		std::vector<Edge> result;

		// First check dirty edges
		for (const auto &[edgeId, info]: dirtyEdges_) {
			if (info.changeType != EntityChangeType::DELETED) { // Use correct enum scope
				Edge edge;
				if (info.backup.has_value()) { // Access backup correctly
					edge = *info.backup;
				} else if (edgeCache_.contains(edgeId)) {
					edge = edgeCache_.peek(edgeId);
				} else {
					continue; // Skip if we can't get the edge
				}

				bool match = false;
				if (direction == "outgoing" && edge.getFromNodeId() == nodeId) {
					match = true;
				} else if (direction == "incoming" && edge.getToNodeId() == nodeId) {
					match = true;
				} else if (direction == "both" && (edge.getFromNodeId() == nodeId || edge.getToNodeId() == nodeId)) {
					match = true;
				}

				if (match) {
					result.push_back(edge);
				}
			}
		}

		// Then proceed with disk scan
		for (const auto &segmentIndex: edgeSegmentIndex_) {
			uint64_t segmentOffset = segmentIndex.segmentOffset;
			SegmentHeader header = segmentTracker_->getSegmentHeader(segmentOffset);

			// Read all edges in this segment
			std::vector<Edge> segmentEdges = readEntitiesFromSegment<Edge>(
					segmentOffset, header.start_id, header.start_id + header.used - 1, header.used);

			for (const Edge &edge: segmentEdges) {
				// Skip edges that are in dirty collection
				if (dirtyEdges_.find(edge.getId()) != dirtyEdges_.end()) {
					continue;
				}

				bool match = false;
				if (direction == "outgoing" && edge.getFromNodeId() == nodeId) {
					match = true;
				} else if (direction == "incoming" && edge.getToNodeId() == nodeId) {
					match = true;
				} else if (direction == "both" && (edge.getFromNodeId() == nodeId || edge.getToNodeId() == nodeId)) {
					match = true;
				}

				if (match) {
					result.push_back(edge);
					edgeCache_.put(edge.getId(), edge); // Cache the edge
				}
			}
		}

		return result;
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getNodeProperties(int64_t nodeId) {
		return getEntityProperties<Node>(nodeId);
	}

	std::unordered_map<std::string, PropertyValue> DataManager::getEdgeProperties(int64_t edgeId) {
		return getEntityProperties<Edge>(edgeId);
	}

	template<typename EntityType>
	std::optional<EntityType> DataManager::getEntityFromDirty(int64_t id) {
		// Get appropriate dirty map through traits
		auto &dirtyMap = EntityTraits<EntityType>::getDirtyMap(this);
		auto it = dirtyMap.find(id);

		if (it != dirtyMap.end()) {
			const auto &info = it->second;

			// Return the entity if it's not deleted
			if (info.changeType != EntityChangeType::DELETED) {
				// If we have a backup, return it
				if (info.backup.has_value()) {
					return info.backup;
				}
				// Otherwise try to get from cache
				else {
					auto &cache = EntityTraits<EntityType>::getCache(this);
					if (cache.contains(id)) {
						return cache.peek(id);
					}
				}
			}
			// If it's deleted or not available, return nullopt
			return std::nullopt;
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

		// Then check cache using traits
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

	void DataManager::clearCache() {
		nodeCache_.clear();
		edgeCache_.clear();
		propertyCache_.clear();
		blobCache_.clear();
	}

	template<typename EntityType>
	void DataManager::addToCache(const EntityType &entity) {
		EntityTraits<EntityType>::addToCache(this, entity);
	}

	// Generic entity operations
	template<typename EntityType>
	EntityType DataManager::getEntity(int64_t id) {
		return EntityTraits<EntityType>::get(this, id);
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
		}
	}

	template<typename EntityType>
	void DataManager::updateEntityId(int64_t tempId, int64_t permId) {
		// Get the appropriate cache and dirty map based on entity type
		auto &cache = EntityTraits<EntityType>::getCache(this);
		auto &dirtyMap = EntityTraits<EntityType>::getDirtyMap(this);

		// Update in cache if it exists
		if (cache.contains(tempId)) {
			// Get the entity from cache
			EntityType entity = cache.get(tempId);

			// Update the ID
			entity.setPermanentId(permId);

			// Remove old entry and add new one
			cache.remove(tempId);
			cache.put(permId, entity);
		}

		// Update in dirty map if it exists
		auto dirtyIt = dirtyMap.find(tempId);
		if (dirtyIt != dirtyMap.end()) {
			// Get the entity info
			auto entityInfo = dirtyIt->second;

			// If this is an ADDED entity, create a new EntityType with updated ID
			if (entityInfo.changeType == EntityChangeType::ADDED) {
				// Ensure we have a backup
				if (entityInfo.backup) {
					EntityType entity = *entityInfo.backup;
					entity.setPermanentId(permId);

					// Create new entry with permanent ID
					DirtyEntityInfo<EntityType> newInfo(entityInfo.changeType, entity);

					// Add to dirty map with new ID and remove old entry
					dirtyMap[permId] = newInfo;
					dirtyMap.erase(tempId);
				}
			}
		}
	}

	template<typename EntityType>
	void DataManager::addEntity(const EntityType &entity) {
		EntityTraits<EntityType>::addToCache(this, entity);
		auto &dirtyMap = EntityTraits<EntityType>::getDirtyMap(this);
		dirtyMap[entity.getId()] = DirtyEntityInfo<EntityType>(EntityChangeType::ADDED, entity);
		checkAndTriggerAutoFlush();
	}

	template<typename EntityType>
	void DataManager::updateEntity(const EntityType &entity) {
		if (!entity.isActive()) {
			throw std::runtime_error("Cannot update inactive " + std::to_string(entity.getId()));
		}

		EntityTraits<EntityType>::addToCache(this, entity);

		auto &dirtyMap = EntityTraits<EntityType>::getDirtyMap(this);
		auto it = dirtyMap.find(entity.getId());

		if (it != dirtyMap.end() && it->second.changeType == EntityChangeType::ADDED) {
			// Keep ADDED state but update the backup
			dirtyMap[entity.getId()] = DirtyEntityInfo<EntityType>(EntityChangeType::ADDED, entity);
			return;
		}

		dirtyMap[entity.getId()] = DirtyEntityInfo<EntityType>(EntityChangeType::MODIFIED, entity);
		checkAndTriggerAutoFlush();
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
	void DataManager::deleteEntity(EntityType &entity) {
		// Otherwise delegate to DeletionManager
		if constexpr (std::is_same_v<EntityType, Node>) {
			deletionManager_->deleteNode(entity);
		} else if constexpr (std::is_same_v<EntityType, Edge>) {
			deletionManager_->deleteEdge(entity);
		} else if constexpr (std::is_same_v<EntityType, Property>) {
			deletionManager_->deleteProperty(entity);
		} else if constexpr (std::is_same_v<EntityType, Blob>) {
			deletionManager_->deleteBlob(entity);
		}
	}

	template<typename EntityType>
	std::vector<EntityType> DataManager::getEntitiesInRange(int64_t startId, int64_t endId, size_t limit) {
		std::vector<EntityType> result;
		if (startId > endId || limit == 0) {
			return result;
		}

		// Find relevant segments
		std::vector<uint64_t> relevantSegments;
		auto &segmentIndex = EntityTraits<EntityType>::getSegmentIndex(this);

		for (const auto &index: segmentIndex) {
			if ((index.startId <= endId && index.endId >= startId) && (result.size() < limit)) {
				relevantSegments.push_back(index.segmentOffset);
			}
		}

		// Load entities from segments
		for (uint64_t segmentOffset: relevantSegments) {
			auto segmentEntities =
					loadEntitiesFromSegment<EntityType>(segmentOffset, startId, endId, limit - result.size());

			for (const EntityType &entity: segmentEntities) {
				result.push_back(entity);
				EntityTraits<EntityType>::addToCache(this, entity);
			}

			if (result.size() >= limit) {
				break;
			}
		}

		return result;
	}

	// Template instantiations
	template Node DataManager::getEntity<Node>(int64_t id);
	template Edge DataManager::getEntity<Edge>(int64_t id);
	template void DataManager::storeProperties<Node>(Node &entity);
	template void DataManager::storeProperties<Edge>(Edge &entity);
	template bool DataManager::hasExternalProperty<Node>(const Node &entity, const std::string &key);
	template bool DataManager::hasExternalProperty<Edge>(const Edge &entity, const std::string &key);
	template void DataManager::cleanupExternalProperties<Node>(Node &entity);
	template void DataManager::cleanupExternalProperties<Edge>(Edge &entity);
	template void DataManager::storePropertiesInBlob<Node>(Node &entity,
														   const std::unordered_map<std::string, PropertyValue> &);
	template void DataManager::storePropertiesInBlob<Edge>(Edge &entity,
														   const std::unordered_map<std::string, PropertyValue> &);
	template void
	DataManager::storePropertiesInPropertyEntity<Node>(Node &entity,
													   const std::unordered_map<std::string, PropertyValue> &);
	template void
	DataManager::storePropertiesInPropertyEntity<Edge>(Edge &entity,
													   const std::unordered_map<std::string, PropertyValue> &);
	template std::unordered_map<std::string, PropertyValue> DataManager::getEntityProperties<Node>(int64_t);
	template std::unordered_map<std::string, PropertyValue> DataManager::getEntityProperties<Edge>(int64_t);
	template void DataManager::addEntityProperties<Node>(int64_t,
														 const std::unordered_map<std::string, PropertyValue> &);
	template void DataManager::addEntityProperties<Edge>(int64_t,
														 const std::unordered_map<std::string, PropertyValue> &);
	template void DataManager::removeEntityProperty<Node>(int64_t, const std::string &);
	template void DataManager::removeEntityProperty<Edge>(int64_t, const std::string &);
	template std::vector<Node>
	DataManager::getDirtyEntitiesWithChangeTypes<Node>(const std::vector<EntityChangeType> &);
	template std::vector<Edge>
	DataManager::getDirtyEntitiesWithChangeTypes<Edge>(const std::vector<EntityChangeType> &);
	template std::vector<Property>
	DataManager::getDirtyEntitiesWithChangeTypes<Property>(const std::vector<EntityChangeType> &);
	template std::vector<Blob>
	DataManager::getDirtyEntitiesWithChangeTypes<Blob>(const std::vector<EntityChangeType> &);
	template void DataManager::addToCache<Node>(const Node &entity);
	template void DataManager::addToCache<Edge>(const Edge &entity);
	template void DataManager::addToCache<Property>(const Property &entity);
	template void DataManager::addToCache<Blob>(const Blob &entity);
	template size_t DataManager::calculateEntityTotalPropertySize<Node>(int64_t entityId);
	template size_t DataManager::calculateEntityTotalPropertySize<Edge>(int64_t entityId);
	template void DataManager::markEntityDeleted<Node>(Node &entity);
	template void DataManager::markEntityDeleted<Edge>(Edge &entity);
	template void DataManager::markEntityDeleted<Property>(Property &entity);
	template void DataManager::markEntityDeleted<Blob>(Blob &entity);
	template uint64_t DataManager::findSegmentForEntityId<Node>(int64_t id) const;
	template uint64_t DataManager::findSegmentForEntityId<Edge>(int64_t id) const;
	template uint64_t DataManager::findSegmentForEntityId<Property>(int64_t id) const;
	template uint64_t DataManager::findSegmentForEntityId<Blob>(int64_t id) const;
} // namespace graph::storage
