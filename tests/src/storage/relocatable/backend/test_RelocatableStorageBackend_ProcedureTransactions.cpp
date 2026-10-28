/**
 * @file test_RelocatableStorageBackend_ProcedureTransactions.cpp
 * @brief Native procedure visibility across logical transaction boundaries.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <memory>
#include <string>
#include <string_view>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <variant>

#include "graph/core/Transaction.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/gds/LogicalProjectionCatalog.hpp"
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

using namespace graph::query;
using namespace graph::storage;
using namespace relocatable_storage_backend_test;

namespace {

	std::atomic<size_t> inTransactionsDelayedPublicationCalls{0U};
	std::atomic<QueryGuard *> inTransactionsDelayedGuard{nullptr};

	[[nodiscard]] PropertyValue delayedProcedurePublicationResult(
			[[maybe_unused]] const std::vector<PropertyValue> &arguments,
			[[maybe_unused]] const graph::query::expressions::EvaluationContext &context) {
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		return PropertyValue(int64_t{1});
	}

	void registerDelayedProcedurePublicationResult() {
		graph::query::expressions::FunctionRegistry::getInstance().registerFunction(
				std::make_unique<graph::query::expressions::LambdaScalarFunction>(
						graph::query::expressions::FunctionSignature{"testProcedurePublicationDelay", 0U, 0U},
						&delayedProcedurePublicationResult));
	}

	[[nodiscard]] PropertyValue delayOnlyTheSecondInTransactionsPublication(
			[[maybe_unused]] const std::vector<PropertyValue> &arguments,
			[[maybe_unused]] const graph::query::expressions::EvaluationContext &context) {
		if (inTransactionsDelayedPublicationCalls.fetch_add(1U, std::memory_order_relaxed) != 0U) {
			QueryGuard *guard = inTransactionsDelayedGuard.load(std::memory_order_acquire);
			if (guard == nullptr) throw std::logic_error("transaction timeout test has no guard");
			// Start the deadline only once the second batch body is evaluated.
			// The first one-row transaction has already returned from publication.
			*guard = QueryGuard(1, 0);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return PropertyValue(int64_t{1});
	}

	void registerSecondInTransactionsPublicationDelay() {
		graph::query::expressions::FunctionRegistry::getInstance().registerFunction(
				std::make_unique<graph::query::expressions::LambdaScalarFunction>(
						graph::query::expressions::FunctionSignature{
								"testSecondInTransactionsPublicationDelay", 0U, 0U},
						&delayOnlyTheSecondInTransactionsPublication));
	}

	[[nodiscard]] std::string shortestPathQuery(uint64_t sourceNodeId, uint64_t targetNodeId) {
		return "CALL algo.shortestPath(" + std::to_string(sourceNodeId) + ", " + std::to_string(targetNodeId) + ", 2)";
	}

	[[nodiscard]] std::vector<uint64_t> stagePath(LogicalGraphTransactionOverlay &overlay) {
		const std::vector<uint64_t> nodes =
				overlay.createLogicalNodes({{{"ProcedureOverlayPath"}, {}}, {{"ProcedureOverlayPath"}, {}}});
		if (nodes.size() != 2U) {
			throw std::logic_error("native transaction overlay did not allocate two path Nodes");
		}
		const std::vector<uint64_t> edges =
				overlay.createLogicalEdges({{nodes[0U], nodes[1U], "PROCEDURE_OVERLAY_LINK", {}}});
		if (edges.size() != 1U) {
			throw std::logic_error("native transaction overlay did not allocate one path relationship");
		}
		return nodes;
	}

} // namespace

TEST_F(RelocatableStorageBackendTest, ShortestPathProcedureReadsStagedOverlayAndRollbackDoesNotLeakIt) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	QueryEngine engine(backend, backend);
	std::unique_ptr<LogicalGraphTransaction> transaction = backend->beginLogicalGraphTransaction(false);
	LogicalGraphTransactionOverlay overlay(*transaction);
	const std::vector<uint64_t> nodes = stagePath(overlay);
	const uint64_t sourceNodeId = nodes[0U];
	const uint64_t targetNodeId = nodes[1U];
	const std::string pathQuery = shortestPathQuery(sourceNodeId, targetNodeId);

	const QueryResult inTransaction = engine.executePlanWithLogicalTransactionOverlay(
			engine.buildPlan(pathQuery), QueryContext{}, overlay, overlay);
	ASSERT_EQ(inTransaction.rowCount(), 2U);
	EXPECT_EQ(inTransaction.getRows().front().at("node").asLogicalNode().node_id, sourceNodeId);
	EXPECT_EQ(inTransaction.getRows().back().at("node").asLogicalNode().node_id, targetNodeId);

	// The database-owned engine remains an autocommit reader while this write
	// transaction is active, so it must not observe the provisional path.
	EXPECT_TRUE(engine.execute(pathQuery).isEmpty());
	transaction->rollback();

	EXPECT_TRUE(engine.execute(pathQuery).isEmpty());
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, DbmsProceduresRejectTransactionOverlaysBeforeAnySideEffect) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	QueryEngine engine(backend, backend);
	std::unique_ptr<LogicalGraphTransaction> transaction = backend->beginLogicalGraphTransaction(false);
	LogicalGraphTransactionOverlay overlay(*transaction);

	EXPECT_THROW(static_cast<void>(engine.executePlanWithLogicalTransactionOverlay(
				engine.buildPlan("CALL dbms.listConfig()"), QueryContext{}, overlay, overlay)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(engine.executePlanWithLogicalTransactionOverlay(
				engine.buildPlan("CALL dbms.setConfig('query.timeout_ms', 45)"), QueryContext{}, overlay, overlay)),
			 std::runtime_error);
	EXPECT_EQ(backend->findConfiguration("query.timeout_ms"), std::nullopt);
	transaction->rollback();
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, InTransactionsSubqueryRejectsTransactionOverlayBeforeAnyIndependentCommit) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	QueryEngine engine(backend, backend);
	std::unique_ptr<LogicalGraphTransaction> transaction = backend->beginLogicalGraphTransaction(false);
	LogicalGraphTransactionOverlay overlay(*transaction);

	// IN TRANSACTIONS owns independent commit boundaries. Letting it run through a
	// caller-owned overlay would make part of that overlay durable before its owner
	// commits, so the runtime must reject it before opening or publishing a batch.
	EXPECT_THROW(static_cast<void>(engine.executePlanWithLogicalTransactionOverlay(
				 engine.buildPlan("CALL { CREATE (:NestedBatchPublication) } IN TRANSACTIONS OF 1 ROWS"),
				 QueryContext{}, overlay, overlay)),
			 std::runtime_error);

	EXPECT_TRUE(engine.execute("MATCH (:NestedBatchPublication) RETURN 1").isEmpty());
	transaction->rollback();
	EXPECT_TRUE(engine.execute("MATCH (:NestedBatchPublication) RETURN 1").isEmpty());
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TimedOutDbmsProcedureDoesNotPublishConfiguration) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	QueryEngine engine(backend, backend);

	// QueryGuard checks periodically. Prime it immediately before the deadline,
	// then expire it so the procedure entry checkpoint must reject before set().
	QueryGuard guard(1, 0);
	for (size_t index = 0U; index < 1023U; ++index) {
		guard.check();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	QueryContext context;
	context.guard = &guard;
	EXPECT_THROW(static_cast<void>(engine.executePlan(
				 engine.buildPlan("CALL dbms.setConfig('query.timeout_ms', 45)"), context)),
			 QueryTimeoutException);
	EXPECT_EQ(backend->findConfiguration("query.timeout_ms"), std::nullopt);

	backend->close();
}

TEST_F(RelocatableStorageBackendTest, FailedDbmsResultPipelineDoesNotPublishConfiguration) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	QueryEngine engine(backend, backend);

	// Result operators run before an administrative mutation is published. A
	// deterministic RETURN failure must leave the durable configuration absent.
	EXPECT_THROW(static_cast<void>(engine.execute(
				 "CALL dbms.setConfig('query.timeout_ms', 45) YIELD status RETURN 1 / 0 AS invalidResult")),
			 std::runtime_error);
	EXPECT_EQ(backend->findConfiguration("query.timeout_ms"), std::nullopt);

	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TimedOutDbmsResultPipelineDoesNotPublishConfiguration) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	registerDelayedProcedurePublicationResult();

	{
		QueryEngine engine(backend, backend);
		QueryGuard guard(100, 0);
		QueryContext context;
		context.guard = &guard;
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CALL dbms.setConfig('query.timeout_ms', 45) YIELD status "
					 "RETURN testProcedurePublicationDelay() AS delayed", context)),
				 QueryTimeoutException);
		EXPECT_EQ(backend->findConfiguration("query.timeout_ms"), std::nullopt);
	}
	backend->close();
	backend.reset();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.findConfiguration("query.timeout_ms"), std::nullopt);
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, FailedDbmsResetStatsResultPipelinePreservesExistingRuntimeStatistics) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	QueryEngine engine(backend, backend);

	static_cast<void>(engine.execute("MATCH (node) RETURN count(node) AS count"));
	const auto queryCompletedCount = [&engine] {
		const QueryResult statistics = engine.execute("CALL dbms.showStats()");
		for (const QueryResult::Row &row: statistics.getRows()) {
			if (row.at("category").asPrimitive() == PropertyValue("query") &&
				row.at("metric").asPrimitive() == PropertyValue("completed")) {
				return std::get<int64_t>(row.at("value").asPrimitive().getVariant());
			}
		}
		throw std::runtime_error("native statistics did not expose the query completed counter");
	};
	const int64_t before = queryCompletedCount();
	ASSERT_GT(before, 0);

	// A result-pipeline failure occurs before the deferred reset. resetStats and
	// showStats are excluded from query statistics finalization, so this observes
	// the administration side effect itself rather than the failed query.
	EXPECT_THROW(static_cast<void>(engine.execute(
				 "CALL dbms.resetStats() YIELD result RETURN 1 / 0 AS invalidResult")),
			 std::runtime_error);
	EXPECT_EQ(queryCompletedCount(), before);

	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TimedOutDbmsResetStatsResultPipelinePreservesExistingRuntimeStatistics) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	registerDelayedProcedurePublicationResult();
	QueryEngine engine(backend, backend);

	// Establish a count that cannot be reproduced merely by finalizing the one
	// timed-out administration query after an erroneous reset.
	for (size_t query = 0U; query < 3U; ++query) {
		static_cast<void>(engine.execute("MATCH (node) RETURN count(node) AS count"));
	}
	const auto queryCompletedCount = [&engine] {
		const QueryResult statistics = engine.execute("CALL dbms.showStats()");
		for (const QueryResult::Row &row: statistics.getRows()) {
			if (row.at("category").asPrimitive() == PropertyValue("query") &&
				row.at("metric").asPrimitive() == PropertyValue("completed")) {
				return std::get<int64_t>(row.at("value").asPrimitive().getVariant());
			}
		}
		throw std::runtime_error("native statistics did not expose the query completed counter");
	};
	const int64_t before = queryCompletedCount();
	ASSERT_GE(before, 3);

	QueryGuard guard(100, 0);
	QueryContext context;
	context.guard = &guard;
	EXPECT_THROW(static_cast<void>(engine.execute(
				 "CALL dbms.resetStats() YIELD result "
				 "RETURN testProcedurePublicationDelay() AS delayed", context)),
			 QueryTimeoutException);
	EXPECT_GE(queryCompletedCount(), before);

	backend->close();
}

TEST_F(RelocatableStorageBackendTest, FailedDbmsClearSlowQueriesResultPipelinePreservesExistingRecords) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	QueryEngine engine(backend, backend);

	static_cast<void>(engine.execute("CALL dbms.setConfig('query.slow_log.enabled', true)"));
	static_cast<void>(engine.execute("CALL dbms.setConfig('query.slow_log.threshold_ms', 0)"));
	static_cast<void>(engine.execute("MATCH (node) RETURN count(node) AS count"));
	const QueryResult before = engine.execute("CALL dbms.listSlowQueries()");
	ASSERT_FALSE(before.isEmpty());

	// The clear procedure is also excluded from slow-query recording, so the
	// before/after comparison is not polluted by the intentionally failed call.
	EXPECT_THROW(static_cast<void>(engine.execute(
				 "CALL dbms.clearSlowQueries() YIELD result RETURN 1 / 0 AS invalidResult")),
			 std::runtime_error);
	const QueryResult after = engine.execute("CALL dbms.listSlowQueries()");
	ASSERT_EQ(after.getColumns(), before.getColumns());
	ASSERT_EQ(after.rowCount(), before.rowCount());
	for (size_t index = 0U; index < before.rowCount(); ++index) {
		for (const std::string_view column: {"sequence", "logicalPlan", "elapsedMs", "succeeded"}) {
			EXPECT_EQ(after.getRows()[index].at(std::string(column)).asPrimitive(),
					before.getRows()[index].at(std::string(column)).asPrimitive());
		}
	}

	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TimedOutDbmsClearSlowQueriesResultPipelinePreservesExistingRecords) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	registerDelayedProcedurePublicationResult();
	QueryEngine engine(backend, backend);

	static_cast<void>(engine.execute("CALL dbms.setConfig('query.slow_log.enabled', true)"));
	static_cast<void>(engine.execute("CALL dbms.setConfig('query.slow_log.threshold_ms', 0)"));
	static_cast<void>(engine.execute("MATCH (node) RETURN count(node) AS count"));
	const QueryResult before = engine.execute("CALL dbms.listSlowQueries()");
	ASSERT_FALSE(before.isEmpty());

	QueryGuard guard(100, 0);
	QueryContext context;
	context.guard = &guard;
	EXPECT_THROW(static_cast<void>(engine.execute(
				 "CALL dbms.clearSlowQueries() YIELD result "
				 "RETURN testProcedurePublicationDelay() AS delayed", context)),
			 QueryTimeoutException);
	const QueryResult after = engine.execute("CALL dbms.listSlowQueries()");
	ASSERT_EQ(after.getColumns(), before.getColumns());
	ASSERT_EQ(after.rowCount(), before.rowCount());
	for (size_t index = 0U; index < before.rowCount(); ++index) {
		for (const std::string_view column: {"sequence", "logicalPlan", "elapsedMs", "succeeded"}) {
			EXPECT_EQ(after.getRows()[index].at(std::string(column)).asPrimitive(),
					before.getRows()[index].at(std::string(column)).asPrimitive());
		}
	}

	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TimedOutGdsCatalogResultPipelinesDoNotPublishMutations) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	static_cast<void>(backend->createLogicalNodes({{{"ProcedureProjectionNode"}, {}}}));
	registerDelayedProcedurePublicationResult();
	QueryEngine engine(backend, backend);

	const auto executeAfterResultPipelineTimeout = [&engine](const std::string_view query) {
		QueryGuard guard(100, 0);
		QueryContext context;
		context.guard = &guard;
		EXPECT_THROW(static_cast<void>(engine.execute(std::string(query), context)), QueryTimeoutException);
	};
	const auto projectionExists = [&engine](const std::string_view name) {
		const QueryResult exists = engine.execute("CALL gds.graph.exists('" + std::string(name) + "')");
		if (exists.rowCount() != 1U) {
			throw std::runtime_error("native GDS catalog existence procedure returned an invalid result shape");
		}
		return exists.getRows().front().at("exists").asPrimitive() == PropertyValue(true);
	};

	// GDS Catalogs are deliberately process-local, so the observable durable
	// contract here is atomic visibility in this Database-owned catalog rather
	// than reopen persistence. Each timeout happens after a prepared mutation and
	// result materialization, immediately before its one visibility publication.
	executeAfterResultPipelineTimeout(
			"CALL gds.graph.project('timed_catalog_projection', {}) YIELD name "
			"RETURN testProcedurePublicationDelay() AS delayed");
	EXPECT_FALSE(projectionExists("timed_catalog_projection"));

	static_cast<void>(engine.execute("CALL gds.graph.project('timed_catalog_projection', {})"));
	ASSERT_TRUE(projectionExists("timed_catalog_projection"));
	executeAfterResultPipelineTimeout(
			"CALL gds.graph.drop('timed_catalog_projection') YIELD name "
			"RETURN testProcedurePublicationDelay() AS delayed");
	EXPECT_TRUE(projectionExists("timed_catalog_projection"));

	executeAfterResultPipelineTimeout(
			"CALL gds.graph.dropAll() YIELD droppedCount "
			"RETURN testProcedurePublicationDelay() AS delayed");
	EXPECT_TRUE(projectionExists("timed_catalog_projection"));

	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TimedOutGraphMutationResultPipelinesDoNotPublishBatches) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	registerDelayedProcedurePublicationResult();
	const std::vector<uint64_t> createdNodes = backend->createLogicalNodes(
			{{{"TimedOutResultSet"}, {{"value", PropertyValue(int64_t{1})}}},
			 {{"TimedOutResultRemove"}, {{"value", PropertyValue(int64_t{1})}}},
			 {{"TimedOutResultDelete"}, {{"value", PropertyValue(int64_t{1})}}},
			 {{"TimedOutResultDetachTarget"}, {{"value", PropertyValue(int64_t{1})}}},
			 {{"TimedOutResultDetachNeighbor"}, {{"value", PropertyValue(int64_t{2})}}}});
	ASSERT_EQ(createdNodes.size(), 5U);
	const uint64_t detachTarget = createdNodes[3U];
	const uint64_t detachNeighbor = createdNodes[4U];
	static_cast<void>(backend->createLogicalEdges(
			{{detachTarget, detachNeighbor, "TIMED_OUT_RESULT_DETACH_EDGE", {}}}));
	QueryEngine engine(backend, backend);

	const auto executeAfterResultPipelineTimeout = [&engine](const std::string_view query) {
		QueryGuard guard(100, 0);
		QueryContext context;
		context.guard = &guard;
		EXPECT_THROW(static_cast<void>(engine.execute(std::string(query), context)), QueryTimeoutException);
	};

	// Each statement has completely staged a native mutation batch before this
	// scalar expression crosses the deadline. The immediate guard at the batch
	// boundary must prevent every type of graph mutation from becoming durable.
	executeAfterResultPipelineTimeout(
			"CREATE (:TimedOutResultCreate {value: 1}) "
			"RETURN testProcedurePublicationDelay() AS delayed");
	executeAfterResultPipelineTimeout(
			"MATCH (node:TimedOutResultSet) SET node.value = 2 "
			"RETURN testProcedurePublicationDelay() AS delayed");
	executeAfterResultPipelineTimeout(
			"MATCH (node:TimedOutResultRemove) REMOVE node.value, node:TimedOutResultRemove "
			"RETURN testProcedurePublicationDelay() AS delayed");
	executeAfterResultPipelineTimeout(
			"MATCH (node:TimedOutResultDelete) DELETE node "
			"RETURN testProcedurePublicationDelay() AS delayed");
	executeAfterResultPipelineTimeout(
			"MATCH (node:TimedOutResultDetachTarget) DETACH DELETE node "
			"RETURN testProcedurePublicationDelay() AS delayed");
	executeAfterResultPipelineTimeout(
			"MERGE (:TimedOutResultMerge {value: 1}) "
			"RETURN testProcedurePublicationDelay() AS delayed");

	EXPECT_TRUE(engine.execute("MATCH (node:TimedOutResultCreate) RETURN node").isEmpty());
	EXPECT_TRUE(engine.execute("MATCH (node:TimedOutResultMerge) RETURN node").isEmpty());
	const QueryResult unchanged = engine.execute("MATCH (node:TimedOutResultSet) RETURN node.value AS value");
	ASSERT_EQ(unchanged.rowCount(), 1U);
	EXPECT_EQ(unchanged.getRows().front().at("value").asPrimitive(), PropertyValue(int64_t{1}));
	const QueryResult unchangedRemove =
			engine.execute("MATCH (node:TimedOutResultRemove) RETURN node.value AS value");
	ASSERT_EQ(unchangedRemove.rowCount(), 1U);
	EXPECT_EQ(unchangedRemove.getRows().front().at("value").asPrimitive(), PropertyValue(int64_t{1}));
	EXPECT_EQ(engine.execute("MATCH (node:TimedOutResultDelete) RETURN node").rowCount(), 1U);
	EXPECT_EQ(engine.execute("MATCH (:TimedOutResultDetachTarget)-[edge:TIMED_OUT_RESULT_DETACH_EDGE]->"
						   "(:TimedOutResultDetachNeighbor) RETURN edge").rowCount(),
			  1U);
	backend->close();
	backend.reset();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto reopened = std::make_shared<RelocatableStorageBackend>(std::move(reopenOptions));
	reopened->open();
	QueryEngine reopenedEngine(reopened, reopened);
	EXPECT_TRUE(reopenedEngine.execute("MATCH (node:TimedOutResultCreate) RETURN node").isEmpty());
	EXPECT_TRUE(reopenedEngine.execute("MATCH (node:TimedOutResultMerge) RETURN node").isEmpty());
	const QueryResult reopenedSet = reopenedEngine.execute("MATCH (node:TimedOutResultSet) RETURN node.value AS value");
	ASSERT_EQ(reopenedSet.rowCount(), 1U);
	EXPECT_EQ(reopenedSet.getRows().front().at("value").asPrimitive(), PropertyValue(int64_t{1}));
	const QueryResult reopenedRemove =
			reopenedEngine.execute("MATCH (node:TimedOutResultRemove) RETURN node.value AS value");
	ASSERT_EQ(reopenedRemove.rowCount(), 1U);
	EXPECT_EQ(reopenedRemove.getRows().front().at("value").asPrimitive(), PropertyValue(int64_t{1}));
	EXPECT_EQ(reopenedEngine.execute("MATCH (node:TimedOutResultDelete) RETURN node").rowCount(), 1U);
	EXPECT_EQ(reopenedEngine.execute("MATCH (:TimedOutResultDetachTarget)-[edge:TIMED_OUT_RESULT_DETACH_EDGE]->"
								   "(:TimedOutResultDetachNeighbor) RETURN edge").rowCount(),
			  1U);
	reopened->close();
}

TEST_F(RelocatableStorageBackendTest, FailedRemoveAndDetachDeleteResultPipelinesDoNotPublishBatches) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	const std::vector<uint64_t> nodes = backend->createLogicalNodes(
			{{{"FailedResultRemove"}, {{"value", PropertyValue(int64_t{1})}}},
			 {{"FailedResultDetachTarget"}, {}},
			 {{"FailedResultDetachNeighbor"}, {}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend->createLogicalEdges(
			{{nodes[1U], nodes[2U], "FAILED_RESULT_DETACH_EDGE", {}}}));

	{
		QueryEngine engine(backend, backend);

		// Result materialization is deliberately preflighted before a mutation is
		// allowed to publish. These failures occur after both mutation shapes have
		// completed their read/staging work but before a backend write is issued.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (node:FailedResultRemove) REMOVE node.value, node:FailedResultRemove "
					 "RETURN 1 / 0 AS invalidResult")),
				 std::runtime_error);
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (node:FailedResultDetachTarget) DETACH DELETE node "
					 "RETURN 1 / 0 AS invalidResult")),
				 std::runtime_error);

		const QueryResult unchangedRemove = engine.execute("MATCH (node:FailedResultRemove) RETURN node.value AS value");
		ASSERT_EQ(unchangedRemove.rowCount(), 1U);
		EXPECT_EQ(unchangedRemove.getRows().front().at("value").asPrimitive(), PropertyValue(int64_t{1}));
		EXPECT_EQ(engine.execute("MATCH (:FailedResultDetachTarget)-[edge:FAILED_RESULT_DETACH_EDGE]->"
								   "(:FailedResultDetachNeighbor) RETURN edge").rowCount(),
			  1U);
	}
	backend->close();
	backend.reset();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto reopened = std::make_shared<RelocatableStorageBackend>(std::move(reopenOptions));
	reopened->open();
	QueryEngine reopenedEngine(reopened, reopened);
	const QueryResult reopenedRemove =
			reopenedEngine.execute("MATCH (node:FailedResultRemove) RETURN node.value AS value");
	ASSERT_EQ(reopenedRemove.rowCount(), 1U);
	EXPECT_EQ(reopenedRemove.getRows().front().at("value").asPrimitive(), PropertyValue(int64_t{1}));
	EXPECT_EQ(reopenedEngine.execute("MATCH (:FailedResultDetachTarget)-[edge:FAILED_RESULT_DETACH_EDGE]->"
								   "(:FailedResultDetachNeighbor) RETURN edge").rowCount(),
			  1U);
	reopened->close();
}

TEST_F(RelocatableStorageBackendTest, FailedForeachAndWriteOnlyCallResultPipelinesDoNotPublishBatches) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();

	{
		QueryEngine engine(backend, backend);

		// Both bodies stage their complete graph batch before the outer RETURN is
		// preflighted. A deterministic result failure must abandon the staged work.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "FOREACH (value IN [1] | CREATE (:FailedForeachResult)) "
					 "RETURN 1 / 0 AS invalidResult")),
				 std::runtime_error);
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CALL { CREATE (:FailedWriteOnlyCallResult) } "
					 "RETURN 1 / 0 AS invalidResult")),
				 std::runtime_error);
		EXPECT_TRUE(engine.execute("MATCH (:FailedForeachResult) RETURN 1").isEmpty());
		EXPECT_TRUE(engine.execute("MATCH (:FailedWriteOnlyCallResult) RETURN 1").isEmpty());
	}
	backend->close();
	backend.reset();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto reopened = std::make_shared<RelocatableStorageBackend>(std::move(reopenOptions));
	reopened->open();
	QueryEngine reopenedEngine(reopened, reopened);
	EXPECT_TRUE(reopenedEngine.execute("MATCH (:FailedForeachResult) RETURN 1").isEmpty());
	EXPECT_TRUE(reopenedEngine.execute("MATCH (:FailedWriteOnlyCallResult) RETURN 1").isEmpty());
	reopened->close();
}

TEST_F(RelocatableStorageBackendTest, TimedOutForeachDoesNotPublishItsStagedBatch) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	registerDelayedProcedurePublicationResult();

	{
		QueryEngine engine(backend, backend);
		QueryGuard guard(100, 0);
		QueryContext context;
		context.guard = &guard;

		// The body has staged a complete create batch when its delayed Property
		// expression returns. The following immediate publication guard must reject
		// that expired request before the FOREACH batch reaches the backend.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "FOREACH (value IN [1] | "
					 "CREATE (:TimedOutForeachResult {value: testProcedurePublicationDelay()}))",
					 context)),
				 QueryTimeoutException);
		EXPECT_TRUE(engine.execute("MATCH (:TimedOutForeachResult) RETURN 1").isEmpty());
	}
	backend->close();
	backend.reset();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto reopened = std::make_shared<RelocatableStorageBackend>(std::move(reopenOptions));
	reopened->open();
	QueryEngine reopenedEngine(reopened, reopened);
	EXPECT_TRUE(reopenedEngine.execute("MATCH (:TimedOutForeachResult) RETURN 1").isEmpty());
	reopened->close();
}

TEST_F(RelocatableStorageBackendTest, TimedOutWriteOnlyCallDoesNotPublishItsStagedBatch) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	registerDelayedProcedurePublicationResult();

	{
		QueryEngine engine(backend, backend);
		QueryGuard guard(100, 0);
		QueryContext context;
		context.guard = &guard;

		// A write-only CALL rebinds its body into the shared create/set planner.
		// The delayed value has been evaluated and the batch is complete before
		// the immediate guard decides whether that batch may become durable.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CALL { CREATE (:TimedOutWriteOnlyCall {value: testProcedurePublicationDelay()}) }",
					 context)),
				 QueryTimeoutException);
		EXPECT_TRUE(engine.execute("MATCH (:TimedOutWriteOnlyCall) RETURN 1").isEmpty());
	}
	backend->close();
	backend.reset();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto reopened = std::make_shared<RelocatableStorageBackend>(std::move(reopenOptions));
	reopened->open();
	QueryEngine reopenedEngine(reopened, reopened);
	EXPECT_TRUE(reopenedEngine.execute("MATCH (:TimedOutWriteOnlyCall) RETURN 1").isEmpty());
	reopened->close();
}

TEST_F(RelocatableStorageBackendTest, TimedOutInTransactionsCallDoesNotPublishItsCurrentBatch) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	registerDelayedProcedurePublicationResult();
	static_cast<void>(backend->createLogicalNodes({{{"TimedOutBatchSource"}, {}}}));

	{
		QueryEngine engine(backend, backend);
		QueryGuard guard(100, 0);
		QueryContext context;
		context.guard = &guard;

		// Every IN TRANSACTIONS batch owns an independent publication boundary.
		// The current batch is fully staged after the delayed Property evaluation,
		// but its immediate guard must reject publication before that independent
		// commit becomes visible.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (source:TimedOutBatchSource) "
					 "CALL { WITH source CREATE (:TimedOutBatchResult "
					 "{value: testProcedurePublicationDelay()}) } IN TRANSACTIONS OF 1 ROWS",
					 context)),
				 QueryTimeoutException);
		EXPECT_TRUE(engine.execute("MATCH (:TimedOutBatchResult) RETURN 1").isEmpty());
	}
	backend->close();
	backend.reset();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto reopened = std::make_shared<RelocatableStorageBackend>(std::move(reopenOptions));
	reopened->open();
	QueryEngine reopenedEngine(reopened, reopened);
	EXPECT_TRUE(reopenedEngine.execute("MATCH (:TimedOutBatchResult) RETURN 1").isEmpty());
	reopened->close();
}

TEST_F(RelocatableStorageBackendTest, TimedOutInTransactionsCallKeepsEarlierCommittedBatchDurable) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	inTransactionsDelayedPublicationCalls.store(0U, std::memory_order_relaxed);
	registerSecondInTransactionsPublicationDelay();
	static_cast<void>(backend->createLogicalNodes(
			{{{"TimedOutLaterBatchSeed"}, {{"value", PropertyValue(int64_t{1})}}},
			 { {"TimedOutLaterBatchSeed"}, {{"value", PropertyValue(int64_t{2})}}}}));

	{
		QueryEngine engine(backend, backend);
		QueryGuard guard(0, 0);
		QueryContext context;
		context.guard = &guard;
		inTransactionsDelayedGuard.store(&guard, std::memory_order_release);
		struct ResetGuard {
			~ResetGuard() { inTransactionsDelayedGuard.store(nullptr, std::memory_order_release); }
		} resetGuard;

		// The first batch completes with no deadline. The second evaluation starts
		// an expired deadline before its publication, after the first batch committed.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (seed:TimedOutLaterBatchSeed) "
					 "CALL { WITH seed CREATE (:TimedOutLaterBatch "
					 "{copied: seed.value, delayed: testSecondInTransactionsPublicationDelay()}) } "
					 "IN TRANSACTIONS OF 1 ROWS",
					 context)),
				 QueryTimeoutException);
		EXPECT_EQ(inTransactionsDelayedPublicationCalls.load(std::memory_order_relaxed), 2U);
		const QueryResult committed = engine.execute("MATCH (node:TimedOutLaterBatch) RETURN node.copied AS copied");
		ASSERT_EQ(committed.rowCount(), 1U);
		EXPECT_EQ(committed.getRows().front().at("copied").asPrimitive(), PropertyValue(int64_t{1}));
	}
	backend->close();
	backend.reset();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto reopened = std::make_shared<RelocatableStorageBackend>(std::move(reopenOptions));
	reopened->open();
	QueryEngine reopenedEngine(reopened, reopened);
	const QueryResult reopenedCommitted =
			reopenedEngine.execute("MATCH (node:TimedOutLaterBatch) RETURN node.copied AS copied");
	ASSERT_EQ(reopenedCommitted.rowCount(), 1U);
	EXPECT_EQ(reopenedCommitted.getRows().front().at("copied").asPrimitive(), PropertyValue(int64_t{1}));
	reopened->close();
}

TEST_F(RelocatableStorageBackendTest, InTransactionsKeepsCompletedBatchesWhenALaterBatchBodyFails) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	static_cast<void>(backend->createLogicalNodes(
			{{{"InTransactionsFailureSeed"}, {{"value", PropertyValue(int64_t{1})}}},
			 { {"InTransactionsFailureSeed"}, {{"value", PropertyValue(int64_t{2})}}},
			 { {"InTransactionsFailureSeed"}, {{"value", PropertyValue(int64_t{0})}}}}));

	{
		QueryEngine engine(backend, backend);
		// The first two rows form one independently committed batch. Division by
		// zero happens while planning the second batch, so that batch must not
		// become visible and the already committed batch must remain durable.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (seed:InTransactionsFailureSeed) "
					 "CALL { WITH seed CREATE (:InTransactionsCompletedBatch "
					 "{copied: 12 / seed.value}) } IN TRANSACTIONS OF 2 ROWS")),
				 std::runtime_error);

		const QueryResult completed = engine.execute(
				"MATCH (node:InTransactionsCompletedBatch) RETURN node.copied AS copied");
		ASSERT_EQ(completed.rowCount(), 2U);
		std::vector<PropertyValue> values;
		values.reserve(completed.rowCount());
		for (const QueryResult::Row &row: completed.getRows()) {
			values.push_back(row.at("copied").asPrimitive());
		}
		std::ranges::sort(values);
		EXPECT_EQ(values, (std::vector<PropertyValue>{PropertyValue(6.0), PropertyValue(12.0)}));
	}
	backend->close();
	backend.reset();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto reopened = std::make_shared<RelocatableStorageBackend>(std::move(reopenOptions));
	reopened->open();
	QueryEngine reopenedEngine(reopened, reopened);
	const QueryResult reopenedCompleted = reopenedEngine.execute(
			"MATCH (node:InTransactionsCompletedBatch) RETURN node.copied AS copied");
	ASSERT_EQ(reopenedCompleted.rowCount(), 2U);
	EXPECT_TRUE(reopenedEngine.execute("MATCH (:InTransactionsCompletedBatch {copied: 0}) RETURN 1").isEmpty());
	reopened->close();
}

TEST_F(RelocatableStorageBackendTest, InTransactionsPreflightsOuterResultFailureBeforeAnyBatchCommits) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	static_cast<void>(backend->createLogicalNodes(
			{{{"InTransactionsProjectionSeed"}, {{"value", PropertyValue(int64_t{1})}}},
			 { {"InTransactionsProjectionSeed"}, {{"value", PropertyValue(int64_t{2})}}}}));

	{
		QueryEngine engine(backend, backend);
		// The outer result belongs to the whole statement, not an individual
		// batch. Its deterministic failure must be discovered while preflighting
		// the materialized input, before the first independent commit.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (seed:InTransactionsProjectionSeed) "
					 "CALL { WITH seed CREATE (:InTransactionsProjectionFailure "
					 "{copied: seed.value}) } IN TRANSACTIONS OF 1 ROW "
					 "RETURN 1 / 0 AS invalidResult")),
				 std::runtime_error);
		EXPECT_TRUE(engine.execute("MATCH (:InTransactionsProjectionFailure) RETURN 1").isEmpty());
	}
	backend->close();
	backend.reset();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto reopened = std::make_shared<RelocatableStorageBackend>(std::move(reopenOptions));
	reopened->open();
	QueryEngine reopenedEngine(reopened, reopened);
	EXPECT_TRUE(reopenedEngine.execute("MATCH (:InTransactionsProjectionFailure) RETURN 1").isEmpty());
	reopened->close();
}

TEST_F(RelocatableStorageBackendTest, FailedVectorTrainingResultPipelineDoesNotPublishDerivedGeneration) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	static_cast<void>(backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(0.0)})}}},
			 {{"Embedding"}, {{"value", PropertyValue(std::vector<PropertyValue>{PropertyValue(0.0), PropertyValue(1.0)})}}}}));
	const LogicalGraphIndexDefinition index =
			backend->createLogicalNodeVectorIndex({"result_pipeline_embedding", "Embedding", "value", 2U, "l2"});
	QueryEngine engine(backend, backend);

	EXPECT_THROW(static_cast<void>(engine.execute(
				 "CALL db.index.vector.train('result_pipeline_embedding') YIELD status RETURN 1 / 0 AS invalidResult")),
			 std::runtime_error);

	// If the failed result pipeline had published, this retry would recognize the
	// existing derived generation. A false value proves the first durable publish
	// happens only after result materialization succeeds.
	const LogicalGraphVectorTrainingResult retried = backend->trainLogicalVector({index.name, 16U, 16U});
	EXPECT_TRUE(retried.trained);
	EXPECT_FALSE(retried.reused_existing_generation);
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TimedOutVectorTrainingResultPipelineDoesNotPublishDerivedGeneration) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	static_cast<void>(backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(0.0)})}}},
			 {{"Embedding"}, {{"value", PropertyValue(std::vector<PropertyValue>{PropertyValue(0.0), PropertyValue(1.0)})}}}}));
	const LogicalGraphIndexDefinition index =
			backend->createLogicalNodeVectorIndex({"timed_result_pipeline_embedding", "Embedding", "value", 2U, "l2"});
	registerDelayedProcedurePublicationResult();

	{
		QueryEngine engine(backend, backend);
		QueryGuard guard(100, 0);
		QueryContext context;
		context.guard = &guard;
		// Preparation succeeds, but the scalar result pipeline crosses the deadline
		// before the one derived-generation publication. The retry below must be the
		// first durable training, including after reopening the single database file.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CALL db.index.vector.train('timed_result_pipeline_embedding') YIELD status "
					 "RETURN testProcedurePublicationDelay() AS delayed", context)),
				 QueryTimeoutException);
	}
	backend->close();
	backend.reset();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	const LogicalGraphVectorTrainingResult retried = reopened.trainLogicalVector({index.name, 16U, 16U});
	EXPECT_TRUE(retried.trained);
	EXPECT_FALSE(retried.reused_existing_generation);
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, PreparedVectorTrainingPublishesOnlyOnOneExplicitCommit) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	static_cast<void>(backend->createLogicalNodes(
			{{{"Embedding"}, {{"value", PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(0.0)})}}},
			 {{"Embedding"}, {{"value", PropertyValue(std::vector<PropertyValue>{PropertyValue(0.0), PropertyValue(1.0)})}}}}));
	const LogicalGraphIndexDefinition index =
			backend->createLogicalNodeVectorIndex({"prepared_embedding", "Embedding", "value", 2U, "l2"});

	{
		std::unique_ptr<PreparedLogicalGraphVectorTraining> abandoned =
				backend->prepareLogicalVectorTraining({index.name, 16U, 16U});
		ASSERT_NE(abandoned, nullptr);
		EXPECT_TRUE(abandoned->preview().will_train);
		// Scope exit releases the writer fence without publishing derived state.
	}

	std::unique_ptr<PreparedLogicalGraphVectorTraining> prepared =
			backend->prepareLogicalVectorTraining({index.name, 16U, 16U});
	ASSERT_NE(prepared, nullptr);
	EXPECT_TRUE(prepared->preview().will_train);
	const LogicalGraphVectorTrainingResult committed = prepared->commit();
	EXPECT_TRUE(committed.trained);
	EXPECT_FALSE(committed.reused_existing_generation);
	EXPECT_THROW(static_cast<void>(prepared->commit()), std::logic_error);
	prepared.reset();

	std::unique_ptr<PreparedLogicalGraphVectorTraining> retried =
			backend->prepareLogicalVectorTraining({index.name, 16U, 16U});
	ASSERT_NE(retried, nullptr);
	EXPECT_TRUE(retried->preview().will_train);
	const LogicalGraphVectorTrainingResult repeated = retried->commit();
	EXPECT_TRUE(repeated.trained);
	EXPECT_TRUE(repeated.reused_existing_generation);
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TimedOutResetStatsLeavesExistingRuntimeStatisticsIntact) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	QueryEngine engine(backend, backend);

	static_cast<void>(engine.execute("MATCH (node) RETURN count(node) AS count"));
	const auto queryCompletedCount = [&engine] {
		const QueryResult statistics = engine.execute("CALL dbms.showStats()");
		for (const QueryResult::Row &row: statistics.getRows()) {
			if (row.at("category").asPrimitive() == PropertyValue("query") &&
				row.at("metric").asPrimitive() == PropertyValue("completed")) {
				return std::get<int64_t>(row.at("value").asPrimitive().getVariant());
			}
		}
		throw std::runtime_error("native statistics did not expose the query completed counter");
	};
	const int64_t before = queryCompletedCount();
	ASSERT_GT(before, 0);

	QueryGuard guard(1, 0);
	for (size_t index = 0U; index < 1023U; ++index) {
		guard.check();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	QueryContext context;
	context.guard = &guard;
	EXPECT_THROW(static_cast<void>(engine.executePlan(engine.buildPlan("CALL dbms.resetStats()"), context)),
			 QueryTimeoutException);
	EXPECT_EQ(queryCompletedCount(), before);

	backend->close();
}

TEST_F(RelocatableStorageBackendTest, TimedOutClearSlowQueriesLeavesExistingRecordsIntact) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	QueryEngine engine(backend, backend);

	static_cast<void>(engine.execute("CALL dbms.setConfig('query.slow_log.enabled', true)"));
	static_cast<void>(engine.execute("CALL dbms.setConfig('query.slow_log.threshold_ms', 0)"));
	static_cast<void>(engine.execute("MATCH (node) RETURN count(node) AS count"));
	const QueryResult before = engine.execute("CALL dbms.listSlowQueries()");
	ASSERT_FALSE(before.isEmpty());

	QueryGuard guard(1, 0);
	for (size_t index = 0U; index < 1023U; ++index) {
		guard.check();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	QueryContext context;
	context.guard = &guard;
	EXPECT_THROW(static_cast<void>(engine.executePlan(engine.buildPlan("CALL dbms.clearSlowQueries()"), context)),
			 QueryTimeoutException);

	const QueryResult after = engine.execute("CALL dbms.listSlowQueries()");
	ASSERT_EQ(after.getColumns(), before.getColumns());
	ASSERT_EQ(after.rowCount(), before.rowCount());
	for (size_t index = 0U; index < before.rowCount(); ++index) {
		for (const std::string_view column: {"sequence", "logicalPlan", "elapsedMs", "succeeded"}) {
			EXPECT_EQ(after.getRows()[index].at(std::string(column)).asPrimitive(),
					before.getRows()[index].at(std::string(column)).asPrimitive());
		}
	}
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, ShortestPathProcedurePublishesOnlyAfterCommitAndSurvivesReopen) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	QueryEngine engine(backend, backend);
	std::unique_ptr<LogicalGraphTransaction> transaction = backend->beginLogicalGraphTransaction(false);
	LogicalGraphTransactionOverlay overlay(*transaction);
	const std::vector<uint64_t> nodes = stagePath(overlay);
	const uint64_t sourceNodeId = nodes[0U];
	const uint64_t targetNodeId = nodes[1U];
	const std::string pathQuery = shortestPathQuery(sourceNodeId, targetNodeId);
	static_cast<void>(transaction->commit());

	const QueryResult committed = engine.execute(pathQuery);
	ASSERT_EQ(committed.rowCount(), 2U);
	EXPECT_EQ(committed.getRows().front().at("node").asLogicalNode().node_id, sourceNodeId);
	EXPECT_EQ(committed.getRows().back().at("node").asLogicalNode().node_id, targetNodeId);
	backend->close();

	RelocatableStorageBackendOptions reopenedOptions = backendOptions();
	reopenedOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto reopened = std::make_shared<RelocatableStorageBackend>(std::move(reopenedOptions));
	reopened->open();
	QueryEngine reopenedEngine(reopened, reopened);
	const QueryResult afterReopen = reopenedEngine.execute(pathQuery);
	ASSERT_EQ(afterReopen.rowCount(), 2U);
	EXPECT_EQ(afterReopen.getRows().front().at("node").asLogicalNode().node_id, sourceNodeId);
	EXPECT_EQ(afterReopen.getRows().back().at("node").asLogicalNode().node_id, targetNodeId);
	reopened->close();
}

TEST_F(RelocatableStorageBackendTest, OverlayAcceptsSequentialMutationStatementsFromItsCurrentRevision) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	QueryEngine engine(backend, backend);
	std::unique_ptr<LogicalGraphTransaction> transaction = backend->beginLogicalGraphTransaction(false);
	LogicalGraphTransactionOverlay overlay(*transaction);

	for (int64_t id = 0; id < 5; ++id) {
		const QueryResult created = engine.executePlanWithLogicalTransactionOverlay(
				engine.buildPlan("CREATE (:OverlayRevision {id: " + std::to_string(id) + "})"), QueryContext{}, overlay,
				overlay);
		EXPECT_TRUE(created.isEmpty());
	}

	const QueryResult staged = engine.executePlanWithLogicalTransactionOverlay(
			engine.buildPlan("MATCH (node:OverlayRevision) RETURN count(node) AS count"), QueryContext{}, overlay, overlay);
	ASSERT_EQ(staged.rowCount(), 1U);
	EXPECT_EQ(staged.getRows().front().at("count").asPrimitive(), PropertyValue(int64_t{5}));
	transaction->rollback();

	const QueryResult hidden = engine.execute("MATCH (node:OverlayRevision) RETURN count(node) AS count");
	ASSERT_EQ(hidden.rowCount(), 1U);
	EXPECT_EQ(hidden.getRows().front().at("count").asPrimitive(), PropertyValue(int64_t{0}));
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, GdsProceduresUseAnOverlayLocalCatalogAndTrackStagedGraphRevisions) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	auto backend = std::make_shared<RelocatableStorageBackend>(std::move(options));
	backend->open();
	QueryEngine engine(backend, backend);
	std::unique_ptr<LogicalGraphTransaction> transaction = backend->beginLogicalGraphTransaction(false);
	LogicalGraphTransactionOverlay overlay(*transaction);
	const std::vector<uint64_t> nodes = stagePath(overlay);
	gds::LogicalProjectionCatalog transactionCatalog;

	const auto executeInTransaction = [&](const std::string &query) {
		return engine.executePlanWithLogicalTransactionOverlay(engine.buildPlan(query), QueryContext{}, overlay, overlay,
				&transactionCatalog);
	};
	EXPECT_THROW(static_cast<void>(executeInTransaction("CALL gds.graph.project('', {})")), std::invalid_argument);
	EXPECT_TRUE(executeInTransaction("CALL gds.graph.list()").isEmpty());
	const QueryResult projected = executeInTransaction("CALL gds.graph.project('transaction_graph', {})");
	ASSERT_EQ(projected.rowCount(), 1U);
	EXPECT_EQ(projected.getRows().front().at("nodeCount").asPrimitive(), PropertyValue(int64_t{2}));
	EXPECT_EQ(projected.getRows().front().at("edgeCount").asPrimitive(), PropertyValue(int64_t{1}));

	const QueryResult components = executeInTransaction("CALL gds.wcc.stream('transaction_graph')");
	ASSERT_EQ(components.rowCount(), 2U);
	for (const QueryResult::Row &row: components.getRows()) {
		EXPECT_TRUE(row.contains("nodeId"));
		EXPECT_TRUE(row.contains("componentId"));
	}
	// Every GDS algorithm must resolve the projection from the transaction-local
	// catalog rather than the Database-owned catalog.
	EXPECT_EQ(executeInTransaction("CALL gds.pageRank.stream('transaction_graph')").rowCount(), 2U);
	EXPECT_EQ(executeInTransaction("CALL gds.betweenness.stream('transaction_graph')").rowCount(), 2U);
	EXPECT_EQ(executeInTransaction("CALL gds.closeness.stream('transaction_graph')").rowCount(), 2U);
	EXPECT_EQ(executeInTransaction("CALL gds.leiden.stream('transaction_graph', {maxIterations: 4})").rowCount(),
			  2U);
	EXPECT_EQ(executeInTransaction("CALL gds.shortestPath.dijkstra.stream('transaction_graph', " +
									 std::to_string(nodes[0U]) + ", " + std::to_string(nodes[1U]) + ")")
				  .rowCount(),
			  2U);
	EXPECT_THROW(static_cast<void>(executeInTransaction("CALL gds.graph.project('transaction_graph', {})")),
			 std::invalid_argument);
	EXPECT_EQ(executeInTransaction("CALL gds.graph.exists('transaction_graph')").getRows().front().at("exists").asPrimitive(),
			  PropertyValue(true));

	// The engine-owned catalog remains isolated from the transaction's derived
	// projection, even though both read the same native database file.
	const QueryResult globalExists = engine.execute("CALL gds.graph.exists('transaction_graph')");
	ASSERT_EQ(globalExists.rowCount(), 1U);
	EXPECT_EQ(globalExists.getRows().front().at("exists").asPrimitive(), PropertyValue(false));

	static_cast<void>(overlay.createLogicalNodes({{{"ProcedureOverlayPath"}, {}}}));
	const QueryResult stale = executeInTransaction("CALL gds.graph.list('transaction_graph')");
	ASSERT_EQ(stale.rowCount(), 1U);
	EXPECT_EQ(stale.getRows().front().at("stale").asPrimitive(), PropertyValue(true));

	transaction->rollback();
	EXPECT_EQ(engine.execute("CALL gds.graph.exists('transaction_graph')").getRows().front().at("exists").asPrimitive(),
			  PropertyValue(false));
	backend->close();
}

TEST_F(RelocatableStorageBackendTest, CoreTransactionOwnsAndRetiresItsLocalGdsCatalog) {
	static_cast<void>(materializeNativeGraphFixture());
	graph::Database database(data_path_.string());
	database.open();
	QueryEngine &engine = *database.getQueryEngine();

	graph::Transaction transaction = database.beginReadOnlyTransaction();
	const QueryResult projected = transaction.execute(engine, "CALL gds.graph.project('core_transaction_graph', {})");
	ASSERT_EQ(projected.rowCount(), 1U);
	const QueryResult inTransaction = transaction.execute(engine, "CALL gds.graph.exists('core_transaction_graph')");
	ASSERT_EQ(inTransaction.rowCount(), 1U);
	EXPECT_EQ(inTransaction.getRows().front().at("exists").asPrimitive(), PropertyValue(true));
	EXPECT_EQ(engine.execute("CALL gds.graph.exists('core_transaction_graph')").getRows().front().at("exists").asPrimitive(),
			  PropertyValue(false));
	transaction.commit();

	graph::Transaction nextTransaction = database.beginReadOnlyTransaction();
	const QueryResult afterCommit = nextTransaction.execute(engine, "CALL gds.graph.exists('core_transaction_graph')");
	ASSERT_EQ(afterCommit.rowCount(), 1U);
	EXPECT_EQ(afterCommit.getRows().front().at("exists").asPrimitive(), PropertyValue(false));
	nextTransaction.commit();
	database.close();
}

TEST_F(RelocatableStorageBackendTest, CoreTransactionRollbackRetiresItsLocalGdsCatalog) {
	static_cast<void>(materializeNativeGraphFixture());
	graph::Database database(data_path_.string());
	database.open();
	QueryEngine &engine = *database.getQueryEngine();

	graph::Transaction transaction = database.beginReadOnlyTransaction();
	const QueryResult projected = transaction.execute(engine, "CALL gds.graph.project('rolled_back_transaction_graph', {})");
	ASSERT_EQ(projected.rowCount(), 1U);
	EXPECT_EQ(transaction.execute(engine, "CALL gds.graph.exists('rolled_back_transaction_graph')")
				  .getRows()
				  .front()
				  .at("exists")
				  .asPrimitive(),
			  PropertyValue(true));
	transaction.rollback();

	graph::Transaction nextTransaction = database.beginReadOnlyTransaction();
	const QueryResult afterRollback = nextTransaction.execute(engine, "CALL gds.graph.exists('rolled_back_transaction_graph')");
	ASSERT_EQ(afterRollback.rowCount(), 1U);
	EXPECT_EQ(afterRollback.getRows().front().at("exists").asPrimitive(), PropertyValue(false));
	nextTransaction.commit();
	database.close();
}

TEST_F(RelocatableStorageBackendTest, PublicTransactionOwnsAndRetiresItsLocalGdsCatalog) {
	static_cast<void>(materializeNativeGraphFixture());
	zyx::Database database(data_path_.string());
	database.open();

	zyx::Transaction transaction = database.beginReadOnlyTransaction();
	const zyx::Result projected = transaction.execute("CALL gds.graph.project('public_transaction_graph', {})");
	ASSERT_TRUE(projected.isSuccess()) << projected.getError();
	ASSERT_TRUE(projected.hasNext());
	projected.next();
	const zyx::Result inTransaction = transaction.execute("CALL gds.graph.exists('public_transaction_graph')");
	ASSERT_TRUE(inTransaction.isSuccess()) << inTransaction.getError();
	ASSERT_TRUE(inTransaction.hasNext());
	inTransaction.next();
	EXPECT_TRUE(std::get<bool>(inTransaction.get("exists")));

	const zyx::Result globalExists = database.execute("CALL gds.graph.exists('public_transaction_graph')");
	ASSERT_TRUE(globalExists.isSuccess()) << globalExists.getError();
	ASSERT_TRUE(globalExists.hasNext());
	globalExists.next();
	EXPECT_FALSE(std::get<bool>(globalExists.get("exists")));
	transaction.commit();

	zyx::Transaction nextTransaction = database.beginReadOnlyTransaction();
	const zyx::Result afterCommit = nextTransaction.execute("CALL gds.graph.exists('public_transaction_graph')");
	ASSERT_TRUE(afterCommit.isSuccess()) << afterCommit.getError();
	ASSERT_TRUE(afterCommit.hasNext());
	afterCommit.next();
	EXPECT_FALSE(std::get<bool>(afterCommit.get("exists")));
	nextTransaction.commit();
	database.close();
}

TEST_F(RelocatableStorageBackendTest, PublicTransactionRollbackRetiresItsLocalGdsCatalog) {
	static_cast<void>(materializeNativeGraphFixture());
	zyx::Database database(data_path_.string());
	database.open();

	zyx::Transaction transaction = database.beginReadOnlyTransaction();
	const zyx::Result projected = transaction.execute("CALL gds.graph.project('public_rolled_back_transaction_graph', {})");
	ASSERT_TRUE(projected.isSuccess()) << projected.getError();
	const zyx::Result inTransaction = transaction.execute("CALL gds.graph.exists('public_rolled_back_transaction_graph')");
	ASSERT_TRUE(inTransaction.isSuccess()) << inTransaction.getError();
	ASSERT_TRUE(inTransaction.hasNext());
	inTransaction.next();
	EXPECT_TRUE(std::get<bool>(inTransaction.get("exists")));
	transaction.rollback();

	zyx::Transaction nextTransaction = database.beginReadOnlyTransaction();
	const zyx::Result afterRollback = nextTransaction.execute("CALL gds.graph.exists('public_rolled_back_transaction_graph')");
	ASSERT_TRUE(afterRollback.isSuccess()) << afterRollback.getError();
	ASSERT_TRUE(afterRollback.hasNext());
	afterRollback.next();
	EXPECT_FALSE(std::get<bool>(afterRollback.get("exists")));
	nextTransaction.commit();
	database.close();
}

TEST_F(RelocatableStorageBackendTest, PublicTransactionPinsVectorSearchAndUsesExactStagedGraphOverlay) {
	materializeNativeDatabase();
	zyx::Database database(data_path_.string());
	database.open();

	ASSERT_TRUE(database.execute("CREATE VECTOR INDEX transaction_embedding ON :Embedding(v) OPTIONS {dim: 2}").isSuccess());
	ASSERT_TRUE(database.execute("CREATE (:Embedding {v: [1.0, 0.0]})").isSuccess());
	ASSERT_TRUE(database.execute("CREATE (:Embedding {v: [0.0, 1.0]})").isSuccess());
	const zyx::Result trained = database.execute("CALL db.index.vector.train('transaction_embedding')");
	ASSERT_TRUE(trained.isSuccess()) << trained.getError();

	zyx::Transaction reader = database.beginReadOnlyTransaction();
	const zyx::Result visible = reader.execute(
			"CALL db.index.vector.queryNodes('transaction_embedding', 1, [1.0, 0.0]) YIELD node, score RETURN node, score");
	ASSERT_TRUE(visible.isSuccess()) << visible.getError();
	ASSERT_TRUE(visible.hasNext());
	visible.next();
	EXPECT_TRUE(std::holds_alternative<std::shared_ptr<zyx::Node>>(visible.get("node")));
	reader.commit();

	zyx::Transaction writer = database.beginTransaction();
	const zyx::Result created = writer.execute("CREATE (n:Embedding {v: [0.5, 0.5]}) RETURN id(n) AS id");
	ASSERT_TRUE(created.isSuccess()) << created.getError();
	ASSERT_TRUE(created.hasNext());
	created.next();
	const int64_t createdId = std::get<int64_t>(created.get("id"));
	const zyx::Result staged = writer.execute(
			"CALL db.index.vector.queryNodes('transaction_embedding', 1, [0.5, 0.5]) YIELD node RETURN id(node) AS id");
	ASSERT_TRUE(staged.isSuccess()) << staged.getError();
	ASSERT_TRUE(staged.hasNext());
	staged.next();
	EXPECT_EQ(std::get<int64_t>(staged.get("id")), createdId);
	EXPECT_FALSE(staged.hasNext());
	writer.rollback();
	database.close();
}

TEST_F(RelocatableStorageBackendTest, PublicTransactionVectorOverlayHonorsReplacementDeletionAndRollback) {
	materializeNativeDatabase();
	zyx::Database database(data_path_.string());
	database.open();

	ASSERT_TRUE(database.execute("CREATE VECTOR INDEX transaction_overlay_embedding ON :Embedding(v) OPTIONS {dim: 2}")
					.isSuccess());
	const zyx::Result firstCreated = database.execute("CREATE (n:Embedding {v: [1.0, 0.0]}) RETURN id(n) AS id");
	ASSERT_TRUE(firstCreated.isSuccess()) << firstCreated.getError();
	ASSERT_TRUE(firstCreated.hasNext());
	firstCreated.next();
	const int64_t firstId = std::get<int64_t>(firstCreated.get("id"));
	const zyx::Result secondCreated = database.execute("CREATE (n:Embedding {v: [0.0, 1.0]}) RETURN id(n) AS id");
	ASSERT_TRUE(secondCreated.isSuccess()) << secondCreated.getError();
	ASSERT_TRUE(secondCreated.hasNext());
	secondCreated.next();
	const int64_t secondId = std::get<int64_t>(secondCreated.get("id"));
	const zyx::Result trained = database.execute("CALL db.index.vector.train('transaction_overlay_embedding')");
	ASSERT_TRUE(trained.isSuccess()) << trained.getError();

	zyx::Transaction writer = database.beginTransaction();
	ASSERT_TRUE(writer.execute("MATCH (n) WHERE id(n) = " + std::to_string(firstId) + " SET n.v = [0.0, 1.0]")
					.isSuccess());
	ASSERT_TRUE(writer.execute("MATCH (n) WHERE id(n) = " + std::to_string(secondId) + " DELETE n").isSuccess());
	const zyx::Result staged = writer.execute(
			"CALL db.index.vector.queryNodes('transaction_overlay_embedding', 1, [1.0, 0.0]) YIELD node RETURN id(node) AS id");
	ASSERT_TRUE(staged.isSuccess()) << staged.getError();
	ASSERT_TRUE(staged.hasNext());
	staged.next();
	EXPECT_EQ(std::get<int64_t>(staged.get("id")), firstId);
	EXPECT_FALSE(staged.hasNext());
	writer.rollback();

	const zyx::Result afterRollback = database.execute(
			"CALL db.index.vector.queryNodes('transaction_overlay_embedding', 1, [1.0, 0.0]) YIELD node RETURN id(node) AS id");
	ASSERT_TRUE(afterRollback.isSuccess()) << afterRollback.getError();
	ASSERT_TRUE(afterRollback.hasNext());
	afterRollback.next();
	EXPECT_EQ(std::get<int64_t>(afterRollback.get("id")), firstId);
	database.close();
}
