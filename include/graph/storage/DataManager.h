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
#include "BlobStore.h"

#include <fstream>
#include <string>
#include <vector>
#include "CacheManager.h"
#include "IDAllocator.h"
#include "StorageHeaders.h"
#include "graph/core/Edge.h"
#include "graph/core/Node.h"

#include <unordered_set>

namespace graph::storage {

	// Structure to map ID ranges to segment positions
	struct SegmentIndex {
		int64_t startId;
		int64_t endId;
		uint64_t segmentOffset;
	};

	class DataManager {
	public:
		explicit DataManager(const std::string &dbFilePath, size_t cacheSize, FileHeader &fileHeader,
							 std::shared_ptr<IDAllocator> idAllocator);
		~DataManager();

		// Enhanced dirty tracking structures - separate from cache
		struct DirtyEntityInfo {
			enum class ChangeType { ADDED, MODIFIED, DELETED };
			ChangeType changeType;
			// For deleted entities, we need to keep a copy since they're removed from cache
			std::optional<Node> nodeBackup;
			std::optional<Edge> edgeBackup;
		};

		void initialize();
		// void refreshFromDisk();

		// Header access
		[[nodiscard]] FileHeader getFileHeader() const { return fileHeader_; }
		FileHeader &getFileHeaderRef() { return fileHeader_; }

		// Node-specific operations
		void addNode(const Node &node);
		void updateNode(const Node &node);
		void deleteNode(Node &node);
		Node getNode(int64_t id);
		void refreshNode(const Node &node);
		std::vector<Node> getNodeBatch(const std::vector<int64_t> &ids);
		std::vector<Node> getNodesInRange(int64_t startId, int64_t endId, size_t limit = 1000);

		// Edge-specific operations
		void addEdge(const Edge &edge);
		void updateEdge(const Edge &edge);
		void deleteEdge(Edge &edge);
		Edge getEdge(int64_t id);
		void refreshEdge(const Edge &edge);
		std::vector<Edge> getEdgeBatch(const std::vector<int64_t> &ids);
		std::vector<Edge> getEdgesInRange(int64_t startId, int64_t endId, size_t limit = 1000);

		// Reserve temporary IDs
		int64_t reserveTemporaryNodeId();
		int64_t reserveTemporaryEdgeId();

		// Update entities with permanent IDs after save
		void updateEntitiesWithPermanentIds();

		// Property operations
		void updateNodeProperty(int64_t nodeId, const std::string &key, const PropertyValue &value);
		PropertyValue getNodeProperty(int64_t nodeId, const std::string &key);
		void removeNodeProperty(int64_t nodeId, const std::string &key);
		void updateEdgeProperty(int64_t edgeId, const std::string &key, const PropertyValue &value);
		PropertyValue getEdgeProperty(int64_t edgeId, const std::string &key);
		void removeEdgeProperty(int64_t edgeId, const std::string &key);

		// Get all properties
		std::unordered_map<std::string, PropertyValue> getNodeProperties(int64_t nodeId);
		std::unordered_map<std::string, PropertyValue> getEdgeProperties(int64_t edgeId);

		template<typename EntityType>
		bool entityNeedsPropertyStorage(const EntityType &entity) const;

		template<typename EntityType>
		void storeEntityProperties(EntityType &entity, const std::shared_ptr<std::fstream> &fileStream);

		// Find segment containing the given ID
		[[nodiscard]] uint64_t findSegmentForNodeId(int64_t id) const;
		[[nodiscard]] uint64_t findSegmentForEdgeId(int64_t id) const;

		// Blob operations
		std::string getBlob(uint64_t blobId);
		uint64_t storeBlob(const std::string &data, const std::string &contentType = "text");
		[[nodiscard]] BlobStore &getBlobStore() const;

		// Find edges connected to a node
		std::vector<Edge> findEdgesByNode(int64_t nodeId, const std::string &direction = "both");

		// Build segment indexes from file for fast lookups
		void buildNodeSegmentIndex();
		void buildEdgeSegmentIndex();
		void buildPropertySegmentIndex();

		// Cache management
		void clearCache();

		[[nodiscard]] bool hasUnsavedChanges() const { return !dirtyNodes_.empty() || !dirtyEdges_.empty(); }
		void markAllSaved();

		void flushToDisk(std::fstream &file);

		void markNodeDirty(Node &node);
		void markEdgeDirty(Edge &edge);

		[[nodiscard]] std::vector<Node> getDirtyNodesWithChangeType(DirtyEntityInfo::ChangeType type) const;
		[[nodiscard]] std::vector<Edge> getDirtyEdgesWithChangeType(DirtyEntityInfo::ChangeType type) const;

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

	private:
		std::string dbFilePath_;
		std::shared_ptr<std::ifstream> file_; // Persistent file handle

		// Cache for frequently accessed nodes and edges
		LRUCache<int64_t, Node> nodeCache_;
		LRUCache<int64_t, Edge> edgeCache_;
		LRUCache<std::pair<int64_t, std::string>, PropertyValue> propertyCache_;
		LRUCache<uint64_t, std::string> blobCache_;

		// Segment indexes for fast lookups
		std::vector<SegmentIndex> nodeSegmentIndex_;
		std::vector<SegmentIndex> edgeSegmentIndex_;
		std::vector<SegmentIndex> propertySegmentIndex_;

		uint64_t propertySegmentHead_ = 0;

		FileHeader &fileHeader_; // Cached file header

		// ID allocator reference
		std::shared_ptr<IDAllocator> idAllocator_;

		// Generic segment operations
		void buildSegmentIndex(std::vector<SegmentIndex> &segmentIndex, uint64_t segmentHead) const;
		static uint64_t findSegmentForId(const std::vector<SegmentIndex> &segmentIndex, int64_t id);

		// Load node or edge from disk
		[[nodiscard]] Node loadNodeFromDisk(int64_t id) const;
		[[nodiscard]] Edge loadEdgeFromDisk(int64_t id) const;

		std::unordered_map<std::string, PropertyValue> loadPropertiesFromDisk(const PropertyReference &ref);

		// Load a batch of items from a segment
		[[nodiscard]] std::vector<Node> loadNodesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
															 size_t limit) const;
		[[nodiscard]] std::vector<Edge> loadEdgesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
															 size_t limit) const;

		[[nodiscard]] uint64_t findPropertyEntry(int64_t entityId, uint8_t entityType) const;

		// Configuration for dirty tracking
		size_t maxDirtyEntities_ = 1000; // Maximum number of dirty entities before auto-flush

		std::unordered_map<int64_t, DirtyEntityInfo> dirtyNodes_;
		std::unordered_map<int64_t, DirtyEntityInfo> dirtyEdges_;

		// Auto-flush callback
		std::function<void()> autoFlushCallback_ = nullptr;

		std::unique_ptr<BlobStore> blobStore_;

		void ensureBlobStoreInitialized();

		template<typename EntityType>
		std::optional<EntityType> readEntityFromDisk(int64_t fileOffset) const;

		template<typename EntityType>
		std::optional<EntityType> findAndReadEntity(int64_t id) const;

		template<typename EntityType>
		std::vector<EntityType> readEntitiesFromSegment(uint64_t segmentOffset, int64_t startId, int64_t endId,
														size_t limit, bool filterDeleted = true) const;

		// Generic methods
		template<typename EntityType>
		PropertyValue getEntityProperty(int64_t entityId, const std::string &key, uint8_t entityTypeId);

		template<typename EntityType>
		void updateEntityProperty(int64_t entityId, const std::string &key, const PropertyValue &value);

		template<typename EntityType>
		void removeEntityProperty(int64_t entityId, const std::string &key);

		// Helper method to retrieve an entity from memory (dirty collections and cache) or disk
		template<typename EntityType>
		EntityType getEntityFromMemoryOrDisk(int64_t id);

		// Helper to check if an entity exists in dirty collections
		template<typename EntityType>
		std::optional<EntityType> getEntityFromDirty(int64_t id) const;

		// Helper to check if a property is in dirty collections
		template<typename EntityType>
		std::optional<PropertyValue> getPropertyFromMemory(int64_t entityId, const std::string &key);

		// Helper to retrieve all properties for an entity, including dirty ones
		template<typename EntityType>
		std::unordered_map<std::string, PropertyValue> getMergedProperties(int64_t entityId);
	};

} // namespace graph::storage
