/**
 * @file test_LogicalGraphReadExecutor.cpp
 * @brief Core unit tests for the bounded format-independent logical graph executor.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "query/execution/LogicalGraphReadExecutorTestFixture.hpp"

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/optimization/LogicalPlanOptimizer.hpp"
#include "graph/query/logical/operators/LogicalUnion.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"

TEST(LogicalGraphReadExecutorTest, StreamsFilteredNodesThroughTheLogicalSnapshot) {
	TestLogicalGraphBackend backend(graphData());
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"Person"};
	request.source_filter.property_predicates = {
			predicate("age", LogicalPropertyPredicateOp::LPP_RANGE_CLOSED, PropertyValue(35), PropertyValue(45))};
	request.bounds = bounds();
	std::vector<LogicalGraphReadRow> rows;

	const LogicalGraphReadExecutionStats stats =
			executor.execute(request, [&rows](const LogicalGraphReadRow &row) { rows.push_back(row); });

	ASSERT_EQ(rows.size(), 1U);
		EXPECT_EQ(rows.front(),
				  (LogicalGraphReadRow{LogicalGraphNode{1U, {"Person", "VIP"},
																	{{"age", PropertyValue(42)}, {"status", PropertyValue("online")}}},
																	std::nullopt, std::nullopt}));
	EXPECT_EQ(stats, (LogicalGraphReadExecutionStats{3U, 1U, 0U, 0U, 2U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LabelOnlyNativeScanMaterializesOnlyMatchingNodes) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_ref_scans = true;
	const auto nodeReads = std::make_shared<size_t>(0U);
	data.node_read_count = nodeReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"Person"};
	request.bounds = bounds();
	std::vector<LogicalGraphReadRow> rows;

	const LogicalGraphReadExecutionStats stats =
			executor.execute(request, [&rows](const LogicalGraphReadRow &row) {
				rows.push_back(row);
				return true;
			});

	EXPECT_EQ(rows.size(), 2U);
	EXPECT_EQ(stats.nodes_scanned, 3U);
	EXPECT_EQ(stats.nodes_matched, 2U);
	EXPECT_EQ(*nodeReads, 2U);
}

TEST(LogicalGraphReadExecutorTest, LabelOnlyNativeScanAvoidsRejectedExpansionSources) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_ref_scans = true;
	data.enable_native_adjacent_topology = true;
	const auto nodeReads = std::make_shared<size_t>(0U);
	data.node_read_count = nodeReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"Person"};
	request.expansion = LogicalGraphExpansion{};
	request.bounds = bounds();
	std::vector<LogicalGraphReadRow> rows;

	const LogicalGraphReadExecutionStats stats = executor.execute(request, [&rows](const LogicalGraphReadRow &row) {
		rows.push_back(row);
	});

	// Nodes 1 and 2 are valid sources; Node 3 is rejected by the label cursor
	// before a complete source object is materialized. The three target reads
	// remain necessary for the three emitted expansions.
	EXPECT_EQ(rows.size(), 3U);
	EXPECT_EQ(stats.nodes_scanned, 3U);
	EXPECT_EQ(stats.nodes_matched, 2U);
	EXPECT_EQ(*nodeReads, 5U);
}

TEST(LogicalGraphReadExecutorTest, OmitsEntitiesWithoutTheRequiredProperty) {
	TestLogicalGraphBackend backend(graphData());
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.property_predicates = {predicate("missing", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(1))};
	request.bounds = bounds();
	std::vector<LogicalGraphReadRow> rows;

	const LogicalGraphReadExecutionStats stats =
			executor.execute(request, [&rows](const LogicalGraphReadRow &row) { rows.push_back(row); });

	EXPECT_TRUE(rows.empty());
	EXPECT_EQ(stats, (LogicalGraphReadExecutionStats{3U, 0U, 0U, 0U, 3U, 0U}));
}

TEST(LogicalGraphReadExecutorTest, FiltersOwnerKeyedPropertiesWithoutReadingALegacyPropertyId) {
	LogicalGraphFixtureData data = graphData();
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.property_predicates = {predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(42))};
	request.bounds = bounds();
	std::vector<LogicalGraphReadRow> rows;

	const LogicalGraphReadExecutionStats stats =
			executor.execute(request, [&rows](const LogicalGraphReadRow &row) { rows.push_back(row); });

	ASSERT_EQ(rows.size(), 1U);
	EXPECT_EQ(rows.front().source_node.node_id, 1U);
	EXPECT_EQ(rows.front().source_node.properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"age", PropertyValue(42)}, {"status", PropertyValue("online")}}));
	EXPECT_EQ(stats, (LogicalGraphReadExecutionStats{3U, 1U, 0U, 0U, 3U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, RejectsInvalidOwnerKeyedPropertyEntriesBeforePredicateEvaluation) {
	LogicalGraphFixtureData duplicate = graphData();
	duplicate.nodes.at(1U).properties = {{"age", PropertyValue(42)}, {"age", PropertyValue(43)}};
	TestLogicalGraphBackend duplicateBackend(std::move(duplicate));
	LogicalGraphReadExecutor duplicateExecutor(duplicateBackend);
	LogicalGraphReadRequest request;
	request.source_filter.property_predicates = {predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(42))};
	request.bounds = bounds();
	EXPECT_THROW(static_cast<void>(duplicateExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
			 std::runtime_error);

	LogicalGraphFixtureData oversized = graphData();
	oversized.nodes.at(1U).properties = {
				{"age", PropertyValue(42)}, {"status", PropertyValue("online")}, {"tier", PropertyValue("gold")}};
	TestLogicalGraphBackend oversizedBackend(std::move(oversized));
	LogicalGraphReadExecutor oversizedExecutor(oversizedBackend);
	request.bounds.maximum_property_entries = 2U;
	EXPECT_THROW(static_cast<void>(oversizedExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
			 std::runtime_error);

}

TEST(LogicalGraphReadExecutorTest, ExpandsOutgoingEdgesAndFiltersBothEndpoints) {
	TestLogicalGraphBackend backend(graphData());
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "KNOWS";
	expansion.edge_filter.property_predicates = {predicate("weight", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(2))};
	expansion.target_filter.required_labels = {"Person"};
	request.expansion = std::move(expansion);
	request.bounds = bounds();
	std::vector<LogicalGraphReadRow> rows;

	const LogicalGraphReadExecutionStats stats =
			executor.execute(request, [&rows](const LogicalGraphReadRow &row) { rows.push_back(row); });

	ASSERT_EQ(rows.size(), 1U);
	ASSERT_TRUE(rows.front().edge.has_value());
	ASSERT_TRUE(rows.front().target_node.has_value());
	EXPECT_EQ(rows.front().edge->edge_id, 10U);
	EXPECT_EQ(rows.front().target_node->node_id, 2U);
	EXPECT_EQ(stats, (LogicalGraphReadExecutionStats{3U, 1U, 2U, 1U, 1U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, BatchesSelectedEdgePropertiesBeforeMaterializingMatches) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	data.edges.emplace(13U, LogicalGraphEdge{13U, 1U, 2U, "KNOWS", {{"weight", PropertyValue(2)}}});
	const auto selectedColumnReads = std::make_shared<size_t>(0U);
	data.edge_property_column_read_count = selectedColumnReads;
	const auto edgeBatchReads = std::make_shared<size_t>(0U);
	data.edge_batch_read_count = edgeBatchReads;
	const auto nodeBatchReads = std::make_shared<size_t>(0U);
	const auto nodePointReads = std::make_shared<size_t>(0U);
	data.node_batch_read_count = nodeBatchReads;
	data.node_read_count = nodePointReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "KNOWS";
	expansion.edge_filter.property_predicates = {predicate("weight", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(2))};
	expansion.target_filter.required_labels = {"Person"};
	request.expansion = std::move(expansion);
	request.bounds = bounds();
	request.bounds.maximum_edges_per_node = 3U;
	std::vector<LogicalGraphReadRow> rows;

	const LogicalGraphReadExecutionStats stats = executor.execute(request, [&rows](const LogicalGraphReadRow &row) {
		rows.push_back(row);
		return true;
	});

	ASSERT_EQ(rows.size(), 2U);
	ASSERT_TRUE(rows.front().edge.has_value());
	EXPECT_EQ(rows.front().edge->edge_id, 10U);
	EXPECT_EQ(rows.back().edge->edge_id, 13U);
	EXPECT_EQ(rows.front().target_node->node_id, 2U);
	EXPECT_EQ(rows.back().target_node->node_id, 2U);
	EXPECT_EQ(stats.edges_scanned, 3U);
	EXPECT_EQ(stats.edges_matched, 2U);
	EXPECT_EQ(*selectedColumnReads, 1U);
	// Each bounded page performs one type-only authentication batch followed by
	// one complete batch containing only type-matching Edges.
	EXPECT_EQ(*edgeBatchReads, 2U);
	EXPECT_EQ(*nodeBatchReads, 1U);
	EXPECT_EQ(*nodePointReads, 0U);
}

TEST(LogicalGraphReadExecutorTest, FiltersRelationshipTypeBeforeFullEdgeMaterialization) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	data.edges.at(11U).properties = {{"weight", PropertyValue(2)}};
	const auto requests = std::make_shared<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>>();
	data.edge_batch_read_requests = requests;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "KNOWS";
	expansion.edge_filter.property_predicates = {
			predicate("weight", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(2))};
	request.expansion = std::move(expansion);
	request.bounds = bounds();

	std::vector<LogicalGraphReadRow> rows;
	static_cast<void>(executor.execute(request, [&rows](const LogicalGraphReadRow &row) {
		rows.push_back(row);
		return true;
	}));

	ASSERT_EQ(rows.size(), 1U);
	ASSERT_TRUE(rows.front().edge.has_value());
	EXPECT_EQ(rows.front().edge->edge_id, 10U);
	ASSERT_EQ(requests->size(), 2U);
	EXPECT_EQ((*requests)[0].first, (std::vector<uint64_t>{10U, 11U}));
	EXPECT_TRUE((*requests)[0].second.include_type);
	EXPECT_FALSE((*requests)[0].second.all_properties);
	EXPECT_EQ((*requests)[1].first, (std::vector<uint64_t>{10U}));
	EXPECT_TRUE((*requests)[1].second.include_type);
	EXPECT_TRUE((*requests)[1].second.all_properties);
}

TEST(LogicalGraphReadExecutorTest, RejectsMismatchedSelectedEdgeBatchEndpoints) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	data.return_mismatched_edge_batch_endpoint = true;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "KNOWS";
	expansion.edge_filter.property_predicates = {
			predicate("weight", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(2))};
	request.expansion = std::move(expansion);
	request.bounds = bounds();

	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {
		return true;
	})), std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, BoundsSelectedEdgePropertyBatchesForSupernodes) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	const auto selectedColumnReads = std::make_shared<size_t>(0U);
	const auto edgeBatchReads = std::make_shared<size_t>(0U);
	data.edge_property_column_read_count = selectedColumnReads;
	data.edge_batch_read_count = edgeBatchReads;
	for (uint64_t edgeId = 100U; edgeId < 357U; ++edgeId) {
		data.edges.emplace(edgeId, LogicalGraphEdge{edgeId, 1U, 2U, "KNOWS", {{"weight", PropertyValue(int64_t{2})}}});
	}
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "KNOWS";
	expansion.edge_filter.property_predicates = {predicate("weight", LogicalPropertyPredicateOp::LPP_EQ,
			PropertyValue(int64_t{2}))};
	request.expansion = std::move(expansion);
	request.bounds = bounds();
	request.bounds.maximum_edges_per_node = 300U;
	request.bounds.maximum_rows = 300U;

	std::vector<uint64_t> edgeIds;
	const LogicalGraphReadExecutionStats stats = executor.execute(request, [&edgeIds](const LogicalGraphReadRow &row) {
		edgeIds.push_back(row.edge->edge_id);
		return true;
	});

	EXPECT_EQ(edgeIds.size(), 258U);
	EXPECT_TRUE(std::ranges::is_sorted(edgeIds));
	EXPECT_EQ(stats.edges_scanned, 259U);
	EXPECT_EQ(stats.edges_matched, 258U);
	EXPECT_EQ(*selectedColumnReads, 2U);
	// Two topology pages each perform a type-only batch and a filtered complete
	// batch; rejected relationship types never enter the latter.
	EXPECT_EQ(*edgeBatchReads, 4U);
}

TEST(LogicalGraphReadExecutorTest, UsesTopologyOnlyAdjacencyBeforeMaterializingMatchedEdges) {
		LogicalGraphFixtureData data = graphData();
		data.enable_native_adjacent_topology = true;
		data.enable_native_adjacent_topology_pages = true;
	const auto topologyScans = std::make_shared<size_t>(0U);
	const auto fullEdgeScans = std::make_shared<size_t>(0U);
	const auto nodeBatchReads = std::make_shared<size_t>(0U);
	data.edge_topology_scan_count = topologyScans;
	data.edge_record_scan_count = fullEdgeScans;
	data.node_batch_read_count = nodeBatchReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "KNOWS";
	expansion.target_filter.required_labels = {"Person"};
	request.expansion = std::move(expansion);
	request.bounds = bounds();
	std::vector<LogicalGraphReadRow> rows;

	const LogicalGraphReadExecutionStats stats =
			executor.execute(request, [&rows](const LogicalGraphReadRow &row) {
			rows.push_back(row);
			return true;
		});

	ASSERT_EQ(rows.size(), 1U);
	ASSERT_TRUE(rows.front().edge.has_value());
	EXPECT_EQ(rows.front().edge->edge_id, 10U);
	EXPECT_EQ(stats.edges_scanned, 2U);
	EXPECT_EQ(stats.edges_matched, 1U);
	EXPECT_EQ(*topologyScans, 1U);
	EXPECT_EQ(*fullEdgeScans, 0U);
	EXPECT_EQ(*nodeBatchReads, 1U);
}

TEST(LogicalGraphReadExecutorTest, BatchesUnfilteredTopologyEdgeAndTargetMaterialization) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	data.enable_native_adjacent_topology_pages = true;
	const auto edgeBatchReads = std::make_shared<size_t>(0U);
	const auto nodeBatchReads = std::make_shared<size_t>(0U);
	const auto nodePointReads = std::make_shared<size_t>(0U);
	data.edge_batch_read_count = edgeBatchReads;
	data.node_batch_read_count = nodeBatchReads;
	data.node_read_count = nodePointReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	request.expansion = std::move(expansion);
	request.bounds = bounds();
	std::vector<uint64_t> edgeIds;

	static_cast<void>(executor.execute(request, [&edgeIds](const LogicalGraphReadRow &row) {
		edgeIds.push_back(row.edge->edge_id);
		return true;
	}));

	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U, 11U}));
	EXPECT_EQ(*edgeBatchReads, 1U);
	EXPECT_EQ(*nodeBatchReads, 1U);
	EXPECT_EQ(*nodePointReads, 0U);
}

TEST(LogicalGraphReadExecutorTest, RejectsMismatchedTopologyPageBatchEndpoints) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	data.enable_native_adjacent_topology_pages = true;
	data.return_mismatched_edge_batch_endpoint = true;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	request.expansion = std::move(expansion);
	request.bounds = bounds();

	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {
		return true;
	})), std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, RejectsNonMonotonicTopologyPageBeforeMaterialization) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	data.enable_native_adjacent_topology_pages = true;
	data.return_nonmonotonic_adjacency_page = true;
	const auto edgeBatchReads = std::make_shared<size_t>(0U);
	data.edge_batch_read_count = edgeBatchReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	request.expansion = std::move(expansion);
	request.bounds = bounds();

	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {
		return true;
	})), std::runtime_error);
	EXPECT_EQ(*edgeBatchReads, 0U);
}

TEST(LogicalGraphReadExecutorTest, RejectsTopologyPageThatDoesNotResumeAtContinuation) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	data.enable_native_adjacent_topology_pages = true;
	data.return_mismatched_topology_page_start = true;
	for (uint64_t edgeId = 12U; edgeId <= 266U; ++edgeId) {
		data.edges.insert_or_assign(edgeId, LogicalGraphEdge{edgeId, 1U, 2U, "KNOWS"});
	}
	const auto edgeBatchReads = std::make_shared<size_t>(0U);
	data.edge_batch_read_count = edgeBatchReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	request.expansion = std::move(expansion);
	request.bounds = bounds();
	request.bounds.maximum_edges_per_node = 257U;
	request.bounds.maximum_rows = 257U;

	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {
		return true;
	})), std::runtime_error);
	// The first bounded page is valid. The malformed second page is rejected
	// before its Edge IDs reach the complete-record batch reader.
	EXPECT_EQ(*edgeBatchReads, 1U);
}

TEST(LogicalGraphReadExecutorTest, DeduplicatesTargetsWithinTopologyBatch) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	data.enable_native_adjacent_topology_pages = true;
	data.edges.emplace(13U, LogicalGraphEdge{13U, 1U, 2U, "KNOWS"});
	const auto nodeBatchReads = std::make_shared<size_t>(0U);
	data.node_batch_read_count = nodeBatchReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "KNOWS";
	expansion.target_filter.required_labels = {"Person"};
	request.expansion = std::move(expansion);
	request.bounds = bounds();
	request.bounds.maximum_edges_per_node = 3U;
	std::vector<LogicalGraphReadRow> rows;

	static_cast<void>(executor.execute(request, [&rows](const LogicalGraphReadRow &row) {
		rows.push_back(row);
		return true;
	}));
	ASSERT_EQ(rows.size(), 2U);
	EXPECT_EQ(rows[0].target_node->node_id, 2U);
	EXPECT_EQ(rows[1].target_node->node_id, 2U);
	EXPECT_EQ(*nodeBatchReads, 1U);
}

TEST(LogicalGraphReadExecutorTest, BoundsTopologyLabelBatchesForSupernodes) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	data.enable_native_adjacent_topology_pages = true;
	const auto topologyScans = std::make_shared<size_t>(0U);
	const auto edgeBatchReads = std::make_shared<size_t>(0U);
	const auto nodeBatchReads = std::make_shared<size_t>(0U);
	data.edge_topology_scan_count = topologyScans;
	data.edge_batch_read_count = edgeBatchReads;
	data.node_batch_read_count = nodeBatchReads;
	// The fixture already has one matching outgoing KNOWS Edge. Add enough
	// parallel Edges to cross the executor's bounded topology page size.
	for (uint64_t edgeId = 100U; edgeId < 357U; ++edgeId) {
		data.edges.emplace(edgeId, LogicalGraphEdge{edgeId, 1U, 2U, "KNOWS"});
	}
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "KNOWS";
	expansion.target_filter.required_labels = {"Person"};
	request.expansion = std::move(expansion);
	request.bounds = bounds();
	request.bounds.maximum_edges_per_node = 300U;
	request.bounds.maximum_rows = 300U;

	std::vector<uint64_t> edgeIds;
	const LogicalGraphReadExecutionStats stats = executor.execute(request, [&edgeIds](const LogicalGraphReadRow &row) {
		edgeIds.push_back(row.edge->edge_id);
		return true;
	});

	EXPECT_EQ(edgeIds.size(), 258U);
	EXPECT_TRUE(std::ranges::is_sorted(edgeIds));
	EXPECT_EQ(stats.edges_scanned, 259U);
	EXPECT_EQ(stats.edges_matched, 258U);
	// 258 topology records require two bounded pages rather than a single
	// maximum_edges_per_node-sized request; matching Edge and target batches
	// follow the same page boundary.
	EXPECT_EQ(*topologyScans, 2U);
	EXPECT_EQ(*edgeBatchReads, 2U);
	EXPECT_EQ(*nodeBatchReads, 2U);
}

TEST(LogicalGraphReadExecutorTest, BatchesTargetLabelsWithoutTopologyPageApi) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_ref_scans = true;
	data.enable_native_adjacent_topology = true;
	const auto labelBatchReads = std::make_shared<size_t>(0U);
	data.node_label_batch_read_count = labelBatchReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "KNOWS";
	expansion.target_filter.required_labels = {"Person"};
	request.expansion = std::move(expansion);
	request.bounds = bounds();
	std::vector<LogicalGraphReadRow> rows;

	static_cast<void>(executor.execute(request, [&rows](const LogicalGraphReadRow &row) {
		rows.push_back(row);
		return true;
	}));
	ASSERT_EQ(rows.size(), 1U);
	EXPECT_EQ(rows.front().target_node->node_id, 2U);
	EXPECT_EQ(*labelBatchReads, 1U);
}

TEST(LogicalGraphReadExecutorTest, ExpandsIncomingEdgesWithDirectionValidation) {
	TestLogicalGraphBackend backend(graphData());
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_INCOMING;
	expansion.edge_filter.relationship_type = "KNOWS";
	expansion.target_filter.required_labels = {"Person"};
	request.expansion = std::move(expansion);
	request.bounds = bounds();
	std::vector<LogicalGraphReadRow> rows;

	const LogicalGraphReadExecutionStats stats =
			executor.execute(request, [&rows](const LogicalGraphReadRow &row) { rows.push_back(row); });

	ASSERT_EQ(rows.size(), 1U);
	ASSERT_TRUE(rows.front().edge.has_value());
	ASSERT_TRUE(rows.front().target_node.has_value());
	EXPECT_EQ(rows.front().edge->edge_id, 12U);
	EXPECT_EQ(rows.front().target_node->node_id, 2U);
	EXPECT_EQ(stats, (LogicalGraphReadExecutionStats{3U, 1U, 1U, 1U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, AppliesTheOutputBoundWithoutMaterializingRows) {
	TestLogicalGraphBackend backend(graphData());
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	request.bounds.maximum_rows = 1U;
	std::vector<uint64_t> ids;

	const LogicalGraphReadExecutionStats stats = executor.execute(
			request, [&ids](const LogicalGraphReadRow &row) { ids.push_back(row.source_node.node_id); });

	EXPECT_EQ(ids, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(stats, (LogicalGraphReadExecutionStats{1U, 1U, 0U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, ExposesBoundedResumableNodePages) {
	TestLogicalGraphBackend backend(graphData());
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);

	const LogicalGraphNodePage first = snapshot->scanNodesAfter(0U, 2U, 1U);
	EXPECT_EQ(first.nodes, (std::vector<LogicalGraphNode>{
				LogicalGraphNode{1U, {"Person", "VIP"}, {{"age", PropertyValue(42)}, {"status", PropertyValue("online")}}},
				LogicalGraphNode{2U, {"Person"}, {{"age", PropertyValue(30)}}}}));
	EXPECT_TRUE(first.may_have_more);
	const LogicalGraphNodePage second = snapshot->scanNodesAfter(2U, 2U, 1U);
	EXPECT_EQ(second.nodes, (std::vector<LogicalGraphNode>{LogicalGraphNode{3U, {"Device"}}}));
	EXPECT_FALSE(second.may_have_more);
	EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 0U, 1U)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 0U)), std::invalid_argument);
}

TEST(LogicalGraphReadExecutorTest, PullCursorRetainsOneSnapshotAndRejectsInvalidPages) {
	TestLogicalGraphBackend backend(graphData());
	LogicalGraphNodeCursor cursor(backend, {2U, 1U});
	EXPECT_THROW(static_cast<void>(cursor.next()), std::logic_error);
	cursor.open();
	EXPECT_THROW(cursor.open(), std::logic_error);
	const auto first = cursor.next();
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(first->nodes[0].node_id, 1U);
	EXPECT_EQ(first->nodes[1].node_id, 2U);
	const auto second = cursor.next();
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(second->nodes[0].node_id, 3U);
	EXPECT_FALSE(cursor.next().has_value());
	cursor.close();
	EXPECT_FALSE(cursor.isOpen());

	LogicalGraphFixtureData invalid = graphData();
	invalid.return_empty_nonterminal_page = true;
	TestLogicalGraphBackend invalidBackend(std::move(invalid));
	LogicalGraphNodeCursor invalidCursor(invalidBackend, {1U, 1U});
	invalidCursor.open();
	EXPECT_THROW(static_cast<void>(invalidCursor.next()), std::runtime_error);
	EXPECT_THROW(static_cast<void>(LogicalGraphNodeCursor(backend, {0U, 1U})), std::invalid_argument);
}

TEST(LogicalGraphReadExecutorTest, AdjacencyPullCursorResumesAtTheNextEdgeAndEnforcesItsProtocol) {
	TestLogicalGraphBackend backend(graphData());
	LogicalGraphAdjacencyCursor cursor(backend, 1U, LogicalGraphReadDirection::LGRD_OUTGOING, {1U, 2U});
	EXPECT_THROW(static_cast<void>(cursor.next()), std::logic_error);
	cursor.open();
	EXPECT_THROW(cursor.open(), std::logic_error);
	const auto first = cursor.next();
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(first->edges, (std::vector<LogicalGraphEdge>{graphData().edges.at(10U)}));
	ASSERT_TRUE(first->continuation.has_value());
	EXPECT_EQ(*first->continuation, (LogicalGraphAdjacencyContinuation{10U, 11U}));
	const auto second = cursor.next();
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(second->edges, (std::vector<LogicalGraphEdge>{graphData().edges.at(11U)}));
	EXPECT_FALSE(second->continuation.has_value());
	EXPECT_FALSE(cursor.next().has_value());
	cursor.close();
	EXPECT_FALSE(cursor.isOpen());

	LogicalGraphFixtureData invalid = graphData();
	invalid.return_invalid_adjacency_continuation = true;
	TestLogicalGraphBackend invalidBackend(std::move(invalid));
	LogicalGraphAdjacencyCursor invalidCursor(invalidBackend, 1U, LogicalGraphReadDirection::LGRD_OUTGOING, {1U, 2U});
	invalidCursor.open();
	EXPECT_THROW(static_cast<void>(invalidCursor.next()), std::runtime_error);

	LogicalGraphAdjacencyCursor boundedCursor(backend, 1U, LogicalGraphReadDirection::LGRD_OUTGOING, {1U, 1U});
	boundedCursor.open();
	EXPECT_THROW(static_cast<void>(boundedCursor.next()), std::runtime_error);
	EXPECT_THROW(static_cast<void>(
						 LogicalGraphAdjacencyCursor(backend, 0U, LogicalGraphReadDirection::LGRD_OUTGOING, {1U, 1U})),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(
						 LogicalGraphAdjacencyCursor(backend, 1U, static_cast<LogicalGraphReadDirection>(0), {1U, 1U})),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(
						 LogicalGraphAdjacencyCursor(backend, 1U, LogicalGraphReadDirection::LGRD_OUTGOING, {2U, 1U})),
				 std::invalid_argument);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeStreamsFilterProjectSkipAndLimitOnOneSnapshot) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t(30)));
	auto filter = std::make_unique<logical::LogicalFilter>(std::move(scan), std::move(predicate));
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "age"), "age");
	auto project = std::make_unique<logical::LogicalProject>(std::move(filter), std::move(items));
	auto skip = std::make_unique<logical::LogicalSkip>(std::move(project), 1);
	logical::LogicalLimit root(std::move(skip), 1);
	std::vector<PropertyValue> ages;
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);

	const LogicalPlanReadRuntimeStats stats =
			runtime.execute(root, runtimeBounds(), [&ages](const LogicalPlanReadRow &row) {
				const auto found = row.values.find("age");
				EXPECT_NE(found, row.values.end());
				if (found != row.values.end()) {
					ages.push_back(std::get<PropertyValue>(found->second));
				}
				return true;
			});

	EXPECT_EQ(ages, (std::vector<PropertyValue>{PropertyValue(30)}));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{2U, 0U, 2U, 1U}));
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_TRUE(trace.contains("logical_plan.skip.discarded_rows"));
	ASSERT_TRUE(trace.contains("logical_plan.limit.delivered_rows"));
	ASSERT_TRUE(trace.contains("logical_plan.limit.early_stop"));
	EXPECT_EQ(trace.at("logical_plan.skip.discarded_rows").totalValue, 1);
	EXPECT_EQ(trace.at("logical_plan.limit.delivered_rows").totalValue, 1);
	EXPECT_EQ(trace.at("logical_plan.limit.early_stop").totalValue, 1);
}

TEST(LogicalGraphReadExecutorTest, SharedPropertyBatchReportsSelectionAndByteAccounting) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_ref_scans = true;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_GREATER,
			std::make_unique<expressions::LiteralExpression>(int64_t{40}));
	logical::LogicalFilter root(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), std::move(predicate));
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	std::vector<uint64_t> ids;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(root, runtimeBounds(), [&ids](const LogicalPlanReadRow &row) {
			ids.push_back(std::get<storage::LogicalGraphNode>(row.values.at("n")).node_id);
			return true;
		});
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	EXPECT_EQ(ids, std::vector<uint64_t>{1U});
	EXPECT_EQ(stats.nodes_scanned, 3U);
	ASSERT_TRUE(trace.contains("logical_plan.batch.flushes"));
	ASSERT_TRUE(trace.contains("logical_plan.batch.rows"));
	ASSERT_TRUE(trace.contains("logical_plan.selection.selected_rows"));
	ASSERT_TRUE(trace.contains("logical_plan.selection.rejected_rows"));
	ASSERT_TRUE(trace.contains("logical_plan.batch.materialized_bytes"));
	EXPECT_EQ(trace.at("logical_plan.batch.flushes").totalValue, 1);
	EXPECT_EQ(trace.at("logical_plan.batch.rows").totalValue, 2);
	EXPECT_EQ(trace.at("logical_plan.selection.selected_rows").totalValue, 1);
	EXPECT_EQ(trace.at("logical_plan.selection.rejected_rows").totalValue, 1);
	EXPECT_GT(trace.at("logical_plan.batch.materialized_bytes").totalValue, 0);
}

TEST(LogicalGraphReadExecutorTest, OptimizedLimitSkipPreservesStreamingRowsAndStopSignal) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	auto root = std::make_unique<logical::LogicalLimit>(
			std::make_unique<logical::LogicalSkip>(
					std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), 1),
			1);
	const auto optimized = graph::query::optimization::LogicalPlanOptimizer{}.optimize(std::move(root));
	ASSERT_NE(optimized.root, nullptr);
	EXPECT_EQ(optimized.rewrite_count, 1U);

	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*optimized.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{2U}));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{2U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeExecutesAParserProducedLogicalReadPlan) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	graph::query::QueryPlan plan =
			parser.parseToLogical("MATCH (n:Person) WHERE n.age >= 30 RETURN n.age AS age SKIP 1 LIMIT 1");
	ASSERT_NE(plan.root, nullptr);
	EXPECT_FALSE(plan.mutatesData);
	std::vector<PropertyValue> ages;
	const LogicalPlanReadRuntimeStats stats =
			runtime.execute(*plan.root, runtimeBounds(), [&ages](const LogicalPlanReadRow &row) {
				ages.push_back(std::get<PropertyValue>(row.values.at("age")));
				return true;
			});
	EXPECT_EQ(ages, (std::vector<PropertyValue>{PropertyValue(30)}));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{2U, 0U, 2U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimePaginatesAdjacencyAndPreservesLogicalBindings) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::unordered_map<std::string, PropertyValue> edgeProperties;
	edgeProperties.emplace("weight", PropertyValue(2));
	auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::move(scan), "n", "r", "m", "KNOWS", "out", std::vector<std::string>{"Person"},
			std::vector<std::pair<std::string, PropertyValue>>{}, std::move(edgeProperties));
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "source");
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r"), "relationship");
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("m"), "target");
	logical::LogicalProject root(std::move(traversal), std::move(items));
	std::vector<std::tuple<uint64_t, uint64_t, uint64_t>> rows;

	const LogicalPlanReadRuntimeStats stats =
			runtime.execute(root, runtimeBounds(), [&rows](const LogicalPlanReadRow &row) {
				const auto &source = std::get<LogicalGraphNode>(row.values.at("source"));
				const auto &edge = std::get<LogicalGraphEdge>(row.values.at("relationship"));
				const auto &target = std::get<LogicalGraphNode>(row.values.at("target"));
				rows.emplace_back(source.node_id, edge.edge_id, target.node_id);
				return true;
			});

	EXPECT_EQ(rows, (std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>{{1U, 10U, 2U}}));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{3U, 3U, 2U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeCachesRepeatedAdjacencyTargetsWithinOneSnapshot) {
	LogicalGraphFixtureData data = graphData();
	data.edges.emplace(13U, LogicalGraphEdge{13U, 1U, 2U, "KNOWS"});
	const auto nodeReads = std::make_shared<size_t>(0U);
	data.node_read_count = nodeReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalTraversal root(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"VIP"}), "source", "edge",
			"target", "", "out");
	LogicalPlanReadRuntimeBounds traversalBounds = runtimeBounds();
	traversalBounds.maximum_edges_per_node = 3U;
	std::vector<uint64_t> targetIds;

	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			root, traversalBounds, [&targetIds](const LogicalPlanReadRow &row) {
				targetIds.push_back(std::get<LogicalGraphNode>(row.values.at("target")).node_id);
				return true;
			});

	EXPECT_EQ(targetIds, (std::vector<uint64_t>{2U, 3U, 2U}));
	EXPECT_EQ(*nodeReads, 2U);
	EXPECT_EQ(stats.edges_scanned, 3U);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeHonorsTheConfiguredNodeReadCacheCapacity) {
	LogicalGraphFixtureData data = graphData();
	const auto nodeReads = std::make_shared<size_t>(0U);
	data.node_read_count = nodeReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalTraversal root(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"VIP"}), "source", "edge",
			"target", "", "out");
	LogicalPlanReadRuntimeBounds traversalBounds = runtimeBounds();
	// The cache shares the public Node-record bound, so two targets exercise the
	// bounded fallback without requiring a scale-sized graph.
	traversalBounds.maximum_node_records = 1U;
	traversalBounds.maximum_edges_per_node = 2U;
	std::vector<uint64_t> targetIds;

	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			root, traversalBounds, [&targetIds](const LogicalPlanReadRow &row) {
				targetIds.push_back(std::get<LogicalGraphNode>(row.values.at("target")).node_id);
				return true;
			});

	EXPECT_EQ(targetIds, (std::vector<uint64_t>{2U, 3U}));
	EXPECT_EQ(*nodeReads, 2U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{1U, 2U, 0U, 2U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeDirectlyCountsUnanchoredRelationships) {
	LogicalGraphFixtureData data = graphData();
	const auto topologyScans = std::make_shared<size_t>(0U);
	const auto recordScans = std::make_shared<size_t>(0U);
	const auto nodeReads = std::make_shared<size_t>(0U);
	const auto nodeExistenceProbes = std::make_shared<size_t>(0U);
	data.edge_topology_scan_count = topologyScans;
	data.edge_record_scan_count = recordScans;
	data.node_read_count = nodeReads;
	data.node_existence_probe_count = nodeExistenceProbes;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source"), "source", "edge", "target", "KNOWS", "out");
	logical::LogicalAggregate root(
			std::move(traversal), {},
			{{"count", sharedExpression(std::make_unique<expressions::VariableReferenceExpression>("edge")),
			  "relationship_count"}});

	std::vector<PropertyValue> counts;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			root, runtimeBounds(), [&counts](const LogicalPlanReadRow &row) {
				counts.push_back(std::get<PropertyValue>(row.values.at("relationship_count")));
				return true;
			});

	EXPECT_EQ(counts, (std::vector<PropertyValue>{PropertyValue(int64_t(2))}));
	// The direct path scans the Edge directory once rather than walking each
	// Node's adjacency chain and re-reading the same relationship endpoints.
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 3U, 0U, 1U}));
	EXPECT_EQ(*topologyScans, 1U);
	EXPECT_EQ(*recordScans, 0U);
	EXPECT_EQ(*nodeReads, 0U);
	EXPECT_EQ(*nodeExistenceProbes, 2U);

	// COUNT(*) has no aggregate argument but is still eligible for the same
	// topology-only path. With no relationship type filter it counts every
	// bounded relationship exactly once.
	auto allTraversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source"), "source", "edge", "target", "", "out");
	logical::LogicalAggregate allRoot(std::move(allTraversal), {}, {{"count", nullptr, "all_relationships"}});
	std::vector<PropertyValue> allCounts;
	const LogicalPlanReadRuntimeStats allStats = runtime.execute(
			allRoot, runtimeBounds(), [&allCounts](const LogicalPlanReadRow &row) {
				allCounts.push_back(std::get<PropertyValue>(row.values.at("all_relationships")));
				return true;
			});
	EXPECT_EQ(allCounts, (std::vector<PropertyValue>{PropertyValue(int64_t{3})}));
	EXPECT_EQ(allStats, (LogicalPlanReadRuntimeStats{0U, 3U, 0U, 1U}));
	EXPECT_EQ(*topologyScans, 2U);
	EXPECT_EQ(*recordScans, 0U);
	EXPECT_EQ(*nodeReads, 0U);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeDirectCountFallsBackWhenExistenceProbesAreUnavailable) {
	LogicalGraphFixtureData data = graphData();
	const auto nodeReads = std::make_shared<size_t>(0U);
	data.node_read_count = nodeReads;
	class CompleteReadOnlySnapshot final : public LogicalGraphReadSnapshot {
	public:
		explicit CompleteReadOnlySnapshot(const LogicalGraphFixtureData &data) : data_(data) {}
		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override {
			if (data_.node_read_count) {
				++*data_.node_read_count;
			}
			return data_.nodes.at(nodeId);
		}
		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override { return data_.edges.at(edgeId); }
		void visitNodes(size_t, const std::function<bool(const LogicalGraphNode &)> &) const override {
			throw std::logic_error("direct count must not scan Nodes");
		}
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override {
			throw std::logic_error("direct count must not page scan Nodes");
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdges(
				uint64_t, LogicalGraphReadDirection, std::optional<LogicalGraphAdjacencyContinuation>, size_t) const override {
			throw std::logic_error("direct count must not scan adjacency");
		}
		void visitEdges(size_t maximumRecords, const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			for (const auto &[id, edge]: data_.edges) {
				static_cast<void>(id);
				if (!visitor(edge)) {
					return;
				}
			}
			if (data_.edges.size() > maximumRecords) {
				throw std::runtime_error("direct count Edge scan exceeded its bound");
			}
		}
		void visitAdjacentEdges(uint64_t, LogicalGraphReadDirection, size_t,
								const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("direct count must not scan adjacency");
		}
	private:
		const LogicalGraphFixtureData &data_;
	};
	class CompleteReadOnlyBackend final : public LogicalGraphReadBackend {
	public:
		explicit CompleteReadOnlyBackend(LogicalGraphFixtureData data) : data_(std::move(data)) {}
		[[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
			return std::make_unique<CompleteReadOnlySnapshot>(data_);
		}
	private:
		LogicalGraphFixtureData data_;
	};

	CompleteReadOnlyBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source"), "source", "edge", "target", "KNOWS", "out");
	logical::LogicalAggregate root(
			std::move(traversal), {},
			{{"count", sharedExpression(std::make_unique<expressions::VariableReferenceExpression>("edge")), "count"}});

	std::vector<PropertyValue> counts;
	static_cast<void>(runtime.execute(root, runtimeBounds(), [&counts](const LogicalPlanReadRow &row) {
		counts.push_back(std::get<PropertyValue>(row.values.at("count")));
		return true;
	}));
	EXPECT_EQ(counts, (std::vector<PropertyValue>{PropertyValue(int64_t(2))}));
	EXPECT_EQ(*nodeReads, 2U);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeCountsAnchoredRelationshipsAcrossBothDirections) {
	LogicalGraphFixtureData data = graphData();
	const auto nodeReads = std::make_shared<size_t>(0U);
	data.node_read_count = nodeReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_edges_per_node = 3U;

	const auto countTargets = [&](std::vector<std::string> targetLabels) {
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"VIP"}), "source", "edge",
				"target", "KNOWS", "both", std::move(targetLabels));
		logical::LogicalAggregate root(
				std::move(traversal), {},
				{{"count", sharedExpression(std::make_unique<expressions::VariableReferenceExpression>("target")),
				  "target_count"}});
		std::vector<PropertyValue> counts;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				root, bounds, [&counts](const LogicalPlanReadRow &row) {
					counts.push_back(std::get<PropertyValue>(row.values.at("target_count")));
					return true;
				});
		EXPECT_EQ(counts.size(), 1U);
		return std::pair{counts.front(), stats};
	};

	// The anchored fast path must count one matching outgoing and one matching
	// incoming relationship, while skipping a differently typed outgoing edge.
	const auto [personCount, personStats] = countTargets({"Person"});
	EXPECT_EQ(personCount, PropertyValue(int64_t{2}));
	EXPECT_EQ(personStats, (LogicalPlanReadRuntimeStats{3U, 3U, 0U, 1U}));
	// Target label validation now uses the storage-neutral sorted label batch;
	// it does not require complete Node reads.
	EXPECT_EQ(*nodeReads, 0U);

	// A target predicate is evaluated from the same bounded topology traversal;
	// it cannot turn a nonmatching endpoint into an aggregate input row.
	const auto [deviceCount, deviceStats] = countTargets({"Device"});
	EXPECT_EQ(deviceCount, PropertyValue(int64_t{0}));
	EXPECT_EQ(deviceStats, (LogicalPlanReadRuntimeStats{3U, 3U, 0U, 1U}));
	EXPECT_EQ(*nodeReads, 0U);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeCountsABidirectionalSelfLoopOnlyOnce) {
	LogicalGraphFixtureData data = graphData();
	data.edges.emplace(13U, LogicalGraphEdge{13U, 1U, 1U, "KNOWS"});
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_edges_per_node = 5U;
	bounds.maximum_adjacency_page_records = 2U;

	auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"VIP"}), "source", "edge",
			"target", "KNOWS", "both");
	logical::LogicalAggregate root(
			std::move(traversal), {},
			{{"count", sharedExpression(std::make_unique<expressions::VariableReferenceExpression>("target")),
			  "target_count"}});

	std::vector<PropertyValue> counts;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			root, bounds, [&counts](const LogicalPlanReadRow &row) {
				counts.push_back(std::get<PropertyValue>(row.values.at("target_count")));
				return true;
			});

	// The self-loop is visible in both adjacency directions but denotes one
	// relationship, so only the outgoing visit may contribute it to COUNT().
	EXPECT_EQ(counts, (std::vector<PropertyValue>{PropertyValue(int64_t{3})}));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{3U, 5U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest,
	 LogicalPlanRuntimeRejectsAnchoredRelationshipCountsThatExceedThePerNodeEdgeBudget) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_edges_per_node = 2U;

	// The VIP source has two outgoing relationships and one incoming relationship.
	// The aggregate must reject that third relationship rather than return a truncated count.
	auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"VIP"}), "source", "edge",
			"target", "", "both");
	logical::LogicalAggregate root(
			std::move(traversal), {},
			{{"count", sharedExpression(std::make_unique<expressions::VariableReferenceExpression>("target")),
			  "target_count"}});

	EXPECT_THROW(static_cast<void>(runtime.execute(root, bounds, [](const LogicalPlanReadRow &) { return true; })),
			 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeCountsIncomingRelationshipsWithoutMaterializingTargets) {
	LogicalGraphFixtureData data = graphData();
	const auto nodeReads = std::make_shared<size_t>(0U);
	const auto nodeExistenceProbes = std::make_shared<size_t>(0U);
	data.node_read_count = nodeReads;
	data.node_existence_probe_count = nodeExistenceProbes;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);

	auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"VIP"}), "source", "edge",
			"target", "", "in");
	logical::LogicalAggregate root(
			std::move(traversal), {},
			{{"count", sharedExpression(std::make_unique<expressions::VariableReferenceExpression>("target")),
			  "target_count"}});

	std::vector<PropertyValue> counts;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			root, runtimeBounds(), [&counts](const LogicalPlanReadRow &row) {
				counts.push_back(std::get<PropertyValue>(row.values.at("target_count")));
				return true;
			});

	// Without target predicates, the anchored aggregate verifies the endpoint
	// through the snapshot probe instead of materializing an unused Node.
	EXPECT_EQ(counts, (std::vector<PropertyValue>{PropertyValue(int64_t{1})}));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{3U, 1U, 0U, 1U}));
	EXPECT_EQ(*nodeReads, 0U);
	EXPECT_EQ(*nodeExistenceProbes, 1U);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeBatchesFullAdjacencyEndpointValidation) {
	LogicalGraphFixtureData data = graphData();
	const auto scalarProbes = std::make_shared<size_t>(0U);
	const auto batchProbes = std::make_shared<size_t>(0U);
	data.node_existence_probe_count = scalarProbes;
	data.node_existence_batch_probe_count = batchProbes;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds limits = runtimeBounds();
	limits.maximum_adjacency_page_records = 2U;

	logical::LogicalTraversal traversal(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"VIP"}),
			"source", "edge", "", "", "out", {}, {}, {{"weight", PropertyValue(int64_t{2})}});
	std::vector<uint64_t> edgeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			traversal, limits, [&edgeIds](const LogicalPlanReadRow &row) {
				edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("edge")).edge_id);
				return true;
			});

	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U}));
	EXPECT_EQ(stats.edges_scanned, 2U);
	EXPECT_EQ(*batchProbes, 1U);
	EXPECT_EQ(*scalarProbes, 0U);
}

TEST(LogicalGraphReadExecutorTest, CountPropertyStreamsLargeValuesWithoutBatchMaterialization) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_ref_scans = true;
	data.nodes.at(1U).properties.emplace_back("payload", PropertyValue(std::string(4096U, 'x')));
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	const auto payload = sharedExpression(
			std::make_unique<expressions::VariableReferenceExpression>("n", "payload"));
	logical::LogicalAggregate root(
			std::make_unique<logical::LogicalNodeScan>("n"), {}, {{"count", payload, "payload_count"}});
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	// The transient EntityRef rows fit, while the selected Property payload does
	// not. COUNT only needs null/non-null cardinality and must stay streaming.
	bounds.maximum_materialized_bytes = 512U;
	std::vector<PropertyValue> counts;
	static_cast<void>(runtime.execute(root, bounds, [&counts](const LogicalPlanReadRow &row) {
		counts.push_back(std::get<PropertyValue>(row.values.at("payload_count")));
		return true;
	}));
	EXPECT_EQ(counts, (std::vector<PropertyValue>{PropertyValue(int64_t{1})}));
}

TEST(LogicalGraphReadExecutorTest, AggregateFlushesAtBoundedBatchRowsWithoutChangingCardinality) {
	std::vector<PropertyValue> values;
	values.reserve(1025U);
	for (int64_t value = 0; value < 1025; ++value) {
		values.emplace_back(value);
	}
	logical::LogicalUnwind unwind(std::make_unique<logical::LogicalSingleRow>(), "value", std::move(values));
	const auto argument = sharedExpression(
			std::make_unique<expressions::VariableReferenceExpression>("value"));
	logical::LogicalAggregate root(std::make_unique<logical::LogicalUnwind>(std::move(unwind)), {},
				{{"count", argument, "value_count"}});
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_rows = 2048U;
	std::vector<PropertyValue> counts;
	static_cast<void>(runtime.execute(root, bounds, [&counts](const LogicalPlanReadRow &row) {
		counts.push_back(std::get<PropertyValue>(row.values.at("value_count")));
		return true;
	}));
	EXPECT_EQ(counts, (std::vector<PropertyValue>{PropertyValue(int64_t{1025})}));
}

TEST(LogicalGraphReadExecutorTest, RejectsMalformedAggregateExtraArgumentsAtTheLogicalBoundary) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto value = sharedExpression(std::make_unique<expressions::LiteralExpression>(int64_t{1}));

	logical::LogicalAggregate missingPercentile(
			std::make_unique<logical::LogicalSingleRow>(), {},
			{{"percentileDisc", value, "percentile"}});
	EXPECT_THROW(static_cast<void>(runtime.execute(
				 missingPercentile, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
			 std::invalid_argument);

	logical::LogicalAggregate unexpectedExtra(
			std::make_unique<logical::LogicalSingleRow>(), {},
			{{"sum", value, "total", false,
			  sharedExpression(std::make_unique<expressions::LiteralExpression>(int64_t{0}))}});
	EXPECT_THROW(static_cast<void>(runtime.execute(
				 unexpectedExtra, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
			 std::invalid_argument);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeMaterializesLosslessLogicalEntityResults) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"});
	auto traversal = std::make_unique<logical::LogicalTraversal>(std::move(scan), "n", "r", "m", "KNOWS", "out",
																 std::vector<std::string>{"Person"});
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "source");
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r"), "relationship");
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("m"), "target");
	logical::LogicalProject root(std::move(traversal), std::move(items));

	const graph::query::QueryResult result = runtime.executeToQueryResult(root, runtimeBounds());

	ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"source", "relationship", "target"}));
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	const auto &source = row.at("source").asLogicalNode();
	EXPECT_EQ(source.node_id, 1U);
	EXPECT_EQ(source.labels, (std::vector<std::string>{"Person", "VIP"}));
	EXPECT_EQ(source.properties.at("age"), PropertyValue(42));
	EXPECT_EQ(source.properties.at("status"), PropertyValue("online"));
	const auto &relationship = row.at("relationship").asLogicalEdge();
	EXPECT_EQ(relationship.edge_id, 10U);
	EXPECT_EQ(relationship.source_node_id, 1U);
	EXPECT_EQ(relationship.target_node_id, 2U);
	EXPECT_EQ(relationship.type, "KNOWS");
	EXPECT_EQ(relationship.properties.at("weight"), PropertyValue(2));
	const auto &target = row.at("target").asLogicalNode();
	EXPECT_EQ(target.node_id, 2U);
	EXPECT_EQ(target.labels, (std::vector<std::string>{"Person"}));
	EXPECT_EQ(target.properties.at("age"), PropertyValue(30));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeMaterializationCachesAndFailsClosed) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);

	std::vector<logical::LogicalProjectItem> primitiveItems;
	primitiveItems.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t(42)), "answer");
	logical::LogicalProject primitiveRoot(std::make_unique<logical::LogicalSingleRow>(), std::move(primitiveItems));
	const graph::query::QueryResult primitiveResult = runtime.executeToQueryResult(primitiveRoot, runtimeBounds());
	ASSERT_EQ(primitiveResult.rowCount(), 1U);
	EXPECT_EQ(primitiveResult.getRows().front().at("answer").asPrimitive(), PropertyValue(42));

	std::vector<logical::LogicalProjectItem> duplicateItems;
	duplicateItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "first");
	duplicateItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "second");
	logical::LogicalProject duplicateRoot(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}),
			std::move(duplicateItems));
	const graph::query::QueryResult duplicateResult = runtime.executeToQueryResult(duplicateRoot, runtimeBounds());
	ASSERT_EQ(duplicateResult.rowCount(), 1U);
	EXPECT_EQ(duplicateResult.getRows().front().at("first").asLogicalNode(),
			  duplicateResult.getRows().front().at("second").asLogicalNode());

	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_EQUAL, std::make_unique<expressions::LiteralExpression>(int64_t(42)));
	logical::LogicalFilter cachedPropertyRoot(std::make_unique<logical::LogicalNodeScan>("n"), std::move(predicate));
	const graph::query::QueryResult cachedPropertyResult =
			runtime.executeToQueryResult(cachedPropertyRoot, runtimeBounds());
	ASSERT_EQ(cachedPropertyResult.rowCount(), 1U);
	EXPECT_EQ(cachedPropertyResult.getRows().front().at("n").asLogicalNode().properties.at("age"), PropertyValue(42));

	LogicalPlanReadRuntimeBounds oneRowBounds = runtimeBounds();
	oneRowBounds.maximum_rows = 1U;
	logical::LogicalNodeScan cappedRoot("n");
	const graph::query::QueryResult cappedResult = runtime.executeToQueryResult(cappedRoot, oneRowBounds);
	ASSERT_EQ(cappedResult.rowCount(), 1U);
	EXPECT_EQ(cappedResult.getRows().front().at("n").asLogicalNode().node_id, 1U);

	MissingOutputLogicalSingleRow malformedRoot;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(malformedRoot, runtimeBounds())), std::runtime_error);
	graph::query::QueryContext context;
	MissingOutputLogicalSingleRow malformedWithContext;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(malformedWithContext, runtimeBounds(), context)),
			 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, BatchesFilterPredicateBeforeMaterializingSelectedRows) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_ref_scans = true;
	data.node_property_column_read_count = std::make_shared<size_t>(0U);
	data.node_read_count = std::make_shared<size_t>(0U);
	data.node_batch_read_count = std::make_shared<size_t>(0U);
	const auto columnReads = data.node_property_column_read_count;
	const auto pointReads = data.node_read_count;
	const auto completeBatchReads = data.node_batch_read_count;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t(42)));
	logical::LogicalFilter root(std::make_unique<logical::LogicalNodeScan>("n"), std::move(predicate));

	std::vector<uint64_t> ids;
	static_cast<void>(runtime.execute(root, runtimeBounds(), [&ids](const LogicalPlanReadRow &row) {
		ids.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
		return true;
	}));

	EXPECT_EQ(ids, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*columnReads, 1U);
	// The selected-column batch rejects non-matching rows before the public
	// boundary materializes the surviving complete Node in one shaped batch.
	EXPECT_EQ(*completeBatchReads, 1U);
	EXPECT_EQ(*pointReads, 0U);
}

TEST(LogicalGraphReadExecutorTest, BatchedFilterPreservesDuplicateRowsAndNullPredicateSemantics) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_ref_scans = true;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t(42)));
	auto left = std::make_unique<logical::LogicalNodeScan>("n");
	auto right = std::make_unique<logical::LogicalNodeScan>("n");
	auto unionAll = std::make_unique<logical::LogicalUnion>(std::move(left), std::move(right), true);
	logical::LogicalFilter root(std::move(unionAll), std::move(predicate));

	std::vector<uint64_t> ids;
	static_cast<void>(runtime.execute(root, runtimeBounds(), [&ids](const LogicalPlanReadRow &row) {
		ids.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
		return true;
	}));
	EXPECT_EQ(ids, (std::vector<uint64_t>{1U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeBindsParametersAndHonorsQueryContext) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto parameterPlan = []() {
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::ParameterExpression>("answer"), "value");
		return std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(),
														 std::move(items));
	};

	graph::query::QueryContext context;
	context.parameters.emplace("answer", PropertyValue(42));
	context.parameters.emplace("allowed", PropertyValue(true));
	std::vector<PropertyValue> values;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*parameterPlan(), runtimeBounds(),
			[&values](const LogicalPlanReadRow &row) {
				values.push_back(std::get<PropertyValue>(row.values.at("value")));
				return true;
			},
			context);
	EXPECT_EQ(values, (std::vector<PropertyValue>{PropertyValue(42)}));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));

	logical::LogicalFilter parameterFilter(std::make_unique<logical::LogicalSingleRow>(),
										   std::make_shared<expressions::ParameterExpression>("allowed"));
	size_t filteredRows = 0U;
	static_cast<void>(runtime.execute(
			parameterFilter, runtimeBounds(),
			[&filteredRows](const LogicalPlanReadRow &) {
				++filteredRows;
				return true;
			},
			context));
	EXPECT_EQ(filteredRows, 1U);

	auto missingContextPlan = parameterPlan();
	EXPECT_THROW(static_cast<void>(runtime.execute(*missingContextPlan, runtimeBounds(),
												   [](const LogicalPlanReadRow &) { return true; })),
				 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, QueryEngineRoutesLogicalOnlyPlansToTheReadBackend) {
	auto backend = std::make_shared<TestLogicalGraphBackend>(graphData());
	graph::query::QueryEngine engine(backend);

	const graph::query::QueryResult result = engine.execute("MATCH (n:Person) WHERE n.age >= 30 RETURN n AS node");

	ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"node"}));
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows()[0].at("node").asLogicalNode().node_id, 1U);
	EXPECT_EQ(result.getRows()[1].at("node").asLogicalNode().node_id, 2U);

	graph::query::QueryContext parameterized;
	parameterized.parameters.emplace("minimumAge", PropertyValue(30));
	const graph::query::QueryResult parameterizedResult =
			engine.execute("MATCH (n:Person) WHERE n.age >= $minimumAge RETURN n AS node", parameterized);
	ASSERT_EQ(parameterizedResult.rowCount(), 2U);
	EXPECT_EQ(parameterizedResult.getRows()[0].at("node").asLogicalNode().node_id, 1U);
	EXPECT_EQ(parameterizedResult.getRows()[1].at("node").asLogicalNode().node_id, 2U);
	EXPECT_THROW(static_cast<void>(engine.execute("RETURN $missing AS value")), std::runtime_error);

	graph::query::QueryGuard guard(0, 0);
	graph::query::QueryContext guarded;
	guarded.guard = &guard;
	EXPECT_NO_THROW(static_cast<void>(engine.execute("MATCH (n:Person) RETURN n LIMIT 1", guarded)));
	EXPECT_THROW(static_cast<void>(engine.execute("CREATE (:Person)")), std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeSupportsRangeCompositeAndBothDirectionTraversal) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	auto rangeScan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	rangeScan->setRangePredicates({logical::RangePredicate{"age", PropertyValue(40), PropertyValue(42), true, true}});
	rangeScan->setCompositeEquality(
			logical::CompositeEqualityPredicate{{"age", "status"}, {PropertyValue(42), PropertyValue("online")}});
	std::vector<uint64_t> rangeIds;
	const LogicalPlanReadRuntimeStats rangeStats =
			runtime.execute(*rangeScan, runtimeBounds(), [&rangeIds](const LogicalPlanReadRow &row) {
				rangeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(rangeIds, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(rangeStats, (LogicalPlanReadRuntimeStats{3U, 0U, 2U, 1U}));

	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"});
	auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::move(scan), "n", "r", "m", "", "both", std::vector<std::string>{},
			std::vector<std::pair<std::string, PropertyValue>>{}, std::unordered_map<std::string, PropertyValue>{});
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r"), "relationship");
	logical::LogicalProject root(std::move(traversal), std::move(items));
	LogicalPlanReadRuntimeBounds bothBounds = runtimeBounds();
	bothBounds.maximum_edges_per_node = 3U;
	std::vector<uint64_t> edgeIds;
	const LogicalPlanReadRuntimeStats bothStats =
			runtime.execute(root, bothBounds, [&edgeIds](const LogicalPlanReadRow &row) {
				edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("relationship")).edge_id);
				return true;
			});
	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U, 11U, 12U}));
	EXPECT_EQ(bothStats, (LogicalPlanReadRuntimeStats{3U, 3U, 0U, 3U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsNonMonotonicAdjacencyPages) {
	LogicalGraphFixtureData data = graphData();
	data.return_nonmonotonic_adjacency_page = true;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalTraversal traversal(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m", "",
			"out");
	LogicalPlanReadRuntimeBounds traversalBounds = runtimeBounds();
	traversalBounds.maximum_adjacency_page_records = 2U;
	EXPECT_THROW(static_cast<void>(runtime.execute(traversal, traversalBounds,
																[](const LogicalPlanReadRow &) { return true; })),
			 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeUsesCompleteExactLookupWithoutMaterializingOtherProperties) {
	LogicalGraphFixtureData data = graphData();
	data.enable_exact_node_lookup = true;
	auto exactLookupCount = std::make_shared<size_t>(0U);
	data.exact_lookup_count = exactLookupCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"},
								  {{"age", PropertyValue(42)}});
	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(scan, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
		nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
		return true;
	});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*exactLookupCount, 1U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{1U, 0U, 1U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsNativeExactCandidatesOutsidePinnedSnapshot) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_exact_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.exact_candidate_result = IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 16U, true, false, {}};
	auto candidateLookupCount = std::make_shared<size_t>(0U);
	auto nodeReadCount = std::make_shared<size_t>(0U);
	data.exact_candidate_lookup_count = candidateLookupCount;
	data.node_read_count = nodeReadCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"}, {{"age", PropertyValue(42)}});

	EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
		return true;
	})), std::runtime_error);
	EXPECT_EQ(*candidateLookupCount, 1U);
	EXPECT_EQ(*nodeReadCount, 0U);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsNativeExactCandidatesWithoutSnapshotGeneration) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_exact_node_candidates = true;
	data.exact_candidate_result = IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 0U, true, false, {}};
	auto candidateLookupCount = std::make_shared<size_t>(0U);
	auto nodeReadCount = std::make_shared<size_t>(0U);
	data.exact_candidate_lookup_count = candidateLookupCount;
	data.node_read_count = nodeReadCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"}, {{"age", PropertyValue(42)}});

	EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
		return true;
	})), std::runtime_error);
	EXPECT_EQ(*candidateLookupCount, 1U);
	EXPECT_EQ(*nodeReadCount, 0U);
}

TEST(LogicalGraphReadExecutorTest, OptimizedFilterReusesVerifiedNativeExactCandidateFields) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_exact_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.exact_candidate_result = IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, true, false, {}};
	auto candidateLookupCount = std::make_shared<size_t>(0U);
	auto nodeReadCount = std::make_shared<size_t>(0U);
	auto nodeBatchReadCount = std::make_shared<size_t>(0U);
	data.exact_candidate_lookup_count = candidateLookupCount;
	data.node_read_count = nodeReadCount;
	data.node_batch_read_count = nodeBatchReadCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{42}));
	auto root = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), predicate);
	const auto optimized = graph::query::optimization::LogicalPlanOptimizer{}.optimize(std::move(root));
	ASSERT_NE(optimized.root, nullptr);
	ASSERT_EQ(optimized.rewrite_count, 1U);

	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats =
			runtime.execute(*optimized.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*candidateLookupCount, 1U);
	// One complete shaped Node batch remains necessary at the public result boundary;
	// the retained Filter must reuse the candidate's selected field instead of
	// causing another point read.
	EXPECT_EQ(*nodeBatchReadCount, 1U);
	EXPECT_EQ(*nodeReadCount, 0U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{1U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, NativeExactCandidateStreamStopsAtDownstreamLimit) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(2U).properties = {{"age", PropertyValue(42)}};
	data.enable_native_exact_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.exact_candidate_stream_results =
			std::vector<IndexCandidateBatch>{IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, true, false, {}},
											 IndexCandidateBatch{{{2U, false, 0U, 0U, 0U}}, 17U, true, false, {}}};
	auto streamBatchCount = std::make_shared<size_t>(0U);
	data.exact_candidate_stream_batch_count = streamBatchCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto limited = std::make_unique<logical::LogicalNodeScan>(
			"n", std::vector<std::string>{"Person"},
			std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(42)}});
	logical::LogicalLimit root(std::move(limited), 1);
	std::vector<uint64_t> ids;
	static_cast<void>(runtime.execute(root, runtimeBounds(), [&ids](const LogicalPlanReadRow &row) {
		ids.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
		return true;
	}));
	EXPECT_EQ(ids, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*streamBatchCount, 1U);
}

TEST(LogicalGraphReadExecutorTest, NativeExactCandidateStreamEnforcesTheCrossBatchNodeBound) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(2U).properties = {{"age", PropertyValue(42)}};
	data.enable_native_exact_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.exact_candidate_stream_results =
			std::vector<IndexCandidateBatch>{IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, true, false, {}},
											 IndexCandidateBatch{{{2U, false, 0U, 0U, 0U}}, 17U, true, false, {}}};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_node_records = 1U;
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"},
								  std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(42)}});

	try {
		static_cast<void>(runtime.execute(scan, bounds, [](const LogicalPlanReadRow &) { return true; }));
		FAIL() << "cross-batch candidates must honor the Node scan bound";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical graph indexed Node lookup exceeded the Node scan bound");
	}
}

TEST(LogicalGraphReadExecutorTest, NativeRangeCandidateStreamStopsAtDownstreamLimit) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).properties = {{"age", PropertyValue(42)}};
	data.nodes.at(2U).properties = {{"age", PropertyValue(43)}};
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.range_candidate_stream_results = std::vector<IndexCandidateBatch>{
			IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, false, true, {{"age", {PropertyValue(42)}}}},
			IndexCandidateBatch{{{2U, false, 0U, 0U, 0U}}, 17U, false, true, {{"age", {PropertyValue(43)}}}}};
	data.range_candidate_stream_batch_count = std::make_shared<size_t>(0U);
	auto streamBatchCount = data.range_candidate_stream_batch_count;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	scan->setRangePredicates({{"age", PropertyValue(42), PropertyValue(), true, true}});
	logical::LogicalLimit root(std::move(scan), 1);
	std::vector<uint64_t> ids;
	static_cast<void>(runtime.execute(root, runtimeBounds(), [&ids](const LogicalPlanReadRow &row) {
		const auto &value = row.values.at("n");
		ids.push_back(std::holds_alternative<storage::EntityRef>(value)
				? std::get<storage::EntityRef>(value).entity_id
				: std::get<storage::LogicalGraphNode>(value).node_id);
		return true;
	}));
	EXPECT_EQ(ids, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*streamBatchCount, 1U);
}

TEST(LogicalGraphReadExecutorTest, NativeCompositeRangeCandidateStreamStopsAtDownstreamLimit) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).properties = {{"status", PropertyValue("online")}, {"age", PropertyValue(42)}};
	data.nodes.at(2U).properties = {{"status", PropertyValue("online")}, {"age", PropertyValue(43)}};
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	IndexCandidateBatch first{{{1U, false, 0U, 0U, 0U}}, 17U, false, true,
			{{"status", {PropertyValue("online")}}, {"age", {PropertyValue(42)}}}};
	IndexCandidateBatch second{{{2U, false, 0U, 0U, 0U}}, 17U, false, true,
			{{"status", {PropertyValue("online")}}, {"age", {PropertyValue(43)}}}};
	data.composite_range_candidate_stream_results = std::vector<IndexCandidateBatch>{first, second};
	data.composite_range_candidate_stream_batch_count = std::make_shared<size_t>(0U);
	auto streamBatchCount = data.composite_range_candidate_stream_batch_count;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>(
			"n", std::vector<std::string>{"Person"},
			std::vector<std::pair<std::string, PropertyValue>>{{"status", PropertyValue("online")}});
	scan->setRangePredicates({{"age", PropertyValue(42), PropertyValue(), true, true}});
	logical::LogicalLimit root(std::move(scan), 1);
	std::vector<uint64_t> ids;
	static_cast<void>(runtime.execute(root, runtimeBounds(), [&ids](const LogicalPlanReadRow &row) {
		const auto &value = row.values.at("n");
		ids.push_back(std::holds_alternative<storage::EntityRef>(value)
							  ? std::get<storage::EntityRef>(value).entity_id
							  : std::get<storage::LogicalGraphNode>(value).node_id);
		return true;
	}));
	EXPECT_EQ(ids, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*streamBatchCount, 1U);
}

TEST(LogicalGraphReadExecutorTest, NativeRangeCandidateStreamAllowsValueMajorOrderAcrossBatches) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	// An ordered B+Tree range is value-major. Entity IDs may therefore move
	// backwards when the next value batch is delivered; each batch remains
	// strictly ordered and the stream must not impose a false global ID order.
	data.range_candidate_stream_results = std::vector<IndexCandidateBatch>{
			IndexCandidateBatch{{{2U, false, 0U, 0U, 0U}}, 17U, false, true,
					{{"age", {PropertyValue(30)}}}},
			IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, false, true,
					{{"age", {PropertyValue(42)}}}}};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
	scan.setRangePredicates({{"age", PropertyValue(30), PropertyValue(42), true, true}});

	std::vector<uint64_t> nodeIds;
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
		const auto &value = row.values.at("n");
		nodeIds.push_back(std::holds_alternative<storage::EntityRef>(value)
				? std::get<storage::EntityRef>(value).entity_id
				: std::get<storage::LogicalGraphNode>(value).node_id);
		return true;
	})));
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{2U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, NativeRangeCandidateStreamRejectsDuplicatesAcrossBatches) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.range_candidate_stream_results = std::vector<IndexCandidateBatch>{
			IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, false, true,
					{{"age", {PropertyValue(42)}}}},
			IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, false, true,
					{{"age", {PropertyValue(42)}}}}};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
	scan.setRangePredicates({{"age", PropertyValue(30), PropertyValue(42), true, true}});

	try {
		static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; }));
		FAIL() << "duplicate stream candidates must fail closed";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical graph indexed Node lookup returned duplicate Node references");
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeBoundsVerifiedIndexCandidateBytes) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.range_candidate_result = IndexCandidateBatch{
			{{1U, false, 0U, 0U, 0U}}, 17U, false, true, {{"age", {PropertyValue(std::string(128U, 'x'))}}}};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_candidate_bytes = 64U;
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
	scan.setRangePredicates({{"age", PropertyValue(int64_t{30}), PropertyValue(), true, true}});

	try {
		static_cast<void>(runtime.execute(scan, bounds, [](const LogicalPlanReadRow &) { return true; }));
		FAIL() << "oversized streamed candidates must fail closed";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical graph indexed Node candidate batch exceeded its byte bound");
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeBoundsNonStreamingIndexCandidateBytes) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_range_node_candidates = true;
	data.disable_native_candidate_stream = true;
	data.native_snapshot_generation = 17U;
	data.range_candidate_result = IndexCandidateBatch{
			{{1U, false, 0U, 0U, 0U}}, 17U, false, true, {{"age", {PropertyValue(std::string(128U, 'x'))}}}};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounds = runtimeBounds();
	bounds.maximum_candidate_bytes = 64U;
	logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
	scan.setRangePredicates({{"age", PropertyValue(int64_t{30}), PropertyValue(), true, true}});

	try {
		static_cast<void>(runtime.execute(scan, bounds, [](const LogicalPlanReadRow &) { return true; }));
		FAIL() << "oversized non-streaming candidates must fail closed";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical graph indexed Node candidate batch exceeded its byte bound");
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsMalformedNativeCandidateColumnsAndOrder) {
	{
		LogicalGraphFixtureData data = graphData();
		data.enable_native_range_node_candidates = true;
		data.native_snapshot_generation = 17U;
		data.range_candidate_result = IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, false, true, {}};
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"});
		scan.setRangePredicates({{"age", PropertyValue(0), PropertyValue(), true, true}});
		EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
			return true;
		})), std::runtime_error);
	}

	for (const std::vector<EntityRef> &malformedCandidates: {
				std::vector<EntityRef>{{1U, false, 0U, 0U, 0U}, {1U, false, 0U, 0U, 0U}},
				std::vector<EntityRef>{{2U, false, 0U, 0U, 0U}, {1U, false, 0U, 0U, 0U}}}) {
		LogicalGraphFixtureData data = graphData();
		data.enable_native_exact_node_candidates = true;
		data.native_snapshot_generation = 17U;
		data.exact_candidate_result = IndexCandidateBatch{malformedCandidates, 17U, true, false, {}};
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"}, {{"age", PropertyValue(42)}});
		EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
			return true;
		})), std::runtime_error);
	}
}

TEST(LogicalGraphReadExecutorTest, OptimizedFilterNodeScanUsesExactLookupAndRetainsRuntimePredicateValidation) {
	LogicalGraphFixtureData data = graphData();
	data.enable_exact_node_lookup = true;
	auto exactLookupCount = std::make_shared<size_t>(0U);
	data.exact_lookup_count = exactLookupCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{42}));
	auto root = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), predicate);
	const auto optimized = graph::query::optimization::LogicalPlanOptimizer{}.optimize(std::move(root));
	ASSERT_NE(optimized.root, nullptr);
	EXPECT_EQ(optimized.rewrite_count, 1U);

	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*optimized.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*exactLookupCount, 1U);
	// The index candidate is revalidated by the NodeScan and then by the
	// retained Filter, so both layers account for their Property reads.
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{1U, 0U, 2U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, OptimizedFilterNodeScanUsesRangeLookupAndRetainsRuntimePredicateValidation) {
	LogicalGraphFixtureData data = graphData();
	data.enable_range_node_lookup = true;
	auto rangeLookupCount = std::make_shared<size_t>(0U);
	data.range_lookup_count = rangeLookupCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{30}));
	auto root = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), predicate);
	const auto optimized = graph::query::optimization::LogicalPlanOptimizer{}.optimize(std::move(root));
	ASSERT_NE(optimized.root, nullptr);
	EXPECT_EQ(optimized.rewrite_count, 1U);

	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*optimized.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U, 2U}));
	EXPECT_EQ(*rangeLookupCount, 1U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{2U, 0U, 4U, 2U}));
}

TEST(LogicalGraphReadExecutorTest, OptimizedFilterReusesVerifiedNativeRangeCandidateFields) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.range_candidate_result = IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, false, true,
			{{"age", {PropertyValue(42)}}}};
	auto candidateLookupCount = std::make_shared<size_t>(0U);
	auto nodeReadCount = std::make_shared<size_t>(0U);
	auto nodeBatchReadCount = std::make_shared<size_t>(0U);
	data.range_candidate_lookup_count = candidateLookupCount;
	data.node_read_count = nodeReadCount;
	data.node_batch_read_count = nodeBatchReadCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{30}));
	auto root = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), predicate);
	const auto optimized = graph::query::optimization::LogicalPlanOptimizer{}.optimize(std::move(root));
	ASSERT_NE(optimized.root, nullptr);
	ASSERT_EQ(optimized.rewrite_count, 1U);

	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*optimized.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*candidateLookupCount, 1U);
	// The filter consumes the authenticated selected range field. The public
	// result boundary uses one complete shaped batch and no point read.
	EXPECT_EQ(*nodeBatchReadCount, 1U);
	EXPECT_EQ(*nodeReadCount, 0U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{1U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, OptimizedFilterNodeScanUsesCanonicalConjunctiveRangeLookup) {
	LogicalGraphFixtureData data = graphData();
	data.enable_range_node_lookup = true;
	data.expected_range_lookup = {"age", PropertyValue(30), PropertyValue(42), true, false};
	auto rangeLookupCount = std::make_shared<size_t>(0U);
	data.range_lookup_count = rangeLookupCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto lower = std::make_unique<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{30}));
	auto upper = std::make_unique<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_LESS,
			std::make_unique<expressions::LiteralExpression>(int64_t{42}));
	auto root = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}),
			std::make_shared<expressions::BinaryOpExpression>(
					std::move(lower), expressions::BinaryOperatorType::BOP_AND, std::move(upper)));
	const auto optimized = graph::query::optimization::LogicalPlanOptimizer{}.optimize(std::move(root));
	ASSERT_NE(optimized.root, nullptr);
	EXPECT_EQ(optimized.rewrite_count, 1U);

	std::vector<uint64_t> nodeIds;
	static_cast<void>(runtime.execute(*optimized.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
		nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
		return true;
	}));
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{2U}));
	EXPECT_EQ(*rangeLookupCount, 1U);
}

TEST(LogicalGraphReadExecutorTest, OptimizedFilterNodeScanUsesCompositeRangeLookupAndRetainsPredicateValidation) {
	LogicalGraphFixtureData data = graphData();
	data.enable_composite_range_node_lookup = true;
	data.expected_composite_range_lookup = {
			{{"status", PropertyValue("online")}}, {"age", PropertyValue(30), PropertyValue(), true, true}};
	auto lookupCount = std::make_shared<size_t>(0U);
	data.composite_range_lookup_count = lookupCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto equality = std::make_unique<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "status"),
			expressions::BinaryOperatorType::BOP_EQUAL,
			std::make_unique<expressions::LiteralExpression>(std::string{"online"}));
	auto range = std::make_unique<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{30}));
	auto root = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}),
			std::make_shared<expressions::BinaryOpExpression>(
					std::move(equality), expressions::BinaryOperatorType::BOP_AND, std::move(range)));
	const auto optimized = graph::query::optimization::LogicalPlanOptimizer{}.optimize(std::move(root));
	ASSERT_NE(optimized.root, nullptr);
	EXPECT_EQ(optimized.rewrite_count, 2U);

	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*optimized.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*lookupCount, 1U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{1U, 0U, 2U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, ParserPushesParameterizedInlinePropertyEqualityIntoExactLookup) {
	LogicalGraphFixtureData data = graphData();
	data.enable_exact_node_lookup = true;
	auto exactLookupCount = std::make_shared<size_t>(0U);
	data.exact_lookup_count = exactLookupCount;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	graph::query::QueryPlan plan = parser.parseToLogical("MATCH (n:Person {age: $age}) RETURN n");
	ASSERT_NE(plan.root, nullptr);

	graph::query::QueryContext context;
	context.parameters.emplace("age", PropertyValue(42));
	std::vector<uint64_t> nodeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*plan.root, runtimeBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			},
			context);
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U}));
	EXPECT_EQ(*exactLookupCount, 1U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{1U, 0U, 1U, 1U}));

	context.parameters["age"] = PropertyValue();
	EXPECT_EQ(runtime.execute(*plan.root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; }, context),
			  (LogicalPlanReadRuntimeStats{}));
	context.parameters.clear();
	EXPECT_THROW(static_cast<void>(runtime.execute(*plan.root, runtimeBounds(),
															 [](const LogicalPlanReadRow &) { return true; }, context)),
				 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsArithmeticOverflowBeforeProducingAResult) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto overflow = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
			expressions::BinaryOperatorType::BOP_ADD,
			std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::max()));
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::move(overflow), "overflow");
	logical::LogicalProject root(std::move(scan), std::move(items));
	EXPECT_THROW(
			static_cast<void>(runtime.execute(root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
			std::overflow_error);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeEvaluatesSupportedScalarExpressionsAndNullSemantics) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto binary = [](std::unique_ptr<expressions::Expression> left, expressions::BinaryOperatorType operation,
						   std::unique_ptr<expressions::Expression> right) {
		return sharedExpression(
				std::make_unique<expressions::BinaryOpExpression>(std::move(left), operation, std::move(right)));
	};
	const auto unary = [](expressions::UnaryOperatorType operation, std::unique_ptr<expressions::Expression> operand) {
		return sharedExpression(std::make_unique<expressions::UnaryOpExpression>(operation, std::move(operand)));
	};
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>(int64_t(3))),
					   "add");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(std::string("zy")),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>(std::string("x"))),
					   "concat");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(9)),
							  expressions::BinaryOperatorType::BOP_SUBTRACT,
							  std::make_unique<expressions::LiteralExpression>(int64_t(4))),
					   "subtract");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(4)),
							  expressions::BinaryOperatorType::BOP_MULTIPLY,
							  std::make_unique<expressions::LiteralExpression>(int64_t(2))),
					   "multiply");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(7)),
							  expressions::BinaryOperatorType::BOP_DIVIDE,
							  std::make_unique<expressions::LiteralExpression>(int64_t(2))),
					   "divide");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(7)),
							  expressions::BinaryOperatorType::BOP_MODULO,
							  std::make_unique<expressions::LiteralExpression>(int64_t(4))),
					   "modulo");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							  expressions::BinaryOperatorType::BOP_POWER,
							  std::make_unique<expressions::LiteralExpression>(int64_t(3))),
					   "power");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							  expressions::BinaryOperatorType::BOP_LESS,
							  std::make_unique<expressions::LiteralExpression>(int64_t(3))),
					   "less");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(3)),
							  expressions::BinaryOperatorType::BOP_GREATER,
							  std::make_unique<expressions::LiteralExpression>(int64_t(2))),
					   "greater");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							  expressions::BinaryOperatorType::BOP_LESS_EQUAL,
							  std::make_unique<expressions::LiteralExpression>(int64_t(2))),
					   "lessEqual");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(3)),
							  expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
							  std::make_unique<expressions::LiteralExpression>(int64_t(3))),
					   "greaterEqual");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(3)),
							  expressions::BinaryOperatorType::BOP_EQUAL,
							  std::make_unique<expressions::LiteralExpression>(int64_t(3))),
					   "equal");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(int64_t(3)),
							  expressions::BinaryOperatorType::BOP_NOT_EQUAL,
							  std::make_unique<expressions::LiteralExpression>(int64_t(4))),
					   "notEqual");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(false),
							  expressions::BinaryOperatorType::BOP_AND,
							  std::make_unique<expressions::LiteralExpression>()),
					   "andFalse");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(true),
							  expressions::BinaryOperatorType::BOP_AND,
							  std::make_unique<expressions::LiteralExpression>(true)),
					   "andTrue");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(true),
							  expressions::BinaryOperatorType::BOP_OR,
							  std::make_unique<expressions::LiteralExpression>()),
					   "orTrue");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(false),
							  expressions::BinaryOperatorType::BOP_OR,
							  std::make_unique<expressions::LiteralExpression>(false)),
					   "orFalse");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(true),
							  expressions::BinaryOperatorType::BOP_XOR,
							  std::make_unique<expressions::LiteralExpression>(false)),
					   "xor");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(std::string("abcdef")),
							  expressions::BinaryOperatorType::BOP_STARTS_WITH,
							  std::make_unique<expressions::LiteralExpression>(std::string("abc"))),
					   "startsWith");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(std::string("abcdef")),
							  expressions::BinaryOperatorType::BOP_ENDS_WITH,
							  std::make_unique<expressions::LiteralExpression>(std::string("def"))),
					   "endsWith");
	items.emplace_back(binary(std::make_unique<expressions::LiteralExpression>(std::string("abcdef")),
							  expressions::BinaryOperatorType::BOP_CONTAINS,
							  std::make_unique<expressions::LiteralExpression>(std::string("cd"))),
					   "contains");
	items.emplace_back(unary(expressions::UnaryOperatorType::UOP_MINUS,
							 std::make_unique<expressions::LiteralExpression>(int64_t(3))),
					   "negativeInteger");
	items.emplace_back(
			unary(expressions::UnaryOperatorType::UOP_MINUS, std::make_unique<expressions::LiteralExpression>(1.5)),
			"negativeDouble");
	items.emplace_back(
			unary(expressions::UnaryOperatorType::UOP_NOT, std::make_unique<expressions::LiteralExpression>(true)),
			"not");
	items.emplace_back(sharedExpression(std::make_unique<expressions::InExpression>(
							   std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							   std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)})),
					   "in");
	items.emplace_back(sharedExpression(std::make_unique<expressions::IsNullExpression>(
							   std::make_unique<expressions::LiteralExpression>(), false)),
					   "isNull");
	items.emplace_back(sharedExpression(std::make_unique<expressions::IsNullExpression>(
							   std::make_unique<expressions::LiteralExpression>(int64_t(1)), true)),
					   "isNotNull");
	logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
	std::vector<LogicalPlanReadRow> rows;

	const LogicalPlanReadRuntimeStats stats =
			runtime.execute(root, runtimeBounds(), [&rows](const LogicalPlanReadRow &row) {
				rows.push_back(row);
				return true;
			});

	ASSERT_EQ(rows.size(), 1U);
	const auto &values = rows.front().values;
	EXPECT_EQ(std::get<PropertyValue>(values.at("add")), PropertyValue(5));
	EXPECT_EQ(std::get<PropertyValue>(values.at("concat")), PropertyValue("zyx"));
	EXPECT_EQ(std::get<PropertyValue>(values.at("subtract")), PropertyValue(5));
	EXPECT_EQ(std::get<PropertyValue>(values.at("multiply")), PropertyValue(8));
	EXPECT_EQ(std::get<PropertyValue>(values.at("divide")), PropertyValue(3.5));
	EXPECT_EQ(std::get<PropertyValue>(values.at("modulo")), PropertyValue(3));
	EXPECT_EQ(std::get<PropertyValue>(values.at("power")), PropertyValue(8.0));
	for (const char *name: {"less", "greater", "lessEqual", "greaterEqual", "equal", "notEqual", "andTrue", "orTrue",
							"xor", "startsWith", "endsWith", "contains", "isNull", "isNotNull"}) {
		EXPECT_EQ(std::get<PropertyValue>(values.at(name)), PropertyValue(true)) << name;
	}
	EXPECT_EQ(std::get<PropertyValue>(values.at("andFalse")), PropertyValue(false));
	EXPECT_EQ(std::get<PropertyValue>(values.at("orFalse")), PropertyValue(false));
	EXPECT_EQ(std::get<PropertyValue>(values.at("negativeInteger")), PropertyValue(-3));
	EXPECT_EQ(std::get<PropertyValue>(values.at("negativeDouble")), PropertyValue(-1.5));
	EXPECT_EQ(std::get<PropertyValue>(values.at("not")), PropertyValue(false));
	EXPECT_EQ(std::get<PropertyValue>(values.at("in")), PropertyValue(true));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimePreservesThreeValuedFiltersAndStopsAtTheVisitorBoundary) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	auto nullFilter = std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalSingleRow>(),
			sharedExpression(std::make_unique<expressions::LiteralExpression>()));
	size_t nullRows = 0U;
	const LogicalPlanReadRuntimeStats nullStats =
			runtime.execute(*nullFilter, runtimeBounds(), [&nullRows](const LogicalPlanReadRow &) {
				++nullRows;
				return true;
			});
	EXPECT_EQ(nullRows, 0U);
	EXPECT_EQ(nullStats, (LogicalPlanReadRuntimeStats{}));

	logical::LogicalSingleRow single;
	const LogicalPlanReadRuntimeStats stopped =
			runtime.execute(single, runtimeBounds(), [](const LogicalPlanReadRow &) { return false; });
	EXPECT_EQ(stopped, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsInvalidScalarOperationsBeforeEmittingRows) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto executeExpression = [&](std::unique_ptr<expressions::Expression> expression) {
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(sharedExpression(std::move(expression)), "value");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
		return runtime.execute(root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; });
	};

	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(int64_t(1)),
						 expressions::BinaryOperatorType::BOP_DIVIDE,
						 std::make_unique<expressions::LiteralExpression>(int64_t(0))))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(int64_t(1)),
						 expressions::BinaryOperatorType::BOP_MODULO,
						 std::make_unique<expressions::LiteralExpression>(0.5)))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::min()),
						 expressions::BinaryOperatorType::BOP_MODULO,
						 std::make_unique<expressions::LiteralExpression>(int64_t(-1))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<double>::max()),
						 expressions::BinaryOperatorType::BOP_MULTIPLY,
						 std::make_unique<expressions::LiteralExpression>(2.0)))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::UnaryOpExpression>(
						 expressions::UnaryOperatorType::UOP_MINUS,
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::min())))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::UnaryOpExpression>(
						 expressions::UnaryOperatorType::UOP_NOT,
						 std::make_unique<expressions::LiteralExpression>(int64_t(1))))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::InExpression>(
						 std::make_unique<expressions::LiteralExpression>(int64_t(1)),
						 std::make_unique<expressions::LiteralExpression>(int64_t(1))))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::min()),
						 expressions::BinaryOperatorType::BOP_SUBTRACT,
						 std::make_unique<expressions::LiteralExpression>(int64_t(1))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::max()),
						 expressions::BinaryOperatorType::BOP_SUBTRACT,
						 std::make_unique<expressions::LiteralExpression>(int64_t(-1))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::min()),
						 expressions::BinaryOperatorType::BOP_MULTIPLY,
						 std::make_unique<expressions::LiteralExpression>(int64_t(-1))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::max()),
						 expressions::BinaryOperatorType::BOP_MULTIPLY,
						 std::make_unique<expressions::LiteralExpression>(int64_t(-2))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::min()),
						 expressions::BinaryOperatorType::BOP_MULTIPLY,
						 std::make_unique<expressions::LiteralExpression>(int64_t(2))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(std::numeric_limits<int64_t>::min()),
						 expressions::BinaryOperatorType::BOP_MULTIPLY,
						 std::make_unique<expressions::LiteralExpression>(int64_t(-2))))),
				 std::overflow_error);
	EXPECT_THROW(static_cast<void>(executeExpression(std::make_unique<expressions::BinaryOpExpression>(
						 std::make_unique<expressions::LiteralExpression>(int64_t(1)),
						 expressions::BinaryOperatorType::BOP_MODULO,
						 std::make_unique<expressions::LiteralExpression>(int64_t(0))))),
				 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeCoversScalarNullAndFailClosedBoundaries) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto evaluate = [&](std::unique_ptr<expressions::Expression> expression) {
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(sharedExpression(std::move(expression)), "value");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
		std::vector<LogicalPlanReadRow> rows;
		static_cast<void>(runtime.execute(root, runtimeBounds(), [&rows](const LogicalPlanReadRow &row) {
			rows.push_back(row);
			return true;
		}));
		if (rows.size() != 1U) {
			throw std::runtime_error("scalar test plan did not produce one row");
		}
		return std::get<PropertyValue>(rows.front().values.at("value"));
	};
	const auto binary = [](std::unique_ptr<expressions::Expression> left, expressions::BinaryOperatorType operation,
						   std::unique_ptr<expressions::Expression> right) {
		return std::make_unique<expressions::BinaryOpExpression>(std::move(left), operation, std::move(right));
	};

	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>(int64_t(1)))),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(1)),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>())),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(),
							  expressions::BinaryOperatorType::BOP_EQUAL,
							  std::make_unique<expressions::LiteralExpression>(int64_t(1)))),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(1)),
							  expressions::BinaryOperatorType::BOP_EQUAL,
							  std::make_unique<expressions::LiteralExpression>())),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(1.5),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>(2.0))),
			  PropertyValue(3.5));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(4.5),
							  expressions::BinaryOperatorType::BOP_SUBTRACT,
							  std::make_unique<expressions::LiteralExpression>(1.0))),
			  PropertyValue(3.5));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(-2)),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>(int64_t(-3)))),
			  PropertyValue(-5));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							  expressions::BinaryOperatorType::BOP_ADD,
							  std::make_unique<expressions::LiteralExpression>(int64_t(-1)))),
			  PropertyValue(1));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(-5)),
							  expressions::BinaryOperatorType::BOP_SUBTRACT,
							  std::make_unique<expressions::LiteralExpression>(int64_t(-2)))),
			  PropertyValue(-3));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(0)),
							  expressions::BinaryOperatorType::BOP_MULTIPLY,
							  std::make_unique<expressions::LiteralExpression>(int64_t(42)))),
			  PropertyValue(0));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(42)),
							  expressions::BinaryOperatorType::BOP_MULTIPLY,
							  std::make_unique<expressions::LiteralExpression>(int64_t(0)))),
			  PropertyValue(0));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(2)),
							  expressions::BinaryOperatorType::BOP_MULTIPLY,
							  std::make_unique<expressions::LiteralExpression>(int64_t(-3)))),
			  PropertyValue(-6));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(-2)),
							  expressions::BinaryOperatorType::BOP_MULTIPLY,
							  std::make_unique<expressions::LiteralExpression>(int64_t(3)))),
			  PropertyValue(-6));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(-2)),
							  expressions::BinaryOperatorType::BOP_MULTIPLY,
							  std::make_unique<expressions::LiteralExpression>(int64_t(-3)))),
			  PropertyValue(6));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(false),
							  expressions::BinaryOperatorType::BOP_AND,
							  std::make_unique<expressions::LiteralExpression>(true))),
			  PropertyValue(false));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(true),
							  expressions::BinaryOperatorType::BOP_AND,
							  std::make_unique<expressions::LiteralExpression>(false))),
			  PropertyValue(false));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(true),
							  expressions::BinaryOperatorType::BOP_AND,
							  std::make_unique<expressions::LiteralExpression>())),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(false),
							  expressions::BinaryOperatorType::BOP_OR,
							  std::make_unique<expressions::LiteralExpression>())),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(false),
							  expressions::BinaryOperatorType::BOP_OR,
							  std::make_unique<expressions::LiteralExpression>(true))),
			  PropertyValue(true));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(),
							  expressions::BinaryOperatorType::BOP_XOR,
							  std::make_unique<expressions::LiteralExpression>(true))),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(false),
							  expressions::BinaryOperatorType::BOP_XOR,
							  std::make_unique<expressions::LiteralExpression>(true))),
			  PropertyValue(true));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(std::string("abcdef")),
							  expressions::BinaryOperatorType::BOP_CONTAINS,
							  std::make_unique<expressions::LiteralExpression>(std::string("zz")))),
			  PropertyValue(false));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(),
							  expressions::BinaryOperatorType::BOP_STARTS_WITH,
							  std::make_unique<expressions::LiteralExpression>(std::string("z")))),
			  PropertyValue());
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(std::string("z")),
							  expressions::BinaryOperatorType::BOP_STARTS_WITH,
							  std::make_unique<expressions::LiteralExpression>())),
			  PropertyValue());
	EXPECT_EQ(evaluate(std::make_unique<expressions::InExpression>(
					  std::make_unique<expressions::LiteralExpression>(int64_t(3)),
					  std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)})),
			  PropertyValue(false));
	EXPECT_EQ(evaluate(std::make_unique<expressions::InExpression>(std::make_unique<expressions::LiteralExpression>(),
																   std::vector<PropertyValue>{PropertyValue(1)})),
			  PropertyValue());
	EXPECT_EQ(evaluate(std::make_unique<expressions::UnaryOpExpression>(
					  expressions::UnaryOperatorType::UOP_NOT, std::make_unique<expressions::LiteralExpression>())),
			  PropertyValue());

	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(std::string("x")),
								 expressions::BinaryOperatorType::BOP_ADD,
								 std::make_unique<expressions::LiteralExpression>(int64_t(1)))),
			  PropertyValue("x1"));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(1)),
								 expressions::BinaryOperatorType::BOP_STARTS_WITH,
								 std::make_unique<expressions::LiteralExpression>(int64_t(1)))),
			  PropertyValue(true));
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(std::string("x")),
								 expressions::BinaryOperatorType::BOP_STARTS_WITH,
								 std::make_unique<expressions::LiteralExpression>(int64_t(1)))),
			  PropertyValue(false));
	EXPECT_THROW(
			static_cast<void>(evaluate(binary(
					std::make_unique<expressions::LiteralExpression>(std::numeric_limits<double>::infinity()),
					expressions::BinaryOperatorType::BOP_ADD, std::make_unique<expressions::LiteralExpression>(1.0)))),
			std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate(binary(std::make_unique<expressions::LiteralExpression>(0.5),
												   expressions::BinaryOperatorType::BOP_MODULO,
												   std::make_unique<expressions::LiteralExpression>(int64_t(1))))),
				 std::runtime_error);
	EXPECT_EQ(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(1)),
								 expressions::BinaryOperatorType::BOP_REGEX_MATCH,
								 std::make_unique<expressions::LiteralExpression>(int64_t(1)))),
			  PropertyValue(true));
	EXPECT_THROW(static_cast<void>(evaluate(binary(std::make_unique<expressions::LiteralExpression>(int64_t(1)),
												   expressions::BinaryOperatorType::BOP_IN,
												   std::make_unique<expressions::LiteralExpression>(int64_t(1))))),
				 std::logic_error);
	EXPECT_THROW(static_cast<void>(evaluate(std::make_unique<expressions::UnaryOpExpression>(
						 expressions::UnaryOperatorType::UOP_MINUS,
						 std::make_unique<expressions::LiteralExpression>(std::string("x"))))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate(std::make_unique<expressions::UnaryOpExpression>(
						 static_cast<expressions::UnaryOperatorType>(99),
						 std::make_unique<expressions::LiteralExpression>(int64_t(1))))),
				 std::logic_error);
}

TEST(LogicalGraphReadExecutorTest, EvaluatesEntityIntrospectionFunctionsAgainstSnapshotBindings) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);

	LogicalPlanReadRow row;
	row.values.emplace("node", snapshot->readNode(1U));
	row.values.emplace("edge", snapshot->readEdge(10U));
	row.values.emplace("scalar", PropertyValue(7));
	row.values.emplace("map", PropertyValue(PropertyValue::MapType{{"enabled", PropertyValue(true)}}));
	graph::query::QueryContext context;
	const auto evaluate = [&](std::string function, std::string variable) {
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>(std::move(variable)));
		expressions::FunctionCallExpression expression(std::move(function), std::move(arguments));
		return LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(*snapshot, expression, row, runtimeBounds(), context);
	};

	EXPECT_EQ(evaluate("id", "node"), LogicalPlanReadValue(PropertyValue(1)));
	EXPECT_EQ(evaluate("id", "edge"), LogicalPlanReadValue(PropertyValue(10)));
	EXPECT_EQ(evaluate("type", "edge"), LogicalPlanReadValue(PropertyValue("KNOWS")));
	EXPECT_EQ(evaluate("labels", "node"),
			  LogicalPlanReadValue(PropertyValue(std::vector<PropertyValue>{PropertyValue("Person"), PropertyValue("VIP")})));
	const PropertyValue keys = std::get<PropertyValue>(evaluate("keys", "node"));
	EXPECT_EQ(keys.getType(), PropertyType::LIST);
	EXPECT_EQ(keys.getList().size(), 2U);
	const PropertyValue properties = std::get<PropertyValue>(evaluate("properties", "edge"));
	EXPECT_EQ(properties.getType(), PropertyType::MAP);
	EXPECT_EQ(properties.getMap().at("weight"), PropertyValue(2));
	EXPECT_EQ(evaluate("properties", "map"),
			  LogicalPlanReadValue(PropertyValue(PropertyValue::MapType{{"enabled", PropertyValue(true)}})));
	EXPECT_EQ(evaluate("keys", "map"),
			  LogicalPlanReadValue(PropertyValue(std::vector<PropertyValue>{PropertyValue("enabled")})));

	EXPECT_THROW(static_cast<void>(evaluate("labels", "edge")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate("type", "node")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate("id", "scalar")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate("keys", "scalar")), std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluate("properties", "scalar")), std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeResolvesNodeEdgeAndMapPropertiesWithoutDuplicateReads) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).properties.emplace_back("attributes",
												  PropertyValue(PropertyValue::MapType{{"answer", PropertyValue(42)}}));
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto nodeScan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"});
	std::vector<logical::LogicalProjectItem> nodeItems;
	nodeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "missing"), "missing");
	nodeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "age"), "age");
	nodeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "attributes"), "attributes");
	nodeItems.emplace_back(std::make_shared<expressions::BinaryOpExpression>(
								   std::make_unique<expressions::VariableReferenceExpression>("n"),
								   expressions::BinaryOperatorType::BOP_EQUAL,
								   std::make_unique<expressions::LiteralExpression>(int64_t(1))),
						   "nodeIdMatches");
	auto nodeProject = std::make_unique<logical::LogicalProject>(std::move(nodeScan), std::move(nodeItems));
	std::vector<logical::LogicalProjectItem> mapItems;
	mapItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("attributes", "missing"),
						  "mapMissing");
	mapItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("attributes", "answer"), "answer");
	std::vector<std::unique_ptr<expressions::Expression>> propertyArguments;
	propertyArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("attributes"));
	mapItems.emplace_back(std::make_shared<expressions::FunctionCallExpression>(
							 std::string("properties"), std::move(propertyArguments)),
						  "allAttributes");
	std::vector<std::unique_ptr<expressions::Expression>> keyArguments;
	keyArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("attributes"));
	mapItems.emplace_back(std::make_shared<expressions::FunctionCallExpression>(std::string("keys"),
							 std::move(keyArguments)),
						  "attributeKeys");
	mapItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("age", "anything"),
						  "scalarMissing");
	logical::LogicalProject mapRoot(std::move(nodeProject), std::move(mapItems));
	std::vector<LogicalPlanReadRow> mapRows;
	const LogicalPlanReadRuntimeStats mapStats =
			runtime.execute(mapRoot, runtimeBounds(), [&mapRows](const LogicalPlanReadRow &row) {
				mapRows.push_back(row);
				return true;
			});
	ASSERT_EQ(mapRows.size(), 1U);
	EXPECT_EQ(std::get<PropertyValue>(mapRows.front().values.at("answer")), PropertyValue(42));
	EXPECT_EQ(std::get<PropertyValue>(mapRows.front().values.at("mapMissing")), PropertyValue());
	EXPECT_EQ(std::get<PropertyValue>(mapRows.front().values.at("allAttributes")),
			  PropertyValue(PropertyValue::MapType{{"answer", PropertyValue(42)}}));
	EXPECT_EQ(std::get<PropertyValue>(mapRows.front().values.at("attributeKeys")),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("answer")}));
	EXPECT_EQ(std::get<PropertyValue>(mapRows.front().values.at("scalarMissing")), PropertyValue());
	EXPECT_EQ(mapStats, (LogicalPlanReadRuntimeStats{3U, 0U, 1U, 1U}));

	auto edgeScan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"});
	auto traversal = std::make_unique<logical::LogicalTraversal>(std::move(edgeScan), "n", "r", "m", "KNOWS", "out");
	std::vector<logical::LogicalProjectItem> edgeItems;
	edgeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "missing"), "missing");
	edgeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "weight"), "weight");
	edgeItems.emplace_back(std::make_shared<expressions::BinaryOpExpression>(
								   std::make_unique<expressions::VariableReferenceExpression>("r"),
								   expressions::BinaryOperatorType::BOP_EQUAL,
								   std::make_unique<expressions::LiteralExpression>(int64_t(10))),
						   "edgeIdMatches");
	logical::LogicalProject edgeRoot(std::move(traversal), std::move(edgeItems));
	std::vector<LogicalPlanReadRow> edgeRows;
	const LogicalPlanReadRuntimeStats edgeStats =
			runtime.execute(edgeRoot, runtimeBounds(), [&edgeRows](const LogicalPlanReadRow &row) {
				edgeRows.push_back(row);
				return true;
			});
	ASSERT_EQ(edgeRows.size(), 1U);
	EXPECT_EQ(std::get<PropertyValue>(edgeRows.front().values.at("weight")), PropertyValue(2));
	EXPECT_EQ(std::get<PropertyValue>(edgeRows.front().values.at("missing")), PropertyValue());
	EXPECT_EQ(std::get<PropertyValue>(edgeRows.front().values.at("edgeIdMatches")), PropertyValue(true));
	// The topology-only traversal resolves the two requested fields through the
	// selected-property API and does not materialize a complete Edge Property map.
	EXPECT_EQ(edgeStats, (LogicalPlanReadRuntimeStats{3U, 2U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeUsesStructuralDistinctKeysAndEnforcesItsBound) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).properties.emplace_back("ambiguous",
												  PropertyValue(std::vector<PropertyValue>{PropertyValue("a, b")}));
	data.nodes.at(2U).properties.emplace_back(
			"ambiguous", PropertyValue(std::vector<PropertyValue>{PropertyValue("a"), PropertyValue("b")}));
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "ambiguous"), "value");
	logical::LogicalProject structuralRoot(std::move(scan), std::move(items), true);
	std::vector<PropertyValue> values;
	const LogicalPlanReadRuntimeStats structuralStats =
			runtime.execute(structuralRoot, runtimeBounds(), [&values](const LogicalPlanReadRow &row) {
				values.push_back(std::get<PropertyValue>(row.values.at("value")));
				return true;
			});
	EXPECT_EQ(values, (std::vector<PropertyValue>{
							  PropertyValue(std::vector<PropertyValue>{PropertyValue("a, b")}),
							  PropertyValue(std::vector<PropertyValue>{PropertyValue("a"), PropertyValue("b")})}));
	EXPECT_EQ(structuralStats, (LogicalPlanReadRuntimeStats{3U, 0U, 2U, 2U}));

	auto boundedScan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<logical::LogicalProjectItem> boundedItems;
	boundedItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "age"), "age");
	logical::LogicalProject boundedRoot(std::move(boundedScan), std::move(boundedItems), true);
	LogicalPlanReadRuntimeBounds limitedDistinct = runtimeBounds();
	limitedDistinct.maximum_distinct_rows = 1U;
	EXPECT_THROW(static_cast<void>(runtime.execute(boundedRoot, limitedDistinct,
												   [](const LogicalPlanReadRow &) { return true; })),
				 std::runtime_error);

	auto duplicateScan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<logical::LogicalProjectItem> duplicateItems;
	duplicateItems.emplace_back(std::make_shared<expressions::LiteralExpression>("same"), "value");
	logical::LogicalProject duplicateRoot(std::move(duplicateScan), std::move(duplicateItems), true);
	size_t duplicateRows = 0U;
	const LogicalPlanReadRuntimeStats duplicateStats =
			runtime.execute(duplicateRoot, runtimeBounds(), [&duplicateRows](const LogicalPlanReadRow &) {
				++duplicateRows;
				return true;
			});
	EXPECT_EQ(duplicateRows, 1U);
	EXPECT_EQ(duplicateStats, (LogicalPlanReadRuntimeStats{3U, 0U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeAppliesAndValidatesAllNodeScanPredicateForms) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto countMatches = [&runtime](std::unique_ptr<logical::LogicalNodeScan> scan) {
		size_t count = 0U;
		static_cast<void>(runtime.execute(*scan, runtimeBounds(), [&count](const LogicalPlanReadRow &) {
			++count;
			return true;
		}));
		return count;
	};

	EXPECT_EQ(countMatches(std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Missing"})), 0U);
	EXPECT_EQ(countMatches(std::make_unique<logical::LogicalNodeScan>(
					  "n", std::vector<std::string>{"Person"},
					  std::vector<std::pair<std::string, PropertyValue>>{{"missing", PropertyValue(1)}})),
			  0U);
	EXPECT_EQ(countMatches(std::make_unique<logical::LogicalNodeScan>(
					  "n", std::vector<std::string>{"Person"},
					  std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(99)}})),
			  0U);

	auto belowRange = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	belowRange->setRangePredicates({logical::RangePredicate{"age", PropertyValue(50), PropertyValue(), true, true}});
	EXPECT_EQ(countMatches(std::move(belowRange)), 0U);
	auto aboveRange = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	aboveRange->setRangePredicates({logical::RangePredicate{"age", PropertyValue(), PropertyValue(40), true, true}});
	EXPECT_EQ(countMatches(std::move(aboveRange)), 1U);
	auto exclusiveRange = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	exclusiveRange->setRangePredicates(
			{logical::RangePredicate{"age", PropertyValue(42), PropertyValue(42), false, false}});
	EXPECT_EQ(countMatches(std::move(exclusiveRange)), 0U);
	auto absentRange = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	absentRange->setRangePredicates(
			{logical::RangePredicate{"missing", PropertyValue(1), PropertyValue(), true, true}});
	EXPECT_EQ(countMatches(std::move(absentRange)), 0U);

	auto invalidLabel = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{""});
	EXPECT_THROW(static_cast<void>(countMatches(std::move(invalidLabel))), std::invalid_argument);
	auto duplicateLabel = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person", "Person"});
	EXPECT_THROW(static_cast<void>(countMatches(std::move(duplicateLabel))), std::invalid_argument);
	auto emptyProperty = std::make_unique<logical::LogicalNodeScan>(
			"n", std::vector<std::string>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"", PropertyValue(1)}});
	EXPECT_THROW(static_cast<void>(countMatches(std::move(emptyProperty))), std::invalid_argument);
	auto invalidRange = std::make_unique<logical::LogicalNodeScan>("n");
	invalidRange->setRangePredicates({logical::RangePredicate{"", PropertyValue(1), PropertyValue(), true, true}});
	EXPECT_THROW(static_cast<void>(countMatches(std::move(invalidRange))), std::invalid_argument);
	auto invalidComposite = std::make_unique<logical::LogicalNodeScan>("n");
	invalidComposite->setCompositeEquality(logical::CompositeEqualityPredicate{{"age"}, {}});
	EXPECT_THROW(static_cast<void>(countMatches(std::move(invalidComposite))), std::invalid_argument);
	auto emptyComposite = std::make_unique<logical::LogicalNodeScan>("n");
	emptyComposite->setCompositeEquality(logical::CompositeEqualityPredicate{{""}, {PropertyValue(42)}});
	EXPECT_THROW(static_cast<void>(countMatches(std::move(emptyComposite))), std::invalid_argument);
	auto unmatchedComposite = std::make_unique<logical::LogicalNodeScan>("n");
	unmatchedComposite->setCompositeEquality(logical::CompositeEqualityPredicate{{"age"}, {PropertyValue(99)}});
	EXPECT_EQ(countMatches(std::move(unmatchedComposite)), 0U);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeBuildsDistinctKeysForEverySupportedPropertyAndEntityValue) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).properties.emplace_back("null", PropertyValue());
	data.nodes.at(1U).properties.emplace_back("boolean", PropertyValue(false));
	data.nodes.at(1U).properties.emplace_back("double", PropertyValue(-0.0));
	data.nodes.at(1U).properties.emplace_back(
			"map", PropertyValue(PropertyValue::MapType{{"z", PropertyValue(1)}, {"a", PropertyValue("value")}}));
	data.nodes.at(1U).properties.emplace_back("date", PropertyValue(TemporalDate{1}));
	data.nodes.at(1U).properties.emplace_back("datetime", PropertyValue(TemporalDateTime{1}));
	data.nodes.at(1U).properties.emplace_back("duration", PropertyValue(TemporalDuration{1, 2, 3}));
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds propertyBounds = runtimeBounds();
	propertyBounds.maximum_property_entries = 16U;
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"});
	std::vector<logical::LogicalProjectItem> values;
	for (const std::string &key: {"null", "boolean", "double", "map", "date", "datetime", "duration"}) {
		values.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", key), key);
	}
	logical::LogicalProject propertyRoot(std::move(scan), std::move(values), true);
	const LogicalPlanReadRuntimeStats propertyStats =
			runtime.execute(propertyRoot, propertyBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(propertyStats, (LogicalPlanReadRuntimeStats{3U, 0U, 1U, 1U}));

	auto nodeScan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"});
	std::vector<logical::LogicalProjectItem> nodeItems;
	nodeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "node");
	logical::LogicalProject nodeRoot(std::move(nodeScan), std::move(nodeItems), true);
	const LogicalPlanReadRuntimeStats nodeStats =
			runtime.execute(nodeRoot, propertyBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(nodeStats, (LogicalPlanReadRuntimeStats{3U, 0U, 0U, 1U}));

	auto edgeScan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"});
	auto traversal = std::make_unique<logical::LogicalTraversal>(std::move(edgeScan), "n", "r", "m", "KNOWS", "out");
	std::vector<logical::LogicalProjectItem> edgeItems;
	edgeItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r"), "edge");
	logical::LogicalProject edgeRoot(std::move(traversal), std::move(edgeItems), true);
	const LogicalPlanReadRuntimeStats edgeStats =
			runtime.execute(edgeRoot, propertyBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(edgeStats, (LogicalPlanReadRuntimeStats{3U, 2U, 0U, 1U}));
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsMalformedOperatorsExpressionsAndBounds) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto visitor = [](const LogicalPlanReadRow &) { return true; };

	logical::LogicalFilter missingPredicate(std::make_unique<logical::LogicalSingleRow>(), nullptr);
	EXPECT_THROW(static_cast<void>(runtime.execute(missingPredicate, runtimeBounds(), visitor)), std::invalid_argument);
	logical::LogicalProject emptyProject(std::make_unique<logical::LogicalSingleRow>(), {});
	EXPECT_THROW(static_cast<void>(runtime.execute(emptyProject, runtimeBounds(), visitor)), std::invalid_argument);
	std::vector<logical::LogicalProjectItem> missingExpressionItems;
	missingExpressionItems.emplace_back(std::shared_ptr<expressions::Expression>{}, "value");
	logical::LogicalProject missingExpressionProject(std::make_unique<logical::LogicalSingleRow>(),
													 std::move(missingExpressionItems));
	EXPECT_THROW(static_cast<void>(runtime.execute(missingExpressionProject, runtimeBounds(), visitor)),
				 std::invalid_argument);
	std::vector<logical::LogicalProjectItem> emptyAliasItems;
	emptyAliasItems.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t(1)), "");
	logical::LogicalProject emptyAliasProject(std::make_unique<logical::LogicalSingleRow>(),
											  std::move(emptyAliasItems));
	EXPECT_THROW(static_cast<void>(runtime.execute(emptyAliasProject, runtimeBounds(), visitor)),
				 std::invalid_argument);
	std::vector<logical::LogicalProjectItem> duplicateAliasItems;
	duplicateAliasItems.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t(1)), "value");
	duplicateAliasItems.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t(2)), "value");
	logical::LogicalProject duplicateAliasProject(std::make_unique<logical::LogicalSingleRow>(),
												  std::move(duplicateAliasItems));
	EXPECT_THROW(static_cast<void>(runtime.execute(duplicateAliasProject, runtimeBounds(), visitor)),
				 std::invalid_argument);
	logical::LogicalLimit missingChild(std::unique_ptr<logical::LogicalOperator>{}, 1);
	EXPECT_THROW(static_cast<void>(runtime.execute(missingChild, runtimeBounds(), visitor)), std::runtime_error);

	const auto evaluateFailure = [&](std::shared_ptr<expressions::Expression> expression) {
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(expression), "value");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
		return runtime.execute(root, runtimeBounds(), visitor);
	};
	EXPECT_THROW(static_cast<void>(evaluateFailure(
						 sharedExpression(std::make_unique<expressions::VariableReferenceExpression>("missing")))),
				 std::runtime_error);
	EXPECT_THROW(static_cast<void>(evaluateFailure(
						 sharedExpression(std::make_unique<expressions::VariableReferenceExpression>("value", "")))),
				 std::invalid_argument);
	EXPECT_THROW(
			static_cast<void>(evaluateFailure(sharedExpression(std::make_unique<expressions::FunctionCallExpression>(
					"unsupported", std::vector<std::unique_ptr<expressions::Expression>>{})))),
			std::runtime_error);

	const auto mutateBounds = [](LogicalPlanReadRuntimeBounds &bounds, size_t variant) {
		if (variant == 0U) {
			bounds.maximum_node_records = 0U;
		} else if (variant == 1U) {
			bounds.maximum_node_page_records = 0U;
		} else if (variant == 2U) {
			bounds.maximum_metadata_pages_per_node_page = 0U;
		} else if (variant == 3U) {
			bounds.maximum_edges_per_node = 0U;
		} else if (variant == 4U) {
			bounds.maximum_edge_records = 0U;
		} else if (variant == 5U) {
			bounds.maximum_adjacency_page_records = 0U;
		} else if (variant == 6U) {
			bounds.maximum_property_entries = 0U;
		} else if (variant == 7U) {
			bounds.maximum_rows = 0U;
		} else if (variant == 8U) {
			bounds.maximum_distinct_rows = 0U;
		} else if (variant == 9U) {
			bounds.maximum_node_page_records = bounds.maximum_node_records + 1U;
		} else {
			bounds.maximum_adjacency_page_records = bounds.maximum_edges_per_node + 1U;
		}
	};
	for (size_t variant: {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U}) {
		LogicalPlanReadRuntimeBounds invalid = runtimeBounds();
		mutateBounds(invalid, variant);
		logical::LogicalSingleRow root;
		EXPECT_THROW(static_cast<void>(runtime.execute(root, invalid, visitor)), std::invalid_argument);
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsMalformedNodePagesAndLogicalShapes) {
	{
		LogicalGraphFixtureData data = graphData();
		data.return_oversized_node_page = true;
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n");
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.return_nonmonotonic_node_page = true;
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		LogicalPlanReadRuntimeBounds pageBounds = runtimeBounds();
		pageBounds.maximum_node_page_records = 2U;
		logical::LogicalNodeScan scan("n");
		EXPECT_THROW(
				static_cast<void>(runtime.execute(scan, pageBounds, [](const LogicalPlanReadRow &) { return true; })),
				std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.return_empty_nonterminal_page = true;
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n");
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}
	{
		TestLogicalGraphBackend backend(graphData(), true);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n");
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}
	{
		TestLogicalGraphBackend backend(graphData());
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan invalidScan("");
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidScan, runtimeBounds(),
													   [](const LogicalPlanReadRow &) { return true; })),
					 std::invalid_argument);
		logical::LogicalSkip invalidSkip(std::make_unique<logical::LogicalSingleRow>(), -1);
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidSkip, runtimeBounds(),
													   [](const LogicalPlanReadRow &) { return true; })),
					 std::invalid_argument);
		logical::LogicalLimit zeroLimit(std::make_unique<logical::LogicalNodeScan>("n"), 0);
		const LogicalPlanReadRuntimeStats zeroStats =
				runtime.execute(zeroLimit, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; });
		EXPECT_EQ(zeroStats, (LogicalPlanReadRuntimeStats{}));
		UnsupportedLogicalReadOperator unsupported;
		EXPECT_THROW(static_cast<void>(runtime.execute(unsupported, runtimeBounds(),
													   [](const LogicalPlanReadRow &) { return true; })),
					 std::logic_error);
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsMalformedAdjacencyPagesAndBindings) {
	const auto traversal = []() {
		return std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m", "",
				"out");
	};
	const auto setCorruption = [](LogicalGraphFixtureData &data, size_t variant) {
		if (variant == 0U) {
			data.return_empty_nonterminal_adjacency_page = true;
		} else if (variant == 1U) {
			data.return_oversized_adjacency_page = true;
		} else if (variant == 2U) {
			data.return_invalid_adjacency_page_endpoint = true;
		} else {
			data.return_invalid_adjacency_continuation = true;
			data.invalid_adjacency_continuation_mode = static_cast<uint8_t>(variant - 3U);
		}
	};
	for (size_t variant: {0U, 1U, 2U, 3U, 4U, 5U}) {
		LogicalGraphFixtureData data = graphData();
		setCorruption(data, variant);
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		std::unique_ptr<logical::LogicalTraversal> root = traversal();
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(*root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}

	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(2U).labels = {"Person", "Person"};
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		std::unique_ptr<logical::LogicalTraversal> root = traversal();
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(*root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}
	{
		TestLogicalGraphBackend backend(graphData());
		LogicalPlanReadRuntime runtime(backend);
		auto invalidTargetLabel = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m",
				"KNOWS", "out", std::vector<std::string>{""});
		EXPECT_THROW(static_cast<void>(runtime.execute(*invalidTargetLabel, runtimeBounds(),
													   [](const LogicalPlanReadRow &) { return true; })),
					 std::invalid_argument);
		auto root = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m", "",
				"sideways");
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(*root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::invalid_argument);
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsCorruptEntitiesPropertiesAndTraversalBindings) {
	const auto expectPropertyScanFailure = [](LogicalGraphFixtureData data,
											  LogicalPlanReadRuntimeBounds bounds = runtimeBounds()) {
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n", {}, {{"age", PropertyValue(42)}});
		return runtime.execute(scan, bounds, [](const LogicalPlanReadRow &) { return true; });
	};
	const auto expectTraversalFailure = [](LogicalGraphFixtureData data) {
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		auto root = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m",
				"KNOWS", "out", std::vector<std::string>{}, std::vector<std::pair<std::string, PropertyValue>>{},
				std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue(2)}});
		return runtime.execute(*root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; });
	};

	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).node_id = 0U;
		EXPECT_THROW(static_cast<void>(expectPropertyScanFailure(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).properties = {{"age", PropertyValue(42)}, {"age", PropertyValue(43)}};
		EXPECT_THROW(static_cast<void>(expectPropertyScanFailure(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).labels = {""};
		EXPECT_THROW(static_cast<void>(expectPropertyScanFailure(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).properties.front().key.clear();
		EXPECT_THROW(static_cast<void>(expectPropertyScanFailure(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).properties = {
				{"age", PropertyValue(42)}, {"status", PropertyValue("online")}, {"tier", PropertyValue("gold")}};
		LogicalPlanReadRuntimeBounds limitedProperties = runtimeBounds();
		limitedProperties.maximum_property_entries = 1U;
		EXPECT_THROW(static_cast<void>(expectPropertyScanFailure(std::move(data), limitedProperties)),
					 std::runtime_error);
	}

	const auto corruptEdge = [](LogicalGraphEdge &edge, size_t variant) {
		if (variant == 0U) {
			edge.edge_id = 0U;
		} else if (variant == 1U) {
			edge.target_node_id = 0U;
		} else {
			edge.properties = {{"", PropertyValue(1)}};
		}
	};
	for (size_t variant: {0U, 1U, 2U}) {
		LogicalGraphFixtureData data = graphData();
		corruptEdge(data.edges.at(10U), variant);
		EXPECT_THROW(static_cast<void>(expectTraversalFailure(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.edges.at(10U).type.clear();
		EXPECT_NO_THROW(static_cast<void>(expectTraversalFailure(std::move(data))));
	}
	{
		TestLogicalGraphBackend backend(graphData());
		LogicalPlanReadRuntime runtime(backend);
		auto scalarProject = std::make_unique<logical::LogicalProject>(
				std::make_unique<logical::LogicalSingleRow>(),
				std::vector<logical::LogicalProjectItem>{logical::LogicalProjectItem(
						std::make_shared<expressions::LiteralExpression>(int64_t(1)), "n")});
		logical::LogicalTraversal root(std::move(scalarProject), "n", "r", "m", "", "out");
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeCoversTraversalFilteringBindingsAndTerminalPages) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds completeBounds = runtimeBounds();
	completeBounds.maximum_node_records = 4U;
	std::vector<uint64_t> nodeIds;
	logical::LogicalNodeScan allNodes("n");
	const LogicalPlanReadRuntimeStats nodeStats =
			runtime.execute(allNodes, completeBounds, [&nodeIds](const LogicalPlanReadRow &row) {
				nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
				return true;
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U, 2U, 3U}));
	EXPECT_EQ(nodeStats, (LogicalPlanReadRuntimeStats{3U, 0U, 0U, 3U}));

	auto deviceTraversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Device"}), "n", "r", "m", "",
			"out");
	const LogicalPlanReadRuntimeStats emptyAdjacency =
			runtime.execute(*deviceTraversal, completeBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(emptyAdjacency, (LogicalPlanReadRuntimeStats{3U, 0U, 0U, 0U}));

	auto typeMismatch = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m", "MISSING",
			"out");
	const LogicalPlanReadRuntimeStats mismatchedType =
			runtime.execute(*typeMismatch, completeBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(mismatchedType, (LogicalPlanReadRuntimeStats{3U, 2U, 0U, 0U}));

	auto targetMismatch = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m", "KNOWS",
			"out", std::vector<std::string>{"VIP"});
	const LogicalPlanReadRuntimeStats mismatchedTarget =
			runtime.execute(*targetMismatch, completeBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(mismatchedTarget, (LogicalPlanReadRuntimeStats{3U, 2U, 0U, 0U}));

	auto conflictingBinding = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "n", "m", "KNOWS",
			"out");
	const LogicalPlanReadRuntimeStats conflictStats =
			runtime.execute(*conflictingBinding, completeBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(conflictStats, (LogicalPlanReadRuntimeStats{3U, 2U, 0U, 0U}));

	auto targetBindingConflict = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "", "n", "KNOWS",
			"out");
	const LogicalPlanReadRuntimeStats targetConflictStats =
			runtime.execute(*targetBindingConflict, completeBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(targetConflictStats, (LogicalPlanReadRuntimeStats{3U, 2U, 0U, 0U}));

	auto stoppedTraversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "m", "KNOWS",
			"out");
	const LogicalPlanReadRuntimeStats stoppedStats =
			runtime.execute(*stoppedTraversal, completeBounds, [](const LogicalPlanReadRow &) { return false; });
	EXPECT_EQ(stoppedStats, (LogicalPlanReadRuntimeStats{1U, 1U, 0U, 1U}));

	LogicalGraphFixtureData loopData = graphData();
	loopData.edges.emplace(13U, LogicalGraphEdge{13U, 1U, 1U, "LOOP"});
	TestLogicalGraphBackend loopBackend(std::move(loopData));
	LogicalPlanReadRuntime loopRuntime(loopBackend);
	LogicalPlanReadRuntimeBounds loopBounds = completeBounds;
	loopBounds.maximum_edges_per_node = 5U;
	auto bothTraversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "", "m", "", "both");
	std::vector<uint64_t> bothTargets;
	const LogicalPlanReadRuntimeStats bothStats =
			loopRuntime.execute(*bothTraversal, loopBounds, [&bothTargets](const LogicalPlanReadRow &row) {
				bothTargets.push_back(std::get<LogicalGraphNode>(row.values.at("m")).node_id);
				return true;
			});
	EXPECT_EQ(bothTargets, (std::vector<uint64_t>{2U, 3U, 1U, 2U}));
	EXPECT_EQ(bothStats, (LogicalPlanReadRuntimeStats{3U, 5U, 0U, 4U}));

	auto selfBinding = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "", "n", "LOOP",
			"out");
	const LogicalPlanReadRuntimeStats selfStats =
			loopRuntime.execute(*selfBinding, loopBounds, [](const LogicalPlanReadRow &) { return true; });
	EXPECT_EQ(selfStats, (LogicalPlanReadRuntimeStats{3U, 3U, 0U, 1U}));
}
