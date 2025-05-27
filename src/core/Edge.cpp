/**
 * @file Edge.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Edge.h"
#include <graph/storage/IDAllocator.h>
#include <stdexcept>

#include "graph/utils/Serializer.h"

namespace graph {

    Edge::Edge(int64_t id, int64_t sourceNodeId, int64_t targetNodeId, const std::string &label) {
        metadata.id = id;
        metadata.sourceNodeId = sourceNodeId;
        metadata.targetNodeId = targetNodeId;
        metadata.isActive = true;
        setLabel(label);
    }

    void Edge::setLabel(const std::string& label) {
        // Copy label to fixed-size buffer, ensuring null termination
        size_t copySize = std::min(label.size(), LABEL_BUFFER_SIZE - 1);
        memcpy(labelBuffer, label.data(), copySize);
        labelBuffer[copySize] = '\0';
    }

    std::string Edge::getLabel() const {
        // Return label as string
        return {labelBuffer};
    }

    bool Edge::hasTemporaryId() const {
        return storage::IDAllocator::isTemporaryId(metadata.id);
    }

    void Edge::setPermanentId(int64_t permanentId) {
        if (!hasTemporaryId()) {
            throw std::runtime_error("Cannot set permanent ID for edge that already has one");
        }
        metadata.id = permanentId;
    }

    void Edge::addProperty(const std::string &key, const PropertyValue &value) {
        properties[key] = value;
    }

    bool Edge::hasProperty(const std::string &key) const {
        return properties.count(key) > 0;
    }

    PropertyValue Edge::getProperty(const std::string &key) const {
        auto it = properties.find(key);
        if (it == properties.end()) {
            throw std::out_of_range("Property " + key + " not found");
        }
        return it->second;
    }

    void Edge::removeProperty(const std::string &key) {
        auto it = properties.find(key);
        if (it != properties.end()) {
            properties.erase(it);
        }
    }

    const std::unordered_map<std::string, PropertyValue> &Edge::getProperties() const {
        return properties;
    }

    void Edge::setPropertyEntityId(int64_t propertyId, PropertyStorageType storageType) {
        metadata.propertyEntityId = propertyId;
        metadata.propertyStorageType = static_cast<uint32_t>(storageType);
    }

    size_t Edge::getTotalPropertySize() const {
        size_t totalSize = 0;
        for (const auto &[key, value]: properties) {
            totalSize += key.size();
            totalSize += property_utils::getPropertyValueSize(value);
        }
        return totalSize;
    }

    void Edge::serialize(std::ostream &os) const {
        // Write metadata fields individually
        utils::Serializer::writePOD(os, metadata.id);
        utils::Serializer::writePOD(os, metadata.sourceNodeId);
        utils::Serializer::writePOD(os, metadata.targetNodeId);
        utils::Serializer::writePOD(os, metadata.propertyEntityId);
        utils::Serializer::writePOD(os, metadata.propertyStorageType);
        utils::Serializer::writePOD(os, metadata.isActive);

        // Write label as string (only writes the actual string length, not the full buffer)
        utils::Serializer::writeString(os, getLabel());
    }

    Edge Edge::deserialize(std::istream &is) {
        Edge edge;

        // Read metadata fields individually
        edge.metadata.id = utils::Serializer::readPOD<int64_t>(is);
        edge.metadata.sourceNodeId = utils::Serializer::readPOD<int64_t>(is);
        edge.metadata.targetNodeId = utils::Serializer::readPOD<int64_t>(is);
        edge.metadata.propertyEntityId = utils::Serializer::readPOD<int64_t>(is);
        edge.metadata.propertyStorageType = utils::Serializer::readPOD<uint32_t>(is);
        edge.metadata.isActive = utils::Serializer::readPOD<bool>(is);

        // Read label
        std::string label = utils::Serializer::readString(is);
        edge.setLabel(label);

        return edge;
    }

    size_t Edge::getSerializedSize() const {
        // Calculate size of all metadata fields
        size_t size = 0;
        size += sizeof(metadata.id);                // int64_t
        size += sizeof(metadata.sourceNodeId);      // int64_t
        size += sizeof(metadata.targetNodeId);      // int64_t
        size += sizeof(metadata.propertyEntityId);  // int64_t
        size += sizeof(metadata.propertyStorageType); // uint32_t
        size += sizeof(metadata.isActive);          // bool

        // Calculate size of the serialized string (length prefix + string content)
        std::string label = getLabel();
        size += sizeof(uint32_t);                   // For string length prefix
        size += label.size();                       // Actual string content

        return size;
    }

} // namespace graph