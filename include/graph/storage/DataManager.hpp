/**
 * @file DataManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <fstream>
#include <graph/core/BlobChainManager.hpp>
#include <graph/core/Index.hpp>
#include <graph/core/StateChainManager.hpp>
#include <graph/traversal/RelationshipTraversal.hpp>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include "CacheManager.hpp"
#include "DeletionManager.hpp"
#include "IDAllocator.hpp"
#include "SegmentIndexManager.hpp"
#include "StorageHeaders.hpp"
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"

namespace graph::storage {

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
		void addEdge(Edge &edge);
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

		// Index-specific operations
		void addIndexEntity(const Index &index);
		void updateIndexEntity(const Index &index);
		void deleteIndex(Index &index);
		Index getIndex(int64_t id);

		// State-specific operations
		void addStateEntity(const State &state);
		void updateStateEntity(const State &state);
		void deleteState(State &state);
		State getState(int64_t id);

		std::vector<State> getAllStates();

		State findStateByKey(const std::string &key);

		void addStateProperties(const std::string &stateKey,
								const std::unordered_map<std::string, PropertyValue> &properties);
		std::unordered_map<std::string, PropertyValue> getStateProperties(const std::string &stateKey);

		// Reserve temporary IDs
		int64_t reserveTemporaryNodeId();
		int64_t reserveTemporaryEdgeId();
		int64_t reserveTemporaryPropertyId();
		int64_t reserveTemporaryBlobId();
		int64_t reserveTemporaryIndexId();
		int64_t reserveTemporaryStateId();

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
		std::vector<Edge> findEdgesByNode(int64_t nodeId, const std::string &direction = "both") const;

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
		[[nodiscard]] Index loadIndexFromDisk(int64_t id) const;
		[[nodiscard]] State loadStateFromDisk(int64_t id) const;

		// get cache
		[[nodiscard]] LRUCache<int64_t, Node> &getNodeCache() { return nodeCache_; }
		[[nodiscard]] LRUCache<int64_t, Edge> &getEdgeCache() { return edgeCache_; }
		[[nodiscard]] LRUCache<int64_t, Property> &getPropertyCache() { return propertyCache_; }
		[[nodiscard]] LRUCache<int64_t, Blob> &getBlobCache() { return blobCache_; }
		[[nodiscard]] LRUCache<int64_t, Index> &getIndexCache() { return indexCache_; }
		[[nodiscard]] LRUCache<int64_t, State> &getStateCache() { return stateCache_; }

		// get dirty collections
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Node>> &getDirtyNodes() { return dirtyNodes_; }
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Edge>> &getDirtyEdges() { return dirtyEdges_; }
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Property>> &getDirtyProperties() {
			return dirtyProperties_;
		}
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Blob>> &getDirtyBlobs() { return dirtyBlobs_; }
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Index>> &getDirtyIndexes() { return dirtyIndexes_; }
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<State>> &getDirtyStates() { return dirtyStates_; }

		void handleIdUpdate(int64_t tempId, int64_t permId, uint32_t entityType);

		void setDeletionManager(std::shared_ptr<DeletionManager> deletionManager);

		// Helper method for DeletionManager to update entity status in memory without recursion
		template<typename EntityType>
		void markEntityDeleted(EntityType &entity);

		[[nodiscard]] std::shared_ptr<EntityReferenceUpdater> getEntityReferenceUpdater() const {
			return entityReferenceUpdater_;
		}

		std::shared_ptr<SegmentIndexManager> getSegmentIndexManager() const { return segmentIndexManager_; }

		[[nodiscard]] std::shared_ptr<SegmentTracker> getSegmentTracker() const { return segmentTracker_; }

		[[nodiscard]] std::shared_ptr<IDAllocator> getIdAllocator() const { return idAllocator_; }

	private:
		std::shared_ptr<std::fstream> file_; // Persistent file handle
		std::unique_ptr<BlobChainManager> blobManager_;
		std::unique_ptr<DeletionManager> deletionManager_;
		std::shared_ptr<EntityReferenceUpdater> entityReferenceUpdater_;
		std::shared_ptr<SegmentIndexManager> segmentIndexManager_;
		std::unique_ptr<StateChainManager> stateManager_;

		// Cache for frequently accessed nodes and edges
		mutable LRUCache<int64_t, Node> nodeCache_;
		mutable LRUCache<int64_t, Edge> edgeCache_;
		mutable LRUCache<int64_t, Property> propertyCache_;
		mutable LRUCache<int64_t, Blob> blobCache_;
		mutable LRUCache<int64_t, Index> indexCache_;
		mutable LRUCache<int64_t, State> stateCache_;

		// Configuration for dirty tracking
		size_t maxDirtyEntities_ = 1000; // Maximum number of dirty entities before auto-flush

		std::unordered_map<int64_t, DirtyEntityInfo<Node>> dirtyNodes_;
		std::unordered_map<int64_t, DirtyEntityInfo<Edge>> dirtyEdges_;
		std::unordered_map<int64_t, DirtyEntityInfo<Property>> dirtyProperties_;
		std::unordered_map<int64_t, DirtyEntityInfo<Blob>> dirtyBlobs_;
		std::unordered_map<int64_t, DirtyEntityInfo<Index>> dirtyIndexes_;
		std::unordered_map<int64_t, DirtyEntityInfo<State>> dirtyStates_;

		std::unordered_map<std::string, int64_t> stateKeyToIdMap_;

		FileHeader &fileHeader_; // Cached file header

		// ID allocator reference
		std::shared_ptr<IDAllocator> idAllocator_;

		std::shared_ptr<SegmentTracker> segmentTracker_;

		std::shared_ptr<SpaceManager> spaceManager_;

		std::unique_ptr<traversal::RelationshipTraversal> relationshipTraversal_;

		void initializeSegmentIndexes();

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

		static bool isChainHeadState(const State& state);
	};

} // namespace graph::storage
