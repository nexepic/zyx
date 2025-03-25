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
#include <iostream>
#include <stdexcept>

#include "graph/utils/ChecksumUtils.h"

namespace graph::storage {

DataManager::DataManager(const std::string& dbFilePath, size_t cacheSize)
    : dbFilePath_(dbFilePath),
      nodeCache_(cacheSize),
      edgeCache_(cacheSize) {
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
    // Read file header
    file_->seekg(0);
    file_->read(reinterpret_cast<char*>(&fileHeader_), sizeof(FileHeader));

    // Validate magic number
    if (memcmp(fileHeader_.magic, "METRIXDB", 8) != 0) {
        throw std::runtime_error("Invalid file format");
    }

    // Validate header CRC
    uint32_t calculatedCrc = utils::calculateCrc(&fileHeader_, offsetof(FileHeader, header_crc));
    if (calculatedCrc != fileHeader_.header_crc) {
        throw std::runtime_error("Header CRC mismatch, data corruption detected");
    }

    // Build segment indexes for efficient lookups
    buildNodeSegmentIndex();
    buildEdgeSegmentIndex();
}

void DataManager::refreshFromDisk() {
    if (!file_->is_open()) {
        file_->open(dbFilePath_, std::ios::binary);
        if (!file_->good()) {
            throw std::runtime_error("Cannot open database file");
        }
    }

    // Read file header
    file_->seekg(0);
    file_->read(reinterpret_cast<char*>(&fileHeader_), sizeof(FileHeader));

    // Validate magic number
    if (memcmp(fileHeader_.magic, "METRIXDB", 8) != 0) {
        throw std::runtime_error("Invalid file format");
    }

    // Validate header CRC
    uint32_t calculatedCrc = utils::calculateCrc(&fileHeader_, offsetof(FileHeader, header_crc));
    if (calculatedCrc != fileHeader_.header_crc) {
        throw std::runtime_error("Header CRC mismatch, data corruption detected");
    }

    // Rebuild segment indexes
    buildNodeSegmentIndex();
    buildEdgeSegmentIndex();
}

void DataManager::buildNodeSegmentIndex() {
    nodeSegmentIndex_.clear();
    uint64_t currentOffset = fileHeader_.node_segment_head;

    while (currentOffset != 0) {
        SegmentHeader header;
        file_->seekg(static_cast<std::streamoff>(currentOffset));
        file_->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

        SegmentIndex index{};
        index.startId = header.start_id;
        index.endId = header.start_id + header.used - 1;
        index.segmentOffset = currentOffset;

        nodeSegmentIndex_.push_back(index);
        currentOffset = header.next_segment_offset;
    }

    // Sort by startId to enable binary search
    std::sort(nodeSegmentIndex_.begin(), nodeSegmentIndex_.end(),
              [](const SegmentIndex& a, const SegmentIndex& b) {
                  return a.startId < b.startId;
              });
}

void DataManager::buildEdgeSegmentIndex() {
    edgeSegmentIndex_.clear();
    uint64_t currentOffset = fileHeader_.edge_segment_head;

    while (currentOffset != 0) {
        SegmentHeader header;
        file_->seekg(static_cast<std::streamoff>(currentOffset));
        file_->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

        SegmentIndex index{};
        index.startId = header.start_id;
        index.endId = header.start_id + header.used - 1;
        index.segmentOffset = currentOffset;

        edgeSegmentIndex_.push_back(index);
        currentOffset = header.next_segment_offset;
    }

    // Sort by startId to enable binary search
    std::sort(edgeSegmentIndex_.begin(), edgeSegmentIndex_.end(),
              [](const SegmentIndex& a, const SegmentIndex& b) {
                  return a.startId < b.startId;
              });
}

uint64_t DataManager::findSegmentForNodeId(uint64_t id) {
    // Binary search to find the segment containing this ID
    auto it = std::lower_bound(nodeSegmentIndex_.begin(), nodeSegmentIndex_.end(), id,
                              [](const SegmentIndex& index, uint64_t value) {
                                  return index.endId < value;
                              });

    if (it != nodeSegmentIndex_.end() && id >= it->startId && id <= it->endId) {
        return it->segmentOffset;
    }

    return 0; // Not found
}

uint64_t DataManager::findSegmentForEdgeId(uint64_t id) {
    // Binary search to find the segment containing this ID
    auto it = std::lower_bound(edgeSegmentIndex_.begin(), edgeSegmentIndex_.end(), id,
                              [](const SegmentIndex& index, uint64_t value) {
                                  return index.endId < value;
                              });

    if (it != edgeSegmentIndex_.end() && id >= it->startId && id <= it->endId) {
        return it->segmentOffset;
    }

    return 0; // Not found
}

void DataManager::addNode(const Node& node) {
	// Add to cache
	nodeCache_.put(node.getId(), node);

	// Mark as dirty (needs to be persisted)
	dirtyNodes_.insert(node.getId());
}

void DataManager::addEdge(const Edge& edge) {
	// Add to cache
	edgeCache_.put(edge.getId(), edge);

	// Mark as dirty (needs to be persisted)
	dirtyEdges_.insert(edge.getId());
}

std::vector<Node> DataManager::getUnsavedNodes() const {
	std::vector<Node> result;
	result.reserve(dirtyNodes_.size());

	for (uint64_t id : dirtyNodes_) {
		// Access from cache without affecting LRU order
		if (nodeCache_.contains(id)) {
			result.push_back(nodeCache_.peek(id));
		}
	}

	return result;
}

std::vector<Edge> DataManager::getUnsavedEdges() const {
	std::vector<Edge> result;
	result.reserve(dirtyEdges_.size());

	for (uint64_t id : dirtyEdges_) {
		// Access from cache without affecting LRU order
		if (edgeCache_.contains(id)) {
			result.push_back(edgeCache_.peek(id));
		}
	}

	return result;
}

void DataManager::markAllSaved() {
	dirtyNodes_.clear();
	dirtyEdges_.clear();
}

Node DataManager::getNode(uint64_t id) {
    // Check if in cache
    if (nodeCache_.contains(id)) {
        return nodeCache_.get(id);
    }

    // Load from disk
    Node node = loadNodeFromDisk(id);
    if (node.getId() != 0) {  // Valid node
        nodeCache_.put(id, node);
    }

    return node;
}

Edge DataManager::getEdge(uint64_t id) {
    // Check if in cache
    if (edgeCache_.contains(id)) {
        return edgeCache_.get(id);
    }

    // Load from disk
    Edge edge = loadEdgeFromDisk(id);
    if (edge.getId() != 0) {  // Valid edge
        edgeCache_.put(id, edge);
    }

    return edge;
}

Node DataManager::loadNodeFromDisk(uint64_t id) {
    uint64_t segmentOffset = findSegmentForNodeId(id);
    if (segmentOffset == 0) {
        return Node(); // Return empty node if not found
    }

    // Read segment header
    SegmentHeader header;
    file_->seekg(static_cast<std::streamoff>(segmentOffset));
    file_->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

    // Calculate position of node within segment
    uint64_t relativePosition = id - header.start_id;
    if (relativePosition >= header.used) {
        return Node(); // ID is out of range for this segment
    }

	std::cout << "Node ID: " << id << " found in segment at offset " << segmentOffset << std::endl;
	std::cout << "Relative position: " << relativePosition << std::endl;

    // Calculate file offset for this node
    std::streamoff nodeOffset = static_cast<std::streamoff>(
        segmentOffset + sizeof(SegmentHeader) + relativePosition * sizeof(Node));

	std::cout << "segmentOffset: " << segmentOffset << std::endl;
	std::cout << "sizeof(SegmentHeader): " << sizeof(SegmentHeader) << std::endl;
	std::cout << "sizeof(Node): " << sizeof(Node) << std::endl;

	std::cout << "Node offset: " << nodeOffset << std::endl;

    file_->seekg(nodeOffset);
    return Node::deserialize(*file_);
}

Edge DataManager::loadEdgeFromDisk(uint64_t id) {
    uint64_t segmentOffset = findSegmentForEdgeId(id);
    if (segmentOffset == 0) {
        return Edge(); // Return empty edge if not found
    }

    // Read segment header
    SegmentHeader header;
    file_->seekg(static_cast<std::streamoff>(segmentOffset));
    file_->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

    // Calculate position of edge within segment
    uint64_t relativePosition = id - header.start_id;
    if (relativePosition >= header.used) {
        return Edge(); // ID is out of range for this segment
    }

    // Calculate file offset for this edge
    std::streamoff edgeOffset = static_cast<std::streamoff>(
        segmentOffset + sizeof(SegmentHeader) + relativePosition * sizeof(Edge));

    file_->seekg(edgeOffset);
    return Edge::deserialize(*file_);
}

std::vector<Node> DataManager::getNodeBatch(const std::vector<uint64_t>& ids) {
    std::vector<Node> result;
    result.reserve(ids.size());
    std::vector<uint64_t> missedIds;

    // First attempt to get nodes from cache
    for (uint64_t id : ids) {
        if (nodeCache_.contains(id)) {
            result.push_back(nodeCache_.get(id));
        } else {
            missedIds.push_back(id);
        }
    }

    // Group missed IDs by segment for efficient loading
    std::unordered_map<uint64_t, std::vector<uint64_t>> segmentToIds;
    for (uint64_t id : missedIds) {
        uint64_t segmentOffset = findSegmentForNodeId(id);
        if (segmentOffset != 0) {
            segmentToIds[segmentOffset].push_back(id);
        }
    }

    // Load nodes segment by segment
    for (const auto& [segmentOffset, segmentIds] : segmentToIds) {
        SegmentHeader header;
        file_->seekg(static_cast<std::streamoff>(segmentOffset));
        file_->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

        // Get all nodes in this segment that we need
        for (uint64_t id : segmentIds) {
            uint64_t relativePosition = id - header.start_id;
            if (relativePosition < header.used) {
                std::streamoff nodeOffset = static_cast<std::streamoff>(
                    segmentOffset + sizeof(SegmentHeader) + relativePosition * sizeof(Node));

                file_->seekg(nodeOffset);
                Node node = Node::deserialize(*file_);
                result.push_back(node);

                // Add to cache
                nodeCache_.put(id, node);
            }
        }
    }

    return result;
}

std::vector<Edge> DataManager::getEdgeBatch(const std::vector<uint64_t>& ids) {
    // Implementation similar to getNodeBatch but for edges
    std::vector<Edge> result;
    result.reserve(ids.size());
    std::vector<uint64_t> missedIds;

    // First attempt to get edges from cache
    for (uint64_t id : ids) {
        if (edgeCache_.contains(id)) {
            result.push_back(edgeCache_.get(id));
        } else {
            missedIds.push_back(id);
        }
    }

    // Group missed IDs by segment for efficient loading
    std::unordered_map<uint64_t, std::vector<uint64_t>> segmentToIds;
    for (uint64_t id : missedIds) {
        uint64_t segmentOffset = findSegmentForEdgeId(id);
        if (segmentOffset != 0) {
            segmentToIds[segmentOffset].push_back(id);
        }
    }

    // Load edges segment by segment
    for (const auto& [segmentOffset, segmentIds] : segmentToIds) {
        SegmentHeader header;
        file_->seekg(static_cast<std::streamoff>(segmentOffset));
        file_->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

        // Get all edges in this segment that we need
        for (uint64_t id : segmentIds) {
            uint64_t relativePosition = id - header.start_id;
            if (relativePosition < header.used) {
                std::streamoff edgeOffset = static_cast<std::streamoff>(
                    segmentOffset + sizeof(SegmentHeader) + relativePosition * sizeof(Edge));

                file_->seekg(edgeOffset);
                Edge edge = Edge::deserialize(*file_);
                result.push_back(edge);

                // Add to cache
                edgeCache_.put(id, edge);
            }
        }
    }

    return result;
}

std::vector<Node> DataManager::getNodesInRange(uint64_t startId, uint64_t endId, size_t limit) {
    std::vector<Node> result;
    if (startId > endId || limit == 0) {
        return result;
    }

    // Find relevant segments that contain the range
    std::vector<uint64_t> relevantSegments;
    for (const auto& segmentIndex : nodeSegmentIndex_) {
        if ((segmentIndex.startId <= endId && segmentIndex.endId >= startId) &&
            (result.size() < limit)) {
            relevantSegments.push_back(segmentIndex.segmentOffset);
        }
    }

    // Load nodes from each relevant segment
    for (uint64_t segmentOffset : relevantSegments) {
        std::vector<Node> segmentNodes = loadNodesFromSegment(
            segmentOffset, startId, endId, limit - result.size());

        // Add to result and cache
        for (const Node& node : segmentNodes) {
            result.push_back(node);
            nodeCache_.put(node.getId(), node);
        }

        if (result.size() >= limit) {
            break;
        }
    }

    return result;
}

std::vector<Edge> DataManager::getEdgesInRange(uint64_t startId, uint64_t endId, size_t limit) {
    // Implementation similar to getNodesInRange
    std::vector<Edge> result;
    if (startId > endId || limit == 0) {
        return result;
    }

    // Find relevant segments that contain the range
    std::vector<uint64_t> relevantSegments;
    for (const auto& segmentIndex : edgeSegmentIndex_) {
        if ((segmentIndex.startId <= endId && segmentIndex.endId >= startId) &&
            (result.size() < limit)) {
            relevantSegments.push_back(segmentIndex.segmentOffset);
        }
    }

    // Load edges from each relevant segment
    for (uint64_t segmentOffset : relevantSegments) {
        std::vector<Edge> segmentEdges = loadEdgesFromSegment(
            segmentOffset, startId, endId, limit - result.size());

        // Add to result and cache
        for (const Edge& edge : segmentEdges) {
            result.push_back(edge);
            edgeCache_.put(edge.getId(), edge);
        }

        if (result.size() >= limit) {
            break;
        }
    }

    return result;
}

std::vector<Node> DataManager::loadNodesFromSegment(uint64_t segmentOffset,
                                                   uint64_t startId,
                                                   uint64_t endId,
                                                   size_t limit) {
    std::vector<Node> result;
    if (segmentOffset == 0 || limit == 0) {
        return result;
    }

    // Read segment header
    SegmentHeader header;
    file_->seekg(static_cast<std::streamoff>(segmentOffset));
    file_->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

    // Calculate effective start and end positions
    uint64_t effectiveStartId = std::max(startId, header.start_id);
    uint64_t effectiveEndId = std::min(endId, header.start_id + header.used - 1);

    if (effectiveStartId > effectiveEndId) {
        return result;
    }

    // Calculate offsets
    uint64_t startOffset = effectiveStartId - header.start_id;
    uint64_t count = std::min(effectiveEndId - effectiveStartId + 1, static_cast<uint64_t>(limit));

    result.reserve(count);

    // Seek to the first node
    std::streamoff nodeOffset = static_cast<std::streamoff>(
        segmentOffset + sizeof(SegmentHeader) + startOffset * sizeof(Node));
    file_->seekg(nodeOffset);

    // Read nodes
    for (uint64_t i = 0; i < count; ++i) {
        Node node = Node::deserialize(*file_);
        result.push_back(node);
    }

    return result;
}

std::vector<Edge> DataManager::loadEdgesFromSegment(uint64_t segmentOffset,
                                                   uint64_t startId,
                                                   uint64_t endId,
                                                   size_t limit) {
    // Implementation similar to loadNodesFromSegment
    std::vector<Edge> result;
    if (segmentOffset == 0 || limit == 0) {
        return result;
    }

    // Read segment header
    SegmentHeader header;
    file_->seekg(static_cast<std::streamoff>(segmentOffset));
    file_->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

    // Calculate effective start and end positions
    uint64_t effectiveStartId = std::max(startId, header.start_id);
    uint64_t effectiveEndId = std::min(endId, header.start_id + header.used - 1);

    if (effectiveStartId > effectiveEndId) {
        return result;
    }

    // Calculate offsets
    uint64_t startOffset = effectiveStartId - header.start_id;
    uint64_t count = std::min(effectiveEndId - effectiveStartId + 1, static_cast<uint64_t>(limit));

    result.reserve(count);

    // Seek to the first edge
    std::streamoff edgeOffset = static_cast<std::streamoff>(
        segmentOffset + sizeof(SegmentHeader) + startOffset * sizeof(Edge));
    file_->seekg(edgeOffset);

    // Read edges
    for (uint64_t i = 0; i < count; ++i) {
        Edge edge = Edge::deserialize(*file_);
        result.push_back(edge);
    }

    return result;
}

std::vector<Edge> DataManager::findEdgesByNode(uint64_t nodeId, const std::string& direction) {
    std::vector<Edge> result;

    // This is a more complex operation that requires scanning edges
    // In a real implementation, you would have secondary indexes for this
    // For now, we'll scan through edges in batches

    for (const auto& segmentIndex : edgeSegmentIndex_) {
        uint64_t segmentOffset = segmentIndex.segmentOffset;
        SegmentHeader header;
        file_->seekg(static_cast<std::streamoff>(segmentOffset));
        file_->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

        // Scan edges in this segment
        file_->seekg(static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader)));

        for (uint32_t i = 0; i < header.used; ++i) {
            Edge edge = Edge::deserialize(*file_);

            bool match = false;
            if (direction == "outgoing" && edge.getFromNodeId() == nodeId) {
                match = true;
            } else if (direction == "incoming" && edge.getToNodeId() == nodeId) {
                match = true;
            } else if (direction == "both" &&
                      (edge.getFromNodeId() == nodeId || edge.getToNodeId() == nodeId)) {
                match = true;
            }

            if (match) {
                result.push_back(edge);
                edgeCache_.put(edge.getId(), edge);
            }
        }
    }

    return result;
}

void DataManager::clearCache() {
    nodeCache_.clear();
    edgeCache_.clear();
}

void DataManager::resizeCache(size_t newNodeCacheSize, size_t newEdgeCacheSize) {
    // In a real implementation, we'd resize the caches
    // For this example, we'll just clear them
    clearCache();
}

} // namespace graph::storage