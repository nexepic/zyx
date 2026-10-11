#include "CypherParserImpl.hpp"
#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"

namespace graph::query::execution {
	using namespace direct_edge_test;

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesOuterPropertiesUsedByPatternComprehension) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		auto map = std::make_unique<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("n", "rank"),
				expressions::BinaryOperatorType::BOP_ADD,
				std::make_unique<expressions::VariableReferenceExpression>("m", "rank"));
		auto pattern = std::make_shared<expressions::PatternComprehensionExpression>(
				"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "", expressions::PatternDirection::PAT_OUTGOING,
				std::move(map));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(pattern), "values");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> values;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("values")));
			return true;
		}));

		ASSERT_EQ(values.size(), 3U);
		EXPECT_EQ(values[0], PropertyValue(std::vector<PropertyValue>{PropertyValue(3), PropertyValue(4)}));
		EXPECT_EQ(values[1], PropertyValue(std::vector<PropertyValue>{PropertyValue(3)}));
		EXPECT_EQ(values[2], PropertyValue(std::vector<PropertyValue>{}));
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesPatternLocalTargetPropertiesByAdjacencyPage) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		auto pattern = std::make_shared<expressions::PatternComprehensionExpression>(
				"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "", expressions::PatternDirection::PAT_OUTGOING,
				std::make_unique<expressions::VariableReferenceExpression>("m", "rank"));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(pattern), "values");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> values;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("values")));
			return true;
		}));

		ASSERT_EQ(values.size(), 3U);
		EXPECT_EQ(values[0], PropertyValue(std::vector<PropertyValue>{PropertyValue(2), PropertyValue(3)}));
		EXPECT_EQ(values[1], PropertyValue(std::vector<PropertyValue>{PropertyValue(1)}));
		EXPECT_EQ(values[2], PropertyValue(std::vector<PropertyValue>{}));
		EXPECT_EQ(graph.node_shaped_batch_reads, 2U);
		EXPECT_EQ(graph.node_shaped_batch_property_tokens, (std::vector<std::vector<uint32_t>>{{1U}, {1U}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesPatternLocalRelationshipPropertiesByAdjacencyPage) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		auto pattern = std::make_shared<expressions::PatternComprehensionExpression>(
				"(n)-[r:KNOWS]->(m)", "n", "m", "KNOWS", "", expressions::PatternDirection::PAT_OUTGOING,
				std::make_unique<expressions::VariableReferenceExpression>("r", "weight"), nullptr, "r");
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(pattern), "values");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> values;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("values")));
			return true;
		}));

		ASSERT_EQ(values.size(), 3U);
		EXPECT_EQ(values[0], PropertyValue(std::vector<PropertyValue>{PropertyValue(7), PropertyValue(7)}));
		EXPECT_EQ(values[1], PropertyValue(std::vector<PropertyValue>{PropertyValue(8)}));
		EXPECT_EQ(values[2], PropertyValue(std::vector<PropertyValue>{}));
		EXPECT_EQ(graph.edge_shaped_batch_reads, 2U);
		EXPECT_EQ(graph.edge_shaped_batch_property_tokens, (std::vector<std::vector<uint32_t>>{{2U}, {2U}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesPatternLocalPropertiesOnTopologyPages) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		auto pattern = std::make_shared<expressions::PatternComprehensionExpression>(
				"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "", expressions::PatternDirection::PAT_OUTGOING,
				std::make_unique<expressions::VariableReferenceExpression>("m", "rank"));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(pattern), "values");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> values;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("values")));
			return true;
		}));

		ASSERT_EQ(values.size(), 3U);
		EXPECT_EQ(values[0], PropertyValue(std::vector<PropertyValue>{PropertyValue(2), PropertyValue(3)}));
		EXPECT_EQ(values[1], PropertyValue(std::vector<PropertyValue>{PropertyValue(1)}));
		EXPECT_EQ(values[2], PropertyValue(std::vector<PropertyValue>{}));
		EXPECT_EQ(graph.topology_scans, 3U);
		EXPECT_EQ(graph.node_shaped_batch_reads, 2U);
		EXPECT_EQ(graph.node_shaped_batch_property_tokens, (std::vector<std::vector<uint32_t>>{{1U}, {1U}}));
		EXPECT_EQ(graph.edge_shaped_batch_reads, 2U);
		EXPECT_EQ(graph.edge_shaped_batch_property_tokens, (std::vector<std::vector<uint32_t>>{{}, {}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ConstantPatternProjectionRetainsAnonymousTargetLabelFiltering) {
		for (const bool references: {false, true}) {
			SCOPED_TRACE(references);
			DirectEdgeGraph graph = directGraph();
			graph.enable_adjacency_reference_pages = references;
			graph.enable_property_tokens = true;
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			auto pattern = std::make_shared<expressions::PatternComprehensionExpression>(
					"(n)-[:KNOWS]->(:Person)", "n", "", "KNOWS", "Person", expressions::PatternDirection::PAT_OUTGOING,
					std::make_unique<expressions::LiteralExpression>(int64_t{7}));
			logical::LogicalProject project(std::make_unique<logical::LogicalNodeScan>("n"), {{pattern, "values"}});
			std::vector<PropertyValue> result;
			static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
				result.push_back(std::get<PropertyValue>(row.values.at("values")));
				return true;
			}));
			EXPECT_EQ(result, (std::vector<PropertyValue>{PropertyValue(std::vector<PropertyValue>{PropertyValue(7)}),
														  PropertyValue(std::vector<PropertyValue>{PropertyValue(7)}),
														  PropertyValue(std::vector<PropertyValue>{})}));
			EXPECT_EQ(graph.node_shaped_reads, 0U);
			EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{false, false}));
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, PatternMetadataOnlyBatchSkipsSourceTargetAndEdgeProperties) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_tokens = true;
		for (auto &[id, node]: graph.nodes) {
			static_cast<void>(id);
			node.labels = {"Person"};
			node.properties.push_back({"payload", PropertyValue(std::string(8192U, 'x'))});
		}
		for (auto &edge: graph.edges)
			edge.properties.push_back({"payload", PropertyValue(std::string(8192U, 'y'))});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("m"));
		auto pattern = std::make_shared<expressions::PatternComprehensionExpression>(
				"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "", expressions::PatternDirection::PAT_OUTGOING,
				std::make_unique<expressions::FunctionCallExpression>("labels", std::move(arguments)));
		logical::LogicalProject project(std::make_unique<logical::LogicalNodeScan>("n"), {{pattern, "labels"}});
		auto bounds = directBounds();
		bounds.maximum_materialized_bytes = 4096U;
		std::vector<PropertyValue> result;
		static_cast<void>(runtime.execute(project, bounds, [&](const LogicalPlanReadRow &row) {
			result.push_back(std::get<PropertyValue>(row.values.at("labels")));
			return true;
		}));
		ASSERT_EQ(result.size(), 3U);
		const PropertyValue labels(std::vector<PropertyValue>{PropertyValue("Person")});
		EXPECT_EQ(result[0], PropertyValue(std::vector<PropertyValue>{labels, labels}));
		EXPECT_EQ(result[1], PropertyValue(std::vector<PropertyValue>{labels}));
		EXPECT_EQ(result[2], PropertyValue(std::vector<PropertyValue>{}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{false, false}));
		EXPECT_EQ(graph.edge_shaped_batch_all_properties, (std::vector<bool>{false, false}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesCompletePatternPropertyMapsWithoutReadingRelationshipProperties) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("m"));
		auto pattern = std::make_shared<expressions::PatternComprehensionExpression>(
				"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "", expressions::PatternDirection::PAT_OUTGOING,
				std::make_unique<expressions::FunctionCallExpression>("properties", std::move(arguments)));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(pattern), "values");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> values;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("values")));
			return true;
		}));

		ASSERT_EQ(values.size(), 3U);
		EXPECT_EQ(values[0], PropertyValue(std::vector<PropertyValue>{
									 PropertyValue(PropertyValue::MapType{{"rank", PropertyValue(2)}}),
									 PropertyValue(PropertyValue::MapType{{"rank", PropertyValue(3)}})}));
		EXPECT_EQ(values[1], PropertyValue(std::vector<PropertyValue>{
									 PropertyValue(PropertyValue::MapType{{"rank", PropertyValue(1)}})}));
		EXPECT_EQ(values[2], PropertyValue(std::vector<PropertyValue>{}));
		EXPECT_EQ(graph.node_shaped_batch_reads, 2U);
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true, true}));
		EXPECT_EQ(graph.edge_shaped_batch_all_properties, (std::vector<bool>{false, false}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CompleteRelationshipPatternBatchDoesNotReadTargetProperties) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("r"));
		auto pattern = std::make_shared<expressions::PatternComprehensionExpression>(
				"(n)-[r:KNOWS]->(m)", "n", "m", "KNOWS", "", expressions::PatternDirection::PAT_OUTGOING,
				std::make_unique<expressions::FunctionCallExpression>("properties", std::move(arguments)), nullptr,
				"r");
		logical::LogicalProject project(std::make_unique<logical::LogicalNodeScan>("n"), {{pattern, "values"}});
		std::vector<PropertyValue> result;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			result.push_back(std::get<PropertyValue>(row.values.at("values")));
			return true;
		}));
		ASSERT_EQ(result.size(), 3U);
		ASSERT_EQ(result[0].getList().size(), 2U);
		EXPECT_EQ(result[0].getList()[0].getMap().at("tier"), PropertyValue(1));
		EXPECT_EQ(result[0].getList()[1].getMap().at("tier"), PropertyValue(2));
		EXPECT_EQ(result[1].getList()[0].getMap().at("weight"), PropertyValue(8));
		EXPECT_EQ(graph.edge_shaped_batch_all_properties, (std::vector<bool>{true, true}));
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{false, false}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, MetadataExistsReadsOnlyOneShapedCandidateBeforeStopping) {
		for (const bool references: {false, true}) {
			SCOPED_TRACE(references);
			DirectEdgeGraph graph = directGraph();
			graph.enable_adjacency_reference_pages = references;
			graph.enable_property_tokens = true;
			for (auto &[id, node]: graph.nodes) {
				static_cast<void>(id);
				node.properties.push_back({"payload", PropertyValue(std::string(8192U, 'p'))});
			}
			graph.nodes.at(3U).labels = {std::string(8192U, 'l')};
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			std::vector<std::unique_ptr<expressions::Expression>> labelsArguments;
			labelsArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("m"));
			std::vector<std::unique_ptr<expressions::Expression>> sizeArguments;
			sizeArguments.push_back(
					std::make_unique<expressions::FunctionCallExpression>("labels", std::move(labelsArguments)));
			auto predicate = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::FunctionCallExpression>("size", std::move(sizeArguments)),
					expressions::BinaryOperatorType::BOP_GREATER,
					std::make_unique<expressions::LiteralExpression>(int64_t{0}));
			auto exists = std::make_shared<expressions::ExistsExpression>("(n)-[:KNOWS]->(m)", "n", "KNOWS", "",
																		  expressions::PatternDirection::PAT_OUTGOING,
																		  std::move(predicate), "m");
			logical::LogicalProject project(std::make_unique<logical::LogicalNodeScan>("n"), {{exists, "match"}});
			auto bounds = directBounds();
			bounds.maximum_materialized_bytes = 4096U;
			size_t visited = 0U;
			static_cast<void>(runtime.execute(project, bounds, [&](const LogicalPlanReadRow &row) {
				++visited;
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("match")), PropertyValue(true));
				return false;
			}));
			EXPECT_EQ(visited, 1U);
			EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{false}));
			EXPECT_EQ(graph.edge_shaped_batch_all_properties, (std::vector<bool>{false}));
			EXPECT_TRUE(graph.node_read_ids.empty());
			// The first target's required labels must still obey the byte bound.
			graph.nodes.at(2U).labels = {std::string(8192U, 'l')};
			EXPECT_THROW(static_cast<void>(
								 runtime.execute(project, bounds, [](const LogicalPlanReadRow &) { return false; })),
						 std::runtime_error);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CompletePropertyExistsStopsBeforeUnneededCandidatePayload) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_tokens = true;
		graph.nodes.at(1U).properties.push_back({"payload", PropertyValue(std::string(8192U, 's'))});
		graph.nodes.at(3U).properties.push_back({"payload", PropertyValue(std::string(8192U, 't'))});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<std::unique_ptr<expressions::Expression>> keysArguments;
		keysArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("m"));
		std::vector<std::unique_ptr<expressions::Expression>> sizeArguments;
		sizeArguments.push_back(
				std::make_unique<expressions::FunctionCallExpression>("keys", std::move(keysArguments)));
		auto predicate = std::make_unique<expressions::BinaryOpExpression>(
				std::make_unique<expressions::FunctionCallExpression>("size", std::move(sizeArguments)),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{0}));
		auto exists = std::make_shared<expressions::ExistsExpression>("(n)-[:KNOWS]->(m)", "n", "KNOWS", "",
																	  expressions::PatternDirection::PAT_OUTGOING,
																	  std::move(predicate), "m");
		logical::LogicalProject project(std::make_unique<logical::LogicalNodeScan>("n"), {{exists, "match"}});
		auto bounds = directBounds();
		bounds.maximum_materialized_bytes = 4096U;
		size_t visited = 0U;
		static_cast<void>(runtime.execute(project, bounds, [&](const LogicalPlanReadRow &row) {
			++visited;
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("match")), PropertyValue(true));
			return false;
		}));
		EXPECT_EQ(visited, 1U);
		EXPECT_TRUE(graph.node_read_ids.empty());
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.edge_shaped_batch_all_properties, (std::vector<bool>{false}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, KeepsPatternLocalPropertiesOnSemanticFallback) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		auto pattern = std::make_shared<expressions::PatternComprehensionExpression>(
				"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "", expressions::PatternDirection::PAT_OUTGOING,
				std::make_unique<expressions::VariableReferenceExpression>("m", "rank"));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(pattern), "values");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> values;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("values")));
			return true;
		}));

		ASSERT_EQ(values.size(), 3U);
		EXPECT_EQ(values[0], PropertyValue(std::vector<PropertyValue>{PropertyValue(2), PropertyValue(3)}));
		EXPECT_EQ(values[1], PropertyValue(std::vector<PropertyValue>{PropertyValue(1)}));
		EXPECT_EQ(values[2], PropertyValue(std::vector<PropertyValue>{}));
		EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesOuterPropertyInsidePatternLocalMapProjection) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		std::vector<expressions::MapProjectionItem> mapItems;
		mapItems.emplace_back(expressions::MapProjectionItemType::MPROP_PROPERTY, "rank");
		mapItems.emplace_back(expressions::MapProjectionItemType::MPROP_LITERAL, "outer",
							  std::make_unique<expressions::VariableReferenceExpression>("n", "rank"));
		auto pattern = std::make_shared<expressions::PatternComprehensionExpression>(
				"(n)-[:KNOWS]->(m)", "n", "m", "KNOWS", "", expressions::PatternDirection::PAT_OUTGOING,
				std::make_unique<expressions::MapProjectionExpression>("m", std::move(mapItems)));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(pattern), "values");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> values;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("values")));
			return true;
		}));

		ASSERT_EQ(values.size(), 3U);
		EXPECT_EQ(
				values[0],
				PropertyValue(std::vector<PropertyValue>{
						PropertyValue(PropertyValue::MapType{{"outer", PropertyValue(1)}, {"rank", PropertyValue(2)}}),
						PropertyValue(
								PropertyValue::MapType{{"outer", PropertyValue(1)}, {"rank", PropertyValue(3)}})}));
		EXPECT_EQ(values[1], PropertyValue(std::vector<PropertyValue>{PropertyValue(PropertyValue::MapType{
									 {"outer", PropertyValue(2)}, {"rank", PropertyValue(1)}})}));
		EXPECT_EQ(values[2], PropertyValue(std::vector<PropertyValue>{}));
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesOuterPropertiesUsedByListComprehensions) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		std::vector<std::unique_ptr<expressions::Expression>> listElements;
		listElements.push_back(std::make_unique<expressions::VariableReferenceExpression>("n", "rank"));
		auto list = std::make_unique<expressions::ListLiteralExpression>(std::move(listElements));
		auto map = std::make_unique<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("x"),
				expressions::BinaryOperatorType::BOP_ADD, std::make_unique<expressions::LiteralExpression>(int64_t{1}));
		auto comprehension = std::make_shared<expressions::ListComprehensionExpression>(
				"x", std::move(list), nullptr, std::move(map),
				expressions::ListComprehensionExpression::ComprehensionType::COMP_EXTRACT);
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(comprehension), "values");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> values;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("values")));
			return true;
		}));

		ASSERT_EQ(values.size(), 3U);
		for (size_t index = 0U; index < values.size(); ++index) {
			const int64_t rank = static_cast<int64_t>(index + 1U);
			EXPECT_EQ(values[index], PropertyValue(std::vector<PropertyValue>{PropertyValue(rank + 1)}));
		}
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesOuterPropertiesUsedByReduceAndQuantifierExpressions) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		auto makeRankList = []() {
			std::vector<std::unique_ptr<expressions::Expression>> elements;
			elements.push_back(std::make_unique<expressions::VariableReferenceExpression>("n", "rank"));
			return std::make_unique<expressions::ListLiteralExpression>(std::move(elements));
		};
		auto reduce = std::make_shared<expressions::ReduceExpression>(
				"total", std::make_unique<expressions::LiteralExpression>(int64_t{0}), "x", makeRankList(),
				std::make_unique<expressions::BinaryOpExpression>(
						std::make_unique<expressions::VariableReferenceExpression>("total"),
						expressions::BinaryOperatorType::BOP_ADD,
						std::make_unique<expressions::VariableReferenceExpression>("x")));
		auto quantifier = std::make_shared<expressions::QuantifierFunctionExpression>(
				"all", "x", makeRankList(),
				std::make_unique<expressions::BinaryOpExpression>(
						std::make_unique<expressions::VariableReferenceExpression>("x"),
						expressions::BinaryOperatorType::BOP_GREATER,
						std::make_unique<expressions::LiteralExpression>(int64_t{0})));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(reduce), "total");
		items.emplace_back(std::move(quantifier), "positive");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<std::pair<PropertyValue, PropertyValue>> values;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			values.emplace_back(std::get<PropertyValue>(row.values.at("total")),
								std::get<PropertyValue>(row.values.at("positive")));
			return true;
		}));

		ASSERT_EQ(values.size(), 3U);
		for (size_t index = 0U; index < values.size(); ++index) {
			EXPECT_EQ(values[index], (std::pair<PropertyValue, PropertyValue>{
											 PropertyValue(static_cast<int64_t>(index + 1U)), PropertyValue(true)}));
		}
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ScopedExpressionBindingsDoNotPrefetchShadowedOuterEntities) {
		const std::vector<std::pair<std::string, PropertyValue>> cases{
				{"[n IN [{local: 99}] | n.local]", PropertyValue(std::vector<PropertyValue>{PropertyValue(99)})},
				{"[n IN [{local: 99}] | size(keys(n))]", PropertyValue(std::vector<PropertyValue>{PropertyValue(1)})},
				{"[n IN [{local: 99}] | n{.local}]", PropertyValue(std::vector<PropertyValue>{PropertyValue(
															 PropertyValue::MapType{{"local", PropertyValue(99)}})})},
				{"[n IN [{local: 99}] | n{.*}]", PropertyValue(std::vector<PropertyValue>{PropertyValue(
														 PropertyValue::MapType{{"local", PropertyValue(99)}})})},
				{"reduce(total = 0, n IN [{local: 99}] | total + n.local)", PropertyValue(99)},
				{"reduce(n = {local: 0}, x IN [1] | {local: n.local + x})",
				 PropertyValue(PropertyValue::MapType{{"local", PropertyValue(1)}})},
				{"all(n IN [{local: 99}] WHERE n.local = 99)", PropertyValue(true)},
				{"[n IN [{local: 99}] | [x IN [1] | n.local + x]]",
				 PropertyValue(
						 std::vector<PropertyValue>{PropertyValue(std::vector<PropertyValue>{PropertyValue(100)})})}};
		for (const auto &[expression, expected]: cases) {
			SCOPED_TRACE(expression);
			DirectEdgeGraph graph = directGraph();
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			graph::parser::cypher::CypherParserImpl parser;
			QueryPlan plan = parser.parseToLogical("MATCH (n) RETURN n.rank AS rank, " + expression + " AS local");
			ASSERT_NE(plan.root, nullptr);
			std::vector<PropertyValue> localValues;
			std::vector<int64_t> ranks;
			static_cast<void>(runtime.execute(*plan.root, directBounds(), [&](const LogicalPlanReadRow &row) {
				localValues.push_back(std::get<PropertyValue>(row.values.at("local")));
				ranks.push_back(std::get<int64_t>(std::get<PropertyValue>(row.values.at("rank")).getVariant()));
				return true;
			}));
			EXPECT_EQ(localValues, std::vector<PropertyValue>(3U, expected));
			EXPECT_EQ(ranks, (std::vector<int64_t>{1, 2, 3}));
			EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
			EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
			EXPECT_EQ(graph.node_property_point_reads, 0U);
			EXPECT_EQ(graph.node_shaped_batch_reads, 0U);
			EXPECT_TRUE(graph.node_read_ids.empty());
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ScopedCollectorsKeepOuterReadsInListSourcesAndReduceInitializers) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		graph::parser::cypher::CypherParserImpl parser;
		QueryPlan plan = parser.parseToLogical("MATCH (n) RETURN [n IN [n.rank] | n] AS items, "
											   "reduce(n = n.rank, x IN [1] | n + x) AS total, "
											   "all(n IN [n.rank] WHERE n > 0) AS positive");
		ASSERT_NE(plan.root, nullptr);
		size_t rows = 0U;
		static_cast<void>(runtime.execute(*plan.root, directBounds(), [&](const LogicalPlanReadRow &row) {
			const int64_t rank = static_cast<int64_t>(++rows);
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("items")),
					  PropertyValue(std::vector<PropertyValue>{PropertyValue(rank)}));
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("total")), PropertyValue(rank + 1));
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("positive")), PropertyValue(true));
			return true;
		}));
		EXPECT_EQ(rows, 3U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_TRUE(graph.node_read_ids.empty());
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, TopologyOnlyExistsUsesReferencePagesWithoutEntityMaterialization) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::ExistsExpression>("(n)-[:KNOWS]->()", "n", "KNOWS", "",
																		   expressions::PatternDirection::PAT_OUTGOING),
						   "has_outgoing_knows");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> results;
		const LogicalPlanReadRuntimeStats stats =
				runtime.execute(project, directBounds(), [&results](const LogicalPlanReadRow &row) {
					results.push_back(std::get<PropertyValue>(row.values.at("has_outgoing_knows")));
					return true;
				});

		EXPECT_EQ(results,
				  (std::vector<PropertyValue>{PropertyValue(true), PropertyValue(true), PropertyValue(false)}));
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
		EXPECT_EQ(graph.adjacency_ref_scans, 3U);
		EXPECT_EQ(graph.adjacency_ref_records, 3U);
		EXPECT_EQ(graph.node_existence_probes, 0U);
		EXPECT_EQ(graph.node_existence_batch_probes, 2U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
		EXPECT_EQ(stats.nodes_scanned, 3U);
		EXPECT_EQ(stats.edges_scanned, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesTargetNodePredicatesByDistinctSortedTargetIds) {
		DirectEdgeGraph graph = directGraph();
		graph.nodes.at(3U).labels = {"Person"};
		graph.edges.emplace_back(13U, 1U, 2U, "KNOWS",
								 std::vector<storage::LogicalGraphPropertyEntry>{{"weight", PropertyValue(9)}});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)}});
		logical::LogicalTraversal traversal(
				std::move(source), "a", "r", "b", "KNOWS", "out", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(2)}});

		std::vector<uint64_t> edges;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edges](const LogicalPlanReadRow &row) {
			edges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edges, (std::vector<uint64_t>{10U, 13U}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_property_sorted_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 2U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}, {"rank"}}));
		EXPECT_EQ(graph.node_label_sorted_reads, 1U);
		EXPECT_EQ(graph.node_label_sorted_ids, (std::vector<std::vector<uint64_t>>{{2U, 3U}}));
		EXPECT_EQ(graph.node_label_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_ref_scans, 1U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CountsNodesFromReferencesWithoutMaterializingNodeRows) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
		logical::LogicalAggregate aggregate(
				std::move(scan), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("n"), "count", false}});

		std::vector<PropertyValue> results;
		const LogicalPlanReadRuntimeStats stats =
				runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
					results.push_back(std::get<PropertyValue>(row.values.at("count")));
					return true;
				});

		EXPECT_EQ(results, std::vector<PropertyValue>{PropertyValue(int64_t{2})});
		EXPECT_EQ(stats.nodes_scanned, 3U);
		EXPECT_EQ(stats.rows_emitted, 1U);
		EXPECT_EQ(graph.node_label_match_scans, 1U);
		EXPECT_EQ(graph.node_label_match_records, 3U);
		EXPECT_EQ(graph.node_label_point_reads, 0U);
		EXPECT_EQ(graph.topology_scans, 0U);
		EXPECT_EQ(graph.adjacency_scans, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsInvalidNodeReferencesAtTheDirectCountBoundary) {
		for (const EntityRef reference: {
					 EntityRef{0U, false, 0U, 0U, 0U},
					 EntityRef{10U, true, 1U, 2U, 1U},
			 }) {
			DirectEdgeGraph graph = directGraph();
			graph.forced_node_reference = reference;
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
			logical::LogicalAggregate aggregate(
					std::move(scan), {},
					{{"count", std::make_shared<expressions::VariableReferenceExpression>("n"), "count", false}});

			EXPECT_THROW(static_cast<void>(runtime.execute(aggregate, directBounds(),
														   [](const LogicalPlanReadRow &) { return true; })),
						 std::runtime_error);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsInvalidEdgeReferencesAtTheDirectTraversalBoundary) {
		for (const EntityRef reference: {
					 EntityRef{0U, true, 1U, 2U, 1U},
					 EntityRef{1U, false, 0U, 0U, 0U},
					 EntityRef{10U, true, 0U, 2U, 1U},
					 EntityRef{10U, true, 1U, 0U, 1U},
			 }) {
			DirectEdgeGraph graph = directGraph();
			graph.forced_edge_reference = reference;
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "",
												"out");

			EXPECT_THROW(static_cast<void>(runtime.execute(traversal, directBounds(),
														   [](const LogicalPlanReadRow &) { return true; })),
						 std::runtime_error);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NestedGraphScopesReadOnlyTheirSelectedEntityProperties) {
		const auto list = [](std::initializer_list<PropertyValue> values) {
			return PropertyValue(std::vector<PropertyValue>(values));
		};
		const std::vector<std::pair<std::string, std::vector<PropertyValue>>> cases{
				{"[(n)-[:KNOWS]->(m) | [(m)-[:KNOWS]->(k) | k.rank + m.rank]]",
				 {list({list({PropertyValue(3)}), list({})}), list({list({PropertyValue(3), PropertyValue(4)})}),
				  list({})}},
				{"[(n)-[:KNOWS]->(m) | [(m)-[:KNOWS]->(n) | n.rank]]",
				 {list({list({PropertyValue(1)}), list({})}), list({list({PropertyValue(2)})}), list({})}},
				{"[(n)-[:KNOWS]->(m) | EXISTS((m)-[:KNOWS]->(k) WHERE k.rank < m.rank)]",
				 {list({PropertyValue(true), PropertyValue(false)}), list({PropertyValue(false)}), list({})}},
				{"EXISTS((n)-[:KNOWS]->(m) WHERE EXISTS((m)-[:KNOWS]->(k) WHERE k.rank < m.rank))",
				 {PropertyValue(true), PropertyValue(false), PropertyValue(false)}}};
		for (const bool referencePages: {false, true}) {
			for (const auto &[expression, expected]: cases) {
				SCOPED_TRACE(expression);
				DirectEdgeGraph graph = directGraph();
				graph.enable_adjacency_reference_pages = referencePages;
				graph.enable_property_tokens = true;
				for (auto &[id, node]: graph.nodes) {
					static_cast<void>(id);
					node.properties.push_back({"payload", PropertyValue(std::string(8192U, 'x'))});
				}
				for (auto &edge: graph.edges) {
					edge.properties.push_back({"payload", PropertyValue(std::string(8192U, 'x'))});
				}
				DirectEdgeBackend backend(graph);
				LogicalPlanReadRuntime runtime(backend);
				graph::parser::cypher::CypherParserImpl parser;
				const auto plan = parser.parseToLogical("MATCH (n) RETURN " + expression + " AS values");
				ASSERT_NE(plan.root, nullptr);
				auto bounded = directBounds();
				bounded.maximum_materialized_bytes = 4096U;
				std::vector<PropertyValue> values;
				static_cast<void>(runtime.execute(*plan.root, bounded, [&](const LogicalPlanReadRow &row) {
					values.push_back(std::get<PropertyValue>(row.values.at("values")));
					return true;
				}));
				EXPECT_EQ(values, expected);
				EXPECT_TRUE(graph.node_read_ids.empty());
				EXPECT_TRUE(
						std::ranges::none_of(graph.node_shaped_batch_all_properties, [](bool full) { return full; }));
				EXPECT_TRUE(
						std::ranges::none_of(graph.edge_shaped_batch_all_properties, [](bool full) { return full; }));
			}
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, NestedCompletePropertyRequirementsStayWithTheirEntityScope) {
		for (const std::string entity: {"m", "k"}) {
			DirectEdgeGraph graph = directGraph();
			graph.enable_adjacency_reference_pages = true;
			graph.enable_property_tokens = true;
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			graph::parser::cypher::CypherParserImpl parser;
			const auto plan =
					parser.parseToLogical("MATCH (n) RETURN [(n)-[:KNOWS]->(m) | [(m)-[:KNOWS]->(k) | size(keys(" +
										  entity + "))]] AS values");
			ASSERT_NE(plan.root, nullptr);
			const auto result = runtime.executeToQueryResult(*plan.root, directBounds());
			ASSERT_EQ(result.rowCount(), 3U);
			const PropertyValue one(std::vector<PropertyValue>{PropertyValue(1)});
			const PropertyValue two(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(1)});
			const PropertyValue empty(std::vector<PropertyValue>{});
			EXPECT_EQ(result.getRows()[0].at("values").asPrimitive(),
					  PropertyValue(std::vector<PropertyValue>{one, empty}));
			EXPECT_EQ(result.getRows()[1].at("values").asPrimitive(), PropertyValue(std::vector<PropertyValue>{two}));
			EXPECT_EQ(result.getRows()[2].at("values").asPrimitive(), empty);
			EXPECT_TRUE(std::ranges::any_of(graph.node_shaped_batch_all_properties, [](bool full) { return full; }));
			EXPECT_TRUE(std::ranges::any_of(graph.node_shaped_batch_all_properties, [](bool full) { return !full; }));
			EXPECT_TRUE(std::ranges::none_of(graph.edge_shaped_batch_all_properties, [](bool full) { return full; }));
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, PatternNamedColumnsPreserveUnknownTokensAndMissingValues) {
		for (const bool referencePages: {false, true}) {
			for (const bool tokens: {false, true}) {
				DirectEdgeGraph graph = directGraph();
				graph.enable_adjacency_reference_pages = referencePages;
				graph.enable_property_tokens = tokens;
				for (auto &[id, node]: graph.nodes) {
					node.properties.push_back({"nickname", PropertyValue(static_cast<int64_t>(id))});
					if (tokens)
						node.properties.push_back({"payload", PropertyValue(std::string(8192U, 'x'))});
				}
				for (auto &edge: graph.edges) {
					edge.properties.push_back({"note", PropertyValue(static_cast<int64_t>(edge.edge_id))});
					if (tokens)
						edge.properties.push_back({"payload", PropertyValue(std::string(8192U, 'x'))});
				}
				DirectEdgeBackend backend(graph);
				LogicalPlanReadRuntime runtime(backend);
				graph::parser::cypher::CypherParserImpl parser;
				const auto plan = parser.parseToLogical(
						"MATCH (n) RETURN [(n)-[r:KNOWS]->(m) | "
						"{rank: m.rank, nickname: m.nickname, note: r.note, missing: m.unregistered}] AS values");
				ASSERT_NE(plan.root, nullptr);
				auto bounded = directBounds();
				const PropertyValue outputMap(PropertyValue::MapType{{"rank", PropertyValue(int64_t{1})},
					{"nickname", PropertyValue(int64_t{1})}, {"note", PropertyValue(int64_t{1})},
					{"missing", PropertyValue()}});
				// Account for three published maps, source/output row frames and
				// list growth; still exclude even one unrequested 8 KiB payload.
				bounded.maximum_materialized_bytes = 4096U + 4U * sizeof(LogicalPlanReadRow) +
					3U * detail::retainedPropertyValueBytes(outputMap) + sizeof(PropertyValue);
				ASSERT_LT(bounded.maximum_materialized_bytes, 8192U);
				const auto result = runtime.executeToQueryResult(*plan.root, bounded);
				ASSERT_EQ(result.rowCount(), 3U);
				const auto expected = [](int64_t target, int64_t edge) {
					return PropertyValue(PropertyValue::MapType{{"rank", PropertyValue(target)},
																{"nickname", PropertyValue(target)},
																{"note", PropertyValue(edge)},
																{"missing", PropertyValue()}});
				};
				EXPECT_EQ(result.getRows()[0].at("values").asPrimitive(),
						  PropertyValue(std::vector<PropertyValue>{expected(2, 10), expected(3, 11)}));
				EXPECT_EQ(result.getRows()[1].at("values").asPrimitive(),
						  PropertyValue(std::vector<PropertyValue>{expected(1, 12)}));
				EXPECT_EQ(result.getRows()[2].at("values").asPrimitive(), PropertyValue(std::vector<PropertyValue>{}));
				if (tokens) {
					EXPECT_TRUE(std::ranges::none_of(graph.node_shaped_batch_all_properties,
													 [](bool full) { return full; }));
					EXPECT_TRUE(std::ranges::none_of(graph.edge_shaped_batch_all_properties,
													 [](bool full) { return full; }));
					EXPECT_EQ(graph.node_property_column_batch_keys,
							  (std::vector<std::vector<std::string>>{{"nickname", "unregistered"},
																	 {"nickname", "unregistered"}}));
					EXPECT_EQ(graph.edge_property_column_batch_keys,
							  (std::vector<std::vector<std::string>>{{"note"}, {"note"}}));
				}
			}
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, PatternNamedColumnsRejectMalformedBatchesAndRequestedLargeValues) {
		for (const bool referencePages: {false, true}) {
			for (const bool edgeColumn: {false, true}) {
				for (const bool oversized: {false, true}) {
					DirectEdgeGraph graph = directGraph();
					graph.enable_adjacency_reference_pages = referencePages;
					graph.enable_property_tokens = true;
					graph.truncate_node_property_batch = !edgeColumn && !oversized;
					graph.truncate_edge_property_batch = edgeColumn && !oversized;
					if (oversized) {
						for (auto &[id, node]: graph.nodes) {
							static_cast<void>(id);
							node.properties.push_back({"payload", PropertyValue(std::string(8192U, 'p'))});
						}
						for (auto &edge: graph.edges)
							edge.properties.push_back({"payload", PropertyValue(std::string(8192U, 'p'))});
					}
					DirectEdgeBackend backend(graph);
					LogicalPlanReadRuntime runtime(backend);
					graph::parser::cypher::CypherParserImpl parser;
					const auto plan = parser.parseToLogical("MATCH (n) RETURN [(n)-[r:KNOWS]->(m) | " +
															std::string(edgeColumn ? "r" : "m") +
															(oversized ? ".payload" : ".unregistered") + "] AS values");
					ASSERT_NE(plan.root, nullptr);
					auto bounded = directBounds();
					bounded.maximum_materialized_bytes = 4096U;
					EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounded)),
								 std::runtime_error);
				}
			}
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CompletePatternMapsDoNotAcquireMissingNamedFields) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		graph::parser::cypher::CypherParserImpl parser;
		const auto plan = parser.parseToLogical(
				"MATCH (n) RETURN [(n)-[:KNOWS]->(m) | {props: properties(m), missing: m.unregistered}] AS values");
		ASSERT_NE(plan.root, nullptr);
		const auto result = runtime.executeToQueryResult(*plan.root, directBounds());
		ASSERT_EQ(result.rowCount(), 3U);
		for (const auto &row: result.getRows()) {
			for (const auto &value: row.at("values").asPrimitive().getList()) {
				const auto &map = value.getMap();
				EXPECT_EQ(map.at("missing"), PropertyValue());
				EXPECT_EQ(map.at("props").getMap().size(), 1U);
				EXPECT_FALSE(map.at("props").getMap().contains("unregistered"));
			}
		}
		EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true, true}));
		EXPECT_TRUE(std::ranges::none_of(graph.edge_shaped_batch_all_properties, [](bool full) { return full; }));
	}


	TEST(LogicalPlanDirectEdgeRuntimeTest, ReferenceTargetPlanLiteralsKeepIndependentOutputAndPropertyOrder) {
		for (const bool earlyStop : {false, true}) {
			DirectEdgeGraph graph = directGraph();
			graph.enable_adjacency_reference_pages = true;
			const PropertyValue payload(std::string(8192, 'v'));
			graph.nodes.at(2).properties.emplace_back("payload", payload);
			graph.edges.emplace_back(13, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
			graph.edges.emplace_back(14, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			std::vector<PropertyValue> escaped;
			{
				auto traversal = std::make_unique<logical::LogicalTraversal>(
					std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Person"}), "a", "r", "b", "", "out",
					std::vector<std::string>{},
					std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(2)}, {"payload", payload}});
				std::vector<logical::LogicalProjectItem> items;
				items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b", "payload"), "value");
				logical::LogicalProject project(std::move(traversal), std::move(items));
				static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
					escaped.push_back(std::get<PropertyValue>(row.values.at("value")));
					return !earlyStop;
				}));
			}
			graph.nodes.clear();
			graph.edges.clear();
			EXPECT_EQ(escaped, std::vector<PropertyValue>(earlyStop ? 1 : 3, payload));
			EXPECT_GT(graph.node_property_column_batch_reads, 0U);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ReferenceTargetPlanLiteralsRetainDefinitionAndNullChecks) {
		for (const bool constrained : {false, true}) {
		for (const auto &predicates : std::vector<std::vector<std::pair<std::string, PropertyValue>>>{
			{{"", PropertyValue(2)}}, {{"rank", PropertyValue(2)}, {"rank", PropertyValue(2)}},
			{{"rank", PropertyValue{}}}}) {
			DirectEdgeGraph graph = directGraph();
			graph.enable_adjacency_reference_pages = true;
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a", constrained ? std::vector<std::string>{"Person"} : std::vector<std::string>{}),
				"a", "r", "b", "", "out", {}, predicates);
			if (predicates.front().first.empty() || predicates.size() == 2) {
				EXPECT_THROW(static_cast<void>(runtime.execute(traversal, directBounds(), [](const auto &) { return true; })),
					std::invalid_argument);
			} else {
				size_t rows = 0;
				static_cast<void>(runtime.execute(traversal, directBounds(), [&](const auto &) { ++rows; return true; }));
				EXPECT_EQ(rows, 0U);
			}
		}
		}
	}
} // namespace graph::query::execution
