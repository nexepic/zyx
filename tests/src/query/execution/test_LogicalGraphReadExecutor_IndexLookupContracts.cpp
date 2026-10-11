/**
 * @file test_LogicalGraphReadExecutor_IndexLookupContracts.cpp
 * @brief Index-lookup protocol contracts for the logical read runtime.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "query/execution/LogicalGraphReadExecutorTestFixture.hpp"

#include "graph/query/QueryGuard.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"

#include <chrono>
#include <thread>

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, RejectsOutOfScopeAndNonMonotonicExactLookupResults) {
	{
		LogicalGraphFixtureData data = graphData();
		data.enable_exact_node_lookup = true;
		data.exact_lookup_result = std::vector<LogicalGraphNode>{data.nodes.at(3U)};
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("node", {"Person"}, {{"age", PropertyValue(42)}});
		EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
			return true;
		})), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.enable_exact_node_lookup = true;
		data.exact_lookup_result = std::vector<LogicalGraphNode>{data.nodes.at(2U), data.nodes.at(1U)};
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("node", {"Person"}, {{"age", PropertyValue(42)}});
		EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
			return true;
		})), std::runtime_error);
	}
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, RejectsLookupResultsBeyondTheConfiguredNodeBound) {
	LogicalGraphFixtureData data = graphData();
	data.enable_exact_node_lookup = true;
	data.exact_lookup_result = std::vector<LogicalGraphNode>{data.nodes.at(1U), data.nodes.at(2U)};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("node", {"Person"}, {{"age", PropertyValue(42)}});
	LogicalPlanReadRuntimeBounds constrained = runtimeBounds();
	constrained.maximum_node_records = 1U;
	EXPECT_THROW(static_cast<void>(runtime.execute(scan, constrained, [](const LogicalPlanReadRow &) {
		return true;
	})), std::runtime_error);
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, RejectsMalformedNodeRecordsFromTheLogicalReadBoundary) {
	const auto execute = [](LogicalGraphFixtureData data) {
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("node");
		return runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; });
	};
	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).node_id = 0U;
		EXPECT_THROW(static_cast<void>(execute(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).labels = {"Person", "Person"};
		EXPECT_THROW(static_cast<void>(execute(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).labels = {""};
		EXPECT_THROW(static_cast<void>(execute(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).properties = {{"age", PropertyValue(42)}, {"age", PropertyValue(43)}};
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("node", {"Person"}, {{"age", PropertyValue(42)}});
		EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
			return true;
		})), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.nodes.at(1U).properties = {{"", PropertyValue(42)}};
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("node", std::vector<std::string>{"Person"},
									 {{"age", PropertyValue(42)}});
		EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
			return true;
		})), std::runtime_error);
	}
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, RejectsMalformedAdjacentEdgeRecordsBeforeTraversal) {
	const auto execute = [](LogicalGraphFixtureData data) {
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Person"});
		logical::LogicalTraversal traversal(std::move(source), "source", "edge", "target", "KNOWS", "out");
		return runtime.execute(traversal, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; });
	};
	{
		LogicalGraphFixtureData data = graphData();
		data.edges.at(10U).edge_id = 0U;
		EXPECT_THROW(static_cast<void>(execute(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.edges.at(10U).source_node_id = 0U;
		// A malicious snapshot can return an Edge that does not belong to the requested adjacency page.
		data.return_invalid_adjacency_page_endpoint = true;
		EXPECT_THROW(static_cast<void>(execute(std::move(data))), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.edges.at(10U).target_node_id = 0U;
		EXPECT_THROW(static_cast<void>(execute(std::move(data))), std::runtime_error);
	}
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, RejectsOutOfScopeRangeAndCompositeLookupResults) {
	{
		LogicalGraphFixtureData data = graphData();
		data.enable_range_node_lookup = true;
		data.range_lookup_result = std::vector<LogicalGraphNode>{data.nodes.at(3U)};
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("node", {"Person"});
		scan.setRangePredicates({{"age", PropertyValue(30), PropertyValue(), true, true}});
		EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
			return true;
		})), std::runtime_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.enable_composite_range_node_lookup = true;
		data.composite_range_lookup_result = std::vector<LogicalGraphNode>{data.nodes.at(3U)};
		data.expected_composite_range_lookup = {
			{{"status", PropertyValue("online")}}, {"age", PropertyValue(30), PropertyValue(), true, true}};
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("node", {"Person"}, {{"status", PropertyValue("online")}});
		scan.setRangePredicates({{"age", PropertyValue(30), PropertyValue(), true, true}});
		EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
			return true;
		})), std::runtime_error);
	}
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, RejectsMalformedVerifiedNativeRangeCandidateFields) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 9U;
	data.range_candidate_result = IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 9U, false, true, {}};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("node", {"Person"});
	scan.setRangePredicates({{"age", PropertyValue(30), PropertyValue(), true, true}});
	EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
		return true;
	})), std::runtime_error);
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, RejectsVerifiedNativeCandidatesWhoseColumnsViolateTheScan) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_range_node_candidates = true;
	data.native_snapshot_generation = 9U;
	data.range_candidate_result = IndexCandidateBatch{
			{{1U, false, 0U, 0U, 0U}}, 9U, false, true, {{"age", {PropertyValue(10)}}}};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("node", {"Person"});
	scan.setRangePredicates({{"age", PropertyValue(30), PropertyValue(), true, true}});

	EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
		return true;
	})), std::runtime_error);
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, RefreshesImportedEntitiesAndRejectsStoppedInputRowExecution) {
	auto data = graphData();
	data.node_batch_read_count = std::make_shared<size_t>(0U);
	data.node_batch_read_requests = std::make_shared<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>>();
	data.edge_batch_read_count = std::make_shared<size_t>(0U);
	data.edge_batch_read_requests = std::make_shared<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>>();
	const auto nodeBatchReadCount = data.node_batch_read_count;
	const auto nodeBatchReadRequests = data.node_batch_read_requests;
	const auto edgeBatchReadCount = data.edge_batch_read_count;
	const auto edgeBatchReadRequests = data.edge_batch_read_requests;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalPlanReadRow input;
	input.values.emplace("node", LogicalGraphNode{1U, {"stale"}});
	input.values.emplace("nodeAlias", LogicalGraphNode{1U, {"stale"}});
	input.values.emplace("node2", LogicalGraphNode{2U, {"stale"}});
	input.values.emplace("node3", LogicalGraphNode{3U, {"stale"}});
	input.values.emplace("edge", LogicalGraphEdge{10U, 1U, 1U, "stale"});
	input.values.emplace("edgeAlias", LogicalGraphEdge{10U, 1U, 1U, "stale"});
	input.values.emplace("edge2", LogicalGraphEdge{11U, 1U, 1U, "stale"});
	input.values.emplace("edge3", LogicalGraphEdge{12U, 1U, 1U, "stale"});
	input.values.emplace("scalar", PropertyValue(42));
	input.property_cache.emplace("node", std::unordered_map<std::string, PropertyValue>{{"cached", PropertyValue(1)}});
	input.property_cache.emplace("edge", std::unordered_map<std::string, PropertyValue>{{"cached", PropertyValue(1)}});
	input.entity_metadata_cache.emplace("node", std::unordered_map<std::string, PropertyValue>{{"labels", PropertyValue("stale")} });
	input.entity_metadata_cache.emplace("edge", std::unordered_map<std::string, PropertyValue>{{"type", PropertyValue("stale")} });
	input.complete_entity_property_cache.insert("node");
	input.complete_entity_property_cache.insert("edge");
	logical::LogicalSingleRow root;
	graph::query::QueryContext context;
	std::vector<LogicalPlanReadRow> rows;
	const LogicalPlanReadRuntimeStats stats = runtime.executeWithSnapshotAndInputRow(
			*snapshot, root, runtimeBounds(), input, [&rows](const LogicalPlanReadRow &row) {
				rows.push_back(row);
				return true;
			}, context);

	ASSERT_EQ(rows.size(), 1U);
	EXPECT_EQ(std::get<LogicalGraphNode>(rows.front().values.at("node")), graphData().nodes.at(1U));
	EXPECT_EQ(std::get<LogicalGraphNode>(rows.front().values.at("nodeAlias")), graphData().nodes.at(1U));
	EXPECT_EQ(std::get<LogicalGraphNode>(rows.front().values.at("node2")), graphData().nodes.at(2U));
	EXPECT_EQ(std::get<LogicalGraphNode>(rows.front().values.at("node3")), graphData().nodes.at(3U));
	EXPECT_EQ(std::get<LogicalGraphEdge>(rows.front().values.at("edge")), graphData().edges.at(10U));
	EXPECT_EQ(std::get<LogicalGraphEdge>(rows.front().values.at("edgeAlias")), graphData().edges.at(10U));
	EXPECT_EQ(std::get<LogicalGraphEdge>(rows.front().values.at("edge2")), graphData().edges.at(11U));
	EXPECT_EQ(std::get<LogicalGraphEdge>(rows.front().values.at("edge3")), graphData().edges.at(12U));
	EXPECT_EQ(std::get<PropertyValue>(rows.front().values.at("scalar")), PropertyValue(42));
	EXPECT_FALSE(rows.front().property_cache.contains("node"));
	EXPECT_FALSE(rows.front().property_cache.contains("edge"));
	EXPECT_FALSE(rows.front().entity_metadata_cache.contains("node"));
	EXPECT_FALSE(rows.front().entity_metadata_cache.contains("edge"));
	EXPECT_FALSE(rows.front().complete_entity_property_cache.contains("node"));
	EXPECT_FALSE(rows.front().complete_entity_property_cache.contains("edge"));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));
	EXPECT_EQ(*nodeBatchReadCount, 1U);
	EXPECT_EQ(*edgeBatchReadCount, 1U);
	ASSERT_EQ(nodeBatchReadRequests->size(), 1U);
	ASSERT_EQ(edgeBatchReadRequests->size(), 1U);
	EXPECT_EQ(nodeBatchReadRequests->front().first, (std::vector<uint64_t>{1U, 2U, 3U}));
	EXPECT_EQ(edgeBatchReadRequests->front().first, (std::vector<uint64_t>{10U, 11U, 12U}));

	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
				 *snapshot, root, runtimeBounds(), input, [](const LogicalPlanReadRow &) { return false; }, context)),
			 std::runtime_error);

	LogicalPlanReadRuntimeBounds bounded = runtimeBounds();
	bounded.maximum_rows = 1U;
	logical::LogicalUnwind overBound(std::make_unique<logical::LogicalSingleRow>(), "item",
										std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)});
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
				 *snapshot, overBound, bounded, input, [](const LogicalPlanReadRow &) { return true; }, context)),
			 std::runtime_error);
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, ChecksCancellationBeforeRefreshingImportedEntities) {
	LogicalGraphFixtureData data = graphData();
	data.node_batch_read_count = std::make_shared<size_t>(0U);
	const auto nodeBatchReadCount = data.node_batch_read_count;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalPlanReadRow input;
	input.values.emplace("node", LogicalGraphNode{1U, {"stale"}});
	logical::LogicalSingleRow root;
	graph::query::QueryGuard guard(1, 0);
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	graph::query::QueryContext context;
	context.guard = &guard;
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
			*snapshot, root, runtimeBounds(), input, [](const LogicalPlanReadRow &) { return true; }, context)),
			 graph::query::QueryTimeoutException);
	EXPECT_EQ(*nodeBatchReadCount, 0U);
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, ImportedEntityBudgetRejectsThenRefreshesAllBindings) {
	LogicalGraphFixtureData data = graphData();
	for (auto &[_, node]: data.nodes) {
		node.properties.push_back({"payload", PropertyValue(std::string(1000U, 'x'))});
	}
	data.node_batch_read_count = std::make_shared<size_t>(0U);
	data.node_batch_read_requests = std::make_shared<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>>();
	const auto nodeBatchReadCount = data.node_batch_read_count;
	const auto nodeBatchReadRequests = data.node_batch_read_requests;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalPlanReadRow input;
	input.values.emplace("first", LogicalGraphNode{1U, {"stale"}});
	input.values.emplace("second", LogicalGraphNode{2U, {"stale"}});
	input.values.emplace("third", LogicalGraphNode{3U, {"stale"}});
	logical::LogicalAggregate countOnly(std::make_unique<logical::LogicalSingleRow>(), {},
		{{"count", nullptr, "count"}});
	auto constrained = runtimeBounds();
	constrained.maximum_materialized_bytes = 3000U;
	graph::query::QueryContext context;
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, countOnly, constrained, input,
		[](const LogicalPlanReadRow &) { return true; }, context)), std::runtime_error);
	*nodeBatchReadCount = 0U; nodeBatchReadRequests->clear();
	constrained.maximum_materialized_bytes = 64U * 1024U;
	std::vector<PropertyValue> counts;
	static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, countOnly, constrained, input,
		[&](const LogicalPlanReadRow &row) {
			counts.push_back(std::get<PropertyValue>(row.values.at("count")));
			return true;
		}, context));
	EXPECT_EQ(counts, (std::vector<PropertyValue>{PropertyValue(int64_t{1})}));
	EXPECT_GE(*nodeBatchReadCount, 1U);
	ASSERT_FALSE(nodeBatchReadRequests->empty());
	EXPECT_EQ(nodeBatchReadRequests->front().first, (std::vector<uint64_t>{1U, 2U, 3U}));

	LogicalGraphFixtureData edgeData = graphData();
	for (auto &[_, edge]: edgeData.edges) {
		edge.properties.push_back({"payload", PropertyValue(std::string(1000U, 'e'))});
	}
	edgeData.edge_batch_read_count = std::make_shared<size_t>(0U);
	edgeData.edge_batch_read_requests = std::make_shared<std::vector<std::pair<std::vector<uint64_t>, EntityReadShape>>>();
	const auto edgeBatchReadCount = edgeData.edge_batch_read_count;
	const auto edgeBatchReadRequests = edgeData.edge_batch_read_requests;
	TestLogicalGraphBackend edgeBackend(std::move(edgeData));
	LogicalPlanReadRuntime edgeRuntime(edgeBackend);
	auto edgeSnapshot = edgeBackend.openLogicalGraphReadSnapshot();
	ASSERT_NE(edgeSnapshot, nullptr);
	LogicalPlanReadRow edgeInput;
	edgeInput.values.emplace("first", LogicalGraphEdge{10U, 1U, 2U, "stale"});
	edgeInput.values.emplace("second", LogicalGraphEdge{11U, 1U, 3U, "stale"});
	edgeInput.values.emplace("third", LogicalGraphEdge{12U, 2U, 1U, "stale"});
	logical::LogicalAggregate edgeCountOnly(std::make_unique<logical::LogicalSingleRow>(), {},
		{{"count", nullptr, "count"}});
	std::vector<PropertyValue> edgeCounts;
	static_cast<void>(edgeRuntime.executeWithSnapshotAndInputRow(*edgeSnapshot, edgeCountOnly, constrained, edgeInput,
		[&](const LogicalPlanReadRow &row) {
			edgeCounts.push_back(std::get<PropertyValue>(row.values.at("count")));
			return true;
		}, context));
	EXPECT_EQ(edgeCounts, (std::vector<PropertyValue>{PropertyValue(int64_t{1})}));
	EXPECT_GE(*edgeBatchReadCount, 1U);
	ASSERT_FALSE(edgeBatchReadRequests->empty());
	EXPECT_EQ(edgeBatchReadRequests->front().first, (std::vector<uint64_t>{10U, 11U, 12U}));
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, DirectRelationshipCountRejectsUnboundedAndInvalidTopologyInputs) {
	const auto makeCount = [] {
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("source"), "source", "edge", "target", "", "out");
		return logical::LogicalAggregate(
				std::move(traversal), {},
				{{"count", sharedExpression(std::make_unique<expressions::VariableReferenceExpression>("edge")), "count"}});
	};
	{
		TestLogicalGraphBackend backend(graphData());
		LogicalPlanReadRuntime runtime(backend);
		LogicalPlanReadRuntimeBounds bounded = runtimeBounds();
		bounded.maximum_rows = 2U;
		auto count = makeCount();
		EXPECT_THROW(static_cast<void>(runtime.execute(count, bounded, [](const LogicalPlanReadRow &) {
			return true;
		})), std::runtime_error);
	}
	{
		TestLogicalGraphBackend backend(graphData());
		LogicalPlanReadRuntime runtime(backend);
		LogicalPlanReadRuntimeBounds unbounded = runtimeBounds();
		unbounded.maximum_rows = std::numeric_limits<size_t>::max();
		auto count = makeCount();
		EXPECT_THROW(static_cast<void>(runtime.execute(count, unbounded, [](const LogicalPlanReadRow &) {
			return true;
		})), std::overflow_error);
	}
	{
		LogicalGraphFixtureData data = graphData();
		data.edges.at(10U).target_node_id = 99U;
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		auto count = makeCount();
		EXPECT_THROW(static_cast<void>(runtime.execute(count, runtimeBounds(), [](const LogicalPlanReadRow &) {
			return true;
		})), std::runtime_error);
	}
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, DirectRelationshipCountAdmitsEndpointIndexBeforeAllocation) {
	const auto makeCount = [] {
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("source"), "source", "edge", "target", "", "out");
		return logical::LogicalAggregate(
				std::move(traversal), {},
				{{"count", sharedExpression(std::make_unique<expressions::VariableReferenceExpression>("edge")), "count"}});
	};
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds bounded = runtimeBounds();
	bool endpointAdmissionRejected = false;
	for (size_t budget = 140U; budget < 4096U; budget += 8U) {
		bounded.maximum_materialized_bytes = budget;
		auto lowBudgetCount = makeCount();
		try {
			static_cast<void>(runtime.execute(lowBudgetCount, bounded, [](const LogicalPlanReadRow &) { return true; }));
			break;
		} catch (const std::runtime_error &error) {
			if (std::string_view(error.what()) == "shared materialization exceeded its byte bound") {
				endpointAdmissionRejected = true;
				break;
			}
			if (std::string_view(error.what()) != "logical-plan output materialization exceeds its row or byte bound") {
				ADD_FAILURE() << "unexpected low-budget failure: " << error.what();
				break;
			}
		}
	}
	EXPECT_TRUE(endpointAdmissionRejected) << "could not reach endpoint-index admission after output-batch admission";

	bounded.maximum_materialized_bytes = 4096U;
	auto admittedCount = makeCount();
	std::vector<PropertyValue> counts;
	static_cast<void>(runtime.execute(admittedCount, bounded, [&counts](const LogicalPlanReadRow &row) {
		counts.push_back(std::get<PropertyValue>(row.values.at("count")));
		return true;
	}));
	EXPECT_EQ(counts, std::vector<PropertyValue>{PropertyValue(int64_t{3})});
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest,
	 RejectsMalformedParameterPropertyPredicatesBeforeOpeningTheNodeScan) {
	const auto execute = [](logical::LogicalNodeScan scan, const graph::query::QueryContext *context) {
		LogicalGraphFixtureData data = graphData();
		auto nodeReads = std::make_shared<size_t>(0U);
		data.node_read_count = nodeReads;
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		if (context == nullptr) {
			EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
				return true;
			})), std::runtime_error);
		} else {
			EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), [](const LogicalPlanReadRow &) {
				return true;
			}, *context)), std::invalid_argument);
		}
		EXPECT_EQ(*nodeReads, 0U);
	};

	logical::LogicalNodeScan missingContext("node");
	missingContext.setParameterPropertyPredicates({{"age", "age"}});
	execute(std::move(missingContext), nullptr);

	graph::query::QueryContext context;
	context.parameters.emplace("age", PropertyValue(42));
	logical::LogicalNodeScan emptyKey("node");
	emptyKey.setParameterPropertyPredicates({{"", "age"}});
	execute(std::move(emptyKey), &context);

	logical::LogicalNodeScan emptyParameter("node");
	emptyParameter.setParameterPropertyPredicates({{"age", ""}});
	execute(std::move(emptyParameter), &context);

	logical::LogicalNodeScan duplicateParameter("node");
	duplicateParameter.setParameterPropertyPredicates({{"age", "age"}, {"age", "age"}});
	execute(std::move(duplicateParameter), &context);

	logical::LogicalNodeScan conflictingStatic("node", {}, {{"age", PropertyValue(42)}});
	conflictingStatic.setParameterPropertyPredicates({{"age", "age"}});
	execute(std::move(conflictingStatic), &context);

	logical::LogicalNodeScan duplicateStatic("node", {}, {{"age", PropertyValue(42)}, {"age", PropertyValue(30)}});
	execute(std::move(duplicateStatic), &context);

	context.parameters.clear();
	logical::LogicalNodeScan missingParameter("node");
	missingParameter.setParameterPropertyPredicates({{"age", "age"}});
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	EXPECT_THROW(static_cast<void>(runtime.execute(missingParameter, runtimeBounds(), [](const LogicalPlanReadRow &) {
		return true;
	}, context)), std::runtime_error);

	context.parameters.emplace("age", PropertyValue());
	logical::LogicalNodeScan nullParameter("node");
	nullParameter.setParameterPropertyPredicates({{"age", "age"}});
	EXPECT_EQ(runtime.execute(nullParameter, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; }, context),
			  (LogicalPlanReadRuntimeStats{}));
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, UnverifiedCandidatesSplitBeforeDeliveryAndKeepMissingNullAndCancellation) {
	for (bool admit: {false, true}) {
		LogicalGraphFixtureData data = graphData();
		data.enable_native_exact_node_candidates = true;
		data.enable_property_admission = admit;
		data.native_snapshot_generation = 17U;
		data.expected_exact_labels = std::vector<std::string>{};
		data.expected_exact_properties = std::vector<std::pair<std::string, PropertyValue>>{{"payload", PropertyValue(std::string(1000U, 'a'))}};
		for (auto &[id, node]: data.nodes) node.properties = {{"payload", PropertyValue(std::string(1000U, id == 2U ? 'b' : 'a'))}};
		data.nodes.emplace(4U, LogicalGraphNode{4U, {}, {}});
		data.exact_candidate_result = IndexCandidateBatch{
			{{1U, false, 0U, 0U, 0U}, {2U, false, 0U, 0U, 0U}, {3U, false, 0U, 0U, 0U}, {4U, false, 0U, 0U, 0U}}, 17U, false, false, {}};
		auto reads = std::make_shared<size_t>(0U);
		data.node_property_column_read_count = reads;
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"payload", PropertyValue(std::string(1000U, 'a'))}});
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "payload"), "payload");
		logical::LogicalProject project(std::move(scan), std::move(items));
		auto bounds = runtimeBounds();
		bounds.maximum_materialized_bytes = 2000U;
		bounds.maximum_node_records = 4U;
		for (bool stop: {false, true, false}) {
			*reads = 0U;
			std::vector<PropertyValue> delivered;
			LogicalPlanReadRuntimeStats stats;
			EXPECT_NO_THROW(stats = runtime.execute(project, bounds, [&](const LogicalPlanReadRow &row) {
				delivered.push_back(std::get<PropertyValue>(row.values.at("payload"))); return !stop;
			}));
			EXPECT_EQ(delivered, std::vector<PropertyValue>(stop ? 1U : 2U, PropertyValue(std::string(1000U, 'a'))));
			EXPECT_GT(*reads, 1U);
			EXPECT_EQ(stats.nodes_scanned, stop ? 1U : 4U);
		}
	}
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, UnverifiedCandidateRejectsOversizedSourceBeforeCopyAndMalformedColumnsBeforeRetry) {
	for (bool malformed: {false, true}) {
		LogicalGraphFixtureData data = graphData();
		data.enable_native_exact_node_candidates = true;
		data.enable_property_admission = !malformed;
		data.truncate_node_property_columns = malformed;
		data.native_snapshot_generation = 17U;
		data.expected_exact_labels = std::vector<std::string>{};
		data.expected_exact_properties = std::vector<std::pair<std::string, PropertyValue>>{{"payload", PropertyValue(std::string(4000U, 'a'))}};
		data.nodes.at(1U).properties = {{"payload", PropertyValue(std::string(4000U, 'a'))}};
		data.exact_candidate_result = IndexCandidateBatch{{{1U, false, 0U, 0U, 0U}}, 17U, false, false, {}};
		auto copies = std::make_shared<size_t>(0U);
		auto reads = std::make_shared<size_t>(0U);
		data.admitted_property_copies = copies;
		data.node_property_column_read_count = reads;
		TestLogicalGraphBackend backend(std::move(data));
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n", {}, {{"payload", PropertyValue(std::string(4000U, 'a'))}});
		auto bounds = runtimeBounds(); bounds.maximum_materialized_bytes = 2000U;
		size_t delivered = 0U;
		try {
			static_cast<void>(runtime.execute(scan, bounds, [&](const LogicalPlanReadRow &) { ++delivered; return true; }));
			FAIL() << "invalid or oversized selected values must reject before delivery";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), malformed ? "logical graph backend changed indexed Node Property column cardinality"
				: "logical-plan indexed Node Property batch exceeded its byte bound");
		}
		EXPECT_EQ(delivered, 0U);
		EXPECT_EQ(*copies, 0U);
		EXPECT_EQ(*reads, 1U);
	}
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, VerifiedColumnsAndIndependentRowsShareAllowanceAndCanRerunAfterRejection) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_exact_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.expected_exact_labels = std::vector<std::string>{};
	const PropertyValue payload(std::string(1000U, 'a'));
	data.expected_exact_properties = std::vector<std::pair<std::string, PropertyValue>>{{"payload", payload}};
	data.exact_candidate_result = IndexCandidateBatch{
		{{1U, false, 0U, 0U, 0U}, {2U, false, 0U, 0U, 0U}, {3U, false, 0U, 0U, 0U}},
		17U, true, true, {{"payload", {payload, payload, payload}}}, true};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{},
		std::vector<std::pair<std::string, PropertyValue>>{{"payload", payload}});
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "payload"), "payload");
	logical::LogicalProject project(std::move(scan), std::move(items));
	auto bounds = runtimeBounds();
	for (const size_t allowance: {size_t{2400U}, size_t{4000U}}) {
		SCOPED_TRACE(allowance);
		bounds.maximum_materialized_bytes = allowance;
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.execute(project, bounds, [&](const LogicalPlanReadRow &) {
			++delivered; return true;
		})), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
	}
	bounds.maximum_materialized_bytes = 7000U;
	for (bool stop: {false, true, false}) {
		std::vector<PropertyValue> values;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(project, bounds, [&](const LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("payload"))); return !stop;
		})));
		EXPECT_EQ(values, std::vector<PropertyValue>(stop ? 1U : 3U, payload));
	}
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, SeenCandidateKeysAreBoundedAcrossStreamsAndReleasedAfterFailure) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_exact_node_candidates = true;
	data.native_snapshot_generation = 17U;
	data.exact_candidate_stream_results = std::vector<IndexCandidateBatch>{};
	for (uint64_t id = 1U; id <= 200U; ++id) {
		data.exact_candidate_stream_results->push_back(IndexCandidateBatch{
			{{id, false, 0U, 0U, 0U}}, 17U, true, true, {{"age", {PropertyValue(42)}}}, true});
	}
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"},
		std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(42)}});
	std::vector<std::unique_ptr<expressions::Expression>> arguments;
	arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("id", std::move(arguments)), "id");
	logical::LogicalProject project(std::move(scan), std::move(items));
	auto bounds = runtimeBounds();
	bounds.maximum_node_records = 256U;
	bounds.maximum_rows = 256U;
	bounds.maximum_materialized_bytes = 2000U;
	size_t firstCount = 0U;
	for (size_t run = 0U; run < 2U; ++run) {
		size_t count = 0U;
		EXPECT_THROW(static_cast<void>(runtime.execute(project, bounds, [&](const LogicalPlanReadRow &) {
			++count; return true;
		})), std::runtime_error);
		EXPECT_GT(count, 0U);
		EXPECT_LT(count, 200U);
		if (run == 0U) firstCount = count;
		else EXPECT_EQ(count, firstCount);
	}
	bounds.maximum_materialized_bytes = 10000U;
	std::vector<PropertyValue> ids;
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(project, bounds, [&](const LogicalPlanReadRow &row) {
		ids.push_back(std::get<PropertyValue>(row.values.at("id"))); return true;
	})));
	ASSERT_EQ(ids.size(), 200U);
	for (size_t index = 0U; index < ids.size(); ++index) EXPECT_EQ(ids[index], PropertyValue(static_cast<int64_t>(index + 1U)));
}

TEST(LogicalGraphReadExecutorIndexLookupContractsTest, ChargesNestedVerifiedCandidateValuesAndRetries) {
	LogicalGraphFixtureData data = graphData();
	data.enable_native_exact_node_candidates = true;
	data.native_snapshot_generation = 17U;
	const PropertyValue payload(std::vector<PropertyValue>(128U));
	data.expected_exact_properties = std::vector<std::pair<std::string, PropertyValue>>{{"payload", payload}};
	data.nodes.at(1U).properties = {{"payload", payload}};
	data.exact_candidate_result = IndexCandidateBatch{
		{{1U, false, 0U, 0U, 0U}}, 17U, true, true, {{"payload", {payload}}}, true};
	TestLogicalGraphBackend backend(std::move(data));
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("node", {"Person"}, {{"payload", payload}});
	auto limited = runtimeBounds();
	limited.maximum_candidate_bytes = 1024U;
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.execute(scan, limited, [&](const LogicalPlanReadRow &) {
		++delivered; return true;
	})), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	limited.maximum_candidate_bytes = 65536U;
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(scan, limited, [&](const LogicalPlanReadRow &row) {
		EXPECT_EQ(std::get<LogicalGraphNode>(row.values.at("node")).properties.front().value, payload);
		++delivered; return true;
	})));
	EXPECT_EQ(delivered, 1U);
}
