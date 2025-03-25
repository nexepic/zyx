/**
 * @file Edge
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

Edge::Edge(uint64_t id, uint64_t from, uint64_t to, const std::string& label)
    : id(id), fromNodeId(from), toNodeId(to), label(label) {}

uint64_t Edge::getId() const {
    return id;
}

uint64_t Edge::getFromNodeId() const {
    return fromNodeId;
}

uint64_t Edge::getToNodeId() const {
    return toNodeId;
}

const std::string& Edge::getLabel() const {
    return label;
}

void Edge::addProperty(const std::string& key, const PropertyValue& value) {
    properties.emplace(key, value);
}

const PropertyValue& Edge::getProperty(const std::string& key) const {
    auto it = properties.find(key);
    if (it == properties.end()) {
        throw std::out_of_range("Property '" + key + "' not found");
    }
    return it->second;
}

void Edge::serialize(std::ostream& os) const {
    utils::Serializer::writePOD(os, id);
    utils::Serializer::writePOD(os, fromNodeId);
    utils::Serializer::writePOD(os, toNodeId);
    utils::Serializer::writeString(os, label);

    utils::Serializer::writePOD(os, static_cast<uint32_t>(properties.size()));
    for (const auto& [key, value] : properties) {
        utils::Serializer::writeString(os, key);
        std::visit([&os](const auto& v) {
            utils::Serializer::serialize(os, v);
        }, value);
    }
}

Edge Edge::deserialize(std::istream& is) {
    uint64_t id = utils::Serializer::readPOD<uint64_t>(is);
    uint64_t from = utils::Serializer::readPOD<uint64_t>(is);
    uint64_t to = utils::Serializer::readPOD<uint64_t>(is);
    std::string label = utils::Serializer::readString(is);

    Edge edge(id, from, to, label);

    uint32_t propCount = utils::Serializer::readPOD<uint32_t>(is);
    for (uint32_t i = 0; i < propCount; ++i) {
        std::string key = utils::Serializer::readString(is);
        PropertyValue value = utils::Serializer::deserializeVariant(is);
        edge.addProperty(key, std::move(value));
    }

    return edge;
}

} // namespace graph