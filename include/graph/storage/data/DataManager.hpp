/**
 * @file DataManager.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/24
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <atomic>
#include <fstream>
#include <functional>
#include <memory>
#include <unordered_map>
#include "DirtyEntityInfo.hpp"
#include "EntityChangeType.hpp"
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/CacheManager.hpp"
#include "graph/storage/FileHeaderManager.hpp"

namespace graph {
	class BlobChainManager;
	class StateChainManager;
} // namespace graph

namespace graph::traversal {
	class RelationshipTraversal;
}

namespace graph::storage {

	class IDAllocator;
	class SegmentTracker;
	class SpaceManager;
	class SegmentIndexManager;
	class EntityReferenceUpdater;
	class DeletionManager;
	class NodeManager;
	class EdgeManager;
	class PropertyEntityManager;
	class BlobManager;
	class IndexEntityManager;
	class StateManager;
	class PropertyManager;

	/**
	 * Core data management class coordinating all entity operations
	 * Acts as a facade to specialized entity managers
	 */
	class DataManager : public std::enable_shared_from_this<DataManager> {
	public:
		/**
		 * Constructs a new DataManager with the specified parameters
		 * @param file The file stream for persistent storage
		 * @param cacheSize Size of the entity caches
		 * @param fileHeader Reference to the file header
		 * @param idAllocator Allocator for entity IDs
		 * @param segmentTracker Tracks segment information
		 * @param spaceManager Manages storage space allocation
		 */
		explicit DataManager(std::shared_ptr<std::fstream> file, size_t cacheSize, FileHeader &fileHeader,
							 std::shared_ptr<IDAllocator> idAllocator, std::shared_ptr<SegmentTracker> segmentTracker,
							 std::shared_ptr<SpaceManager> spaceManager);

		/**
		 * Destructor that ensures the file is closed
		 */
		~DataManager();

		/**
		 * Initializes all managers and components
		 */
		void initialize();

		// Header access
		[[nodiscard]] FileHeader getFileHeader() const { return fileHeader_; }
		FileHeader &getFileHeaderRef() const { return fileHeader_; }

		// Node-specific operations
		void addNode(const Node &node) const;
		void updateNode(const Node &node) const;
		void deleteNode(Node &node) const;
		Node getNode(int64_t id) const;
		std::vector<Node> getNodeBatch(const std::vector<int64_t> &ids) const;
		std::vector<Node> getNodesInRange(int64_t startId, int64_t endId, size_t limit = 1000) const;
		void addNodeProperties(int64_t nodeId, const std::unordered_map<std::string, PropertyValue> &properties) const;
		void removeNodeProperty(int64_t nodeId, const std::string &key) const;
		std::unordered_map<std::string, PropertyValue> getNodeProperties(int64_t nodeId) const;
		int64_t reserveTemporaryNodeId() const;

		// Edge-specific operations
		void addEdge(const Edge &edge) const;
		void updateEdge(const Edge &edge) const;
		void deleteEdge(Edge &edge) const;
		Edge getEdge(int64_t id) const;
		std::vector<Edge> getEdgeBatch(const std::vector<int64_t> &ids) const;
		std::vector<Edge> getEdgesInRange(int64_t startId, int64_t endId, size_t limit = 1000) const;
		void addEdgeProperties(int64_t edgeId, const std::unordered_map<std::string, PropertyValue> &properties) const;
		void removeEdgeProperty(int64_t edgeId, const std::string &key) const;
		std::unordered_map<std::string, PropertyValue> getEdgeProperties(int64_t edgeId) const;
		std::vector<Edge> findEdgesByNode(int64_t nodeId, const std::string &direction = "both") const;
		int64_t reserveTemporaryEdgeId() const;

		// Property entity operations
		void addPropertyEntity(const Property &property) const;
		void updatePropertyEntity(const Property &property) const;
		void deleteProperty(Property &property) const;
		Property getProperty(int64_t id) const;
		int64_t reserveTemporaryPropertyId() const;

		// Blob operations
		void addBlobEntity(const Blob &blob) const;
		void updateBlobEntity(const Blob &blob) const;
		void deleteBlob(Blob &blob) const;
		Blob getBlob(int64_t id) const;
		int64_t reserveTemporaryBlobId() const;

		// Index operations
		void addIndexEntity(const Index &index) const;
		void updateIndexEntity(const Index &index) const;
		void deleteIndex(Index &index) const;
		Index getIndex(int64_t id) const;
		int64_t reserveTemporaryIndexId() const;

		// State operations
		void addStateEntity(const State &state) const;
		void updateStateEntity(const State &state) const;
		void deleteState(State &state) const;
		State getState(int64_t id) const;
		int64_t reserveTemporaryStateId() const;
		std::vector<State> getAllStates() const;
		State findStateByKey(const std::string &key) const;
		void addStateProperties(const std::string &stateKey,
								const std::unordered_map<std::string, PropertyValue> &properties) const;
		std::unordered_map<std::string, PropertyValue> getStateProperties(const std::string &stateKey) const;
		void removeState(const std::string &stateKey) const;

		// Generic entity operations
		template<typename EntityType>
		void addToCache(const EntityType &entity);

		template<typename EntityType>
		EntityType getEntity(int64_t id);

		template<typename EntityType>
		void updateEntity(const EntityType &entity);

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

		// Cache management
		void clearCache() const;

		// Transaction management
		[[nodiscard]] bool hasUnsavedChanges() const;
		void markAllSaved();
		static void flushToDisk(std::fstream &file);

		template<typename EntityType>
		std::vector<EntityType> getDirtyEntitiesWithChangeTypes(const std::vector<EntityChangeType> &types);

		// Auto-flush configuration
		void setMaxDirtyEntities(size_t maxDirtyEntities) { maxDirtyEntities_ = maxDirtyEntities; }
		void setAutoFlushCallback(std::function<void()> callback) { autoFlushCallback_ = std::move(callback); }
		[[nodiscard]] bool needsAutoFlush() const {
			return dirtyNodes_.size() >= maxDirtyEntities_ || dirtyEdges_.size() >= maxDirtyEntities_;
		}
		void checkAndTriggerAutoFlush() const;

		// Helper method to retrieve an entity from memory (dirty collections and cache) or disk
		template<typename EntityType>
		EntityType getEntityFromMemoryOrDisk(int64_t id);

		// Loading entities from disk
		[[nodiscard]] Node loadNodeFromDisk(int64_t id) const;
		[[nodiscard]] Edge loadEdgeFromDisk(int64_t id) const;
		[[nodiscard]] Property loadPropertyFromDisk(int64_t id) const;
		[[nodiscard]] Blob loadBlobFromDisk(int64_t id) const;
		[[nodiscard]] Index loadIndexFromDisk(int64_t id) const;
		[[nodiscard]] State loadStateFromDisk(int64_t id) const;

		// Cache access (for backward compatibility)
		[[nodiscard]] LRUCache<int64_t, Node> &getNodeCache() const { return nodeCache_; }
		[[nodiscard]] LRUCache<int64_t, Edge> &getEdgeCache() const { return edgeCache_; }
		[[nodiscard]] LRUCache<int64_t, Property> &getPropertyCache() const { return propertyCache_; }
		[[nodiscard]] LRUCache<int64_t, Blob> &getBlobCache() const { return blobCache_; }
		[[nodiscard]] LRUCache<int64_t, Index> &getIndexCache() const { return indexCache_; }
		[[nodiscard]] LRUCache<int64_t, State> &getStateCache() const { return stateCache_; }

		// Dirty collections access (for backward compatibility)
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Node>> &getDirtyNodes() { return dirtyNodes_; }
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Edge>> &getDirtyEdges() { return dirtyEdges_; }
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Property>> &getDirtyProperties() {
			return dirtyProperties_;
		}
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Blob>> &getDirtyBlobs() { return dirtyBlobs_; }
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<Index>> &getDirtyIndexes() { return dirtyIndexes_; }
		[[nodiscard]] std::unordered_map<int64_t, DirtyEntityInfo<State>> &getDirtyStates() { return dirtyStates_; }

		// ID management
		void handleIdUpdate(int64_t tempId, int64_t permId, uint32_t entityType);

		// Helper method for DeletionManager to update entity status in memory without recursion
		template<typename EntityType>
		void markEntityDeleted(EntityType &entity);

		// Deletion tracking
		void setDeletionFlagReference(std::atomic<bool> *flagReference) {
			deleteOperationPerformedFlag_ = flagReference;
		}
		void markDeletionPerformed() const {
			if (deleteOperationPerformedFlag_) {
				deleteOperationPerformedFlag_->store(true);
			}
		}

		// Component accessors
		[[nodiscard]] std::shared_ptr<EntityReferenceUpdater> getEntityReferenceUpdater() const {
			return entityReferenceUpdater_;
		}
		[[nodiscard]] std::shared_ptr<SegmentIndexManager> getSegmentIndexManager() const {
			return segmentIndexManager_;
		}
		[[nodiscard]] std::shared_ptr<SegmentTracker> getSegmentTracker() const { return segmentTracker_; }
		[[nodiscard]] std::shared_ptr<IDAllocator> getIdAllocator() const { return idAllocator_; }
		[[nodiscard]] std::shared_ptr<traversal::RelationshipTraversal> getRelationshipTraversal() const {
			return relationshipTraversal_;
		}
		[[nodiscard]] std::shared_ptr<PropertyManager> getPropertyManager() const { return propertyManager_; }
		[[nodiscard]] std::shared_ptr<NodeManager> getNodeManager() const { return nodeManager_; }
		[[nodiscard]] std::shared_ptr<EdgeManager> getEdgeManager() const { return edgeManager_; }
		[[nodiscard]] std::shared_ptr<BlobChainManager> getBlobManager() const { return blobChainManager_; }

	private:
		// Core file and state
		std::shared_ptr<std::fstream> file_; // Persistent file handle
		FileHeader &fileHeader_; // Cached file header

		// Caches
		mutable LRUCache<int64_t, Node> nodeCache_;
		mutable LRUCache<int64_t, Edge> edgeCache_;
		mutable LRUCache<int64_t, Property> propertyCache_;
		mutable LRUCache<int64_t, Blob> blobCache_;
		mutable LRUCache<int64_t, Index> indexCache_;
		mutable LRUCache<int64_t, State> stateCache_;

		// Dirty tracking
		std::unordered_map<int64_t, DirtyEntityInfo<Node>> dirtyNodes_;
		std::unordered_map<int64_t, DirtyEntityInfo<Edge>> dirtyEdges_;
		std::unordered_map<int64_t, DirtyEntityInfo<Property>> dirtyProperties_;
		std::unordered_map<int64_t, DirtyEntityInfo<Blob>> dirtyBlobs_;
		std::unordered_map<int64_t, DirtyEntityInfo<Index>> dirtyIndexes_;
		std::unordered_map<int64_t, DirtyEntityInfo<State>> dirtyStates_;

		// Auto-flush settings
		size_t maxDirtyEntities_ = 1000; // Maximum number of dirty entities before auto-flush
		std::function<void()> autoFlushCallback_ = nullptr;

		// Flag for deletion tracking
		std::atomic<bool> *deleteOperationPerformedFlag_ = nullptr;

		// Dependency components
		std::shared_ptr<IDAllocator> idAllocator_;
		std::shared_ptr<SegmentTracker> segmentTracker_;
		std::shared_ptr<SpaceManager> spaceManager_;
		std::shared_ptr<SegmentIndexManager> segmentIndexManager_;
		std::shared_ptr<EntityReferenceUpdater> entityReferenceUpdater_;
		std::shared_ptr<traversal::RelationshipTraversal> relationshipTraversal_;

		// Specialized managers
		std::shared_ptr<BlobChainManager> blobChainManager_;
		std::shared_ptr<DeletionManager> deletionManager_;
		std::shared_ptr<StateChainManager> stateChainManager_;
		std::shared_ptr<PropertyManager> propertyManager_;
		std::shared_ptr<NodeManager> nodeManager_;
		std::shared_ptr<EdgeManager> edgeManager_;
		std::shared_ptr<PropertyEntityManager> propertyEntityManager_;
		std::shared_ptr<BlobManager> blobEntityManager_;
		std::shared_ptr<IndexEntityManager> indexEntityManager_;
		std::shared_ptr<StateManager> stateEntityManager_;

		// Initialization helpers
		void initializeSegmentIndexes() const;
		void initializeManagers();

		// Helper for state operations
		static bool isChainHeadState(const State &state);

		// Entity loading from disk
		template<typename EntityType>
		std::vector<EntityType> loadEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
														size_t limit) const;

		template<typename EntityType>
		std::optional<EntityType> readEntityFromDisk(int64_t fileOffset) const;

		template<typename EntityType>
		std::optional<EntityType> findAndReadEntity(int64_t id) const;

		template<typename EntityType>
		std::vector<EntityType> readEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
														size_t limit, bool filterDeleted = true) const;

		// Entity property operations
		template<typename EntityType>
		void removeEntityProperty(int64_t entityId, const std::string &key);

		// Helper to check if an entity exists in dirty collections
		template<typename EntityType>
		std::optional<EntityType> getEntityFromDirty(int64_t id);

		// ID management helper
		template<typename EntityType>
		void updateEntityId(int64_t tempId, int64_t permId);
	};

} // namespace graph::storage
