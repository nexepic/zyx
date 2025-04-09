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
#include "graph/utils/FixedSizeSerializer.h"

using namespace graph::storage;

FileStorage::FileStorage(std::string path, size_t cacheSize)
	: dbFilePath(std::move(path)), fileStream(nullptr) {

	if (std::filesystem::exists(dbFilePath)) {
		// Initialize DataManager with the existing file
		dataManager = std::make_unique<DataManager>(dbFilePath, cacheSize);

		// Set up auto-flush callback
		dataManager->setAutoFlushCallback([this]() {
			this->flush();
		});
	}
}

FileStorage::~FileStorage() {
	close();
}

void FileStorage::open() {
	if (isFileOpen) return;

	bool fileExists = std::filesystem::exists(dbFilePath);

	// Create a new file with header if it doesn't exist
	if (!fileExists) {
		fileStream = std::make_shared<std::fstream>(dbFilePath, std::ios::binary | std::ios::out);
		if (!*fileStream) {
			throw std::runtime_error("Cannot create database file");
		}

		// Initialize header
		currentHeader = FileHeader();
		currentHeader.header_crc = utils::calculateCrc(&currentHeader, offsetof(FileHeader, header_crc));

		// Write header
		fileStream->write(reinterpret_cast<char*>(&currentHeader), sizeof(FileHeader));
		fileStream->flush();
		fileStream->close();
	}

	// Open file for reading and writing
	fileStream = std::make_shared<std::fstream>(dbFilePath, std::ios::binary | std::ios::in | std::ios::out);
	if (!*fileStream) {
		throw std::runtime_error("Cannot open database file");
	}

	// Initialize data manager
	dataManager = std::make_unique<DataManager>(dbFilePath, 10000); // Default cache sizes
	dataManager->initialize();

	// After dataManager initialization
	deletionManager = std::make_unique<DeletionManager>(
		fileStream, *this, *dataManager);
	deletionManager->initialize(currentHeader.free_pool_head);

	// Read header into memory
	currentHeader = dataManager->getFileHeader();
	max_node_id = currentHeader.max_node_id;
	max_edge_id = currentHeader.max_edge_id;

	isFileOpen = true;
}

void FileStorage::close() {
    if (isFileOpen) {
        flush(); // Ensure any pending changes are written

        if (fileStream) {
            fileStream->flush();
            fileStream->close();
            fileStream.reset();
        }

        dataManager->clearCache();
        dataManager.reset();
        isFileOpen = false;
    }
}

void FileStorage::save() {
    if (!isFileOpen) {
        throw std::runtime_error("Database must be open before saving");
    }

    // Check if we have any changes to save
    if (!dataManager->hasUnsavedChanges()) {
        return;
    }

    // Get entities by their change type
    auto newNodes = dataManager->getDirtyNodesWithChangeType(DataManager::DirtyEntityInfo::ChangeType::ADDED);
    auto modifiedNodes = dataManager->getDirtyNodesWithChangeType(DataManager::DirtyEntityInfo::ChangeType::MODIFIED);

    auto newEdges = dataManager->getDirtyEdgesWithChangeType(DataManager::DirtyEntityInfo::ChangeType::ADDED);
    auto modifiedEdges = dataManager->getDirtyEdgesWithChangeType(DataManager::DirtyEntityInfo::ChangeType::MODIFIED);

    // First, store properties for all entities that need it
    // For new nodes
    for (auto& node : newNodes) {
        dataManager->storeEntityProperties<Node>(node, fileStream);
    }

    // For modified nodes
    for (auto& node : modifiedNodes) {
        dataManager->storeEntityProperties<Node>(node, fileStream);
    }

    // For new edges
    for (auto& edge : newEdges) {
        dataManager->storeEntityProperties<Edge>(edge, fileStream);
    }

    // For modified edges
    for (auto& edge : modifiedEdges) {
        dataManager->storeEntityProperties<Edge>(edge, fileStream);
    }

    // Now save the actual entities
    // Save new nodes
    if (!newNodes.empty()) {
        std::unordered_map<uint64_t, Node> nodesToSave;
        for (const auto& node : newNodes) {
            nodesToSave[node.getId()] = node;
        }
        saveData(nodesToSave, currentHeader.node_segment_head, NODES_PER_SEGMENT);
    }

    // Save new edges
    if (!newEdges.empty()) {
        std::unordered_map<uint64_t, Edge> edgesToSave;
        for (const auto& edge : newEdges) {
            edgesToSave[edge.getId()] = edge;
        }
        saveData(edgesToSave, currentHeader.edge_segment_head, EDGES_PER_SEGMENT);
    }

    // Update modified nodes in-place
    for (const auto& node : modifiedNodes) {
        updateEntityInPlace(node);
    }

    // Update modified edges in-place
    for (const auto& edge : modifiedEdges) {
        updateEntityInPlace(edge);
    }

    // Update header
    if (deletionManager) {
        currentHeader.free_pool_head = deletionManager->getFreePoolHead();
    }
    currentHeader.property_segment_head = dataManager->getFileHeader().property_segment_head;
    currentHeader.max_node_id = max_node_id;
    currentHeader.max_edge_id = max_edge_id;
    currentHeader.header_crc = utils::calculateCrc(&currentHeader, offsetof(FileHeader, header_crc));

    // Write header
    fileStream->seekp(0);
    fileStream->write(reinterpret_cast<char*>(&currentHeader), sizeof(FileHeader));
    fileStream->flush();

    // Mark everything as saved
    dataManager->markAllSaved();
}

template<typename T>
void FileStorage::saveData(std::unordered_map<uint64_t, T> &data,
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
            fileStream->seekg(static_cast<std::streamoff>(currentSegmentOffset));
            fileStream->read(reinterpret_cast<char*>(&currentSegHeader), sizeof(SegmentHeader));
            if (currentSegHeader.next_segment_offset == 0) break;
            currentSegmentOffset = currentSegHeader.next_segment_offset;
        }
    } else {
        // Allocate first segment
        currentSegmentOffset = allocateSegment(T::typeId, itemsPerSegment);
        segmentHead = currentSegmentOffset;
        fileStream->seekg(static_cast<std::streamoff>(currentSegmentOffset));
        fileStream->read(reinterpret_cast<char*>(&currentSegHeader), sizeof(SegmentHeader));
    }

    auto dataIt = sortedData.begin();
    while (dataIt != sortedData.end()) {
        // Calculate remaining space
        uint32_t remaining = currentSegHeader.capacity - currentSegHeader.used;

        if (remaining == 0) {
            // Allocate new segment and link
            uint64_t newOffset = allocateSegment(T::typeId, itemsPerSegment);
            currentSegHeader.next_segment_offset = newOffset;

            // Update current segment header
            fileStream->seekp(static_cast<std::streamoff>(currentSegmentOffset));
            fileStream->write(reinterpret_cast<char*>(&currentSegHeader), sizeof(SegmentHeader));
            fileStream->flush();

            // Move to new segment
            currentSegmentOffset = newOffset;
            fileStream->seekg(static_cast<std::streamoff>(currentSegmentOffset));
            fileStream->read(reinterpret_cast<char*>(&currentSegHeader), sizeof(SegmentHeader));
            remaining = currentSegHeader.capacity;
        }

        // Calculate number of items to write
        uint32_t writeCount = std::min(remaining, static_cast<uint32_t>(sortedData.end() - dataIt));
        std::vector<T> batch(dataIt, dataIt + writeCount);

        // Write data
        writeSegmentData(currentSegmentOffset, batch, currentSegHeader.used);

        // Reload header to get updated used count
        fileStream->seekg(static_cast<std::streamoff>(currentSegmentOffset));
        fileStream->read(reinterpret_cast<char*>(&currentSegHeader), sizeof(SegmentHeader));

        dataIt += writeCount;
    }
}

uint64_t FileStorage::allocateSegment(uint8_t type, uint32_t capacity) const {
	fileStream->seekp(0, std::ios::end);
	if (!*fileStream) {
		throw std::runtime_error("Failed to seek to end of file.");
	}

	const uint64_t baseOffset = fileStream->tellp();

	SegmentHeader header;
	header.data_type = type;
	header.capacity = capacity;
	header.used = 0;
	header.next_segment_offset = 0;
	header.start_id = 0;

	// Write segment header
	fileStream->write(reinterpret_cast<const char*>(&header), sizeof(SegmentHeader));
	if (!*fileStream) {
		throw std::runtime_error("Failed to write SegmentHeader.");
	}

	// Write empty data area
	size_t itemSize = (type == 0) ? sizeof(Node) : sizeof(Edge);
	size_t dataSize = capacity * itemSize;
	size_t alignedSize = ((dataSize + currentHeader.page_size - 1) / currentHeader.page_size) * currentHeader.page_size;
	std::vector<char> emptyData(alignedSize, 0);
	fileStream->write(emptyData.data(), static_cast<std::streamsize>(alignedSize));
	if (!*fileStream) {
		throw std::runtime_error("Failed to write segment data.");
	}

	fileStream->flush();
	return baseOffset;
}

template<typename T>
void FileStorage::writeSegmentData(uint64_t segmentOffset,
								 const std::vector<T>& data, uint32_t baseUsed) {
	// Read segment header
	SegmentHeader header;
	fileStream->seekg(static_cast<std::streamoff>(segmentOffset));
	fileStream->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

	// Calculate item size
	const size_t itemSize = (header.data_type == 0) ? sizeof(Node) : sizeof(Edge);

	// Write data
	uint64_t dataOffset = segmentOffset + sizeof(SegmentHeader) + baseUsed * itemSize;
	fileStream->seekp(static_cast<std::streamoff>(dataOffset));
	for (const auto& item : data) {
		utils::FixedSizeSerializer::serializeWithFixedSize(*fileStream, item, itemSize);
		dataOffset += itemSize;
	}

	// Update header
	header.used = baseUsed + static_cast<uint32_t>(data.size());
	if (baseUsed == 0 && !data.empty()) {
		header.start_id = data.front().getId();
	}

	// Calculate and write CRC of segment data
	header.segment_crc = utils::calculateCrc(&header, offsetof(SegmentHeader, segment_crc));

	// Write updated header back to file
	fileStream->seekp(static_cast<std::streamoff>(segmentOffset));
	fileStream->write(reinterpret_cast<const char*>(&header), sizeof(SegmentHeader));
	fileStream->flush();
}

template<typename T>
void FileStorage::updateEntityInPlace(const T& entity) {
	uint64_t id = entity.getId();
	uint64_t segmentOffset;

	if constexpr (std::is_same_v<T, Node>) {
		segmentOffset = dataManager->findSegmentForNodeId(id);
	} else {
		segmentOffset = dataManager->findSegmentForEdgeId(id);
	}

	if (segmentOffset == 0) {
		throw std::runtime_error("Cannot update entity: entity not found");
	}

	// Read segment header
	SegmentHeader header;
	fileStream->seekg(static_cast<std::streamoff>(segmentOffset));
	fileStream->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

	// Calculate entity position within segment
	uint64_t entityIndex = id - header.start_id;
	if (entityIndex >= header.capacity) {
		throw std::runtime_error("Entity index out of bounds for segment");
	}

	// Calculate file offset for this entity
	uint64_t entityOffset = segmentOffset + sizeof(SegmentHeader) + entityIndex * sizeof(T);

	// Write entity
	fileStream->seekp(static_cast<std::streamoff>(entityOffset));
	utils::FixedSizeSerializer::serializeWithFixedSize(*fileStream, entity, sizeof(T));
	fileStream->flush();
}

// Specializations for Node and Edge
template void FileStorage::updateEntityInPlace<graph::Node>(const Node& entity);
template void FileStorage::updateEntityInPlace<graph::Edge>(const Edge& entity);

void FileStorage::flush() {
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

void FileStorage::updateNode(const Node& node) {
	if (!isFileOpen) {
		throw std::runtime_error("Database must be open before updating data");
	}

	// Ensure node exists
	uint64_t nodeId = node.getId();
	if (nodeId == 0 || dataManager->findSegmentForNodeId(nodeId) == 0) {
		throw std::runtime_error("Cannot update node with ID " + std::to_string(nodeId) + ": node doesn't exist");
	}

	// Update the node in DataManager
	dataManager->updateNode(node);
}

void FileStorage::updateEdge(const Edge& edge) {
	if (!isFileOpen) {
		throw std::runtime_error("Database must be open before updating data");
	}

	// Ensure edge exists
	uint64_t edgeId = edge.getId();
	if (edgeId == 0 || dataManager->findSegmentForEdgeId(edgeId) == 0) {
		throw std::runtime_error("Cannot update edge with ID " + std::to_string(edgeId) + ": edge doesn't exist");
	}

	// Update the edge in DataManager
	dataManager->updateEdge(edge);
}

uint64_t FileStorage::saveNode(const Node& node) {
	if (!isFileOpen) {
		open();
	}

	if (node.getId() != 0 && dataManager->findSegmentForNodeId(node.getId()) != 0) {
		// Node exists, update it
		updateNode(node);
		return node.getId();
	} else {
		// Node is new, insert it
		return insertNode(node);
	}
}

uint64_t FileStorage::saveEdge(const Edge& edge) {
	if (!isFileOpen) {
		open();
	}

	if (edge.getId() != 0 && dataManager->findSegmentForEdgeId(edge.getId()) != 0) {
		// Edge exists, update it
		updateEdge(edge);
		return edge.getId();
	} else {
		// Edge is new, insert it
		return insertEdge(edge);
	}
}

bool FileStorage::deleteNode(uint64_t nodeId, bool cascadeEdges) {
	if (!isFileOpen) {
		open();
	}
	return deletionManager->deleteNode(nodeId, cascadeEdges);
}

bool FileStorage::deleteEdge(uint64_t edgeId) {
	if (!isFileOpen) {
		open();
	}
	return deletionManager->deleteEdge(edgeId);
}

void FileStorage::compactStorage(bool force) {
	if (!isFileOpen) {
		open();
	}
	deletionManager->compactSegments(force);
}

size_t FileStorage::getDeletedNodeCount() const {
	if (!isFileOpen) {
		const_cast<FileStorage*>(this)->open();
	}
	return deletionManager->getDeletedNodeCount();
}

size_t FileStorage::getDeletedEdgeCount() const {
	if (!isFileOpen) {
		const_cast<FileStorage*>(this)->open();
	}
	return deletionManager->getDeletedEdgeCount();
}

double FileStorage::getFragmentationRatio() const {
	if (!isFileOpen) {
		const_cast<FileStorage*>(this)->open();
	}
	return deletionManager->getFragmentationRatio();
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