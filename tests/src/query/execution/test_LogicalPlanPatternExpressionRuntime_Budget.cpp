#include "query/execution/LogicalPlanPatternExpressionRuntimeTestFixture.hpp"

TEST(LogicalPlanPatternExpressionRuntimeTest, RetainsEdgeBytesWhileAdmittingPatternTargetBatch) {
	for (const bool referencePages: {false, true}) {
		SCOPED_TRACE(referencePages);
		PatternExpressionGraph graph;
		graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Seed"}});
		graph.nodes.emplace(2U,
							LogicalGraphNode{2U, {"Target"}, {{"payload", PropertyValue(std::string(1200U, 'n'))}}});
		graph.edges.emplace(10U,
							LogicalGraphEdge{10U, 1U, 2U, "R", {{"payload", PropertyValue(std::string(1200U, 'e'))}}});
		PatternExpressionGraph controlGraph = graph;
		controlGraph.edges.at(10U).properties.front().value = PropertyValue(std::string{});
		PatternExpressionSnapshotFaults faults;
		faults.supports_native_adjacency_refs = referencePages;
		faults.supports_shaped_property_admission = true;
		faults.edge_shaped_batch_read_count = std::make_shared<size_t>(0U);
		faults.node_shaped_admission_read_count = std::make_shared<size_t>(0U);
		faults.node_shaped_completed_read_count = std::make_shared<size_t>(0U);
		PatternExpressionSnapshotFaults controlFaults;
		controlFaults.supports_native_adjacency_refs = referencePages;
		controlFaults.supports_shaped_property_admission = true;
		controlFaults.node_shaped_completed_read_count = std::make_shared<size_t>(0U);
		PatternExpressionBackend controlBackend(std::move(controlGraph), controlFaults);
		LogicalPlanReadRuntime controlRuntime(controlBackend);
		PatternExpressionBackend backend(std::move(graph), faults);
		LogicalPlanReadRuntime runtime(backend);
		const QueryPlan plan =
				parse("MATCH (n:Seed) RETURN [(n)-[r:R]->(m) | size(keys(r)) + size(keys(m))] AS counts");
		ASSERT_NE(plan.root, nullptr);
		auto bounded = bounds();
		// Compare the same target-read stage: without the large Edge payload
		// target decoding must finish, while retaining it rejects before decode.
		// Later result-publication peaks are outside this admission assertion.
		bool targetAdmissionRejected = false;
		for (size_t allowance = 2300U; allowance < 10000U; allowance += 32U) {
			bounded.maximum_materialized_bytes = allowance;
			*controlFaults.node_shaped_completed_read_count = 0U;
			try {
				const QueryResult control = controlRuntime.executeToQueryResult(*plan.root, bounded);
				ASSERT_EQ(control.rowCount(), 1U);
				EXPECT_EQ(control.getRows().front().at("counts").asPrimitive(),
					PropertyValue(std::vector<PropertyValue>{PropertyValue(2)}));
			} catch (const std::runtime_error &) {}
			if (*controlFaults.node_shaped_completed_read_count == 0U) continue;
			*faults.edge_shaped_batch_read_count = 0U;
			*faults.node_shaped_admission_read_count = 0U;
			*faults.node_shaped_completed_read_count = 0U;
			try {
				static_cast<void>(runtime.executeToQueryResult(*plan.root, bounded));
				break;
			} catch (const std::runtime_error &error) {
				if (std::string_view(error.what()) == "shared materialization exceeded its byte bound" &&
					*faults.edge_shaped_batch_read_count > 0U &&
					*faults.node_shaped_admission_read_count > 0U &&
					*faults.node_shaped_completed_read_count == 0U) {
					targetAdmissionRejected = true;
					break;
				}
			}
		}
		EXPECT_TRUE(targetAdmissionRejected) << "no retained-Edge rejection with completed target decode in control";
		bounded.maximum_materialized_bytes = 10000U;
		const QueryResult result = runtime.executeToQueryResult(*plan.root, bounded);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_GT(*faults.node_shaped_completed_read_count, 0U);
		EXPECT_EQ(result.getRows().front().at("counts").asPrimitive(),
				  PropertyValue(std::vector<PropertyValue>{PropertyValue(2)}));
	}
}
TEST(LogicalPlanPatternExpressionRuntimeTest, RetainsPatternComprehensionValuesAcrossAdjacencyPages) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Seed"}, {}});
	const std::string payload(1000U, 'v');
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Target"}, {{"payload", PropertyValue(payload)}}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Target"}, {{"payload", PropertyValue(payload)}}});
	graph.edges.emplace(10U, LogicalGraphEdge{10U, 1U, 2U, "R"});
	graph.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 3U, "R"});
	PatternExpressionGraph controlGraph = graph;
	controlGraph.edges.erase(11U);
	PatternExpressionBackend controlBackend(std::move(controlGraph));
	LogicalPlanReadRuntime controlRuntime(controlBackend);
	PatternExpressionSnapshotFaults faults;
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	const QueryPlan plan = parse("MATCH (n:Seed) RETURN [(n)-[:R]->(m) | m.payload] AS values");
	ASSERT_NE(plan.root, nullptr);
	auto limited = bounds();
	limited.maximum_adjacency_page_records = 1U;
	const PropertyValue expected(std::vector<PropertyValue>{PropertyValue(payload), PropertyValue(payload)});
	bool secondPageRejected = false;
	for (size_t allowance = 4500U; allowance < 12000U; allowance += 64U) {
		limited.maximum_materialized_bytes = allowance;
		try {
			const QueryResult control = controlRuntime.executeToQueryResult(*plan.root, limited);
			ASSERT_EQ(control.rowCount(), 1U);
			EXPECT_EQ(control.getRows().front().at("values").asPrimitive(),
				PropertyValue(std::vector<PropertyValue>{PropertyValue(payload)}));
		} catch (const std::runtime_error &) { continue; }
		*faults.adjacency_topology_scan_count = 0U;
		try {
			static_cast<void>(runtime.executeToQueryResult(*plan.root, limited));
			break;
		} catch (const std::runtime_error &error) {
			if (std::string_view(error.what()) == "shared materialization exceeded its byte bound" &&
				*faults.adjacency_topology_scan_count >= 2U) {
				ASSERT_LT(property_utils::getPropertyValueSize(expected), allowance);
				secondPageRejected = true;
				break;
			}
		}
	}
	EXPECT_TRUE(secondPageRejected) << "no second-page rejection with successful single-page control";
	limited.maximum_materialized_bytes = 12000U;
	const QueryResult result = runtime.executeToQueryResult(*plan.root, limited);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("values").asPrimitive(), expected);
}
