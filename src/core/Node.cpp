/**
 * @file Node
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/core/Node.h"
#include <stdexcept>
#include "graph/utils/Serializer.h"
#include "graph/utils/VectorUtils.h"

namespace graph {

	Node::Node(uint64_t id, const std::string &label) : id(id), label(label) {}

	uint64_t Node::getId() const { return id; }

	const std::string &Node::getLabel() const { return label; }

	void Node::addProperties(const std::string &key, const PropertyValue &value) { properties.emplace(key, value); }

	const std::unordered_map<std::string, PropertyValue>& Node::getProperties() const {
	    return properties;
	}

	void Node::addOutEdge(uint64_t edgeId) {
		outEdges.push_back(edgeId);
	}

	void Node::addInEdge(uint64_t edgeId) {
		inEdges.push_back(edgeId);
	}

	const std::vector<uint64_t>& Node::getOutEdges() const {
		return outEdges;
	}

	const std::vector<uint64_t>& Node::getInEdges() const {
		return inEdges;
	}

	void Node::serialize(std::ostream &os) const {
		// Write metadata
		utils::Serializer::writePOD(os, id);
		utils::Serializer::writeString(os, label);

		// Write properties
		utils::Serializer::writePOD(os, static_cast<uint32_t>(properties.size()));
		for (const auto &[key, value]: properties) {
			utils::Serializer::writeString(os, key);
			std::visit([&os](const auto &v) { utils::Serializer::serialize(os, v); }, value);
		}

		utils::Serializer::writePOD(os, static_cast<uint32_t>(inEdges.size()));
		for (const auto &edgeId: inEdges) {
			utils::Serializer::writePOD(os, edgeId);
		}

		utils::Serializer::writePOD(os, static_cast<uint32_t>(outEdges.size()));
		for (const auto &edgeId: outEdges) {
			utils::Serializer::writePOD(os, edgeId);
		}
	}

	Node Node::deserialize(std::istream &is) {
		uint64_t id = utils::Serializer::readPOD<uint64_t>(is);
		std::string label = utils::Serializer::readString(is);

		Node node(id, label);

		uint32_t propCount = utils::Serializer::readPOD<uint32_t>(is);
		for (uint32_t i = 0; i < propCount; ++i) {
			std::string key = utils::Serializer::readString(is);
			PropertyValue value = utils::Serializer::deserializeVariant(is);
			node.addProperties(key, std::move(value));
		}

		uint32_t inEdgeCount = utils::Serializer::readPOD<uint32_t>(is);
		for (uint32_t i = 0; i < inEdgeCount; ++i) {
			uint64_t edgeId = utils::Serializer::readPOD<uint64_t>(is);
			node.addInEdge(edgeId);
		}

		uint32_t outEdgeCount = utils::Serializer::readPOD<uint32_t>(is);
		for (uint32_t i = 0; i < outEdgeCount; ++i) {
			uint64_t edgeId = utils::Serializer::readPOD<uint64_t>(is);
			node.addOutEdge(edgeId);
		}

		return node;
	}

} // namespace graph