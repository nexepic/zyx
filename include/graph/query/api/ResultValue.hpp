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

#include <algorithm>
#include <functional>
#include <variant>
#include <vector>
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
		using TokenResolver = std::function<std::string(int64_t)>;

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
		 * @brief Converts the underlying value to a Cypher-like string.
		 *
		 * @param resolver Optional function to resolve token IDs to names.
		 *   When provided, nodes display as (:Person {...}) and edges as [:KNOWS {...}].
		 *   Without resolver, falls back to numeric IDs: (:?1 {...}), [:?2 {...}].
		 */
		std::string toString(const TokenResolver &resolver = nullptr) const {
			return std::visit(
					[&resolver]<typename T0>(T0 &&arg) -> std::string {
						using T = std::decay_t<T0>;

						if constexpr (std::is_same_v<T, PropertyValue>) {
							return arg.toString();
						} else if constexpr (std::is_same_v<T, Node>) {
							return formatNode(arg, resolver);
						} else if constexpr (std::is_same_v<T, Edge>) {
							return formatEdge(arg, resolver);
						}
						return "";
					},
					data_);
		}

	private:
		VariantType data_;

		static std::string resolveToken(int64_t id, const TokenResolver &resolver) {
			if (resolver) {
				return resolver(id);
			}
			return "?" + std::to_string(id);
		}

		static std::string formatNode(const Node &node, const TokenResolver &resolver) {
			std::ostringstream oss;
			oss << "(";

			auto labelIds = node.getLabelIds();
			for (int64_t lid : labelIds) {
				if (lid != 0) {
					oss << ":" << resolveToken(lid, resolver);
				}
			}

			std::string props = formatProps(node.getProperties());
			if (!props.empty())
				oss << " " << props;

			oss << ")";
			return oss.str();
		}

		static std::string formatEdge(const Edge &edge, const TokenResolver &resolver) {
			std::ostringstream oss;
			oss << "[";

			int64_t tid = edge.getTypeId();
			if (tid != 0) {
				oss << ":" << resolveToken(tid, resolver);
			}

			std::string props = formatProps(edge.getProperties());
			if (!props.empty())
				oss << " " << props;

			oss << "]";
			return oss.str();
		}

		static std::string formatProps(const std::unordered_map<std::string, PropertyValue> &props) {
			if (props.empty())
				return "";

			// Sort keys for stable output
			std::vector<std::string> keys;
			keys.reserve(props.size());
			for (const auto &[k, v] : props) {
				keys.push_back(k);
			}
			std::sort(keys.begin(), keys.end());

			std::ostringstream oss;
			oss << "{";
			for (size_t i = 0; i < keys.size(); ++i) {
				if (i > 0)
					oss << ", ";
				oss << keys[i] << ": " << props.at(keys[i]).toString();
			}
			oss << "}";
			return oss.str();
		}
	};

} // namespace graph::query
