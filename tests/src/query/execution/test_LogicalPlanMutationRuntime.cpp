/**
 * @file test_LogicalPlanMutationRuntime.cpp
 * @brief End-to-end tests for bounded logical-plan mutations.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "query/execution/LogicalPlanMutationRuntimeTestFixture.hpp"

#include "graph/query/logical/operators/LogicalTraversal.hpp"

namespace graph::query {

	TEST(LogicalPlanMutationRuntimeTest, MergeUsesOneAtomicBatchAndSelectsOnCreateOrOnMatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult created =
				engine.execute("MERGE (n:Person {name: 'Ada'}) ON CREATE SET n.created = 1, n.copied = n.name "
							   "RETURN n.name AS name, n.created AS created, n.copied AS copied");
		ASSERT_EQ(created.rowCount(), 1U);
		EXPECT_EQ(std::get<std::string>(created.getRows().front().at("name").asPrimitive().getVariant()), "Ada");
		EXPECT_EQ(std::get<int64_t>(created.getRows().front().at("created").asPrimitive().getVariant()), 1);
		EXPECT_EQ(std::get<std::string>(created.getRows().front().at("copied").asPrimitive().getVariant()), "Ada");
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);

		QueryResult matched = engine.execute("MERGE (n:Person {name: 'Ada'}) ON MATCH SET n.seen = true RETURN "
											 "n.created + 1 AS created, n.seen AS seen");
		ASSERT_EQ(matched.rowCount(), 1U);
		EXPECT_EQ(std::get<int64_t>(matched.getRows().front().at("created").asPrimitive().getVariant()), 2);
		EXPECT_TRUE(std::get<bool>(matched.getRows().front().at("seen").asPrimitive().getVariant()));
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 2U);

		const QueryResult noOp = engine.execute(
				"MERGE (n:Person {name: 'Ada'}) ON MATCH SET n.seen = true, n.missing = null RETURN n");
		ASSERT_EQ(noOp.rowCount(), 1U);
		const LogicalNodeResult &unchanged = noOp.getRows().front().at("n").asLogicalNode();
		EXPECT_TRUE(std::get<bool>(unchanged.properties.at("seen").getVariant()));
		EXPECT_FALSE(unchanged.properties.contains("missing"));
		EXPECT_EQ(backend->graph().mutation_batches, 2U);

		QueryResult combined =
				engine.execute("MATCH (n:Person {name: 'Ada'}) SET n.rank = 7, n:Visible RETURN n.rank AS rank");
		ASSERT_EQ(combined.rowCount(), 1U);
		EXPECT_EQ(std::get<int64_t>(combined.getRows().front().at("rank").asPrimitive().getVariant()), 7);
		EXPECT_EQ(backend->countNodesWithLabel("Visible"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 3U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeReevaluatesTheEntireDecisionAfterAnOptimisticConflict) {
		auto backend = std::make_shared<TestLogicalGraph>();
		backend->rejectNextMutation();
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute("MERGE (n:Person {name: 'Ada'}) RETURN n");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_TRUE(result.getRows().front().at("n").isLogicalNode());
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachDeletesPersistedEntitiesInOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId = backend->addNode({"Ephemeral"}, {});
		QueryEngine engine(backend, backend);

		EXPECT_NO_THROW(static_cast<void>(
				engine.execute("MATCH (node:Ephemeral) FOREACH (value IN [1] | DELETE node)")));

		EXPECT_FALSE(backend->graph().nodes.contains(nodeId));
		EXPECT_EQ(backend->countNodesWithLabel("Ephemeral"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CountsRelationshipsThroughDirectAndAnchoredReadPlans) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"CREATE (ada:Person {name: 'Ada'}), (bob:Person {name: 'Bob'}), (cora:Person {name: 'Cora'})"));
		static_cast<void>(engine.execute(
				"MATCH (ada:Person {name: 'Ada'}), (bob:Person {name: 'Bob'}), (cora:Person {name: 'Cora'}) "
				"CREATE (ada)-[:KNOWS]->(bob), (ada)-[:KNOWS]->(cora), (ada)-[:KNOWS]->(ada)"));

		const QueryResult direct =
				engine.execute("MATCH ()-[relationship:KNOWS]->() RETURN count(relationship) AS relationshipCount");
		ASSERT_EQ(direct.rowCount(), 1U);
		EXPECT_EQ(direct.getRows().front().at("relationshipCount").asPrimitive(), PropertyValue(3));

		const QueryResult anchored = engine.execute(
				"MATCH (source:Person {name: 'Ada'})-[relationship:KNOWS]-(target:Person) "
				"RETURN count(target) AS targetCount");
		ASSERT_EQ(anchored.rowCount(), 1U);
		// The self-loop is visible in both directions but is one logical match.
		EXPECT_EQ(anchored.getRows().front().at("targetCount").asPrimitive(), PropertyValue(3));
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeDeduplicatesCreatedPatternsAcrossInputRowsAndMergesRelationships) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {}));
		static_cast<void>(backend->addNode({"Seed"}, {}));
		QueryEngine engine(backend, backend);

		QueryResult shared = engine.execute("MATCH (s:Seed) MERGE (n:Person {name: 'Ada'}) RETURN n.name AS name");
		ASSERT_EQ(shared.rowCount(), 2U);
		EXPECT_EQ(std::get<std::string>(shared.getRows()[0].at("name").asPrimitive().getVariant()), "Ada");
		EXPECT_EQ(std::get<std::string>(shared.getRows()[1].at("name").asPrimitive().getVariant()), "Ada");
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 1U);

		QueryResult created =
				engine.execute("MERGE (a:Person {name: 'Ada'})-[r:KNOWS {since: 2026}]->(b:Person {name: 'Bob'}) "
							   "ON CREATE SET r.created = true, r.copied = r.since RETURN a, r, b");
		ASSERT_EQ(created.rowCount(), 1U);
		const auto &edge = created.getRows().front().at("r").asLogicalEdge();
		EXPECT_EQ(edge.type, "KNOWS");
		EXPECT_EQ(edge.properties.at("since"), PropertyValue(int64_t(2026)));
		EXPECT_EQ(edge.properties.at("created"), PropertyValue(true));
		EXPECT_EQ(edge.properties.at("copied"), PropertyValue(int64_t(2026)));
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 2U);
		EXPECT_EQ(backend->edgeCount(), 1U);

		QueryResult matched =
				engine.execute("MERGE (a:Person {name: 'Ada'})-[r:KNOWS {since: 2026}]->(b:Person {name: 'Bob'}) "
							   "ON MATCH SET r.seen = true RETURN r.seen AS seen");
		ASSERT_EQ(matched.rowCount(), 1U);
		EXPECT_TRUE(std::get<bool>(matched.getRows().front().at("seen").asPrimitive().getVariant()));
		EXPECT_EQ(backend->edgeCount(), 1U);

		QueryResult incoming =
				engine.execute("MERGE (b:Person {name: 'Bob'})<-[r:KNOWS {since: 2026}]-(a:Person {name: 'Ada'}) "
							   "ON MATCH SET r.incoming = true RETURN r.incoming AS incoming");
		ASSERT_EQ(incoming.rowCount(), 1U);
		EXPECT_TRUE(std::get<bool>(incoming.getRows().front().at("incoming").asPrimitive().getVariant()));
		EXPECT_EQ(backend->edgeCount(), 1U);

		QueryResult either =
				engine.execute("MERGE (a:Person {name: 'Ada'})-[r:KNOWS {since: 2026}]-(b:Person {name: 'Bob'}) "
							   "ON MATCH SET r.either = true RETURN r.either AS either");
		ASSERT_EQ(either.rowCount(), 1U);
		EXPECT_TRUE(std::get<bool>(either.getRows().front().at("either").asPrimitive().getVariant()));
		EXPECT_EQ(backend->edgeCount(), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeUndirectedRelationshipFallsBackToIncomingAdjacency) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Person"}, {{"name", PropertyValue("Ada")}}));
		static_cast<void>(backend->addNode({"Person"}, {{"name", PropertyValue("Bob")}}));
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"MATCH (a:Person {name: 'Ada'}), (b:Person {name: 'Bob'}) CREATE (a)-[:KNOWS {since: 2026}]->(b)"));
		QueryResult result = engine.execute(
				"MERGE (a:Person {name: 'Bob'})-[r:KNOWS {since: 2026}]-(b:Person {name: 'Ada'}) RETURN r");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalEdgeResult &edge = result.getRows().front().at("r").asLogicalEdge();
		EXPECT_EQ(edge.type, "KNOWS");
		EXPECT_EQ(edge.properties.at("since"), PropertyValue(int64_t(2026)));
		EXPECT_EQ(backend->edgeCount(), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeOnMatchMapPatchReplacesAndMaterializesRelationshipProperties) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"CREATE (source:Source {name: 'source'})-"
				"[:LINK {weight: 1, stale: true}]->(target:Target {name: 'target'})"));
		const size_t batchesBefore = backend->graph().mutation_batches;

		const QueryResult result = engine.execute(
				"MATCH (source:Source {name: 'source'}), (target:Target {name: 'target'}) "
				"MERGE (source)-[relationship:LINK]->(target) "
				"ON MATCH SET relationship += {weight: 2, stale: null} RETURN relationship");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalEdgeResult &relationship = result.getRows().front().at("relationship").asLogicalEdge();
		EXPECT_EQ(relationship.type, "LINK");
		EXPECT_EQ(relationship.properties.at("weight"), PropertyValue(2));
		EXPECT_FALSE(relationship.properties.contains("stale"));
		EXPECT_EQ(backend->graph().edges.at(relationship.edge_id).properties, relationship.properties);
		EXPECT_EQ(backend->graph().mutation_batches, batchesBefore + 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergePostMutationsSynchronizeCreatedRelationshipProperties) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		const QueryResult result = engine.execute(
				"MERGE (source:Source {name: 'source'})-"
				"[relationship:LINK {weight: 1}]->(target:Target {name: 'target'}) "
				"SET relationship += {weight: 2, added: true, transient: 'remove'} "
				"REMOVE relationship.transient RETURN relationship");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalEdgeResult &relationship = result.getRows().front().at("relationship").asLogicalEdge();
		EXPECT_EQ(relationship.properties.at("weight"), PropertyValue(2));
		EXPECT_EQ(relationship.properties.at("added"), PropertyValue(true));
		EXPECT_FALSE(relationship.properties.contains("transient"));
		EXPECT_EQ(backend->graph().edges.at(relationship.edge_id).properties, relationship.properties);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeUndirectedRelationshipSkipsItsSelfLoopBeforeFindingIncomingMatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Person"}, {{"name", PropertyValue("Ada")}}));
		static_cast<void>(backend->addNode({"Person"}, {{"name", PropertyValue("Bob")}}));
		QueryEngine engine(backend, backend);

		// The self-loop is visible through both adjacency directions. An undirected
		// MERGE must not inspect it twice before finding Bob's incoming relationship.
		static_cast<void>(engine.execute(
				"MATCH (ada:Person {name: 'Ada'}), (bob:Person {name: 'Bob'}) "
				"CREATE (ada)-[:OTHER]->(ada), (bob)-[:KNOWS {since: 2026}]->(ada)"));
		const QueryResult result = engine.execute(
				"MATCH (ada:Person {name: 'Ada'}), (bob:Person {name: 'Bob'}) "
				"MERGE (ada)-[relationship:KNOWS {since: 2026}]-(bob) RETURN relationship");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalEdgeResult &edge = result.getRows().front().at("relationship").asLogicalEdge();
		EXPECT_EQ(edge.type, "KNOWS");
		EXPECT_EQ(edge.properties.at("since"), PropertyValue(int64_t(2026)));
		EXPECT_EQ(backend->edgeCount(), 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeIncomingSelfLoopRemainsVisibleToTheIncomingDirection) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Person"}, {{"name", PropertyValue("Ada")}}));
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute("MATCH (node:Person {name: 'Ada'}) CREATE (node)-[:LOOP]->(node)"));
		const QueryResult result = engine.execute(
				"MATCH (node:Person {name: 'Ada'}) MERGE (node)<-[relationship:LOOP]-(node) RETURN relationship");

		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("relationship").asLogicalEdge().type, "LOOP");
		EXPECT_EQ(backend->edgeCount(), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergePreservesExistingRelationshipInputBindings) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"CREATE (source:Seed {name: 'source'})-[existing:EXISTING {weight: 7}]->(target:Seed {name: 'target'})"));

		const QueryResult created = engine.execute(
				"MATCH (source:Seed)-[existing:EXISTING]->(target:Seed) "
				"MERGE (source)-[merged:MERGED]->(target) RETURN existing, merged");
		ASSERT_EQ(created.rowCount(), 1U);
		const LogicalEdgeResult &existing = created.getRows().front().at("existing").asLogicalEdge();
		const LogicalEdgeResult &merged = created.getRows().front().at("merged").asLogicalEdge();
		EXPECT_EQ(existing.type, "EXISTING");
		EXPECT_EQ(existing.properties.at("weight"), PropertyValue(int64_t(7)));
		EXPECT_EQ(merged.type, "MERGED");
		EXPECT_EQ(backend->edgeCount(), 2U);

		const QueryResult matched = engine.execute(
				"MATCH (source:Seed)-[existing:EXISTING]->(target:Seed) "
				"MERGE (source)-[merged:MERGED]->(target) RETURN existing, merged");
		ASSERT_EQ(matched.rowCount(), 1U);
		EXPECT_EQ(matched.getRows().front().at("existing").asLogicalEdge().type, "EXISTING");
		EXPECT_EQ(matched.getRows().front().at("merged").asLogicalEdge().type, "MERGED");
		EXPECT_EQ(backend->edgeCount(), 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateIncomingRelationshipWritesTheReversedLogicalEndpoints) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		const QueryResult created = engine.execute(
				"CREATE (left:Left {name: 'left'})<-[edge:POINTS]-(right:Right {name: 'right'}) RETURN edge");
		ASSERT_EQ(created.rowCount(), 1U);
		const LogicalEdgeResult &edge = created.getRows().front().at("edge").asLogicalEdge();
		EXPECT_EQ(edge.type, "POINTS");
		EXPECT_EQ(backend->edgeCount(), 1U);

		const QueryResult incoming = engine.execute(
				"MATCH (left:Left)<-[:POINTS]-(right:Right) RETURN left.name AS left, right.name AS right");
		ASSERT_EQ(incoming.rowCount(), 1U);
		EXPECT_EQ(incoming.getRows().front().at("left").asPrimitive(), PropertyValue("left"));
		EXPECT_EQ(incoming.getRows().front().at("right").asPrimitive(), PropertyValue("right"));

		const QueryResult reverse = engine.execute("MATCH (:Left)-[:POINTS]->(:Right) RETURN 1 AS wrong_direction");
		EXPECT_EQ(reverse.rowCount(), 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, UnwindCreatesOneLogicalNodePerInputValueInOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult result =
				engine.execute("UNWIND [4, 5, 6] AS value CREATE (n:Item {value: value}) RETURN n.value AS value");
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue(4));
		EXPECT_EQ(result.getRows()[1].at("value").asPrimitive(), PropertyValue(5));
		EXPECT_EQ(result.getRows()[2].at("value").asPrimitive(), PropertyValue(6));
		EXPECT_EQ(backend->countNodesWithLabel("Item"), 3U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateDistinctMaterializesLogicalNodeAndRelationshipIdentity) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t sourceId = backend->addNode({"Source"}, {});
		const uint64_t targetId = backend->addNode({"Target"}, {});
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"MATCH (source:Source), (target:Target) CREATE (source)-[:LINK]->(target)"));
		const QueryResult nodeResult = engine.execute(
				"MATCH (node:Source) UNWIND [1, 2] AS copy "
				"CREATE (:DistinctNodeChild {copy: copy}) RETURN DISTINCT node");
		ASSERT_EQ(nodeResult.rowCount(), 1U);
		EXPECT_EQ(nodeResult.getRows().front().at("node").asLogicalNode().node_id, sourceId);
		// DISTINCT reshapes only the result: every input row remains part of the
		// one atomic CREATE publication.
		EXPECT_EQ(backend->countNodesWithLabel("DistinctNodeChild"), 2U);

		const QueryResult edgeResult = engine.execute(
				"MATCH ()-[relationship:LINK]->() UNWIND [1, 2] AS copy "
				"CREATE (:DistinctRelationshipChild {copy: copy}) RETURN DISTINCT relationship");
		ASSERT_EQ(edgeResult.rowCount(), 1U);
		const LogicalEdgeResult &relationship = edgeResult.getRows().front().at("relationship").asLogicalEdge();
		EXPECT_EQ(relationship.source_node_id, sourceId);
		EXPECT_EQ(relationship.target_node_id, targetId);
		EXPECT_EQ(relationship.type, "LINK");
		EXPECT_EQ(backend->countNodesWithLabel("DistinctRelationshipChild"), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 3U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateDistinctRetainsDifferentLogicalEntityIdentities) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t firstSource = backend->addNode({"Source"}, {});
		const uint64_t secondSource = backend->addNode({"Source"}, {});
		QueryEngine engine(backend, backend);

		const QueryResult nodeResult = engine.execute(
				"MATCH (node:Source) CREATE (:DistinctNodeChild) RETURN DISTINCT node");
		ASSERT_EQ(nodeResult.rowCount(), 2U);
		std::set<uint64_t> nodeIds;
		for (const QueryResult::Row &row: nodeResult.getRows()) {
			nodeIds.insert(row.at("node").asLogicalNode().node_id);
		}
		EXPECT_EQ(nodeIds, (std::set<uint64_t>{firstSource, secondSource}));
		EXPECT_EQ(backend->countNodesWithLabel("DistinctNodeChild"), 2U);

		static_cast<void>(engine.execute(
				"MATCH (source:Source) CREATE (source)-[:LINK]->(:Target)"));
		const QueryResult relationshipResult =
				engine.execute("MATCH ()-[relationship:LINK]->() CREATE (:DistinctRelationshipChild) "
							   "RETURN DISTINCT relationship");
		ASSERT_EQ(relationshipResult.rowCount(), 2U);
		std::set<uint64_t> relationshipIds;
		for (const QueryResult::Row &row: relationshipResult.getRows()) {
			relationshipIds.insert(row.at("relationship").asLogicalEdge().edge_id);
		}
		EXPECT_EQ(relationshipIds.size(), 2U);
		EXPECT_EQ(backend->countNodesWithLabel("DistinctRelationshipChild"), 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateDeleteRemapsTheSurvivingRelationshipResult) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		const QueryResult result = engine.execute(
				"CREATE (source:Keep), (target:Keep), (discarded:Discard), "
				"(discarded)-[temporary:TEMPORARY]->(discarded), (source)-[link:LINK]->(target) "
				"DELETE temporary, discarded RETURN link");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalEdgeResult &link = result.getRows().front().at("link").asLogicalEdge();
		EXPECT_EQ(link.type, "LINK");
		EXPECT_EQ(backend->edgeCount(), 1U);
		EXPECT_TRUE(backend->graph().edges.contains(link.edge_id));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, WriteOnlyCallSubqueryRebindsEveryOuterRowIntoOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t firstSeed = backend->addNode({"Seed"}, {});
		const uint64_t secondSeed = backend->addNode({"Seed"}, {});
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute("MATCH (seed:Seed) CALL { CREATE (:Child {created: true}) } RETURN seed");

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("seed").asLogicalNode().node_id, firstSeed);
		EXPECT_EQ(result.getRows()[1].at("seed").asLogicalNode().node_id, secondSeed);
		EXPECT_EQ(backend->countNodesWithLabel("Child"), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, WriteOnlyCallSubqueryFoldsCreateSetAndRemoveIntoOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"value", PropertyValue(9)}});
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"MATCH (seed:Seed) CALL { CREATE (child:Child {copied: seed.value, temporary: true}) "
				"SET child.doubled = child.copied * 2 REMOVE child.temporary } RETURN seed");

		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("seed").asLogicalNode().node_id, seedId);
		ASSERT_EQ(backend->countNodesWithLabel("Child"), 1U);
		const TestNode &child = std::find_if(backend->graph().nodes.begin(), backend->graph().nodes.end(),
										  [](const auto &entry) {
											  return std::ranges::find(entry.second.labels, "Child") != entry.second.labels.end();
										  })
										  ->second;
		EXPECT_EQ(child.properties.at("copied"), PropertyValue(9));
		EXPECT_EQ(child.properties.at("doubled"), PropertyValue(18));
		EXPECT_FALSE(child.properties.contains("temporary"));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, WriteOnlyCallSubqueryCreatesAndDeletesGraphEntitiesAtomically) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t source = backend->addNode({"Source"}, {});
		const uint64_t target = backend->addNode({"Target"}, {});
		QueryEngine engine(backend, backend);

		const QueryResult created = engine.execute(
				"MATCH (source:Source), (target:Target) "
				"CALL { WITH source, target CREATE (source)-[:CALL_LINK]->(target) } RETURN source, target");
		ASSERT_EQ(created.rowCount(), 1U);
		ASSERT_EQ(backend->edgeCount(), 1U);
		const TestEdge &edge = backend->graph().edges.begin()->second;
		EXPECT_EQ(edge.source, source);
		EXPECT_EQ(edge.target, target);
		EXPECT_EQ(edge.type, "CALL_LINK");
		EXPECT_EQ(backend->graph().mutation_batches, 1U);

		static_cast<void>(engine.execute(
				"MATCH ()-[relationship:CALL_LINK]->() CALL { WITH relationship DELETE relationship }"));
		EXPECT_TRUE(backend->graph().nodes.contains(source));
		EXPECT_TRUE(backend->graph().nodes.contains(target));
		EXPECT_EQ(backend->edgeCount(), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallUsesConfiguredIndependentPublicationBatches) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(3)}}));
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(4)}}));
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(5)}}));
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"MATCH (seed:Seed) CALL { WITH seed CREATE (:BatchedChild {copied: seed.value}) } "
				"IN TRANSACTIONS OF 2 ROWS RETURN seed.value AS value");

		ASSERT_EQ(result.rowCount(), 3U);
		std::vector<PropertyValue> values;
		for (const QueryResult::Row &row: result.getRows()) {
			values.push_back(row.at("value").asPrimitive());
		}
		std::ranges::sort(values);
		EXPECT_EQ(values, (std::vector<PropertyValue>{PropertyValue(3), PropertyValue(4), PropertyValue(5)}));
		EXPECT_EQ(backend->countNodesWithLabel("BatchedChild"), 3U);
		EXPECT_EQ(backend->graph().mutation_batches, 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallUsesOneDefaultBatchForBoundedInput) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(3)}}));
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(4)}}));
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(5)}}));
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"MATCH (seed:Seed) CALL { WITH seed CREATE (:DefaultBatchedChild {copied: seed.value}) } "
				"IN TRANSACTIONS RETURN seed.value AS value");

		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(backend->countNodesWithLabel("DefaultBatchedChild"), 3U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallExportsCreatedValuesToItsOuterProjection) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(3)}}));
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(4)}}));
		QueryEngine engine(backend, backend);

		const QueryResult result = engine.execute(
				"MATCH (seed:Seed) "
				"CALL { WITH seed CREATE (child:ExportedBatchChild {copied: seed.value}) "
				"RETURN child.copied AS copied } IN TRANSACTIONS OF 1 ROWS "
				"RETURN copied");

		ASSERT_EQ(result.rowCount(), 2U);
		std::vector<PropertyValue> copiedValues;
		for (const QueryResult::Row &row: result.getRows()) {
			copiedValues.push_back(row.at("copied").asPrimitive());
		}
		std::ranges::sort(copiedValues);
		EXPECT_EQ(copiedValues, (std::vector<PropertyValue>{PropertyValue(3), PropertyValue(4)}));
		EXPECT_EQ(backend->countNodesWithLabel("ExportedBatchChild"), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallReturnsItsExportedColumnsWithoutAnOuterProjection) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		const QueryResult result = engine.execute(
				"UNWIND [3, 4] AS value "
				"CALL { WITH value CREATE (child:StandaloneBatchChild {copied: value}) "
				"RETURN child.copied AS copied } IN TRANSACTIONS OF 1 ROWS");

		ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"copied"}));
		ASSERT_EQ(result.rowCount(), 2U);
		std::vector<PropertyValue> copiedValues;
		for (const QueryResult::Row &row: result.getRows()) {
			copiedValues.push_back(row.at("copied").asPrimitive());
		}
		std::ranges::sort(copiedValues);
		EXPECT_EQ(copiedValues, (std::vector<PropertyValue>{PropertyValue(3), PropertyValue(4)}));
		EXPECT_EQ(backend->countNodesWithLabel("StandaloneBatchChild"), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallPreservesNodeOuterRows) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t firstSeed = backend->addNode({"Seed"}, {{"value", PropertyValue(3)}});
		const uint64_t secondSeed = backend->addNode({"Seed"}, {{"value", PropertyValue(4)}});
		QueryEngine engine(backend, backend);

		const QueryResult result = engine.execute(
				"MATCH (seed:Seed) CALL { WITH seed CREATE (:NodeBatchChild {copied: seed.value}) } "
				"IN TRANSACTIONS OF 1 ROWS RETURN seed");

		ASSERT_EQ(result.rowCount(), 2U);
		std::set<uint64_t> returnedIds;
		for (const QueryResult::Row &row: result.getRows()) {
			const LogicalNodeResult &seed = row.at("seed").asLogicalNode();
			returnedIds.insert(seed.node_id);
		}
		EXPECT_EQ(returnedIds, (std::set<uint64_t>{firstSeed, secondSeed}));
		EXPECT_EQ(backend->countNodesWithLabel("NodeBatchChild"), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallPreservesRelationshipOuterRows) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t sourceId = backend->addNode({"Source"}, {});
		const uint64_t targetId = backend->addNode({"Target"}, {});
		QueryEngine engine(backend, backend);
		static_cast<void>(engine.execute(
				"MATCH (source:Source), (target:Target) CREATE (source)-[:LINK {weight: 7}]->(target)"));

		auto input = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source",
				"relationship", "target", "LINK", "out", std::vector<std::string>{"Target"});
		auto import = std::make_unique<logical::LogicalProject>(
				std::make_unique<logical::LogicalSingleRow>(),
				std::vector<logical::LogicalProjectItem>{
						{std::make_shared<expressions::VariableReferenceExpression>("relationship"), "relationship"}});
		auto body = std::make_unique<logical::LogicalCreateNode>(
				"child", std::vector<std::string>{"RelationshipBatchChild"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{}, std::move(import));
		auto call = std::make_unique<logical::LogicalCallSubquery>(
				std::move(input), std::move(body), std::vector<std::string>{"relationship"}, std::vector<std::string>{},
				true, 1);
		logical::LogicalProject outer(
				std::move(call), std::vector<logical::LogicalProjectItem>{
						{std::make_shared<expressions::VariableReferenceExpression>("relationship"), "relationship"}});

		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const QueryResult result = runtime.execute(outer, nativeMutationBounds(), QueryContext{});

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalEdgeResult &relationship = result.getRows().front().at("relationship").asLogicalEdge();
		EXPECT_EQ(relationship.type, "LINK");
		EXPECT_EQ(relationship.properties.at("weight"), PropertyValue(7));
		EXPECT_EQ(relationship.source_node_id, sourceId);
		EXPECT_EQ(relationship.target_node_id, targetId);
		EXPECT_EQ(backend->countNodesWithLabel("RelationshipBatchChild"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallWithNoInputRowsDoesNotPublishAnEmptyBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		const QueryResult result = engine.execute(
				"MATCH (seed:Missing) CALL { WITH seed CREATE (:UnreachableBatch {copied: seed.value}) } "
				"IN TRANSACTIONS OF 1 ROWS RETURN seed.value AS value");

		EXPECT_EQ(result.rowCount(), 0U);
		EXPECT_EQ(result.getPipelineRowCount(), 0U);
		EXPECT_EQ(backend->countNodesWithLabel("UnreachableBatch"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, WriteOnlyCallAppliesImportedSetAndRemoveUpdatesAtomically) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId =
				backend->addNode({"Seed"}, {{"value", PropertyValue(4)}, {"obsolete", PropertyValue(true)}});
		QueryEngine engine(backend, backend);

		const QueryResult result = engine.execute(
				"MATCH (seed:Seed) "
				"CALL { WITH seed SET seed.value = seed.value + 1, seed:Processed, seed += {merged: 'yes'} "
				"REMOVE seed.obsolete } "
				"RETURN seed.value AS value");

		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(5));
		const TestNode &stored = backend->graph().nodes.at(nodeId);
		EXPECT_EQ(stored.properties.at("value"), PropertyValue(5));
		EXPECT_EQ(stored.properties.at("merged"), PropertyValue("yes"));
		EXPECT_FALSE(stored.properties.contains("obsolete"));
		EXPECT_TRUE(std::ranges::find(stored.labels, "Processed") != stored.labels.end());
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallAppliesImportedSetAndRemoveUpdatesPerBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t first =
				backend->addNode({"Seed"}, {{"value", PropertyValue(2)}, {"obsolete", PropertyValue(true)}});
		const uint64_t second =
				backend->addNode({"Seed"}, {{"value", PropertyValue(3)}, {"obsolete", PropertyValue(true)}});
		const uint64_t third =
				backend->addNode({"Seed"}, {{"value", PropertyValue(5)}, {"obsolete", PropertyValue(true)}});
		QueryEngine engine(backend, backend);

		const QueryResult result = engine.execute(
				"MATCH (seed:Seed) "
				"CALL { WITH seed SET seed.value = seed.value * 2, seed:Processed REMOVE seed.obsolete } "
				"IN TRANSACTIONS OF 2 ROWS RETURN seed.value AS value");

		ASSERT_EQ(result.rowCount(), 3U);
		std::vector<PropertyValue> values;
		for (const QueryResult::Row &row: result.getRows()) {
			values.push_back(row.at("value").asPrimitive());
		}
		std::ranges::sort(values);
		// Batched CALL preserves its outer input rows for the following projection;
		// the mutation itself is published once each batch has completed.
		EXPECT_EQ(values, (std::vector<PropertyValue>{PropertyValue(2), PropertyValue(3), PropertyValue(5)}));
		for (const auto &[nodeId, expectedValue]:
				 std::initializer_list<std::pair<uint64_t, PropertyValue>>{{first, PropertyValue(4)},
																											 {second, PropertyValue(6)},
																											 {third, PropertyValue(10)}}) {
			const TestNode &stored = backend->graph().nodes.at(nodeId);
			EXPECT_EQ(stored.properties.at("value"), expectedValue);
			EXPECT_FALSE(stored.properties.contains("obsolete"));
			EXPECT_TRUE(std::ranges::find(stored.labels, "Processed") != stored.labels.end());
		}
		EXPECT_EQ(backend->graph().mutation_batches, 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallUsesAnImplicitSingleRowWhenItsInputIsOmitted) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		auto body = std::make_unique<logical::LogicalCreateNode>(
				"created", std::vector<std::string>{"ImplicitBatch"},
				PropertyMap{{"value", PropertyValue(7)}},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		logical::LogicalCallSubquery call(nullptr, std::move(body), std::vector<std::string>{},
				std::vector<std::string>{}, true, 1);

		const QueryResult result = runtime.execute(call, nativeMutationBounds(), QueryContext{});
		EXPECT_EQ(result.getPipelineRowCount(), 1U);
		EXPECT_EQ(result.rowCount(), 0U);
		ASSERT_EQ(backend->countNodesWithLabel("ImplicitBatch"), 1U);
		EXPECT_EQ(backend->graph().nodes.begin()->second.properties.at("value"), PropertyValue(7));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallKeepsCompletedBatchesWhenALaterBatchFailsAtomically) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(1)}}));
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(2)}}));
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(0)}}));
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (seed:Seed) CALL { WITH seed CREATE (:BatchFailure {quotient: 12 / seed.value}) } "
					 "IN TRANSACTIONS OF 2 ROWS")),
			 std::runtime_error);

		EXPECT_EQ(backend->countNodesWithLabel("BatchFailure"), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallPreflightsOuterProjectionBeforePublishingAnyBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(1)}}));
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(2)}}));
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (seed:Seed) CALL { WITH seed CREATE (:ProjectionFailure {copied: seed.value}) } "
					 "IN TRANSACTIONS OF 1 ROW RETURN 1 / 0 AS invalid")),
			 std::runtime_error);

		EXPECT_EQ(backend->countNodesWithLabel("ProjectionFailure"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallKeepsCompletedBatchesWhenCancellationPreventsTheNextPublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(1)}}));
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(2)}}));
		backend->setDelayAfterMutationBatch(std::chrono::milliseconds(250));
		QueryEngine engine(backend, backend);

		// The first independent batch publishes before its deadline. Its backend
		// completion deliberately crosses that deadline, so the immediate guard
		// at the second batch's publication boundary must reject only that batch.
		QueryGuard guard(200, 0);
		QueryContext context;
		context.guard = &guard;
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (seed:Seed) CALL { WITH seed CREATE (:CancelledBatch {copied: seed.value}) } "
					 "IN TRANSACTIONS OF 1 ROWS", context)),
				 QueryTimeoutException);

		EXPECT_EQ(backend->countNodesWithLabel("CancelledBatch"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateDoesNotPublishWhenItsReturnExpressionFails) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CREATE (:AtomicCreate {value: 1}) RETURN 1 / 0 AS invalidResult")),
				 std::runtime_error);
		EXPECT_EQ(backend->countNodesWithLabel("AtomicCreate"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateCachesPrepublicationScalarResultsButReturnsDurableEntityIds) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);
		registerCountingMergeResultExpression();
		mergeResultExpressionCalls.store(0, std::memory_order_relaxed);

		const QueryResult result = engine.execute(
				"CREATE (item:CachedCreate {value: 1}) "
				"RETURN id(item) AS itemId, testMergeResultCounter() AS counter");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("itemId").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(result.getRows().front().at("counter").asPrimitive(), PropertyValue(0));
		EXPECT_EQ(mergeResultExpressionCalls.load(std::memory_order_relaxed), 1);
	}

	TEST(LogicalPlanMutationRuntimeTest, SetAndDeleteDoNotPublishWhenTheirReturnExpressionsFail) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t setTarget = backend->addNode({"AtomicSet"}, {{"value", PropertyValue(1)}});
		const uint64_t deleteTarget = backend->addNode({"AtomicDelete"}, {{"value", PropertyValue(2)}});
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (node:AtomicSet) SET node.value = 3 RETURN 1 / 0 AS invalidResult")),
				 std::runtime_error);
		EXPECT_EQ(backend->graph().nodes.at(setTarget).properties.at("value"), PropertyValue(1));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (node:AtomicDelete) DELETE node RETURN 1 / 0 AS invalidResult")),
				 std::runtime_error);
		EXPECT_TRUE(backend->graph().nodes.contains(deleteTarget));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallRejectsUnsupportedBodiesBeforePublishingAnyBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(1)}}));
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (seed:Seed) CALL { WITH seed MERGE (:UnsupportedBatch {copied: seed.value}) } "
					 "IN TRANSACTIONS OF 1 ROW")),
			 std::runtime_error);

		EXPECT_EQ(backend->countNodesWithLabel("UnsupportedBatch"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallRejectsExportingBodyBeforePublishingAnyBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(1)}}));
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (seed:Seed) CALL { WITH seed CREATE (:ExportedBatch {copied: seed.value}) RETURN seed } "
					 "IN TRANSACTIONS OF 1 ROW")),
			 std::runtime_error);

		EXPECT_EQ(backend->countNodesWithLabel("ExportedBatch"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallRejectsNegativeProgrammaticBatchSizeBeforeOpeningABatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		auto body = std::make_unique<logical::LogicalCreateNode>(
				"child", std::vector<std::string>{"NegativeBatch"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		logical::LogicalCallSubquery call(std::make_unique<logical::LogicalSingleRow>(), std::move(body),
				std::vector<std::string>{}, std::vector<std::string>{}, true, -1);

		EXPECT_THROW(static_cast<void>(runtime.execute(call, nativeMutationBounds(), QueryContext{})), std::invalid_argument);
		EXPECT_EQ(backend->countNodesWithLabel("NegativeBatch"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InTransactionsCallRejectsMalformedExportsAndBodiesBeforePublishing) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const auto makeCreate = [] {
			return std::make_unique<logical::LogicalCreateNode>(
					"child", std::vector<std::string>{"RejectedBatch"}, PropertyMap{},
					std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
					std::make_unique<logical::LogicalSingleRow>());
		};

		logical::LogicalCallSubquery exportingCall(std::make_unique<logical::LogicalSingleRow>(), makeCreate(),
				std::vector<std::string>{}, std::vector<std::string>{"child"}, true, 1);
		EXPECT_THROW(static_cast<void>(runtime.execute(exportingCall, nativeMutationBounds(), QueryContext{})),
				std::runtime_error);

		// A batched body has one result per independently committed input batch.
		// DISTINCT would change that public cardinality, so reject the direct-plan
		// shape before any batch can publish its CREATE.
		auto distinctExport = std::make_unique<logical::LogicalProject>(
				makeCreate(),
				std::vector<logical::LogicalProjectItem>{{
						std::make_shared<expressions::VariableReferenceExpression>("child"), "child"}},
				true);
		logical::LogicalCallSubquery distinctExportingCall(std::make_unique<logical::LogicalSingleRow>(),
				std::move(distinctExport), std::vector<std::string>{}, std::vector<std::string>{"child"}, true, 1);
		EXPECT_THROW(static_cast<void>(runtime.execute(distinctExportingCall, nativeMutationBounds(), QueryContext{})),
				std::runtime_error);

		logical::LogicalCallSubquery nonCreateCall(
				std::make_unique<logical::LogicalSingleRow>(),
				std::make_unique<logical::LogicalSet>(std::vector<logical::LogicalSetItem>{},
						std::make_unique<logical::LogicalSingleRow>()),
				std::vector<std::string>{}, std::vector<std::string>{}, true, 1);
		EXPECT_THROW(static_cast<void>(runtime.execute(nonCreateCall, nativeMutationBounds(), QueryContext{})),
			 std::logic_error);

		EXPECT_EQ(backend->countNodesWithLabel("RejectedBatch"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, WriteOnlyCallSubqueryRejectsExportsAndBatchesIndependently) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const auto makeBody = [] {
			return std::make_unique<logical::LogicalCreateNode>(
					"child", std::vector<std::string>{"Child"}, PropertyMap{},
					std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
					std::make_unique<logical::LogicalSingleRow>());
		};

		logical::LogicalCallSubquery exportingCall(std::make_unique<logical::LogicalSingleRow>(), makeBody(),
											   std::vector<std::string>{}, std::vector<std::string>{"child"});
		EXPECT_THROW(static_cast<void>(runtime.execute(exportingCall, nativeMutationBounds(), QueryContext{})),
					 std::runtime_error);

		logical::LogicalCallSubquery transactionBatch(std::make_unique<logical::LogicalSingleRow>(), makeBody(),
													  std::vector<std::string>{}, std::vector<std::string>{}, true, 32);
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(transactionBatch, nativeMutationBounds(), QueryContext{})));
		EXPECT_EQ(backend->countNodesWithLabel("Child"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsMalformedPublicMutationPlansBeforePublishing) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const auto execute = [&](const logical::LogicalOperator &root) {
			return runtime.execute(root, nativeMutationBounds(), QueryContext{});
		};
		const auto makeCreate = [] {
			return std::make_unique<logical::LogicalCreateNode>(
					"created", std::vector<std::string>{"Created"}, PropertyMap{},
					std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
					std::make_unique<logical::LogicalSingleRow>());
		};
		execution::LogicalPlanReadRuntimeBounds zeroBounds = nativeMutationBounds();
		zeroBounds.maximum_rows = 0U;
		logical::LogicalSingleRow seed;
		EXPECT_THROW(static_cast<void>(runtime.execute(seed, zeroBounds, QueryContext{})), std::invalid_argument);

		logical::LogicalAggregate unsupportedAggregate(
				std::make_unique<logical::LogicalSingleRow>(), {},
				{{"sum", nullptr, "total", false}});
		EXPECT_THROW(static_cast<void>(execute(unsupportedAggregate)), std::logic_error);
		const auto validCount = [] {
			return logical::LogicalAggItem{"count", nullptr, "total", false, nullptr};
		};
		// Exercise each public count(*) shape guard independently. Keeping all
		// earlier predicates valid makes every later fail-closed branch reachable.
		logical::LogicalAggregate groupedCount(
				std::make_unique<logical::LogicalSingleRow>(),
				{std::make_shared<expressions::LiteralExpression>(PropertyValue(int64_t{1}))}, {validCount()});
		EXPECT_THROW(static_cast<void>(execute(groupedCount)), std::logic_error);
		logical::LogicalAggregate emptyAggregations(std::make_unique<logical::LogicalSingleRow>(), {}, {});
		EXPECT_THROW(static_cast<void>(execute(emptyAggregations)), std::logic_error);
		logical::LogicalAggregate argumentCount(
				std::make_unique<logical::LogicalSingleRow>(), {},
				{{"count", std::make_shared<expressions::LiteralExpression>(PropertyValue(int64_t{1})), "total"}});
		EXPECT_THROW(static_cast<void>(execute(argumentCount)), std::logic_error);
		logical::LogicalAggregate distinctCount(
				std::make_unique<logical::LogicalSingleRow>(), {}, {{"count", nullptr, "total", true}});
		EXPECT_THROW(static_cast<void>(execute(distinctCount)), std::logic_error);
		logical::LogicalAggregate extraCount(
				std::make_unique<logical::LogicalSingleRow>(), {},
				{{"count", nullptr, "total", false,
				  std::make_shared<expressions::LiteralExpression>(PropertyValue(int64_t{1}))}});
		EXPECT_THROW(static_cast<void>(execute(extraCount)), std::logic_error);
		logical::LogicalAggregate unnamedCount(
				std::make_unique<logical::LogicalSingleRow>(), {}, {{"count", nullptr, "", false}});
		EXPECT_THROW(static_cast<void>(execute(unnamedCount)), std::logic_error);

		logical::LogicalDelete emptyTargets({}, false, std::make_unique<logical::LogicalSingleRow>());
		EXPECT_THROW(static_cast<void>(execute(emptyTargets)), std::logic_error);

		logical::LogicalDelete missingChild({"n"}, false);
		EXPECT_THROW(static_cast<void>(execute(missingChild)), std::logic_error);

		// DELETE must reject both an unresolved binding and a scalar binding; only
		// persisted Node/Edge values are valid deletion targets.
		logical::LogicalDelete unresolvedDelete({"n"}, false, std::make_unique<logical::LogicalSingleRow>());
		EXPECT_THROW(static_cast<void>(execute(unresolvedDelete)), std::logic_error);
		logical::LogicalDelete scalarDelete(
				{"n"}, false,
				std::make_unique<logical::LogicalProject>(
						std::make_unique<logical::LogicalSingleRow>(),
						std::vector<logical::LogicalProjectItem>{
								{std::make_shared<expressions::LiteralExpression>(PropertyValue(int64_t{1})), "n"}}));
		EXPECT_THROW(static_cast<void>(execute(scalarDelete)), std::logic_error);

		// The logical operator hierarchy is public, so enum values are not proof of
		// the concrete operator type. Every mutation-root dispatch must reject a
		// forged tag before opening a snapshot or staging a write.
		MalformedLogicalOperator forgedAggregate(logical::LogicalOpType::LOP_AGGREGATE, {&seed});
		EXPECT_THROW(static_cast<void>(execute(forgedAggregate)), std::logic_error);
		MalformedLogicalOperator forgedForeach(logical::LogicalOpType::LOP_FOREACH, {&seed});
		EXPECT_THROW(static_cast<void>(execute(forgedForeach)), std::logic_error);
		MalformedLogicalOperator forgedDelete(logical::LogicalOpType::LOP_DELETE, {&seed});
		EXPECT_THROW(static_cast<void>(execute(forgedDelete)), std::logic_error);
		MalformedLogicalOperator forgedSet(logical::LogicalOpType::LOP_SET, {&seed});
		EXPECT_THROW(static_cast<void>(execute(forgedSet)), std::logic_error);
		MalformedLogicalOperator forgedRemove(logical::LogicalOpType::LOP_REMOVE, {&seed});
		EXPECT_THROW(static_cast<void>(execute(forgedRemove)), std::logic_error);
		MalformedLogicalOperator forgedCreateNode(logical::LogicalOpType::LOP_CREATE_NODE, {});
		EXPECT_THROW(static_cast<void>(execute(forgedCreateNode)), std::logic_error);
		MalformedLogicalOperator forgedCreateEdge(logical::LogicalOpType::LOP_CREATE_EDGE, {});
		EXPECT_THROW(static_cast<void>(execute(forgedCreateEdge)), std::logic_error);
		MalformedLogicalOperator forgedMerge(logical::LogicalOpType::LOP_MERGE_NODE, {});
		EXPECT_THROW(static_cast<void>(execute(forgedMerge)), std::logic_error);
		MalformedLogicalOperator forgedProject(logical::LogicalOpType::LOP_PROJECT, {&seed});
		EXPECT_THROW(static_cast<void>(execute(forgedProject)), std::logic_error);
		MalformedLogicalOperator forgedCall(logical::LogicalOpType::LOP_CALL_SUBQUERY, {});
		EXPECT_THROW(static_cast<void>(execute(forgedCall)), std::logic_error);
		logical::LogicalProject forgedVariableReferenceProjection(
				std::make_unique<logical::LogicalCreateNode>(
						"created", std::vector<std::string>{"Created"}, PropertyMap{},
						std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
						std::make_unique<logical::LogicalSingleRow>()),
				{{std::make_shared<MalformedExpression>(expressions::ExpressionType::VARIABLE_REFERENCE), "created"}});
		EXPECT_THROW(static_cast<void>(execute(forgedVariableReferenceProjection)), std::logic_error);

		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(1)}}));
		logical::LogicalSet emptySet({}, std::make_unique<logical::LogicalNodeScan>("n"));
		EXPECT_THROW(static_cast<void>(execute(emptySet)), std::logic_error);

		logical::LogicalRemove emptyRemove({}, std::make_unique<logical::LogicalNodeScan>("n"));
		EXPECT_THROW(static_cast<void>(execute(emptyRemove)), std::logic_error);

		logical::LogicalSet unresolvedSet(
				{{logical::SetActionType::LSET_PROPERTY, "", "value",
				  std::make_shared<expressions::LiteralExpression>(PropertyValue(int64_t(2)))}},
				std::make_unique<logical::LogicalNodeScan>("n"));
		EXPECT_THROW(static_cast<void>(execute(unresolvedSet)), std::logic_error);

		logical::LogicalRemove unresolvedRemove(
				{{logical::LogicalRemoveActionType::LREM_PROPERTY, "", "value"}},
				std::make_unique<logical::LogicalNodeScan>("n"));
		EXPECT_THROW(static_cast<void>(execute(unresolvedRemove)), std::logic_error);

		logical::LogicalProject missingChildProject(nullptr, {});
		EXPECT_THROW(static_cast<void>(execute(missingChildProject)), std::runtime_error);

		logical::LogicalForeach missingIterationVariable(
				std::make_unique<logical::LogicalSingleRow>(), "", nullptr, makeCreate());
		EXPECT_THROW(static_cast<void>(execute(missingIterationVariable)), std::logic_error);

		logical::LogicalForeach missingListExpression(
				std::make_unique<logical::LogicalSingleRow>(), "item", nullptr, makeCreate());
		EXPECT_THROW(static_cast<void>(execute(missingListExpression)), std::logic_error);

		logical::LogicalForeach missingBody(std::make_unique<logical::LogicalSingleRow>(), "item",
				std::make_shared<expressions::LiteralExpression>(PropertyValue(std::vector<PropertyValue>{})), nullptr);
		EXPECT_THROW(static_cast<void>(execute(missingBody)), std::logic_error);

		logical::LogicalForeach emptySetBody(
				std::make_unique<logical::LogicalSingleRow>(), "item",
				std::make_shared<expressions::LiteralExpression>(PropertyValue(std::vector<PropertyValue>{})),
				std::make_unique<logical::LogicalSet>(std::vector<logical::LogicalSetItem>{},
																	  std::make_unique<logical::LogicalSingleRow>()));
		EXPECT_THROW(static_cast<void>(execute(emptySetBody)), std::logic_error);

		logical::LogicalForeach emptyRemoveBody(
				std::make_unique<logical::LogicalSingleRow>(), "item",
				std::make_shared<expressions::LiteralExpression>(PropertyValue(std::vector<PropertyValue>{})),
				std::make_unique<logical::LogicalRemove>(std::vector<logical::LogicalRemoveItem>{},
																											std::make_unique<logical::LogicalSingleRow>()));
		EXPECT_THROW(static_cast<void>(execute(emptyRemoveBody)), std::logic_error);

		// The public logical-operator interface can be implemented outside the
		// parser. A forged operator tag must fail closed instead of assuming its
		// dynamic type and dereferencing a null cast during FOREACH validation.
		MalformedLogicalOperator forgedSetBody(
				logical::LogicalOpType::LOP_SET, std::vector<logical::LogicalOperator *>{&seed});
		logical::LogicalForeach forgedSetForeach(
				std::make_unique<logical::LogicalSingleRow>(), "item",
				std::make_shared<expressions::LiteralExpression>(PropertyValue(std::vector<PropertyValue>{})),
				std::make_unique<MalformedLogicalOperator>(forgedSetBody));
		EXPECT_THROW(static_cast<void>(execute(forgedSetForeach)), std::logic_error);

		MalformedLogicalOperator forgedRemoveBody(
				logical::LogicalOpType::LOP_REMOVE, std::vector<logical::LogicalOperator *>{&seed});
		logical::LogicalForeach forgedRemoveForeach(
				std::make_unique<logical::LogicalSingleRow>(), "item",
				std::make_shared<expressions::LiteralExpression>(PropertyValue(std::vector<PropertyValue>{})),
				std::make_unique<MalformedLogicalOperator>(forgedRemoveBody));
		EXPECT_THROW(static_cast<void>(execute(forgedRemoveForeach)), std::logic_error);

		// Public callers may construct logical plans directly. An update body must
		// therefore reject both a non-mutation root and a non-unary mutation shape
		// before the runtime attempts to stage any graph changes.
		logical::LogicalForeach unsupportedForeachBody(
				std::make_unique<logical::LogicalSingleRow>(), "item",
				std::make_shared<expressions::LiteralExpression>(PropertyValue(std::vector<PropertyValue>{})),
				std::make_unique<logical::LogicalSingleRow>());
		EXPECT_THROW(static_cast<void>(execute(unsupportedForeachBody)), std::logic_error);

		MalformedLogicalOperator malformedForeachBody(
				logical::LogicalOpType::LOP_CREATE_NODE, {&seed, &seed});
		logical::LogicalForeach nonUnaryForeachBody(
				std::make_unique<logical::LogicalSingleRow>(), "item",
				std::make_shared<expressions::LiteralExpression>(PropertyValue(std::vector<PropertyValue>{})),
				std::make_unique<MalformedLogicalOperator>(malformedForeachBody));
		EXPECT_THROW(static_cast<void>(execute(nonUnaryForeachBody)), std::logic_error);

		// A unary update chain may expose a null child through the public operator
		// interface.  Reject it before dereferencing the child while validating the
		// FOREACH body.
		MalformedLogicalOperator nullForeachChild(
				logical::LogicalOpType::LOP_CREATE_NODE, std::vector<logical::LogicalOperator *>{nullptr});
		logical::LogicalForeach nullUnaryForeach(
				std::make_unique<logical::LogicalSingleRow>(), "item",
				std::make_shared<expressions::LiteralExpression>(PropertyValue(std::vector<PropertyValue>{})),
				std::make_unique<MalformedLogicalOperator>(nullForeachChild));
		EXPECT_THROW(static_cast<void>(execute(nullUnaryForeach)), std::logic_error);

		// A SingleRow seed is a terminal operator. Permitting a hidden child would
		// make the update body depend on an unaccounted read pipeline.
		auto malformedSeed = std::make_unique<MalformedLogicalOperator>(
				logical::LogicalOpType::LOP_SINGLE_ROW, std::vector<logical::LogicalOperator *>{&seed});
		auto createWithMalformedSeed = std::make_unique<logical::LogicalCreateNode>(
				"created", std::vector<std::string>{"Created"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{}, std::move(malformedSeed));
		logical::LogicalForeach nonTerminalSingleRow(
				std::make_unique<logical::LogicalSingleRow>(), "item",
				std::make_shared<expressions::LiteralExpression>(PropertyValue(std::vector<PropertyValue>{})),
				std::move(createWithMalformedSeed));
		EXPECT_THROW(static_cast<void>(execute(nonTerminalSingleRow)), std::logic_error);

		logical::LogicalCallSubquery importingCall(
				std::make_unique<logical::LogicalSingleRow>(), makeCreate(), std::vector<std::string>{"outer"},
				std::vector<std::string>{});
		EXPECT_THROW(static_cast<void>(execute(importingCall)), std::runtime_error);

		logical::LogicalCallSubquery callWithBatchSize(
				std::make_unique<logical::LogicalSingleRow>(), makeCreate(), std::vector<std::string>{},
				std::vector<std::string>{}, false, 1);
		EXPECT_THROW(static_cast<void>(execute(callWithBatchSize)), std::runtime_error);

		logical::LogicalCallSubquery missingCallBody(std::make_unique<logical::LogicalSingleRow>(), nullptr,
																	 std::vector<std::string>{}, std::vector<std::string>{});
		EXPECT_THROW(static_cast<void>(execute(missingCallBody)), std::logic_error);

		logical::LogicalCallSubquery callWithoutCreate(
				std::make_unique<logical::LogicalSingleRow>(),
				std::make_unique<logical::LogicalSet>(std::vector<logical::LogicalSetItem>{},
																	  std::make_unique<logical::LogicalSingleRow>()),
				std::vector<std::string>{}, std::vector<std::string>{});
		EXPECT_THROW(static_cast<void>(execute(callWithoutCreate)), std::logic_error);

		EXPECT_EQ(backend->countNodesWithLabel("Created"), 0U);
		EXPECT_EQ(backend->graph().nodes.at(1U).properties.at("value"), PropertyValue(1));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CountStarAggregateCompilesAnAnonymousCreateThroughTheNativeMutationPath) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		auto create = std::make_unique<logical::LogicalCreateNode>(
				"", std::vector<std::string>{"Counted"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		logical::LogicalAggregate aggregate(std::move(create), {}, {{"count", nullptr, "count", false}});

		QueryResult result = runtime.execute(aggregate, nativeMutationBounds(), QueryContext{});
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("count").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(backend->countNodesWithLabel("Counted"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, AnonymousCreatePublishesWithoutExposingAnonymousBindings) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		logical::LogicalCreateNode create(
				"", std::vector<std::string>{"Anonymous"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());

		const QueryResult result = runtime.execute(create, nativeMutationBounds(), QueryContext{});

		EXPECT_EQ(result.rowCount(), 0U);
		EXPECT_EQ(result.getPipelineRowCount(), 1U);
		EXPECT_EQ(backend->countNodesWithLabel("Anonymous"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CypherAnonymousCreatePublishesWithoutVisibleResultRows) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		const QueryResult result = engine.execute("CREATE (:AnonymousCypher)");

		EXPECT_TRUE(result.getColumns().empty());
		EXPECT_EQ(result.rowCount(), 0U);
		EXPECT_EQ(result.getPipelineRowCount(), 1U);
		EXPECT_EQ(backend->countNodesWithLabel("AnonymousCypher"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CountStarAggregateAcceptsAProjectWrappedAnonymousCreateAndRejectsNonCanonicalForms) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		auto makeAnonymousCreate = [] {
			return std::make_unique<logical::LogicalCreateNode>(
					"", std::vector<std::string>{"ProjectedCounted"}, PropertyMap{},
					std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
					std::make_unique<logical::LogicalSingleRow>());
		};

		// The aggregate accepts the parser's invisible projection around a write-only
		// anonymous CREATE and must count its pipeline row rather than its columns.
		auto project = std::make_unique<logical::LogicalProject>(makeAnonymousCreate(),
														  std::vector<logical::LogicalProjectItem>{});
		logical::LogicalAggregate valid(std::move(project), {}, {{"count", nullptr, "count", false}});
		const QueryResult result = runtime.execute(valid, nativeMutationBounds(), QueryContext{});
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("count").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(backend->countNodesWithLabel("ProjectedCounted"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);

		const auto rejects = [&](std::unique_ptr<logical::LogicalOperator> child,
								std::vector<logical::LogicalAggItem> items) {
			logical::LogicalAggregate aggregate(std::move(child), {}, std::move(items));
			EXPECT_THROW(static_cast<void>(runtime.execute(aggregate, nativeMutationBounds(), QueryContext{})),
						 std::logic_error);
		};
		rejects(nullptr, {{"count", nullptr, "count", false}});
		rejects(makeAnonymousCreate(), {{"count", nullptr, "", false}});
		rejects(makeAnonymousCreate(), {{"count", nullptr, "count", true}});
		rejects(makeAnonymousCreate(), {{"count", nullptr, "first", false}, {"count", nullptr, "second", false}});
		rejects(makeAnonymousCreate(), {{"sum", nullptr, "count", false}});
		rejects(makeAnonymousCreate(),
				{{"count", std::make_shared<expressions::LiteralExpression>(int64_t{1}), "count", false}});
		rejects(makeAnonymousCreate(),
				{{"count", nullptr, "count", false, std::make_shared<expressions::LiteralExpression>(0.5)}});
		{
			logical::LogicalAggregate grouped(
					makeAnonymousCreate(), {std::make_shared<expressions::LiteralExpression>(int64_t{1})},
					{{"count", nullptr, "count", false}});
			EXPECT_THROW(static_cast<void>(runtime.execute(grouped, nativeMutationBounds(), QueryContext{})),
						 std::logic_error);
		}

		// Rejected logical shapes fail before they can append another mutation batch.
		EXPECT_EQ(backend->countNodesWithLabel("ProjectedCounted"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CountStarAggregateCountsAnAnonymousCreateRelationshipThroughTheNativePath) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t source = backend->addNode({"Source"}, {});
		const uint64_t target = backend->addNode({"Target"}, {});
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);

		auto sourceScan = std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"});
		auto targetScan = std::make_unique<logical::LogicalNodeScan>("target", std::vector<std::string>{"Target"});
		auto join = std::make_unique<logical::LogicalJoin>(std::move(sourceScan), std::move(targetScan));
		auto create = std::make_unique<logical::LogicalCreateEdge>(
				"", "COUNTED", PropertyMap{}, "source", "target", std::move(join));
		logical::LogicalAggregate aggregate(std::move(create), {}, {{"count", nullptr, "count", false}});

		const QueryResult result = runtime.execute(aggregate, nativeMutationBounds(), QueryContext{});
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("count").asPrimitive(), PropertyValue(1));
		ASSERT_EQ(backend->edgeCount(), 1U);
		const TestEdge &edge = backend->graph().edges.begin()->second;
		EXPECT_EQ(edge.source, source);
		EXPECT_EQ(edge.target, target);
		EXPECT_EQ(edge.type, "COUNTED");
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, TypedCreateEdgePreservesIncomingDirectionEndpointOrder) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t source = backend->addNode({"Source"}, {});
		const uint64_t target = backend->addNode({"Target"}, {});
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);

		auto sourceScan = std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"});
		auto targetScan = std::make_unique<logical::LogicalNodeScan>("target", std::vector<std::string>{"Target"});
		auto input = std::make_unique<logical::LogicalJoin>(std::move(sourceScan), std::move(targetScan));
		logical::LogicalCreateEdge create(
				"relationship", "INCOMING", PropertyMap{}, "source", "target", std::move(input), "in");

		const QueryResult result = runtime.execute(create, nativeMutationBounds(), QueryContext{});
		ASSERT_EQ(result.rowCount(), 1U);
		ASSERT_EQ(backend->edgeCount(), 1U);
		const TestEdge &edge = backend->graph().edges.begin()->second;
		EXPECT_EQ(edge.source, target);
		EXPECT_EQ(edge.target, source);
		EXPECT_EQ(edge.type, "INCOMING");
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, TypedCreateEdgePreservesEndpointOrderForUndirectedDirection) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t source = backend->addNode({"Source"}, {});
		const uint64_t target = backend->addNode({"Target"}, {});
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);

		auto sourceScan = std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"});
		auto targetScan = std::make_unique<logical::LogicalNodeScan>("target", std::vector<std::string>{"Target"});
		auto input = std::make_unique<logical::LogicalJoin>(std::move(sourceScan), std::move(targetScan));
		logical::LogicalCreateEdge create(
				"relationship", "UNDIRECTED", PropertyMap{}, "source", "target", std::move(input), "both");

		const QueryResult result = runtime.execute(create, nativeMutationBounds(), QueryContext{});
		ASSERT_EQ(result.rowCount(), 1U);
		ASSERT_EQ(backend->edgeCount(), 1U);
		const TestEdge &edge = backend->graph().edges.begin()->second;
		EXPECT_EQ(edge.source, source);
		EXPECT_EQ(edge.target, target);
		EXPECT_EQ(edge.type, "UNDIRECTED");
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachCreatesOneLogicalNodePerListElementInOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute("FOREACH (value IN [4, 5, 6] | CREATE (:Item {value: value}))"));
		EXPECT_EQ(backend->countNodesWithLabel("Item"), 3U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachCreatesAllBodyEntitiesInOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute("FOREACH (value IN [4, 5, 6] | CREATE (item:Item {value: value}) CREATE "
										 "(audit:Audit {value: value}))"));
		EXPECT_EQ(backend->countNodesWithLabel("Item"), 3U);
		EXPECT_EQ(backend->countNodesWithLabel("Audit"), 3U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateThenSetUsesTheCreatedEntityWithinOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"CREATE (n:Item {value: 4}) SET n.doubled = n.value RETURN n.value + n.doubled AS total, "
				"n.doubled AS doubled");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(8));
		EXPECT_EQ(result.getRows().front().at("doubled").asPrimitive(), PropertyValue(4));
		ASSERT_EQ(backend->countNodesWithLabel("Item"), 1U);
		ASSERT_EQ(backend->graph().nodes.size(), 1U);
		EXPECT_EQ(backend->graph().nodes.begin()->second.properties.at("value"), PropertyValue(4));
		EXPECT_EQ(backend->graph().nodes.begin()->second.properties.at("doubled"), PropertyValue(4));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ReusesLogicalReadExpressionSemanticsForCreateAndSet) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult created = engine.execute("CREATE (n:Counter {value: 1 + 2}) RETURN n.value + 1 AS value");
		ASSERT_EQ(created.rowCount(), 1U);
		EXPECT_EQ(created.getRows().front().at("value").asPrimitive(), PropertyValue(4));

		QueryResult updated =
				engine.execute("MATCH (n:Counter) SET n.value = n.value * 2 + 1 RETURN n.value + 1 AS value");
		ASSERT_EQ(updated.rowCount(), 1U);
		EXPECT_EQ(updated.getRows().front().at("value").asPrimitive(), PropertyValue(8));

		QueryResult copied = engine.execute(
				"MATCH (n:Counter) CREATE (copy:Copy {value: n.value + 1}) RETURN copy.value + 1 AS value");
		ASSERT_EQ(copied.rowCount(), 1U);
		EXPECT_EQ(copied.getRows().front().at("value").asPrimitive(), PropertyValue(9));
		EXPECT_EQ(backend->graph().nodes.at(1U).properties.at("value"), PropertyValue(7));
		EXPECT_EQ(backend->countNodesWithLabel("Copy"), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ProjectsScalarExpressionsFromCreatedRelationshipFinalState) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"CREATE (a:Start {value: 2})-[r:LINK {weight: 3}]->(b:End {value: 4}) RETURN r.weight * 2 AS weight");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("weight").asPrimitive(), PropertyValue(6));
		EXPECT_EQ(backend->edgeCount(), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateSetMaterializesCreatedRelationshipAfterFinalStateFold) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult result =
				engine.execute("CREATE (a:Start)-[r:LINK {weight: 1}]->(b:End) SET r.weight = r.weight + 1 RETURN r");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalEdgeResult &edge = result.getRows().front().at("r").asLogicalEdge();
		EXPECT_EQ(edge.type, "LINK");
		EXPECT_EQ(edge.properties.at("weight"), PropertyValue(2));
		EXPECT_EQ(backend->edgeCount(), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachCreateThenSetUsesOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"FOREACH (value IN [4, 5, 6] | CREATE (item:Item {value: value}) SET item.doubled = item.value)"));
		ASSERT_EQ(backend->countNodesWithLabel("Item"), 3U);
		for (const auto &[_, node]: backend->graph().nodes) {
			EXPECT_EQ(node.properties.at("doubled"), node.properties.at("value"));
		}
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachCreateThenRemoveFoldsEachCreatedPropertyState) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"FOREACH (value IN [4, 5, 6] | CREATE (item:Item {value: value, temporary: true}) "
				"SET item.doubled = item.value * 2 REMOVE item.temporary)"));

		ASSERT_EQ(backend->countNodesWithLabel("Item"), 3U);
		for (const auto &[_, node]: backend->graph().nodes) {
			EXPECT_EQ(node.properties.at("doubled"), PropertyValue(std::get<int64_t>(node.properties.at("value").getVariant()) * 2));
			EXPECT_FALSE(node.properties.contains("temporary"));
		}
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachFoldsCreatedRelationshipPropertiesAgainstCreatedNodeState) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"FOREACH (value IN [4, 5, 6] | CREATE (source:Start {value: value})-"
				"[relationship:LINK {temporary: true}]->(:End) "
				"SET relationship.weight = source.value * 2 REMOVE relationship.temporary)"));

		ASSERT_EQ(backend->edgeCount(), 3U);
		for (const auto &[_, edge]: backend->graph().edges) {
			EXPECT_EQ(edge.type, "LINK");
			const TestNode &source = backend->graph().nodes.at(edge.source);
			const int64_t value = std::get<int64_t>(source.properties.at("value").getVariant());
			EXPECT_EQ(edge.properties.at("weight"), PropertyValue(value * 2));
			EXPECT_FALSE(edge.properties.contains("temporary"));
		}
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachPreservesMatchedRowsForFollowingProjection) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"MATCH (n:Seed) FOREACH (value IN [4, 5, 6] | SET n.value = value) RETURN n.value AS value");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(6));
		EXPECT_EQ(backend->graph().nodes.at(seedId).properties.at("value"), PropertyValue(6));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachScalarInputSkipsItsBodyAndRetainsTheInputRow) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"value", PropertyValue(7)}});
		QueryEngine engine(backend, backend);

		QueryResult result =
				engine.execute("MATCH (n:Seed) FOREACH (value IN n.value | SET n.changed = true) RETURN n");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalNodeResult &node = result.getRows().front().at("n").asLogicalNode();
		EXPECT_EQ(node.node_id, seedId);
		EXPECT_FALSE(node.properties.contains("changed"));
		EXPECT_FALSE(backend->graph().nodes.at(seedId).properties.contains("changed"));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachUsesItsOuterInputWithoutAChildOnTheUpdateBody) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		auto body = std::make_unique<logical::LogicalCreateNode>(
				"created", std::vector<std::string>{"ImplicitForeach"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{}, nullptr);
		logical::LogicalForeach foreach(
				std::make_unique<logical::LogicalSingleRow>(), "value",
				std::make_shared<expressions::LiteralExpression>(
						PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)})),
				std::move(body));

		const QueryResult result = runtime.execute(foreach, nativeMutationBounds(), QueryContext{});
		EXPECT_EQ(result.getPipelineRowCount(), 1U);
		EXPECT_EQ(result.rowCount(), 0U);
		EXPECT_EQ(backend->countNodesWithLabel("ImplicitForeach"), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachRejectsAnEntityListExpressionBeforePublishingAnyMutation) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"value", PropertyValue(7)}});
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (n:Seed) FOREACH (value IN n | SET n.changed = true) RETURN n")),
				 std::logic_error);

		EXPECT_FALSE(backend->graph().nodes.at(seedId).properties.contains("changed"));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachRejectsAnOverBoundListBeforePublishingAnyMutation) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		execution::LogicalPlanReadRuntimeBounds bounds = nativeMutationBounds();
		bounds.maximum_rows = 1U;

		auto body = std::make_unique<logical::LogicalCreateNode>(
				"created", std::vector<std::string>{"Item"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		logical::LogicalForeach foreach(
				std::make_unique<logical::LogicalSingleRow>(), "value",
				std::make_shared<expressions::LiteralExpression>(
						PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)})),
				std::move(body));

		EXPECT_THROW(static_cast<void>(runtime.execute(foreach, bounds, QueryContext{})), std::runtime_error);
		EXPECT_EQ(backend->countNodesWithLabel("Item"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachRejectsCumulativeIterationsBeforePublishingAnyMutation) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t first =
				backend->addNode({"Seed"}, {{"values", PropertyValue(std::vector<PropertyValue>{PropertyValue(1)})}});
		const uint64_t second = backend->addNode(
				{"Seed"}, {{"values", PropertyValue(std::vector<PropertyValue>{PropertyValue(2), PropertyValue(3)})}});
		LogicalReadExecutionOptions options;
		options.bounds = nativeMutationBounds();
		options.bounds.maximum_rows = 2U;
		QueryEngine engine(backend, backend, options);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "MATCH (n:Seed) FOREACH (value IN n.values | SET n.changed = value) RETURN n")),
				 std::runtime_error);
		EXPECT_FALSE(backend->graph().nodes.at(first).properties.contains("changed"));
		EXPECT_FALSE(backend->graph().nodes.at(second).properties.contains("changed"));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, TimedOutForeachDoesNotPublishAnyMutation) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		auto body = std::make_unique<logical::LogicalCreateNode>(
				"created", std::vector<std::string>{"TimedOutItem"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		logical::LogicalForeach foreach(
				std::make_unique<logical::LogicalSingleRow>(), "value",
				std::make_shared<expressions::LiteralExpression>(
						PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)})),
				std::move(body));

		// Prime the periodic guard immediately before its deadline. The first
		// mutation-runtime checkpoint must abort before batch publication.
		QueryGuard guard(1, 0);
		for (size_t index = 0U; index < 1023U; ++index) {
			guard.check();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		QueryContext context;
		context.guard = &guard;

		EXPECT_THROW(static_cast<void>(runtime.execute(foreach, nativeMutationBounds(), context)), QueryTimeoutException);
		EXPECT_EQ(backend->countNodesWithLabel("TimedOutItem"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

} // namespace graph::query
