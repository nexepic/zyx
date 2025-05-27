/**
 * @file Property.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/25
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include "PropertyValue.h"

namespace graph {

    class Property {
    public:
        // Metadata struct to contain fixed property data
        struct Metadata {
            int64_t id = 0;
            int64_t entityId = 0;  // ID of the entity this property belongs to
            uint32_t entityType = 0; // 0 = Node, 1 = Edge
            bool isActive = true;
        };

        static constexpr uint32_t typeId = 2; // Node = 0, Edge = 1, Property = 2
        static constexpr size_t TOTAL_PROPERTY_SIZE = 256;
        static constexpr size_t METADATA_SIZE = sizeof(Metadata);

        static constexpr size_t getTotalSize() {
            return TOTAL_PROPERTY_SIZE;
        }

        Property(int64_t id, int64_t entityId, uint32_t entityType);
        Property() = default;

        // Basic getters
        int64_t getId() const { return metadata.id; }
        int64_t getEntityId() const { return metadata.entityId; }
        uint32_t getEntityType() const { return metadata.entityType; }
        bool isActive() const { return metadata.isActive; }

        // Basic setters
        void setId(int64_t newId) { metadata.id = newId; }
        void setEntityId(int64_t newEntityId) { metadata.entityId = newEntityId; }
        void setEntityType(uint32_t newEntityType) { metadata.entityType = newEntityType; }
        void markInactive() { metadata.isActive = false; }

        // ID management
        bool hasTemporaryId() const;
        void setPermanentId(int64_t permanentId);

        // Property value management
        void addPropertyValue(const std::string& key, const PropertyValue& value);
        bool hasPropertyValue(const std::string& key) const;
        PropertyValue getPropertyValue(const std::string& key) const;
        void removePropertyValue(const std::string& key);
        const std::unordered_map<std::string, PropertyValue>& getPropertyValues() const;

        // Entity information
        void setEntityInfo(int64_t newEntityId, uint32_t newEntityType) {
            metadata.entityId = newEntityId;
            metadata.entityType = newEntityType;
        }

        // Property map management
        void setProperties(const std::unordered_map<std::string, PropertyValue>& newValues) {
            values = newValues;
        }

        // Serialization methods
        void serialize(std::ostream& os) const;
        static Property deserialize(std::istream& is);
        [[nodiscard]] size_t getSerializedSize() const;

        // Metadata access for advanced usage
        const Metadata& getMetadata() const { return metadata; }
        Metadata& getMutableMetadata() { return metadata; }

    private:
        // Fixed-size metadata structure
        Metadata metadata;

        // Variable-sized data (not included in the fixed-size structure)
        std::unordered_map<std::string, PropertyValue> values;
    };

} // namespace graph