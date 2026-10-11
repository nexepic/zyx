/**
 * @file test_LogicalVectorBufferedSort.cpp
 * @brief Buffered Vector sorting permutation and row ownership contracts.
 */
#include <algorithm>
#include <array>
#include <memory>
#include <numeric>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "query/api/LogicalProcedureTestFixture.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/gds/LogicalProjectionCatalog.hpp"
#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"

namespace graph::query {
	TEST(LogicalVectorBufferedSortTest, PermutesMultipleCyclesAndFixedRowsWithStableTiesAndAlignedPayloads) {
		auto backend = std::make_shared<test::RecordingVectorProcedureBackend>();
		// Ascending order has two three-row cycles, a two-row cycle, fixed
		// rows, and equal keys. Descending order exercises different cycles.
		const std::array<int64_t, 12> ranks{2, 0, 1, 5, 3, 4, 6, 8, 7, 9, 10, 10};
		for (size_t index = 0U; index < ranks.size(); ++index) {
			storage::LogicalGraphNode node;
			node.node_id = index + 1U;
			node.properties = {{"rank", PropertyValue(ranks[index])},
				{"body", PropertyValue(std::string(1024U, static_cast<char>('a' + index)))}};
			backend->state->nodes.emplace(node.node_id, std::move(node));
			backend->state->search_result.hits.push_back({index + 1U, static_cast<float>(index)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		LogicalReadExecutionOptions options;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		for (const bool ascending: {true, false}) {
			SCOPED_TRACE(ascending);
			logical::LogicalProject project(std::make_unique<logical::LogicalSort>(
				std::make_unique<logical::LogicalCallProcedure>("db.index.vector.queryNodes",
					std::vector<PropertyValue>{PropertyValue("memory_embedding"), PropertyValue(int64_t{12}),
						PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})}),
				std::vector<logical::LogicalSortItem>{{rank, ascending}}),
				{{score, "score"}, {body, "body"}});
			const auto result = runtime.execute(project, options.bounds, options.maximum_vector_mapping_entries,
				options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
				options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
				options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
				options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
			std::vector<size_t> expected(ranks.size());
			std::iota(expected.begin(), expected.end(), 0U);
			std::stable_sort(expected.begin(), expected.end(), [&](size_t left, size_t right) {
				return ascending ? ranks[left] < ranks[right] : ranks[left] > ranks[right];
			});
			ASSERT_EQ(result.rowCount(), expected.size());
			for (size_t index = 0U; index < expected.size(); ++index) {
				EXPECT_EQ(result.getRows()[index].at("score").asPrimitive(), PropertyValue(static_cast<double>(expected[index])));
				EXPECT_EQ(result.getRows()[index].at("body").asPrimitive(),
					PropertyValue(std::string(1024U, static_cast<char>('a' + expected[index]))));
			}
		}
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}
	TEST(LogicalVectorBufferedSortTest, SelectedPropertyGatherPreservesThreeHundredRowsAcrossBoundedReads) {
		auto backend = std::make_shared<test::RecordingVectorProcedureBackend>();
		backend->state->enable_property_admission = true;
		for (uint64_t id = 1U; id <= 300U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(301U - id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id - 1U)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		logical::LogicalProject project(std::make_unique<logical::LogicalSort>(
			std::make_unique<logical::LogicalCallProcedure>("db.index.vector.queryNodes",
				std::vector<PropertyValue>{PropertyValue("memory_embedding"), PropertyValue(int64_t{300}),
					PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})}),
			std::vector<logical::LogicalSortItem>{{rank, true}}), {{rank, "rank"}, {score, "score"}});
		LogicalReadExecutionOptions options;
		options.bounds.maximum_materialized_bytes = 256U * 1024U;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		const auto execute = [&] {
			return runtime.execute(project, options.bounds, options.maximum_vector_mapping_entries,
			options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
			options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
			options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
			options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};
		// Final two-column publication also shares the source allowance.
		EXPECT_THROW(static_cast<void>(execute()), std::runtime_error);
		backend->state->property_column_ids.clear();
		options.bounds.maximum_materialized_bytes = 512U * 1024U;
		const auto result = execute();
		ASSERT_EQ(result.rowCount(), 300U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("rank").asPrimitive(), PropertyValue(static_cast<int64_t>(index + 1U)));
			EXPECT_EQ(result.getRows()[index].at("score").asPrimitive(), PropertyValue(static_cast<double>(299U - index)));
		}
		ASSERT_EQ(backend->state->property_column_ids.size(), 2U);
		EXPECT_EQ(backend->state->property_column_ids[0].size(), 256U);
		EXPECT_EQ(backend->state->property_column_ids[1].size(), 44U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST(LogicalVectorBufferedSortTest, CompactsProjectedFilterAndDistinctRowsWithoutMisaligningOwnedPayloads) {
		auto backend = std::make_shared<test::RecordingVectorProcedureBackend>();
		for (uint64_t id = 1U; id <= 9U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id % 3U))},
				{"body", PropertyValue(std::string(1024U, static_cast<char>('a' + id - 1U)))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id - 1U)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		const auto projectedBody = std::make_shared<expressions::VariableReferenceExpression>("body");
		const auto projectedRank = std::make_shared<expressions::VariableReferenceExpression>("r");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("r"),
			expressions::BinaryOperatorType::BOP_EQUAL,
			std::make_unique<expressions::LiteralExpression>(int64_t{2}));
		const auto filtered = [&] {
			return std::make_unique<logical::LogicalFilter>(
				std::make_unique<logical::LogicalProject>(
					std::make_unique<logical::LogicalCallProcedure>("db.index.vector.queryNodes",
						std::vector<PropertyValue>{PropertyValue("memory_embedding"), PropertyValue(int64_t{9}),
							PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})}),
					std::vector<logical::LogicalProjectItem>{{rank, "r"}, {body, "body"}, {score, "score"}}), predicate);
		};
		LogicalReadExecutionOptions options;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		const auto execute = [&](const logical::LogicalOperator &root) {
			return runtime.execute(root, options.bounds, options.maximum_vector_mapping_entries,
				options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
				options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
				options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
				options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};
		logical::LogicalProject projected(filtered(), {{score, "score"}, {projectedBody, "body"}});
		const auto result = execute(projected);
		ASSERT_EQ(result.rowCount(), 3U);
		for (size_t index = 0U; index < 3U; ++index) {
			const size_t sourceIndex = 1U + index * 3U;
			EXPECT_EQ(result.getRows()[index].at("score").asPrimitive(), PropertyValue(static_cast<double>(sourceIndex)));
			EXPECT_EQ(result.getRows()[index].at("body").asPrimitive(),
				PropertyValue(std::string(1024U, static_cast<char>('a' + sourceIndex))));
		}
		logical::LogicalProject distinct(filtered(), {{projectedRank, "r"}}, true);
		const auto unique = execute(distinct);
		ASSERT_EQ(unique.rowCount(), 1U);
		EXPECT_EQ(unique.getRows()[0].at("r").asPrimitive(), PropertyValue(int64_t{2}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST(LogicalVectorBufferedSortTest, OrdersMixedNullKeysForFullSortTopKAndOffsetWithoutLosingStableTies) {
		auto backend = std::make_shared<test::RecordingVectorProcedureBackend>();
		for (uint64_t id = 1U; id <= 4U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			if (id == 1U || id == 3U) node.properties = {{"rank", PropertyValue(id == 1U ? int64_t{2} : int64_t{1})}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id - 1U)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		LogicalReadExecutionOptions options;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		for (const bool ascending: {true, false}) {
			const std::array<int64_t, 4> expected = ascending ? std::array<int64_t, 4>{2, 0, 1, 3}
															  : std::array<int64_t, 4>{1, 3, 0, 2};
			for (const size_t window: {0U, 1U, 2U}) {
				SCOPED_TRACE(ascending);
				SCOPED_TRACE(window);
				std::unique_ptr<logical::LogicalOperator> input = std::make_unique<logical::LogicalSort>(
					std::make_unique<logical::LogicalCallProcedure>("db.index.vector.queryNodes",
						std::vector<PropertyValue>{PropertyValue("memory_embedding"), PropertyValue(int64_t{4}),
							PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})}),
					std::vector<logical::LogicalSortItem>{{rank, ascending}});
				if (window == 2U) input = std::make_unique<logical::LogicalSkip>(std::move(input), 1);
				if (window != 0U) input = std::make_unique<logical::LogicalLimit>(std::move(input), 2);
				logical::LogicalProject project(std::move(input), {{score, "score"}});
				const auto result = runtime.execute(project, options.bounds, options.maximum_vector_mapping_entries,
					options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
					options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
					options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
					options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
				const size_t offset = window == 2U ? 1U : 0U;
				ASSERT_EQ(result.rowCount(), window == 0U ? 4U : 2U);
				for (size_t index = 0U; index < result.rowCount(); ++index)
					EXPECT_EQ(result.getRows()[index].at("score").asPrimitive(), PropertyValue(static_cast<double>(expected[offset + index])));
			}
		}
	}

	TEST(LogicalGdsPathBudgetTest, AuthenticatesDescendingPathInBoundedSlicesAndRejectsMissingNodes) {
		auto backend = std::make_shared<test::RecordingVectorProcedureBackend>();
		for (uint64_t id = 1U; id <= 20U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			backend->state->nodes.emplace(id, std::move(node));
			if (id > 1U) backend->state->edges.push_back({100U + id, id, id - 1U, "LINK"});
		}
		gds::LogicalProjectionCatalog catalog;
		auto snapshot = backend->openLogicalGraphReadSnapshot();
		static_cast<void>(catalog.create({.name = "sliced_path"}, *snapshot,
			{.maximum_nodes = 64U, .maximum_metadata_pages = 64U, .maximum_edges_per_node = 64U}));
		backend->state->batched_existence_scan_sizes.clear();
		LogicalReadExecutionOptions options;
		options.bounds.maximum_adjacency_page_records = 3U;
		options.bounds.maximum_materialized_bytes = 64U * 1024U;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get(), nullptr, &catalog);
		const auto nodeId = std::make_shared<expressions::VariableReferenceExpression>("nodeId");
		const auto cost = std::make_shared<expressions::VariableReferenceExpression>("cost");
		logical::LogicalProject project(
			std::make_unique<logical::LogicalCallProcedure>("gds.shortestPath.dijkstra.stream",
				std::vector<PropertyValue>{PropertyValue("sliced_path"), PropertyValue(20), PropertyValue(1)}),
			{{nodeId, "nodeId"}, {cost, "cost"}});
		const auto execute = [&] {
			return runtime.execute(project, options.bounds, options.maximum_vector_mapping_entries,
				options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
				options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
				options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
				options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};
		const auto result = execute();
		ASSERT_EQ(result.rowCount(), 20U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("nodeId").asPrimitive(), PropertyValue(static_cast<int64_t>(20U - index)));
			EXPECT_EQ(result.getRows()[index].at("cost").asPrimitive(), PropertyValue(static_cast<double>(index)));
		}
		EXPECT_EQ(backend->state->batched_existence_scan_sizes, (std::vector<size_t>{3U, 3U, 3U, 3U, 3U, 3U, 2U}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
		backend->state->nodes.erase(10U);
		EXPECT_THROW(static_cast<void>(execute()), std::runtime_error);
	}

	TEST(LogicalGdsPathBudgetTest, ShortestPathShrinksUnprocessedTopologyPagesUnderSharedBytePressure) {
		auto backend = std::make_shared<test::RecordingVectorProcedureBackend>();
		for (uint64_t id = 1U; id <= 201U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			backend->state->nodes.emplace(id, std::move(node));
			if (id > 1U) backend->state->edges.push_back({id, 1U, id, "LINK"});
		}
		LogicalReadExecutionOptions options;
		options.bounds.maximum_materialized_bytes = 8192U;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		const auto step = std::make_shared<expressions::VariableReferenceExpression>("step");
		const auto length = std::make_shared<expressions::VariableReferenceExpression>("path_length");
		logical::LogicalProject project(
			std::make_unique<logical::LogicalCallProcedure>("algo.shortestPath",
				std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(1)}),
			{{step, "step"}, {length, "length"}});
		const auto execute = [&] {
			return runtime.execute(project, options.bounds, options.maximum_vector_mapping_entries,
				options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
				options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
				options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
				options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};
		const auto result = execute();
		ASSERT_EQ(result.rowCount(), 2U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("step").asPrimitive(), PropertyValue(static_cast<int64_t>(index)));
			EXPECT_EQ(result.getRows()[index].at("length").asPrimitive(), PropertyValue(2));
		}
		EXPECT_GT(backend->state->topology_scans, 1U);
		ASSERT_EQ(backend->state->batched_existence_scan_sizes.size(), 2U);
		EXPECT_LT(backend->state->batched_existence_scan_sizes.back(), 200U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		// Move the target beyond several pages. Self loops keep the BFS state
		// small while forcing continuation and page-local endpoint deduplication.
		backend->state->edges.clear();
		for (uint64_t id = 2U; id < 201U; ++id) backend->state->edges.push_back({id, 1U, 1U, "LINK"});
		backend->state->edges.push_back({201U, 1U, 2U, "LINK"});
		backend->state->batched_existence_scan_sizes.clear();
		const auto continued = execute();
		ASSERT_EQ(continued.rowCount(), 2U);
		EXPECT_EQ(continued.getRows()[1].at("step").asPrimitive(), PropertyValue(1));
		EXPECT_GE(backend->state->batched_existence_scan_sizes.size(), 4U);
		backend->state->return_nonmonotonic_adjacency_page = true;
		try {
			static_cast<void>(execute());
			FAIL() << "budget retry hid a malformed topology page";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure backend returned non-monotonic shortest-path relationship IDs");
		}
	}

}
