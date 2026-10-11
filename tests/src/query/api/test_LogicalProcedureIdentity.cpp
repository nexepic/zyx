/**
 * @brief Procedure endpoint validation without a batched existence probe.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#include <gtest/gtest.h>
#include "query/api/LogicalProcedureTestFixture.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalVarLengthTraversal.hpp"

namespace graph::query {
TEST(LogicalProcedureIdentityTest, UnresolvedTraversalPredicatesUseNamedColumnsAndPreserveNullableFallback) {
	test::RecordingVectorProcedureBackend backend;
	backend.state->enable_property_admission = true;
	for (uint64_t id = 1U; id <= 3U; ++id) {
		backend.state->nodes.emplace(id, storage::LogicalGraphNode{id, {},
			{{"unused", PropertyValue(std::string(16000U, 'p'))}}});
	}
	backend.state->nodes.at(1U).properties.push_back({"fresh", PropertyValue(5)});
	backend.state->nodes.at(3U).properties.push_back({"fresh", PropertyValue(5)});
	backend.state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
	execution::LogicalPlanReadRuntime runtime(backend);
	execution::LogicalPlanReadRuntimeBounds bounds{.maximum_node_records = 8U, .maximum_node_page_records = 8U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U, .maximum_edge_records = 16U,
		.maximum_adjacency_page_records = 8U, .maximum_property_entries = 8U, .maximum_rows = 8U,
		.maximum_distinct_rows = 8U, .maximum_materialized_bytes = 8192U};
	const auto run = [&](std::string key, PropertyValue expected, bool batched = false) {
		std::unique_ptr<logical::LogicalOperator> child;
		if (batched) child = std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"});
		else child = std::make_unique<logical::LogicalSingleRow>();
		auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::move(child), "source", "", "target", "LINK", "out", batched ? 1 : 0, 2,
			std::vector<std::string>{}, std::vector<std::pair<std::string, PropertyValue>>{{std::move(key), std::move(expected)}});
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("target"));
		logical::LogicalProject project(std::move(traversal),
			{{std::make_shared<expressions::FunctionCallExpression>("id", std::move(arguments)), "id"}});
		if (batched) {
			std::vector<int64_t> ids;
			const auto result = runtime.executeToQueryResult(project, bounds);
			for (const auto &row: result.getRows()) ids.push_back(std::get<int64_t>(row.at("id").asPrimitive().getVariant()));
			return ids;
		}
		auto snapshot = backend.openLogicalGraphReadSnapshot();
		execution::LogicalPlanReadRow input;
		input.values.emplace("source", storage::EntityRef{3U, false, 0U, 0U, 0U});
		std::vector<int64_t> ids;
		static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, project, bounds, input,
			[&](const execution::LogicalPlanReadRow &row) {
				ids.push_back(std::get<int64_t>(std::get<PropertyValue>(row.values.at("id")).getVariant()));
				return true;
			}, QueryContext{}));
		return ids;
	};
	for (size_t retry = 0U; retry < 2U; ++retry) {
		backend.state->node_point_shapes.clear(); backend.state->node_batch_shapes.clear();
		EXPECT_EQ(run("fresh", PropertyValue(5)), std::vector<int64_t>({3, 1}));
		EXPECT_TRUE(run("absent", PropertyValue(5)).empty());
		EXPECT_GT(backend.state->property_column_reads, 0U);
		for (const auto &shape: backend.state->node_point_shapes) EXPECT_FALSE(shape.all_properties);
		for (const auto &shape: backend.state->node_batch_shapes) EXPECT_FALSE(shape.all_properties);
	}
	backend.state->nodes.at(3U).labels = {"Source"};
	backend.state->nodes.at(3U).properties.front().value = PropertyValue("");
	backend.state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 3U, 2U, "LINK"}, {13U, 3U, 1U, "LINK"}};
	backend.state->node_batch_shapes.clear(); backend.state->node_batch_sizes.clear();
	for (size_t retry = 0U; retry < 2U; ++retry) EXPECT_EQ(run("fresh", PropertyValue(5), true), std::vector<int64_t>({1, 1}));
	EXPECT_NE(std::ranges::find(backend.state->node_batch_sizes, 2U), backend.state->node_batch_sizes.end());
	for (const auto &shape: backend.state->node_batch_shapes) EXPECT_FALSE(shape.all_properties);
	backend.state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
	backend.state->nodes.at(1U).properties.back().value = PropertyValue();
	EXPECT_EQ(run("fresh", PropertyValue(5)), std::vector<int64_t>({3}));
	EXPECT_THROW(static_cast<void>(run("unused", PropertyValue(std::string(16000U, 'p')))), std::runtime_error);
	EXPECT_EQ(run("fresh", PropertyValue(5)), std::vector<int64_t>({3}));
	// A stored NULL matches this logical predicate; a missing field does not.
	// Cheap presence probes permit named NULL reads; legacy readers retain the fallback.
	for (bool presence: {false, true}) {
		backend.state->enable_property_presence_probes = presence;
		bounds.maximum_materialized_bytes = presence ? 8192U : 128U * 1024U;
		backend.state->node_point_shapes.clear(); backend.state->node_batch_shapes.clear();
		for (size_t retry = 0U; retry < 2U; ++retry) {
			EXPECT_EQ(run("fresh", PropertyValue()), std::vector<int64_t>({1}));
			EXPECT_TRUE(run("absent", PropertyValue()).empty());
			backend.state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 3U, 2U, "LINK"}, {13U, 3U, 1U, "LINK"}};
			EXPECT_EQ(run("fresh", PropertyValue(), true), std::vector<int64_t>({1, 1}));
			backend.state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
		}
		if (presence) {
			for (const auto &shape: backend.state->node_point_shapes) EXPECT_FALSE(shape.all_properties);
			for (const auto &shape: backend.state->node_batch_shapes) EXPECT_FALSE(shape.all_properties);
		}
	}
}

TEST(LogicalProcedureIdentityTest, VariableTraversalWithoutExistenceProbeAuthenticatesOnlyIdentity) {
	test::RecordingVectorProcedureBackend backend;
	backend.state->enable_existence_probes = false;
	for (uint64_t id = 1U; id <= 3U; ++id) {
		backend.state->nodes.emplace(id, storage::LogicalGraphNode{id, {"Endpoint"},
			{{"payload", PropertyValue(std::string(16000U, 'p'))}}});
	}
	backend.state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
	execution::LogicalPlanReadRuntime runtime(backend);
	execution::LogicalPlanReadRuntimeBounds bounds{.maximum_node_records = 8U, .maximum_node_page_records = 8U,
		.maximum_metadata_pages_per_node_page = 8U, .maximum_edges_per_node = 8U, .maximum_edge_records = 16U,
		.maximum_adjacency_page_records = 8U, .maximum_property_entries = 8U, .maximum_rows = 8U,
		.maximum_distinct_rows = 8U, .maximum_materialized_bytes = 8192U};
	for (int minimumHops: {0, 1}) {
		SCOPED_TRACE(minimumHops);
		auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalSingleRow>(), "source", "", "target", "LINK", "out", minimumHops, 2);
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("target"));
		logical::LogicalProject project(std::move(traversal),
			{{std::make_shared<expressions::FunctionCallExpression>("id", std::move(arguments)), "id"}});
		const auto run = [&](bool stop) {
			auto snapshot = backend.openLogicalGraphReadSnapshot();
			execution::LogicalPlanReadRow input;
			input.values.emplace("source", storage::EntityRef{3U, false, 0U, 0U, 0U});
			std::vector<uint64_t> ids;
			try {
				static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, project, bounds, input,
					[&](const execution::LogicalPlanReadRow &row) {
						ids.push_back(static_cast<uint64_t>(std::get<int64_t>(std::get<PropertyValue>(row.values.at("id")).getVariant())));
						return !stop;
					}, QueryContext{}));
			} catch (const std::runtime_error &error) {
				if (!stop) throw;
				EXPECT_STREQ(error.what(), "logical-plan read runtime input-row execution was stopped by its visitor");
			}
			return ids;
		};
		for (bool stop: {false, true}) for (size_t retry = 0U; retry < 2U; ++retry) {
			backend.state->node_point_shapes.clear();
			const std::vector<uint64_t> expected = minimumHops == 0
				? (stop ? std::vector<uint64_t>{3U} : std::vector<uint64_t>{3U, 1U, 2U})
				: (stop ? std::vector<uint64_t>{1U} : std::vector<uint64_t>{1U, 2U});
			EXPECT_EQ(run(stop), expected);
			ASSERT_FALSE(backend.state->node_point_shapes.empty());
			for (const auto &shape: backend.state->node_point_shapes) {
				EXPECT_FALSE(shape.include_labels); EXPECT_FALSE(shape.include_type);
				EXPECT_FALSE(shape.all_properties); EXPECT_TRUE(shape.property_tokens.empty());
			}
		}
		backend.state->nodes.erase(1U);
		EXPECT_THROW(static_cast<void>(run(false)), std::runtime_error);
		backend.state->nodes.emplace(1U, storage::LogicalGraphNode{1U, {}});
		EXPECT_FALSE(run(false).empty());
	}
}

TEST(LogicalProcedureIdentityTest, ShortestPathWithoutExistenceProbeRequestsOnlyIdentity) {
	test::RecordingVectorProcedureBackend backend;
	backend.state->enable_existence_probes = false;
	for (uint64_t id = 1U; id <= 3U; ++id) {
		backend.state->nodes.emplace(id, storage::LogicalGraphNode{id, {"Endpoint"},
			{{"payload", PropertyValue(std::string(16000U, 'p'))}}});
	}
	backend.state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
	LogicalReadExecutionOptions options;
	options.bounds.maximum_materialized_bytes = 8192U;
	execution::LogicalPlanProcedureRuntime runtime(backend, &backend, &backend);
	const auto run = [&](int64_t end) {
		logical::LogicalProject project(std::make_unique<logical::LogicalCallProcedure>("algo.shortestPath",
			std::vector<PropertyValue>{PropertyValue(3), PropertyValue(end), PropertyValue(2)}),
			{{std::make_shared<expressions::VariableReferenceExpression>("step"), "step"},
			 {std::make_shared<expressions::VariableReferenceExpression>("path_length"), "length"}});
		return runtime.execute(project, options.bounds, options.maximum_vector_mapping_entries,
			options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
			options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
			options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
			options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
	};
	for (int64_t end: {2, 3}) for (size_t retry = 0U; retry < 2U; ++retry) {
		backend.state->node_point_shapes.clear();
		const auto result = run(end);
		const size_t expected = end == 3 ? 1U : 3U;
		ASSERT_EQ(result.rowCount(), expected);
		for (size_t i = 0U; i < expected; ++i) {
			EXPECT_EQ(result.getRows()[i].at("step").asPrimitive(), PropertyValue(static_cast<int64_t>(i)));
			EXPECT_EQ(result.getRows()[i].at("length").asPrimitive(), PropertyValue(static_cast<int64_t>(expected)));
		}
		ASSERT_FALSE(backend.state->node_point_shapes.empty());
		for (const auto &shape: backend.state->node_point_shapes) {
			EXPECT_FALSE(shape.include_labels); EXPECT_FALSE(shape.include_type);
			EXPECT_FALSE(shape.all_properties); EXPECT_TRUE(shape.property_tokens.empty());
		}
		EXPECT_EQ(backend.state->node_batch_reads, 0U);
		EXPECT_EQ(backend.state->property_column_reads, 0U);
		EXPECT_EQ(backend.state->full_adjacency_scans, 0U);
	}
	backend.state->nodes.erase(1U);
	EXPECT_THROW(static_cast<void>(run(2)), std::runtime_error);
	backend.state->nodes.emplace(1U, storage::LogicalGraphNode{1U, {}});
	EXPECT_EQ(run(2).rowCount(), 3U);
}
TEST(LogicalProcedureIdentityTest, ShortestPathRejectsMalformedEndpointPresenceBatchesAndRetries) {
	test::RecordingVectorProcedureBackend backend;
	backend.state->nodes = {{1U, {1U, {}}}, {2U, {2U, {}}}, {3U, {3U, {}}}};
	backend.state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
	LogicalReadExecutionOptions options;
	execution::LogicalPlanProcedureRuntime runtime(backend, &backend, &backend);
	const auto run = [&] {
		logical::LogicalProject project(std::make_unique<logical::LogicalCallProcedure>("algo.shortestPath",
			std::vector<PropertyValue>{PropertyValue(3), PropertyValue(2), PropertyValue(2)}),
			{{std::make_shared<expressions::VariableReferenceExpression>("step"), "step"}});
		return runtime.execute(project, options.bounds, options.maximum_vector_mapping_entries,
			options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
			options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
			options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
			options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
	};
	for (size_t malformedCall: {1U, 2U}) for (int adjustment: {-1, 1}) {
		backend.state->batched_existence_scans = 0U;
		backend.state->malformed_existence_call = malformedCall;
		backend.state->existence_result_adjustment = adjustment;
		try {
			static_cast<void>(run());
			FAIL() << "malformed endpoint batch accepted";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure backend changed shortest-path endpoint batch cardinality");
		}
		backend.state->existence_result_adjustment = 0;
		for (int retry = 0; retry < 2; ++retry) EXPECT_EQ(run().rowCount(), 3U);
	}
}
} // namespace graph::query
