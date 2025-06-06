/**
 * @file DataManager.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <fstream>
#include <graph/core/BlobChainManager.h>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include "CacheManager.h"
#include "DeletionManager.h"
#include "IDAllocator.h"
#include "StorageHeaders.h"
#include "graph/core/Blob.h"
#include "graph/core/Edge.h"
#include "graph/core/Node.h"
#include "graph/core/Property.h"

namespace graph::storage {

	// Structure to map ID ranges to segment positions
	struct SegmentIndex {
		int64_t startId;
		int64_t endId;
		uint64_t segmentOffset;
	};

	enum class EntityChangeType { ADDED, MODIFIED, DELETED };

	template<typename T>
	struct DirtyEntityInfo {
		EntityChangeType changeType;
		std::optional<T> backup;

		// Default constructor
		DirtyEntityInfo() : changeType(EntityChangeType::MODIFIED), backup(std::nullopt) {}

		// Copy constructor from EntityChangeType and T
		DirtyEntityInfo(EntityChangeType type, const T &entity) : changeType(type) {
			// Use emplace which directly constructs the object in-place
			backup.emplace(entity);
		}

		// Constructor with just type
		explicit DirtyEntityInfo(EntityChangeType type) : changeType(type), backup(std::nullopt) {}
	};

	class DataManager : public std::enable_shared_from_this<DataManager> {
	public:
		explicit DataManager(std::shared_ptr<std::fstream> file, size_t cacheSize, FileHeader &fileHeader,
							 std::shared_ptr<IDAllocator> idAllocator, std::shared_ptr<SegmentTracker> segmentTracker,
							 std::shared_ptr<SpaceManager> spaceManager);
		~DataManager();

		void initialize();

		// Header access
		[[nodiscard]] FileHeader getFileHeader() const { return fileHeader_; }
		FileHeader &getFileHeaderRef() { return fileHeader_; }

		// Node-specific operations
		void addNode(const Node &node);
		void updateNode(const Node &node);
		void deleteNode(Node &node);
		Node getNode(int64_t id);
		std::vector<Node> getNodeBatch(const std::vector<int64_t> &ids);
		std::vector<Node> getNodesInRange(int64_t startId, int64_t endId, size_t limit = 1000);

		// Edge-specific operations
		void addEdge(const Edge &edge);
		void updateEdge(const Edge &edge);
		void deleteEdge(Edge &edge);
		Edge getEdge(int64_t id);
		std::vector<Edge> getEdgeBatch(const std::vector<int64_t> &ids);
		std::vector<Edge> getEdgesInRange(int64_t startId, int64_t endId, size_t limit = 1000);

		template<typename EntityType>
		void storeProperties(EntityType &entity);

		// Node property methods
		void addNodeProperties(int64_t nodeId, const std::unordered_map<std::string, PropertyValue> &properties);
		void removeNodeProperty(int64_t nodeId, const std::string &key);

		// Edge property methods
		void addEdgeProperties(int64_t edgeId, const std::unordered_map<std::string, PropertyValue> &properties);
		void removeEdgeProperty(int64_t edgeId, const std::string &key);

		uint32_t calculateSerializedSize(const std::unordered_map<std::string, PropertyValue> &properties) const;
		void serializeProperties(std::ostream &os,
								 const std::unordered_map<std::string, PropertyValue> &properties) const;
		std::unordered_map<std::string, PropertyValue> deserializeProperties(std::istream &is) const;

		template<typename EntityType>
		bool hasExternalProperty(const EntityType &entity, const std::string &key);

		std::unordered_map<std::string, PropertyValue> getPropertiesFromBlob(int64_t blobId);

		template<typename EntityType>
		void cleanupExternalProperties(EntityType &entity);

		template<typename EntityType>
		void storePropertiesInPropertyEntity(EntityType &entity,
											 const std::unordered_map<std::string, PropertyValue> &properties);

		template<typename EntityType>
		void storePropertiesInBlob(EntityType &entity,
								   const std::unordered_map<std::string, PropertyValue> &properties);

		// Property-specific operations
		void addPropertyEntity(const Property &property);
		void updatePropertyEntity(const Property &property);
		void deleteProperty(Property &property);
		Property getProperty(int64_t id);

		// Blob-specific operations
		void addBlobEntity(const Blob &blob);
		void updateBlobEntity(const Blob &blob);
		void deleteBlob(Blob &blob);
		Blob getBlob(int64_t id);

		// Reserve temporary IDs
		int64_t reserveTemporaryNodeId();
		int64_t reserveTemporaryEdgeId();
		int64_t reserveTemporaryPropertyId();
		int64_t reserveTemporaryBlobId();

		// Get all properties
		std::unordered_map<std::string, PropertyValue> getNodeProperties(int64_t nodeId);
		std::unordered_map<std::string, PropertyValue> getEdgeProperties(int64_t edgeId);

		template<typename EntityType>
		void addToCache(const EntityType &entity);

		template<typename EntityType>
		EntityType getEntity(int64_t id);

		template<typename EntityType>
		void addEntity(const EntityType &entity);

		template<typename EntityType>
		void updateEntity(const EntityType &entity);

		template<typename EntityType>
		void deleteEntity(EntityType &entity);

		template<typename EntityType>
		std::vector<EntityType> getEntitiesInRange(int64_t startId, int64_t endId, size_t limit);

		template<typename EntityType>
		size_t calculateEntityTotalPropertySize(int64_t entityId);

		template<typename EntityType>
		void addEntityProperties(int64_t entityId, const std::unordered_map<std::string, PropertyValue> &properties);

		template<typename EntityType>
		std::unordered_map<std::string, PropertyValue> getEntityProperties(int64_t entityId);

		template<typename EntityType>
		uint64_t findSegmentForEntityId(int64_t id) const;

		// Find edges connected to a node
		std::vector<Edge> findEdgesByNode(int64_t nodeId, const std::string &direction = "both");

		// Build segment indexes from file for fast lookups
		void buildNodeSegmentIndex();
		void buildEdgeSegmentIndex();
		void buildPropertySegmentIndex();
		void buildBlobSegmentIndex();

		// Cache management
		void clearCache();

		[[nodiscard]] bool hasUnsavedChanges() const;
		void markAllSaved();

		void flushToDisk(std::fstream &file);

		template<typename EntityType>
		std::vector<EntityType> getDirtyEntitiesWithChangeTypes(const std::vector<EntityChangeType> &types);

		std::vector<Node> getDirtyNodesWithChangeTypes(const std::vector<EntityChangeType> &types) const;
		std::vector<Edge> getDirtyEdgesWithChangeTypes(const std::vector<EntityChangeType> &types) const;
		std::vector<Property> getDirtyPropertiesWithChangeTypes(const std::vector<EntityChangeType> &types) const;
		std::vector<Blob> getDirtyBlobsWithChangeTypes(const std::vector<EntityChangeType> &types) const;

		// Set maximum dirty entities before auto-flush
		void setMaxDirtyEntities(size_t maxDirtyEntities) { maxDirtyEntities_ = maxDirtyEntities; }

		// Set callback for auto-flush
		void setAutoFlushCallback(std::function<void()> callback) { autoFlushCallback_ = std::move(callback); }

		// Check if we need auto-flush
		[[nodiscard]] bool needsAutoFlush() const {
			return dirtyNodes_.size() >= maxDirtyEntities_ || dirtyEdges_.size() >= maxDirtyEntities_;
		}

		// Trigger auto-flush if needed
		void checkAndTriggerAutoFlush() const;

		// Helper method to retrieve an entity from memory (dirty collections and cache) or disk
		template<typename EntityType>
		EntityType getEntityFromMemoryOrDisk(int64_t id);

		// Load node or edge from disk
		[[nodiscard]] Node loadNodeFromDisk(int64_t id) const;
		[[nodiscard]] Edge loadEdgeFromDisk(int64_t id) const;
		[[nodiscard]] Property loadPropertyFromDisk(int64_t id) const;
		[[nodiscard]] Blob loadBlobFromDisk(int64_t id) const;

		// get cache
		[[nodiscard]] LRUCache<int64_t, Node> &getNodeCache() { return nodeCache_; }
		[[nodiscard]] LRUCache<int64_t, Edge> &getEdgeCache() { return edgeCache_; }
		[[nodiscard]] LRUCache<int64_t, Property> &getPropertyCache() { return propertyCache_; }
		[[nodiscard]] LRUCache<int64_t, Blob> &getBlobCache() { return blobCache_; }

		// get dirty collections
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Node>> &getDirtyNodes() { return dirtyNodes_; }
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Edge>> &getDirtyEdges() { return dirtyEdges_; }
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Property>> &getDirtyProperties() {
			return dirtyProperties_;
		}
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Blob>> &getDirtyBlobs() { return dirtyBlobs_; }

		// Get segment indexes
		[[nodiscard]] const std::vector<SegmentIndex> &getNodeSegmentIndex() const { return nodeSegmentIndex_; }
		[[nodiscard]] const std::vector<SegmentIndex> &getEdgeSegmentIndex() const { return edgeSegmentIndex_; }
		[[nodiscard]] const std::vector<SegmentIndex> &getPropertySegmentIndex() const { return propertySegmentIndex_; }
		[[nodiscard]] const std::vector<SegmentIndex> &getBlobSegmentIndex() const { return blobSegmentIndex_; }

		void handleIdUpdate(int64_t tempId, int64_t permId, uint32_t entityType);

		void setDeletionManager(std::shared_ptr<DeletionManager> deletionManager);

		// Helper method for DeletionManager to update entity status in memory without recursion
		template<typename EntityType>
		void markEntityDeleted(EntityType &entity);

		[[nodiscard]] std::shared_ptr<EntityReferenceUpdater> getEntityReferenceUpdater() const {
			return entityReferenceUpdater_;
		}

	private:
		std::shared_ptr<std::fstream> file_; // Persistent file handle
		std::unique_ptr<BlobChainManager> blobManager_;
		std::shared_ptr<DeletionManager> deletionManager_;
		std::shared_ptr<EntityReferenceUpdater> entityReferenceUpdater_;

		// Cache for frequently accessed nodes and edges
		mutable LRUCache<int64_t, Node> nodeCache_;
		mutable LRUCache<int64_t, Edge> edgeCache_;
		mutable LRUCache<int64_t, Property> propertyCache_;
		mutable LRUCache<int64_t, Blob> blobCache_;

		// Segment indexes for fast lookups
		std::vector<SegmentIndex> nodeSegmentIndex_;
		std::vector<SegmentIndex> edgeSegmentIndex_;
		std::vector<SegmentIndex> propertySegmentIndex_;
		std::vector<SegmentIndex> blobSegmentIndex_;

		// Configuration for dirty tracking
		size_t maxDirtyEntities_ = 1000; // Maximum number of dirty entities before auto-flush

		std::unordered_map<int64_t, DirtyEntityInfo<Node>> dirtyNodes_;
		std::unordered_map<int64_t, DirtyEntityInfo<Edge>> dirtyEdges_;
		std::unordered_map<int64_t, DirtyEntityInfo<Property>> dirtyProperties_;
		std::unordered_map<int64_t, DirtyEntityInfo<Blob>> dirtyBlobs_;

		FileHeader &fileHeader_; // Cached file header

		// ID allocator reference
		std::shared_ptr<IDAllocator> idAllocator_;

		std::shared_ptr<SegmentTracker> segmentTracker_;

		std::shared_ptr<SpaceManager> spaceManager_;

		// Generic segment operations
		void buildSegmentIndex(std::vector<SegmentIndex> &segmentIndex, uint64_t segmentHead) const;
		static uint64_t findSegmentForId(const std::vector<SegmentIndex> &segmentIndex, int64_t id);

		template<typename EntityType>
		std::vector<EntityType> loadEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
														size_t limit) const;

		// Auto-flush callback
		std::function<void()> autoFlushCallback_ = nullptr;

		static constexpr uint32_t PROPERTY_ENTITY_OVERHEAD = 100;

		void ensureBlobStoreInitialized();

		template<typename EntityType>
		std::optional<EntityType> readEntityFromDisk(int64_t fileOffset) const;

		template<typename EntityType>
		std::optional<EntityType> findAndReadEntity(int64_t id) const;

		template<typename EntityType>
		std::vector<EntityType> readEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
														size_t limit, bool filterDeleted = true) const;

		template<typename EntityType>
		void removeEntityProperty(int64_t entityId, const std::string &key);

		// Helper to check if an entity exists in dirty collections
		template<typename EntityType>
		std::optional<EntityType> getEntityFromDirty(int64_t id);

		// Helper to check if a property is in dirty collections
		template<typename EntityType>
		std::optional<PropertyValue> getPropertyFromMemory(int64_t entityId, const std::string &key);

		// Helper to retrieve all properties for an entity, including dirty ones
		template<typename EntityType>
		std::unordered_map<std::string, PropertyValue> getMergedProperties(int64_t entityId);

		template<typename EntityType>
		void updateEntityId(int64_t tempId, int64_t permId);
	};

	template<typename EntityType>
	struct EntityTraits {
		// Will be specialized for each entity type
	};

	// Node specialization
	template<>
	struct EntityTraits<Node> {
		using CacheType = LRUCache<int64_t, Node>;
		using DirtyMapType = std::unordered_map<int64_t, DirtyEntityInfo<Node>>;

		static constexpr uint32_t typeId = Node::typeId;

		static Node get(DataManager *manager, int64_t id) { return manager->getEntityFromMemoryOrDisk<Node>(id); }

		static Node loadFromDisk(const DataManager *manager, int64_t id) { return manager->loadNodeFromDisk(id); }

		static void addToCache(DataManager *manager, const Node &entity) {
			manager->getNodeCache().put(entity.getId(), entity);
		}

		static void removeFromCache(DataManager *manager, int64_t id) { manager->getNodeCache().remove(id); }

		static CacheType &getCache(DataManager *manager) { return manager->getNodeCache(); }

		static DirtyMapType &getDirtyMap(DataManager *manager) { return manager->getDirtyNodes(); }

		static const std::vector<SegmentIndex> &getSegmentIndex(const DataManager *manager) {
			return manager->getNodeSegmentIndex();
		}
	};

	// Edge specialization
	template<>
	struct EntityTraits<Edge> {
		using CacheType = LRUCache<int64_t, Edge>;
		using DirtyMapType = std::unordered_map<int64_t, DirtyEntityInfo<Edge>>;

		static constexpr uint32_t typeId = Edge::typeId;

		static Edge get(DataManager *manager, int64_t id) { return manager->getEntityFromMemoryOrDisk<Edge>(id); }

		static Edge loadFromDisk(const DataManager *manager, int64_t id) { return manager->loadEdgeFromDisk(id); }

		static void addToCache(DataManager *manager, const Edge &entity) {
			manager->getEdgeCache().put(entity.getId(), entity);
		}

		static void removeFromCache(DataManager *manager, int64_t id) { manager->getEdgeCache().remove(id); }

		static CacheType &getCache(DataManager *manager) { return manager->getEdgeCache(); }

		static DirtyMapType &getDirtyMap(DataManager *manager) { return manager->getDirtyEdges(); }

		static const std::vector<SegmentIndex> &getSegmentIndex(const DataManager *manager) {
			return manager->getEdgeSegmentIndex();
		}
	};

	// Property specialization
	template<>
	struct EntityTraits<Property> {
		using CacheType = LRUCache<int64_t, Property>;
		using DirtyMapType = std::unordered_map<int64_t, DirtyEntityInfo<Property>>;

		static constexpr uint32_t typeId = Property::typeId;

		static Property get(DataManager *manager, int64_t id) {
			return manager->getEntityFromMemoryOrDisk<Property>(id);
		}

		static Property loadFromDisk(const DataManager *manager, int64_t id) {
			return manager->loadPropertyFromDisk(id);
		}

		static void addToCache(DataManager *manager, const Property &entity) {
			manager->getPropertyCache().put(entity.getId(), entity);
		}

		static void removeFromCache(DataManager *manager, int64_t id) { manager->getPropertyCache().remove(id); }

		static CacheType &getCache(DataManager *manager) { return manager->getPropertyCache(); }

		static DirtyMapType &getDirtyMap(DataManager *manager) { return manager->getDirtyProperties(); }

		static const std::vector<SegmentIndex> &getSegmentIndex(const DataManager *manager) {
			return manager->getPropertySegmentIndex();
		}
	};

	// Blob specialization
	template<>
	struct EntityTraits<Blob> {
		using CacheType = LRUCache<int64_t, Blob>;
		using DirtyMapType = std::unordered_map<int64_t, DirtyEntityInfo<Blob>>;

		static constexpr uint32_t typeId = Blob::typeId;

		static Blob get(DataManager *manager, int64_t id) { return manager->getEntityFromMemoryOrDisk<Blob>(id); }

		static Blob loadFromDisk(const DataManager *manager, int64_t id) { return manager->loadBlobFromDisk(id); }

		static void addToCache(DataManager *manager, const Blob &entity) {
			manager->getBlobCache().put(entity.getId(), entity);
		}

		static void removeFromCache(DataManager *manager, int64_t id) { manager->getBlobCache().remove(id); }

		static CacheType &getCache(DataManager *manager) { return manager->getBlobCache(); }

		static DirtyMapType &getDirtyMap(DataManager *manager) { return manager->getDirtyBlobs(); }

		static const std::vector<SegmentIndex> &getSegmentIndex(const DataManager *manager) {
			return manager->getBlobSegmentIndex();
		}
	};

} // namespace graph::storage
