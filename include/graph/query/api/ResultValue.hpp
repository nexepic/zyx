/**
 * @file ResultValue.hpp
 * @author Nexepic
 * @date 2026/1/5
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include <variant>
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/PropertyTypes.hpp"

namespace graph::query {

	/**
	 * @brief A superset of PropertyValue that can also hold Graph Entities.
	 */
	class ResultValue {
	public:
		using VariantType = std::variant<PropertyValue, Node, Edge>;

		// Constructors
		ResultValue() : data_(PropertyValue()) {}
		ResultValue(PropertyValue val) : data_(std::move(val)) {}
		ResultValue(Node node) : data_(std::move(node)) {}
		ResultValue(Edge edge) : data_(std::move(edge)) {}

		const VariantType &getVariant() const { return data_; }

		// Type checkers
		bool isNode() const { return std::holds_alternative<Node>(data_); }
		bool isEdge() const { return std::holds_alternative<Edge>(data_); }
		bool isPrimitive() const { return std::holds_alternative<PropertyValue>(data_); }

		// Unwrappers
		const Node &asNode() const { return std::get<Node>(data_); }
		const Edge &asEdge() const { return std::get<Edge>(data_); }
		const PropertyValue &asPrimitive() const { return std::get<PropertyValue>(data_); }

		/**
		 * @brief Converts the underlying value to a string representation.
		 *
		 * - Primitives: Returns the value as string.
		 * - Nodes: Returns Cypher-like format `(:Label {props})` or `(Node:ID)`.
		 * - Edges: Returns Cypher-like format `[:TYPE {props}]`.
		 */
		std::string toString() const {
			return std::visit(
					[/*this*/]<typename T0>(T0 &&arg) -> std::string {
						using T = std::decay_t<T0>;

						if constexpr (std::is_same_v<T, PropertyValue>) {
							return arg.toString();
						} else if constexpr (std::is_same_v<T, Node>) {
							std::ostringstream oss;
							// Format: (Label:ID {prop1:val, ...})
							oss << "(";
							if (!arg.getLabel().empty())
								oss << ":" << arg.getLabel();
							else
								oss << "Node";

							oss << ":" << arg.getId();

							std::string props = formatProps(arg.getProperties());
							if (!props.empty())
								oss << " " << props;

							oss << ")";
							return oss.str();
						} else if constexpr (std::is_same_v<T, Edge>) {
							std::ostringstream oss;
							// Format: [:TYPE:ID {props}]
							oss << "[:" << arg.getLabel() << ":" << arg.getId();

							std::string props = formatProps(arg.getProperties());
							if (!props.empty())
								oss << " " << props;

							oss << "]";
							return oss.str();
						}
						return "";
					},
					data_);
		}

	private:
		VariantType data_;

		// Helper to format properties map to JSON-like string
		static std::string formatProps(const std::unordered_map<std::string, PropertyValue> &props) {
			if (props.empty())
				return "";
			std::ostringstream oss;
			oss << "{";
			auto it = props.begin();
			while (it != props.end()) {
				oss << it->first << ": " << it->second.toString();
				if (++it != props.end())
					oss << ", ";
			}
			oss << "}";
			return oss.str();
		}
	};

} // namespace graph::query
