/**
 * @file test_LogicalGraphReadExecutor_IndexLookupContracts.cpp
 * @brief Index-lookup protocol contracts for the logical read runtime.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "query/execution/LogicalGraphReadExecutorTestFixture.hpp"

#include "graph/query/logical/operators/LogicalUnwind.hpp"

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
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_NE(snapshot, nullptr);
	LogicalPlanReadRow input;
	input.values.emplace("node", LogicalGraphNode{1U, {"stale"}});
	input.values.emplace("edge", LogicalGraphEdge{10U, 1U, 1U, "stale"});
	input.values.emplace("scalar", PropertyValue(42));
	input.property_cache.emplace("node", std::unordered_map<std::string, PropertyValue>{{"cached", PropertyValue(1)}});
	input.property_cache.emplace("edge", std::unordered_map<std::string, PropertyValue>{{"cached", PropertyValue(1)}});
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
	EXPECT_EQ(std::get<LogicalGraphEdge>(rows.front().values.at("edge")), graphData().edges.at(10U));
	EXPECT_EQ(std::get<PropertyValue>(rows.front().values.at("scalar")), PropertyValue(42));
	EXPECT_FALSE(rows.front().property_cache.contains("node"));
	EXPECT_FALSE(rows.front().property_cache.contains("edge"));
	EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));

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
