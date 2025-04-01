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
#include "StorageHeaders.h"
#include "graph/core/Edge.h"
#include "graph/core/Node.h"

#include <unordered_set>

namespace graph::storage {

// Structure to map ID ranges to segment positions
struct SegmentIndex {
    uint64_t startId;
    uint64_t endId;
    uint64_t segmentOffset;
};

class DataManager {
public:
    explicit DataManager(const std::string& dbFilePath, size_t cacheSize = 10000);
    ~DataManager();

    void initialize();
    void refreshFromDisk();

    // Header access
    [[nodiscard]] FileHeader getFileHeader() const { return fileHeader_; }
    FileHeader& getFileHeaderRef() { return fileHeader_; }

    // Node-specific operations
    void addNode(const Node& node);
    Node getNode(uint64_t id);
    void refreshNode(const Node& node);
    std::vector<Node> getNodeBatch(const std::vector<uint64_t>& ids);
    std::vector<Node> getNodesInRange(uint64_t startId, uint64_t endId, size_t limit = 1000);

    // Edge-specific operations
    void addEdge(const Edge& edge);
    Edge getEdge(uint64_t id);
    void refreshEdge(const Edge& edge);
    std::vector<Edge> getEdgeBatch(const std::vector<uint64_t>& ids);
    std::vector<Edge> getEdgesInRange(uint64_t startId, uint64_t endId, size_t limit = 1000);

    // Property operations
    void updateNodeProperty(uint64_t nodeId, const std::string& key, const PropertyValue& value);
    PropertyValue getNodeProperty(uint64_t nodeId, const std::string& key);
    void removeNodeProperty(uint64_t nodeId, const std::string& key);
    void updateEdgeProperty(uint64_t edgeId, const std::string& key, const PropertyValue& value);
    PropertyValue getEdgeProperty(uint64_t edgeId, const std::string& key);
    void removeEdgeProperty(uint64_t edgeId, const std::string& key);

    // Get all properties
    std::unordered_map<std::string, PropertyValue> getNodeProperties(uint64_t nodeId);
    std::unordered_map<std::string, PropertyValue> getEdgeProperties(uint64_t edgeId);

    // Blob operations
    std::string getBlob(uint64_t blobId);
    uint64_t storeBlob(const std::string& data, const std::string& contentType = "text");
    [[nodiscard]] BlobStore& getBlobStore() const;

    // Find edges connected to a node
    std::vector<Edge> findEdgesByNode(uint64_t nodeId, const std::string& direction = "both");

    // Cache management
    void clearCache();

    [[nodiscard]] bool hasUnsavedChanges() const { return !dirtyNodes_.empty() || !dirtyEdges_.empty(); }
    [[nodiscard]] std::vector<Node> getUnsavedNodes() const;
    [[nodiscard]] std::vector<Edge> getUnsavedEdges() const;
    void markAllSaved();

    void flushToDisk(std::fstream& file);

    void markNodePropsDirty(uint64_t nodeId);
    void markEdgePropsDirty(uint64_t edgeId);

    // Get entities with dirty properties
    [[nodiscard]] const std::unordered_set<uint64_t>& getNodesPropsDirty() const;
    [[nodiscard]] const std::unordered_set<uint64_t>& getEdgesPropsDirty() const;

    // Clear dirty flags
    void clearPropsDirtyFlags();

private:
    std::string dbFilePath_;
    std::shared_ptr<std::ifstream> file_; // Persistent file handle

    // Cache for frequently accessed nodes and edges
    LRUCache<uint64_t, Node> nodeCache_;
    LRUCache<uint64_t, Edge> edgeCache_;
    LRUCache<std::pair<uint64_t, std::string>, PropertyValue> propertyCache_;
    LRUCache<uint64_t, std::string> blobCache_;

    // Segment indexes for fast lookups
    std::vector<SegmentIndex> nodeSegmentIndex_;
    std::vector<SegmentIndex> edgeSegmentIndex_;
    std::vector<SegmentIndex> propertySegmentIndex_;

    uint64_t propertySegmentHead_ = 0;

    FileHeader fileHeader_; // Cached file header

    // Generic segment operations
    void buildSegmentIndex(std::vector<SegmentIndex> &segmentIndex, uint64_t segmentHead) const;
	static uint64_t findSegmentForId(const std::vector<SegmentIndex>& segmentIndex, uint64_t id);

    // Find segment containing the given ID
    [[nodiscard]] uint64_t findSegmentForNodeId(uint64_t id) const;
    [[nodiscard]] uint64_t findSegmentForEdgeId(uint64_t id) const;

    // Load node or edge from disk
    [[nodiscard]] Node loadNodeFromDisk(uint64_t id) const;
    [[nodiscard]] Edge loadEdgeFromDisk(uint64_t id) const;

    PropertyValue loadPropertyFromDisk(uint64_t entityId, const std::string& key, bool isNode);

    std::unordered_map<std::string, PropertyValue> loadPropertiesFromDisk(const PropertyReference& ref);

    // Load a batch of items from a segment
    [[nodiscard]] std::vector<Node> loadNodesFromSegment(uint64_t segmentOffset, uint64_t startId, uint64_t endId, size_t limit) const;
    [[nodiscard]] std::vector<Edge> loadEdgesFromSegment(uint64_t segmentOffset, uint64_t startId, uint64_t endId, size_t limit) const;

    // Build segment indexes from file for fast lookups
    void buildNodeSegmentIndex();
    void buildEdgeSegmentIndex();
    void buildPropertySegmentIndex();

    [[nodiscard]] uint64_t findPropertyEntry(uint64_t entityId, uint8_t entityType) const;

    std::unordered_set<uint64_t> dirtyNodes_;
    std::unordered_set<uint64_t> dirtyEdges_;

    // Track entities with dirty properties
    std::unordered_set<uint64_t> nodesPropsDirty;
    std::unordered_set<uint64_t> edgesPropsDirty;

    std::unique_ptr<BlobStore> blobStore_;

    void ensureBlobStoreInitialized();

    // Generic methods
    template<typename EntityType>
    PropertyValue getEntityProperty(uint64_t entityId, const std::string& key, uint8_t entityTypeId);

    template<typename EntityType>
    void updateEntityProperty(uint64_t entityId, const std::string& key, const PropertyValue& value, uint8_t entityTypeId);

    template<typename EntityType>
    void removeEntityProperty(uint64_t entityId, const std::string& key, uint8_t entityTypeId);

    template<typename EntityType>
    std::unordered_map<std::string, PropertyValue> getEntityProperties(uint64_t entityId);

    template<typename EntityType>
    std::vector<EntityType> loadEntitiesFromSegment(uint64_t segmentOffset,
                                                 uint64_t startId,
                                                 uint64_t endId,
                                                 size_t limit) const;

};

} // namespace graph::storage