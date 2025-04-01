/**
 * @file FileStorage.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/storage/FileStorage.h"
#include <algorithm> // for std::sort
#include <fstream>
#include <graph/storage/BlobStore.h>
#include <graph/storage/PropertyStorage.h>
#include <sstream>
#include <zlib.h>

#include "graph/core/Edge.h"
#include "graph/core/Node.h"
#include "graph/utils/ChecksumUtils.h"
#include "graph/utils/Serializer.h"

using namespace graph::storage;

FileStorage::FileStorage(std::string path, size_t cacheSize)
	: dbFilePath(std::move(path)) {

	if (std::filesystem::exists(dbFilePath)) {
		// Initialize DataManager with the existing file
		dataManager = std::make_unique<DataManager>(dbFilePath, cacheSize);
	}

	// Initialize empty tracking sets
	nodePropsDirty.clear();
	edgePropsDirty.clear();
}

FileStorage::~FileStorage() {
	close();
}

void FileStorage::open() {
    if (isFileOpen) return;

    bool fileExists = std::filesystem::exists(dbFilePath);

    if (!fileExists) {
        // Create a new file with header
        std::fstream file(dbFilePath, std::ios::binary | std::ios::out);
        if (!file) {
            throw std::runtime_error("Cannot create database file");
        }

        // Initialize header
        currentHeader = FileHeader();
        currentHeader.header_crc = utils::calculateCrc(&currentHeader, offsetof(FileHeader, header_crc));

        // Write header
        file.write(reinterpret_cast<char*>(&currentHeader), sizeof(FileHeader));
        file.close();
    }

    // Initialize data manager
    dataManager = std::make_unique<DataManager>(dbFilePath, 10000); // Default cache sizes
    dataManager->initialize();

    // Read header into memory
    currentHeader = dataManager->getFileHeader();
    max_node_id = currentHeader.max_node_id;
    max_edge_id = currentHeader.max_edge_id;

    isFileOpen = true;
}

void FileStorage::close() {
	if (isFileOpen) {
		flush(); // Ensure any pending changes are written
		dataManager->clearCache();
		dataManager.reset();
		isFileOpen = false;
	}
}

void FileStorage::save() {
    if (!isFileOpen) {
        throw std::runtime_error("Database must be open before saving");
    }

    // Check if we only have property changes but no entity changes
    bool onlyPropertyChanges = !dataManager->hasUnsavedChanges() &&
        (!dataManager->getNodesPropsDirty().empty() || !dataManager->getEdgesPropsDirty().empty());

    if (onlyPropertyChanges) {
        // If we only have property changes, use the specialized update method
        updateEntityProperties();

        // Update property segment head in header
        currentHeader.property_segment_head = dataManager->getFileHeader().property_segment_head;

        // Update header CRC
        currentHeader.header_crc = utils::calculateCrc(&currentHeader, offsetof(FileHeader, header_crc));

        // Write header
        std::fstream file(dbFilePath, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(0);
        file.write(reinterpret_cast<char*>(&currentHeader), sizeof(FileHeader));
        file.close();

        // Clear property dirty flags
        dataManager->clearPropsDirtyFlags();
        return;
    }

    // Original save logic for entity changes
    if (!dataManager->hasUnsavedChanges()) {
        return;
    }

    // Open file for writing
    std::fstream file(dbFilePath, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        throw std::runtime_error("Cannot open database file for saving");
    }

    auto filePtr = std::make_shared<std::fstream>(dbFilePath, std::ios::binary | std::ios::in | std::ios::out);

    // Get only unsaved nodes and edges - more efficient than dumping all
    auto dirtyNodes = dataManager->getUnsavedNodes();
    auto dirtyEdges = dataManager->getUnsavedEdges();

    // Convert to map format expected by saveData
    std::unordered_map<uint64_t, Node> nodesToSave;
    for (const auto& node : dirtyNodes) {
        nodesToSave[node.getId()] = node;
    }

    std::unordered_map<uint64_t, Edge> edgesToSave;
    for (const auto& edge : dirtyEdges) {
        edgesToSave[edge.getId()] = edge;
    }

    // Save dirty nodes
    if (!nodesToSave.empty()) {
        saveData(file, nodesToSave, currentHeader.node_segment_head, NODES_PER_SEGMENT);
    }

    // Save dirty edges
    if (!edgesToSave.empty()) {
        saveData(file, edgesToSave, currentHeader.edge_segment_head, EDGES_PER_SEGMENT);
    }

    // // Handle property reference updates for existing entities
    // if (!dataManager->getNodesPropsDirty().empty() || !dataManager->getEdgesPropsDirty().empty()) {
    //     updateEntityProperties();
    // }

    // Update header
    currentHeader.property_segment_head = dataManager->getFileHeader().property_segment_head;
    currentHeader.max_node_id = max_node_id;
    currentHeader.max_edge_id = max_edge_id;
    currentHeader.header_crc = utils::calculateCrc(&currentHeader, offsetof(FileHeader, header_crc));

    // Write header
    file.seekp(0);
    file.write(reinterpret_cast<char*>(&currentHeader), sizeof(FileHeader));

    file.close();

    // Mark everything as saved
    dataManager->markAllSaved();
    dataManager->clearPropsDirtyFlags();

    // Update indexes
    dataManager->refreshFromDisk();
}

template<typename T>
void FileStorage::saveData(std::fstream &file, std::unordered_map<uint64_t, T> &data,
                          uint64_t &segmentHead, uint32_t itemsPerSegment) {
    if (data.empty()) return;

    // Sort data
    std::vector<T> sortedData;
    sortedData.reserve(data.size());
    for (const auto& [id, item] : data) {
        sortedData.push_back(item);
    }
    std::sort(sortedData.begin(), sortedData.end(),
             [](const T& a, const T& b) { return a.getId() < b.getId(); });

    uint64_t currentSegmentOffset = segmentHead;
    SegmentHeader currentSegHeader;

    // Locate the last segment if segmentHead != 0
    if (currentSegmentOffset != 0) {
        while (true) {
            file.seekg(static_cast<std::streamoff>(currentSegmentOffset));
            file.read(reinterpret_cast<char*>(&currentSegHeader), sizeof(SegmentHeader));
            if (currentSegHeader.next_segment_offset == 0) break;
            currentSegmentOffset = currentSegHeader.next_segment_offset;
        }
    } else {
        // Allocate first segment
        currentSegmentOffset = allocateSegment(file, T::typeId, itemsPerSegment);
        segmentHead = currentSegmentOffset;
        file.seekg(static_cast<std::streamoff>(currentSegmentOffset));
        file.read(reinterpret_cast<char*>(&currentSegHeader), sizeof(SegmentHeader));
    }

    auto dataIt = sortedData.begin();
    while (dataIt != sortedData.end()) {
        // Calculate remaining space
        uint32_t remaining = currentSegHeader.capacity - currentSegHeader.used;

        if (remaining == 0) {
            // Allocate new segment and link
            uint64_t newOffset = allocateSegment(file, T::typeId, itemsPerSegment);
            currentSegHeader.next_segment_offset = newOffset;

            // Update current segment header
            file.seekp(static_cast<std::streamoff>(currentSegmentOffset));
            file.write(reinterpret_cast<char*>(&currentSegHeader), sizeof(SegmentHeader));

            // Move to new segment
            currentSegmentOffset = newOffset;
            file.seekg(static_cast<std::streamoff>(currentSegmentOffset));
            file.read(reinterpret_cast<char*>(&currentSegHeader), sizeof(SegmentHeader));
            remaining = currentSegHeader.capacity;
        }

        // Calculate number of items to write
        uint32_t writeCount = std::min(remaining, static_cast<uint32_t>(sortedData.end() - dataIt));
        std::vector<T> batch(dataIt, dataIt + writeCount);

        // Write data
        writeSegmentData(file, currentSegmentOffset, batch, currentSegHeader.used);

        // Reload header
        file.seekg(static_cast<std::streamoff>(currentSegmentOffset));
        file.read(reinterpret_cast<char*>(&currentSegHeader), sizeof(SegmentHeader));
        dataIt += writeCount;
    }
}

uint64_t FileStorage::allocateSegment(std::fstream &file, uint8_t type, uint32_t capacity) const {
	file.seekp(0, std::ios::end);
	if (!file) {
		throw std::runtime_error("Failed to seek to end of file.");
	}

	const uint64_t baseOffset = file.tellp();

	SegmentHeader header;
	header.data_type = type;
	header.capacity = capacity;
	header.used = 0;
	header.next_segment_offset = 0;
	header.start_id = 0;

	// Write segment header
	file.write(reinterpret_cast<const char*>(&header), sizeof(SegmentHeader));
	if (!file) {
		throw std::runtime_error("Failed to write SegmentHeader.");
	}

	// Write empty data area
	size_t itemSize = (type == 0) ? sizeof(Node) : sizeof(Edge);
	size_t dataSize = capacity * itemSize;
	size_t alignedSize = ((dataSize + currentHeader.page_size - 1) / currentHeader.page_size) * currentHeader.page_size;
	std::vector<char> emptyData(alignedSize, 0);
	file.write(emptyData.data(), static_cast<std::streamsize>(alignedSize));
	if (!file) {
		throw std::runtime_error("Failed to write segment data.");
	}

	file.flush();
	return baseOffset;
}

template<typename T>
void FileStorage::writeSegmentData(std::fstream &file, uint64_t segmentOffset,
                                  const std::vector<T>& data, uint32_t baseUsed) {
    // Read segment header
    SegmentHeader header;
    file.seekg(static_cast<std::streamoff>(segmentOffset));
    file.read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

    // Calculate item size
    const size_t itemSize = (header.data_type == 0) ? sizeof(Node) : sizeof(Edge);

    // Write data
    uint64_t dataOffset = segmentOffset + sizeof(SegmentHeader) + baseUsed * itemSize;
    file.seekp(static_cast<std::streamoff>(dataOffset));
    for (const auto& item : data) {
        item.serialize(file);
        dataOffset += itemSize;
    }

    // Update header
    header.used = baseUsed + static_cast<uint32_t>(data.size());
    if (baseUsed == 0 && !data.empty()) {
        header.start_id = data.front().getId();
    }

    // Calculate and write CRC of segment data
    header.segment_crc = utils::calculateCrc(&header, offsetof(SegmentHeader, segment_crc));

    // Write updated header
    file.seekp(static_cast<std::streamoff>(segmentOffset));
    file.write(reinterpret_cast<const char*>(&header), sizeof(SegmentHeader));
    file.flush();
}

void FileStorage::flush() {
	// Before saving, sync dirty property sets from DataManager
	if (dataManager) {
		for (auto nodeId : dataManager->getNodesPropsDirty()) {
			nodePropsDirty.insert(nodeId);
		}

		for (auto edgeId : dataManager->getEdgesPropsDirty()) {
			edgePropsDirty.insert(edgeId);
		}
	}

	save();
}

graph::Node FileStorage::getNode(uint64_t id) {
	if (!isFileOpen) {
		open();
	}
	// Load from data manager
	Node node = dataManager->getNode(id);
	return node;
}

graph::Edge FileStorage::getEdge(uint64_t id) {
    if (!isFileOpen) {
        open();
    }
    Edge edge = dataManager->getEdge(id);
    return edge;
}

std::vector<graph::Node> FileStorage::getNodes(const std::vector<uint64_t>& ids) {
    if (!isFileOpen) {
        open();
    }
    return dataManager->getNodeBatch(ids);
}

std::vector<graph::Edge> FileStorage::getEdges(const std::vector<uint64_t>& ids) {
    if (!isFileOpen) {
        open();
    }
    return dataManager->getEdgeBatch(ids);
}

std::vector<graph::Node> FileStorage::getNodesInRange(uint64_t startId, uint64_t endId, size_t limit) {
    if (!isFileOpen) {
        open();
    }
    return dataManager->getNodesInRange(startId, endId, limit);
}

std::vector<graph::Edge> FileStorage::getEdgesInRange(uint64_t startId, uint64_t endId, size_t limit) {
    if (!isFileOpen) {
        open();
    }
    return dataManager->getEdgesInRange(startId, endId, limit);
}

std::vector<graph::Edge> FileStorage::findEdgesByNode(uint64_t nodeId, const std::string& direction) {
    if (!isFileOpen) {
        open();
    }
    return dataManager->findEdgesByNode(nodeId, direction);
}

std::vector<uint64_t> FileStorage::findConnectedNodeIds(uint64_t nodeId, const std::string& direction) {
    std::vector<Edge> edges = findEdgesByNode(nodeId, direction);
    std::vector<uint64_t> connectedNodeIds;

    for (const auto& edge : edges) {
        if (direction == "outgoing" || direction == "both") {
            if (edge.getFromNodeId() == nodeId) {
                connectedNodeIds.push_back(edge.getToNodeId());
            }
        }
        if (direction == "incoming" || direction == "both") {
            if (edge.getToNodeId() == nodeId) {
                connectedNodeIds.push_back(edge.getFromNodeId());
            }
        }
    }
    return connectedNodeIds;
}

void FileStorage::updateNodeProperty(uint64_t nodeId, const std::string& key, const PropertyValue& value) {
	if (!isFileOpen) {
		open();
	}
	dataManager->updateNodeProperty(nodeId, key, value);
}

void FileStorage::updateEdgeProperty(uint64_t edgeId, const std::string& key, const PropertyValue& value) {
	if (!isFileOpen) {
		open();
	}
	dataManager->updateEdgeProperty(edgeId, key, value);
}

graph::PropertyValue FileStorage::getNodeProperty(uint64_t nodeId, const std::string& key) {
	if (!isFileOpen) {
		open();
	}
	return dataManager->getNodeProperty(nodeId, key);
}

graph::PropertyValue FileStorage::getEdgeProperty(uint64_t edgeId, const std::string& key) {
	if (!isFileOpen) {
		open();
	}
	return dataManager->getEdgeProperty(edgeId, key);
}

std::unordered_map<std::string, graph::PropertyValue> FileStorage::getNodeProperties(uint64_t nodeId) {
	if (!isFileOpen) {
		open();
	}
	return dataManager->getNodeProperties(nodeId);
}

std::unordered_map<std::string, graph::PropertyValue> FileStorage::getEdgeProperties(uint64_t edgeId) {
	if (!isFileOpen) {
		open();
	}
	return dataManager->getEdgeProperties(edgeId);
}

void FileStorage::removeNodeProperty(uint64_t nodeId, const std::string& key) {
	if (!isFileOpen) {
		open();
	}
	dataManager->removeNodeProperty(nodeId, key);
}

void FileStorage::removeEdgeProperty(uint64_t edgeId, const std::string& key) {
	if (!isFileOpen) {
		open();
	}
	dataManager->removeEdgeProperty(edgeId, key);
}

uint64_t FileStorage::storeBlob(const std::string &data, const std::string &contentType) {
	if (!isFileOpen) {
		open();
	}
	return dataManager->storeBlob(data, contentType);
}

BlobStore& FileStorage::getBlobStore() {
	if (!isFileOpen) {
		open();
	}
	return dataManager->getBlobStore();
}

uint64_t FileStorage::insertNode(const Node& node) {
	if (!isFileOpen) {
		throw std::runtime_error("Database must be open before inserting data");
	}

	Node newNode = node;
	if (newNode.getId() == 0) {
		newNode.setId(++max_node_id);
	} else {
		max_node_id = std::max(max_node_id, newNode.getId());
	}
	dataManager->addNode(newNode);
	return newNode.getId();
}

uint64_t FileStorage::insertEdge(const Edge& edge) {
	if (!isFileOpen) {
		throw std::runtime_error("Database must be open before inserting data");
	}

	Edge newEdge = edge;
	if (newEdge.getId() == 0) {
		newEdge.setId(++max_edge_id);
	} else {
		max_edge_id = std::max(max_edge_id, newEdge.getId());
	}
	dataManager->addEdge(newEdge);
	return newEdge.getId();
}

void FileStorage::updateEntityProperties() {
    if (!isFileOpen) {
        throw std::runtime_error("Database must be open before updating properties");
    }

    if (nodePropsDirty.empty() && edgePropsDirty.empty()) {
        return; // Nothing to update
    }

    std::fstream file(dbFilePath, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        throw std::runtime_error("Cannot open database file for property update");
    }

    // Update node properties
    updateNodePropertyReferences(file);

    // Update edge properties
    updateEdgePropertyReferences(file);

    file.close();
}

void FileStorage::updateNodePropertyReferences(std::fstream& file) {
    if (nodePropsDirty.empty()) return;

    uint64_t segmentOffset = currentHeader.node_segment_head;
    SegmentHeader header;

    // Iterate through all segments to find nodes that need property updates
    while (segmentOffset != 0) {
        file.seekg(static_cast<std::streamoff>(segmentOffset));
        file.read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

        // Calculate size of each node
        size_t nodeSize = sizeof(Node);

        // Iterate through nodes in segment
        for (uint32_t i = 0; i < header.used; ++i) {
            uint64_t dataOffset = segmentOffset + sizeof(SegmentHeader) + i * nodeSize;
            file.seekg(static_cast<std::streamoff>(dataOffset));

            // Read just the ID to check if it's in our dirty set
            auto id = utils::Serializer::readPOD<uint64_t>(file);

            if (nodePropsDirty.find(id) != nodePropsDirty.end()) {
                // We found a node that needs property reference update
                // Fetch the current node with updated property reference
                Node node = dataManager->getNode(id);

                // Seek to the position where property reference is stored
                // Skip ID and label (string length + string)
                file.seekg(static_cast<std::streamoff>(dataOffset)); // Go back to start of node

                // Skip ID
                file.seekg(sizeof(uint64_t), std::ios::cur);

                // Skip label (read length, then skip that many bytes)
                auto labelSize = utils::Serializer::readPOD<uint32_t>(file);
                file.seekg(labelSize, std::ios::cur);

                // Now we're at the property reference position
                // Write the updated property reference
                utils::Serializer::writePOD(file, static_cast<uint8_t>(node.getPropertyReference().type));
                utils::Serializer::writePOD(file, node.getPropertyReference().reference);
                utils::Serializer::writePOD(file, node.getPropertyReference().size);

                // Remove from dirty set
                nodePropsDirty.erase(id);

                // If all nodes are processed, we can break early
                if (nodePropsDirty.empty()) break;
            }
        }

        // Move to next segment if needed
        if (!nodePropsDirty.empty() && header.next_segment_offset != 0) {
            segmentOffset = header.next_segment_offset;
        } else {
            break;
        }
    }
}

void FileStorage::updateEdgePropertyReferences(std::fstream& file) {
    // Similar to updateNodePropertyReferences but for edges
    if (edgePropsDirty.empty()) return;

    uint64_t segmentOffset = currentHeader.edge_segment_head;
    SegmentHeader header;

    while (segmentOffset != 0) {
        file.seekg(static_cast<std::streamoff>(segmentOffset));
        file.read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

        // Calculate size of each edge
        size_t edgeSize = sizeof(Edge);

        for (uint32_t i = 0; i < header.used; ++i) {
            uint64_t dataOffset = segmentOffset + sizeof(SegmentHeader) + i * edgeSize;
            file.seekg(static_cast<std::streamoff>(dataOffset));

            auto id = utils::Serializer::readPOD<uint64_t>(file);

            if (edgePropsDirty.find(id) != edgePropsDirty.end()) {
                Edge edge = dataManager->getEdge(id);

                // Similar to node property update, skip to property reference position
                file.seekg(static_cast<std::streamoff>(dataOffset));

                // Skip ID
                file.seekg(sizeof(uint64_t), std::ios::cur);

                // Skip fromNodeId, toNodeId, and type
                file.seekg(sizeof(uint64_t) * 2 + sizeof(uint32_t), std::ios::cur);

                // Skip label
                auto labelSize = utils::Serializer::readPOD<uint32_t>(file);
                file.seekg(labelSize, std::ios::cur);

                // Now at property reference position
                utils::Serializer::writePOD(file, static_cast<uint8_t>(edge.getPropertyReference().type));
                utils::Serializer::writePOD(file, edge.getPropertyReference().reference);
                utils::Serializer::writePOD(file, edge.getPropertyReference().size);

                edgePropsDirty.erase(id);

                if (edgePropsDirty.empty()) break;
            }
        }

        if (!edgePropsDirty.empty() && header.next_segment_offset != 0) {
            segmentOffset = header.next_segment_offset;
        } else {
            break;
        }
    }
}

uint64_t FileStorage::getLastId() const {
    return std::max(max_node_id, max_edge_id);
}

uint64_t FileStorage::getNodeCount() const {
    return max_node_id;  // Estimate; some IDs may be deleted
}

uint64_t FileStorage::getEdgeCount() const {
    return max_edge_id;  // Estimate; some IDs may be deleted
}

void FileStorage::beginWrite() {
    // Placeholder for transaction management
}

void FileStorage::commitWrite() {
    save();
}

void FileStorage::rollbackWrite() {
    // Placeholder for transaction management
}

void FileStorage::clearCache() const {
    dataManager->clearCache();
}

std::unordered_map<uint64_t, graph::Node> FileStorage::getAllNodes() {
    if (!isFileOpen) {
        open();
    }
    // Example approach: fetch IDs in range [1..max_node_id], then batch-load
    std::vector<uint64_t> allIds;
    allIds.reserve(static_cast<size_t>(max_node_id));
    for (uint64_t i = 1; i <= max_node_id; ++i) {
        // You might maintain a deleted-list, but this is a simple approach
        allIds.push_back(i);
    }

    auto nodeVec = dataManager->getNodeBatch(allIds);
    std::unordered_map<uint64_t, graph::Node> result;
    result.reserve(nodeVec.size());
    for (auto &n : nodeVec) {
        if (n.getId() != 0) { // skip invalid / missing
            result[n.getId()] = std::move(n);
        }
    }
    return result;
}

std::unordered_map<uint64_t, graph::Edge> FileStorage::getAllEdges() {
    if (!isFileOpen) {
        open();
    }
    // Similar approach for edges
    std::vector<uint64_t> allIds;
    allIds.reserve(static_cast<size_t>(max_edge_id));
    for (uint64_t i = 1; i <= max_edge_id; ++i) {
        allIds.push_back(i);
    }

    auto edgeVec = dataManager->getEdgeBatch(allIds);
    std::unordered_map<uint64_t, graph::Edge> result;
    result.reserve(edgeVec.size());
    for (auto &e : edgeVec) {
        if (e.getId() != 0) {
            result[e.getId()] = std::move(e);
        }
    }
    return result;
}