/**
 * @file Node.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Node.h"
#include <graph/storage/IDAllocator.h>
#include <stdexcept>
#include "graph/core/PropertyValue.h"
#include "graph/utils/Serializer.h"

namespace graph {

	Node::Node(int64_t id, std::string label = "") : id(id), label(std::move(label)) {}

	int64_t Node::getId() const { return id; }

	bool Node::hasTemporaryId() const { return storage::IDAllocator::isTemporaryId(id); }

	void Node::setPermanentId(int64_t permanentId) {
		if (!hasTemporaryId()) {
			throw std::runtime_error("Cannot set permanent ID for node that already has one");
		}
		id = permanentId;
	}

	const std::string &Node::getLabel() const { return label; }

	void Node::addProperty(const std::string &key, const PropertyValue &value) {
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

	bool Node::hasProperty(const std::string &key) const { return properties.count(key) > 0; }

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

	const std::unordered_map<std::string, PropertyValue> &Node::getProperties() const { return properties; }

	void Node::setPropertyEntityId(int64_t propertyId, PropertyStorageType storageType) {
		this->propertyEntityId = propertyId;
		this->propertyStorageType = storageType;
	}

	int64_t Node::getPropertyEntityId() const {
		return propertyEntityId;
	}

	PropertyStorageType Node::getPropertyStorageType() const {
		return propertyStorageType;
	}

	bool Node::hasPropertyEntity() const {
		return propertyStorageType != PropertyStorageType::NONE && propertyEntityId != 0;
	}

	size_t Node::getTotalPropertySize() const {
		size_t totalSize = 0;
		for (const auto &[key, value]: properties) {
			totalSize += key.size();
			totalSize += property_utils::getPropertyValueSize(value);
		}
		return totalSize;
	}

	void Node::addOutEdge(uint64_t edgeId) { outEdges.push_back(edgeId); }

	void Node::addInEdge(uint64_t edgeId) { inEdges.push_back(edgeId); }

	const std::vector<uint64_t> &Node::getOutEdges() const { return outEdges; }

	const std::vector<uint64_t> &Node::getInEdges() const { return inEdges; }

	void Node::serialize(std::ostream &os) const {
		// Write fixed-size node data
		utils::Serializer::writePOD(os, id);
		utils::Serializer::writeString(os, label);

		// // Write edge counts and IDs
		// utils::Serializer::writePOD(os, static_cast<uint32_t>(inEdges.size()));
		// for (const auto &edgeId: inEdges) {
		// 	utils::Serializer::writePOD(os, edgeId);
		// }
		//
		// utils::Serializer::writePOD(os, static_cast<uint32_t>(outEdges.size()));
		// for (const auto &edgeId: outEdges) {
		// 	utils::Serializer::writePOD(os, edgeId);
		// }

		// Write property storage type
		utils::Serializer::writePOD(os, static_cast<uint8_t>(propertyStorageType));

		// Write property entity ID
		utils::Serializer::writePOD(os, propertyEntityId);

		// Write activation flag
		utils::Serializer::writePOD(os, isActive_);
	}

	Node Node::deserialize(std::istream &is) {
		auto id = utils::Serializer::readPOD<uint64_t>(is);
		std::string label = utils::Serializer::readString(is);

		Node node(id, label);

		// // Read edge counts and IDs
		// auto inEdgeCount = utils::Serializer::readPOD<uint32_t>(is);
		// for (uint32_t i = 0; i < inEdgeCount; ++i) {
		// 	auto edgeId = utils::Serializer::readPOD<uint64_t>(is);
		// 	node.addInEdge(edgeId);
		// }
		//
		// auto outEdgeCount = utils::Serializer::readPOD<uint32_t>(is);
		// for (uint32_t i = 0; i < outEdgeCount; ++i) {
		// 	auto edgeId = utils::Serializer::readPOD<uint64_t>(is);
		// 	node.addOutEdge(edgeId);
		// }

		// Read property storage type
		node.propertyStorageType = static_cast<PropertyStorageType>(utils::Serializer::readPOD<uint8_t>(is));

		// Read property entity ID
		node.propertyEntityId = utils::Serializer::readPOD<int64_t>(is);

		// Read activation flag
		node.isActive_ = utils::Serializer::readPOD<bool>(is);

		return node;
	}

} // namespace graph
