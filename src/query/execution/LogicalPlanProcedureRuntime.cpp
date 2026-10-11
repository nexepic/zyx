/**
 * @file LogicalPlanProcedureRuntime.cpp
 * @brief Implementation of bounded logical stored procedures.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "graph/core/Types.hpp"
#include "graph/query/NativeProcedureManifest.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/gds/LogicalGraphAlgorithms.hpp"
#include "graph/query/gds/LogicalProjectionSpecCodec.hpp"
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
		// An options map cannot contain a key more than once. Keeping the accepted
		// names in one place makes the resource bound grow with the public contract.
		constexpr std::array<std::string_view, 5U> LEIDEN_OPTION_NAMES = {"maxIterations", "maxLevels", "resolution",
																		  "refinementThreshold", "concurrency"};

		[[nodiscard]] const logical::LogicalOperator *requireOnlyChild(const logical::LogicalOperator &op) {
			const std::vector<logical::LogicalOperator *> children = op.getChildren();
			if (children.size() != 1U || children.front() == nullptr) {
				throw std::logic_error("logical procedure projection requires exactly one child");
			}
			return children.front();
		}

		[[nodiscard]] PropertyMap materializeNodeProperties(const storage::LogicalGraphReadSnapshot &snapshot,
															const storage::LogicalGraphNode &node,
															size_t maximumEntries, const QueryContext &context) {
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
				if (binding ==
					row.values
							.end()) { // ZYX_COV_EXCL_LINE: evaluator writes cache entries only for aliases in this row.
					throw std::logic_error("logical Vector procedure Property cache has no matching binding");
				}
				const auto *node = std::get_if<storage::LogicalGraphNode>(&binding->second);
				if (node == nullptr) { // ZYX_COV_EXCL_LINE: only Node Property evaluation populates this cache.
					continue;
				}
				auto [cached, inserted] = nodeProperties.emplace(node->node_id, properties);
				if (!inserted &&
					cached->second !=
							properties) { // ZYX_COV_EXCL_LINE: one snapshot cannot resolve divergent Node Properties.
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
						throw std::invalid_argument("db.index.vector.queryNodes query has a non-finite or "
													"float-unrepresentable coordinate");
					}
					converted = static_cast<float>(source);
				} else if (coordinate.getType() == PropertyType::INTEGER) {
					converted = static_cast<float>(std::get<int64_t>(coordinate.getVariant()));
				} else {
					throw std::invalid_argument("db.index.vector.queryNodes requires numeric query coordinates");
				}
				if (!std::isfinite(converted)) { // ZYX_COV_EXCL_LINE: finite, float-representable inputs cannot convert
												 // to infinity.
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

		[[nodiscard]] std::string requireProjectionName(const std::vector<PropertyValue> &arguments,
														const std::string_view procedureName,
														const size_t expectedArguments) {
			if (arguments.size() != expectedArguments || arguments.empty() ||
				arguments.front().getType() != PropertyType::STRING) {
				throw std::invalid_argument(std::string(procedureName) + " requires a nonempty projection name");
			}
			const std::string &name = std::get<std::string>(arguments.front().getVariant());
			if (name.empty()) {
				throw std::invalid_argument(std::string(procedureName) + " requires a nonempty projection name");
			}
			return name;
		}

		[[nodiscard]] int64_t requireGdsPositiveInteger(const PropertyValue &value, const std::string_view context) {
			if (value.getType() != PropertyType::INTEGER) {
				throw std::invalid_argument(std::string(context) + " must be a positive integer");
			}
			const int64_t parsed = std::get<int64_t>(value.getVariant());
			if (parsed <= 0) {
				throw std::invalid_argument(std::string(context) + " must be a positive integer");
			}
			return parsed;
		}

		[[nodiscard]] size_t requireGdsNonNegativeInteger(const PropertyValue &value, const std::string_view context) {
			if (value.getType() != PropertyType::INTEGER) {
				throw std::invalid_argument(std::string(context) + " must be a non-negative integer");
			}
			const int64_t parsed = std::get<int64_t>(value.getVariant());
			if (parsed < 0) {
				throw std::invalid_argument(std::string(context) + " must be a non-negative integer");
			}
			return static_cast<size_t>(parsed);
		}

		[[nodiscard]] double requireGdsFiniteNumber(const PropertyValue &value, const std::string_view context) {
			double parsed = 0.0;
			if (value.getType() == PropertyType::INTEGER) {
				parsed = static_cast<double>(std::get<int64_t>(value.getVariant()));
			} else if (value.getType() == PropertyType::DOUBLE) {
				parsed = std::get<double>(value.getVariant());
			} else {
				throw std::invalid_argument(std::string(context) + " must be numeric");
			}
			if (!std::isfinite(parsed)) {
				throw std::invalid_argument(std::string(context) + " must be finite");
			}
			return parsed;
		}

		[[nodiscard]] int64_t checkedOutputNodeId(const uint64_t nodeId) {
			if (nodeId == 0U || nodeId > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("logical GDS result Node ID cannot be represented as a Cypher integer");
			}
			return static_cast<int64_t>(nodeId);
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
				const auto visitDirection = [&](storage::LogicalGraphReadDirection direction) {
					std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
					uint64_t previousEdgeId = 0U;
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
							throw std::runtime_error(
									"logical procedure backend returned an invalid shortest-path adjacency page");
						}
						for (const storage::LogicalGraphEdge &edge: page.edges) {
							context.checkGuard();
							validateEdge(edge);
							if (previousEdgeId != 0U && edge.edge_id <= previousEdgeId) {
								throw std::runtime_error(
										"logical procedure backend returned non-monotonic shortest-path relationship IDs");
							}
							previousEdgeId = edge.edge_id;
							const bool endpointMatches = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																 ? edge.source_node_id == currentNodeId
																 : edge.target_node_id == currentNodeId;
							if (!endpointMatches) {
								throw std::runtime_error(
										"logical procedure backend returned a mismatched shortest-path relationship");
							}
							++exploredEdges;
							++sourceEdges;
							// OUT is visited first, so only an incoming self-loop repeats an
							// already processed logical relationship.
							if (direction == storage::LogicalGraphReadDirection::LGRD_INCOMING &&
								edge.source_node_id == currentNodeId) {
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
							throw std::runtime_error(
									"logical procedure backend returned an invalid shortest-path continuation");
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

		[[nodiscard]] std::optional<LogicalPlanReadRow>
		applyProjection(const logical::LogicalProject &projection, storage::LogicalGraphReadSnapshot &snapshot,
						const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context,
						const LogicalPlanReadRow &source, NodePropertyMaps &nodeProperties,
						std::unordered_set<std::string> *distinctRows) {
			if (projection.isDistinct() !=
				(distinctRows !=
				 nullptr)) { // ZYX_COV_EXCL_LINE: caller derives this pointer directly from isDistinct().
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
				const std::string key =
						LogicalPlanReadRuntime::makeDistinctRowKey(result, projection.getOutputVariables());
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
									   storage::LogicalGraphReadSnapshot &snapshot,
									   const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context,
									   LogicalPlanReadRow &row, NodePropertyMaps &nodeProperties) {
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
					properties =
							nodeProperties
									.emplace(node->node_id,
											 materializeNodeProperties(snapshot, *node,
																	   readBounds.maximum_property_entries, context))
									.first;
				}
				return LogicalNodeResult{node->node_id, node->labels, properties->second};
			}
			throw std::logic_error(
					"logical Vector procedure result contains an unsupported Edge binding"); // ZYX_COV_EXCL_LINE:
																							 // vector procedures
																							 // construct only Node and
																							 // scalar bindings.
		}

		struct BufferedProcedureRow {
			LogicalPlanReadRow row;
			NodePropertyMaps node_properties;
		};

		void appendBoundedProcedureRow(std::vector<BufferedProcedureRow> &rows, BufferedProcedureRow row,
									  const size_t maximumRows, const std::string_view procedureName) {
			if (maximumRows == 0U) {
				throw std::invalid_argument("logical procedure runtime requires a positive result-row bound");
			}
			if (rows.size() == maximumRows) {
				throw std::runtime_error(std::string(procedureName) + " exceeded its configured result-row bound");
			}
			rows.push_back(std::move(row));
		}

		[[nodiscard]] PropertyValue toStringListValue(const std::vector<std::string> &values) {
			std::vector<PropertyValue> result;
			result.reserve(values.size());
			for (const std::string &value: values) {
				result.emplace_back(value);
			}
			return PropertyValue(std::move(result));
		}

		[[nodiscard]] PropertyValue
		toRelationshipListValue(const std::vector<gds::LogicalRelationshipProjection> &relationships) {
			std::vector<PropertyValue> result;
			result.reserve(relationships.size());
			for (const gds::LogicalRelationshipProjection &relationship: relationships) {
				PropertyValue::MapType entry;
				entry.emplace("type", PropertyValue(relationship.type));
				switch (relationship.orientation) {
					case gds::LogicalProjectionOrientation::LPO_NATURAL:
						entry.emplace("orientation", PropertyValue("NATURAL"));
						break;
					case gds::LogicalProjectionOrientation::LPO_REVERSE:
						entry.emplace("orientation", PropertyValue("REVERSE"));
						break;
					case gds::LogicalProjectionOrientation::LPO_UNDIRECTED:
						entry.emplace("orientation", PropertyValue("UNDIRECTED"));
						break;
				}
				// Relationship descriptors are public catalog data. Keep one canonical
				// map shape for every weight mode so bindings never have to infer a
				// field's existence from weightKind.
				switch (relationship.weight.kind) {
					case gds::LogicalProjectionWeightKind::LPWK_NONE:
						entry.emplace("weightKind", PropertyValue("NONE"));
						break;
					case gds::LogicalProjectionWeightKind::LPWK_CONSTANT:
						entry.emplace("weightKind", PropertyValue("CONSTANT"));
						break;
					case gds::LogicalProjectionWeightKind::LPWK_PROPERTY:
						entry.emplace("weightKind", PropertyValue("PROPERTY"));
						break;
				}
				entry.emplace("weightProperty", PropertyValue(relationship.weight.property_name));
				entry.emplace("constantWeight", PropertyValue(relationship.weight.constant_weight));
				entry.emplace("defaultWeight", PropertyValue(relationship.weight.default_weight));
				result.emplace_back(PropertyValue(std::move(entry)));
			}
			return PropertyValue(std::move(result));
		}

		[[nodiscard]] BufferedProcedureRow
		makeProjectionDescriptorRow(const gds::LogicalProjectionDescriptor &descriptor) {
			if (descriptor.node_count > static_cast<size_t>(std::numeric_limits<int64_t>::max()) ||
				descriptor.edge_count > static_cast<size_t>(std::numeric_limits<int64_t>::max()) ||
				descriptor.estimated_memory_bytes > static_cast<size_t>(std::numeric_limits<int64_t>::max()) ||
				descriptor.source_revision > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
				descriptor.current_revision > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("logical projection descriptor cannot represent an output count");
			}
			BufferedProcedureRow result;
			result.row.values.emplace("name", PropertyValue(descriptor.name));
			result.row.values.emplace("nodeCount", PropertyValue(static_cast<int64_t>(descriptor.node_count)));
			result.row.values.emplace("edgeCount", PropertyValue(static_cast<int64_t>(descriptor.edge_count)));
			result.row.values.emplace("isWeighted", PropertyValue(descriptor.weighted));
			result.row.values.emplace("createdAtMillis", PropertyValue(descriptor.created_at_epoch_millis));
			result.row.values.emplace("buildMillis", PropertyValue(descriptor.build_millis));
			result.row.values.emplace("memoryBytes",
									  PropertyValue(static_cast<int64_t>(descriptor.estimated_memory_bytes)));
			result.row.values.emplace("hasCsr", PropertyValue(descriptor.has_csr));
			result.row.values.emplace("csrMemoryBytes",
									  PropertyValue(static_cast<int64_t>(descriptor.csr_memory_bytes)));
			result.row.values.emplace("stale", PropertyValue(descriptor.stale));
			result.row.values.emplace("sourceRevision",
									  PropertyValue(static_cast<int64_t>(descriptor.source_revision)));
			result.row.values.emplace("currentRevision",
									  PropertyValue(static_cast<int64_t>(descriptor.current_revision)));
			result.row.values.emplace("nodeLabels", toStringListValue(descriptor.node_labels));
			result.row.values.emplace("relationships", toRelationshipListValue(descriptor.relationships));
			return result;
		}

		[[nodiscard]] QueryResult makeProcedureResult(std::vector<BufferedProcedureRow> rows,
											  const std::vector<std::string> &columns,
											  const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context) {
			if (rows.size() > readBounds.maximum_rows) {
				throw std::runtime_error("logical procedure exceeded its configured result-row bound");
			}
			QueryResult result;
			result.setColumns(columns);
			for (BufferedProcedureRow &buffered: rows) {
				context.checkGuard();
				QueryResult::Row row;
				row.reserve(columns.size());
				for (const std::string &column: columns) {
					const auto found = buffered.row.values.find(column);
					if (found == buffered.row.values.end()) {
						throw std::logic_error("logical projection procedure did not bind its declared output column");
					}
					const auto *scalar = std::get_if<PropertyValue>(&found->second);
					if (scalar == nullptr) {
						throw std::logic_error("logical projection procedure emitted a non-scalar catalog value");
					}
					row.emplace(column, *scalar);
				}
				result.addRow(std::move(row));
			}
			return result;
		}

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

		[[nodiscard]] std::vector<BufferedProcedureRow>
		makeBufferedVectorProcedureRows(const std::vector<storage::LogicalGraphVectorSearchHit> &hits,
										storage::LogicalGraphVectorSearchSnapshot &snapshot,
										const QueryContext &context) {
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
					throw std::runtime_error(
							"logical Vector procedure backend returned unordered or invalid search hits");
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

		[[nodiscard]] QueryResult
		executeBufferedProcedure(std::vector<BufferedProcedureRow> rows, storage::LogicalGraphReadSnapshot &snapshot,
								 const std::vector<const logical::LogicalOperator *> &operations,
								 const logical::LogicalOperator &root, const LogicalPlanReadRuntimeBounds &readBounds,
								 const QueryContext &context) {
			if (rows.size() > readBounds.maximum_rows) {
				throw std::runtime_error("logical procedure exceeded its configured result-row bound");
			}
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
						const size_t skip = static_cast<size_t>(
								std::min<uint64_t>(static_cast<uint64_t>(offset), static_cast<uint64_t>(rows.size())));
						rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(skip));
						break;
					}
					case logical::LogicalOpType::LOP_LIMIT: {
						const int64_t limit = dynamic_cast<const logical::LogicalLimit &>(*operation).getLimit();
						if (limit < 0) {
							throw std::invalid_argument("logical Vector procedure LIMIT cannot be negative");
						}
						const size_t maximum = static_cast<size_t>(
								std::min<uint64_t>(static_cast<uint64_t>(limit), static_cast<uint64_t>(rows.size())));
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
									throw std::invalid_argument(
											"logical Vector procedure Sort has an empty sort expression");
								}
								LogicalPlanReadRow evaluationRow = decorated.row.row;
								const LogicalPlanReadValue value =
										LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
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
				context.checkGuard();
				QueryResult::Row output;
				output.reserve(buffered.row.values.size());
				for (const std::string &column: result.getColumns()) {
					const auto value = buffered.row.values.find(column);
					if (value == buffered.row.values.end() || // ZYX_COV_EXCL_LINE: output variables are derived from
															  // the row-producing operators.
						!output.emplace(column, materializeResultValue(value->second, snapshot, readBounds, context,
																	   buffered.node_properties))
								 .second) {
						throw std::logic_error(
								"logical Vector procedure projection did not bind a declared output column");
					}
				}
				result.addRow(std::move(output));
			}
			return result;
		}

		[[nodiscard]] QueryResult
		executeBufferedVectorProcedure(const std::vector<storage::LogicalGraphVectorSearchHit> &hits,
									   storage::LogicalGraphVectorSearchSnapshot &snapshot,
									   const std::vector<const logical::LogicalOperator *> &operations,
									   const logical::LogicalOperator &root,
									   const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context) {
			return executeBufferedProcedure(makeBufferedVectorProcedureRows(hits, snapshot, context), snapshot,
											operations, root, readBounds, context);
		}

	} // namespace

	bool LogicalPlanProcedureRuntime::supportsNativeProcedure(const std::string_view name) noexcept {
		const NativeProcedureManifestEntry *entry = findNativeProcedureManifestEntry(name);
		if (entry == nullptr) {
			return false;
		}
		switch (entry->kind) {
			case NativeProcedureKind::NPK_SHORTEST_PATH:
			case NativeProcedureKind::NPK_VECTOR_QUERY_NODES:
			case NativeProcedureKind::NPK_VECTOR_TRAIN:
			case NativeProcedureKind::NPK_GDS_GRAPH_PROJECT:
			case NativeProcedureKind::NPK_GDS_GRAPH_DROP:
			case NativeProcedureKind::NPK_GDS_GRAPH_DROP_ALL:
			case NativeProcedureKind::NPK_GDS_GRAPH_EXISTS:
			case NativeProcedureKind::NPK_GDS_GRAPH_LIST:
			case NativeProcedureKind::NPK_GDS_GRAPH_SCHEMA:
			case NativeProcedureKind::NPK_GDS_PAGE_RANK:
			case NativeProcedureKind::NPK_GDS_WCC:
			case NativeProcedureKind::NPK_GDS_BETWEENNESS:
			case NativeProcedureKind::NPK_GDS_CLOSENESS:
			case NativeProcedureKind::NPK_GDS_LEIDEN:
			case NativeProcedureKind::NPK_GDS_DIJKSTRA:
			case NativeProcedureKind::NPK_DBMS_LIST_CONFIG:
			case NativeProcedureKind::NPK_DBMS_GET_CONFIG:
			case NativeProcedureKind::NPK_DBMS_SET_CONFIG:
			case NativeProcedureKind::NPK_DBMS_SHOW_STATS:
			case NativeProcedureKind::NPK_DBMS_RESET_STATS:
			case NativeProcedureKind::NPK_DBMS_LIST_SLOW_QUERIES:
			case NativeProcedureKind::NPK_DBMS_CLEAR_SLOW_QUERIES:
				return true;
		}
		return false;
	}

	QueryResult LogicalPlanProcedureRuntime::execute(
			const logical::LogicalOperator &root, const LogicalPlanReadRuntimeBounds &readBounds,
			size_t maximumVectorMappingEntries, size_t maximumRawVectorReads, size_t vectorCandidateSearchBudget,
			size_t vectorCandidateRefinementBudget, size_t maximumVectorTrainingRecords,
			size_t maximumVectorTrainingPages, const size_t maximumGdsAlgorithmEdges,
			const size_t maximumGdsAlgorithmIterations, const size_t maximumGdsAlgorithmWorkUnits,
			const QueryContext &context, const LogicalProcedureExecutionScope scope) const {
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
			throw std::logic_error("logical procedure runtime received a malformed CallProcedure operator");
		}
		const std::vector<PropertyValue> arguments = call->resolveArguments(context.parameters);
		if (!supportsNativeProcedure(call->getProcedureName())) {
			// The parser accepted a valid CALL, but this native backend has no handler
			// for it. Keep that public capability error distinct from a malformed plan.
			throw std::runtime_error("logical backend does not support this stored procedure");
		}
		const NativeProcedureManifestEntry *procedure = findNativeProcedureManifestEntry(call->getProcedureName());
		if (procedure == nullptr) { // ZYX_COV_EXCL_LINE: supportsNativeProcedure above proves manifest membership.
			throw std::logic_error("logical procedure manifest lookup became inconsistent");
		}
		if (scope == LogicalProcedureExecutionScope::LPES_TRANSACTION_OVERLAY) {
			switch (nativeProcedureTransactionMode(procedure->kind)) {
				case NativeProcedureTransactionMode::NPTM_OVERLAY_SNAPSHOT_READ:
					break;
				case NativeProcedureTransactionMode::NPTM_TRANSACTION_LOCAL_CATALOG:
					if (projection_catalog_ == nullptr) {
						throw std::runtime_error("stored procedure requires a transaction-local projection catalog");
					}
					break;
				case NativeProcedureTransactionMode::NPTM_AUTOCOMMIT_ONLY:
					throw std::runtime_error(
							"stored procedure is autocommit-only because it has no transactional overlay semantics");
			}
		}
		if (readBounds.maximum_rows == 0U) {
			throw std::invalid_argument("logical procedure runtime requires a positive result-row bound");
		}
		// This is the shared backend-acquisition boundary for every manifest entry.
		// It must not rely on periodic sampling: an already expired query may not
		// even open a snapshot, maintenance operation, or administration service.
		context.checkGuardNow();
		if (procedure->kind == NativeProcedureKind::NPK_DBMS_LIST_CONFIG ||
			procedure->kind == NativeProcedureKind::NPK_DBMS_GET_CONFIG ||
			procedure->kind == NativeProcedureKind::NPK_DBMS_SET_CONFIG ||
			procedure->kind == NativeProcedureKind::NPK_DBMS_SHOW_STATS ||
			procedure->kind == NativeProcedureKind::NPK_DBMS_RESET_STATS ||
			procedure->kind == NativeProcedureKind::NPK_DBMS_LIST_SLOW_QUERIES ||
			procedure->kind == NativeProcedureKind::NPK_DBMS_CLEAR_SLOW_QUERIES) {
			if (configuration_service_ == nullptr) {
				throw std::runtime_error("logical backend does not provide native administration services");
			}
			const auto requireConfigurationKey = [](const PropertyValue &value, const std::string_view procedureName) {
				if (value.getType() != PropertyType::STRING || std::get<std::string>(value.getVariant()).empty()) {
					throw std::invalid_argument(std::string(procedureName) +
												" requires a nonempty String configuration key");
				}
				return std::get<std::string>(value.getVariant());
			};
			std::vector<BufferedProcedureRow> rows;
			std::vector<std::string> defaultColumns;
			std::optional<std::pair<std::string, PropertyValue>> deferredConfigurationUpdate;
			bool deferredStatisticsReset = false;
			bool deferredSlowQueryClear = false;
			switch (procedure->kind) {
				case NativeProcedureKind::NPK_DBMS_LIST_CONFIG: {
					if (!arguments.empty()) {
						throw std::invalid_argument("dbms.listConfig accepts no arguments");
					}
					for (const storage::LogicalGraphConfiguration &configuration: configuration_service_->list()) {
						context.checkGuard();
						BufferedProcedureRow row;
						row.row.values.emplace("key", PropertyValue(configuration.key));
						row.row.values.emplace("value", configuration.value);
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "dbms.listConfig");
					}
					defaultColumns = {"key", "value"};
					break;
				}
				case NativeProcedureKind::NPK_DBMS_GET_CONFIG: {
					if (arguments.size() != 1U) {
						throw std::invalid_argument("dbms.getConfig expects one configuration key");
					}
					const std::string key = requireConfigurationKey(arguments.front(), "dbms.getConfig");
					const storage::LogicalGraphConfiguration configuration = configuration_service_->get(key);
					BufferedProcedureRow row;
					row.row.values.emplace("key", PropertyValue(configuration.key));
					row.row.values.emplace("value", configuration.value);
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "dbms.getConfig");
					defaultColumns = {"key", "value"};
					break;
				}
				case NativeProcedureKind::NPK_DBMS_SET_CONFIG: {
					if (arguments.size() != 2U) {
						throw std::invalid_argument("dbms.setConfig expects (key, value)");
					}
					const std::string key = requireConfigurationKey(arguments.front(), "dbms.setConfig");
					deferredConfigurationUpdate.emplace(key, arguments[1]);
					BufferedProcedureRow row;
					row.row.values.emplace("key", PropertyValue(key));
					row.row.values.emplace("value", arguments[1]);
					row.row.values.emplace("status", PropertyValue("OK"));
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "dbms.setConfig");
					defaultColumns = {"key", "value", "status"};
					break;
				}
				case NativeProcedureKind::NPK_DBMS_SHOW_STATS:
					if (!arguments.empty()) {
						throw std::invalid_argument("dbms.showStats accepts no arguments");
					}
					for (const storage::LogicalGraphRuntimeStatistic &statistic: configuration_service_->statistics()) {
						context.checkGuard();
						BufferedProcedureRow row;
						row.row.values.emplace("category", PropertyValue(statistic.category));
						row.row.values.emplace("metric", PropertyValue(statistic.metric));
						row.row.values.emplace("value", statistic.value);
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "dbms.showStats");
					}
					defaultColumns = {"category", "metric", "value"};
					break;
				case NativeProcedureKind::NPK_DBMS_RESET_STATS: {
					if (!arguments.empty()) {
						throw std::invalid_argument("dbms.resetStats accepts no arguments");
					}
					deferredStatisticsReset = true;
					BufferedProcedureRow row;
					row.row.values.emplace("result", PropertyValue("Statistics reset"));
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "dbms.resetStats");
					defaultColumns = {"result"};
					break;
				}
				case NativeProcedureKind::NPK_DBMS_LIST_SLOW_QUERIES:
					if (!arguments.empty()) {
						throw std::invalid_argument("dbms.listSlowQueries accepts no arguments");
					}
					for (const NativeSlowQueryRecord &record: configuration_service_->slowQueries()) {
						context.checkGuard();
						if (record.sequence > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
							record.elapsed_ms > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
							throw std::runtime_error("native slow-query recorder returned an unrepresentable value");
						}
						BufferedProcedureRow row;
						row.row.values.emplace("sequence", PropertyValue(static_cast<int64_t>(record.sequence)));
						row.row.values.emplace("logicalPlan", PropertyValue(record.logical_plan_type));
						row.row.values.emplace("elapsedMs", PropertyValue(static_cast<int64_t>(record.elapsed_ms)));
						row.row.values.emplace("succeeded", PropertyValue(record.succeeded));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows,
													  "dbms.listSlowQueries");
					}
					defaultColumns = {"sequence", "logicalPlan", "elapsedMs", "succeeded"};
					break;
				case NativeProcedureKind::NPK_DBMS_CLEAR_SLOW_QUERIES: {
					if (!arguments.empty()) {
						throw std::invalid_argument("dbms.clearSlowQueries accepts no arguments");
					}
					deferredSlowQueryClear = true;
					BufferedProcedureRow row;
					row.row.values.emplace("result", PropertyValue("Slow query records cleared"));
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows,
													  "dbms.clearSlowQueries");
					defaultColumns = {"result"};
					break;
				}
				default:
					throw std::logic_error("logical administration procedure dispatch became inconsistent");
			}
			const bool hasDeferredAdministrationMutation = deferredConfigurationUpdate.has_value() ||
																				 deferredStatisticsReset || deferredSlowQueryClear;
			if (hasDeferredAdministrationMutation) {
				// Fully materialize the result pipeline before the administrative write.
				// A failing YIELD/WHERE/RETURN must not leave a durable configuration or
				// observability mutation behind after the client observes an error.
				QueryResult result;
				if (operations.empty()) {
					result = makeProcedureResult(rows, defaultColumns, readBounds, context);
				} else {
					context.checkGuard();
					std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot =
							read_backend_.openLogicalGraphReadSnapshot();
					if (!snapshot) {
						throw std::runtime_error("logical administration procedure backend could not open a read snapshot");
					}
					result = executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context);
				}
				context.checkGuardNow();
				if (deferredConfigurationUpdate.has_value()) {
					configuration_service_->set(deferredConfigurationUpdate->first, deferredConfigurationUpdate->second);
				} else if (deferredStatisticsReset) {
					configuration_service_->resetStatistics();
				} else {
					configuration_service_->resetSlowQueries();
				}
				return result;
			}
			if (operations.empty()) {
				return makeProcedureResult(std::move(rows), defaultColumns, readBounds, context);
			}
			context.checkGuard();
			std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot = read_backend_.openLogicalGraphReadSnapshot();
			if (!snapshot) {
				throw std::runtime_error("logical administration procedure backend could not open a read snapshot");
			}
			return executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context);
		}
		if (procedure->kind == NativeProcedureKind::NPK_GDS_GRAPH_PROJECT ||
			procedure->kind == NativeProcedureKind::NPK_GDS_GRAPH_DROP ||
			procedure->kind == NativeProcedureKind::NPK_GDS_GRAPH_DROP_ALL ||
			procedure->kind == NativeProcedureKind::NPK_GDS_GRAPH_EXISTS ||
			procedure->kind == NativeProcedureKind::NPK_GDS_GRAPH_LIST ||
			procedure->kind == NativeProcedureKind::NPK_GDS_GRAPH_SCHEMA ||
			procedure->kind == NativeProcedureKind::NPK_GDS_PAGE_RANK ||
			procedure->kind == NativeProcedureKind::NPK_GDS_WCC ||
			procedure->kind == NativeProcedureKind::NPK_GDS_BETWEENNESS ||
			procedure->kind == NativeProcedureKind::NPK_GDS_CLOSENESS ||
			procedure->kind == NativeProcedureKind::NPK_GDS_LEIDEN ||
			procedure->kind == NativeProcedureKind::NPK_GDS_DIJKSTRA) {
			if (projection_catalog_ == nullptr) {
				throw std::runtime_error("logical backend does not provide a projection catalog");
			}
			if (readBounds.maximum_node_records == 0U || readBounds.maximum_metadata_pages_per_node_page == 0U ||
				readBounds.maximum_edges_per_node == 0U || readBounds.maximum_rows == 0U) {
				throw std::invalid_argument("logical projection procedures require positive graph read bounds");
			}
			context.checkGuard();
			std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot = read_backend_.openLogicalGraphReadSnapshot();
			if (!snapshot) {
				throw std::runtime_error("logical projection procedure backend could not open a read snapshot");
			}

			std::vector<BufferedProcedureRow> rows;
			std::vector<std::string> defaultColumns;
			std::unique_ptr<gds::PreparedLogicalProjectionCatalogMutation> deferredCatalogMutation;
			switch (procedure->kind) {
				case NativeProcedureKind::NPK_GDS_GRAPH_PROJECT: {
					const gds::LogicalGraphProjectionSpec spec =
							gds::LogicalProjectionSpecCodec::decodeGraphProjectArguments(arguments);
					gds::LogicalProjectionCatalogCreatePreparation preparation = projection_catalog_->prepareCreate(
							spec, *snapshot,
							{.maximum_nodes = readBounds.maximum_node_records,
							 .maximum_metadata_pages = readBounds.maximum_metadata_pages_per_node_page,
							 .maximum_edges_per_node = readBounds.maximum_edges_per_node},
							[&context] { context.checkGuard(); });
					if (!preparation.mutation) {
						throw std::logic_error("logical projection catalog returned no prepared creation operation");
					}
					appendBoundedProcedureRow(rows, makeProjectionDescriptorRow(preparation.descriptor), readBounds.maximum_rows,
													  "gds.graph.project");
					deferredCatalogMutation = std::move(preparation.mutation);
					defaultColumns = {"name", "nodeCount", "edgeCount"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_GRAPH_DROP: {
					const std::string name = requireProjectionName(arguments, "gds.graph.drop", 1U);
					deferredCatalogMutation = projection_catalog_->prepareDrop(name);
					if (!deferredCatalogMutation) {
						throw std::out_of_range("logical projection catalog does not contain this name");
					}
					BufferedProcedureRow row;
					row.row.values.emplace("name", PropertyValue(name));
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.graph.drop");
					defaultColumns = {"name"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_GRAPH_DROP_ALL: {
					if (!arguments.empty()) {
						throw std::invalid_argument("gds.graph.dropAll accepts no arguments");
					}
					gds::LogicalProjectionCatalogDropAllPreparation preparation = projection_catalog_->prepareDropAll();
					if (!preparation.mutation) {
						throw std::logic_error("logical projection catalog returned no prepared clear operation");
					}
					const size_t dropped = preparation.dropped_count;
					if (dropped > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
						throw std::overflow_error("logical projection catalog drop count cannot be represented");
					}
					BufferedProcedureRow row;
					row.row.values.emplace("droppedCount", PropertyValue(static_cast<int64_t>(dropped)));
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.graph.dropAll");
					deferredCatalogMutation = std::move(preparation.mutation);
					defaultColumns = {"droppedCount"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_GRAPH_EXISTS: {
					const std::string name = requireProjectionName(arguments, "gds.graph.exists", 1U);
					BufferedProcedureRow row;
					row.row.values.emplace("name", PropertyValue(name));
					row.row.values.emplace("exists", PropertyValue(projection_catalog_->exists(name)));
					appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.graph.exists");
					defaultColumns = {"name", "exists"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_GRAPH_LIST: {
					if (arguments.size() > 1U) {
						throw std::invalid_argument("gds.graph.list accepts zero or one projection name");
					}
					if (arguments.empty()) {
						for (const gds::LogicalProjectionDescriptor &descriptor: projection_catalog_->list(
									 *snapshot, readBounds.maximum_rows, [&context] { context.checkGuard(); })) {
							context.checkGuard();
							appendBoundedProcedureRow(rows, makeProjectionDescriptorRow(descriptor), readBounds.maximum_rows,
													  "gds.graph.list");
						}
					} else {
						const std::string name = requireProjectionName(arguments, "gds.graph.list", 1U);
						// Listing one name is a filtered enumeration: a missing entry is an
						// empty result, while gds.graph.schema remains the strict lookup API.
						try {
							appendBoundedProcedureRow(rows, makeProjectionDescriptorRow(projection_catalog_->describe(name, *snapshot)),
													  readBounds.maximum_rows, "gds.graph.list");
						} catch (const std::out_of_range &) {
							// A concurrent drop is indistinguishable from a missing filtered
							// entry and therefore has the same empty-list result.
						}
					}
					defaultColumns = {"name",			"nodeCount",	   "edgeCount",
									  "isWeighted",		"createdAtMillis", "buildMillis",
									  "memoryBytes",	"hasCsr",		   "csrMemoryBytes",
									  "sourceRevision", "currentRevision", "stale",
									  "nodeLabels",		"relationships"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_GRAPH_SCHEMA: {
					const std::string name = requireProjectionName(arguments, "gds.graph.schema", 1U);
					appendBoundedProcedureRow(rows, makeProjectionDescriptorRow(projection_catalog_->describe(name, *snapshot)),
												  readBounds.maximum_rows, "gds.graph.schema");
					defaultColumns = {"name", "nodeLabels", "relationships", "stale"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_PAGE_RANK: {
					if (arguments.empty() || arguments.size() > 3U) {
						throw std::invalid_argument(
								"gds.pageRank.stream expects (graphName[, maxIterations[, dampingFactor]])");
					}
					gds::LogicalPageRankOptions options;
					if (arguments.size() >= 2U) {
						options.maximum_iterations = static_cast<size_t>(
								requireGdsPositiveInteger(arguments[1], "gds.pageRank.stream maxIterations"));
					}
					if (arguments.size() == 3U) {
						options.damping_factor =
								requireGdsFiniteNumber(arguments[2], "gds.pageRank.stream dampingFactor");
					}
					const std::string name = requireProjectionName(arguments, "gds.pageRank.stream", arguments.size());
					const std::vector<gds::LogicalGdsNodeScore> scores =
							gds::LogicalGraphAlgorithms::pageRank(*projection_catalog_->get(name), options,
																  {.maximum_nodes = readBounds.maximum_node_records,
																   .maximum_edges = maximumGdsAlgorithmEdges,
																   .maximum_iterations = maximumGdsAlgorithmIterations,
																   .maximum_work_units = maximumGdsAlgorithmWorkUnits},
																  [&context] { context.checkGuard(); });
					rows.reserve(scores.size());
					for (const gds::LogicalGdsNodeScore &score: scores) {
						context.checkGuard();
						BufferedProcedureRow row;
						row.row.values.emplace("nodeId", PropertyValue(checkedOutputNodeId(score.node_id)));
						row.row.values.emplace("score", PropertyValue(score.score));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.pageRank.stream");
					}
					defaultColumns = {"nodeId", "score"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_WCC: {
					const std::string name = requireProjectionName(arguments, "gds.wcc.stream", 1U);
					const std::vector<gds::LogicalGdsNodeComponent> components =
							gds::LogicalGraphAlgorithms::weaklyConnectedComponents(
									*projection_catalog_->get(name),
									{.maximum_nodes = readBounds.maximum_node_records,
									 .maximum_edges = maximumGdsAlgorithmEdges,
									 .maximum_iterations = maximumGdsAlgorithmIterations,
									 .maximum_work_units = maximumGdsAlgorithmWorkUnits},
									[&context] { context.checkGuard(); });
					rows.reserve(components.size());
					for (const gds::LogicalGdsNodeComponent &component: components) {
						context.checkGuard();
						BufferedProcedureRow row;
						row.row.values.emplace("nodeId", PropertyValue(checkedOutputNodeId(component.node_id)));
						row.row.values.emplace("componentId",
											   PropertyValue(checkedOutputNodeId(component.component_id)));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.wcc.stream");
					}
					defaultColumns = {"nodeId", "componentId"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_BETWEENNESS: {
					if (arguments.empty() || arguments.size() > 2U) {
						throw std::invalid_argument("gds.betweenness.stream expects (graphName[, samplingSize])");
					}
					gds::LogicalBetweennessOptions options;
					if (arguments.size() == 2U) {
						options.sampling_size =
								requireGdsNonNegativeInteger(arguments[1], "gds.betweenness.stream samplingSize");
					}
					const std::string name =
							requireProjectionName(arguments, "gds.betweenness.stream", arguments.size());
					const std::vector<gds::LogicalGdsNodeScore> scores =
							gds::LogicalGraphAlgorithms::betweennessCentrality(
									*projection_catalog_->get(name), options,
									{.maximum_nodes = readBounds.maximum_node_records,
									 .maximum_edges = maximumGdsAlgorithmEdges,
									 .maximum_iterations = maximumGdsAlgorithmIterations,
									 .maximum_work_units = maximumGdsAlgorithmWorkUnits},
									[&context] { context.checkGuard(); });
					rows.reserve(scores.size());
					for (const gds::LogicalGdsNodeScore &score: scores) {
						context.checkGuard();
						BufferedProcedureRow row;
						row.row.values.emplace("nodeId", PropertyValue(checkedOutputNodeId(score.node_id)));
						row.row.values.emplace("score", PropertyValue(score.score));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows,
													  "gds.betweenness.stream");
					}
					defaultColumns = {"nodeId", "score"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_CLOSENESS: {
					const std::string name = requireProjectionName(arguments, "gds.closeness.stream", 1U);
					const std::vector<gds::LogicalGdsNodeScore> scores =
							gds::LogicalGraphAlgorithms::closenessCentrality(
									*projection_catalog_->get(name),
									{.maximum_nodes = readBounds.maximum_node_records,
									 .maximum_edges = maximumGdsAlgorithmEdges,
									 .maximum_iterations = maximumGdsAlgorithmIterations,
									 .maximum_work_units = maximumGdsAlgorithmWorkUnits},
									[&context] { context.checkGuard(); });
					rows.reserve(scores.size());
					for (const gds::LogicalGdsNodeScore &score: scores) {
						context.checkGuard();
						BufferedProcedureRow row;
						row.row.values.emplace("nodeId", PropertyValue(checkedOutputNodeId(score.node_id)));
						row.row.values.emplace("score", PropertyValue(score.score));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows,
													  "gds.closeness.stream");
					}
					defaultColumns = {"nodeId", "score"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_LEIDEN: {
					if (arguments.empty() || arguments.size() > 2U) {
						throw std::invalid_argument("gds.leiden.stream expects (graphName[, options])");
					}
					gds::LogicalLeidenOptions options;
					if (arguments.size() == 2U) {
						if (arguments[1].getType() != PropertyType::MAP) {
							throw std::invalid_argument("gds.leiden.stream options must be a map");
						}
						const PropertyValue::MapType &optionMap = arguments[1].getMap();
						if (optionMap.size() > LEIDEN_OPTION_NAMES.size()) {
							throw std::invalid_argument("gds.leiden.stream options exceed the supported entry bound");
						}
						for (const auto &[key, value]: optionMap) {
							context.checkGuard();
							if (std::ranges::find(LEIDEN_OPTION_NAMES, key) == LEIDEN_OPTION_NAMES.end()) {
								throw std::invalid_argument("gds.leiden.stream options contain an unknown key");
							}
							if (key == "maxIterations") {
								options.maximum_iterations = static_cast<size_t>(
										requireGdsPositiveInteger(value, "gds.leiden.stream maxIterations"));
							} else if (key == "maxLevels") {
								options.maximum_levels = static_cast<size_t>(
										requireGdsPositiveInteger(value, "gds.leiden.stream maxLevels"));
							} else if (key == "resolution") {
								options.resolution = requireGdsFiniteNumber(value, "gds.leiden.stream resolution");
							} else if (key == "refinementThreshold") {
								options.refinement_threshold =
										requireGdsFiniteNumber(value, "gds.leiden.stream refinementThreshold");
							} else if (key == "concurrency") {
								options.concurrency =
										requireGdsNonNegativeInteger(value, "gds.leiden.stream concurrency");
							}
						}
					}
					const std::string name = requireProjectionName(arguments, "gds.leiden.stream", arguments.size());
					const std::vector<gds::LogicalGdsNodeCommunity> communities = gds::LogicalGraphAlgorithms::leiden(
							*projection_catalog_->get(name), options,
							{.maximum_nodes = readBounds.maximum_node_records,
							 .maximum_edges = maximumGdsAlgorithmEdges,
							 .maximum_iterations = maximumGdsAlgorithmIterations,
							 .maximum_work_units = maximumGdsAlgorithmWorkUnits},
							[&context] { context.checkGuard(); }, thread_pool_);
					rows.reserve(communities.size());
					for (const gds::LogicalGdsNodeCommunity &community: communities) {
						context.checkGuard();
						BufferedProcedureRow row;
						row.row.values.emplace("nodeId", PropertyValue(checkedOutputNodeId(community.node_id)));
						row.row.values.emplace("communityId",
											   PropertyValue(checkedOutputNodeId(community.community_id)));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.leiden.stream");
					}
					defaultColumns = {"nodeId", "communityId"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_DIJKSTRA: {
					if (arguments.size() != 3U) {
						throw std::invalid_argument(
								"gds.shortestPath.dijkstra.stream expects (graphName, startId, endId)");
					}
					const std::string name = requireProjectionName(arguments, "gds.shortestPath.dijkstra.stream", 3U);
					const uint64_t source = requireShortestPathNodeId(arguments[1], "startId");
					const uint64_t target = requireShortestPathNodeId(arguments[2], "endId");
					const gds::LogicalGdsWeightedPath path =
							gds::LogicalGraphAlgorithms::dijkstra(*projection_catalog_->get(name), source, target,
																  {.maximum_nodes = readBounds.maximum_node_records,
																   .maximum_edges = maximumGdsAlgorithmEdges,
																   .maximum_iterations = maximumGdsAlgorithmIterations,
																   .maximum_work_units = maximumGdsAlgorithmWorkUnits},
																  [&context] { context.checkGuard(); });
					if (path.node_ids.size() != path.step_weights.size()) {
						throw std::logic_error("logical Dijkstra returned an invalid path shape");
					}
					rows.reserve(path.node_ids.size());
					double accumulatedCost = 0.0;
					for (size_t index = 0U; index < path.node_ids.size(); ++index) {
						context.checkGuard();
						// step_weights[index] is the edge used to reach this node. Account
						// for it before materializing the row so cost is cumulative at node.
						if (index != 0U) {
							accumulatedCost += path.step_weights[index];
						}
						const storage::LogicalGraphNode node = readProcedureNode(*snapshot, path.node_ids[index]);
						BufferedProcedureRow row;
						row.row.values.emplace("nodeId", PropertyValue(checkedOutputNodeId(path.node_ids[index])));
						row.row.values.emplace("node", node);
						row.row.values.emplace("cost", PropertyValue(accumulatedCost));
						row.row.values.emplace("totalCost", PropertyValue(path.total_weight));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows,
													  "gds.shortestPath.dijkstra.stream");
					}
					const double costTolerance =
							std::numeric_limits<double>::epsilon() *
							std::max({1.0, std::abs(accumulatedCost), std::abs(path.total_weight)}) * 8.0;
					if (!std::isfinite(accumulatedCost) || !std::isfinite(path.total_weight) ||
						(!path.node_ids.empty() && std::abs(accumulatedCost - path.total_weight) > costTolerance)) {
						throw std::runtime_error("logical Dijkstra returned inconsistent path costs");
					}
					defaultColumns = {"nodeId", "node", "cost", "totalCost"};
					break;
				}
				default: // NOLINTNEXTLINE(bugprone-branch-clone): this group is guarded above.
					throw std::logic_error("logical projection procedure dispatch became inconsistent");
			}
			QueryResult result;
			if (operations.empty() && procedure->kind != NativeProcedureKind::NPK_GDS_DIJKSTRA) {
				result = makeProcedureResult(std::move(rows), defaultColumns, readBounds, context);
			} else if (operations.empty()) {
				result.setColumns(defaultColumns);
				for (BufferedProcedureRow &buffered: rows) {
					context.checkGuard();
					QueryResult::Row row;
					NodePropertyMaps properties;
					row.emplace("nodeId", std::get<PropertyValue>(buffered.row.values.at("nodeId")));
					row.emplace("node", materializeResultValue(buffered.row.values.at("node"), *snapshot, readBounds,
															   context, properties));
					row.emplace("cost", std::get<PropertyValue>(buffered.row.values.at("cost")));
					row.emplace("totalCost", std::get<PropertyValue>(buffered.row.values.at("totalCost")));
					result.addRow(std::move(row));
				}
			} else {
				result = executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context);
			}
			if (deferredCatalogMutation) {
				// Catalog visibility begins only after the result pipeline has fully
				// materialized. Do not check after commit: a completed catalog mutation
				// must never be reported as a late timeout.
				context.checkGuardNow();
				deferredCatalogMutation->commit();
			}
			return result;
		}
		if (procedure->kind == NativeProcedureKind::NPK_SHORTEST_PATH) {
			if (arguments.size() < 2U || arguments.size() > 3U) {
				throw std::invalid_argument("algo.shortestPath expects (startId, endId[, maxDepth])");
			}
			const uint64_t startNodeId = requireShortestPathNodeId(arguments[0], "startId");
			const uint64_t endNodeId = requireShortestPathNodeId(arguments[1], "endId");
			const size_t maximumDepth = arguments.size() == 3U ? requireShortestPathDepth(arguments[2]) : 15U;
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
		if (procedure->kind == NativeProcedureKind::NPK_VECTOR_TRAIN) {
			if (arguments.size() != 1U) {
				throw std::invalid_argument("logical Vector train requires one index-name argument");
			}
			if (vector_maintenance_backend_ == nullptr) {
				throw std::runtime_error("logical backend does not support Vector index training");
			}
			if (maximumVectorTrainingRecords == 0U || maximumVectorTrainingPages == 0U) {
				throw std::invalid_argument("logical Vector train requires positive record and page bounds");
			}
			class QueryGuardCancellationCheckpoint final : public storage::CancellationCheckpoint {
			public:
				explicit QueryGuardCancellationCheckpoint(const QueryContext &context) : context_(context) {}

				void check() const override { context_.checkGuard(); }

			private:
				const QueryContext &context_;
			};

			QueryGuardCancellationCheckpoint cancellation(context);
			context.checkGuard();
			std::unique_ptr<storage::PreparedLogicalGraphVectorTraining> prepared =
					vector_maintenance_backend_->prepareLogicalVectorTraining(
							{requireIndexName(arguments.front()), maximumVectorTrainingRecords, maximumVectorTrainingPages,
							 &cancellation});
			if (!prepared) {
				throw std::runtime_error("logical Vector maintenance backend returned no prepared training operation");
			}
			const storage::LogicalGraphVectorTrainingPreview preview = prepared->preview();
			const auto status = [&preview] {
				return PropertyValue(preview.will_train ? "Success" : "Skipped (Empty Data)");
			};
			// Result materialization remains entirely before commit(). A failure in
			// YIELD/WHERE/RETURN destroys the prepared operation and cannot publish a
			// derived Vector generation.
			QueryResult result;
			if (operations.empty()) {
				result.setColumns({"status"});
				QueryResult::Row row;
				row.emplace("status", status());
				result.addRow(std::move(row));
			} else {
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
				source.row.values.emplace("status", status());
				std::vector<BufferedProcedureRow> rows;
				rows.push_back(std::move(source));
				result = executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context);
			}
			// A prepared maintenance operation is now at its atomic commit point.
			// A periodic checkpoint can miss a deadline crossed while preparing a
			// small Vector generation. Sample immediately before the only durable
			// publication; never add a check after a successful commit.
			context.checkGuardNow();
			static_cast<void>(prepared->commit());
			return result;
		}
		if (procedure->kind != NativeProcedureKind::NPK_VECTOR_QUERY_NODES) {
			// This is a valid user query whose native implementation is unavailable,
			// not a malformed logical plan or an execution invariant violation.
			throw std::runtime_error("logical backend does not support this stored procedure");
		}
		if (arguments.size() != 3U) {
			throw std::invalid_argument("db.index.vector.queryNodes expects (indexName, topK, queryVector)");
		}
		if (readBounds.maximum_property_entries == 0U || maximumVectorMappingEntries == 0U ||
			maximumRawVectorReads == 0U || vectorCandidateSearchBudget == 0U || vectorCandidateRefinementBudget == 0U ||
			vectorCandidateSearchBudget > maximumVectorMappingEntries ||
			vectorCandidateRefinementBudget > maximumRawVectorReads) {
			throw std::invalid_argument("logical Vector procedure runtime requires positive read and search bounds");
		}
		if (vector_backend_ == nullptr) {
			throw std::runtime_error("logical backend does not support Vector index search");
		}

		class QueryGuardCancellationCheckpoint final : public storage::CancellationCheckpoint {
		public:
			explicit QueryGuardCancellationCheckpoint(const QueryContext &context) : context_(context) {}

			void check() const override { context_.checkGuard(); }

		private:
			const QueryContext &context_;
		};

		QueryGuardCancellationCheckpoint cancellation(context);
		storage::LogicalGraphVectorSearchRequest request;
		request.index_name = requireIndexName(arguments[0]);
		request.top_k = requireTopK(arguments[1]);
		request.query_vector = requireQueryVector(arguments[2]);
		request.maximum_mapping_entries = maximumVectorMappingEntries;
		request.maximum_raw_vector_reads = maximumRawVectorReads;
		request.candidate_search_budget = vectorCandidateSearchBudget;
		request.candidate_refinement_budget = vectorCandidateRefinementBudget;
		request.cancellation = &cancellation;
		if (request.top_k > readBounds.maximum_rows) {
			throw std::invalid_argument("db.index.vector.queryNodes topK exceeds the logical row bound");
		}
		if (request.top_k == 0U) {
			QueryResult result;
			result.setColumns(operations.empty() ? std::vector<std::string>{"node", "score"}
												 : root.getOutputVariables());
			return result;
		}

		// Opening a transaction-owned Vector snapshot pins observable state, so a
		// timed-out procedure must fail before acquiring it.
		context.checkGuard();
		std::unique_ptr<storage::LogicalGraphVectorSearchSnapshot> snapshot =
				vector_backend_->openLogicalGraphVectorSearchSnapshot();
		if (!snapshot) {
			throw std::runtime_error("logical Vector procedure backend could not open a search snapshot");
		}
		const storage::LogicalGraphVectorSearchResult searchResult = snapshot->searchLogicalVector(request);
		context.checkGuard();
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
					std::optional<LogicalPlanReadRow> projected =
							applyProjection(projection, *snapshot, readBounds, context, row, nodeProperties,
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
				if (value == row.values.end() || // ZYX_COV_EXCL_LINE: output variables are derived from the
												 // row-producing operators.
					!output.emplace(column, materializeResultValue(value->second, *snapshot, readBounds, context,
																   nodeProperties))
							 .second) {
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
