/**
 * @file DatabaseImpl.cpp
 * @author Nexepic
 * @date 2025/12/16
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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "DatabaseBulkInternal.hpp"
#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/QueryPlan.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/LogicalGraphSchema.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"
#include "zyx/zyx.hpp"

namespace zyx {

	// ========================================================================
	// 1. Helpers: Type Conversion
	// ========================================================================

	namespace {
		size_t skipSpaces(const std::string_view s, size_t i) {
			while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
				++i;
			}
			return i;
		}

		std::string toUpperCopy(const std::string_view s) {
			std::string out;
			out.reserve(s.size());
			for (char ch: s) {
				out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
			}
			return out;
		}

		std::string readToken(const std::string_view s, size_t &i) {
			i = skipSpaces(s, i);
			size_t start = i;
			while (i < s.size()) {
				const unsigned char ch = static_cast<unsigned char>(s[i]);
				if (std::isalnum(ch) || ch == '_' || ch == '.') {
					++i;
				} else {
					break;
				}
			}
			return std::string(s.substr(start, i - start));
		}

		enum class CypherTxnKind { NONE, TXN_BEGIN, TXN_COMMIT, TXN_ROLLBACK };

		CypherTxnKind detectCypherTxnStatement(const std::string_view query) {
			size_t pos = 0;
			std::string first = readToken(query, pos);
			std::string upper = toUpperCopy(first);
			if (upper == "BEGIN") {
				return CypherTxnKind::TXN_BEGIN;
			}
			if (upper == "COMMIT") {
				return CypherTxnKind::TXN_COMMIT;
			}
			if (upper == "ROLLBACK") {
				return CypherTxnKind::TXN_ROLLBACK;
			}
			return CypherTxnKind::NONE;
		}
	} // namespace

	// Forward declaration
	Value toPublicValue(const graph::query::ResultValue &v);
	Value toPublicValue(const graph::PropertyValue &v);
	Value toDriverAbiValue(const graph::query::ResultValue &v);
	Value toDriverAbiValue(const graph::PropertyValue &v);
	Value toStringListValue(const std::vector<graph::PropertyValue> &values);

	int64_t checkedPublicEntityId(uint64_t id, const char *entityName) {
		if (id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			throw std::overflow_error(std::string("logical ") + entityName + " ID exceeds the public API range");
		}
		return static_cast<int64_t>(id);
	}

	std::shared_ptr<Node> toPublicLogicalNodePtr(const graph::query::LogicalNodeResult &internalNode) {
		auto pubNode = std::make_shared<Node>();
		pubNode->id = checkedPublicEntityId(internalNode.node_id, "Node");
		pubNode->labels = internalNode.labels;
		pubNode->label = pubNode->labels.empty() ? "" : pubNode->labels.front();
		for (const auto &[key, value]: internalNode.properties) {
			pubNode->properties.emplace(key, toPublicValue(value));
		}
		return pubNode;
	}

	std::shared_ptr<Edge> toPublicLogicalEdgePtr(const graph::query::LogicalEdgeResult &internalEdge) {
		auto pubEdge = std::make_shared<Edge>();
		pubEdge->id = checkedPublicEntityId(internalEdge.edge_id, "Edge");
		pubEdge->sourceId = checkedPublicEntityId(internalEdge.source_node_id, "Edge source Node");
		pubEdge->targetId = checkedPublicEntityId(internalEdge.target_node_id, "Edge target Node");
		pubEdge->type = internalEdge.type;
		for (const auto &[key, value]: internalEdge.properties) {
			pubEdge->properties.emplace(key, toPublicValue(value));
		}
		return pubEdge;
	}

	// Convert internal PropertyValue to public Value variant
	Value toStringListValue(const std::vector<graph::PropertyValue> &values) {
		std::vector<std::string> strings;
		strings.reserve(values.size());
		for (const auto &val: values) {
			strings.push_back(val.toString());
		}
		return strings;
	}

	Value toPublicValue(const graph::PropertyValue &v) {
		return std::visit(
				[]<typename T0>(T0 &&arg) -> Value {
					using T = std::decay_t<T0>;
					if constexpr (std::is_same_v<T, std::monostate>) {
						return std::monostate{};
					} else if constexpr (std::is_same_v<T, std::vector<graph::PropertyValue>>) {
						return toStringListValue(arg);
					} else if constexpr (std::is_same_v<T, graph::PropertyValue::MapType>) {
						graph::PropertyValue pv(arg);
						return pv.toString();
					} else if constexpr (std::is_same_v<T, graph::PropertyValue::Bytes>) {
						zyx::Bytes bytes;
						bytes.reserve(arg.size());
						for (const std::byte byte: arg) {
							bytes.push_back(std::to_integer<std::uint8_t>(byte));
						}
						return bytes;
					} else if constexpr (std::is_same_v<T, graph::TemporalDate> ||
										 std::is_same_v<T, graph::TemporalDateTime> ||
										 std::is_same_v<T, graph::TemporalDuration>) {
						return arg.toISO();
					} else {
						// Primitives (int, double, bool, string) map directly
						return arg;
					}
				},
				v.getVariant());
	}

	Value toDriverAbiValue(const graph::PropertyValue &v) {
		return std::visit(
				[]<typename T0>(T0 &&arg) -> Value {
					using T = std::decay_t<T0>;
					if constexpr (std::is_same_v<T, std::monostate>) {
						return std::monostate{};
					} else if constexpr (std::is_same_v<T, std::vector<graph::PropertyValue>>) {
						auto list = std::make_shared<ValueList>();
						list->elements.reserve(arg.size());
						for (const auto &val: arg) {
							list->elements.push_back(toDriverAbiValue(val));
						}
						return list;
					} else if constexpr (std::is_same_v<T, graph::PropertyValue::MapType>) {
						auto map = std::make_shared<ValueMap>();
						for (const auto &[key, val]: arg) {
							map->entries.emplace(key, toDriverAbiValue(val));
						}
						return map;
					} else if constexpr (std::is_same_v<T, graph::PropertyValue::Bytes>) {
						zyx::Bytes bytes;
						bytes.reserve(arg.size());
						for (const std::byte byte: arg) {
							bytes.push_back(std::to_integer<std::uint8_t>(byte));
						}
						return bytes;
					} else if constexpr (std::is_same_v<T, graph::TemporalDate> ||
										 std::is_same_v<T, graph::TemporalDateTime> ||
										 std::is_same_v<T, graph::TemporalDuration>) {
						return arg.toISO();
					} else {
						return arg;
					}
				},
				v.getVariant());
	}

	// Main conversion entry point for ResultValue
	Value toPublicValue(const graph::query::ResultValue &v) {
		return std::visit(
				[]<typename T0>(T0 &&arg) -> Value {
					using T = std::decay_t<T0>;

					if constexpr (std::is_same_v<T, graph::query::LogicalNodeResult>) {
						return toPublicLogicalNodePtr(arg);
					} else if constexpr (std::is_same_v<T, graph::query::LogicalEdgeResult>) {
						return toPublicLogicalEdgePtr(arg);
					} else if constexpr (std::is_same_v<T, graph::PropertyValue>) {
						return toPublicValue(arg);
					} else {
						return std::monostate{};
					}
				},
				v.getVariant());
	}

	Value toDriverAbiValue(const graph::query::ResultValue &v) {
		return std::visit(
				[]<typename T0>(T0 &&arg) -> Value {
					using T = std::decay_t<T0>;

					if constexpr (std::is_same_v<T, graph::query::LogicalNodeResult>) {
						return toPublicLogicalNodePtr(arg);
					} else if constexpr (std::is_same_v<T, graph::query::LogicalEdgeResult>) {
						return toPublicLogicalEdgePtr(arg);
					} else if constexpr (std::is_same_v<T, graph::PropertyValue>) {
						return toDriverAbiValue(arg);
					} else {
						return std::monostate{};
					}
				},
				v.getVariant());
	}

	// Convert public API Value to internal PropertyValue
	graph::PropertyValue toInternal(const Value &v) {
		return std::visit(
				[]<typename T0>(T0 &&arg) -> graph::PropertyValue {
					using T = std::decay_t<T0>;

					if constexpr (std::is_same_v<T, std::vector<float>>) {
						std::vector<graph::PropertyValue> vec;
						vec.reserve(arg.size());
						for (const auto value: arg) {
							vec.emplace_back(static_cast<double>(value));
						}
						return graph::PropertyValue(vec);
					} else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
						std::vector<graph::PropertyValue> vec;
						vec.reserve(arg.size());
						for (const auto &s: arg) {
							// Try parsing as different types in order
							// First try int
							try {
								size_t pos = 0;
								int64_t val = std::stoll(s, &pos);
								if (pos == s.size()) {
									vec.push_back(graph::PropertyValue(val));
									continue;
								}
							} catch (...) {
								// Not an int, continue to next type
							}
							// Then try double
							try {
								size_t pos = 0;
								double val = std::stod(s, &pos);
								if (pos == s.size()) {
									vec.push_back(graph::PropertyValue(val));
									continue;
								}
							} catch (...) {
								// Not a double, continue to next type
							}
							// Otherwise store as string
							vec.push_back(graph::PropertyValue(s));
						}
						return graph::PropertyValue(vec);
					} else if constexpr (std::is_same_v<T, std::shared_ptr<ValueList>>) {
						if (!arg)
							return graph::PropertyValue();
						std::vector<graph::PropertyValue> vec;
						vec.reserve(arg->elements.size());
						for (const auto &elem: arg->elements) {
							vec.push_back(toInternal(elem));
						}
						return graph::PropertyValue(std::move(vec));
					} else if constexpr (std::is_same_v<T, std::shared_ptr<ValueMap>>) {
						if (!arg)
							return graph::PropertyValue();
						graph::PropertyValue::MapType map;
						for (const auto &[k, v]: arg->entries) {
							map.emplace(k, toInternal(v));
						}
						return graph::PropertyValue(std::move(map));
					} else if constexpr (std::is_same_v<T, zyx::Bytes>) {
						graph::PropertyValue::Bytes bytes;
						bytes.reserve(arg.size());
						for (const std::uint8_t byte: arg) {
							bytes.push_back(static_cast<std::byte>(byte));
						}
						return graph::PropertyValue(std::move(bytes));
					} else if constexpr (std::is_same_v<T, std::shared_ptr<Node>> ||
										 std::is_same_v<T, std::shared_ptr<Edge>>) {
						return {};
					} else {
						return graph::PropertyValue(arg);
					}
				},
				v);
	}

	std::vector<graph::storage::LogicalGraphWriteProperty>
	toLogicalWriteProperties(const std::unordered_map<std::string, Value> &properties) {
		std::vector<graph::storage::LogicalGraphWriteProperty> result;
		result.reserve(properties.size());
		for (const auto &[key, value]: properties) {
			result.push_back({key, toInternal(value)});
		}
		std::ranges::sort(result, {}, &graph::storage::LogicalGraphWriteProperty::key);
		return result;
	}

	std::vector<std::string> singleLogicalLabel(const std::string &label) {
		return label.empty() ? std::vector<std::string>{} : std::vector<std::string>{label};
	}

	int64_t checkedDirectWriteId(uint64_t id, const char *entityName) {
		if (id > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			throw std::overflow_error(std::string("logical ") + entityName + " ID exceeds the public API range");
		}
		return static_cast<int64_t>(id);
	}

	uint64_t checkedDirectWriteEndpoint(int64_t id, const char *endpointName) {
		if (id <= 0) {
			throw std::invalid_argument(std::string("createEdge requires a positive ") + endpointName + " ID");
		}
		return static_cast<uint64_t>(id);
	}

	std::vector<int64_t> checkedDirectWriteIds(const std::vector<uint64_t> &ids, const char *entityName) {
		std::vector<int64_t> result;
		result.reserve(ids.size());
		for (const uint64_t id: ids) {
			result.push_back(checkedDirectWriteId(id, entityName));
		}
		return result;
	}

	void validatePropertyColumns(const std::vector<PropertyColumn> &columns, size_t rowCount,
								 std::string_view context) {
		std::unordered_set<std::string> seenNames;
		seenNames.reserve(columns.size());

		for (const auto &column: columns) {
			if (column.name.empty()) {
				throw std::invalid_argument(std::string(context) + " requires non-empty property column names");
			}
			if (column.values.size() != rowCount) {
				throw std::invalid_argument(std::string(context) + " property column '" + column.name + "' has " +
											std::to_string(column.values.size()) + " values for " +
											std::to_string(rowCount) + " rows");
			}
			auto [_, inserted] = seenNames.insert(column.name);
			if (!inserted) {
				throw std::invalid_argument(std::string(context) + " duplicate property column: " + column.name);
			}
		}
	}

	std::vector<detail::BulkPropertyColumn>
	toInternalPropertyColumns(const std::vector<PropertyColumn> &columns) {
		std::vector<detail::BulkPropertyColumn> internalColumns;
		internalColumns.reserve(columns.size());
		for (const auto &column: columns) {
			detail::BulkPropertyColumn internalColumn;
			internalColumn.key = column.name;
			internalColumn.values.reserve(column.values.size());
			for (const auto &value: column.values) {
				internalColumn.values.push_back(toInternal(value));
			}
			internalColumns.push_back(std::move(internalColumn));
		}
		return internalColumns;
	}

	void validateInternalPropertyColumns(const std::vector<detail::BulkPropertyColumn> &columns,
												  const size_t rowCount, const std::string_view context) {
		std::unordered_set<std::string> keys;
		keys.reserve(columns.size());
		for (const detail::BulkPropertyColumn &column: columns) {
			if (column.key.empty()) {
				throw std::invalid_argument(std::string(context) + " requires non-empty property column names");
			}
			if (column.values.size() != rowCount) {
				throw std::invalid_argument(std::string(context) + " property column values must match row count");
			}
			if (!keys.insert(column.key).second) {
				throw std::invalid_argument(std::string(context) + " requires unique property column names");
			}
		}
	}

		[[nodiscard]] std::vector<graph::storage::LogicalGraphEdgeCreateRequest>
		makeLogicalColumnarEdgeRequests(const std::string &edgeType, const std::vector<int64_t> &sourceIds,
									  const std::vector<int64_t> &targetIds,
									  const std::vector<detail::BulkPropertyColumn> &columns,
									  const std::string_view context) {
		if (sourceIds.size() != targetIds.size()) {
			throw std::invalid_argument(std::string(context) + " requires matching source and target counts");
		}
		validateInternalPropertyColumns(columns, sourceIds.size(), context);
		std::vector<graph::storage::LogicalGraphEdgeCreateRequest> requests(sourceIds.size());
		for (size_t row = 0U; row < requests.size(); ++row) {
			requests[row].source_node_id = checkedDirectWriteEndpoint(sourceIds[row], "source");
			requests[row].target_node_id = checkedDirectWriteEndpoint(targetIds[row], "target");
			requests[row].type = edgeType;
			requests[row].properties.reserve(columns.size());
		}
		for (const detail::BulkPropertyColumn &column: columns) {
			for (size_t row = 0U; row < requests.size(); ++row) {
				requests[row].properties.push_back({column.key, column.values[row]});
			}
		}
			return requests;
		}

		void validateLogicalColumnarEdgeEndpoints(graph::storage::LogicalGraphReadBackend &readBackend,
											 const std::vector<int64_t> &sourceIds,
											 const std::vector<int64_t> &targetIds) {
			if (sourceIds.size() != targetIds.size()) {
				throw std::invalid_argument("logical columnar Edge endpoint counts must match");
			}
			std::unique_ptr<graph::storage::LogicalGraphReadSnapshot> snapshot =
					readBackend.openLogicalGraphReadSnapshot();
			if (!snapshot) {
				throw std::runtime_error("logical columnar Edge ingest could not open its validation snapshot");
			}
			std::unordered_map<int64_t, bool> visibleEndpoints;
			visibleEndpoints.reserve(sourceIds.size() + targetIds.size());
			const auto isVisibleEndpoint = [&visibleEndpoints, &snapshot](int64_t nodeId) {
				if (nodeId <= 0) {
					return false;
				}
				if (const auto found = visibleEndpoints.find(nodeId); found != visibleEndpoints.end()) {
					return found->second;
				}
				const bool visible = snapshot->containsNode(static_cast<uint64_t>(nodeId));
				visibleEndpoints.emplace(nodeId, visible);
				return visible;
			};

			for (size_t row = 0U; row < sourceIds.size(); ++row) {
				if (!isVisibleEndpoint(sourceIds[row])) {
				throw detail::BulkInputError(
							row, "source_id", "source node " + std::to_string(sourceIds[row]) +
													 " does not exist or is inactive");
				}
				if (!isVisibleEndpoint(targetIds[row])) {
				throw detail::BulkInputError(
							row, "target_id", "target node " + std::to_string(targetIds[row]) +
													 " does not exist or is inactive");
				}
			}
		}

	// Convert public Value params to internal PropertyValue params
	graph::query::QueryContext toQueryContext(const std::unordered_map<std::string, Value> &params) {
		graph::query::QueryContext ctx;
		for (const auto &[key, val]: params) {
			ctx.parameters.emplace(key, toInternal(val));
		}
		return ctx;
	}

	namespace {

		// These mirror the production logical-query limits. The public traversal
		// APIs have no caller-provided resource budget, so they must not turn a
		// damaged or unexpectedly dense graph into unbounded backend work.
		constexpr size_t LOGICAL_ALGORITHM_MAXIMUM_NODES = 1'000'000U;
		constexpr size_t LOGICAL_ALGORITHM_MAXIMUM_EDGES_PER_NODE = 100'000U;
		constexpr size_t LOGICAL_ALGORITHM_ADJACENCY_PAGE_RECORDS = 1'024U;
		constexpr size_t LOGICAL_ALGORITHM_MAXIMUM_PROPERTY_ENTRIES = 10'000U;
		constexpr size_t LOGICAL_ALGORITHM_MAXIMUM_METADATA_PAGES = 4'096U;

		void validateLogicalAlgorithmNode(const graph::storage::LogicalGraphNode &node) {
			if (node.node_id == 0U) {
				throw std::runtime_error("logical graph backend returned an invalid Node");
			}
			std::unordered_set<std::string_view> labels;
			labels.reserve(node.labels.size());
			for (const std::string &label: node.labels) {
				if (label.empty() || !labels.insert(label).second) {
					throw std::runtime_error("logical graph backend returned an invalid Node label set");
				}
			}
		}

		void validateLogicalAlgorithmEdge(const graph::storage::LogicalGraphEdge &edge, uint64_t sourceNodeId) {
			if (edge.edge_id == 0U || edge.source_node_id != sourceNodeId || edge.target_node_id == 0U) {
				throw std::runtime_error("logical graph backend returned an invalid outgoing Edge");
			}
		}

		[[nodiscard]] std::optional<graph::storage::LogicalGraphNode>
		findLogicalAlgorithmNode(const graph::storage::LogicalGraphReadSnapshot &snapshot, uint64_t nodeId) {
			if (nodeId == 0U) {
				return std::nullopt;
			}
			const graph::storage::LogicalGraphNodePage page =
					snapshot.scanNodesAfter(nodeId - 1U, 1U, LOGICAL_ALGORITHM_MAXIMUM_METADATA_PAGES);
			if (page.nodes.size() > 1U || page.metadata_pages_visited > LOGICAL_ALGORITHM_MAXIMUM_METADATA_PAGES) {
				throw std::runtime_error("logical graph backend returned an invalid bounded Node page");
			}
			if (page.nodes.empty()) {
				if (page.may_have_more) {
					throw std::runtime_error("logical graph backend returned an empty nonterminal Node page");
				}
				return std::nullopt;
			}
			const graph::storage::LogicalGraphNode &node = page.nodes.front();
			if (node.node_id <= nodeId - 1U) {
				throw std::runtime_error("logical graph backend returned an unordered Node page");
			}
			validateLogicalAlgorithmNode(node);
			return node.node_id == nodeId ? std::optional<graph::storage::LogicalGraphNode>(node) : std::nullopt;
		}

		[[nodiscard]] Node toPublicLogicalAlgorithmNode(const graph::storage::LogicalGraphNode &node) {
			validateLogicalAlgorithmNode(node);
			Node result;
			result.id = checkedPublicEntityId(node.node_id, "Node");
			result.labels = node.labels;
			result.label = result.labels.empty() ? "" : result.labels.front();
			if (node.properties.size() > LOGICAL_ALGORITHM_MAXIMUM_PROPERTY_ENTRIES) {
				throw std::runtime_error("logical graph backend returned oversized Node properties");
			}
			result.properties.reserve(node.properties.size());
			for (const graph::storage::LogicalGraphPropertyEntry &entry: node.properties) {
				if (entry.key.empty() || !result.properties.emplace(entry.key, toPublicValue(entry.value)).second) {
					throw std::runtime_error("logical graph backend returned an invalid Node Property entry set");
				}
			}
			return result;
		}

		[[nodiscard]] graph::storage::LogicalGraphNode
		readRequiredLogicalAlgorithmNode(const graph::storage::LogicalGraphReadSnapshot &snapshot, uint64_t nodeId) {
			const graph::storage::LogicalGraphNode node = snapshot.readNode(nodeId);
			if (node.node_id != nodeId) {
				throw std::runtime_error("logical graph backend resolved a mismatched Node identity");
			}
			validateLogicalAlgorithmNode(node);
			return node;
		}

		template<typename EdgeVisitor>
		[[nodiscard]] bool visitLogicalAlgorithmOutgoingEdges(const graph::storage::LogicalGraphReadSnapshot &snapshot,
																	 uint64_t sourceNodeId, EdgeVisitor &&visitor) {
			std::optional<graph::storage::LogicalGraphAdjacencyContinuation> continuation;
			std::unordered_set<uint64_t> edgeIds;
			edgeIds.reserve(LOGICAL_ALGORITHM_ADJACENCY_PAGE_RECORDS);
			size_t visited = 0U;
			while (true) {
				const size_t remaining = LOGICAL_ALGORITHM_MAXIMUM_EDGES_PER_NODE - visited;
				const size_t pageRecords = std::min(LOGICAL_ALGORITHM_ADJACENCY_PAGE_RECORDS, remaining);
				const graph::storage::LogicalGraphAdjacencyPage page = snapshot.scanAdjacentEdges(
						sourceNodeId, graph::storage::LogicalGraphReadDirection::LGRD_OUTGOING, continuation, pageRecords);
				if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value())) {
					throw std::runtime_error("logical graph backend returned an invalid adjacency page");
				}
				for (const graph::storage::LogicalGraphEdge &edge: page.edges) {
					validateLogicalAlgorithmEdge(edge, sourceNodeId);
					if (!edgeIds.insert(edge.edge_id).second) {
						throw std::runtime_error("logical graph backend returned a duplicate Edge during traversal");
					}
					++visited;
					if (!visitor(edge)) {
						return false;
					}
				}
				if (!page.continuation.has_value()) {
					return true;
				}
				const graph::storage::LogicalGraphAdjacencyContinuation &next = *page.continuation;
				if (next.previous_edge_id != page.edges.back().edge_id || next.next_edge_id == 0U ||
					next.previous_edge_id == next.next_edge_id) {
					throw std::runtime_error("logical graph backend returned an invalid adjacency continuation");
				}
				if (visited == LOGICAL_ALGORITHM_MAXIMUM_EDGES_PER_NODE) {
					throw std::runtime_error("logical graph traversal exceeded its per-Node Edge bound");
				}
				continuation = next;
			}
		}

		[[nodiscard]] std::vector<Node>
		materializeLogicalAlgorithmPath(const graph::storage::LogicalGraphReadSnapshot &snapshot,
															 const std::unordered_map<uint64_t, uint64_t> &parents, uint64_t endNodeId) {
			std::vector<uint64_t> reverseIds;
			reverseIds.reserve(parents.size());
			uint64_t current = endNodeId;
			while (current != 0U) {
				if (reverseIds.size() == parents.size()) {
					throw std::runtime_error("logical graph shortest-path parent chain contains a cycle");
				}
				reverseIds.push_back(current);
				const auto parent = parents.find(current);
				if (parent == parents.end()) {
					throw std::runtime_error("logical graph shortest-path parent chain is incomplete");
				}
				current = parent->second;
			}
			std::reverse(reverseIds.begin(), reverseIds.end());

			std::vector<Node> path;
			path.reserve(reverseIds.size());
			for (const uint64_t nodeId: reverseIds) {
				const graph::storage::LogicalGraphNode node = readRequiredLogicalAlgorithmNode(snapshot, nodeId);
				path.push_back(toPublicLogicalAlgorithmNode(node));
			}
			return path;
		}

		[[nodiscard]] std::vector<Node>
		findLogicalShortestPath(const graph::storage::LogicalGraphReadBackend &backend, int64_t startId, int64_t endId,
								int maxDepth) {
			if (startId <= 0 || endId <= 0) {
				return {};
			}
			std::unique_ptr<graph::storage::LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
			if (!snapshot) {
				throw std::runtime_error("logical graph backend returned no read snapshot");
			}
			const uint64_t startNodeId = static_cast<uint64_t>(startId);
			const uint64_t endNodeId = static_cast<uint64_t>(endId);
			const std::optional<graph::storage::LogicalGraphNode> start =
					findLogicalAlgorithmNode(*snapshot, startNodeId);
			if (!start.has_value()) {
				return {};
			}
			if (startNodeId == endNodeId) {
				return {toPublicLogicalAlgorithmNode(*start)};
			}
			if (maxDepth <= 0) {
				return {};
			}

			struct QueueEntry {
				uint64_t node_id = 0U;
				int depth = 0;
			};
			std::deque<QueueEntry> queue;
			std::unordered_map<uint64_t, uint64_t> parents;
			parents.reserve(1'024U);
			parents.emplace(startNodeId, 0U);
			queue.push_back({startNodeId, 0});

			while (!queue.empty()) {
				const QueueEntry current = queue.front();
				queue.pop_front();
				if (current.node_id != startNodeId) {
					static_cast<void>(readRequiredLogicalAlgorithmNode(*snapshot, current.node_id));
				}
				if (current.depth >= maxDepth) {
					continue;
				}

				bool found = false;
				static_cast<void>(visitLogicalAlgorithmOutgoingEdges(*snapshot, current.node_id,
																					  [&](const graph::storage::LogicalGraphEdge &edge) {
					if (parents.contains(edge.target_node_id)) {
						return true;
					}
					if (parents.size() == LOGICAL_ALGORITHM_MAXIMUM_NODES) {
						throw std::runtime_error("logical graph shortest-path traversal exceeded its Node bound");
					}
					parents.emplace(edge.target_node_id, current.node_id);
					if (edge.target_node_id == endNodeId) {
						found = true;
						return false;
					}
					queue.push_back({edge.target_node_id, current.depth + 1});
					return true;
				}));
				if (found) {
					return materializeLogicalAlgorithmPath(*snapshot, parents, endNodeId);
				}
			}
			return {};
		}

		void breadthFirstLogicalTraversal(const graph::storage::LogicalGraphReadBackend &backend, int64_t startId,
												 const std::function<bool(const Node &)> &visitor) {
			if (!visitor) {
				throw std::invalid_argument("bfs requires a visitor");
			}
			if (startId <= 0) {
				return;
			}
			std::unique_ptr<graph::storage::LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
			if (!snapshot) {
				throw std::runtime_error("logical graph backend returned no read snapshot");
			}
			const uint64_t startNodeId = static_cast<uint64_t>(startId);
			const std::optional<graph::storage::LogicalGraphNode> start =
					findLogicalAlgorithmNode(*snapshot, startNodeId);
			if (!start.has_value()) {
				return;
			}

			std::deque<uint64_t> queue;
			std::unordered_set<uint64_t> visited;
			visited.reserve(1'024U);
			visited.insert(startNodeId);
			queue.push_back(startNodeId);
			while (!queue.empty()) {
				const uint64_t currentNodeId = queue.front();
				queue.pop_front();
				const graph::storage::LogicalGraphNode current =
						currentNodeId == startNodeId ? *start : readRequiredLogicalAlgorithmNode(*snapshot, currentNodeId);
				if (!visitor(toPublicLogicalAlgorithmNode(current))) {
					return;
				}
				static_cast<void>(visitLogicalAlgorithmOutgoingEdges(
						*snapshot, currentNodeId, [&](const graph::storage::LogicalGraphEdge &edge) {
						if (visited.contains(edge.target_node_id)) {
							return true;
						}
						if (visited.size() == LOGICAL_ALGORITHM_MAXIMUM_NODES) {
							throw std::runtime_error("logical graph breadth-first traversal exceeded its Node bound");
						}
						visited.insert(edge.target_node_id);
						queue.push_back(edge.target_node_id);
						return true;
					}));
			}
		}

	} // namespace

	// ========================================================================
	// 2. Result Implementation
	// ========================================================================

	class ResultImpl {
	public:
		explicit ResultImpl(graph::query::QueryResult internalRes) : result_(std::move(internalRes)) {
			prepareMetadata();
		}

		graph::query::QueryResult result_;

		// Cursor State
		size_t cursor_ = 0;
		bool started_ = false;

		std::vector<std::string> columnNames_;
		int mode_ = 0; // 0=Rows (Default), 1=NodeStream, 2=EdgeStream
		std::string error_msg;

		void prepareMetadata() {
			// Priority: Explicit columns from Engine -> Implicit columns from Data
			const auto &executorCols = result_.getColumns();
			if (!executorCols.empty()) {
				columnNames_ = executorCols;
			} else if (!result_.getRows().empty()) {
				// Implicit columns: Extract from the first row and SORT for determinism
				for (const auto &firstRow = result_.getRows()[0]; const auto &k: firstRow | std::views::keys)
					columnNames_.push_back(k);
				std::ranges::sort(columnNames_);
			}

			// Determine mode based on content types of the first row
			// This preserves legacy "stream" behavior for single-entity queries
			if (columnNames_.size() == 1 && !result_.getRows().empty()) {
				const auto &firstVal = result_.getRows()[0].at(columnNames_[0]);
				if (firstVal.isGraphNode()) {
					mode_ = 1; // Node Stream
				} else if (firstVal.isGraphEdge()) {
					mode_ = 2; // Edge Stream
				}
			} else {
				mode_ = 0; // Standard Table Row
			}
		}

		double getDuration() const { return result_.getDuration(); }
	};

	Result::~Result() = default;
	Result::Result() = default;
	Result::Result(Result &&) noexcept = default;
	Result &Result::operator=(Result &&) noexcept = default;

	bool Result::hasNext() const {
		if (!impl_)
			return false;
		size_t total = impl_->result_.rowCount(); // Unified: everything is in rows now

		// If we haven't started, check if there is at least 1 item
		if (!impl_->started_)
			return total > 0;

		// If we have started, check if there is a next item
		return impl_->cursor_ + 1 < total;
	}

	void Result::next() const {
		if (impl_) {
			if (!impl_->started_) {
				impl_->started_ = true;
				impl_->cursor_ = 0;
			} else {
				impl_->cursor_++;
			}
		}
	}

	Value Result::get(const std::string &key) const {
		if (!impl_ || !impl_->started_)
			return std::monostate{};

		const auto &rows = impl_->result_.getRows();
		if (impl_->cursor_ >= rows.size())
			return std::monostate{};

		const auto &row = rows[impl_->cursor_];

		// --- MODE 0: ROWS (Standard) ---
		if (impl_->mode_ == 0) {
			// 1. Exact Match
			auto it = row.find(key);
			if (it != row.end())
				return toPublicValue(it->second);

			// 2. Fuzzy Match (User asks "name", DB has "n.name")
			for (const auto &[dbKey, dbVal]: row) {
				if (key.find('.') == std::string::npos && dbKey.size() > key.size()) {
					if (dbKey.ends_with("." + key))
						return toPublicValue(dbVal);
				}
				if (dbKey.find('.') == std::string::npos && key.size() > dbKey.size()) {
					if (key.ends_with("." + dbKey))
						return toPublicValue(dbVal);
				}
			}
			return std::monostate{};
		}

		// --- MODE 1 & 2: ENTITY STREAM (Legacy Compatibility) ---
		// In these modes, we assume there is exactly 1 column containing the entity.
		else if (impl_->mode_ == 1 || impl_->mode_ == 2) {
			const std::string &colName = impl_->columnNames_[0];
			const auto &entityVal = row.at(colName); // Safe because mode implies !empty

			// If requesting the entity itself (key empty or standard aliases)
			if (key.empty() || key == colName) {
				return toPublicValue(entityVal);
			}

			// If requesting a property inside the entity
			if (entityVal.isNode()) {
				const auto &props = entityVal.asNode().getProperties();
				auto it = props.find(key);
				if (it != props.end())
					return toPublicValue(it->second);
			} else if (entityVal.isLogicalNode()) {
				const auto &props = entityVal.asLogicalNode().properties;
				auto it = props.find(key);
				if (it != props.end())
					return toPublicValue(it->second);
			} else if (entityVal.isEdge()) {
				const auto &props = entityVal.asEdge().getProperties();
				auto it = props.find(key);
				if (it != props.end())
					return toPublicValue(it->second);
			} else if (entityVal.isLogicalEdge()) {
				const auto &props = entityVal.asLogicalEdge().properties;
				auto it = props.find(key);
				if (it != props.end())
					return toPublicValue(it->second);
			}
		}

		return std::monostate{};
	}

	Value Result::get(int index) const {
		if (!impl_ || !impl_->started_ || index < 0 || index >= static_cast<int>(impl_->columnNames_.size()))
			return std::monostate{};

		// In the unified Row model, index maps directly to the sorted column name list
		return get(impl_->columnNames_[index]);
	}

	namespace detail {
		Value getTypedResultValue(const Result &result, int index) {
			if (!result.impl_ || !result.impl_->started_ || index < 0 ||
				index >= static_cast<int>(result.impl_->columnNames_.size())) {
				return std::monostate{};
			}

			const auto &rows = result.impl_->result_.getRows();
			if (result.impl_->cursor_ >= rows.size()) {
				return std::monostate{};
			}

			const auto &row = rows[result.impl_->cursor_];
			const auto &columnName = result.impl_->columnNames_[index];
			auto it = row.find(columnName);
			if (it == row.end()) {
				return std::monostate{};
			}
			return toDriverAbiValue(it->second);
		}
	} // namespace detail

	int Result::getColumnCount() const { return impl_ ? static_cast<int>(impl_->columnNames_.size()) : 0; }

	std::string Result::getColumnName(int index) const {
		return (impl_ && index >= 0 && index < static_cast<int>(impl_->columnNames_.size()))
					   ? impl_->columnNames_[index]
					   : "";
	}

	double Result::getDuration() const { return impl_ ? impl_->getDuration() : 0.0; }

	bool Result::isSuccess() const { return impl_ && impl_->error_msg.empty(); }

	std::string Result::getError() const {
		if (!impl_)
			return "Result not initialized";
		return impl_->error_msg;
	}

	// ========================================================================
	// 3. Database Implementation
	// ========================================================================

	class DatabaseImpl {
	public:
		explicit DatabaseImpl(const std::string &path) : db_(path) {}
		graph::Database db_;
		std::optional<Transaction> cypherTxn_;
		std::atomic<size_t> active_public_transactions_{0U};

		void registerPublicTransaction(TransactionImpl *transaction);
		void unregisterPublicTransaction(TransactionImpl *transaction) noexcept;
		[[nodiscard]] graph::storage::LogicalGraphReadBackend &resolveLogicalReadTarget() const;
		[[nodiscard]] graph::storage::LogicalGraphWriteBackend &resolveLogicalWriteTarget() const;

		void ensureOpen() {
			if (!db_.isOpen()) {
				db_.open();
			}
		}

	};

	// ========================================================================
	// 4. Transaction Implementation
	// ========================================================================

	class TransactionImpl {
	public:
		explicit TransactionImpl(std::unique_ptr<graph::storage::LogicalGraphTransaction> logicalTxn,
										 DatabaseImpl *dbImpl) :
			logical_txn_(std::move(logicalTxn)), dbImpl_(dbImpl) {
			if (!logical_txn_) {
				throw std::invalid_argument("logical public transaction requires an inner transaction");
			}
			logical_overlay_ = std::make_unique<graph::storage::LogicalGraphTransactionOverlay>(*logical_txn_);
			registerTransaction();
		}
		~TransactionImpl() { unregisterTransaction(); }

		[[nodiscard]] bool isActive() const {
			return logical_txn_ && logical_txn_->state() == graph::storage::LogicalGraphTransactionState::LGTTS_ACTIVE;
		}
		[[nodiscard]] bool isReadOnly() const { return logical_txn_ && logical_txn_->isReadOnly(); }
		[[nodiscard]] graph::storage::LogicalGraphTransactionOverlay &requireLogicalWriteOverlay() {
			if (!isActive()) {
				throw std::logic_error("transaction-bound direct write requires an active transaction");
			}
			if (logical_overlay_ == nullptr) {
				throw std::logic_error("transaction-bound direct writes require a logical transaction");
			}
			if (isReadOnly()) {
				throw std::logic_error("transaction-bound direct write requires a read-write transaction");
			}
			return *logical_overlay_;
		}
		[[nodiscard]] graph::storage::LogicalGraphTransactionOverlay &requireLogicalReadOverlay() {
			if (!isActive()) {
				throw std::logic_error("transaction-bound read requires an active transaction");
			}
			if (logical_overlay_ == nullptr) {
				throw std::logic_error("transaction-bound reads require a logical transaction");
			}
			return *logical_overlay_;
		}
		void commit() {
			if (!isActive()) {
				throw std::runtime_error("Cannot commit: transaction is not active");
			}
			try {
				static_cast<void>(logical_txn_->commit());
			} catch (...) {
				if (!isActive()) {
					unregisterTransaction();
				}
				throw;
			}
			unregisterTransaction();
		}
		void rollback() {
			if (!isActive()) {
				throw std::runtime_error("Cannot rollback: transaction is not active");
			}
			try {
				logical_txn_->rollback();
			} catch (...) {
				if (!isActive()) {
					unregisterTransaction();
				}
				throw;
			}
			unregisterTransaction();
		}

	private:
		void registerTransaction() {
			if (dbImpl_ == nullptr) {
				throw std::invalid_argument("public transaction requires an owning Database");
			}
			dbImpl_->registerPublicTransaction(this);
			registered_ = true;
		}
		void unregisterTransaction() noexcept {
			if (registered_ && dbImpl_ != nullptr) {
				dbImpl_->unregisterPublicTransaction(this);
				registered_ = false;
			}
		}

	public:

		std::unique_ptr<graph::storage::LogicalGraphTransaction> logical_txn_;
		std::unique_ptr<graph::storage::LogicalGraphTransactionOverlay> logical_overlay_;
		DatabaseImpl *dbImpl_; // Non-owning reference to parent database
		bool registered_ = false;
	};

	void DatabaseImpl::registerPublicTransaction(TransactionImpl *transaction) {
		if (transaction == nullptr) {
			throw std::invalid_argument("public transaction registration requires a transaction");
		}
		size_t current = active_public_transactions_.load(std::memory_order_acquire);
		for (;;) {
			if (current == std::numeric_limits<size_t>::max()) {
				throw std::overflow_error("public transaction counter overflowed");
			}
			if (active_public_transactions_.compare_exchange_weak(current, current + 1U, std::memory_order_acq_rel,
																	 std::memory_order_acquire)) {
				return;
			}
		}
	}

	void DatabaseImpl::unregisterPublicTransaction(TransactionImpl *transaction) noexcept {
		if (transaction == nullptr) {
			return;
		}
		size_t current = active_public_transactions_.load(std::memory_order_acquire);
		while (current != 0U &&
			   !active_public_transactions_.compare_exchange_weak(current, current - 1U, std::memory_order_acq_rel,
																			 std::memory_order_acquire)) {
		}
	}

	graph::storage::LogicalGraphReadBackend &DatabaseImpl::resolveLogicalReadTarget() const {
		const auto logicalReads =
				std::dynamic_pointer_cast<graph::storage::LogicalGraphReadBackend>(db_.getStorageBackend());
		if (!logicalReads) {
			throw std::logic_error("logical backend does not provide a logical read interface");
		}
		return *logicalReads;
	}

	graph::storage::LogicalGraphWriteBackend &DatabaseImpl::resolveLogicalWriteTarget() const {
		if (active_public_transactions_.load(std::memory_order_acquire) != 0U) {
			throw std::logic_error("direct public API writes are not allowed while a transaction is active");
		}

		const auto logicalWrites = db_.getLogicalGraphWriteBackend();
		if (!logicalWrites) {
			throw std::logic_error("logical backend was opened read-only and cannot perform public API writes");
		}
		return *logicalWrites;
	}

	Transaction::Transaction() = default;
	Transaction::~Transaction() = default;
	Transaction::Transaction(Transaction &&) noexcept = default;
	Transaction &Transaction::operator=(Transaction &&) noexcept = default;

	Result Transaction::execute(const std::string &cypher) const { return execute(cypher, {}); }

	Result Transaction::execute(const std::string &cypher, const std::unordered_map<std::string, Value> &params) const {
		if (!impl_ || !impl_->isActive()) {
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(graph::query::QueryResult());
			res.impl_->error_msg = "Transaction is not active";
			return res;
		}

		try {
			auto start = std::chrono::high_resolution_clock::now();

			auto ctx = toQueryContext(params);
			// Propagate read-only mode from internal transaction
			if (impl_->isReadOnly()) {
				ctx.execMode = graph::query::ExecMode::EM_READ_ONLY;
			}
			auto queryEngine = impl_->dbImpl_->db_.getQueryEngine();
			auto plan = queryEngine->buildPlan(cypher);
			graph::query::QueryResult internalRes = queryEngine->executePlanWithLogicalTransactionOverlay(
					std::move(plan), ctx, *impl_->logical_overlay_, *impl_->logical_overlay_);

			auto end = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double, std::milli> elapsed = end - start;
			internalRes.setDuration(elapsed.count());

			Result res;
			res.impl_ = std::make_unique<ResultImpl>(std::move(internalRes));
			return res;
		} catch (const std::exception &e) {
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(graph::query::QueryResult());
			res.impl_->error_msg = e.what();
			return res;
		} catch (...) {
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(graph::query::QueryResult());
			res.impl_->error_msg = "Unknown error";
			return res;
		}
	}

	int64_t Transaction::createNode(const std::string &label,
									 const std::unordered_map<std::string, Value> &props) const {
		if (!impl_) {
			throw std::logic_error("transaction-bound direct write requires an initialized transaction");
		}
		auto &overlay = impl_->requireLogicalWriteOverlay();
		graph::storage::LogicalGraphNodeCreateRequest request;
		request.labels = singleLogicalLabel(label);
		request.properties = toLogicalWriteProperties(props);
		const std::vector<uint64_t> ids = overlay.createLogicalNodes({std::move(request)});
		if (ids.size() != 1U) {
			throw std::logic_error("logical Node creation returned an unexpected result count");
		}
		return checkedDirectWriteId(ids.front(), "Node");
	}

	int64_t Transaction::createNode(const std::vector<std::string> &labels,
									 const std::unordered_map<std::string, Value> &props) const {
		if (!impl_) {
			throw std::logic_error("transaction-bound direct write requires an initialized transaction");
		}
		auto &overlay = impl_->requireLogicalWriteOverlay();
		graph::storage::LogicalGraphNodeCreateRequest request;
		request.labels = labels;
		request.properties = toLogicalWriteProperties(props);
		const std::vector<uint64_t> ids = overlay.createLogicalNodes({std::move(request)});
		if (ids.size() != 1U) {
			throw std::logic_error("logical Node creation returned an unexpected result count");
		}
		return checkedDirectWriteId(ids.front(), "Node");
	}

	std::vector<int64_t> Transaction::createNodes(
			const std::string &label, const std::vector<std::unordered_map<std::string, Value>> &propsList) const {
		if (propsList.empty()) {
			return {};
		}
		if (!impl_) {
			throw std::logic_error("transaction-bound direct write requires an initialized transaction");
		}
		auto &overlay = impl_->requireLogicalWriteOverlay();
		std::vector<graph::storage::LogicalGraphNodeCreateRequest> requests;
		requests.reserve(propsList.size());
		for (const auto &properties: propsList) {
			requests.push_back({singleLogicalLabel(label), toLogicalWriteProperties(properties)});
		}
		return checkedDirectWriteIds(overlay.createLogicalNodes(requests), "Node");
	}

	std::vector<int64_t> Transaction::createNodesColumnar(const std::string &label, const size_t count,
																 const std::vector<PropertyColumn> &columns) const {
		validatePropertyColumns(columns, count, "Transaction::createNodesColumnar");
		if (count == 0U) {
			return {};
		}
		if (!impl_) {
			throw std::logic_error("transaction-bound direct write requires an initialized transaction");
		}
		auto &overlay = impl_->requireLogicalWriteOverlay();
		std::vector<graph::storage::LogicalGraphNodeCreateRequest> requests(count);
		for (auto &request: requests) {
			request.labels = singleLogicalLabel(label);
			request.properties.reserve(columns.size());
		}
		for (const PropertyColumn &column: columns) {
			for (size_t row = 0U; row < count; ++row) {
				requests[row].properties.push_back({column.name, toInternal(column.values[row])});
			}
		}
		return checkedDirectWriteIds(overlay.createLogicalNodes(requests), "Node");
	}

	int64_t Transaction::createEdge(const int64_t sourceId, const int64_t targetId, const std::string &edgeType,
									 const std::unordered_map<std::string, Value> &props) const {
		if (!impl_) {
			throw std::logic_error("transaction-bound direct write requires an initialized transaction");
		}
		auto &overlay = impl_->requireLogicalWriteOverlay();
		graph::storage::LogicalGraphEdgeCreateRequest request;
		request.source_node_id = checkedDirectWriteEndpoint(sourceId, "source");
		request.target_node_id = checkedDirectWriteEndpoint(targetId, "target");
		request.type = edgeType;
		request.properties = toLogicalWriteProperties(props);
		const std::vector<uint64_t> ids = overlay.createLogicalEdges({std::move(request)});
		if (ids.size() != 1U) {
			throw std::logic_error("logical Edge creation returned an unexpected result count");
		}
		return checkedDirectWriteId(ids.front(), "Edge");
	}

	std::vector<int64_t> Transaction::createEdges(
			const std::string &edgeType,
			const std::vector<std::tuple<int64_t, int64_t, std::unordered_map<std::string, Value>>> &edgesList) const {
		if (edgesList.empty()) {
			return {};
		}
		if (!impl_) {
			throw std::logic_error("transaction-bound direct write requires an initialized transaction");
		}
		auto &overlay = impl_->requireLogicalWriteOverlay();
		std::vector<graph::storage::LogicalGraphEdgeCreateRequest> requests;
		requests.reserve(edgesList.size());
		for (const auto &[sourceId, targetId, properties]: edgesList) {
			requests.push_back({checkedDirectWriteEndpoint(sourceId, "source"),
								checkedDirectWriteEndpoint(targetId, "target"), edgeType,
								toLogicalWriteProperties(properties)});
		}
		return checkedDirectWriteIds(overlay.createLogicalEdges(requests), "Edge");
	}

	std::vector<int64_t> Transaction::createEdgesColumnar(const std::string &edgeType,
														 const std::vector<int64_t> &sourceIds,
														 const std::vector<int64_t> &targetIds,
														 const std::vector<PropertyColumn> &columns) const {
		if (sourceIds.size() != targetIds.size()) {
			throw std::invalid_argument("createEdgesColumnar requires matching source and target counts");
		}
		validatePropertyColumns(columns, sourceIds.size(), "Transaction::createEdgesColumnar");
		if (sourceIds.empty()) {
			return {};
		}
		if (!impl_) {
			throw std::logic_error("transaction-bound direct write requires an initialized transaction");
		}
		auto &overlay = impl_->requireLogicalWriteOverlay();
		const std::vector<detail::BulkPropertyColumn> internalColumns = toInternalPropertyColumns(columns);
		validateLogicalColumnarEdgeEndpoints(overlay, sourceIds, targetIds);
		const std::vector<graph::storage::LogicalGraphEdgeCreateRequest> requests =
				makeLogicalColumnarEdgeRequests(edgeType, sourceIds, targetIds, internalColumns,
																	 "Transaction::createEdgesColumnar");
		return checkedDirectWriteIds(overlay.createLogicalEdges(requests), "Edge");
	}

	std::vector<Node> Transaction::getShortestPath(const int64_t startId, const int64_t endId, const int maxDepth) const {
		if (!impl_) {
			throw std::logic_error("transaction-bound read requires an initialized transaction");
		}
		return findLogicalShortestPath(impl_->requireLogicalReadOverlay(), startId, endId, maxDepth);
	}

	void Transaction::bfs(const int64_t startNodeId, const std::function<bool(const Node &)> &visitor) const {
		if (!impl_) {
			throw std::logic_error("transaction-bound read requires an initialized transaction");
		}
		breadthFirstLogicalTraversal(impl_->requireLogicalReadOverlay(), startNodeId, visitor);
	}

	void Transaction::commit() {
		if (!impl_)
			throw std::runtime_error("Transaction not initialized");
		impl_->commit();
	}

	void Transaction::rollback() {
		if (!impl_)
			throw std::runtime_error("Transaction not initialized");
		impl_->rollback();
	}

	bool Transaction::isActive() const { return impl_ && impl_->isActive(); }

	bool Transaction::isReadOnly() const { return impl_ && impl_->isReadOnly(); }

	// ========================================================================
	// 5. Database API Methods
	// ========================================================================

	Database::Database(const std::string &path) : impl_(std::make_unique<DatabaseImpl>(path)) {}
	Database::~Database() = default;

	void Database::open() const { impl_->db_.open(); }
	bool Database::openIfExists() const { return impl_->db_.openIfExists(); }
	void Database::close() const { impl_->db_.close(); }
	void Database::save() const { impl_->db_.flush(); }

	Transaction Database::beginTransaction() {
		if (!impl_->db_.isOpen()) {
			impl_->db_.open();
		}

		const auto logicalTransactions = impl_->db_.getLogicalGraphTransactionalBackend();
		if (!logicalTransactions) {
			throw std::logic_error("public Database requires a logical transaction backend");
		}
		Transaction txn;
		txn.impl_ = std::make_unique<TransactionImpl>(logicalTransactions->beginLogicalGraphTransaction(false), impl_.get());
		return txn;
	}

	Transaction Database::beginBulkTransaction() {
		if (!impl_->db_.isOpen()) {
			impl_->db_.open();
		}

		const auto logicalTransactions = impl_->db_.getLogicalGraphTransactionalBackend();
		if (!logicalTransactions) {
			throw std::logic_error("public Database requires a logical transaction backend");
		}
		Transaction txn;
		txn.impl_ = std::make_unique<TransactionImpl>(logicalTransactions->beginLogicalGraphTransaction(false), impl_.get());
		return txn;
	}

	Transaction Database::beginReadOnlyTransaction() {
		if (!impl_->db_.isOpen()) {
			impl_->db_.open();
		}

		const auto logicalTransactions = impl_->db_.getLogicalGraphTransactionalBackend();
		if (!logicalTransactions) {
			throw std::logic_error("public Database requires a logical transaction backend");
		}
		Transaction txn;
		txn.impl_ = std::make_unique<TransactionImpl>(logicalTransactions->beginLogicalGraphTransaction(true), impl_.get());
		return txn;
	}

	bool Database::hasActiveTransaction() const {
		return impl_->db_.hasActiveTransaction() || impl_->cypherTxn_.has_value() ||
			   impl_->active_public_transactions_.load(std::memory_order_acquire) != 0U;
	}

	void Database::setThreadPoolSize(size_t poolSize) const { impl_->db_.setThreadPoolSize(poolSize); }

	Result Database::execute(const std::string &cypher) const { return execute(cypher, {}); }

	Result Database::execute(const std::string &cypher, const std::unordered_map<std::string, Value> &params) const {
		try {
			{
				graph::debug::ScopedPerfTimer timer("api.ensure_open");
				impl_->ensureOpen();
			}
			// Intercept transaction control statements (BEGIN/COMMIT/ROLLBACK)
			auto txnKind = CypherTxnKind::NONE;
			{
				graph::debug::ScopedPerfTimer timer("api.detect_txn_statement");
				txnKind = detectCypherTxnStatement(cypher);
			}
			if (txnKind != CypherTxnKind::NONE) {
				graph::query::QueryResult txnResult;
				auto start = std::chrono::high_resolution_clock::now();

				switch (txnKind) {
					case CypherTxnKind::TXN_BEGIN:
						if (impl_->cypherTxn_.has_value()) {
							throw std::runtime_error(
									"Nested transactions are not supported: transaction already active");
						}
						impl_->cypherTxn_.emplace(const_cast<Database *>(this)->beginTransaction());
						txnResult.addRow(
								{{"result", graph::query::ResultValue(graph::PropertyValue("Transaction started"))}});
						break;
					case CypherTxnKind::TXN_COMMIT:
						if (!impl_->cypherTxn_.has_value()) {
							throw std::runtime_error("No active transaction to commit");
						}
						impl_->cypherTxn_->commit();
						impl_->cypherTxn_.reset();
						txnResult.addRow(
								{{"result", graph::query::ResultValue(graph::PropertyValue("Transaction committed"))}});
						break;
					case CypherTxnKind::TXN_ROLLBACK:
						if (!impl_->cypherTxn_.has_value()) {
							throw std::runtime_error("No active transaction to rollback");
						}
						impl_->cypherTxn_->rollback();
						impl_->cypherTxn_.reset();
						txnResult.addRow({{"result", graph::query::ResultValue(
															 graph::PropertyValue("Transaction rolled back"))}});
						break;
					default: // ZYX_COV_EXCL_LINE: detectCypherTxnStatement returns only handled transaction kinds.
						break;
				}

				auto end = std::chrono::high_resolution_clock::now();
				std::chrono::duration<double, std::milli> elapsed = end - start;
				txnResult.setDuration(elapsed.count());

				Result res;
				res.impl_ = std::make_unique<ResultImpl>(std::move(txnResult));
				return res;
			}

			if (impl_->cypherTxn_.has_value()) {
				return impl_->cypherTxn_->execute(cypher, params);
			}

			// Build the plan first to determine if the query mutates data
			std::shared_ptr<graph::query::QueryEngine> queryEngine;
			{
				graph::debug::ScopedPerfTimer timer("api.get_query_engine");
				queryEngine = impl_->db_.getQueryEngine();
			}
			auto plan = [&]() {
				graph::debug::ScopedPerfTimer timer("api.build_plan");
				return queryEngine->buildPlan(cypher);
			}();
			if ((plan.mutatesData || plan.mutatesSchema) && !impl_->db_.getLogicalGraphWriteBackend()) {
				throw std::runtime_error("relocatable backend was opened read-only and cannot execute Cypher mutations");
			}

			auto start = std::chrono::high_resolution_clock::now();

			auto ctx = [&]() {
				graph::debug::ScopedPerfTimer timer("api.to_query_context");
				return toQueryContext(params);
			}();
			auto internalRes = [&]() {
				graph::debug::ScopedPerfTimer timer("api.execute_plan");
				return queryEngine->executePlan(std::move(plan), ctx);
			}();

			auto end = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double, std::milli> elapsed = end - start;

			internalRes.setDuration(elapsed.count());

			graph::debug::ScopedPerfTimer resultTimer("api.result_wrap");
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(std::move(internalRes));
			return res;
		} catch (const std::exception &e) {
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(graph::query::QueryResult());
			res.impl_->error_msg = e.what();
			return res;
		} catch (...) {
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(graph::query::QueryResult());
			res.impl_->error_msg = "Unknown error";
			return res;
		}
	}

	int64_t Database::createNode(const std::string &label, const std::unordered_map<std::string, Value> &props) const {
		impl_->ensureOpen();
		auto &logicalWrites = impl_->resolveLogicalWriteTarget();
		graph::storage::LogicalGraphNodeCreateRequest request;
		request.labels = singleLogicalLabel(label);
		request.properties = toLogicalWriteProperties(props);
		const std::vector<uint64_t> ids = logicalWrites.createLogicalNodes({std::move(request)});
		if (ids.size() != 1U) {
			throw std::logic_error("logical Node creation returned an unexpected result count");
		}
		return checkedDirectWriteId(ids.front(), "Node");
	}

	int64_t Database::createNode(const std::vector<std::string> &labels,
								 const std::unordered_map<std::string, Value> &props) const {
		impl_->ensureOpen();
		auto &logicalWrites = impl_->resolveLogicalWriteTarget();
		graph::storage::LogicalGraphNodeCreateRequest request;
		request.labels = labels;
		request.properties = toLogicalWriteProperties(props);
		const std::vector<uint64_t> ids = logicalWrites.createLogicalNodes({std::move(request)});
		if (ids.size() != 1U) {
			throw std::logic_error("logical Node creation returned an unexpected result count");
		}
		return checkedDirectWriteId(ids.front(), "Node");
	}

	std::vector<int64_t>
	Database::createNodes(const std::string &label,
						  const std::vector<std::unordered_map<std::string, Value>> &propsList) const {
		if (propsList.empty())
			return {};

		impl_->ensureOpen();
		auto &logicalWrites = impl_->resolveLogicalWriteTarget();
		std::vector<graph::storage::LogicalGraphNodeCreateRequest> requests;
		requests.reserve(propsList.size());
		for (const auto &properties: propsList) {
			requests.push_back({singleLogicalLabel(label), toLogicalWriteProperties(properties)});
		}
		return checkedDirectWriteIds(logicalWrites.createLogicalNodes(requests), "Node");
	}

	std::vector<int64_t> Database::createNodesColumnar(const std::string &label, size_t count,
													   const std::vector<PropertyColumn> &columns) const {
		validatePropertyColumns(columns, count, "createNodesColumnar");
		if (count == 0) {
			return {};
		}

		impl_->ensureOpen();
		auto &logicalWrites = impl_->resolveLogicalWriteTarget();
		std::vector<graph::storage::LogicalGraphNodeCreateRequest> requests(count);
		for (graph::storage::LogicalGraphNodeCreateRequest &request: requests) {
			request.labels = singleLogicalLabel(label);
			request.properties.reserve(columns.size());
		}
		for (const PropertyColumn &column: columns) {
			for (size_t row = 0U; row < count; ++row) {
				requests[row].properties.push_back({column.name, toInternal(column.values[row])});
			}
		}
		return checkedDirectWriteIds(logicalWrites.createLogicalNodes(requests), "Node");
	}

	int64_t Database::createEdge(int64_t sourceId, int64_t targetId, const std::string &edgeType,
								 const std::unordered_map<std::string, Value> &props) const {
		impl_->ensureOpen();
		auto &logicalWrites = impl_->resolveLogicalWriteTarget();
		graph::storage::LogicalGraphEdgeCreateRequest request;
		request.source_node_id = checkedDirectWriteEndpoint(sourceId, "source");
		request.target_node_id = checkedDirectWriteEndpoint(targetId, "target");
		request.type = edgeType;
		request.properties = toLogicalWriteProperties(props);
		const std::vector<uint64_t> ids = logicalWrites.createLogicalEdges({std::move(request)});
		if (ids.size() != 1U) {
			throw std::logic_error("logical Edge creation returned an unexpected result count");
		}
		return checkedDirectWriteId(ids.front(), "Edge");
	}

	std::vector<int64_t> Database::createEdges(
			const std::string &edgeType,
			const std::vector<std::tuple<int64_t, int64_t, std::unordered_map<std::string, Value>>> &edgesList) const {
		if (edgesList.empty())
			return {};

		impl_->ensureOpen();
		auto &logicalWrites = impl_->resolveLogicalWriteTarget();
		std::vector<graph::storage::LogicalGraphEdgeCreateRequest> requests;
		requests.reserve(edgesList.size());
		for (const auto &[sourceId, targetId, properties]: edgesList) {
			requests.push_back({checkedDirectWriteEndpoint(sourceId, "source"),
								checkedDirectWriteEndpoint(targetId, "target"), edgeType,
								toLogicalWriteProperties(properties)});
		}
		return checkedDirectWriteIds(logicalWrites.createLogicalEdges(requests), "Edge");
	}

	std::vector<int64_t> Database::createEdgesColumnar(const std::string &edgeType,
													   const std::vector<int64_t> &sourceIds,
													   const std::vector<int64_t> &targetIds,
													   const std::vector<PropertyColumn> &columns) const {
		if (sourceIds.size() != targetIds.size()) {
			throw std::invalid_argument("createEdgesColumnar requires matching source and target counts");
		}

		const size_t count = sourceIds.size();
		validatePropertyColumns(columns, count, "createEdgesColumnar");
		if (count == 0) {
			return {};
		}
		impl_->ensureOpen();
		const std::vector<detail::BulkPropertyColumn> internalColumns = toInternalPropertyColumns(columns);
		auto &logicalWrites = impl_->resolveLogicalWriteTarget();
		auto &logicalReads = impl_->resolveLogicalReadTarget();
		validateLogicalColumnarEdgeEndpoints(logicalReads, sourceIds, targetIds);
		const std::vector<graph::storage::LogicalGraphEdgeCreateRequest> requests =
				makeLogicalColumnarEdgeRequests(edgeType, sourceIds, targetIds, internalColumns, "createEdgesColumnar");
		return checkedDirectWriteIds(logicalWrites.createLogicalEdges(requests), "Edge");
	}

	std::vector<int64_t> detail::DatabaseBulkInternal::createEdgesColumnar(
			const Database &database, Transaction &transaction, const std::string &edgeType,
			const std::vector<int64_t> &sourceIds, const std::vector<int64_t> &targetIds,
			const std::vector<BulkPropertyColumn> &columns) {
		if (sourceIds.size() != targetIds.size()) {
			throw std::invalid_argument("createEdgesColumnar requires matching source and target counts");
		}
		if (sourceIds.empty()) {
			return {};
		}
		database.impl_->ensureOpen();
		if (transaction.impl_ == nullptr || transaction.impl_->dbImpl_ != database.impl_.get()) {
			throw std::invalid_argument("columnar Edge ingest transaction does not belong to its Database");
		}
		if (!transaction.impl_->isActive()) {
			throw std::logic_error("columnar Edge ingest requires an active transaction");
		}
		if (transaction.impl_->isReadOnly() || transaction.impl_->logical_overlay_ == nullptr) {
			throw std::logic_error("columnar Edge ingest cannot write through a read-only logical transaction");
		}
		validateLogicalColumnarEdgeEndpoints(*transaction.impl_->logical_overlay_, sourceIds, targetIds);
		const std::vector<graph::storage::LogicalGraphEdgeCreateRequest> requests =
				makeLogicalColumnarEdgeRequests(edgeType, sourceIds, targetIds, columns, "createEdgesColumnar");
		return checkedDirectWriteIds(transaction.impl_->logical_overlay_->createLogicalEdges(requests), "Edge");
	}

	bool Database::createNodePropertyIndexes(const std::string &label,
											 const std::vector<std::string> &properties) const {
		impl_->ensureOpen();
		if (label.empty()) {
			throw std::invalid_argument("createNodePropertyIndexes requires a non-empty label");
		}
		if (properties.empty()) {
			return true;
		}
		if (impl_->db_.hasActiveTransaction() || impl_->cypherTxn_.has_value() ||
			impl_->active_public_transactions_.load(std::memory_order_acquire) != 0U) {
			throw std::runtime_error("Schema operations cannot run inside an active transaction");
		}

		std::vector<graph::storage::LogicalGraphNodeIndexCreateRequest> logicalRequests;
		logicalRequests.reserve(properties.size());
		for (const auto &property: properties) {
			if (property.empty()) {
				throw std::invalid_argument("createNodePropertyIndexes requires non-empty property names");
			}
			logicalRequests.push_back({"", label, {property}});
		}
		const auto logicalSchema = std::dynamic_pointer_cast<graph::storage::LogicalGraphSchemaBackend>(
				impl_->db_.getStorageBackend());
		if (!logicalSchema) {
			throw std::logic_error("public Database requires a logical schema backend");
		}
		const std::vector<graph::storage::LogicalGraphIndexDefinition> created =
				logicalSchema->createLogicalNodeIndexes(logicalRequests);
		if (created.size() != logicalRequests.size()) {
			throw std::runtime_error("logical schema backend returned an incomplete node-index batch");
		}
		for (size_t index = 0U; index < created.size(); ++index) {
			const graph::storage::LogicalGraphIndexDefinition &definition = created[index];
			if (definition.index_id == 0U || definition.kind != graph::storage::LogicalGraphIndexKind::LGIK_PROPERTY ||
				definition.label != label || definition.property_keys != logicalRequests[index].property_keys) {
				throw std::runtime_error("logical schema backend returned an inconsistent node-index definition");
			}
		}
		impl_->db_.getQueryEngine()->getPlanCache().clear();
		return true;
	}

	std::vector<Node> Database::getShortestPath(int64_t startId, int64_t endId, int maxDepth) const {
		impl_->ensureOpen();
		return findLogicalShortestPath(impl_->resolveLogicalReadTarget(), startId, endId, maxDepth);
	}

	void Database::bfs(int64_t startNodeId, const std::function<bool(const Node &)> &visitor) const {
		impl_->ensureOpen();
		breadthFirstLogicalTraversal(impl_->resolveLogicalReadTarget(), startNodeId, visitor);
	}

} // namespace zyx
