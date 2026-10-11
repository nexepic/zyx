/**
 * @brief Native overlay token selection and presence-only reads.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/execution/LogicalGraphReadExecutor.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalVarLengthTraversal.hpp"

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, OverlaySelectedTokensExcludeUnresolvedFieldsAndPresenceKeepsNullDistinct) {
	using namespace graph::query;
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const PropertyValue payload(std::string(16000U, 'p'));
	const auto nodes = backend.createLogicalNodes({{{"Source"}, {{"known", PropertyValue(5)}}},
		{{"Target"}, {{"known", PropertyValue(5)}, {"nil", PropertyValue()}, {"payload", payload}}},
		{{"Target"}, {{"known", PropertyValue(6)}, {"payload", payload}}}});
	const auto edges = backend.createLogicalEdges({{nodes[0], nodes[1], "LINK", {{"known", PropertyValue(5)}}},
		{nodes[0], nodes[2], "LINK", {{"known", PropertyValue(6)}}}});
	for (bool vector: {false, true}) {
		SCOPED_TRACE(vector);
		auto transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphTransactionOverlay overlay(*transaction);
		auto snapshot = vector ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
			: overlay.openLogicalGraphReadSnapshot();
		ASSERT_TRUE(snapshot->supportsPropertyTokenResolution());
		ASSERT_TRUE(snapshot->supportsPropertyPresenceProbes());
		ASSERT_TRUE(snapshot->findPropertyToken("known"));
		const uint32_t known = *snapshot->findPropertyToken("known");
		EXPECT_EQ(snapshot->nodePropertyExists(nodes[1], "nil"), true);
		EXPECT_EQ(snapshot->nodePropertyExists(nodes[2], "nil"), false);
		overlay.replaceLogicalNodeProperties({{nodes[1], {{"known", PropertyValue(5)}, {"fresh", PropertyValue()},
			{"unallocated_payload", payload}}}});
		overlay.replaceLogicalEdgeProperties({{edges[0], {{"known", PropertyValue(5)}, {"fresh", PropertyValue()},
			{"unallocated_payload", payload}}}});
		ASSERT_FALSE(snapshot->findPropertyToken("fresh"));
		ASSERT_FALSE(snapshot->findPropertyToken("unallocated_payload"));
		const EntityReadShape shape{.property_tokens = {known}};
		for (size_t retry = 0U; retry < 2U; ++retry) {
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
			EXPECT_EQ(snapshot->nodePropertyExists(nodes[1], "fresh"), true);
			EXPECT_EQ(snapshot->nodePropertyExists(nodes[1], "absent"), false);
			EXPECT_EQ(snapshot->edgePropertyExists(edges[0], "fresh"), true);
			EXPECT_EQ(snapshot->edgePropertyExists(edges[0], "absent"), false);
			const auto node = snapshot->readNodeShaped(nodes[1], shape);
			const auto edge = snapshot->readEdgeShaped(edges[0], shape);
			ASSERT_EQ(node.properties.size(), 1U); EXPECT_EQ(node.properties[0].key, "known");
			ASSERT_EQ(edge.properties.size(), 1U); EXPECT_EQ(edge.properties[0].value, PropertyValue(5));
			size_t admitted = 0U;
			const auto admit = [&](size_t bytes) { admitted += bytes; if (admitted > 256U) throw std::length_error("unrelated source admitted"); };
			EXPECT_EQ(snapshot->readNodesShapedSortedWithAdmission(std::array<uint64_t, 1U>{nodes[1]}, shape, admit)[0].properties.size(), 1U);
			EXPECT_EQ(snapshot->readEdgesShapedSortedWithAdmission(std::array<uint64_t, 1U>{edges[0]}, shape, admit)[0].properties.size(), 1U);
			for (bool adjacent: {false, true}) {
				for (bool stop: {false, true}) {
					std::vector<uint64_t> visited;
					const auto visitor = [&](const LogicalGraphEdgeTopology &topology) {
						EXPECT_EQ(topology.source_node_id, nodes[0]);
						EXPECT_EQ(topology.type, "LINK");
						visited.push_back(topology.edge_id);
						return !stop;
					};
					if (adjacent) snapshot->visitAdjacentEdgeTopologies(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, 8U, visitor);
					else snapshot->visitEdgeTopologies(8U, visitor);
					EXPECT_EQ(visited, stop ? std::vector<uint64_t>{edges[0]} : edges);
				}
			}
			EXPECT_THROW(snapshot->visitEdgeTopologies(8U, {}), std::invalid_argument);
			EXPECT_THROW(snapshot->visitAdjacentEdgeTopologies(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, 8U, {}), std::invalid_argument);
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
		}
		execution::LogicalPlanReadRuntime runtime(overlay);
		auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalSingleRow>(), "source", "", "target", "LINK", "out", 0, 1,
			std::vector<std::string>{}, std::vector<std::pair<std::string, PropertyValue>>{{"fresh", PropertyValue()}});
		std::vector<std::unique_ptr<expressions::Expression>> args;
		args.push_back(std::make_unique<expressions::VariableReferenceExpression>("target"));
		logical::LogicalProject project(std::move(traversal),
			{{std::make_shared<expressions::FunctionCallExpression>("id", std::move(args)), "id"}});
		execution::LogicalPlanReadRuntimeBounds bounds{.maximum_node_records = 8U, .maximum_node_page_records = 8U,
			.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U, .maximum_edge_records = 8U,
			.maximum_adjacency_page_records = 8U, .maximum_property_entries = 8U, .maximum_rows = 8U,
			.maximum_distinct_rows = 8U, .maximum_materialized_bytes = 8192U};
		execution::LogicalPlanReadRow input;
		input.values.emplace("source", EntityRef{nodes[0], false, 0U, 0U, 0U});
		for (size_t retry = 0U; retry < 2U; ++retry) {
			std::vector<int64_t> result;
			static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, project, bounds, input,
				[&](const execution::LogicalPlanReadRow &row) {
					result.push_back(std::get<int64_t>(std::get<PropertyValue>(row.values.at("id")).getVariant()));
					return true;
				}, QueryContext{}));
			EXPECT_EQ(result, std::vector<int64_t>{static_cast<int64_t>(nodes[1])});
		}
		EXPECT_EQ(snapshot->readNodeShaped(nodes[1], {.all_properties = true}).properties.size(), 3U);
		for (bool targetFilter: {false, true}) {
			SCOPED_TRACE(targetFilter);
			execution::LogicalGraphReadRequest request;
			request.source_filter.required_labels = {targetFilter ? "Source" : "Target"};
			if (targetFilter) {
				execution::LogicalGraphExpansion expansion;
				expansion.edge_filter.relationship_type = "LINK";
				expansion.target_filter.required_labels = {"Target"};
				expansion.target_filter.property_predicates = {{"fresh", execution::LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(7), {}}};
				request.expansion = expansion;
			} else request.source_filter.property_predicates = {{"fresh", execution::LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(7), {}}};
			request.bounds = {8U, 8U, 8U, 8U};
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
			execution::LogicalGraphReadExecutor executor(overlay);
			const auto stats = executor.execute(request, [](const auto &) { ADD_FAILURE() << "NULL/missing must not match 7"; });
			EXPECT_EQ(stats.rows_emitted, 0U);
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
		}
		for (const std::string key: {"fresh", "absent"}) {
			auto countTraversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}),
				"source", "edge", "target", "LINK", "out", std::vector<std::string>{"Target"},
				std::vector<std::pair<std::string, PropertyValue>>{{key, PropertyValue()}});
			logical::LogicalAggregate aggregate(std::move(countTraversal), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("target"), "count", false}});
			const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
			graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
			for (size_t retry = 0U; retry < 2U; ++retry) {
				size_t rows = 0U;
				static_cast<void>(runtime.execute(aggregate, bounds, [&](const execution::LogicalPlanReadRow &row) {
					++rows;
					EXPECT_EQ(std::get<PropertyValue>(row.values.at("count")), PropertyValue(key == "fresh" ? 1 : 0));
					return true;
				}));
				EXPECT_EQ(rows, 1U);
			}
			const auto trace = graph::debug::PerfTrace::snapshotAndReset();
			graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
			EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
		}
		transaction->rollback();
		EXPECT_THROW(static_cast<void>(snapshot->nodePropertyExists(nodes[1], "fresh")), std::logic_error);
		EXPECT_THROW(static_cast<void>(snapshot->supportsPropertyTokenResolution()), std::logic_error);
	}
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, SelectedNodeScanReadsAllFieldsInOneOwnerBatchAndReusesSession) {
	materializeNativeDatabase();
	auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options)); backend.open();
	const auto ids = backend.createLogicalNodes({{{"Batch"}, {{"first", PropertyValue(3)}, {"second", PropertyValue()},
		{"unused", PropertyValue(std::string(16000U, 'u'))}}}, {{"Batch"}, {{"first", PropertyValue(4)},
		{"unused", PropertyValue(std::string(16000U, 'v'))}}}});
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	ASSERT_TRUE(snapshot->findPropertyToken("first")); ASSERT_TRUE(snapshot->findPropertyToken("second"));
	EntityReadShape shape{.include_labels = true, .property_tokens = {*snapshot->findPropertyToken("first"), *snapshot->findPropertyToken("second")}};
	std::ranges::sort(shape.property_tokens);
	const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	for (size_t retry = 0U; retry < 2U; ++retry) {
		graph::debug::PerfTrace::reset();
		const auto page = snapshot->scanNodesAfter(0U, 8U, 8U, shape);
		ASSERT_EQ(page.nodes.size(), 2U);
		EXPECT_EQ(page.nodes[0].node_id, ids[0]); EXPECT_EQ(page.nodes[1].node_id, ids[1]);
		EXPECT_EQ(page.nodes[0].properties.size(), 2U); EXPECT_EQ(page.nodes[1].properties.size(), 1U);
		EXPECT_EQ(page.nodes[0].properties[0].value, PropertyValue(3));
		EXPECT_EQ(page.nodes[0].properties[1].value, PropertyValue());
		EXPECT_EQ(page.nodes[1].properties[0].value, PropertyValue(4));
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
		if (retry == 0U) {
			ASSERT_TRUE(trace.contains("native_graph.read.property_selected_column_decode"));
			EXPECT_EQ(trace.at("native_graph.read.property_selected_column_decode").totalValue, 2);
		} else EXPECT_FALSE(trace.contains("native_graph.read.property_selected_decode"));
	}
	graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
	snapshot.reset(); backend.close();
}
