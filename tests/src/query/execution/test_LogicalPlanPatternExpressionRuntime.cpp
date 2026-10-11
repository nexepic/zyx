#include "query/execution/LogicalPlanPatternExpressionRuntimeTestFixture.hpp"

TEST(LogicalPlanPatternExpressionRuntimeTest, ExecutesExistsWithTypedTargetAndScopedWherePredicate) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) WHERE EXISTS((n)-[:KNOWS]->(m:Friend) WHERE m.age > 21) RETURN n.name AS name");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("name").asPrimitive(), PropertyValue("Ada"));
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ExcludesExistingRelationshipsThatFailTheExistsWherePredicate) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse(
			"MATCH (n:Person) WHERE EXISTS((n)-[:KNOWS]->(m:Friend) WHERE m.age > 30) RETURN n.name AS name");
	ASSERT_NE(plan.root, nullptr);

	EXPECT_EQ(runtime.executeToQueryResult(*plan.root, bounds()).rowCount(), 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, UsesNativeAdjacencyReferencesForTypedExistsPatterns) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Person"}, {{"name", PropertyValue("Ada")}}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Friend"}, {{"age", PropertyValue(30)}}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.adjacency_reference_scan_count = std::make_shared<size_t>(0U);
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
		faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) WHERE EXISTS((n)-[:R]->(m:Friend) WHERE m.age > 21) RETURN n.name AS name");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("name").asPrimitive(), PropertyValue("Ada"));
	EXPECT_GT(*faults.adjacency_reference_scan_count, 0U);
	EXPECT_EQ(*faults.adjacency_topology_scan_count, 0U);
		EXPECT_EQ(*faults.node_existence_probe_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, NativeReferenceExistsStopsBeforeMaterializingLaterMatches) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Person"}, {{"name", PropertyValue("Ada")}}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Friend"}, {{"age", PropertyValue(30)}}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Friend"}, {{"age", PropertyValue(40)}}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.adjacency_reference_scan_count = std::make_shared<size_t>(0U);
	faults.edge_read_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse(
			"MATCH (n:Person) WHERE EXISTS((n)-[:R]->(m:Friend) WHERE m.age > 21) RETURN n.name AS name");
	ASSERT_NE(plan.root, nullptr);
	LogicalPlanReadRuntimeBounds queryBounds = bounds();
	queryBounds.maximum_adjacency_page_records = 2U;

	const QueryResult result = runtime.executeToQueryResult(*plan.root, queryBounds);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("name").asPrimitive(), PropertyValue("Ada"));
	EXPECT_EQ(*faults.adjacency_reference_scan_count, 1U);
	// EXISTS can stop after the first matching candidate; later full Edge payloads
	// must not be prefetched merely because they share one topology page.
	EXPECT_EQ(*faults.edge_read_count, 1U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, BatchesEndpointExistenceForTopologyOnlyExistsPatterns) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Person"}, {{"name", PropertyValue("Ada")}}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Friend"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Other"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 3U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 2U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.supports_node_existence_probes = true;
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	PatternExpressionSnapshot snapshot(std::move(graph), faults);
	LogicalPlanReadRow row;
	row.values.emplace("n", EntityRef{1U, false, 0U, 0U, 0U});
	graph::query::expressions::ExistsExpression expression(
			"(n)-[:R]->()", "n", "R", "", graph::query::expressions::PatternDirection::PAT_OUTGOING);
	QueryContext context;
	LogicalPlanReadRuntimeBounds batchBounds = bounds();
	batchBounds.maximum_adjacency_page_records = 2U;
	const LogicalPlanReadValue result = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, expression, row, batchBounds, context);
	ASSERT_TRUE(std::holds_alternative<PropertyValue>(result));
	EXPECT_EQ(std::get<PropertyValue>(result), PropertyValue(true));
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	EXPECT_GT(*faults.node_existence_batch_probe_count, 0U);
}
TEST(LogicalPlanPatternExpressionRuntimeTest,
		GenericTraversalPrefiltersRelationshipTypeBeforeCompleteEdgeRead) {
	PatternExpressionGraph graph = makeDefaultPatternExpressionGraph();
	graph.edges.at(10U).properties.push_back({"weight", PropertyValue(int64_t{7})});
	graph.edges.at(11U).properties.push_back({"weight", PropertyValue(int64_t{7})});
	PatternExpressionSnapshotFaults faults;
	faults.edge_shaped_batch_read_count = std::make_shared<size_t>(0U);
	faults.edge_shaped_batch_ids = std::make_shared<std::vector<uint64_t>>();
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	// The projection deliberately prevents the direct-traversal shortcut, so this
	// exercises the ordinary adjacency-page consumer rather than a query-specific
	// global/reference fast path.
	auto source = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "n");
	auto projectedSource = std::make_unique<logical::LogicalProject>(std::move(source), std::move(items));
	logical::LogicalTraversal traversal(
			std::move(projectedSource), "n", "r", "m", "KNOWS", "out", {}, {},
			{{"weight", PropertyValue(int64_t{7})}});
	std::vector<uint64_t> edgeIds;
	LogicalPlanReadRuntimeBounds traversalBounds = bounds();
	traversalBounds.maximum_adjacency_page_records = 2U;
	static_cast<void>(runtime.execute(traversal, traversalBounds, [&edgeIds](const LogicalPlanReadRow &row) {
		edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
		return true;
	}));
	EXPECT_EQ(edgeIds, std::vector<uint64_t>{10U});
	ASSERT_EQ(*faults.edge_shaped_batch_read_count, 1U);
	EXPECT_EQ(*faults.edge_shaped_batch_ids, std::vector<uint64_t>{10U});
}
TEST(LogicalPlanPatternExpressionRuntimeTest, DoesNotMaterializeTopologyEdgesRejectedByRelationshipType) {
	PatternExpressionGraph graph = makeDefaultPatternExpressionGraph();
	PatternExpressionSnapshotFaults faults;
	faults.edge_shaped_batch_read_count = std::make_shared<size_t>(0U);
	faults.edge_shaped_batch_ids = std::make_shared<std::vector<uint64_t>>();
	PatternExpressionSnapshot snapshot(std::move(graph), faults);
	LogicalPlanReadRow row;
	row.values.emplace("n", EntityRef{1U, false, 0U, 0U, 0U});
	graph::query::expressions::ExistsExpression expression(
			"(n)-[:KNOWS]->(m)", "n", "KNOWS", "", graph::query::expressions::PatternDirection::PAT_OUTGOING);
	QueryContext context;
	LogicalPlanReadRuntimeBounds expressionBounds = bounds();
	expressionBounds.maximum_adjacency_page_records = 2U;
	const LogicalPlanReadValue result = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, expression, row, expressionBounds, context);
	ASSERT_TRUE(std::holds_alternative<PropertyValue>(result));
	EXPECT_EQ(std::get<PropertyValue>(result), PropertyValue(true));
	ASSERT_EQ(*faults.edge_shaped_batch_read_count, 1U);
	ASSERT_EQ(*faults.edge_shaped_batch_ids, std::vector<uint64_t>({10U}));
	*faults.edge_shaped_batch_read_count = 0U;
	graph::query::expressions::ExistsExpression rejected(
			"(n)-[:KNOWS]->(m:Robot)", "n", "KNOWS", "Robot", graph::query::expressions::PatternDirection::PAT_OUTGOING);
	const auto absent = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, rejected, row, expressionBounds, context);
	EXPECT_EQ(std::get<PropertyValue>(absent), PropertyValue(false));
	EXPECT_EQ(*faults.edge_shaped_batch_read_count, 0U);
}
TEST(LogicalPlanPatternExpressionRuntimeTest, EvaluatesLengthForANamedSingleRelationshipPath) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse(
			"MATCH p=(a:Person)-[:KNOWS]->(b:Friend) "
			"RETURN length(p) AS relationship_count, size(nodes(p)) AS node_count, "
			"size(relationships(p)) AS relationship_list_count");
	ASSERT_NE(plan.root, nullptr);
	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("relationship_count").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(row.at("node_count").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(row.at("relationship_list_count").asPrimitive(), PropertyValue(1));
}
TEST(LogicalPlanPatternExpressionRuntimeTest, ResolvesParameterizedNodePropertyPredicatesAtExecutionTime) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n", {"Person"});
	scan.setParameterPropertyPredicates({{"name", "name"}});
	QueryContext context;
	context.parameters.emplace("name", PropertyValue("Ada"));
	const QueryResult matching = runtime.executeToQueryResult(scan, bounds(), context);
	ASSERT_EQ(matching.rowCount(), 1U);
	EXPECT_EQ(matching.getRows().front().at("n").asLogicalNode().node_id, 1U);
	context.parameters.insert_or_assign("name", PropertyValue());
	const QueryResult nullPredicate = runtime.executeToQueryResult(scan, bounds(), context);
	EXPECT_EQ(nullPredicate.rowCount(), 0U);
	QueryContext missingParameter;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(scan, bounds(), missingParameter)), std::runtime_error);
}
TEST(LogicalPlanPatternExpressionRuntimeTest, EvaluatesEntityIntrospectionFunctionsFromParserProducedPlans) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse(
			"MATCH (n:Person)-[r:KNOWS]->(m:Friend) "
			"RETURN id(n) AS node_id, labels(n) AS node_labels, id(r) AS edge_id, type(r) AS edge_type, "
			"keys(n) AS node_keys, properties(n) AS node_properties, keys(r) AS edge_keys, properties(r) AS edge_properties");
	ASSERT_NE(plan.root, nullptr);
	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &row = result.getRows().front();
	EXPECT_EQ(row.at("node_id").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(row.at("node_labels").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("Person")}));
	EXPECT_EQ(row.at("edge_id").asPrimitive(), PropertyValue(10));
	EXPECT_EQ(row.at("edge_type").asPrimitive(), PropertyValue("KNOWS"));
	EXPECT_EQ(row.at("node_keys").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue("name")}));
	EXPECT_EQ(row.at("node_properties").asPrimitive(),
			  PropertyValue(PropertyValue::MapType{{"name", PropertyValue("Ada")}}));
	EXPECT_EQ(row.at("edge_keys").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
	EXPECT_EQ(row.at("edge_properties").asPrimitive(), PropertyValue(PropertyValue::MapType{}));
}
TEST(LogicalPlanPatternExpressionRuntimeTest, EvaluatesMapProjectionThroughTheLogicalStorageBoundary) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse(
			"MATCH (n:Person) WHERE n.name = 'Ada' "
			"RETURN n {.*, .name, missing: null, greeting: 'hello'} AS projection");
	ASSERT_NE(plan.root, nullptr);
	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const PropertyValue projection = result.getRows().front().at("projection").asPrimitive();
	ASSERT_EQ(projection.getType(), PropertyType::MAP);
	EXPECT_EQ(projection.getMap().at("name"), PropertyValue("Ada"));
	EXPECT_EQ(projection.getMap().at("missing"), PropertyValue());
	EXPECT_EQ(projection.getMap().at("greeting"), PropertyValue("hello"));
}
TEST(LogicalPlanPatternExpressionRuntimeTest, MapProjectionAdmitsCopiesAndBoundsExpandedProperties) {
	PatternExpressionSnapshot snapshot(makeDefaultPatternExpressionGraph());
	LogicalPlanReadRow row;
	QueryContext context;
	const std::string payload(1000U, 'm');
	context.parameters.emplace("first", PropertyValue(payload));
	context.parameters.emplace("second", PropertyValue(payload));
	std::vector<expressions::MapProjectionItem> items;
	items.emplace_back(expressions::MapProjectionItemType::MPROP_LITERAL, "a",
		std::make_unique<expressions::ParameterExpression>("first"));
	items.emplace_back(expressions::MapProjectionItemType::MPROP_LITERAL, "b",
		std::make_unique<expressions::ParameterExpression>("second"));
	expressions::MapProjectionExpression literal(std::move(items));
	const PropertyValue expected(PropertyValue::MapType{{"a", PropertyValue(payload)}, {"b", PropertyValue(payload)}});
	auto limited = bounds();
	limited.maximum_materialized_bytes = 2200U;
	ASSERT_LT(property_utils::getPropertyValueSize(expected), limited.maximum_materialized_bytes);
	EXPECT_THROW(static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, literal, row, limited, context)), std::runtime_error);
	limited.maximum_materialized_bytes = 6000U;
	EXPECT_EQ(std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, literal, row, limited, context)), expected);
	std::vector<expressions::MapProjectionItem> overriding;
	overriding.emplace_back(expressions::MapProjectionItemType::MPROP_LITERAL, "a",
		std::make_unique<expressions::ParameterExpression>("first"));
	overriding.emplace_back(expressions::MapProjectionItemType::MPROP_LITERAL, "a",
		std::make_unique<expressions::LiteralExpression>(std::string("short")));
	expressions::MapProjectionExpression overrideLiteral(std::move(overriding));
	limited.maximum_materialized_bytes = 2500U;
	EXPECT_EQ(std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, overrideLiteral, row, limited, context)),
		PropertyValue(PropertyValue::MapType{{"a", PropertyValue("short")}}));
	row.values.emplace("m", PropertyValue(PropertyValue::MapType{
		{"a", PropertyValue(1)}, {"b", PropertyValue(2)}, {"c", PropertyValue(3)}}));
	std::vector<expressions::MapProjectionItem> selectors;
	selectors.emplace_back(expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES, "");
	expressions::MapProjectionExpression all("m", std::move(selectors));
	limited.maximum_materialized_bytes = 6000U;
	limited.maximum_property_entries = 2U;
	EXPECT_THROW(static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, all, row, limited, context)), std::runtime_error);
	limited.maximum_property_entries = 4U;
	EXPECT_EQ(std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, all, row, limited, context)), std::get<PropertyValue>(row.values.at("m")));
	EXPECT_TRUE(row.property_cache.empty());
	expressions::VariableReferenceExpression property("m", "a");
	EXPECT_EQ(std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
		snapshot, property, row, limited, context)), PropertyValue(1));
	EXPECT_TRUE(row.property_cache.empty());
}
TEST(LogicalPlanPatternExpressionRuntimeTest, ProducesCompletePatternComprehensionAndPreservesOuterBindings) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) RETURN n.name AS name, [(n)-[:KNOWS]->(m) WHERE m.age > 21 | m.age] AS ages");
	ASSERT_NE(plan.root, nullptr);
	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows().front().at("name").asPrimitive(), PropertyValue("Ada"));
	EXPECT_EQ(result.getRows().front().at("ages").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(30)}));
	EXPECT_EQ(result.getRows().back().at("ages").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
}

TEST(LogicalPlanPatternExpressionRuntimeTest, AdmitsCompleteAdjacencyTargetColumnsBeforeReadingPayloads) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Seed"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Target"}, {{"payload", PropertyValue(std::string(4096U, 'p'))}}});
	graph.edges.emplace(10U, LogicalGraphEdge{10U, 1U, 2U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_node_ref_scans = true;
	faults.node_property_column_admission_count = std::make_shared<size_t>(0U);
	faults.node_property_column_read_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("n"), "n", "r", "m", "R", "out", {},
										{{"payload", PropertyValue(std::string(4096U, 'p'))}});
	auto limited = bounds();
	limited.maximum_materialized_bytes = 1024U;
	EXPECT_THROW(
			static_cast<void>(runtime.execute(traversal, limited, [](const LogicalPlanReadRow &) { return true; })),
			std::runtime_error);
	EXPECT_EQ(*faults.node_property_column_admission_count, 1U);
	EXPECT_EQ(*faults.node_property_column_read_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, BoundsPatternComprehensionContainerOverhead) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) RETURN [(n)-[:KNOWS]->(m) | m.age] AS ages");
	ASSERT_NE(plan.root, nullptr);
	LogicalPlanReadRuntimeBounds limited = bounds();
	// The mapped integer fits by itself, but the returned list also needs its
	// collection envelope and must be rejected under the tighter bound.
	limited.maximum_materialized_bytes = 8U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limited)), std::runtime_error);
}


TEST(LogicalPlanPatternExpressionRuntimeTest, BatchesEndpointExistenceForCompleteTopologyPages) {
	PatternExpressionSnapshotFaults faults;
	faults.supports_node_existence_probes = true;
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	faults.edge_read_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(makeDefaultPatternExpressionGraph(), faults);
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) RETURN [(n)-[:KNOWS]->(m) | m.age] AS ages");
	ASSERT_NE(plan.root, nullptr);

	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows().front().at("ages").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(30)}));
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	EXPECT_GT(*faults.node_existence_batch_probe_count, 0U);
	for (bool references: {false, true}) {
	faults.supports_native_adjacency_refs = references;
	faults.adjacency_reference_scan_count = std::make_shared<size_t>(0U);
	const auto brokenPlan = parse("MATCH (n:Person) RETURN [(n)-->(m) | m.age] AS ages");
	PatternExpressionGraph broken = makeDefaultPatternExpressionGraph();
	broken.nodes.erase(2U);
	*faults.edge_read_count = 0U;
	PatternExpressionBackend brokenBackend(std::move(broken), faults);
	LogicalPlanReadRuntime brokenRuntime(brokenBackend);
	EXPECT_THROW(static_cast<void>(brokenRuntime.executeToQueryResult(*brokenPlan.root, bounds())), std::runtime_error);
	EXPECT_EQ(*faults.edge_read_count, 0U);
	if (references) EXPECT_GT(*faults.adjacency_reference_scan_count, 0U);
	}
}

TEST(LogicalPlanPatternExpressionRuntimeTest,
	 BindsTheRelationshipWhenAPatternComprehensionUsesAnAnonymousTarget) {
	PatternExpressionGraph graph = makeDefaultPatternExpressionGraph();
	graph.edges.at(10U).properties.push_back({"weight", PropertyValue(int64_t{7})});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse(
			"MATCH (n:Person) RETURN n.name AS name, [(n)-[r:KNOWS]->() | r.weight] AS weights");
	ASSERT_NE(plan.root, nullptr);
	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows().front().at("name").asPrimitive(), PropertyValue("Ada"));
	EXPECT_EQ(result.getRows().front().at("weights").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t{7})}));
	EXPECT_EQ(result.getRows().back().at("weights").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
}
TEST(LogicalPlanPatternExpressionRuntimeTest,
	 PreservesUndirectedPatternSemanticsAcrossSelfLoopsTypesAndTargetLabels) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Seed"}, {}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Friend"}, {{"age", PropertyValue(30)}}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Other"}, {}});
	// A correct undirected traversal observes the self-loop in both directions,
	// but returns it only once. The other two edges exercise type and label
	// filtering without requiring a larger graph fixture.
	graph.edges.emplace(10U, LogicalGraphEdge{10U, 1U, 1U, "KNOWS"});
	graph.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, "KNOWS"});
	graph.edges.emplace(12U, LogicalGraphEdge{12U, 3U, 1U, "OTHER"});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Seed) RETURN [(n)-[:KNOWS]-(m:Friend) | m.age] AS ages");
	ASSERT_NE(plan.root, nullptr);
	const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("ages").asPrimitive(),
			  PropertyValue(std::vector<PropertyValue>{PropertyValue(30)}));
}
TEST(LogicalPlanPatternExpressionRuntimeTest, DoesNotRebindPreexistingPatternTargets) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan matchingExistsPlan = parse(
			"MATCH (n:Person), (m:Friend) WHERE EXISTS((n)-[:KNOWS]->(m)) RETURN n.name AS name");
	ASSERT_NE(matchingExistsPlan.root, nullptr);
	const QueryResult matching = runtime.executeToQueryResult(*matchingExistsPlan.root, bounds());
	ASSERT_EQ(matching.rowCount(), 1U);
	EXPECT_EQ(matching.getRows().front().at("name").asPrimitive(), PropertyValue("Ada"));
	QueryPlan existsPlan = parse(
			"MATCH (n:Person), (m:Robot) WHERE EXISTS((n)-[:KNOWS]->(m)) RETURN n.name AS name");
	ASSERT_NE(existsPlan.root, nullptr);
	EXPECT_EQ(runtime.executeToQueryResult(*existsPlan.root, bounds()).rowCount(), 0U);
	QueryPlan comprehensionPlan = parse(
			"MATCH (n:Person), (m:Robot) RETURN n.name AS name, [(n)-[:KNOWS]->(m) | m.age] AS ages");
	ASSERT_NE(comprehensionPlan.root, nullptr);
	const QueryResult result = runtime.executeToQueryResult(*comprehensionPlan.root, bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	for (const auto &row: result.getRows()) {
		EXPECT_EQ(row.at("ages").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
	}
}
TEST(LogicalPlanPatternExpressionRuntimeTest, EvaluatesStructuredNodeOnlyPatternsAndMissingSourceBindings) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);

	std::vector<logical::LogicalProjectItem> missingExistsItems;
	missingExistsItems.emplace_back(std::make_shared<expressions::ExistsExpression>(
														"(n)", "n", "", "", expressions::PatternDirection::PAT_OUTGOING),
														"exists");
	logical::LogicalProject missingExists(std::make_unique<logical::LogicalSingleRow>(), std::move(missingExistsItems));
	const QueryResult missingExistsResult = runtime.executeToQueryResult(missingExists, bounds());
	ASSERT_EQ(missingExistsResult.rowCount(), 1U);
	EXPECT_EQ(missingExistsResult.getRows().front().at("exists").asPrimitive(), PropertyValue(false));

	std::vector<logical::LogicalProjectItem> unstructuredExistsItems;
	unstructuredExistsItems.emplace_back(std::make_shared<expressions::ExistsExpression>("(n)"), "exists");
	logical::LogicalProject unstructuredExists(std::make_unique<logical::LogicalSingleRow>(),
				std::move(unstructuredExistsItems));
	const QueryResult unstructuredExistsResult = runtime.executeToQueryResult(unstructuredExists, bounds());
	ASSERT_EQ(unstructuredExistsResult.rowCount(), 1U);
	EXPECT_EQ(unstructuredExistsResult.getRows().front().at("exists").asPrimitive(), PropertyValue(false));

	std::vector<logical::LogicalProjectItem> nodeOnlyExistsItems;
	nodeOnlyExistsItems.emplace_back(std::make_shared<expressions::ExistsExpression>(
														"(n)", "n", "", "", expressions::PatternDirection::PAT_OUTGOING),
														"exists");
	logical::LogicalProject nodeOnlyExists(
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}),
				std::move(nodeOnlyExistsItems));
	const QueryResult nodeOnlyExistsResult = runtime.executeToQueryResult(nodeOnlyExists, bounds());
	ASSERT_EQ(nodeOnlyExistsResult.rowCount(), 2U);
	for (const auto &row: nodeOnlyExistsResult.getRows()) {
		EXPECT_EQ(row.at("exists").asPrimitive(), PropertyValue(true));
	}

	auto mapValue = std::make_unique<expressions::LiteralExpression>(std::string("mapped"));
	std::vector<logical::LogicalProjectItem> missingComprehensionItems;
	missingComprehensionItems.emplace_back(std::make_shared<expressions::PatternComprehensionExpression>(
														"(n)", "n", "target", "", "", expressions::PatternDirection::PAT_OUTGOING,
														std::move(mapValue)),
														"values");
	logical::LogicalProject missingComprehension(std::make_unique<logical::LogicalSingleRow>(),
				std::move(missingComprehensionItems));
	const QueryResult missingComprehensionResult = runtime.executeToQueryResult(missingComprehension, bounds());
	ASSERT_EQ(missingComprehensionResult.rowCount(), 1U);
	EXPECT_EQ(missingComprehensionResult.getRows().front().at("values").asPrimitive(),
				  PropertyValue(std::vector<PropertyValue>{}));

	std::vector<logical::LogicalProjectItem> unstructuredComprehensionItems;
	unstructuredComprehensionItems.emplace_back(std::make_shared<expressions::PatternComprehensionExpression>(
														"(n)", "target",
														std::make_unique<expressions::LiteralExpression>(std::string("mapped"))),
														"values");
	logical::LogicalProject unstructuredComprehension(std::make_unique<logical::LogicalSingleRow>(),
				std::move(unstructuredComprehensionItems));
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(unstructuredComprehension, bounds())), std::logic_error);

	std::vector<logical::LogicalProjectItem> nodeOnlyComprehensionItems;
	nodeOnlyComprehensionItems.emplace_back(std::make_shared<expressions::PatternComprehensionExpression>(
														"(n)", "n", "target", "", "", expressions::PatternDirection::PAT_OUTGOING,
														std::make_unique<expressions::LiteralExpression>(std::string("mapped"))),
														"values");
	logical::LogicalProject nodeOnlyComprehension(
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}),
				std::move(nodeOnlyComprehensionItems));
	const QueryResult nodeOnlyComprehensionResult = runtime.executeToQueryResult(nodeOnlyComprehension, bounds());
	ASSERT_EQ(nodeOnlyComprehensionResult.rowCount(), 2U);
	for (const auto &row: nodeOnlyComprehensionResult.getRows()) {
		const PropertyValue values = row.at("values").asPrimitive();
		ASSERT_EQ(values.getType(), PropertyType::LIST);
		ASSERT_EQ(values.getList().size(), 1U);
		EXPECT_EQ(values.getList().front(), PropertyValue("mapped"));
	}
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsTruncatedPatternComprehensionInsteadOfReturningAPrefix) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) RETURN [(n)-->(m) | m] AS targets");
	ASSERT_NE(plan.root, nullptr);
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_edges_per_node = 1U;
	limited.maximum_adjacency_page_records = 1U;

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limited)), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, MaterializesBoundedShortestPathAndAllShortestPaths) {
	PatternExpressionBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const auto makeRoot = [](bool all) {
		auto left = std::make_unique<graph::query::logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"name", PropertyValue("Ada")}});
		auto right = std::make_unique<graph::query::logical::LogicalNodeScan>("b", std::vector<std::string>{"Friend"});
		auto join = std::make_unique<graph::query::logical::LogicalJoin>(std::move(left), std::move(right));
		std::vector<graph::query::logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<graph::query::expressions::ShortestPathExpression>(
						   "a", "b", "KNOWS", graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 3, all),
						   "path");
		return graph::query::logical::LogicalProject(std::move(join), std::move(items));
	};

	auto shortest = makeRoot(false);
	const QueryResult result = runtime.executeToQueryResult(shortest, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &path = result.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(path.size(), 3U);
	EXPECT_EQ(std::get<std::string>(path[0].getMap().at("_type").getVariant()), "node");
	EXPECT_EQ(std::get<int64_t>(path[1].getMap().at("_id").getVariant()), 10);

	auto allShortest = makeRoot(true);
	const QueryResult allResult = runtime.executeToQueryResult(allShortest, bounds());
	const auto &paths = allResult.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(paths.size(), 1U);
	EXPECT_EQ(paths.front().getType(), PropertyType::LIST);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathCompleteEdgeAdmissionRejectsBeforeBatchCopy) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}, {}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}, {}});
	const std::string payload(4000U, 'e');
	graph.edges.emplace(10U, LogicalGraphEdge{10U, 1U, 2U, "R", {{"payload", PropertyValue(payload)}}});
	PatternExpressionSnapshotFaults faults;
	faults.supports_shaped_property_admission = true;
	faults.edge_shaped_admission_read_count = std::make_shared<size_t>(0U);
	faults.edge_shaped_batch_read_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
		graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 2, false);
	auto limited = bounds();
	limited.maximum_materialized_bytes = 3500U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*root, limited)), std::runtime_error);
	EXPECT_GT(*faults.edge_shaped_admission_read_count, 0U);
	EXPECT_EQ(*faults.edge_shaped_batch_read_count, 0U);
	limited.maximum_materialized_bytes = 32000U;
	const auto result = runtime.executeToQueryResult(*root, limited);
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &path = result.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(path.size(), 3U);
	EXPECT_EQ(path[1].getMap().at("payload"), PropertyValue(payload));
	EXPECT_GT(*faults.edge_shaped_batch_read_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathRetainsNodesWhileAdmittingEdgeBatch) {
	PatternExpressionGraph graph;
	const std::string payload(1200U, 'p');
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}, {{"payload", PropertyValue(payload)}}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}, {{"payload", PropertyValue(payload)}}});
	graph.edges.emplace(10U, LogicalGraphEdge{10U, 1U, 2U, "R", {{"payload", PropertyValue(payload)}}});
	PatternExpressionSnapshotFaults faults;
	faults.supports_shaped_property_admission = true;
	faults.node_shaped_completed_read_count = std::make_shared<size_t>(0U);
	faults.edge_shaped_admission_read_count = std::make_shared<size_t>(0U);
	faults.edge_shaped_batch_read_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R", expressions::PatternDirection::PAT_OUTGOING, 1, 2, false);
	auto limited = bounds();
	// Identity authentication no longer retains complete BFS endpoint copies.
	// Keep the limit between the Node source and the combined Node/Edge sources.
	limited.maximum_materialized_bytes = 9000U;
	try {
		static_cast<void>(runtime.executeToQueryResult(*root, limited));
		FAIL() << "the retained Node and Edge source batches must share the budget";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "shared materialization exceeded its byte bound");
	}
	EXPECT_GT(*faults.node_shaped_completed_read_count, 0U);
	EXPECT_GT(*faults.edge_shaped_admission_read_count, 0U);
	EXPECT_EQ(*faults.edge_shaped_batch_read_count, 0U);
	limited.maximum_materialized_bytes = 16000U;
	const auto result = runtime.executeToQueryResult(*root, limited);
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &path = result.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(path.size(), 3U);
	for (const PropertyValue &entity: path)
		EXPECT_EQ(entity.getMap().at("payload"), PropertyValue(payload));
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathRejectsDuplicateDirectProperties) {
	for (const bool duplicateEdge: {false, true}) {
		auto graph = makeDefaultPatternExpressionGraph();
		if (duplicateEdge) {
			graph.edges.at(10U).properties = {{"weight", PropertyValue(1)}, {"weight", PropertyValue(2)}};
		} else {
			graph.nodes.at(1U).properties.push_back({"name", PropertyValue("duplicate")});
		}
		PatternExpressionSnapshot snapshot(std::move(graph));
		LogicalPlanReadRow row;
		row.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
		row.values.emplace("b", EntityRef{2U, false, 0U, 0U, 0U});
		expressions::ShortestPathExpression shortest("a", "b", "KNOWS",
			expressions::PatternDirection::PAT_OUTGOING, 1, 2, false);
		QueryContext context;
		try {
			static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(snapshot, shortest, row, bounds(), context));
			FAIL() << "a duplicate direct Property key must be rejected while constructing the path";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical graph backend returned an invalid direct Property entry set");
		}
	}
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathChargesAdjacencyTopologyPageBeforeRetainingParent) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}, {}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}, {}});
	graph.edges.emplace(10U, LogicalGraphEdge{10U, 1U, 2U, std::string(1024U, 'R'), {}});
	PatternExpressionSnapshotFaults faults;
	faults.supports_node_existence_probes = true;
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	faults.edge_shaped_batch_read_count = std::make_shared<size_t>(0U);
	PatternExpressionSnapshot snapshot(std::move(graph), faults);
	LogicalPlanReadRow row;
	row.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
	row.values.emplace("b", EntityRef{2U, false, 0U, 0U, 0U});
	expressions::ShortestPathExpression shortest("a", "b", "",
		expressions::PatternDirection::PAT_OUTGOING, 1, 1, false);
	QueryContext context;
	auto limited = bounds();
	limited.maximum_materialized_bytes = 900U;
	try {
		static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(snapshot, shortest, row, limited, context));
		FAIL() << "the adjacency topology page must fit the shared budget before BFS retains a parent";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "shared materialization exceeded its byte bound");
	}
	EXPECT_GT(*faults.adjacency_topology_scan_count, 0U);
	EXPECT_EQ(*faults.edge_shaped_batch_read_count, 0U);
	limited.maximum_materialized_bytes = 20000U;
	const auto result = std::get<PropertyValue>(
		LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(snapshot, shortest, row, limited, context));
	ASSERT_EQ(result.getList().size(), 3U);
	EXPECT_EQ(result.getList()[1].getMap().at("_id"), PropertyValue(10));
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathUsesNativeEndpointProbesBeforePathMaterialization) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Detour"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_node_existence_probes = true;
	faults.node_read_count = std::make_shared<size_t>(0U);
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 1, false);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(entityId(result.getRows().front().at("path").asPrimitive().getList()[1]), 1);
	// Node-scan bindings are already authenticated; the topology-only BFS does
	// not need a second point probe before materializing the returned path.
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	// The start and end records are materialized only while building the returned
	// path. The non-path frontier Node is never fully decoded.
	EXPECT_EQ(*faults.node_read_count, 2U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ChargesShortestPathEndpointProbeBuffersToPageBudget) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Detour"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_node_existence_probes = true;
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	const auto makeRoot = [] {
		return makeShortestPathRoot("Start", "End", "R",
				graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 1, false);
	};
	auto limited = bounds();
	limited.maximum_materialized_bytes = 128U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*makeRoot(), limited)), std::runtime_error);
	EXPECT_EQ(*faults.node_existence_batch_probe_count, 0U);

	limited.maximum_materialized_bytes = 20000U;
	const QueryResult result = runtime.executeToQueryResult(*makeRoot(), limited);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(entityId(result.getRows().front().at("path").asPrimitive().getList()[1]), 1);
	EXPECT_GT(*faults.node_existence_batch_probe_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathSeedsBfsFromEntityIdsWithoutReadingEndpoints) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}, {{"payload", PropertyValue("large")}}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}, {{"payload", PropertyValue("large")}}});
	PatternExpressionSnapshotFaults faults;
	faults.node_read_count = std::make_shared<size_t>(0U);
	faults.supports_node_existence_probes = true;
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	PatternExpressionSnapshot snapshot(std::move(graph), faults);
	LogicalPlanReadRow row;
	row.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
	row.values.emplace("b", EntityRef{2U, false, 0U, 0U, 0U});
	graph::query::expressions::ShortestPathExpression expression(
		"a", "b", "R", graph::query::expressions::PatternDirection::PAT_OUTGOING, 0, 0, false);
	graph::query::QueryContext context;
	const LogicalPlanReadValue result = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, expression, row, bounds(), context);
	ASSERT_TRUE(std::holds_alternative<PropertyValue>(result));
	EXPECT_EQ(std::get<PropertyValue>(result).getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(*faults.node_read_count, 0U);
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	EXPECT_EQ(*faults.node_existence_batch_probe_count, 1U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathUsesTypedNativeAdjacencyReferences) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	faults.adjacency_reference_scan_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 1, false);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(entityId(result.getRows().front().at("path").asPrimitive().getList()[1]), 1);
	EXPECT_EQ(*faults.adjacency_reference_scan_count, 1U);
	EXPECT_EQ(*faults.adjacency_topology_scan_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathUsesUntypedNativeAdjacencyReferences) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "OTHER"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	faults.adjacency_reference_scan_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 1, false);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(entityId(result.getRows().front().at("path").asPrimitive().getList()[1]), 1);
	EXPECT_EQ(*faults.adjacency_reference_scan_count, 1U);
	EXPECT_EQ(*faults.adjacency_topology_scan_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, AnchoredRelationshipCountUsesTypedNativeAdjacencyReferences) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Other"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "OTHER"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.supports_node_existence_probes = true;
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	faults.adjacency_reference_scan_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto source = std::make_unique<graph::query::logical::LogicalNodeScan>("a", std::vector<std::string>{"Start"});
	auto traversal = std::make_unique<graph::query::logical::LogicalTraversal>(
			std::move(source), "a", "r", "b", "R", "out");
	graph::query::logical::LogicalAggregate aggregate(
			std::move(traversal), {},
			{{"count", std::make_shared<graph::query::expressions::VariableReferenceExpression>("b"), "count", false}});

	std::vector<PropertyValue> results;
	static_cast<void>(runtime.execute(aggregate, bounds(), [&results](const LogicalPlanReadRow &row) {
		results.push_back(std::get<PropertyValue>(row.values.at("count")));
		return true;
	}));

	EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{1})});
	EXPECT_GT(*faults.adjacency_reference_scan_count, 0U);
	EXPECT_EQ(*faults.adjacency_topology_scan_count, 0U);
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	EXPECT_GT(*faults.node_existence_batch_probe_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, AnchoredRelationshipCountUsesUntypedNativeAdjacencyReferences) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Other"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "OTHER"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true;
	faults.supports_node_existence_probes = true;
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	faults.adjacency_reference_scan_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto source = std::make_unique<graph::query::logical::LogicalNodeScan>("a", std::vector<std::string>{"Start"});
	auto traversal = std::make_unique<graph::query::logical::LogicalTraversal>(
			std::move(source), "a", "r", "b", "", "out");
	graph::query::logical::LogicalAggregate aggregate(
			std::move(traversal), {},
			{{"count", std::make_shared<graph::query::expressions::VariableReferenceExpression>("b"), "count", false}});

	std::vector<PropertyValue> results;
	static_cast<void>(runtime.execute(aggregate, bounds(), [&results](const LogicalPlanReadRow &row) {
		results.push_back(std::get<PropertyValue>(row.values.at("count")));
		return true;
	}));

	EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{2})});
	EXPECT_GT(*faults.adjacency_reference_scan_count, 0U);
	EXPECT_EQ(*faults.adjacency_topology_scan_count, 0U);
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	EXPECT_GT(*faults.node_existence_batch_probe_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, AnchoredRelationshipCountBatchesTopologyPageEndpointProbes) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Other"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "R"});
	PatternExpressionSnapshotFaults faults;
	faults.supports_node_existence_probes = true;
	faults.node_existence_probe_count = std::make_shared<size_t>(0U);
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	faults.adjacency_topology_scan_count = std::make_shared<size_t>(0U);
	PatternExpressionBackend backend(std::move(graph), faults);
	LogicalPlanReadRuntime runtime(backend);
	auto source = std::make_unique<graph::query::logical::LogicalNodeScan>("a", std::vector<std::string>{"Start"});
	auto traversal = std::make_unique<graph::query::logical::LogicalTraversal>(
			std::move(source), "a", "r", "b", "", "out");
	graph::query::logical::LogicalAggregate aggregate(
			std::move(traversal), {},
			{{"count", std::make_shared<graph::query::expressions::VariableReferenceExpression>("b"), "count", false}});

	std::vector<PropertyValue> results;
	static_cast<void>(runtime.execute(aggregate, bounds(), [&results](const LogicalPlanReadRow &row) {
		results.push_back(std::get<PropertyValue>(row.values.at("count")));
		return true;
	}));

	EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{2})});
	EXPECT_GT(*faults.adjacency_topology_scan_count, 0U);
	EXPECT_EQ(*faults.node_existence_probe_count, 0U);
	EXPECT_GT(*faults.node_existence_batch_probe_count, 0U);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, MaterializesAllEqualShortestPathsInDeterministicParentOrder) {
	PatternExpressionBackend backend(makeDiamondGraph());
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 3, true);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &paths = result.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(paths.size(), 2U);
	ASSERT_EQ(paths[0].getType(), PropertyType::LIST);
	ASSERT_EQ(paths[1].getType(), PropertyType::LIST);
	const auto &first = paths[0].getList();
	const auto &second = paths[1].getList();
	ASSERT_EQ(first.size(), 5U);
	ASSERT_EQ(second.size(), 5U);
	EXPECT_EQ(entityId(first[1]), 13);
	EXPECT_EQ(entityId(first[2]), 3);
	EXPECT_EQ(entityId(first[3]), 14);
	EXPECT_EQ(entityId(second[1]), 11);
	EXPECT_EQ(entityId(second[2]), 4);
	EXPECT_EQ(entityId(second[3]), 12);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ReturnsNullWhenNoTypedShortestPathExists) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "OTHER"});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 3, false);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("path").asPrimitive().getType(), PropertyType::NULL_TYPE);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, HonorsTheMinimumHopBoundInsteadOfReturningATooShortRoute) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Middle"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "R"});
	graph.edges.emplace(3U, LogicalGraphEdge{3U, 3U, 2U, "R"});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 2, 2, false);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &path = result.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(path.size(), 5U);
	EXPECT_EQ(entityId(path[1]), 2);
	EXPECT_EQ(entityId(path[3]), 3);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, MaterializesZeroHopPathWhenStartAndEndAreTheSameNode) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Anchor"}});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Anchor", "Anchor", "",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 0, 0, false);

	const QueryResult result = runtime.executeToQueryResult(*root, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &path = result.getRows().front().at("path").asPrimitive().getList();
	ASSERT_EQ(path.size(), 1U);
	EXPECT_EQ(entityId(path.front()), 1);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, SupportsIncomingAndUndirectedTypedShortestPaths) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.edges.emplace(7U, LogicalGraphEdge{7U, 2U, 1U, "BACK"});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);

	auto incoming = makeShortestPathRoot("Start", "End", "BACK",
			graph::query::expressions::PatternDirection::PAT_INCOMING, 1, 1, false);
	const QueryResult incomingResult = runtime.executeToQueryResult(*incoming, bounds());
	ASSERT_EQ(incomingResult.rowCount(), 1U);
	EXPECT_EQ(entityId(incomingResult.getRows().front().at("path").asPrimitive().getList()[1]), 7);

	auto undirected = makeShortestPathRoot("Start", "End", "BACK",
			graph::query::expressions::PatternDirection::PAT_BOTH, 1, 1, false);
	const QueryResult undirectedResult = runtime.executeToQueryResult(*undirected, bounds());
	ASSERT_EQ(undirectedResult.rowCount(), 1U);
	EXPECT_EQ(entityId(undirectedResult.getRows().front().at("path").asPrimitive().getList()[1]), 7);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsShortestPathWhenAnAdjacencyPageExceedsThePerNodeBound) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Other"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	graph.edges.emplace(2U, LogicalGraphEdge{2U, 1U, 3U, "R"});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 2, false);
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_edges_per_node = 1U;
	limited.maximum_adjacency_page_records = 1U;

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*root, limited)), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsShortestPathWhenTheParentDagWouldExceedItsBound) {
	PatternExpressionBackend backend(makeDiamondGraph());
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 3, true);
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_rows = 3U;

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*root, limited)), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsShortestPathWhenTraversalStateExceedsByteBound) {
	PatternExpressionBackend backend(makeLayeredPathGraph());
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 6, false);
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_node_records = 12U;
	limited.maximum_rows = 20U;
	limited.maximum_edges_per_node = 2U;
	limited.maximum_adjacency_page_records = 1U;
	limited.maximum_materialized_bytes = 128U;

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*root, limited)), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsShortestPathWhenAllPathMaterializationWouldExceedItsBound) {
	PatternExpressionBackend backend(makeLayeredPathGraph());
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 6, true);
	LogicalPlanReadRuntimeBounds limited = bounds();
	limited.maximum_node_records = 12U;
	limited.maximum_rows = 20U;
	limited.maximum_edges_per_node = 2U;
	limited.maximum_adjacency_page_records = 1U;

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*root, limited)), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsMalformedShortestPathAdjacencyPagesAndEndpoints) {
	const auto executeFaultyPath = [](PatternExpressionSnapshotFaults faults) {
		PatternExpressionBackend backend(makeDefaultPatternExpressionGraph(), faults);
		LogicalPlanReadRuntime runtime(backend);
		auto root = makeShortestPathRoot("Person", "Friend", "KNOWS",
				graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 2, false);
		return runtime.executeToQueryResult(*root, bounds());
	};

	EXPECT_THROW(static_cast<void>(executeFaultyPath({.malformed_adjacency_page = true})), std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeFaultyPath({.invalid_adjacency_endpoint = true})), std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeFaultyPath({.wrong_node_identity = true})), std::runtime_error);
	EXPECT_THROW(static_cast<void>(executeFaultyPath({.discontinuous_adjacency_page = true})), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsADiscontinuousPatternExpressionAdjacencyPage) {
	PatternExpressionBackend backend(makeDefaultPatternExpressionGraph(), {.discontinuous_adjacency_page = true});
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) WHERE EXISTS((n)-[:OTHER]->(m)) RETURN n");
	ASSERT_NE(plan.root, nullptr);

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds())), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsAPatternExpressionTargetWithTheWrongLogicalIdentity) {
	PatternExpressionBackend backend(makeDefaultPatternExpressionGraph(), {.wrong_node_identity = true});
	LogicalPlanReadRuntime runtime(backend);
	QueryPlan plan = parse("MATCH (n:Person) WHERE EXISTS((n)-[:KNOWS]->(m:Friend)) RETURN n");
	ASSERT_NE(plan.root, nullptr);

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds())), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, RejectsShortestPathWithMalformedNodeMetadata) {
	PatternExpressionGraph graph;
	graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Start", "Start"}});
	graph.nodes.emplace(2U, LogicalGraphNode{2U, {"End"}});
	graph.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "R"});
	PatternExpressionBackend backend(std::move(graph));
	LogicalPlanReadRuntime runtime(backend);
	auto root = makeShortestPathRoot("Start", "End", "R",
			graph::query::expressions::PatternDirection::PAT_OUTGOING, 1, 1, false);

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*root, bounds())), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathScalarConsumersAvoidCompleteEntityProperties) {
	for (const std::string function: {"size", "length"}) {
		for (const bool nativeRefs: {false, true}) {
			for (const bool all: {false, true}) {
				auto graph = makeDiamondGraph();
				for (auto &[id, node]: graph.nodes) {
					static_cast<void>(id);
					node.properties.push_back({"payload", PropertyValue(std::string(8192U, 'n'))});
				}
				for (auto &[id, edge]: graph.edges) {
					static_cast<void>(id);
					edge.properties.push_back({"payload", PropertyValue(std::string(8192U, 'e'))});
				}
				if (all) graph.edges.emplace(15U, LogicalGraphEdge{15U, 1U, 3U, "R"});
				PatternExpressionSnapshotFaults faults;
				faults.supports_node_existence_probes = true;
				faults.supports_native_adjacency_refs = nativeRefs;
				faults.node_read_count = std::make_shared<size_t>(0U);
				faults.edge_read_count = std::make_shared<size_t>(0U);
				PatternExpressionSnapshot snapshot(std::move(graph), faults);
				LogicalPlanReadRow row;
				row.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
				row.values.emplace("b", EntityRef{2U, false, 0U, 0U, 0U});
				std::vector<std::unique_ptr<expressions::Expression>> arguments;
				arguments.push_back(std::make_unique<expressions::ShortestPathExpression>(
						"a", "b", "R", expressions::PatternDirection::PAT_OUTGOING, 0, 3, all));
				expressions::FunctionCallExpression size(function, std::move(arguments));
				auto bounded = bounds();
				bounded.maximum_materialized_bytes = 4096U;
				QueryContext context;
				const auto result = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(snapshot, size, row, bounded, context);
				EXPECT_EQ(std::get<PropertyValue>(result), PropertyValue(all ? 3 : function == "size" ? 5 : 2));
				EXPECT_EQ(*faults.node_read_count, 0U);
				EXPECT_EQ(*faults.edge_read_count, 0U);
			}
		}
	}
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathScalarConsumersPreserveSelfNullDepthAndListBounds) {
	for (const std::string function: {"size", "length"}) {
		PatternExpressionSnapshotFaults faults;
		faults.supports_node_existence_probes = true;
		PatternExpressionSnapshot snapshot(makeDiamondGraph(), faults);
		const auto evaluate = [&](uint64_t end, std::string type, bool all, int minimum,
				const LogicalPlanReadRuntimeBounds &bounded, const QueryContext &context) {
			LogicalPlanReadRow row;
			row.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
			row.values.emplace("b", EntityRef{end, false, 0U, 0U, 0U});
			std::vector<std::unique_ptr<expressions::Expression>> arguments;
			arguments.push_back(std::make_unique<expressions::ShortestPathExpression>(
					"a", "b", std::move(type), expressions::PatternDirection::PAT_OUTGOING, minimum, 3, all));
			expressions::FunctionCallExpression size(function, std::move(arguments));
			return std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(snapshot, size, row, bounded, context));
		};
		QueryContext context;
		for (const bool all: {false, true}) {
			EXPECT_EQ(evaluate(1U, "R", all, 0, bounds(), context), PropertyValue(all || function == "size" ? 1 : 0));
			EXPECT_EQ(evaluate(1U, "R", all, 1, bounds(), context), PropertyValue());
			EXPECT_EQ(evaluate(2U, "MISSING", all, 0, bounds(), context), PropertyValue());
		}
		context.maxVarLengthDepth = 0;
		EXPECT_EQ(evaluate(2U, "R", false, 0, bounds(), context), PropertyValue());
		context.maxVarLengthDepth.reset();
		auto limited = bounds();
		limited.maximum_rows = 4U;
		EXPECT_THROW(static_cast<void>(evaluate(2U, "R", false, 0, limited, context)), std::runtime_error);
		limited.maximum_materialized_bytes = 32U;
		EXPECT_THROW(static_cast<void>(evaluate(2U, "R", true, 0, limited, context)), std::runtime_error);
	}
}

TEST(LogicalPlanPatternExpressionRuntimeTest, AllShortestPathSizeCountsParentDagMultiplicityWithinResultBound) {
	PatternExpressionGraph graph;
	for (uint64_t id = 1U; id <= 5U; ++id) graph.nodes.emplace(id, LogicalGraphNode{id, {"N"}});
	for (uint64_t id = 1U; id < 5U; ++id) {
		graph.edges.emplace(2U * id, LogicalGraphEdge{2U * id, id, id + 1U, "R"});
		graph.edges.emplace(2U * id + 1U, LogicalGraphEdge{2U * id + 1U, id, id + 1U, "R"});
	}
	PatternExpressionSnapshotFaults faults;
	faults.supports_node_existence_probes = true;
	PatternExpressionSnapshot snapshot(std::move(graph), faults);
	LogicalPlanReadRow row;
	row.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
	row.values.emplace("b", EntityRef{5U, false, 0U, 0U, 0U});
	std::vector<std::unique_ptr<expressions::Expression>> arguments;
	arguments.push_back(std::make_unique<expressions::ShortestPathExpression>(
			"a", "b", "R", expressions::PatternDirection::PAT_OUTGOING, 0, 4, true));
	expressions::FunctionCallExpression size("size", std::move(arguments));
	QueryContext context;
	auto bounded = bounds();
	bounded.maximum_rows = 8U;
	EXPECT_THROW(static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(snapshot, size, row, bounded, context)), std::runtime_error);
	bounded.maximum_rows = 16U;
	EXPECT_EQ(std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(snapshot, size, row, bounded, context)), PropertyValue(16));
}

TEST(LogicalPlanPatternExpressionRuntimeTest, EntityRefSelectedPropertyKeepsNullAndRejectsMalformedColumn) {
	for (const bool edge: {false, true}) {
		for (const bool malformed: {false, true}) {
			auto graph = makeDefaultPatternExpressionGraph();
			if (edge) graph.edges.at(10U).properties.push_back({"_type", PropertyValue()});
			else graph.nodes.at(1U).properties.push_back({"_type", PropertyValue()});
			PatternExpressionSnapshotFaults faults;
			faults.node_property_column_admission_count = std::make_shared<size_t>(0U);
			faults.edge_property_column_admission_count = std::make_shared<size_t>(0U);
			faults.truncate_selected_property_column = malformed;
			PatternExpressionSnapshot snapshot(std::move(graph), faults);
			LogicalPlanReadRow row;
			row.values.emplace("entity", EntityRef{edge ? 10U : 1U, edge, edge ? 1U : 0U, edge ? 2U : 0U, 0U});
			expressions::VariableReferenceExpression property("entity", "_type");
			QueryContext context;
			if (malformed) {
				EXPECT_THROW(static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
					snapshot, property, row, bounds(), context)), std::runtime_error);
			} else {
				EXPECT_EQ(std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
					snapshot, property, row, bounds(), context)), PropertyValue());
			}
		}
	}
}

TEST(LogicalPlanPatternExpressionRuntimeTest, EntityRefPropertyMissAdmitsBeforeCopyAndRetriesWithoutCachingFailure) {
	for (const bool edge: {false, true}) {
		auto graph = makeDefaultPatternExpressionGraph();
		const PropertyValue large(std::string(8192U, 'x'));
		if (edge) graph.edges.at(10U).properties.push_back({"_type", large});
		else graph.nodes.at(1U).properties.push_back({"_type", large});
		PatternExpressionSnapshotFaults faults;
		auto admitted = std::make_shared<size_t>(0U);
		auto completed = std::make_shared<size_t>(0U);
		if (edge) {
			faults.edge_property_column_admission_count = admitted;
			faults.edge_property_column_read_count = completed;
		} else {
			faults.node_property_column_admission_count = admitted;
			faults.node_property_column_read_count = completed;
		}
		PatternExpressionSnapshot snapshot(std::move(graph), faults);
		LogicalPlanReadRow row;
		row.values.emplace("entity", EntityRef{edge ? 10U : 1U, edge, edge ? 1U : 0U, edge ? 2U : 0U, 0U});
		expressions::VariableReferenceExpression property("entity", "_type");
		QueryContext context;
		auto limited = bounds();
		limited.maximum_materialized_bytes = 4096U;
		EXPECT_THROW(static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, property, row, limited, context)), std::runtime_error);
		EXPECT_EQ(*admitted, 1U);
		EXPECT_EQ(*completed, 0U);
		EXPECT_FALSE(row.property_cache.contains("entity"));
		limited.maximum_materialized_bytes = 32768U;
		EXPECT_EQ(std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, property, row, limited, context)), large);
		EXPECT_EQ(*completed, 1U);
	}
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathLengthPreservesOverriddenPathMarkerSemantics) {
	for (const bool unsupported: {false, true}) {
		for (const bool edgeMarker: {false, true}) {
			for (const PropertyValue &marker: {PropertyValue(), PropertyValue(42), PropertyValue("spoof"),
					PropertyValue(edgeMarker ? "relationship" : "node")}) {
				auto graph = makeDiamondGraph();
				if (edgeMarker) graph.edges.at(14U).properties.push_back({"_type", marker});
				else graph.nodes.at(3U).properties.push_back({"_type", marker});
				PatternExpressionSnapshotFaults faults;
				faults.supports_node_existence_probes = true;
				faults.unsupported_marker_presence = unsupported;
				if (!unsupported) {
					faults.node_property_column_admission_count = std::make_shared<size_t>(0U);
					faults.edge_property_column_admission_count = std::make_shared<size_t>(0U);
				}
				PatternExpressionSnapshot snapshot(std::move(graph), faults);
				LogicalPlanReadRow row;
				row.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
				row.values.emplace("b", EntityRef{2U, false, 0U, 0U, 0U});
				std::vector<std::unique_ptr<expressions::Expression>> arguments;
				arguments.push_back(std::make_unique<expressions::ShortestPathExpression>(
						"a", "b", "R", expressions::PatternDirection::PAT_OUTGOING, 0, 3, false));
				expressions::FunctionCallExpression length("length", std::move(arguments));
				QueryContext context;
				const auto actual = std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(snapshot, length, row, bounds(), context));
				const bool valid = marker == PropertyValue(edgeMarker ? "relationship" : "node");
				EXPECT_EQ(actual, PropertyValue(valid ? 2 : 5));
			}
		}
	}
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathLengthRejectsOversizedRequestedMarker) {
	auto graph = makeDiamondGraph();
	graph.nodes.at(3U).properties.push_back({"_type", PropertyValue(std::string(8192U, 'x'))});
	PatternExpressionSnapshotFaults faults;
	faults.supports_node_existence_probes = true;
	PatternExpressionSnapshot snapshot(std::move(graph), faults);
	LogicalPlanReadRow row;
	row.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
	row.values.emplace("b", EntityRef{2U, false, 0U, 0U, 0U});
	std::vector<std::unique_ptr<expressions::Expression>> arguments;
	arguments.push_back(std::make_unique<expressions::ShortestPathExpression>(
			"a", "b", "R", expressions::PatternDirection::PAT_OUTGOING, 0, 3, false));
	expressions::FunctionCallExpression length("length", std::move(arguments));
	QueryContext context;
	auto bounded = bounds();
	bounded.maximum_materialized_bytes = 4096U;
	EXPECT_THROW(static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(snapshot, length, row, bounded, context)), std::runtime_error);
}

TEST(LogicalPlanPatternExpressionRuntimeTest, FusesShortestPathScalarObserversAcrossProjectionAlias) {
	for (const bool all: {false, true}) {
		PatternExpressionSnapshotFaults faults;
		faults.supports_node_existence_probes = true;
		faults.node_read_count = std::make_shared<size_t>(0U);
		faults.edge_read_count = std::make_shared<size_t>(0U);
		PatternExpressionBackend backend(makeDiamondGraph(), faults);
		LogicalPlanReadRuntime runtime(backend);
		const auto plan = parse("MATCH (a:Start), (b:End) WITH " + std::string(all ? "allShortestPaths" : "shortestPath") +
				"((a)-[:R*0..3]->(b)) AS p, id(a) AS source "
				"RETURN size(p) AS size, length(p) AS length, source, size(p) + 10 AS adjusted");
		ASSERT_NE(plan.root, nullptr);
		const auto result = runtime.executeToQueryResult(*plan.root, bounds());
		ASSERT_EQ(result.rowCount(), 1U);
		const auto &row = result.getRows().front();
		EXPECT_EQ(row.at("size").asPrimitive(), PropertyValue(all ? 2 : 5));
		EXPECT_EQ(row.at("length").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(row.at("source").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(row.at("adjusted").asPrimitive(), PropertyValue(all ? 12 : 15));
		EXPECT_EQ(*faults.node_read_count, 0U);
		EXPECT_EQ(*faults.edge_read_count, 0U);
	}
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ScalarPathAliasPreservesNullAndCompletePathFallbacks) {
	PatternExpressionSnapshotFaults faults;
	faults.supports_node_existence_probes = true;
	PatternExpressionBackend backend(makeDiamondGraph(), faults);
	LogicalPlanReadRuntime runtime(backend);
	const auto missing = parse("MATCH (a:Start), (b:End) WITH shortestPath((a)-[:MISSING*0..3]->(b)) AS p "
			"RETURN size(p) AS size, length(p) AS length");
	ASSERT_NE(missing.root, nullptr);
	const auto absent = runtime.executeToQueryResult(*missing.root, bounds());
	ASSERT_EQ(absent.rowCount(), 1U);
	EXPECT_EQ(absent.getRows().front().at("size").asPrimitive(), PropertyValue());
	EXPECT_EQ(absent.getRows().front().at("length").asPrimitive(), PropertyValue());
	const auto complete = parse("MATCH (a:Start), (b:End) WITH shortestPath((a)-[:R*0..3]->(b)) AS p "
			"RETURN p, size(p) AS size");
	ASSERT_NE(complete.root, nullptr);
	const auto full = runtime.executeToQueryResult(*complete.root, bounds());
	ASSERT_EQ(full.rowCount(), 1U);
	EXPECT_EQ(full.getRows().front().at("p").asPrimitive().getList().size(), 5U);
	EXPECT_EQ(full.getRows().front().at("size").asPrimitive(), PropertyValue(5));
	const auto conditional = parse("MATCH (a:Start), (b:End) WITH shortestPath((a)-[:R*0..3]->(b)) AS p "
			"RETURN CASE WHEN false THEN size(p) ELSE 0 END AS value");
	ASSERT_NE(conditional.root, nullptr);
	auto limited = bounds();
	limited.maximum_rows = 4U;
	const auto conditionalResult = runtime.executeToQueryResult(*conditional.root, limited);
	ASSERT_EQ(conditionalResult.rowCount(), 1U);
	EXPECT_EQ(conditionalResult.getRows().front().at("value").asPrimitive(), PropertyValue(0));
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ShortestPathTopologySourceShrinksRejectedPagesWithoutReplayingResults) {
	for (const bool references: {false, true}) {
		SCOPED_TRACE(references);
		PatternExpressionSnapshotFaults faults;
		faults.supports_native_adjacency_refs = references;
		faults.topology_admission_requests = std::make_shared<std::vector<size_t>>();
		faults.supports_node_existence_probes = true;
		PatternExpressionSnapshot snapshot(makeDefaultPatternExpressionGraph(), faults);
		LogicalPlanReadRow row;
		row.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
		row.values.emplace("b", EntityRef{2U, false, 0U, 0U, 0U});
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::ShortestPathExpression>(
			"a", "b", "", expressions::PatternDirection::PAT_OUTGOING, 0, 3, false));
		expressions::FunctionCallExpression length("length", std::move(arguments));
		QueryContext context;
		auto limited = bounds();
		limited.maximum_edges_per_node = 16U;
		limited.maximum_adjacency_page_records = 16U;
		limited.maximum_materialized_bytes = 4096U;
		EXPECT_EQ(std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
			snapshot, length, row, limited, context)), PropertyValue(1));
		ASSERT_GT(faults.topology_admission_requests->size(), 1U);
		EXPECT_GT(faults.topology_admission_requests->front(), faults.topology_admission_requests->back());
	}
}

TEST(LogicalPlanPatternExpressionRuntimeTest, ExistsEndpointGatherRejectsBeforeProbeAndCanRetry) {
	PatternExpressionSnapshotFaults faults;
	faults.supports_native_adjacency_refs = true; faults.supports_node_existence_probes = true;
	faults.node_existence_batch_probe_count = std::make_shared<size_t>(0U);
	PatternExpressionSnapshot snapshot(makeDefaultPatternExpressionGraph(), faults);
	LogicalPlanReadRow row; row.values.emplace("a", EntityRef{1U, false, 0U, 0U, 0U});
	expressions::ExistsExpression expression("(a)-->()", "a", "", "", expressions::PatternDirection::PAT_OUTGOING);
	auto limited = bounds(); limited.maximum_adjacency_page_records = 2U;
	limited.maximum_materialized_bytes = sizeof(EntityRef) + sizeof(const EntityRef *);
	EXPECT_THROW(static_cast<void>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(snapshot, expression, row, limited, QueryContext{})), std::runtime_error);
	EXPECT_EQ(*faults.node_existence_batch_probe_count, 0U);
	limited.maximum_materialized_bytes = 10000U;
	EXPECT_EQ(std::get<PropertyValue>(LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(snapshot, expression, row, limited, QueryContext{})), PropertyValue(true));
	EXPECT_EQ(*faults.node_existence_batch_probe_count, 1U);
}
