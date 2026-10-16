/**
 * @brief Vector procedure batching and deferred materialization contracts.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "query/api/LogicalVectorProcedureBatchFixture.hpp"
namespace graph::query {
	TEST(QueryEngineLogicalBackendBoundaryTest, VectorProcedureRuntimeValidatesBufferedSearchRowsAndSortKeys) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		const LogicalReadExecutionOptions options;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		const auto searchCall = [] {
			return std::make_unique<logical::LogicalCallProcedure>(
					"db.index.vector.queryNodes",
					std::vector<PropertyValue>{PropertyValue("memory_embedding"), PropertyValue(int64_t{2}),
							PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})});
		};
		const auto execute = [&runtime, &options](const logical::LogicalOperator &root) {
			return runtime.execute(root, options.bounds, options.maximum_vector_mapping_entries,
					options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
					options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
					options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
					options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};

		storage::LogicalGraphNode firstNode;
		firstNode.node_id = 1U;
		firstNode.labels = {"Memory"};
		storage::LogicalGraphNode secondNode;
		secondNode.node_id = 2U;
		secondNode.labels = {"Memory"};
		backend->state->nodes = {{firstNode.node_id, firstNode}, {secondNode.node_id, secondNode}};
		const auto literalSortKey = std::make_shared<expressions::LiteralExpression>(int64_t{1});

		backend->state->search_result.hits = {{0U, 0.0F}};
		logical::LogicalSort zeroIdHitSort(searchCall(), {{literalSortKey, true}});
		EXPECT_THROW(static_cast<void>(execute(zeroIdHitSort)), std::runtime_error);
		backend->state->search_result.hits = {{firstNode.node_id, std::numeric_limits<float>::quiet_NaN()}};
		logical::LogicalSort nonFiniteHitSort(searchCall(), {{literalSortKey, true}});
		EXPECT_THROW(static_cast<void>(execute(nonFiniteHitSort)), std::runtime_error);
		backend->state->search_result.hits = {{firstNode.node_id, 0.0F}, {firstNode.node_id, 0.0F}};
		logical::LogicalSort duplicateHitSort(searchCall(), {{literalSortKey, true}});
		EXPECT_THROW(static_cast<void>(execute(duplicateHitSort)), std::runtime_error);
		backend->state->search_result.hits = {{firstNode.node_id, 1.0F}, {secondNode.node_id, 0.0F}};
		logical::LogicalSort unorderedScoreSort(searchCall(), {{literalSortKey, true}});
		EXPECT_THROW(static_cast<void>(execute(unorderedScoreSort)), std::runtime_error);
		backend->state->search_result.hits = {{firstNode.node_id, 0.0F}, {firstNode.node_id, 1.0F}};
		logical::LogicalSort repeatedIdSort(searchCall(), {{literalSortKey, true}});
		EXPECT_THROW(static_cast<void>(execute(repeatedIdSort)), std::runtime_error);

		backend->state->nodes.at(firstNode.node_id).node_id = secondNode.node_id;
		backend->state->search_result.hits = {{firstNode.node_id, 0.0F}};
		logical::LogicalSort mismatchedNodeSort(searchCall(), {{literalSortKey, true}});
		// The sort key and declared output do not require a Node payload, so the
		// lazy EntityRef path must not read or validate an unused complete Node.
		EXPECT_NO_THROW(static_cast<void>(execute(mismatchedNodeSort)));
		const auto nodeValue = std::make_shared<expressions::VariableReferenceExpression>("node");
		logical::LogicalProject mismatchedNodeOutput(
				std::make_unique<logical::LogicalSort>(searchCall(),
						std::vector<logical::LogicalSortItem>{{literalSortKey, true}}),
				std::vector<logical::LogicalProjectItem>{{nodeValue, "node"}});
		EXPECT_THROW(static_cast<void>(execute(mismatchedNodeOutput)), std::runtime_error);
		backend->state->nodes.at(firstNode.node_id) = firstNode;

		backend->state->search_result.hits = {{firstNode.node_id, 0.0F}};
		logical::LogicalSort nodeSortKey(searchCall(), {{nodeValue, true}});
		EXPECT_THROW(static_cast<void>(execute(nodeSortKey)), std::runtime_error);
		logical::LogicalFilter nodePredicate(searchCall(), nodeValue);
		EXPECT_THROW(static_cast<void>(execute(nodePredicate)), std::runtime_error);

		backend->state->node_batch_reads = 0U;
		backend->state->search_result.hits = {{firstNode.node_id, 0.0F}, {secondNode.node_id, 1.0F}};
		logical::LogicalSort equalSortKeys(searchCall(), {{literalSortKey, true}});
		EXPECT_NO_THROW(static_cast<void>(execute(equalSortKeys)));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		logical::LogicalProject equalSortNodeOutput(
				std::make_unique<logical::LogicalSort>(searchCall(),
						std::vector<logical::LogicalSortItem>{{literalSortKey, true}}),
				std::vector<logical::LogicalProjectItem>{{nodeValue, "node"}});
		EXPECT_NO_THROW(static_cast<void>(execute(equalSortNodeOutput)));
		EXPECT_GT(backend->state->node_batch_reads, 0U);
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		logical::LogicalSort ascendingScoreSort(searchCall(), {{score, true}});
		EXPECT_NO_THROW(static_cast<void>(execute(ascendingScoreSort)));
		logical::LogicalSort descendingScoreSort(searchCall(), {{score, false}});
		EXPECT_NO_THROW(static_cast<void>(execute(descendingScoreSort)));
	}




	TEST(QueryEngineLogicalBackendBoundaryTest, VectorProcedureRestoresSearchOrderAfterSortedNodeBatches) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		const LogicalReadExecutionOptions options;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		logical::LogicalCallProcedure search(
				"db.index.vector.queryNodes",
				{PropertyValue("memory_embedding"), PropertyValue(int64_t{3}),
						PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})});
		for (uint64_t id = 1U; id <= 3U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.labels = {"Memory"};
			backend->state->nodes.emplace(id, std::move(node));
		}
		// Search results are score ordered, while the storage batch contract requires
		// ascending logical IDs. The output must retain the search ranking.
		backend->state->search_result.hits = {{3U, 0.0F}, {1U, 1.0F}, {2U, 2.0F}};
		const QueryResult result = runtime.execute(
				search, options.bounds, options.maximum_vector_mapping_entries, options.maximum_raw_vector_reads,
				options.vector_candidate_search_budget, options.vector_candidate_refinement_budget,
				options.maximum_vector_training_records, options.maximum_vector_training_pages,
				options.maximum_gds_algorithm_edges, options.maximum_gds_algorithm_iterations,
				options.maximum_gds_algorithm_work_units, QueryContext{});
		ASSERT_EQ(result.rowCount(), 3U);
		const auto &rows = result.getRows();
		const auto first = rows[0].at("node");
		const auto second = rows[1].at("node");
		const auto third = rows[2].at("node");
		ASSERT_TRUE(first.isLogicalNode());
		ASSERT_TRUE(second.isLogicalNode());
		ASSERT_TRUE(third.isLogicalNode());
		EXPECT_EQ(first.asNode().getId(), 3U);
		EXPECT_EQ(second.asNode().getId(), 1U);
		EXPECT_EQ(third.asNode().getId(), 2U);
	}




	TEST(QueryEngineLogicalBackendBoundaryTest, VectorProcedureMaterializesLargeHitSetsInBoundedNodeBatches) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		LogicalReadExecutionOptions options;
		// The hit vector itself is retained source state, so the cap must admit it
		// before Node batches can be read. A sufficient shared allowance still
		// keeps each physical Node batch at or below the 256-row ceiling.
		options.bounds.maximum_materialized_bytes = 1024U;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		logical::LogicalCallProcedure search(
				"db.index.vector.queryNodes",
				{PropertyValue("memory_embedding"), PropertyValue(int64_t{300}),
						PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})});
		for (uint64_t id = 1U; id <= 300U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.labels = {"Memory"};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		EXPECT_THROW(static_cast<void>(runtime.execute(
			search, options.bounds, options.maximum_vector_mapping_entries, options.maximum_raw_vector_reads,
			options.vector_candidate_search_budget, options.vector_candidate_refinement_budget,
			options.maximum_vector_training_records, options.maximum_vector_training_pages,
			options.maximum_gds_algorithm_edges, options.maximum_gds_algorithm_iterations,
			options.maximum_gds_algorithm_work_units, QueryContext{})), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 1024U * 1024U;
		const QueryResult result = runtime.execute(
			search, options.bounds, options.maximum_vector_mapping_entries, options.maximum_raw_vector_reads,
				options.vector_candidate_search_budget, options.vector_candidate_refinement_budget,
				options.maximum_vector_training_records, options.maximum_vector_training_pages,
				options.maximum_gds_algorithm_edges, options.maximum_gds_algorithm_iterations,
				options.maximum_gds_algorithm_work_units, QueryContext{});
		EXPECT_EQ(result.rowCount(), 300U);
		EXPECT_GE(backend->state->node_batch_reads, 2U);
		ASSERT_FALSE(backend->state->node_batch_sizes.empty());
		EXPECT_LE(*std::ranges::max_element(backend->state->node_batch_sizes), 256U);
	}




	TEST(QueryEngineLogicalBackendBoundaryTest, VectorProcedureEnforcesMaterializedByteBoundForCompleteNodes) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		LogicalReadExecutionOptions options;
		options.bounds.maximum_materialized_bytes = 512U;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		logical::LogicalCallProcedure search(
				"db.index.vector.queryNodes",
				{PropertyValue("memory_embedding"), PropertyValue(int64_t{1}),
						PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})});
		storage::LogicalGraphNode node;
		node.node_id = 1U;
		node.labels = {"Memory"};
		node.properties = {{"body", PropertyValue(std::string(4096U, 'x'))}};
		backend->state->nodes = {{node.node_id, node}};
		backend->state->search_result.hits = {{node.node_id, 0.0F}};

		EXPECT_THROW(static_cast<void>(runtime.execute(
				search, options.bounds, options.maximum_vector_mapping_entries, options.maximum_raw_vector_reads,
				options.vector_candidate_search_budget, options.vector_candidate_refinement_budget,
				options.maximum_vector_training_records, options.maximum_vector_training_pages,
				options.maximum_gds_algorithm_edges, options.maximum_gds_algorithm_iterations,
				options.maximum_gds_algorithm_work_units, QueryContext{})),
				std::runtime_error);
	}




	TEST(QueryEngineLogicalBackendBoundaryTest, VectorProcedureAvoidsCompleteNodeReadsForScalarProjection) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		const LogicalReadExecutionOptions options;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		logical::LogicalCallProcedure search(
				"db.index.vector.queryNodes",
				{PropertyValue("memory_embedding"), PropertyValue(int64_t{3}),
						PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})});
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		logical::LogicalLimit limit(
				std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalCallProcedure>(search),
						std::vector<logical::LogicalProjectItem>{{score, "score"}}),
				1);
		backend->state->search_result.hits = {{3U, 0.0F}, {1U, 1.0F}, {2U, 2.0F}};

		const QueryResult result = runtime.execute(
				limit, options.bounds, options.maximum_vector_mapping_entries, options.maximum_raw_vector_reads,
				options.vector_candidate_search_budget, options.vector_candidate_refinement_budget,
				options.maximum_vector_training_records, options.maximum_vector_training_pages,
				options.maximum_gds_algorithm_edges, options.maximum_gds_algorithm_iterations,
				options.maximum_gds_algorithm_work_units, QueryContext{});

		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(std::get<double>(result.getRows().front().at("score").asPrimitive().getVariant()), 0.0);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}




} // namespace graph::query
