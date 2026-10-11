/**
 * @file test_LogicalGraphTransactionOverlay.cpp
 * @brief Unit tests for transaction-local logical graph read overlays.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Types.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/LogicalGraphReadExecutor.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"

#include "storage/LogicalGraphTransactionOverlayTestFixture.hpp"

namespace graph::storage {

	namespace {
	using namespace transaction_overlay_test;

	TEST(LogicalGraphTransactionOverlayTest, PaginationRetainsOnlyDeletedGapTranslationsAndReplaysOldTokens) {
		for (const bool vectorSnapshot: {false, true}) {
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.native_topology_pages = true;
			base.edges.clear();
			for (uint64_t id = 11U; id <= 1010U; ++id)
				base.edges.emplace(id, LogicalGraphEdge{id, 1U, 2U, "LINK", {}});
			LogicalGraphTransactionOverlay overlay(transaction);
			overlay.deleteLogicalEdges({{512U}});
			auto snapshot = vectorSnapshot
				? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
				: overlay.openLogicalGraphReadSnapshot();
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset();
			graph::debug::PerfTrace::setValueCollectionEnabled(true);
			std::optional<LogicalGraphAdjacencyContinuation> current, first, gap;
			size_t count = 0U;
			do {
				const auto page = snapshot->scanAdjacentEdgeTopologies(
					1U, LogicalGraphReadDirection::LGRD_OUTGOING, current, 1U);
				ASSERT_EQ(page.edges.size(), 1U);
				EXPECT_NE(page.edges.front().edge_id, 512U);
				++count;
				if (!first) first = page.continuation;
				if (page.edges.front().edge_id == 511U) gap = page.continuation;
				current = page.continuation;
			} while (current);
			EXPECT_EQ(count, 999U);
			ASSERT_TRUE(first && gap);
			const auto replay = snapshot->scanAdjacentEdgeTopologies(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, first, 2U);
			ASSERT_EQ(replay.edges.size(), 2U);
			EXPECT_EQ(replay.edges.front().edge_id, 12U);
			const auto gapReplay = snapshot->scanAdjacentEdgeTopologies(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, gap, 2U);
			ASSERT_EQ(gapReplay.edges.size(), 2U);
			EXPECT_EQ(gapReplay.edges.front().edge_id, 513U);
			current.reset();
			size_t incomingCount = 0U;
			do {
				const auto page = snapshot->scanAdjacentEdgeTopologies(
					2U, LogicalGraphReadDirection::LGRD_INCOMING, current, 1U);
				ASSERT_EQ(page.edges.size(), 1U);
				++incomingCount;
				current = page.continuation;
			} while (current);
			EXPECT_EQ(incomingCount, 999U);
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			EXPECT_FALSE(trace.contains("native_graph.read.overlay_continuation_retained"));
			ASSERT_TRUE(trace.contains("native_graph.read.overlay_topology_continuation_retained"));
			EXPECT_EQ(trace.at("native_graph.read.overlay_topology_continuation_retained").totalValue, 2);
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, VectorExactScanRetainsOnlyTopKAndRetriesAfterPartialScoring) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		std::vector<LogicalGraphNodeCreateRequest> creates(8U, {{"Embedding"}, {{"value", PropertyValue(1.0)}}});
		const auto ids = overlay.createLogicalNodes(creates);
		auto &scorer = static_cast<InMemoryVectorSnapshot &>(transaction.pinnedLogicalGraphVectorSearchSnapshot());
		const std::array<float, 8U> scores{4.0F, 3.0F, 3.0F, 2.0F, 1.0F, 1.0F, 0.0F, 3.0F};
		std::vector<std::pair<float, uint64_t>> expected;
		for (size_t row = 0U; row < ids.size(); ++row) {
			scorer.scores.emplace(ids[row], scores[row]);
			expected.emplace_back(scores[row], ids[row]);
		}
		std::sort(expected.begin(), expected.end());
		auto snapshot = overlay.openLogicalGraphVectorSearchSnapshot();
		LogicalGraphVectorSearchRequest request;
		request.index_name = "embedding";
		request.query_vector = {1.0F, 0.0F};
		request.maximum_mapping_entries = 10U;
		request.maximum_raw_vector_reads = 10U;
		for (const size_t k: {0U, 1U, 3U, 20U}) {
			request.top_k = k;
			const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::setValueCollectionEnabled(true);
			graph::debug::PerfTrace::reset();
			const auto result = snapshot->searchLogicalVector(request);
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
			ASSERT_EQ(result.hits.size(), std::min(k, expected.size()));
			for (size_t row = 0U; row < result.hits.size(); ++row) {
				EXPECT_EQ(result.hits[row].score, expected[row].first);
				EXPECT_EQ(result.hits[row].node_id, expected[row].second);
			}
			if (k == 0U) EXPECT_FALSE(trace.contains("native_graph.read.overlay_vector_retained_hits"));
			else {
				ASSERT_TRUE(trace.contains("native_graph.read.overlay_vector_retained_hits"));
				const auto &entry = trace.at("native_graph.read.overlay_vector_retained_hits");
				EXPECT_EQ(entry.valueCalls, ids.size());
				int64_t retainedSum = 0;
				for (size_t row = 1U; row <= ids.size(); ++row) retainedSum += int64_t(std::min(row, k));
				EXPECT_EQ(entry.totalValue, retainedSum);
			}
		}
		request.top_k = 3U;
		ThrowAfterCancellationCheckpoint cancellation(6U);
		request.cancellation = &cancellation;
		EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
		request.cancellation = nullptr;
		const auto retry = snapshot->searchLogicalVector(request);
		ASSERT_EQ(retry.hits.size(), 3U);
		for (size_t row = 0U; row < retry.hits.size(); ++row) EXPECT_EQ(retry.hits[row].node_id, expected[row].second);
	}

	TEST(LogicalGraphTransactionOverlayTest, CancelledVectorOverlayScanPreservesStagedFinalStateForRetry) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		const std::vector<uint64_t> created = overlay.createLogicalNodes({{{"Embedding"}, {{"value", PropertyValue(1.0)}}}});
		ASSERT_EQ(created, (std::vector<uint64_t>{100U}));

		auto snapshot = overlay.openLogicalGraphVectorSearchSnapshot();
		ASSERT_NE(snapshot, nullptr);
		LogicalGraphVectorSearchRequest request;
		request.index_name = "embedding";
		request.query_vector = {1.0F, 0.0F};
		request.top_k = 1U;
		request.maximum_mapping_entries = 4U;
		request.maximum_raw_vector_reads = 4U;
		ThrowAfterCancellationCheckpoint cancellation(2U);
		request.cancellation = &cancellation;

		EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
		EXPECT_EQ(cancellation.checks, 2U);
		EXPECT_EQ(transaction.vectorScoreCalls(), 0U);
		EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->readNode(created.front()).node_id, created.front());

		request.cancellation = nullptr;
		const LogicalGraphVectorSearchResult retry = snapshot->searchLogicalVector(request);
		ASSERT_EQ(retry.hits.size(), 1U);
		EXPECT_EQ(retry.hits.front().node_id, created.front());
		EXPECT_TRUE(retry.stats.used_exact_overlay);
		EXPECT_GT(retry.stats.exact_overlay_candidate_scans, 0U);
	}

	TEST(LogicalGraphTransactionOverlayTest, VectorExactScanHonorsScorerReadShapeAndCompleteFallback) {
		for (const bool selected: {false, true}) {
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.nodes.at(1U) = {1U, {"Embedding"}, {{"value", PropertyValue(1.0)}, {"payload", PropertyValue(std::string(4096U, 'x'))}}};
			auto &scorer = static_cast<InMemoryVectorSnapshot &>(transaction.pinnedLogicalGraphVectorSearchSnapshot());
			scorer.selected_candidates = selected;
			LogicalGraphTransactionOverlay overlay(transaction);
			ASSERT_EQ(overlay.createLogicalNodes({{{"Embedding"}, {{"value", PropertyValue(1.0)},
					{"payload", PropertyValue(std::string(4096U, 'x'))}}}}).size(), 1U);
			ASSERT_EQ(overlay.createLogicalNodes({{{"DeletedTail"}, {}}}), (std::vector<uint64_t>{101U}));
			overlay.deleteLogicalNodes({{101U, false, 0U}});
			LogicalGraphVectorSearchRequest request;
			request.index_name = "embedding";
			request.query_vector = {1.0F, 0.0F};
			request.top_k = 2U;
			request.maximum_mapping_entries = 3U;
			request.maximum_raw_vector_reads = 3U;
			const auto result = overlay.openLogicalGraphVectorSearchSnapshot()->searchLogicalVector(request);
			ASSERT_EQ(result.hits.size(), 2U);
			EXPECT_EQ(result.hits[0].node_id, 1U);
			EXPECT_EQ(result.hits[1].node_id, 100U);
			EXPECT_EQ(result.stats.exact_overlay_candidate_scans, 3U);
			ASSERT_EQ(scorer.scored_nodes.size(), 3U);
			for (const auto &node: scorer.scored_nodes) {
				if (node.node_id == 2U) continue;
				EXPECT_EQ(node.properties.size(), selected ? 1U : 2U);
				EXPECT_TRUE(std::ranges::any_of(node.properties, [](const auto &entry) { return entry.key == "value"; }));
			}
			ASSERT_FALSE(base.scan_node_shapes.empty());
			EXPECT_EQ(base.scan_node_shapes.front().all_properties, !selected);
			request.maximum_mapping_entries = 2U;
			EXPECT_THROW(static_cast<void>(overlay.openLogicalGraphVectorSearchSnapshot()->searchLogicalVector(request)), std::runtime_error);
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, VectorSnapshotDelegatesTheReadSurfaceAcrossOverlayRevisions) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		std::unique_ptr<LogicalGraphVectorSearchSnapshot> snapshot = overlay.openLogicalGraphVectorSearchSnapshot();
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());

		EXPECT_EQ(snapshot->readNode(1U).node_id, 1U);
		EXPECT_TRUE(snapshot->containsNode(1U));
		EXPECT_EQ(snapshot->readEdge(10U).edge_id, 10U);
		ASSERT_FALSE(base.node_shapes.empty());
		EXPECT_EQ(base.node_shapes.back(), (EntityReadShape{.include_labels = true, .all_properties = true}));
		ASSERT_FALSE(base.edge_shapes.empty());
		EXPECT_EQ(base.edge_shapes.back(), (EntityReadShape{.include_type = true, .all_properties = true}));
		EXPECT_FALSE(snapshot->findNodesByExactProperties({"Base"}, {}, 2U, 2U).has_value());
		EXPECT_EQ(snapshot->scanNodesAfter(0U, 2U, 1U).nodes.size(), 2U);
		EXPECT_EQ(snapshot->scanAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)
				  .edges.size(),
			  1U);
		size_t visitedNodes = 0U;
		snapshot->visitNodes(2U, [&visitedNodes](const LogicalGraphNode &) {
			++visitedNodes;
			return true;
		});
		EXPECT_EQ(visitedNodes, 2U);
		size_t visitedEdges = 0U;
		snapshot->visitEdges(1U, [&visitedEdges](const LogicalGraphEdge &) {
			++visitedEdges;
			return true;
		});
		EXPECT_EQ(visitedEdges, 1U);
		EXPECT_TRUE(snapshot->mutationFence().has_value());
		EXPECT_EQ(snapshot->mutationFence()->logical_revision, 0U);

		const uint64_t created = overlay.createLogicalNodes({{{"Added"}, {}}}).front();
		EXPECT_EQ(snapshot->readNode(created).node_id, created);
		EXPECT_TRUE(snapshot->containsNode(created));
		EXPECT_EQ(snapshot->scanNodesAfter(2U, 2U, 1U).nodes,
				  (std::vector<LogicalGraphNode>{{created, {"Added"}}}));
		EXPECT_EQ(snapshot->mutationFence()->logical_revision, 1U);

		static_cast<void>(transaction.commit());
		EXPECT_THROW(static_cast<void>(snapshot->containsNode(1U)), std::logic_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, VectorSnapshotPreservesShapedBatchesAcrossOverlayRevisions) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		auto snapshot = overlay.openLogicalGraphVectorSearchSnapshot();
		const EntityReadShape nodeShape{.include_labels = true};
		const EntityReadShape edgeShape{.include_type = true};
		EXPECT_EQ(snapshot->readNodesShapedSorted(std::array<uint64_t, 2U>{1U, 2U}, nodeShape).size(), 2U);
		EXPECT_EQ(snapshot->readEdgesShapedSorted(std::array<uint64_t, 1U>{10U}, edgeShape).size(), 1U);
		EXPECT_EQ(transaction.readNodeBatchCalls(), 1U);
		EXPECT_EQ(transaction.readEdgeBatchCalls(), 1U);

		const uint64_t createdNode = overlay.createLogicalNodes({{{"Added"}, {{"name", PropertyValue("Grace")}}}}).front();
		const uint64_t createdEdge = overlay.createLogicalEdges({{1U, createdNode, "ADDED", {}}}).front();
		const auto nodes = snapshot->readNodesShapedSorted(std::array<uint64_t, 2U>{1U, createdNode}, nodeShape);
		ASSERT_EQ(nodes.size(), 2U);
		EXPECT_EQ(nodes.front().node_id, 1U);
		EXPECT_EQ(nodes.back(), (LogicalGraphNode{createdNode, {"Added"}, {}}));
		const auto edges = snapshot->readEdgesShapedSorted(std::array<uint64_t, 2U>{10U, createdEdge}, edgeShape);
		ASSERT_EQ(edges.size(), 2U);
		EXPECT_EQ(edges.front().edge_id, 10U);
		EXPECT_EQ(edges.back(), (LogicalGraphEdge{createdEdge, 1U, createdNode, "ADDED", {}}));
		EXPECT_EQ(transaction.readNodeBatchCalls(), 2U);
		EXPECT_EQ(transaction.readEdgeBatchCalls(), 2U);
		EXPECT_EQ(snapshot->readNodesShapedSorted({}, nodeShape).size(), 0U);
		EXPECT_EQ(snapshot->readEdgesShapedSorted({}, edgeShape).size(), 0U);
		EXPECT_THROW(static_cast<void>(snapshot->readNodesShapedSorted(
				std::array<uint64_t, 2U>{createdNode, 1U}, nodeShape)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(snapshot->readEdgesShapedSorted(
				std::array<uint64_t, 2U>{createdEdge, 10U}, edgeShape)), std::invalid_argument);
		transaction.rollback();
		EXPECT_THROW(static_cast<void>(snapshot->readNodesShapedSorted({}, nodeShape)), std::logic_error);
		EXPECT_THROW(static_cast<void>(snapshot->readEdgesShapedSorted({}, edgeShape)), std::logic_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, VectorSnapshotPreservesPropertyBatchesAcrossOverlayRevisions) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		auto snapshot = overlay.openLogicalGraphVectorSearchSnapshot();
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.edges.at(10U).properties = {{"weight", PropertyValue(int64_t(2))}};
		const std::array<std::string_view, 3U> nodeKeys{"name", "missing", "name"};
		const std::array<std::string_view, 2U> edgeKeys{"weight", "missing"};
		EXPECT_EQ(snapshot->readNodePropertiesSorted(std::array<uint64_t, 2U>{1U, 2U}, "name"),
				  (std::vector<PropertyValue>{PropertyValue("Ada"), PropertyValue()}));
		EXPECT_EQ(snapshot->readEdgePropertiesSorted(std::array<uint64_t, 1U>{10U}, "weight"),
				  (std::vector<PropertyValue>{PropertyValue(int64_t(2))}));
		EXPECT_EQ(base.read_node_property_batch_calls, 1U);
		EXPECT_EQ(base.read_edge_property_batch_calls, 1U);
		const auto baseNodes = snapshot->readNodePropertyColumnsSorted(std::array<uint64_t, 2U>{1U, 2U}, nodeKeys);
		ASSERT_EQ(baseNodes.size(), 3U);
		EXPECT_EQ(baseNodes[0], (std::vector<PropertyValue>{PropertyValue("Ada"), PropertyValue()}));
		EXPECT_EQ(baseNodes[1], (std::vector<PropertyValue>{PropertyValue(), PropertyValue()}));
		EXPECT_EQ(baseNodes[2], baseNodes[0]);
		const auto baseEdges = snapshot->readEdgePropertyColumnsSorted(std::array<uint64_t, 1U>{10U}, edgeKeys);
		ASSERT_EQ(baseEdges.size(), 2U);
		EXPECT_EQ(baseEdges[0], (std::vector<PropertyValue>{PropertyValue(int64_t(2))}));
		EXPECT_EQ(baseEdges[1], (std::vector<PropertyValue>{PropertyValue()}));
		EXPECT_EQ(base.read_node_property_column_calls, 1U);
		EXPECT_EQ(base.read_edge_property_column_calls, 1U);

		const uint64_t createdNode = overlay.createLogicalNodes({{{"Added"}, {{"name", PropertyValue("Grace")}}}}).front();
		const uint64_t createdEdge = overlay.createLogicalEdges(
				{{1U, createdNode, "ADDED", {{"weight", PropertyValue(int64_t(3))}}}}).front();
		const std::array<uint64_t, 3U> nodeIds{1U, 2U, createdNode};
		const std::array<uint64_t, 2U> edgeIds{10U, createdEdge};
		const auto priorNodeCalls = base.read_node_property_batch_calls;
		const auto priorEdgeCalls = base.read_edge_property_batch_calls;
		EXPECT_EQ(snapshot->readNodePropertiesSorted(nodeIds, "name"),
				  (std::vector<PropertyValue>{PropertyValue("Ada"), PropertyValue(), PropertyValue("Grace")}));
		EXPECT_EQ(snapshot->readEdgePropertiesSorted(edgeIds, "weight"),
				  (std::vector<PropertyValue>{PropertyValue(int64_t(2)), PropertyValue(int64_t(3))}));
		EXPECT_EQ(base.read_node_property_batch_calls, priorNodeCalls + 1U);
		EXPECT_EQ(base.read_edge_property_batch_calls, priorEdgeCalls + 1U);
		const auto nodes = snapshot->readNodePropertyColumnsSorted(nodeIds, nodeKeys);
		ASSERT_EQ(nodes.size(), 3U);
		EXPECT_EQ(nodes[0], (std::vector<PropertyValue>{PropertyValue("Ada"), PropertyValue(), PropertyValue("Grace")}));
		EXPECT_EQ(nodes[1], std::vector<PropertyValue>(3U));
		EXPECT_EQ(nodes[2], nodes[0]);
		const auto edges = snapshot->readEdgePropertyColumnsSorted(edgeIds, edgeKeys);
		ASSERT_EQ(edges.size(), 2U);
		EXPECT_EQ(edges[0], (std::vector<PropertyValue>{PropertyValue(int64_t(2)), PropertyValue(int64_t(3))}));
		EXPECT_EQ(edges[1], std::vector<PropertyValue>(2U));
		EXPECT_EQ(base.read_node_property_column_calls, 2U);
		EXPECT_EQ(base.read_edge_property_column_calls, 2U);
		EXPECT_EQ(snapshot->readNodePropertyColumnsSorted({}, nodeKeys), std::vector<std::vector<PropertyValue>>(3U));
		EXPECT_EQ(snapshot->readEdgePropertyColumnsSorted({}, edgeKeys), std::vector<std::vector<PropertyValue>>(2U));
		EXPECT_THROW(static_cast<void>(snapshot->readNodePropertiesSorted(nodeIds, "")), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(snapshot->readEdgePropertiesSorted(edgeIds, "")), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(snapshot->readNodePropertyColumnsSorted(
				std::array<uint64_t, 2U>{2U, 1U}, nodeKeys)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(snapshot->readEdgePropertyColumnsSorted(
				std::array<uint64_t, 2U>{createdEdge, 10U}, edgeKeys)), std::invalid_argument);
		overlay.deleteLogicalEdges({{createdEdge}});
		EXPECT_THROW(static_cast<void>(snapshot->readEdgePropertyColumnsSorted(edgeIds, edgeKeys)), std::runtime_error);
		transaction.rollback();
		EXPECT_THROW(static_cast<void>(snapshot->readNodePropertiesSorted({}, "name")), std::logic_error);
		EXPECT_THROW(static_cast<void>(snapshot->readEdgePropertiesSorted({}, "weight")), std::logic_error);
		EXPECT_THROW(static_cast<void>(snapshot->readNodePropertyColumnsSorted({}, {})), std::logic_error);
		EXPECT_THROW(static_cast<void>(snapshot->readEdgePropertyColumnsSorted({}, {})), std::logic_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, PreservesReadShapesAcrossOverlayAndVectorNodeScanPages) {
		for (const bool vectorSnapshot: {false, true}) {
			SCOPED_TRACE(vectorSnapshot);
			InMemoryLogicalTransaction transaction;
			LogicalGraphTransactionOverlay overlay(transaction);
			std::unique_ptr<LogicalGraphReadSnapshot> snapshot = vectorSnapshot
					? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
					: overlay.openLogicalGraphReadSnapshot();
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			EXPECT_EQ(snapshot->findPropertyToken("name"), 1U);
			EXPECT_FALSE(snapshot->findPropertyToken("missing").has_value());
			const auto initial = snapshot->scanNodesAfter(0U, 2U, 1U, EntityReadShape{});
			EXPECT_EQ(initial.nodes, (std::vector<LogicalGraphNode>{{1U, {}, {}}, {2U, {}, {}}}));
			ASSERT_EQ(base.scan_node_shapes.size(), 1U);
			EXPECT_EQ(base.scan_node_shapes.back(), EntityReadShape{});
			EXPECT_EQ(base.read_node_calls, 0U);
			overlay.replaceLogicalNodeProperties({{1U, {{"name", PropertyValue("Updated")}, {"rank", PropertyValue(int64_t(5))}}}});
			overlay.deleteLogicalNodes({{2U, true, 1U}});
			const auto created = overlay.createLogicalNodes({
					{{"Added"}, {{"name", PropertyValue("Grace")}, {"rank", PropertyValue(int64_t(7))}}},
					{{"Added"}, {{"name", PropertyValue("Linus")}}}});
			ASSERT_EQ(created.size(), 2U);
			EXPECT_EQ(snapshot->findPropertyToken("rank"), 2U);
			const size_t priorNodeReads = base.read_node_calls;
			for (const EntityReadShape &shape: {EntityReadShape{}, EntityReadShape{.include_labels = true},
					EntityReadShape{.property_tokens = {2U}}}) {
				SCOPED_TRACE(shape.include_labels);
				base.scan_node_shapes.clear();
				std::vector<LogicalGraphNode> actual;
				uint64_t after = 0U;
				for (size_t probe = 0U; probe < 4U; ++probe) {
					const auto page = snapshot->scanNodesAfter(after, 2U, 4U, shape);
					EXPECT_LE(page.metadata_pages_visited, 4U);
					actual.insert(actual.end(), page.nodes.begin(), page.nodes.end());
					if (!page.may_have_more) break;
					ASSERT_FALSE(page.nodes.empty());
					after = page.nodes.back().node_id;
				}
				ASSERT_EQ(actual.size(), 3U);
				EXPECT_EQ(actual[0].node_id, 1U);
				EXPECT_EQ(actual[1].node_id, created[0]);
				EXPECT_EQ(actual[2].node_id, created[1]);
				for (size_t row = 0U; row < actual.size(); ++row) {
					EXPECT_EQ(actual[row].labels, shape.include_labels
							? std::vector<std::string>{row == 0U ? "Base" : "Added"} : std::vector<std::string>{});
					const std::vector<LogicalGraphPropertyEntry> expectedProperties = !shape.property_tokens.empty() && row < 2U
							? std::vector<LogicalGraphPropertyEntry>{{"rank", PropertyValue(int64_t(row == 0U ? 5 : 7))}}
							: std::vector<LogicalGraphPropertyEntry>{};
					EXPECT_EQ(actual[row].properties, expectedProperties);
				}
				ASSERT_FALSE(base.scan_node_shapes.empty());
				for (const auto &seen: base.scan_node_shapes) EXPECT_EQ(seen, shape);
				EXPECT_EQ(base.read_node_calls, priorNodeReads);
			}
			transaction.rollback();
			EXPECT_THROW(static_cast<void>(snapshot->findPropertyToken("rank")), std::logic_error);
			EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 2U, 4U, EntityReadShape{})), std::logic_error);
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, VectorSnapshotRetainsAdjacencyContinuationsWithinEachRevision) {
		for (const bool topologyOnly: {false, true}) {
			SCOPED_TRACE(topologyOnly);
			InMemoryLogicalTransaction transaction;
			LogicalGraphTransactionOverlay overlay(transaction);
			auto snapshot = overlay.openLogicalGraphVectorSearchSnapshot();
			const uint64_t created = overlay.createLogicalNodes({{{"Created"}, {}}}).front();
			ASSERT_EQ(overlay.createLogicalEdges({{1U, created, "FIRST", {}}, {1U, created, "SECOND", {}}}),
					(std::vector<uint64_t>{200U, 201U}));
			const auto collect = [&](uint64_t nodeId) {
				std::vector<uint64_t> ids;
				std::optional<LogicalGraphAdjacencyContinuation> continuation;
				for (size_t probe = 0U; probe < 8U; ++probe) {
					if (topologyOnly) {
						const auto page = snapshot->scanAdjacentEdgeTopologies(
								nodeId, LogicalGraphReadDirection::LGRD_INCOMING, continuation, 1U);
						for (const auto &edge: page.edges) ids.push_back(edge.edge_id);
						continuation = page.continuation;
					} else {
						const auto page = snapshot->scanAdjacentEdges(
								nodeId, LogicalGraphReadDirection::LGRD_INCOMING, continuation, 1U);
						for (const auto &edge: page.edges) ids.push_back(edge.edge_id);
						continuation = page.continuation;
					}
					if (!continuation.has_value()) return ids;
				}
				ADD_FAILURE() << "adjacency scan did not terminate";
				return ids;
			};
			EXPECT_EQ(collect(created), (std::vector<uint64_t>{200U, 201U}));
			const auto priorContinuation = topologyOnly
					? snapshot->scanAdjacentEdgeTopologies(created, LogicalGraphReadDirection::LGRD_INCOMING,
							std::nullopt, 1U).continuation
					: snapshot->scanAdjacentEdges(created, LogicalGraphReadDirection::LGRD_INCOMING,
							std::nullopt, 1U).continuation;
			ASSERT_TRUE(priorContinuation.has_value());
			ASSERT_EQ(overlay.createLogicalEdges({{1U, created, "THIRD", {}}}), (std::vector<uint64_t>{202U}));
			const auto resumeOldRevision = [&] {
				if (topologyOnly) {
					static_cast<void>(snapshot->scanAdjacentEdgeTopologies(
							created, LogicalGraphReadDirection::LGRD_INCOMING, priorContinuation, 1U));
				} else {
					static_cast<void>(snapshot->scanAdjacentEdges(
							created, LogicalGraphReadDirection::LGRD_INCOMING, priorContinuation, 1U));
				}
			};
			EXPECT_THROW(resumeOldRevision(), std::runtime_error);
			EXPECT_EQ(collect(created), (std::vector<uint64_t>{200U, 201U, 202U}));
			overlay.deleteLogicalEdges({{200U}});
			EXPECT_EQ(collect(created), (std::vector<uint64_t>{201U, 202U}));
			ASSERT_EQ(overlay.createLogicalEdges({{1U, 2U, "BASE_TARGET_A", {}}, {1U, 2U, "BASE_TARGET_B", {}}}),
					(std::vector<uint64_t>{203U, 204U}));
			EXPECT_EQ(collect(2U), (std::vector<uint64_t>{10U, 203U, 204U}));
		}
	}


	} // namespace

} // namespace graph::storage
