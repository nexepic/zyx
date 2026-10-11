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
	const auto nodeBatchReads = std::make_shared<size_t>(0U);
	data.node_read_count = nodeReads;
	data.node_batch_read_count = nodeBatchReads;
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
	// before a complete source object is materialized. Target objects are read
	// once per bounded source-adjacency batch instead of once per Edge.
	EXPECT_EQ(rows.size(), 3U);
	EXPECT_EQ(stats.nodes_scanned, 3U);
	EXPECT_EQ(stats.nodes_matched, 2U);
	EXPECT_EQ(*nodeReads, 2U);
	EXPECT_EQ(*nodeBatchReads, 2U);
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

TEST(LogicalGraphReadExecutorTest, BatchesSelectedSourcePropertiesBeforeCompleteMaterialization) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_ref_scans = true;
	data.property_tokens.emplace("age", 1U);
	const auto nodeBatchReads = std::make_shared<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>>();
	data.node_batch_read_requests = nodeBatchReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.property_predicates = {
			predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(42))};
	request.bounds = bounds();

	std::vector<LogicalGraphReadRow> rows;
	static_cast<void>(executor.execute(request, [&rows](const LogicalGraphReadRow &row) {
		rows.push_back(row);
		return true;
	}));

	ASSERT_EQ(rows.size(), 1U);
	EXPECT_EQ(rows.front().source_node.node_id, 1U);
	ASSERT_EQ(nodeBatchReads->size(), 2U);
	EXPECT_EQ((*nodeBatchReads)[0].first, (std::vector<uint64_t>{1U, 2U, 3U}));
	EXPECT_EQ((*nodeBatchReads)[0].second.property_tokens, std::vector<uint32_t>{1U});
	EXPECT_FALSE((*nodeBatchReads)[0].second.all_properties);
	EXPECT_EQ((*nodeBatchReads)[1].first, (std::vector<uint64_t>{1U}));
	EXPECT_TRUE((*nodeBatchReads)[1].second.all_properties);
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

TEST(LogicalGraphReadExecutorTest, UsesSelectedTargetPropertiesBeforeCompleteMaterialization) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	data.property_tokens.emplace("age", 1U);
	const auto shapedReads = std::make_shared<std::vector<std::pair<uint64_t, EntityReadShape>>>();
	const auto batchReads = std::make_shared<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>>();
	const auto edgeBatchReads = std::make_shared<size_t>(0U);
	const auto edgeBatchRequests = std::make_shared<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>>();
	data.node_shaped_read_requests = shapedReads;
	data.node_batch_read_requests = batchReads;
	data.edge_batch_read_count = edgeBatchReads;
	data.edge_batch_read_requests = edgeBatchRequests;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.target_filter.property_predicates = {
			predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(30))};
	request.expansion = std::move(expansion);
	request.bounds = bounds();

	std::vector<LogicalGraphReadRow> rows;
	static_cast<void>(executor.execute(request, [&rows](const LogicalGraphReadRow &row) {
		rows.push_back(row);
		return true;
	}));

	ASSERT_EQ(rows.size(), 1U);
	ASSERT_TRUE(rows.front().target_node.has_value());
	EXPECT_EQ(rows.front().target_node->node_id, 2U);
	EXPECT_TRUE(shapedReads->empty());
	ASSERT_EQ(batchReads->size(), 3U);
	EXPECT_EQ((*batchReads)[0].first, std::vector<uint64_t>({2U, 3U}));
	EXPECT_EQ((*batchReads)[0].second.property_tokens, std::vector<uint32_t>{1U});
	EXPECT_FALSE((*batchReads)[0].second.all_properties);
	EXPECT_EQ((*batchReads)[1].first, std::vector<uint64_t>{2U});
	EXPECT_TRUE((*batchReads)[1].second.all_properties);
	EXPECT_EQ((*batchReads)[2].first, std::vector<uint64_t>{1U});
	EXPECT_EQ((*batchReads)[2].second.property_tokens, std::vector<uint32_t>{1U});
	EXPECT_FALSE((*batchReads)[2].second.all_properties);
	EXPECT_EQ(*edgeBatchReads, 1U);
	ASSERT_EQ(edgeBatchRequests->size(), 1U);
	EXPECT_EQ(edgeBatchRequests->front().first, std::vector<uint64_t>{10U});
}

TEST(LogicalGraphReadExecutorTest, BatchesSelectedTargetPropertiesBeforeCompleteMaterialization) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	data.property_tokens.emplace("age", 1U);
	const auto nodeBatchReads = std::make_shared<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>>();
	data.node_batch_read_requests = nodeBatchReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.property_predicates = {
			predicate("weight", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(2))};
	expansion.target_filter.property_predicates = {
			predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(30))};
	request.expansion = std::move(expansion);
	request.bounds = bounds();

	std::vector<LogicalGraphReadRow> rows;
	static_cast<void>(executor.execute(request, [&rows](const LogicalGraphReadRow &row) {
		rows.push_back(row);
		return true;
	}));

	ASSERT_EQ(rows.size(), 1U);
	ASSERT_EQ(nodeBatchReads->size(), 2U);
	EXPECT_EQ((*nodeBatchReads)[0].first, std::vector<uint64_t>{2U});
	EXPECT_EQ((*nodeBatchReads)[0].second.property_tokens, std::vector<uint32_t>{1U});
	EXPECT_FALSE((*nodeBatchReads)[0].second.all_properties);
	EXPECT_EQ((*nodeBatchReads)[1].first, std::vector<uint64_t>{2U});
	EXPECT_TRUE((*nodeBatchReads)[1].second.all_properties);
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

TEST(LogicalGraphReadExecutorTest, BatchesTopologyCallbacksWithoutTargetLabelFilter) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_adjacent_topology = true;
	for (uint64_t edgeId = 100U; edgeId < 357U; ++edgeId) {
		data.edges.emplace(edgeId, LogicalGraphEdge{edgeId, 1U, 2U, "KNOWS"});
	}
	const auto nodeBatchReads = std::make_shared<size_t>(0U);
	const auto edgeBatchReads = std::make_shared<size_t>(0U);
	const auto shapedNodeReads = std::make_shared<std::vector<std::pair<uint64_t, EntityReadShape>>>();
	const auto nodeBatchRequests = std::make_shared<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>>();
	const auto edgeBatchRequests = std::make_shared<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>>();
	data.node_batch_read_count = nodeBatchReads;
	data.edge_batch_read_count = edgeBatchReads;
	data.node_shaped_read_requests = shapedNodeReads;
	data.node_batch_read_requests = nodeBatchRequests;
	data.edge_batch_read_requests = edgeBatchRequests;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "KNOWS";
	request.expansion = std::move(expansion);
	request.bounds = bounds();
	request.bounds.maximum_edges_per_node = 300U;
	request.bounds.maximum_rows = 300U;
	std::vector<LogicalGraphReadRow> rows;

	const LogicalGraphReadExecutionStats stats = executor.execute(request, [&rows](const LogicalGraphReadRow &row) {
		rows.push_back(row);
	});

	ASSERT_EQ(rows.size(), 258U);
	ASSERT_TRUE(rows.front().edge.has_value());
	ASSERT_TRUE(rows.front().target_node.has_value());
	EXPECT_EQ(rows.front().edge->edge_id, 10U);
	EXPECT_EQ(rows.front().target_node->node_id, 2U);
	EXPECT_EQ(rows.back().edge->edge_id, 356U);
	EXPECT_EQ(stats.edges_scanned, 259U);
	EXPECT_EQ(stats.edges_matched, 258U);
	EXPECT_EQ(*edgeBatchReads, 2U);
	EXPECT_EQ(*nodeBatchReads, 2U);
	EXPECT_TRUE(shapedNodeReads->empty());
	ASSERT_EQ(edgeBatchRequests->size(), 2U);
	ASSERT_EQ(nodeBatchRequests->size(), 2U);
	for (const auto &batch: *edgeBatchRequests) EXPECT_LE(batch.first.size(), 256U);
	for (const auto &batch: *nodeBatchRequests) EXPECT_LE(batch.first.size(), 256U);
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

TEST(LogicalGraphReadExecutorTest, BatchesTopologyTargetPredicatesBeforeCompleteReads) {
	LogicalGraphFixtureData data = graphData();
	// Keep the native Node reference/column readers enabled while forcing the
	// traversal through topology pages, which is the path under test.
	data.enable_native_ref_scans = true;
	data.enable_native_adjacent_topology_pages = true;
	const auto nodeBatchReads = std::make_shared<size_t>(0U);
	const auto nodeLabelBatchReads = std::make_shared<size_t>(0U);
	const auto nodePropertyColumnReads = std::make_shared<size_t>(0U);
	data.node_batch_read_count = nodeBatchReads;
	data.node_label_batch_read_count = nodeLabelBatchReads;
	data.node_property_column_read_count = nodePropertyColumnReads;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalTraversal traversal(
			std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"VIP"}), "n", "r", "", "",
			"out", {"Person"}, {{"age", PropertyValue(30)}}, {});
	LogicalPlanReadRuntimeBounds traversalBounds = runtimeBounds();
	traversalBounds.maximum_adjacency_page_records = 2U;

	std::vector<uint64_t> edgeIds;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			traversal, traversalBounds, [&edgeIds](const LogicalPlanReadRow &row) {
				edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
				return true;
			});

	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U}));
	EXPECT_EQ(*nodeLabelBatchReads, 1U);
	EXPECT_EQ(*nodePropertyColumnReads, 1U);
	// The target variable is omitted, so a matching target predicate is fully
	// decided from selected columns; no additional target Node materialization is
	// needed. The one complete Node batch is the source binding at the public row
	// boundary.
	EXPECT_EQ(*nodeBatchReads, 1U);
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{3U, 2U, 0U, 1U}));
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
	// Admit the resident output row and aggregate group state while leaving no
	// room for the 4 KiB selected Property payload. COUNT only needs
	// null/non-null cardinality and must stay streaming.
	bounds.maximum_materialized_bytes = 2048U;
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

TEST(LogicalGraphReadExecutorTest, PublicResultRowGrowthSharesTheReadExecutionByteBudget) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	std::vector<PropertyValue> values(16U, PropertyValue(int64_t{7}));
	logical::LogicalProject project(std::make_unique<logical::LogicalUnwind>(
				std::make_unique<logical::LogicalSingleRow>(), "value", std::move(values)),
			{{std::make_shared<expressions::VariableReferenceExpression>("value"), "value"}});
	LogicalPlanReadRuntimeBounds bounded = runtimeBounds();
	bounded.maximum_rows = 16U;
	bounded.maximum_materialized_bytes = 2500U;
	try {
		static_cast<void>(runtime.executeToQueryResult(project, bounded));
		FAIL() << "public QueryResult row-vector growth should consume the shared byte budget";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "shared materialization exceeded its byte bound");
	}

	bounded.maximum_materialized_bytes = 16U * 1024U;
	const graph::query::QueryResult result = runtime.executeToQueryResult(project, bounded);
	ASSERT_EQ(result.rowCount(), 16U);
	for (const auto &row: result.getRows()) {
		EXPECT_EQ(row.at("value").asPrimitive(), PropertyValue(int64_t{7}));
	}
}

TEST(LogicalGraphReadExecutorTest, OutputColumnStorageConsumesBudgetEvenWhenLimitReturnsNoRows) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const std::string wideAlias(2048U, 'c');
	logical::LogicalLimit root(std::make_unique<logical::LogicalProject>(
			std::make_unique<logical::LogicalSingleRow>(),
			std::vector<logical::LogicalProjectItem>{{
					std::make_shared<expressions::LiteralExpression>(int64_t{1}), wideAlias}}), 0);
	LogicalPlanReadRuntimeBounds bounded = runtimeBounds();
	bounded.maximum_materialized_bytes = 1024U;
	try {
		static_cast<void>(runtime.executeToQueryResult(root, bounded));
		FAIL() << "output column storage must be bounded even when the plan emits no rows";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical-plan result columns exceeded their materialized byte bound");
	}

	bounded.maximum_materialized_bytes = 16U * 1024U;
	const graph::query::QueryResult result = runtime.executeToQueryResult(root, bounded);
	EXPECT_TRUE(result.getRows().empty());
	EXPECT_EQ(result.getColumns(), (std::vector<std::string>{wideAlias}));
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

TEST(LogicalGraphReadExecutorTest, BatchesDeferredEntityRefsAcrossResultRowsWithoutDroppingDuplicates) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_ref_scans = true;
	const auto batchRequests = std::make_shared<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>>();
	data.node_batch_read_requests = batchRequests;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);

	// UNION ALL deliberately repeats every EntityRef. The public result boundary
	// should read each distinct Node once, then scatter the materialized records
	// back into the original row order.
	auto left = std::make_unique<logical::LogicalNodeScan>("n");
	auto right = std::make_unique<logical::LogicalNodeScan>("n");
	auto unionAll = std::make_unique<logical::LogicalUnion>(std::move(left), std::move(right), true);
	const graph::query::QueryResult result = runtime.executeToQueryResult(*unionAll, runtimeBounds());

	ASSERT_EQ(result.rowCount(), 6U);
	ASSERT_EQ(batchRequests->size(), 1U);
	EXPECT_EQ((*batchRequests)[0].first, (std::vector<uint64_t>{1U, 2U, 3U}));
	EXPECT_TRUE((*batchRequests)[0].second.all_properties);
	for (size_t index = 0U; index < result.rowCount(); ++index) {
		const auto &node = result.getRows()[index].at("n").asLogicalNode();
		EXPECT_EQ(node.node_id, static_cast<uint64_t>((index % 3U) + 1U));
	}
}

TEST(LogicalGraphReadExecutorTest, BatchesDeferredEdgeRefsAcrossResultRowsWithoutDroppingDuplicates) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_ref_scans = true;
	data.enable_native_adjacent_topology = true;
	const auto edgeBatchRequests = std::make_shared<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>>();
	data.edge_batch_read_requests = edgeBatchRequests;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);

	auto first = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source"), "source", "relationship", "target", "", "out");
	auto second = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source"), "source", "relationship", "target", "", "out");
	logical::LogicalUnion unionAll(std::move(first), std::move(second), true);
	const graph::query::QueryResult result = runtime.executeToQueryResult(unionAll, runtimeBounds());

	ASSERT_EQ(result.rowCount(), 6U);
	ASSERT_EQ(edgeBatchRequests->size(), 1U);
	EXPECT_EQ((*edgeBatchRequests)[0].first, (std::vector<uint64_t>{10U, 11U, 12U}));
	EXPECT_TRUE((*edgeBatchRequests)[0].second.all_properties);
	for (size_t index = 0U; index < result.rowCount(); ++index) {
		const auto &edge = result.getRows()[index].at("relationship").asLogicalEdge();
		EXPECT_EQ(edge.edge_id, static_cast<uint64_t>((index % 3U) + 10U));
	}
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
