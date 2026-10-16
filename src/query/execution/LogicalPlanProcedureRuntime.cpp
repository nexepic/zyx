#include "query/execution/OrderByComparison.hpp"
/**
 * @file LogicalPlanProcedureRuntime.cpp
 * @brief Implementation of bounded logical stored procedures.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/execution/StringBufferBytes.hpp"
#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "graph/core/Types.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "graph/utils/OwnedStdVector.hpp"
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

        [[nodiscard]] QueryResult makeAdmittedProcedureResult(const QueryContext &context) {
            if (context.resource_account) return QueryResult(detail::ResourceLease(context.resource_account));
            return QueryResult{};
        }
        void setOwnedProcedureColumns(QueryResult &result, const logical::LogicalOperator &root) {
            const auto columns = root.getOwnedOutputVariables();
            result.setColumns(static_cast<const std::vector<std::string> &>(columns));
        }

		using PropertyMap = std::unordered_map<std::string, PropertyValue>;
		[[nodiscard]] size_t checkedProcedureSizeAdd(size_t left, size_t right);
		[[nodiscard]] size_t checkedProcedureSizeMul(size_t left, size_t right);
		struct CachedNodeProperties {
			PropertyMap values;
			bool complete = false;
		};
		using NodePropertyMaps = std::unordered_map<uint64_t, CachedNodeProperties>;
		[[nodiscard]] std::optional<uint64_t> procedureNodeIdentity(const LogicalPlanReadValue &value) {
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				return node->node_id;
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&value);
					reference != nullptr && !reference->is_edge && reference->entity_id != 0U) {
				return reference->entity_id;
			}
			return std::nullopt;
		}
		// An options map cannot contain a key more than once. Keeping the accepted
		// names in one place makes the resource bound grow with the public contract.
		constexpr std::array<std::string_view, 5U> LEIDEN_OPTION_NAMES = {"maxIterations", "maxLevels", "resolution",
																			  "refinementThreshold", "concurrency"};

		// Complete materialization is still required at procedure result and
		// algorithm boundaries, but it must enter through the same explicit shape
		// contract as the regular logical read runtime.  This keeps native readers
		// from accidentally treating a procedure point read as an unplanned shape.
		[[nodiscard]] storage::EntityReadShape completeNodeReadShape() {
			return {.include_labels = true, .include_type = false, .all_properties = true};
		}

		[[nodiscard]] const logical::LogicalOperator *requireOnlyChild(const logical::LogicalOperator &op) {
			const std::vector<logical::LogicalOperator *> children = op.getChildren();
			if (children.size() != 1U || children.front() == nullptr) {
				throw std::logic_error("logical procedure projection requires exactly one child");
			}
			return children.front();
		}

		[[nodiscard]] PropertyMap takeNodeProperties(storage::LogicalGraphNode &node,
				size_t maximumEntries, const QueryContext &context) {
			context.checkGuard();
			if (node.properties.size() > maximumEntries) throw std::runtime_error("logical Vector procedure Node Properties exceeded its caller-supplied bound");
			{
				std::unordered_set<std::string_view> keys;
				for (const auto &entry: node.properties) {
					context.checkGuard();
					if (entry.key.empty() || !keys.insert(entry.key).second) throw std::runtime_error("logical Vector procedure read an invalid Node Property entry set");
				}
			}
			PropertyMap properties;
			properties.reserve(node.properties.size());
			for (auto &entry: node.properties) {
				context.checkGuard();
				properties.emplace(std::move(entry.key), std::move(entry.value));
			}
			std::vector<storage::LogicalGraphPropertyEntry>().swap(node.properties);
			return properties;
		}

		void cacheResolvedNodeProperties(const LogicalPlanReadRow &row, const LogicalPlanReadRow &output,
				NodePropertyMaps &nodeProperties) {
			for (const auto &[alias, properties]: row.property_cache) {
				const auto binding = row.values.find(alias);
				if (binding ==
					row.values
							.end()) { // ZYX_COV_EXCL_LINE: evaluator writes cache entries only for aliases in this row.
					throw std::logic_error("logical Vector procedure Property cache has no matching binding");
				}
				const auto nodeId = procedureNodeIdentity(binding->second);
				if (!nodeId.has_value()) { // ZYX_COV_EXCL_LINE: only Node Property evaluation populates this cache.
					continue;
				}
				if (!std::ranges::any_of(output.values, [&](const auto &binding) {
					return procedureNodeIdentity(binding.second) == nodeId;
				})) continue;
				const auto [cached, inserted] = nodeProperties.try_emplace(*nodeId);
				static_cast<void>(inserted);
				if (row.complete_entity_property_cache.contains(alias)) {
					// Selected reads represent missing fields with NULL. A complete map
					// must drop those sentinels instead of promoting them to real keys.
					for (const auto &[key, value]: cached->second.values) {
						const auto found = properties.find(key);
						if (found == properties.end() ? value != PropertyValue() : found->second != value) {
							throw std::runtime_error("logical Vector procedure observed inconsistent complete Node Properties");
						}
					}
					cached->second = CachedNodeProperties{properties, true};
					continue;
				}
				for (const auto &[key, value]: properties) {
					if (cached->second.complete) {
						const auto found = cached->second.values.find(key);
						if (found == cached->second.values.end() ? value != PropertyValue() : found->second != value) {
							throw std::runtime_error("logical Vector procedure observed inconsistent selected Node Properties");
						}
						continue;
					}
					const auto [existing, valueInserted] = cached->second.values.emplace(key, value);
					if (!valueInserted && existing->second != value) { // ZYX_COV_EXCL_LINE: one snapshot cannot diverge.
						throw std::runtime_error("logical Vector procedure observed inconsistent Node Properties");
					}
				}
			}
		}

		void attachCachedNodeProperties(LogicalPlanReadRow &row, const NodePropertyMaps &nodeProperties) {
			for (const auto &[alias, value]: row.values) {
				const auto nodeId = procedureNodeIdentity(value);
				if (!nodeId.has_value()) {
					continue;
				}
				if (const auto properties = nodeProperties.find(*nodeId); properties != nodeProperties.end()) {
					row.property_cache.insert_or_assign(alias, properties->second.values);
					if (properties->second.complete) {
						row.complete_entity_property_cache.insert(alias);
					}
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

		void validateEdge(const storage::LogicalGraphEdgeTopology &edge) {
			if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
				throw std::runtime_error("logical procedure backend returned an invalid relationship topology");
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

		[[nodiscard]] std::vector<float> requireQueryVector(const PropertyValue &value,
				size_t maximumMaterializedBytes, const QueryContext &context) {
			if (value.getType() != PropertyType::LIST) {
				throw std::invalid_argument("db.index.vector.queryNodes queryVector argument must be a List of floats");
			}
			if (value.getList().empty()) {
				throw std::invalid_argument("db.index.vector.queryNodes requires a nonempty numeric query vector");
			}
			if (value.getList().size() > std::numeric_limits<size_t>::max() / sizeof(float)) {
				throw std::overflow_error("db.index.vector.queryNodes query vector size overflowed");
			}
			const size_t queryBytes = value.getList().size() * sizeof(float);
			if (queryBytes > maximumMaterializedBytes) {
				throw std::runtime_error("db.index.vector.queryNodes query vector exceeded its materialized byte bound");
			}
			context.checkGuardNow();
			std::vector<float> query;
			query.reserve(value.getList().size());
			if (checkedProcedureSizeMul(query.capacity(), sizeof(float)) > maximumMaterializedBytes) {
				throw std::runtime_error("db.index.vector.queryNodes query vector exceeded its materialized byte bound");
			}
			size_t coordinateIndex = 0U;
			for (const PropertyValue &coordinate: value.getList()) {
				if ((coordinateIndex++ & 1023U) == 1023U) context.checkGuard();
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

		[[nodiscard]] size_t retainedVectorRequestBytes(
				const storage::LogicalGraphVectorSearchRequest &request) {
			size_t bytes = 0U;
			// The request object and string's inline storage are part of the stack
			// frame; charge only dynamically retained payloads.
			bytes = checkedProcedureSizeAdd(bytes, detail::dynamicStringBufferBytes(request.index_name));
			return checkedProcedureSizeAdd(bytes,
					checkedProcedureSizeMul(request.query_vector.capacity(), sizeof(float)));
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

		[[nodiscard]] std::string requireProjectionName(const logical::LogicalCallProcedure::ArgumentView &arguments,
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

		[[nodiscard]] storage::LogicalGraphNode validateProcedureNodeIdentity(storage::LogicalGraphReadSnapshot &snapshot,
																	  uint64_t nodeId) {
			const storage::LogicalGraphNode node = snapshot.readNodeShaped(nodeId, {});
			validateNode(node);
			if (node.node_id != nodeId) {
				throw std::runtime_error("logical procedure backend resolved a mismatched Node identity");
			}
			return node;
		}

		[[nodiscard]] utils::OwnedStdVector<uint64_t>
		findShortestPath(storage::LogicalGraphReadSnapshot &snapshot, uint64_t startNodeId, uint64_t endNodeId,
						 size_t maximumDepth, const LogicalPlanReadRuntimeBounds &bounds, const QueryContext &context) {
			if (bounds.maximum_node_records == 0U || bounds.maximum_edges_per_node == 0U ||
				bounds.maximum_adjacency_page_records == 0U || bounds.maximum_property_entries == 0U ||
				bounds.maximum_rows == 0U || bounds.maximum_materialized_bytes == 0U) {
				throw std::invalid_argument("logical shortest-path procedure requires positive graph read bounds");
			}
			std::array<uint64_t, 2U> endpointIds{startNodeId, endNodeId};
			std::ranges::sort(endpointIds);
			const std::span<const uint64_t> endpoints(endpointIds.data(), startNodeId == endNodeId ? 1U : 2U);
			if (snapshot.supportsNodeExistenceProbes()) {
				const std::vector<bool> present = snapshot.containsNodesSorted(endpoints);
				if (present.size() != endpoints.size()) {
					throw std::runtime_error("logical procedure backend changed shortest-path endpoint batch cardinality");
				}
				if (std::ranges::any_of(present, [](const bool value) { return !value; })) {
					return {};
				}
			} else if (!snapshot.containsNode(startNodeId) || !snapshot.containsNode(endNodeId)) {
				return {};
			}
			if (startNodeId == endNodeId) {
				if (!snapshot.supportsNodeExistenceProbes()) static_cast<void>(validateProcedureNodeIdentity(snapshot, startNodeId));
				return {startNodeId};
			}
			// A snapshot with a sorted existence probe can authenticate the start
			// endpoint without decoding its complete payload. Expression or output
			// requirements perform any subsequent shaped reads. Adapters without that
			// probe use an ID-only point read for endpoint validation.
			if (!snapshot.supportsNodeExistenceProbes()) {
				static_cast<void>(validateProcedureNodeIdentity(snapshot, startNodeId));
			}
			if (bounds.maximum_node_records > std::numeric_limits<size_t>::max() / bounds.maximum_edges_per_node) {
				throw std::overflow_error("logical shortest-path procedure Edge bound overflowed");
			}
			const size_t maximumExploredEdges = bounds.maximum_node_records * bounds.maximum_edges_per_node;

			struct Parent {
				uint64_t node_id = 0U;
			};
			const auto checkedStateAdd = [](const size_t left, const size_t right) {
				if (right > std::numeric_limits<size_t>::max() - left) {
					throw std::overflow_error("logical shortest-path procedure state size overflowed");
				}
				return left + right;
			};
			const auto checkedStateMul = [](const size_t left, const size_t right) {
				if (right != 0U && left > std::numeric_limits<size_t>::max() / right) {
					throw std::overflow_error("logical shortest-path procedure state size overflowed");
				}
				return left * right;
			};
			const auto stateBytesFor = [&](const size_t nodeCount, const size_t frontierHighWater,
					const size_t parentCount) {
				const size_t depthBytes = checkedStateMul(nodeCount,
						sizeof(std::pair<const uint64_t, size_t>) + 3U * sizeof(void *));
				const size_t parentBytes = checkedStateMul(parentCount,
						sizeof(std::pair<const uint64_t, Parent>) + 3U * sizeof(void *));
				const size_t frontierBytes = checkedStateMul(frontierHighWater,
						sizeof(uint64_t) + 2U * sizeof(void *));
				return checkedStateAdd(checkedStateAdd(depthBytes, parentBytes), frontierBytes);
			};
			// Charge allocator requests (including bucket arrays and deque blocks)
            // independently of the conservative local algorithm work limit.
            std::deque<uint64_t, utils::OwnedPmrAllocator<uint64_t>> frontier;
            using DepthEntry = std::pair<const uint64_t, size_t>;
            using ParentEntry = std::pair<const uint64_t, Parent>;
            std::unordered_map<uint64_t, size_t, std::hash<uint64_t>, std::equal_to<uint64_t>,
                utils::OwnedPmrAllocator<DepthEntry>> depths;
            std::unordered_map<uint64_t, Parent, std::hash<uint64_t>, std::equal_to<uint64_t>,
                utils::OwnedPmrAllocator<ParentEntry>> parents;
			MaterializedByteBudget procedureBudget(bounds.maximum_materialized_bytes);
			MaterializedByteReservation retainedStateBytes(&procedureBudget);
			size_t frontierHighWater = 1U;
			const auto admitState = [&](const size_t nodes, const size_t frontierCount, const size_t parentCount) {
				frontierHighWater = std::max(frontierHighWater, frontierCount);
				const size_t nextBytes = stateBytesFor(nodes, frontierHighWater, parentCount);
				if (nextBytes > bounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical shortest-path procedure exceeded its materialized state byte bound");
				}
				retainedStateBytes.set(nextBytes);
			};
			admitState(1U, 1U, 0U);
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
						MaterializedByteReservation adjacencyPageBytes(&procedureBudget);
						// Shortest-path expansion only needs Edge identity and endpoints.
						// Keep the topology cursor on the hot BFS path so relationship
						// Property roots and payloads are not decoded for edges that are
						// never returned.
						size_t requestedRecords = pageRecords;
						const auto page = [&] {
							for (;;) {
								context.checkGuard();
								struct TopologyBufferPressure {};
								storage::LogicalGraphAdjacencyTopologyPage candidate;
								try {
									candidate = snapshot.supportsAdjacencyTopologyBufferAdmission()
										? snapshot.scanAdjacentEdgeTopologiesWithBufferAdmission(currentNodeId, direction,
											continuation, requestedRecords, [&](size_t bytes) {
												context.checkGuard();
												if (adjacencyPageBytes.wouldExceed(bytes)) throw TopologyBufferPressure{};
												adjacencyPageBytes.set(bytes);
											})
										: snapshot.scanAdjacentEdgeTopologies(currentNodeId, direction, continuation, requestedRecords);
								} catch (const TopologyBufferPressure &) {
									adjacencyPageBytes.reset();
									if (requestedRecords == 1U) throw std::runtime_error("shortest-path topology source buffers exceeded their byte bound");
									requestedRecords = std::max<size_t>(1U, requestedRecords / 2U);
									continue;
								}
								if (candidate.edges.size() > requestedRecords ||
									(candidate.edges.empty() && candidate.continuation.has_value()) ||
									(continuation.has_value() && (candidate.edges.empty() ||
										candidate.edges.front().edge_id != continuation->next_edge_id))) {
									throw std::runtime_error("logical procedure backend returned an invalid shortest-path adjacency page");
								}
								uint64_t candidatePreviousId = previousEdgeId;
								for (const auto &edge: candidate.edges) {
									context.checkGuard();
									validateEdge(edge);
									if (candidatePreviousId != 0U && edge.edge_id <= candidatePreviousId) {
										throw std::runtime_error("logical procedure backend returned non-monotonic shortest-path relationship IDs");
									}
									candidatePreviousId = edge.edge_id;
									if ((direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING && edge.source_node_id != currentNodeId) ||
										(direction == storage::LogicalGraphReadDirection::LGRD_INCOMING && edge.target_node_id != currentNodeId)) {
										throw std::runtime_error("logical procedure backend returned a mismatched shortest-path relationship");
									}
								}
								size_t bytes = checkedStateMul(candidate.edges.capacity(), sizeof(storage::LogicalGraphEdgeTopology));
								for (const auto &edge: candidate.edges) bytes = checkedStateAdd(bytes, edge.type.capacity());
								const size_t routingBytes = snapshot.supportsNodeExistenceProbes()
									? checkedStateMul(candidate.edges.size(), 2U * sizeof(uint64_t)) : 0U;
								const size_t admissionBytes = checkedStateAdd(bytes, routingBytes);
								if (!adjacencyPageBytes.wouldExceed(admissionBytes) || requestedRecords == 1U) {
									adjacencyPageBytes.set(admissionBytes);
									return candidate;
								}
								// No edge was processed: release this page and retry the same
								// continuation with a smaller prefix before probing endpoints.
								adjacencyPageBytes.reset();
								requestedRecords = std::max<size_t>(1U, requestedRecords / 2U);
							}
						}();
						size_t retainedPageBytes = checkedStateMul(page.edges.capacity(), sizeof(storage::LogicalGraphEdgeTopology));
						for (const auto &edge: page.edges) retainedPageBytes = checkedStateAdd(retainedPageBytes, edge.type.capacity());
						// Endpoint validation is independent of edge traversal order. Resolve
						// the distinct endpoint set once per topology page so a high-degree
						// frontier node does not repeat the same Entity Directory descent for
						// parallel edges or repeated targets.
						utils::OwnedStdVector<uint64_t> endpointIds;
						if (snapshot.supportsNodeExistenceProbes()) {
							const size_t plannedEndpointBytes = checkedStateMul(page.edges.size(), sizeof(uint64_t));
							const size_t plannedPresenceBytes = checkedStateMul(page.edges.size(), sizeof(uint64_t));
							adjacencyPageBytes.set(checkedStateAdd(retainedPageBytes,
									checkedStateAdd(plannedEndpointBytes, plannedPresenceBytes)));
							endpointIds.reserve(page.edges.size());
							const size_t reservedEndpointBytes = checkedStateMul(endpointIds.capacity(),
									sizeof(uint64_t));
							const size_t expectedPresenceBytes = plannedPresenceBytes;
							adjacencyPageBytes.set(checkedStateAdd(retainedPageBytes,
									checkedStateAdd(reservedEndpointBytes, expectedPresenceBytes)));
							for (const storage::LogicalGraphEdgeTopology &candidate: page.edges) {
								if (candidate.source_node_id == currentNodeId &&
										 direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING) {
									endpointIds.push_back(candidate.target_node_id);
								} else if (candidate.target_node_id == currentNodeId &&
										 direction == storage::LogicalGraphReadDirection::LGRD_INCOMING) {
									endpointIds.push_back(candidate.source_node_id);
								}
							}
							std::ranges::sort(endpointIds);
							endpointIds.erase(std::unique(endpointIds.begin(), endpointIds.end()), endpointIds.end());
						}
						const std::vector<bool> endpointPresent = snapshot.supportsNodeExistenceProbes()
								? snapshot.containsNodesSorted(endpointIds)
								: std::vector<bool>{};
						if (snapshot.supportsNodeExistenceProbes()) {
							if (endpointPresent.size() != endpointIds.size()) {
								throw std::runtime_error("logical procedure backend changed shortest-path endpoint batch cardinality");
							}
							const size_t actualEndpointBytes = checkedStateMul(endpointIds.capacity(),
									sizeof(uint64_t));
							const size_t actualPresenceBytes = checkedStateAdd(endpointPresent.capacity(), 7U) / 8U;
							adjacencyPageBytes.set(checkedStateAdd(retainedPageBytes,
									checkedStateAdd(actualEndpointBytes, actualPresenceBytes)));
						}
						for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
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
							if (snapshot.supportsNodeExistenceProbes()) {
								const auto endpoint = std::ranges::lower_bound(endpointIds, nextNodeId);
								if (endpoint == endpointIds.end() || *endpoint != nextNodeId ||
										endpointPresent[static_cast<size_t>(endpoint - endpointIds.begin())] == false) {
									throw std::runtime_error(
											"logical shortest-path procedure encountered an Edge with a missing endpoint");
								}
							} else {
								// Older logical adapters have no cheap existence probe; retain
								// an ID-only shaped read for endpoint authentication.
								static_cast<void>(validateProcedureNodeIdentity(snapshot, nextNodeId));
							}
							if (depths.contains(nextNodeId)) {
								continue;
							}
							if (depths.size() == bounds.maximum_node_records) {
								throw std::runtime_error("logical shortest-path procedure exceeded its Node bound");
							}
							const size_t nextNodeCount = checkedStateAdd(depths.size(), 1U);
							const size_t nextFrontierCount = nextNodeId == endNodeId ? frontier.size()
									: checkedStateAdd(frontier.size(), 1U);
							admitState(nextNodeCount, nextFrontierCount,
									checkedStateAdd(parents.size(), 1U));
							if (nextNodeCount > depths.bucket_count() * depths.max_load_factor()) {
								depths.reserve(nextNodeCount);
							}
							if (parents.size() + 1U > parents.bucket_count() * parents.max_load_factor()) {
								parents.reserve(parents.size() + 1U);
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

			utils::OwnedStdVector<uint64_t> reversed;
			MaterializedByteReservation resultPathBytes(&procedureBudget);
			resultPathBytes.set(sizeof(uint64_t));
			reversed.reserve(1U);
			reversed.push_back(endNodeId);
			while (reversed.back() != startNodeId) {
				const auto parent = parents.find(reversed.back());
				if (parent == parents.end() || parent->second.node_id == 0U) {
					throw std::runtime_error("logical shortest-path procedure has an incomplete parent chain");
				}
				const size_t nextSize = checkedStateAdd(reversed.size(), 1U);
				if (nextSize > bounds.maximum_rows) {
					throw std::runtime_error("logical shortest-path procedure exceeded its result-row bound");
				}
				const size_t nextCapacity = nextSize > reversed.capacity()
						? std::max(nextSize, checkedStateMul(reversed.capacity(), 2U))
						: reversed.capacity();
				resultPathBytes.set(checkedStateMul(nextCapacity, sizeof(uint64_t)));
				if (nextCapacity > reversed.capacity()) reversed.reserve(nextCapacity);
				resultPathBytes.set(checkedStateMul(reversed.capacity(), sizeof(uint64_t)));
				reversed.push_back(parent->second.node_id);
			}
			std::ranges::reverse(reversed);
			return reversed;
		}

		[[nodiscard]] std::optional<LogicalPlanReadRow>
		applyProjection(const logical::LogicalProject &projection, storage::LogicalGraphReadSnapshot &snapshot,
						const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context,
						LogicalPlanReadRow &source, NodePropertyMaps &nodeProperties,
						std::unordered_set<std::string> *distinctRows, const size_t retainedOtherBytes = 0U,
						const bool accountRetainedProjectionBytes = false, const bool deferDistinct = false) {
			if ((projection.isDistinct() && distinctRows == nullptr && !deferDistinct) ||
				(!projection.isDistinct() && distinctRows != nullptr) || (deferDistinct && distinctRows != nullptr)) {
				throw std::logic_error("logical Vector procedure projection DISTINCT state is inconsistent");
			}
			if (projection.getItems().empty()) {
				throw std::invalid_argument("logical Vector procedure Project has no items");
			}

			LogicalPlanReadRow result;
			result.values.reserve(projection.getItems().size());
			result.property_cache.reserve(projection.getItems().size());
			const auto addBytes = [](const size_t left, const size_t right) {
				if (right > std::numeric_limits<size_t>::max() - left) {
					throw std::overflow_error("logical procedure Project materialization size overflowed");
				}
				return left + right;
			};
			const auto mulBytes = [](const size_t left, const size_t right) {
				if (right != 0U && left > std::numeric_limits<size_t>::max() / right) {
					throw std::overflow_error("logical procedure Project materialization size overflowed");
				}
				return left * right;
			};
			const auto valueBytes = [&](const LogicalPlanReadValue &value) {
				size_t bytes = sizeof(value);
				if (const auto *scalar = std::get_if<PropertyValue>(&value)) {
					return addBytes(bytes, property_utils::getPropertyValueSize(*scalar));
				}
				if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
					static_cast<void>(reference);
					return addBytes(bytes, sizeof(storage::EntityRef));
				}
				if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
					bytes = addBytes(bytes, mulBytes(node->labels.capacity(), sizeof(std::string)));
					for (const std::string &label: node->labels) bytes = addBytes(bytes, label.size());
					bytes = addBytes(bytes, mulBytes(node->properties.capacity(), sizeof(storage::LogicalGraphPropertyEntry)));
					for (const auto &entry: node->properties) {
						bytes = addBytes(bytes, entry.key.size());
						bytes = addBytes(bytes, property_utils::getPropertyValueSize(entry.value));
					}
					return bytes;
				}
				const auto &edge = std::get<storage::LogicalGraphEdge>(value);
				bytes = addBytes(bytes, edge.type.size());
				bytes = addBytes(bytes, mulBytes(edge.properties.capacity(), sizeof(storage::LogicalGraphPropertyEntry)));
				for (const auto &entry: edge.properties) {
					bytes = addBytes(bytes, entry.key.size());
					bytes = addBytes(bytes, property_utils::getPropertyValueSize(entry.value));
				}
				return bytes;
			};
			const auto rowPayloadBytes = [&](const LogicalPlanReadRow &row) {
				size_t bytes = sizeof(LogicalPlanReadRow);
				for (const auto &[name, value]: row.values) {
					bytes = addBytes(bytes, addBytes(name.size(), valueBytes(value)));
				}
				for (const auto &[alias, properties]: row.property_cache) {
					bytes = addBytes(bytes, alias.size());
					for (const auto &[key, value]: properties) {
						bytes = addBytes(bytes, addBytes(key.size(), property_utils::getPropertyValueSize(value)));
					}
				}
				for (const auto &[alias, metadata]: row.entity_metadata_cache) {
					bytes = addBytes(bytes, alias.size());
					for (const auto &[key, value]: metadata) {
						bytes = addBytes(bytes, addBytes(key.size(), property_utils::getPropertyValueSize(value)));
					}
				}
				return bytes;
			};
			const auto retainedProjectionBytes = [&] {
				if (!accountRetainedProjectionBytes) return size_t{0U};
				// Project outputs replace the source row after evaluation. Keep the
				// newly built output payload charged across sibling expressions, while
				// the procedure's batch and Property readers retain source-row ownership.
				return addBytes(retainedOtherBytes, rowPayloadBytes(result) - sizeof(LogicalPlanReadRow));
			};
			for (const logical::LogicalProjectItem &item: projection.getItems()) {
				context.checkGuard();
				if (item.alias.empty() || !item.expression) {
					throw std::logic_error("logical Vector procedure projection has an invalid output item");
				}
				// Evaluate against the original bindings while retaining new cache
				// entries in place. Output aliases never enter the source scope.
				const size_t retainedBytes = retainedProjectionBytes();
				if (retainedBytes >= readBounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical procedure Project exceeded its shared materialized byte bound before expression");
				}
				LogicalPlanReadRuntimeBounds expressionBounds = readBounds;
				expressionBounds.maximum_materialized_bytes -= retainedBytes;
				LogicalPlanReadValue value = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
					snapshot, *item.expression, source, expressionBounds, context);
				if (!result.values.emplace(item.alias, std::move(value)).second) {
					throw std::logic_error("logical Vector procedure projection has duplicate output aliases");
				}
				if (retainedProjectionBytes() > readBounds.maximum_materialized_bytes) {
					throw std::runtime_error("logical procedure Project exceeded its shared materialized byte bound after expression");
				}
			}
			// Scalar-only output cannot use the identity cache in a later layer.
			// Populate it once only for entities that remain in the output scope.
			std::erase_if(nodeProperties, [&](const auto &cached) {
				return !std::ranges::any_of(result.values, [&](const auto &binding) {
					return procedureNodeIdentity(binding.second) == cached.first;
				});
			});
			if (std::ranges::any_of(result.values, [](const auto &binding) {
				return procedureNodeIdentity(binding.second).has_value();
			})) {
				cacheResolvedNodeProperties(source, result, nodeProperties);
				attachCachedNodeProperties(result, nodeProperties);
			}
			// Preserve metadata across an Entity alias projection without fetching
			// it again in a later layer. Scalar outputs carry no entity metadata.
			for (const auto &[alias, value]: result.values) {
				const auto identity = procedureNodeIdentity(value);
				if (!identity) continue;
				for (const auto &[sourceAlias, sourceValue]: source.values) {
					if (procedureNodeIdentity(sourceValue) != identity) continue;
					const auto metadata = source.entity_metadata_cache.find(sourceAlias);
					if (metadata != source.entity_metadata_cache.end()) {
						result.entity_metadata_cache.insert_or_assign(alias, metadata->second);
						break;
					}
				}
			}

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
			// The evaluator only enriches caches. Keep them on the surviving row
			// instead of copying the row and a second identity-indexed payload map.
			const LogicalPlanReadValue evaluated = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
					snapshot, *filter.getPredicate(), row, readBounds, context);
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

		// Final procedure rows are consumed exactly once. Transfer their payloads
		// directly instead of rebuilding a reusable identity cache at the result
		// boundary. Each bare output alias already owns its complete Node.
		[[nodiscard]] ResultValue materializeResultValue(LogicalPlanReadValue &value,
				storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &readBounds,
				const QueryContext &context, NodePropertyMaps &nodeProperties) {
			context.checkGuard();
			if (auto *property = std::get_if<PropertyValue>(&value)) return std::move(*property);
			if (auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				validateNode(*node);
				nodeProperties.erase(node->node_id);
				auto properties = takeNodeProperties(*node, readBounds.maximum_property_entries, context);
				return LogicalNodeResult{node->node_id, std::move(node->labels), std::move(properties)};
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				if (reference->is_edge || reference->entity_id == 0U) throw std::logic_error("logical Vector procedure result contains an invalid Node reference");
				const uint64_t nodeId = reference->entity_id;
				auto node = snapshot.readNodeShaped(nodeId, completeNodeReadShape());
				validateNode(node);
				if (node.node_id != nodeId) throw std::runtime_error("logical Vector procedure result Node reference changed identity");
				value = std::move(node);
				return materializeResultValue(value, snapshot, readBounds, context, nodeProperties);
			}
			throw std::logic_error("logical Vector procedure result contains an unsupported Edge binding");
		}

		struct BufferedProcedureRow {
			LogicalPlanReadRow row;
			NodePropertyMaps node_properties;
		};
        // Only the vector backing is admitted here. Nested row/cache payloads
        // retain their separate materialization admission and lifetime rules.
        using ProcedureRows = std::vector<BufferedProcedureRow, utils::OwnedPmrAllocator<BufferedProcedureRow>>;


		template<typename Container>
		void releaseProcedureContainer(Container &container) {
			Container empty;
			container.swap(empty);
		}

		constexpr size_t MAXIMUM_PROCEDURE_NODE_BATCH_ROWS = 256U;

		[[nodiscard]] size_t procedureNodeBatchCapacity(const LogicalPlanReadRuntimeBounds &bounds) {
			if (bounds.maximum_rows == 0U || bounds.maximum_materialized_bytes == 0U) {
				throw std::invalid_argument("logical procedure runtime requires positive Node batch bounds");
			}
			// Reserve only a conservative minimum per complete Node.  The actual
			// payload is checked after the read, but this prevents a small byte budget
			// from staging the default 256 full records before that check can run.
			constexpr size_t minimumNodeBytes = sizeof(BufferedProcedureRow) + sizeof(LogicalPlanReadRow) +
					 sizeof(storage::LogicalGraphNode) + sizeof(PropertyValue);
			const size_t byteCapacity = std::max<size_t>(
				1U, bounds.maximum_materialized_bytes / std::max<size_t>(1U, minimumNodeBytes));
			return std::min({MAXIMUM_PROCEDURE_NODE_BATCH_ROWS, bounds.maximum_rows, byteCapacity});
		}

		[[nodiscard]] size_t reserveBoundedProcedureRows(ProcedureRows &rows,
											  const size_t requestedRows,
											  const LogicalPlanReadRuntimeBounds &bounds,
											  const size_t retainedSourceBytes = 0U) {
			if (retainedSourceBytes > bounds.maximum_materialized_bytes) {
				throw std::runtime_error("logical procedure source and output exceeded their byte bound");
			}
		const size_t availableBytes = bounds.maximum_materialized_bytes - retainedSourceBytes;
		const size_t byteCapacity = availableBytes / sizeof(BufferedProcedureRow);
			rows.reserve(std::min({requestedRows, bounds.maximum_rows, byteCapacity}));
			return availableBytes;
		}

		[[nodiscard]] std::optional<size_t> vectorProcedurePrefixThroughLimit(
				const std::vector<const logical::LogicalOperator *> &operations, const size_t totalRows) {
			if (operations.empty() || !std::ranges::all_of(operations, [](const logical::LogicalOperator *operation) {
					const auto type = operation->getType();
					return type == logical::LogicalOpType::LOP_SKIP || type == logical::LogicalOpType::LOP_LIMIT ||
							(type == logical::LogicalOpType::LOP_PROJECT &&
									!dynamic_cast<const logical::LogicalProject &>(*operation).isDistinct());
				})) {
				return std::nullopt;
			}

			size_t remaining = totalRows;
			size_t requiredPrefix = 0U;
			bool reachedLimit = false;
			for (const logical::LogicalOperator *operation: operations) {
				if (operation->getType() == logical::LogicalOpType::LOP_SKIP) {
					const int64_t offset = dynamic_cast<const logical::LogicalSkip &>(*operation).getOffset();
					if (offset < 0) {
						throw std::invalid_argument("logical Vector procedure SKIP cannot be negative");
					}
					const size_t skipped = std::min(remaining, static_cast<size_t>(offset));
					if (!reachedLimit) {
						requiredPrefix += skipped;
					}
					remaining -= skipped;
					continue;
				}
				if (operation->getType() == logical::LogicalOpType::LOP_LIMIT) {
					const int64_t limit = dynamic_cast<const logical::LogicalLimit &>(*operation).getLimit();
					if (limit < 0) {
						throw std::invalid_argument("logical Vector procedure LIMIT cannot be negative");
					}
					remaining = std::min(remaining, static_cast<size_t>(limit));
					if (!reachedLimit) {
						requiredPrefix += remaining;
						reachedLimit = true;
					}
				}
			}
			return reachedLimit ? std::optional<size_t>(requiredPrefix) : std::nullopt;
		}

		[[nodiscard]] size_t checkedProcedureSizeAdd(const size_t left, const size_t right) {
			if (right > std::numeric_limits<size_t>::max() - left) {
				throw std::overflow_error("logical procedure materialization size overflowed");
			}
			return left + right;
		}

		[[nodiscard]] size_t checkedProcedureSizeMul(const size_t left, const size_t right) {
			if (right != 0U && left > std::numeric_limits<size_t>::max() / right) {
				throw std::overflow_error("logical procedure materialization size overflowed");
			}
			return left * right;
		}

		[[nodiscard]] size_t procedureValueBytes(const LogicalPlanReadValue &value) {
			size_t bytes = sizeof(value);
			if (const auto *scalar = std::get_if<PropertyValue>(&value)) {
				return checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(*scalar));
			}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				bytes = checkedProcedureSizeAdd(bytes, checkedProcedureSizeMul(node->labels.size(), sizeof(std::string)));
				for (const std::string &label: node->labels) {
					bytes = checkedProcedureSizeAdd(bytes, label.size());
				}
				bytes = checkedProcedureSizeAdd(
						bytes, checkedProcedureSizeMul(node->properties.size(), sizeof(storage::LogicalGraphPropertyEntry)));
				for (const storage::LogicalGraphPropertyEntry &entry: node->properties) {
					bytes = checkedProcedureSizeAdd(bytes, entry.key.size());
					bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(entry.value));
				}
				return bytes;
			}
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				bytes = checkedProcedureSizeAdd(bytes, edge->type.size());
				bytes = checkedProcedureSizeAdd(
						bytes, checkedProcedureSizeMul(edge->properties.size(), sizeof(storage::LogicalGraphPropertyEntry)));
				for (const storage::LogicalGraphPropertyEntry &entry: edge->properties) {
					bytes = checkedProcedureSizeAdd(bytes, entry.key.size());
					bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(entry.value));
				}
				return bytes;
			}
			return checkedProcedureSizeAdd(bytes, sizeof(storage::EntityRef));
		}

		// Account for a complete Node result without constructing a temporary
		// LogicalPlanReadRow. Procedure hit materialization keeps the Node in the
		// sorted-read buffer until it is moved into its final score-order slot;
		// copying it just to measure the byte bound would recreate the payload
		// duplication this path is designed to avoid.
		[[nodiscard]] size_t bufferedProcedureNodeRowBytes(const storage::LogicalGraphNode &node,
																	 const bool includesScore) {
			size_t bytes = sizeof(BufferedProcedureRow);
			bytes = checkedProcedureSizeAdd(bytes, std::string_view("node").size());
			bytes = checkedProcedureSizeAdd(bytes, sizeof(LogicalPlanReadValue));
			bytes = checkedProcedureSizeAdd(bytes, checkedProcedureSizeMul(node.labels.size(), sizeof(std::string)));
			for (const std::string &label: node.labels) {
				bytes = checkedProcedureSizeAdd(bytes, label.size());
			}
			bytes = checkedProcedureSizeAdd(
					bytes, checkedProcedureSizeMul(node.properties.size(), sizeof(storage::LogicalGraphPropertyEntry)));
			for (const storage::LogicalGraphPropertyEntry &entry: node.properties) {
				bytes = checkedProcedureSizeAdd(bytes, entry.key.size());
				bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(entry.value));
			}
			if (includesScore) {
				bytes = checkedProcedureSizeAdd(bytes, std::string_view("score").size());
				bytes = checkedProcedureSizeAdd(bytes, sizeof(LogicalPlanReadValue));
				bytes = checkedProcedureSizeAdd(bytes, sizeof(double));
			}
			return bytes;
		}

		[[nodiscard]] size_t bufferedProcedureRowBytes(const BufferedProcedureRow &buffered) {
			size_t bytes = sizeof(buffered);
			for (const auto &[name, value]: buffered.row.values) {
				bytes = checkedProcedureSizeAdd(bytes, name.size());
				bytes = checkedProcedureSizeAdd(bytes, procedureValueBytes(value));
			}
			for (const auto &[alias, properties]: buffered.row.property_cache) {
				bytes = checkedProcedureSizeAdd(bytes, alias.size());
				for (const auto &[key, value]: properties) {
					bytes = checkedProcedureSizeAdd(bytes, key.size());
					bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(value));
				}
			}
			for (const auto &[alias, metadata]: buffered.row.entity_metadata_cache) {
				bytes = checkedProcedureSizeAdd(bytes, alias.size());
				for (const auto &[key, value]: metadata) {
					bytes = checkedProcedureSizeAdd(bytes, key.size());
					bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(value));
				}
			}
			for (const auto &[alias, properties]: buffered.node_properties) {
				bytes = checkedProcedureSizeAdd(bytes, sizeof(alias));
				for (const auto &[key, value]: properties.values) {
					bytes = checkedProcedureSizeAdd(bytes, key.size());
					bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(value));
				}
			}
			return bytes;
		}

		class ProcedureByteBoundExceeded final : public std::runtime_error {
		public:
			explicit ProcedureByteBoundExceeded(const std::string &message) : std::runtime_error(message) {}
		};

		void admitProcedureBytes(MaterializedByteReservation &reservation, size_t bytes) {
			if (reservation.wouldExceed(bytes)) throw ProcedureByteBoundExceeded("logical procedure shared materialization exceeded its byte bound");
			reservation.set(bytes);
		}

		void validateBufferedProcedureBytes(const ProcedureRows &rows, const size_t maximumBytes) {
			if (maximumBytes == 0U) {
				throw std::invalid_argument("logical procedure runtime requires a positive materialization byte bound");
			}
			size_t total = 0U;
			for (const BufferedProcedureRow &row: rows) {
				const size_t rowBytes = bufferedProcedureRowBytes(row);
				if (rowBytes > maximumBytes - std::min(total, maximumBytes)) {
					throw ProcedureByteBoundExceeded("logical procedure materialization exceeded its byte bound");
				}
				total = checkedProcedureSizeAdd(total, rowBytes);
			}
		}

		[[nodiscard]] size_t bufferedProcedureRowsBytes(const ProcedureRows &rows,
				size_t firstIndex = 0U) {
			size_t bytes = 0U;
			for (size_t index = firstIndex; index < rows.size(); ++index) {
				bytes = checkedProcedureSizeAdd(bytes, bufferedProcedureRowBytes(rows[index]));
			}
			return bytes;
		}

		[[nodiscard]] size_t procedureResultPropertyMapBytes(
				const std::unordered_map<std::string, PropertyValue> &properties) {
			size_t bytes = checkedProcedureSizeMul(properties.bucket_count(), sizeof(void *));
			for (const auto &[key, value]: properties) {
				bytes = checkedProcedureSizeAdd(bytes, sizeof(std::pair<const std::string, PropertyValue>) + 3U * sizeof(void *));
				bytes = checkedProcedureSizeAdd(bytes, key.capacity());
				bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(value));
			}
			return bytes;
		}

		[[nodiscard]] size_t procedureResultValueBytes(const ResultValue &value) {
			size_t bytes = sizeof(ResultValue);
			const auto &variant = value.getVariant();
			if (const auto *property = std::get_if<PropertyValue>(&variant)) {
				return checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(*property));
			}
			if (const auto *node = std::get_if<LogicalNodeResult>(&variant)) {
				bytes = checkedProcedureSizeAdd(bytes, checkedProcedureSizeMul(node->labels.capacity(), sizeof(std::string)));
				for (const std::string &label: node->labels) bytes = checkedProcedureSizeAdd(bytes, label.capacity());
				return checkedProcedureSizeAdd(bytes, procedureResultPropertyMapBytes(node->properties));
			}
			if (const auto *edge = std::get_if<LogicalEdgeResult>(&variant)) {
				bytes = checkedProcedureSizeAdd(bytes, edge->type.capacity());
				return checkedProcedureSizeAdd(bytes, procedureResultPropertyMapBytes(edge->properties));
			}
			if (const auto *node = std::get_if<Node>(&variant)) {
				return checkedProcedureSizeAdd(bytes, node->getTotalPropertySize());
			}
			if (const auto *edge = std::get_if<Edge>(&variant)) {
				return checkedProcedureSizeAdd(bytes, edge->getTotalPropertySize());
			}
			return bytes;
		}

		[[nodiscard]] size_t procedureQueryResultBytes(const QueryResult &result) {
			size_t bytes = checkedProcedureSizeMul(result.getRows().capacity(), sizeof(QueryResult::Row));
			bytes = checkedProcedureSizeAdd(bytes,
					checkedProcedureSizeMul(result.getColumns().capacity(), sizeof(std::string)));
			for (const std::string &column: result.getColumns()) {
				bytes = checkedProcedureSizeAdd(bytes, detail::dynamicStringBufferBytes(column));
			}
			for (const QueryResult::Row &row: result.getRows()) {
				bytes = checkedProcedureSizeAdd(bytes, checkedProcedureSizeMul(row.bucket_count(), sizeof(void *)));
				for (const auto &[key, value]: row) {
					bytes = checkedProcedureSizeAdd(bytes, sizeof(std::pair<const std::string, ResultValue>) + 3U * sizeof(void *));
					bytes = checkedProcedureSizeAdd(bytes, key.capacity());
					bytes = checkedProcedureSizeAdd(bytes, procedureResultValueBytes(value));
				}
			}
			return bytes;
		}

		void validateProcedureResultBudget(const QueryResult &result, const size_t retainedSourceBytes,
				const size_t maximumBytes) {
			const size_t resultBytes = procedureQueryResultBytes(result);
			if (retainedSourceBytes > maximumBytes || resultBytes > maximumBytes - retainedSourceBytes) {
				throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
			}
		}

		[[nodiscard]] size_t procedureQueryResultRowBytes(const QueryResult::Row &row) {
			size_t bytes = checkedProcedureSizeMul(row.bucket_count(), sizeof(void *));
			for (const auto &[key, value]: row) {
				bytes = checkedProcedureSizeAdd(bytes, sizeof(std::pair<const std::string, ResultValue>) + 3U * sizeof(void *));
				bytes = checkedProcedureSizeAdd(bytes, key.capacity());
				bytes = checkedProcedureSizeAdd(bytes, procedureResultValueBytes(value));
			}
			return bytes;
		}

		void appendProcedureResultRow(QueryResult &result, ProcedureRows &rows,
				size_t rowIndex, QueryResult::Row output, size_t maximumBytes,
				const size_t retainedSourceBytes = 0U) {
			const size_t pendingRowsBytes = bufferedProcedureRowsBytes(rows, rowIndex);
			const size_t pendingResultBytes = checkedProcedureSizeAdd(procedureQueryResultBytes(result),
					procedureQueryResultRowBytes(output));
			size_t pendingBytes = checkedProcedureSizeAdd(retainedSourceBytes, pendingRowsBytes);
			pendingBytes = checkedProcedureSizeAdd(pendingBytes, pendingResultBytes);
			if (result.getRows().capacity() == result.getRows().size()) {
				const size_t currentCapacity = result.getRows().capacity();
				const size_t minimumCapacity = checkedProcedureSizeAdd(result.getRows().size(), 1U);
				const size_t preferredCapacity = currentCapacity == 0U
						? minimumCapacity
						: std::max(minimumCapacity, checkedProcedureSizeMul(currentCapacity, 2U));
                // Admit old/new backing overlap without rejecting a smaller
                // growth that fits the same materialization ceiling.
                const size_t affordableCapacity = pendingBytes > maximumBytes ? 0U :
                    (maximumBytes - pendingBytes) / sizeof(QueryResult::Row);
                if (affordableCapacity < minimumCapacity)
                    throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
                const size_t requestedCapacity = std::min(preferredCapacity, affordableCapacity);
				const size_t requestedGrowthBytes = checkedProcedureSizeMul(requestedCapacity, sizeof(QueryResult::Row));
				if (checkedProcedureSizeAdd(pendingBytes, requestedGrowthBytes) > maximumBytes) {
					throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
				}
				result.reserveRows(requestedCapacity);
				const size_t liveBytesAfterReserve = checkedProcedureSizeAdd(retainedSourceBytes,
						checkedProcedureSizeAdd(bufferedProcedureRowsBytes(rows, rowIndex),
								checkedProcedureSizeAdd(procedureQueryResultBytes(result), procedureQueryResultRowBytes(output))));
				if (liveBytesAfterReserve > maximumBytes) {
					throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
				}
			}
			if (pendingBytes > maximumBytes) {
				throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
			}

			result.addRow(std::move(output));
			BufferedProcedureRow &buffered = rows[rowIndex];
			releaseProcedureContainer(buffered.row.values);
			releaseProcedureContainer(buffered.row.property_cache);
			releaseProcedureContainer(buffered.row.entity_metadata_cache);
			releaseProcedureContainer(buffered.row.path_cache);
			releaseProcedureContainer(buffered.node_properties);
			const size_t liveBytes = checkedProcedureSizeAdd(retainedSourceBytes,
					checkedProcedureSizeAdd(procedureQueryResultBytes(result),
							bufferedProcedureRowsBytes(rows, rowIndex + 1U)));
			if (liveBytes > maximumBytes) {
				throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
			}
		}

		void prefetchProcedureNodeMetadata(ProcedureRows &rows,
				const std::vector<const expressions::Expression *> &expressions,
				storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &bounds,
				const QueryContext &context) {
			for (const auto &request: LogicalPlanReadRuntime::collectBatchedMetadataRequests(expressions)) {
				if (request.function != "labels") continue;
				MaterializedByteBudget budget(bounds.maximum_materialized_bytes);
				MaterializedByteReservation rowBytes(&budget);
				for (const auto &row: rows) admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(rowBytes.bytes(), bufferedProcedureRowBytes(row)));
				MaterializedByteReservation idBytes(&budget);
				const auto uncachedReference = [&](const BufferedProcedureRow &buffered) -> const storage::EntityRef * {
					const auto found = buffered.row.values.find(request.variable);
					if (found == buffered.row.values.end()) return nullptr;
					const auto *reference = std::get_if<storage::EntityRef>(&found->second);
					if (reference == nullptr || reference->is_edge) return nullptr;
					if (reference->entity_id == 0U) throw std::runtime_error("logical procedure metadata batch received a zero Node ID");
					const auto cached = buffered.row.entity_metadata_cache.find(request.variable);
					return cached != buffered.row.entity_metadata_cache.end() && cached->second.contains("labels") ? nullptr : reference;
				};
				size_t referenceCount = 0U;
				for (const auto &row: rows) {
					context.checkGuard();
					const auto *reference = uncachedReference(row);
					if (reference != nullptr) referenceCount = checkedProcedureSizeAdd(referenceCount, 1U);
				}
				idBytes.set(checkedProcedureSizeMul(referenceCount, sizeof(uint64_t)));
				utils::OwnedStdVector<uint64_t> ids;
				ids.reserve(referenceCount);
				idBytes.set(checkedProcedureSizeMul(ids.capacity(), sizeof(uint64_t)));
				for (const auto &row: rows) {
					context.checkGuard();
					const auto *reference = uncachedReference(row);
					if (reference == nullptr) continue;
					ids.push_back(reference->entity_id);
				}
				std::ranges::sort(ids);
				ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
				if (ids.size() > bounds.maximum_node_records) throw std::runtime_error("logical procedure metadata batch exceeded its Node bound");
				struct MetadataColumnPressure {};
				const auto processOnce = [&](size_t offset, size_t count) {
					context.checkGuard();
					const auto batchIds = std::span<const uint64_t>(ids).subspan(offset, count);
					MaterializedByteReservation consumerBytes(&budget);
					const size_t countBytes = checkedProcedureSizeMul(count, sizeof(size_t));
					if (consumerBytes.wouldExceed(countBytes)) throw MetadataColumnPressure{};
					consumerBytes.set(countBytes);
					utils::OwnedStdVector<size_t> consumers(count, 0U);
					const auto rowIndex = [&](const BufferedProcedureRow &row) -> std::optional<size_t> {
						const auto *reference = uncachedReference(row);
						if (reference == nullptr) return std::nullopt;
						const auto found = std::ranges::lower_bound(batchIds, reference->entity_id);
						return found == batchIds.end() || *found != reference->entity_id ? std::nullopt
							: std::optional<size_t>(static_cast<size_t>(found - batchIds.begin()));
					};
					for (const auto &row: rows) if (const auto index = rowIndex(row)) ++consumers[*index];
					MaterializedByteReservation sourceBytes(&budget);
					auto nodes = snapshot.readNodesShapedSorted(batchIds, {.include_labels = true});
					context.checkGuardNow();
					if (nodes.size() != count) throw std::runtime_error("logical procedure backend changed metadata batch cardinality");
					size_t bytes = 0U;
					for (size_t index = 0U; index < count; ++index) {
						context.checkGuard();
						validateNode(nodes[index]);
						if (nodes[index].node_id != batchIds[index]) throw std::runtime_error("logical procedure metadata batch changed Node order");
						bytes = checkedProcedureSizeAdd(bytes, bufferedProcedureNodeRowBytes(nodes[index], false) -
							sizeof(BufferedProcedureRow) - 4U - sizeof(LogicalPlanReadValue) + sizeof(storage::LogicalGraphNode));
					}
					if (sourceBytes.wouldExceed(bytes)) throw MetadataColumnPressure{};
					sourceBytes.set(bytes);
					const auto labelPayload = [](const storage::LogicalGraphNode &node) {
						size_t payload = 0U;
						for (const auto &label: node.labels) payload = checkedProcedureSizeAdd(payload, label.size());
						return payload;
					};
					const auto cacheBytes = [&](const BufferedProcedureRow &row, const storage::LogicalGraphNode &node, size_t payload) {
						const auto metadata = row.row.entity_metadata_cache.find(request.variable);
						const size_t alias = metadata == row.row.entity_metadata_cache.end() ? request.variable.size() : 0U;
						return checkedProcedureSizeAdd(alias + 6U + sizeof(uint32_t),
							checkedProcedureSizeAdd(payload, checkedProcedureSizeMul(node.labels.size(), sizeof(size_t))));
					};
					// Preflight every publication peak before adding any cache entry.
					size_t plannedRows = rowBytes.bytes();
					size_t plannedSource = bytes;
					const size_t otherBytes = budget.used() - plannedRows - plannedSource;
					for (const auto &row: rows) {
						context.checkGuard();
						const auto index = rowIndex(row);
						if (!index) continue;
						const auto &node = nodes[*index];
						const size_t payload = labelPayload(node);
						if (--consumers[*index] == 0U) plannedSource -= payload;
						plannedRows = checkedProcedureSizeAdd(plannedRows, cacheBytes(row, node, payload));
						if (checkedProcedureSizeAdd(plannedRows, plannedSource) > bounds.maximum_materialized_bytes - otherBytes) throw MetadataColumnPressure{};
					}
					std::fill(consumers.begin(), consumers.end(), 0U);
					for (const auto &row: rows) if (const auto index = rowIndex(row)) ++consumers[*index];
					for (auto &row: rows) {
						context.checkGuard();
						const auto index = rowIndex(row);
						if (!index) continue;
						auto &node = nodes[*index];
						const bool last = --consumers[*index] == 0U;
						const size_t payload = labelPayload(node);
						if (last) sourceBytes.set(sourceBytes.bytes() - payload);
						admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(rowBytes.bytes(), cacheBytes(row, node, payload)));
						std::vector<PropertyValue> labels;
						labels.reserve(node.labels.size());
						for (auto &label: node.labels) {
							if (last) labels.emplace_back(std::move(label));
							else labels.emplace_back(label);
						}
						row.row.entity_metadata_cache[request.variable].insert_or_assign("labels", PropertyValue(std::move(labels)));
					}
					validateBufferedProcedureBytes(rows, bounds.maximum_materialized_bytes);
				};
				std::function<void(size_t, size_t)> processRange;
				processRange = [&](size_t offset, size_t count) {
					try { processOnce(offset, count); }
					catch (const MetadataColumnPressure &) {
						if (count == 1U) throw ProcedureByteBoundExceeded("logical procedure metadata batch exceeded its joint byte bound");
						const size_t first = count / 2U;
						processRange(offset, first);
						processRange(offset + first, count - first);
					}
				};
				const size_t capacity = procedureNodeBatchCapacity(bounds);
				for (size_t offset = 0U; offset < ids.size(); offset += capacity) processRange(offset, std::min(capacity, ids.size() - offset));
			}
		}

		void prefetchProcedureCompleteNodes(ProcedureRows &rows,
				const std::vector<const expressions::Expression *> &expressions,
				storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &bounds,
				const QueryContext &context) {
			const auto variables = LogicalPlanReadRuntime::collectBatchedCompleteEntityVariables(expressions);
			for (const std::string &variable: variables) {
				MaterializedByteBudget budget(bounds.maximum_materialized_bytes);
				MaterializedByteReservation rowBytes(&budget);
				for (const auto &row: rows) admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(rowBytes.bytes(), bufferedProcedureRowBytes(row)));
				MaterializedByteReservation idBytes(&budget);
				size_t referenceCount = 0U;
				for (const BufferedProcedureRow &buffered: rows) {
					context.checkGuard();
					const auto found = buffered.row.values.find(variable);
					if (found == buffered.row.values.end() || buffered.row.complete_entity_property_cache.contains(variable)) continue;
					const auto *reference = std::get_if<storage::EntityRef>(&found->second);
					if (reference == nullptr || reference->is_edge) continue;
					if (reference->entity_id == 0U) throw std::runtime_error("logical procedure complete batch received a zero Node ID");
					referenceCount = checkedProcedureSizeAdd(referenceCount, 1U);
				}
				idBytes.set(checkedProcedureSizeMul(referenceCount, sizeof(uint64_t)));
				utils::OwnedStdVector<uint64_t> ids;
				ids.reserve(referenceCount);
				idBytes.set(checkedProcedureSizeMul(ids.capacity(), sizeof(uint64_t)));
				for (const BufferedProcedureRow &buffered: rows) {
					context.checkGuard();
					const auto found = buffered.row.values.find(variable);
					if (found == buffered.row.values.end()) continue;
					const auto *reference = std::get_if<storage::EntityRef>(&found->second);
					if (reference == nullptr || reference->is_edge || buffered.row.complete_entity_property_cache.contains(variable)) continue;
					if (reference->entity_id == 0U) throw std::runtime_error("logical procedure complete batch received a zero Node ID");
					ids.push_back(reference->entity_id);
				}
				std::ranges::sort(ids);
				ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
				if (ids.size() > bounds.maximum_node_records) {
					throw std::runtime_error("logical procedure complete batch exceeded its Node bound");
				}
				struct CompleteColumnPressure {};
				const auto processOnce = [&](size_t offset, size_t count) {
					context.checkGuard();
					const auto batchIds = std::span<const uint64_t>(ids).subspan(offset, count);
					MaterializedByteReservation consumerBytes(&budget);
					const size_t countBytes = checkedProcedureSizeMul(count, sizeof(size_t));
					if (consumerBytes.wouldExceed(countBytes)) throw CompleteColumnPressure{};
					consumerBytes.set(countBytes);
					utils::OwnedStdVector<size_t> consumers(count, 0U);
					const auto rowIndex = [&](const BufferedProcedureRow &row) -> std::optional<size_t> {
						const auto found = row.row.values.find(variable);
						if (found == row.row.values.end() || row.row.complete_entity_property_cache.contains(variable)) return std::nullopt;
						const auto *reference = std::get_if<storage::EntityRef>(&found->second);
						if (reference == nullptr || reference->is_edge) return std::nullopt;
						const auto position = std::ranges::lower_bound(batchIds, reference->entity_id);
						return position == batchIds.end() || *position != reference->entity_id ? std::nullopt
							: std::optional<size_t>(static_cast<size_t>(position - batchIds.begin()));
					};
					for (const auto &row: rows) if (const auto index = rowIndex(row)) ++consumers[*index];
					MaterializedByteReservation sourceBytes(&budget);
					const bool admitSource = snapshot.supportsShapedPropertyReadAdmission();
					if (admitSource) {
						const size_t nodeFrames = checkedProcedureSizeMul(count, sizeof(storage::LogicalGraphNode));
						if (sourceBytes.wouldExceed(nodeFrames)) throw CompleteColumnPressure{};
						sourceBytes.set(nodeFrames);
					}
					const std::function<void(size_t)> admitProperty = [&](size_t encodedBytes) {
						context.checkGuard();
						const size_t next = checkedProcedureSizeAdd(sourceBytes.bytes(), encodedBytes);
						if (sourceBytes.wouldExceed(next)) throw CompleteColumnPressure{};
						sourceBytes.set(next);
					};
					auto nodes = admitSource
						? snapshot.readNodesShapedSortedWithAdmission(batchIds, completeNodeReadShape(), admitProperty)
						: snapshot.readNodesShapedSorted(batchIds, completeNodeReadShape());
					context.checkGuardNow();
					if (nodes.size() != count) throw std::runtime_error("logical procedure backend changed complete batch cardinality");
					size_t bytes = 0U;
					for (size_t index = 0U; index < count; ++index) {
						context.checkGuard();
						validateNode(nodes[index]);
						if (nodes[index].node_id != batchIds[index]) throw std::runtime_error("logical procedure complete batch changed Node order");
						if (nodes[index].properties.size() > bounds.maximum_property_entries) throw std::runtime_error("logical procedure complete batch exceeded its Property bound");
						std::unordered_set<std::string_view> keys;
						for (const auto &entry: nodes[index].properties) {
							if (entry.key.empty() || !keys.insert(entry.key).second) throw std::runtime_error("logical procedure complete batch has invalid Property keys");
						}
						// Count the temporary Node itself, not an imaginary output row.
						bytes = checkedProcedureSizeAdd(bytes, bufferedProcedureNodeRowBytes(nodes[index], false) -
							sizeof(BufferedProcedureRow) - 4U - sizeof(LogicalPlanReadValue) + sizeof(storage::LogicalGraphNode));
					}
					// Reconcile canonical entry scratch to the returned Node/label/key
					// retention after every returned-record semantic check.
					if (sourceBytes.wouldExceed(bytes)) throw CompleteColumnPressure{};
					sourceBytes.set(bytes);
					// Simulate every publication peak before mutating rows. Reuse the
					// consumer counters; no second payload or count array is needed.
					size_t plannedRows = rowBytes.bytes();
					size_t plannedSource = bytes;
					const size_t otherBytes = budget.used() - plannedRows - plannedSource;
					for (const auto &buffered: rows) {
						context.checkGuard();
						const auto index = rowIndex(buffered);
						if (!index) continue;
						const auto &node = nodes[*index];
						size_t propertyBytes = 0U;
						for (const auto &entry: node.properties) propertyBytes = checkedProcedureSizeAdd(propertyBytes,
							checkedProcedureSizeAdd(entry.key.size(), property_utils::getPropertyValueSize(entry.value)));
						size_t labelPayload = 0U;
						for (const auto &label: node.labels) labelPayload = checkedProcedureSizeAdd(labelPayload, label.size());
						if (--consumers[*index] == 0U) plannedSource -= propertyBytes + labelPayload;
						if (const auto old = buffered.row.property_cache.find(variable); old != buffered.row.property_cache.end()) {
							plannedRows -= variable.size();
							for (const auto &[key, value]: old->second) plannedRows -= key.size() + property_utils::getPropertyValueSize(value);
						}
						plannedRows = checkedProcedureSizeAdd(plannedRows, checkedProcedureSizeAdd(variable.size(), propertyBytes));
						const auto metadata = buffered.row.entity_metadata_cache.find(variable);
						if (metadata == buffered.row.entity_metadata_cache.end()) plannedRows = checkedProcedureSizeAdd(plannedRows, variable.size() + 6U);
						else if (const auto old = metadata->second.find("labels"); old != metadata->second.end()) plannedRows -= property_utils::getPropertyValueSize(old->second);
						else plannedRows = checkedProcedureSizeAdd(plannedRows, 6U);
						plannedRows = checkedProcedureSizeAdd(plannedRows, checkedProcedureSizeAdd(sizeof(uint32_t),
							checkedProcedureSizeAdd(labelPayload, checkedProcedureSizeMul(node.labels.size(), sizeof(size_t)))));
						if (checkedProcedureSizeAdd(plannedRows, plannedSource) > bounds.maximum_materialized_bytes - otherBytes) throw CompleteColumnPressure{};
					}
					std::fill(consumers.begin(), consumers.end(), 0U);
					for (const auto &row: rows) if (const auto index = rowIndex(row)) ++consumers[*index];
					for (auto &buffered: rows) {
						context.checkGuard();
						const auto index = rowIndex(buffered);
						if (!index) continue;
						auto &node = nodes[*index];
						const bool last = --consumers[*index] == 0U;
						// Drop selected/NULL sentinels before replacing with the complete
						// map; account copied bindings before publishing their payload.
						const size_t previous = bufferedProcedureRowBytes(buffered);
						buffered.row.property_cache.erase(variable);
						rowBytes.set(rowBytes.bytes() - previous + bufferedProcedureRowBytes(buffered));
						size_t propertyBytes = 0U;
						for (const auto &entry: node.properties) propertyBytes = checkedProcedureSizeAdd(propertyBytes,
							checkedProcedureSizeAdd(entry.key.size(), property_utils::getPropertyValueSize(entry.value)));
						size_t labelPayload = 0U;
						for (const auto &label: node.labels) labelPayload = checkedProcedureSizeAdd(labelPayload, label.size());
						if (last) sourceBytes.set(sourceBytes.bytes() - propertyBytes - labelPayload);
						size_t next = checkedProcedureSizeAdd(rowBytes.bytes(), checkedProcedureSizeAdd(variable.size(), propertyBytes));
						const auto metadata = buffered.row.entity_metadata_cache.find(variable);
						if (metadata == buffered.row.entity_metadata_cache.end()) next = checkedProcedureSizeAdd(next, variable.size() + 6U);
						else if (const auto existing = metadata->second.find("labels"); existing != metadata->second.end()) next -= property_utils::getPropertyValueSize(existing->second);
						else next = checkedProcedureSizeAdd(next, 6U);
						next = checkedProcedureSizeAdd(next, checkedProcedureSizeAdd(sizeof(uint32_t),
							checkedProcedureSizeAdd(labelPayload, checkedProcedureSizeMul(node.labels.size(), sizeof(size_t)))));
						admitProcedureBytes(rowBytes, next);
						PropertyMap complete;
						for (auto &entry: node.properties) {
							if (last) complete.emplace(std::move(entry.key), std::move(entry.value));
							else complete.emplace(entry.key, entry.value);
						}
						buffered.row.property_cache.insert_or_assign(variable, std::move(complete));
						buffered.row.complete_entity_property_cache.insert(variable);
						std::vector<PropertyValue> labels;
						labels.reserve(node.labels.size());
						for (auto &label: node.labels) {
							if (last) labels.emplace_back(std::move(label));
							else labels.emplace_back(label);
						}
						buffered.row.entity_metadata_cache[variable].insert_or_assign("labels", PropertyValue(std::move(labels)));
					}
					validateBufferedProcedureBytes(rows, bounds.maximum_materialized_bytes);
				};
				std::function<void(size_t, size_t)> processRange;
				processRange = [&](size_t offset, size_t count) {
					try { processOnce(offset, count); }
					catch (const CompleteColumnPressure &) {
						if (count == 1U) throw ProcedureByteBoundExceeded("logical procedure complete batch exceeded its joint byte bound");
						const size_t first = count / 2U;
						processRange(offset, first);
						processRange(offset + first, count - first);
					}
				};
				const size_t capacity = procedureNodeBatchCapacity(bounds);
				for (size_t offset = 0U; offset < ids.size(); offset += capacity) processRange(offset, std::min(capacity, ids.size() - offset));
			}
		}

		// Buffered procedures already allow read-ahead (notably ORDER BY). Use
		// the logical runtime's expression collector and selected-column contract
		// before evaluating each layer; unsupported expressions keep their fallback.
		void prefetchProcedureProperties(ProcedureRows &rows,
				const std::vector<const expressions::Expression *> &expressions,
				storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &bounds,
				const QueryContext &context,
				const std::vector<LogicalPlanBatchedPropertyRequest> &additionalRequests = {}) {
			prefetchProcedureCompleteNodes(rows, expressions, snapshot, bounds, context);
			prefetchProcedureNodeMetadata(rows, expressions, snapshot, bounds, context);
			std::vector<LogicalPlanBatchedPropertyRequest> requests;
			static_cast<void>(LogicalPlanReadRuntime::collectBatchedPropertyRequests(expressions, requests));
			for (const auto &request: additionalRequests) {
				if (std::ranges::find(requests, request) == requests.end()) requests.push_back(request);
			}
			if (requests.empty()) return;
			struct Group {
				std::string variable;
				std::vector<std::string_view> keys;
			};
			std::vector<Group> groups;
			for (const auto &request: requests) {
				auto group = std::ranges::find(groups, request.variable, &Group::variable);
				if (group == groups.end()) {
					groups.push_back({request.variable, {request.key}});
				} else if (std::ranges::find(group->keys, request.key) == group->keys.end()) {
					group->keys.push_back(request.key);
				}
			}
			for (const Group &group: groups) {
				if (group.keys.size() > bounds.maximum_property_entries) {
					throw std::runtime_error("logical procedure Property batch exceeded its key bound");
				}
				for (const bool isEdge: {false, true}) {
					std::vector<std::string_view> keys;
					keys.reserve(group.keys.size());
					for (const std::string_view key: group.keys) {
						bool foundConsumer = false;
						bool cachedByEveryConsumer = true;
						for (const BufferedProcedureRow &buffered: rows) {
							context.checkGuard();
							const auto found = buffered.row.values.find(group.variable);
							if (found == buffered.row.values.end() ||
									buffered.row.complete_entity_property_cache.contains(group.variable)) continue;
							const auto *reference = std::get_if<storage::EntityRef>(&found->second);
							if (reference == nullptr || reference->is_edge != isEdge) continue;
							if (reference->entity_id == 0U) {
								throw std::runtime_error("logical procedure Property batch received a zero Entity ID");
							}
							foundConsumer = true;
							const auto cache = buffered.row.property_cache.find(group.variable);
							if (cache == buffered.row.property_cache.end() || !cache->second.contains(std::string(key))) {
								cachedByEveryConsumer = false;
								break;
							}
						}
						if (!foundConsumer || !cachedByEveryConsumer) keys.push_back(key);
					}
					if (keys.empty()) continue;
					MaterializedByteBudget budget(bounds.maximum_materialized_bytes);
					MaterializedByteReservation rowBytes(&budget);
					for (const auto &row: rows) admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(rowBytes.bytes(), bufferedProcedureRowBytes(row)));
					MaterializedByteReservation keyBytes(&budget);
					admitProcedureBytes(keyBytes, checkedProcedureSizeMul(keys.capacity(), sizeof(std::string_view)));
					MaterializedByteReservation idBytes(&budget);
					const auto uncachedId = [&](const BufferedProcedureRow &buffered) -> std::optional<uint64_t> {
						context.checkGuard();
						const auto found = buffered.row.values.find(group.variable);
						if (found == buffered.row.values.end()) return std::nullopt;
						const auto *reference = std::get_if<storage::EntityRef>(&found->second);
						if (reference == nullptr || reference->is_edge != isEdge || buffered.row.complete_entity_property_cache.contains(group.variable)) return std::nullopt;
						if (reference->entity_id == 0U) {
							throw std::runtime_error("logical procedure Property batch received a zero Entity ID");
						}
						const auto cached = buffered.row.property_cache.find(group.variable);
						if (cached != buffered.row.property_cache.end() &&
							std::ranges::all_of(keys, [&](const std::string_view key) {
								return cached->second.contains(std::string(key));
							})) return std::nullopt;
						return reference->entity_id;
					};
					size_t referenceCount = 0U;
					for (const auto &buffered: rows) {
						if (uncachedId(buffered).has_value()) referenceCount = checkedProcedureSizeAdd(referenceCount, 1U);
					}
					admitProcedureBytes(idBytes, checkedProcedureSizeMul(referenceCount, sizeof(uint64_t)));
					utils::OwnedStdVector<uint64_t> ids;
					ids.reserve(referenceCount);
					admitProcedureBytes(idBytes, checkedProcedureSizeMul(ids.capacity(), sizeof(uint64_t)));
					for (const auto &buffered: rows) {
						if (const auto id = uncachedId(buffered)) ids.push_back(*id);
					}
					std::ranges::sort(ids);
					ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
					// Deduplication keeps the allocation alive at its original capacity.
					const size_t maximumIds = isEdge ? bounds.maximum_edge_records : bounds.maximum_node_records;
					if (ids.size() > maximumIds) {
						throw std::runtime_error("logical procedure Property batch exceeded its Entity bound");
					}
					struct ProcedureColumnBytePressure {};
					const auto processOnce = [&](size_t offset, size_t count) {
						context.checkGuard();
						const auto batch = std::span<const uint64_t>(ids).subspan(offset, count);
						MaterializedByteReservation consumerBytes(&budget);
						const size_t consumerSize = checkedProcedureSizeMul(count, sizeof(size_t));
						if (consumerBytes.wouldExceed(consumerSize)) throw ProcedureColumnBytePressure{};
						consumerBytes.set(consumerSize);
						utils::OwnedStdVector<size_t> consumers(count, 0U);
						const auto rowIndex = [&](const BufferedProcedureRow &buffered) -> std::optional<size_t> {
							const auto found = buffered.row.values.find(group.variable);
							if (found == buffered.row.values.end() || buffered.row.complete_entity_property_cache.contains(group.variable)) return std::nullopt;
							const auto *reference = std::get_if<storage::EntityRef>(&found->second);
							if (reference == nullptr || reference->is_edge != isEdge) return std::nullopt;
							const auto position = std::ranges::lower_bound(batch, reference->entity_id);
							return position == batch.end() || *position != reference->entity_id ? std::nullopt
								: std::optional<size_t>(static_cast<size_t>(position - batch.begin()));
						};
						for (const auto &row: rows) if (const auto index = rowIndex(row)) ++consumers[*index];
						MaterializedByteReservation columnBytes(&budget);
						const size_t structureBytes = checkedProcedureSizeAdd(
							checkedProcedureSizeMul(keys.size(), sizeof(std::vector<PropertyValue>)),
							checkedProcedureSizeMul(checkedProcedureSizeMul(keys.size(), count), sizeof(PropertyValue)));
						if (columnBytes.wouldExceed(structureBytes)) throw ProcedureColumnBytePressure{};
						columnBytes.set(structureBytes);
						const std::function<void(size_t)> admission = [&](size_t bytes) {
							context.checkGuard();
							const size_t next = checkedProcedureSizeAdd(columnBytes.bytes(), bytes);
							if (columnBytes.wouldExceed(next)) throw ProcedureColumnBytePressure{};
							columnBytes.set(next);
						};
						auto columns = snapshot.supportsPropertyReadAdmission()
							? (isEdge ? snapshot.readEdgePropertyColumnsSortedWithAdmission(batch, keys, admission)
								: snapshot.readNodePropertyColumnsSortedWithAdmission(batch, keys, admission))
							: (isEdge ? snapshot.readEdgePropertyColumnsSorted(batch, keys)
								: snapshot.readNodePropertyColumnsSorted(batch, keys));
						if (columns.size() != keys.size() || std::ranges::any_of(columns, [&](const auto &column) {
							return column.size() != count;
						})) throw std::runtime_error("logical procedure backend changed Property batch cardinality");
						size_t retainedBytes = structureBytes;
						for (const auto &column: columns) for (const auto &value: column) {
							retainedBytes = checkedProcedureSizeAdd(retainedBytes, property_utils::getPropertyValueSize(value));
						}
						if (columnBytes.wouldExceed(retainedBytes)) throw ProcedureColumnBytePressure{};
						columnBytes.set(retainedBytes);
						for (BufferedProcedureRow &buffered: rows) {
							context.checkGuard();
							const auto index = rowIndex(buffered);
							if (!index) continue;
							const bool lastConsumer = --consumers[*index] == 0U;
							for (size_t column = 0U; column < keys.size(); ++column) {
								auto &value = columns[column][*index];
								const size_t payload = property_utils::getPropertyValueSize(value);
								const auto cache = buffered.row.property_cache.find(group.variable);
								size_t next = rowBytes.bytes();
								if (cache == buffered.row.property_cache.end()) {
									next = checkedProcedureSizeAdd(next, group.variable.size());
									next = checkedProcedureSizeAdd(next, keys[column].size());
								} else {
									const auto previous = cache->second.find(std::string(keys[column]));
									if (previous == cache->second.end()) next = checkedProcedureSizeAdd(next, keys[column].size());
									else next -= property_utils::getPropertyValueSize(previous->second);
								}
								if (lastConsumer) columnBytes.set(columnBytes.bytes() - payload);
								admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(next, payload));
								auto &target = buffered.row.property_cache[group.variable][std::string(keys[column])];
								if (lastConsumer) target = std::move(value);
								else target = value;
							}
						}
						validateBufferedProcedureBytes(rows, bounds.maximum_materialized_bytes);
					};
					std::function<void(size_t, size_t)> processRange;
					processRange = [&](size_t offset, size_t count) {
						try { processOnce(offset, count); }
						catch (const ProcedureColumnBytePressure &) {
							if (count == 1U) throw ProcedureByteBoundExceeded("logical procedure Property columns exceeded their byte bound (retained rows=" +
								std::to_string(rowBytes.bytes()) + ", IDs=" + std::to_string(idBytes.bytes()) +
								", limit=" + std::to_string(bounds.maximum_materialized_bytes) + ")");
							const size_t first = count / 2U;
							processRange(offset, first);
							processRange(offset + first, count - first);
						}
					};
					const size_t capacity = procedureNodeBatchCapacity(bounds);
					for (size_t offset = 0U; offset < ids.size(); offset += capacity) {
						processRange(offset, std::min(capacity, ids.size() - offset));
					}
					}
				}

		}

		void appendBoundedProcedureRow(ProcedureRows &rows, BufferedProcedureRow row,
									  const size_t maximumRows, const std::string_view procedureName,
									  size_t *retainedBytes = nullptr,
									  const size_t maximumBytes = std::numeric_limits<size_t>::max()) {
			if (maximumRows == 0U) {
				throw std::invalid_argument("logical procedure runtime requires a positive result-row bound");
			}
			if (rows.size() == maximumRows) {
				throw std::runtime_error(std::string(procedureName) + " exceeded its configured result-row bound");
			}
			size_t nextRetainedBytes = 0U;
			if (retainedBytes != nullptr) {
				nextRetainedBytes = checkedProcedureSizeAdd(*retainedBytes, bufferedProcedureRowBytes(row));
				if (nextRetainedBytes > maximumBytes) {
					throw ProcedureByteBoundExceeded("logical procedure materialization exceeded its byte bound");
				}
			}
			rows.push_back(std::move(row));
			if (retainedBytes != nullptr) *retainedBytes = nextRetainedBytes;
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

		[[nodiscard]] QueryResult makeProcedureResult(ProcedureRows rows,
															  const std::vector<std::string> &columns,
															  const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context) {
			if (rows.size() > readBounds.maximum_rows) {
				throw std::runtime_error("logical procedure exceeded its configured result-row bound");
			}
			validateBufferedProcedureBytes(rows, readBounds.maximum_materialized_bytes);
			QueryResult result = makeAdmittedProcedureResult(context);
			result.setColumns(columns);
			for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
				BufferedProcedureRow &buffered = rows[rowIndex];
				context.checkGuard();
				QueryResult::Row row;
				row.reserve(columns.size());
				for (const std::string &column: columns) {
					const auto found = buffered.row.values.find(column);
					if (found == buffered.row.values.end()) {
						throw std::logic_error("logical projection procedure did not bind its declared output column");
					}
					auto *scalar = std::get_if<PropertyValue>(&found->second);
					if (scalar == nullptr) {
						throw std::logic_error("logical projection procedure emitted a non-scalar catalog value");
					}
					row.emplace(column, std::move(*scalar));
				}
				appendProcedureResultRow(result, rows, rowIndex, std::move(row),
						readBounds.maximum_materialized_bytes);
			}
			ProcedureRows().swap(rows);
			validateProcedureResultBudget(result, 0U, readBounds.maximum_materialized_bytes);
			return result;
		}

		[[nodiscard]] ProcedureRows
		makeBufferedShortestPathRows(const std::vector<uint64_t> &path,
									 const LogicalPlanReadRuntimeBounds &bounds) {
			if (path.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("logical shortest-path procedure result cannot represent its row count");
			}
			ProcedureRows rows;
		const size_t retainedPathBytes = checkedProcedureSizeAdd(sizeof(path),
				checkedProcedureSizeMul(path.capacity(), sizeof(uint64_t)));
		const size_t outputByteAllowance = reserveBoundedProcedureRows(rows, path.size(), bounds, retainedPathBytes);
			size_t retainedBytes = 0U;
			for (size_t step = 0U; step < path.size(); ++step) {
				BufferedProcedureRow row;
				row.node_properties.reserve(1U);
				row.row.values.emplace("step", PropertyValue(static_cast<int64_t>(step)));
				row.row.values.emplace("node", storage::EntityRef{path[step], false, 0U, 0U, 0U});
				row.row.values.emplace("path_length", PropertyValue(static_cast<int64_t>(path.size())));
				appendBoundedProcedureRow(rows, std::move(row), bounds.maximum_rows, "algo.shortestPath",
						&retainedBytes, outputByteAllowance);
			}
			return rows;
		}

		void validateVectorSearchHits(const std::span<storage::LogicalGraphVectorSearchHit> hits,
										 const LogicalPlanReadRuntimeBounds &bounds, const QueryContext &context) {
			uint64_t previousNodeId = 0U;
			float previousScore = -std::numeric_limits<float>::infinity();
			for (const storage::LogicalGraphVectorSearchHit &hit: hits) {
				context.checkGuard();
				if (hit.node_id == 0U || !std::isfinite(hit.score) ||
					(hit.score < previousScore || (hit.score == previousScore && hit.node_id <= previousNodeId))) {
					throw std::runtime_error("logical Vector procedure backend returned unordered or invalid search hits");
				}
				previousNodeId = hit.node_id;
				previousScore = hit.score;
			}
			const auto idOrder = [&context](const auto &left, const auto &right) {
				context.checkGuard();
				return left.node_id < right.node_id;
			};
			std::ranges::sort(hits, idOrder);
			for (size_t index = 1U; index < hits.size(); ++index) {
				context.checkGuard();
				if (hits[index - 1U].node_id == hits[index].node_id) {
					throw std::runtime_error("logical Vector procedure backend returned unordered or invalid search hits");
				}
			}
			std::ranges::sort(hits, [&context](const auto &left, const auto &right) {
				context.checkGuard();
				return left.score < right.score || (left.score == right.score && left.node_id < right.node_id);
			});
		}

		struct ProcedureNodeOutputReference {
			uint64_t node_id = 0U;
			size_t row_index = 0U;
			size_t column_index = 0U;
		};

		// Keep procedure rows as logical references until a declared output column
		// actually needs a complete Node. This avoids opening Property roots for
		// scalar-only projections while preserving one sorted batch for bare Nodes.
		void materializeProcedureNodeOutputs(ProcedureRows &rows,
											 const std::vector<std::string> &columns,
											 storage::LogicalGraphReadSnapshot &snapshot,
											 const LogicalPlanReadRuntimeBounds &readBounds,
											 const QueryContext &context) {
			MaterializedByteBudget budget(readBounds.maximum_materialized_bytes);
			MaterializedByteReservation rowBytes(&budget);
			for (const auto &row: rows) admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(rowBytes.bytes(), bufferedProcedureRowBytes(row)));
			MaterializedByteReservation referenceBytes(&budget);
			const auto visitOutputReferences = [&](auto &&visitor) {
				for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
					context.checkGuard();
					for (size_t columnIndex = 0U; columnIndex < columns.size(); ++columnIndex) {
						const auto found = rows[rowIndex].row.values.find(columns[columnIndex]);
						if (found == rows[rowIndex].row.values.end()) continue;
						const auto *reference = std::get_if<storage::EntityRef>(&found->second);
						if (reference == nullptr) continue;
						if (reference->is_edge || reference->entity_id == 0U) {
							throw std::logic_error("logical procedure output contains an invalid Node reference");
						}
						visitor(ProcedureNodeOutputReference{reference->entity_id, rowIndex, columnIndex});
					}
				}
			};
			size_t referenceCount = 0U;
			visitOutputReferences([&](const ProcedureNodeOutputReference &) {
				referenceCount = checkedProcedureSizeAdd(referenceCount, 1U);
			});
			referenceBytes.set(checkedProcedureSizeMul(referenceCount, sizeof(ProcedureNodeOutputReference)));
			std::vector<ProcedureNodeOutputReference, utils::OwnedPmrAllocator<ProcedureNodeOutputReference>> references;
			references.reserve(referenceCount);
			referenceBytes.set(checkedProcedureSizeMul(references.capacity(), sizeof(ProcedureNodeOutputReference)));
			visitOutputReferences([&](const ProcedureNodeOutputReference &reference) {
				references.push_back(reference);
			});
			if (references.empty()) return;
			std::ranges::sort(references, [](const ProcedureNodeOutputReference &left, const ProcedureNodeOutputReference &right) {
				if (left.node_id != right.node_id) return left.node_id < right.node_id;
				if (left.row_index != right.row_index) return left.row_index < right.row_index;
				return left.column_index < right.column_index;
			});
			MaterializedByteReservation idBytes(&budget);
			size_t uniqueIdCount = 0U;
			for (size_t index = 0U; index < references.size(); ++index) {
				if (index == 0U || references[index - 1U].node_id != references[index].node_id) {
					uniqueIdCount = checkedProcedureSizeAdd(uniqueIdCount, 1U);
				}
			}
			idBytes.set(checkedProcedureSizeMul(uniqueIdCount, sizeof(uint64_t)));
			utils::OwnedStdVector<uint64_t> sortedIds;
			sortedIds.reserve(uniqueIdCount);
			idBytes.set(checkedProcedureSizeMul(sortedIds.capacity(), sizeof(uint64_t)));
			for (const auto &reference: references) {
				if (sortedIds.empty() || sortedIds.back() != reference.node_id) {
					sortedIds.push_back(reference.node_id);
				}
			}
			const auto nodeBytes = [](const storage::LogicalGraphNode &node) {
				return bufferedProcedureNodeRowBytes(node, false) - sizeof(BufferedProcedureRow) - 4U -
					sizeof(LogicalPlanReadValue) + sizeof(storage::LogicalGraphNode);
			};
			struct NodeOutputPressure { const char *phase; size_t requested; };
			const auto processOnce = [&](size_t offset, size_t count) {
				context.checkGuard();
				const auto batch = std::span<const uint64_t>(sortedIds).subspan(offset, count);
				MaterializedByteReservation sourceBytes(&budget);
				const bool admitSource = snapshot.supportsShapedPropertyReadAdmission();
				if (admitSource) {
					const size_t frames = checkedProcedureSizeMul(count, sizeof(storage::LogicalGraphNode));
					if (sourceBytes.wouldExceed(frames)) throw NodeOutputPressure{"source frames", frames};
					sourceBytes.set(frames);
				}
				const std::function<void(size_t)> admission = [&](size_t encodedBytes) {
					context.checkGuard();
					const size_t next = checkedProcedureSizeAdd(sourceBytes.bytes(), encodedBytes);
					if (sourceBytes.wouldExceed(next)) throw NodeOutputPressure{"encoded Property", next};
					sourceBytes.set(next);
				};
				auto nodes = admitSource ? snapshot.readNodesShapedSortedWithAdmission(batch, completeNodeReadShape(), admission)
					: snapshot.readNodesShapedSorted(batch, completeNodeReadShape());
				context.checkGuardNow();
				if (nodes.size() != count) throw std::runtime_error("logical procedure Node batch changed its cardinality");
				size_t bytes = 0U;
				for (size_t index = 0U; index < count; ++index) {
					context.checkGuard();
					const auto &node = nodes[index];
					if (node.node_id != batch[index]) throw std::runtime_error("logical procedure Node batch changed logical ID order");
					validateNode(node);
					if (node.properties.size() > readBounds.maximum_property_entries) throw std::runtime_error("logical Vector procedure Node Properties exceeded its caller-supplied bound");
					std::unordered_set<std::string_view> keys;
					for (const auto &entry: node.properties) if (entry.key.empty() || !keys.insert(entry.key).second)
						throw std::runtime_error("logical Vector procedure read an invalid Node Property entry set");
					bytes = checkedProcedureSizeAdd(bytes, nodeBytes(node));
				}
				if (sourceBytes.wouldExceed(bytes)) throw NodeOutputPressure{"returned Node", bytes};
				sourceBytes.set(bytes);
				const auto firstReference = std::ranges::lower_bound(references, batch.front(), {}, &ProcedureNodeOutputReference::node_id);
				const size_t first = static_cast<size_t>(firstReference - references.begin());
				size_t referenceIndex = first;
				size_t plannedRows = rowBytes.bytes();
				size_t plannedSource = bytes;
				const size_t otherBytes = budget.used() - plannedRows - plannedSource;
				// Validate and admit the whole transition before touching any output row.
				for (const auto &node: nodes) {
					const size_t payload = nodeBytes(node) - sizeof(storage::LogicalGraphNode);
					while (referenceIndex < references.size() && references[referenceIndex].node_id == node.node_id) {
						context.checkGuard();
						++referenceIndex;
						const bool last = referenceIndex == references.size() || references[referenceIndex].node_id != node.node_id;
						if (last) plannedSource -= payload;
						plannedRows = checkedProcedureSizeAdd(plannedRows - sizeof(storage::EntityRef), payload);
						if (checkedProcedureSizeAdd(plannedRows, plannedSource) > readBounds.maximum_materialized_bytes - otherBytes) throw NodeOutputPressure{"publication", checkedProcedureSizeAdd(plannedRows, plannedSource)};
					}
				}
				referenceIndex = first;
				for (auto &node: nodes) {
					const size_t payload = nodeBytes(node) - sizeof(storage::LogicalGraphNode);
					while (referenceIndex < references.size() && references[referenceIndex].node_id == node.node_id) {
						context.checkGuard();
						const auto &reference = references[referenceIndex++];
						const bool last = referenceIndex == references.size() || references[referenceIndex].node_id != node.node_id;
						if (last) sourceBytes.set(sourceBytes.bytes() - payload);
						admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(rowBytes.bytes() - sizeof(storage::EntityRef), payload));
						auto &value = rows[reference.row_index].row.values.at(columns[reference.column_index]);
						if (last) value = std::move(node);
						else value = node;
					}
				}
			};
			std::function<void(size_t, size_t)> processRange;
			processRange = [&](size_t offset, size_t count) {
				try { processOnce(offset, count); }
				catch (const NodeOutputPressure &pressure) {
					if (count == 1U) throw ProcedureByteBoundExceeded("logical procedure Node output exceeded its joint byte bound: phase=" + std::string(pressure.phase) +
						", requested=" + std::to_string(pressure.requested) + ", rows=" + std::to_string(rowBytes.bytes()) +
						", routing=" + std::to_string(referenceBytes.bytes() + idBytes.bytes()) + ", limit=" + std::to_string(readBounds.maximum_materialized_bytes));
					const size_t first = count / 2U;
					processRange(offset, first);
					processRange(offset + first, count - first);
				}
			};
			const size_t capacity = procedureNodeBatchCapacity(readBounds);
			for (size_t offset = 0U; offset < sortedIds.size(); offset += capacity) processRange(offset, std::min(capacity, sortedIds.size() - offset));
		}

		[[nodiscard]] ProcedureRows
		makeBufferedVectorProcedureRows(const std::span<const storage::LogicalGraphVectorSearchHit> hits,
										const QueryContext &context) {
			ProcedureRows rows;
			rows.reserve(hits.size());
			for (const auto &hit: hits) {
				context.checkGuard();
				BufferedProcedureRow buffered;
				buffered.node_properties.reserve(1U);
				buffered.row.values.emplace("node", storage::EntityRef{hit.node_id, false, 0U, 0U, 0U});
				buffered.row.values.emplace("score", PropertyValue(static_cast<double>(hit.score)));
				rows.push_back(std::move(buffered));
			}
			return rows;
		}

		[[nodiscard]] bool appendBufferedProcedureRows(ProcedureRows rows, storage::LogicalGraphReadSnapshot &snapshot,
								 const std::vector<const logical::LogicalOperator *> &operations,
								 const LogicalPlanReadRuntimeBounds &readBounds,
								 const QueryContext &context, QueryResult &result,
								 std::vector<std::unordered_set<std::string>> *distinctRowsByOperation = nullptr,
								 std::vector<uint64_t> *operatorRowCounts = nullptr,
								 const size_t retainedSourceBytes = 0U,
								 const bool accountOutputTransition = false) {
			LogicalPlanReadRuntimeBounds batchBounds = readBounds;
			const size_t retainedBytes = checkedProcedureSizeAdd(
					retainedSourceBytes, procedureQueryResultBytes(result));
		if (retainedBytes > readBounds.maximum_materialized_bytes) {
			throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
		}
		batchBounds.maximum_materialized_bytes -= retainedBytes;
			if (rows.size() > readBounds.maximum_rows) {
				throw std::runtime_error("logical procedure exceeded its configured result-row bound");
			}
			validateBufferedProcedureBytes(rows, batchBounds.maximum_materialized_bytes);
			utils::OwnedStdVector<uint64_t> localOperatorRowCounts(operations.size(), 0U);
			if (operatorRowCounts == nullptr) {
				operatorRowCounts = &localOperatorRowCounts;
			} else if (operatorRowCounts->size() != operations.size()) {
				throw std::logic_error("logical procedure operator state does not match its pipeline");
			}
		bool continueInput = true;

			for (size_t operationIndex = 0U; operationIndex < operations.size(); ++operationIndex) {
				const logical::LogicalOperator *operation = operations[operationIndex];
				context.checkGuard();
				if (rows.empty()) {
					break;
				}
				switch (operation->getType()) {
					case logical::LogicalOpType::LOP_PROJECT: {
						const auto &projection = dynamic_cast<const logical::LogicalProject &>(*operation);
						utils::OwnedStdVector<const expressions::Expression *> propertyExpressions;
						for (const auto &item: projection.getItems()) {
							propertyExpressions.push_back(item.expression.get());
						}
						prefetchProcedureProperties(rows, propertyExpressions, snapshot, batchBounds, context);
						std::unordered_set<std::string> localDistinctRows;
						std::unordered_set<std::string> *distinctRows = nullptr;
						if (projection.isDistinct()) {
							if (distinctRowsByOperation != nullptr) {
								if (distinctRowsByOperation->size() != operations.size()) {
									throw std::logic_error("logical procedure DISTINCT state does not match its operator pipeline");
								}
								distinctRows = &distinctRowsByOperation->at(operationIndex);
							} else {
								distinctRows = &localDistinctRows;
							}
						}
						size_t outputCount = 0U;
						size_t pendingSourceBytes = 0U;
						for (const BufferedProcedureRow &buffered: rows) {
							pendingSourceBytes = checkedProcedureSizeAdd(pendingSourceBytes,
									bufferedProcedureRowBytes(buffered));
						}
						size_t retainedOutputBytes = 0U;
						for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
							BufferedProcedureRow &buffered = rows[rowIndex];
							const size_t currentSourceBytes = bufferedProcedureRowBytes(buffered);
							if (currentSourceBytes > pendingSourceBytes) {
								throw std::logic_error("logical procedure pending source-byte accounting underflowed");
							}
							pendingSourceBytes -= currentSourceBytes;
							size_t retainedOtherBytes = checkedProcedureSizeAdd(retainedOutputBytes, pendingSourceBytes);
							// Rows not yet projected remain owned by the input batch while
							// completed outputs accumulate beside it. Include those live source
							// rows in the same allowance passed to this row's expressions.
							std::optional<LogicalPlanReadRow> projected = applyProjection(
								projection, snapshot, batchBounds, context, buffered.row, buffered.node_properties,
									distinctRows, retainedOtherBytes, true);
							if (projected.has_value()) {
								buffered.row = std::move(*projected);
								retainedOutputBytes = checkedProcedureSizeAdd(retainedOutputBytes,
										bufferedProcedureRowBytes(buffered));
								if (outputCount != rowIndex) rows[outputCount] = std::move(buffered);
								++outputCount;
							} else {
								// DISTINCT drops release payload before processing later rows.
								buffered = BufferedProcedureRow{};
							}
						}
						rows.resize(outputCount);
						validateBufferedProcedureBytes(rows, batchBounds.maximum_materialized_bytes);
						break;
					}
					case logical::LogicalOpType::LOP_FILTER: {
						const auto &filter = dynamic_cast<const logical::LogicalFilter &>(*operation);
						prefetchProcedureProperties(rows, {filter.getPredicate().get()}, snapshot, batchBounds, context);
						size_t outputCount = 0U;
						for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
							context.checkGuard();
							BufferedProcedureRow &buffered = rows[rowIndex];
							if (applyFilter(filter, snapshot, batchBounds, context, buffered.row,
											buffered.node_properties)) {
								if (outputCount != rowIndex) rows[outputCount] = std::move(buffered);
								++outputCount;
							} else {
								buffered = BufferedProcedureRow{};
							}
						}
						rows.resize(outputCount);
						validateBufferedProcedureBytes(rows, batchBounds.maximum_materialized_bytes);
						break;
					}
					case logical::LogicalOpType::LOP_SKIP: {
						const int64_t offset = dynamic_cast<const logical::LogicalSkip &>(*operation).getOffset();
						if (offset < 0) {
							throw std::invalid_argument("logical Vector procedure SKIP cannot be negative");
						}
						uint64_t &skippedRows = operatorRowCounts->at(operationIndex);
						const uint64_t remainingSkip = static_cast<uint64_t>(offset) -
								std::min(static_cast<uint64_t>(offset), skippedRows);
						const size_t skip = static_cast<size_t>(std::min<uint64_t>(remainingSkip, rows.size()));
						skippedRows += static_cast<uint64_t>(skip);
						rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(skip));
						break;
					}
					case logical::LogicalOpType::LOP_LIMIT: {
						const int64_t limit = dynamic_cast<const logical::LogicalLimit &>(*operation).getLimit();
						if (limit < 0) {
							throw std::invalid_argument("logical Vector procedure LIMIT cannot be negative");
						}
						uint64_t &emittedRows = operatorRowCounts->at(operationIndex);
						const uint64_t limitValue = static_cast<uint64_t>(limit);
						const uint64_t remaining = limitValue - std::min(limitValue, emittedRows);
						const size_t maximum = static_cast<size_t>(std::min<uint64_t>(remaining, rows.size()));
						rows.resize(maximum);
						emittedRows += static_cast<uint64_t>(maximum);
						if (emittedRows >= limitValue) {
							continueInput = false;
						}
						break;
					}
					case logical::LogicalOpType::LOP_SORT: {
						const auto &sort = dynamic_cast<const logical::LogicalSort &>(*operation);
						const std::vector<logical::LogicalSortItem> &items = sort.getSortItems();
						if (items.empty()) {
							throw std::invalid_argument("logical Vector procedure Sort has no sort items");
						}
						utils::OwnedStdVector<const expressions::Expression *> propertyExpressions;
						for (const auto &item: items) {
							propertyExpressions.push_back(item.expression.get());
						}
						prefetchProcedureProperties(rows, propertyExpressions, snapshot, batchBounds, context);
						struct DecoratedRow {
							std::vector<PropertyValue> keys;
							size_t sequence = 0U;
						};
						MaterializedByteBudget sortBudget(batchBounds.maximum_materialized_bytes);
						MaterializedByteReservation retainedRows(&sortBudget);
						for (const auto &buffered: rows) {
							retainedRows.set(checkedProcedureSizeAdd(retainedRows.bytes(), bufferedProcedureRowBytes(buffered)));
						}
						MaterializedByteReservation retainedKeys(&sortBudget);
						// Only indices and keys are copied; row payload stays in its buffer.
						MaterializedByteReservation rowSlack(&sortBudget);
						rowSlack.set(checkedProcedureSizeMul(rows.capacity() - rows.size(), sizeof(BufferedProcedureRow)));
						retainedKeys.set(checkedProcedureSizeMul(rows.size(), checkedProcedureSizeAdd(
							sizeof(DecoratedRow),
							checkedProcedureSizeMul(items.size(), sizeof(PropertyValue)))));
						std::vector<DecoratedRow, utils::OwnedPmrAllocator<DecoratedRow>> decoratedRows;
						decoratedRows.reserve(rows.size());
						retainedKeys.set(checkedProcedureSizeAdd(retainedKeys.bytes(),
							checkedProcedureSizeMul(decoratedRows.capacity() - rows.size(), sizeof(DecoratedRow))));
						for (BufferedProcedureRow &buffered: rows) {
							DecoratedRow decorated{{}, decoratedRows.size()};
							decorated.keys.reserve(items.size());
							retainedKeys.set(checkedProcedureSizeAdd(retainedKeys.bytes(),
								checkedProcedureSizeMul(decorated.keys.capacity() - items.size(), sizeof(PropertyValue))));
							for (const logical::LogicalSortItem &item: items) {
								if (!item.expression) {
									throw std::invalid_argument(
											"logical Vector procedure Sort has an empty sort expression");
								}
								const size_t previousRowBytes = bufferedProcedureRowBytes(buffered);
								LogicalPlanReadValue value =
										LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
											 snapshot, *item.expression, buffered.row, batchBounds, context);
								auto *key = std::get_if<PropertyValue>(&value);
								if (key == nullptr) {
									throw std::runtime_error("logical Vector procedure Sort requires scalar sort keys");
								}
								retainedRows.set(checkedProcedureSizeAdd(retainedRows.bytes() - previousRowBytes,
									bufferedProcedureRowBytes(buffered)));
								retainedKeys.set(checkedProcedureSizeAdd(retainedKeys.bytes(), property_utils::getPropertyValueSize(*key)));
								decorated.keys.push_back(std::move(*key));
							}
							decoratedRows.push_back(std::move(decorated));
						}
						std::sort(decoratedRows.begin(), decoratedRows.end(),
										 [&context, &items](const DecoratedRow &left, const DecoratedRow &right) {
											 context.checkGuard();
											 for (size_t index = 0U; index < items.size(); ++index) {
												 if (left.keys[index] == right.keys[index]) {
													 continue;
												 }
												 return detail::orderByPrecedes(left.keys[index], right.keys[index], items[index].ascending);
											 }
											 // Explicit input sequence preserves stable ties while in-place
											 // sorting avoids a second buffered-row array.
											 return left.sequence < right.sequence;
										 });
						// Release keys before rotating permutation cycles. One moved row
						// frame suffices; no second row array or copied payload is needed.
						for (auto &decorated: decoratedRows) std::vector<PropertyValue>().swap(decorated.keys);
						retainedKeys.set(checkedProcedureSizeMul(decoratedRows.capacity(), sizeof(DecoratedRow)));
						MaterializedByteReservation rotationFrame(&sortBudget);
						for (size_t index = 0U; index < rows.size(); ++index) {
							context.checkGuard();
							if (decoratedRows[index].sequence == index) continue;
							rotationFrame.set(sizeof(BufferedProcedureRow));
							BufferedProcedureRow saved = std::move(rows[index]);
							size_t destination = index;
							while (decoratedRows[destination].sequence != index) {
								context.checkGuard();
								const size_t source = decoratedRows[destination].sequence;
								rows[destination] = std::move(rows[source]);
								decoratedRows[destination].sequence = destination;
								destination = source;
							}
							rows[destination] = std::move(saved);
							decoratedRows[destination].sequence = destination;
							rotationFrame.reset();
						}
						validateBufferedProcedureBytes(rows, batchBounds.maximum_materialized_bytes);
						break;
					}
					default: // ZYX_COV_EXCL_LINE: execute() admits only the five unary types dispatched above.
						throw std::logic_error("logical Vector procedure runtime received an unsupported operation");
				}
			}
			materializeProcedureNodeOutputs(rows, result.getColumns(), snapshot, batchBounds, context);
			validateBufferedProcedureBytes(rows, batchBounds.maximum_materialized_bytes);

			for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
				BufferedProcedureRow &buffered = rows[rowIndex];
				context.checkGuard();
				QueryResult::Row output;
				output.reserve(buffered.row.values.size());
				for (const std::string &column: result.getColumns()) {
					const auto value = buffered.row.values.find(column);
					if (value == buffered.row.values.end() || // ZYX_COV_EXCL_LINE: output variables are derived from
															  // the row-producing operators.
						!output.emplace(column, materializeResultValue(value->second, snapshot, batchBounds, context,
																	   buffered.node_properties))
								 .second) {
						throw std::logic_error(
								"logical Vector procedure projection did not bind a declared output column");
					}
				}
				if (accountOutputTransition) {
					appendProcedureResultRow(result, rows, rowIndex, std::move(output),
							readBounds.maximum_materialized_bytes, retainedSourceBytes);
				} else {
					result.addRow(std::move(output));
				}
			}
			return continueInput;
		}

		[[nodiscard]] QueryResult
		executeBufferedProcedure(ProcedureRows rows, storage::LogicalGraphReadSnapshot &snapshot,
				const std::vector<const logical::LogicalOperator *> &operations,
				const logical::LogicalOperator &root, const LogicalPlanReadRuntimeBounds &readBounds,
				const QueryContext &context, const size_t retainedSourceBytes = 0U,
			const std::vector<std::string> *outputColumns = nullptr,
			const bool accountOutputTransition = false) {
			QueryResult result = makeAdmittedProcedureResult(context);
			result.setColumns(outputColumns == nullptr ? root.getOutputVariables() : *outputColumns);
			static_cast<void>(appendBufferedProcedureRows(
					std::move(rows), snapshot, operations, readBounds, context, result, nullptr, nullptr, retainedSourceBytes,
					accountOutputTransition));
			validateProcedureResultBudget(result, retainedSourceBytes, readBounds.maximum_materialized_bytes);
			return result;
		}

		[[nodiscard]] QueryResult
		executeBufferedVectorProcedure(std::vector<storage::LogicalGraphVectorSearchHit> &hits,
									   storage::LogicalGraphVectorSearchSnapshot &snapshot,
									   const std::vector<const logical::LogicalOperator *> &operations,
									   const logical::LogicalOperator &root,
									   const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context,
									   const size_t retainedRequestBytes) {
			validateVectorSearchHits(hits, readBounds, context);
			const size_t retainedHitBytes = checkedProcedureSizeMul(
				hits.capacity(), sizeof(storage::LogicalGraphVectorSearchHit));
			const size_t retainedSourceBytes = checkedProcedureSizeAdd(retainedRequestBytes, retainedHitBytes);
			const size_t outputRowSlots = checkedProcedureSizeMul(hits.size(), sizeof(BufferedProcedureRow));
			if (retainedSourceBytes > readBounds.maximum_materialized_bytes ||
				outputRowSlots > readBounds.maximum_materialized_bytes - retainedSourceBytes) {
				throw ProcedureByteBoundExceeded("logical Vector procedure source and output slots exceeded their byte bound");
			}
			return executeBufferedProcedure(makeBufferedVectorProcedureRows(hits, context), snapshot,
													operations, root, readBounds, context, retainedSourceBytes, nullptr, true);
		}

		struct VectorProcedurePostSkipWindowFilter {
			size_t first_filter_index = 0U;
			size_t limit_index = 0U;
			size_t result_limit = 0U;
		};

		[[nodiscard]] std::optional<VectorProcedurePostSkipWindowFilter> vectorProcedurePostSkipFilterLimit(
				const std::vector<const logical::LogicalOperator *> &operations) {
			for (size_t index = 1U; index < operations.size(); ++index) {
				if (operations[index]->getType() != logical::LogicalOpType::LOP_SKIP) continue;
				size_t firstFilterIndex = operations.size();
				size_t limitIndex = index + 1U;
				for (; limitIndex < operations.size(); ++limitIndex) {
					const logical::LogicalOperator *operation = operations[limitIndex];
					if (operation->getType() == logical::LogicalOpType::LOP_FILTER) {
						if (firstFilterIndex == operations.size()) firstFilterIndex = limitIndex;
						continue;
					}
					if (operation->getType() == logical::LogicalOpType::LOP_PROJECT &&
							!dynamic_cast<const logical::LogicalProject &>(*operation).isDistinct()) continue;
					break;
				}
				if (firstFilterIndex == operations.size() || limitIndex == operations.size() ||
						operations[limitIndex]->getType() != logical::LogicalOpType::LOP_LIMIT) {
					continue;
				}
				const int64_t limit = dynamic_cast<const logical::LogicalLimit &>(*operations[limitIndex]).getLimit();
				if (limit < 0) throw std::invalid_argument("logical Vector procedure LIMIT cannot be negative");
				for (size_t suffix = limitIndex + 1U; suffix < operations.size(); ++suffix) {
					if (operations[suffix]->getType() != logical::LogicalOpType::LOP_PROJECT ||
							dynamic_cast<const logical::LogicalProject &>(*operations[suffix]).isDistinct()) {
						return std::nullopt;
					}
				}
				return VectorProcedurePostSkipWindowFilter{firstFilterIndex, limitIndex, static_cast<size_t>(limit)};
			}
			return std::nullopt;
		}

		// DISTINCT top-K may stop after the requested output prefix only when
		// every sort key is determined by a projected expression. Compare the
		// expression tree structurally for the pure, side-effect-free expression
		// forms supported here; unknown expression types keep the full-sort path.
		[[nodiscard]] bool sameDistinctTopKExpression(const expressions::Expression &left,
											 const expressions::Expression &right) {
			if (left.getExpressionType() != right.getExpressionType()) return false;
			switch (left.getExpressionType()) {
			case expressions::ExpressionType::VARIABLE_REFERENCE:
			case expressions::ExpressionType::PROPERTY_ACCESS: {
				const auto *leftReference = dynamic_cast<const expressions::VariableReferenceExpression *>(&left);
				const auto *rightReference = dynamic_cast<const expressions::VariableReferenceExpression *>(&right);
				return leftReference != nullptr && rightReference != nullptr &&
						leftReference->getVariableName() == rightReference->getVariableName() &&
						leftReference->hasProperty() == rightReference->hasProperty() &&
						(!leftReference->hasProperty() ||
						 leftReference->getPropertyName() == rightReference->getPropertyName());
			}
			case expressions::ExpressionType::BINARY_OP: {
				const auto *leftBinary = dynamic_cast<const expressions::BinaryOpExpression *>(&left);
				const auto *rightBinary = dynamic_cast<const expressions::BinaryOpExpression *>(&right);
				return leftBinary != nullptr && rightBinary != nullptr &&
						leftBinary->getOperator() == rightBinary->getOperator() &&
						leftBinary->getLeft() != nullptr && rightBinary->getLeft() != nullptr &&
						leftBinary->getRight() != nullptr && rightBinary->getRight() != nullptr &&
						sameDistinctTopKExpression(*leftBinary->getLeft(), *rightBinary->getLeft()) &&
						sameDistinctTopKExpression(*leftBinary->getRight(), *rightBinary->getRight());
			}
			case expressions::ExpressionType::UNARY_OP: {
				const auto *leftUnary = dynamic_cast<const expressions::UnaryOpExpression *>(&left);
				const auto *rightUnary = dynamic_cast<const expressions::UnaryOpExpression *>(&right);
				return leftUnary != nullptr && rightUnary != nullptr &&
						leftUnary->getOperator() == rightUnary->getOperator() &&
						leftUnary->getOperand() != nullptr && rightUnary->getOperand() != nullptr &&
						sameDistinctTopKExpression(*leftUnary->getOperand(), *rightUnary->getOperand());
			}
			case expressions::ExpressionType::LITERAL: {
				const auto *leftLiteral = dynamic_cast<const expressions::LiteralExpression *>(&left);
				const auto *rightLiteral = dynamic_cast<const expressions::LiteralExpression *>(&right);
				if (leftLiteral == nullptr || rightLiteral == nullptr) return false;
				if (leftLiteral->isNull()) return rightLiteral->isNull();
				if (leftLiteral->isBoolean()) {
					return rightLiteral->isBoolean() && leftLiteral->getBooleanValue() == rightLiteral->getBooleanValue();
				}
				if (leftLiteral->isInteger()) {
					return rightLiteral->isInteger() && leftLiteral->getIntegerValue() == rightLiteral->getIntegerValue();
				}
				if (leftLiteral->isDouble()) {
				if (!rightLiteral->isDouble()) return false;
				const double leftValue = leftLiteral->getDoubleValue();
				const double rightValue = rightLiteral->getDoubleValue();
				return leftValue == rightValue && std::signbit(leftValue) == std::signbit(rightValue);
				}
				if (leftLiteral->isString()) {
					return rightLiteral->isString() && leftLiteral->getStringValue() == rightLiteral->getStringValue();
				}
				return leftLiteral->isPropertyValue() && rightLiteral->isPropertyValue() &&
						leftLiteral->getPropertyValue() == rightLiteral->getPropertyValue();
			}
			default:
				return false;
			}
		}

		[[nodiscard]] const logical::LogicalProject *vectorProcedureDistinctTopKProject(
				const std::vector<const logical::LogicalOperator *> &operations) {
			if (operations.size() < 3U || operations[0]->getType() != logical::LogicalOpType::LOP_SORT ||
				operations[1]->getType() != logical::LogicalOpType::LOP_PROJECT) return nullptr;
			const auto *projection = &dynamic_cast<const logical::LogicalProject &>(*operations[1]);
			if (!projection->isDistinct() || projection->getItems().empty()) return nullptr;
			bool hasSkip = false;
			bool hasLimit = false;
			for (size_t index = 2U; index < operations.size(); ++index) {
				const logical::LogicalOpType type = operations[index]->getType();
				if (type == logical::LogicalOpType::LOP_SKIP && !hasLimit) {
					hasSkip = true;
					continue;
				}
				if (type == logical::LogicalOpType::LOP_FILTER && hasSkip && !hasLimit) {
					continue;
				}
				if (type == logical::LogicalOpType::LOP_LIMIT && !hasLimit) {
					hasLimit = true;
					continue;
				}
				if (type == logical::LogicalOpType::LOP_PROJECT &&
						!dynamic_cast<const logical::LogicalProject &>(*operations[index]).isDistinct()) continue;
				return nullptr;
			}
			if (!hasLimit) return nullptr;
			const auto &sort = dynamic_cast<const logical::LogicalSort &>(*operations[0]);
			if (sort.getSortItems().empty()) return nullptr;
			for (const logical::LogicalSortItem &sortItem: sort.getSortItems()) {
				if (!sortItem.expression) return nullptr;
				const bool represented = std::ranges::any_of(projection->getItems(), [&](const logical::LogicalProjectItem &item) {
					return item.expression != nullptr &&
							sameDistinctTopKExpression(*item.expression, *sortItem.expression);
				});
				if (!represented) return nullptr;
			}
			return projection;
		}

		[[nodiscard]] std::optional<size_t> vectorProcedureSortPrefix(
				const std::vector<const logical::LogicalOperator *> &operations, const size_t totalRows,
				const size_t maximumRows, const size_t maximumDistinctRows) {
			if (operations.empty() || operations.front()->getType() != logical::LogicalOpType::LOP_SORT) {
				return std::nullopt;
			}
			size_t remaining = totalRows;
			size_t requiredPrefix = 0U;
			size_t filtersBeforeWindow = 0U;
			bool firstOperationIsProject = false;
			bool firstOperationIsFilter = false;
			bool secondOperationIsProject = false;
			const logical::LogicalProject *distinctTopKProject = vectorProcedureDistinctTopKProject(operations);
			const std::optional<VectorProcedurePostSkipWindowFilter> postSkipWindowFilter =
					vectorProcedurePostSkipFilterLimit(operations);
			for (size_t index = 1U; index < operations.size(); ++index) {
				const logical::LogicalOperator *operation = operations[index];
				switch (operation->getType()) {
				case logical::LogicalOpType::LOP_FILTER:
					// Row-local Filters can run immediately after Sort, after one
					// Project, on both sides of that Project before the window, or
					// after SKIP when the top-K prefix expands to satisfy the later LIMIT.
					if (!((postSkipWindowFilter.has_value() && index >= postSkipWindowFilter->first_filter_index &&
							index < postSkipWindowFilter->limit_index) ||
						(index == 1U && filtersBeforeWindow == 0U) ||
						(index == 2U && firstOperationIsProject && filtersBeforeWindow == 0U) ||
						(index == 3U && firstOperationIsFilter && secondOperationIsProject &&
							filtersBeforeWindow == 1U))) {
						return std::nullopt;
					}
					++filtersBeforeWindow;
					if (index == 1U) firstOperationIsFilter = true;
					break;
				case logical::LogicalOpType::LOP_PROJECT:
					if (dynamic_cast<const logical::LogicalProject &>(*operation).isDistinct()) {
						if (distinctTopKProject == nullptr || index != 1U) return std::nullopt;
					}
					if (index == 1U) firstOperationIsProject = true;
					if (index == 2U) secondOperationIsProject = true;
					break;
				case logical::LogicalOpType::LOP_SKIP: {
					const int64_t offset = dynamic_cast<const logical::LogicalSkip &>(*operation).getOffset();
					if (offset < 0) {
						throw std::invalid_argument("logical Vector procedure SKIP cannot be negative");
					}
					const size_t skipped = std::min(remaining, static_cast<size_t>(offset));
					requiredPrefix = checkedProcedureSizeAdd(requiredPrefix, skipped);
					remaining -= skipped;
					break;
				}
				case logical::LogicalOpType::LOP_LIMIT: {
					const int64_t limit = dynamic_cast<const logical::LogicalLimit &>(*operation).getLimit();
					if (limit < 0) {
						throw std::invalid_argument("logical Vector procedure LIMIT cannot be negative");
					}
					remaining = std::min(remaining, static_cast<size_t>(limit));
					requiredPrefix = checkedProcedureSizeAdd(requiredPrefix, remaining);
					if (requiredPrefix == 0U || requiredPrefix > maximumRows ||
							(distinctTopKProject != nullptr && requiredPrefix > maximumDistinctRows) ||
							requiredPrefix >= totalRows) {
						return std::nullopt;
					}
					return requiredPrefix;
				}
				default:
					return std::nullopt;
				}
			}
			return std::nullopt;
		}

		[[nodiscard]] QueryResult executeTopKVectorProcedure(
				std::vector<storage::LogicalGraphVectorSearchHit> &hits,
				storage::LogicalGraphVectorSearchSnapshot &snapshot,
				const std::vector<const logical::LogicalOperator *> &operations,
				const logical::LogicalOperator &root, const size_t topK,
					const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context,
					const size_t retainedRequestBytes) {
			const logical::LogicalProject *distinctProjection = vectorProcedureDistinctTopKProject(operations);
			std::vector<std::string> distinctOutputVariables;
			size_t distinctOutputStorageBytes = 0U;
			if (distinctProjection != nullptr) {
				distinctOutputVariables = distinctProjection->getOutputVariables();
				distinctOutputStorageBytes = checkedProcedureSizeMul(
						distinctOutputVariables.capacity(), sizeof(std::string));
				for (const std::string &variable: distinctOutputVariables) {
					distinctOutputStorageBytes = checkedProcedureSizeAdd(distinctOutputStorageBytes,
						detail::dynamicStringBufferBytes(variable));
				}
			}
			const size_t retainedHitBytes = checkedProcedureSizeMul(
				hits.capacity(), sizeof(storage::LogicalGraphVectorSearchHit));
			const size_t retainedSourceBytes = checkedProcedureSizeAdd(
					checkedProcedureSizeAdd(retainedRequestBytes, retainedHitBytes), distinctOutputStorageBytes);
			if (retainedSourceBytes >= readBounds.maximum_materialized_bytes && !hits.empty()) {
				throw ProcedureByteBoundExceeded("logical Vector procedure source leaves no top-K materialization budget");
			}
		LogicalPlanReadRuntimeBounds workingBounds = readBounds;
		workingBounds.maximum_materialized_bytes -= retainedSourceBytes;
			const auto &sort = dynamic_cast<const logical::LogicalSort &>(*operations.front());
			const auto &items = sort.getSortItems();
			if (items.empty()) {
				throw std::invalid_argument("logical Vector procedure Sort has no sort items");
			}
			utils::OwnedStdVector<const expressions::Expression *> expressions;
			expressions.reserve(items.size());
			for (const logical::LogicalSortItem &item: items) {
				if (!item.expression) {
					throw std::invalid_argument("logical Vector procedure Sort has an empty sort expression");
				}
				expressions.push_back(item.expression.get());
			}
			const logical::LogicalFilter *sourceFilter = nullptr;
			if (operations.size() > 1U && operations[1U]->getType() == logical::LogicalOpType::LOP_FILTER) {
				sourceFilter = &dynamic_cast<const logical::LogicalFilter &>(*operations[1U]);
				expressions.push_back(sourceFilter->getPredicate().get());
			}
			const size_t projectIndex = sourceFilter == nullptr ? 1U : 2U;
			const logical::LogicalProject *earlyProjection = nullptr;
			if (operations.size() > projectIndex + 1U &&
					operations[projectIndex]->getType() == logical::LogicalOpType::LOP_PROJECT &&
					operations[projectIndex + 1U]->getType() == logical::LogicalOpType::LOP_FILTER) {
				earlyProjection = &dynamic_cast<const logical::LogicalProject &>(*operations[projectIndex]);
				for (const logical::LogicalProjectItem &item: earlyProjection->getItems()) {
					expressions.push_back(item.expression.get());
				}
			}
			if (distinctProjection != nullptr) {
				for (const logical::LogicalProjectItem &item: distinctProjection->getItems()) {
					expressions.push_back(item.expression.get());
				}
			}
			const logical::LogicalFilter *earlyFilter = sourceFilter;
			std::vector<LogicalPlanBatchedPropertyRequest> projectedFilterRequests;
			if (earlyProjection != nullptr) {
				earlyFilter = &dynamic_cast<const logical::LogicalFilter &>(*operations[projectIndex + 1U]);
				std::vector<LogicalPlanBatchedPropertyRequest> filterRequests;
				static_cast<void>(LogicalPlanReadRuntime::collectBatchedPropertyRequests(
					{earlyFilter->getPredicate().get()}, filterRequests));
				for (const auto &request: filterRequests) {
					for (const logical::LogicalProjectItem &item: earlyProjection->getItems()) {
						if (item.alias != request.variable) continue;
						const auto *source = dynamic_cast<const expressions::VariableReferenceExpression *>(item.expression.get());
						if (source != nullptr && !source->hasProperty()) {
							projectedFilterRequests.push_back({source->getVariableName(), request.key});
						}
						break;
					}
				}
			}
			struct DecoratedRow {
				BufferedProcedureRow row;
				std::vector<PropertyValue> keys;
				size_t sequence = 0U;
			};
			const auto precedes = [&items, &context](const DecoratedRow &left, const DecoratedRow &right) {
				context.checkGuard();
				for (size_t index = 0U; index < items.size(); ++index) {
					if (left.keys[index] == right.keys[index]) {
						continue;
					}
					return detail::orderByPrecedes(left.keys[index], right.keys[index], items[index].ascending);
				}
				return left.sequence < right.sequence;
			};
			validateVectorSearchHits(hits, readBounds, context);
			// With `precedes` as the heap comparator, the heap front is the worst
			// retained row and can be replaced as better candidates arrive.
			MaterializedByteBudget topKBudget(workingBounds.maximum_materialized_bytes);
			MaterializedByteReservation heapBytes(&topKBudget);
			MaterializedByteReservation heapSlotSlackBytes(&topKBudget);
			std::set<std::string> retainedDistinctKeys;
			struct RetainedDistinctInfo {
				std::set<std::string>::iterator key;
				size_t storage_bytes = 0U;
			};
			std::map<size_t, RetainedDistinctInfo> retainedDistinctRows;
			std::vector<DecoratedRow, utils::OwnedPmrAllocator<DecoratedRow>> retained;
			size_t retainedBytes = 0U;
			const auto decoratedBytes = [](const DecoratedRow &row) {
				size_t bytes = checkedProcedureSizeAdd(sizeof(DecoratedRow) - sizeof(BufferedProcedureRow), bufferedProcedureRowBytes(row.row));
				for (const PropertyValue &key: row.keys) {
					bytes = checkedProcedureSizeAdd(bytes,
							checkedProcedureSizeAdd(sizeof(PropertyValue), property_utils::getPropertyValueSize(key)));
				}
				return bytes;
			};
			const size_t capacity = procedureNodeBatchCapacity(workingBounds);
			const size_t keyStructure = checkedProcedureSizeAdd(sizeof(DecoratedRow) - sizeof(BufferedProcedureRow),
				checkedProcedureSizeMul(items.size(), sizeof(PropertyValue)));
			const auto ensureRetainedCapacity = [&](const auto &releaseReadAhead) {
				if (retained.size() < retained.capacity()) return;
				const size_t minimumCapacity = checkedProcedureSizeAdd(retained.size(), 1U);
				const size_t doubledCapacity = retained.capacity() == 0U
						? minimumCapacity
						: checkedProcedureSizeMul(retained.capacity(), 2U);
				const size_t requestedCapacity = std::min(topK, std::max(minimumCapacity, doubledCapacity));
				const size_t requestedBytes = checkedProcedureSizeMul(requestedCapacity, sizeof(DecoratedRow));
				// Existing heap row frames and slack account for the old allocation.
				// Reserve the new buffer as well while both may coexist during growth.
				const size_t growthBytes = checkedProcedureSizeAdd(heapSlotSlackBytes.bytes(), requestedBytes);
				if (heapSlotSlackBytes.wouldExceed(growthBytes)) releaseReadAhead();
				heapSlotSlackBytes.set(growthBytes);
				retained.reserve(requestedCapacity);
				const size_t actualBytes = checkedProcedureSizeMul(retained.capacity(), sizeof(DecoratedRow));
				const size_t occupiedBytes = checkedProcedureSizeMul(retained.size(), sizeof(DecoratedRow));
				heapSlotSlackBytes.set(actualBytes - occupiedBytes);
			};
			const auto retainCandidate = [&](DecoratedRow &&decorated, const size_t candidateBytes,
											 MaterializedByteReservation &candidateCharge, const auto &releaseReadAhead) {
				ensureRetainedCapacity(releaseReadAhead);
				// The reserved slack already covers the destination slot. Keep the
				// candidate reservation through the move, then transfer its charge to
				// the retained row and release the now-occupied slot from slack.
				retained.push_back(std::move(decorated));
				heapBytes.absorb(candidateCharge.splitOff(candidateBytes - sizeof(DecoratedRow)));
				heapBytes.absorb(heapSlotSlackBytes.splitOff(sizeof(DecoratedRow)));
				candidateCharge.reset();
				retainedBytes = checkedProcedureSizeAdd(retainedBytes, candidateBytes);
				std::push_heap(retained.begin(), retained.end(), precedes);
			};
			for (size_t offset = 0U; offset < hits.size();) {
				context.checkGuard();
				if (topKBudget.available() <= keyStructure) throw ProcedureByteBoundExceeded("logical procedure top-K has no candidate headroom");
				LogicalPlanReadRuntimeBounds sourceBounds = workingBounds;
				// Key slots are not live during source prefetch. Admit them after
				// temporary columns unwind, rather than double-reserving phases.
				sourceBounds.maximum_materialized_bytes = topKBudget.available();
				size_t count = std::min({capacity, procedureNodeBatchCapacity(sourceBounds), hits.size() - offset});
				ProcedureRows rows;
				for (;;) {
					try {
						auto prepared = makeBufferedVectorProcedureRows(std::span<const storage::LogicalGraphVectorSearchHit>(hits).subspan(offset, count), context);
						prefetchProcedureProperties(prepared, expressions, snapshot, sourceBounds, context,
							projectedFilterRequests);
						validateBufferedProcedureBytes(prepared, sourceBounds.maximum_materialized_bytes);
						rows = std::move(prepared);
						break;
					} catch (const ProcedureByteBoundExceeded &) {
						// No heap candidate has been processed. Drop the source batch
						// and retry a smaller prefix; malformed/guard errors propagate.
						if (count == 1U) throw;
						count = std::max<size_t>(1U, count / 2U);
					}
				}
				MaterializedByteReservation pendingRows(&topKBudget);
				for (const auto &row: rows) admitProcedureBytes(pendingRows, checkedProcedureSizeAdd(pendingRows.bytes(), bufferedProcedureRowBytes(row)));
				for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
					context.checkGuard();
					const size_t inputBytes = bufferedProcedureRowBytes(rows[rowIndex]);
					pendingRows.set(pendingRows.bytes() - inputBytes);
					MaterializedByteReservation candidateCharge(&topKBudget);
					const auto releaseReadAhead = [&] {
						rows.resize(rowIndex + 1U);
						pendingRows.reset();
					};
					const auto admitCandidate = [&](size_t bytes) {
						if (candidateCharge.wouldExceed(bytes) && pendingRows.bytes() != 0U) {
							// Release unprocessed read-ahead, never replay heap candidates.
							// Resume its hit suffix after the current candidate is handled.
							rows.resize(rowIndex + 1U);
							pendingRows.reset();
						}
						admitProcedureBytes(candidateCharge, bytes);
					};
					admitCandidate(checkedProcedureSizeAdd(inputBytes, keyStructure));
					DecoratedRow decorated{std::move(rows[rowIndex]), {}, offset + rowIndex};
					std::string candidateDistinctKey;
					size_t candidateDistinctStorageBytes = 0U;
					decorated.keys.reserve(items.size());
					for (const logical::LogicalSortItem &item: items) {
						LogicalPlanReadValue value = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
													snapshot, *item.expression, decorated.row.row, workingBounds, context);
						auto *key = std::get_if<PropertyValue>(&value);
						if (key == nullptr) throw std::runtime_error("logical Vector procedure Sort requires scalar sort keys");
						// All key slots are already reserved; add only owned payload.
						size_t next = checkedProcedureSizeAdd(bufferedProcedureRowBytes(decorated.row), keyStructure);
						for (const auto &existing: decorated.keys) next = checkedProcedureSizeAdd(next, property_utils::getPropertyValueSize(existing));
						admitCandidate(checkedProcedureSizeAdd(next, property_utils::getPropertyValueSize(*key)));
						decorated.keys.push_back(std::move(*key));
					}
					if (distinctProjection != nullptr) {
						std::optional<LogicalPlanReadRow> projected = applyProjection(*distinctProjection,
								snapshot, workingBounds, context, decorated.row.row, decorated.row.node_properties,
								nullptr, candidateCharge.bytes(), true, true);
						if (!projected.has_value()) {
							throw std::logic_error("logical Vector procedure deferred DISTINCT Project dropped a row");
						}
						decorated.row.row = std::move(*projected);
						candidateDistinctKey = LogicalPlanReadRuntime::makeDistinctRowKey(
								decorated.row.row, distinctOutputVariables);
						const size_t setNodeBytes = checkedProcedureSizeAdd(
								checkedProcedureSizeAdd(sizeof(std::string), 4U * sizeof(void *)),
								checkedProcedureSizeAdd(candidateDistinctKey.capacity(), 1U));
						const size_t mapNodeBytes = checkedProcedureSizeAdd(
								sizeof(std::pair<const size_t, RetainedDistinctInfo>), 4U * sizeof(void *));
						candidateDistinctStorageBytes = checkedProcedureSizeAdd(setNodeBytes, mapNodeBytes);
						admitCandidate(checkedProcedureSizeAdd(decoratedBytes(decorated), candidateDistinctStorageBytes));
						if (retainedDistinctKeys.contains(candidateDistinctKey)) continue;
					}
					if (sourceFilter != nullptr && earlyProjection != nullptr) {
						const bool keep = applyFilter(*sourceFilter, snapshot, workingBounds, context,
							decorated.row.row, decorated.row.node_properties);
						admitCandidate(decoratedBytes(decorated));
						if (!keep) continue;
					}
					if (earlyProjection != nullptr) {
						std::optional<LogicalPlanReadRow> projected = applyProjection(*earlyProjection,
								 snapshot, workingBounds, context, decorated.row.row, decorated.row.node_properties, nullptr);
						if (!projected.has_value()) {
							throw std::logic_error("logical Vector procedure non-DISTINCT top-K projection dropped a row");
						}
						decorated.row.row = std::move(*projected);
						admitCandidate(decoratedBytes(decorated));
					}
					if (earlyFilter != nullptr) {
						const bool keep = applyFilter(*earlyFilter, snapshot, workingBounds, context,
							decorated.row.row, decorated.row.node_properties);
						admitCandidate(decoratedBytes(decorated));
						if (!keep) continue;
					}
					const size_t candidateBytes = candidateCharge.bytes();
					if (retained.size() < topK) {
						if (distinctProjection != nullptr &&
								retainedDistinctKeys.size() == workingBounds.maximum_distinct_rows) {
							throw std::runtime_error("logical Vector procedure DISTINCT exceeded its row bound");
						}
						if (candidateBytes > workingBounds.maximum_materialized_bytes -
								std::min(retainedBytes, workingBounds.maximum_materialized_bytes)) {
							throw std::runtime_error("logical Vector procedure top-K rows exceeded their materialized byte bound");
						}
						if (distinctProjection != nullptr) {
							const auto [iterator, inserted] = retainedDistinctKeys.insert(std::move(candidateDistinctKey));
							if (!inserted) throw std::logic_error("logical Vector procedure retained a duplicate DISTINCT row");
							retainedDistinctRows.emplace(decorated.sequence,
									RetainedDistinctInfo{iterator, candidateDistinctStorageBytes});
						}
						retainCandidate(std::move(decorated), candidateBytes, candidateCharge, releaseReadAhead);
					} else if (precedes(decorated, retained.front())) {
						size_t replacedBytes = decoratedBytes(retained.front());
						if (distinctProjection != nullptr) {
							const auto oldDistinct = retainedDistinctRows.find(retained.front().sequence);
							if (oldDistinct == retainedDistinctRows.end()) throw std::logic_error("logical Vector top-K lost a DISTINCT key");
							replacedBytes = checkedProcedureSizeAdd(replacedBytes, oldDistinct->second.storage_bytes);
						}
						const size_t remainingBytes = retainedBytes - replacedBytes;
						if (candidateBytes > workingBounds.maximum_materialized_bytes -
								std::min(remainingBytes, workingBounds.maximum_materialized_bytes)) {
							throw std::runtime_error("logical Vector procedure top-K rows exceeded their materialized byte bound");
						}
						if (distinctProjection != nullptr) {
							const auto [iterator, inserted] = retainedDistinctKeys.insert(std::move(candidateDistinctKey));
							if (!inserted) throw std::logic_error("logical Vector procedure retained a duplicate DISTINCT row");
							retainedDistinctRows.emplace(decorated.sequence,
									RetainedDistinctInfo{iterator, candidateDistinctStorageBytes});
						}
						std::pop_heap(retained.begin(), retained.end(), precedes);
						if (distinctProjection != nullptr) {
							const auto oldDistinct = retainedDistinctRows.find(retained.back().sequence);
							if (oldDistinct == retainedDistinctRows.end()) throw std::logic_error("logical Vector top-K lost an evicted DISTINCT key");
							retainedDistinctKeys.erase(oldDistinct->second.key);
							retainedDistinctRows.erase(oldDistinct);
						}
						retained.pop_back();
						heapBytes.set(remainingBytes);
						heapSlotSlackBytes.set(checkedProcedureSizeMul(
								retained.capacity() - retained.size(), sizeof(DecoratedRow)));
						retainedBytes = remainingBytes;
						retainCandidate(std::move(decorated), candidateBytes, candidateCharge, releaseReadAhead);
					}
				}
				offset += rows.size();
			}
			std::sort_heap(retained.begin(), retained.end(), precedes);
			// Sort keys and DISTINCT membership no longer participate in output.
			// Release them before allocating the destination row buffer.
			retainedDistinctKeys.clear();
			retainedDistinctRows.clear();
			retainedBytes = 0U;
			for (DecoratedRow &decorated: retained) {
				std::vector<PropertyValue>().swap(decorated.keys);
				retainedBytes = checkedProcedureSizeAdd(retainedBytes, decoratedBytes(decorated));
			}
			heapBytes.set(retainedBytes);
			MaterializedByteReservation outputRowSlots(&topKBudget);
			const size_t requestedOutputRowBytes =
					checkedProcedureSizeMul(retained.size(), sizeof(BufferedProcedureRow));
			outputRowSlots.set(requestedOutputRowBytes);
			ProcedureRows rows;
			rows.reserve(retained.size());
			outputRowSlots.set(checkedProcedureSizeMul(rows.capacity(), sizeof(BufferedProcedureRow)));
			const size_t outputRowSlackBytes = checkedProcedureSizeMul(rows.capacity() - retained.size(),
																	 sizeof(BufferedProcedureRow));
			for (DecoratedRow &decorated: retained) {
				rows.push_back(std::move(decorated.row));
			}
			retained.clear();
			retainedDistinctKeys.clear();
			retainedDistinctRows.clear();
			heapBytes.reset();
			std::vector<DecoratedRow, utils::OwnedPmrAllocator<DecoratedRow>>().swap(retained);
			heapSlotSlackBytes.reset();
			outputRowSlots.reset();
			const size_t suffixOffset = earlyProjection != nullptr ? projectIndex + 2U
				: (distinctProjection != nullptr ? 2U : (sourceFilter == nullptr ? 1U : 2U));
			std::vector<const logical::LogicalOperator *> suffix(operations.begin() +
													 static_cast<std::ptrdiff_t>(suffixOffset), operations.end());
			QueryResult result = makeAdmittedProcedureResult(context);
			setOwnedProcedureColumns(result, root);
			static_cast<void>(appendBufferedProcedureRows(
					std::move(rows), snapshot, suffix, workingBounds, context, result, nullptr, nullptr,
					outputRowSlackBytes, true));
			validateProcedureResultBudget(result, outputRowSlackBytes, workingBounds.maximum_materialized_bytes);
			return result;
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
        context.checkGuardNow();
        std::optional<utils::OwnedAllocationScope> allocation;
        if (context.resource_account) {
            const auto domain = read_backend_.queryResourceDomain();
            if (context.resource_account->resourceClass() != detail::ResourceClass::RCK_QUERY ||
                (domain && !context.resource_account->belongsTo(*domain)))
                throw std::invalid_argument("procedure execution requires QUERY ownership within its database");
            allocation.emplace(std::make_shared<MaterializedByteBudget>(readBounds.maximum_materialized_bytes,
                context.resource_account, std::pmr::new_delete_resource()));
        }
        const logical::LogicalOperator *current = &root;
        utils::OwnedStdVector<const logical::LogicalOperator *> operations;
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
		const auto arguments = call->resolveArgumentView(context.parameters);
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
			ProcedureRows rows;
			utils::OwnedStringVector defaultColumns;
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
				QueryResult result = makeAdmittedProcedureResult(context);
				if (operations.empty()) {
					result = makeProcedureResult(rows, defaultColumns, readBounds, context);
				} else {
					context.checkGuard();
					std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot =
							read_backend_.openLogicalGraphReadSnapshot();
					if (!snapshot) {
						throw std::runtime_error("logical administration procedure backend could not open a read snapshot");
					}
					result = executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context,
							0U, nullptr, true);
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
			return executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context,
					0U, nullptr, true);
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

			ProcedureRows rows;
			utils::OwnedStringVector defaultColumns;
			size_t retainedProcedureSourceBytes = 0U;
			std::unique_ptr<gds::PreparedLogicalProjectionCatalogMutation> deferredCatalogMutation;
			switch (procedure->kind) {
				case NativeProcedureKind::NPK_GDS_GRAPH_PROJECT: {
					std::vector<std::reference_wrapper<const PropertyValue>> projectionArguments;
					projectionArguments.reserve(arguments.size());
					for (size_t index = 0U; index < arguments.size(); ++index) {
						projectionArguments.emplace_back(arguments[index]);
					}
					const gds::LogicalGraphProjectionSpec spec =
							gds::LogicalProjectionSpecCodec::decodeGraphProjectArguments(
									std::span<const std::reference_wrapper<const PropertyValue>>(projectionArguments));
					gds::LogicalProjectionCatalogCreatePreparation preparation = [&] {
                        utils::OwnedAllocationScope cacheAllocation(projection_memory_owner_
                            ? projection_memory_owner_ : utils::OwnedAllocationScope::owner());
                        return projection_catalog_->prepareCreate(
							spec, *snapshot,
							{.maximum_nodes = readBounds.maximum_node_records,
							 .maximum_metadata_pages = readBounds.maximum_metadata_pages_per_node_page,
							 .maximum_edges_per_node = readBounds.maximum_edges_per_node,
							 .maximum_property_bytes = readBounds.maximum_materialized_bytes},
							[&context] { context.checkGuard(); });
                    }();
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
						for (const gds::LogicalProjectionDescriptor &descriptor: projection_catalog_->listOwned(
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
					const auto scores =
							gds::LogicalGraphAlgorithms::pageRankOwned(*projection_catalog_->get(name), options,
								   {.maximum_nodes = readBounds.maximum_node_records,
									.maximum_edges = maximumGdsAlgorithmEdges,
									.maximum_iterations = maximumGdsAlgorithmIterations,
									.maximum_work_units = maximumGdsAlgorithmWorkUnits,
									.maximum_materialized_bytes = readBounds.maximum_materialized_bytes},
									   [&context] { context.checkGuard(); });
					const size_t outputByteAllowance = reserveBoundedProcedureRows(rows, scores.size(), readBounds,
							checkedProcedureSizeMul(scores.capacity(), sizeof(gds::LogicalGdsNodeScore)));
					size_t retainedBytes = 0U;
					for (const gds::LogicalGdsNodeScore &score: scores) {
						context.checkGuard();
						BufferedProcedureRow row;
						row.row.values.emplace("nodeId", PropertyValue(checkedOutputNodeId(score.node_id)));
						row.row.values.emplace("score", PropertyValue(score.score));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.pageRank.stream",
								&retainedBytes, outputByteAllowance);
					}
					defaultColumns = {"nodeId", "score"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_WCC: {
					const std::string name = requireProjectionName(arguments, "gds.wcc.stream", 1U);
					const auto components =
							gds::LogicalGraphAlgorithms::weaklyConnectedComponentsOwned(
									*projection_catalog_->get(name),
								{.maximum_nodes = readBounds.maximum_node_records,
								 .maximum_edges = maximumGdsAlgorithmEdges,
								 .maximum_iterations = maximumGdsAlgorithmIterations,
								 .maximum_work_units = maximumGdsAlgorithmWorkUnits,
								 .maximum_materialized_bytes = readBounds.maximum_materialized_bytes},
									[&context] { context.checkGuard(); });
					const size_t outputByteAllowance = reserveBoundedProcedureRows(rows, components.size(), readBounds,
							checkedProcedureSizeMul(components.capacity(), sizeof(gds::LogicalGdsNodeComponent)));
					size_t retainedBytes = 0U;
					for (const gds::LogicalGdsNodeComponent &component: components) {
						context.checkGuard();
						BufferedProcedureRow row;
						row.row.values.emplace("nodeId", PropertyValue(checkedOutputNodeId(component.node_id)));
						row.row.values.emplace("componentId",
											   PropertyValue(checkedOutputNodeId(component.component_id)));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.wcc.stream",
								&retainedBytes, outputByteAllowance);
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
					const auto scores =
							gds::LogicalGraphAlgorithms::betweennessCentralityOwned(
									*projection_catalog_->get(name), options,
									{.maximum_nodes = readBounds.maximum_node_records,
									 .maximum_edges = maximumGdsAlgorithmEdges,
									 .maximum_iterations = maximumGdsAlgorithmIterations,
									 .maximum_work_units = maximumGdsAlgorithmWorkUnits,
									 .maximum_materialized_bytes = readBounds.maximum_materialized_bytes},
									[&context] { context.checkGuard(); });
					const size_t outputByteAllowance = reserveBoundedProcedureRows(rows, scores.size(), readBounds,
							checkedProcedureSizeMul(scores.capacity(), sizeof(gds::LogicalGdsNodeScore)));
					size_t retainedBytes = 0U;
					for (const gds::LogicalGdsNodeScore &score: scores) {
						context.checkGuard();
						BufferedProcedureRow row;
						row.row.values.emplace("nodeId", PropertyValue(checkedOutputNodeId(score.node_id)));
						row.row.values.emplace("score", PropertyValue(score.score));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows,
													  "gds.betweenness.stream", &retainedBytes,
													  outputByteAllowance);
					}
					defaultColumns = {"nodeId", "score"};
					break;
				}
				case NativeProcedureKind::NPK_GDS_CLOSENESS: {
					const std::string name = requireProjectionName(arguments, "gds.closeness.stream", 1U);
					const auto scores =
							gds::LogicalGraphAlgorithms::closenessCentralityOwned(
									*projection_catalog_->get(name),
									{.maximum_nodes = readBounds.maximum_node_records,
									 .maximum_edges = maximumGdsAlgorithmEdges,
									 .maximum_iterations = maximumGdsAlgorithmIterations,
									 .maximum_work_units = maximumGdsAlgorithmWorkUnits,
									 .maximum_materialized_bytes = readBounds.maximum_materialized_bytes},
									[&context] { context.checkGuard(); });
					const size_t outputByteAllowance = reserveBoundedProcedureRows(rows, scores.size(), readBounds,
							checkedProcedureSizeMul(scores.capacity(), sizeof(gds::LogicalGdsNodeScore)));
					size_t retainedBytes = 0U;
					for (const gds::LogicalGdsNodeScore &score: scores) {
						context.checkGuard();
						BufferedProcedureRow row;
						row.row.values.emplace("nodeId", PropertyValue(checkedOutputNodeId(score.node_id)));
						row.row.values.emplace("score", PropertyValue(score.score));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows,
													  "gds.closeness.stream", &retainedBytes,
													  outputByteAllowance);
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
					const auto communities = gds::LogicalGraphAlgorithms::leidenOwned(
							*projection_catalog_->get(name), options,
										{.maximum_nodes = readBounds.maximum_node_records,
										 .maximum_edges = maximumGdsAlgorithmEdges,
										 .maximum_iterations = maximumGdsAlgorithmIterations,
										 .maximum_work_units = maximumGdsAlgorithmWorkUnits,
										 .maximum_materialized_bytes = readBounds.maximum_materialized_bytes},
							[&context] { context.checkGuard(); }, thread_pool_);
					const size_t outputByteAllowance = reserveBoundedProcedureRows(rows, communities.size(), readBounds,
							checkedProcedureSizeMul(communities.capacity(), sizeof(gds::LogicalGdsNodeCommunity)));
					size_t retainedBytes = 0U;
					for (const gds::LogicalGdsNodeCommunity &community: communities) {
						context.checkGuard();
						BufferedProcedureRow row;
						row.row.values.emplace("nodeId", PropertyValue(checkedOutputNodeId(community.node_id)));
						row.row.values.emplace("communityId",
											   PropertyValue(checkedOutputNodeId(community.community_id)));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows, "gds.leiden.stream",
								&retainedBytes, outputByteAllowance);
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
														   .maximum_work_units = maximumGdsAlgorithmWorkUnits,
														   .maximum_materialized_bytes = readBounds.maximum_materialized_bytes},
																  [&context] { context.checkGuard(); });
					if (path.node_ids.size() != path.step_weights.size()) {
						throw std::logic_error("logical Dijkstra returned an invalid path shape");
					}
					// Projection paths still authenticate against this pinned snapshot,
					// even when the caller projects only costs and never requests Nodes.
					if (path.node_ids.size() > readBounds.maximum_node_records) {
						throw std::runtime_error("logical Dijkstra returned an over-bound path");
					}
					const size_t retainedPathBytes = checkedProcedureSizeAdd(sizeof(path),
							checkedProcedureSizeAdd(checkedProcedureSizeMul(path.node_ids.capacity(), sizeof(uint64_t)),
									checkedProcedureSizeMul(path.step_weights.capacity(), sizeof(double))));
					{
						MaterializedByteBudget validationBudget(readBounds.maximum_materialized_bytes);
						MaterializedByteReservation retainedPath(&validationBudget);
						retainedPath.set(retainedPathBytes);
						// Authenticate in bounded slices instead of copying and sorting the
						// entire path alongside Dijkstra's retained result vectors.
						const size_t maximumProbeRecords = std::min(readBounds.maximum_adjacency_page_records,
								(validationBudget.available() - std::min(validationBudget.available(), sizeof(size_t))) /
								(sizeof(uint64_t) + sizeof(bool)));
						if (!path.node_ids.empty() && maximumProbeRecords == 0U) {
							throw std::runtime_error("logical Dijkstra path validation exceeded its byte bound");
						}
						MaterializedByteReservation probeIds(&validationBudget);
						probeIds.set(checkedProcedureSizeMul(std::min(path.node_ids.size(), maximumProbeRecords), sizeof(uint64_t)));
						utils::OwnedStdVector<uint64_t> pathIds;
						pathIds.reserve(std::min(path.node_ids.size(), maximumProbeRecords));
						probeIds.set(checkedProcedureSizeMul(pathIds.capacity(), sizeof(uint64_t)));
						for (size_t offset = 0U; offset < path.node_ids.size();) {
							context.checkGuard();
							const size_t count = std::min(maximumProbeRecords, path.node_ids.size() - offset);
							pathIds.clear();
							pathIds.insert(pathIds.end(),
									path.node_ids.begin() + static_cast<std::ptrdiff_t>(offset),
									path.node_ids.begin() + static_cast<std::ptrdiff_t>(offset + count));
							std::ranges::sort(pathIds);
							pathIds.erase(std::unique(pathIds.begin(), pathIds.end()), pathIds.end());
							if (!pathIds.empty() && pathIds.front() == 0U) {
								throw std::runtime_error("logical Dijkstra returned an invalid path Node");
							}
							MaterializedByteReservation probeResults(&validationBudget);
							// Reserve byte-per-flag storage plus a packed-word rounding margin.
							probeResults.set(checkedProcedureSizeAdd(
								checkedProcedureSizeMul(pathIds.size(), sizeof(bool)), sizeof(size_t)));
							const auto present = snapshot->containsNodesSorted(pathIds);
							probeResults.set(checkedProcedureSizeAdd(present.capacity(), 7U) / 8U);
							if (present.size() != pathIds.size() ||
									std::ranges::any_of(present, [](bool exists) { return !exists; })) {
								throw std::runtime_error("logical Dijkstra path contains a missing Node");
							}
							offset += count;
						}
					} // Authentication buffers unwind before output row allocation.
					retainedProcedureSourceBytes = retainedPathBytes;
					const size_t outputByteAllowance = reserveBoundedProcedureRows(
							rows, path.node_ids.size(), readBounds, retainedPathBytes);
					size_t retainedBytes = 0U;
					double accumulatedCost = 0.0;
					for (size_t index = 0U; index < path.node_ids.size(); ++index) {
						context.checkGuard();
						// step_weights[index] is the edge used to reach this node. Account
						// for it before materializing the row so cost is cumulative at node.
						if (index != 0U) {
							accumulatedCost += path.step_weights[index];
						}
						BufferedProcedureRow row;
						row.row.values.emplace("nodeId", PropertyValue(checkedOutputNodeId(path.node_ids[index])));
						row.row.values.emplace("node", storage::EntityRef{path.node_ids[index], false, 0U, 0U, 0U});
						row.row.values.emplace("cost", PropertyValue(accumulatedCost));
						row.row.values.emplace("totalCost", PropertyValue(path.total_weight));
						appendBoundedProcedureRow(rows, std::move(row), readBounds.maximum_rows,
													  "gds.shortestPath.dijkstra.stream", &retainedBytes,
													  outputByteAllowance);
					}
					validateBufferedProcedureBytes(rows, readBounds.maximum_materialized_bytes);
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
			QueryResult result = makeAdmittedProcedureResult(context);
			if (operations.empty() && procedure->kind != NativeProcedureKind::NPK_GDS_DIJKSTRA) {
				result = makeProcedureResult(std::move(rows), defaultColumns, readBounds, context);
			} else {
				result = executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context,
						retainedProcedureSourceBytes, operations.empty() && procedure->kind == NativeProcedureKind::NPK_GDS_DIJKSTRA
								? &defaultColumns
								: nullptr,
					true);
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
			const utils::OwnedStdVector<uint64_t> path = findShortestPath(*snapshot, startNodeId, endNodeId, maximumDepth, readBounds, context);
			auto rows = makeBufferedShortestPathRows(path, readBounds);
			const size_t retainedPathBytes = checkedProcedureSizeAdd(sizeof(path),
					checkedProcedureSizeMul(path.capacity(), sizeof(uint64_t)));
			if (!operations.empty()) {
				return executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context,
						retainedPathBytes, nullptr, true);
			}
			const utils::OwnedStringVector columns{"step", "node", "path_length"};
			return executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context,
					retainedPathBytes, &columns, true);
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
			QueryResult result = makeAdmittedProcedureResult(context);
			if (operations.empty()) {
				result.setColumns({"status"});
				ProcedureRows rows(1U);
				rows.front().row.values.emplace("status", status());
				QueryResult::Row row;
				row.emplace("status", status());
				appendProcedureResultRow(result, rows, 0U, std::move(row), readBounds.maximum_materialized_bytes);
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
				ProcedureRows rows;
				rows.push_back(std::move(source));
				result = executeBufferedProcedure(std::move(rows), *snapshot, operations, root, readBounds, context,
						0U, nullptr, true);
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
		size_t retainedRequestBytes = retainedVectorRequestBytes(request);
		if (retainedRequestBytes > readBounds.maximum_materialized_bytes) {
			throw ProcedureByteBoundExceeded("logical Vector procedure request exceeded its materialized byte bound");
		}
		request.query_vector = requireQueryVector(arguments[2],
				readBounds.maximum_materialized_bytes - retainedRequestBytes, context);
		retainedRequestBytes = retainedVectorRequestBytes(request);
		if (retainedRequestBytes > readBounds.maximum_materialized_bytes) {
			throw ProcedureByteBoundExceeded("logical Vector procedure request exceeded its materialized byte bound");
		}
		request.maximum_mapping_entries = maximumVectorMappingEntries;
		request.maximum_raw_vector_reads = maximumRawVectorReads;
		request.candidate_search_budget = vectorCandidateSearchBudget;
		request.candidate_refinement_budget = vectorCandidateRefinementBudget;
		request.cancellation = &cancellation;
		if (request.top_k > readBounds.maximum_rows) {
			throw std::invalid_argument("db.index.vector.queryNodes topK exceeds the logical row bound");
		}
		if (request.top_k == 0U) {
			QueryResult result = makeAdmittedProcedureResult(context);
			result.setColumns(operations.empty() ? std::vector<std::string>{"node", "score"}
													 : root.getOutputVariables());
			validateProcedureResultBudget(result, retainedRequestBytes, readBounds.maximum_materialized_bytes);
			return result;
		}
		// Validate row-window operators before any specialized path can return early
		// for an empty hit stream. The row-wise fallback performed this validation
		// even when the Vector search produced no candidates.
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

		// Opening a transaction-owned Vector snapshot pins observable state, so a
		// timed-out procedure must fail before acquiring it.
		context.checkGuard();
		std::unique_ptr<storage::LogicalGraphVectorSearchSnapshot> snapshot =
				vector_backend_->openLogicalGraphVectorSearchSnapshot();
		if (!snapshot) {
			throw std::runtime_error("logical Vector procedure backend could not open a search snapshot");
		}
		storage::LogicalGraphVectorSearchResult searchResult = snapshot->searchLogicalVector(request);
		context.checkGuard();
		std::vector<storage::LogicalGraphVectorSearchHit> &hits = searchResult.hits;
		if (hits.size() > request.top_k) {
			throw std::runtime_error("logical Vector procedure backend exceeded its requested result bound");
		}
		const size_t retainedHitBytes = checkedProcedureSizeMul(
			hits.capacity(), sizeof(storage::LogicalGraphVectorSearchHit));
		if (retainedRequestBytes > readBounds.maximum_materialized_bytes) {
			throw ProcedureByteBoundExceeded("logical Vector procedure request exceeded its materialized byte bound");
		}
		if (retainedHitBytes > readBounds.maximum_materialized_bytes - retainedRequestBytes) {
			throw ProcedureByteBoundExceeded("logical Vector procedure hit buffer exceeded its materialized byte bound");
		}
		const size_t retainedVectorSourceBytes = checkedProcedureSizeAdd(retainedRequestBytes, retainedHitBytes);
		if (!hits.empty() && retainedVectorSourceBytes >= readBounds.maximum_materialized_bytes) {
			throw ProcedureByteBoundExceeded("logical Vector procedure hit buffer leaves no output materialization budget");
		}
		LogicalPlanReadRuntimeBounds outputBounds = readBounds;
		outputBounds.maximum_materialized_bytes -= retainedVectorSourceBytes;
		if (const std::optional<size_t> prefix =
				vectorProcedureSortPrefix(operations, hits.size(), readBounds.maximum_rows,
						readBounds.maximum_distinct_rows);
				prefix.has_value()) {
			const auto postSkipFilterLimit = vectorProcedurePostSkipFilterLimit(operations);
			size_t candidateCount = *prefix;
			while (true) {
				QueryResult result = executeTopKVectorProcedure(hits, *snapshot, operations, root, candidateCount,
						readBounds, context, retainedRequestBytes);
				if (!postSkipFilterLimit.has_value() || result.rowCount() >= postSkipFilterLimit->result_limit ||
						candidateCount >= hits.size()) {
					return result;
				}
				const size_t doubled = candidateCount > std::numeric_limits<size_t>::max() / 2U
						? hits.size()
						: checkedProcedureSizeMul(candidateCount, 2U);
				const logical::LogicalProject *distinctTopKProject = vectorProcedureDistinctTopKProject(operations);
				const size_t candidateBound = distinctTopKProject == nullptr
						? hits.size()
						: std::min(hits.size(), readBounds.maximum_distinct_rows);
				const size_t nextCandidateCount = std::min(candidateBound, std::max(candidateCount + 1U, doubled));
				if (nextCandidateCount <= candidateCount) {
					return executeBufferedVectorProcedure(hits, *snapshot, operations, root, readBounds, context,
							retainedRequestBytes);
				}
				if (nextCandidateCount > readBounds.maximum_rows && nextCandidateCount < hits.size()) {
					return executeBufferedVectorProcedure(hits, *snapshot, operations, root, readBounds, context,
							retainedRequestBytes);
				}
				candidateCount = nextCandidateCount;
			}
		}
		if (std::ranges::any_of(operations, [](const logical::LogicalOperator *operation) {
				return operation->getType() == logical::LogicalOpType::LOP_SORT;
			})) {
			return executeBufferedVectorProcedure(hits, *snapshot, operations, root, readBounds, context,
					retainedRequestBytes);
		}
		if (const std::optional<size_t> prefix = vectorProcedurePrefixThroughLimit(operations, hits.size());
				prefix.has_value()) {
			const auto hitPrefix = std::span<storage::LogicalGraphVectorSearchHit>(hits).first(*prefix);
			validateVectorSearchHits(hitPrefix, readBounds, context);
			QueryResult result = makeAdmittedProcedureResult(context);
			setOwnedProcedureColumns(result, root);
			const size_t capacity = procedureNodeBatchCapacity(outputBounds);
			utils::OwnedStdVector<uint64_t> operatorRowCounts(operations.size(), 0U);
			for (size_t offset = 0U; offset < hitPrefix.size(); offset += capacity) {
				context.checkGuard();
				const auto batch = hitPrefix.subspan(offset, std::min(capacity, hitPrefix.size() - offset));
				if (!appendBufferedProcedureRows(makeBufferedVectorProcedureRows(batch, context), *snapshot,
						operations, readBounds, context, result, nullptr, &operatorRowCounts, retainedVectorSourceBytes, true)) {
					break;
				}
			}
			validateProcedureResultBudget(result, retainedVectorSourceBytes, readBounds.maximum_materialized_bytes);
			return result;
		}
		// Unary Project/Filter/SKIP/LIMIT layers can share bounded expression
		// batches. DISTINCT and row-count state carry across batches; LIMIT stops
		// pulling hit batches once its input quota has been consumed.
		if (!operations.empty() && std::ranges::all_of(operations, [](const logical::LogicalOperator *operation) {
				const auto type = operation->getType();
				return type == logical::LogicalOpType::LOP_FILTER || type == logical::LogicalOpType::LOP_PROJECT ||
						type == logical::LogicalOpType::LOP_SKIP || type == logical::LogicalOpType::LOP_LIMIT;
			})) {
			validateVectorSearchHits(hits, readBounds, context);
			QueryResult result = makeAdmittedProcedureResult(context);
			setOwnedProcedureColumns(result, root);
			const size_t capacity = procedureNodeBatchCapacity(outputBounds);
			std::vector<std::unordered_set<std::string>> distinctRowsByOperation(operations.size());
			utils::OwnedStdVector<uint64_t> operatorRowCounts(operations.size(), 0U);
			for (size_t offset = 0U; offset < hits.size(); offset += capacity) {
				context.checkGuard();
				const auto batch = std::span<const storage::LogicalGraphVectorSearchHit>(hits).subspan(
						offset, std::min(capacity, hits.size() - offset));
				if (!appendBufferedProcedureRows(makeBufferedVectorProcedureRows(batch, context), *snapshot,
						operations, readBounds, context, result, &distinctRowsByOperation, &operatorRowCounts,
						retainedVectorSourceBytes, true)) {
					break;
				}
			}
			validateProcedureResultBudget(result, retainedVectorSourceBytes, readBounds.maximum_materialized_bytes);
			return result;
		}

		QueryResult result = makeAdmittedProcedureResult(context);
		result.setColumns({"node", "score"});
		validateVectorSearchHits(hits, readBounds, context);
		const size_t capacity = procedureNodeBatchCapacity(outputBounds);
		for (size_t offset = 0U; offset < hits.size(); offset += capacity) {
			context.checkGuard();
			const auto batch = std::span<const storage::LogicalGraphVectorSearchHit>(hits).subspan(
					offset, std::min(capacity, hits.size() - offset));
			if (!appendBufferedProcedureRows(makeBufferedVectorProcedureRows(batch, context), *snapshot, {},
					readBounds, context, result, nullptr, nullptr, retainedVectorSourceBytes, true)) {
				break;
			}
		}
		validateProcedureResultBudget(result, retainedVectorSourceBytes, readBounds.maximum_materialized_bytes);
		return result;
	}

} // namespace graph::query::execution
