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
#include <graph/storage/DatabaseInspector.h>
#include <graph/storage/PropertyStorage.h>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "graph/utils/ChecksumUtils.h"

namespace graph::storage {

DataManager::DataManager(const std::string& dbFilePath, size_t cacheSize)
	: dbFilePath_(dbFilePath),
	  nodeCache_(cacheSize),
	  edgeCache_(cacheSize),
	  propertyCache_(cacheSize * 2),
	  blobCache_(cacheSize / 4) {
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
    if (memcmp(fileHeader_.magic, FILE_HEADER_MAGIC_STRING, 8) != 0) {
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

	propertySegmentHead_ = fileHeader_.property_segment_head;
	if (propertySegmentHead_ != 0) {
		buildPropertySegmentIndex();
	}

	if (fileHeader_.blob_section_head != 0) {
		// Reopen file with read-write access for BlobStore
		auto fileStream = std::make_shared<std::fstream>(dbFilePath_, std::ios::binary | std::ios::in | std::ios::out);
		blobStore_ = std::make_unique<BlobStore>(fileStream, fileHeader_.blob_section_head);
	}
	displayDatabaseStructure(file_);
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
    if (memcmp(fileHeader_.magic, FILE_HEADER_MAGIC_STRING, 8) != 0) {
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

template<typename EntityType>
struct EntityGetter {
	// This will be specialized for Node and Edge
};

// Specialization for Node
template<>
struct EntityGetter<Node> {
	static Node get(DataManager* manager, uint64_t id) {
		return manager->getNode(id);
	}

	static void refresh(DataManager* manager, const Node& entity) {
		manager->refreshNode(entity);
	}

	static void markEntityPropsDirty(DataManager* manager, uint64_t id) {
		manager->markNodePropsDirty(id);
	}

	static constexpr uint8_t typeId = Node::typeId;
};

// Specialization for Edge
template<>
struct EntityGetter<Edge> {
	static Edge get(DataManager* manager, uint64_t id) {
		return manager->getEdge(id);
	}

	static void refresh(DataManager* manager, const Edge& entity) {
		manager->refreshEdge(entity);
	}

	static void markEntityPropsDirty(DataManager* manager, uint64_t id) {
		manager->markEdgePropsDirty(id);
	}

	static constexpr uint8_t typeId = Edge::typeId;
};

// Generic method to build segment index
void DataManager::buildSegmentIndex(std::vector<SegmentIndex>& segmentIndex, uint64_t segmentHead) const {
	segmentIndex.clear();
	uint64_t currentOffset = segmentHead;

	while (currentOffset != 0) {
		SegmentHeader header;
		file_->seekg(static_cast<std::streamoff>(currentOffset));
		file_->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

		SegmentIndex index{};
		index.startId = header.start_id;
		index.endId = header.start_id + header.used - 1;
		index.segmentOffset = currentOffset;

		segmentIndex.push_back(index);
		currentOffset = header.next_segment_offset;
	}

	// Sort by startId to enable binary search
	std::sort(segmentIndex.begin(), segmentIndex.end(),
			  [](const SegmentIndex& a, const SegmentIndex& b) {
				  return a.startId < b.startId;
			  });
}

void DataManager::buildNodeSegmentIndex() {
	buildSegmentIndex(nodeSegmentIndex_, fileHeader_.node_segment_head);
}

void DataManager::buildEdgeSegmentIndex() {
	buildSegmentIndex(edgeSegmentIndex_, fileHeader_.edge_segment_head);
}

void DataManager::buildPropertySegmentIndex() {
    propertySegmentIndex_.clear();
    uint64_t currentOffset = propertySegmentHead_;

    while (currentOffset != 0) {
        PropertySegmentHeader header;
        file_->seekg(static_cast<std::streamoff>(currentOffset));
        file_->read(reinterpret_cast<char*>(&header), sizeof(header));

        // Here we do not store ID ranges like nodes and edges, because property entries are organized by entity ID
        SegmentIndex index{};
        index.segmentOffset = currentOffset;
        index.startId = 0; // For property segments, we do not use ID ranges
        index.endId = 0;

        propertySegmentIndex_.push_back(index);
        currentOffset = header.next_segment_offset;
    }
}

// Generic method to find segment for ID
uint64_t DataManager::findSegmentForId(const std::vector<SegmentIndex>& segmentIndex, uint64_t id) {
	// Binary search to find the segment containing this ID
	auto it = std::lower_bound(segmentIndex.begin(), segmentIndex.end(), id,
							  [](const SegmentIndex& index, uint64_t value) {
								  return index.endId < value;
							  });

	if (it != segmentIndex.end() && id >= it->startId && id <= it->endId) {
		return it->segmentOffset;
	}

	return 0; // Not found
}

uint64_t DataManager::findSegmentForNodeId(uint64_t id) const {
	return findSegmentForId(nodeSegmentIndex_, id);
}

uint64_t DataManager::findSegmentForEdgeId(uint64_t id) const {
	return findSegmentForId(edgeSegmentIndex_, id);
}

uint64_t DataManager::findPropertyEntry(uint64_t entityId, uint8_t entityType) const {
	// Find the property entry for the given entity in the property segments
	for (const auto& segmentIndex : propertySegmentIndex_) {
		uint64_t segmentOffset = segmentIndex.segmentOffset;

		// Read the segment header
		PropertySegmentHeader header;
		file_->seekg(static_cast<std::streamoff>(segmentOffset));
		file_->read(reinterpret_cast<char*>(&header), sizeof(header));

		// Iterate through all entries in the segment
		uint64_t currentPos = segmentOffset + sizeof(PropertySegmentHeader);
		uint64_t endPos = segmentOffset + sizeof(PropertySegmentHeader) + header.used;

		while (currentPos < endPos) {
			file_->seekg(static_cast<std::streamoff>(currentPos));

			// Read the entry header
			PropertyEntryHeader entryHeader;
			file_->read(reinterpret_cast<char*>(&entryHeader), sizeof(entryHeader));

			// Check if it matches
			if (entryHeader.entity_id == entityId && entryHeader.entity_type == entityType) {
				return currentPos;
			}

			// Move to the next entry
			currentPos += sizeof(PropertyEntryHeader) + entryHeader.data_size;
		}
	}

	// Not found
	return 0;
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

void DataManager::refreshNode(const Node &node) {
	// Add to cache
	nodeCache_.put(node.getId(), node);
}

void DataManager::refreshEdge(const Edge &edge) {
	// Add to cache
	edgeCache_.put(edge.getId(), edge);
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

Node DataManager::loadNodeFromDisk(uint64_t id) const {
    uint64_t segmentOffset = findSegmentForNodeId(id);
    if (segmentOffset == 0) {
        std::cerr << "Segment not found for Node ID: " << id << std::endl;
        return {}; // Return empty node if not found
    }

    // Read segment header
    SegmentHeader header;
    file_->seekg(static_cast<std::streamoff>(segmentOffset));
    file_->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

    // Calculate position of node within segment
    uint64_t relativePosition = id - header.start_id;
    if (relativePosition >= header.used) {
        std::cerr << "ID " << id << " is out of range for this segment" << std::endl;
        return {}; // ID is out of range for this segment
    }

    // Calculate file offset for this node
    auto nodeOffset = static_cast<std::streamoff>(
        segmentOffset + sizeof(SegmentHeader) + relativePosition * sizeof(Node));

    file_->seekg(nodeOffset);
    Node node = Node::deserialize(*file_);

    return node;
    // Note: At this point, only the basic data and PropertyReference of the Node are deserialized, but the properties are not loaded.
}

Edge DataManager::loadEdgeFromDisk(uint64_t id) const {
    uint64_t segmentOffset = findSegmentForEdgeId(id);
    if (segmentOffset == 0) {
        return {}; // Return empty edge if not found
    }

    // Read segment header
    SegmentHeader header;
    file_->seekg(static_cast<std::streamoff>(segmentOffset));
    file_->read(reinterpret_cast<char*>(&header), sizeof(SegmentHeader));

    // Calculate position of edge within segment
    uint64_t relativePosition = id - header.start_id;
    if (relativePosition >= header.used) {
        return {}; // ID is out of range for this segment
    }

    // Calculate file offset for this edge
    auto edgeOffset = static_cast<std::streamoff>(
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
                auto nodeOffset = static_cast<std::streamoff>(
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
                auto edgeOffset = static_cast<std::streamoff>(
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

// Generic method to load entities from segment
template<typename EntityType>
std::vector<EntityType> DataManager::loadEntitiesFromSegment(uint64_t segmentOffset,
															 uint64_t startId,
															 uint64_t endId,
															 size_t limit) const {
	std::vector<EntityType> result;
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

	// Seek to the first entity
	auto entityOffset = static_cast<std::streamoff>(
		segmentOffset + sizeof(SegmentHeader) + startOffset * sizeof(EntityType));
	file_->seekg(entityOffset);

	// Read entities
	for (uint64_t i = 0; i < count; ++i) {
		EntityType entity = EntityType::deserialize(*file_);
		result.push_back(entity);
	}

	return result;
}

std::vector<Node> DataManager::loadNodesFromSegment(uint64_t segmentOffset,
													   uint64_t startId,
													   uint64_t endId,
													   size_t limit) const {
	return loadEntitiesFromSegment<Node>(segmentOffset, startId, endId, limit);
}

std::vector<Edge> DataManager::loadEdgesFromSegment(uint64_t segmentOffset,
													   uint64_t startId,
													   uint64_t endId,
													   size_t limit) const {
	return loadEntitiesFromSegment<Edge>(segmentOffset, startId, endId, limit);
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

template<typename EntityType>
void DataManager::updateEntityProperty(uint64_t entityId, const std::string& key, const PropertyValue& value, uint8_t entityTypeId) {
    // Get the entity
    EntityType entity = EntityGetter<EntityType>::get(this, entityId);

    if (entity.getId() == 0) {
        throw std::runtime_error("Entity " + std::to_string(entityId) + " not found");
    }

    // IMPORTANT: Load all existing properties first
    // This ensures we don't lose existing properties
    const PropertyReference& propRef = entity.getPropertyReference();
    if (propRef.type != PropertyReference::StorageType::NONE) {
        // Load existing properties from disk
        auto properties = loadPropertiesFromDisk(propRef);

        // Apply existing properties to the entity
        for (const auto& [propKey, propValue] : properties) {
            if (propKey != key) { // Skip the key we're about to update
                entity.addProperty(propKey, propValue);
                propertyCache_.put({entityId, propKey}, propValue);
            }
        }
    }

    // Now add/update the property
    entity.addProperty(key, value);

    // If the total size of properties exceeds the threshold, store externally
    if (entity.getTotalPropertySize() > MAX_INLINE_PROPERTY_SIZE) {
        auto fileStream = std::make_shared<std::fstream>(dbFilePath_, std::ios::binary | std::ios::in | std::ios::out);

        // Store all properties (existing ones + updated one)
        PropertyReference newPropRef = property_storage::PropertyStorage::storeProperties(
            fileStream,
            entityId,
            entityTypeId,
            entity.getProperties(), // This now contains all properties
            propertySegmentHead_,
            *this
        );

        // Set the property reference WITHOUT clearing properties
        entity.setPropertyReference(newPropRef);
    }

    // Cache the updated property
    std::pair cacheKey = {entityId, key};
    propertyCache_.put(cacheKey, value);

    // Update the entity
    EntityGetter<EntityType>::refresh(this, entity);

    // Mark as property dirty
    EntityGetter<EntityType>::markEntityPropsDirty(this, entityId);
}

void DataManager::updateNodeProperty(uint64_t nodeId, const std::string& key, const PropertyValue& value) {
	updateEntityProperty<Node>(nodeId, key, value, Node::typeId);
}

void DataManager::updateEdgeProperty(uint64_t edgeId, const std::string& key, const PropertyValue& value) {
	updateEntityProperty<Edge>(edgeId, key, value, Edge::typeId);
}

// Generic method to handle entity properties
template<typename EntityType>
PropertyValue DataManager::getEntityProperty(uint64_t entityId, const std::string& key, uint8_t entityTypeId) {
    // First check the property cache
    std::pair<uint64_t, std::string> cacheKey = {entityId, key};
    if (propertyCache_.contains(cacheKey)) {
        return propertyCache_.get(cacheKey);
    }

    // Get the entity (only load basic information and property reference)
	EntityType entity = EntityGetter<EntityType>::get(this, entityId);

    if (entity.getId() == 0) {
        throw std::out_of_range("Entity " + std::to_string(entityId) + " not found");
    }

    // Get the property reference
    const PropertyReference& propRef = entity.getPropertyReference();

    // If the entity has no property reference, check for inline properties
    if (propRef.type == PropertyReference::StorageType::NONE) {
        if (entity.hasProperty(key)) {
            PropertyValue value = entity.getProperty(key);
            propertyCache_.put(cacheKey, value);
            return value;
        }
        throw std::out_of_range("Property " + key + " not found for entity " + std::to_string(entityId));
    }

    // If there is a property reference, load properties from disk
    uint64_t entryOffset = 0;
    if (propRef.type == PropertyReference::StorageType::SEGMENT) {
        entryOffset = propRef.reference;
    } else if (propRef.type == PropertyReference::StorageType::BLOB) {
        // Properties are stored in a BLOB, load all properties directly
        auto properties = loadPropertiesFromDisk(propRef);
        // Update the entity's property set
        for (const auto& [propKey, propValue] : properties) {
            entity.addProperty(propKey, propValue);
            propertyCache_.put({entityId, propKey}, propValue);
        }

        // Check for the specific property needed
        if (properties.find(key) != properties.end()) {
            return properties.at(key);
        }
        throw std::out_of_range("Property " + key + " not found for entity " + std::to_string(entityId));
    }

    // If we have a property entry offset, load it
    if (entryOffset != 0 || (entryOffset = findPropertyEntry(entityId, entityTypeId)) != 0) {
        file_->seekg(static_cast<std::streamoff>(entryOffset));

        // Read the property entry header
        PropertyEntryHeader entryHeader;
        file_->read(reinterpret_cast<char*>(&entryHeader), sizeof(entryHeader));

        // Deserialize properties
        auto properties = property_storage::PropertyStorage::deserialize(*file_);

        // Update the entity's property set and cache all properties
        for (const auto& [propKey, propValue] : properties) {
            entity.addProperty(propKey, propValue);
            propertyCache_.put({entityId, propKey}, propValue);
        }

        // Check for the specific property needed
        if (properties.find(key) != properties.end()) {
            return properties.at(key);
        }
    }

    throw std::out_of_range("Property " + key + " not found for entity " + std::to_string(entityId));
}

PropertyValue DataManager::getNodeProperty(uint64_t nodeId, const std::string& key) {
	return getEntityProperty<Node>(nodeId, key, Node::typeId);
}

PropertyValue DataManager::getEdgeProperty(uint64_t edgeId, const std::string& key) {
	return getEntityProperty<Edge>(edgeId, key, Edge::typeId);
}

void DataManager::markNodePropsDirty(uint64_t nodeId) {
    nodesPropsDirty.insert(nodeId);
}

void DataManager::markEdgePropsDirty(uint64_t edgeId) {
    edgesPropsDirty.insert(edgeId);
}

const std::unordered_set<uint64_t>& DataManager::getNodesPropsDirty() const {
    return nodesPropsDirty;
}

const std::unordered_set<uint64_t>& DataManager::getEdgesPropsDirty() const {
    return edgesPropsDirty;
}

void DataManager::clearPropsDirtyFlags() {
    nodesPropsDirty.clear();
    edgesPropsDirty.clear();
}

template<typename EntityType>
void DataManager::removeEntityProperty(uint64_t entityId, const std::string& key, uint8_t entityTypeId) {
    // Get the entity
	EntityType entity = EntityGetter<EntityType>::get(this, entityId);

    if (entity.getId() == 0) {
        throw std::runtime_error("Entity " + std::to_string(entityId) + " not found");
    }

    // Get the entity's property reference
    const PropertyReference& propRef = entity.getPropertyReference();

    // Check if the property exists
    if (!entity.hasProperty(key)) {
        // If the entity does not have this property locally but has a property reference, load all properties
        if (propRef.type != PropertyReference::StorageType::NONE) {
            auto properties = loadPropertiesFromDisk(propRef);

            // If the external storage also does not have this property, return directly
            if (properties.find(key) == properties.end()) {
                return;
            }

            // Load all properties (except the one to be deleted) into the entity
            for (const auto& [propKey, propValue] : properties) {
                if (propKey != key) {
                    entity.addProperty(propKey, propValue);
                }
            }

            // Clear the original property reference as we will recreate it
            entity.setPropertyReference(PropertyReference{});
        } else {
            // If there is neither a local property nor an external reference, return directly
            return;
        }
    } else {
        // If the property exists locally, remove it from the entity
        entity.removeProperty(key);
    }

    // Remove from cache
    std::pair cacheKey = {entityId, key};
    if (propertyCache_.contains(cacheKey)) {
        propertyCache_.remove(cacheKey);
    }

    // If the entity still has properties and the total size is large, store them externally again
    if (!entity.getProperties().empty() && entity.getTotalPropertySize() > SMALL_PROPERTY_THRESHOLD) {
        auto fileStream = std::make_shared<std::fstream>(
            dbFilePath_, std::ios::binary | std::ios::in | std::ios::out);

        // Store the remaining properties again and get a new reference
        PropertyReference newPropRef = property_storage::PropertyStorage::storeProperties(
            fileStream,
            entityId,
            entityTypeId,
            entity.getProperties(),
            propertySegmentHead_,
            *this
        );

        // Set the new property reference
        entity.setPropertyReference(newPropRef);
    }

    // Update the entity
	EntityGetter<EntityType>::refresh(this, entity);
}

void DataManager::removeNodeProperty(uint64_t nodeId, const std::string& key) {
	removeEntityProperty<Node>(nodeId, key, Node::typeId);
}

void DataManager::removeEdgeProperty(uint64_t edgeId, const std::string& key) {
	removeEntityProperty<Edge>(edgeId, key, Edge::typeId);
}

template<typename EntityType>
std::unordered_map<std::string, PropertyValue> DataManager::getEntityProperties(uint64_t entityId) {
    // Get the entity
    EntityType entity = EntityGetter<EntityType>::get(this, entityId);

    if (entity.getId() == 0) {
        throw std::runtime_error("Entity " + std::to_string(entityId) + " not found");
    }

    // Check if we already have properties in memory
    if (!entity.getProperties().empty()) {
        return entity.getProperties();
    }

    // Check the entity's property reference
    const PropertyReference& propRef = entity.getPropertyReference();

    // If the entity has a property reference, load all properties from disk
    if (propRef.type != PropertyReference::StorageType::NONE) {
        auto properties = loadPropertiesFromDisk(propRef);

        // Update the entity's properties and cache
        for (const auto& [key, value] : properties) {
            entity.addProperty(key, value);
            propertyCache_.put({entityId, key}, value);
        }

        // Update the entity in cache
        EntityGetter<EntityType>::refresh(this, entity);
    }

    return entity.getProperties();
}

std::unordered_map<std::string, PropertyValue> DataManager::getNodeProperties(uint64_t nodeId) {
	return getEntityProperties<Node>(nodeId);
}

std::unordered_map<std::string, PropertyValue> DataManager::getEdgeProperties(uint64_t edgeId) {
	return getEntityProperties<Edge>(edgeId);
}

PropertyValue DataManager::loadPropertyFromDisk(uint64_t entityId, const std::string& key, bool isNode) {
    // Find the property entry
    uint64_t entryOffset = findPropertyEntry(entityId, isNode ? Node::typeId : Edge::typeId);
    if (entryOffset == 0) {
        return std::monostate{}; // Property entry not found
    }

    // Read the property data
    file_->seekg(static_cast<std::streamoff>(entryOffset));

    // Read the property entry header
    PropertyEntryHeader entryHeader;
    file_->read(reinterpret_cast<char*>(&entryHeader), sizeof(entryHeader));

    // Deserialize the properties
    auto properties = property_storage::PropertyStorage::deserialize(*file_);

    // Check for the specific property
    if (properties.find(key) != properties.end()) {
        // Cache all properties
        for (const auto& [propKey, propValue] : properties) {
            propertyCache_.put({entityId, propKey}, propValue);
        }
        return properties.at(key);
    }

    return std::monostate{}; // Specific property not found
}

std::unordered_map<std::string, PropertyValue> DataManager::loadPropertiesFromDisk(const PropertyReference& ref) {
    // If the reference is invalid, return an empty map
    if (ref.type == PropertyReference::StorageType::NONE) {
        return {};
	}

	// Load from property segment
	if (ref.type == PropertyReference::StorageType::SEGMENT) {
		file_->seekg(static_cast<std::streamoff>(ref.reference));

		// Skip PropertyEntryHeader
		PropertyEntryHeader entryHeader;
		file_->read(reinterpret_cast<char *>(&entryHeader), sizeof(entryHeader));

		// Deserialize the property data
		return property_storage::PropertyStorage::deserialize(*file_);
	}
	// Load from BLOB
	if (ref.type == PropertyReference::StorageType::BLOB) {
		// Ensure BlobStore is initialized
		ensureBlobStoreInitialized();

		// Get the BLOB data
		std::string blobData = getBlob(ref.reference);
		std::stringstream ss(blobData);
		return property_storage::PropertyStorage::deserialize(ss);
    }

    return {};
}

std::string DataManager::getBlob(uint64_t blobId) {
	// Check cache first
	if (blobCache_.contains(blobId)) {
		return blobCache_.get(blobId);
	}

	ensureBlobStoreInitialized();

	// Load blob from store
	std::string blobData = blobStore_->getBlob(blobId);

	// Add to cache
	blobCache_.put(blobId, blobData);

	return blobData;
}

uint64_t DataManager::storeBlob(const std::string& data, const std::string& contentType) {
	ensureBlobStoreInitialized();

	// Store blob in BlobStore
	uint64_t blobId = blobStore_->storeBlob(data, contentType);

	// Update file header
	fileHeader_.max_blob_id = std::max(fileHeader_.max_blob_id, blobId);

	// Add to cache
	blobCache_.put(blobId, data);

	return blobId;
}

void DataManager::ensureBlobStoreInitialized() {
	if (!blobStore_) {
		auto fileStream = std::make_shared<std::fstream>(
			dbFilePath_, std::ios::binary | std::ios::in | std::ios::out);

		blobStore_ = std::make_unique<BlobStore>(fileStream, fileHeader_.blob_section_head);

		// Allocate blob section if needed
		if (fileHeader_.blob_section_head == 0) {
			fileHeader_.blob_section_head = blobStore_->allocateSection(BLOB_SECTION_SIZE);
		}
	}
}

BlobStore& DataManager::getBlobStore() const {
	if (!blobStore_) {
		throw std::runtime_error("BlobStore is not initialized");
	}
	return *blobStore_;
}

void DataManager::flushToDisk(std::fstream& file) {

	// Flush blob store if needed
	if (blobStore_) {
		blobStore_->flush();
	}

	// Update file header with blob section info
	if (blobStore_ && fileHeader_.blob_section_head == 0) {
		fileHeader_.blob_section_head = file.tellp();
	}

}

void DataManager::clearCache() {
    nodeCache_.clear();
    edgeCache_.clear();
	propertyCache_.clear();
	blobCache_.clear();
}

} // namespace graph::storage