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
#include <cstdint>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/PropertyTypes.hpp"

namespace graph::query {

	// A logical result entity carries resolved schema names and Properties. It is
	// deliberately distinct from legacy Node so a relocatable read never invents token IDs.
	struct LogicalNodeResult {
		uint64_t node_id = 0U;
		std::vector<std::string> labels;
		std::unordered_map<std::string, PropertyValue> properties;

		bool operator==(const LogicalNodeResult &) const = default;
	};

	// A logical relationship result retains the canonical endpoint IDs and
	// relationship type without depending on a physical token registry.
	struct LogicalEdgeResult {
		uint64_t edge_id = 0U;
		uint64_t source_node_id = 0U;
		uint64_t target_node_id = 0U;
		std::string type;
		std::unordered_map<std::string, PropertyValue> properties;

		bool operator==(const LogicalEdgeResult &) const = default;
	};

	/**
	 * @brief Format-independent read-only view of a Node returned by Cypher.
	 *
	 * Query results must describe graph semantics rather than expose the active
	 * on-disk representation. The view keeps legacy physical execution and the
	 * logical storage backend behind one stable entity contract.
	 */
	class NodeResultView {
	public:
		explicit NodeResultView(const Node &node) : physical_(&node) {}
		explicit NodeResultView(const LogicalNodeResult &node) : logical_(&node) {}

		[[nodiscard]] uint64_t getId() const {
			if (logical_ != nullptr) {
				return logical_->node_id;
			}
			const int64_t id = physical_->getId();
			if (id < 0) {
				throw std::domain_error("physical query-result Node has a negative ID");
			}
			return static_cast<uint64_t>(id);
		}

		[[nodiscard]] size_t getLabelCount() const {
			return logical_ != nullptr ? logical_->labels.size() : physical_->getLabelCount();
		}

		[[nodiscard]] const std::vector<std::string> &getLabels() const {
			if (logical_ == nullptr) {
				throw std::logic_error("schema names are unavailable for a physical query-result Node");
			}
			return logical_->labels;
		}

		[[nodiscard]] const std::unordered_map<std::string, PropertyValue> &getProperties() const {
			return logical_ != nullptr ? logical_->properties : physical_->getProperties();
		}

		[[nodiscard]] bool hasResolvedLabels() const noexcept { return logical_ != nullptr; }

	private:
		const Node *physical_ = nullptr;
		const LogicalNodeResult *logical_ = nullptr;
	};

	/** @brief Format-independent read-only view of an Edge returned by Cypher. */
	class EdgeResultView {
	public:
		explicit EdgeResultView(const Edge &edge) : physical_(&edge) {}
		explicit EdgeResultView(const LogicalEdgeResult &edge) : logical_(&edge) {}

		[[nodiscard]] uint64_t getId() const {
			return logical_ != nullptr ? logical_->edge_id : checkedPhysicalId(physical_->getId(), "Edge");
		}

		[[nodiscard]] uint64_t getSourceNodeId() const {
			return logical_ != nullptr
					   ? logical_->source_node_id
					   : checkedPhysicalId(physical_->getSourceNodeId(), "Edge source Node");
		}

		[[nodiscard]] uint64_t getTargetNodeId() const {
			return logical_ != nullptr
					   ? logical_->target_node_id
					   : checkedPhysicalId(physical_->getTargetNodeId(), "Edge target Node");
		}

		[[nodiscard]] const std::string &getType() const {
			if (logical_ == nullptr) {
				throw std::logic_error("schema name is unavailable for a physical query-result Edge");
			}
			return logical_->type;
		}

		[[nodiscard]] const std::unordered_map<std::string, PropertyValue> &getProperties() const {
			return logical_ != nullptr ? logical_->properties : physical_->getProperties();
		}

		[[nodiscard]] bool hasResolvedType() const noexcept { return logical_ != nullptr; }

	private:
		static uint64_t checkedPhysicalId(const int64_t id, const char *entity) {
			if (id < 0) {
				throw std::domain_error(std::string("physical query-result ") + entity + " has a negative ID");
			}
			return static_cast<uint64_t>(id);
		}

		const Edge *physical_ = nullptr;
		const LogicalEdgeResult *logical_ = nullptr;
	};

	/**
	 * @brief A superset of PropertyValue that can also hold Graph Entities.
	 */
	class ResultValue {
	public:
		using VariantType = std::variant<PropertyValue, Node, Edge, LogicalNodeResult, LogicalEdgeResult>;
		using TokenResolver = std::function<std::string(int64_t)>;

		// Constructors
		ResultValue() : data_(PropertyValue()) {}
		ResultValue(PropertyValue val) : data_(std::move(val)) {}
		ResultValue(Node node) : data_(std::move(node)) {}
		ResultValue(Edge edge) : data_(std::move(edge)) {}
		ResultValue(LogicalNodeResult node) : data_(std::move(node)) {}
		ResultValue(LogicalEdgeResult edge) : data_(std::move(edge)) {}

		const VariantType &getVariant() const { return data_; }

		// Type checkers
		bool isNode() const { return std::holds_alternative<Node>(data_) || isLogicalNode(); }
		bool isEdge() const { return std::holds_alternative<Edge>(data_) || isLogicalEdge(); }
		bool isLogicalNode() const { return std::holds_alternative<LogicalNodeResult>(data_); }
		bool isLogicalEdge() const { return std::holds_alternative<LogicalEdgeResult>(data_); }
		bool isGraphNode() const { return isNode(); }
		bool isGraphEdge() const { return isEdge(); }
		bool isPrimitive() const { return std::holds_alternative<PropertyValue>(data_); }

		// Unwrappers
		NodeResultView asNode() const {
			if (const auto *node = std::get_if<Node>(&data_)) {
				return NodeResultView(*node);
			}
			return NodeResultView(std::get<LogicalNodeResult>(data_));
		}
		EdgeResultView asEdge() const {
			if (const auto *edge = std::get_if<Edge>(&data_)) {
				return EdgeResultView(*edge);
			}
			return EdgeResultView(std::get<LogicalEdgeResult>(data_));
		}
		const LogicalNodeResult &asLogicalNode() const { return std::get<LogicalNodeResult>(data_); }
		const LogicalEdgeResult &asLogicalEdge() const { return std::get<LogicalEdgeResult>(data_); }
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
						} else if constexpr (std::is_same_v<T, LogicalNodeResult>) {
							return formatLogicalNode(arg);
						} else if constexpr (std::is_same_v<T, LogicalEdgeResult>) {
							return formatLogicalEdge(arg);
						}
						return ""; // ZYX_COV_EXCL_LINE std::variant alternatives are exhaustive.
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
			for (int64_t lid: labelIds) {
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

		static std::string formatLogicalNode(const LogicalNodeResult &node) {
			std::ostringstream oss;
			oss << "(";
			for (const std::string &label: node.labels) {
				oss << ":" << label;
			}
			const std::string props = formatProps(node.properties);
			if (!props.empty()) {
				oss << " " << props;
			}
			oss << ")";
			return oss.str();
		}

		static std::string formatLogicalEdge(const LogicalEdgeResult &edge) {
			std::ostringstream oss;
			oss << "[";
			if (!edge.type.empty()) {
				oss << ":" << edge.type;
			}
			const std::string props = formatProps(edge.properties);
			if (!props.empty()) {
				oss << " " << props;
			}
			oss << "]";
			return oss.str();
		}

		static std::string formatProps(const std::unordered_map<std::string, PropertyValue> &props) {
			if (props.empty())
				return "";

			// Sort keys for stable output
			std::vector<std::string> keys;
			keys.reserve(props.size());
			for (const auto &[k, v]: props) {
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
