/**
 * @file LogicalPlanProcedureRuntime.cpp
 * @brief Implementation of bounded logical stored procedures.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph/core/Types.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"

namespace graph::query::execution {

	namespace {

		using PropertyMap = std::unordered_map<std::string, PropertyValue>;
		using NodePropertyMaps = std::unordered_map<uint64_t, PropertyMap>;

		[[nodiscard]] const logical::LogicalOperator *requireOnlyChild(const logical::LogicalOperator &op) {
			const std::vector<logical::LogicalOperator *> children = op.getChildren();
			if (children.size() != 1U || children.front() == nullptr) {
				throw std::logic_error("logical procedure projection requires exactly one child");
			}
			return children.front();
		}

		[[nodiscard]] PropertyMap materializeNodeProperties(const storage::LogicalGraphReadSnapshot &snapshot,
															 const storage::LogicalGraphNode &node,
															 size_t maximumEntries,
															 const QueryContext &context) {
			context.checkGuard();
			static_cast<void>(snapshot);
			PropertyMap properties;
		if (node.properties.size() > maximumEntries) {
			throw std::runtime_error("logical Vector procedure Node Properties exceeded its caller-supplied bound");
			}
		properties.reserve(node.properties.size());
		for (const storage::LogicalGraphPropertyEntry &entry: node.properties) {
				context.checkGuard();
				if (entry.key.empty() || !properties.emplace(entry.key, entry.value).second) {
					throw std::runtime_error("logical Vector procedure read an invalid Node Property entry set");
				}
			}
			return properties;
		}

		void cacheResolvedNodeProperties(const LogicalPlanReadRow &row, NodePropertyMaps &nodeProperties) {
			for (const auto &[alias, properties]: row.property_cache) {
				const auto binding = row.values.find(alias);
				if (binding == row.values.end()) { // ZYX_COV_EXCL_LINE: evaluator writes cache entries only for aliases in this row.
					throw std::logic_error("logical Vector procedure Property cache has no matching binding");
				}
				const auto *node = std::get_if<storage::LogicalGraphNode>(&binding->second);
				if (node == nullptr) { // ZYX_COV_EXCL_LINE: only Node Property evaluation populates this cache.
					continue;
				}
				auto [cached, inserted] = nodeProperties.emplace(node->node_id, properties);
				if (!inserted && cached->second != properties) { // ZYX_COV_EXCL_LINE: one snapshot cannot resolve divergent Node Properties.
					throw std::runtime_error("logical Vector procedure observed inconsistent Node Properties");
				}
			}
		}

		void attachCachedNodeProperties(LogicalPlanReadRow &row, const NodePropertyMaps &nodeProperties) {
			for (const auto &[alias, value]: row.values) {
				const auto *node = std::get_if<storage::LogicalGraphNode>(&value);
				if (node == nullptr) {
					continue;
				}
				if (const auto properties = nodeProperties.find(node->node_id); properties != nodeProperties.end()) {
					row.property_cache.emplace(alias, properties->second);
				}
			}
		}

		void validateNode(const storage::LogicalGraphNode &node) {
			if (node.node_id == 0U) { // ZYX_COV_EXCL_LINE: both vector-hit paths reject zero IDs before Node hydration.
				throw std::runtime_error("logical Vector procedure backend returned an invalid Node");
			}
			std::unordered_set<std::string_view> labels;
			labels.reserve(node.labels.size());
			for (const std::string &label: node.labels) {
				if (label.empty() || !labels.insert(label).second) {
					throw std::runtime_error("logical Vector procedure backend returned an invalid Node label set");
				}
			}
		}

		void validateEdge(const storage::LogicalGraphEdge &edge) {
			if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
				throw std::runtime_error("logical procedure backend returned an invalid relationship");
			}
		}

		[[nodiscard]] std::string requireIndexName(const PropertyValue &value) {
			if (value.getType() != PropertyType::STRING) {
				throw std::invalid_argument("db.index.vector.queryNodes requires a String index name");
			}
			const std::string &name = std::get<std::string>(value.getVariant());
			if (name.empty()) {
				throw std::invalid_argument("db.index.vector.queryNodes requires a nonempty index name");
			}
			return name;
		}

		[[nodiscard]] size_t requireTopK(const PropertyValue &value) {
			if (value.getType() != PropertyType::INTEGER) {
				throw std::invalid_argument("db.index.vector.queryNodes requires a nonnegative integer topK");
			}
			const int64_t topK = std::get<int64_t>(value.getVariant());
			if (topK < 0 || static_cast<uint64_t>(topK) > std::numeric_limits<size_t>::max()) {
				throw std::invalid_argument("db.index.vector.queryNodes topK is outside the supported range");
			}
			return static_cast<size_t>(topK);
		}

		[[nodiscard]] std::vector<float> requireQueryVector(const PropertyValue &value) {
			if (value.getType() != PropertyType::LIST) {
				throw std::invalid_argument("db.index.vector.queryNodes queryVector argument must be a List of floats");
			}
			if (value.getList().empty()) {
				throw std::invalid_argument("db.index.vector.queryNodes requires a nonempty numeric query vector");
			}
			std::vector<float> query;
			query.reserve(value.getList().size());
			for (const PropertyValue &coordinate: value.getList()) {
				float converted = 0.0F;
				if (coordinate.getType() == PropertyType::DOUBLE) {
					const double source = std::get<double>(coordinate.getVariant());
					if (!std::isfinite(source) || source > std::numeric_limits<float>::max() ||
						source < -std::numeric_limits<float>::max()) {
						throw std::invalid_argument(
								"db.index.vector.queryNodes query has a non-finite or float-unrepresentable coordinate");
					}
					converted = static_cast<float>(source);
				} else if (coordinate.getType() == PropertyType::INTEGER) {
					converted = static_cast<float>(std::get<int64_t>(coordinate.getVariant()));
				} else {
					throw std::invalid_argument("db.index.vector.queryNodes requires numeric query coordinates");
				}
				if (!std::isfinite(converted)) { // ZYX_COV_EXCL_LINE: finite, float-representable inputs cannot convert to infinity.
					throw std::invalid_argument("db.index.vector.queryNodes query has a non-finite coordinate");
				}
				query.push_back(converted);
			}
			return query;
		}

		[[nodiscard]] uint64_t requireShortestPathNodeId(const PropertyValue &value, const char *argumentName) {
			if (value.getType() == PropertyType::INTEGER) {
				const int64_t parsed = std::get<int64_t>(value.getVariant());
				if (parsed > 0) {
					return static_cast<uint64_t>(parsed);
				}
			} else if (value.getType() == PropertyType::STRING) {
				const std::string &text = std::get<std::string>(value.getVariant());
				uint64_t parsed = 0U;
				const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
				if (error == std::errc{} && end == text.data() + text.size() && parsed != 0U) {
					return parsed;
				}
			}
			throw std::invalid_argument(std::string("algo.shortestPath requires a positive integer ") + argumentName);
		}

		[[nodiscard]] size_t requireShortestPathDepth(const PropertyValue &value) {
			int64_t parsed = 0;
			if (value.getType() == PropertyType::INTEGER) {
				parsed = std::get<int64_t>(value.getVariant());
			} else if (value.getType() == PropertyType::STRING) {
				const std::string &text = std::get<std::string>(value.getVariant());
				const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
				if (error != std::errc{} || end != text.data() + text.size()) {
					parsed = 0;
				}
			}
			if (parsed <= 0) {
				throw std::invalid_argument("algo.shortestPath requires a positive integer maxDepth");
			}
			return static_cast<size_t>(parsed);
		}

		[[nodiscard]] storage::LogicalGraphNode readProcedureNode(storage::LogicalGraphReadSnapshot &snapshot,
																						   uint64_t nodeId) {
			const storage::LogicalGraphNode node = snapshot.readNode(nodeId);
			validateNode(node);
			if (node.node_id != nodeId) {
				throw std::runtime_error("logical procedure backend resolved a mismatched Node identity");
			}
			return node;
		}

		[[nodiscard]] std::vector<storage::LogicalGraphNode>
		findShortestPath(storage::LogicalGraphReadSnapshot &snapshot, uint64_t startNodeId, uint64_t endNodeId,
						 size_t maximumDepth, const LogicalPlanReadRuntimeBounds &bounds, const QueryContext &context) {
			if (bounds.maximum_node_records == 0U || bounds.maximum_edges_per_node == 0U ||
				bounds.maximum_adjacency_page_records == 0U || bounds.maximum_property_entries == 0U ||
				bounds.maximum_rows == 0U) {
				throw std::invalid_argument("logical shortest-path procedure requires positive graph read bounds");
			}
			if (!snapshot.containsNode(startNodeId) || !snapshot.containsNode(endNodeId)) {
				return {};
			}
			const storage::LogicalGraphNode start = readProcedureNode(snapshot, startNodeId);
			if (startNodeId == endNodeId) {
				return {start};
			}
			if (bounds.maximum_node_records > std::numeric_limits<size_t>::max() / bounds.maximum_edges_per_node) {
				throw std::overflow_error("logical shortest-path procedure Edge bound overflowed");
			}
			const size_t maximumExploredEdges = bounds.maximum_node_records * bounds.maximum_edges_per_node;

			struct Parent {
				uint64_t node_id = 0U;
			};
			std::deque<uint64_t> frontier;
			std::unordered_map<uint64_t, size_t> depths;
			std::unordered_map<uint64_t, Parent> parents;
			frontier.push_back(startNodeId);
			depths.emplace(startNodeId, 0U);
			size_t exploredEdges = 0U;
			bool found = false;

			while (!frontier.empty() && !found) {
				context.checkGuard();
				const uint64_t currentNodeId = frontier.front();
				frontier.pop_front();
				const size_t depth = depths.at(currentNodeId);
				if (depth == maximumDepth) {
					continue;
				}
				size_t sourceEdges = 0U;
				std::unordered_set<uint64_t> bothDirectionEdges;
				const auto visitDirection = [&](storage::LogicalGraphReadDirection direction) {
					std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
					std::unordered_set<uint64_t> directionEdges;
					for (;;) {
						if (sourceEdges == bounds.maximum_edges_per_node || exploredEdges == maximumExploredEdges) {
							throw std::runtime_error("logical shortest-path procedure exceeded its Edge bound");
						}
						const size_t pageRecords = std::min(bounds.maximum_edges_per_node - sourceEdges,
																				 bounds.maximum_adjacency_page_records);
						const storage::LogicalGraphAdjacencyPage page =
								snapshot.scanAdjacentEdges(currentNodeId, direction, continuation, pageRecords);
						if (page.edges.size() > pageRecords || (page.edges.empty() && page.continuation.has_value()) ||
							(continuation.has_value() &&
							 (page.edges.empty() || page.edges.front().edge_id != continuation->next_edge_id))) {
							throw std::runtime_error("logical procedure backend returned an invalid shortest-path adjacency page");
						}
						for (const storage::LogicalGraphEdge &edge: page.edges) {
							context.checkGuard();
							validateEdge(edge);
							if (!directionEdges.insert(edge.edge_id).second) {
								throw std::runtime_error("logical procedure backend returned a duplicate shortest-path relationship");
							}
							const bool endpointMatches = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																? edge.source_node_id == currentNodeId
																: edge.target_node_id == currentNodeId;
							if (!endpointMatches) {
								throw std::runtime_error("logical procedure backend returned a mismatched shortest-path relationship");
							}
							++exploredEdges;
							++sourceEdges;
							if (!bothDirectionEdges.insert(edge.edge_id).second) {
								continue;
							}
							const uint64_t nextNodeId = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																? edge.target_node_id
																: edge.source_node_id;
							static_cast<void>(readProcedureNode(snapshot, nextNodeId));
							if (depths.contains(nextNodeId)) {
								continue;
							}
							if (depths.size() == bounds.maximum_node_records) {
								throw std::runtime_error("logical shortest-path procedure exceeded its Node bound");
							}
							depths.emplace(nextNodeId, depth + 1U);
							parents.emplace(nextNodeId, Parent{currentNodeId});
							if (nextNodeId == endNodeId) {
								found = true;
								return;
							}
							frontier.push_back(nextNodeId);
						}
						if (!page.continuation.has_value()) {
							return;
						}
						if (sourceEdges == bounds.maximum_edges_per_node ||
							page.continuation->previous_edge_id != page.edges.back().edge_id ||
							page.continuation->next_edge_id == 0U ||
							page.continuation->previous_edge_id == page.continuation->next_edge_id) {
							throw std::runtime_error("logical procedure backend returned an invalid shortest-path continuation");
						}
						continuation = page.continuation;
					}
				};
				visitDirection(storage::LogicalGraphReadDirection::LGRD_OUTGOING);
				if (!found) {
					visitDirection(storage::LogicalGraphReadDirection::LGRD_INCOMING);
				}
			}
			if (!found) {
				return {};
			}

			std::vector<uint64_t> reversed{endNodeId};
			while (reversed.back() != startNodeId) {
				const auto parent = parents.find(reversed.back());
				if (parent == parents.end() || parent->second.node_id == 0U) {
					throw std::runtime_error("logical shortest-path procedure has an incomplete parent chain");
				}
				reversed.push_back(parent->second.node_id);
				if (reversed.size() > bounds.maximum_rows) {
					throw std::runtime_error("logical shortest-path procedure exceeded its result-row bound");
				}
			}
			std::vector<storage::LogicalGraphNode> result;
			result.reserve(reversed.size());
			for (auto node = reversed.rbegin(); node != reversed.rend(); ++node) {
				result.push_back(readProcedureNode(snapshot, *node));
			}
			return result;
		}

		[[nodiscard]] std::optional<LogicalPlanReadRow> applyProjection(
				const logical::LogicalProject &projection, storage::LogicalGraphReadSnapshot &snapshot,
				const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context,
				const LogicalPlanReadRow &source, NodePropertyMaps &nodeProperties,
				std::unordered_set<std::string> *distinctRows) {
			if (projection.isDistinct() != (distinctRows != nullptr)) { // ZYX_COV_EXCL_LINE: caller derives this pointer directly from isDistinct().
				throw std::logic_error("logical Vector procedure projection DISTINCT state is inconsistent");
			}
			if (projection.getItems().empty()) {
				throw std::invalid_argument("logical Vector procedure Project has no items");
			}

			LogicalPlanReadRow result;
			result.values.reserve(projection.getItems().size());
			result.property_cache.reserve(projection.getItems().size());
			for (const logical::LogicalProjectItem &item: projection.getItems()) {
				context.checkGuard();
				if (item.alias.empty() || !item.expression) {
					throw std::logic_error("logical Vector procedure projection has an invalid output item");
				}
				LogicalPlanReadRow evaluationRow = source;
				LogicalPlanReadValue value = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
						snapshot, *item.expression, evaluationRow, readBounds, context);
				cacheResolvedNodeProperties(evaluationRow, nodeProperties);
				if (!result.values.emplace(item.alias, value).second) {
					throw std::logic_error("logical Vector procedure projection has duplicate output aliases");
				}
			}
			// An earlier item may have resolved Properties for a Node projected by a
			// later item. Attach every cache entry only after the full layer is known.
			attachCachedNodeProperties(result, nodeProperties);
			if (distinctRows != nullptr) {
				const std::string key = LogicalPlanReadRuntime::makeDistinctRowKey(
						result, projection.getOutputVariables());
				if (distinctRows->contains(key)) {
					return std::nullopt;
				}
				if (distinctRows->size() == readBounds.maximum_distinct_rows) {
					throw std::runtime_error("logical Vector procedure DISTINCT exceeded its row bound");
				}
				static_cast<void>(distinctRows->insert(key));
			}
			return result;
		}

		[[nodiscard]] bool applyFilter(const logical::LogicalFilter &filter,
				storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &readBounds,
				const QueryContext &context, LogicalPlanReadRow &row, NodePropertyMaps &nodeProperties) {
			if (!filter.getPredicate()) {
				throw std::invalid_argument("logical Vector procedure Filter has no predicate");
			}
			LogicalPlanReadRow evaluationRow = row;
			const LogicalPlanReadValue evaluated = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
					snapshot, *filter.getPredicate(), evaluationRow, readBounds, context);
			cacheResolvedNodeProperties(evaluationRow, nodeProperties);
			attachCachedNodeProperties(row, nodeProperties);
			const auto *predicate = std::get_if<PropertyValue>(&evaluated);
			if (predicate == nullptr) {
				throw std::runtime_error("logical Vector procedure Filter requires a scalar predicate");
			}
			if (predicate->getType() == PropertyType::NULL_TYPE) {
				return false;
			}
			if (predicate->getType() != PropertyType::BOOLEAN) {
				throw std::runtime_error("logical Vector procedure Filter requires a Boolean predicate");
			}
			return std::get<bool>(predicate->getVariant());
		}

		[[nodiscard]] ResultValue materializeResultValue(const LogicalPlanReadValue &value,
																		 storage::LogicalGraphReadSnapshot &snapshot,
																							 const LogicalPlanReadRuntimeBounds &readBounds,
																							 const QueryContext &context,
																							 NodePropertyMaps &nodeProperties) {
			if (const auto *property = std::get_if<PropertyValue>(&value)) {
				return *property;
			}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				auto properties = nodeProperties.find(node->node_id);
				if (properties == nodeProperties.end()) {
					properties = nodeProperties
								 .emplace(node->node_id,
											  materializeNodeProperties(snapshot, *node,
																		   readBounds.maximum_property_entries, context))
								 .first;
				}
				return LogicalNodeResult{node->node_id, node->labels, properties->second};
			}
			throw std::logic_error("logical Vector procedure result contains an unsupported Edge binding"); // ZYX_COV_EXCL_LINE: vector procedures construct only Node and scalar bindings.
		}

		struct BufferedProcedureRow {
			LogicalPlanReadRow row;
			NodePropertyMaps node_properties;
		};

		[[nodiscard]] std::vector<BufferedProcedureRow>
		makeBufferedShortestPathRows(const std::vector<storage::LogicalGraphNode> &path) {
			if (path.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("logical shortest-path procedure result cannot represent its row count");
			}
			std::vector<BufferedProcedureRow> rows;
			rows.reserve(path.size());
			for (size_t step = 0U; step < path.size(); ++step) {
				BufferedProcedureRow row;
				row.node_properties.reserve(1U);
				row.row.values.emplace("step", PropertyValue(static_cast<int64_t>(step)));
				row.row.values.emplace("node", path[step]);
				row.row.values.emplace("path_length", PropertyValue(static_cast<int64_t>(path.size())));
				rows.push_back(std::move(row));
			}
			return rows;
		}

		[[nodiscard]] std::vector<BufferedProcedureRow> makeBufferedVectorProcedureRows(
				const std::vector<storage::LogicalGraphVectorSearchHit> &hits,
				storage::LogicalGraphVectorSearchSnapshot &snapshot, const QueryContext &context) {
			std::vector<BufferedProcedureRow> rows;
			rows.reserve(hits.size());
			std::unordered_set<uint64_t> seenNodeIds;
			seenNodeIds.reserve(hits.size());
			uint64_t previousNodeId = 0U;
			float previousScore = -std::numeric_limits<float>::infinity();
			for (const storage::LogicalGraphVectorSearchHit &hit: hits) {
				context.checkGuard();
				if (hit.node_id == 0U || !std::isfinite(hit.score) ||
					(hit.score < previousScore || (hit.score == previousScore && hit.node_id <= previousNodeId)) ||
					!seenNodeIds.insert(hit.node_id).second) {
					throw std::runtime_error("logical Vector procedure backend returned unordered or invalid search hits");
				}
				const storage::LogicalGraphNode node = snapshot.readNode(hit.node_id);
				if (node.node_id != hit.node_id) {
					throw std::runtime_error("logical Vector procedure backend resolved a mismatched Node identity");
				}
				validateNode(node);
				BufferedProcedureRow buffered;
				buffered.node_properties.reserve(1U);
				buffered.row.values.emplace("node", node);
				buffered.row.values.emplace("score", PropertyValue(static_cast<double>(hit.score)));
				rows.push_back(std::move(buffered));
				previousNodeId = hit.node_id;
				previousScore = hit.score;
			}
			return rows;
		}

		[[nodiscard]] QueryResult executeBufferedProcedure(
				std::vector<BufferedProcedureRow> rows, storage::LogicalGraphReadSnapshot &snapshot,
				const std::vector<const logical::LogicalOperator *> &operations,
				const logical::LogicalOperator &root, const LogicalPlanReadRuntimeBounds &readBounds,
				const QueryContext &context) {
			QueryResult result;
			result.setColumns(root.getOutputVariables());

			for (const logical::LogicalOperator *operation: operations) {
				context.checkGuard();
				switch (operation->getType()) {
					case logical::LogicalOpType::LOP_PROJECT: {
						const auto &projection = dynamic_cast<const logical::LogicalProject &>(*operation);
						std::unordered_set<std::string> distinctRows;
						std::vector<BufferedProcedureRow> projectedRows;
						projectedRows.reserve(rows.size());
						for (BufferedProcedureRow &buffered: rows) {
							std::optional<LogicalPlanReadRow> projected = applyProjection(
									projection, snapshot, readBounds, context, buffered.row, buffered.node_properties,
									projection.isDistinct() ? &distinctRows : nullptr);
							if (projected.has_value()) {
								buffered.row = std::move(*projected);
								projectedRows.push_back(std::move(buffered));
							}
						}
						rows = std::move(projectedRows);
						break;
					}
					case logical::LogicalOpType::LOP_FILTER: {
						const auto &filter = dynamic_cast<const logical::LogicalFilter &>(*operation);
						std::vector<BufferedProcedureRow> filteredRows;
						filteredRows.reserve(rows.size());
						for (BufferedProcedureRow &buffered: rows) {
							if (applyFilter(filter, snapshot, readBounds, context, buffered.row,
													buffered.node_properties)) {
								filteredRows.push_back(std::move(buffered));
							}
						}
						rows = std::move(filteredRows);
						break;
					}
					case logical::LogicalOpType::LOP_SKIP: {
						const int64_t offset = dynamic_cast<const logical::LogicalSkip &>(*operation).getOffset();
						if (offset < 0) {
							throw std::invalid_argument("logical Vector procedure SKIP cannot be negative");
						}
						const size_t skip = static_cast<size_t>(std::min<uint64_t>(
								static_cast<uint64_t>(offset), static_cast<uint64_t>(rows.size())));
						rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(skip));
						break;
					}
					case logical::LogicalOpType::LOP_LIMIT: {
						const int64_t limit = dynamic_cast<const logical::LogicalLimit &>(*operation).getLimit();
						if (limit < 0) {
							throw std::invalid_argument("logical Vector procedure LIMIT cannot be negative");
						}
						const size_t maximum = static_cast<size_t>(std::min<uint64_t>(
								static_cast<uint64_t>(limit), static_cast<uint64_t>(rows.size())));
						rows.resize(maximum);
						break;
					}
					case logical::LogicalOpType::LOP_SORT: {
						const auto &sort = dynamic_cast<const logical::LogicalSort &>(*operation);
						const std::vector<logical::LogicalSortItem> &items = sort.getSortItems();
						if (items.empty()) {
							throw std::invalid_argument("logical Vector procedure Sort has no sort items");
						}
						struct DecoratedRow {
							BufferedProcedureRow row;
							std::vector<PropertyValue> keys;
						};
						std::vector<DecoratedRow> decoratedRows;
						decoratedRows.reserve(rows.size());
						for (BufferedProcedureRow &buffered: rows) {
							DecoratedRow decorated{std::move(buffered), {}};
							decorated.keys.reserve(items.size());
							for (const logical::LogicalSortItem &item: items) {
								if (!item.expression) {
									throw std::invalid_argument("logical Vector procedure Sort has an empty sort expression");
								}
								LogicalPlanReadRow evaluationRow = decorated.row.row;
								const LogicalPlanReadValue value = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
										snapshot, *item.expression, evaluationRow, readBounds, context);
								cacheResolvedNodeProperties(evaluationRow, decorated.row.node_properties);
								attachCachedNodeProperties(decorated.row.row, decorated.row.node_properties);
								const auto *key = std::get_if<PropertyValue>(&value);
								if (key == nullptr) {
									throw std::runtime_error("logical Vector procedure Sort requires scalar sort keys");
								}
								decorated.keys.push_back(*key);
							}
							decoratedRows.push_back(std::move(decorated));
						}
						std::stable_sort(decoratedRows.begin(), decoratedRows.end(),
								[&context, &items](const DecoratedRow &left, const DecoratedRow &right) {
									context.checkGuard();
									for (size_t index = 0U; index < items.size(); ++index) {
										if (left.keys[index] == right.keys[index]) {
											continue;
										}
										return items[index].ascending ? left.keys[index] < right.keys[index]
																  : left.keys[index] > right.keys[index];
									}
									return false;
								});
						rows.clear();
						rows.reserve(decoratedRows.size());
						for (DecoratedRow &decorated: decoratedRows) {
							rows.push_back(std::move(decorated.row));
						}
						break;
					}
					default: // ZYX_COV_EXCL_LINE: execute() admits only the five unary types dispatched above.
						throw std::logic_error("logical Vector procedure runtime received an unsupported operation");
				}
			}

			for (BufferedProcedureRow &buffered: rows) {
				QueryResult::Row output;
				output.reserve(buffered.row.values.size());
				for (const std::string &column: result.getColumns()) {
					const auto value = buffered.row.values.find(column);
					if (value == buffered.row.values.end() || // ZYX_COV_EXCL_LINE: output variables are derived from the row-producing operators.
						!output.emplace(column, materializeResultValue(value->second, snapshot, readBounds, context,
																										  buffered.node_properties)).second) {
						throw std::logic_error("logical Vector procedure projection did not bind a declared output column");
					}
				}
				result.addRow(std::move(output));
			}
			return result;
		}

		[[nodiscard]] QueryResult executeBufferedVectorProcedure(
				const std::vector<storage::LogicalGraphVectorSearchHit> &hits,
				storage::LogicalGraphVectorSearchSnapshot &snapshot,
				const std::vector<const logical::LogicalOperator *> &operations,
				const logical::LogicalOperator &root, const LogicalPlanReadRuntimeBounds &readBounds,
				const QueryContext &context) {
			return executeBufferedProcedure(makeBufferedVectorProcedureRows(hits, snapshot, context), snapshot,
					operations, root, readBounds, context);
		}

	} // namespace

	QueryResult LogicalPlanProcedureRuntime::execute(const logical::LogicalOperator &root,
											 const LogicalPlanReadRuntimeBounds &readBounds,
											 size_t maximumVectorMappingEntries,
											 size_t maximumRawVectorReads,
											 size_t vectorCandidateSearchBudget,
											 size_t vectorCandidateRefinementBudget,
											 size_t maximumVectorTrainingRecords,
											 size_t maximumVectorTrainingPages,
											 const QueryContext &context) const {
		const logical::LogicalOperator *current = &root;
		std::vector<const logical::LogicalOperator *> operations;
		while (current->getType() == logical::LogicalOpType::LOP_PROJECT ||
			   current->getType() == logical::LogicalOpType::LOP_FILTER ||
			   current->getType() == logical::LogicalOpType::LOP_SKIP ||
			   current->getType() == logical::LogicalOpType::LOP_LIMIT ||
			   current->getType() == logical::LogicalOpType::LOP_SORT) {
			if ((current->getType() == logical::LogicalOpType::LOP_PROJECT &&
				 dynamic_cast<const logical::LogicalProject *>(current) == nullptr) ||
				(current->getType() == logical::LogicalOpType::LOP_FILTER &&
				 dynamic_cast<const logical::LogicalFilter *>(current) == nullptr) ||
				(current->getType() == logical::LogicalOpType::LOP_SKIP &&
				 dynamic_cast<const logical::LogicalSkip *>(current) == nullptr) ||
				(current->getType() == logical::LogicalOpType::LOP_LIMIT &&
				 dynamic_cast<const logical::LogicalLimit *>(current) == nullptr) ||
				(current->getType() == logical::LogicalOpType::LOP_SORT &&
				 dynamic_cast<const logical::LogicalSort *>(current) == nullptr)) {
				throw std::logic_error("logical Vector procedure runtime received a malformed unary operator");
			}
			operations.push_back(current);
			current = requireOnlyChild(*current);
		}
		std::ranges::reverse(operations);
		if (current->getType() != logical::LogicalOpType::LOP_CALL_PROCEDURE) {
			throw std::logic_error("logical procedure runtime requires a CallProcedure root");
		}
		const auto *call = dynamic_cast<const logical::LogicalCallProcedure *>(current);
		if (call == nullptr) {
			throw std::logic_error("logical backend does not support this stored procedure");
		}
		if (call->getProcedureName() == "algo.shortestPath") {
			if (call->getArgs().size() < 2U || call->getArgs().size() > 3U) {
				throw std::invalid_argument("algo.shortestPath expects (startId, endId[, maxDepth])");
			}
			const uint64_t startNodeId = requireShortestPathNodeId(call->getArgs()[0], "startId");
			const uint64_t endNodeId = requireShortestPathNodeId(call->getArgs()[1], "endId");
			const size_t maximumDepth = call->getArgs().size() == 3U
									? requireShortestPathDepth(call->getArgs()[2])
									: 15U;
			context.checkGuard();
			std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot = read_backend_.openLogicalGraphReadSnapshot();
			if (!snapshot) {
				throw std::runtime_error("logical shortest-path procedure backend could not open a read snapshot");
			}
			const std::vector<storage::LogicalGraphNode> path =
					findShortestPath(*snapshot, startNodeId, endNodeId, maximumDepth, readBounds, context);
			if (!operations.empty()) {
				return executeBufferedProcedure(makeBufferedShortestPathRows(path), *snapshot, operations, root,
																readBounds, context);
			}
			QueryResult result;
			result.setColumns({"step", "node", "path_length"});
			if (path.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("logical shortest-path procedure result cannot represent its row count");
			}
			for (size_t step = 0U; step < path.size(); ++step) {
				context.checkGuard();
				NodePropertyMaps properties;
				properties.reserve(1U);
				QueryResult::Row row;
				row.emplace("step", PropertyValue(static_cast<int64_t>(step)));
				row.emplace("node", materializeResultValue(path[step], *snapshot, readBounds, context, properties));
				row.emplace("path_length", PropertyValue(static_cast<int64_t>(path.size())));
				result.addRow(std::move(row));
			}
			return result;
		}
		if (call->getProcedureName() == "db.index.vector.train") {
			if (call->getArgs().size() != 1U) {
				throw std::invalid_argument("logical Vector train requires one index-name argument");
			}
			if (vector_maintenance_backend_ == nullptr) {
				throw std::runtime_error("logical backend does not support Vector index training");
			}
			if (maximumVectorTrainingRecords == 0U || maximumVectorTrainingPages == 0U) {
				throw std::invalid_argument("logical Vector train requires positive record and page bounds");
			}
			context.checkGuard();
			const storage::LogicalGraphVectorTrainingResult trained = vector_maintenance_backend_->trainLogicalVector(
					{requireIndexName(call->getArgs().front()), maximumVectorTrainingRecords, maximumVectorTrainingPages});
			context.checkGuard();
			if (operations.empty()) {
				QueryResult result;
				result.setColumns({"status"});
				QueryResult::Row row;
				row.emplace("status", PropertyValue(trained.trained ? "Success" : "Skipped (Empty Data)"));
				result.addRow(std::move(row));
				return result;
			}
			if (vector_backend_ == nullptr) {
				throw std::runtime_error("logical backend does not support Vector index search snapshots");
			}
			// The procedure produced a scalar row, but projections and predicates use
			// the same bounded logical expression evaluator as vector searches.
			std::unique_ptr<storage::LogicalGraphVectorSearchSnapshot> snapshot =
					vector_backend_->openLogicalGraphVectorSearchSnapshot();
			if (!snapshot) {
				throw std::runtime_error("logical Vector procedure backend could not open a search snapshot");
			}
			BufferedProcedureRow source;
			source.row.values.emplace("status", PropertyValue(trained.trained ? "Success" : "Skipped (Empty Data)"));
			std::vector<BufferedProcedureRow> rows;
			rows.push_back(std::move(source));
			return executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context);
		}
		if (call->getProcedureName() != "db.index.vector.queryNodes") {
			// This is a valid user query whose native implementation is unavailable,
			// not a malformed logical plan or an execution invariant violation.
			throw std::runtime_error("logical backend does not support this stored procedure");
		}
		if (call->getArgs().size() != 3U) {
			throw std::invalid_argument("db.index.vector.queryNodes expects (indexName, topK, queryVector)");
		}
		if (readBounds.maximum_property_entries == 0U || maximumVectorMappingEntries == 0U ||
			maximumRawVectorReads == 0U || vectorCandidateSearchBudget == 0U ||
			vectorCandidateRefinementBudget == 0U || vectorCandidateSearchBudget > maximumVectorMappingEntries ||
			vectorCandidateRefinementBudget > maximumRawVectorReads) {
			throw std::invalid_argument("logical Vector procedure runtime requires positive read and search bounds");
		}
		if (vector_backend_ == nullptr) {
			throw std::runtime_error("logical backend does not support Vector index search");
		}

		storage::LogicalGraphVectorSearchRequest request;
		request.index_name = requireIndexName(call->getArgs()[0]);
		request.top_k = requireTopK(call->getArgs()[1]);
		request.query_vector = requireQueryVector(call->getArgs()[2]);
		request.maximum_mapping_entries = maximumVectorMappingEntries;
		request.maximum_raw_vector_reads = maximumRawVectorReads;
		request.candidate_search_budget = vectorCandidateSearchBudget;
		request.candidate_refinement_budget = vectorCandidateRefinementBudget;
		if (request.top_k > readBounds.maximum_rows) {
			throw std::invalid_argument("db.index.vector.queryNodes topK exceeds the logical row bound");
		}
		if (request.top_k == 0U) {
			QueryResult result;
			result.setColumns(operations.empty() ? std::vector<std::string>{"node", "score"}
															 : root.getOutputVariables());
			return result;
		}

		std::unique_ptr<storage::LogicalGraphVectorSearchSnapshot> snapshot =
				vector_backend_->openLogicalGraphVectorSearchSnapshot();
		if (!snapshot) {
			throw std::runtime_error("logical Vector procedure backend could not open a search snapshot");
		}
		const storage::LogicalGraphVectorSearchResult searchResult = snapshot->searchLogicalVector(request);
		const std::vector<storage::LogicalGraphVectorSearchHit> &hits = searchResult.hits;
		if (hits.size() > request.top_k) {
			throw std::runtime_error("logical Vector procedure backend exceeded its requested result bound");
		}
		if (std::ranges::any_of(operations, [](const logical::LogicalOperator *operation) {
				return operation->getType() == logical::LogicalOpType::LOP_SORT;
			})) {
			return executeBufferedVectorProcedure(hits, *snapshot, operations, root, readBounds, context);
		}

		QueryResult result;
		result.setColumns(operations.empty() ? std::vector<std::string>{"node", "score"} : root.getOutputVariables());
		std::vector<std::unordered_set<std::string>> distinctProjectionRows(operations.size());
		std::vector<uint64_t> operatorRowCounts(operations.size(), 0U);
		for (const logical::LogicalOperator *operation: operations) {
			if (operation->getType() == logical::LogicalOpType::LOP_SKIP &&
				dynamic_cast<const logical::LogicalSkip &>(*operation).getOffset() < 0) {
				throw std::invalid_argument("logical Vector procedure SKIP cannot be negative");
			}
			if (operation->getType() == logical::LogicalOpType::LOP_LIMIT &&
				dynamic_cast<const logical::LogicalLimit &>(*operation).getLimit() < 0) {
				throw std::invalid_argument("logical Vector procedure LIMIT cannot be negative");
			}
		}
		std::unordered_set<uint64_t> seenNodeIds;
		seenNodeIds.reserve(hits.size());
		uint64_t previousNodeId = 0U;
		float previousScore = -std::numeric_limits<float>::infinity();
		for (const storage::LogicalGraphVectorSearchHit &hit: hits) {
			context.checkGuard();
			if (hit.node_id == 0U || !std::isfinite(hit.score) ||
				(hit.score < previousScore || (hit.score == previousScore && hit.node_id <= previousNodeId)) ||
				!seenNodeIds.insert(hit.node_id).second) {
				throw std::runtime_error("logical Vector procedure backend returned unordered or invalid search hits");
			}
			const storage::LogicalGraphNode node = snapshot->readNode(hit.node_id);
			if (node.node_id != hit.node_id) {
				throw std::runtime_error("logical Vector procedure backend resolved a mismatched Node identity");
			}
			validateNode(node);
			// Hits have unique Node IDs. Keeping this cache per hit prevents a
			// scalar-only or high-topK query from retaining unrelated Property maps.
			NodePropertyMaps nodeProperties;
			nodeProperties.reserve(1U);
			LogicalPlanReadRow row;
			row.values.emplace("node", node);
			row.values.emplace("score", PropertyValue(static_cast<double>(hit.score)));
			previousNodeId = hit.node_id;
			previousScore = hit.score;
			bool keepRow = true;
			bool stopAfterCurrentRow = false;
			for (size_t index = 0U; index < operations.size(); ++index) {
				const logical::LogicalOperator &operation = *operations[index];
				if (operation.getType() == logical::LogicalOpType::LOP_PROJECT) {
					const auto &projection = dynamic_cast<const logical::LogicalProject &>(operation);
					std::optional<LogicalPlanReadRow> projected = applyProjection(
							projection, *snapshot, readBounds, context, row, nodeProperties,
							projection.isDistinct() ? &distinctProjectionRows[index] : nullptr);
					if (!projected.has_value()) {
						keepRow = false;
						break;
					}
					row = std::move(*projected);
					continue;
				}
				if (operation.getType() == logical::LogicalOpType::LOP_FILTER) {
					const auto &filter = dynamic_cast<const logical::LogicalFilter &>(operation);
					if (!applyFilter(filter, *snapshot, readBounds, context, row, nodeProperties)) {
						keepRow = false;
						break;
					}
					continue;
				}
				if (operation.getType() == logical::LogicalOpType::LOP_SKIP) {
					const uint64_t offset =
							static_cast<uint64_t>(dynamic_cast<const logical::LogicalSkip &>(operation).getOffset());
					if (operatorRowCounts[index] < offset) {
						++operatorRowCounts[index];
						keepRow = false;
						break;
					}
					continue;
				}
				const uint64_t maximum =
						static_cast<uint64_t>(dynamic_cast<const logical::LogicalLimit &>(operation).getLimit());
				if (maximum == 0U || operatorRowCounts[index] == maximum) {
					keepRow = false;
					stopAfterCurrentRow = true;
					break;
				}
				++operatorRowCounts[index];
				stopAfterCurrentRow = operatorRowCounts[index] == maximum;
			}
			if (!keepRow) {
				if (stopAfterCurrentRow) {
					break;
				}
				continue;
			}

			QueryResult::Row output;
			output.reserve(row.values.size());
			for (const std::string &column: result.getColumns()) {
				const auto value = row.values.find(column);
				if (value == row.values.end() || // ZYX_COV_EXCL_LINE: output variables are derived from the row-producing operators.
					!output.emplace(column, materializeResultValue(value->second, *snapshot, readBounds, context,
																									  nodeProperties)).second) {
					throw std::logic_error("logical Vector procedure projection did not bind a declared output column");
				}
			}
			result.addRow(std::move(output));
			if (stopAfterCurrentRow) {
				break;
			}
		}
		return result;
	}

} // namespace graph::query::execution
