/**
 * @file Property.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/25
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string>
#include <unordered_map>
#include "PropertyValue.hpp"
#include "Types.hpp"
#include "graph/core/Entity.hpp"

namespace graph {

	class Property : public EntityBase<Property>, public EntityReferenceMixin<Property> {
	public:
		// Metadata struct to contain fixed property data
		struct Metadata {
			int64_t id = 0;
			int64_t entityId = 0; // ID of the entity this property belongs to
			uint32_t entityType = 0; // 0 = Node, 1 = Edge
			bool isActive = true;
		};

		static constexpr uint32_t typeId = toUnderlying(EntityType::Property);
		static constexpr size_t TOTAL_PROPERTY_SIZE = 256;
		static constexpr size_t METADATA_SIZE = offsetof(Metadata, isActive) + sizeof(Metadata::isActive);

		static constexpr size_t getTotalSize() { return TOTAL_PROPERTY_SIZE; }

		Property(int64_t id, int64_t entityId, uint32_t entityType);
		Property() = default;

		// Metadata access for CRTP base class
		[[nodiscard]] const Metadata &getMetadata() const { return metadata; }
		Metadata &getMutableMetadata() { return metadata; }

		// Basic getters
		[[nodiscard]] bool isActive() const { return metadata.isActive; }

		// Basic setters
		void markInactive() { metadata.isActive = false; }

		// Property value management
		[[nodiscard]] bool hasPropertyValue(const std::string &key) const;
		[[nodiscard]] const std::unordered_map<std::string, PropertyValue> &getPropertyValues() const;

		// Property map management
		void setProperties(const std::unordered_map<std::string, PropertyValue> &newValues) { values = newValues; }

		// Serialization methods
		void serialize(std::ostream &os) const;
		static Property deserialize(std::istream &is);
		[[nodiscard]] size_t getSerializedSize() const;

	private:
		// Fixed-size metadata structure
		Metadata metadata;

		// Variable-sized data (not included in the fixed-size structure)
		std::unordered_map<std::string, PropertyValue> values;
	};

} // namespace graph
