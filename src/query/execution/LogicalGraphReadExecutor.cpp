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

namespace graph::query::execution {

	namespace {

		constexpr size_t MAXIMUM_FILTER_TERMS = 1024U;

		using LogicalProperties = std::unordered_map<std::string, PropertyValue>;

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
			snapshot->visitAdjacentEdges(
					source.node_id, expansion.direction, request.bounds.maximum_edges_per_node,
					[&](const storage::LogicalGraphEdge &edge) {
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
						if (invalidDirection || targetId == 0U ||
							!matchesEdge(edge, expansion.edge_filter, request.bounds, stats)) {
							if (invalidDirection || targetId == 0U) {
								throw std::runtime_error("logical graph backend returned an invalid adjacent Edge");
							}
							return true;
						}
						incrementBounded(stats.edges_matched, "logical graph matched-Edge counter overflowed");
						const storage::LogicalGraphNode target = snapshot->readNode(targetId);
						if (!matchesNode(target, expansion.target_filter, request.bounds, stats)) {
							return true;
						}
						const bool emitted = emit({source, edge, target});
						const bool shouldContinue = emitted && stats.rows_emitted < request.bounds.maximum_rows;
						edgeScanStopped = !shouldContinue;
						return shouldContinue;
					});
			const bool shouldContinue = stats.rows_emitted < request.bounds.maximum_rows;
			nodeScanStopped = !shouldContinue;
			return shouldContinue;
		};
		snapshot->visitNodes(request.bounds.maximum_node_records, processNode);
		return stats;
	}

} // namespace graph::query::execution
