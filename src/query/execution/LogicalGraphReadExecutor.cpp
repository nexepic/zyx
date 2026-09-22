/**
 * @file LogicalGraphReadExecutor.cpp
 * @brief Format-independent bounded logical graph read execution.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/query/execution/LogicalGraphReadExecutor.hpp"

#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "graph/core/Types.hpp"
#include "graph/query/execution/SelectionVector.hpp"

namespace graph::query::execution {

	namespace {

		constexpr size_t MAXIMUM_FILTER_TERMS = 1024U;
		constexpr size_t DEFAULT_TOPOLOGY_BATCH_ROWS = 256U;

		using LogicalProperties = std::unordered_map<std::string, PropertyValue>;

		[[nodiscard]] storage::EntityReadShape completeNodeReadShape() {
			return {.include_labels = true, .include_type = false, .all_properties = true};
		}

		void validatePredicate(const LogicalPropertyPredicate &predicate) {
			if (predicate.property_key.empty()) {
				throw std::invalid_argument("logical graph Property predicate requires a nonempty key");
			}
			switch (predicate.op) {
				case LogicalPropertyPredicateOp::LPP_EQ:
				case LogicalPropertyPredicateOp::LPP_NE:
				case LogicalPropertyPredicateOp::LPP_LT:
				case LogicalPropertyPredicateOp::LPP_LE:
				case LogicalPropertyPredicateOp::LPP_GT:
				case LogicalPropertyPredicateOp::LPP_GE:
					if (predicate.upper_value.has_value()) {
						throw std::invalid_argument(
								"logical graph non-range Property predicate cannot carry an upper bound");
					}
					return;
				case LogicalPropertyPredicateOp::LPP_RANGE_CLOSED:
					if (!predicate.upper_value.has_value()) {
						throw std::invalid_argument(
								"logical graph closed-range Property predicate requires an upper bound");
					}
					return;
			}
			throw std::invalid_argument("logical graph Property predicate has an unknown operation");
		}

		void validateNodeFilter(const LogicalGraphNodeFilter &filter) {
			if (filter.required_labels.size() > MAXIMUM_FILTER_TERMS ||
				filter.property_predicates.size() > MAXIMUM_FILTER_TERMS) {
				throw std::invalid_argument("logical graph filter exceeds the bounded term count");
			}
			std::unordered_set<std::string> labels;
			labels.reserve(filter.required_labels.size());
			for (const std::string &label: filter.required_labels) {
				if (label.empty() || !labels.insert(label).second) {
					throw std::invalid_argument("logical graph Node filter has an empty or duplicate label");
				}
			}
			for (const LogicalPropertyPredicate &predicate: filter.property_predicates) {
				validatePredicate(predicate);
			}
		}

		void validateEdgeFilter(const LogicalGraphEdgeFilter &filter) {
			// `nullopt` means every relationship type, while an empty string means
			// only relationships created without an explicit type.
			if (filter.property_predicates.size() > MAXIMUM_FILTER_TERMS) {
				throw std::invalid_argument("logical graph Edge filter exceeds the bounded predicate count");
			}
			for (const LogicalPropertyPredicate &predicate: filter.property_predicates) {
				validatePredicate(predicate);
			}
		}

		void validateRequest(const LogicalGraphReadRequest &request, const LogicalGraphReadExecutor::RowVisitor &visitor) {
			if (!visitor) {
				throw std::invalid_argument("logical graph execution requires a row visitor");
			}
			const LogicalGraphReadExecutionBounds &bounds = request.bounds;
			if (bounds.maximum_node_records == 0U || bounds.maximum_edges_per_node == 0U ||
				bounds.maximum_property_entries == 0U || bounds.maximum_rows == 0U) {
				throw std::invalid_argument("logical graph execution requires positive explicit bounds");
			}
			validateNodeFilter(request.source_filter);
			if (request.expansion.has_value()) {
				const LogicalGraphExpansion &expansion = *request.expansion;
				switch (expansion.direction) {
					case storage::LogicalGraphReadDirection::LGRD_OUTGOING:
					case storage::LogicalGraphReadDirection::LGRD_INCOMING:
						break;
					default:
						throw std::invalid_argument("logical graph expansion has an unknown direction");
				}
				validateEdgeFilter(expansion.edge_filter);
				validateNodeFilter(expansion.target_filter);
			}
		}

		[[nodiscard]] bool hasRequiredLabels(const storage::LogicalGraphNode &node,
												 const std::vector<std::string> &requiredLabels) {
			std::unordered_set<std::string_view> labels;
			labels.reserve(node.labels.size());
			for (const std::string &label: node.labels) {
				if (label.empty() || !labels.insert(label).second) {
					throw std::runtime_error("logical graph backend returned an invalid Node label set");
				}
			}
			for (const std::string &required: requiredLabels) {
				if (!labels.contains(required)) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] LogicalProperties readInlineProperties(
				const std::vector<storage::LogicalGraphPropertyEntry> &entries, size_t maximumEntries,
				LogicalGraphReadExecutionStats &stats) {
			if (entries.size() > maximumEntries) {
				throw std::runtime_error("logical graph backend returned more Property entries than the caller bound");
			}
			LogicalProperties result;
			result.reserve(entries.size());
			for (const storage::LogicalGraphPropertyEntry &entry: entries) {
				if (entry.key.empty() || !result.emplace(entry.key, entry.value).second) {
					throw std::runtime_error("logical graph backend returned an invalid owner-keyed Property entry set");
				}
			}
			if (stats.property_sets_materialized == std::numeric_limits<size_t>::max()) {
				throw std::overflow_error("logical graph Property materialization counter overflowed");
			}
			++stats.property_sets_materialized;
			return result;
		}

		[[nodiscard]] bool matchesPredicates(const LogicalProperties &properties,
															 const std::vector<LogicalPropertyPredicate> &predicates) {
			for (const LogicalPropertyPredicate &predicate: predicates) {
				const auto iterator = properties.find(predicate.property_key);
				const std::optional<PropertyValue> value =
						iterator == properties.end() ? std::nullopt : std::optional<PropertyValue>(iterator->second);
				if (!evaluateLogicalPropertyPredicate(value, predicate)) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool matchesNode(const storage::LogicalGraphNode &node, const LogicalGraphNodeFilter &filter,
																			 const LogicalGraphReadExecutionBounds &bounds,
																			 LogicalGraphReadExecutionStats &stats) {
			if (node.node_id == 0U) {
				throw std::runtime_error("logical graph backend returned a Node without a logical ID");
			}
			if (!hasRequiredLabels(node, filter.required_labels)) {
				return false;
			}
			if (filter.property_predicates.empty()) {
				return true;
			}
			return matchesPredicates(readInlineProperties(node.properties, bounds.maximum_property_entries, stats),
									 filter.property_predicates);
		}

		[[nodiscard]] bool matchesEdge(const storage::LogicalGraphEdge &edge, const LogicalGraphEdgeFilter &filter,
																										 const LogicalGraphReadExecutionBounds &bounds,
																										 LogicalGraphReadExecutionStats &stats) {
			// The adjacency visitor validates both endpoints against the selected direction before this filter runs.
			if (edge.edge_id == 0U) {
				throw std::runtime_error("logical graph backend returned an Edge with an invalid identity");
			}
			if (filter.relationship_type.has_value() && edge.type != *filter.relationship_type) {
				return false;
			}
			if (filter.property_predicates.empty()) {
				return true;
			}
			return matchesPredicates(readInlineProperties(edge.properties, bounds.maximum_property_entries, stats),
																										 filter.property_predicates);
		}

		[[nodiscard]] bool matchesEdge(const storage::LogicalGraphEdgeTopology &edge,
													 const LogicalGraphEdgeFilter &filter) {
			if (edge.edge_id == 0U) {
				throw std::runtime_error("logical graph backend returned an Edge with an invalid identity");
			}
			return !filter.property_predicates.empty() ? false
													 : (!filter.relationship_type.has_value() || edge.type == *filter.relationship_type);
		}

		void incrementBounded(size_t &counter, const char *message) {
			if (counter == std::numeric_limits<size_t>::max()) {
				throw std::overflow_error(message);
			}
			++counter;
		}

	} // namespace

	LogicalGraphReadExecutionStats LogicalGraphReadExecutor::execute(const LogicalGraphReadRequest &request,
																		const RowVisitor &visitor) const {
		validateRequest(request, visitor);
		std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot = backend_.openLogicalGraphReadSnapshot();
		if (!snapshot) {
			throw std::runtime_error("logical graph backend returned no read snapshot");
		}

		LogicalGraphReadExecutionStats stats;
		const auto emit = [&](const LogicalGraphReadRow &row) {
			visitor(row);
			incrementBounded(stats.rows_emitted, "logical graph row counter overflowed");
			return true;
		};
		bool nodeScanStopped = false;
		const auto processNode = [&](const storage::LogicalGraphNode &source) {
			if (nodeScanStopped) {
				throw std::runtime_error("logical graph backend ignored a Node scan stop request");
			}
			if (stats.nodes_scanned == request.bounds.maximum_node_records) {
				throw std::runtime_error("logical graph backend exceeded the requested Node scan bound");
			}
			incrementBounded(stats.nodes_scanned, "logical graph Node counter overflowed");
			if (!matchesNode(source, request.source_filter, request.bounds, stats)) {
				return true;
			}
			incrementBounded(stats.nodes_matched, "logical graph matched-Node counter overflowed");
			if (!request.expansion.has_value()) {
				const bool emitted = emit({source, std::nullopt, std::nullopt});
				const bool shouldContinue = emitted && stats.rows_emitted < request.bounds.maximum_rows;
				nodeScanStopped = !shouldContinue;
				return shouldContinue;
			}

			const LogicalGraphExpansion &expansion = *request.expansion;
			size_t nodeEdges = 0U;
			bool edgeScanStopped = false;
			// A label-only target predicate is common in topology traversals.  Keep
			// the page-local result here so one sorted label batch serves all edges in
			// the page instead of issuing one directory lookup per target.
			std::unordered_map<uint64_t, bool> targetLabelMatches;
			std::unordered_map<uint64_t, storage::LogicalGraphEdge> batchedEdges;
			std::unordered_map<uint64_t, storage::LogicalGraphNode> batchedNodes;
			bool batchedTargetNodesActive = false;
			const auto processTopology = [&](const storage::LogicalGraphEdgeTopology &edge) {
						if (edgeScanStopped) {
							throw std::runtime_error("logical graph backend ignored an adjacency scan stop request");
						}
						if (nodeEdges == request.bounds.maximum_edges_per_node) {
							throw std::runtime_error("logical graph backend exceeded the requested adjacency bound");
						}
						incrementBounded(nodeEdges, "logical graph per-Node edge counter overflowed");
						incrementBounded(stats.edges_scanned, "logical graph Edge counter overflowed");
						const uint64_t targetId = expansion.direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																		? edge.target_node_id
																		: edge.source_node_id;
						const bool invalidDirection =
									expansion.direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																		? edge.source_node_id != source.node_id
																		: edge.target_node_id != source.node_id;
						if (invalidDirection || targetId == 0U || !matchesEdge(edge, expansion.edge_filter)) {
							if (invalidDirection || targetId == 0U) {
								throw std::runtime_error("logical graph backend returned an invalid adjacent Edge");
							}
							return true;
						}
						incrementBounded(stats.edges_matched, "logical graph matched-Edge counter overflowed");
						// A topology expansion commonly filters the target by labels only.
						// Test that projection at the storage boundary first so rejected
						// targets do not force complete Node/Property materialization. The
						// complete record is still read for rows that survive the label test.
				if (expansion.target_filter.property_predicates.empty() &&
						!expansion.target_filter.required_labels.empty()) {
					const auto cached = targetLabelMatches.find(targetId);
					const bool matches = cached != targetLabelMatches.end()
							? cached->second
							: snapshot->nodeHasLabels(targetId, expansion.target_filter.required_labels);
					if (!matches) {
						return true;
					}
				}
				if (batchedTargetNodesActive && !expansion.target_filter.required_labels.empty()) {
					const auto label = targetLabelMatches.find(targetId);
					if (label == targetLabelMatches.end() || !label->second) {
						return true;
					}
				}
						const auto batched = batchedEdges.find(edge.edge_id);
						const storage::LogicalGraphEdge materializedEdge = batched != batchedEdges.end()
								? batched->second
								: snapshot->readEdgeShaped(edge.edge_id, {.include_type = true, .all_properties = true});
						const auto targetIt = batchedNodes.find(targetId);
						// The page-specialized label-only path fills batchedNodes. Other
						// topology paths retain the generic complete-read fallback.
						const storage::LogicalGraphNode target = targetIt != batchedNodes.end()
								? targetIt->second
								: snapshot->readNodeShaped(targetId, completeNodeReadShape());
						if (!expansion.target_filter.property_predicates.empty() &&
								!matchesNode(target, expansion.target_filter, request.bounds, stats)) {
							return true;
						}
						const bool emitted = emit({source, materializedEdge, target});
						const bool shouldContinue = emitted && stats.rows_emitted < request.bounds.maximum_rows;
						edgeScanStopped = !shouldContinue;
						return shouldContinue;
					};
			const auto processFullEdge = [&](const storage::LogicalGraphEdge &edge,
														 const bool edgePropertiesAlreadyMatch = false,
														 const bool edgeAlreadyAccounted = false) {
						if (edgeScanStopped) {
							throw std::runtime_error("logical graph backend ignored an adjacency scan stop request");
						}
						if (!edgeAlreadyAccounted) {
							if (nodeEdges == request.bounds.maximum_edges_per_node) {
								throw std::runtime_error("logical graph backend exceeded the requested adjacency bound");
							}
							incrementBounded(nodeEdges, "logical graph per-Node edge counter overflowed");
							incrementBounded(stats.edges_scanned, "logical graph Edge counter overflowed");
						}
						const uint64_t targetId = expansion.direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
															  ? edge.target_node_id
															  : edge.source_node_id;
						const bool invalidDirection =
										expansion.direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
																? edge.source_node_id != source.node_id
																: edge.target_node_id != source.node_id;
						const bool typeMatches = !expansion.edge_filter.relationship_type.has_value() ||
								edge.type == *expansion.edge_filter.relationship_type;
						if (invalidDirection || targetId == 0U || !typeMatches ||
							(!edgePropertiesAlreadyMatch && !matchesEdge(edge, expansion.edge_filter, request.bounds, stats))) {
							if (invalidDirection || targetId == 0U) {
								throw std::runtime_error("logical graph backend returned an invalid adjacent Edge");
							}
							return true;
						}
						incrementBounded(stats.edges_matched, "logical graph matched-Edge counter overflowed");
						const auto targetIt = batchedNodes.find(targetId);
						const storage::LogicalGraphNode target = targetIt != batchedNodes.end()
								? targetIt->second
								: snapshot->readNodeShaped(targetId, completeNodeReadShape());
						if (!matchesNode(target, expansion.target_filter, request.bounds, stats)) {
							return true;
						}
						const bool emitted = emit({source, edge, target});
						const bool shouldContinue = emitted && stats.rows_emitted < request.bounds.maximum_rows;
						edgeScanStopped = !shouldContinue;
						return shouldContinue;
					};
			if (expansion.edge_filter.property_predicates.empty() &&
					snapshot->supportsNativeAdjacentEdgeRefScans() &&
					snapshot->supportsNativeAdjacentEdgeTopologyPages() &&
					expansion.target_filter.property_predicates.empty()) {
				batchedTargetNodesActive = true;
				// Page the topology cursor explicitly so Edge and target Node
				// materialization share one bounded batch. Optional target labels are
				// evaluated before complete records are read.
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
				uint64_t previousEdgeId = 0U;
				while (!edgeScanStopped) {
					const size_t remaining = request.bounds.maximum_edges_per_node - nodeEdges;
					if (remaining == 0U) {
						throw std::runtime_error("logical graph backend exceeded the requested adjacency bound");
					}
					// Keep topology validation, label lookup, and the follow-up shaped
					// Edge/Node materialization independently bounded for supernodes.
					const size_t pageRecords = std::min(remaining, DEFAULT_TOPOLOGY_BATCH_ROWS);
					const storage::LogicalGraphAdjacencyTopologyPage page = snapshot->scanAdjacentEdgeTopologies(
							source.node_id, expansion.direction, continuation, pageRecords);
					if (page.edges.size() > pageRecords) {
						throw std::runtime_error("logical graph backend exceeded the requested adjacency bound");
					}
					if (continuation.has_value() &&
							(page.edges.empty() || page.edges.front().edge_id != continuation->next_edge_id)) {
						throw std::runtime_error("logical graph backend returned an invalid topology continuation");
					}
					if (page.edges.empty() && page.continuation.has_value()) {
						throw std::runtime_error("logical graph backend returned an empty nonterminal topology page");
					}
					if (page.continuation.has_value() &&
							(page.continuation->previous_edge_id == 0U || page.continuation->next_edge_id == 0U ||
							 page.continuation->previous_edge_id != page.edges.back().edge_id)) {
						throw std::runtime_error("logical graph backend returned an invalid topology continuation");
					}
					std::vector<uint64_t> targetIds;
					targetIds.reserve(page.edges.size());
					std::unordered_map<uint64_t, storage::LogicalGraphEdgeTopology> expectedTopologies;
					expectedTopologies.reserve(page.edges.size());
					for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
						const bool validDirection = expansion.direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
								? edge.source_node_id == source.node_id : edge.target_node_id == source.node_id;
						const uint64_t targetId = expansion.direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
								? edge.target_node_id : edge.source_node_id;
						if (!validDirection || edge.edge_id == 0U || edge.edge_id <= previousEdgeId || targetId == 0U) {
							throw std::runtime_error("logical graph backend returned an invalid adjacent Edge");
						}
						previousEdgeId = edge.edge_id;
						if (!expectedTopologies.emplace(edge.edge_id, edge).second) {
							throw std::runtime_error("logical graph backend returned a duplicate adjacent Edge");
						}
						targetIds.push_back(targetId);
					}
					std::ranges::sort(targetIds);
					targetIds.erase(std::ranges::unique(targetIds).begin(), targetIds.end());
					targetLabelMatches.clear();
					if (expansion.target_filter.required_labels.empty()) {
						for (const uint64_t targetId: targetIds) {
							targetLabelMatches.emplace(targetId, true);
						}
					} else {
						const std::vector<bool> matches = snapshot->readNodeLabelMatchesSorted(
								targetIds, expansion.target_filter.required_labels);
						if (matches.size() != targetIds.size()) {
							throw std::runtime_error("logical graph backend changed target-label batch cardinality");
						}
						for (size_t index = 0U; index < targetIds.size(); ++index) {
							targetLabelMatches.emplace(targetIds[index], matches[index]);
						}
					}
					std::vector<uint64_t> materializeIds;
					materializeIds.reserve(page.edges.size());
					for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
						const auto target = targetLabelMatches.find(expansion.direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
								? edge.target_node_id : edge.source_node_id);
						if (target != targetLabelMatches.end() && target->second &&
								(!expansion.edge_filter.relationship_type.has_value() ||
								 edge.type == *expansion.edge_filter.relationship_type)) {
							materializeIds.push_back(edge.edge_id);
						}
					}
					std::ranges::sort(materializeIds);
					materializeIds.erase(std::ranges::unique(materializeIds).begin(), materializeIds.end());
					batchedEdges.clear();
					if (!materializeIds.empty()) {
						const std::vector<storage::LogicalGraphEdge> materialized = snapshot->readEdgesShapedSorted(
								materializeIds, {.include_type = true, .all_properties = true});
						if (materialized.size() != materializeIds.size()) {
							throw std::runtime_error("logical graph backend changed batched Edge cardinality");
						}
						for (size_t index = 0U; index < materialized.size(); ++index) {
							const storage::LogicalGraphEdge &edge = materialized[index];
							const auto expected = expectedTopologies.find(materializeIds[index]);
							if (edge.edge_id != materializeIds[index] || expected == expectedTopologies.end() ||
								edge.source_node_id != expected->second.source_node_id ||
								edge.target_node_id != expected->second.target_node_id || edge.type != expected->second.type) {
								throw std::runtime_error("logical graph backend returned mismatched batched Edge topology");
							}
							batchedEdges.emplace(materializeIds[index], materialized[index]);
						}
					}
					// The label-only topology path has already authenticated the target
					// set. Materialize each surviving target Node once per bounded page;
					// processTopology restores adjacency order without per-Edge reads.
					std::vector<uint64_t> targetMaterializeIds;
					targetMaterializeIds.reserve(materializeIds.size());
					for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
						const uint64_t targetId = expansion.direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
								? edge.target_node_id : edge.source_node_id;
						const auto label = targetLabelMatches.find(targetId);
						if (label != targetLabelMatches.end() && label->second &&
							(!expansion.edge_filter.relationship_type.has_value() ||
								edge.type == *expansion.edge_filter.relationship_type)) {
							targetMaterializeIds.push_back(targetId);
						}
					}
					std::ranges::sort(targetMaterializeIds);
					targetMaterializeIds.erase(std::ranges::unique(targetMaterializeIds).begin(), targetMaterializeIds.end());
					batchedNodes.clear();
					if (!targetMaterializeIds.empty()) {
						const std::vector<storage::LogicalGraphNode> materializedTargets =
								snapshot->readNodesShapedSorted(targetMaterializeIds, completeNodeReadShape());
						if (materializedTargets.size() != targetMaterializeIds.size()) {
							throw std::runtime_error("logical graph backend changed batched target Node cardinality");
						}
						for (size_t index = 0U; index < materializedTargets.size(); ++index) {
							if (materializedTargets[index].node_id != targetMaterializeIds[index]) {
								throw std::runtime_error("logical graph backend returned a mismatched batched target Node identity");
							}
							batchedNodes.emplace(targetMaterializeIds[index], materializedTargets[index]);
						}
					}
					for (const storage::LogicalGraphEdgeTopology &edge: page.edges) {
						if (!processTopology(edge)) {
							edgeScanStopped = true;
							break;
						}
					}
					if (edgeScanStopped || !page.continuation.has_value()) {
						break;
					}
					continuation = page.continuation;
				}
			} else if (!expansion.edge_filter.property_predicates.empty() &&
					snapshot->supportsNativeAdjacentEdgeRefScans()) {
				// Read only the predicate columns for each bounded adjacency page.  A
				// complete Edge is materialized only after the selected values match;
				// this keeps rejected high-degree relationships out of the full payload
				// path while preserving the public row shape for retained results.
				std::vector<std::string_view> propertyKeys;
				propertyKeys.reserve(expansion.edge_filter.property_predicates.size());
				for (const LogicalPropertyPredicate &predicate: expansion.edge_filter.property_predicates) {
					propertyKeys.push_back(predicate.property_key);
				}
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation;
				uint64_t previousEdgeId = 0U;
				while (!edgeScanStopped) {
					const size_t remaining = request.bounds.maximum_edges_per_node - nodeEdges;
					if (remaining == 0U) {
						throw std::runtime_error("logical graph backend exceeded the requested adjacency bound");
					}
					const size_t pageRecords = std::min(remaining, DEFAULT_TOPOLOGY_BATCH_ROWS);
					const storage::LogicalGraphAdjacencyReferencePage page = snapshot->scanAdjacentEdgeRefs(
							source.node_id, expansion.direction, continuation, pageRecords);
					if (page.edges.size() > pageRecords ||
							(page.edges.empty() && page.continuation.has_value()) ||
							(continuation.has_value() &&
								(page.edges.empty() || page.edges.front().entity_id != continuation->next_edge_id))) {
						throw std::runtime_error("logical graph backend returned an invalid selected-property adjacency page");
					}
					std::vector<uint64_t> edgeIds;
					edgeIds.reserve(page.edges.size());
					for (const storage::EntityRef &reference: page.edges) {
						if (!reference.is_edge || reference.entity_id == 0U || reference.entity_id <= previousEdgeId ||
								reference.source_node_id == 0U || reference.target_node_id == 0U) {
							throw std::runtime_error("logical graph backend returned an invalid selected-property Edge reference");
						}
						const bool endpointMatches = expansion.direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
								? reference.source_node_id == source.node_id
								: reference.target_node_id == source.node_id;
						if (!endpointMatches) {
							throw std::runtime_error("logical graph backend returned an Edge reference for the wrong source");
						}
						previousEdgeId = reference.entity_id;
						edgeIds.push_back(reference.entity_id);
						if (nodeEdges == request.bounds.maximum_edges_per_node) {
							throw std::runtime_error("logical graph backend exceeded the requested adjacency bound");
						}
						incrementBounded(nodeEdges, "logical graph per-Node edge counter overflowed");
						incrementBounded(stats.edges_scanned, "logical graph Edge counter overflowed");
					}
					const auto columns = snapshot->readEdgePropertyColumnsSorted(edgeIds, propertyKeys);
					if (columns.size() != propertyKeys.size()) {
						throw std::runtime_error("logical graph backend changed selected Edge Property column cardinality");
					}
					SelectionVector matches(page.edges.size());
					for (size_t index = 0U; index < page.edges.size(); ++index) {
						for (const auto &column: columns) {
							if (column.size() != page.edges.size()) {
								throw std::runtime_error("logical graph backend changed selected Edge Property result cardinality");
							}
						}
						std::vector<storage::LogicalGraphPropertyEntry> selectedEntries;
						selectedEntries.reserve(propertyKeys.size());
						for (size_t property = 0U; property < propertyKeys.size(); ++property) {
							selectedEntries.push_back({std::string(propertyKeys[property]), columns[property][index]});
						}
						const LogicalProperties selected = readInlineProperties(
								selectedEntries, request.bounds.maximum_property_entries, stats);
					if (!matchesPredicates(selected, expansion.edge_filter.property_predicates)) {
							matches.reject(index);
						}
					}
					std::vector<uint64_t> materializeIds;
					std::vector<size_t> materializeOrdinals;
					materializeIds.reserve(matches.selectedCount());
					materializeOrdinals.reserve(matches.selectedCount());
					matches.forEachSelected([&](const size_t index) {
						materializeIds.push_back(page.edges[index].entity_id);
						materializeOrdinals.push_back(index);
					});
					// Relationship type is topology, not a complete Edge Property.  Resolve
					// it with an identity/type-only batch before requesting full records so a
					// rejected type never pays for its Property collection.
					if (expansion.edge_filter.relationship_type.has_value() && !materializeIds.empty()) {
						const std::vector<storage::LogicalGraphEdge> typedEdges = snapshot->readEdgesShapedSorted(
								materializeIds, {.include_type = true});
						if (typedEdges.size() != materializeIds.size()) {
							throw std::runtime_error("logical graph backend changed selected Edge type batch cardinality");
						}
						std::vector<uint64_t> typeMatchedIds;
						std::vector<size_t> typeMatchedOrdinals;
						typeMatchedIds.reserve(materializeIds.size());
						typeMatchedOrdinals.reserve(materializeOrdinals.size());
						for (size_t index = 0U; index < typedEdges.size(); ++index) {
							const storage::LogicalGraphEdge &typedEdge = typedEdges[index];
							const storage::EntityRef &reference = page.edges[materializeOrdinals[index]];
							if (typedEdge.edge_id != materializeIds[index] ||
									typedEdge.source_node_id != reference.source_node_id ||
									typedEdge.target_node_id != reference.target_node_id) {
								throw std::runtime_error("logical graph backend returned mismatched selected Edge type topology");
							}
							if (*expansion.edge_filter.relationship_type == typedEdge.type) {
								typeMatchedIds.push_back(materializeIds[index]);
								typeMatchedOrdinals.push_back(materializeOrdinals[index]);
							}
						}
						materializeIds = std::move(typeMatchedIds);
						materializeOrdinals = std::move(typeMatchedOrdinals);
					}
					const std::vector<storage::LogicalGraphEdge> materializedEdges = materializeIds.empty()
							? std::vector<storage::LogicalGraphEdge>{}
							: snapshot->readEdgesShapedSorted(materializeIds, {.include_type = true, .all_properties = true});
					if (materializedEdges.size() != materializeIds.size()) {
						throw std::runtime_error("logical graph backend changed selected Edge batch cardinality");
					}
					for (size_t materializedIndex = 0U; materializedIndex < materializedEdges.size(); ++materializedIndex) {
						const storage::LogicalGraphEdge &edge = materializedEdges[materializedIndex];
						const storage::EntityRef &reference = page.edges[materializeOrdinals[materializedIndex]];
						if (edge.edge_id != materializeIds[materializedIndex] ||
								edge.source_node_id != reference.source_node_id ||
								edge.target_node_id != reference.target_node_id) {
							throw std::runtime_error(
									"logical graph backend returned mismatched selected Edge topology");
						}
					}
					// Selected Edge predicates can leave many parallel relationships to the
					// same target. Resolve each distinct target once for this bounded page,
					// then restore adjacency order in the selection loop below.
					std::vector<uint64_t> targetIds;
					targetIds.reserve(materializedEdges.size());
					for (const storage::LogicalGraphEdge &edge: materializedEdges) {
						if (!expansion.edge_filter.relationship_type.has_value() ||
								edge.type == *expansion.edge_filter.relationship_type) {
							targetIds.push_back(expansion.direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
									? edge.target_node_id
									: edge.source_node_id);
						}
					}
					std::ranges::sort(targetIds);
					targetIds.erase(std::unique(targetIds.begin(), targetIds.end()), targetIds.end());
					batchedNodes.clear();
					if (!targetIds.empty()) {
						const std::vector<storage::LogicalGraphNode> targets =
								snapshot->readNodesShapedSorted(targetIds, completeNodeReadShape());
						if (targets.size() != targetIds.size()) {
							throw std::runtime_error("logical graph backend changed selected target Node cardinality");
						}
						for (size_t index = 0U; index < targets.size(); ++index) {
							if (targets[index].node_id != targetIds[index]) {
								throw std::runtime_error("logical graph backend returned a mismatched selected target Node");
							}
							batchedNodes.emplace(targetIds[index], targets[index]);
						}
					}
					for (const storage::LogicalGraphEdge &edge: materializedEdges) {
						if (!processFullEdge(edge, true, true)) {
							edgeScanStopped = true;
							break;
						}
					}
					if (edgeScanStopped || !page.continuation.has_value()) {
						break;
					}
					if (page.edges.empty() || page.continuation->previous_edge_id != page.edges.back().entity_id ||
							page.continuation->next_edge_id == 0U ||
							page.continuation->previous_edge_id == page.continuation->next_edge_id) {
						throw std::runtime_error("logical graph backend returned an invalid selected-property continuation");
					}
					continuation = page.continuation;
				}
			} else if (expansion.edge_filter.property_predicates.empty() &&
					snapshot->supportsNativeAdjacentEdgeRefScans()) {
				if (!expansion.target_filter.required_labels.empty()) {
					// Backends without an explicit topology-page API still expose a
					// bounded callback stream. Buffer a small batch so target labels are
					// resolved through one sorted lookup instead of one point lookup per
					// Edge, then feed the authenticated records through the normal path.
					std::vector<storage::LogicalGraphEdgeTopology> pendingTopology;
					const size_t batchCapacity = std::max<size_t>(1U,
							std::min(DEFAULT_TOPOLOGY_BATCH_ROWS, request.bounds.maximum_edges_per_node));
					pendingTopology.reserve(batchCapacity);
					const auto flushTopology = [&]() {
						if (pendingTopology.empty()) {
							return true;
						}
						std::vector<uint64_t> targetIds;
						targetIds.reserve(pendingTopology.size());
						for (const auto &edge: pendingTopology) {
							if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
								throw std::runtime_error("logical graph backend returned an invalid adjacent Edge topology");
							}
							targetIds.push_back(expansion.direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
									? edge.target_node_id : edge.source_node_id);
						}
						std::ranges::sort(targetIds);
						targetIds.erase(std::unique(targetIds.begin(), targetIds.end()), targetIds.end());
						const std::vector<bool> matches = snapshot->readNodeLabelMatchesSorted(
								targetIds, expansion.target_filter.required_labels);
						if (matches.size() != targetIds.size()) {
							throw std::runtime_error("logical graph backend changed topology target-label cardinality");
						}
						targetLabelMatches.clear();
						for (size_t index = 0U; index < targetIds.size(); ++index) {
							targetLabelMatches.emplace(targetIds[index], matches[index]);
						}
						for (const auto &edge: pendingTopology) {
							if (!processTopology(edge)) {
								pendingTopology.clear();
								return false;
							}
						}
						pendingTopology.clear();
						return true;
					};
					const auto collectTopology = [&](const storage::LogicalGraphEdgeTopology &edge) {
						pendingTopology.push_back(edge);
						if (pendingTopology.size() == batchCapacity) {
							return flushTopology();
						}
						return true;
					};
				snapshot->visitAdjacentEdgeTopologies(source.node_id, expansion.direction,
						request.bounds.maximum_edges_per_node, collectTopology);
				if (!edgeScanStopped) {
					static_cast<void>(flushTopology());
				}
				} else {
					snapshot->visitAdjacentEdgeTopologies(source.node_id, expansion.direction,
							request.bounds.maximum_edges_per_node, processTopology);
				}
			} else {
				snapshot->visitAdjacentEdges(source.node_id, expansion.direction,
						request.bounds.maximum_edges_per_node, processFullEdge);
			}
			const bool shouldContinue = stats.rows_emitted < request.bounds.maximum_rows;
			nodeScanStopped = !shouldContinue;
			return shouldContinue;
		};
		// A label-only source scan does not need complete records for rejected
		// Nodes. Native readers can validate labels while the Entity page is pinned,
		// then materialize only rows that survive the label predicate. The same
		// source shape remains valid when an expansion follows: processNode() still
		// owns expansion semantics, but rejected source Nodes never become objects.
		if (request.source_filter.property_predicates.empty() &&
				snapshot->supportsNativeNodeRefScans()) {
			uint64_t previousNodeId = 0U;
			const auto processReference = [&](const storage::EntityRef &reference, const bool labelsMatch) {
				if (reference.is_edge || reference.entity_id == 0U || reference.entity_id <= previousNodeId) {
					throw std::runtime_error("logical graph backend returned an invalid or non-monotonic Node reference");
				}
				previousNodeId = reference.entity_id;
				if (!labelsMatch) {
					if (stats.nodes_scanned == request.bounds.maximum_node_records) {
						throw std::runtime_error("logical graph backend exceeded the requested Node scan bound");
					}
					incrementBounded(stats.nodes_scanned, "logical graph Node counter overflowed");
					return true;
				}
				if (request.expansion.has_value()) {
					// processNode owns source accounting and expansion semantics. Do not
					// pre-increment here or a matched source would be counted twice.
					const storage::LogicalGraphNode node =
							snapshot->readNodeShaped(reference.entity_id, completeNodeReadShape());
					if (node.node_id != reference.entity_id) {
						throw std::runtime_error("logical graph backend resolved a mismatched Node identity");
					}
					return processNode(node);
				}
				if (stats.nodes_scanned == request.bounds.maximum_node_records) {
					throw std::runtime_error("logical graph backend exceeded the requested Node scan bound");
				}
				incrementBounded(stats.nodes_scanned, "logical graph Node counter overflowed");
				incrementBounded(stats.nodes_matched, "logical graph matched-Node counter overflowed");
				const storage::LogicalGraphNode node =
						snapshot->readNodeShaped(reference.entity_id, completeNodeReadShape());
				if (node.node_id != reference.entity_id) {
					throw std::runtime_error("logical graph backend resolved a mismatched Node identity");
				}
				const bool emitted = emit({node, std::nullopt, std::nullopt});
				return emitted && stats.rows_emitted < request.bounds.maximum_rows;
			};
			if (request.source_filter.required_labels.empty()) {
				snapshot->visitNodeRefs(request.bounds.maximum_node_records,
						[&](const storage::EntityRef &reference) { return processReference(reference, true); });
			} else {
				snapshot->visitNodeRefsWithLabelMatches(request.bounds.maximum_node_records,
						request.source_filter.required_labels, processReference);
			}
		} else {
			snapshot->visitNodes(request.bounds.maximum_node_records, processNode);
		}
		return stats;
	}

} // namespace graph::query::execution
