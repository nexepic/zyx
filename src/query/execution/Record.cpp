/**
 * @file Record.cpp
 * @author Nexepic
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/query/execution/Record.hpp"

namespace graph::query::execution {

	void Record::setNode(const std::string &variable, const Node &node) { nodes_[variable] = node; }

	std::optional<Node> Record::getNode(const std::string &variable) const {
		auto it = nodes_.find(variable);
		if (it != nodes_.end()) {
			return it->second;
		}
		return std::nullopt;
	}

	void Record::setEdge(const std::string &variable, const Edge &edge) { edges_[variable] = edge; }

	std::optional<Edge> Record::getEdge(const std::string &variable) const {
		auto it = edges_.find(variable);
		if (it != edges_.end()) {
			return it->second;
		}
		return std::nullopt;
	}

	void Record::setValue(const std::string &key, const PropertyValue &value) { values_[key] = value; }

	std::optional<PropertyValue> Record::getValue(const std::string &key) const {
		auto it = values_.find(key);
		if (it != values_.end()) {
			return it->second;
		}
		return std::nullopt;
	}

	void Record::merge(const Record &other) {
		// Merge Nodes
		for (const auto &[k, v]: other.nodes_) {
			nodes_[k] = v;
		}
		// Merge Edges
		for (const auto &[k, v]: other.edges_) {
			edges_[k] = v;
		}
		// Merge Values
		for (const auto &[k, v]: other.values_) {
			values_[k] = v;
		}
	}

} // namespace graph::query::execution
