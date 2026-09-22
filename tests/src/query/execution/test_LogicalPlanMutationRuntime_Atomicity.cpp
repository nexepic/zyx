/**
 * @file test_LogicalPlanMutationRuntime_Atomicity.cpp
 * @brief End-to-end tests for bounded logical-plan mutations.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "query/execution/LogicalPlanMutationRuntimeTestFixture.hpp"

namespace graph::query {

	TEST(LogicalPlanMutationRuntimeTest, ForeachFailureAfterStagingDoesNotPublishAnyMutation) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		// The first iteration stages a Node. The second hits a real expression
		// failure before the shared batch can be published.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "FOREACH (value IN [1, 0] | CREATE (:AtomicItem {quotient: 1 / value}))")),
				 std::runtime_error);

		EXPECT_EQ(backend->countNodesWithLabel("AtomicItem"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, SetFailureAfterStagingDoesNotPublishAnyMutation) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t first = backend->addNode({"Seed"}, {{"divisor", PropertyValue(1)}});
		const uint64_t second = backend->addNode({"Seed"}, {{"divisor", PropertyValue(0)}});
		QueryEngine engine(backend, backend);

		// The first match stages a replacement. The second match raises division by
		// zero, so neither replacement may cross the batch publication boundary.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (n:Seed) SET n.quotient = 1 / n.divisor")),
				 std::runtime_error);

		EXPECT_FALSE(backend->graph().nodes.at(first).properties.contains("quotient"));
		EXPECT_FALSE(backend->graph().nodes.at(second).properties.contains("quotient"));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeFailureAfterStagingDoesNotPublishAnyMutation) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MERGE (source:AtomicMerge)-[relationship:LINK]->(target:AtomicMergeEnd) "
					 "ON CREATE SET relationship.quotient = 1 / 0")),
				 std::runtime_error);

		EXPECT_EQ(backend->countNodesWithLabel("AtomicMerge"), 0U);
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMergeEnd"), 0U);
		EXPECT_EQ(backend->edgeCount(), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeEvaluatesDynamicNodeIdentityPropertiesPerInputRow) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute("UNWIND [1, 2, 1] AS value MERGE (item:AtomicMerge {value: value})"));
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMerge"), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
		std::set<int64_t> values;
		for (const auto &[_, node] : backend->graph().nodes) {
			values.insert(std::get<int64_t>(node.properties.at("value").getVariant()));
		}
		EXPECT_EQ(values, (std::set<int64_t>{1, 2}));

		static_cast<void>(engine.execute("UNWIND [1, 2] AS value MERGE (:AtomicMerge {value: value})"));
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMerge"), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeAppliesReturnOrderingAndPaginationAfterPublishingAllInputRows) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		// LIMIT constrains only visible result rows. Every UNWIND row must take
		// part in the one atomic MERGE batch before ORDER BY/SKIP/LIMIT runs.
		const QueryResult result = engine.execute(
				"UNWIND [1, 2, 1] AS value "
				"MERGE (item:AtomicMerge {value: value}) "
				"RETURN item.value AS orderedValue ORDER BY orderedValue DESC SKIP 1 LIMIT 1");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("orderedValue").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMerge"), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeRebuildsMixedCreatedAndScalarReturnExpressionsAfterPublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		// The scalar is available before publication, while item is assigned a
		// durable ID only by the committed batch. The combined expression must
		// therefore be evaluated against both phases without leaking a provisional
		// entity into the returned result.
		const QueryResult result = engine.execute(
				"UNWIND [1, 2] AS value "
				"MERGE (item:AtomicMerge {value: value}) "
				"RETURN item.value + value AS doubled");

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0U].at("doubled").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(result.getRows()[1U].at("doubled").asPrimitive(), PropertyValue(4));
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMerge"), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateAppliesReturnOrderingAndPaginationAfterPublishingAllInputRows) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		// Result pagination controls only what the caller sees. All UNWIND rows
		// still participate in the one atomic CREATE publication before sorting.
		const QueryResult result = engine.execute(
				"UNWIND [1, 3, 2] AS value "
				"CREATE (:AtomicCreate {value: value}) "
				"RETURN value AS orderedValue ORDER BY orderedValue DESC SKIP 1 LIMIT 1");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("orderedValue").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(backend->countNodesWithLabel("AtomicCreate"), 3U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateDistinctReturnAppliesAfterPublishingAllInputRows) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		// DISTINCT and pagination reshape only the result. Duplicate input rows must
		// still be written before the one atomic mutation batch is published.
		const QueryResult result = engine.execute(
				"UNWIND [1, 1, 2] AS value "
				"CREATE (:AtomicDistinctCreate {value: value}) "
				"RETURN DISTINCT value AS orderedValue ORDER BY orderedValue DESC SKIP 1 LIMIT 1");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("orderedValue").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(backend->countNodesWithLabel("AtomicDistinctCreate"), 3U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateReturnSkipPastDistinctRowsStillPublishesAllInputRows) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		// An empty paginated result is not an empty mutation. CREATE must publish
		// every input row before a SKIP beyond the distinct result set is applied.
		const QueryResult result = engine.execute(
				"UNWIND [1, 1, 2] AS value "
				"CREATE (:AtomicSkippedCreate {value: value}) "
				"RETURN DISTINCT value AS orderedValue ORDER BY orderedValue DESC SKIP 2 LIMIT 1");
		EXPECT_EQ(result.rowCount(), 0U);
		EXPECT_EQ(result.getPipelineRowCount(), 0U);
		EXPECT_EQ(backend->countNodesWithLabel("AtomicSkippedCreate"), 3U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateReturnLimitZeroStillPublishesAllInputRows) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		// LIMIT 0 discards visible rows only; it must not suppress the CREATE
		// statements that were staged before result shaping begins.
		const QueryResult result = engine.execute(
				"UNWIND [1, 2, 3] AS value "
				"CREATE (:AtomicLimitedCreate {value: value}) "
				"RETURN value AS orderedValue ORDER BY orderedValue DESC LIMIT 0");
		EXPECT_EQ(result.rowCount(), 0U);
		EXPECT_EQ(result.getPipelineRowCount(), 0U);
		EXPECT_EQ(backend->countNodesWithLabel("AtomicLimitedCreate"), 3U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeRejectsAnOverBoundInputWithoutPublishingItsPrefix) {
		auto backend = std::make_shared<TestLogicalGraph>();
		LogicalReadExecutionOptions options;
		options.bounds.maximum_rows = 1U;
		QueryEngine engine(backend, backend, options);

		// MERGE must fully stage every input row before publication. A resource
		// limit therefore rejects the complete statement instead of committing the
		// first row and silently dropping the remainder.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "UNWIND [1, 2] AS value MERGE (:AtomicMerge {value: value})")),
				 std::runtime_error);
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMerge"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeCachesPrepublicationScalarResultsButUsesPublishedEntityIds) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);
		registerCountingMergeResultExpression();
		mergeResultExpressionCalls.store(0, std::memory_order_relaxed);

		const QueryResult result = engine.execute(
				"MERGE (item:AtomicMerge {value: 11}) "
				"RETURN id(item) AS itemId, testMergeResultCounter() AS counter");

		ASSERT_EQ(result.rowCount(), 1U);
		const QueryResult::Row &row = result.getRows().front();
		// The created entity is materialized after publication, so id(item) is the
		// durable backend ID rather than the pre-publication sentinel.
		EXPECT_EQ(row.at("itemId").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(row.at("counter").asPrimitive(), PropertyValue(0));
		EXPECT_EQ(mergeResultExpressionCalls.load(std::memory_order_relaxed), 1);
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMerge"), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeCachesPrepublicationSortKeys) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);
		registerCountingMergeResultExpression();
		mergeResultExpressionCalls.store(0, std::memory_order_relaxed);

		const QueryResult result = engine.execute(
				"UNWIND [1, 2] AS value "
				"MERGE (:AtomicMerge {value: value}) "
				"RETURN value AS value ORDER BY testMergeResultCounter()");

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows().at(0).at("value").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(result.getRows().at(1).at("value").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(mergeResultExpressionCalls.load(std::memory_order_relaxed), 2);
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMerge"), 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateCachesPrepublicationSortKeys) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);
		registerCountingMergeResultExpression();
		mergeResultExpressionCalls.store(0, std::memory_order_relaxed);

		const QueryResult result = engine.execute(
				"UNWIND [1, 2] AS value "
				"CREATE (:AtomicCreate {value: value}) "
				"RETURN value AS value ORDER BY testMergeResultCounter()");

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows().at(0).at("value").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(result.getRows().at(1).at("value").asPrimitive(), PropertyValue(2));
		// The volatile scalar sort key is evaluated during pre-publication
		// validation and reused after the durable batch is published.
		EXPECT_EQ(mergeResultExpressionCalls.load(std::memory_order_relaxed), 2);
		EXPECT_EQ(backend->countNodesWithLabel("AtomicCreate"), 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateReevaluatesSortKeysThatDependOnDurableEntityIds) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		const QueryResult result = engine.execute(
				"UNWIND [1, 2] AS value "
				"CREATE (item:AtomicCreate {value: value}) "
				"RETURN id(item) AS itemId ORDER BY itemId DESC");

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows().at(0).at("itemId").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(result.getRows().at(1).at("itemId").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(backend->countNodesWithLabel("AtomicCreate"), 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateRejectsEntitySortKeysBeforePublishingTheBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CREATE (item:AtomicCreate) RETURN item ORDER BY item")),
				 std::logic_error);
		EXPECT_EQ(backend->countNodesWithLabel("AtomicCreate"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeDoesNotPublishWhenItsReturnExpressionFails) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MERGE (:AtomicMerge {value: 1}) RETURN 1 / 0 AS invalidResult")),
				 std::runtime_error);
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMerge"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeDynamicIdentitySupportsParametersAndReadBindings) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(8)}}));
		QueryEngine engine(backend, backend);
		QueryContext parameterContext;
		parameterContext.parameters.emplace("identity", PropertyValue(7));

		const QueryResult parameterResult = engine.execute(
				"MERGE (item:AtomicMerge {value: $identity}) RETURN item.value AS value", parameterContext);
		ASSERT_EQ(parameterResult.rowCount(), 1U);
		EXPECT_EQ(parameterResult.getRows().front().at("value").asPrimitive(), PropertyValue(7));
		static_cast<void>(engine.execute("MERGE (:AtomicMerge {value: $identity})", parameterContext));

		const QueryResult bindingResult = engine.execute(
				"MATCH (source:Seed) MERGE (item:AtomicMerge {value: source.value}) RETURN item.value AS value");
		ASSERT_EQ(bindingResult.rowCount(), 1U);
		EXPECT_EQ(bindingResult.getRows().front().at("value").asPrimitive(), PropertyValue(8));
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMerge"), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeDynamicIdentityNullOrFailureDoesNotPublishStagedRows) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "UNWIND [1, null] AS value MERGE (:AtomicMerge {value: value})")),
				 std::runtime_error);
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMerge"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "UNWIND [1, 0] AS divisor MERGE (:AtomicMerge {value: 1 / divisor})")),
				 std::runtime_error);
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMerge"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeEvaluatesDynamicRelationshipIdentityPropertiesPerInputRow) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"UNWIND [5, 7, 5] AS value "
				"MERGE (:AtomicMerge {id: 1})-[:LINK {weight: value}]->(:AtomicMergeEnd {id: 2})"));
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMerge"), 1U);
		EXPECT_EQ(backend->countNodesWithLabel("AtomicMergeEnd"), 1U);
		EXPECT_EQ(backend->edgeCount(), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);

		static_cast<void>(engine.execute(
				"UNWIND [5, 7] AS value "
				"MERGE (:AtomicMerge {id: 1})-[:LINK {weight: value}]->(:AtomicMergeEnd {id: 2})"));
		EXPECT_EQ(backend->edgeCount(), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, WriteOnlyCallFailureAfterStagingDoesNotPublishAnyMutation) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CALL { CREATE (:AtomicCall) CREATE (:AtomicCall {quotient: 1 / 0}) }")),
				 std::runtime_error);

		EXPECT_EQ(backend->countNodesWithLabel("AtomicCall"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachReturnsCapturedRowsAfterItsMutationChangesTheInputMatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute("MATCH (n:Seed) FOREACH (value IN [1] | REMOVE n:Seed) RETURN n");
		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalNodeResult &returned = result.getRows().front().at("n").asLogicalNode();
		EXPECT_EQ(returned.node_id, seedId);
		EXPECT_TRUE(returned.labels.empty());
		EXPECT_TRUE(backend->graph().nodes.at(seedId).labels.empty());
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateThenRemoveFoldsTheFinalPropertyStateIntoOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"CREATE (n:Item {value: 4, temporary: true}) REMOVE n.temporary RETURN n.value + 1 AS value");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(5));
		ASSERT_EQ(backend->graph().nodes.size(), 1U);
		const PropertyMap &properties = backend->graph().nodes.begin()->second.properties;
		EXPECT_EQ(properties.at("value"), PropertyValue(4));
		EXPECT_FALSE(properties.contains("temporary"));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateThenSetAndRemoveLabelsPublishesOnlyTheFinalLabelSet) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"CREATE (n:Original {value: 4}) SET n:Retained REMOVE n:Original RETURN n");
		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalNodeResult &node = result.getRows().front().at("n").asLogicalNode();
		EXPECT_TRUE(std::ranges::find(node.labels, "Retained") != node.labels.end());
		EXPECT_TRUE(std::ranges::find(node.labels, "Original") == node.labels.end());
		EXPECT_EQ(node.properties.at("value"), PropertyValue(4));

		ASSERT_EQ(backend->graph().nodes.size(), 1U);
		const TestNode &stored = backend->graph().nodes.begin()->second;
		EXPECT_EQ(stored.labels, node.labels);
		EXPECT_EQ(stored.properties, node.properties);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

TEST(LogicalPlanMutationRuntimeTest, CreateSetRetainsAnAlreadyDeclaredLabelWithoutDuplicatingIt) {
	auto backend = std::make_shared<TestLogicalGraph>();
	QueryEngine engine(backend, backend);

		const QueryResult result = engine.execute("CREATE (node:Stable {value: 7}) SET node:Stable RETURN node");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalNodeResult &node = result.getRows().front().at("node").asLogicalNode();
		EXPECT_EQ(node.labels, (std::vector<std::string>{"Stable"}));
		EXPECT_EQ(node.properties.at("value"), PropertyValue(7));
		ASSERT_EQ(backend->graph().nodes.size(), 1U);
	EXPECT_EQ(backend->graph().nodes.begin()->second.labels, node.labels);
	EXPECT_EQ(backend->graph().mutation_batches, 1U);
}

TEST(LogicalPlanMutationRuntimeTest, SetLabelOnlyPublishesThePersistedNodeLabelReplacement) {
	auto backend = std::make_shared<TestLogicalGraph>();
	const uint64_t nodeId = backend->addNode({"Seed"}, {{"value", PropertyValue(7)}});
	QueryEngine engine(backend, backend);

	const QueryResult result = engine.execute("MATCH (node:Seed) SET node:Published RETURN node");

	ASSERT_EQ(result.rowCount(), 1U);
	const LogicalNodeResult &node = result.getRows().front().at("node").asLogicalNode();
	EXPECT_EQ(node.node_id, nodeId);
	EXPECT_EQ(node.properties, (PropertyMap{{"value", PropertyValue(7)}}));
	EXPECT_TRUE(std::ranges::find(node.labels, "Seed") != node.labels.end());
	EXPECT_TRUE(std::ranges::find(node.labels, "Published") != node.labels.end());
	EXPECT_EQ(backend->graph().nodes.at(nodeId).labels, node.labels);
	EXPECT_EQ(backend->graph().nodes.at(nodeId).properties, node.properties);
	EXPECT_EQ(backend->graph().mutation_batches, 1U);
}

TEST(LogicalPlanMutationRuntimeTest, MatchCreateThenSetPublishesPersistedAndCreatedFinalStatesTogether) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"value", PropertyValue(4)}});
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"MATCH (seed:Seed) CREATE (child:Child {copied: seed.value}) SET seed.updated = child.copied "
				"RETURN seed.updated AS updated, child.copied AS copied");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("updated").asPrimitive(), PropertyValue(4));
		EXPECT_EQ(result.getRows().front().at("copied").asPrimitive(), PropertyValue(4));
		EXPECT_EQ(backend->graph().nodes.at(seedId).properties.at("updated"), PropertyValue(4));
		EXPECT_EQ(backend->countNodesWithLabel("Child"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, SetAndRemoveUseCompleteNodeAndRelationshipPropertyReplacements) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t first = backend->addNode({"Person", "Stale"},
												{{"name", PropertyValue("Ada")}, {"remove", PropertyValue(true)}});
		const uint64_t second = backend->addNode({"Person"}, {{"name", PropertyValue("Bob")}});
		QueryEngine engine(backend, backend);

		QueryResult nodeResult =
				engine.execute("MATCH (n:Person {name: 'Ada'}) SET n += {age: 42, remove: null}, n:Current RETURN n");
		ASSERT_EQ(nodeResult.rowCount(), 1U);
		const LogicalNodeResult &updatedNode = nodeResult.getRows().front().at("n").asLogicalNode();
		EXPECT_EQ(updatedNode.node_id, first);
		EXPECT_EQ(updatedNode.properties.at("name"), PropertyValue("Ada"));
		EXPECT_EQ(updatedNode.properties.at("age"), PropertyValue(42));
		EXPECT_FALSE(updatedNode.properties.contains("remove"));
		EXPECT_TRUE(std::ranges::find(updatedNode.labels, "Current") != updatedNode.labels.end());
		EXPECT_EQ(backend->graph().mutation_batches, 1U);

		QueryResult edgeCreate = engine.execute("MATCH (a:Person {name: 'Ada'}), (b:Person {name: 'Bob'}) CREATE "
												"(a)-[r:KNOWS {weight: 1, stale: true}]->(b) RETURN r");
		ASSERT_EQ(edgeCreate.rowCount(), 1U);
		const uint64_t edgeId = edgeCreate.getRows().front().at("r").asLogicalEdge().edge_id;
		QueryResult edgeResult = engine.execute("MATCH (:Person {name: 'Ada'})-[r:KNOWS]->(:Person {name: 'Bob'}) SET "
												"r.weight = r.weight + 1 REMOVE r.stale RETURN r");
		ASSERT_EQ(edgeResult.rowCount(), 1U);
		const LogicalEdgeResult &updatedEdge = edgeResult.getRows().front().at("r").asLogicalEdge();
		EXPECT_EQ(updatedEdge.edge_id, edgeId);
		EXPECT_EQ(updatedEdge.properties.at("weight"), PropertyValue(2));
		EXPECT_FALSE(updatedEdge.properties.contains("stale"));
		EXPECT_EQ(backend->graph().edges.at(edgeId).properties, updatedEdge.properties);
		EXPECT_EQ(backend->graph().nodes.at(second).properties.at("name"), PropertyValue("Bob"));
	}

	TEST(LogicalPlanMutationRuntimeTest, SetOnRelationshipDoesNotLeakItsPropertyCacheIntoEndpointBindings) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"CREATE (source:Source {name: 'source'})-"
				"[:LINK {weight: 1}]->(target:Target {name: 'target'})"));
		const QueryResult result = engine.execute(
				"MATCH (source:Source {name: 'source'})-[relationship:LINK]->(target:Target {name: 'target'}) "
				"SET relationship.weight = relationship.weight + 1 RETURN source, relationship, target");

		ASSERT_EQ(result.rowCount(), 1U);
		const QueryResult::Row &row = result.getRows().front();
		const LogicalNodeResult &source = row.at("source").asLogicalNode();
		const LogicalEdgeResult &relationship = row.at("relationship").asLogicalEdge();
		const LogicalNodeResult &target = row.at("target").asLogicalNode();
		EXPECT_EQ(source.properties, (PropertyMap{{"name", PropertyValue("source")}}));
		EXPECT_EQ(target.properties, (PropertyMap{{"name", PropertyValue("target")}}));
		EXPECT_EQ(relationship.properties, (PropertyMap{{"weight", PropertyValue(2)}}));
		EXPECT_EQ(backend->graph().edges.at(relationship.edge_id).properties, relationship.properties);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateChainUpdatesPersistedRelationshipAndPublishesOneFinalState) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"CREATE (a:Person {name: 'Ada'})-[r:KNOWS {weight: 1, stale: true}]->(b:Person {name: 'Bob'})"));
		ASSERT_EQ(backend->edgeCount(), 1U);
		const uint64_t edgeId = backend->graph().edges.begin()->first;
		const size_t batchesBefore = backend->graph().mutation_batches;

		const QueryResult result = engine.execute(
				"MATCH (:Person {name: 'Ada'})-[r:KNOWS]->(:Person {name: 'Bob'}) "
				"CREATE (:Audit {event: 'relationship-update'}) "
				"SET r.weight = r.weight + 1 REMOVE r.stale RETURN r");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalEdgeResult &edge = result.getRows().front().at("r").asLogicalEdge();
		EXPECT_EQ(edge.edge_id, edgeId);
		EXPECT_EQ(edge.properties.at("weight"), PropertyValue(2));
		EXPECT_FALSE(edge.properties.contains("stale"));
		EXPECT_EQ(backend->graph().edges.at(edgeId).properties, edge.properties);
		EXPECT_EQ(backend->countNodesWithLabel("Audit"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, batchesBefore + 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateChainDeletesPersistedRelationshipAndReturnsItsPreDeleteBinding) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		const QueryResult created = engine.execute(
				"CREATE (a:Person {name: 'Ada'})-[r:KNOWS {weight: 3}]->(b:Person {name: 'Bob'}) RETURN r");
		ASSERT_EQ(created.rowCount(), 1U);
		const LogicalEdgeResult original = created.getRows().front().at("r").asLogicalEdge();
		const size_t batchesBefore = backend->graph().mutation_batches;

		const QueryResult deleted = engine.execute(
				"MATCH (:Person {name: 'Ada'})-[r:KNOWS]->(:Person {name: 'Bob'}) "
				"CREATE (:Audit {event: 'relationship-delete'}) DELETE r RETURN r");

		ASSERT_EQ(deleted.rowCount(), 1U);
		EXPECT_EQ(deleted.getRows().front().at("r").asLogicalEdge(), original);
		EXPECT_FALSE(backend->graph().edges.contains(original.edge_id));
		EXPECT_EQ(backend->countNodesWithLabel("Audit"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, batchesBefore + 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, DeleteMaterializesPreDeleteBindingsAndHonorsDetachSemantics) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Person"}, {{"name", PropertyValue("Ada")}}));
		static_cast<void>(backend->addNode({"Person"}, {{"name", PropertyValue("Bob")}}));
		QueryEngine engine(backend, backend);

		QueryResult created = engine.execute(
				"MATCH (a:Person {name: 'Ada'}), (b:Person {name: 'Bob'}) CREATE (a)-[r:KNOWS]->(b) RETURN r");
		ASSERT_EQ(created.rowCount(), 1U);
		const LogicalEdgeResult originalEdge = created.getRows().front().at("r").asLogicalEdge();

		QueryResult deletedEdge = engine.execute("MATCH ()-[r:KNOWS]->() DELETE r RETURN r");
		ASSERT_EQ(deletedEdge.rowCount(), 1U);
		EXPECT_EQ(deletedEdge.getRows().front().at("r").asLogicalEdge(), originalEdge);
		EXPECT_EQ(backend->edgeCount(), 0U);

		static_cast<void>(
				engine.execute("MATCH (a:Person {name: 'Ada'}), (b:Person {name: 'Bob'}) CREATE (a)-[:KNOWS]->(b)"));
		QueryResult detached = engine.execute("MATCH (n:Person {name: 'Ada'}) DETACH DELETE n RETURN n");
		ASSERT_EQ(detached.rowCount(), 1U);
		EXPECT_EQ(detached.getRows().front().at("n").asLogicalNode().properties.at("name"), PropertyValue("Ada"));
		EXPECT_EQ(backend->edgeCount(), 0U);
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, DeleteWithNoMatchesReturnsWithoutPublishingAnEmptyMutation) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t retained = backend->addNode({"Retained"}, {{"value", PropertyValue(7)}});
		QueryEngine engine(backend, backend);

		const QueryResult result = engine.execute("MATCH (n:Missing) DELETE n RETURN n");
		EXPECT_EQ(result.rowCount(), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
		ASSERT_TRUE(backend->graph().nodes.contains(retained));
		EXPECT_EQ(backend->graph().nodes.at(retained).properties.at("value"), PropertyValue(7));
	}

	TEST(LogicalPlanMutationRuntimeTest, CountStarObservesMutationPipelineRowsAndMergePostMutations) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult count = engine.execute("CREATE (:Person {name: 'Ada'}) RETURN count(*) AS count");
		ASSERT_EQ(count.rowCount(), 1U);
		EXPECT_EQ(count.getRows().front().at("count").asPrimitive(), PropertyValue(1));

		QueryResult merge = engine.execute(
				"MERGE (n:Person {name: 'Ada'}) SET n += {seen: true, name: 'Ada'} REMOVE n.name RETURN n");
		ASSERT_EQ(merge.rowCount(), 1U);
		const LogicalNodeResult &node = merge.getRows().front().at("n").asLogicalNode();
		EXPECT_TRUE(std::get<bool>(node.properties.at("seen").getVariant()));
		EXPECT_FALSE(node.properties.contains("name"));
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergePostMutationsFoldLabelsAndNullMapEntriesForCreatedAndMatchedNodes) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult created = engine.execute(
				"MERGE (n:Person {name: 'Ada'}) SET n:CreatedMarker, n:Person, n += {legacy: 1} RETURN n");
		ASSERT_EQ(created.rowCount(), 1U);
		const LogicalNodeResult &createdNode = created.getRows().front().at("n").asLogicalNode();
		EXPECT_TRUE(std::ranges::find(createdNode.labels, "Person") != createdNode.labels.end());
		EXPECT_TRUE(std::ranges::find(createdNode.labels, "CreatedMarker") != createdNode.labels.end());
		EXPECT_EQ(createdNode.properties.at("legacy"), PropertyValue(1));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);

		QueryResult matched = engine.execute(
				"MERGE (n:Person {name: 'Ada'}) SET n:Active, n:Person, n += {legacy: null, alreadyAbsent: null, score: 4} "
				"REMOVE n:CreatedMarker, n:AlreadyAbsent RETURN n");
		ASSERT_EQ(matched.rowCount(), 1U);
		const LogicalNodeResult &matchedNode = matched.getRows().front().at("n").asLogicalNode();
		EXPECT_TRUE(std::ranges::find(matchedNode.labels, "Person") != matchedNode.labels.end());
		EXPECT_TRUE(std::ranges::find(matchedNode.labels, "Active") != matchedNode.labels.end());
		EXPECT_TRUE(std::ranges::find(matchedNode.labels, "CreatedMarker") == matchedNode.labels.end());
		EXPECT_FALSE(matchedNode.properties.contains("legacy"));
		EXPECT_FALSE(matchedNode.properties.contains("alreadyAbsent"));
		EXPECT_EQ(matchedNode.properties.at("score"), PropertyValue(4));
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 2U);

		const QueryResult noOp =
				engine.execute("MERGE (n:Person {name: 'Ada'}) SET n:Person REMOVE n:AlreadyAbsent RETURN n");
		ASSERT_EQ(noOp.rowCount(), 1U);
		EXPECT_EQ(noOp.getRows().front().at("n").asLogicalNode().node_id, matchedNode.node_id);
		EXPECT_EQ(backend->graph().mutation_batches, 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeAppliesRuntimeMapParametersToCreatedAndMatchedNodes) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);
		QueryContext context;
		context.parameters.emplace(
				"patch", PropertyValue(PropertyValue::MapType{{"remove", PropertyValue()},
																						  {"alreadyAbsent", PropertyValue()}, {"added", PropertyValue(4)}}));

		const QueryResult created = engine.execute(
				"MERGE (node:RuntimeMap {key: 'one', remove: 'temporary'}) SET node += $patch RETURN node", context);
		ASSERT_EQ(created.rowCount(), 1U);
		const LogicalNodeResult &createdNode = created.getRows().front().at("node").asLogicalNode();
		EXPECT_EQ(createdNode.properties.at("key"), PropertyValue("one"));
		EXPECT_EQ(createdNode.properties.at("added"), PropertyValue(4));
		EXPECT_FALSE(createdNode.properties.contains("remove"));
		EXPECT_FALSE(createdNode.properties.contains("alreadyAbsent"));

		const QueryResult matched =
				engine.execute("MERGE (node:RuntimeMap {key: 'one'}) SET node += $patch RETURN node", context);
		ASSERT_EQ(matched.rowCount(), 1U);
		const LogicalNodeResult &matchedNode = matched.getRows().front().at("node").asLogicalNode();
		EXPECT_EQ(matchedNode.node_id, createdNode.node_id);
		EXPECT_EQ(matchedNode.properties.at("added"), PropertyValue(4));
		EXPECT_EQ(backend->countNodesWithLabel("RuntimeMap"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MutationAggregatesPreserveInputRowsAcrossSetAndMerge) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(1)}}));
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(2)}}));
		QueryEngine engine(backend, backend);

		QueryResult setCount = engine.execute("MATCH (n:Seed) SET n.value = n.value + 1 RETURN count(*) AS count");
		ASSERT_EQ(setCount.rowCount(), 1U);
		EXPECT_EQ(setCount.getRows().front().at("count").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(backend->graph().nodes.at(1U).properties.at("value"), PropertyValue(2));
		EXPECT_EQ(backend->graph().nodes.at(2U).properties.at("value"), PropertyValue(3));

		QueryResult mergeCount = engine.execute("MERGE (:Created {name: 'Ada'}) RETURN count(*) AS count");
		ASSERT_EQ(mergeCount.rowCount(), 1U);
		EXPECT_EQ(mergeCount.getRows().front().at("count").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(backend->countNodesWithLabel("Created"), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RetriesStandaloneCreateSetAndDeleteAfterOptimisticConflicts) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		backend->rejectNextMutation();
		QueryResult created = engine.execute("CREATE (n:Retry {value: 1}) RETURN n");
		ASSERT_EQ(created.rowCount(), 1U);
		const uint64_t retryId = created.getRows().front().at("n").asLogicalNode().node_id;
		EXPECT_EQ(backend->graph().nodes.at(retryId).properties.at("value"), PropertyValue(1));

		backend->rejectNextMutation();
		QueryResult updated = engine.execute("MATCH (n:Retry) SET n.value = n.value + 1 RETURN n.value AS value");
		ASSERT_EQ(updated.rowCount(), 1U);
		EXPECT_EQ(updated.getRows().front().at("value").asPrimitive(), PropertyValue(2));

		backend->rejectNextMutation();
		QueryResult deleted = engine.execute("MATCH (n:Retry) DELETE n RETURN n");
		ASSERT_EQ(deleted.rowCount(), 1U);
		EXPECT_EQ(deleted.getRows().front().at("n").asLogicalNode().node_id, retryId);
		EXPECT_FALSE(backend->graph().nodes.contains(retryId));
		EXPECT_EQ(backend->graph().mutation_batches, 3U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateSetChainCancelsTransientCreatesAndPublishesPersistedDeleteAtomically) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"name", PropertyValue("Ada")}});
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute("MATCH (:Seed) CREATE (transient:Transient {value: 1}) DELETE transient"));
		EXPECT_EQ(backend->countNodesWithLabel("Transient"), 0U);
		EXPECT_TRUE(backend->graph().nodes.contains(seedId));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);

		static_cast<void>(engine.execute("MATCH (seed:Seed) CREATE (child:Child {copied: seed.name}) SET child += "
										 "{created: true, copied: 'Ada'} "
										 "REMOVE child.copied DELETE seed"));
		EXPECT_FALSE(backend->graph().nodes.contains(seedId));
		ASSERT_EQ(backend->countNodesWithLabel("Child"), 1U);
		const auto child = std::ranges::find_if(backend->graph().nodes, [](const auto &entry) {
			return std::ranges::find(entry.second.labels, "Child") != entry.second.labels.end();
		});
		ASSERT_NE(child, backend->graph().nodes.end());
		EXPECT_EQ(child->second.properties.at("created"), PropertyValue(true));
		EXPECT_FALSE(child->second.properties.contains("copied"));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);

		static_cast<void>(engine.execute("CREATE (a:Ephemeral)-[r:LINK]->(b:Ephemeral) DELETE r, a, b"));
		EXPECT_EQ(backend->countNodesWithLabel("Ephemeral"), 0U);
		EXPECT_EQ(backend->edgeCount(), 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, DetachDeleteCreatedNodeCancelsIncidentRelationshipAndKeepsOtherEndpoint) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		const QueryResult result = engine.execute(
				"CREATE (deleted:Ephemeral)-[:LINK]->(retained:Retained {value: 7}) "
				"DETACH DELETE deleted RETURN retained");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalNodeResult &retained = result.getRows().front().at("retained").asLogicalNode();
		EXPECT_EQ(retained.properties.at("value"), PropertyValue(7));
		EXPECT_EQ(backend->countNodesWithLabel("Ephemeral"), 0U);
		EXPECT_EQ(backend->countNodesWithLabel("Retained"), 1U);
		EXPECT_EQ(backend->edgeCount(), 0U);
		EXPECT_EQ(backend->graph().nodes.at(retained.node_id).properties.at("value"), PropertyValue(7));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, DeleteCreatedNodeWithIncidentRelationshipFailsBeforePublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CREATE (deleted:Ephemeral)-[:LINK]->(:Retained {value: 7}) DELETE deleted")),
				 std::logic_error);
		EXPECT_EQ(backend->countNodesWithLabel("Ephemeral"), 0U);
		EXPECT_EQ(backend->countNodesWithLabel("Retained"), 0U);
		EXPECT_EQ(backend->edgeCount(), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, DeleteCreatedTargetNodeWithIncidentRelationshipFailsBeforePublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CREATE (:Retained {value: 7})-[:LINK]->(deleted:Ephemeral) DELETE deleted")),
				 std::logic_error);
		EXPECT_EQ(backend->countNodesWithLabel("Ephemeral"), 0U);
		EXPECT_EQ(backend->countNodesWithLabel("Retained"), 0U);
		EXPECT_EQ(backend->edgeCount(), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachFoldsCreatedEntityDeletesWithoutPublishingResidualGraphState) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		EXPECT_NO_THROW(static_cast<void>(engine.execute(
				"FOREACH (value IN [1] | CREATE (a:Ephemeral)-[r:LINK]->(b:Ephemeral) DELETE r, a, b)")));

		// CREATE and DELETE are folded into one final-state batch. A successful
		// FOREACH must not leave transient Nodes or their relationship reachable.
		EXPECT_EQ(backend->countNodesWithLabel("Ephemeral"), 0U);
		EXPECT_EQ(backend->edgeCount(), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, UnifiedSetRemovePathFoldsLabelsNullsAndMapUpdatesInOrder) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId = backend->addNode(
				{"Seed", "Old"},
				{{"keep", PropertyValue(1)}, {"remove", PropertyValue(true)}, {"old", PropertyValue("stale")}});
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute("MATCH (n:Seed) SET n.remove = null, n += {added: 4, old: null}, n:New "
											"REMOVE n.old, n:Old RETURN n");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalNodeResult &node = result.getRows().front().at("n").asLogicalNode();
		EXPECT_EQ(node.node_id, nodeId);
		EXPECT_EQ(node.properties.at("keep"), PropertyValue(1));
		EXPECT_EQ(node.properties.at("added"), PropertyValue(4));
		EXPECT_FALSE(node.properties.contains("remove"));
		EXPECT_FALSE(node.properties.contains("old"));
		EXPECT_TRUE(std::ranges::find(node.labels, "Seed") != node.labels.end());
		EXPECT_TRUE(std::ranges::find(node.labels, "New") != node.labels.end());
		EXPECT_TRUE(std::ranges::find(node.labels, "Old") == node.labels.end());
		EXPECT_EQ(backend->graph().nodes.at(nodeId).properties, node.properties);
		EXPECT_EQ(backend->graph().nodes.at(nodeId).labels, node.labels);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, UnifiedSetRemovePathAvoidsNoOpPublicationAndRetriesConflicts) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId = backend->addNode({"Seed", "Stable"}, {{"value", PropertyValue(1)}});
		QueryEngine engine(backend, backend);

		QueryResult noOp =
				engine.execute("MATCH (n:Seed) SET n.value = 1, n:Stable REMOVE n.missing, n:Missing RETURN n");
		ASSERT_EQ(noOp.rowCount(), 1U);
		EXPECT_EQ(noOp.getRows().front().at("n").asLogicalNode().node_id, nodeId);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);

		backend->rejectNextMutation();
		QueryResult retried = engine.execute(
				"MATCH (n:Seed) SET n.value = n.value + 1, n:Updated REMOVE n:Stable RETURN n.value AS value");
		ASSERT_EQ(retried.rowCount(), 1U);
		EXPECT_EQ(retried.getRows().front().at("value").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(backend->graph().nodes.at(nodeId).properties.at("value"), PropertyValue(2));
		EXPECT_TRUE(std::ranges::find(backend->graph().nodes.at(nodeId).labels, "Updated") !=
					backend->graph().nodes.at(nodeId).labels.end());
		EXPECT_TRUE(std::ranges::find(backend->graph().nodes.at(nodeId).labels, "Stable") ==
					backend->graph().nodes.at(nodeId).labels.end());
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, UnifiedSetRemovePathRejectsBoundedPartialMutations) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t first = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
		const uint64_t second = backend->addNode({"Seed"}, {{"value", PropertyValue(2)}});
		LogicalReadExecutionOptions options;
		options.bounds.maximum_node_records = 16U;
		options.bounds.maximum_node_page_records = 4U;
		options.bounds.maximum_metadata_pages_per_node_page = 4U;
		options.bounds.maximum_edges_per_node = 16U;
		options.bounds.maximum_edge_records = 32U;
		options.bounds.maximum_adjacency_page_records = 4U;
		options.bounds.maximum_property_entries = 16U;
		options.bounds.maximum_rows = 1U;
		options.bounds.maximum_distinct_rows = 16U;
		QueryEngine engine(backend, backend, options);

		EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) SET n.value = n.value + 1 RETURN n")),
					 std::runtime_error);
		EXPECT_EQ(backend->graph().nodes.at(first).properties.at("value"), PropertyValue(1));
		EXPECT_EQ(backend->graph().nodes.at(second).properties.at("value"), PropertyValue(2));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, AppliesRuntimeMapParametersToPersistedAndCreatedEntities) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t persisted =
				backend->addNode({"Seed"}, {{"keep", PropertyValue(1)}, {"remove", PropertyValue("stale")}});
		QueryEngine engine(backend, backend);

		QueryContext persistedContext;
		persistedContext.parameters.emplace(
				"patch",
				PropertyValue(PropertyValue::MapType{{"added", PropertyValue(2)}, {"remove", PropertyValue()}}));
		QueryResult persistedResult = engine.execute("MATCH (n:Seed) SET n += $patch RETURN n", persistedContext);
		ASSERT_EQ(persistedResult.rowCount(), 1U);
		const LogicalNodeResult &persistedNode = persistedResult.getRows().front().at("n").asLogicalNode();
		EXPECT_EQ(persistedNode.node_id, persisted);
		EXPECT_EQ(persistedNode.properties.at("keep"), PropertyValue(1));
		EXPECT_EQ(persistedNode.properties.at("added"), PropertyValue(2));
		EXPECT_FALSE(persistedNode.properties.contains("remove"));

		QueryContext createdContext;
		createdContext.parameters.emplace("patch", PropertyValue(PropertyValue::MapType{{"added", PropertyValue(4)},
																						{"remove", PropertyValue()}, {"alreadyAbsent", PropertyValue()}}));
		QueryResult createdResult = engine.execute(
				"CREATE (n:Created {keep: 3, remove: 'temporary'}) SET n += $patch RETURN n", createdContext);
		ASSERT_EQ(createdResult.rowCount(), 1U);
		const LogicalNodeResult &createdNode = createdResult.getRows().front().at("n").asLogicalNode();
		EXPECT_EQ(createdNode.properties.at("keep"), PropertyValue(3));
		EXPECT_EQ(createdNode.properties.at("added"), PropertyValue(4));
		EXPECT_FALSE(createdNode.properties.contains("remove"));
		EXPECT_FALSE(createdNode.properties.contains("alreadyAbsent"));
		EXPECT_EQ(backend->graph().nodes.at(createdNode.node_id).properties, createdNode.properties);
	}

	TEST(LogicalPlanMutationRuntimeTest, NoOpSetAndRemoveDoNotPublishAnEmptyMutationBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId =
				backend->addNode({"Existing"}, {{"value", PropertyValue(7)}, {"stable", PropertyValue(true)}});
		QueryEngine engine(backend, backend);

		// A statement can still return its matched row when every requested update
		// is already true. Publishing an empty batch would unnecessarily advance the
		// native graph generation and force concurrent snapshots to retry.
		const QueryResult result = engine.execute(
				"MATCH (node:Existing) SET node.value = 7, node.missingByNull = null, "
				"node += {alsoMissing: null}, node:Existing "
				"REMOVE node.missing, node:Missing RETURN node");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalNodeResult &returned = result.getRows().front().at("node").asLogicalNode();
		EXPECT_EQ(returned.node_id, nodeId);
		EXPECT_EQ(returned.labels, (std::vector<std::string>{"Existing"}));
		EXPECT_FALSE(returned.properties.contains("missingByNull"));
		EXPECT_FALSE(returned.properties.contains("alsoMissing"));
		EXPECT_EQ(returned.properties, backend->graph().nodes.at(nodeId).properties);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, SetRemoveFailsClosedWhenTheSnapshotCannotSupplyAnOptimisticFence) {
		for (const TestSnapshotMode mode: {TestSnapshotMode::TSSM_NULL, TestSnapshotMode::TSSM_MISSING_FENCE,
										 TestSnapshotMode::TSSM_INVALID_FENCE}) {
			auto backend = std::make_shared<TestLogicalGraph>();
			const uint64_t nodeId = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
			backend->setSnapshotMode(mode);
			QueryEngine engine(backend, backend);

			if (mode == TestSnapshotMode::TSSM_NULL) {
				EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) SET n.value = 2 RETURN n")),
						 std::runtime_error);
			} else {
				EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) SET n.value = 2 RETURN n")),
						 std::logic_error);
			}
			EXPECT_EQ(backend->graph().nodes.at(nodeId).properties.at("value"), PropertyValue(1));
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, DeleteFailsClosedWhenTheSnapshotCannotSupplyAnOptimisticFence) {
		for (const TestSnapshotMode mode: {TestSnapshotMode::TSSM_NULL, TestSnapshotMode::TSSM_MISSING_FENCE,
										 TestSnapshotMode::TSSM_INVALID_FENCE}) {
			auto backend = std::make_shared<TestLogicalGraph>();
			const uint64_t nodeId = backend->addNode({"Seed"}, {});
			backend->setSnapshotMode(mode);
			QueryEngine engine(backend, backend);

			if (mode == TestSnapshotMode::TSSM_NULL) {
				EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) DELETE n")), std::runtime_error);
			} else {
				EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) DELETE n")), std::logic_error);
			}
			EXPECT_TRUE(backend->graph().nodes.contains(nodeId));
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeFailsClosedWhenTheSnapshotCannotSupplyAnOptimisticFence) {
		for (const TestSnapshotMode mode: {TestSnapshotMode::TSSM_NULL, TestSnapshotMode::TSSM_MISSING_FENCE,
										 TestSnapshotMode::TSSM_INVALID_FENCE}) {
			auto backend = std::make_shared<TestLogicalGraph>();
			backend->setSnapshotMode(mode);
			QueryEngine engine(backend, backend);

			if (mode == TestSnapshotMode::TSSM_NULL) {
				EXPECT_THROW(static_cast<void>(engine.execute("MERGE (:Person {name: 'Ada'})")), std::runtime_error);
			} else {
				EXPECT_THROW(static_cast<void>(engine.execute("MERGE (:Person {name: 'Ada'})")), std::logic_error);
			}
			EXPECT_EQ(backend->countNodesWithLabel("Person"), 0U);
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateFailsClosedWhenTheSnapshotCannotSupplyAnOptimisticFence) {
		for (const TestSnapshotMode mode: {TestSnapshotMode::TSSM_NULL, TestSnapshotMode::TSSM_MISSING_FENCE,
										 TestSnapshotMode::TSSM_INVALID_FENCE}) {
			auto backend = std::make_shared<TestLogicalGraph>();
			backend->setSnapshotMode(mode);
			QueryEngine engine(backend, backend);

			if (mode == TestSnapshotMode::TSSM_NULL) {
				EXPECT_THROW(static_cast<void>(engine.execute("CREATE (:Seed {value: 1})")), std::runtime_error);
			} else {
				EXPECT_THROW(static_cast<void>(engine.execute("CREATE (:Seed {value: 1})")), std::logic_error);
			}
			EXPECT_EQ(backend->countNodesWithLabel("Seed"), 0U);
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateExhaustsConflictsWithoutPublishingAnyPartialMutation) {
		auto backend = std::make_shared<TestLogicalGraph>();
		backend->rejectMutationAttempts(4U);
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute("CREATE (:Seed {value: 1})")), std::runtime_error);
		EXPECT_EQ(backend->countNodesWithLabel("Seed"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, SetRemoveExhaustsConflictsWithoutPublishingAnyPartialMutation) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
		backend->rejectMutationAttempts(4U);
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) SET n.value = 2 RETURN n")),
				 std::runtime_error);
		EXPECT_EQ(backend->graph().nodes.at(nodeId).properties.at("value"), PropertyValue(1));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, DeleteExhaustsConflictsWithoutPublishingAnyPartialMutation) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId = backend->addNode({"Seed"}, {});
		backend->rejectMutationAttempts(4U);
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) DELETE n")), std::runtime_error);
		EXPECT_TRUE(backend->graph().nodes.contains(nodeId));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeExhaustsConflictsWithoutPublishingAnyPartialMutation) {
		auto backend = std::make_shared<TestLogicalGraph>();
		backend->rejectMutationAttempts(4U);
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute("MERGE (:Person {name: 'Ada'})")), std::runtime_error);
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsMalformedCreateChainsAndInvalidStagedReferencesBeforePublishing) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		logical::LogicalSingleRow seed;

		// A claimed CREATE type without the corresponding concrete operator, and a
		// non-unary CREATE shape, must never be interpreted as a writable plan.
		MalformedLogicalOperator mismatchedCreate(logical::LogicalOpType::LOP_CREATE_NODE, {&seed});
		MalformedLogicalOperator binaryCreate(logical::LogicalOpType::LOP_CREATE_NODE, {&seed, &seed});
		EXPECT_THROW(static_cast<void>(runtime.execute(mismatchedCreate, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		EXPECT_THROW(static_cast<void>(runtime.execute(binaryCreate, nativeMutationBounds(), QueryContext{})),
					 std::runtime_error);

		// Empty update clauses cannot be made meaningful by a preceding CREATE.
		// Reject them while collecting the write chain, before a snapshot or a
		// mutation batch is opened.
		logical::LogicalSet emptySet({}, std::make_unique<logical::LogicalCreateNode>(
				"created", std::vector<std::string>{"Person"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>()));
		EXPECT_THROW(static_cast<void>(runtime.execute(emptySet, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		logical::LogicalRemove emptyRemove({}, std::make_unique<logical::LogicalCreateNode>(
				"removed", std::vector<std::string>{"Person"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>()));
		EXPECT_THROW(static_cast<void>(runtime.execute(emptyRemove, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);

		std::unordered_map<std::string, std::shared_ptr<expressions::Expression>> duplicateExpression;
		duplicateExpression.emplace("name", std::make_shared<expressions::LiteralExpression>(PropertyValue("Grace")));
		logical::LogicalCreateNode duplicateProperty(
				"person", std::vector<std::string>{"Person"}, {{"name", PropertyValue("Ada")}},
				std::move(duplicateExpression), std::make_unique<logical::LogicalSingleRow>());
		EXPECT_THROW(static_cast<void>(runtime.execute(duplicateProperty, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		auto first = std::make_unique<logical::LogicalCreateNode>(
				"first", std::vector<std::string>{"Person"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		auto second = std::make_unique<logical::LogicalCreateNode>(
				"second", std::vector<std::string>{"Person"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{}, std::move(first));
		logical::LogicalCreateEdge invalidDirection("relationship", "KNOWS", PropertyMap{}, "first", "second",
				std::move(second), "sideways");
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidDirection, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		// A surviving relationship cannot retain a reference to a CREATE that the
		// same statement deletes; rejection happens while the batch is still local.
		auto source = std::make_unique<logical::LogicalCreateNode>(
				"source", std::vector<std::string>{"Person"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		auto target = std::make_unique<logical::LogicalCreateNode>(
				"target", std::vector<std::string>{"Person"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{}, std::move(source));
		auto relationship = std::make_unique<logical::LogicalCreateEdge>(
				"relationship", "KNOWS", PropertyMap{}, "source", "target", std::move(target));
		logical::LogicalDelete deleteSource({"source"}, false, std::move(relationship));
		EXPECT_THROW(static_cast<void>(runtime.execute(deleteSource, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		EXPECT_EQ(backend->countNodesWithLabel("Person"), 0U);
		EXPECT_EQ(backend->edgeCount(), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsMalformedMutationEntryPlansBeforeOpeningOrPublishing) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		logical::LogicalSingleRow seed;

		// External/native callers can construct logical plans directly. A claimed
		// operator type must never bypass its concrete-shape validation and reach a
		// mutation publication path.
		MalformedLogicalOperator malformedForeach(logical::LogicalOpType::LOP_FOREACH, {});
		EXPECT_THROW(static_cast<void>(runtime.execute(malformedForeach, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		MalformedLogicalOperator malformedProject(logical::LogicalOpType::LOP_PROJECT, {&seed});
		EXPECT_THROW(static_cast<void>(runtime.execute(malformedProject, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		MalformedLogicalOperator malformedCall(logical::LogicalOpType::LOP_CALL_SUBQUERY, {&seed});
		EXPECT_THROW(static_cast<void>(runtime.execute(malformedCall, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		// A write-only CALL body must contain a CREATE operation. It is rejected
		// before the body is rebound to an outer input or a write batch is staged.
		auto setOnlyBody = std::make_unique<logical::LogicalSet>(
				std::vector<logical::LogicalSetItem>{
						{logical::SetActionType::LSET_PROPERTY, "n", "value",
						 std::make_shared<expressions::LiteralExpression>(PropertyValue(2))}},
				std::make_unique<logical::LogicalSingleRow>());
		logical::LogicalCallSubquery setOnlyCall(nullptr, std::move(setOnlyBody), {}, {}, false, 0);
		EXPECT_THROW(static_cast<void>(runtime.execute(setOnlyCall, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		// An unknown enum value is invalid public-plan input, not a silent no-op.
		logical::LogicalSet invalidSet(
				{{static_cast<logical::SetActionType>(99), "n", "value",
				  std::make_shared<expressions::LiteralExpression>(PropertyValue(2))}},
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Seed"}));
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidSet, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		logical::LogicalRemove invalidRemove(
				{{static_cast<logical::LogicalRemoveActionType>(99), "n", "value"}},
				std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Seed"}));
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidRemove, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		EXPECT_EQ(backend->graph().mutation_batches, 0U);
		EXPECT_TRUE(backend->graph().nodes.empty());
	}

} // namespace graph::query
