#include "CypherParserImpl.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"

namespace graph::query::execution {
	using namespace direct_edge_test;

	TEST(LogicalPlanDirectEdgeRuntimeTest, NodePredicateBufferLeavesRoomForHeapPayloadsAcrossManyBatches) {
		DirectEdgeGraph graph = directGraph();
		for (uint64_t id = 1U; id <= 51U; ++id) {
			auto node = graph.nodes.at(1U);
			node.node_id = id;
			node.properties = {{"payload", PropertyValue(std::string(1000U, 'a'))}};
			graph.nodes.insert_or_assign(id, std::move(node));
		}
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		graph::parser::cypher::CypherParserImpl parser;
		const auto plan =
				parser.parseToLogical("MATCH (n {payload: '" + std::string(1000U, 'a') + "'}) RETURN id(n) AS id");
		auto bounds = directBounds();
		bounds.maximum_node_records = 64U;
		bounds.maximum_rows = 64U;
		// Fit one payload and a small reference window with output/cache staging;
		// the full 51-payload column must remain far larger than this allowance.
		bounds.maximum_materialized_bytes = 1000U +
			4U * (sizeof(LogicalPlanReadRow) + sizeof(LogicalPlanReadValue) + sizeof(PropertyValue)) + 512U;
		ASSERT_LT(bounds.maximum_materialized_bytes, 51U * 1000U);
		std::vector<int64_t> ids;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, bounds, [&](const LogicalPlanReadRow &row) {
			ids.push_back(std::get<int64_t>(std::get<PropertyValue>(row.values.at("id")).getVariant()));
			return true;
		})));
		ASSERT_EQ(ids.size(), 51U);
		for (size_t index = 0U; index < ids.size(); ++index)
			EXPECT_EQ(ids[index], static_cast<int64_t>(index + 1U));
		EXPECT_GT(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_TRUE(graph.node_read_ids.empty());
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NodePredicateSplitPreservesScanOrderMissingCellsAndCancellation) {
		DirectEdgeGraph graph = directGraph();
		graph.node_ref_scan_order = {3U, 1U, 2U};
		for (auto &[id, node]: graph.nodes)
			node.properties.emplace_back("payload", PropertyValue(std::string(1000U, 'a')));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		graph::parser::cypher::CypherParserImpl parser;
		const auto plan =
				parser.parseToLogical("MATCH (n {payload: '" + std::string(1000U, 'a') + "'}) RETURN id(n) AS id");
		auto bounds = directBounds();
		bounds.maximum_materialized_bytes = 2000U;
		for (bool stop: {false, true, false}) {
			graph.node_property_column_batch_reads = 0U;
			std::vector<int64_t> ids;
			EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, bounds, [&](const LogicalPlanReadRow &row) {
				ids.push_back(std::get<int64_t>(std::get<PropertyValue>(row.values.at("id")).getVariant()));
				return !stop;
			})));
			EXPECT_EQ(ids, stop ? (std::vector<int64_t>{3}) : (std::vector<int64_t>{3, 1, 2}));
			EXPECT_GT(graph.node_property_column_batch_reads, 1U);
		}
		graph.nodes.at(1U).properties.pop_back();
		std::vector<int64_t> ids;
		static_cast<void>(runtime.execute(*plan.root, bounds, [&](const LogicalPlanReadRow &row) {
			ids.push_back(std::get<int64_t>(std::get<PropertyValue>(row.values.at("id")).getVariant()));
			return true;
		}));
		EXPECT_EQ(ids, (std::vector<int64_t>{3, 2}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_TRUE(graph.node_read_ids.empty());
		graph.nodes.at(3U).properties.back().value = PropertyValue(std::string(4000U, 'a'));
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.execute(*plan.root, bounds,
													   [&](const LogicalPlanReadRow &) {
														   ++delivered;
														   return true;
													   })),
					 std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		graph.truncate_node_property_batch = true;
		graph.node_property_column_batch_reads = 0U;
		try {
			static_cast<void>(runtime.execute(*plan.root, bounds, [](const LogicalPlanReadRow &) { return true; }));
			FAIL() << "invalid column shape must fail before byte-pressure splitting";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical graph backend changed sorted Node Property result cardinality");
		}
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, PropertyColumnLastConsumerPreservesRepeatedEntityPayloads) {
		for (const bool isEdge: {false, true}) {
			SCOPED_TRACE(isEdge);
			DirectEdgeGraph graph = directGraph();
			graph.enable_property_tokens = true;
			for (auto &[id, node]: graph.nodes) {
				node.properties.emplace_back("payload",
											 PropertyValue(std::string(1000U, static_cast<char>('a' + id - 1U))));
			}
			for (size_t index = 0U; index < graph.edges.size(); ++index) {
				graph.edges[index].properties.emplace_back(
						"payload", PropertyValue(std::string(1000U, static_cast<char>('a' + index))));
			}
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			graph::parser::cypher::CypherParserImpl parser;
			const auto plan = parser.parseToLogical(
					isEdge ? "MATCH ()-[r:KNOWS]->() UNWIND [1, 2] AS repeat RETURN r.payload AS payload"
						   : "MATCH (n) UNWIND [1, 2] AS repeat RETURN n.payload AS payload");
			auto bounds = directBounds();
			bounds.maximum_materialized_bytes = 16000U;
			for (const bool stopAfterFirst: {false, true, false}) {
				SCOPED_TRACE(stopAfterFirst);
				graph.node_property_column_batch_reads = 0U;
				graph.edge_property_column_batch_reads = 0U;
				std::vector<PropertyValue> delivered;
				static_cast<void>(runtime.execute(*plan.root, bounds, [&](const LogicalPlanReadRow &row) {
					delivered.push_back(std::get<PropertyValue>(row.values.at("payload")));
					return !stopAfterFirst;
				}));
				std::vector<PropertyValue> expected;
				for (char value: {'a', 'a', 'b', 'b', 'c', 'c'})
					expected.emplace_back(std::string(1000U, value));
				if (stopAfterFirst)
					expected.resize(1U);
				EXPECT_EQ(delivered, expected);
				EXPECT_EQ(isEdge ? graph.edge_property_column_batch_reads : graph.node_property_column_batch_reads, 1U);
				EXPECT_EQ(graph.node_property_point_reads, 0U);
				EXPECT_EQ(graph.edge_property_point_reads, 0U);
			}
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CompleteNodeAndEdgeCachesShareTheExecutionBudgetAndReuseRecords) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		graph.nodes.erase(3U);
		graph.edges.resize(1U);
		graph.nodes.at(1U).properties.emplace_back("payload", PropertyValue(std::string(2000U, 'n')));
		graph.edges.front().properties.emplace_back("payload", PropertyValue(std::string(2000U, 'r')));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		graph::parser::cypher::CypherParserImpl parser;
		const auto nodeOnly = parser.parseToLogical("MATCH (n) RETURN size(keys(n)) AS keys");
		const auto edgeOnly = parser.parseToLogical("MATCH ()-[r:KNOWS]->() RETURN size(keys(r)) AS keys");
		const std::string query =
				"MATCH (n)-[r:KNOWS]->(m) RETURN size(keys(n)) AS nodeKeys, size(keys(r)) AS edgeKeys";
		const auto combined = parser.parseToLogical(query);
		auto bounds = directBounds();
		bool sharedCacheRejected = false;
		// Find a same-budget boundary where each entity kind fits separately,
		// but the combined query cannot retain both complete caches.
		for (size_t allowance = 4000U; allowance < 16000U; allowance += 64U) {
			bounds.maximum_materialized_bytes = allowance;
			try {
				EXPECT_EQ(runtime.executeToQueryResult(*nodeOnly.root, bounds).rowCount(), 2U);
				EXPECT_EQ(runtime.executeToQueryResult(*edgeOnly.root, bounds).rowCount(), 1U);
			} catch (const std::runtime_error &) { continue; }
			size_t delivered = 0U;
			try {
				static_cast<void>(runtime.execute(*combined.root, bounds, [&](const LogicalPlanReadRow &) {
					++delivered;
					return true;
				}));
				break;
			} catch (const std::runtime_error &error) {
				EXPECT_NE(std::string(error.what()).find("byte"), std::string::npos);
				EXPECT_EQ(delivered, 0U);
				sharedCacheRejected = true;
				break;
			}
		}
		EXPECT_TRUE(sharedCacheRejected)
				<< "no shared-cache rejection at an allowance admitting each entity kind separately";
		bounds.maximum_materialized_bytes = 16000U;
		const auto repeated = parser.parseToLogical(query + " UNION ALL " + query);
		for (size_t execution = 0U; execution < 2U; ++execution) {
			SCOPED_TRACE(execution);
			graph.node_shaped_batch_reads = 0U;
			graph.edge_shaped_batch_reads = 0U;
			const auto result = runtime.executeToQueryResult(*repeated.root, bounds);
			ASSERT_EQ(result.rowCount(), 2U);
			for (const auto &row: result.getRows()) {
				EXPECT_EQ(row.at("nodeKeys").asPrimitive(), PropertyValue(2));
				EXPECT_EQ(row.at("edgeKeys").asPrimitive(), PropertyValue(3));
			}
			EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
			EXPECT_EQ(graph.edge_shaped_batch_reads, 1U);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AppliesSourceEdgeAndTargetPredicatesWithoutAdjacencyDescent) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)}});
		logical::LogicalTraversal traversal(
				std::move(source), "a", "r", "b", "KNOWS", "out", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(2)}},
				PropertyValue::MapType{{"weight", PropertyValue(7)}});

		std::vector<uint64_t> edges;
		const LogicalPlanReadRuntimeStats stats =
				runtime.execute(traversal, directBounds(), [&edges](const LogicalPlanReadRow &row) {
					edges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
					return true;
				});

		EXPECT_EQ(edges, std::vector<uint64_t>{10U});
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.edge_ref_records, 3U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight"}}));
		EXPECT_EQ(stats.nodes_scanned, 3U);
		EXPECT_EQ(stats.edges_scanned, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesEdgePredicatesOnAnchoredReferencePages) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)}});
		logical::LogicalTraversal traversal(std::move(source), "a", "r", "b", "KNOWS", "out", {}, {},
											PropertyValue::MapType{{"weight", PropertyValue(7)}});

		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U, 11U}));
		EXPECT_EQ(graph.adjacency_ref_scans, 1U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsOversizedAnchoredEdgePredicateBatchBeforeFullEdgeRead) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.edges.front().properties.emplace_back(
				"payload", PropertyValue(PropertyValue::Bytes(2U * 1024U * 1024U, std::byte{0})));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)}});
		logical::LogicalTraversal traversal(std::move(source), "a", "r", "b", "KNOWS", "out", {}, {},
											PropertyValue::MapType{{"payload", PropertyValue(PropertyValue::Bytes(1U, std::byte{1}))}});
		LogicalPlanReadRuntimeBounds bounds = directBounds();
		bounds.maximum_materialized_bytes = 1U * 1024U * 1024U;

		// The payload is requested only for the predicate and matches no Edge. The
		// selected-column staging boundary must still fail before full Edge materialization.
		EXPECT_THROW(
				static_cast<void>(runtime.execute(traversal, bounds, [](const LogicalPlanReadRow &) { return true; })),
				std::runtime_error);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_shaped_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesEveryEdgePredicateAndPreservesSelectedValues) {
		DirectEdgeGraph graph = directGraph();
		std::swap(graph.edges[0], graph.edges[1]);
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)}});
		logical::LogicalTraversal traversal(std::move(source), "a", "r", "b", "KNOWS", "out", {}, {},
											PropertyValue::MapType{{"weight", PropertyValue(7)}, {"tier", PropertyValue(1)}});

		std::vector<uint64_t> edges;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edges](const LogicalPlanReadRow &row) {
			edges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edges, std::vector<uint64_t>{10U});
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_property_sorted_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"tier", "weight"}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, DeduplicatesPropertyReadsButPreservesDuplicateEdgeRows) {
		DirectEdgeGraph graph = directGraph();
		graph.duplicate_first_edge_reference = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS",
											"out", {}, {}, PropertyValue::MapType{{"weight", PropertyValue(7)}});

		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U, 10U, 11U}));
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight"}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, SplitsLargeEdgeProjectionIntoBoundedBatches) {
		DirectEdgeGraph graph = directGraph();
		for (uint64_t edgeId = 100U; edgeId < 800U; ++edgeId) {
			graph.edges.emplace_back(edgeId, 1U, 2U, "KNOWS",
									 std::vector<storage::LogicalGraphPropertyEntry>{{"weight", PropertyValue(7)}});
		}
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS",
											"out", {}, {}, PropertyValue::MapType{{"weight", PropertyValue(7)}});
		auto bounds = directBounds();
		bounds.maximum_edge_records = 1024U;
		bounds.maximum_edges_per_node = 1024U;
		bounds.maximum_rows = 1024U;
		bounds.maximum_node_records = 16U;
		size_t resultCount = 0U;
		static_cast<void>(runtime.execute(traversal, bounds, [&resultCount](const LogicalPlanReadRow &) {
			++resultCount;
			return true;
		}));
		EXPECT_EQ(resultCount, 702U);
		// The bounded batch capacity is intentionally smaller than this input; no
		// unbounded Edge/property vector is retained by the runtime.
		EXPECT_EQ(graph.edge_property_column_batch_reads, 2U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesEdgeFilterPropertyAndReusesItForProjection) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>("a");
		auto traversal = std::make_unique<logical::LogicalTraversal>(std::move(source), "a", "r", "b", "KNOWS", "out");
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(int64_t{7}));
		auto filter = std::make_unique<logical::LogicalFilter>(std::move(traversal), std::move(predicate));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "weight"), "weight");
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b"), "target");
		logical::LogicalProject project(std::move(filter), std::move(items));

		std::vector<PropertyValue> weights;
		const LogicalPlanReadRuntimeStats stats =
				runtime.execute(project, directBounds(), [&weights](const LogicalPlanReadRow &row) {
					weights.push_back(std::get<PropertyValue>(row.values.at("weight")));
					return true;
				});

		EXPECT_EQ(weights, (std::vector<PropertyValue>{PropertyValue(7), PropertyValue(7)}));
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(stats.edges_scanned, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, GlobalEdgePropertyPathDoesNotScanUnconstrainedSources) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("source"), "source",
											"relationship", "target", "KNOWS", "out", std::vector<std::string>{},
											std::vector<std::pair<std::string, PropertyValue>>{},
											PropertyValue::MapType{{"weight", PropertyValue(7)}});

		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("relationship")).edge_id);
			return true;
		}));

		EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U, 11U}));
		EXPECT_EQ(graph.node_label_match_scans, 0U);
		EXPECT_EQ(graph.node_label_match_records, 0U);
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_existence_probes, 0U);
		EXPECT_EQ(graph.node_existence_batch_probes, 1U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, GlobalEdgePropertyPathFailsClosedForMissingSourceEndpoint) {
		DirectEdgeGraph graph = directGraph();
		graph.edges.emplace_back(20U, 99U, 2U, "KNOWS",
								 std::vector<storage::LogicalGraphPropertyEntry>{{"weight", PropertyValue(7)}});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("source"), "source",
											"relationship", "target", "KNOWS", "out", std::vector<std::string>{},
											std::vector<std::pair<std::string, PropertyValue>>{},
											PropertyValue::MapType{{"weight", PropertyValue(7)}});

		EXPECT_THROW(static_cast<void>(runtime.execute(traversal, directBounds(),
													   [](const LogicalPlanReadRow &) { return true; })),
					 std::runtime_error);
		EXPECT_EQ(graph.node_existence_probes, 0U);
		EXPECT_EQ(graph.node_existence_batch_probes, 1U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesMultiplePropertiesFromOneFilterExpression) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a"),
																	 "a", "r", "b", "KNOWS", "out");
		auto weight = std::make_unique<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r", "weight"),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(int64_t{7}));
		auto tier = std::make_unique<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r", "tier"),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(int64_t{1}));
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::move(weight), expressions::BinaryOperatorType::BOP_AND, std::move(tier));
		logical::LogicalFilter filter(std::move(traversal), std::move(predicate));

		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(filter, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edgeIds, std::vector<uint64_t>{10U});
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight", "tier"}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesDeferredEdgeProjectionAcrossTheTraversal) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>("a");
		auto traversal = std::make_unique<logical::LogicalTraversal>(std::move(source), "a", "r", "b", "KNOWS", "out");
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "weight"), "weight");
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "tier"), "tier");
		logical::LogicalProject project(std::move(traversal), std::move(items));

		std::vector<std::pair<PropertyValue, PropertyValue>> values;
		static_cast<void>(runtime.execute(project, directBounds(), [&values](const LogicalPlanReadRow &row) {
			values.emplace_back(std::get<PropertyValue>(row.values.at("weight")),
								std::get<PropertyValue>(row.values.at("tier")));
			return true;
		}));

		EXPECT_EQ(values, (std::vector<std::pair<PropertyValue, PropertyValue>>{{PropertyValue(7), PropertyValue(1)},
																				{PropertyValue(7), PropertyValue(2)},
																				{PropertyValue(8), PropertyValue()}}));
		EXPECT_EQ(graph.edge_property_sorted_reads, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight", "tier"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, EnforcesMaterializedByteBoundForLargeSelectedProperties) {
		DirectEdgeGraph graph = directGraph();
		graph.edges.front().properties.emplace_back(
				"payload", PropertyValue(PropertyValue::Bytes(2U * 1024U * 1024U, std::byte{0})));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a"),
																	 "a", "r", "b", "KNOWS", "out");
		std::vector<logical::LogicalSortItem> sortItems;
		sortItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "payload"), true);
		logical::LogicalSort sort(std::move(traversal), std::move(sortItems));
		LogicalPlanReadRuntimeBounds bounds = directBounds();
		bounds.maximum_materialized_bytes = 1U * 1024U * 1024U;

		try {
			static_cast<void>(runtime.execute(sort, bounds, [](const LogicalPlanReadRow &) { return true; }));
			FAIL() << "oversized selected Property columns must fail before row-cache enrichment";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical-plan Property batch exceeded its byte bound");
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsOversizedSelectedEdgePredicateBatchBeforeOutputMaterialization) {
		DirectEdgeGraph graph = directGraph();
		graph.edges.front().properties.emplace_back(
				"payload", PropertyValue(PropertyValue::Bytes(2U * 1024U * 1024U, std::byte{0})));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS",
											"out", {}, {},
											PropertyValue::MapType{{"payload", PropertyValue(PropertyValue::Bytes(1U, std::byte{1}))}});
		LogicalPlanReadRuntimeBounds bounds = directBounds();
		bounds.maximum_materialized_bytes = 1U * 1024U * 1024U;

		// The predicate rejects every Edge, so this assertion exercises the selected-column
		// staging budget rather than the public complete-entity result boundary.
		EXPECT_THROW(
				static_cast<void>(runtime.execute(traversal, bounds, [](const LogicalPlanReadRow &) { return true; })),
				std::runtime_error);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_read_ids, (std::vector<uint64_t>{10U, 11U, 12U}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, EnforcesMaterializedByteBoundAtPublicEntityResultBoundary) {
		DirectEdgeGraph graph = directGraph();
		graph.edges.front().properties.emplace_back(
				"payload", PropertyValue(PropertyValue::Bytes(2U * 1024U * 1024U, std::byte{0})));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS",
											"out");
		LogicalPlanReadRuntimeBounds limited = directBounds();
		limited.maximum_materialized_bytes = 1U * 1024U * 1024U;

		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(traversal, limited)), std::runtime_error);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RawNodeResultPreflightsItsCompletePayloadBeforePublication) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_native_node_ref_scans = false;
		graph.nodes.erase(2U);
		graph.nodes.erase(3U);
		graph.edges.clear();
		const std::string payload(600U, 'n');
		graph.nodes.at(1U).properties.emplace_back("payload", PropertyValue(payload));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		graph::parser::cypher::CypherParserImpl parser;
		const auto plan = parser.parseToLogical("MATCH (n) RETURN n");
		const auto renamedPlan = parser.parseToLogical("MATCH (n) RETURN n AS entity");
		ASSERT_NE(plan.root, nullptr);
		auto constrained = directBounds();
		bool publicationRejected = false;
		// Locate a same-budget witness: both streaming aliases fit, but owning
		// the complete public entity must still fail before result publication.
		for (size_t allowance = 1500U; allowance < 8000U; allowance += 64U) {
			constrained.maximum_materialized_bytes = allowance;
			size_t streamed = 0U;
			try {
				static_cast<void>(runtime.execute(*plan.root, constrained, [&](const LogicalPlanReadRow &) {
					++streamed;
					return true;
				}));
				EXPECT_EQ(streamed, 1U);
				streamed = 0U;
				static_cast<void>(runtime.execute(*renamedPlan.root, constrained, [&](const LogicalPlanReadRow &row) {
					EXPECT_TRUE(row.values.contains("entity"));
					EXPECT_FALSE(row.values.contains("n"));
					++streamed;
					return true;
				}));
				EXPECT_EQ(streamed, 1U);
			} catch (const std::runtime_error &) { continue; }
			try {
				static_cast<void>(runtime.executeToQueryResult(*plan.root, constrained));
				break;
			} catch (const std::runtime_error &) {
				publicationRejected = true;
				break;
			}
		}
		EXPECT_TRUE(publicationRejected) << "no publication rejection at a budget admitting both streaming aliases";
		constrained.maximum_materialized_bytes = 8000U;
		const QueryResult result = runtime.executeToQueryResult(*plan.root, constrained);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("n").asLogicalNode().properties.at("payload"), PropertyValue(payload));
		std::vector<logical::LogicalProjectItem> aliases;
		aliases.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "left");
		aliases.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "right");
		logical::LogicalProject aliasProject(std::make_unique<logical::LogicalNodeScan>("n"), std::move(aliases));
		const QueryResult aliasResult = runtime.executeToQueryResult(aliasProject, constrained);
		ASSERT_EQ(aliasResult.rowCount(), 1U);
		const auto &leftBody = std::get<std::string>(
				aliasResult.getRows().front().at("left").asLogicalNode().properties.at("payload").getVariant());
		const auto &rightBody = std::get<std::string>(
				aliasResult.getRows().front().at("right").asLogicalNode().properties.at("payload").getVariant());
		EXPECT_EQ(leftBody, payload);
		EXPECT_EQ(rightBody, payload);
		EXPECT_NE(leftBody.data(), rightBody.data());
		bool invalidKeysRejectedUnderPressure = false;
		// The valid record must still exceed the publication budget at the same
		// allowance where the malformed record reaches Property-key validation.
		// A smaller allowance can reject source staging before either boundary.
		for (size_t allowance = 1500U; allowance < 8000U; allowance += 64U) {
			constrained.maximum_materialized_bytes = allowance;
			try {
				static_cast<void>(runtime.executeToQueryResult(*plan.root, constrained));
				break;
			} catch (const std::runtime_error &) {}
			graph.nodes.at(1U).properties.emplace_back("payload", PropertyValue("duplicate"));
			try {
				static_cast<void>(runtime.executeToQueryResult(*plan.root, constrained));
			} catch (const std::runtime_error &error) {
				invalidKeysRejectedUnderPressure =
						std::string_view(error.what()) ==
						"logical graph backend returned an invalid direct Property entry set";
			}
			graph.nodes.at(1U).properties.pop_back();
			if (invalidKeysRejectedUnderPressure)
				break;
		}
		EXPECT_TRUE(invalidKeysRejectedUnderPressure)
				<< "invalid Property keys must not be hidden by result byte pressure";
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RawEdgeResultPreflightsItsCompletePayloadBeforePublication) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_native_node_ref_scans = false;
		graph.edges.resize(1U);
		const std::string payload(1000U, 'e');
		graph.edges.front().properties.emplace_back("payload", PropertyValue(payload));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out",
				std::vector<std::string>{}, std::vector<std::pair<std::string, PropertyValue>>{},
				std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue(7)}});
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r"), "edge");
		logical::LogicalProject project(std::move(traversal), std::move(items));
		auto constrained = directBounds();
		constrained.maximum_materialized_bytes = 2500U;
		size_t streamed = 0U;
		// The complete traversal row must be admitted before Project drops a/b.
		EXPECT_THROW(static_cast<void>(runtime.execute(project, constrained, [&](const LogicalPlanReadRow &row) {
			EXPECT_TRUE(std::holds_alternative<storage::LogicalGraphEdge>(row.values.at("edge")));
			EXPECT_FALSE(row.values.contains("r"));
			++streamed;
			return true;
		})), std::runtime_error);
		EXPECT_EQ(streamed, 0U);
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(project, constrained)), std::runtime_error);
		constrained.maximum_materialized_bytes = 12000U;
		const QueryResult result = runtime.executeToQueryResult(project, constrained);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("edge").asLogicalEdge().properties.at("payload"), PropertyValue(payload));
		graph.edges.front().properties.emplace_back("payload", PropertyValue("duplicate"));
		constrained.maximum_materialized_bytes = 2500U;
		try {
			static_cast<void>(runtime.executeToQueryResult(project, constrained));
			FAIL() << "invalid Edge Property keys must not be hidden by result byte pressure";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical graph backend returned an invalid direct Property entry set");
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CachedNodeReferenceResultPreservesPayloadAndRejectsDuplicates) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		graph.nodes.erase(2U);
		graph.nodes.erase(3U);
		graph.edges.clear();
		const std::string payload(600U, 'n');
		graph.nodes.at(1U).properties.emplace_back("payload", PropertyValue(payload));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n");
		auto bounds = directBounds();
		bounds.maximum_materialized_bytes = 10000U;
		const auto result = runtime.executeToQueryResult(scan, bounds);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("n").asLogicalNode().properties.at("payload"), PropertyValue(payload));
		EXPECT_GT(graph.node_shaped_batch_reads, 0U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		graph.node_shaped_batch_reads = 0U;
		std::vector<logical::LogicalProjectItem> aliases;
		aliases.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "left");
		aliases.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "right");
		logical::LogicalProject aliasProject(std::make_unique<logical::LogicalNodeScan>("n"), std::move(aliases));
		const auto aliasResult = runtime.executeToQueryResult(aliasProject, bounds);
		ASSERT_EQ(aliasResult.rowCount(), 1U);
		EXPECT_EQ(aliasResult.getRows().front().at("left").asLogicalNode().properties.at("payload"),
				  PropertyValue(payload));
		EXPECT_EQ(aliasResult.getRows().front().at("right").asLogicalNode().properties.at("payload"),
				  PropertyValue(payload));
		const auto &leftPayload = std::get<std::string>(
				aliasResult.getRows().front().at("left").asLogicalNode().properties.at("payload").getVariant());
		const auto &rightPayload = std::get<std::string>(
				aliasResult.getRows().front().at("right").asLogicalNode().properties.at("payload").getVariant());
		EXPECT_NE(leftPayload.data(), rightPayload.data());
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		graph.nodes.at(1U).properties.emplace_back("payload", PropertyValue("duplicate"));
		try {
			static_cast<void>(runtime.executeToQueryResult(scan, bounds));
			FAIL() << "duplicate complete Node properties must be rejected";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical-plan result received duplicate complete Entity properties");
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CachedEdgeReferenceResultPreservesPayloadAndRejectsDuplicates) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		graph.edges.resize(1U);
		const std::string payload(1000U, 'e');
		graph.edges.front().properties.emplace_back("payload", PropertyValue(payload));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS",
											"out");
		auto bounds = directBounds();
		bounds.maximum_materialized_bytes = 12000U;
		const auto result = runtime.executeToQueryResult(traversal, bounds);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("r").asLogicalEdge().properties.at("payload"), PropertyValue(payload));
		EXPECT_GT(graph.edge_shaped_batch_reads, 0U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		graph.edges.front().properties.emplace_back("payload", PropertyValue("duplicate"));
		try {
			static_cast<void>(runtime.executeToQueryResult(traversal, bounds));
			FAIL() << "duplicate complete Edge properties must be rejected";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical-plan result received duplicate complete Entity properties");
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, OversizedEdgeReferenceFallbackPreservesCompleteResult) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		graph.edges.resize(1U);
		const std::string payload(4U * 1024U * 1024U + 128U, 'e');
		graph.edges.front().properties.emplace_back("payload", PropertyValue(payload));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS",
											"out");
		auto bounds = directBounds();
		bounds.maximum_materialized_bytes = 32U * 1024U * 1024U;
		auto constrained = bounds;
		constrained.maximum_materialized_bytes = 5U * 1024U * 1024U;
		try {
			static_cast<void>(runtime.executeToQueryResult(traversal, constrained));
			FAIL() << "the uncached shaped Edge and its public result must fit together";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "shared materialization exceeded its byte bound");
		}
		EXPECT_GT(graph.edge_shaped_reads, 0U);
		for (size_t attempt = 0U; attempt < 2U; ++attempt) {
			graph.edge_shaped_batch_reads = 0U;
			graph.edge_shaped_reads = 0U;
			const auto result = runtime.executeToQueryResult(traversal, bounds);
			ASSERT_EQ(result.rowCount(), 1U);
			EXPECT_EQ(result.getRows().front().at("r").asLogicalEdge().properties.at("payload"),
					  PropertyValue(payload));
			EXPECT_EQ(graph.edge_shaped_batch_reads, 1U);
			EXPECT_GT(graph.edge_shaped_reads, 0U);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ShapedReferenceSourceAdmissionRejectsBeforePayloadCopy) {
		for (const bool isEdge: {false, true}) {
			SCOPED_TRACE(isEdge);
			DirectEdgeGraph graph = directGraph();
			graph.enable_property_tokens = true;
			graph.enable_shaped_property_admission = true;
			const std::string payload(4000U, isEdge ? 'e' : 'n');
			if (isEdge) {
				graph.edges.resize(1U);
				graph.edges.front().properties.emplace_back("payload", PropertyValue(payload));
			} else {
				graph.nodes.erase(2U);
				graph.nodes.erase(3U);
				graph.edges.clear();
				graph.nodes.at(1U).properties.emplace_back("payload", PropertyValue(payload));
			}
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			auto bounds = directBounds();
			bounds.maximum_materialized_bytes = 3000U;
			if (isEdge) {
				logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b",
													"KNOWS", "out");
				EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(traversal, bounds)), std::runtime_error);
				EXPECT_EQ(graph.edge_shaped_admission_reads, 1U);
				EXPECT_EQ(graph.edge_shaped_batch_reads, 0U);
				bounds.maximum_materialized_bytes = 16000U;
				const auto result = runtime.executeToQueryResult(traversal, bounds);
				ASSERT_EQ(result.rowCount(), 1U);
				EXPECT_EQ(result.getRows().front().at("r").asLogicalEdge().properties.at("payload"),
						  PropertyValue(payload));
				EXPECT_GT(graph.edge_shaped_batch_reads, 0U);
			} else {
				logical::LogicalNodeScan scan("n");
				EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(scan, bounds)), std::runtime_error);
				EXPECT_EQ(graph.node_shaped_admission_reads, 1U);
				EXPECT_EQ(graph.node_shaped_batch_reads, 0U);
				bounds.maximum_materialized_bytes = 16000U;
				const auto result = runtime.executeToQueryResult(scan, bounds);
				ASSERT_EQ(result.rowCount(), 1U);
				EXPECT_EQ(result.getRows().front().at("n").asLogicalNode().properties.at("payload"),
						  PropertyValue(payload));
				EXPECT_GT(graph.node_shaped_batch_reads, 0U);
			}
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, PatternNamedColumnsShareSourceAndRetainedBatchBudget) {
		for (const bool referencePages: {false, true}) {
			SCOPED_TRACE(referencePages);
			DirectEdgeGraph graph = directGraph();
			graph.nodes.clear();
			graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Seed"}});
			graph.nodes.emplace(
					2U, LogicalGraphNode{2U, {"Target"}, {{"payload", PropertyValue(std::string(1200U, 'n'))}}});
			graph.edges = {
					LogicalGraphEdge{10U, 1U, 2U, "KNOWS", {{"payload", PropertyValue(std::string(1200U, 'e'))}}}};
			graph.enable_property_tokens = true;
			graph.enable_property_read_admission = true;
			graph.enable_adjacency_reference_pages = referencePages;
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			graph::parser::cypher::CypherParserImpl parser;
			const auto plan = parser.parseToLogical(
					"MATCH (n:Seed) RETURN [(n)-[r:KNOWS]->(m) | size(r.payload) + size(m.payload)] AS lengths");
			ASSERT_NE(plan.root, nullptr);
			auto bounded = directBounds();
			bounded.maximum_materialized_bytes = 1900U;
			EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounded)), std::runtime_error);
			// Both selected payloads together exceed the complete execution budget;
			// preflight must reject before opening either property column.
			EXPECT_EQ(graph.edge_property_column_batch_reads, 0U);
			EXPECT_EQ(graph.node_property_column_admission_reads, 0U);
			EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
			bounded.maximum_materialized_bytes = 10000U;
			const auto result = runtime.executeToQueryResult(*plan.root, bounded);
			ASSERT_EQ(result.rowCount(), 1U);
			EXPECT_EQ(result.getRows().front().at("lengths").asPrimitive(),
					  PropertyValue(std::vector<PropertyValue>{PropertyValue(2400)}));
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ReusesQueryBoundEdgeMaterializationForRepeatedIntrospection) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a"),
																	 "a", "r", "b", "KNOWS", "out");
		std::vector<std::unique_ptr<expressions::Expression>> firstArguments;
		firstArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("r"));
		std::vector<std::unique_ptr<expressions::Expression>> secondArguments;
		secondArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("r"));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("type", std::move(firstArguments)),
						   "first_type");
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("type", std::move(secondArguments)),
						   "second_type");
		logical::LogicalProject project(std::move(traversal), std::move(items));

		std::vector<std::pair<PropertyValue, PropertyValue>> types;
		static_cast<void>(runtime.execute(project, directBounds(), [&types](const LogicalPlanReadRow &row) {
			types.emplace_back(std::get<PropertyValue>(row.values.at("first_type")),
							   std::get<PropertyValue>(row.values.at("second_type")));
			return true;
		}));

		EXPECT_EQ(types, (std::vector<std::pair<PropertyValue, PropertyValue>>{
								 {PropertyValue("KNOWS"), PropertyValue("KNOWS")},
								 {PropertyValue("KNOWS"), PropertyValue("KNOWS")},
								 {PropertyValue("KNOWS"), PropertyValue("KNOWS")}}));
		// The type-only shape is resolved once even though type(r) is evaluated twice;
		// it does not force complete Property materialization.
		EXPECT_EQ(graph.edge_read_ids, (std::vector<uint64_t>{10U, 11U, 12U}));
		EXPECT_EQ(graph.edge_shaped_reads, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesEntityMetadataForRepeatedIntrospectionExpressions) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("n"),
																	 "n", "r", "m", "KNOWS", "out");
		std::vector<std::unique_ptr<expressions::Expression>> labelArguments;
		labelArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		std::vector<std::unique_ptr<expressions::Expression>> typeArguments;
		typeArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("r"));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("labels", std::move(labelArguments)),
						   "node_labels");
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("type", std::move(typeArguments)),
						   "edge_type");
		std::vector<std::unique_ptr<expressions::Expression>> repeatedLabelArguments;
		repeatedLabelArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		std::vector<std::unique_ptr<expressions::Expression>> repeatedTypeArguments;
		repeatedTypeArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("r"));
		items.emplace_back(
				std::make_shared<expressions::FunctionCallExpression>("labels", std::move(repeatedLabelArguments)),
				"node_labels_again");
		items.emplace_back(
				std::make_shared<expressions::FunctionCallExpression>("type", std::move(repeatedTypeArguments)),
				"edge_type_again");
		logical::LogicalProject project(std::move(traversal), std::move(items));

		std::vector<std::array<PropertyValue, 4>> metadata;
		static_cast<void>(runtime.execute(project, directBounds(), [&metadata](const LogicalPlanReadRow &row) {
			metadata.push_back({std::get<PropertyValue>(row.values.at("node_labels")),
								std::get<PropertyValue>(row.values.at("edge_type")),
								std::get<PropertyValue>(row.values.at("node_labels_again")),
								std::get<PropertyValue>(row.values.at("edge_type_again"))});
			return true;
		}));

		EXPECT_EQ(metadata.size(), 3U);
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.edge_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_reads, 0U);
		EXPECT_EQ(graph.edge_shaped_reads, 0U);
		EXPECT_TRUE(std::ranges::all_of(metadata, [](const auto &values) {
			return values[0] == PropertyValue(std::vector<PropertyValue>{PropertyValue("Person")}) &&
				   values[1] == PropertyValue("KNOWS") && values[0] == values[2] && values[1] == values[3];
		}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, SplitsMetadataPrefetchWhenSourceAndRowCopiesExceedBudget) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		for (auto &[id, node]: graph.nodes) {
			static_cast<void>(id);
			node.labels = {std::string(2048U, 'l')};
		}
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("labels", std::move(arguments)),
						   "labels");
		logical::LogicalProject project(std::make_unique<logical::LogicalNodeScan>("n"), std::move(items));
		LogicalPlanReadRuntimeBounds limits = directBounds();
		limits.maximum_materialized_bytes = 10U * 1024U;
		std::vector<PropertyValue> results;

		static_cast<void>(runtime.execute(project, limits, [&](const LogicalPlanReadRow &row) {
			results.push_back(std::get<PropertyValue>(row.values.at("labels")));
			return true;
		}));

		ASSERT_EQ(results.size(), 3U);
		for (const PropertyValue &result: results) {
			ASSERT_EQ(result.getList().size(), 1U);
			EXPECT_EQ(std::get<std::string>(result.getList().front().getVariant()).size(), 2048U);
		}
		EXPECT_GT(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, JoinAndOptionalMatchRetainEntityMetadataCaches) {
		const auto labelsExpression = [] {
			std::vector<std::unique_ptr<expressions::Expression>> arguments;
			arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
			return std::make_shared<expressions::FunctionCallExpression>("labels", std::move(arguments));
		};
		const auto makeMetadataProjection = [&] {
			std::vector<logical::LogicalProjectItem> items;
			items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "n");
			items.emplace_back(labelsExpression(), "labels_before");
			return std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalNodeScan>("n"),
															 std::move(items));
		};
		const auto makeFinalProjection = [&](std::unique_ptr<logical::LogicalOperator> child) {
			std::vector<logical::LogicalProjectItem> items;
			items.emplace_back(labelsExpression(), "labels_after");
			return logical::LogicalProject(std::move(child), std::move(items));
		};
		const auto verifyResult = [](const QueryResult &result) {
			ASSERT_EQ(result.rowCount(), 3U);
			EXPECT_EQ(result.getRows()[0].at("labels_after").asPrimitive(),
					  PropertyValue(std::vector<PropertyValue>{PropertyValue("Person")}));
			EXPECT_EQ(result.getRows()[1].at("labels_after").asPrimitive(),
					  PropertyValue(std::vector<PropertyValue>{PropertyValue("Person")}));
			EXPECT_EQ(result.getRows()[2].at("labels_after").asPrimitive(),
					  PropertyValue(std::vector<PropertyValue>{PropertyValue("Other")}));
		};

		DirectEdgeGraph joinGraph = directGraph();
		joinGraph.enable_property_tokens = true;
		DirectEdgeBackend joinBackend(joinGraph);
		LogicalPlanReadRuntime joinRuntime(joinBackend);
		auto join = std::make_unique<logical::LogicalJoin>(std::make_unique<logical::LogicalSingleRow>(),
														   makeMetadataProjection());
		const QueryResult joinResult =
				joinRuntime.executeToQueryResult(makeFinalProjection(std::move(join)), directBounds());
		verifyResult(joinResult);
		EXPECT_EQ(joinGraph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(joinGraph.node_shaped_reads, 0U);

		DirectEdgeGraph optionalGraph = directGraph();
		optionalGraph.enable_property_tokens = true;
		DirectEdgeBackend optionalBackend(optionalGraph);
		LogicalPlanReadRuntime optionalRuntime(optionalBackend);
		auto optional = std::make_unique<logical::LogicalOptionalMatch>(std::make_unique<logical::LogicalSingleRow>(),
																		makeMetadataProjection());
		const QueryResult optionalResult =
				optionalRuntime.executeToQueryResult(makeFinalProjection(std::move(optional)), directBounds());
		verifyResult(optionalResult);
		EXPECT_EQ(optionalGraph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(optionalGraph.node_shaped_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, InvalidatesEntityMetadataWhenUnwindRebindsAnEntityVariable) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);

		auto labelsArguments = std::vector<std::unique_ptr<expressions::Expression>>{};
		labelsArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		auto labelsExpression =
				std::make_unique<expressions::FunctionCallExpression>("labels", std::move(labelsArguments));
		auto expectedLabels = std::make_unique<expressions::ListLiteralExpression>(
				PropertyValue(std::vector<PropertyValue>{PropertyValue("Person")}));
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::move(labelsExpression), expressions::BinaryOperatorType::BOP_EQUAL, std::move(expectedLabels));
		auto filtered = std::make_unique<logical::LogicalFilter>(std::make_unique<logical::LogicalNodeScan>("n"),
																 std::move(predicate));
		auto rebound = std::make_unique<logical::LogicalUnwind>(std::move(filtered), "n",
																std::vector<PropertyValue>{PropertyValue(1)});
		std::vector<logical::LogicalProjectItem> items;
		auto reboundLabelsArguments = std::vector<std::unique_ptr<expressions::Expression>>{};
		reboundLabelsArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		items.emplace_back(
				std::make_shared<expressions::FunctionCallExpression>("labels", std::move(reboundLabelsArguments)),
				"labels");
		logical::LogicalProject project(std::move(rebound), std::move(items));

		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(project, directBounds())), std::runtime_error);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsRelationshipReferenceWithStaleTypeToken) {
		DirectEdgeGraph graph = directGraph();
		graph.forced_edge_reference_type_token = 99U;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out");

		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(traversal, directBounds())), std::runtime_error);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, DoesNotCacheMaterializedEdgesBeyondTheByteBudget) {
		DirectEdgeGraph graph = directGraph();
		// A complete entity cache is a convenience for repeated bindings, not an
		// alternate LargeValue store.  Oversized payloads must be read on demand so
		// a single query cannot retain an unbounded copy of the property pages.
		graph.edges.front().properties.emplace_back(
				"source", PropertyValue(PropertyValue::Bytes(5U * 1024U * 1024U, std::byte{0})));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a"),
																	 "a", "r", "b", "KNOWS", "out");
		std::vector<std::unique_ptr<expressions::Expression>> firstArguments;
		firstArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("r"));
		std::vector<std::unique_ptr<expressions::Expression>> secondArguments;
		secondArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("r"));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("type", std::move(firstArguments)),
						   "first_type");
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("type", std::move(secondArguments)),
						   "second_type");
		logical::LogicalProject project(std::move(traversal), std::move(items));

		static_cast<void>(runtime.execute(project, directBounds(), [](const LogicalPlanReadRow &) { return true; }));
		// Metadata-only type() remains cacheable even when the complete entity carries
		// an oversized value. The complete-object byte budget is enforced separately.
		EXPECT_EQ(graph.edge_read_ids, (std::vector<uint64_t>{10U, 11U, 12U}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BorrowedUnwindKeepsEntityScalarConversionAndSuppression) {
		for (const bool scalarAsRow: {false, true}) {
			DirectEdgeGraph graph = directGraph();
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			logical::LogicalUnwind root(std::make_unique<logical::LogicalNodeScan>("n"), "id",
										std::make_shared<expressions::VariableReferenceExpression>("n"), scalarAsRow);
			std::vector<PropertyValue> ids;
			static_cast<void>(runtime.execute(root, directBounds(), [&](const LogicalPlanReadRow &row) {
				const auto &node = std::get<LogicalGraphNode>(row.values.at("n"));
				const auto &id = std::get<PropertyValue>(row.values.at("id"));
				EXPECT_EQ(id, PropertyValue(node.node_id));
				ids.push_back(id);
				return true;
			}));
			EXPECT_EQ(ids, scalarAsRow
								   ? (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)})
								   : std::vector<PropertyValue>{});
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, SubqueryOuterAndImportedRowsShareTheExecutionBudget) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		const PropertyValue payload(std::string(3000U, 'p'));
		const auto makeInput = [&]() {
			return std::make_unique<logical::LogicalUnwind>(std::make_unique<logical::LogicalSingleRow>(), "outer",
															std::vector<PropertyValue>{payload});
		};
		std::vector<logical::LogicalProjectItem> items;
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("outer"));
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("size", std::move(arguments)), "size");
		auto body = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(),
															  std::move(items));
		logical::LogicalCallSubquery root(makeInput(), std::move(body), {"outer"}, {"size"});
		auto independent = makeInput();
		auto bounds = directBounds();
		bounds.maximum_materialized_bytes = 5000U;
		size_t delivered = 0U;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*independent, bounds, [&](const LogicalPlanReadRow &) {
			++delivered;
			return true;
		})));
		EXPECT_EQ(delivered, 1U);
		delivered = 0U;
		try {
			static_cast<void>(runtime.execute(root, bounds, [&](const LogicalPlanReadRow &) {
				++delivered;
				return true;
			}));
			FAIL() << "outer and imported copies must share the execution allowance";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical-plan CallSubquery imports exceeded its shared byte bound");
		}
		EXPECT_EQ(delivered, 0U);
		bounds.maximum_materialized_bytes = 16000U;
		for (const bool continueVisiting: {false, true}) {
			delivered = 0U;
			EXPECT_NO_THROW(static_cast<void>(runtime.execute(root, bounds, [&](const LogicalPlanReadRow &row) {
				++delivered;
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("outer")), payload);
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("size")), PropertyValue(3000));
				return continueVisiting;
			})));
			EXPECT_EQ(delivered, 1U);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, UnitSubqueryReleasesImportedRowsBeforeDownstreamSort) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		const PropertyValue payload(std::string(3000U, 'p'));
		auto input = std::make_unique<logical::LogicalUnwind>(std::make_unique<logical::LogicalSingleRow>(), "outer",
															  std::vector<PropertyValue>{payload});
		auto noRows = std::make_unique<logical::LogicalFilter>(std::make_unique<logical::LogicalSingleRow>(),
															   std::make_shared<expressions::LiteralExpression>(false));
		auto unit = std::make_unique<logical::LogicalCallSubquery>(
				std::move(input), std::move(noRows), std::vector<std::string>{"outer"}, std::vector<std::string>{});
		std::vector<logical::LogicalSortItem> keys;
		keys.emplace_back(std::make_shared<expressions::LiteralExpression>(std::string(2000U, 'k')), true);
		logical::LogicalSort root(std::move(unit), std::move(keys));
		auto bounds = directBounds();
		// The allowance fits both 3 KiB input owners, including row metadata,
		// or the output plus its 2 KiB key, but not the stale import plus Sort.
		bounds.maximum_materialized_bytes = 7500U;
		for (size_t execution = 0U; execution < 2U; ++execution) {
			SCOPED_TRACE(execution);
			size_t delivered = 0U;
			EXPECT_NO_THROW(static_cast<void>(runtime.execute(root, bounds, [&](const LogicalPlanReadRow &row) {
				++delivered;
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("outer")), payload);
				return true;
			})));
			EXPECT_EQ(delivered, 1U);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NestedSortPrefetchesOuterPropertyAndTransfersRows) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		graph.nodes.at(1U).properties.emplace_back("priority", PropertyValue(3));
		graph.nodes.at(2U).properties.emplace_back("priority", PropertyValue(1));
		graph.nodes.at(3U).properties.emplace_back("priority", PropertyValue(2));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalSortItem> innerKeys;
		innerKeys.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{0}), true);
		auto inner = std::make_unique<logical::LogicalSort>(std::make_unique<logical::LogicalNodeScan>("n"),
															std::move(innerKeys));
		std::vector<logical::LogicalSortItem> outerKeys;
		outerKeys.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "priority"), true);
		logical::LogicalSort outer(std::move(inner), std::move(outerKeys));
		auto constrained = directBounds();
		// Include buffered/source/output row frames and key/Property slots.
		constrained.maximum_materialized_bytes =
				8U * sizeof(LogicalPlanReadRow) + 12U * sizeof(PropertyValue) + 824U + sizeof(LogicalPlanReadRow) + resultEntityCapacityDelta(graph) + entityCacheMetadataDelta(graph);
		const QueryResult result = runtime.executeToQueryResult(outer, constrained);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("n").asNode().getId(), 2U);
		EXPECT_EQ(result.getRows()[1].at("n").asNode().getId(), 3U);
		EXPECT_EQ(result.getRows()[2].at("n").asNode().getId(), 1U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"priority"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, FilterBetweenSortsTransfersSelectedRowReservations) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		graph.nodes.at(1U).properties.emplace_back("priority", PropertyValue(3));
		graph.nodes.at(2U).properties.emplace_back("priority", PropertyValue(1));
		graph.nodes.at(3U).properties.emplace_back("priority", PropertyValue(2));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalSortItem> innerKeys;
		innerKeys.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{0}), true);
		auto inner = std::make_unique<logical::LogicalSort>(std::make_unique<logical::LogicalNodeScan>("n"),
															std::move(innerKeys));
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("n", "priority"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{1}));
		auto filtered = std::make_unique<logical::LogicalFilter>(std::move(inner), std::move(predicate));
		std::vector<logical::LogicalSortItem> outerKeys;
		outerKeys.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "priority"), true);
		logical::LogicalSort outer(std::move(filtered), std::move(outerKeys));
		auto constrained = directBounds();
		constrained.maximum_materialized_bytes = 3000U + sizeof(LogicalPlanReadRow);
		const QueryResult result = runtime.executeToQueryResult(outer, constrained);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("n").asNode().getId(), 3U);
		EXPECT_EQ(result.getRows()[1].at("n").asNode().getId(), 1U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ProjectBetweenSortsTransfersProjectedRowReservations) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		graph.nodes.at(1U).properties.emplace_back("priority", PropertyValue(3));
		graph.nodes.at(2U).properties.emplace_back("priority", PropertyValue(1));
		graph.nodes.at(3U).properties.emplace_back("priority", PropertyValue(2));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalSortItem> innerKeys;
		innerKeys.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{0}), true);
		auto inner = std::make_unique<logical::LogicalSort>(std::make_unique<logical::LogicalNodeScan>("n"),
															std::move(innerKeys));
		std::vector<logical::LogicalProjectItem> projectedItems;
		projectedItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "m");
		auto projected = std::make_unique<logical::LogicalProject>(std::move(inner), std::move(projectedItems));
		std::vector<logical::LogicalSortItem> outerKeys;
		outerKeys.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("m", "priority"), true);
		logical::LogicalSort outer(std::move(projected), std::move(outerKeys));
		auto constrained = directBounds();
		// Include buffered/source/output row frames and key/Property slots.
		constrained.maximum_materialized_bytes =
				8U * sizeof(LogicalPlanReadRow) + 12U * sizeof(PropertyValue) + 824U + sizeof(LogicalPlanReadRow) + resultEntityCapacityDelta(graph) + entityCacheMetadataDelta(graph);
		const QueryResult result = runtime.executeToQueryResult(outer, constrained);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("m").asNode().getId(), 2U);
		EXPECT_EQ(result.getRows()[1].at("m").asNode().getId(), 3U);
		EXPECT_EQ(result.getRows()[2].at("m").asNode().getId(), 1U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, DistinctProjectBetweenSortsReleasesDuplicateRowReservations) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalSortItem> innerKeys;
		innerKeys.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{0}), true);
		auto inner = std::make_unique<logical::LogicalSort>(std::make_unique<logical::LogicalNodeScan>("n"),
															std::move(innerKeys));
		std::vector<logical::LogicalProjectItem> projectedItems;
		projectedItems.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{1}), "value");
		auto projected = std::make_unique<logical::LogicalProject>(std::move(inner), std::move(projectedItems), true);
		std::vector<logical::LogicalSortItem> outerKeys;
		outerKeys.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("value"), true);
		logical::LogicalSort outer(std::move(projected), std::move(outerKeys));
		auto constrained = directBounds();
		constrained.maximum_materialized_bytes = 3000U;
		const QueryResult result = runtime.executeToQueryResult(outer, constrained);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue(1));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, SortPreservesProjectedFilteredRowsWithoutOuterPrefetch) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		graph.nodes.at(1U).properties.emplace_back("priority", PropertyValue(3));
		graph.nodes.at(2U).properties.emplace_back("priority", PropertyValue(1));
		graph.nodes.at(3U).properties.emplace_back("priority", PropertyValue(2));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalSortItem> innerKeys;
		innerKeys.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{0}), true);
		auto inner = std::make_unique<logical::LogicalSort>(std::make_unique<logical::LogicalNodeScan>("n"),
															std::move(innerKeys));
		std::vector<logical::LogicalProjectItem> projectedItems;
		projectedItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "m");
		auto projected = std::make_unique<logical::LogicalProject>(std::move(inner), std::move(projectedItems));
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("m", "priority"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{1}));
		auto filtered = std::make_unique<logical::LogicalFilter>(std::move(projected), std::move(predicate));
		std::vector<logical::LogicalSortItem> outerKeys;
		outerKeys.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{0}), true);
		logical::LogicalSort outer(std::move(filtered), std::move(outerKeys));
		auto constrained = directBounds();
		// Include buffered/source/output row frames and key/Property slots.
		constrained.maximum_materialized_bytes =
				8U * sizeof(LogicalPlanReadRow) + 12U * sizeof(PropertyValue) + 824U;
		const QueryResult result = runtime.executeToQueryResult(outer, constrained);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("m").asNode().getId(), 1U);
		EXPECT_EQ(result.getRows()[1].at("m").asNode().getId(), 3U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, SortReceivesWindowedRowsThroughSkipAndLimit) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalSortItem> innerKeys;
		innerKeys.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{0}), true);
		auto inner = std::make_unique<logical::LogicalSort>(std::make_unique<logical::LogicalNodeScan>("n"),
															std::move(innerKeys));
		auto skipped = std::make_unique<logical::LogicalSkip>(std::move(inner), 1);
		auto limited = std::make_unique<logical::LogicalLimit>(std::move(skipped), 1);
		std::vector<logical::LogicalSortItem> outerKeys;
		outerKeys.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{0}), true);
		logical::LogicalSort outer(std::move(limited), std::move(outerKeys));
		auto constrained = directBounds();
		constrained.maximum_materialized_bytes = 3000U;
		const QueryResult result = runtime.executeToQueryResult(outer, constrained);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows()[0].at("n").asNode().getId(), 2U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, SortAdmitsExpandedRowsWhileUnwindRetainsItsInputCharge) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalSortItem> innerKeys;
		innerKeys.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{0}), true);
		auto inner = std::make_unique<logical::LogicalSort>(std::make_unique<logical::LogicalNodeScan>("n"),
															std::move(innerKeys));
		auto expanded = std::make_unique<logical::LogicalUnwind>(
				std::move(inner), "item", std::vector<PropertyValue>{PropertyValue(2), PropertyValue(1)});
		std::vector<logical::LogicalSortItem> outerKeys;
		outerKeys.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("item"), true);
		logical::LogicalSort outer(std::move(expanded), std::move(outerKeys));
		auto constrained = directBounds();
		// Six expanded rows coexist with retained source frames, sorting keys
		// and result publication. Account for actual platform slot sizes.
		constrained.maximum_materialized_bytes =
				16U * sizeof(LogicalPlanReadRow) + 24U * sizeof(PropertyValue) + 3648U;
		const QueryResult result = runtime.executeToQueryResult(outer, constrained);
		ASSERT_EQ(result.rowCount(), 6U);
		for (size_t index = 0U; index < 6U; ++index) {
			EXPECT_EQ(result.getRows()[index].at("item").asPrimitive(), PropertyValue(index < 3U ? 1 : 2));
			EXPECT_EQ(result.getRows()[index].at("n").asNode().getId(), index % 3U + 1U);
		}
	}


	TEST(LogicalPlanDirectEdgeRuntimeTest, OwnedSubqueryReturnsPreserveLargePayloadDuplicatesAndVisitorStop) {
		for (const bool stopEarly: {false, true}) {
			SCOPED_TRACE(stopEarly);
			DirectEdgeGraph graph = directGraph();
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			const PropertyValue payload(std::string(8192U, 'x'));
			auto input = std::make_unique<logical::LogicalUnwind>(
					std::make_unique<logical::LogicalSingleRow>(), "outer",
					std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)});
			auto body =
					std::make_unique<logical::LogicalUnwind>(std::make_unique<logical::LogicalSingleRow>(), "returned",
															 std::vector<PropertyValue>{payload, payload});
			logical::LogicalCallSubquery root(std::move(input), std::move(body), {"outer"}, {"returned"});
			std::vector<PropertyValue> outers;
			static_cast<void>(runtime.execute(root, directBounds(), [&](const LogicalPlanReadRow &row) {
				outers.push_back(std::get<PropertyValue>(row.values.at("outer")));
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("returned")), payload);
				EXPECT_EQ(row.values.size(), 2U);
				return !stopEarly;
			}));
			EXPECT_EQ(outers, stopEarly ? (std::vector<PropertyValue>{PropertyValue(1)})
										: (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(1),
																	  PropertyValue(2), PropertyValue(2)}));
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CorrelatedSubqueryAdmitsCombinedOutputBesideLiveImports) {
		for (const bool sorted: {false, true}) {
			SCOPED_TRACE(sorted);
			DirectEdgeGraph graph = directGraph();
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			auto input = std::make_unique<logical::LogicalUnwind>(
					std::make_unique<logical::LogicalSingleRow>(), "outer",
					std::vector<PropertyValue>{PropertyValue(std::string(3000U, 'o'))});
			std::vector<logical::LogicalProjectItem> items;
			items.emplace_back(std::make_shared<expressions::LiteralExpression>(PropertyValue(std::string(3000U, 'r'))),
							   "returned");
			auto body = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(),
																  std::move(items));
			std::unique_ptr<logical::LogicalOperator> plan = std::make_unique<logical::LogicalCallSubquery>(
					std::move(input), std::move(body), std::vector<std::string>{"outer"},
					std::vector<std::string>{"returned"});
			if (sorted) {
				std::vector<logical::LogicalSortItem> keys;
				keys.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("returned"), true);
				plan = std::make_unique<logical::LogicalSort>(std::move(plan), std::move(keys));
			}
			auto constrained = directBounds();
			constrained.maximum_materialized_bytes = 10000U + sizeof(LogicalPlanReadRow);
			size_t delivered = 0U;
			try {
				static_cast<void>(runtime.execute(*plan, constrained, [&](const LogicalPlanReadRow &) {
					++delivered;
					return true;
				}));
				FAIL() << "subquery output must overlap its live outer and import rows";
			} catch (const std::runtime_error &error) {
				EXPECT_TRUE(std::string(error.what()) ==
									"logical-plan CallSubquery output exceeded its shared byte bound" ||
							std::string(error.what()) == "shared materialization exceeded its byte bound");
			}
			EXPECT_EQ(delivered, 0U);
			constrained.maximum_materialized_bytes = sorted ? 30000U : 20000U;
			EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan, constrained, [&](const LogicalPlanReadRow &row) {
				++delivered;
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("outer")), PropertyValue(std::string(3000U, 'o')));
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("returned")), PropertyValue(std::string(3000U, 'r')));
				return true;
			})));
			EXPECT_EQ(delivered, 1U);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, JoinBatchBudgetStillRejectsOversizedRowsDiscardedBySkip) {
		for (const bool optional: {false, true}) {
			for (const int64_t skip: {0, 1}) {
				for (const size_t budget: {size_t{5000U}, size_t{12000U}}) {
					SCOPED_TRACE(optional);
					SCOPED_TRACE(skip);
					SCOPED_TRACE(budget);
					DirectEdgeGraph graph = directGraph();
					DirectEdgeBackend backend(graph);
					LogicalPlanReadRuntime runtime(backend);
					auto left = std::make_unique<logical::LogicalUnwind>(
							std::make_unique<logical::LogicalSingleRow>(), "left",
							std::vector<PropertyValue>{PropertyValue(std::string(3000U, 'a'))});
					auto right = std::make_unique<logical::LogicalUnwind>(
							std::make_unique<logical::LogicalSingleRow>(), "right",
							std::vector<PropertyValue>{PropertyValue(std::string(3000U, 'b'))});
					std::unique_ptr<logical::LogicalOperator> joined;
					if (optional)
						joined = std::make_unique<logical::LogicalOptionalMatch>(std::move(left), std::move(right));
					else
						joined = std::make_unique<logical::LogicalJoin>(std::move(left), std::move(right));
					logical::LogicalSkip root(std::move(joined), skip);
					auto bounds = directBounds();
					bounds.maximum_materialized_bytes = budget;
					size_t delivered = 0U;
					const auto run = [&] {
						return runtime.execute(root, bounds, [&](const LogicalPlanReadRow &row) {
							++delivered;
							EXPECT_EQ(std::get<PropertyValue>(row.values.at("left")),
									  PropertyValue(std::string(3000U, 'a')));
							EXPECT_EQ(std::get<PropertyValue>(row.values.at("right")),
									  PropertyValue(std::string(3000U, 'b')));
							return true;
						});
					};
					if (budget == 5000U) {
						try {
							static_cast<void>(run());
							FAIL() << "expected joined row rejection";
						} catch (const std::runtime_error &error) {
							// The retained right side and left probe now exhaust the common
							// allowance before SKIP can buffer the oversized combined row.
							EXPECT_STREQ(error.what(), "shared materialization exceeded its byte bound");
						}
						EXPECT_EQ(delivered, 0U);
					} else {
						EXPECT_NO_THROW(static_cast<void>(run()));
						EXPECT_EQ(delivered, skip == 0 ? 1U : 0U);
					}
				}
			}
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NestedJoinAndOptionalBuffersShareTheExecutionBudget) {
		for (const bool optional: {false, true}) {
			SCOPED_TRACE(optional);
			DirectEdgeGraph graph = directGraph();
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			const auto makeRows = [](const std::string &column, char first) {
				return std::make_unique<logical::LogicalUnwind>(
						std::make_unique<logical::LogicalSingleRow>(), column,
						std::vector<PropertyValue>{PropertyValue(std::string(1000U, first)),
												   PropertyValue(std::string(1000U, static_cast<char>(first + 1)))});
			};
			const auto makeJoin = [optional](std::unique_ptr<logical::LogicalOperator> left,
											 std::unique_ptr<logical::LogicalOperator> right)
					-> std::unique_ptr<logical::LogicalOperator> {
				if (optional)
					return std::make_unique<logical::LogicalOptionalMatch>(std::move(left), std::move(right));
				return std::make_unique<logical::LogicalJoin>(std::move(left), std::move(right));
			};
			auto independent = makeJoin(std::make_unique<logical::LogicalSingleRow>(), makeRows("inner", 'c'));
			auto nested = makeJoin(makeJoin(std::make_unique<logical::LogicalSingleRow>(), makeRows("inner", 'c')),
								   makeRows("outer", 'a'));
			auto bounds = directBounds();
			// Both retained right rows coexist with a separately owned output
			// row until the public visitor consumes it.
			bounds.maximum_materialized_bytes = 5000U;
			size_t delivered = 0U;
			EXPECT_NO_THROW(static_cast<void>(runtime.execute(*independent, bounds, [&](const LogicalPlanReadRow &) {
				++delivered;
				return true;
			})));
			EXPECT_EQ(delivered, 2U);
			delivered = 0U;
			try {
				static_cast<void>(runtime.execute(*nested, bounds, [&](const LogicalPlanReadRow &) {
					++delivered;
					return true;
				}));
				FAIL() << "nested retained sides must share one execution allowance";
			} catch (const std::runtime_error &error) {
				EXPECT_STREQ(error.what(), "shared materialization exceeded its byte bound");
			}
			EXPECT_EQ(delivered, 0U);
			bounds.maximum_materialized_bytes = 12000U;
			for (const bool stopAfterFirst: {true, false}) {
				SCOPED_TRACE(stopAfterFirst);
				std::vector<std::string> order;
				EXPECT_NO_THROW(static_cast<void>(runtime.execute(*nested, bounds, [&](const LogicalPlanReadRow &row) {
					const auto inner =
							std::get<std::string>(std::get<PropertyValue>(row.values.at("inner")).getVariant());
					const auto outer =
							std::get<std::string>(std::get<PropertyValue>(row.values.at("outer")).getVariant());
					order.push_back(std::string{inner.front(), outer.front()});
					return !stopAfterFirst;
				})));
				EXPECT_EQ(order, stopAfterFirst ? std::vector<std::string>{"ca"}
												: (std::vector<std::string>{"ca", "cb", "da", "db"}));
			}
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, JoinChargesBufferedRowsOnceAndStillEnforcesItsByteBound) {
		const std::vector<PropertyValue> values{PropertyValue(std::string(4096U, 'a')),
												PropertyValue(std::string(4096U, 'a')),
												PropertyValue(std::string(4096U, 'b'))};
		for (const bool optional: {false, true}) {
			for (const size_t byteBudget: {size_t{20U * 1024U}, size_t{8U * 1024U}}) {
				SCOPED_TRACE(optional);
				SCOPED_TRACE(byteBudget);
				DirectEdgeGraph graph = directGraph();
				DirectEdgeBackend backend(graph);
				LogicalPlanReadRuntime runtime(backend);
				auto right = std::make_unique<logical::LogicalUnwind>(std::make_unique<logical::LogicalSingleRow>(),
																	  "value", values);
				std::unique_ptr<logical::LogicalOperator> root;
				if (optional) {
					root = std::make_unique<logical::LogicalOptionalMatch>(
							std::make_unique<logical::LogicalSingleRow>(), std::move(right));
				} else {
					root = std::make_unique<logical::LogicalJoin>(std::make_unique<logical::LogicalSingleRow>(),
																  std::move(right));
				}
				LogicalPlanReadRuntimeBounds bounds = directBounds();
				bounds.maximum_materialized_bytes = byteBudget;
				std::vector<PropertyValue> delivered;
				const auto run = [&]() {
					return runtime.execute(*root, bounds, [&](const LogicalPlanReadRow &row) {
						delivered.push_back(std::get<PropertyValue>(row.values.at("value")));
						return true;
					});
				};
				if (byteBudget == 20U * 1024U) {
					// Three 4 KiB retained right rows and a 4 KiB copied output row
					// coexist; row/index overhead makes 16 KiB insufficient.
					EXPECT_NO_THROW(static_cast<void>(run()));
					EXPECT_EQ(delivered, values);
				} else {
					try {
						static_cast<void>(run());
						FAIL() << "right-side rows and index must stay within the byte budget";
					} catch (const std::runtime_error &error) {
						// UNWIND now admits the next owned payload before copying it,
						// so shared-budget rejection may precede the receiving buffer check.
						const std::string message(error.what());
						EXPECT_TRUE(message == "shared materialization exceeded its byte bound" ||
							message == (optional ? "logical-plan OptionalMatch exceeded its materialized byte bound"
								: "logical-plan Join exceeded its materialized byte bound"));
					}
					EXPECT_TRUE(delivered.empty());
				}
			}
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, JoinAndOptionalAdmitCombinedRowsWithRetainedPatternBytes) {
		for (const auto [optional, sorted]:
			 {std::pair{false, false}, std::pair{false, true}, std::pair{true, false}, std::pair{true, true}}) {
			SCOPED_TRACE(optional);
			SCOPED_TRACE(sorted);
			DirectEdgeGraph graph = directGraph();
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			auto makePlan = [&]() -> std::unique_ptr<logical::LogicalOperator> {
				auto left = std::make_unique<logical::LogicalUnwind>(
						std::make_unique<logical::LogicalSingleRow>(), "left_value",
						std::vector<PropertyValue>{PropertyValue(std::string(3000U, 'l'))});
				auto right = std::make_unique<logical::LogicalUnwind>(
						std::make_unique<logical::LogicalSingleRow>(), "right_value",
						std::vector<PropertyValue>{PropertyValue(std::string(3000U, 'r'))});
				std::unique_ptr<logical::LogicalOperator> root;
				if (optional)
					root = std::make_unique<logical::LogicalOptionalMatch>(std::move(left), std::move(right));
				else
					root = std::make_unique<logical::LogicalJoin>(std::move(left), std::move(right));
				if (sorted) {
					std::vector<logical::LogicalSortItem> keys;
					keys.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("left_value"), true);
					root = std::make_unique<logical::LogicalSort>(std::move(root), std::move(keys));
				}
				return root;
			};
			auto plan = makePlan();
			LogicalPlanReadRuntimeBounds bounds = directBounds();
			bounds.maximum_materialized_bytes = 8000U;
			size_t delivered = 0U;
			EXPECT_THROW(static_cast<void>(runtime.execute(*plan, bounds,
														   [&](const LogicalPlanReadRow &) {
															   ++delivered;
															   return true;
														   })),
						 std::runtime_error);
			EXPECT_EQ(delivered, 0U);
			bounds.maximum_materialized_bytes = sorted ? 24000U : 16000U;
			EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan, bounds, [&](const LogicalPlanReadRow &row) {
				++delivered;
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("left_value")), PropertyValue(std::string(3000U, 'l')));
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("right_value")),
						  PropertyValue(std::string(3000U, 'r')));
				return true;
			})));
			EXPECT_EQ(delivered, 1U);
		}
	}

} // namespace graph::query::execution
