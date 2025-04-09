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
#include <stdexcept>
#include "graph/utils/Serializer.h"

namespace graph {

Edge::Edge(uint64_t id, uint64_t from, uint64_t to, std::string label) :
	id(id), fromNodeId(from), toNodeId(to), label(std::move(label)) {}

uint64_t Edge::getId() const { return id; }

uint64_t Edge::getFromNodeId() const { return fromNodeId; }

uint64_t Edge::getToNodeId() const { return toNodeId; }

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
	utils::Serializer::writePOD(os, fromNodeId);
	utils::Serializer::writePOD(os, toNodeId);
	utils::Serializer::writeString(os, label);

	// Write property reference instead of properties themselves
	utils::Serializer::writePOD(os, static_cast<uint8_t>(propertyRef.type));
	utils::Serializer::writePOD(os, propertyRef.reference);
	utils::Serializer::writePOD(os, propertyRef.size);

	// Write activation flag
	utils::Serializer::writePOD(os, isActive_);
}

Edge Edge::deserialize(std::istream &is) {
	auto id = utils::Serializer::readPOD<uint64_t>(is);
	auto from = utils::Serializer::readPOD<uint64_t>(is);
	auto to = utils::Serializer::readPOD<uint64_t>(is);
	std::string label = utils::Serializer::readString(is);

	Edge edge(id, from, to, label);

	PropertyReference propRef;
	propRef.type = static_cast<PropertyReference::StorageType>(
		utils::Serializer::readPOD<uint8_t>(is));
	propRef.reference = utils::Serializer::readPOD<uint64_t>(is);
	propRef.size = utils::Serializer::readPOD<uint32_t>(is);

	edge.setPropertyReference(propRef);

	// Read activation flag
	edge.isActive_ = utils::Serializer::readPOD<bool>(is);

	return edge;
}

void Edge::setPropertyReference(const PropertyReference& ref) {
	propertyRef = ref;
	// Clear cached properties as the reference has changed
	// properties.clear();
}

const PropertyReference& Edge::getPropertyReference() const {
	return propertyRef;
}

} // namespace graph