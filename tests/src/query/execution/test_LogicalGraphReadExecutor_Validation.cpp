/**
 * @file test_LogicalGraphReadExecutor_Validation.cpp
 * @brief Validation and defensive runtime tests for the logical graph reader.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "query/execution/LogicalGraphReadExecutorTestFixture.hpp"

#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeFailsClosedForMalformedBoundsAndUnsupportedPlans) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalNodeScan scan("n");
	LogicalPlanReadRuntimeBounds invalid = runtimeBounds();
	invalid.maximum_adjacency_page_records = 0U;
	EXPECT_THROW(static_cast<void>(runtime.execute(scan, invalid, [](const LogicalPlanReadRow &) { return true; })),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(runtime.execute(scan, runtimeBounds(), LogicalPlanReadRuntime::RowVisitor{})),
				 std::invalid_argument);

	logical::LogicalLimit negativeLimit(std::make_unique<logical::LogicalNodeScan>("n"), -1);
	EXPECT_THROW(static_cast<void>(runtime.execute(negativeLimit, runtimeBounds(),
												   [](const LogicalPlanReadRow &) { return true; })),
				 std::invalid_argument);
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsProjectionOnlyItemsInMapLiterals) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);

	const auto expectRejected = [&runtime](expressions::MapProjectionItemType itemType, std::string key) {
		std::vector<expressions::MapProjectionItem> mapItems;
		mapItems.emplace_back(itemType, std::move(key));
		std::vector<logical::LogicalProjectItem> projectItems;
		projectItems.emplace_back(std::make_shared<expressions::MapProjectionExpression>(std::move(mapItems)), "map");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(projectItems));
		EXPECT_THROW(static_cast<void>(runtime.execute(
					 root, runtimeBounds(), [](const LogicalPlanReadRow &) { return true; })),
				 std::logic_error);
	};

	expectRejected(expressions::MapProjectionItemType::MPROP_PROPERTY, "property");
	expectRejected(expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES, "");
	expectRejected(expressions::MapProjectionItemType::MPROP_LITERAL, "");
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeRejectsEntityFunctionsOnNonEntityBindings) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	const auto visitor = [](const LogicalPlanReadRow &) { return true; };

	const auto scalarInput = [] {
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t(7)), "scalar");
		return std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
	};
	const auto entityFunction = [](std::string name, std::vector<std::unique_ptr<expressions::Expression>> arguments) {
		return sharedExpression(std::make_unique<expressions::FunctionCallExpression>(std::move(name),
																					 std::move(arguments)));
	};
	const auto execute = [&](std::string name, std::vector<std::unique_ptr<expressions::Expression>> arguments) {
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(entityFunction(std::move(name), std::move(arguments)), "result");
		logical::LogicalProject root(scalarInput(), std::move(items));
		return runtime.execute(root, runtimeBounds(), visitor);
	};

	EXPECT_THROW(static_cast<void>(execute("id", {})), std::runtime_error);
	std::vector<std::unique_ptr<expressions::Expression>> literalArgument;
	literalArgument.push_back(std::make_unique<expressions::LiteralExpression>(int64_t(7)));
	EXPECT_THROW(static_cast<void>(execute("id", std::move(literalArgument))), std::runtime_error);
	for (const std::string &name: {"id", "labels", "type", "keys", "properties"}) {
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("scalar"));
		EXPECT_THROW(static_cast<void>(execute(name, std::move(arguments))), std::runtime_error);
	}
}

TEST(LogicalGraphReadExecutorTest, LogicalPlanRuntimeBoundsStaticAndDynamicCollectionExpressions) {
	TestLogicalGraphBackend backend(graphData());
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRuntimeBounds collectionBounds = runtimeBounds();
	collectionBounds.maximum_rows = 2U;
	collectionBounds.maximum_property_entries = 1U;

	const auto expectRejected = [&runtime, &collectionBounds](std::shared_ptr<expressions::Expression> expression) {
		std::vector<logical::LogicalProjectItem> projectItems;
		projectItems.emplace_back(std::move(expression), "value");
		logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(projectItems));
		EXPECT_THROW(static_cast<void>(runtime.execute(
					 root, collectionBounds, [](const LogicalPlanReadRow &) { return true; })),
				 std::runtime_error);
	};

	expectRejected(std::make_shared<expressions::ListLiteralExpression>(
				PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)})));

	std::vector<std::unique_ptr<expressions::Expression>> dynamicList;
	for (int64_t value = 1; value <= 3; ++value) {
		dynamicList.push_back(std::make_unique<expressions::LiteralExpression>(value));
	}
	expectRejected(std::make_shared<expressions::ListLiteralExpression>(std::move(dynamicList)));

	std::vector<expressions::MapProjectionItem> mapItems;
	mapItems.emplace_back(expressions::MapProjectionItemType::MPROP_LITERAL, "first",
			std::make_unique<expressions::LiteralExpression>(int64_t(1)));
	mapItems.emplace_back(expressions::MapProjectionItemType::MPROP_LITERAL, "second",
			std::make_unique<expressions::LiteralExpression>(int64_t(2)));
	expectRejected(std::make_shared<expressions::MapProjectionExpression>(std::move(mapItems)));
}

TEST(LogicalGraphReadExecutorTest, RejectsInvalidBoundsVisitorsAndPredicatesBeforeOpeningTheBackend) {
	TestLogicalGraphBackend backend(graphData());
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	request.bounds.maximum_rows = 0U;
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::invalid_argument);

	request.bounds = bounds();
	EXPECT_THROW(static_cast<void>(executor.execute(request, LogicalGraphReadExecutor::RowVisitor{})),
				 std::invalid_argument);

	request.source_filter.required_labels = {""};
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::invalid_argument);
	request.source_filter.required_labels = {"Person", "Person"};
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::invalid_argument);
	request.source_filter.required_labels.clear();

	request.source_filter.property_predicates = {
			predicate("age", LogicalPropertyPredicateOp::LPP_RANGE_CLOSED, PropertyValue(1))};
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::invalid_argument);
	request.source_filter.property_predicates = {
			predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(1), PropertyValue(2))};
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::invalid_argument);
}

TEST(LogicalGraphReadExecutorTest, RejectsUnknownExpansionDirectionsAndSupportsUntypedRelationshipFilters) {
	LogicalGraphFixtureData data = graphData();
	data.edges.at(11U).type.clear();
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	LogicalGraphExpansion expansion;
	expansion.direction = static_cast<LogicalGraphReadDirection>(0);
	request.expansion = expansion;
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::invalid_argument);

	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.relationship_type = "";
	request.expansion = std::move(expansion);
	EXPECT_EQ(executor.execute(request, [](const LogicalGraphReadRow &) { return true; }).rows_emitted, 1U);
}

TEST(LogicalGraphReadExecutorTest, RejectsBackendsThatViolateLogicalEntityAndPropertyInvariants) {
	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).labels = {"Person", "Person"};
	TestLogicalGraphBackend duplicateLabels(std::move(data));
	LogicalGraphReadExecutor duplicateExecutor(duplicateLabels);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	EXPECT_THROW(static_cast<void>(duplicateExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::runtime_error);

	data = graphData();
	data.nodes.at(1U).properties = {{"age", PropertyValue(42)}, {"age", PropertyValue(43)}};
	TestLogicalGraphBackend wrongPropertyOwner(std::move(data));
	LogicalGraphReadExecutor propertyExecutor(wrongPropertyOwner);
	request.source_filter.required_labels = {"VIP"};
	request.source_filter.property_predicates = {predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(42))};
	EXPECT_THROW(static_cast<void>(propertyExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::runtime_error);

	data = graphData();
	data.nodes.at(1U).properties = {{"", PropertyValue(42)}};
	TestLogicalGraphBackend zeroPropertyId(std::move(data));
	LogicalGraphReadExecutor zeroPropertyIdExecutor(zeroPropertyId);
	EXPECT_THROW(static_cast<void>(zeroPropertyIdExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::runtime_error);

	data = graphData();
	data.edges.at(10U).source_node_id = 2U;
	data.force_invalid_adjacency = true;
	TestLogicalGraphBackend wrongDirection(std::move(data));
	LogicalGraphReadExecutor directionExecutor(wrongDirection);
	request.source_filter.property_predicates.clear();
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	request.expansion = std::move(expansion);
	EXPECT_THROW(static_cast<void>(directionExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, RejectsBackendsThatIgnoreTheRequestedScanBoundOrReturnNoSnapshot) {
	LogicalGraphFixtureData data = graphData();
	data.ignore_node_bound = true;
	TestLogicalGraphBackend boundViolating(std::move(data));
	LogicalGraphReadExecutor boundExecutor(boundViolating);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	request.bounds.maximum_node_records = 2U;
	EXPECT_THROW(static_cast<void>(boundExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::runtime_error);

	TestLogicalGraphBackend noSnapshot(graphData(), true);
	LogicalGraphReadExecutor noSnapshotExecutor(noSnapshot);
	request.bounds = bounds();
	EXPECT_THROW(static_cast<void>(noSnapshotExecutor.execute(request, [](const LogicalGraphReadRow &) {})),
				 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, RejectsBackendsThatIgnoreTheRequestedAdjacencyBound) {
	LogicalGraphFixtureData data = graphData();
	data.ignore_edge_bound = true;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	request.expansion = std::move(expansion);
	request.bounds = bounds();
	request.bounds.maximum_edges_per_node = 1U;
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, RejectsBackendsThatIgnoreTraversalStopRequests) {
	LogicalGraphFixtureData data = graphData();
	data.ignore_stop_signal = true;
	TestLogicalGraphBackend backend(std::move(data));
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	request.bounds.maximum_rows = 1U;

	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::runtime_error);

	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	request.expansion = std::move(expansion);
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, RejectsEveryBoundedFilterShapeAndPredicateOperation) {
	TestLogicalGraphBackend backend(graphData());
	LogicalGraphReadExecutor executor(backend);
	LogicalGraphReadRequest request;
	request.bounds = bounds();

	request.source_filter.property_predicates = {
			predicate("", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(42))};
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);

	request.source_filter.property_predicates = {
			predicate("age", static_cast<LogicalPropertyPredicateOp>(255U), PropertyValue(42))};
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);

	for (const LogicalPropertyPredicateOp operation: {LogicalPropertyPredicateOp::LPP_NE, LogicalPropertyPredicateOp::LPP_LT,
													 LogicalPropertyPredicateOp::LPP_LE, LogicalPropertyPredicateOp::LPP_GT,
													 LogicalPropertyPredicateOp::LPP_GE}) {
		request.source_filter.required_labels = {"VIP"};
		request.source_filter.property_predicates = {predicate("age", operation, PropertyValue(42))};
		EXPECT_EQ(executor.execute(request, [](const LogicalGraphReadRow &) {}).rows_emitted,
				  operation == LogicalPropertyPredicateOp::LPP_NE || operation == LogicalPropertyPredicateOp::LPP_LT ||
						  operation == LogicalPropertyPredicateOp::LPP_GT
						  ? 0U
						  : 1U);
	}

	request.source_filter.required_labels.assign(1025U, "ignored");
	request.source_filter.property_predicates.clear();
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);

	request.source_filter.required_labels.clear();
	request.source_filter.property_predicates.assign(1025U, predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(42)));
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);

	request.source_filter.property_predicates.clear();
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.property_predicates.assign(1025U,
																predicate("weight", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(2)));
	request.expansion = std::move(expansion);
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);

	request.expansion->edge_filter.property_predicates.clear();
	request.bounds.maximum_node_records = 0U;
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);
	request.bounds = bounds();
	request.bounds.maximum_edges_per_node = 0U;
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);
	request.bounds = bounds();
	request.bounds.maximum_property_entries = 0U;
	EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::invalid_argument);
}

TEST(LogicalGraphReadExecutorTest, RejectsMalformedPropertyCollectionsAndLogicalEntityIdentities) {
	const auto expectMalformedSourceProperties = [](LogicalGraphFixtureData data) {
		TestLogicalGraphBackend backend(std::move(data));
		LogicalGraphReadExecutor executor(backend);
		LogicalGraphReadRequest request;
		request.bounds = bounds();
		request.source_filter.required_labels = {"VIP"};
		request.source_filter.property_predicates = {predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(42))};
		EXPECT_THROW(static_cast<void>(executor.execute(request, [](const LogicalGraphReadRow &) {})), std::runtime_error);
	};

	LogicalGraphFixtureData data = graphData();
	data.nodes.at(1U).properties.push_back({"extra", PropertyValue(1)});
	LogicalGraphReadExecutionBounds propertyBound = bounds();
	propertyBound.maximum_property_entries = 2U;
	TestLogicalGraphBackend oversizedProperties(std::move(data));
	LogicalGraphReadExecutor oversizedPropertyExecutor(oversizedProperties);
	LogicalGraphReadRequest propertyRequest;
	propertyRequest.bounds = propertyBound;
	propertyRequest.source_filter.required_labels = {"VIP"};
	propertyRequest.source_filter.property_predicates = {predicate("age", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(42))};
	EXPECT_THROW(static_cast<void>(oversizedPropertyExecutor.execute(propertyRequest, [](const LogicalGraphReadRow &) {})),
			 std::runtime_error);

	data = graphData();
	data.nodes.at(1U).properties = {{"", PropertyValue(42)}};
	expectMalformedSourceProperties(std::move(data));

	data = graphData();
	data.nodes.at(1U).properties = {{"age", PropertyValue(42)}, {"age", PropertyValue(43)}};
	expectMalformedSourceProperties(std::move(data));

	data = graphData();
	data.nodes.at(1U).labels = {"", "VIP"};
	TestLogicalGraphBackend emptyNodeLabel(std::move(data));
	LogicalGraphReadExecutor emptyNodeLabelExecutor(emptyNodeLabel);
	LogicalGraphReadRequest emptyLabelRequest;
	emptyLabelRequest.bounds = bounds();
	EXPECT_THROW(static_cast<void>(emptyNodeLabelExecutor.execute(emptyLabelRequest, [](const LogicalGraphReadRow &) {})),
			 std::runtime_error);

	data = graphData();
	data.nodes.at(1U).node_id = 0U;
	TestLogicalGraphBackend zeroNode(std::move(data));
	LogicalGraphReadExecutor zeroNodeExecutor(zeroNode);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	EXPECT_THROW(static_cast<void>(zeroNodeExecutor.execute(request, [](const LogicalGraphReadRow &) {})), std::runtime_error);

	const auto edgeRequest = [] {
		LogicalGraphReadRequest result;
		result.bounds = bounds();
		result.source_filter.required_labels = {"VIP"};
		LogicalGraphExpansion expansion;
		expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
		result.expansion = std::move(expansion);
		return result;
	};

	data = graphData();
	data.edges.at(10U).edge_id = 0U;
	TestLogicalGraphBackend zeroEdge(std::move(data));
	LogicalGraphReadExecutor zeroEdgeExecutor(zeroEdge);
	EXPECT_THROW(static_cast<void>(zeroEdgeExecutor.execute(edgeRequest(), [](const LogicalGraphReadRow &) {})),
			 std::runtime_error);

	data = graphData();
	data.edges.at(10U).source_node_id = 0U;
	data.force_invalid_adjacency = true;
	TestLogicalGraphBackend zeroEdgeSource(std::move(data));
	LogicalGraphReadExecutor zeroEdgeSourceExecutor(zeroEdgeSource);
	EXPECT_THROW(static_cast<void>(zeroEdgeSourceExecutor.execute(edgeRequest(), [](const LogicalGraphReadRow &) {})),
			 std::runtime_error);

	data = graphData();
	data.edges.at(10U).target_node_id = 0U;
	TestLogicalGraphBackend zeroEdgeTarget(std::move(data));
	LogicalGraphReadExecutor zeroEdgeTargetExecutor(zeroEdgeTarget);
	EXPECT_THROW(static_cast<void>(zeroEdgeTargetExecutor.execute(edgeRequest(), [](const LogicalGraphReadRow &) {})),
			 std::runtime_error);
}

TEST(LogicalGraphReadExecutorTest, OmitsEdgesWithoutPropertiesAndTargetsThatMissTheExpansionFilter) {
	LogicalGraphFixtureData data = graphData();
	data.edges.at(10U).properties.clear();
	TestLogicalGraphBackend propertylessBackend(std::move(data));
	LogicalGraphReadExecutor propertylessExecutor(propertylessBackend);
	LogicalGraphReadRequest request;
	request.bounds = bounds();
	request.source_filter.required_labels = {"VIP"};
	LogicalGraphExpansion expansion;
	expansion.direction = LogicalGraphReadDirection::LGRD_OUTGOING;
	expansion.edge_filter.property_predicates = {predicate("weight", LogicalPropertyPredicateOp::LPP_EQ, PropertyValue(2))};
	request.expansion = std::move(expansion);
	EXPECT_EQ(propertylessExecutor.execute(request, [](const LogicalGraphReadRow &) {}).rows_emitted, 0U);

	TestLogicalGraphBackend targetBackend(graphData());
	LogicalGraphReadExecutor targetExecutor(targetBackend);
	request.expansion->edge_filter.property_predicates.clear();
	request.expansion->target_filter.required_labels = {"VIP"};
	EXPECT_EQ(targetExecutor.execute(request, [](const LogicalGraphReadRow &) {}).rows_emitted, 0U);
}
