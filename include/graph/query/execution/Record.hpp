/**
 * @file Record.hpp
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

#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"

namespace graph::query::execution {

	/**
	 * @class Record
	 * @brief Represents a single row in the intermediate result set.
	 *
	 * A record holds the mapping between query variables (e.g., "n")
	 * and the actual graph entities or calculated values.
	 */
	class Record {
	public:
		// Stores node variables (e.g., MATCH (n))
		void setNode(const std::string &variable, const Node &node);
		[[nodiscard]] std::optional<Node> getNode(const std::string &variable) const;

		// Stores edge variables (e.g., MATCH (n)-[e]->(m))
		void setEdge(const std::string &variable, const Edge &edge);
		[[nodiscard]] std::optional<Edge> getEdge(const std::string &variable) const;

		// Stores projected or calculated values
		void setValue(const std::string &key, const PropertyValue &value);
		[[nodiscard]] std::optional<PropertyValue> getValue(const std::string &key) const;

		void merge(const Record &other);

	private:
		std::unordered_map<std::string, Node> nodes_;
		std::unordered_map<std::string, Edge> edges_;
		std::unordered_map<std::string, PropertyValue> values_;
	};

	/**
	 * @typedef RecordBatch
	 * @brief A vector of records processed together for cache efficiency (Vectorized Execution).
	 */
	using RecordBatch = std::vector<Record>;

} // namespace graph::query::execution
