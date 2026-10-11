/**
 * @file test_LogicalVectorProcedureBatch.cpp
 * @brief Vector procedure batching and deferred materialization contracts.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include "query/api/LogicalProcedureTestFixture.hpp"

#include "graph/core/Types.hpp"
#include "graph/query/NativeProcedureManifest.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ListLiteralExpression.hpp"
#include "graph/query/expressions/MapProjectionExpression.hpp"
#include "graph/query/gds/LogicalProjectionCatalog.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalExplain.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProfile.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/LogicalGraphVectorSearch.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"

namespace graph::query {

	using test::RecordingVectorProcedureBackend;

	namespace {
		class LogicalVectorPropertyBatchTest : public ::testing::Test {
		protected:
			void SetUp() override {
				for (uint64_t id = 1U; id <= 3U; ++id) {
					storage::LogicalGraphNode node;
					node.node_id = id;
					node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id * 10U))},
							{"body", PropertyValue(std::string(4096U, 'x'))}};
					backend->state->nodes.emplace(id, std::move(node));
				}
				backend->state->search_result.hits = {{3U, 0.0F}, {1U, 1.0F}, {2U, 2.0F}};
			}
			[[nodiscard]] std::unique_ptr<logical::LogicalCallProcedure> search(const int64_t topK = 3) const {
				return std::make_unique<logical::LogicalCallProcedure>("db.index.vector.queryNodes",
						std::vector<PropertyValue>{PropertyValue("memory_embedding"), PropertyValue(topK),
								PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})});
			}
			[[nodiscard]] QueryResult execute(const logical::LogicalOperator &root) const {
				execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
				return runtime.execute(root, options.bounds, options.maximum_vector_mapping_entries,
						options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
						options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
						options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
						options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
			}
			std::shared_ptr<RecordingVectorProcedureBackend> backend = std::make_shared<RecordingVectorProcedureBackend>();
			LogicalReadExecutionOptions options;
		};
	}

	TEST_F(LogicalVectorPropertyBatchTest, ProjectExpressionsShareRetainedProcedureOutputBudget) {
		backend->state->search_result.hits = {{1U, 0.0F}};
		const auto list = [] {
			std::vector<PropertyValue> values;
			for (size_t index = 0U; index < 3U; ++index) values.emplace_back(std::string(1200U, static_cast<char>('a' + index)));
			return std::make_shared<expressions::ListLiteralExpression>(PropertyValue(std::move(values)));
		};
		options.bounds.maximum_rows = 128U;
		options.bounds.maximum_materialized_bytes = 8192U;
		logical::LogicalProject single(search(1), {{list(), "values"}});
		EXPECT_NO_THROW(static_cast<void>(execute(single)));

		logical::LogicalProject pair(search(1), {{list(), "first"}, {list(), "second"}});
		try {
			static_cast<void>(execute(pair));
			FAIL() << "the second procedure projection ignored the retained first List";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure source and retained result exceeded their byte bound");
		}
		options.bounds.maximum_materialized_bytes = 20000U;
		const QueryResult result = execute(pair);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("first").asPrimitive().getList().size(), 3U);
		EXPECT_EQ(result.getRows().front().at("second").asPrimitive().getList().size(), 3U);

		backend->state->search_result.hits = {{1U, 0.0F}, {2U, 1.0F}};
		logical::LogicalProject perRow(search(2), {{list(), "values"}});
		options.bounds.maximum_materialized_bytes = 6000U;
		try {
			static_cast<void>(execute(perRow));
			FAIL() << "a later procedure row ignored the retained earlier output List";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "shared materialization exceeded its byte bound");
		}
		options.bounds.maximum_materialized_bytes = 20000U;
		const QueryResult multiple = execute(perRow);
		ASSERT_EQ(multiple.rowCount(), 2U);
		for (const auto &row: multiple.getRows()) EXPECT_EQ(row.at("values").asPrimitive().getList().size(), 3U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, ProcedureArgumentViewReferencesLargeDirectAndBoundValues) {
		const PropertyValue largeList(std::vector<PropertyValue>(4096U, PropertyValue(3.0)));
		logical::LogicalCallProcedure direct("db.index.vector.queryNodes",
				{PropertyValue("memory_embedding"), PropertyValue(int64_t{1}), largeList});
		const std::unordered_map<std::string, PropertyValue> noParameters;
		const auto directArguments = direct.resolveArgumentView(noParameters);
		ASSERT_EQ(directArguments.size(), 3U);
		EXPECT_EQ(&directArguments[2U], &direct.getArgs()[2U]);

		const std::unordered_map<std::string, PropertyValue> parameters{{"query", largeList}};
		const auto bound = logical::LogicalCallProcedure::withBoundArguments(
				"db.index.vector.queryNodes", {"query"});
		const auto boundArguments = bound->resolveArgumentView(parameters);
		ASSERT_EQ(boundArguments.size(), 1U);
		EXPECT_EQ(&boundArguments[0U], &parameters.at("query"));
	}

	TEST_F(LogicalVectorPropertyBatchTest, EmptyVectorProcedureResultChargesOutputColumnStorage) {
		backend->state->search_result.hits.clear();
		const std::string outputAlias(8192U, 'c');
		auto project = std::make_unique<logical::LogicalProject>(search(1),
				std::vector<logical::LogicalProjectItem>{{
						std::make_shared<expressions::VariableReferenceExpression>("node"), outputAlias}});
		logical::LogicalLimit empty(std::move(project), 0);
		options.bounds.maximum_materialized_bytes = 4096U;
		try {
			static_cast<void>(execute(empty));
			FAIL() << "empty procedure output omitted its retained column name from the byte budget";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure source and retained result exceeded their byte bound");
		}
		auto zeroTopKProjection = std::make_unique<logical::LogicalProject>(search(0),
				std::vector<logical::LogicalProjectItem>{{
						std::make_shared<expressions::VariableReferenceExpression>("node"), outputAlias}});
		try {
			static_cast<void>(execute(*zeroTopKProjection));
			FAIL() << "zero-topK procedure output omitted its retained column name from the byte budget";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure source and retained result exceeded their byte bound");
		}

		options.bounds.maximum_materialized_bytes = 16U * 1024U;
		const QueryResult result = execute(empty);
		EXPECT_EQ(result.rowCount(), 0U);
		ASSERT_EQ(result.getColumns().size(), 1U);
		EXPECT_EQ(result.getColumns().front(), outputAlias);
	}

	TEST_F(LogicalVectorPropertyBatchTest, TopKAllFilteredRowsStillChargesOutputColumnStorage) {
		for (uint64_t id = 4U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const std::string outputAlias(8192U, 't');
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto rejectAll = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{100}));
		auto project = std::make_unique<logical::LogicalProject>(
				std::make_unique<logical::LogicalLimit>(
						std::make_unique<logical::LogicalSkip>(
								std::make_unique<logical::LogicalFilter>(
										std::make_unique<logical::LogicalSort>(search(10),
												std::vector<logical::LogicalSortItem>{{rank, true}}), rejectAll), 0), 1),
				std::vector<logical::LogicalProjectItem>{{rank, outputAlias}});
		options.bounds.maximum_materialized_bytes = 4096U;
		EXPECT_THROW(static_cast<void>(execute(*project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 16U * 1024U;
		const QueryResult result = execute(*project);
		EXPECT_EQ(result.rowCount(), 0U);
		ASSERT_EQ(result.getColumns().size(), 1U);
		EXPECT_EQ(result.getColumns().front(), outputAlias);
	}

	TEST_F(LogicalVectorPropertyBatchTest, DirectVectorResultSharesBufferedRowsAndRetainedResultBudget) {
		for (auto &[id, node]: backend->state->nodes) {
			static_cast<void>(id);
			node.properties = {{"body", PropertyValue(std::string(700U, 'x'))}};
		}
		options.bounds.maximum_materialized_bytes = 4000U;
		EXPECT_THROW(static_cast<void>(execute(*search())), std::runtime_error);

		options.bounds.maximum_materialized_bytes = 16U * 1024U;
		const QueryResult result = execute(*search());
		ASSERT_EQ(result.rowCount(), 3U);
		for (const auto &row: result.getRows()) {
			EXPECT_EQ(row.at("node").asLogicalNode().properties.at("body"),
					PropertyValue(std::string(700U, 'x')));
		}
	}

	TEST_F(LogicalVectorPropertyBatchTest, RejectsQueryVectorCopyBeforeSearchWhenItExceedsMaterializedBound) {
		backend->state->cancellation_checks_before_throw = 1U;
		options.bounds.maximum_materialized_bytes = sizeof(float);
		logical::LogicalCallProcedure oversized("db.index.vector.queryNodes",
				{PropertyValue("memory_embedding"), PropertyValue(int64_t{1}),
						PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(2.0)})});

		EXPECT_THROW(static_cast<void>(execute(oversized)), std::runtime_error);
		EXPECT_EQ(backend->state->search_cancellation_checks, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, RetainsConvertedQueryVectorThroughResultPublication) {
		backend->state->search_result.hits = {{1U, 0.5F}};
		std::vector<PropertyValue> coordinates(750U, PropertyValue(1.0));
		const auto call = [&coordinates] {
			return std::make_unique<logical::LogicalCallProcedure>("db.index.vector.queryNodes",
					std::vector<PropertyValue>{PropertyValue("memory_embedding"), PropertyValue(int64_t{1}),
							PropertyValue(coordinates)});
		};
		logical::LogicalProject project(call(),
				{{std::make_shared<expressions::VariableReferenceExpression>("score"), "score"}});

		options.bounds.maximum_materialized_bytes = 4096U;
		try {
			static_cast<void>(execute(project));
			FAIL() << "the live converted Vector query and retained result should share the byte bound";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure source and retained result exceeded their byte bound");
		}

		options.bounds.maximum_materialized_bytes = 64U * 1024U;
		const QueryResult result = execute(project);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("score").asPrimitive(), PropertyValue(0.5));
	}

	TEST_F(LogicalVectorPropertyBatchTest, RejectsOversizedSearchHitBufferBeforeReadingNodes) {
		options.bounds.maximum_materialized_bytes =
				2U * sizeof(storage::LogicalGraphVectorSearchHit);
		try {
			static_cast<void>(execute(*search()));
			FAIL() << "the retained Vector hit source exceeded the shared byte bound";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical Vector procedure hit buffer exceeded its materialized byte bound");
		}
		EXPECT_EQ(backend->state->node_point_reads, 0U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, CountsRetainedQueryResultRowsAcrossProcedureBatches) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 100U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(search(100), {{rank, "rank"}});
		options.bounds.maximum_rows = 128U;
		options.bounds.maximum_materialized_bytes = 12'000U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		EXPECT_GT(backend->state->property_column_reads, 1U);

		backend->state->property_column_reads = 0U;
		// The complete source rows and the retained public result coexist during
		// publication, so the successful bound must admit both owners.
		options.bounds.maximum_materialized_bytes = 128'000U;
		const QueryResult result = execute(project);
		ASSERT_EQ(result.rowCount(), 100U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("rank").asPrimitive(),
					PropertyValue(static_cast<int64_t>(index + 1U)));
		}
	}

	TEST_F(LogicalVectorPropertyBatchTest, ShortestPathScalarOutputSkipsFullNodesUnderSmallByteBound) {
		backend->state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
		for (auto &[id, node]: backend->state->nodes) {
			static_cast<void>(id);
			node.properties.back().value = PropertyValue(std::string(8192U, 'x'));
		}
		options.bounds.maximum_materialized_bytes = 8192U;
		const auto call = [] {
			return std::make_unique<logical::LogicalCallProcedure>("algo.shortestPath",
				std::vector<PropertyValue>{PropertyValue(3), PropertyValue(2), PropertyValue(2)});
		};
		logical::LogicalProject project(call(),
			{{std::make_shared<expressions::VariableReferenceExpression>("step"), "step"},
			 {std::make_shared<expressions::VariableReferenceExpression>("path_length"), "length"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("step").asPrimitive(), PropertyValue(static_cast<int64_t>(index)));
			EXPECT_EQ(result.getRows()[index].at("length").asPrimitive(), PropertyValue(3));
		}
		EXPECT_EQ(backend->state->node_point_reads, 0U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->property_column_reads, 0U);
		options.bounds.maximum_rows = 2U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_rows = 128U;
		EXPECT_THROW(static_cast<void>(execute(*call())), std::runtime_error);
	}

	TEST_F(LogicalVectorPropertyBatchTest, ShortestPathBfsStateObeysMaterializedByteBudget) {
		backend->state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
		options.bounds.maximum_materialized_bytes = 32U;
		const auto call = [] {
			return std::make_unique<logical::LogicalCallProcedure>("algo.shortestPath",
					std::vector<PropertyValue>{PropertyValue(3), PropertyValue(2), PropertyValue(2)});
		};
		const auto makeScalarProject = [&] {
			return logical::LogicalProject(call(),
					{{std::make_shared<expressions::VariableReferenceExpression>("step"), "step"},
							{std::make_shared<expressions::VariableReferenceExpression>("path_length"), "length"}});
		};
		try {
			static_cast<void>(execute(makeScalarProject()));
			FAIL() << "shortestPath expanded BFS state beyond its shared byte limit";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(),
					"logical shortest-path procedure exceeded its materialized state byte bound");
		}
		EXPECT_EQ(backend->state->topology_scans, 0U);

		options.bounds.maximum_materialized_bytes = 64U;
		backend->state->topology_scans = 0U;
		backend->state->batched_existence_scans = 0U;
		try {
			static_cast<void>(execute(makeScalarProject()));
			FAIL() << "shortestPath retained its adjacency page beyond the shared byte limit";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "shared materialization exceeded its byte bound");
		}
		EXPECT_GT(backend->state->topology_scans, 0U);
		// The initial start/end probe happens before BFS; the page-local probe
		// must not run until its endpoint buffers have been admitted.
		EXPECT_EQ(backend->state->batched_existence_scans, 1U);

		options.bounds.maximum_materialized_bytes = 8192U;
		const QueryResult result = execute(makeScalarProject());
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows().front().at("length").asPrimitive(), PropertyValue(3));
	}

	TEST_F(LogicalVectorPropertyBatchTest, ShortestPathDirectResultSharesBufferedOutputBudget) {
		backend->state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
		const auto call = [] {
			return logical::LogicalCallProcedure("algo.shortestPath",
					{PropertyValue(3), PropertyValue(2), PropertyValue(2)});
		};
		options.bounds.maximum_materialized_bytes = 16U * 1024U;
		try {
			static_cast<void>(execute(call()));
			FAIL() << "direct shortestPath output ignored concurrently retained result rows";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure source and retained result exceeded their byte bound");
		}

		options.bounds.maximum_materialized_bytes = 64U * 1024U;
		const QueryResult result = execute(call());
		ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"step", "node", "path_length"}));
		ASSERT_EQ(result.rowCount(), 3U);
		const std::array<uint64_t, 3U> expected{3U, 1U, 2U};
		for (size_t index = 0U; index < expected.size(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("step").asPrimitive(), PropertyValue(static_cast<int64_t>(index)));
			EXPECT_EQ(result.getRows()[index].at("node").asNode().getId(), expected[index]);
			EXPECT_EQ(result.getRows()[index].at("path_length").asPrimitive(), PropertyValue(3));
		}
	}

	TEST_F(LogicalVectorPropertyBatchTest, ShortestPathProjectedResultCountsLivePathIds) {
		backend->state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
		const auto call = [] {
			return std::make_unique<logical::LogicalCallProcedure>("algo.shortestPath",
					std::vector<PropertyValue>{PropertyValue(3), PropertyValue(2), PropertyValue(2)});
		};
		logical::LogicalProject project(call(),
				{{std::make_shared<expressions::VariableReferenceExpression>("step"), "step"},
				 {std::make_shared<expressions::VariableReferenceExpression>("path_length"), "length"}});
		options.bounds.maximum_materialized_bytes = 4096U;
		try {
			static_cast<void>(execute(project));
			FAIL() << "shortestPath result publication ignored its live path-ID vector";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure source and retained result exceeded their byte bound");
		}

		options.bounds.maximum_materialized_bytes = 8192U;
		const QueryResult result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("step").asPrimitive(), PropertyValue(static_cast<int64_t>(index)));
			EXPECT_EQ(result.getRows()[index].at("length").asPrimitive(), PropertyValue(3));
		}
	}

	TEST_F(LogicalVectorPropertyBatchTest, GdsDijkstraProcedureForwardsMaterializedByteBudget) {
		backend->state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
		gds::LogicalProjectionCatalog catalog;
		auto snapshot = backend->openLogicalGraphReadSnapshot();
		const auto descriptor = catalog.create({.name = "bounded_path"}, *snapshot,
				{.maximum_nodes = 16U, .maximum_metadata_pages = 16U, .maximum_edges_per_node = 16U});
		EXPECT_EQ(descriptor.name, "bounded_path");
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get(), nullptr, &catalog);
		const auto call = [] {
			return logical::LogicalCallProcedure("gds.shortestPath.dijkstra.stream",
					{PropertyValue("bounded_path"), PropertyValue(3), PropertyValue(2)});
		};
		const auto executeGds = [&](const size_t maximumBytes) {
			auto bounds = options.bounds;
			bounds.maximum_materialized_bytes = maximumBytes;
			bounds.maximum_adjacency_page_records = 1U;
			return runtime.execute(call(), bounds, options.maximum_vector_mapping_entries,
					options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
					options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
					options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
					options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};

		EXPECT_THROW(static_cast<void>(executeGds(80U)), std::runtime_error);
		try {
			static_cast<void>(executeGds(16U * 1024U));
			FAIL() << "the buffered Node rows and retained Dijkstra QueryResult exceeded their shared budget";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure source and retained result exceeded their byte bound");
		}
		const QueryResult result = executeGds(24U * 1024U);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("nodeId").asPrimitive(), PropertyValue(3));
		EXPECT_EQ(result.getRows()[1].at("nodeId").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(result.getRows()[2].at("nodeId").asPrimitive(), PropertyValue(2));
		EXPECT_DOUBLE_EQ(std::get<double>(result.getRows()[2].at("totalCost").asPrimitive().getVariant()), 2.0);
		ASSERT_GE(backend->state->batched_existence_scan_sizes.size(), 3U);
		EXPECT_EQ(std::vector<size_t>(backend->state->batched_existence_scan_sizes.end() - 3,
				backend->state->batched_existence_scan_sizes.end()), (std::vector<size_t>{1U, 1U, 1U}));
	}

	TEST_F(LogicalVectorPropertyBatchTest, GdsDijkstraPipelineRetainsPathBytesDuringResultPublication) {
		backend->state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
		gds::LogicalProjectionCatalog catalog;
		auto snapshot = backend->openLogicalGraphReadSnapshot();
		static_cast<void>(catalog.create({.name = "yielded_path"}, *snapshot,
				{.maximum_nodes = 16U, .maximum_metadata_pages = 16U, .maximum_edges_per_node = 16U}));
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get(), nullptr, &catalog);
		auto call = std::make_unique<logical::LogicalCallProcedure>("gds.shortestPath.dijkstra.stream",
				std::vector<PropertyValue>{PropertyValue("yielded_path"), PropertyValue(3), PropertyValue(2)});
		logical::LogicalProject project(std::move(call), {{
				std::make_shared<expressions::LiteralExpression>(PropertyValue(std::string(6000U, 'x'))), "payload"}});
		const auto executeGds = [&](const size_t maximumBytes) {
			auto bounds = options.bounds;
			bounds.maximum_materialized_bytes = maximumBytes;
			return runtime.execute(project, bounds, options.maximum_vector_mapping_entries,
					options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
					options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
					options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
					options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};
		EXPECT_THROW(static_cast<void>(executeGds(20U * 1024U)), std::runtime_error);
		const QueryResult result = executeGds(64U * 1024U);
		ASSERT_EQ(result.rowCount(), 3U);
		for (const auto &row: result.getRows()) {
			EXPECT_EQ(row.at("payload").asPrimitive(), PropertyValue(std::string(6000U, 'x')));
		}
	}

	TEST_F(LogicalVectorPropertyBatchTest, GdsPageRankPipelineSharesAlgorithmRowsAndQueryResultBudget) {
		backend->state->edges = {{11U, 1U, 2U, "LINK"}, {12U, 2U, 3U, "LINK"}};
		gds::LogicalProjectionCatalog catalog;
		auto snapshot = backend->openLogicalGraphReadSnapshot();
		static_cast<void>(catalog.create({.name = "bounded_rank"}, *snapshot,
				{.maximum_nodes = 16U, .maximum_metadata_pages = 16U, .maximum_edges_per_node = 16U}));
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get(), nullptr, &catalog);
		auto call = std::make_unique<logical::LogicalCallProcedure>("gds.pageRank.stream",
				std::vector<PropertyValue>{PropertyValue("bounded_rank")});
		logical::LogicalProject project(std::move(call), {{
				std::make_shared<expressions::LiteralExpression>(PropertyValue(std::string(6000U, 'r'))), "payload"}});
		const auto executeGds = [&](const size_t maximumBytes) {
			auto bounds = options.bounds;
			bounds.maximum_materialized_bytes = maximumBytes;
			return runtime.execute(project, bounds, options.maximum_vector_mapping_entries,
					options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
					options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
					options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
					options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};
		EXPECT_THROW(static_cast<void>(executeGds(20U * 1024U)), std::runtime_error);
		const QueryResult result = executeGds(64U * 1024U);
		ASSERT_EQ(result.rowCount(), 3U);
		for (const auto &row: result.getRows()) {
			EXPECT_EQ(row.at("payload").asPrimitive(), PropertyValue(std::string(6000U, 'r')));
		}
	}

	TEST_F(LogicalVectorPropertyBatchTest, GdsCatalogListSharesBufferedRowsAndRetainedResultBudget) {
		gds::LogicalProjectionCatalog catalog;
		auto snapshot = backend->openLogicalGraphReadSnapshot();
		for (const std::string name: {"gamma", "alpha", "beta"}) {
			static_cast<void>(catalog.create({.name = name}, *snapshot,
					{.maximum_nodes = 16U, .maximum_metadata_pages = 16U, .maximum_edges_per_node = 16U}));
		}
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get(), nullptr, &catalog);
		const auto executeList = [&](const size_t maximumBytes) {
			auto bounds = options.bounds;
			bounds.maximum_materialized_bytes = maximumBytes;
			logical::LogicalCallProcedure call("gds.graph.list", {});
			return runtime.execute(call, bounds, options.maximum_vector_mapping_entries,
					options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
					options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
					options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
					options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};

		try {
			static_cast<void>(executeList(8U * 1024U));
			FAIL() << "gds.graph.list ignored the accumulated catalog result during publication";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure source and retained result exceeded their byte bound");
		}
		const QueryResult result = executeList(64U * 1024U);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("name").asPrimitive(), PropertyValue("alpha"));
		EXPECT_EQ(result.getRows()[1].at("name").asPrimitive(), PropertyValue("beta"));
		EXPECT_EQ(result.getRows()[2].at("name").asPrimitive(), PropertyValue("gamma"));
	}

	TEST_F(LogicalVectorPropertyBatchTest, GdsPageRankAndWccProceduresForwardMaterializedByteBudget) {
		backend->state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
		gds::LogicalProjectionCatalog catalog;
		auto snapshot = backend->openLogicalGraphReadSnapshot();
		const auto descriptor = catalog.create({.name = "bounded_algorithms"}, *snapshot,
				{.maximum_nodes = 16U, .maximum_metadata_pages = 16U, .maximum_edges_per_node = 16U});
		EXPECT_EQ(descriptor.name, "bounded_algorithms");
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get(), nullptr, &catalog);
		const auto executeGds = [&](const std::string &procedure, const size_t maximumBytes,
									const std::optional<size_t> maximumRows = std::nullopt) {
			auto bounds = options.bounds;
			bounds.maximum_materialized_bytes = maximumBytes;
			bounds.maximum_rows = maximumRows.value_or(options.bounds.maximum_rows);
			logical::LogicalCallProcedure call(procedure, {PropertyValue("bounded_algorithms")});
			return runtime.execute(call, bounds, options.maximum_vector_mapping_entries,
					options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
					options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
					options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
					options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};

		EXPECT_THROW(static_cast<void>(executeGds("gds.pageRank.stream", 95U)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(executeGds("gds.wcc.stream", 71U)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(executeGds("gds.closeness.stream", 1U)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(executeGds("gds.betweenness.stream", 1U)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(executeGds("gds.leiden.stream", 1U)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(executeGds("gds.pageRank.stream", 16U * 1024U, 1U)), std::runtime_error);
		// The algorithm's retained state fits, but its buffered procedure rows do not.
		EXPECT_THROW(static_cast<void>(executeGds("gds.pageRank.stream", 512U)), std::runtime_error);
		EXPECT_EQ(executeGds("gds.pageRank.stream", 16U * 1024U).rowCount(), 3U);
		EXPECT_EQ(executeGds("gds.wcc.stream", 16U * 1024U).rowCount(), 3U);
		EXPECT_EQ(executeGds("gds.closeness.stream", 16U * 1024U).rowCount(), 3U);
		EXPECT_EQ(executeGds("gds.betweenness.stream", 16U * 1024U).rowCount(), 3U);
		EXPECT_EQ(executeGds("gds.leiden.stream", 16U * 1024U).rowCount(), 3U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, ShortestPathSelectedPropertiesRestorePathOrderWithoutCompleteReads) {
		backend->state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
		options.bounds.maximum_materialized_bytes = 4096U;
		logical::LogicalProject project(std::make_unique<logical::LogicalCallProcedure>("algo.shortestPath",
			std::vector<PropertyValue>{PropertyValue(3), PropertyValue(2), PropertyValue(2)}),
			{{std::make_shared<expressions::VariableReferenceExpression>("node", "rank"), "rank"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(30));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(10));
		EXPECT_EQ(result.getRows()[2].at("rank").asPrimitive(), PropertyValue(20));
		EXPECT_EQ(backend->state->property_column_reads, 1U);
		EXPECT_EQ(backend->state->property_column_ids.front(), (std::vector<uint64_t>{1U, 2U, 3U}));
		EXPECT_EQ(backend->state->node_point_reads, 0U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, ShortestPathLimitMaterializesOnlyRequestedNodeAndHandlesSelfPath) {
		backend->state->edges = {{11U, 3U, 1U, "LINK"}, {12U, 1U, 2U, "LINK"}};
		logical::LogicalProject project(std::make_unique<logical::LogicalLimit>(
			std::make_unique<logical::LogicalCallProcedure>("algo.shortestPath",
				std::vector<PropertyValue>{PropertyValue(3), PropertyValue(2), PropertyValue(2)}), 1),
			{{std::make_shared<expressions::VariableReferenceExpression>("node"), "node"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows()[0].at("node").asLogicalNode().node_id, 3U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
		EXPECT_EQ(backend->state->node_batch_sizes, (std::vector<size_t>{1U}));
		backend->state->node_batch_reads = 0U;
		logical::LogicalProject self(std::make_unique<logical::LogicalCallProcedure>("algo.shortestPath",
			std::vector<PropertyValue>{PropertyValue(2), PropertyValue(2)}),
			{{std::make_shared<expressions::VariableReferenceExpression>("path_length"), "length"}});
		const auto selfResult = execute(self);
		ASSERT_EQ(selfResult.rowCount(), 1U);
		EXPECT_EQ(selfResult.getRows()[0].at("length").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, BatchesMetadataAndSelectedPropertiesInTheSameExpression) {
		backend->state->nodes.at(1U).labels = {"One"};
		backend->state->nodes.at(2U).labels = {"Two", "Extra"};
		backend->state->nodes.at(3U).labels = {"Three"};
		options.bounds.maximum_materialized_bytes = 4096U;
		for (const bool sorted: {false, true}) {
			SCOPED_TRACE(sorted);
			backend->state->node_batch_reads = 0U;
			backend->state->node_batch_shapes.clear();
			backend->state->property_column_reads = 0U;
			std::vector<std::unique_ptr<expressions::Expression>> labelsArguments;
			labelsArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
			std::vector<std::unique_ptr<expressions::Expression>> sizeArguments;
			sizeArguments.push_back(std::make_unique<expressions::FunctionCallExpression>("labels", std::move(labelsArguments)));
			const auto value = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::FunctionCallExpression>("size", std::move(sizeArguments)),
				expressions::BinaryOperatorType::BOP_ADD,
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"));
			std::unique_ptr<logical::LogicalOperator> child = search();
			if (sorted) child = std::make_unique<logical::LogicalSort>(std::move(child),
				std::vector<logical::LogicalSortItem>{{std::make_shared<expressions::VariableReferenceExpression>("score"), true}});
			logical::LogicalProject project(std::move(child), {{value, "value"}});
			const auto result = execute(project);
			ASSERT_EQ(result.rowCount(), 3U);
			EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue(31));
			EXPECT_EQ(result.getRows()[1].at("value").asPrimitive(), PropertyValue(11));
			EXPECT_EQ(result.getRows()[2].at("value").asPrimitive(), PropertyValue(22));
			EXPECT_EQ(backend->state->node_batch_reads, 1U);
			ASSERT_EQ(backend->state->node_batch_shapes.size(), 1U);
			EXPECT_TRUE(backend->state->node_batch_shapes.front().include_labels);
			EXPECT_FALSE(backend->state->node_batch_shapes.front().all_properties);
			EXPECT_EQ(backend->state->property_column_reads, 1U);
			EXPECT_EQ(backend->state->node_point_reads, 0U);
		}
	}

	TEST_F(LogicalVectorPropertyBatchTest, ReusesMetadataAcrossEntityRenamingFilterAndSort) {
		backend->state->nodes.at(1U).labels = {"One", "Extra"};
		backend->state->nodes.at(2U).labels.clear();
		backend->state->nodes.at(3U).labels = {"Three"};
		const auto labels = [](const std::string &alias) {
			std::vector<std::unique_ptr<expressions::Expression>> arguments;
			arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>(alias));
			return std::make_unique<expressions::FunctionCallExpression>("labels", std::move(arguments));
		};
		const auto labelCount = [&](const std::string &alias) {
			std::vector<std::unique_ptr<expressions::Expression>> arguments;
			arguments.push_back(labels(alias));
			return std::make_unique<expressions::FunctionCallExpression>("size", std::move(arguments));
		};
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(labelCount("entity"),
			expressions::BinaryOperatorType::BOP_GREATER, std::make_unique<expressions::LiteralExpression>(int64_t{0}));
		std::shared_ptr<expressions::Expression> sortKey = labelCount("entity");
		std::shared_ptr<expressions::Expression> originalLabels = labels("node");
		std::shared_ptr<expressions::Expression> outputLabels = labels("entity");
		logical::LogicalProject project(std::make_unique<logical::LogicalSort>(
			std::make_unique<logical::LogicalFilter>(std::make_unique<logical::LogicalProject>(search(),
				std::vector<logical::LogicalProjectItem>{{std::make_shared<expressions::VariableReferenceExpression>("node"), "entity"},
					{originalLabels, "initial"}}), predicate), std::vector<logical::LogicalSortItem>{{sortKey, true}}),
			{{outputLabels, "labels"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("labels").asPrimitive(), PropertyValue(std::vector<PropertyValue>{PropertyValue("Three")}));
		EXPECT_EQ(result.getRows()[1].at("labels").asPrimitive(), PropertyValue(std::vector<PropertyValue>{PropertyValue("One"), PropertyValue("Extra")}));
		EXPECT_EQ(backend->state->node_batch_reads, 1U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
		EXPECT_EQ(backend->state->property_column_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, CompletePropertyBatchAlsoSuppliesLabelsWithoutSecondRead) {
		backend->state->nodes.at(1U).labels = {"One"};
		std::vector<std::unique_ptr<expressions::Expression>> propertiesArguments;
		propertiesArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		std::vector<std::unique_ptr<expressions::Expression>> labelsArguments;
		labelsArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto properties = std::make_shared<expressions::FunctionCallExpression>("properties", std::move(propertiesArguments));
		const auto labels = std::make_shared<expressions::FunctionCallExpression>("labels", std::move(labelsArguments));
		logical::LogicalProject project(search(), {{properties, "all"}, {labels, "labels"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[1].at("labels").asPrimitive(), PropertyValue(std::vector<PropertyValue>{PropertyValue("One")}));
		EXPECT_EQ(backend->state->node_batch_reads, 1U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, MetadataBatchRejectsMalformedAndOversizedLabels) {
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto labels = std::make_shared<expressions::FunctionCallExpression>("labels", std::move(arguments));
		logical::LogicalProject project(search(), {{labels, "labels"}});
		backend->state->malformed_node_batch_cardinality = true;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		backend->state->malformed_node_batch_cardinality = false;
		backend->state->reverse_node_batch = true;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		backend->state->reverse_node_batch = false;
		backend->state->nodes.at(1U).labels = {"Repeated", "Repeated"};
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		backend->state->nodes.at(1U).labels = {std::string(8192U, 'x')};
		options.bounds.maximum_materialized_bytes = 2048U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
	}

	TEST_F(LogicalVectorPropertyBatchTest, MetadataJointRetentionRejectsLowBudgetAndCanRetry) {
		for (auto &[id, node] : backend->state->nodes) {
			node.labels.clear();
			for (size_t index = 0U; index < 20U; ++index) node.labels.push_back(std::string(50U, static_cast<char>('a' + id)) + std::to_string(index));
		}
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto labels = std::make_shared<expressions::FunctionCallExpression>("labels", std::move(arguments));
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		logical::LogicalProject project(std::make_unique<logical::LogicalSort>(search(),
			std::vector<logical::LogicalSortItem>{{score, true}}), {{labels, "labels"}});
		options.bounds.maximum_materialized_bytes = 6000U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 7500U;
		for (int retry = 0; retry < 2; ++retry) {
			const size_t before = backend->state->node_batch_reads;
			const auto result = execute(project);
			ASSERT_EQ(result.rowCount(), 3U);
			const std::array<uint64_t, 3U> order{3U, 1U, 2U};
			for (size_t row = 0U; row < order.size(); ++row) {
				std::vector<PropertyValue> expected;
				for (const auto &label : backend->state->nodes.at(order[row]).labels) expected.emplace_back(label);
				EXPECT_EQ(result.getRows()[row].at("labels").asPrimitive(), PropertyValue(expected));
			}
			EXPECT_EQ(backend->state->node_batch_reads - before, 1U);
		}
		ASSERT_FALSE(backend->state->node_batch_sizes.empty());
		EXPECT_EQ(backend->state->node_batch_sizes.front(), 3U);
		for (const auto &shape : backend->state->node_batch_shapes) {
			EXPECT_TRUE(shape.include_labels);
			EXPECT_FALSE(shape.all_properties);
			EXPECT_TRUE(shape.property_tokens.empty());
		}
		EXPECT_EQ(backend->state->property_column_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, MetadataSemanticErrorsPrecedeJointPressureSplitting) {
		for (auto &[id, node] : backend->state->nodes) node.labels = {std::string(1500U, static_cast<char>('a' + id))};
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto labels = std::make_shared<expressions::FunctionCallExpression>("labels", std::move(arguments));
		logical::LogicalProject project(search(), {{labels, "labels"}});
		options.bounds.maximum_materialized_bytes = 6000U;
		backend->state->reverse_node_batch = true;
		try { static_cast<void>(execute(project)); FAIL() << "invalid Node order must fail"; }
		catch (const std::runtime_error &error) { EXPECT_STREQ(error.what(), "logical procedure metadata batch changed Node order"); }
		EXPECT_EQ(backend->state->node_batch_reads, 1U);
		backend->state->reverse_node_batch = false;
		backend->state->nodes.at(3U).labels.push_back(backend->state->nodes.at(3U).labels.front());
		try { static_cast<void>(execute(project)); FAIL() << "invalid labels must fail"; }
		catch (const std::runtime_error &error) { EXPECT_STREQ(error.what(), "logical Vector procedure backend returned an invalid Node label set"); }
		EXPECT_EQ(backend->state->node_batch_reads, 2U);
		backend->state->nodes.at(3U).labels.pop_back();
		// Correct labels still require space together with all retained rows;
		// splitting cannot make an insufficient final row allowance succeed.
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 8000U;
		EXPECT_EQ(execute(project).rowCount(), 3U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, BatchesSelectedProjectionAndRestoresScoreOrderWithMissingFields) {
		backend->state->nodes.at(1U).properties.erase(backend->state->nodes.at(1U).properties.begin());
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(
				std::make_unique<logical::LogicalSort>(search(), std::vector<logical::LogicalSortItem>{{score, true}}),
				{{rank, "rank"}, {rank, "again"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{30}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue());
		EXPECT_EQ(result.getRows()[2].at("rank").asPrimitive(), PropertyValue(int64_t{20}));
		for (const auto &row: result.getRows()) {
			EXPECT_EQ(row.at("rank").asPrimitive(), row.at("again").asPrimitive());
		}
		EXPECT_EQ(backend->state->property_column_reads, 1U);
		ASSERT_EQ(backend->state->property_column_ids.size(), 1U);
		EXPECT_EQ(backend->state->property_column_ids.front(), (std::vector<uint64_t>{1U, 2U, 3U}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, ReusesSortPropertiesForFollowingProjection) {
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(
				std::make_unique<logical::LogicalSort>(search(), std::vector<logical::LogicalSortItem>{{rank, true}}),
				{{rank, "rank"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("rank").asPrimitive(), PropertyValue(static_cast<int64_t>((index + 1U) * 10U)));
		}
		EXPECT_EQ(backend->state->property_column_reads, 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, UsesBoundedTopKForSortSkipLimitPipeline) {
		options.bounds.maximum_rows = 10U;
		for (uint64_t id = 4U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id * 10U))},
					{"tag", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id - 1U)});
		}
		std::vector<PropertyValue> queryCoordinates(500U, PropertyValue(1.0));
		const auto topKSearch = [&queryCoordinates] {
			return std::make_unique<logical::LogicalCallProcedure>("db.index.vector.queryNodes",
					std::vector<PropertyValue>{PropertyValue("memory_embedding"), PropertyValue(int64_t{10}),
							PropertyValue(queryCoordinates)});
		};
		const size_t topKBudget = 1800U +
			backend->state->search_result.hits.capacity() * sizeof(storage::LogicalGraphVectorSearchHit) +
			queryCoordinates.size() * sizeof(float);
		options.bounds.maximum_materialized_bytes = topKBudget;
		backend->state->nodes.at(1U).properties.push_back({"tag", PropertyValue(int64_t{1})});
		backend->state->nodes.at(2U).properties.front().value = PropertyValue(int64_t{10});
		backend->state->nodes.at(2U).properties.push_back({"tag", PropertyValue(int64_t{2})});
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto tag = std::make_shared<expressions::VariableReferenceExpression>("node", "tag");
		logical::LogicalProject project(
				std::make_unique<logical::LogicalLimit>(
						std::make_unique<logical::LogicalSkip>(
								std::make_unique<logical::LogicalSort>(topKSearch(),
								std::vector<logical::LogicalSortItem>{{rank, true}}), 1), 1),
				{{tag, "tag"}});

		const QueryResult result = execute(project);

		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("tag").asPrimitive(), PropertyValue(int64_t{2}));
		// The first source batch fits before the heap fills. With two rows
		// retained, later source reads also cover heap vector growth peaks.
		// Growth drops the unread suffix, which is fetched again without
		// replaying admitted candidates; the last read fetches the retained tag.
		EXPECT_EQ(backend->state->property_column_reads, 10U);
		ASSERT_EQ(backend->state->property_column_ids.size(), 10U);
		EXPECT_EQ(backend->state->property_column_ids.front(), (std::vector<uint64_t>{1U, 2U, 3U}));
		for (size_t index = 1U; index < 9U; ++index) EXPECT_EQ(backend->state->property_column_ids[index].size(), 1U);
		EXPECT_EQ(backend->state->property_column_ids.back(), (std::vector<uint64_t>{2U}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);

		// Without charging the still-live converted query vector, this budget
		// would leave the old top-K allowance plus 500 bytes of false headroom.
		// The shared request/source budget must reject before publication.
		options.bounds.maximum_materialized_bytes = topKBudget - 1500U;
		EXPECT_THROW(static_cast<void>(execute(logical::LogicalLimit(
				std::make_unique<logical::LogicalProject>(
						std::make_unique<logical::LogicalSort>(topKSearch(),
								std::vector<logical::LogicalSortItem>{{rank, true}}),
						std::vector<logical::LogicalProjectItem>{{tag, "tag"}}), 1))), std::runtime_error);

		backend->state->property_column_reads = 0U;
		backend->state->property_column_ids.clear();
		options.bounds.maximum_materialized_bytes = topKBudget;
		logical::LogicalLimit topDescending(
				std::make_unique<logical::LogicalProject>(
						std::make_unique<logical::LogicalSort>(search(10),
								std::vector<logical::LogicalSortItem>{{rank, false}}),
						std::vector<logical::LogicalProjectItem>{{tag, "tag"}}), 1);
		const QueryResult descending = execute(topDescending);
		ASSERT_EQ(descending.rowCount(), 1U);
		EXPECT_EQ(descending.getRows().front().at("tag").asPrimitive(), PropertyValue(int64_t{10}));
	}

	TEST_F(LogicalVectorPropertyBatchTest, TopKResultPublicationSharesHeapRowsAndRetainedResultBudget) {
		for (auto &[id, node]: backend->state->nodes) {
			static_cast<void>(id);
			node.properties.back().value = PropertyValue(std::string(1300U, 'x'));
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		logical::LogicalLimit topTwo(
				std::make_unique<logical::LogicalProject>(
						std::make_unique<logical::LogicalSort>(search(3),
								std::vector<logical::LogicalSortItem>{{rank, true}}),
						std::vector<logical::LogicalProjectItem>{{body, "body"}}),
				2);
		options.bounds.maximum_materialized_bytes = 4000U;
		EXPECT_THROW(static_cast<void>(execute(topTwo)), std::runtime_error);

		options.bounds.maximum_materialized_bytes = 10U * 1024U;
		const QueryResult result = execute(topTwo);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("body").asPrimitive(), PropertyValue(std::string(1300U, 'x')));
		EXPECT_EQ(result.getRows()[1].at("body").asPrimitive(), PropertyValue(std::string(1300U, 'x')));
	}

	TEST_F(LogicalVectorPropertyBatchTest, UsesBoundedTopKWhenFilterPrecedesSkipAndLimitAfterSort) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		options.bounds.maximum_rows = 10U;
		options.bounds.maximum_materialized_bytes = 3000U;
		options.bounds.maximum_property_entries = 1U;
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{5}));
		logical::LogicalProject project(
				std::make_unique<logical::LogicalLimit>(
						std::make_unique<logical::LogicalSkip>(
								std::make_unique<logical::LogicalFilter>(
										std::make_unique<logical::LogicalSort>(search(10),
												std::vector<logical::LogicalSortItem>{{rank, true}}), predicate), 1), 2),
				{{rank, "rank"}});

		const QueryResult result = execute(project);

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{7}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{8}));
		// Heap allocation peaks shrink read-ahead; exact ID preallocation
		// avoids geometric gather slack under this byte bound.
		EXPECT_EQ(backend->state->property_column_reads, 6U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, UsesExpandableTopKWhenFilterFollowsSkipBeforeLimit) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		options.bounds.maximum_rows = 10U;
		options.bounds.maximum_materialized_bytes = 4096U;
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{4}));
		logical::LogicalProject project(
				std::make_unique<logical::LogicalLimit>(
						std::make_unique<logical::LogicalFilter>(
								std::make_unique<logical::LogicalSkip>(
										std::make_unique<logical::LogicalSort>(search(10),
											std::vector<logical::LogicalSortItem>{{rank, true}}), 1), predicate), 2),
					{{rank, "rank"}});

		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 8U * 1024U;
		const QueryResult result = execute(project);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{5}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{6}));
		EXPECT_GE(backend->state->property_column_reads, 3U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);

		backend->state->property_column_reads = 0U;
		const auto projectedPredicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("r"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{4}));
		logical::LogicalProject projectedFilter(
				std::make_unique<logical::LogicalLimit>(
						std::make_unique<logical::LogicalFilter>(
								std::make_unique<logical::LogicalProject>(
										std::make_unique<logical::LogicalSkip>(
												std::make_unique<logical::LogicalSort>(search(10),
													std::vector<logical::LogicalSortItem>{{rank, true}}), 1),
										std::vector<logical::LogicalProjectItem>{{rank, "r"}}), projectedPredicate), 2),
				{{std::make_shared<expressions::VariableReferenceExpression>("r"), "r"}});
		const QueryResult projectedResult = execute(projectedFilter);
		ASSERT_EQ(projectedResult.rowCount(), 2U);
		EXPECT_EQ(projectedResult.getRows()[0].at("r").asPrimitive(), PropertyValue(int64_t{5}));
		EXPECT_EQ(projectedResult.getRows()[1].at("r").asPrimitive(), PropertyValue(int64_t{6}));
		EXPECT_GE(backend->state->property_column_reads, 2U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, UsesBoundedTopKWhenFilterReadsAProjectedAliasBeforeWindow) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		options.bounds.maximum_rows = 10U;
		options.bounds.maximum_materialized_bytes = 2200U;
		options.bounds.maximum_property_entries = 1U;
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("r"),
			expressions::BinaryOperatorType::BOP_GREATER,
			std::make_unique<expressions::LiteralExpression>(int64_t{5}));
		logical::LogicalLimit limit(
			std::make_unique<logical::LogicalSkip>(
				std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalProject>(
						std::make_unique<logical::LogicalSort>(search(10),
							std::vector<logical::LogicalSortItem>{{rank, true}}),
						std::vector<logical::LogicalProjectItem>{{rank, "r"}}), predicate), 1), 2);
		// The old and new heap buffers coexist during growth. The narrow
		// bound must reject that peak even when retained payload alone fits.
		EXPECT_THROW(static_cast<void>(execute(limit)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 4096U;
		backend->state->property_column_reads = 0U;
		const QueryResult result = execute(limit);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("r").asPrimitive(), PropertyValue(int64_t{7}));
		EXPECT_EQ(result.getRows()[1].at("r").asPrimitive(), PropertyValue(int64_t{8}));
		EXPECT_GT(backend->state->property_column_reads, 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, TopKFilterReadsProjectedEntityAliasAfterItsBinding) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		options.bounds.maximum_rows = 10U;
		options.bounds.maximum_materialized_bytes = 2500U;
		options.bounds.maximum_property_entries = 1U;
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto aliasedRank = std::make_shared<expressions::VariableReferenceExpression>("n", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("n", "rank"),
			expressions::BinaryOperatorType::BOP_GREATER,
			std::make_unique<expressions::LiteralExpression>(int64_t{5}));
		logical::LogicalProject output(
			std::make_unique<logical::LogicalLimit>(
				std::make_unique<logical::LogicalSkip>(
					std::make_unique<logical::LogicalFilter>(
						std::make_unique<logical::LogicalProject>(
							std::make_unique<logical::LogicalSort>(search(10),
								std::vector<logical::LogicalSortItem>{{rank, true}}),
							std::vector<logical::LogicalProjectItem>{{
								std::make_shared<expressions::VariableReferenceExpression>("node"), "n"}}), predicate), 1), 2),
			{{aliasedRank, "rank"}});
		const QueryResult result = execute(output);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{7}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{8}));
		EXPECT_GT(backend->state->property_column_reads, 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, TopKPrefetchesFilterPropertyThroughProjectedEntityAlias) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))},
				{"selected", PropertyValue(id % 2U == 1U)}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		options.bounds.maximum_rows = 10U;
		options.bounds.maximum_materialized_bytes = 2800U;
		options.bounds.maximum_property_entries = 2U;
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::VariableReferenceExpression>("n", "selected");
		logical::LogicalProject output(
			std::make_unique<logical::LogicalLimit>(
				std::make_unique<logical::LogicalSkip>(
					std::make_unique<logical::LogicalFilter>(
						std::make_unique<logical::LogicalProject>(
							std::make_unique<logical::LogicalSort>(search(10),
								std::vector<logical::LogicalSortItem>{{rank, true}}),
							std::vector<logical::LogicalProjectItem>{{
								std::make_shared<expressions::VariableReferenceExpression>("node"), "n"}}), predicate), 1), 2),
			{{std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank"}});
		const QueryResult result = execute(output);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{3}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{5}));
		ASSERT_FALSE(backend->state->property_column_keys.empty());
		for (const auto &keys: backend->state->property_column_keys) {
			EXPECT_EQ(keys, (std::vector<std::string>{"rank", "selected"}));
		}
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, TopKPreservesFiltersOnBothSidesOfEntityProjection) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))},
				{"selected", PropertyValue(id % 2U == 1U)}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		options.bounds.maximum_rows = 10U;
		options.bounds.maximum_materialized_bytes = 2800U;
		options.bounds.maximum_property_entries = 2U;
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto sourcePredicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
			expressions::BinaryOperatorType::BOP_GREATER,
			std::make_unique<expressions::LiteralExpression>(int64_t{2}));
		const auto projectedPredicate = std::make_shared<expressions::VariableReferenceExpression>("n", "selected");
		logical::LogicalProject output(
			std::make_unique<logical::LogicalLimit>(
				std::make_unique<logical::LogicalSkip>(
					std::make_unique<logical::LogicalFilter>(
						std::make_unique<logical::LogicalProject>(
							std::make_unique<logical::LogicalFilter>(
								std::make_unique<logical::LogicalSort>(search(10),
									std::vector<logical::LogicalSortItem>{{rank, true}}), sourcePredicate),
							std::vector<logical::LogicalProjectItem>{{
								std::make_shared<expressions::VariableReferenceExpression>("node"), "n"}}),
						projectedPredicate), 1), 2),
			{{std::make_shared<expressions::VariableReferenceExpression>("n", "rank"), "rank"}});
		const QueryResult result = execute(output);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{5}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{7}));
		ASSERT_FALSE(backend->state->property_column_keys.empty());
		for (const auto &keys: backend->state->property_column_keys) {
			EXPECT_EQ(keys, (std::vector<std::string>{"rank", "selected"}));
		}
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, DistinctBeforeLimitKeepsFullSortedCandidateSemantics) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 4U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>((id + 1U) / 2U))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalLimit limit(
			std::make_unique<logical::LogicalProject>(
				std::make_unique<logical::LogicalSort>(search(4),
					std::vector<logical::LogicalSortItem>{{rank, true}}),
				std::vector<logical::LogicalProjectItem>{{rank, "r"}}, true), 2);
		const QueryResult result = execute(limit);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("r").asPrimitive(), PropertyValue(int64_t{1}));
		EXPECT_EQ(result.getRows()[1].at("r").asPrimitive(), PropertyValue(int64_t{2}));
	}

	TEST_F(LogicalVectorPropertyBatchTest, UsesBoundedTopKForSortDistinctSkipLimit) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		options.bounds.maximum_rows = 8U;
		options.bounds.maximum_distinct_rows = 3U;
		options.bounds.maximum_materialized_bytes = 8192U;
		for (uint64_t id = 1U; id <= 8U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>((id + 1U) / 2U))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalLimit projection(
			std::make_unique<logical::LogicalSkip>(
				std::make_unique<logical::LogicalProject>(
					std::make_unique<logical::LogicalSort>(search(8),
						std::vector<logical::LogicalSortItem>{{rank, true}}),
					std::vector<logical::LogicalProjectItem>{{rank, "rank"}}, true), 1), 2);

		const QueryResult result = execute(projection);

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{2}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{3}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
		logical::LogicalLimit descending(
			std::make_unique<logical::LogicalSkip>(
				std::make_unique<logical::LogicalProject>(
					std::make_unique<logical::LogicalSort>(search(8),
						std::vector<logical::LogicalSortItem>{{rank, false}}),
					std::vector<logical::LogicalProjectItem>{{rank, "rank"}}, true), 1), 2);
		const QueryResult descendingResult = execute(descending);
		ASSERT_EQ(descendingResult.rowCount(), 2U);
		EXPECT_EQ(descendingResult.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{3}));
		EXPECT_EQ(descendingResult.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{2}));
		options.bounds.maximum_materialized_bytes = 1536U;
		EXPECT_THROW(static_cast<void>(execute(projection)), std::runtime_error);
	}

	TEST_F(LogicalVectorPropertyBatchTest, UsesBoundedTopKWhenDistinctSortAndProjectionShareAnExpression) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		options.bounds.maximum_rows = 8U;
		// Only the two distinct rows needed by LIMIT may be retained. A full-sort
		// fallback would try to admit all four distinct projected values.
		options.bounds.maximum_distinct_rows = 2U;
		options.bounds.maximum_materialized_bytes = 8192U;
		for (uint64_t id = 1U; id <= 8U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>((id + 1U) / 2U))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const auto rankPlusOne = [] {
			return std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_ADD,
				std::make_unique<expressions::LiteralExpression>(int64_t{1}));
		};
		auto sorted = std::make_unique<logical::LogicalSort>(search(8),
			std::vector<logical::LogicalSortItem>{{rankPlusOne(), true}});
		auto distinct = std::make_unique<logical::LogicalProject>(std::move(sorted),
			std::vector<logical::LogicalProjectItem>{{rankPlusOne(), "rank"}}, true);
		logical::LogicalLimit limit(std::move(distinct), 2);

		const QueryResult result = execute(limit);

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{2}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{3}));
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, DistinctTopKFallsBackWhenSortKeyIsNotProjected) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		options.bounds.maximum_rows = 4U;
		options.bounds.maximum_distinct_rows = 1U;
		for (uint64_t id = 1U; id <= 4U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalLimit limit(
			std::make_unique<logical::LogicalProject>(
				std::make_unique<logical::LogicalSort>(search(4),
					std::vector<logical::LogicalSortItem>{{rank, true}}),
				std::vector<logical::LogicalProjectItem>{{
					std::make_shared<expressions::LiteralExpression>(int64_t{7}), "constant"}}, true), 1);

		const QueryResult result = execute(limit);

		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("constant").asPrimitive(), PropertyValue(int64_t{7}));
	}

	TEST_F(LogicalVectorPropertyBatchTest, ExpandsDistinctTopKForFilterAfterSkip) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		options.bounds.maximum_rows = 10U;
		options.bounds.maximum_distinct_rows = 4U;
		options.bounds.maximum_materialized_bytes = 4096U;
		for (uint64_t id = 1U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>((id + 1U) / 2U))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("rank"),
			expressions::BinaryOperatorType::BOP_GREATER,
			std::make_unique<expressions::LiteralExpression>(int64_t{2}));
		const auto secondPredicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("rank"),
			expressions::BinaryOperatorType::BOP_LESS,
			std::make_unique<expressions::LiteralExpression>(int64_t{5}));
		logical::LogicalLimit limit(
			std::make_unique<logical::LogicalFilter>(
				std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalSkip>(
						std::make_unique<logical::LogicalProject>(
							std::make_unique<logical::LogicalSort>(search(10),
								std::vector<logical::LogicalSortItem>{{rank, true}}),
							std::vector<logical::LogicalProjectItem>{{rank, "rank"}}, true), 1), predicate), secondPredicate), 2);

		const QueryResult result = execute(limit);

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{3}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{4}));
	}

	TEST_F(LogicalVectorPropertyBatchTest, KeepsFilterAfterSkipSemanticsInSortWindowPipeline) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 10U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{5}));
		logical::LogicalProject project(
				std::make_unique<logical::LogicalLimit>(
						std::make_unique<logical::LogicalFilter>(
								std::make_unique<logical::LogicalSkip>(
										std::make_unique<logical::LogicalSort>(search(10),
												std::vector<logical::LogicalSortItem>{{rank, true}}), 1), predicate), 2),
				{{rank, "rank"}});

		const QueryResult result = execute(project);

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{6}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{7}));
	}

	TEST_F(LogicalVectorPropertyBatchTest, RejectsMalformedColumnsAndOversizedSelectedValues) {
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		logical::LogicalProject project(
				std::make_unique<logical::LogicalSort>(search(), std::vector<logical::LogicalSortItem>{{score, true}}),
				{{body, "body"}});
		backend->state->malformed_property_columns = true;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		backend->state->malformed_property_columns = false;
		options.bounds.maximum_materialized_bytes = 2048U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, SelectedSourceRejectsBeforeCopyAndRetriesAfterCancellationOrPressure) {
		backend->state->enable_property_admission = true;
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		logical::LogicalProject project(
			std::make_unique<logical::LogicalSort>(search(), std::vector<logical::LogicalSortItem>{{score, true}}),
			{{body, "body"}});
		options.bounds.maximum_materialized_bytes = 3000U;
		backend->state->malformed_property_columns = true;
		try {
			static_cast<void>(execute(project));
			FAIL() << "malformed source columns must fail";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure backend changed Property batch cardinality");
		}
		EXPECT_EQ(backend->state->property_column_reads, 1U);
		backend->state->malformed_property_columns = false;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		EXPECT_EQ(backend->state->admitted_property_copies, 0U);
		options.bounds.maximum_materialized_bytes = 65536U;
		backend->state->before_property_admission = [] { throw std::domain_error("cancelled source"); };
		const size_t reads = backend->state->property_column_reads;
		EXPECT_THROW(static_cast<void>(execute(project)), std::domain_error);
		EXPECT_EQ(backend->state->property_column_reads, reads + 1U);
		EXPECT_EQ(backend->state->admitted_property_copies, 0U);
		backend->state->before_property_admission = {};
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		for (const auto &row: result.getRows()) EXPECT_EQ(row.at("body").asPrimitive(), PropertyValue(std::string(4096U, 'x')));
		EXPECT_EQ(backend->state->admitted_property_copies, 3U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, SelectedColumnsShareRetainedResultBudgetAndKeepHitOrder) {
		for (const bool admission: {false, true}) {
			backend->state->enable_property_admission = admission;
			for (auto &[id, node]: backend->state->nodes) {
				node.properties.back().value = PropertyValue(std::string(1000U, static_cast<char>('a' + id)));
			}
			const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
			std::vector<std::unique_ptr<expressions::Expression>> sizeArguments;
			sizeArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node", "body"));
			const auto size = std::make_shared<expressions::FunctionCallExpression>("size", std::move(sizeArguments));
			logical::LogicalProject project(
				std::make_unique<logical::LogicalSort>(search(), std::vector<logical::LogicalSortItem>{{score, true}}),
				{{size, "size"}, {score, "score"}});
			// Keep room for three final payloads and one temporary column, but
			// not three temporary slots. Derive the row base from public types
			// so the pressure contract does not depend on the platform ABI.
			const size_t rowBase = sizeof(execution::LogicalPlanReadRow) +
				sizeof(std::unordered_map<uint64_t, PropertyValue>) + 9U +
				2U * sizeof(execution::LogicalPlanReadValue) + sizeof(storage::EntityRef) + sizeof(double);
			options.bounds.maximum_materialized_bytes = 3U * rowBase + 3483U +
				backend->state->search_result.hits.capacity() * sizeof(storage::LogicalGraphVectorSearchHit);
			options.bounds.maximum_materialized_bytes = 3U * rowBase + 3283U +
				backend->state->search_result.hits.capacity() * sizeof(storage::LogicalGraphVectorSearchHit);
			EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
			options.bounds.maximum_materialized_bytes += 500U;
			backend->state->property_column_ids.clear();
			const auto result = execute(project);
			ASSERT_EQ(result.rowCount(), 3U);
			EXPECT_EQ(result.getRows()[0].at("size").asPrimitive(), PropertyValue(int64_t{1000}));
			EXPECT_EQ(result.getRows()[0].at("score").asPrimitive(), PropertyValue(double{0}));
			EXPECT_EQ(result.getRows()[1].at("size").asPrimitive(), PropertyValue(int64_t{1000}));
			EXPECT_EQ(result.getRows()[1].at("score").asPrimitive(), PropertyValue(double{1}));
			EXPECT_EQ(result.getRows()[2].at("size").asPrimitive(), PropertyValue(int64_t{1000}));
			EXPECT_EQ(result.getRows()[2].at("score").asPrimitive(), PropertyValue(double{2}));
			EXPECT_EQ(backend->state->property_column_ids.size(), 1U);
			for (const auto &ids: backend->state->property_column_ids) EXPECT_TRUE(std::ranges::is_sorted(ids));
			backend->state->nodes.at(1U).properties.pop_back();
			const auto missing = execute(project);
			ASSERT_EQ(missing.rowCount(), 3U);
			EXPECT_EQ(missing.getRows()[1].at("size").asPrimitive(), PropertyValue());
			// Restore the fixture for the second capability case.
			backend->state->nodes.at(1U).properties.push_back({"body", PropertyValue(std::string(1000U, 'b'))});
		}
	}

	TEST_F(LogicalVectorPropertyBatchTest, ScalarHeapProjectionSharesRetainedResultBudget) {
		backend->state->enable_property_admission = true;
		for (auto &[id, node]: backend->state->nodes) node.properties.back().value = PropertyValue(std::string(1000U, 'x'));
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		logical::LogicalProject project(
			std::make_unique<logical::LogicalSort>(search(), std::vector<logical::LogicalSortItem>{{score, true}}),
			{{body, "body"}});
		const size_t rowBase = sizeof(execution::LogicalPlanReadRow) +
			sizeof(std::unordered_map<uint64_t, PropertyValue>) + 9U +
			2U * sizeof(execution::LogicalPlanReadValue) + sizeof(storage::EntityRef) + sizeof(double);
		options.bounds.maximum_materialized_bytes = 3U * rowBase + 3283U +
			backend->state->search_result.hits.capacity() * sizeof(storage::LogicalGraphVectorSearchHit);
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 3U * rowBase + 8000U +
			backend->state->search_result.hits.capacity() * sizeof(storage::LogicalGraphVectorSearchHit);
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		for (const auto &row: result.getRows()) EXPECT_EQ(row.at("body").asPrimitive(), PropertyValue(std::string(1000U, 'x')));
		EXPECT_EQ(backend->state->property_column_ids.size(), 4U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, ProjectionAliasesKeepOriginalScopeAndIndependentPayloads) {
		backend->state->enable_property_admission = true;
		backend->state->nodes.at(1U).properties.pop_back();
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(search(), {{body, "node"}, {rank, "rank"}, {body, "copy"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{30}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{10}));
		EXPECT_EQ(result.getRows()[2].at("rank").asPrimitive(), PropertyValue(int64_t{20}));
		EXPECT_EQ(result.getRows()[1].at("node").asPrimitive(), PropertyValue());
		EXPECT_EQ(result.getRows()[1].at("copy").asPrimitive(), PropertyValue());
		backend->state->nodes.at(3U).properties.back().value = PropertyValue("changed");
		EXPECT_EQ(result.getRows()[0].at("node").asPrimitive(), PropertyValue(std::string(4096U, 'x')));
		EXPECT_EQ(result.getRows()[0].at("copy").asPrimitive(), PropertyValue(std::string(4096U, 'x')));
		EXPECT_EQ(backend->state->property_column_reads, 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, FilterRetainsOneRowCacheAcrossSelectedProjectionUnderJointBound) {
		backend->state->enable_property_admission = true;
		for (auto &[id, node]: backend->state->nodes) node.properties.back().value = PropertyValue(std::string(1000U, 'x'));
		std::vector<std::unique_ptr<expressions::Expression>> sizeArguments;
		sizeArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node", "body"));
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::FunctionCallExpression>("size", std::move(sizeArguments)),
			expressions::BinaryOperatorType::BOP_GREATER, std::make_unique<expressions::LiteralExpression>(int64_t{0}));
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(std::make_unique<logical::LogicalFilter>(
			std::make_unique<logical::LogicalSort>(search(), std::vector<logical::LogicalSortItem>{{score, true}}), predicate),
			{{rank, "rank"}});
		const size_t rowBase = sizeof(execution::LogicalPlanReadRow) +
			sizeof(std::unordered_map<uint64_t, PropertyValue>) + 9U +
			2U * sizeof(execution::LogicalPlanReadValue) + sizeof(storage::EntityRef) + sizeof(double);
		// This pipeline retains both body and the subsequent rank field.
		// Admission also holds one encoded entry header before reconciliation.
		options.bounds.maximum_materialized_bytes = 3U * rowBase + 3155U + 3U * (4U + sizeof(int64_t)) +
			sizeof(uint32_t) + storage::CanonicalPropertyValueCodec::HEADER_BYTES +
			128U + backend->state->search_result.hits.capacity() * sizeof(storage::LogicalGraphVectorSearchHit);
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{30}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{10}));
		EXPECT_EQ(result.getRows()[2].at("rank").asPrimitive(), PropertyValue(int64_t{20}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_GT(backend->state->property_column_ids.size(), 2U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, BufferedSortChargesHeapKeysTogetherWithRowsAndCanRetry) {
		backend->state->enable_property_admission = true;
		for (auto &[id, node]: backend->state->nodes) node.properties.back().value = PropertyValue(std::string(1000U, static_cast<char>('a' + id)));
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(std::make_unique<logical::LogicalSort>(search(),
			std::vector<logical::LogicalSortItem>{{body, true}}), {{rank, "rank"}});
		// Rows (~4.5 KiB) and owned string keys (~3 KiB) fit separately.
		// Their simultaneous retention must reject rather than use two allowances.
		options.bounds.maximum_materialized_bytes = 5000U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 12000U;
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{10}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{20}));
		EXPECT_EQ(result.getRows()[2].at("rank").asPrimitive(), PropertyValue(int64_t{30}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, BufferedSortPreservesStableTiesMultiKeyDirectionAndNulls) {
		for (auto &[id, node]: backend->state->nodes) node.properties.back().value = PropertyValue("shared");
		backend->state->nodes.at(2U).properties.front().value = PropertyValue(int64_t{10});
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		logical::LogicalProject project(std::make_unique<logical::LogicalSort>(search(),
			std::vector<logical::LogicalSortItem>{{body, true}, {rank, false}}), {{score, "score"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		// Node 3 is first; equal rank Nodes 1/2 keep their search order.
		for (size_t index = 0U; index < 3U; ++index) {
			EXPECT_EQ(result.getRows()[index].at("score").asPrimitive(), PropertyValue(static_cast<double>(index)));
		}
		// Exceed small-array sort thresholds: equal NULL keys must retain the
		// full original order, not merely happen to survive a three-row sort.
		for (uint64_t id = 64U; id >= 4U; --id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(backend->state->search_result.hits.size())});
		}
		const auto missing = std::make_shared<expressions::VariableReferenceExpression>("node", "missing");
		logical::LogicalProject nullKeys(std::make_unique<logical::LogicalSort>(search(64),
			std::vector<logical::LogicalSortItem>{{missing, false}}), {{score, "score"}});
		const auto nullResult = execute(nullKeys);
		ASSERT_EQ(nullResult.rowCount(), 64U);
		for (size_t index = 0U; index < 64U; ++index) {
			EXPECT_EQ(nullResult.getRows()[index].at("score").asPrimitive(), PropertyValue(static_cast<double>(index)));
		}
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, TopKHeapAndNextHeapPayloadShareAllowanceAndRetryCleanly) {
		backend->state->enable_property_admission = true;
		for (auto &[id, node]: backend->state->nodes) node.properties.back().value = PropertyValue(std::string(1000U, static_cast<char>('a' + id)));
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(std::make_unique<logical::LogicalLimit>(
			std::make_unique<logical::LogicalSort>(search(), std::vector<logical::LogicalSortItem>{{body, true}}), 1),
			{{rank, "rank"}});
		// One retained row/key and the next row/key fit separately, but their
		// overlap exceeds 4 KiB even when only one row wins the final window.
		options.bounds.maximum_materialized_bytes = 4000U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 12000U;
		backend->state->malformed_property_columns = true;
		const size_t reads = backend->state->property_column_reads;
		try { static_cast<void>(execute(project)); FAIL() << "malformed columns must fail"; }
		catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure backend changed Property batch cardinality");
		}
		EXPECT_EQ(backend->state->property_column_reads, reads + 1U);
		backend->state->malformed_property_columns = false;
		backend->state->before_property_admission = [] { throw std::domain_error("cancelled source"); };
		const size_t cancellationReads = backend->state->property_column_reads;
		EXPECT_THROW(static_cast<void>(execute(project)), std::domain_error);
		EXPECT_EQ(backend->state->property_column_reads, cancellationReads + 1U);
		backend->state->before_property_admission = {};
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{10}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, TopKHeapPayloadBackpressurePreservesStableTieWindowAcrossBatches) {
		for (auto &[id, node]: backend->state->nodes) node.properties.back().value = PropertyValue(std::string(1000U, 'x'));
		for (uint64_t id = 4U; id <= 40U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"body", PropertyValue(std::string(1000U, 'x'))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(backend->state->search_result.hits.size())});
		}
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		logical::LogicalProject project(std::make_unique<logical::LogicalLimit>(
			std::make_unique<logical::LogicalSort>(search(40), std::vector<logical::LogicalSortItem>{{body, true}}), 2),
			{{score, "score"}});
		options.bounds.maximum_materialized_bytes = 10000U;
		for (const bool admission: {false, true}) {
			backend->state->enable_property_admission = admission;
			backend->state->property_column_ids.clear();
			const auto result = execute(project);
			ASSERT_EQ(result.rowCount(), 2U);
			EXPECT_EQ(result.getRows()[0].at("score").asPrimitive(), PropertyValue(0.0));
			EXPECT_EQ(result.getRows()[1].at("score").asPrimitive(), PropertyValue(1.0));
			EXPECT_GT(backend->state->property_column_ids.size(), 1U);
			for (const auto &ids: backend->state->property_column_ids) EXPECT_TRUE(std::ranges::is_sorted(ids));
			EXPECT_EQ(backend->state->node_batch_reads, 0U);
		}
	}

	TEST_F(LogicalVectorPropertyBatchTest, TopKComputedKeyReleasesUnreadSuffixAndPreservesWindowOrder) {
		backend->state->enable_property_admission = true;
		for (auto &[id, node]: backend->state->nodes) node.properties.back().value = PropertyValue(std::string(1000U, 'x'));
		for (uint64_t id = 4U; id <= 20U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"body", PropertyValue(std::string(1000U, 'x'))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(backend->state->search_result.hits.size())});
		}
		std::unique_ptr<expressions::Expression> key = std::make_unique<expressions::VariableReferenceExpression>("node", "body");
		for (size_t repeat = 1U; repeat < 10U; ++repeat) {
			key = std::make_unique<expressions::BinaryOpExpression>(std::move(key), expressions::BinaryOperatorType::BOP_ADD,
				std::make_unique<expressions::VariableReferenceExpression>("node", "body"));
		}
		std::shared_ptr<expressions::Expression> sortKey = std::move(key);
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		logical::LogicalProject project(std::make_unique<logical::LogicalLimit>(
			std::make_unique<logical::LogicalSort>(search(20), std::vector<logical::LogicalSortItem>{{sortKey, true}}), 1),
			{{score, "score"}});
		// A computed 10 KiB key is larger than its selected 1 KiB source.
		// After the heap fills, keeping all unread prefetched rows would exceed
		// this allowance. Release their suffix before admitting the next key.
		options.bounds.maximum_materialized_bytes = 30000U;
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows()[0].at("score").asPrimitive(), PropertyValue(0.0));
		EXPECT_GT(backend->state->property_column_ids.size(), 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, CompletePropertiesAndLabelsSplitJointRetentionBeforeCachePublication) {
		for (auto &[id, node]: backend->state->nodes) {
			node.labels = {"Person"};
			node.properties.back().value = PropertyValue(std::string(1000U, 'x'));
		}
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto properties = std::make_shared<expressions::FunctionCallExpression>("properties", std::move(arguments));
		std::vector<std::unique_ptr<expressions::Expression>> labelArguments;
		labelArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto labels = std::make_shared<expressions::FunctionCallExpression>("labels", std::move(labelArguments));
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		logical::LogicalProject project(std::make_unique<logical::LogicalSort>(search(),
			std::vector<logical::LogicalSortItem>{{score, true}}), {{properties, "all"}, {labels, "labels"}});
		options.bounds.maximum_materialized_bytes = 5000U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 9000U;
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		const std::array<int64_t, 3U> ranks{30, 10, 20};
		for (size_t row = 0U; row < 3U; ++row) {
			EXPECT_EQ(result.getRows()[row].at("all").asPrimitive().getMap().at("rank"), PropertyValue(ranks[row]));
			EXPECT_EQ(result.getRows()[row].at("all").asPrimitive().getMap().at("body"), PropertyValue(std::string(1000U, 'x')));
			EXPECT_EQ(result.getRows()[row].at("labels").asPrimitive(), PropertyValue(std::vector<PropertyValue>{PropertyValue("Person")}));
		}
		EXPECT_GT(backend->state->node_batch_reads, 1U);
		ASSERT_FALSE(backend->state->node_batch_sizes.empty());
		EXPECT_EQ(backend->state->node_batch_sizes.front(), 3U);
		EXPECT_EQ(backend->state->property_column_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
		// A fresh execution must reuse no moved payload from the earlier query.
		EXPECT_EQ(execute(project).rowCount(), 3U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, BareNodeOutputSharesRetainedResultBudgetAndPreservesHitOrder) {
		for (auto &[id, node] : backend->state->nodes) {
			node.labels.clear();
			node.properties.back().value = PropertyValue(std::string(1000U, 'x'));
		}
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		const auto node = std::make_shared<expressions::VariableReferenceExpression>("node");
		logical::LogicalProject project(std::make_unique<logical::LogicalSort>(search(),
			std::vector<logical::LogicalSortItem>{{score, true}}), {{node, "node"}, {score, "score"}});
		const size_t rowBase = sizeof(execution::LogicalPlanReadRow) + sizeof(std::unordered_map<uint64_t, PropertyValue>) +
			9U + 2U * sizeof(execution::LogicalPlanReadValue) + sizeof(storage::EntityRef) + sizeof(double);
		// Room for all final Node bindings plus one source frame and routing
		// scratch and the reference retained until that source returns; three
		// source frames alongside the same rows cannot fit.
		const size_t nodePayload = 2U * sizeof(storage::LogicalGraphPropertyEntry) + 1024U;
		options.bounds.maximum_materialized_bytes = 3U * (rowBase - sizeof(storage::EntityRef) + nodePayload) +
		3U * (2U * sizeof(uint64_t) + 2U * sizeof(size_t)) + sizeof(storage::LogicalGraphNode) + sizeof(storage::EntityRef) + 128U +
			backend->state->search_result.hits.capacity() * sizeof(storage::LogicalGraphVectorSearchHit) + 5000U;
		const size_t sufficientBytes = options.bounds.maximum_materialized_bytes;
		options.bounds.maximum_materialized_bytes -= 4000U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = sufficientBytes;
		for (int retry = 0; retry < 2; ++retry) {
			const size_t before = backend->state->node_batch_reads;
			const auto result = execute(project);
			ASSERT_EQ(result.rowCount(), 3U);
			const std::array<uint64_t, 3U> order{3U, 1U, 2U};
			for (size_t row = 0U; row < order.size(); ++row) {
				ASSERT_TRUE(result.getRows()[row].contains("node")) << "Node output column missing";
				EXPECT_EQ(result.getRows()[row].at("node").asNode().getId(), order[row]);
				ASSERT_TRUE(result.getRows()[row].at("node").asNode().getProperties().contains("body")) << "Node " << order[row] << " properties missing";
				EXPECT_EQ(result.getRows()[row].at("node").asNode().getProperties().at("body"), PropertyValue(std::string(1000U, 'x')));
			}
			EXPECT_EQ(backend->state->node_batch_reads - before, 1U);
		}
		EXPECT_EQ(backend->state->node_batch_sizes.front(), 3U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, BareNodeAliasesAdmitSourceAndIndependentCopiesBeforePublishing) {
		backend->state->enable_shaped_property_admission = true;
		backend->state->search_result.hits = {{1U, 0.0F}};
		const auto node = std::make_shared<expressions::VariableReferenceExpression>("node");
		logical::LogicalProject project(search(), {{node, "left"}, {node, "right"}});
		backend->state->nodes.at(1U).properties.back().value = PropertyValue(std::string(10000U, 'x'));
		options.bounds.maximum_materialized_bytes = 5000U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		backend->state->nodes.at(1U).properties.back().value = PropertyValue(std::string(1000U, 'x'));
		options.bounds.maximum_materialized_bytes = 2200U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 5000U;
		for (int retry = 0; retry < 2; ++retry) {
			const auto result = execute(project);
			ASSERT_EQ(result.rowCount(), 1U);
			const auto &leftBody = std::get<std::string>(result.getRows()[0].at("left").asNode().getProperties().at("body").getVariant());
			const auto &rightBody = std::get<std::string>(result.getRows()[0].at("right").asNode().getProperties().at("body").getVariant());
			// The last alias transfers the owned source allocation all the way
			// into ResultValue; the earlier alias keeps its independent copy.
			EXPECT_EQ(reinterpret_cast<uintptr_t>(rightBody.data()), backend->state->last_node_body_addresses.at(1U));
			EXPECT_NE(leftBody.data(), rightBody.data());
			for (const auto &alias : {"left", "right"}) {
				EXPECT_EQ(result.getRows()[0].at(alias).asNode().getId(), 1U);
				EXPECT_EQ(result.getRows()[0].at(alias).asNode().getProperties().at("body"), PropertyValue(std::string(1000U, 'x')));
			}
		}
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, CompleteSourceAdmissionRejectsBeforeCopyAndDoesNotRetrySourceErrors) {
		backend->state->enable_shaped_property_admission = true;
		backend->state->search_result.hits = {{1U, 0.0F}};
		backend->state->nodes.at(1U).properties.back().value = PropertyValue(std::string(10000U, 'x'));
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto properties = std::make_shared<expressions::FunctionCallExpression>("properties", std::move(arguments));
		logical::LogicalProject project(search(), {{properties, "all"}});
		options.bounds.maximum_materialized_bytes = 5000U;
		for (int retry = 0; retry < 2; ++retry) {
			EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
			EXPECT_EQ(backend->state->node_batch_reads, 0U);
			EXPECT_EQ(backend->state->shaped_admission_reads, static_cast<size_t>(retry + 1));
		}
		backend->state->nodes.at(1U).properties.back().value = PropertyValue(std::string(1000U, 'x'));
		backend->state->before_shaped_property_admission = [] { throw std::domain_error("complete source interrupted"); };
		const size_t beforeError = backend->state->shaped_admission_reads;
		try {
			static_cast<void>(execute(project));
			FAIL() << "source interruption must propagate";
		} catch (const std::domain_error &error) {
			EXPECT_STREQ(error.what(), "complete source interrupted");
		}
		EXPECT_EQ(backend->state->shaped_admission_reads, beforeError + 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		backend->state->before_shaped_property_admission = {};
		for (int retry = 0; retry < 2; ++retry) {
			const auto result = execute(project);
			ASSERT_EQ(result.rowCount(), 1U);
			EXPECT_EQ(result.getRows()[0].at("all").asPrimitive().getMap().at("body"), PropertyValue(std::string(1000U, 'x')));
		}
		EXPECT_EQ(backend->state->node_batch_reads, 2U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);

	}

	TEST_F(LogicalVectorPropertyBatchTest, CompleteSemanticErrorsPrecedeJointPressureRetry) {
		for (auto &[id, node]: backend->state->nodes) node.properties.back().value = PropertyValue(std::string(1000U, 'x'));
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto properties = std::make_shared<expressions::FunctionCallExpression>("properties", std::move(arguments));
		logical::LogicalProject project(search(), {{properties, "all"}});
		options.bounds.maximum_materialized_bytes = 5000U;
		backend->state->reverse_node_batch = true;
		try { static_cast<void>(execute(project)); FAIL() << "bad source order must fail"; }
		catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure complete batch changed Node order");
		}
		EXPECT_EQ(backend->state->node_batch_reads, 1U);
		backend->state->reverse_node_batch = false;
		backend->state->nodes.at(3U).properties.push_back({"rank", PropertyValue(int64_t{30})});
		try { static_cast<void>(execute(project)); FAIL() << "bad Property keys must fail"; }
		catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical procedure complete batch has invalid Property keys");
		}
		EXPECT_EQ(backend->state->node_batch_reads, 2U);
		backend->state->nodes.at(3U).properties.pop_back();
		options.bounds.maximum_materialized_bytes = 8192U;
		EXPECT_EQ(execute(project).rowCount(), 3U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, BatchesFilterAndReusesSurvivingFields) {
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{10}));
		logical::LogicalProject project(
				std::make_unique<logical::LogicalSort>(
						std::make_unique<logical::LogicalFilter>(search(), predicate),
						std::vector<logical::LogicalSortItem>{{score, true}}),
				{{rank, "rank"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{30}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{20}));
		EXPECT_EQ(backend->state->property_column_reads, 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, SelectedCacheDoesNotReplaceCompleteMapOrNodeOutput) {
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto missing = std::make_shared<expressions::VariableReferenceExpression>("node", "missing");
		const auto node = std::make_shared<expressions::VariableReferenceExpression>("node");
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto properties = std::make_shared<expressions::FunctionCallExpression>("properties", std::move(arguments));
		logical::LogicalProject project(
				std::make_unique<logical::LogicalSort>(search(), std::vector<logical::LogicalSortItem>{{score, true}}),
				{{rank, "rank"}, {missing, "missing"}, {properties, "all"}, {node, "node"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		for (const auto &row: result.getRows()) {
			EXPECT_TRUE(row.at("all").asPrimitive().getMap().contains("rank"));
			EXPECT_FALSE(row.at("all").asPrimitive().getMap().contains("missing"));
			EXPECT_EQ(row.at("all").asPrimitive().getMap().at("body"), PropertyValue(std::string(4096U, 'x')));
			EXPECT_FALSE(row.at("node").asLogicalNode().properties.contains("missing"));
			EXPECT_EQ(row.at("node").asLogicalNode().properties.at("body"), PropertyValue(std::string(4096U, 'x')));
		}
		EXPECT_EQ(backend->state->property_column_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
		EXPECT_EQ(backend->state->node_batch_reads, 2U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, BatchesCompleteExpressionsForBufferedAndStreamingProjection) {
		for (const bool sorted: {false, true}) {
			for (const std::string function: {"properties", "keys", "map"}) {
				SCOPED_TRACE(sorted);
				SCOPED_TRACE(function);
				backend->state->node_batch_reads = 0U;
				backend->state->node_batch_sizes.clear();
				backend->state->node_point_reads = 0U;
				backend->state->property_column_reads = 0U;
				std::shared_ptr<expressions::Expression> complete;
				if (function == "map") {
					std::vector<expressions::MapProjectionItem> items;
					items.emplace_back(expressions::MapProjectionItemType::MPROP_ALL_PROPERTIES, "");
					complete = std::make_shared<expressions::MapProjectionExpression>("node", std::move(items));
				} else {
					std::vector<std::unique_ptr<expressions::Expression>> arguments;
					arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
					complete = std::make_shared<expressions::FunctionCallExpression>(function, std::move(arguments));
				}
				std::unique_ptr<logical::LogicalOperator> child = search();
				if (sorted) child = std::make_unique<logical::LogicalSort>(std::move(child),
					std::vector<logical::LogicalSortItem>{{std::make_shared<expressions::VariableReferenceExpression>("score"), true}});
				logical::LogicalProject project(std::move(child), {{complete, "all"},
					{std::make_shared<expressions::VariableReferenceExpression>("node", "rank"), "rank"},
					{std::make_shared<expressions::VariableReferenceExpression>("node", "missing"), "missing"}});
				const auto result = execute(project);
				ASSERT_EQ(result.rowCount(), 3U);
				EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(30));
				EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(10));
				EXPECT_EQ(result.getRows()[2].at("rank").asPrimitive(), PropertyValue(20));
				for (const auto &row: result.getRows()) {
					EXPECT_EQ(row.at("missing").asPrimitive(), PropertyValue());
					const auto value = row.at("all").asPrimitive();
					if (function == "keys") {
						EXPECT_EQ(value.getList().size(), 2U);
					} else {
						EXPECT_EQ(value.getMap().size(), 2U);
						EXPECT_EQ(value.getMap().at("body"), PropertyValue(std::string(4096U, 'x')));
					}
				}
				EXPECT_EQ(backend->state->node_batch_reads, 1U);
				EXPECT_EQ(backend->state->node_batch_sizes, (std::vector<size_t>{3U}));
				EXPECT_EQ(backend->state->node_point_reads, 0U);
				EXPECT_EQ(backend->state->property_column_reads, 0U);
			}
		}
	}

	TEST_F(LogicalVectorPropertyBatchTest, CompleteBatchReplacesEarlierSelectedMissingFieldCache) {
		const auto node = std::make_shared<expressions::VariableReferenceExpression>("node");
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("entity"));
		const auto properties = std::make_shared<expressions::FunctionCallExpression>("properties", std::move(arguments));
		logical::LogicalProject project(std::make_unique<logical::LogicalProject>(search(),
			std::vector<logical::LogicalProjectItem>{{node, "entity"},
				{std::make_shared<expressions::VariableReferenceExpression>("node", "missing"), "missing"}}),
			{{properties, "all"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		for (const auto &row: result.getRows()) {
			EXPECT_FALSE(row.at("all").asPrimitive().getMap().contains("missing"));
			EXPECT_TRUE(row.at("all").asPrimitive().getMap().contains("body"));
		}
		EXPECT_EQ(backend->state->property_column_reads, 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 1U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, CompleteBatchCacheSurvivesFilterSortAndProjection) {
		const auto countKeys = [] {
			std::vector<std::unique_ptr<expressions::Expression>> keysArguments;
			keysArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
			std::vector<std::unique_ptr<expressions::Expression>> sizeArguments;
			sizeArguments.push_back(std::make_unique<expressions::FunctionCallExpression>("keys", std::move(keysArguments)));
			return std::make_unique<expressions::FunctionCallExpression>("size", std::move(sizeArguments));
		};
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(countKeys(),
			expressions::BinaryOperatorType::BOP_GREATER, std::make_unique<expressions::LiteralExpression>(int64_t{1}));
		std::shared_ptr<expressions::Expression> sortKey = countKeys();
		std::vector<std::unique_ptr<expressions::Expression>> propertiesArguments;
		propertiesArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto properties = std::make_shared<expressions::FunctionCallExpression>("properties", std::move(propertiesArguments));
		logical::LogicalProject project(std::make_unique<logical::LogicalSort>(
			std::make_unique<logical::LogicalFilter>(search(), predicate),
			std::vector<logical::LogicalSortItem>{{sortKey, true}}), {{properties, "all"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("all").asPrimitive().getMap().at("rank"), PropertyValue(30));
		EXPECT_EQ(result.getRows()[1].at("all").asPrimitive().getMap().at("rank"), PropertyValue(10));
		EXPECT_EQ(result.getRows()[2].at("all").asPrimitive().getMap().at("rank"), PropertyValue(20));
		EXPECT_EQ(backend->state->node_batch_reads, 1U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
		EXPECT_EQ(backend->state->property_column_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, CompleteEntityGatherAndRowsShareBudgetAcrossBoundedBatches) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 300U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({id, static_cast<float>(id)});
		}
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto keys = std::make_shared<expressions::FunctionCallExpression>("keys", std::move(arguments));
		logical::LogicalProject project(search(300), {{keys, "keys"}});
		options.bounds.maximum_materialized_bytes = 256U * 1024U;

		const QueryResult result = execute(project);

		ASSERT_EQ(result.rowCount(), 300U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			const auto &keysValue = result.getRows()[index].at("keys").asPrimitive().getList();
			ASSERT_EQ(keysValue.size(), 1U);
			EXPECT_EQ(keysValue.front(), PropertyValue("rank"));
		}
		EXPECT_EQ(backend->state->node_batch_reads, 2U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);

		backend->state->node_batch_reads = 0U;
		std::vector<std::unique_ptr<expressions::Expression>> labelArguments;
		labelArguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto labels = std::make_shared<expressions::FunctionCallExpression>("labels", std::move(labelArguments));
		logical::LogicalProject labelsProject(search(300), {{labels, "labels"}});
		const QueryResult labelsResult = execute(labelsProject);
		ASSERT_EQ(labelsResult.rowCount(), 300U);
		for (const auto &row: labelsResult.getRows()) {
			EXPECT_TRUE(row.at("labels").asPrimitive().getList().empty());
		}
		EXPECT_EQ(backend->state->node_batch_reads, 2U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, MaterializesRepeatedNodeOutputsWithCapacityAccountedGatherBuffers) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 300U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"body", PropertyValue(std::string(128U, static_cast<char>('a' + id % 26U)))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({301U - id, static_cast<float>(id)});
		}
		const auto node = std::make_shared<expressions::VariableReferenceExpression>("node");
		logical::LogicalProject project(search(300), {{node, "left"}, {node, "right"}});

		const QueryResult result = execute(project);

		ASSERT_EQ(result.rowCount(), 300U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			const uint64_t expectedId = 300U - index;
			const auto &left = result.getRows()[index].at("left").asNode();
			const auto &right = result.getRows()[index].at("right").asNode();
			EXPECT_EQ(left.getId(), expectedId);
			EXPECT_EQ(right.getId(), expectedId);
			const auto &leftBody = std::get<std::string>(left.getProperties().at("body").getVariant());
			const auto &rightBody = std::get<std::string>(right.getProperties().at("body").getVariant());
			EXPECT_EQ(leftBody, rightBody);
			EXPECT_NE(leftBody.data(), rightBody.data());
		}
		EXPECT_EQ(backend->state->node_batch_reads, 2U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, ProcedureProjectionReadsOnlySelectedColumnsMissingFromFilterCache) {
		backend->state->enable_property_admission = true;
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node", "body"));
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::FunctionCallExpression>("size", std::move(arguments)),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{0}));
		const auto body = std::make_shared<expressions::VariableReferenceExpression>("node", "body");
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(
				std::make_unique<logical::LogicalFilter>(search(), predicate), {{body, "body"}, {rank, "rank"}});

		const QueryResult result = execute(project);

		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{30}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{10}));
		EXPECT_EQ(result.getRows()[2].at("rank").asPrimitive(), PropertyValue(int64_t{20}));
		EXPECT_EQ(backend->state->property_column_reads, 2U);
		EXPECT_EQ(backend->state->property_column_keys,
				(std::vector<std::vector<std::string>>{{"body"}, {"rank"}}));
	}


	TEST_F(LogicalVectorPropertyBatchTest, CompleteBatchRejectsInvalidPropertiesAndResourceBounds) {
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("node"));
		const auto properties = std::make_shared<expressions::FunctionCallExpression>("properties", std::move(arguments));
		logical::LogicalProject project(search(), {{properties, "all"}});
		options.bounds.maximum_node_records = 2U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_node_records = 16U;
		options.bounds.maximum_property_entries = 1U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_property_entries = 4U;
		options.bounds.maximum_materialized_bytes = 2048U;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		options.bounds.maximum_materialized_bytes = 65536U;
		backend->state->malformed_node_batch_cardinality = true;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		backend->state->malformed_node_batch_cardinality = false;
		backend->state->reverse_node_batch = true;
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
		backend->state->reverse_node_batch = false;
		backend->state->nodes.at(1U).properties.push_back({"rank", PropertyValue(11)});
		EXPECT_THROW(static_cast<void>(execute(project)), std::runtime_error);
	}

	TEST_F(LogicalVectorPropertyBatchTest, CarriesSelectedFieldsAcrossEntityRefRenaming) {
		const auto node = std::make_shared<expressions::VariableReferenceExpression>("node");
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto renamedRank = std::make_shared<expressions::VariableReferenceExpression>("entity", "rank");
		logical::LogicalProject project(
				std::make_unique<logical::LogicalProject>(search(),
						std::vector<logical::LogicalProjectItem>{{node, "entity"}, {rank, "first"}}),
				std::vector<logical::LogicalProjectItem>{{renamedRank, "rank"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{30}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{10}));
		EXPECT_EQ(result.getRows()[2].at("rank").asPrimitive(), PropertyValue(int64_t{20}));
		EXPECT_EQ(backend->state->property_column_reads, 1U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, StreamsSelectedProjectionAcrossBoundedHitBatches) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 300U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))},
					{"body", PropertyValue(std::string(4096U, 'x'))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({301U - id, static_cast<float>(id)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(search(300), {{rank, "rank"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 300U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("rank").asPrimitive(),
					PropertyValue(static_cast<int64_t>(300U - index)));
		}
		EXPECT_EQ(backend->state->property_column_reads, 2U);
		for (const auto &ids: backend->state->property_column_ids) {
			EXPECT_LE(ids.size(), 256U);
			EXPECT_TRUE(std::ranges::is_sorted(ids));
		}
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, StreamsFilterInBoundedBatchesAndReusesSurvivingProperties) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 300U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({301U - id, static_cast<float>(id)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{298}));
		logical::LogicalProject project(
				std::make_unique<logical::LogicalFilter>(search(300), predicate), {{rank, "rank"}});
		const auto result = execute(project);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{300}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{299}));
		EXPECT_EQ(backend->state->property_column_reads, 2U);
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, BatchesOnlyPrefixNeededForProjectSkipLimit) {
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalLimit limit(std::make_unique<logical::LogicalSkip>(
				std::make_unique<logical::LogicalProject>(search(),
						std::vector<logical::LogicalProjectItem>{{rank, "rank"}}), 1), 1);
		const QueryResult result = execute(limit);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("rank").asPrimitive(), PropertyValue(int64_t{10}));
		EXPECT_EQ(backend->state->property_column_reads, 1U);
		ASSERT_EQ(backend->state->property_column_ids.size(), 1U);
		EXPECT_EQ(backend->state->property_column_ids.front(), (std::vector<uint64_t>{1U, 3U}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, PreservesDistinctProjectionStateAcrossHitBatches) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 300U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			const uint64_t rank = id == 44U ? 300U : id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(rank))}};
			backend->state->nodes.emplace(id, std::move(node));
			backend->state->search_result.hits.push_back({301U - id, static_cast<float>(id - 1U)});
		}
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(search(300), std::vector<logical::LogicalProjectItem>{{rank, "rank"}}, true);
		const QueryResult result = execute(project);
		ASSERT_EQ(result.rowCount(), 299U);
		EXPECT_EQ(result.getRows().front().at("rank").asPrimitive(), PropertyValue(int64_t{300}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{299}));
		EXPECT_EQ(std::ranges::count_if(result.getRows(), [](const QueryResult::Row &row) {
			return row.at("rank").asPrimitive() == PropertyValue(int64_t{300});
		}), 1);
		EXPECT_EQ(backend->state->property_column_reads, 2U);
		ASSERT_EQ(backend->state->property_column_ids.size(), 2U);
		EXPECT_EQ(backend->state->property_column_ids.front().size(), 256U);
		EXPECT_EQ(backend->state->property_column_ids.back().size(), 44U);
		for (const auto &ids: backend->state->property_column_ids) {
			EXPECT_TRUE(std::ranges::is_sorted(ids));
		}
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, SkipAndLimitBeforeProjectReadOnlyRetainedRows) {
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		logical::LogicalProject project(
				std::make_unique<logical::LogicalLimit>(std::make_unique<logical::LogicalSkip>(search(), 1), 1),
				std::vector<logical::LogicalProjectItem>{{rank, "rank"}});
		const QueryResult result = execute(project);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("rank").asPrimitive(), PropertyValue(int64_t{10}));
		EXPECT_EQ(backend->state->property_column_reads, 1U);
		ASSERT_EQ(backend->state->property_column_ids.size(), 1U);
		EXPECT_EQ(backend->state->property_column_ids.front(), (std::vector<uint64_t>{1U}));
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, BatchesFilterDistinctSkipLimitAcrossHitBoundaryAndStopsAtLimit) {
		backend->state->nodes.clear();
		backend->state->search_result.hits.clear();
		for (uint64_t id = 1U; id <= 600U; ++id) {
			storage::LogicalGraphNode node;
			node.node_id = id;
			node.properties = {{"rank", PropertyValue(static_cast<int64_t>(id))}};
			backend->state->nodes.emplace(id, std::move(node));
			// Search order descends by node ID while retaining a valid increasing score.
			backend->state->search_result.hits.push_back({601U - id, static_cast<float>(id)});
		}
		// Repeat the first result's key on the first hit of batch two. DISTINCT
		// must retain its key set while SKIP and LIMIT retain their own counters.
		backend->state->nodes.at(344U).properties.front().value = PropertyValue(int64_t{600});
		const auto rank = std::make_shared<expressions::VariableReferenceExpression>("node", "rank");
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{40}));
		logical::LogicalLimit limit(std::make_unique<logical::LogicalSkip>(
				std::make_unique<logical::LogicalProject>(
						std::make_unique<logical::LogicalFilter>(search(600), predicate),
						std::vector<logical::LogicalProjectItem>{{rank, "rank"}}, true), 257), 2);
		const QueryResult result = execute(limit);

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("rank").asPrimitive(), PropertyValue(int64_t{342}));
		EXPECT_EQ(result.getRows()[1].at("rank").asPrimitive(), PropertyValue(int64_t{341}));
		EXPECT_EQ(backend->state->property_column_reads, 2U);
		ASSERT_EQ(backend->state->property_column_ids.size(), 2U);
		EXPECT_EQ(backend->state->property_column_ids[0].size(), 256U);
		EXPECT_EQ(backend->state->property_column_ids[1].size(), 256U);
		for (const auto &ids: backend->state->property_column_ids) {
			EXPECT_LE(ids.size(), 256U);
			EXPECT_TRUE(std::ranges::is_sorted(ids));
		}
		EXPECT_EQ(backend->state->node_batch_reads, 0U);
		EXPECT_EQ(backend->state->node_point_reads, 0U);
	}

	TEST_F(LogicalVectorPropertyBatchTest, ValidatesNegativeWindowEvenWhenSearchReturnsNoHits) {
		backend->state->search_result.hits.clear();
		const auto predicate = std::make_shared<expressions::BinaryOpExpression>(
				std::make_unique<expressions::VariableReferenceExpression>("node", "rank"),
				expressions::BinaryOperatorType::BOP_GREATER,
				std::make_unique<expressions::LiteralExpression>(int64_t{0}));
		logical::LogicalSkip skip(std::make_unique<logical::LogicalFilter>(search(), predicate), -1);
		EXPECT_THROW(static_cast<void>(execute(skip)), std::invalid_argument);
		logical::LogicalLimit limit(std::make_unique<logical::LogicalFilter>(search(), predicate), -1);
		EXPECT_THROW(static_cast<void>(execute(limit)), std::invalid_argument);
	}

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

} // namespace graph::query
