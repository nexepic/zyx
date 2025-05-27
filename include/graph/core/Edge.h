/**
 * @file Edge.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <ostream>
#include <string>
#include <unordered_map>
#include "PropertyValue.h"
#include "Types.h"

#include <graph/utils/Serializer.h>

namespace graph {

    class Edge {
    public:
        // Metadata struct to contain fixed edge data
        struct Metadata {
            int64_t id = 0;
            int64_t sourceNodeId = 0;
            int64_t targetNodeId = 0;
            int64_t propertyEntityId = 0;
            uint32_t propertyStorageType = 0; // Corresponds to PropertyStorageType enum
            bool isActive = true;
        };

        static constexpr size_t TOTAL_EDGE_SIZE = 256;
        static constexpr size_t METADATA_SIZE = sizeof(Metadata);
        static constexpr size_t LABEL_BUFFER_SIZE = TOTAL_EDGE_SIZE - METADATA_SIZE;
        static constexpr uint32_t typeId = static_cast<uint32_t>(EntityType::Edge);

        static constexpr size_t getTotalSize() {
        	return TOTAL_EDGE_SIZE;
        }

        Edge() = default;
        Edge(int64_t id, int64_t sourceNodeId, int64_t targetNodeId, const std::string &label);

        // Property methods
        void addProperty(const std::string &key, const PropertyValue &value);
        [[nodiscard]] bool hasProperty(const std::string &key) const;
        [[nodiscard]] PropertyValue getProperty(const std::string &key) const;
        void removeProperty(const std::string &key);
        [[nodiscard]] const std::unordered_map<std::string, PropertyValue>& getProperties() const;
        [[nodiscard]] size_t getTotalPropertySize() const;
        void clearProperties() { properties.clear(); }

        // Property entity
        void setPropertyEntityId(int64_t propertyId, PropertyStorageType storageType);
        int64_t getPropertyEntityId() const { return metadata.propertyEntityId; }
        PropertyStorageType getPropertyStorageType() const {
            return static_cast<PropertyStorageType>(metadata.propertyStorageType);
        }
        bool hasPropertyEntity() const {
            return getPropertyStorageType() != PropertyStorageType::NONE && metadata.propertyEntityId != 0;
        }

        // Basic getters
        [[nodiscard]] std::string getLabel() const;
        [[nodiscard]] int64_t getId() const { return metadata.id; }
        [[nodiscard]] int64_t getFromNodeId() const { return metadata.sourceNodeId; }
        [[nodiscard]] int64_t getToNodeId() const { return metadata.targetNodeId; }
        [[nodiscard]] bool isActive() const { return metadata.isActive; }

        // Node relationship accessors
        [[nodiscard]] int64_t getSourceNodeId() const { return metadata.sourceNodeId; }
        [[nodiscard]] int64_t getTargetNodeId() const { return metadata.targetNodeId; }

        // ID management
        [[nodiscard]] bool hasTemporaryId() const;
        void setPermanentId(int64_t permanentId);

        // Setters
        void setId(int64_t id) { metadata.id = id; }
        void markInactive(bool active = false) { metadata.isActive = active; }
        void setSourceNodeId(int64_t newSourceId) { metadata.sourceNodeId = newSourceId; }
        void setTargetNodeId(int64_t newTargetId) { metadata.targetNodeId = newTargetId; }

        // Serialization
        void serialize(std::ostream& os) const;
        static Edge deserialize(std::istream& is);
        [[nodiscard]] size_t getSerializedSize() const;

        // Metadata access for advanced usage
        const Metadata& getMetadata() const { return metadata; }
        Metadata& getMutableMetadata() { return metadata; }

    private:
        // Fixed-size metadata structure
        Metadata metadata;

        // Fixed-size buffer for label
        char labelBuffer[LABEL_BUFFER_SIZE] = {0};

        // Variable-sized data (not included in the fixed-size structure)
        std::unordered_map<std::string, PropertyValue> properties;

        // Helper method to set label
        void setLabel(const std::string& label);
    };

} // namespace graph