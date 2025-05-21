/**
 * @file Property.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/25
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Property.h"
#include <graph/storage/IDAllocator.h>
#include "graph/utils/Serializer.h"

namespace graph {

	Property::Property(int64_t id, int64_t entityId, uint32_t entityType) :
		id(id), entityId(entityId), entityType(entityType) {}

	int64_t Property::getId() const { return id; }

	int64_t Property::getEntityId() const { return entityId; }

	uint32_t Property::getEntityType() const { return entityType; }

	bool Property::hasTemporaryId() const { return storage::IDAllocator::isTemporaryId(id); }

	void Property::setPermanentId(int64_t permanentId) {
		if (!hasTemporaryId()) {
			throw std::runtime_error("Cannot set permanent ID for property that already has one");
		}
		id = permanentId;
	}

	void Property::addPropertyValue(const std::string &key, const PropertyValue &value) { values[key] = value; }

	bool Property::hasPropertyValue(const std::string &key) const { return values.count(key) > 0; }

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

	void Property::markInactive() { isActive_ = false; }

	bool Property::isActive() const { return isActive_; }

	void Property::serialize(std::ostream &os) const {
		// Write fixed-size property data
		utils::Serializer::writePOD(os, id);
		utils::Serializer::writePOD(os, entityId);
		utils::Serializer::writePOD(os, entityType);
		utils::Serializer::writePOD(os, isActive_);

		// Write property values
		utils::Serializer::writePOD(os, static_cast<uint32_t>(values.size()));
		for (const auto &[key, value]: values) {
			utils::Serializer::writeString(os, key);
			utils::Serializer::writePropertyValue(os, value);
		}
	}

	Property Property::deserialize(std::istream &is) {
		auto id = utils::Serializer::readPOD<int64_t>(is);
		auto entityId = utils::Serializer::readPOD<int64_t>(is);
		auto entityType = utils::Serializer::readPOD<uint8_t>(is);

		Property property(id, entityId, entityType);

		property.isActive_ = utils::Serializer::readPOD<bool>(is);

		auto propertyCount = utils::Serializer::readPOD<uint32_t>(is);
		for (uint32_t i = 0; i < propertyCount; ++i) {
			std::string key = utils::Serializer::readString(is);
			PropertyValue value = utils::Serializer::readPropertyValue(is);
			property.values[key] = value;
		}

		return property;
	}

	size_t Property::getTotalSize() const {
		size_t totalSize = 0;
		for (const auto &[key, value]: values) {
			totalSize += key.size();
			totalSize += property_utils::getPropertyValueSize(value);
		}
		return totalSize;
	}

} // namespace graph
