/**
 * @file DataManager.hpp
 * @author Nexepic
 * @date 2025/7/24
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

#pragma once

#include <atomic>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <cstdint>

// Platform-specific type definitions for Windows compatibility
#ifdef _WIN32
using ssize_t = intptr_t;
// Don't define off_t - Windows SDK already defines it in sys/types.h
// Use int64_t in function signatures instead to avoid conflicts
#endif

#include "DirtyEntityInfo.hpp"
#include "EntityObserverManager.hpp"
#include "TransactionContext.hpp"
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "graph/core/Transaction.hpp"
#include "graph/storage/FileHeaderManager.hpp"
#include "graph/storage/PageBufferPool.hpp"
#include "graph/storage/PersistenceManager.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/constraints/IEntityValidator.hpp"
#include "graph/storage/indexes/IEntityObserver.hpp"

namespace graph {
	class BlobChainManager;
	class StateChainManager;
} // namespace graph

namespace graph::concurrent {
	class ThreadPool;
}

namespace graph::traversal {
	class RelationshipTraversal;
}

namespace graph::storage {
	struct CommittedSnapshot;
	namespace wal {
		class WALManager;
	}
	namespace state {
		class SystemStateManager;
	}
	class LabelTokenRegistry;

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
							 std::shared_ptr<SpaceManager> spaceManager,
							 const std::string &filePath = "");

		/**
		 * Destructor that ensures the file is closed
		 */
		~DataManager();

		/**
		 * Initializes all managers and components.
		 * @param skipSegmentIndexBuild If true, skips building segment indexes
		 *        (caller has already populated them via StorageBootstrap).
		 */
		void initialize(bool skipSegmentIndexBuild = false);

		// Header access
		[[nodiscard]] FileHeader getFileHeader() const { return fileHeader_; }
		FileHeader &getFileHeaderRef() const { return fileHeader_; }
		[[nodiscard]] uint32_t getFileVersion() const { return fileHeader_.version; }

		void registerObserver(std::shared_ptr<IEntityObserver> observer) {
			observerManager_.registerObserver(std::move(observer));
		}
		void registerValidator(std::shared_ptr<constraints::IEntityValidator> validator) {
			observerManager_.registerValidator(std::move(validator));
		}

		[[nodiscard]] EntityObserverManager &getObserverManager() { return observerManager_; }
		[[nodiscard]] const EntityObserverManager &getObserverManager() const { return observerManager_; }

		template<typename T>
		void updateEntityImpl(const T &entity, std::function<T(int64_t)> getOldFunc,
							  std::function<void(T)> internalUpdateFunc,
							  std::function<void(const T &, const T &)> notifyFunc);

		void initializeLabelRegistry(std::shared_ptr<state::SystemStateManager> sm);

		[[nodiscard]] int64_t getOrCreateLabelId(const std::string &label) const;
		[[nodiscard]] std::string resolveLabel(int64_t labelId) const;

		// Node-specific operations
		void addNode(Node &node) const;
		void addNodes(std::vector<Node> &nodes) const;
		void updateNode(const Node &node);
		void deleteNode(Node &node) const;
		Node getNode(int64_t id) const;
		std::vector<Node> getNodeBatch(const std::vector<int64_t> &ids) const;
		std::vector<Node> getNodesInRange(int64_t startId, int64_t endId, size_t limit = 1000) const;
		// Lock-free property loading for parallel scans. Accepts an already-loaded node
		// to avoid redundant reads, and uses loadEntityDirect for Property entities.
		std::unordered_map<std::string, PropertyValue> getNodePropertiesDirect(const Node &node);
		// Resolve node properties using a pre-loaded property map (avoids per-entity pread).
		// Falls back to getNodePropertiesDirect for blob-stored properties.
		std::unordered_map<std::string, PropertyValue> getNodePropertiesFromMap(
			const Node &node, const std::unordered_map<int64_t, Property> &propertyMap);
		// Bulk-load Property entities by a set of IDs using parallel segment pread.
		std::unordered_map<int64_t, Property> bulkLoadPropertyEntities(
			const std::vector<int64_t> &ids, concurrent::ThreadPool *pool = nullptr) const;
		void addNodeProperties(int64_t nodeId, const std::unordered_map<std::string, PropertyValue> &properties) const;
		void removeNodeProperty(int64_t nodeId, const std::string &key) const;
		std::unordered_map<std::string, PropertyValue> getNodeProperties(int64_t nodeId) const;

		// Edge-specific operations
		void addEdge(Edge &edge) const;
		void addEdges(std::vector<Edge> &edges) const;
		void updateEdge(const Edge &edge);
		void deleteEdge(Edge &edge) const;
		Edge getEdge(int64_t id) const;
		std::vector<Edge> getEdgeBatch(const std::vector<int64_t> &ids) const;
		std::vector<Edge> getEdgesInRange(int64_t startId, int64_t endId, size_t limit = 1000) const;
		void addEdgeProperties(int64_t edgeId, const std::unordered_map<std::string, PropertyValue> &properties) const;
		void removeEdgeProperty(int64_t edgeId, const std::string &key) const;
		std::unordered_map<std::string, PropertyValue> getEdgeProperties(int64_t edgeId) const;
		std::vector<Edge> findEdgesByNode(int64_t nodeId, const std::string &direction = "both") const;

		// Property entity operations
		void addPropertyEntity(Property &property) const;
		void updatePropertyEntity(const Property &property) const;
		void deleteProperty(Property &property) const;
		Property getProperty(int64_t id) const;

		// Blob operations
		void addBlobEntity(Blob &blob) const;
		void updateBlobEntity(const Blob &blob) const;
		void deleteBlob(Blob &blob) const;
		Blob getBlob(int64_t id) const;

		// Index operations
		void addIndexEntity(Index &index) const;
		void updateIndexEntity(const Index &index) const;
		void deleteIndex(Index &index) const;
		Index getIndex(int64_t id) const;

		// State operations
		void addStateEntity(State &state) const;
		void updateStateEntity(const State &state) const;
		void deleteState(State &state) const;
		State getState(int64_t id) const;
		State findStateByKey(const std::string &key) const;
		void addStateProperties(const std::string &stateKey,
								const std::unordered_map<std::string, PropertyValue> &properties,
								bool useBlobStorage = false) const;
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
		uint64_t findSegmentForEntityId(int64_t id) const;

		// Cache management
		void clearCache() const;

		/**
		 * Eagerly closes file handles (readFd_) so the database file can be
		 * deleted on Windows even if the DataManager object is still alive.
		 * Called by FileStorage::close() and by the destructor as a safety net.
		 */
		void closeFileHandles();

		void setMaxDirtyEntities(size_t maxDirtyEntities) const;
		void setAutoFlushCallback(std::function<void()> callback) const;
		void checkAndTriggerAutoFlush() const;

		template<typename EntityType>
		std::vector<DirtyEntityInfo<EntityType>> getDirtyEntityInfos(const std::vector<EntityChangeType> &types);

		// Helper method to retrieve an entity from memory (dirty collections and cache) or disk
		template<typename EntityType>
		EntityType getEntityFromMemoryOrDisk(int64_t id);

		// Snapshot-aware read: checks snapshot dirty state, then cache/disk
		template<typename EntityType>
		EntityType getEntityWithSnapshot(int64_t id, const CommittedSnapshot *snapshot);

		// Lock-free read path for parallel scans. Uses pread() to bypass cache entirely.
		// Only checks dirty entities (uncommitted changes), then reads directly from disk.
		// Returns default-constructed entity (id==0) if not found.
		template<typename EntityType>
		EntityType loadEntityDirect(int64_t id);

		// Bulk-load all active entities of a type using segment-sequential pread.
		// Reads entire segment data areas in single pread calls, drastically reducing
		// syscall count compared to per-entity reads. Returns entities with IDs in
		// [filterStartId, filterEndId]. Pass 0,INT64_MAX to get all.
		template<typename EntityType>
		std::vector<EntityType> bulkLoadEntities(int64_t filterStartId = 0,
												 int64_t filterEndId = INT64_MAX) const;

		// Thread-safe read via pread (no locks needed)
		[[nodiscard]] bool hasPreadSupport() const { return readFd_ != INVALID_FILE_HANDLE; }
		[[nodiscard]] ssize_t preadBytes(void *buf, size_t count, int64_t offset) const;

		// Loading entities from disk
		[[nodiscard]] Node loadNodeFromDisk(int64_t id) const;
		[[nodiscard]] Edge loadEdgeFromDisk(int64_t id) const;
		[[nodiscard]] Property loadPropertyFromDisk(int64_t id) const;
		[[nodiscard]] Blob loadBlobFromDisk(int64_t id) const;
		[[nodiscard]] Index loadIndexFromDisk(int64_t id) const;
		[[nodiscard]] State loadStateFromDisk(int64_t id) const;

		// Page buffer pool access
		[[nodiscard]] PageBufferPool &getPagePool() const { return *pagePool_; }

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
		[[nodiscard]] std::shared_ptr<BlobManager> getBlobManager() const { return blobManager_; }
		[[nodiscard]] std::shared_ptr<IndexEntityManager> getIndexEntityManager() const { return indexEntityManager_; }
		[[nodiscard]] std::shared_ptr<StateManager> getStateManager() const { return stateManager_; }

		[[nodiscard]] std::shared_ptr<DeletionManager> getDeletionManager() const { return deletionManager_; }

		FlushSnapshot prepareFlushSnapshot() const;
		void commitFlushSnapshot() const;
		[[nodiscard]] bool hasUnsavedChanges() const;

		template<typename EntityType>
		std::optional<DirtyEntityInfo<EntityType>> getDirtyInfo(int64_t id);

		template<typename EntityType>
		void setEntityDirty(const DirtyEntityInfo<EntityType> &info);

		std::shared_ptr<PersistenceManager> getPersistenceManager() const { return persistenceManager_; }

		/**
		 * @brief Injects the SystemStateManager dependency.
		 * This must be called after DataManager and SystemStateManager are both created.
		 * This method will initialize components that depend on SystemStateManager (e.g., LabelTokenRegistry).
		 */
		void setSystemStateManager(const std::shared_ptr<state::SystemStateManager> &systemStateManager);

		// Per-transaction snapshot for read-only transactions (thread-local)
		void setCurrentSnapshot(const CommittedSnapshot *snapshot) { currentSnapshot_ = snapshot; }
		void clearCurrentSnapshot() { currentSnapshot_ = nullptr; }
		[[nodiscard]] const CommittedSnapshot *getCurrentSnapshot() const { return currentSnapshot_; }

		// Transaction context management
		void setActiveTransaction(uint64_t txnId) { txnContext_.setActive(txnId); }
		void clearActiveTransaction() { txnContext_.clear(); }
		[[nodiscard]] bool hasActiveTransaction() const { return txnContext_.isActive(); }
		[[nodiscard]] uint64_t getActiveTxnId() const { return txnContext_.activeTxnId(); }
		[[nodiscard]] const std::vector<Transaction::TxnOperation> &getTransactionOps() const {
			return txnContext_.getOps();
		}

		void setWALManager(wal::WALManager *wal) { txnContext_.setWALManager(wal); }
		[[nodiscard]] TransactionContext &getTransactionContext() { return txnContext_; }

		void rollbackActiveTransaction();

	private:
		// Core file and state
		std::shared_ptr<std::fstream> file_; // Persistent file handle
		FileHeader &fileHeader_; // Cached file header

		// Unified page buffer pool (replaces 6 entity-level LRU caches)
		std::unique_ptr<PageBufferPool> pagePool_;

		// Dirty tracking
		std::unordered_map<int64_t, DirtyEntityInfo<Node>> dirtyNodes_;
		std::unordered_map<int64_t, DirtyEntityInfo<Edge>> dirtyEdges_;
		std::unordered_map<int64_t, DirtyEntityInfo<Property>> dirtyProperties_;
		std::unordered_map<int64_t, DirtyEntityInfo<Blob>> dirtyBlobs_;
		std::unordered_map<int64_t, DirtyEntityInfo<Index>> dirtyIndexes_;
		std::unordered_map<int64_t, DirtyEntityInfo<State>> dirtyStates_;

		// Transaction bookkeeping (mutable: logically separate from entity state,
		// needs to be modifiable from const entity mutation methods)
		mutable TransactionContext txnContext_;

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
		std::shared_ptr<BlobManager> blobManager_;
		std::shared_ptr<IndexEntityManager> indexEntityManager_;
		std::shared_ptr<StateManager> stateManager_;

		std::shared_ptr<PersistenceManager> persistenceManager_;

		std::unique_ptr<LabelTokenRegistry> labelRegistry_;

		std::weak_ptr<state::SystemStateManager> systemStateManager_;

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

		EntityObserverManager observerManager_;

		// Thread-local snapshot pointer for read-only transactions.
		// Each reader thread has its own snapshot pointer, set at transaction begin.
		static thread_local const CommittedSnapshot *currentSnapshot_;

		// Native file handle for thread-safe pread() based parallel reads.
		// pread() is atomic (no separate seek+read) and multiple threads
		// can call it concurrently on the same fd without synchronization.
		file_handle_t readFd_ = INVALID_FILE_HANDLE;
	};

} // namespace graph::storage
