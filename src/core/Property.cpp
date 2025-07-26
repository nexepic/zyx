/**
 * @file Property.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/25
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Property.hpp"
#include <graph/storage/IDAllocator.hpp>
#include "graph/utils/Serializer.hpp"

namespace graph {

	Property::Property(int64_t id, int64_t entityId, uint32_t entityType) {
		metadata.id = id;
		metadata.entityId = entityId;
		metadata.entityType = entityType;
		metadata.isActive = true;
	}

	void Property::addPropertyValue(const std::string &key, const PropertyValue &value) { values[key] = value; }

	bool Property::hasPropertyValue(const std::string &key) const { return values.contains(key); }

	PropertyValue Property::getPropertyValue(const std::string &key) const {
		auto it = values.find(key);
		if (it == values.end()) {
			throw std::out_of_range("Property value " + key + " not found");
		}
		return it->second;
	}

	void Property::removePropertyValue(const std::string &key) {
		auto it = values.find(key);
		if (it != values.end()) {
			values.erase(it);
		}
	}

	const std::unordered_map<std::string, PropertyValue> &Property::getPropertyValues() const { return values; }

	void Property::serialize(std::ostream &os) const {
		// Write metadata fields individually
		utils::Serializer::writePOD(os, metadata.id);
		utils::Serializer::writePOD(os, metadata.entityId);
		utils::Serializer::writePOD(os, metadata.entityType);
		utils::Serializer::writePOD(os, metadata.isActive);

		// Write property values
		utils::Serializer::writePOD(os, static_cast<uint32_t>(values.size()));
		for (const auto &[key, value]: values) {
			utils::Serializer::writeString(os, key);
			utils::Serializer::writePropertyValue(os, value);
		}
	}

	Property Property::deserialize(std::istream &is) {
		Property property;

		// Read metadata fields individually
		property.metadata.id = utils::Serializer::readPOD<int64_t>(is);
		property.metadata.entityId = utils::Serializer::readPOD<int64_t>(is);
		property.metadata.entityType = utils::Serializer::readPOD<uint32_t>(is);
		property.metadata.isActive = utils::Serializer::readPOD<bool>(is);

		// Read property values
		auto propertyCount = utils::Serializer::readPOD<uint32_t>(is);
		for (uint32_t i = 0; i < propertyCount; ++i) {
			std::string key = utils::Serializer::readString(is);
			PropertyValue value = utils::Serializer::readPropertyValue(is);
			property.values[key] = value;
		}

		return property;
	}

	size_t Property::getSerializedSize() const {
		// Calculate size of all metadata fields
		size_t size = 0;
		size += sizeof(metadata.id); // int64_t
		size += sizeof(metadata.entityId); // int64_t
		size += sizeof(metadata.entityType); // uint32_t
		size += sizeof(metadata.isActive); // bool

		// Size for property count
		size += sizeof(uint32_t); // For property count

		// Calculate size of all property values
		for (const auto &[key, value]: values) {
			// String key (length prefix + string content)
			size += sizeof(uint32_t); // String length prefix
			size += key.size(); // Key string content

			// Property value
			size += property_utils::getPropertyValueSize(value);
		}

		return size;
	}

} // namespace graph
