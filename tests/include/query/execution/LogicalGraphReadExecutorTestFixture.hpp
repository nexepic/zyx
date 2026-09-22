/**
 * @file LogicalGraphReadExecutorTestFixture.hpp
 * @brief Shared logical graph read executor test fixture and helpers.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "CypherParserImpl.hpp"
#include "graph/core/Types.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/LogicalGraphAdjacencyCursor.hpp"
#include "graph/query/execution/LogicalGraphNodeCursor.hpp"
#include "graph/query/execution/LogicalGraphReadExecutor.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/IsNullExpression.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"

using namespace graph;
using namespace graph::query::execution;
using namespace graph::storage;
namespace logical = graph::query::logical;
namespace expressions = graph::query::expressions;

namespace {

	struct LogicalGraphFixtureData {
		std::map<uint64_t, LogicalGraphNode> nodes;
		std::map<uint64_t, LogicalGraphEdge> edges;
		bool ignore_node_bound = false;
		bool ignore_edge_bound = false;
		bool force_invalid_adjacency = false;
		bool ignore_stop_signal = false;
		bool return_empty_nonterminal_page = false;
		bool return_invalid_adjacency_continuation = false;
		uint8_t invalid_adjacency_continuation_mode = 0U;
		bool return_oversized_node_page = false;
		bool return_nonmonotonic_node_page = false;
		bool return_empty_nonterminal_adjacency_page = false;
		bool return_oversized_adjacency_page = false;
		bool return_invalid_adjacency_page_endpoint = false;
		bool return_nonmonotonic_adjacency_page = false;
		bool return_mismatched_topology_page_start = false;
		bool return_mismatched_edge_batch_endpoint = false;
		bool enable_exact_node_lookup = false;
		bool enable_native_exact_node_candidates = false;
		bool enable_native_range_node_candidates = false;
		bool disable_native_candidate_stream = false;
		bool enable_native_ref_scans = false;
		bool enable_native_adjacent_topology = false;
		bool enable_native_adjacent_topology_pages = false;
		bool enable_range_node_lookup = false;
		bool enable_composite_range_node_lookup = false;
		std::optional<std::vector<LogicalGraphNode>> exact_lookup_result;
		std::optional<IndexCandidateBatch> exact_candidate_result;
		std::optional<std::vector<IndexCandidateBatch>> exact_candidate_stream_results;
		std::optional<std::vector<IndexCandidateBatch>> range_candidate_stream_results;
		std::optional<std::vector<IndexCandidateBatch>> composite_range_candidate_stream_results;
		std::optional<IndexCandidateBatch> range_candidate_result;
		std::optional<IndexCandidateBatch> composite_range_candidate_result;
		std::optional<std::vector<LogicalGraphNode>> range_lookup_result;
		std::optional<std::vector<LogicalGraphNode>> composite_range_lookup_result;
		std::optional<uint64_t> native_snapshot_generation;
		std::optional<LogicalGraphPropertyRange> expected_range_lookup;
		std::optional<LogicalGraphCompositePropertyRange> expected_composite_range_lookup;
		std::shared_ptr<size_t> edge_topology_scan_count;
		std::shared_ptr<size_t> edge_record_scan_count;
		std::shared_ptr<size_t> exact_lookup_count;
		std::shared_ptr<size_t> exact_candidate_lookup_count;
		std::shared_ptr<size_t> exact_candidate_stream_batch_count;
		std::shared_ptr<size_t> range_candidate_stream_batch_count;
		std::shared_ptr<size_t> composite_range_candidate_stream_batch_count;
		std::shared_ptr<size_t> range_candidate_lookup_count;
		std::shared_ptr<size_t> composite_range_candidate_lookup_count;
		std::shared_ptr<size_t> range_lookup_count;
		std::shared_ptr<size_t> composite_range_lookup_count;
		std::shared_ptr<size_t> node_read_count;
		std::shared_ptr<size_t> node_batch_read_count;
		std::shared_ptr<size_t> node_label_batch_read_count;
		std::shared_ptr<size_t> node_property_column_read_count;
		std::shared_ptr<size_t> node_existence_probe_count;
		std::shared_ptr<size_t> node_existence_batch_probe_count;
		std::shared_ptr<size_t> edge_property_column_read_count;
		std::shared_ptr<size_t> edge_batch_read_count;
		std::shared_ptr<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>> edge_batch_read_requests;
	};

	class TestLogicalGraphSnapshot final : public LogicalGraphReadSnapshot {
	public:
		explicit TestLogicalGraphSnapshot(const LogicalGraphFixtureData &data) : data_(data) {}

		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override {
			if (data_.node_read_count) {
				++*data_.node_read_count;
			}
			const auto found = data_.nodes.find(nodeId);
			if (found == data_.nodes.end()) {
				throw std::out_of_range("test logical graph Node is absent");
			}
			return found->second;
		}

		[[nodiscard]] std::vector<LogicalGraphNode> readNodesShapedSorted(
				std::span<const uint64_t> nodeIds, const EntityReadShape &shape) const override {
			validateEntityReadShape(shape);
			if (data_.node_batch_read_count) {
				++*data_.node_batch_read_count;
			}
			std::vector<LogicalGraphNode> result;
			result.reserve(nodeIds.size());
			uint64_t previous = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (nodeId == 0U || nodeId <= previous) {
					throw std::invalid_argument("test shaped Node batch is not strictly ordered");
				}
				previous = nodeId;
				const auto found = data_.nodes.find(nodeId);
				if (found == data_.nodes.end()) {
					throw std::out_of_range("test logical graph Node is absent");
				}
				result.push_back(found->second);
			}
			return result;
		}

		[[nodiscard]] std::vector<bool> readNodeLabelMatchesSorted(
				std::span<const uint64_t> nodeIds, const std::vector<std::string> &requiredLabels) const override {
			if (data_.node_label_batch_read_count) {
				++*data_.node_label_batch_read_count;
			}
			std::vector<bool> result;
			result.reserve(nodeIds.size());
			uint64_t previous = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (nodeId == 0U || nodeId <= previous) {
					throw std::invalid_argument("test indexed candidate label IDs are not strictly increasing");
				}
				previous = nodeId;
				const auto found = data_.nodes.find(nodeId);
				if (found == data_.nodes.end()) {
					throw std::out_of_range("test indexed candidate Node is absent");
				}
				result.push_back(std::ranges::all_of(requiredLabels, [&found](const std::string &label) {
					return !label.empty() && std::ranges::find(found->second.labels, label) != found->second.labels.end();
				}));
			}
			return result;
		}

		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override {
			const auto found = data_.edges.find(edgeId);
			if (found == data_.edges.end()) {
				throw std::out_of_range("test logical graph Edge is absent");
			}
			return found->second;
		}

		[[nodiscard]] std::vector<LogicalGraphEdge> readEdgesShapedSorted(
				std::span<const uint64_t> edgeIds, const EntityReadShape &shape) const override {
			validateEntityReadShape(shape);
			if (data_.edge_batch_read_count) {
				++*data_.edge_batch_read_count;
			}
			if (data_.edge_batch_read_requests) {
				data_.edge_batch_read_requests->emplace_back(std::vector<uint64_t>(edgeIds.begin(), edgeIds.end()), shape);
			}
			std::vector<LogicalGraphEdge> result;
			result.reserve(edgeIds.size());
			uint64_t previous = 0U;
			for (const uint64_t edgeId: edgeIds) {
				if (edgeId == 0U || edgeId <= previous) {
					throw std::invalid_argument("test shaped Edge batch is not strictly ordered");
				}
				previous = edgeId;
				const auto found = data_.edges.find(edgeId);
				if (found == data_.edges.end()) {
					throw std::out_of_range("test logical graph Edge is absent");
				}
				result.push_back(found->second);
			}
			if (data_.return_mismatched_edge_batch_endpoint && !result.empty()) {
				++result.front().target_node_id;
			}
			return result;
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>> readEdgePropertyColumnsSorted(
				std::span<const uint64_t> edgeIds, std::span<const std::string_view> keys) const override {
			if (data_.edge_property_column_read_count) {
				++*data_.edge_property_column_read_count;
			}
			std::vector<std::vector<PropertyValue>> columns;
			columns.reserve(keys.size());
			for (const std::string_view key: keys) {
				std::vector<PropertyValue> values;
				values.reserve(edgeIds.size());
				for (const uint64_t edgeId: edgeIds) {
					const auto found = data_.edges.find(edgeId);
					if (found == data_.edges.end()) {
						throw std::out_of_range("test logical graph Edge is absent");
					}
					const auto property = std::ranges::find(found->second.properties, key,
							&LogicalGraphPropertyEntry::key);
					values.push_back(property == found->second.properties.end() ? PropertyValue() : property->value);
				}
				columns.push_back(std::move(values));
			}
			return columns;
		}

		[[nodiscard]] bool containsNode(uint64_t nodeId) const override {
			if (data_.node_existence_probe_count) {
				++*data_.node_existence_probe_count;
			}
			return data_.nodes.contains(nodeId);
		}

		[[nodiscard]] std::vector<bool> containsNodesSorted(
				std::span<const uint64_t> nodeIds) const override {
			if (!data_.node_existence_batch_probe_count) {
				return LogicalGraphReadSnapshot::containsNodesSorted(nodeIds);
			}
			++*data_.node_existence_batch_probe_count;
			std::vector<bool> result;
			result.reserve(nodeIds.size());
			uint64_t previous = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (nodeId == 0U || nodeId <= previous) {
					throw std::invalid_argument("test Node existence batch is not strictly ordered");
				}
				previous = nodeId;
				result.push_back(data_.nodes.contains(nodeId));
			}
			return result;
		}

		[[nodiscard]] bool supportsNodeExistenceProbes() const override { return true; }

		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
		findNodesByExactProperties(const std::vector<std::string> &labels,
											 const std::vector<std::pair<std::string, PropertyValue>> &properties,
											 size_t maximumNodes, size_t maximumPropertyEntries) const override {
			if (!data_.enable_exact_node_lookup) {
				return std::nullopt;
			}
			if (data_.exact_lookup_count) {
				++*data_.exact_lookup_count;
			}
			if (maximumNodes == 0U || maximumPropertyEntries == 0U || labels != std::vector<std::string>{"Person"} ||
				properties != std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(42)}}) {
				throw std::runtime_error("test logical graph exact lookup received an unexpected request");
			}
			if (data_.exact_lookup_result.has_value()) {
				return *data_.exact_lookup_result;
			}
			return std::vector<LogicalGraphNode>{data_.nodes.at(1U)};
		}

		[[nodiscard]] std::optional<IndexCandidateBatch> findNodeCandidatesByExactProperties(
				const std::vector<std::string> &labels,
				const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
				size_t maximumPropertyEntries) const override {
			if (!data_.enable_native_exact_node_candidates) {
				return std::nullopt;
			}
			if (data_.exact_candidate_lookup_count) {
				++*data_.exact_candidate_lookup_count;
			}
			if (maximumNodes == 0U || maximumPropertyEntries == 0U || labels != std::vector<std::string>{"Person"} ||
				properties != std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(42)}}) {
				throw std::runtime_error("test logical graph native exact candidate lookup received an unexpected request");
			}
			if (data_.exact_candidate_result.has_value()) {
				return *data_.exact_candidate_result;
			}
			return IndexCandidateBatch{
					{{1U, false, 0U, 0U, 0U}}, data_.native_snapshot_generation.value_or(1U), true, false, {}};
		}

		[[nodiscard]] bool supportsNativeExactNodeCandidates() const override {
			return data_.enable_native_exact_node_candidates;
		}

		[[nodiscard]] std::optional<bool>
		visitNodeCandidatesByExactProperties(const std::vector<std::string> &labels,
											 const std::vector<std::pair<std::string, PropertyValue>> &properties,
											 size_t maximumNodes, size_t maximumPropertyEntries,
											 const IndexCandidateBatchVisitor &visitor) const override {
			if (data_.disable_native_candidate_stream) {
				return std::nullopt;
			}
			if (!data_.exact_candidate_stream_results.has_value()) {
				return LogicalGraphReadSnapshot::visitNodeCandidatesByExactProperties(labels, properties, maximumNodes,
																					  maximumPropertyEntries, visitor);
			}
			if (!visitor) {
				throw std::invalid_argument("test exact candidate stream requires a visitor");
			}
			for (const IndexCandidateBatch &batch: *data_.exact_candidate_stream_results) {
				if (data_.exact_candidate_stream_batch_count) {
					++*data_.exact_candidate_stream_batch_count;
				}
				if (!visitor(batch)) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] std::optional<IndexCandidateBatch>
		findNodeCandidatesByPropertyRange(const std::vector<std::string> &labels,
										  const LogicalGraphPropertyRange &range, size_t maximumNodes,
										  size_t maximumPropertyEntries) const override {
			if (!data_.enable_native_range_node_candidates) {
				return std::nullopt;
			}
			if (data_.range_candidate_lookup_count) {
				++*data_.range_candidate_lookup_count;
			}
			const LogicalGraphPropertyRange expected = data_.expected_range_lookup.value_or(
					LogicalGraphPropertyRange{"age", PropertyValue(30), PropertyValue(), true, true});
			if (maximumNodes == 0U || maximumPropertyEntries == 0U || labels != std::vector<std::string>{"Person"} ||
				range != expected) {
				throw std::runtime_error(
						"test logical graph native range candidate lookup received an unexpected request");
			}
			if (data_.range_candidate_result.has_value()) {
				return *data_.range_candidate_result;
			}
			return IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}},
									   data_.native_snapshot_generation.value_or(1U),
									   false,
									   true,
									   {{"age", {PropertyValue(42)}}}};
		}

		[[nodiscard]] std::optional<IndexCandidateBatch>
		findNodeCandidatesByCompositePropertyRange(const std::vector<std::string> &labels,
												   const LogicalGraphCompositePropertyRange &request,
												   size_t maximumNodes, size_t maximumPropertyEntries) const override {
			if (!data_.enable_native_range_node_candidates) {
				return std::nullopt;
			}
			if (data_.composite_range_candidate_lookup_count) {
				++*data_.composite_range_candidate_lookup_count;
			}
			if (maximumNodes == 0U || maximumPropertyEntries == 0U || labels != std::vector<std::string>{"Person"} ||
				!data_.expected_composite_range_lookup.has_value() ||
				request != *data_.expected_composite_range_lookup) {
				throw std::runtime_error(
						"test logical graph native composite range candidate lookup received an unexpected request");
			}
			if (data_.composite_range_candidate_result.has_value()) {
				return *data_.composite_range_candidate_result;
			}
			return IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}},
									   data_.native_snapshot_generation.value_or(1U),
									   false,
									   true,
									   {{"city", {PropertyValue("Shanghai")}}, {"age", {PropertyValue(42)}}}};
		}

		[[nodiscard]] std::optional<bool> visitNodeCandidatesByPropertyRange(
				const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range, size_t maximumNodes,
				size_t maximumPropertyEntries, const IndexCandidateBatchVisitor &visitor) const override {
			if (data_.disable_native_candidate_stream) {
				return std::nullopt;
			}
			if (!data_.range_candidate_stream_results.has_value()) {
				return LogicalGraphReadSnapshot::visitNodeCandidatesByPropertyRange(labels, range, maximumNodes,
																					maximumPropertyEntries, visitor);
			}
			if (!visitor) {
				throw std::invalid_argument("test range candidate stream requires a visitor");
			}
			for (const IndexCandidateBatch &batch: *data_.range_candidate_stream_results) {
				if (data_.range_candidate_stream_batch_count) {
					++*data_.range_candidate_stream_batch_count;
				}
				if (!visitor(batch)) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] std::optional<bool>
		visitNodeCandidatesByCompositePropertyRange(const std::vector<std::string> &labels,
													const LogicalGraphCompositePropertyRange &request,
													size_t maximumNodes, size_t maximumPropertyEntries,
													const IndexCandidateBatchVisitor &visitor) const override {
			if (data_.disable_native_candidate_stream) {
				return std::nullopt;
			}
			if (!data_.composite_range_candidate_stream_results.has_value()) {
				return LogicalGraphReadSnapshot::visitNodeCandidatesByCompositePropertyRange(
						labels, request, maximumNodes, maximumPropertyEntries, visitor);
			}
			if (!visitor) {
				throw std::invalid_argument("test composite range candidate stream requires a visitor");
			}
			for (const IndexCandidateBatch &batch: *data_.composite_range_candidate_stream_results) {
				if (data_.composite_range_candidate_stream_batch_count) {
					++*data_.composite_range_candidate_stream_batch_count;
				}
				if (!visitor(batch)) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool supportsNativeRangeNodeCandidates() const override {
			return data_.enable_native_range_node_candidates;
		}

		[[nodiscard]] std::optional<uint64_t> nativeSnapshotGeneration() const override {
			return (data_.enable_native_exact_node_candidates || data_.enable_native_range_node_candidates)
					? data_.native_snapshot_generation
					: std::nullopt;
		}

		[[nodiscard]] bool supportsNativeNodeRefScans() const override {
			return data_.enable_native_ref_scans || data_.enable_native_exact_node_candidates ||
				data_.enable_native_range_node_candidates;
		}

		[[nodiscard]] std::vector<std::vector<PropertyValue>> readNodePropertyColumnsSorted(
				std::span<const uint64_t> nodeIds, std::span<const std::string_view> keys) const override {
			if (data_.node_property_column_read_count) {
				++*data_.node_property_column_read_count;
			}
			std::vector<std::vector<PropertyValue>> columns;
			columns.reserve(keys.size());
			for (const std::string_view key: keys) {
				std::vector<PropertyValue> values;
				values.reserve(nodeIds.size());
				for (const uint64_t nodeId: nodeIds) {
					const auto found = data_.nodes.find(nodeId);
					if (found == data_.nodes.end() || nodeId == 0U) {
						throw std::out_of_range("test logical graph Node is absent");
					}
					const auto property = std::ranges::find(found->second.properties, key,
							&LogicalGraphPropertyEntry::key);
					values.push_back(property == found->second.properties.end() ? PropertyValue() : property->value);
				}
				columns.push_back(std::move(values));
			}
			return columns;
		}

		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
		findNodesByPropertyRange(const std::vector<std::string> &labels, const LogicalGraphPropertyRange &range,
								 size_t maximumNodes, size_t maximumPropertyEntries) const override {
			if (!data_.enable_range_node_lookup) {
				return std::nullopt;
			}
			if (data_.range_lookup_count) {
				++*data_.range_lookup_count;
			}
			const LogicalGraphPropertyRange expected = data_.expected_range_lookup.value_or(
					LogicalGraphPropertyRange{"age", PropertyValue(30), PropertyValue(), true, true});
			if (maximumNodes == 0U || maximumPropertyEntries == 0U || labels != std::vector<std::string>{"Person"} ||
				range != expected) {
				throw std::runtime_error("test logical graph range lookup received an unexpected request");
			}
			if (data_.range_lookup_result.has_value()) {
				return *data_.range_lookup_result;
			}
			return std::vector<LogicalGraphNode>{data_.nodes.at(1U), data_.nodes.at(2U)};
		}

		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>> findNodesByCompositePropertyRange(
				const std::vector<std::string> &labels, const LogicalGraphCompositePropertyRange &request,
				size_t maximumNodes, size_t maximumPropertyEntries) const override {
			if (!data_.enable_composite_range_node_lookup) {
				return std::nullopt;
			}
			if (data_.composite_range_lookup_count) {
				++*data_.composite_range_lookup_count;
			}
			if (maximumNodes == 0U || maximumPropertyEntries == 0U || labels != std::vector<std::string>{"Person"} ||
				!data_.expected_composite_range_lookup.has_value() || request != *data_.expected_composite_range_lookup) {
				throw std::runtime_error("test logical graph composite range lookup received an unexpected request");
			}
			if (data_.composite_range_lookup_result.has_value()) {
				return *data_.composite_range_lookup_result;
			}
			return std::vector<LogicalGraphNode>{data_.nodes.at(1U)};
		}

		void visitNodes(size_t maximumRecords,
						const std::function<bool(const LogicalGraphNode &)> &visitor) const override {
			if (!data_.ignore_node_bound && data_.nodes.size() > maximumRecords) {
				throw std::runtime_error("test logical graph Node scan exceeds its bound");
			}
			for (const auto &[id, node]: data_.nodes) {
				static_cast<void>(id);
				if (!visitor(node) && !data_.ignore_stop_signal) {
					return;
				}
			}
		}

		void visitNodeRefs(size_t maximumRecords,
						const std::function<bool(const EntityRef &)> &visitor) const override {
			if (data_.nodes.size() > maximumRecords) {
				throw std::runtime_error("test logical graph Node reference scan exceeds its bound");
			}
			for (const auto &[id, node]: data_.nodes) {
				static_cast<void>(node);
				if (!visitor(EntityRef{id, false, 0U, 0U, 0U}) && !data_.ignore_stop_signal) {
					return;
				}
			}
		}

		void visitNodeRefsWithLabelMatches(size_t maximumRecords, const std::vector<std::string> &requiredLabels,
						const std::function<bool(const EntityRef &, bool)> &visitor) const override {
			if (data_.nodes.size() > maximumRecords) {
				throw std::runtime_error("test logical graph labeled Node reference scan exceeds its bound");
			}
			for (const auto &[id, node]: data_.nodes) {
				const bool matches = std::ranges::all_of(requiredLabels, [&node](const std::string &label) {
					return !label.empty() && std::ranges::find(node.labels, label) != node.labels.end();
				});
				if (!visitor(EntityRef{id, false, 0U, 0U, 0U}, matches) && !data_.ignore_stop_signal) {
					return;
				}
			}
		}

		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
														  size_t maximumMetadataPages) const override {
			if (maximumRecords == 0U || maximumMetadataPages == 0U) {
				throw std::invalid_argument("test logical graph Node page requires positive bounds");
			}
			LogicalGraphNodePage page;
			page.metadata_pages_visited = 1U;
			if (data_.return_empty_nonterminal_page) {
				page.may_have_more = true;
				return page;
			}
			if (data_.return_oversized_node_page && afterNodeId == 0U) {
				for (const auto &[id, node]: data_.nodes) {
					static_cast<void>(id);
					page.nodes.push_back(node);
				}
				return page;
			}
			for (const auto &[id, node]: data_.nodes) {
				if (id <= afterNodeId) {
					continue;
				}
				if (page.nodes.size() == maximumRecords) {
					page.may_have_more = true;
					break;
				}
				page.nodes.push_back(node);
			}
			if (data_.return_nonmonotonic_node_page && page.nodes.size() > 1U) {
				std::swap(page.nodes[0], page.nodes[1]);
			}
			return page;
		}

		[[nodiscard]] LogicalGraphAdjacencyPage
		scanAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
						  std::optional<LogicalGraphAdjacencyContinuation> continuation,
						  size_t maximumRecords) const override {
			if (nodeId == 0U || maximumRecords == 0U ||
				(direction != LogicalGraphReadDirection::LGRD_OUTGOING &&
				 direction != LogicalGraphReadDirection::LGRD_INCOMING)) {
				throw std::invalid_argument("test logical graph adjacency page has invalid bounds or direction");
			}
			std::vector<LogicalGraphEdge> adjacent;
			for (const auto &[id, edge]: data_.edges) {
				static_cast<void>(id);
				const bool matches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
											 ? edge.source_node_id == nodeId
											 : edge.target_node_id == nodeId;
				if (matches) {
					adjacent.push_back(edge);
				}
			}
			size_t first = 0U;
			if (continuation.has_value()) {
				if (continuation->previous_edge_id == 0U || continuation->next_edge_id == 0U) {
					throw std::invalid_argument("test logical graph adjacency continuation has a zero Edge ID");
				}
				while (first < adjacent.size() && adjacent[first].edge_id != continuation->next_edge_id) {
					++first;
				}
				if (first == 0U || first == adjacent.size() ||
					adjacent[first - 1U].edge_id != continuation->previous_edge_id) {
					throw std::runtime_error("test logical graph adjacency continuation does not match the chain");
				}
			}
			LogicalGraphAdjacencyPage page;
			if (data_.return_empty_nonterminal_adjacency_page) {
				page.continuation = LogicalGraphAdjacencyContinuation{10U, 11U};
				return page;
			}
			if (data_.return_oversized_adjacency_page) {
				page.edges = std::move(adjacent);
				return page;
			}
			if (data_.return_invalid_adjacency_page_endpoint) {
				const auto invalid = std::find_if(data_.edges.begin(), data_.edges.end(), [&](const auto &entry) {
					return direction == LogicalGraphReadDirection::LGRD_OUTGOING
								   ? entry.second.source_node_id != nodeId
								   : entry.second.target_node_id != nodeId;
				});
				if (invalid == data_.edges.end()) {
					throw std::runtime_error("test fixture has no invalid adjacent Edge");
				}
				page.edges.push_back(invalid->second);
				return page;
			}
			const size_t end = std::min(adjacent.size(), first + maximumRecords);
			page.edges.insert(page.edges.end(), adjacent.begin() + static_cast<std::ptrdiff_t>(first),
							  adjacent.begin() + static_cast<std::ptrdiff_t>(end));
			if (data_.return_nonmonotonic_adjacency_page && page.edges.size() > 1U) {
				std::swap(page.edges[0], page.edges[1]);
			}
			if (end < adjacent.size()) {
				page.continuation = {page.edges.back().edge_id, adjacent[end].edge_id};
				if (data_.return_invalid_adjacency_continuation) {
					if (data_.invalid_adjacency_continuation_mode == 1U) {
						page.continuation->next_edge_id = 0U;
					} else if (data_.invalid_adjacency_continuation_mode == 2U) {
						page.continuation->next_edge_id = page.continuation->previous_edge_id;
					} else {
						page.continuation->previous_edge_id = page.edges.back().edge_id + 1U;
					}
				}
			}
			return page;
		}

		void visitEdges(size_t maximumRecords,
						const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			if (data_.edge_record_scan_count) {
				++*data_.edge_record_scan_count;
			}
			if (data_.edges.size() > maximumRecords) {
				throw std::runtime_error("test logical graph Edge scan exceeds its bound");
			}
			for (const auto &[id, edge]: data_.edges) {
				static_cast<void>(id);
				if (!visitor(edge) && !data_.ignore_stop_signal) {
					return;
				}
			}
		}

		void visitEdgeTopologies(
				size_t maximumRecords, const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
			if (data_.edge_topology_scan_count) {
				++*data_.edge_topology_scan_count;
			}
			if (!data_.ignore_edge_bound && data_.edges.size() > maximumRecords) {
				throw std::runtime_error("test logical graph Edge topology scan exceeds its bound");
			}
			for (const auto &[id, edge]: data_.edges) {
				static_cast<void>(id);
				if (!visitor({edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type}) &&
					!data_.ignore_stop_signal) {
					return;
				}
			}
		}

		void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
								const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			if (data_.force_invalid_adjacency) {
				for (const auto &[id, edge]: data_.edges) {
					static_cast<void>(id);
					const bool hasWrongEndpoint = direction == LogicalGraphReadDirection::LGRD_OUTGOING
														  ? edge.source_node_id != nodeId
														  : edge.target_node_id != nodeId;
					if (hasWrongEndpoint) {
						static_cast<void>(visitor(edge));
						return;
					}
				}
			}
			std::vector<LogicalGraphEdge> adjacent;
			for (const auto &[id, edge]: data_.edges) {
				static_cast<void>(id);
				const bool matches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
											 ? edge.source_node_id == nodeId
											 : direction == LogicalGraphReadDirection::LGRD_INCOMING &&
													   edge.target_node_id == nodeId;
				if (matches) {
					adjacent.push_back(edge);
				}
			}
			if (!data_.ignore_edge_bound && adjacent.size() > maximumEdges) {
				throw std::runtime_error("test logical graph adjacency scan exceeds its bound");
			}
			for (const LogicalGraphEdge &edge: adjacent) {
				if (!visitor(edge) && !data_.ignore_stop_signal) {
					return;
				}
			}
		}

		void visitAdjacentEdgeTopologies(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
										 const std::function<bool(const LogicalGraphEdgeTopology &)> &visitor) const override {
			if (!data_.enable_native_adjacent_topology) {
				LogicalGraphReadSnapshot::visitAdjacentEdgeTopologies(nodeId, direction, maximumEdges, visitor);
				return;
			}
			if (data_.edge_topology_scan_count) {
				++*data_.edge_topology_scan_count;
			}
			std::vector<LogicalGraphEdgeTopology> adjacent;
			for (const auto &[id, edge]: data_.edges) {
				static_cast<void>(id);
				const bool matches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
										 ? edge.source_node_id == nodeId
										 : edge.target_node_id == nodeId;
				if (matches) {
					adjacent.push_back({edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type});
				}
			}
			if (adjacent.size() > maximumEdges) {
				throw std::runtime_error("test logical graph topology adjacency scan exceeds its bound");
			}
			for (const LogicalGraphEdgeTopology &edge: adjacent) {
				if (!visitor(edge) && !data_.ignore_stop_signal) {
					return;
				}
			}
		}

		[[nodiscard]] LogicalGraphAdjacencyTopologyPage scanAdjacentEdgeTopologies(
				uint64_t nodeId, LogicalGraphReadDirection direction,
				std::optional<LogicalGraphAdjacencyContinuation> continuation,
				size_t maximumRecords) const override {
			if (!data_.enable_native_adjacent_topology_pages) {
				return LogicalGraphReadSnapshot::scanAdjacentEdgeTopologies(nodeId, direction, std::move(continuation),
						maximumRecords);
			}
			if (data_.edge_topology_scan_count) {
				++*data_.edge_topology_scan_count;
			}
			const std::optional<uint64_t> expectedFirst = continuation.has_value()
					? std::optional<uint64_t>(continuation->next_edge_id)
					: std::nullopt;
			const LogicalGraphAdjacencyPage page = scanAdjacentEdges(nodeId, direction, std::move(continuation), maximumRecords);
			LogicalGraphAdjacencyTopologyPage result;
			result.continuation = page.continuation;
			result.edges.reserve(page.edges.size());
			for (const LogicalGraphEdge &edge: page.edges) {
				result.edges.push_back({edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type});
			}
			if (data_.return_mismatched_topology_page_start && expectedFirst.has_value() && !result.edges.empty()) {
				result.edges.front().edge_id = *expectedFirst + 1U;
			}
			return result;
		}

		[[nodiscard]] bool supportsNativeAdjacentEdgeRefScans() const override {
			return data_.enable_native_adjacent_topology;
		}

		[[nodiscard]] bool supportsNativeAdjacentEdgeTopologyPages() const override {
			return data_.enable_native_adjacent_topology_pages;
		}

	private:
		const LogicalGraphFixtureData &data_;
	};

	class TestLogicalGraphBackend final : public LogicalGraphReadBackend {
	public:
		explicit TestLogicalGraphBackend(LogicalGraphFixtureData data, bool returnNullSnapshot = false) :
			data_(std::move(data)), return_null_snapshot_(returnNullSnapshot) {}


		[[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
			if (return_null_snapshot_) {
				return nullptr;
			}
			return std::make_unique<TestLogicalGraphSnapshot>(data_);
		}

	private:
		LogicalGraphFixtureData data_;
		bool return_null_snapshot_ = false;
	};

	[[nodiscard]] LogicalGraphFixtureData graphData() {
		LogicalGraphFixtureData result;
		result.nodes.emplace(1U, LogicalGraphNode{1U, {"Person", "VIP"},
															{{"age", PropertyValue(42)}, {"status", PropertyValue("online")}}});
		result.nodes.emplace(2U, LogicalGraphNode{2U, {"Person"}, {{"age", PropertyValue(30)}}});
		result.nodes.emplace(3U, LogicalGraphNode{3U, {"Device"}});
		result.edges.emplace(10U, LogicalGraphEdge{10U, 1U, 2U, "KNOWS", {{"weight", PropertyValue(2)}}});
		result.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 3U, "OWNS"});
		result.edges.emplace(12U, LogicalGraphEdge{12U, 2U, 1U, "KNOWS"});
		return result;
	}

	[[maybe_unused]] [[nodiscard]] LogicalGraphReadExecutionBounds bounds() { return {3U, 2U, 4U, 10U}; }

	[[nodiscard]] LogicalPlanReadRuntimeBounds runtimeBounds() {
		return {
				.maximum_node_records = 3U,
				.maximum_node_page_records = 1U,
				.maximum_metadata_pages_per_node_page = 1U,
				.maximum_edges_per_node = 2U,
				.maximum_edge_records = 10U,
				.maximum_adjacency_page_records = 1U,
				.maximum_property_entries = 4U,
				.maximum_rows = 10U,
				.maximum_distinct_rows = 10U,
		};
	}

	[[maybe_unused]] [[nodiscard]] std::shared_ptr<expressions::Expression>
	sharedExpression(std::unique_ptr<expressions::Expression> expression) {
		return std::shared_ptr<expressions::Expression>(std::move(expression));
	}

	class UnsupportedLogicalReadOperator final : public logical::LogicalOperator {
	public:
		[[nodiscard]] logical::LogicalOpType getType() const override {
			return logical::LogicalOpType::LOP_ALGO_SHORTEST_PATH;
		}
		[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override { return {}; }
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }
		[[nodiscard]] std::unique_ptr<logical::LogicalOperator> clone() const override {
			return std::make_unique<UnsupportedLogicalReadOperator>();
		}
		[[nodiscard]] std::string toString() const override { return "UnsupportedLogicalReadOperator"; }
		void setChild(size_t, std::unique_ptr<logical::LogicalOperator>) override {}
		[[nodiscard]] std::unique_ptr<logical::LogicalOperator> detachChild(size_t) override { return nullptr; }
	};

	class MissingOutputLogicalSingleRow final : public logical::LogicalOperator {
	public:
		[[nodiscard]] logical::LogicalOpType getType() const override { return logical::LogicalOpType::LOP_SINGLE_ROW; }
		[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override { return {}; }
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"missing"}; }
		[[nodiscard]] std::unique_ptr<logical::LogicalOperator> clone() const override {
			return std::make_unique<MissingOutputLogicalSingleRow>();
		}
		[[nodiscard]] std::string toString() const override { return "MissingOutputLogicalSingleRow"; }
		void setChild(size_t, std::unique_ptr<logical::LogicalOperator>) override {}
		[[nodiscard]] std::unique_ptr<logical::LogicalOperator> detachChild(size_t) override { return nullptr; }
	};

	[[maybe_unused]] [[nodiscard]] LogicalPropertyPredicate predicate(std::string key, LogicalPropertyPredicateOp op, PropertyValue value,
															std::optional<PropertyValue> upper = std::nullopt) {
		LogicalPropertyPredicate result;
		result.property_key = std::move(key);
		result.op = op;
		result.value = std::move(value);
		result.upper_value = std::move(upper);
		return result;
	}

} // namespace
