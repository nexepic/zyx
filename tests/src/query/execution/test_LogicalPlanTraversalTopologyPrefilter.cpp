#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"

namespace graph::query::execution {
using namespace direct_edge_test;
TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsTargetLabelsBeforeMaterializingTypedEdgePredicates) {
	DirectEdgeGraph graph = directGraph();
	graph.enable_native_topology_pages = true;
	graph.nodes.at(2U).labels = {"Other"};
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	for (const std::string type: {std::string("KNOWS"), std::string{}}) {
	logical::LogicalTraversal traversal(std::make_unique<logical::LogicalSingleRow>(),
			"a", "r", "b", type, "out", {"Person"}, {}, {{"weight", PropertyValue(7)}});
	for (int retry = 0; retry < 2; ++retry) {
		size_t rows = 0U;
		auto snapshot = backend.openLogicalGraphReadSnapshot();
		LogicalPlanReadRow input;
		input.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
		static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, traversal, directBounds(), input,
				[&](const LogicalPlanReadRow &) { ++rows; return true; }, QueryContext{}));
		EXPECT_EQ(rows, 0U);
		EXPECT_EQ(graph.edge_shaped_batch_reads, 0U);
		EXPECT_GT(graph.topology_scans, 0U);
	}
}
}
TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsMissingTypedTopologyEndpointBeforeReadingEdgeProperties) {
	DirectEdgeGraph graph = directGraph();
	graph.enable_native_topology_pages = true;
	graph.nodes.erase(2U);
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalTraversal traversal(std::make_unique<logical::LogicalSingleRow>(),
			"a", "r", "b", "KNOWS", "out", {}, {}, {{"weight", PropertyValue(7)}});
	const auto run = [&] {
		auto snapshot = backend.openLogicalGraphReadSnapshot();
		LogicalPlanReadRow input;
		input.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
		return runtime.executeWithSnapshotAndInputRow(*snapshot, traversal, directBounds(), input,
				[](const LogicalPlanReadRow &) { return true; }, QueryContext{});
	};
	EXPECT_THROW(static_cast<void>(run()), std::runtime_error);
	EXPECT_EQ(graph.edge_shaped_batch_reads, 0U);
	EXPECT_GT(graph.topology_scans, 0U);
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Person"}});
	for (int retry = 0; retry < 2; ++retry) EXPECT_NO_THROW(static_cast<void>(run()));
}
TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsDuplicateTopologyBeforeReadingEdgePropertiesAndRetries) {
	DirectEdgeGraph graph = directGraph();
	graph.enable_native_topology_pages = true;
	graph.edges.push_back(graph.edges.front());
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalTraversal traversal(std::make_unique<logical::LogicalSingleRow>(),
			"a", "r", "b", "KNOWS", "out", {}, {}, {{"weight", PropertyValue(7)}});
	const auto run = [&] {
		auto snapshot = backend.openLogicalGraphReadSnapshot();
		LogicalPlanReadRow input;
		input.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
		return runtime.executeWithSnapshotAndInputRow(*snapshot, traversal, directBounds(), input,
				[](const LogicalPlanReadRow &) { return true; }, QueryContext{});
	};
	EXPECT_THROW(static_cast<void>(run()), std::runtime_error);
	EXPECT_EQ(graph.edge_shaped_batch_reads, 0U);
	EXPECT_GT(graph.topology_scans, 0U);
	graph.edges.pop_back();
	for (int retry = 0; retry < 2; ++retry) EXPECT_NO_THROW(static_cast<void>(run()));
}
TEST(LogicalPlanDirectEdgeRuntimeTest, ReferenceTraversalRejectsMissingEndpointBeforePropertyColumns) {
	DirectEdgeGraph graph = directGraph();
	graph.enable_adjacency_reference_pages = true;
	graph.nodes.erase(2U);
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalTraversal traversal(std::make_unique<logical::LogicalSingleRow>(),
			"a", "r", "b", "KNOWS", "out", {}, {}, {{"weight", PropertyValue(7)}});
	const auto run = [&] {
		auto snapshot = backend.openLogicalGraphReadSnapshot();
		LogicalPlanReadRow input;
		input.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
		return runtime.executeWithSnapshotAndInputRow(*snapshot, traversal, directBounds(), input,
				[](const LogicalPlanReadRow &) { return true; }, QueryContext{});
	};
	EXPECT_THROW(static_cast<void>(run()), std::runtime_error);
	EXPECT_EQ(graph.edge_property_column_batch_reads, 0U);
	EXPECT_GT(graph.adjacency_ref_scans, 0U);
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Person"}});
	for (int retry = 0; retry < 2; ++retry) EXPECT_NO_THROW(static_cast<void>(run()));
}
} // namespace graph::query::execution
