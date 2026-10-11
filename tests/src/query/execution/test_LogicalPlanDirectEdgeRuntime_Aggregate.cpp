#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"

namespace graph::query::execution {
	using namespace direct_edge_test;





	TEST(LogicalPlanDirectEdgeRuntimeTest, TwoHopMergedTargetsFilterBeforeEnforcingMultiplicityRowBound) {
		DirectEdgeGraph graph; graph.enable_batch_adjacency_reference_scan = true;
		graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Seed"}});
		graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Mid"}});
		graph.nodes.emplace(3U, LogicalGraphNode{3U, {"Mid"}});
		graph.nodes.emplace(4U, LogicalGraphNode{4U, {"End"}});
		graph.nodes.emplace(5U, LogicalGraphNode{5U, {"Other"}});
		graph.edges.emplace_back(10U, 1U, 2U, "KNOWS");
		graph.edges.emplace_back(11U, 1U, 2U, "KNOWS");
		for (uint64_t edge = 12U; edge < 15U; ++edge) graph.edges.emplace_back(edge, 1U, 3U, "KNOWS");
		graph.edges.emplace_back(15U, 2U, 4U, "KNOWS");
		graph.edges.emplace_back(16U, 2U, 4U, "KNOWS");
		graph.edges.emplace_back(17U, 3U, 4U, "KNOWS");
		graph.edges.emplace_back(18U, 2U, 5U, "KNOWS");
		graph.edges.emplace_back(19U, 3U, 5U, "KNOWS");
		DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
		auto inner = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Seed"}),
			"a", "r", "b", "KNOWS", "out", std::vector<std::string>{"Mid"});
		auto outer = std::make_unique<logical::LogicalTraversal>(std::move(inner), "b", "s", "c", "KNOWS", "out",
			std::vector<std::string>{"End"});
		logical::LogicalAggregate aggregate(std::move(outer), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("c"), "count", false}});
		auto bounds = directBounds(); bounds.maximum_materialized_bytes = 8192U;
		bounds.maximum_edges_per_node = 16U; bounds.maximum_edge_records = 16U; bounds.maximum_rows = 6U;
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.execute(aggregate, bounds,
			[&](const LogicalPlanReadRow &) { ++delivered; return true; })), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		bounds.maximum_rows = 7U;
		static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
			++delivered; EXPECT_EQ(std::get<PropertyValue>(row.values.at("count")), PropertyValue(7)); return true;
		}));
		EXPECT_EQ(delivered, 1U);
		EXPECT_GT(graph.batch_adjacency_ref_scans, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, TwoHopParallelFinalEdgesAggregateWithinBudget) {
		DirectEdgeGraph graph; graph.enable_batch_adjacency_reference_scan = true;
		graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Seed"}});
		graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Mid"}});
		graph.nodes.emplace(3U, LogicalGraphNode{3U, {"End"}});
		graph.edges.emplace_back(10U, 1U, 2U, "KNOWS");
		for (uint64_t edge = 11U; edge < 111U; ++edge) graph.edges.emplace_back(edge, 2U, 3U, "KNOWS");
		DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
		auto inner = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Seed"}),
			"a", "r", "b", "KNOWS", "out", std::vector<std::string>{"Mid"});
		auto outer = std::make_unique<logical::LogicalTraversal>(std::move(inner), "b", "s", "c", "KNOWS", "out", std::vector<std::string>{"End"});
		logical::LogicalAggregate aggregate(std::move(outer), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("c"), "count", false}});
		auto bounds = directBounds(); bounds.maximum_materialized_bytes = 1024U;
		bounds.maximum_edges_per_node = 128U; bounds.maximum_edge_records = 128U; bounds.maximum_rows = 128U;
		size_t delivered = 0U;
		static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
			++delivered; EXPECT_EQ(std::get<PropertyValue>(row.values.at("count")), PropertyValue(100)); return true;
		}));
		EXPECT_EQ(delivered, 1U);
		EXPECT_GT(graph.batch_adjacency_ref_scans, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, TwoHopParallelFirstEdgesUseDistinctEndpointGatherWithinBudget) {
		DirectEdgeGraph graph; graph.enable_batch_adjacency_reference_scan = true;
		graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Seed"}});
		graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Mid"}});
		graph.nodes.emplace(3U, LogicalGraphNode{3U, {"End"}});
		for (uint64_t edge = 10U; edge < 110U; ++edge) graph.edges.emplace_back(edge, 1U, 2U, "KNOWS");
		graph.edges.emplace_back(110U, 2U, 3U, "KNOWS");
		DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
		auto inner = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Seed"}),
			"a", "r", "b", "KNOWS", "out", std::vector<std::string>{"Mid"});
		auto outer = std::make_unique<logical::LogicalTraversal>(std::move(inner), "b", "s", "c", "KNOWS", "out");
		logical::LogicalAggregate aggregate(std::move(outer), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("c"), "count", false}});
		auto bounds = directBounds(); bounds.maximum_materialized_bytes = 1024U;
		bounds.maximum_edges_per_node = 128U; bounds.maximum_edge_records = 128U; bounds.maximum_rows = 128U;
		size_t delivered = 0U;
		static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
			++delivered; EXPECT_EQ(std::get<PropertyValue>(row.values.at("count")), PropertyValue(100)); return true;
		}));
		EXPECT_EQ(delivered, 1U);
		EXPECT_GT(graph.batch_adjacency_ref_scans, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, TwoHopLabelCardinalityFailureCanRetryAtEitherHop) {
		for (bool innerLabels : {false, true}) {
			DirectEdgeGraph graph; graph.enable_batch_adjacency_reference_scan = true;
			graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Seed"}});
			graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Mid"}});
			graph.nodes.emplace(3U, LogicalGraphNode{3U, {"End"}});
			graph.edges.emplace_back(10U, 1U, 2U, "KNOWS");
			graph.edges.emplace_back(11U, 2U, 3U, "KNOWS");
			DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
			auto inner = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Seed"}),
				"a", "r", "b", "KNOWS", "out", innerLabels ? std::vector<std::string>{"Mid"} : std::vector<std::string>{});
			auto outer = std::make_unique<logical::LogicalTraversal>(std::move(inner), "b", "s", "c", "KNOWS", "out",
				innerLabels ? std::vector<std::string>{} : std::vector<std::string>{"End"});
			logical::LogicalAggregate aggregate(std::move(outer), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("c"), "count", false}});
			auto bounds = directBounds(); bounds.maximum_materialized_bytes = 8192U;
			graph.truncate_node_label_batch = true;
			size_t delivered = 0U;
			EXPECT_THROW(static_cast<void>(runtime.execute(aggregate, bounds,
				[&](const LogicalPlanReadRow &) { ++delivered; return true; })), std::runtime_error);
			EXPECT_EQ(delivered, 0U);
			graph.truncate_node_label_batch = false;
			static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
				++delivered; EXPECT_EQ(std::get<PropertyValue>(row.values.at("count")), PropertyValue(1)); return true;
			}));
			EXPECT_EQ(delivered, 1U);
			EXPECT_GT(graph.batch_adjacency_ref_scans, 0U);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, TwoHopCountBudgetFailureCanRetryWithParallelEdgeMultiplicity) {
		DirectEdgeGraph graph; graph.enable_batch_adjacency_reference_scan = true;
		graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Seed"}});
		graph.nodes.emplace(2U, LogicalGraphNode{2U, {"Mid"}});
		graph.nodes.emplace(3U, LogicalGraphNode{3U, {"End"}});
		graph.edges.emplace_back(10U, 1U, 2U, "KNOWS");
		graph.edges.emplace_back(11U, 1U, 2U, "KNOWS");
		graph.edges.emplace_back(12U, 2U, 3U, "KNOWS");
		graph.edges.emplace_back(13U, 2U, 3U, "KNOWS");
		DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
		auto inner = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Seed"}),
			"a", "r", "b", "KNOWS", "out");
		auto outer = std::make_unique<logical::LogicalTraversal>(std::move(inner), "b", "s", "c", "KNOWS", "out");
		logical::LogicalAggregate aggregate(std::move(outer), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("c"), "count", false}});
		auto bounds = directBounds(); bounds.maximum_materialized_bytes = 1U;
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.execute(aggregate, bounds,
			[&](const LogicalPlanReadRow &) { ++delivered; return true; })), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		bounds.maximum_materialized_bytes = 8192U;
		static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
			++delivered; EXPECT_EQ(std::get<PropertyValue>(row.values.at("count")), PropertyValue(4)); return true;
		}));
		EXPECT_EQ(delivered, 1U);
		EXPECT_GT(graph.batch_adjacency_ref_scans, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AggregateKeepsInputChargeWhileGrowingGroupsButStreamsCountOnly) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto snapshot = backend.openLogicalGraphReadSnapshot();
		LogicalPlanReadRow input;
		input.values.emplace("value", PropertyValue(std::string(4096U, 'x')));
		logical::LogicalAggregate grouped(std::make_unique<logical::LogicalSingleRow>(),
			{std::make_shared<expressions::VariableReferenceExpression>("value")},
			{{"count", nullptr, "total"}}, {"key"});
		logical::LogicalAggregate countOnly(std::make_unique<logical::LogicalSingleRow>(), {},
			{{"count", nullptr, "total"}});
		auto limited = directBounds();
		limited.maximum_materialized_bytes = 2048U;
		size_t delivered = 0U;
		try {
			static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, grouped, limited, input,
				[&](const LogicalPlanReadRow &) { ++delivered; return true; }, QueryContext{}));
			FAIL() << "Aggregate accepted a live input row beyond the shared allowance";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical-plan Aggregate input row exceeded its shared byte bound");
		}
		EXPECT_EQ(delivered, 0U);
		static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, countOnly, limited, input,
			[&](const LogicalPlanReadRow &row) {
				++delivered;
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("total")), PropertyValue(int64_t{1}));
				return true;
			}, QueryContext{}));
		EXPECT_EQ(delivered, 1U);
		limited.maximum_materialized_bytes = 32768U;
		static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, grouped, limited, input,
			[&](const LogicalPlanReadRow &row) {
				++delivered;
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("key")), PropertyValue(std::string(4096U, 'x')));
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("total")), PropertyValue(int64_t{1}));
				return true;
			}, QueryContext{}));
		EXPECT_EQ(delivered, 2U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AggregateChargesGroupVectorGrowthPeak) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<PropertyValue> keys;
		for (int64_t key = 0; key < 9; ++key) keys.emplace_back(key);
		auto unwind = std::make_unique<logical::LogicalUnwind>(std::make_unique<logical::LogicalSingleRow>(),
				"key", std::make_shared<expressions::ListLiteralExpression>(PropertyValue(std::move(keys))));
		logical::LogicalAggregate aggregate(std::move(unwind),
				{std::make_shared<expressions::VariableReferenceExpression>("key")},
				{{"count", nullptr, "count"}}, {"key"});
		auto bounds = directBounds();
		size_t delivered = 0U;
		bool groupGrowthRejected = false;
		// The input List and row objects must fit before this can exercise group
		// growth. Their sizes vary across standard libraries, so locate that
		// specific admission boundary within the existing ample allowance.
		for (size_t allowance = 9U * sizeof(PropertyValue); allowance < 32U * 1024U; allowance += 64U) {
			bounds.maximum_materialized_bytes = allowance;
			try {
				static_cast<void>(runtime.execute(aggregate, bounds,
						[&](const LogicalPlanReadRow &) { ++delivered; return true; }));
				break;
			} catch (const std::runtime_error &error) {
				EXPECT_EQ(delivered, 0U);
				if (std::string_view(error.what()) ==
					"logical-plan Aggregate group vector growth exceeded its materialized byte bound") {
					groupGrowthRejected = true;
					break;
				}
			}
		}
		EXPECT_TRUE(groupGrowthRejected) << "group-vector growth was never rejected before output";
		EXPECT_EQ(delivered, 0U);

		delivered = 0U;
		bounds.maximum_materialized_bytes = 32U * 1024U;
		const auto stats = runtime.execute(aggregate, bounds,
				[&](const LogicalPlanReadRow &row) {
					++delivered;
					EXPECT_EQ(std::get<int64_t>(std::get<PropertyValue>(row.values.at("key")).getVariant()),
							static_cast<int64_t>(delivered - 1U));
					EXPECT_EQ(std::get<int64_t>(std::get<PropertyValue>(row.values.at("count")).getVariant()), 1);
					return true;
				});
		EXPECT_EQ(delivered, 9U);
		EXPECT_EQ(stats.rows_emitted, 9U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BorrowedCountPredicatesPreserveLiteralTypesDuplicatesAndContradictions) {
		const std::vector<PropertyValue> values{
			PropertyValue(std::string(1000U, 's')), PropertyValue(true), PropertyValue(int64_t{7}), PropertyValue(7.0),
			PropertyValue(std::vector<PropertyValue>{PropertyValue(std::string(1000U, 'l')), PropertyValue()}),
			PropertyValue(PropertyValue::MapType{{"payload", PropertyValue(std::string(1000U, 'm'))}})};
		for (const auto &expected: values) {
			for (const bool conflict: {false, true}) {
				DirectEdgeGraph graph = directGraph();
				for (auto &edge: graph.edges) edge.properties.emplace_back("payload", expected);
				DirectEdgeBackend backend(graph);
				LogicalPlanReadRuntime runtime(backend);
				auto traversal = std::make_unique<logical::LogicalTraversal>(
						std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out",
						std::vector<std::string>{}, std::vector<std::pair<std::string, PropertyValue>>{},
						std::unordered_map<std::string, PropertyValue>{{"payload", expected}});
				const auto literal = [&]() -> std::unique_ptr<expressions::Expression> {
					if (conflict) return std::make_unique<expressions::LiteralExpression>(PropertyValue("different"));
					if (const auto *text = std::get_if<std::string>(&expected.getVariant())) {
						return std::make_unique<expressions::LiteralExpression>(*text);
					}
					if (const auto *boolean = std::get_if<bool>(&expected.getVariant())) {
						return std::make_unique<expressions::LiteralExpression>(*boolean);
					}
					if (const auto *integer = std::get_if<int64_t>(&expected.getVariant())) {
						return std::make_unique<expressions::LiteralExpression>(*integer);
					}
					if (const auto *number = std::get_if<double>(&expected.getVariant())) {
						return std::make_unique<expressions::LiteralExpression>(*number);
					}
					return std::make_unique<expressions::LiteralExpression>(expected);
				};
				auto forward = std::make_unique<expressions::BinaryOpExpression>(
						std::make_unique<expressions::VariableReferenceExpression>("r", "payload"),
						expressions::BinaryOperatorType::BOP_EQUAL, literal());
				auto reverse = std::make_unique<expressions::BinaryOpExpression>(literal(),
						expressions::BinaryOperatorType::BOP_EQUAL,
						std::make_unique<expressions::VariableReferenceExpression>("r", "payload"));
				auto filter = std::make_unique<logical::LogicalFilter>(std::move(traversal),
						std::make_shared<expressions::BinaryOpExpression>(std::move(forward),
								expressions::BinaryOperatorType::BOP_AND, std::move(reverse)));
				logical::LogicalAggregate aggregate(std::move(filter), {},
						{{"count", std::make_shared<expressions::VariableReferenceExpression>("r"), "count", false}});
				auto bounds = directBounds();
				bounds.maximum_materialized_bytes = 16000U;
				for (const bool stop: {false, true, false}) {
					std::vector<PropertyValue> actual;
					EXPECT_NO_THROW(static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
						actual.push_back(std::get<PropertyValue>(row.values.at("count")));
						return !stop;
					})));
					EXPECT_EQ(actual, (std::vector<PropertyValue>{PropertyValue(int64_t{conflict ? 0 : 3})}));
				}
				EXPECT_EQ(graph.edge_ref_scans, conflict ? 0U : 3U);
				EXPECT_EQ(graph.edge_property_point_reads, 0U);
			}
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, DirectCountRejectsCorruptedColumnsBeforeBytePressureRetries) {
		DirectEdgeGraph graph = directGraph();
		graph.truncate_edge_property_batch = true;
		for (auto &edge: graph.edges) edge.properties.emplace_back("payload", PropertyValue(std::string(4000U, 'a')));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out",
				std::vector<std::string>{}, std::vector<std::pair<std::string, PropertyValue>>{},
				std::unordered_map<std::string, PropertyValue>{{"payload", PropertyValue(7)}});
		logical::LogicalAggregate aggregate(std::move(traversal), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("r"), "count", false}});
		auto bounds = directBounds();
		bounds.maximum_materialized_bytes = 2000U;
		size_t delivered = 0U;
		try {
			static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &) { ++delivered; return true; }));
			FAIL() << "invalid column cardinality must fail before budget retries";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical graph backend changed sorted Edge Property result cardinality");
		}
		EXPECT_EQ(delivered, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AnchoredCountAdmitsTargetColumnsBeforeCopyingPayloads) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_read_admission = true;
		const PropertyValue payload(std::string(4096U, 'p'));
		graph.nodes.at(2U).properties.emplace_back("payload", payload);
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out",
				std::vector<std::string>{}, std::vector<std::pair<std::string, PropertyValue>>{{"payload", payload}});
		logical::LogicalAggregate aggregate(std::move(traversal), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("b"), "count", false}});
		auto limits = directBounds();
		limits.maximum_materialized_bytes = 1024U;

		EXPECT_THROW(static_cast<void>(runtime.execute(aggregate, limits,
				[](const LogicalPlanReadRow &) { return true; })), std::runtime_error);
		EXPECT_EQ(graph.node_property_column_admission_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, DirectFilteredCountSplitsSharedPropertyScratchAndRejectsOversizedCells) {
		for (const size_t payloadBytes: {size_t{1000U}, size_t{4000U}}) {
			SCOPED_TRACE(payloadBytes);
			DirectEdgeGraph graph = directGraph();
			for (size_t index = 0U; index < graph.edges.size(); ++index) {
				graph.edges[index].properties.emplace_back("payload",
						PropertyValue(std::string(payloadBytes, index == 1U ? 'b' : 'a')));
			}
			std::swap(graph.edges[0], graph.edges[1]);
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			auto traversal = std::make_unique<logical::LogicalTraversal>(
					std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out",
					std::vector<std::string>{}, std::vector<std::pair<std::string, PropertyValue>>{},
					std::unordered_map<std::string, PropertyValue>{{"payload", PropertyValue(std::string(payloadBytes, 'a'))}});
			logical::LogicalAggregate aggregate(std::move(traversal), {},
					{{"count", std::make_shared<expressions::VariableReferenceExpression>("r"), "count", false}});
			auto bounds = directBounds();
			bounds.maximum_materialized_bytes = 2000U;
			std::vector<PropertyValue> delivered;
			const auto run = [&](bool stop) {
				return runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
					delivered.push_back(std::get<PropertyValue>(row.values.at("count")));
					return !stop;
				});
			};
			if (payloadBytes == 1000U) {
				for (bool stop: {false, true, false}) {
					delivered.clear();
					EXPECT_NO_THROW(static_cast<void>(run(stop)));
					EXPECT_EQ(delivered, (std::vector<PropertyValue>{PropertyValue(int64_t{2})}));
				}
				EXPECT_GT(graph.edge_property_column_batch_reads, 3U);
			} else {
				EXPECT_THROW(static_cast<void>(run(false)), std::runtime_error);
				EXPECT_TRUE(delivered.empty());
			}
			EXPECT_EQ(graph.edge_property_point_reads, 0U);
			EXPECT_TRUE(graph.node_read_ids.empty());
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AggregatesNodePropertiesThroughOneSelectedColumnBatch) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		logical::LogicalAggregate aggregate(
				std::move(scan), {},
				{{"sum", std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "total", false}});

		std::vector<PropertyValue> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			results.push_back(std::get<PropertyValue>(row.values.at("total")));
			return true;
		}));

		EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{6})});
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CountPropertyFallbackExcludesNullAndAvoidsExtraEntityReads) {
		for (const bool edges: {false, true}) {
			SCOPED_TRACE(edges);
			DirectEdgeGraph graph = directGraph();
			graph.enable_adjacency_reference_pages = true;
			std::unique_ptr<logical::LogicalOperator> source;
			if (edges) {
				graph.edges[0].properties = {{"weight", PropertyValue()}};
				graph.edges[1].properties = {{"weight", PropertyValue(7)}};
				graph.edges[2].properties.clear();
				source = std::make_unique<logical::LogicalTraversal>(
					std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");
			} else {
				graph.nodes.at(1U).properties = {{"rank", PropertyValue()}};
				graph.nodes.at(2U).properties = {{"rank", PropertyValue(7)}};
				graph.nodes.at(3U).properties.clear();
				source = std::make_unique<logical::LogicalNodeScan>("n");
			}
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			logical::LogicalAggregate aggregate(std::move(source), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>(edges ? "r" : "n", edges ? "weight" : "rank"), "count", false}});
			std::vector<PropertyValue> results;
			static_cast<void>(runtime.execute(aggregate, directBounds(), [&](const LogicalPlanReadRow &row) {
				results.push_back(std::get<PropertyValue>(row.values.at("count")));
				return true;
			}));
			EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(1)});
			EXPECT_EQ(edges ? graph.edge_property_point_reads : graph.node_property_point_reads, 3U);
			// This compatibility reader implements an Edge Property point read
			// by reading its Edge. COUNT must not add an existence/full-read pass.
			EXPECT_EQ(graph.edge_read_ids.size(), edges ? 3U : 0U);
			if (!edges) EXPECT_TRUE(graph.node_read_ids.empty());
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, GroupedMixedCountOmitsPayloadOnlyWithNonNullCapability) {
		for (const bool supportsProbe: {false, true}) {
			SCOPED_TRACE(supportsProbe);
			DirectEdgeGraph graph = directGraph();
			graph.enable_non_null_probes = supportsProbe;
			graph.nodes.at(1U).properties = {{"rank", PropertyValue(0)},
				{"payload", PropertyValue(std::string(supportsProbe ? 8192U : 8U, 'x'))}};
			graph.nodes.at(2U).properties = {{"rank", PropertyValue(0)}, {"payload", PropertyValue()}};
			graph.nodes.at(3U).properties = {{"rank", PropertyValue(1)}};
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			auto source = std::make_unique<logical::LogicalUnwind>(
				std::make_unique<logical::LogicalNodeScan>("n"), "repeat",
				std::vector<PropertyValue>{PropertyValue(1), PropertyValue(1)});
			auto rank = std::make_shared<expressions::VariableReferenceExpression>("n", "rank");
			logical::LogicalAggregate aggregate(std::move(source), {rank},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("n", "payload"), "count", false},
				 {"sum", rank, "sum", false}}, {"rank"});
			auto bounds = directBounds();
			bounds.maximum_materialized_bytes = 4096U;
			std::map<int64_t, std::pair<PropertyValue, PropertyValue>> results;
			static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
				results.emplace(std::get<int64_t>(std::get<PropertyValue>(row.values.at("rank")).getVariant()),
					std::make_pair(std::get<PropertyValue>(row.values.at("count")), std::get<PropertyValue>(row.values.at("sum"))));
				return true;
			}));
			ASSERT_EQ(results.size(), 2U);
			EXPECT_EQ(results.at(0), (std::pair<PropertyValue, PropertyValue>{PropertyValue(2), PropertyValue(0)}));
			EXPECT_EQ(results.at(1), (std::pair<PropertyValue, PropertyValue>{PropertyValue(0), PropertyValue(2)}));
			// Byte pressure can split the fallback into different batch counts
			// across standard libraries; every batch must use the same read shape.
			EXPECT_GT(graph.node_property_column_batch_reads, 0U);
			ASSERT_EQ(graph.node_property_column_batch_keys.size(), graph.node_property_column_batch_reads);
			for (const auto &keys: graph.node_property_column_batch_keys) {
				EXPECT_EQ(keys, supportsProbe ? (std::vector<std::string>{"rank"})
											 : (std::vector<std::string>{"rank", "payload"}));
			}
			EXPECT_EQ(graph.node_non_null_probes.size(), supportsProbe ? 6U : 0U);
			EXPECT_EQ(graph.node_property_point_reads, 0U);
			EXPECT_TRUE(graph.node_read_ids.empty());
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CountUsesProjectedPropertyColumnBeforeEntityProbe) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_non_null_probes = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "n");
		items.emplace_back(std::make_shared<expressions::LiteralExpression>(PropertyValue()), "n.rank");
		auto project = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalNodeScan>("n"), std::move(items));
		logical::LogicalAggregate aggregate(std::move(project), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "count", false}});
		std::vector<PropertyValue> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&](const LogicalPlanReadRow &row) {
			results.push_back(std::get<PropertyValue>(row.values.at("count")));
			return true;
		}));
		EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(0)});
		EXPECT_TRUE(graph.node_non_null_probes.empty());
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesSafeSubsetOfMixedAggregateExpressions) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto rank = std::make_shared<expressions::VariableReferenceExpression>("n", "rank");
		auto entity = std::make_shared<expressions::VariableReferenceExpression>("n");
		logical::LogicalAggregate aggregate(
				std::make_unique<logical::LogicalNodeScan>("n"), {rank},
				{{"sum", rank, "total", false}, {"count", entity, "entity_count", false}}, {"rank"});

		std::map<int64_t, std::pair<int64_t, int64_t>> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			const int64_t group = std::get<int64_t>(std::get<PropertyValue>(row.values.at("rank")).getVariant());
			results.emplace(group, std::make_pair(
					std::get<int64_t>(std::get<PropertyValue>(row.values.at("total")).getVariant()),
					std::get<int64_t>(std::get<PropertyValue>(row.values.at("entity_count")).getVariant())));
			return true;
		}));

		ASSERT_EQ(results.size(), 3U);
		for (const int64_t value: {1, 2, 3}) {
			EXPECT_EQ(results.at(value), (std::pair<int64_t, int64_t>{value, 1}));
		}
		// The complete entity count remains semantically evaluated, while the
		// independent rank group/sum expressions share one selected column batch.
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys,
				  (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, SharesOneBatchAcrossGroupedAndMultipleAggregatePropertyExpressions) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto group = std::make_shared<expressions::VariableReferenceExpression>("n", "rank");
		auto value = std::make_shared<expressions::VariableReferenceExpression>("n", "rank");
		logical::LogicalAggregate aggregate(
				std::make_unique<logical::LogicalNodeScan>("n"), {group},
				{{"sum", value, "total", false}, {"avg", value, "average", false}}, {"rank"});

		std::map<int64_t, std::pair<int64_t, double>> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			const int64_t rank = std::get<int64_t>(std::get<PropertyValue>(row.values.at("rank")).getVariant());
			results.emplace(rank,
					std::make_pair(std::get<int64_t>(std::get<PropertyValue>(row.values.at("total")).getVariant()),
							std::get<double>(std::get<PropertyValue>(row.values.at("average")).getVariant())));
			return true;
		}));

		ASSERT_EQ(results.size(), 3U);
		EXPECT_EQ(results.at(1).first, 1);
		EXPECT_EQ(results.at(2).first, 2);
		EXPECT_EQ(results.at(3).first, 3);
		EXPECT_DOUBLE_EQ(results.at(1).second, 1.0);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AggregatesEdgePropertiesThroughOneSelectedColumnBatch) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		logical::LogicalAggregate aggregate(
				std::move(traversal), {},
				{{"sum", std::make_shared<expressions::VariableReferenceExpression>("r", "weight"), "total", false}});

		std::vector<PropertyValue> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			results.push_back(std::get<PropertyValue>(row.values.at("total")));
			return true;
		}));

		EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{22})});
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_ref_scans, 1U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CountsEdgePropertyMatchesThroughSortedColumnsWithoutRowMaterialization) {
		DirectEdgeGraph graph = directGraph();
		std::swap(graph.edges[0], graph.edges[1]);
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto seed = std::make_unique<logical::LogicalNodeScan>("a");
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::move(seed), "a", "r", "b", "KNOWS", "out", std::vector<std::string>{},
				std::vector<std::pair<std::string, PropertyValue>>{},
				std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue(7)}, {"tier", PropertyValue(1)}});
		logical::LogicalAggregate aggregate(
				std::move(traversal), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("r"), "count", false}});

		std::vector<PropertyValue> results;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
					results.push_back(std::get<PropertyValue>(row.values.at("count")));
					return true;
				});

		EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{1})});
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys,
				  (std::vector<std::vector<std::string>>{{"tier", "weight"}}));
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
		EXPECT_EQ(stats.edges_scanned, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, FusesFilteredEdgeCountIntoTheSelectedColumnScan) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		auto weight = std::make_unique<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(int64_t{7}));
		auto tier = std::make_unique<expressions::BinaryOpExpression>(
				std::make_unique<expressions::LiteralExpression>(int64_t{1}),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::VariableReferenceExpression>("r", "tier"));
		auto filter = std::make_unique<logical::LogicalFilter>(
				std::move(traversal), std::make_shared<expressions::BinaryOpExpression>(
						std::move(weight), expressions::BinaryOperatorType::BOP_AND, std::move(tier)));
		logical::LogicalAggregate aggregate(
				std::move(filter), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("r"), "count", false}});

		std::vector<PropertyValue> results;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
					results.push_back(std::get<PropertyValue>(row.values.at("count")));
					return true;
				});

		EXPECT_EQ(results, (std::vector<PropertyValue>{PropertyValue(int64_t{1})}));
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys,
				  (std::vector<std::vector<std::string>>{{"weight", "tier"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
		EXPECT_EQ(graph.node_existence_batch_probes, 1U);
		EXPECT_EQ(stats.edges_scanned, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, FilteredEdgeCountValidatesEndpointsOnceAndFailsClosed) {
		DirectEdgeGraph graph = directGraph();
		graph.nodes.erase(3U);
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(int64_t{7}));
		auto filter = std::make_unique<logical::LogicalFilter>(std::move(traversal), std::move(predicate));
		logical::LogicalAggregate aggregate(
				std::move(filter), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("r"), "count", false}});

		EXPECT_THROW(static_cast<void>(runtime.execute(
				aggregate, directBounds(), [](const LogicalPlanReadRow &) { return true; })), std::runtime_error);
		EXPECT_EQ(graph.node_existence_batch_probes, 1U);
		EXPECT_EQ(graph.edge_ref_scans, 1U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ShortCircuitsContradictoryInlineAndFilterEdgeCounts) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out",
				std::vector<std::string>{}, std::vector<std::pair<std::string, PropertyValue>>{},
				std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue(7)}});
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(int64_t{8}));
		auto filter = std::make_unique<logical::LogicalFilter>(std::move(traversal), std::move(predicate));
		logical::LogicalAggregate aggregate(
				std::move(filter), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("r"), "count", false}});

		std::vector<PropertyValue> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			results.push_back(std::get<PropertyValue>(row.values.at("count")));
			return true;
		}));

		EXPECT_EQ(results, (std::vector<PropertyValue>{PropertyValue(int64_t{0})}));
		EXPECT_EQ(graph.edge_ref_scans, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CountsUnconstrainedEdgesThroughReferencesWithoutTypeMaterialization) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto seed = std::make_unique<logical::LogicalNodeScan>("a");
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::move(seed), "a", "r", "b", "", "out");
		logical::LogicalAggregate aggregate(
				std::move(traversal), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("r"), "count", false}});

		std::vector<PropertyValue> results;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
					results.push_back(std::get<PropertyValue>(row.values.at("count")));
					return true;
				});

		EXPECT_EQ(results, (std::vector<PropertyValue>{PropertyValue(int64_t{3})}));
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.edge_ref_records, 3U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(stats.edges_scanned, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, DoesNotMaterializeUnboundTopologyOnlyTargetNodes) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "", "KNOWS", "out");

		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U, 11U, 12U}));
		// Public materialization still validates the bound source Node, but a
		// topology-only traversal must never read an unbound target (Node 3 is
		// only a target in this graph).
		EXPECT_EQ(graph.node_read_ids, (std::vector<uint64_t>{1U, 2U}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsAnUnknownNativeRelationshipTypeBeforeScanningEdges) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "MISSING", "out");

		std::vector<uint64_t> edges;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				traversal, directBounds(), [&edges](const LogicalPlanReadRow &row) {
					edges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
					return true;
				});

		EXPECT_TRUE(edges.empty());
		EXPECT_EQ(graph.edge_ref_scans, 0U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(stats.edges_scanned, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, DoesNotApplyDirectNodeCountWhenAParameterPredicateFiltersTheScan) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
		scan->setParameterPropertyPredicates({{"rank", "rank"}});
		logical::LogicalAggregate aggregate(
				std::move(scan), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("n"), "count", false}});
		QueryContext context;
		context.parameters.emplace("rank", PropertyValue(int64_t{1}));

		std::vector<PropertyValue> results;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
					results.push_back(std::get<PropertyValue>(row.values.at("count")));
					return true;
				}, context);

		EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{1})});
		EXPECT_EQ(stats.nodes_scanned, 3U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, EnforcesGlobalAndPerSourceEdgeBounds) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto makeTraversal = [] {
			return logical::LogicalTraversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");
		};

		auto globalBounds = directBounds();
		globalBounds.maximum_edge_records = 1U;
		auto globalTraversal = makeTraversal();
		EXPECT_THROW(static_cast<void>(runtime.execute(globalTraversal, globalBounds,
															 [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);

		graph.topology_records = 0U;
		auto perSourceBounds = directBounds();
		perSourceBounds.maximum_edges_per_node = 1U;
		perSourceBounds.maximum_adjacency_page_records = 1U;
		auto perSourceTraversal = makeTraversal();
		std::vector<uint64_t> edges;
		static_cast<void>(runtime.execute(perSourceTraversal, perSourceBounds,
													 [&edges](const LogicalPlanReadRow &row) {
														edges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
														return true;
													 }));
		EXPECT_EQ(edges, (std::vector<uint64_t>{10U, 12U}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AcceptsUnorderedPhysicalTopologyAndHonorsEarlyStop) {
		DirectEdgeGraph unordered = directGraph();
		std::swap(unordered.edges[0], unordered.edges[1]);
		DirectEdgeBackend unorderedBackend(unordered);
		LogicalPlanReadRuntime unorderedRuntime(unorderedBackend);
		logical::LogicalTraversal unorderedTraversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");
		std::vector<uint64_t> unorderedEdges;
		static_cast<void>(unorderedRuntime.execute(unorderedTraversal, directBounds(), [&unorderedEdges](const LogicalPlanReadRow &row) {
			unorderedEdges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));
		EXPECT_EQ(unorderedEdges, (std::vector<uint64_t>{11U, 10U, 12U}));
		EXPECT_EQ(unordered.edge_ref_scans, 1U);
		EXPECT_EQ(unordered.topology_scans, 0U);

		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");
		std::vector<uint64_t> sourceNodes;
		static_cast<void>(runtime.execute(traversal, directBounds(), [](const LogicalPlanReadRow &) { return false; }));
		EXPECT_EQ(graph.edge_ref_records, 1U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.adjacency_scans, 0U);

		graph.edge_ref_records = 0U;
		logical::LogicalTraversal materializedSourceTraversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");
		static_cast<void>(runtime.execute(materializedSourceTraversal, directBounds(),
				[&sourceNodes](const LogicalPlanReadRow &row) {
					sourceNodes.push_back(std::get<LogicalGraphNode>(row.values.at("a")).node_id);
					return true;
				}));
		EXPECT_EQ(sourceNodes, (std::vector<uint64_t>{1U, 1U, 2U}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, LimitStopsTopologyOnlyDirectTraversalBeforeReadAhead) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		logical::LogicalLimit limit(std::move(traversal), 1);

		const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setValueCollectionEnabled(true);
		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(limit, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));
		const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

		EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U}));
		// A topology-only direct traversal uses its single-record path, allowing
		// LIMIT to stop the native cursor before it observes another Edge.
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.edge_ref_records, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 0U);
		// One shaped read is necessary to bind the returned relationship, but no
		// additional relationship can be materialized after LIMIT has stopped it.
		EXPECT_EQ(graph.edge_shaped_reads, 1U);
		ASSERT_TRUE(trace.contains("logical_plan.direct_edge_records"));
		EXPECT_EQ(trace.at("logical_plan.direct_edge_records").totalValue, 1);
		ASSERT_TRUE(trace.contains("logical_plan.limit.delivered_rows"));
		EXPECT_EQ(trace.at("logical_plan.limit.delivered_rows").totalValue, 1);
		ASSERT_TRUE(trace.contains("logical_plan.limit.early_stop"));
		EXPECT_EQ(trace.at("logical_plan.limit.early_stop").totalValue, 1);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsAnUnincrementableGlobalEdgeBound) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");
		auto bounds = directBounds();
		bounds.maximum_edge_records = std::numeric_limits<size_t>::max();
		EXPECT_THROW(static_cast<void>(runtime.execute(
					 traversal, bounds, [](const LogicalPlanReadRow &) { return true; })),
					 std::invalid_argument);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.edge_ref_scans, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchAdjacencyCountPreservesDirectionAndSelfLoopSemantics) {
		for (const auto &[direction, expected]:
				 std::vector<std::pair<std::string, int64_t>>{{"out", 4}, {"in", 4}, {"both", 7}}) {
			DirectEdgeGraph graph = directGraph();
			graph.enable_batch_adjacency_reference_scan = true;
			graph.edges.emplace_back(13U, 1U, 1U, "KNOWS",
					std::vector<storage::LogicalGraphPropertyEntry>{});
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			auto traversal = std::make_unique<logical::LogicalTraversal>(
					std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", direction);
			logical::LogicalAggregate aggregate(
					std::move(traversal), {},
					{{"count", std::make_shared<expressions::VariableReferenceExpression>("b"), "count", false}});

			std::vector<PropertyValue> results;
			static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
				results.push_back(std::get<PropertyValue>(row.values.at("count")));
				return true;
			}));

			EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(expected)}) << direction;
			EXPECT_EQ(graph.batch_adjacency_ref_scans, direction == "both" ? 2U : 1U) << direction;
			EXPECT_EQ(graph.edge_ref_scans, 0U) << direction;
			EXPECT_EQ(graph.adjacency_ref_scans, 0U) << direction;
			EXPECT_GT(graph.node_existence_batch_probes, 0U) << direction;
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchAdjacencyCountAppliesTypeLabelsAndEndpointLiveness) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_batch_adjacency_reference_scan = true;
		graph.edges.emplace_back(13U, 1U, 2U, "OTHER",
				std::vector<storage::LogicalGraphPropertyEntry>{});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out",
				std::vector<std::string>{"Person"});
		logical::LogicalAggregate aggregate(
				std::move(traversal), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("b"), "count", false}});

		std::vector<PropertyValue> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			results.push_back(std::get<PropertyValue>(row.values.at("count")));
			return true;
		}));

		EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{2})});
		EXPECT_EQ(graph.batch_adjacency_ref_scans, 1U);
		EXPECT_GT(graph.node_label_sorted_reads, 0U);

		graph.nodes.erase(3U);
		auto invalidTraversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		logical::LogicalAggregate invalidAggregate(
				std::move(invalidTraversal), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("b"), "count", false}});
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidAggregate, directBounds(),
													   [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NativeAdjacencySkipsTargetIoAfterEdgePredicatesRejectTheBatch) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>(
						"a", std::vector<std::string>{"Person"},
						std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(int64_t{1})}}),
				"a", "r", "b", "KNOWS", "out", std::vector<std::string>{},
				std::vector<std::pair<std::string, PropertyValue>>{},
				std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue()}});

		std::vector<LogicalPlanReadRow> rows;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&rows](const LogicalPlanReadRow &row) {
			rows.push_back(row);
			return true;
		}));

		EXPECT_TRUE(rows.empty());
		EXPECT_EQ(graph.edge_property_column_batch_reads, 0U);
		EXPECT_EQ(graph.node_existence_batch_probes, 0U);
		EXPECT_EQ(graph.node_label_sorted_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U); // Source predicate only.
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NativeAdjacencyRejectsInvalidTargetPropertyDefinitions) {
		for (const auto &targetProperties:
			 {std::vector<std::pair<std::string, PropertyValue>>{{"", PropertyValue(int64_t{2})}},
			  std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(int64_t{2})},
																 {"rank", PropertyValue(int64_t{2})}}}) {
			DirectEdgeGraph graph = directGraph();
			graph.enable_adjacency_reference_pages = true;
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			logical::LogicalTraversal traversal(
					std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Person"}), "a", "r", "b",
					"KNOWS", "out", std::vector<std::string>{}, targetProperties);

			EXPECT_THROW(static_cast<void>(runtime.execute(traversal, directBounds(),
														   [](const LogicalPlanReadRow &) { return true; })),
						 std::invalid_argument);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NativeAdjacencyTreatsTargetLabelMissAsAFilter) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>(
						"a", std::vector<std::string>{},
						std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(int64_t{1})}}),
				"a", "r", "b", "KNOWS", "out", std::vector<std::string>{"Employee"});

		std::vector<LogicalPlanReadRow> rows;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&rows](const LogicalPlanReadRow &row) {
			rows.push_back(row);
			return true;
		}));

		EXPECT_TRUE(rows.empty());
		EXPECT_GT(graph.node_label_sorted_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NativeAdjacencyRejectsMissingEndpointsAndBatchCardinalityChanges) {
		auto makeTraversal = [] {
			return logical::LogicalTraversal(
					std::make_unique<logical::LogicalNodeScan>(
							"a", std::vector<std::string>{},
							std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(int64_t{1})}}),
					"a", "r", "b", "KNOWS", "out");
		};
		for (const auto &configure: {
					 std::function<void(DirectEdgeGraph &)>{[](DirectEdgeGraph &graph) { graph.nodes.erase(3U); }},
					 std::function<void(DirectEdgeGraph &)>{
							 [](DirectEdgeGraph &graph) { graph.truncate_node_existence_batch = true; }},
			 }) {
			DirectEdgeGraph graph = directGraph();
			graph.enable_adjacency_reference_pages = true;
			configure(graph);
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			auto traversal = makeTraversal();
			EXPECT_THROW(static_cast<void>(runtime.execute(traversal, directBounds(),
														   [](const LogicalPlanReadRow &) { return true; })),
						 std::runtime_error);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NativeAdjacencyRejectsCorruptedSelectedColumnCardinality) {
		for (const bool corruptEdgeColumn: {false, true}) {
			DirectEdgeGraph graph = directGraph();
			graph.enable_adjacency_reference_pages = true;
			graph.truncate_edge_property_batch = corruptEdgeColumn;
			graph.truncate_node_property_batch = !corruptEdgeColumn;
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			logical::LogicalTraversal traversal(
					std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Person"}), "a", "r", "b",
					"KNOWS", "out", std::vector<std::string>{},
					std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(int64_t{2})}},
					std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue(int64_t{7})}});

			EXPECT_THROW(static_cast<void>(runtime.execute(traversal, directBounds(),
														   [](const LogicalPlanReadRow &) { return true; })),
						 std::runtime_error);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AnchoredCountReservesOnlyDemandedPendingCapacity) {
		DirectEdgeGraph graph = directGraph(); graph.enable_adjacency_reference_pages = true;
		DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		logical::LogicalAggregate aggregate(std::move(traversal), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("b"), "count", false}});
		auto bounds = directBounds(); bounds.maximum_edges_per_node = 256U;
		bounds.maximum_edge_records = 256U; bounds.maximum_adjacency_page_records = 256U;
		bounds.maximum_materialized_bytes = 4096U;
		const bool previous = debug::PerfTrace::areValuesEnabled();
		debug::PerfTrace::setValueCollectionEnabled(true); debug::PerfTrace::reset();
		PropertyValue count;
		try { static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
			count = std::get<PropertyValue>(row.values.at("count")); return true;
		})); } catch (...) { debug::PerfTrace::setValueCollectionEnabled(previous); throw; }
		const auto trace = debug::PerfTrace::snapshotAndReset();
		debug::PerfTrace::setValueCollectionEnabled(previous);
		EXPECT_EQ(count, PropertyValue(3));
		ASSERT_TRUE(trace.contains("logical_plan.anchored_pending_capacity"));
		EXPECT_EQ(trace.at("logical_plan.anchored_pending_capacity").totalValue, 32);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AnchoredLabelBitsRejectMalformedProbeAndRetryReferenceSource) {
		DirectEdgeGraph graph = directGraph(); graph.enable_adjacency_reference_pages = true;
		graph.truncate_node_label_batch = true;
		DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out",
			std::vector<std::string>{"Person"});
		logical::LogicalAggregate aggregate(std::move(traversal), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("b"), "count", false}});
		auto bounds = directBounds(); bounds.maximum_materialized_bytes = 4096U;
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.execute(aggregate, bounds,
			[&](const LogicalPlanReadRow &) { ++delivered; return true; })), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		EXPECT_GT(graph.node_label_sorted_reads, 0U);
		graph.truncate_node_label_batch = false;
		PropertyValue count;
		static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
			count = std::get<PropertyValue>(row.values.at("count")); ++delivered; return true;
		}));
		EXPECT_EQ(count, PropertyValue(2)); EXPECT_EQ(delivered, 1U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AnchoredLabelsRemainCorrectBeyondOptionalCacheLimit) {
		DirectEdgeGraph graph; graph.enable_adjacency_reference_pages = true;
		graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Seed"}});
		for (uint64_t index = 0U; index < 70U; ++index) {
			const uint64_t id = index + 2U;
			graph.nodes.emplace(id, LogicalGraphNode{id, {index % 2U ? "Other" : "Person"}});
			graph.edges.emplace_back(index + 10U, 1U, id, "KNOWS");
		}
		for (uint64_t index = 0U; index < 10U; ++index) graph.edges.emplace_back(index + 100U, 1U, 70U, "KNOWS");
		DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Seed"}),
			"a", "r", "b", "KNOWS", "out", std::vector<std::string>{"Person"});
		logical::LogicalAggregate aggregate(std::move(traversal), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("b"), "count", false}});
		auto bounds = directBounds(); bounds.maximum_node_records = 128U;
		bounds.maximum_edges_per_node = 128U; bounds.maximum_edge_records = 128U;
		bounds.maximum_adjacency_page_records = 8U; bounds.maximum_rows = 128U;
		bounds.maximum_materialized_bytes = 32768U;
		PropertyValue count;
		static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
			count = std::get<PropertyValue>(row.values.at("count")); return true;
		}));
		EXPECT_EQ(count, PropertyValue(45));
		EXPECT_TRUE(std::ranges::any_of(graph.node_label_sorted_ids, [](const auto &ids) {
			return ids == std::vector<uint64_t>{70U};
		}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AnchoredPropertiesRemainCorrectBeyondOptionalCacheLimit) {
		DirectEdgeGraph graph; graph.enable_adjacency_reference_pages = true;
		graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Seed"}});
		for (uint64_t index = 0U; index < 70U; ++index) {
			const uint64_t id = index + 2U;
			graph.nodes.emplace(id, LogicalGraphNode{id, {}, {{"rank", PropertyValue(index % 2U ? 0 : 1)}}});
			graph.edges.emplace_back(index + 10U, 1U, id, "KNOWS");
		}
		for (uint64_t index = 0U; index < 10U; ++index) graph.edges.emplace_back(index + 100U, 1U, 70U, "KNOWS");
		DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Seed"}),
			"a", "r", "b", "KNOWS", "out", std::vector<std::string>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)}});
		logical::LogicalAggregate aggregate(std::move(traversal), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("b"), "count", false}});
		auto bounds = directBounds(); bounds.maximum_node_records = 128U;
		bounds.maximum_edges_per_node = 128U; bounds.maximum_edge_records = 128U;
		bounds.maximum_adjacency_page_records = 8U; bounds.maximum_rows = 128U;
		bounds.maximum_materialized_bytes = 32768U;
		PropertyValue count;
		static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
			count = std::get<PropertyValue>(row.values.at("count")); return true;
		}));
		EXPECT_EQ(count, PropertyValue(45));
		EXPECT_TRUE(std::ranges::any_of(graph.node_property_column_batch_ids, [](const auto &ids) {
			return ids == std::vector<uint64_t>{70U};
		}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AnchoredPropertiesBorrowLongDuplicateKeysWithinBudget) {
		DirectEdgeGraph graph = directGraph(); graph.enable_adjacency_reference_pages = true;
		graph.enable_property_read_admission = true;
		graph.nodes.at(1U).labels = {"Seed"}; graph.nodes.at(1U).properties.clear();
		const std::string key(8192U, 'k');
		graph.nodes.at(2U).properties.emplace_back(key, PropertyValue(1));
		graph.nodes.at(3U).properties.emplace_back(key, PropertyValue(1));
		DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Seed"}),
			"a", "r", "b", "KNOWS", "out", std::vector<std::string>{},
			std::vector<std::pair<std::string, PropertyValue>>{{key, PropertyValue(1)}, {key, PropertyValue(1)}});
		logical::LogicalAggregate aggregate(std::move(traversal), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("b"), "count", false}});
		auto bounds = directBounds(); bounds.maximum_materialized_bytes = 4096U;
		PropertyValue count;
		static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
			count = std::get<PropertyValue>(row.values.at("count")); return true;
		}));
		EXPECT_EQ(count, PropertyValue(2));
		ASSERT_FALSE(graph.node_property_column_batch_keys.empty());
		for (const auto &keys: graph.node_property_column_batch_keys) EXPECT_EQ(keys, std::vector<std::string>{key});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AggregateObjectAdmissionRejectsBeforeScanningAndAllowsFreshExecution) {
		DirectEdgeGraph graph;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalAggItem> items;
		for (size_t index = 0U; index < 128U; ++index)
			items.push_back({"min", std::make_shared<expressions::VariableReferenceExpression>("n"),
				"m" + std::to_string(index), false});
		logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {}, std::move(items));
		auto bounds = directBounds();
		bounds.maximum_materialized_bytes = 12000U;
		size_t delivered = 0U;
		try {
			static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &) {
				++delivered; return true;
			}));
			FAIL() << "aggregate objects must be admitted before allocation";
		} catch (const std::runtime_error &error) {
			EXPECT_NE(std::string(error.what()).find("Aggregate accumulator object exceeded its materialized byte bound"), std::string::npos);
		}
		EXPECT_EQ(delivered, 0U);
		bounds.maximum_materialized_bytes = 1024U * 1024U;
		static_cast<void>(runtime.execute(aggregate, bounds, [&](const LogicalPlanReadRow &row) {
			++delivered; EXPECT_EQ(row.values.size(), 128U);
			for (const auto &[key, value] : row.values)
				EXPECT_EQ(std::get<PropertyValue>(value).getType(), PropertyType::NULL_TYPE);
			return true;
		}));
		EXPECT_EQ(delivered, 1U);
	}

} // namespace graph::query::execution
