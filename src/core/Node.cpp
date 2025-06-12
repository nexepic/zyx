/**
 * @file Node.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Node.hpp"
#include <graph/storage/IDAllocator.hpp>
#include <stdexcept>
#include "graph/core/PropertyValue.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph {

    Node::Node(int64_t id, const std::string &label) {
        metadata.id = id;
        metadata.firstOutEdgeId = 0;
        metadata.firstInEdgeId = 0;
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

    void Node::setFirstOutEdgeId(int64_t edgeId) {
        metadata.firstOutEdgeId = edgeId;
    }

    void Node::setFirstInEdgeId(int64_t edgeId) {
        metadata.firstInEdgeId = edgeId;
    }

    void Node::addProperty(const std::string &key, const PropertyValue &value) {
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

    void Node::serialize(std::ostream &os) const {
        // Write metadata fields individually
        utils::Serializer::writePOD(os, metadata.id);
    	utils::Serializer::writePOD(os, metadata.firstOutEdgeId);
    	utils::Serializer::writePOD(os, metadata.firstInEdgeId);
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
    	node.metadata.firstOutEdgeId = utils::Serializer::readPOD<int64_t>(is);
    	node.metadata.firstInEdgeId = utils::Serializer::readPOD<int64_t>(is);
        node.metadata.propertyEntityId = utils::Serializer::readPOD<int64_t>(is);
        node.metadata.propertyStorageType = utils::Serializer::readPOD<uint32_t>(is);
        node.metadata.isActive = utils::Serializer::readPOD<bool>(is);

        // Read label
        std::string label = utils::Serializer::readString(is);
        node.setLabel(label);

        return node;
    }

    size_t Node::getSerializedSize() const {
        // Calculate size of all metadata fields
        size_t size = 0;
        size += sizeof(metadata.id);                 // int64_t
    	size += sizeof(metadata.firstOutEdgeId);     // int64_t
    	size += sizeof(metadata.firstInEdgeId);      // int64_t
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