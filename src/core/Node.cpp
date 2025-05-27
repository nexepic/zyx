/**
 * @file Node.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Node.h"
#include <graph/storage/IDAllocator.h>
#include <stdexcept>
#include "graph/core/PropertyValue.h"
#include "graph/utils/Serializer.h"

namespace graph {

    Node::Node(int64_t id, const std::string &label) {
        metadata.id = id;
        setLabel(label);
    }

    void Node::setLabel(const std::string& label) {
        // Copy label to fixed-size buffer, ensuring null termination
        size_t copySize = std::min(label.size(), LABEL_BUFFER_SIZE - 1);
        memcpy(labelBuffer, label.data(), copySize);
        labelBuffer[copySize] = '\0';
    }

    std::string Node::getLabel() const {
        // Return label as string
        return {labelBuffer};
    }

    bool Node::hasTemporaryId() const {
        return storage::IDAllocator::isTemporaryId(metadata.id);
    }

    void Node::setPermanentId(int64_t permanentId) {
        if (!hasTemporaryId()) {
            throw std::runtime_error("Cannot set permanent ID for node that already has one");
        }
        metadata.id = permanentId;
    }

    void Node::addProperty(const std::string &key, const PropertyValue &value) {
    	// TODO: clear existing properties
        properties[key] = value;
    }

    bool Node::hasProperty(const std::string &key) const {
        return properties.count(key) > 0;
    }

    PropertyValue Node::getProperty(const std::string &key) const {
        auto it = properties.find(key);
        if (it == properties.end()) {
            throw std::out_of_range("Property " + key + " not found");
        }
        return it->second;
    }

    void Node::removeProperty(const std::string &key) {
        auto it = properties.find(key);
        if (it != properties.end()) {
            properties.erase(it);
        }
    }

    const std::unordered_map<std::string, PropertyValue> &Node::getProperties() const {
        return properties;
    }

    void Node::setPropertyEntityId(int64_t propertyId, PropertyStorageType storageType) {
        metadata.propertyEntityId = propertyId;
        metadata.propertyStorageType = static_cast<uint32_t>(storageType);
    }

    size_t Node::getTotalPropertySize() const {
        size_t totalSize = 0;
        for (const auto &[key, value]: properties) {
            totalSize += key.size();
            totalSize += property_utils::getPropertyValueSize(value);
        }
        return totalSize;
    }

    void Node::addOutEdge(uint64_t edgeId) {
        outEdges.push_back(edgeId);
    }

    void Node::addInEdge(uint64_t edgeId) {
        inEdges.push_back(edgeId);
    }

    const std::vector<uint64_t> &Node::getOutEdges() const {
        return outEdges;
    }

    const std::vector<uint64_t> &Node::getInEdges() const {
        return inEdges;
    }

	void Node::serialize(std::ostream &os) const {
    	// Write metadata fields individually
    	utils::Serializer::writePOD(os, metadata.id);
    	utils::Serializer::writePOD(os, metadata.propertyEntityId);
    	utils::Serializer::writePOD(os, metadata.propertyStorageType);
    	utils::Serializer::writePOD(os, metadata.isActive);

    	// Write label (using the actual length of the string)
    	std::string label = getLabel();
    	utils::Serializer::writeString(os, label);
    }

	Node Node::deserialize(std::istream &is) {
    	Node node;

    	// Read metadata fields individually
    	node.metadata.id = utils::Serializer::readPOD<int64_t>(is);
    	node.metadata.propertyEntityId = utils::Serializer::readPOD<int64_t>(is);
    	node.metadata.propertyStorageType = utils::Serializer::readPOD<uint32_t>(is);
    	node.metadata.isActive = utils::Serializer::readPOD<bool>(is);

    	// Read label
    	std::string label = utils::Serializer::readString(is);
    	node.setLabel(label);

    	return node;
    }

	// Implementation for Node's getSerializedSize method
	size_t Node::getSerializedSize() const {
    	// Calculate size of all metadata fields
    	size_t size = 0;
    	size += sizeof(metadata.id);                 // int64_t
    	size += sizeof(metadata.propertyEntityId);   // int64_t
    	size += sizeof(metadata.propertyStorageType); // uint32_t
    	size += sizeof(metadata.isActive);           // bool

    	// Calculate size of the serialized string (length prefix + string content)
    	std::string label = getLabel();
    	size += sizeof(uint32_t);                    // For string length prefix
    	size += label.size();                        // Actual string content

    	return size;
    }

} // namespace graph