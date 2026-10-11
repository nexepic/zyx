#include "CypherParserImpl.hpp"
#include "graph/query/execution/SelectionVector.hpp"
#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"

namespace graph::query::execution {
	using namespace direct_edge_test;

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesEdgePropertySortKeysBeforeOrdering) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a"),
																	 "a", "r", "b", "KNOWS", "out");
		std::vector<logical::LogicalSortItem> sortItems;
		sortItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "weight"), true);
		logical::LogicalSort sort(std::move(traversal), std::move(sortItems));

		std::vector<int64_t> weights;
		static_cast<void>(runtime.execute(sort, directBounds(), [&weights](const LogicalPlanReadRow &row) {
			weights.push_back(std::get<int64_t>(
					std::get<LogicalGraphEdge>(row.values.at("r")).properties.front().value.getVariant()));
			return true;
		}));

		EXPECT_EQ(weights, (std::vector<int64_t>{7, 7, 8}));
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AdmitsDirectTraversalEdgePropertyColumnsBeforeCopyingPayloads) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_read_admission = true;
		const PropertyValue payload(std::string(4096U, 'e'));
		for (LogicalGraphEdge &edge: graph.edges)
			edge.properties.emplace_back("payload", payload);
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out",
											{}, {}, {{"payload", payload}});
		LogicalPlanReadRuntimeBounds limits = directBounds();
		limits.maximum_materialized_bytes = 1024U;

		EXPECT_THROW(
				static_cast<void>(runtime.execute(traversal, limits, [](const LogicalPlanReadRow &) { return true; })),
				std::runtime_error);
		EXPECT_EQ(graph.edge_property_column_admission_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AdmitsDirectTraversalTargetColumnsBeforeCopyingPayloads) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_read_admission = true;
		const PropertyValue payload(std::string(4096U, 't'));
		for (auto &[id, node]: graph.nodes) {
			static_cast<void>(id);
			node.properties.emplace_back("payload", payload);
		}
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out",
											{}, {{"payload", payload}});
		LogicalPlanReadRuntimeBounds limits = directBounds();
		limits.maximum_materialized_bytes = 1024U;

		EXPECT_THROW(
				static_cast<void>(runtime.execute(traversal, limits, [](const LogicalPlanReadRow &) { return true; })),
				std::runtime_error);
		EXPECT_EQ(graph.node_property_column_admission_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AdmitsConstrainedSourceColumnsBeforeCopyingPayloads) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_read_admission = true;
		const PropertyValue payload(std::string(4096U, 's'));
		for (auto &[id, node]: graph.nodes) {
			static_cast<void>(id);
			node.properties.emplace_back("payload", payload);
		}
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{},
				std::vector<std::pair<std::string, PropertyValue>>{{"payload", payload}});
		logical::LogicalTraversal traversal(std::move(source), "a", "r", "b", "", "out");
		LogicalPlanReadRuntimeBounds limits = directBounds();
		limits.maximum_materialized_bytes = 1024U;

		EXPECT_THROW(
				static_cast<void>(runtime.execute(traversal, limits, [](const LogicalPlanReadRow &) { return true; })),
				std::runtime_error);
		EXPECT_EQ(graph.node_property_column_admission_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesAdjacentTargetPropertyPredicatesByReferencePage) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out",
											{}, {{"rank", PropertyValue(2)}});

		std::vector<uint64_t> matchingEdges;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&](const LogicalPlanReadRow &row) {
			matchingEdges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(matchingEdges, std::vector<uint64_t>{10U});
		// All adjacent references share one sorted target-column read.
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AdmitsAdjacentTargetColumnsBeforeCopyingPayloads) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_read_admission = true;
		const PropertyValue payload(std::string(4096U, 'n'));
		for (auto &[id, node]: graph.nodes) {
			static_cast<void>(id);
			node.properties.emplace_back("payload", payload);
		}
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out",
											{}, {{"payload", payload}});
		LogicalPlanReadRuntimeBounds limits = directBounds();
		limits.maximum_materialized_bytes = 1024U;

		EXPECT_THROW(
				static_cast<void>(runtime.execute(traversal, limits, [](const LogicalPlanReadRow &) { return true; })),
				std::runtime_error);
		EXPECT_EQ(graph.node_property_column_admission_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ReservesAdjacentTargetColumnFramesBeforeReadingMissingProperties) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_read_admission = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out",
											{}, {{"missing", PropertyValue(1)}});
		LogicalPlanReadRuntimeBounds limits = directBounds();
		limits.maximum_materialized_bytes = 64U;

		EXPECT_THROW(
				static_cast<void>(runtime.execute(traversal, limits, [](const LogicalPlanReadRow &) { return true; })),
				std::runtime_error);
		EXPECT_EQ(graph.node_property_column_admission_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ReservesAdjacentEdgeColumnFramesBeforeReadingMissingProperties) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_read_admission = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "", "out",
											{}, {}, {{"missing", PropertyValue(1)}});
		LogicalPlanReadRuntimeBounds limits = directBounds();
		limits.maximum_materialized_bytes = 64U;

		EXPECT_THROW(
				static_cast<void>(runtime.execute(traversal, limits, [](const LogicalPlanReadRow &) { return true; })),
				std::runtime_error);
		EXPECT_EQ(graph.edge_property_column_admission_reads, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, AccountsPendingReferencesForConstrainedSourceTraversal) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_read_admission = true;
		const PropertyValue payload(std::string(4096U, 'p'));
		for (LogicalGraphEdge &edge: graph.edges)
			edge.properties.emplace_back("payload", payload);
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
				std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Person"}), "a", "r", "b", "",
				"out", {}, {}, {{"payload", payload}});
		LogicalPlanReadRuntimeBounds limits = directBounds();
		limits.maximum_materialized_bytes = 1024U;

		EXPECT_THROW(
				static_cast<void>(runtime.execute(traversal, limits, [](const LogicalPlanReadRow &) { return true; })),
				std::runtime_error);
		EXPECT_EQ(graph.edge_property_column_admission_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesSafeSubsetOfMixedSortKeys) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		std::vector<logical::LogicalSortItem> sortItems;
		sortItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), true);
		std::vector<std::unique_ptr<expressions::Expression>> propertiesArguments;
		propertiesArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		sortItems.emplace_back(
				std::make_shared<expressions::FunctionCallExpression>("properties", std::move(propertiesArguments)),
				true);
		logical::LogicalSort sort(std::move(scan), std::move(sortItems));

		std::vector<int64_t> ranks;
		static_cast<void>(runtime.execute(sort, directBounds(), [&ranks](const LogicalPlanReadRow &row) {
			ranks.push_back(static_cast<int64_t>(std::get<LogicalGraphNode>(row.values.at("n")).node_id));
			return true;
		}));

		// `properties(n)` deliberately requires a complete entity map. The shared
		// batch planner satisfies both sort keys with one complete shaped read;
		// it must not fall back to one complete point read per row.
		EXPECT_EQ(ranks, (std::vector<int64_t>{1, 2, 3}));
		EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_batch_property_tokens, (std::vector<std::vector<uint32_t>>{{}}));
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	// Measure container allocations for the complete records kept by these tests.
	// The remaining allowance holds a bounded 64-record working window, not
	// another complete 300-record cache.
	template<typename Record>
	static size_t completeRecordCacheAllowance(const std::vector<Record> &records) {
		MaterializedByteBudget budget(16U * 1024U * 1024U);
		detail::BudgetMemoryResource memory(budget);
		std::pmr::unordered_map<uint64_t, Record> cache(&memory);
		size_t payloadBytes = 0U;
		uint64_t id = 0U;
		for (const Record &record: records) {
			const auto &owned = cache.emplace(++id, record).first->second;
			payloadBytes += owned.properties.capacity() * sizeof(storage::LogicalGraphPropertyEntry);
			for (const auto &entry: owned.properties) {
				payloadBytes += std::max(entry.key.size(), detail::dynamicStringBufferBytes(entry.key));
				payloadBytes += detail::retainedPropertyValueBytes(entry.value);
			}
			if constexpr (std::is_same_v<Record, LogicalGraphNode>) {
				payloadBytes += owned.labels.capacity() * sizeof(std::string);
				for (const auto &label: owned.labels)
					payloadBytes += std::max(label.size(), detail::dynamicStringBufferBytes(label));
			} else {
				payloadBytes += std::max(owned.type.size(), detail::dynamicStringBufferBytes(owned.type));
			}
		}
		const size_t cacheBytes = budget.used() + payloadBytes;
		const size_t workingBytes = cache.bucket_count() * sizeof(void *) +
				64U * (sizeof(LogicalPlanReadRow) + sizeof(LogicalPlanReadValue) + sizeof(Record) +
						2U * sizeof(storage::LogicalGraphPropertyEntry) + 64U) + 4096U;
		EXPECT_LT(workingBytes, cacheBytes) << "working allowance must not fit a second complete cache";
		return cacheBytes + workingBytes;
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesCompleteEntityReferencesAndPreservesProperties) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		const PropertyValue payload(std::string(64U, 'p'));
		for (auto &[nodeId, node]: graph.nodes) {
			static_cast<void>(nodeId);
			node.properties.emplace_back("payload", payload);
		}
		for (uint64_t nodeId = 4U; nodeId <= 300U; ++nodeId) {
			graph.nodes.emplace(nodeId, LogicalGraphNode{nodeId, {"Person"}, {{"payload", payload}}});
		}
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		auto properties = std::make_shared<expressions::FunctionCallExpression>("properties", std::move(arguments));
		logical::LogicalProject project(std::make_unique<logical::LogicalNodeScan>("n"), {{properties, "props"}});
		LogicalPlanReadRuntimeBounds bounds = directBounds();
		bounds.maximum_node_records = 512U;
		bounds.maximum_rows = 512U;
		std::vector<LogicalGraphNode> records;
		for (const auto &[id, node]: graph.nodes) records.push_back(node);
		bounds.maximum_materialized_bytes = completeRecordCacheAllowance(records);
		std::vector<PropertyValue> values;

		static_cast<void>(runtime.execute(project, bounds, [&values](const LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("props")));
			return true;
		}));

		ASSERT_EQ(values.size(), 300U);
		for (const PropertyValue &value: values) {
			using PropertyMap = std::unordered_map<std::string, PropertyValue>;
			ASSERT_TRUE((std::holds_alternative<PropertyMap>(value.getVariant())));
			EXPECT_EQ(std::get<PropertyMap>(value.getVariant()).at("payload"), payload);
		}
		EXPECT_GT(graph.node_shaped_batch_reads, 3U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_TRUE(graph.node_read_ids.empty());
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesCompleteEdgeEntityReferencesUnderSharedBudget) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		for (uint64_t nodeId = 4U; nodeId <= 300U; ++nodeId) {
			graph.nodes.emplace(nodeId, LogicalGraphNode{nodeId, {"Person"}, {}});
		}
		const PropertyValue payload(std::string(64U, 'e'));
		graph.edges.clear();
		for (uint64_t targetId = 2U; targetId <= 300U; ++targetId) {
			graph.edges.emplace_back(targetId, 1U, targetId, "KNOWS",
								 std::vector<storage::LogicalGraphPropertyEntry>{{"payload", payload}});
		}
		graph.edges.emplace_back(301U, 1U, 2U, "KNOWS",
							 std::vector<storage::LogicalGraphPropertyEntry>{{"payload", payload}});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Person"}), "a", "r", "b",
				"KNOWS", "out");
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("r"));
		auto properties = std::make_shared<expressions::FunctionCallExpression>("properties", std::move(arguments));
		logical::LogicalProject project(std::move(traversal), {{properties, "props"}});
		LogicalPlanReadRuntimeBounds bounds = directBounds();
		bounds.maximum_node_records = 512U;
		bounds.maximum_edge_records = 512U;
		bounds.maximum_edges_per_node = 512U;
		bounds.maximum_adjacency_page_records = 512U;
		bounds.maximum_rows = 512U;
		bounds.maximum_materialized_bytes = completeRecordCacheAllowance(graph.edges) + entityCacheMetadataDelta(graph)
				+ sizeof(LogicalGraphEdgeTopology) + sizeof(uint64_t) + sizeof(size_t);
		std::vector<PropertyValue> values;

		static_cast<void>(runtime.execute(project, bounds, [&values](const LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("props")));
			return true;
		}));

		ASSERT_EQ(values.size(), 300U);
		for (const PropertyValue &value: values) {
			using PropertyMap = std::unordered_map<std::string, PropertyValue>;
			ASSERT_TRUE((std::holds_alternative<PropertyMap>(value.getVariant())));
			EXPECT_EQ(std::get<PropertyMap>(value.getVariant()).at("payload"), payload);
		}
		EXPECT_GT(graph.edge_shaped_batch_reads, 3U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_TRUE(graph.edge_read_ids.empty());
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesDeferredNodeSortKeysBeforeQueryResultMaterialization) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		// Make the physical Node order differ from the selected Property order so the
		// test proves that sorting used the deferred EntityRef's batched column.
		graph.nodes.at(1U).properties.front().value = PropertyValue(3);
		graph.nodes.at(2U).properties.front().value = PropertyValue(1);
		graph.nodes.at(3U).properties.front().value = PropertyValue(2);
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalSortItem> sortItems;
		sortItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), true);
		logical::LogicalSort sort(std::make_unique<logical::LogicalNodeScan>("n"), std::move(sortItems));

		const QueryResult result = runtime.executeToQueryResult(sort, directBounds());
		std::vector<uint64_t> nodeIds;
		for (const QueryResult::Row &row: result.getRows()) {
			nodeIds.push_back(row.at("n").asNode().getId());
		}

		EXPECT_EQ(nodeIds, (std::vector<uint64_t>{2U, 3U, 1U}));
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		// QueryResult permits bounded cross-row read-ahead, so the deferred references
		// are materialized by one complete shaped batch after sorting.
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_batch_property_tokens, (std::vector<std::vector<uint32_t>>{{}}));
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesCompleteEntityProjectionAcrossNodeRows) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto arguments = std::vector<std::unique_ptr<expressions::Expression>>{};
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("properties", std::move(arguments)),
						   "props");
		logical::LogicalProject project(std::make_unique<logical::LogicalNodeScan>("n"), std::move(items));

		std::vector<PropertyValue::MapType> properties;
		static_cast<void>(runtime.execute(project, directBounds(), [&properties](const LogicalPlanReadRow &row) {
			properties.push_back(std::get<PropertyValue>(row.values.at("props")).getMap());
			return true;
		}));

		ASSERT_EQ(properties.size(), 3U);
		EXPECT_EQ(properties[0].at("rank"), PropertyValue(1));
		EXPECT_EQ(properties[1].at("rank"), PropertyValue(2));
		EXPECT_EQ(properties[2].at("rank"), PropertyValue(3));
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_batch_property_tokens, (std::vector<std::vector<uint32_t>>{{}}));
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesImportedPropertiesAcrossReadOnlyCorrelatedSubqueryRows) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);

		std::vector<logical::LogicalProjectItem> bodyItems;
		bodyItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank");
		auto body = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(),
															  std::move(bodyItems));
		auto input = std::make_unique<logical::LogicalNodeScan>("n");
		logical::LogicalCallSubquery call(std::move(input), std::move(body), std::vector<std::string>{"n"},
										  std::vector<std::string>{"rank"});

		const QueryResult result = runtime.executeToQueryResult(call, directBounds());
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(result.getRows()[2].at("rank").asPrimitive(), PropertyValue(3));
		// The imported Entity Property is prefetched once across all outer rows;
		// the subquery body still evaluates one logical row at a time.
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesImportedListPropertiesForCorrelatedSubqueryUnwind) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		graph.nodes.at(1U).properties.emplace_back(
				"tags", PropertyValue(std::vector<PropertyValue>{PropertyValue("a"), PropertyValue("b")}));
		graph.nodes.at(2U).properties.emplace_back("tags",
												   PropertyValue(std::vector<PropertyValue>{PropertyValue("c")}));
		graph.nodes.at(3U).properties.emplace_back("tags", PropertyValue(std::vector<PropertyValue>{}));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		const auto tags = std::make_shared<expressions::VariableReferenceExpression>("n", "tags");
		auto unwind =
				std::make_unique<logical::LogicalUnwind>(std::make_unique<logical::LogicalSingleRow>(), "tag", tags);
		std::vector<logical::LogicalProjectItem> bodyItems;
		bodyItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("tag"), "tag");
		auto body = std::make_unique<logical::LogicalProject>(std::move(unwind), std::move(bodyItems));
		logical::LogicalCallSubquery call(std::make_unique<logical::LogicalNodeScan>("n"), std::move(body),
										  std::vector<std::string>{"n"}, std::vector<std::string>{"tag"});

		const QueryResult result = runtime.executeToQueryResult(call, directBounds());
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("tag").asPrimitive(), PropertyValue("a"));
		EXPECT_EQ(result.getRows()[1].at("tag").asPrimitive(), PropertyValue("b"));
		EXPECT_EQ(result.getRows()[2].at("tag").asPrimitive(), PropertyValue("c"));
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"tags"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_shaped_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesCompleteImportedEntityAcrossReadOnlyCorrelatedSubqueryRows) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);

		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		std::vector<logical::LogicalProjectItem> bodyItems;
		bodyItems.emplace_back(
				std::make_shared<expressions::FunctionCallExpression>("properties", std::move(arguments)), "props");
		auto body = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(),
															  std::move(bodyItems));
		logical::LogicalCallSubquery call(std::make_unique<logical::LogicalNodeScan>("n"), std::move(body),
										  std::vector<std::string>{"n"}, std::vector<std::string>{"props"});

		const QueryResult result = runtime.executeToQueryResult(call, directBounds());
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("props").asPrimitive().getMap().at("rank"), PropertyValue(1));
		EXPECT_EQ(result.getRows()[1].at("props").asPrimitive().getMap().at("rank"), PropertyValue(2));
		EXPECT_EQ(result.getRows()[2].at("props").asPrimitive().getMap().at("rank"), PropertyValue(3));
		// Complete Entity expressions are prefetched once across all imported rows;
		// the subquery body still evaluates one logical row at a time.
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_batch_property_tokens, (std::vector<std::vector<uint32_t>>{{}}));
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CallSubqueryCarriesEntityMetadataCachesAcrossImportedAndReturnedAliases) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		const auto labelsExpression = [](const std::string &variable) {
			std::vector<std::unique_ptr<expressions::Expression>> arguments;
			arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>(variable));
			return std::make_shared<expressions::FunctionCallExpression>("labels", std::move(arguments));
		};

		std::vector<logical::LogicalProjectItem> inputItems;
		inputItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "n");
		inputItems.emplace_back(labelsExpression("n"), "outer_labels");
		auto input = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalNodeScan>("n"),
															   std::move(inputItems));

		std::vector<logical::LogicalProjectItem> bodyItems;
		bodyItems.emplace_back(labelsExpression("n"), "inner_labels");
		bodyItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "m");
		auto body = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(),
															  std::move(bodyItems));
		auto call = std::make_unique<logical::LogicalCallSubquery>(std::move(input), std::move(body),
																   std::vector<std::string>{"n"},
																   std::vector<std::string>{"inner_labels", "m"});
		std::vector<logical::LogicalProjectItem> outputItems;
		outputItems.emplace_back(labelsExpression("m"), "labels_after_subquery");
		logical::LogicalProject project(std::move(call), std::move(outputItems));

		const QueryResult result = runtime.executeToQueryResult(project, directBounds());
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("labels_after_subquery").asPrimitive(),
				  PropertyValue(std::vector<PropertyValue>{PropertyValue("Person")}));
		EXPECT_EQ(result.getRows()[1].at("labels_after_subquery").asPrimitive(),
				  PropertyValue(std::vector<PropertyValue>{PropertyValue("Person")}));
		EXPECT_EQ(result.getRows()[2].at("labels_after_subquery").asPrimitive(),
				  PropertyValue(std::vector<PropertyValue>{PropertyValue("Other")}));
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesCompleteEntityPayloadAfterJoinAndOptionalMatch) {
		const auto makePropertiesProjection = [] {
			std::vector<std::unique_ptr<expressions::Expression>> arguments;
			arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
			std::vector<logical::LogicalProjectItem> items;
			items.emplace_back(
					std::make_shared<expressions::FunctionCallExpression>("properties", std::move(arguments)), "props");
			return items;
		};

		DirectEdgeGraph joinGraph = directGraph();
		joinGraph.enable_property_tokens = true;
		DirectEdgeBackend joinBackend(joinGraph);
		LogicalPlanReadRuntime joinRuntime(joinBackend);
		auto join = std::make_unique<logical::LogicalJoin>(std::make_unique<logical::LogicalNodeScan>("n"),
														   std::make_unique<logical::LogicalNodeScan>("m"));
		logical::LogicalProject joined(std::move(join), makePropertiesProjection());
		const QueryResult joinResult = joinRuntime.executeToQueryResult(joined, directBounds());

		ASSERT_EQ(joinResult.rowCount(), 9U);
		std::vector<int64_t> joinRanks;
		for (const auto &row: joinResult.getRows()) {
			joinRanks.push_back(std::get<int64_t>(row.at("props").asPrimitive().getMap().at("rank").getVariant()));
		}
		EXPECT_EQ(joinRanks, (std::vector<int64_t>{1, 1, 1, 2, 2, 2, 3, 3, 3}));
		EXPECT_EQ(joinGraph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(joinGraph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(joinGraph.node_shaped_reads, 0U);
		EXPECT_EQ(joinGraph.node_read_ids, std::vector<uint64_t>{});

		DirectEdgeGraph optionalGraph = directGraph();
		optionalGraph.enable_property_tokens = true;
		DirectEdgeBackend optionalBackend(optionalGraph);
		LogicalPlanReadRuntime optionalRuntime(optionalBackend);
		auto optional = std::make_unique<logical::LogicalOptionalMatch>(
				std::make_unique<logical::LogicalNodeScan>("n"), std::make_unique<logical::LogicalNodeScan>("m"));
		logical::LogicalProject optionalProjection(std::move(optional), makePropertiesProjection());
		const QueryResult optionalResult = optionalRuntime.executeToQueryResult(optionalProjection, directBounds());

		ASSERT_EQ(optionalResult.rowCount(), 9U);
		std::vector<int64_t> optionalRanks;
		for (const auto &row: optionalResult.getRows()) {
			optionalRanks.push_back(std::get<int64_t>(row.at("props").asPrimitive().getMap().at("rank").getVariant()));
		}
		EXPECT_EQ(optionalRanks, joinRanks);
		EXPECT_EQ(optionalGraph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(optionalGraph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(optionalGraph.node_shaped_reads, 0U);
		EXPECT_EQ(optionalGraph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesCompleteEntityFilterPredicateAcrossNodeRows) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::FunctionCallExpression>("properties", std::move(arguments)),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(
						PropertyValue(PropertyValue::MapType{{"rank", PropertyValue(2)}})));
		logical::LogicalFilter filter(std::make_unique<logical::LogicalNodeScan>("n"), std::move(predicate));

		std::vector<uint64_t> nodeIds;
		static_cast<void>(runtime.execute(filter, directBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
			nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
			return true;
		}));

		EXPECT_EQ(nodeIds, (std::vector<uint64_t>{2U}));
		// The complete map predicate is evaluated from one bounded shaped batch;
		// rejected Nodes must not fall back to one complete read each.
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_batch_property_tokens, (std::vector<std::vector<uint32_t>>{{}}));
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesCompleteEntityKeysFilterAndPreservesDuplicateRows) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::FunctionCallExpression>("keys", std::move(arguments)),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::ListLiteralExpression>(
						PropertyValue(std::vector<PropertyValue>{PropertyValue("rank")})));
		auto left = std::make_unique<logical::LogicalNodeScan>("n");
		auto right = std::make_unique<logical::LogicalNodeScan>("n");
		auto unionAll = std::make_unique<logical::LogicalUnion>(std::move(left), std::move(right), true);
		logical::LogicalFilter filter(std::move(unionAll), std::move(predicate));

		std::vector<uint64_t> nodeIds;
		static_cast<void>(runtime.execute(filter, directBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
			nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
			return true;
		}));

		EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U, 2U, 3U, 1U, 2U, 3U}));
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_batch_property_tokens, (std::vector<std::vector<uint32_t>>{{}}));
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CompleteEntityFilterTreatsNullPredicateAsUnknown) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::FunctionCallExpression>("properties", std::move(arguments)),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::LiteralExpression>(PropertyValue()));
		logical::LogicalFilter filter(std::make_unique<logical::LogicalNodeScan>("n"), std::move(predicate));

		size_t emitted = 0U;
		static_cast<void>(runtime.execute(filter, directBounds(), [&emitted](const LogicalPlanReadRow &) {
			++emitted;
			return true;
		}));

		EXPECT_EQ(emitted, 0U);
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CompleteEntityFilterStopsAtTheVisitorBoundary) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::FunctionCallExpression>("keys", std::move(arguments)),
				expressions::BinaryOperatorType::BOP_EQUAL,
				std::make_unique<expressions::ListLiteralExpression>(
						PropertyValue(std::vector<PropertyValue>{PropertyValue("rank")})));
		logical::LogicalFilter filter(std::make_unique<logical::LogicalNodeScan>("n"), std::move(predicate));

		std::vector<uint64_t> nodeIds;
		static_cast<void>(runtime.execute(filter, directBounds(), [&nodeIds](const LogicalPlanReadRow &row) {
			nodeIds.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
			return false;
		}));

		// The public visitor receives exactly one row and stops the output path;
		// the complete read remains one bounded batch rather than a point-read loop.
		EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U}));
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesCompleteEntityAggregateArguments) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		const auto properties =
				std::make_shared<expressions::FunctionCallExpression>("properties", std::move(arguments));
		logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
											{{"count", properties, "property_maps", false}});

		std::vector<PropertyValue> counts;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&counts](const LogicalPlanReadRow &row) {
			counts.push_back(std::get<PropertyValue>(row.values.at("property_maps")));
			return true;
		}));

		EXPECT_EQ(counts, (std::vector<PropertyValue>{PropertyValue(3)}));
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesSelectedPropertyAndCompleteEntityWithinOneAggregateExpression) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<std::unique_ptr<expressions::Expression>> keyArguments;
		keyArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		auto keys = std::make_unique<expressions::FunctionCallExpression>("keys", std::move(keyArguments));
		std::vector<std::unique_ptr<expressions::Expression>> sizeArguments;
		sizeArguments.push_back(std::move(keys));
		auto keyCount = std::make_unique<expressions::FunctionCallExpression>("size", std::move(sizeArguments));
		auto combined = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("n", "rank"),
				expressions::BinaryOperatorType::BOP_ADD, std::move(keyCount));
		logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {},
											{{"sum", combined, "total", false}});

		std::vector<PropertyValue> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			results.push_back(std::get<PropertyValue>(row.values.at("total")));
			return true;
		}));

		EXPECT_EQ(results, (std::vector<PropertyValue>{PropertyValue(int64_t{9})}));
		// The same expression needs both n.rank and the complete keys(n) map. The
		// batch path should satisfy both from one complete shaped Entity read.
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_shaped_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, KeepsSafeCompleteEntityAggregateBatchBesideExistsExpression) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<std::unique_ptr<expressions::Expression>> keyArguments;
		keyArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		auto keys = std::make_shared<expressions::FunctionCallExpression>("keys", std::move(keyArguments));
		auto exists = std::make_shared<expressions::ExistsExpression>("(n)-[:KNOWS]->()", "n", "KNOWS", "",
																	  expressions::PatternDirection::PAT_OUTGOING);
		logical::LogicalAggregate aggregate(std::make_unique<logical::LogicalNodeScan>("n"), {exists},
											{{"count", keys, "property_key_count", false}}, {"has_outgoing"});

		std::map<bool, int64_t> counts;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&counts](const LogicalPlanReadRow &row) {
			const bool hasOutgoing =
					std::get<bool>(std::get<PropertyValue>(row.values.at("has_outgoing")).getVariant());
			const int64_t keyCount =
					std::get<int64_t>(std::get<PropertyValue>(row.values.at("property_key_count")).getVariant());
			counts.emplace(hasOutgoing, keyCount);
			return true;
		}));

		EXPECT_EQ(counts, (std::map<bool, int64_t>{{false, 1}, {true, 2}}));
		// EXISTS remains on its scoped evaluator, while the independent complete
		// Entity requirement is still fetched once for the whole Aggregate batch.
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
		EXPECT_EQ(graph.adjacency_ref_scans, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesCompleteEntityInsideCaseBesideNestedExistsScope) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto exists = std::make_unique<expressions::ExistsExpression>("(n)-[:KNOWS]->()", "n", "KNOWS", "",
																	  expressions::PatternDirection::PAT_OUTGOING);
		std::vector<std::unique_ptr<expressions::Expression>> keyArguments;
		keyArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		auto keys = std::make_unique<expressions::FunctionCallExpression>("keys", std::move(keyArguments));
		std::vector<std::unique_ptr<expressions::Expression>> sizeArguments;
		sizeArguments.push_back(std::move(keys));
		auto size = std::make_unique<expressions::FunctionCallExpression>("size", std::move(sizeArguments));
		auto expression = std::make_unique<expressions::CaseExpression>();
		expression->addBranch(std::move(exists), std::move(size));
		expression->setElseExpression(std::make_unique<expressions::LiteralExpression>(int64_t{0}));
		logical::LogicalAggregate aggregate(
				std::make_unique<logical::LogicalNodeScan>("n"), {},
				{{"sum", std::shared_ptr<expressions::Expression>(std::move(expression)), "matched_key_count", false}});

		std::vector<PropertyValue> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			results.push_back(std::get<PropertyValue>(row.values.at("matched_key_count")));
			return true;
		}));

		EXPECT_EQ(results, (std::vector<PropertyValue>{PropertyValue(int64_t{2})}));
		// The collector finds keys(n) after the unsupported EXISTS subtree without
		// entering that graph scope, so both evaluation paths retain their contract.
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
		EXPECT_EQ(graph.adjacency_ref_scans, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesGroupedAggregateWithCompleteEntityArguments) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto rank = std::make_shared<expressions::VariableReferenceExpression>("n", "rank");
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n"));
		const auto properties =
				std::make_shared<expressions::FunctionCallExpression>("properties", std::move(arguments));
		logical::LogicalAggregate aggregate(
				std::make_unique<logical::LogicalNodeScan>("n"), {rank},
				{{"sum", rank, "total_rank", false}, {"count", properties, "property_maps", false}}, {"rank"});

		std::map<int64_t, std::pair<int64_t, int64_t>> results;
		static_cast<void>(runtime.execute(aggregate, directBounds(), [&results](const LogicalPlanReadRow &row) {
			const int64_t group = std::get<int64_t>(std::get<PropertyValue>(row.values.at("rank")).getVariant());
			const int64_t total = std::get<int64_t>(std::get<PropertyValue>(row.values.at("total_rank")).getVariant());
			const int64_t propertyMaps =
					std::get<int64_t>(std::get<PropertyValue>(row.values.at("property_maps")).getVariant());
			results.emplace(group, std::make_pair(total, propertyMaps));
			return true;
		}));

		ASSERT_EQ(results.size(), 3U);
		for (const int64_t value: {1, 2, 3}) {
			EXPECT_EQ(results.at(value), (std::pair<int64_t, int64_t>{value, 1}));
		}
		// The grouped scalar sibling still uses its selected column, while the
		// complete Entity argument is fetched once for the bounded input batch.
		EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
		EXPECT_EQ(graph.node_shaped_batch_reads, 1U);
		EXPECT_EQ(graph.node_shaped_batch_property_tokens, (std::vector<std::vector<uint32_t>>{{}}));
		EXPECT_EQ(graph.node_shaped_batch_all_properties, (std::vector<bool>{true}));
		EXPECT_EQ(graph.node_shaped_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, PreservesReversedPropertyComparisonSemantics) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>("a");
		auto traversal = std::make_unique<logical::LogicalTraversal>(std::move(source), "a", "r", "b", "KNOWS", "out");
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::LiteralExpression>(int64_t{7}), expressions::BinaryOperatorType::BOP_LESS,
				std::make_unique<expressions::VariableReferenceExpression>("r", "weight"));
		logical::LogicalFilter filter(std::move(traversal), std::move(predicate));

		std::vector<uint64_t> edgeIds;
		static_cast<void>(runtime.execute(filter, directBounds(), [&edgeIds](const LogicalPlanReadRow &row) {
			edgeIds.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edgeIds, std::vector<uint64_t>{12U});
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesAllSourcePredicateColumnsInOneSortedRead) {
		DirectEdgeGraph graph = directGraph();
		graph.nodes.at(1U).properties.emplace_back("score", PropertyValue(int64_t{10}));
		graph.nodes.at(2U).properties.emplace_back("score", PropertyValue(int64_t{20}));
		graph.nodes.at(3U).properties.emplace_back("score", PropertyValue(int64_t{30}));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"rank", PropertyValue(1)},
																   {"score", PropertyValue(int64_t{10})}});
		logical::LogicalTraversal traversal(std::move(source), "a", "r", "b", "KNOWS", "out");

		std::vector<uint64_t> edges;
		static_cast<void>(runtime.execute(traversal, directBounds(), [&edges](const LogicalPlanReadRow &row) {
			edges.push_back(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id);
			return true;
		}));

		EXPECT_EQ(edges, (std::vector<uint64_t>{10U, 11U}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_property_sorted_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank", "score"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
		EXPECT_EQ(graph.edge_property_column_batch_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, TransfersSourcePredicateColumnIntoDownstreamRowCache) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		const PropertyValue payload(std::string(4096U, 'p'));
		for (auto &[id, node]: graph.nodes) {
			static_cast<void>(id);
			node.properties.emplace_back("payload", payload);
		}
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto source = std::make_unique<logical::LogicalNodeScan>(
				"a", std::vector<std::string>{"Person"},
				std::vector<std::pair<std::string, PropertyValue>>{{"payload", payload}});
		auto traversal = std::make_unique<logical::LogicalTraversal>(std::move(source), "a", "r", "b", "KNOWS", "out");
		logical::LogicalProject project(
				std::move(traversal),
				{{std::make_shared<expressions::VariableReferenceExpression>("a", "payload"), "payload"}});

		std::vector<PropertyValue> values;
		static_cast<void>(runtime.execute(project, directBounds(), [&values](const LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("payload")));
			return true;
		}));

		ASSERT_FALSE(values.empty());
		for (const PropertyValue &value: values)
			EXPECT_EQ(value, payload);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesNodeScanPredicateColumnsBeforeRowMaterialization) {
		DirectEdgeGraph graph = directGraph();
		graph.nodes.at(1U).properties.emplace_back("score", PropertyValue(int64_t{10}));
		graph.nodes.at(2U).properties.emplace_back("score", PropertyValue(int64_t{20}));
		graph.nodes.at(3U).properties.emplace_back("score", PropertyValue(int64_t{30}));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNodeScan scan("n", std::vector<std::string>{"Person"},
									  std::vector<std::pair<std::string, PropertyValue>>{
											  {"rank", PropertyValue(1)}, {"score", PropertyValue(int64_t{10})}});

		std::vector<uint64_t> nodes;
		const LogicalPlanReadRuntimeStats stats =
				runtime.execute(scan, directBounds(), [&nodes](const LogicalPlanReadRow &row) {
					nodes.push_back(std::get<LogicalGraphNode>(row.values.at("n")).node_id);
					return true;
				});

		EXPECT_EQ(nodes, (std::vector<uint64_t>{1U}));
		EXPECT_EQ(stats.nodes_scanned, 3U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_property_sorted_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank", "score"}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesPropertyProjectionDirectlyOverNodeScan) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> ranks;
		static_cast<void>(runtime.execute(project, directBounds(), [&ranks](const LogicalPlanReadRow &row) {
			ranks.push_back(std::get<PropertyValue>(row.values.at("rank")));
			return true;
		}));

		EXPECT_EQ(ranks, (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)}));
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_property_sorted_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, FlushesSelectedPropertyRowsBeforeTheByteBudgetIsExceeded) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		for (uint64_t nodeId = 4U; nodeId <= 30U; ++nodeId) {
			graph.nodes.emplace(nodeId, LogicalGraphNode{nodeId, {"Person"}, {{"rank", PropertyValue(nodeId)}}});
		}
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"});
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank");
		logical::LogicalProject project(std::move(scan), std::move(items));
		LogicalPlanReadRuntimeBounds bounds = directBounds();
		bounds.maximum_node_records = 64U;
		bounds.maximum_rows = 64U;
		bounds.maximum_materialized_bytes = 1024U;
		std::vector<PropertyValue> ranks;

		static_cast<void>(runtime.execute(project, bounds, [&ranks](const LogicalPlanReadRow &row) {
			ranks.push_back(std::get<PropertyValue>(row.values.at("rank")));
			return true;
		}));

		ASSERT_EQ(ranks.size(), 29U);
		std::vector<PropertyValue> expectedRanks{PropertyValue(1), PropertyValue(2)};
		for (int64_t rank = 4; rank <= 30; ++rank) {
			expectedRanks.emplace_back(rank);
		}
		EXPECT_EQ(ranks, expectedRanks);
		EXPECT_GT(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, SplitsVariablePropertyPayloadBatchesAndPreservesEarlyStop) {
		for (const bool stopAfterFirst: {false, true}) {
			SCOPED_TRACE(stopAfterFirst);
			DirectEdgeGraph graph = directGraph();
			graph.enable_property_tokens = true;
			std::vector<PropertyValue> expected;
			for (uint64_t nodeId = 1U; nodeId <= 3U; ++nodeId) {
				PropertyValue payload(std::string(400U, static_cast<char>('a' + nodeId - 1U)));
				graph.nodes.at(nodeId).properties.emplace_back("payload", payload);
				expected.push_back(std::move(payload));
			}
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			std::vector<logical::LogicalProjectItem> items;
			items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "payload"), "payload");
			logical::LogicalProject project(std::make_unique<logical::LogicalNodeScan>("n"), std::move(items));
			LogicalPlanReadRuntimeBounds bounds = directBounds();
			// Keep three pending reference rows, one payload and output/column
			// staging live; the batch must still split its three payloads.
			bounds.maximum_materialized_bytes =
				3U * (sizeof(LogicalPlanReadRow) + sizeof(LogicalPlanReadValue) + sizeof(EntityRef)) +
				400U + sizeof(LogicalPlanReadRow) + 2U * sizeof(PropertyValue) +
				SelectionVector::requiredStorageBytes(3U) + 3U * sizeof(uint64_t) + 128U;
			std::vector<PropertyValue> delivered;
			size_t readsAtVisitor = 0U;

			static_cast<void>(runtime.execute(project, bounds, [&](const LogicalPlanReadRow &row) {
				delivered.push_back(std::get<PropertyValue>(row.values.at("payload")));
				readsAtVisitor = graph.node_property_column_batch_reads;
				return !stopAfterFirst;
			}));

			if (stopAfterFirst) {
				expected.resize(1U);
			}
			EXPECT_EQ(delivered, expected);
			if (stopAfterFirst) {
				// Budget retries may precede delivery; cancellation must prevent
				// every subsequent read after the first visitor invocation.
				EXPECT_GT(readsAtVisitor, 0U);
				EXPECT_EQ(graph.node_property_column_batch_reads, readsAtVisitor);
			} else {
				EXPECT_GT(graph.node_property_column_batch_reads, 1U);
			}
			EXPECT_EQ(graph.node_property_point_reads, 0U);
			EXPECT_TRUE(graph.node_read_ids.empty());
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, PrefetchRetriesRetainPendingRowsAndPreservePartialCachesAndOrder) {
		for (const size_t payloadBytes: {size_t{500U}, size_t{1500U}}) {
			for (const bool stopEarly: {false, true}) {
				SCOPED_TRACE(payloadBytes);
				SCOPED_TRACE(stopEarly);
				DirectEdgeGraph graph = directGraph();
				graph.enable_property_tokens = true;
				graph.nodes.clear();
				std::vector<PropertyValue> expected;
				for (uint64_t id = 1U; id <= 6U; ++id) {
					PropertyValue payload(std::string(payloadBytes, static_cast<char>('a' + id - 1U)));
					graph.nodes.emplace(id, LogicalGraphNode{id, {"Person"}, {{"payload", payload}}});
					expected.push_back(payload);
				}
				DirectEdgeBackend backend(graph);
				LogicalPlanReadRuntime runtime(backend);
				std::vector<logical::LogicalProjectItem> items;
				items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "payload"),
								   "payload");
				logical::LogicalProject root(std::make_unique<logical::LogicalNodeScan>("n"), std::move(items));
				auto bounds = directBounds();
				// Six pending reference rows coexist with a single 1500-byte
				// payload, its column staging and the public output slot. Keep
				// room for bounded cache bookkeeping without fitting all payloads.
				bounds.maximum_materialized_bytes =
					6U * (sizeof(LogicalPlanReadRow) + sizeof(LogicalPlanReadValue) + sizeof(EntityRef)) +
					1500U + sizeof(LogicalPlanReadRow) + 2U * sizeof(PropertyValue) +
					SelectionVector::requiredStorageBytes(6U) + 6U * sizeof(uint64_t) + 512U;
				std::vector<PropertyValue> delivered;
				EXPECT_NO_THROW(static_cast<void>(runtime.execute(root, bounds, [&](const LogicalPlanReadRow &row) {
					delivered.push_back(std::get<PropertyValue>(row.values.at("payload")));
					return !stopEarly;
				})));
				if (stopEarly)
					expected.resize(1U);
				EXPECT_EQ(delivered, expected);
				if (!stopEarly || payloadBytes == 1500U)
					EXPECT_GT(graph.node_property_column_batch_reads, 1U);
				// This backend has no source admission: the first complete column
				// exceeds the allowance shared with input rows, so post-read
				// reservation retries a smaller batch before the first delivery.
				if (stopEarly && payloadBytes == 500U) {
					// Row objects differ by standard library, so more than one
					// halving can be needed. Every retry must preserve the pending
					// prefix and shrink before the first successful delivery.
					EXPECT_GE(graph.node_property_column_batch_reads, 2U);
					ASSERT_EQ(graph.node_property_column_batch_ids.size(), graph.node_property_column_batch_reads);
					for (size_t index = 1U; index < graph.node_property_column_batch_ids.size(); ++index) {
						const auto &previous = graph.node_property_column_batch_ids[index - 1U];
						const auto &retry = graph.node_property_column_batch_ids[index];
						ASSERT_LT(retry.size(), previous.size());
						EXPECT_TRUE(std::equal(retry.begin(), retry.end(), previous.begin()));
					}
				}
				EXPECT_EQ(graph.node_property_point_reads, 0U);
				EXPECT_TRUE(graph.node_read_ids.empty());
			}
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, PendingPrefetchRowsRemainChargedWhileRetryEnrichesOneRow) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		graph.nodes.clear();
		const PropertyValue payload(std::string(3000U, 'p'));
		graph.nodes.emplace(1U, LogicalGraphNode{1U, {"Person"}, {{"payload", payload}}});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "payload"), "payload");
		logical::LogicalProject root(std::make_unique<logical::LogicalNodeScan>("n"), std::move(items));
		auto bounds = directBounds();
		bounds.maximum_materialized_bytes = 4096U;
		size_t delivered = 0U;
		const auto visit = [&](const LogicalPlanReadRow &row) {
			++delivered;
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("payload")), payload);
			return true;
		};
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(root, bounds, visit)));
		EXPECT_EQ(delivered, 1U);
		for (uint64_t id = 2U; id <= 6U; ++id) {
			graph.nodes.emplace(id, LogicalGraphNode{id, {"Person"}, {{"payload", payload}}});
		}
		delivered = 0U;
		const size_t readsBefore = graph.node_property_column_batch_reads;
		EXPECT_THROW(static_cast<void>(runtime.execute(root, bounds, visit)), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		EXPECT_GT(graph.node_property_column_batch_reads, readsBefore + 1U);
		bounds.maximum_materialized_bytes = 16000U;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(root, bounds, visit)));
		EXPECT_EQ(delivered, 6U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_TRUE(graph.node_read_ids.empty());
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, PropertyPrefetchTransfersItsChargeBeforeSortAndVisitorCancellation) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		graph.nodes.erase(2U);
		graph.nodes.erase(3U);
		const PropertyValue payload(std::string(400U, 'p'));
		graph.nodes.at(1U).properties.emplace_back("payload", payload);
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "payload"), "payload");
		auto project = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalNodeScan>("n"),
																 std::move(items));
		std::vector<logical::LogicalSortItem> sortItems;
		sortItems.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{0}), true);
		logical::LogicalSort sort(std::move(project), std::move(sortItems));
		auto bounds = directBounds();
		// One retained payload plus source/output row slots and column staging.
		bounds.maximum_materialized_bytes = 2U * sizeof(LogicalPlanReadRow) +
				sizeof(LogicalPlanReadValue) + sizeof(PropertyValue) + 400U + 216U;
		EXPECT_LT(bounds.maximum_materialized_bytes,
				2U * (sizeof(LogicalPlanReadRow) + sizeof(LogicalPlanReadValue) + 400U + 7U));
		for (const bool continueVisiting: {false, true}) {
			SCOPED_TRACE(continueVisiting);
			size_t delivered = 0U;
			EXPECT_NO_THROW(static_cast<void>(runtime.execute(sort, bounds, [&](const LogicalPlanReadRow &row) {
				++delivered;
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("payload")), payload);
				return continueVisiting;
			})));
			EXPECT_EQ(delivered, 1U);
		}
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_TRUE(graph.node_read_ids.empty());
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ReservesCrossRowPropertyColumnFramesBeforeReading) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		std::vector<logical::LogicalProjectItem> items;
		for (size_t index = 0U; index < 8U; ++index) {
			items.emplace_back(
					std::make_shared<expressions::VariableReferenceExpression>("n", "missing" + std::to_string(index)),
					"value" + std::to_string(index));
		}
		logical::LogicalProject project(std::make_unique<logical::LogicalNodeScan>("n"), std::move(items));
		LogicalPlanReadRuntimeBounds limits = directBounds();
		limits.maximum_node_records = 1U;
		limits.maximum_node_page_records = 1U;
		limits.maximum_rows = 1U;
		limits.maximum_materialized_bytes = 512U;

		EXPECT_THROW(
				static_cast<void>(runtime.execute(project, limits, [](const LogicalPlanReadRow &) { return true; })),
				std::runtime_error);
		EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RejectsMalformedPropertyColumnsBeforeBudgetRetries) {
		for (const bool edgeColumn: {false, true}) {
			SCOPED_TRACE(edgeColumn);
			DirectEdgeGraph graph = directGraph();
			graph.enable_property_tokens = true;
			graph.truncate_node_property_batch = !edgeColumn;
			graph.truncate_edge_property_batch = edgeColumn;
			for (auto &[id, node]: graph.nodes) {
				static_cast<void>(id);
				node.properties.emplace_back("payload", PropertyValue(std::string(5000U, 'n')));
			}
			for (auto &edge: graph.edges) {
				edge.properties.emplace_back("payload", PropertyValue(std::string(5000U, 'r')));
			}
			std::unique_ptr<logical::LogicalOperator> child = std::make_unique<logical::LogicalNodeScan>("n");
			if (edgeColumn) {
				child = std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a"),
																	"a", "r", "b", "KNOWS", "out");
			}
			std::vector<logical::LogicalProjectItem> items;
			items.emplace_back(
					std::make_shared<expressions::VariableReferenceExpression>(edgeColumn ? "r" : "n", "payload"),
					"payload");
			logical::LogicalProject project(std::move(child), std::move(items));
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			LogicalPlanReadRuntimeBounds bounds = directBounds();
			bounds.maximum_materialized_bytes = 4096U;
			size_t visitedRows = 0U;
			try {
				static_cast<void>(runtime.execute(project, bounds, [&](const LogicalPlanReadRow &) {
					++visitedRows;
					return true;
				}));
				FAIL() << "malformed columns must fail before retrying an oversized batch";
			} catch (const std::runtime_error &error) {
				EXPECT_STREQ(error.what(), "logical graph backend changed Property batch cardinality");
			}
			EXPECT_EQ(visitedRows, 0U);
			EXPECT_EQ(edgeColumn ? graph.edge_property_column_batch_reads : graph.node_property_column_batch_reads, 1U);
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, DeduplicatesComplexProjectionPropertyRequestsInOneBatch) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");

		auto rankCase = std::make_shared<expressions::CaseExpression>();
		rankCase->addBranch(std::make_unique<expressions::BinaryOpExpression>(
									std::make_unique<expressions::VariableReferenceExpression>("n", "rank"),
									expressions::BinaryOperatorType::BOP_GREATER,
									std::make_unique<expressions::LiteralExpression>(int64_t{1})),
							std::make_unique<expressions::VariableReferenceExpression>("n", "rank"));
		rankCase->setElseExpression(std::make_unique<expressions::VariableReferenceExpression>("n", "rank"));

		std::vector<std::unique_ptr<expressions::Expression>> functionArguments;
		functionArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("n", "rank"));
		auto rankText = std::make_shared<expressions::FunctionCallExpression>("toString", std::move(functionArguments));
		auto selected = std::make_shared<expressions::InExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("n", "rank"),
				std::vector<PropertyValue>{PropertyValue(int64_t{1}), PropertyValue(int64_t{3})});

		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::move(rankCase), "rank_case");
		items.emplace_back(std::move(rankText), "rank_text");
		items.emplace_back(std::move(selected), "selected");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<LogicalPlanReadRow> rows;
		static_cast<void>(runtime.execute(project, directBounds(), [&rows](const LogicalPlanReadRow &row) {
			rows.push_back(row);
			return true;
		}));

		ASSERT_EQ(rows.size(), 3U);
		for (size_t index = 0U; index < rows.size(); ++index) {
			const int64_t rank = static_cast<int64_t>(index + 1U);
			EXPECT_EQ(std::get<PropertyValue>(rows[index].values.at("rank_case")), PropertyValue(rank));
			EXPECT_EQ(std::get<PropertyValue>(rows[index].values.at("rank_text")), PropertyValue(std::to_string(rank)));
			EXPECT_EQ(std::get<PropertyValue>(rows[index].values.at("selected")), PropertyValue(rank != 2));
		}
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesPropertySubsetOfMixedProjection) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank");
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "entity");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> ranks;
		std::vector<uint64_t> entityIds;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			ranks.push_back(std::get<PropertyValue>(row.values.at("rank")));
			entityIds.push_back(std::get<LogicalGraphNode>(row.values.at("entity")).node_id);
			return true;
		}));

		EXPECT_EQ(ranks, (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)}));
		EXPECT_EQ(entityIds, (std::vector<uint64_t>{1U, 2U, 3U}));
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, (std::vector<uint64_t>{1U, 2U, 3U}));
		EXPECT_EQ(graph.node_shaped_reads, 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesNodeListPropertiesBeforeUnwindExpansion) {
		DirectEdgeGraph graph = directGraph();
		graph.nodes.at(1U).properties.emplace_back(
				"tags", PropertyValue(std::vector<PropertyValue>{PropertyValue("a"), PropertyValue("b")}));
		graph.nodes.at(2U).properties.emplace_back("tags",
												   PropertyValue(std::vector<PropertyValue>{PropertyValue("c")}));
		graph.nodes.at(3U).properties.emplace_back("tags", PropertyValue(std::vector<PropertyValue>{}));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		const auto tags = std::make_shared<expressions::VariableReferenceExpression>("n", "tags");
		auto unwind =
				std::make_unique<logical::LogicalUnwind>(std::make_unique<logical::LogicalNodeScan>("n"), "tag", tags);
		logical::LogicalProject project(
				std::move(unwind), std::vector<logical::LogicalProjectItem>{
										   {std::make_shared<expressions::VariableReferenceExpression>("tag"), "tag"}});

		const QueryResult result = runtime.executeToQueryResult(project, directBounds());
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("tag").asPrimitive(), PropertyValue("a"));
		EXPECT_EQ(result.getRows()[1].at("tag").asPrimitive(), PropertyValue("b"));
		EXPECT_EQ(result.getRows()[2].at("tag").asPrimitive(), PropertyValue("c"));
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"tags"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_EQ(graph.node_read_ids, std::vector<uint64_t>{});
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, PublicVisitorKeepsUnwindPropertyReadAheadDisabled) {
		DirectEdgeGraph graph = directGraph();
		graph.nodes.at(1U).properties.emplace_back(
				"tags", PropertyValue(std::vector<PropertyValue>{PropertyValue("a"), PropertyValue("b")}));
		graph.nodes.at(2U).properties.emplace_back("tags",
												   PropertyValue(std::vector<PropertyValue>{PropertyValue("c")}));
		graph.nodes.at(3U).properties.emplace_back("tags", PropertyValue(std::vector<PropertyValue>{}));
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		const auto tags = std::make_shared<expressions::VariableReferenceExpression>("n", "tags");
		auto unwind =
				std::make_unique<logical::LogicalUnwind>(std::make_unique<logical::LogicalNodeScan>("n"), "tag", tags);
		logical::LogicalProject project(
				std::move(unwind), std::vector<logical::LogicalProjectItem>{
										   {std::make_shared<expressions::VariableReferenceExpression>("tag"), "tag"}});
		std::vector<PropertyValue> observed;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			observed.push_back(std::get<PropertyValue>(row.values.at("tag")));
			return false;
		}));

		ASSERT_EQ(observed, (std::vector<PropertyValue>{PropertyValue("a")}));
		EXPECT_EQ(graph.node_property_column_batch_reads, 0U);
		EXPECT_EQ(graph.node_property_point_reads, 1U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ReusesSelectedPropertiesAcrossEntityAliasProjections) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);

		auto traversal = std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a"),
																	 "a", "r", "b", "KNOWS", "out");
		std::vector<logical::LogicalProjectItem> firstItems;
		firstItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r"), "edge");
		firstItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("r", "weight"),
								"first_weight");
		auto firstProject = std::make_unique<logical::LogicalProject>(std::move(traversal), std::move(firstItems));

		std::vector<logical::LogicalProjectItem> secondItems;
		secondItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("edge"), "renamed_edge");
		auto secondProject = std::make_unique<logical::LogicalProject>(std::move(firstProject), std::move(secondItems));
		std::vector<logical::LogicalProjectItem> thirdItems;
		thirdItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("renamed_edge", "weight"),
								"weight");
		logical::LogicalProject project(std::move(secondProject), std::move(thirdItems));

		std::vector<PropertyValue> weights;
		static_cast<void>(runtime.execute(project, directBounds(), [&weights](const LogicalPlanReadRow &row) {
			weights.push_back(std::get<PropertyValue>(row.values.at("weight")));
			return true;
		}));

		EXPECT_EQ(weights, (std::vector<PropertyValue>{PropertyValue(7), PropertyValue(7), PropertyValue(8)}));
		// The single-item identity Project transfers the selected cache with the
		// renamed Edge binding, so the final Project needs no second column read.
		EXPECT_EQ(graph.edge_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.edge_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"weight"}}));
		EXPECT_EQ(graph.edge_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, RenamedIdentityProjectionPreservesEntityMetadataCache) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		const auto labelsExpression = [](const std::string &variable) {
			std::vector<std::unique_ptr<expressions::Expression>> arguments;
			arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>(variable));
			return std::make_shared<expressions::FunctionCallExpression>("labels", std::move(arguments));
		};

		std::vector<logical::LogicalProjectItem> seedItems;
		seedItems.emplace_back(labelsExpression("n"), "initial_labels");
		seedItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "n");
		auto seed = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalNodeScan>("n"),
															  std::move(seedItems));
		std::vector<logical::LogicalProjectItem> renameItems;
		renameItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "renamed");
		auto rename = std::make_unique<logical::LogicalProject>(std::move(seed), std::move(renameItems));
		std::vector<logical::LogicalProjectItem> outputItems;
		outputItems.emplace_back(labelsExpression("renamed"), "labels_after_rename");
		logical::LogicalProject project(std::move(rename), std::move(outputItems));

		const QueryResult result = runtime.executeToQueryResult(project, directBounds());
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("labels_after_rename").asPrimitive(),
				  PropertyValue(std::vector<PropertyValue>{PropertyValue("Person")}));
		EXPECT_EQ(result.getRows()[1].at("labels_after_rename").asPrimitive(),
				  PropertyValue(std::vector<PropertyValue>{PropertyValue("Person")}));
		EXPECT_EQ(result.getRows()[2].at("labels_after_rename").asPrimitive(),
				  PropertyValue(std::vector<PropertyValue>{PropertyValue("Other")}));
		EXPECT_EQ(graph.node_shaped_batch_reads, 0U);
		// The seed expression reads labels once per Node; the renamed Entity
		// must reuse those cached values instead of issuing another three reads.
		EXPECT_EQ(graph.node_shaped_reads, 3U);
		EXPECT_EQ(graph.node_read_ids.size(), 3U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesMapProjectionPropertySelectorsWithNestedPropertyExpressions) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		std::vector<expressions::MapProjectionItem> mapItems;
		mapItems.emplace_back(expressions::MapProjectionItemType::MPROP_PROPERTY, "rank");
		mapItems.emplace_back(expressions::MapProjectionItemType::MPROP_LITERAL, "copy",
							  std::make_unique<expressions::VariableReferenceExpression>("n", "rank"));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::MapProjectionExpression>("n", std::move(mapItems)),
						   "projection");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> projections;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			projections.push_back(std::get<PropertyValue>(row.values.at("projection")));
			return true;
		}));

		ASSERT_EQ(projections.size(), 3U);
		for (size_t index = 0U; index < projections.size(); ++index) {
			const PropertyValue expectedRank(static_cast<int64_t>(index + 1U));
			EXPECT_EQ(projections[index],
					  PropertyValue(PropertyValue::MapType{{"copy", expectedRank}, {"rank", expectedRank}}));
		}
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, BatchesDynamicListLiteralPropertyExpressions) {
		DirectEdgeGraph graph = directGraph();
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto scan = std::make_unique<logical::LogicalNodeScan>("n");
		std::vector<std::unique_ptr<expressions::Expression>> elements;
		elements.push_back(std::make_unique<expressions::VariableReferenceExpression>("n", "rank"));
		elements.push_back(std::make_unique<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("n", "rank"),
				expressions::BinaryOperatorType::BOP_ADD,
				std::make_unique<expressions::LiteralExpression>(int64_t{1})));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::ListLiteralExpression>(std::move(elements)), "values");
		logical::LogicalProject project(std::move(scan), std::move(items));

		std::vector<PropertyValue> values;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			values.push_back(std::get<PropertyValue>(row.values.at("values")));
			return true;
		}));

		ASSERT_EQ(values.size(), 3U);
		for (size_t index = 0U; index < values.size(); ++index) {
			const int64_t rank = static_cast<int64_t>(index + 1U);
			EXPECT_EQ(values[index],
					  PropertyValue(std::vector<PropertyValue>{PropertyValue(rank), PropertyValue(rank + 1)}));
		}
		EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
		EXPECT_EQ(graph.node_property_column_batch_keys, (std::vector<std::vector<std::string>>{{"rank"}}));
		EXPECT_EQ(graph.node_property_point_reads, 0U);
	}


 TEST(LogicalPlanDirectEdgeRuntimeTest, ReportsPrefetchMovesWithoutChangingProjectionValues) {
  DirectEdgeGraph graph=directGraph();DirectEdgeBackend backend(graph);LogicalPlanReadRuntime runtime(backend);
  std::vector<logical::LogicalProjectItem> items;
  items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n","rank"),"rank");
  logical::LogicalProject project(std::make_unique<logical::LogicalNodeScan>("n"),std::move(items));
  const bool previous=debug::PerfTrace::areValuesEnabled();debug::PerfTrace::reset();debug::PerfTrace::setValueCollectionEnabled(true);
  std::vector<PropertyValue> values;
  try {static_cast<void>(runtime.execute(project,directBounds(),[&](const LogicalPlanReadRow &row){values.push_back(std::get<PropertyValue>(row.values.at("rank")));return true;}));}
  catch (...) {debug::PerfTrace::setValueCollectionEnabled(previous);throw;}
  const auto trace=debug::PerfTrace::snapshotAndReset();debug::PerfTrace::setValueCollectionEnabled(previous);
  EXPECT_EQ(values,(std::vector<PropertyValue>{PropertyValue(1),PropertyValue(2),PropertyValue(3)}));
  ASSERT_TRUE(trace.contains("logical_plan.prefetch.moved_values"));
  EXPECT_EQ(trace.at("logical_plan.prefetch.moved_values").totalValue,3);
  EXPECT_FALSE(trace.contains("logical_plan.prefetch.copied_values"));
 }

 TEST(LogicalPlanDirectEdgeRuntimeTest, BorrowsRepeatedOwnerColumnsWhilePreservingBagRows) {
  DirectEdgeGraph graph=directGraph();
  graph.edges.emplace_back(13,1,2,"KNOWS",std::vector<storage::LogicalGraphPropertyEntry>{});
  graph.edges.emplace_back(14,1,2,"KNOWS",std::vector<storage::LogicalGraphPropertyEntry>{});
  DirectEdgeBackend backend(graph);LogicalPlanReadRuntime runtime(backend);
  auto traversal=std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a"),"a","r","b","KNOWS","out");
  std::vector<logical::LogicalProjectItem> items;
  items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b","rank"),"rank");
  logical::LogicalProject project(std::move(traversal),std::move(items));
  const bool previous=debug::PerfTrace::areValuesEnabled();debug::PerfTrace::reset();debug::PerfTrace::setValueCollectionEnabled(true);
  std::vector<PropertyValue> values;
  try {static_cast<void>(runtime.execute(project,directBounds(),[&](const LogicalPlanReadRow &row){values.push_back(std::get<PropertyValue>(row.values.at("rank")));return true;}));}
  catch (...) {debug::PerfTrace::setValueCollectionEnabled(previous);throw;}
  const auto trace=debug::PerfTrace::snapshotAndReset();debug::PerfTrace::setValueCollectionEnabled(previous);
  ASSERT_EQ(values.size(),5U);
  EXPECT_EQ(std::count(values.begin(),values.end(),PropertyValue(2)),3);
  EXPECT_EQ(std::count(values.begin(),values.end(),PropertyValue(1)),1);
  EXPECT_EQ(std::count(values.begin(),values.end(),PropertyValue(3)),1);
  EXPECT_FALSE(trace.contains("logical_plan.prefetch.copied_values"));
  EXPECT_FALSE(trace.contains("logical_plan.prefetch.moved_values"));
  ASSERT_TRUE(trace.contains("logical_plan.prefetch.borrowed_values"));
  EXPECT_EQ(trace.at("logical_plan.prefetch.borrowed_values").totalValue,5);
 }

 TEST(LogicalPlanDirectEdgeRuntimeTest, BorrowedWideColumnsOwnEscapedResultsAndReleaseOnEarlyStop) {
  for(bool earlyStop:{false,true}) {
   DirectEdgeGraph graph=directGraph();const std::string payload(4096,'x');
   for(auto &[id,node]:graph.nodes)node.properties.emplace_back("payload",PropertyValue(payload));
   graph.edges.emplace_back(13,1,2,"KNOWS",std::vector<storage::LogicalGraphPropertyEntry>{});
   graph.edges.emplace_back(14,1,2,"KNOWS",std::vector<storage::LogicalGraphPropertyEntry>{});
   DirectEdgeBackend backend(graph);LogicalPlanReadRuntime runtime(backend);
   auto traversal=std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a"),"a","r","b","KNOWS","out");
   std::vector<logical::LogicalProjectItem> items;items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b","payload"),"value");
   logical::LogicalProject project(std::move(traversal),std::move(items));
   auto bounds=directBounds();bounds.maximum_materialized_bytes=32768;
   std::vector<PropertyValue> escaped;
   static_cast<void>(runtime.execute(project,bounds,[&](const LogicalPlanReadRow &row){escaped.push_back(std::get<PropertyValue>(row.values.at("value")));return !earlyStop;}));
   EXPECT_EQ(escaped.size(),earlyStop?1U:5U);
   graph.nodes.clear();
   for(const auto &value:escaped)EXPECT_EQ(value,PropertyValue(payload));
  }
 }

 TEST(LogicalPlanDirectEdgeRuntimeTest, BorrowsMultipleNodeColumnsPreservingOrderMissingAndNull) {
	 DirectEdgeGraph graph = directGraph();
	 graph.nodes.at(2).properties.emplace_back("optional", PropertyValue());
	 graph.nodes.at(3).properties.emplace_back("optional", PropertyValue(true));
	 graph.edges.emplace_back(13, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
	 graph.edges.emplace_back(14, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
	 DirectEdgeBackend backend(graph);
	 LogicalPlanReadRuntime runtime(backend);
	 auto traversal = std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a"), "a",
																  "r", "b", "KNOWS", "out");
	 std::vector<logical::LogicalProjectItem> items;
	 items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b", "rank"), "rank");
	 items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b", "optional"), "optional");
	 logical::LogicalProject project(std::move(traversal), std::move(items));
	 const bool previous = debug::PerfTrace::areValuesEnabled();
	 debug::PerfTrace::reset();
	 debug::PerfTrace::setValueCollectionEnabled(true);
	 std::vector<PropertyValue> ranks, optional;
	 try {
		 static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			 ranks.push_back(std::get<PropertyValue>(row.values.at("rank")));
			 optional.push_back(std::get<PropertyValue>(row.values.at("optional")));
			 return true;
		 }));
	 } catch (...) {
		 debug::PerfTrace::setValueCollectionEnabled(previous);
		 throw;
	 }
	 const auto trace = debug::PerfTrace::snapshotAndReset();
	 debug::PerfTrace::setValueCollectionEnabled(previous);
	 EXPECT_EQ(ranks, (std::vector<PropertyValue>{PropertyValue(2), PropertyValue(3), PropertyValue(1),
												  PropertyValue(2), PropertyValue(2)}));
	 EXPECT_EQ(optional, (std::vector<PropertyValue>{PropertyValue(), PropertyValue(true), PropertyValue(),
													 PropertyValue(), PropertyValue()}));
	 // A literal disables column borrowing. Compare with the existing scatter path,
	 // whose all-edge scan emits records in graph.edges order.
	 auto baselineTraversal = std::make_unique<logical::LogicalTraversal>(
			 std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
	 std::vector<logical::LogicalProjectItem> baselineItems;
	 baselineItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b", "rank"), "rank");
	 baselineItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b", "optional"),
								"optional");
	 baselineItems.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{1}), "marker");
	 logical::LogicalProject baselineProject(std::move(baselineTraversal), std::move(baselineItems));
	 std::vector<PropertyValue> baselineRanks, baselineOptional;
	 static_cast<void>(runtime.execute(baselineProject, directBounds(), [&](const LogicalPlanReadRow &row) {
		 baselineRanks.push_back(std::get<PropertyValue>(row.values.at("rank")));
		 baselineOptional.push_back(std::get<PropertyValue>(row.values.at("optional")));
		 return true;
	 }));
	 EXPECT_EQ(ranks, baselineRanks);
	 EXPECT_EQ(optional, baselineOptional);
	 ASSERT_TRUE(trace.contains("logical_plan.prefetch.borrowed_values"));
	 EXPECT_EQ(trace.at("logical_plan.prefetch.borrowed_values").totalValue, 10);
	 EXPECT_FALSE(trace.contains("logical_plan.prefetch.copied_values"));
 }

 TEST(LogicalPlanDirectEdgeRuntimeTest, BorrowsAggregateNodeColumnsPreservingGroupsAndBagMultiplicity) {
	 for (const bool grouped: {false, true}) {
		 DirectEdgeGraph graph = directGraph();
		 graph.edges.emplace_back(13, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
		 graph.edges.emplace_back(14, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
		 DirectEdgeBackend backend(graph);
		 LogicalPlanReadRuntime runtime(backend);
		 auto traversal = std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a"),
																	  "a", "r", "b", "KNOWS", "out");
		 auto rank = std::make_shared<expressions::VariableReferenceExpression>("b", "rank");
		 std::vector<std::shared_ptr<expressions::Expression>> groups;
		 if (grouped)
			 groups.push_back(rank);
		 logical::LogicalAggregate aggregate(std::move(traversal), std::move(groups),
											 {{"sum", rank, "total", false}, {"count", nullptr, "rows", false}},
											 grouped ? std::vector<std::string>{"rank"} : std::vector<std::string>{});
		 const bool previous = debug::PerfTrace::areValuesEnabled();
		 debug::PerfTrace::reset();
		 debug::PerfTrace::setValueCollectionEnabled(true);
		 std::map<int64_t, std::pair<int64_t, int64_t>> results;
		 try {
			 static_cast<void>(runtime.execute(aggregate, directBounds(), [&](const LogicalPlanReadRow &row) {
				 const auto integer = [&](const char *key) {
					 return std::get<int64_t>(std::get<PropertyValue>(row.values.at(key)).getVariant());
				 };
				 results.emplace(grouped ? integer("rank") : 0, std::make_pair(integer("total"), integer("rows")));
				 return true;
			 }));
		 } catch (...) {
			 debug::PerfTrace::setValueCollectionEnabled(previous);
			 throw;
		 }
		 const auto trace = debug::PerfTrace::snapshotAndReset();
		 debug::PerfTrace::setValueCollectionEnabled(previous);
		 if (grouped) {
			 EXPECT_EQ(results,
					   (std::map<int64_t, std::pair<int64_t, int64_t>>{{1, {1, 1}}, {2, {6, 3}}, {3, {3, 1}}}));
		 } else {
			 EXPECT_EQ(results, (std::map<int64_t, std::pair<int64_t, int64_t>>{{0, {10, 5}}}));
		 }
		 ASSERT_TRUE(trace.contains("logical_plan.prefetch.borrowed_values"));
		 EXPECT_EQ(trace.at("logical_plan.prefetch.borrowed_values").totalValue, grouped ? 10 : 5);
		 EXPECT_FALSE(trace.contains("logical_plan.prefetch.copied_values"));
	 }
 }

 TEST(LogicalPlanDirectEdgeRuntimeTest, BorrowedAggregateColumnsIgnoreMissingAndNullValues) {
	 DirectEdgeGraph graph = directGraph();
	 graph.nodes.at(2).properties.emplace_back("optional", PropertyValue());
	 graph.nodes.at(3).properties.emplace_back("optional", PropertyValue(7));
	 graph.edges.emplace_back(13, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
	 graph.edges.emplace_back(14, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
	 DirectEdgeBackend backend(graph);
	 LogicalPlanReadRuntime runtime(backend);
	 auto traversal = std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a"), "a",
																  "r", "b", "KNOWS", "out");
	 auto optional = std::make_shared<expressions::VariableReferenceExpression>("b", "optional");
	 logical::LogicalAggregate aggregate(std::move(traversal), {optional},
										 {{"sum", optional, "total", false}, {"count", nullptr, "rows", false}},
										 {"optional"});
	 std::map<bool, std::pair<PropertyValue, int64_t>> results;
	 static_cast<void>(runtime.execute(aggregate, directBounds(), [&](const LogicalPlanReadRow &row) {
		 const auto integer = [&](const char *key) {
			 return std::get<int64_t>(std::get<PropertyValue>(row.values.at(key)).getVariant());
		 };
		 const bool isNull = std::get<PropertyValue>(row.values.at("optional")) == PropertyValue();
		 results.emplace(isNull, std::make_pair(std::get<PropertyValue>(row.values.at("total")), integer("rows")));
		 return true;
	 }));
	 EXPECT_EQ(results, (std::map<bool, std::pair<PropertyValue, int64_t>>{{true, {PropertyValue(), 4}}, {false, {PropertyValue(7), 1}}}));
 }

	TEST(LogicalPlanDirectEdgeRuntimeTest, CollectedTwoHopTopologyPreservesStreamingOrderAndBag) {
		std::vector<PropertyValue> streamed;
		for (const bool collected : {false, true}) {
			DirectEdgeGraph graph = directGraph();
			graph.enable_adjacency_reference_pages = true;
			graph.edges.emplace_back(13, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
			graph.edges.emplace_back(14, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
			DirectEdgeBackend backend(graph);
			LogicalPlanReadRuntime runtime(backend);
			auto first = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
			auto second = std::make_unique<logical::LogicalTraversal>(std::move(first), "b", "s", "c", "KNOWS", "out");
			std::vector<logical::LogicalProjectItem> items;
			items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("c", "rank"), "rank");
			logical::LogicalProject project(std::move(second), std::move(items));
			auto bounds = directBounds();
			bounds.maximum_materialized_bytes = 8192;
			std::vector<PropertyValue> values;
			if (collected) {
				const auto result = runtime.executeToQueryResult(project, bounds);
				for (const auto &row : result.getRows()) values.push_back(row.at("rank").asPrimitive());
				EXPECT_EQ(values, streamed);
			} else {
				static_cast<void>(runtime.execute(project, bounds, [&](const LogicalPlanReadRow &row) {
					values.push_back(std::get<PropertyValue>(row.values.at("rank")));return true;
				}));
				streamed = values;
				ASSERT_EQ(values.size(), 7U);
				EXPECT_EQ(std::count(values.begin(), values.end(), PropertyValue(1)), 3);
				EXPECT_EQ(std::count(values.begin(), values.end(), PropertyValue(2)), 3);
				EXPECT_EQ(std::count(values.begin(), values.end(), PropertyValue(3)), 1);
			}
		}
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, CollectedTwoHopLimitDoesNotValidateALaterDanglingTarget) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		graph.edges.emplace_back(15, 2, 999, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto first = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		auto second = std::make_unique<logical::LogicalTraversal>(std::move(first), "b", "s", "c", "KNOWS", "out");
		logical::LogicalLimit limit(std::move(second), 1);
		const auto result = runtime.executeToQueryResult(limit, directBounds());
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows()[0].at("c").asNode().getId(), 1U);
	}

	TEST(LogicalPlanDirectEdgeRuntimeTest, ReleasesReferenceTargetScratchAcrossRejectedLabelBatchesAndFailure) {
		DirectEdgeGraph graph = directGraph();
		graph.enable_adjacency_reference_pages = true;
		for (uint64_t id = 20; id < 116; ++id) {
			graph.edges.emplace_back(id, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{{"tier", PropertyValue(1)}});
		}
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalTraversal traversal(
			std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Person"}),
			"a", "r", "b", "KNOWS", "out", {"MissingLabel"}, {}, {{"tier", PropertyValue(1)}});
		auto bounds = directBounds();
		bounds.maximum_edges_per_node = 120;
		bounds.maximum_edge_records = 128;
		bounds.maximum_adjacency_page_records = 8;
		bounds.maximum_rows = 128;
		bounds.maximum_materialized_bytes = 4096;
		graph.truncate_node_label_batch = true;
		EXPECT_THROW(static_cast<void>(runtime.execute(traversal, bounds, [](const auto &) { return true; })), std::runtime_error);
		graph.truncate_node_label_batch = false;
		const size_t previous = graph.node_label_sorted_reads;
		size_t emitted = 0;
		static_cast<void>(runtime.execute(traversal, bounds, [&](const auto &) { ++emitted;return true; }));
		EXPECT_EQ(emitted, 0U);
		EXPECT_GT(graph.node_label_sorted_reads - previous, 10U);
	}
} // namespace graph::query::execution

namespace graph::query::execution {
using namespace direct_edge_test;
TEST(LogicalPlanDirectEdgeRuntimeTest, BorrowedColumnOutputCopiesReportRetainedBytesAndOwnTheirValues) {
	DirectEdgeGraph graph = directGraph();
	graph.edges.emplace_back(13U, 1U, 2U, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
	const PropertyValue payload(std::string(4096U, 'x'));
	for (auto &[id, node] : graph.nodes) node.properties.emplace_back("payload", payload);
	DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalTraversal>(
		std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
	logical::LogicalProject project(std::move(traversal),
		{{std::make_shared<expressions::VariableReferenceExpression>("b", "payload"), "payload"}});
	const bool previous = debug::PerfTrace::areValuesEnabled();
	debug::PerfTrace::reset(); debug::PerfTrace::setValueCollectionEnabled(true);
	std::vector<PropertyValue> outputs;
	try {
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			outputs.push_back(std::get<PropertyValue>(row.values.at("payload"))); return true;
		}));
	} catch (...) { debug::PerfTrace::setValueCollectionEnabled(previous); throw; }
	const auto trace = debug::PerfTrace::snapshotAndReset();
	debug::PerfTrace::setValueCollectionEnabled(previous);
	ASSERT_EQ(outputs.size(), graph.edges.size());
	ASSERT_TRUE(trace.contains("logical_plan.prefetch.borrowed_value_copy_bytes"));
	EXPECT_GE(trace.at("logical_plan.prefetch.borrowed_value_copy_bytes").totalValue,
		static_cast<int64_t>(outputs.size() * 4096U));
	outputs[0] = PropertyValue("changed output");
	for (size_t i = 1U; i < outputs.size(); ++i) EXPECT_EQ(outputs[i], payload);
	for (const auto &[id, node] : graph.nodes) EXPECT_EQ(node.properties.back().value, payload);
}
} // namespace graph::query::execution

namespace graph::query::execution {
using namespace direct_edge_test;
TEST(LogicalPlanDirectEdgeRuntimeTest, GenericMapPropertyCopiesAreOwnedAndReportRetainedPayloads) {
	DirectEdgeGraph graph = directGraph(); DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
	const PropertyValue payload(std::string(4096U, 'm'));
	const PropertyValue map(PropertyValue::MapType{{"payload", payload}, {"scalar", PropertyValue(7)}});
	auto unwind = std::make_unique<logical::LogicalUnwind>(std::make_unique<logical::LogicalSingleRow>(), "m",
		std::make_shared<expressions::LiteralExpression>(PropertyValue(std::vector<PropertyValue>{map, map})));
	logical::LogicalProject project(std::move(unwind),
		{{std::make_shared<expressions::VariableReferenceExpression>("m", "payload"), "payload"},
		 {std::make_shared<expressions::VariableReferenceExpression>("m", "scalar"), "scalar"},
		 {std::make_shared<expressions::VariableReferenceExpression>("m", "missing"), "missing"}});
	const bool previous = debug::PerfTrace::areValuesEnabled();
	debug::PerfTrace::reset(); debug::PerfTrace::setValueCollectionEnabled(true);
	std::vector<PropertyValue> outputs;
	try {
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			outputs.push_back(std::get<PropertyValue>(row.values.at("payload")));
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("scalar")), PropertyValue(7));
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("missing")), PropertyValue());
			return true;
		}));
	} catch (...) { debug::PerfTrace::setValueCollectionEnabled(previous); throw; }
	const auto trace = debug::PerfTrace::snapshotAndReset(); debug::PerfTrace::setValueCollectionEnabled(previous);
	ASSERT_EQ(outputs.size(), 2U);
	ASSERT_TRUE(trace.contains("logical_plan.property.owned_result_copy_bytes"));
	EXPECT_GE(trace.at("logical_plan.property.owned_result_copy_bytes").totalValue, 8192);
	outputs[0] = PropertyValue("changed"); EXPECT_EQ(outputs[1], payload);
	EXPECT_EQ(map.getMap().at("payload"), payload);
}
} // namespace graph::query::execution
