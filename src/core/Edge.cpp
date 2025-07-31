/**
 * @file Edge.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Edge.hpp"
#include <cstring>
#include <stdexcept>
#include "graph/storage/IDAllocator.hpp"
#include "graph/utils/Serializer.hpp"

namespace graph {

	Edge::Edge(const int64_t id, const int64_t sourceId, const int64_t targetId, const std::string &label) {
		metadata.id = id;
		metadata.sourceNodeId = sourceId;
		metadata.targetNodeId = targetId;
		metadata.nextOutEdgeId = 0;
		metadata.prevOutEdgeId = 0;
		metadata.nextInEdgeId = 0;
		metadata.prevInEdgeId = 0;
		metadata.isActive = true;
		setLabel(label);
	}

	void Edge::setLabel(const std::string &label) {
		// Copy label to fixed-size buffer, ensuring null termination
		size_t copySize = std::min(label.size(), LABEL_BUFFER_SIZE - 1);
		memcpy(labelBuffer, label.data(), copySize);
		labelBuffer[copySize] = '\0';
	}

	// TODO: Using return value char* instead of std::string?
	std::string Edge::getLabel() const {
		// Return label as string
		return {labelBuffer};
	}

	void Edge::addProperty(const std::string &key, const PropertyValue &value) { properties[key] = value; }

	bool Edge::hasProperty(const std::string &key) const { return properties.contains(key); }

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
		utils::Serializer::writePOD(os, metadata.nextOutEdgeId);
		utils::Serializer::writePOD(os, metadata.prevOutEdgeId);
		utils::Serializer::writePOD(os, metadata.nextInEdgeId);
		utils::Serializer::writePOD(os, metadata.prevInEdgeId);
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
		edge.metadata.nextOutEdgeId = utils::Serializer::readPOD<int64_t>(is);
		edge.metadata.prevOutEdgeId = utils::Serializer::readPOD<int64_t>(is);
		edge.metadata.nextInEdgeId = utils::Serializer::readPOD<int64_t>(is);
		edge.metadata.prevInEdgeId = utils::Serializer::readPOD<int64_t>(is);
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
		size += sizeof(metadata.id); // int64_t
		size += sizeof(metadata.sourceNodeId); // int64_t
		size += sizeof(metadata.targetNodeId); // int64_t
		size += sizeof(metadata.nextOutEdgeId); // int64_t
		size += sizeof(metadata.prevOutEdgeId); // int64_t
		size += sizeof(metadata.nextInEdgeId); // int64_t
		size += sizeof(metadata.prevInEdgeId); // int64_t
		size += sizeof(metadata.propertyEntityId); // int64_t
		size += sizeof(metadata.propertyStorageType); // uint32_t
		size += sizeof(metadata.isActive); // bool

		// Calculate size of the serialized string (length prefix + string content)
		const std::string label = getLabel();
		size += sizeof(uint32_t); // For string length prefix
		size += label.size(); // Actual string content

		return size;
	}

} // namespace graph
