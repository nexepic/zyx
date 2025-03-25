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

    // Node-specific operations
    void addNode(const Node& node);
    Node getNode(uint64_t id);
    std::vector<Node> getNodeBatch(const std::vector<uint64_t>& ids);
    std::vector<Node> getNodesInRange(uint64_t startId, uint64_t endId, size_t limit = 1000);

    // Edge-specific operations
    void addEdge(const Edge& edge);
    Edge getEdge(uint64_t id);
    std::vector<Edge> getEdgeBatch(const std::vector<uint64_t>& ids);
    std::vector<Edge> getEdgesInRange(uint64_t startId, uint64_t endId, size_t limit = 1000);

    // Find edges connected to a node
    std::vector<Edge> findEdgesByNode(uint64_t nodeId, const std::string& direction = "both");

    // Cache management
    void clearCache();
    void resizeCache(size_t newNodeCacheSize, size_t newEdgeCacheSize);

    bool hasUnsavedChanges() const { return !dirtyNodes_.empty() || !dirtyEdges_.empty(); }
    std::vector<Node> getUnsavedNodes() const;
    std::vector<Edge> getUnsavedEdges() const;
    void markAllSaved();

    void flushToDisk(std::fstream& file, uint64_t& nodeSegmentHead, uint64_t& edgeSegmentHead);

private:
    std::string dbFilePath_;
    std::shared_ptr<std::ifstream> file_; // Persistent file handle

    // Cache for frequently accessed nodes and edges
    LRUCache<uint64_t, Node> nodeCache_;
    LRUCache<uint64_t, Edge> edgeCache_;

    // Segment indexes for fast lookups
    std::vector<SegmentIndex> nodeSegmentIndex_;
    std::vector<SegmentIndex> edgeSegmentIndex_;

    FileHeader fileHeader_; // Cached file header

    // Find segment containing the given ID
    uint64_t findSegmentForNodeId(uint64_t id);
    uint64_t findSegmentForEdgeId(uint64_t id);

    // Load node or edge from disk
    Node loadNodeFromDisk(uint64_t id);
    Edge loadEdgeFromDisk(uint64_t id);

    // Load a batch of items from a segment
    std::vector<Node> loadNodesFromSegment(uint64_t segmentOffset, uint64_t startId, uint64_t endId, size_t limit);
    std::vector<Edge> loadEdgesFromSegment(uint64_t segmentOffset, uint64_t startId, uint64_t endId, size_t limit);

    // Build segment indexes from file for fast lookups
    void buildNodeSegmentIndex();
    void buildEdgeSegmentIndex();

    std::unordered_set<uint64_t> dirtyNodes_;
    std::unordered_set<uint64_t> dirtyEdges_;
};

} // namespace graph::storage