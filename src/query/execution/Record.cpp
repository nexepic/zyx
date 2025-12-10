/**
 * @file Record.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/execution/Record.hpp"

namespace graph::query::execution {

	void Record::setNode(const std::string& variable, const Node& node) {
		nodes_[variable] = node;
	}

	std::optional<Node> Record::getNode(const std::string& variable) const {
		auto it = nodes_.find(variable);
		if (it != nodes_.end()) {
			return it->second;
		}
		return std::nullopt;
	}

	void Record::setEdge(const std::string& variable, const Edge& edge) {
		edges_[variable] = edge;
	}

	std::optional<Edge> Record::getEdge(const std::string& variable) const {
		auto it = edges_.find(variable);
		if (it != edges_.end()) {
			return it->second;
		}
		return std::nullopt;
	}

	void Record::setValue(const std::string& key, const PropertyValue& value) {
		values_[key] = value;
	}

	std::optional<PropertyValue> Record::getValue(const std::string& key) const {
		auto it = values_.find(key);
		if (it != values_.end()) {
			return it->second;
		}
		return std::nullopt;
	}

} // namespace graph::query::execution