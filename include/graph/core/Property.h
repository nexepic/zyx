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

	class alignas(512) Property {
	public:
		static constexpr uint32_t typeId = 2; // Node = 0, Edge = 1, Property = 2

		Property(int64_t id, int64_t entityId, uint32_t entityType);
		Property() = default;

		int64_t getId() const;
		void setId(int64_t newId) { id = newId; }
		int64_t getEntityId() const;
		uint32_t getEntityType() const;
		bool hasTemporaryId() const;
		void setPermanentId(int64_t permanentId);

		void addPropertyValue(const std::string& key, const PropertyValue& value);
		bool hasPropertyValue(const std::string& key) const;
		PropertyValue getPropertyValue(const std::string& key) const;
		void removePropertyValue(const std::string& key);
		const std::unordered_map<std::string, PropertyValue>& getPropertyValues() const;

		void markInactive();
		bool isActive() const;

		// Serialization methods
		void serialize(std::ostream& os) const;
		static Property deserialize(std::istream& is);

		size_t getTotalSize() const;

		void setEntityId(int64_t newEntityId) {
			entityId = newEntityId;
		}

		void setEntityType(uint32_t newEntityType) {
			entityType = newEntityType;
		}

		// setEntityInfo
		void setEntityInfo(int64_t newEntityId, uint32_t newEntityType) {
			entityId = newEntityId;
			entityType = newEntityType;
		}

		// setProperties
		void setProperties(const std::unordered_map<std::string, PropertyValue>& newValues) {
			values = newValues;
		}

	private:
		int64_t id = 0;
		int64_t entityId = 0;  // ID of the entity this property belongs to
		uint32_t entityType = 0; // 0 = Node, 1 = Edge
		std::unordered_map<std::string, PropertyValue> values;
		bool isActive_ = true;
	};

} // namespace graph