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

	Edge::Edge(int64_t id, int64_t sourceNodeId, int64_t targetNodeId, std::string label) :
		id(id), sourceNodeId(sourceNodeId), targetNodeId(targetNodeId), label(std::move(label)) {}

	int64_t Edge::getId() const { return id; }

	bool Edge::hasTemporaryId() const { return storage::IDAllocator::isTemporaryId(id); }

	void Edge::setPermanentId(int64_t permanentId) {
		if (!hasTemporaryId()) {
			throw std::runtime_error("Cannot set permanent ID for node that already has one");
		}
		id = permanentId;
	}

	int64_t Edge::getFromNodeId() const { return sourceNodeId; }

	int64_t Edge::getToNodeId() const { return targetNodeId; }

	const std::string &Edge::getLabel() const { return label; }

	void Edge::addProperty(const std::string &key, const PropertyValue &value) {
		size_t valueSize = property_utils::getPropertyValueSize(value);
		size_t keySize = key.size();
		size_t newTotalSize = getTotalPropertySize();

		if (properties.count(key) > 0) {
			newTotalSize -= property_utils::getPropertyValueSize(properties[key]);
			newTotalSize -= key.size();
		}

		newTotalSize += keySize + valueSize;

		if (newTotalSize > MAX_TOTAL_PROPERTY_SIZE) {
			throw std::runtime_error("Property size limit exceeded for node " + std::to_string(id));
		}

		properties[key] = value;
	}

	bool Edge::hasProperty(const std::string &key) const { return properties.count(key) > 0; }

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

	const std::unordered_map<std::string, PropertyValue> &Edge::getProperties() const { return properties; }

	void Edge::setPropertyEntityId(int64_t propertyId, PropertyStorageType storageType) {
		this->propertyEntityId = propertyId;
		this->propertyStorageType = storageType;
	}

	int64_t Edge::getPropertyEntityId() const {
		return propertyEntityId;
	}

	PropertyStorageType Edge::getPropertyStorageType() const {
		return propertyStorageType;
	}

	bool Edge::hasPropertyEntity() const {
		return propertyStorageType != PropertyStorageType::NONE && propertyEntityId != 0;
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
		utils::Serializer::writePOD(os, id);
		utils::Serializer::writePOD(os, sourceNodeId);
		utils::Serializer::writePOD(os, targetNodeId);
		utils::Serializer::writeString(os, label);

		// write property storage type
		utils::Serializer::writePOD(os, static_cast<uint8_t>(propertyStorageType));

		// Write property entity ID
		utils::Serializer::writePOD(os, propertyEntityId);

		// Write activation flag
		utils::Serializer::writePOD(os, isActive_);
	}

	Edge Edge::deserialize(std::istream &is) {
		auto id = utils::Serializer::readPOD<uint64_t>(is);
		auto sourceNodeId = utils::Serializer::readPOD<uint64_t>(is);
		auto targetNodeId = utils::Serializer::readPOD<uint64_t>(is);
		std::string label = utils::Serializer::readString(is);

		Edge edge(id, sourceNodeId, targetNodeId, label);

		// Read property storage type
		edge.propertyStorageType = static_cast<PropertyStorageType>(utils::Serializer::readPOD<uint8_t>(is));

		// Read property entity ID
		edge.propertyEntityId = utils::Serializer::readPOD<int64_t>(is);

		// Read activation flag
		edge.isActive_ = utils::Serializer::readPOD<bool>(is);

		return edge;
	}

} // namespace graph
