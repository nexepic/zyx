/**
 * @file test_LogicalPlanMutationRuntime_Atomicity.cpp
 * @brief End-to-end tests for bounded logical-plan mutations.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "query/execution/LogicalPlanMutationRuntimeTestFixture.hpp"
#include "graph/query/logical/operators/LogicalMergeNode.hpp"
#include "graph/query/logical/operators/LogicalMergeEdge.hpp"

namespace graph::query {

	TEST(LogicalPlanMutationRuntimeTest, PropertyPatchChainPreservesFieldsAndRetriesWithoutDoubleApplication) {
		for (const bool conflict : {false, true}) {
			auto backend = std::make_shared<TestLogicalGraph>();
			const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}, {"name", PropertyValue("Ada")}, {"keep", PropertyValue(9)}});
			backend->setShapedPropertyAdmission(true);
			if (conflict) backend->rejectNextMutation();
			QueryEngine engine(backend, backend);
			EXPECT_NO_THROW(static_cast<void>(engine.execute(
				"MATCH (n:Seed) SET n.rank = n.rank + 1 SET n.rank = n.rank + 1 REMOVE n.name SET n:Updated RETURN n.rank")));
			const auto &node = backend->graph().nodes.at(id);
			EXPECT_EQ(node.properties.at("rank"), PropertyValue(3));
			EXPECT_EQ(node.properties.at("keep"), PropertyValue(9));
			EXPECT_FALSE(node.properties.contains("name"));
			EXPECT_EQ(backend->countNodesWithLabel("Updated"), 1U);
			EXPECT_EQ(backend->graph().mutation_batches, 1U);
			EXPECT_EQ(backend->read_control_calls, conflict ? 2U : 1U);
			EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, PropertyPatchChainRestoringOriginalValueDoesNotPublish) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		EXPECT_NO_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) SET n.rank = 2 SET n.rank = 1 RETURN n.rank")));
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(1));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
		EXPECT_EQ(backend->read_control_calls, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, PropertyPatchMapMergeRemovesNullAndPreservesUnspecifiedFields) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}, {"name", PropertyValue("Ada")}, {"keep", PropertyValue(9)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		EXPECT_NO_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) SET n += {rank: 4, name: null} RETURN n")));
		const auto &properties = backend->graph().nodes.at(id).properties;
		EXPECT_EQ(properties.at("rank"), PropertyValue(4));
		EXPECT_EQ(properties.at("keep"), PropertyValue(9));
		EXPECT_FALSE(properties.contains("name"));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
		EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, PropertyPatchControlFailureDoesNotPublishCompanionLabels) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		backend->read_control_hook = [](const storage::LogicalGraphMutationReadControl &control) {
			ASSERT_TRUE(control.property_admission);
			throw std::runtime_error("test mutation control rejected publication");
		};
		QueryEngine engine(backend, backend);
		EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) SET n.rank = 2 SET n:Updated RETURN n")), std::runtime_error);
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(1));
		EXPECT_EQ(backend->countNodesWithLabel("Updated"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
		EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, EdgePropertyPatchPreservesOtherFieldsAndReturnsFinalValue) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto source = backend->addNode({"Source"}, {});
		const auto target = backend->addNode({"Target"}, {});
		const auto edge = backend->addEdge(source, target, "LINK", {{"weight", PropertyValue(1)}, {"keep", PropertyValue(9)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		const auto result = engine.execute("MATCH (:Source)-[r:LINK]->(:Target) SET r.weight = 5 RETURN r.weight AS weight");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("weight").asPrimitive(), PropertyValue(5));
		EXPECT_EQ(backend->graph().edges.at(edge).properties.at("weight"), PropertyValue(5));
		EXPECT_EQ(backend->graph().edges.at(edge).properties.at("keep"), PropertyValue(9));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
		EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, PropertyPatchCompleteMapReplacementClearsUnspecifiedFields) {
		for (const bool patches : {false, true}) {
			auto backend = std::make_shared<TestLogicalGraph>();
			const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}, {"keep", PropertyValue(9)}});
			backend->setShapedPropertyAdmission(patches);
			QueryEngine engine(backend, backend);
			const auto result = engine.execute("MATCH (n:Seed) SET n = {rank: 'changed', removed: null} RETURN n.rank AS rank");
			ASSERT_EQ(result.rowCount(), 1U);
			EXPECT_EQ(result.getRows().front().at("rank").asPrimitive(), PropertyValue("changed"));
			const auto &properties = backend->graph().nodes.at(id).properties;
			ASSERT_EQ(properties.size(), 1U);
			EXPECT_EQ(properties.at("rank"), PropertyValue("changed"));
			EXPECT_EQ(backend->graph().mutation_batches, 1U);
			if (patches) EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, PropertyPatchEmptyMapReplacementRemovesAllFields) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}, {"name", PropertyValue("Ada")}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		static_cast<void>(engine.execute("MATCH (n:Seed) SET n = {} RETURN n"));
		EXPECT_TRUE(backend->graph().nodes.at(id).properties.empty());
		EXPECT_EQ(backend->countNodesWithLabel("Seed"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
		EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, PropertyPatchSourceAdmissionFailurePrecedesBackendPublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		backend->setShapedAdmissionHook([](size_t bytes) {
			EXPECT_GT(bytes, 0U);
			throw std::runtime_error("test rejected patch source before copying");
		});
		QueryEngine engine(backend, backend);
		EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) SET n.rank = 2 SET n:Updated RETURN n")), std::runtime_error);
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(1));
		EXPECT_EQ(backend->countNodesWithLabel("Updated"), 0U);
		EXPECT_EQ(backend->read_control_calls, 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
		EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CompleteMapReplacementWorksAcrossCreateMergeAndForeachChains) {
		for (const std::string query : {
			"CREATE (n:Seed {old: 1}) SET n = {rank: 3} RETURN n",
			"MERGE (n:Seed {old: 1}) SET n = {rank: 3} RETURN n",
			"MATCH (n:Seed) FOREACH (value IN [3] | SET n = {rank: value}) RETURN n"}) {
			SCOPED_TRACE(query);
			auto backend = std::make_shared<TestLogicalGraph>();
			if (query.starts_with("MATCH")) static_cast<void>(backend->addNode({"Seed"}, {{"old", PropertyValue(1)}}));
			QueryEngine engine(backend, backend);
			static_cast<void>(engine.execute(query));
			ASSERT_EQ(backend->graph().nodes.size(), 1U);
			const auto &properties = backend->graph().nodes.begin()->second.properties;
			ASSERT_EQ(properties.size(), 1U);
			EXPECT_EQ(properties.at("rank"), PropertyValue(3));
			EXPECT_EQ(backend->graph().mutation_batches, 1U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, CompleteMapReplacementRejectsScalarBeforePublishingEarlierChanges) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) SET n.rank = 2 SET n = 3 RETURN n")), std::logic_error);
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(1));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MapMergeEvaluatesCompleteCaseExpressionBeforeApplyingFields) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		static_cast<void>(engine.execute("MATCH (n:Seed) SET n += CASE WHEN false THEN {rank: 2} ELSE {rank: 3} END RETURN n"));
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(3));
	}

	TEST(LogicalPlanMutationRuntimeTest, MapMergeEvaluatesFieldReferencesAgainstTheSameInputState) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		static_cast<void>(engine.execute("MATCH (n:Seed) SET n += {rank: 2, previous: n.rank} RETURN n"));
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(2));
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("previous"), PropertyValue(1));
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeMapActionsRetainTypesAndExpressionsWhenCloned) {
		const auto expression = std::make_shared<expressions::LiteralExpression>(PropertyValue(1));
		const std::vector<logical::MergeSetAction> actions{
			{"n", "", expression, logical::SetActionType::LSET_MAP_MERGE},
			{"n", "", expression, logical::SetActionType::LSET_MAP_REPLACE}};
		logical::LogicalMergeNode node("n", {}, {}, actions, actions);
		const auto clonedNode = node.clone();
		const auto *nodePlan = dynamic_cast<const logical::LogicalMergeNode *>(clonedNode.get());
		ASSERT_NE(nodePlan, nullptr);
		logical::LogicalMergeEdge edge("source", "n", "target", "LINK", "out", {}, actions, actions);
		const auto clonedEdge = edge.clone();
		const auto *edgePlan = dynamic_cast<const logical::LogicalMergeEdge *>(clonedEdge.get());
		ASSERT_NE(edgePlan, nullptr);
		for (const auto *copied : {&nodePlan->getOnCreateItems(), &nodePlan->getOnMatchItems(),
			&edgePlan->getOnCreateActions(), &edgePlan->getOnMatchActions()}) {
			ASSERT_EQ(copied->size(), actions.size());
			for (size_t index = 0U; index < actions.size(); ++index) {
				EXPECT_EQ((*copied)[index].type, actions[index].type);
				EXPECT_EQ((*copied)[index].variable, actions[index].variable);
				EXPECT_EQ((*copied)[index].key, actions[index].key);
				ASSERT_NE((*copied)[index].expression, nullptr);
				EXPECT_NE((*copied)[index].expression.get(), expression.get());
			}
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeOnCreateAndOnMatchKeepMapReplacementDistinctFromMerge) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);
		static_cast<void>(engine.execute("MERGE (n:Seed {identity: 1}) ON CREATE SET n += {rank: 2, previous: n.identity} RETURN n"));
		ASSERT_EQ(backend->graph().nodes.size(), 1U);
		const auto id = backend->graph().nodes.begin()->first;
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("previous"), PropertyValue(1));
		static_cast<void>(engine.execute("MERGE (n:Seed {identity: 1}) ON MATCH SET n = {identity: 1, rank: 3, discarded: null} RETURN n"));
		const auto &properties = backend->graph().nodes.at(id).properties;
		ASSERT_EQ(properties.size(), 2U);
		EXPECT_EQ(properties.at("rank"), PropertyValue(3));
		EXPECT_FALSE(properties.contains("previous"));
		EXPECT_FALSE(properties.contains("discarded"));
		EXPECT_EQ(backend->graph().mutation_batches, 2U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeMapActionFailureDoesNotPublishCreatedOrMatchedState) {
		for (const bool existing : {false, true}) {
			auto backend = std::make_shared<TestLogicalGraph>();
			if (existing) static_cast<void>(backend->addNode({"Seed"}, {{"identity", PropertyValue(1)}, {"rank", PropertyValue(9)}}));
			QueryEngine engine(backend, backend);
			EXPECT_THROW(static_cast<void>(engine.execute(
				"MERGE (n:Seed {identity: 1}) ON CREATE SET n += 3 ON MATCH SET n = 4 RETURN n")), std::logic_error);
			EXPECT_EQ(backend->graph().nodes.size(), existing ? 1U : 0U);
			if (existing) EXPECT_EQ(backend->graph().nodes.begin()->second.properties.at("rank"), PropertyValue(9));
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, CompleteMapReplacementRejectsEntryGrowthBeforePublication) {
		for (const std::string query : {
			"MATCH (n:Seed) SET n = {a: 1, b: 2} RETURN n.a AS value",
			"CREATE (n:Seed) SET n = {a: 1, b: 2} RETURN n.a AS value",
			"MERGE (n:Seed {identity: 1}) ON MATCH SET n = {a: 1, b: 2} RETURN n.a AS value",
			"MERGE (n:Other) ON CREATE SET n = {a: 1, b: 2} RETURN n.a AS value"}) {
			SCOPED_TRACE(query);
			auto backend = std::make_shared<TestLogicalGraph>();
			const auto id = backend->addNode({"Seed"}, {{"identity", PropertyValue(1)}});
			backend->setShapedPropertyAdmission(true);
			LogicalReadExecutionOptions options;
			options.bounds.maximum_property_entries = 1U;
			QueryEngine engine(backend, backend, options);
			EXPECT_THROW(static_cast<void>(engine.execute(query)), std::runtime_error);
			ASSERT_EQ(backend->graph().nodes.size(), 1U);
			EXPECT_EQ(backend->graph().nodes.at(id).properties.at("identity"), PropertyValue(1));
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, CompleteMapReplacementCountsOnlyPersistedFieldsAtEntryLimit) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"identity", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		LogicalReadExecutionOptions options;
		options.bounds.maximum_property_entries = 1U;
		QueryEngine engine(backend, backend, options);
		const auto result = engine.execute("MATCH (n:Seed) SET n = {rank: 2, absent: null} RETURN n.rank AS rank");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("rank").asPrimitive(), PropertyValue(2));
		ASSERT_EQ(backend->graph().nodes.at(id).properties.size(), 1U);
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(2));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, PropertyGrowthRejectsScalarAndMapWritesAcrossMutationChains) {
		for (const std::string query : {
			"MATCH (n:Seed) SET n.extra = 2 RETURN n.extra",
			"MATCH (n:Seed) SET n += {extra: 2} RETURN n.extra",
			"CREATE (n:Other {identity: 1}) SET n.extra = 2 RETURN n.extra",
			"CREATE (n:Other {identity: 1}) SET n += {extra: 2} RETURN n.extra",
			"MERGE (n:Seed {identity: 1}) ON MATCH SET n.extra = 2 RETURN n.extra",
			"MERGE (n:Seed {identity: 1}) ON MATCH SET n += {extra: 2} RETURN n.extra",
			"MERGE (n:Other {identity: 1}) ON CREATE SET n += {extra: 2} RETURN n.extra",
			"MERGE (n:Seed {identity: 1}) SET n.extra = 2 RETURN n.extra",
			"MERGE (n:Seed {identity: 1}) SET n += {extra: 2} RETURN n.extra",
			"MATCH (n:Seed) FOREACH (value IN [2] | SET n.extra = value) RETURN n"}) {
			SCOPED_TRACE(query);
			auto backend = std::make_shared<TestLogicalGraph>();
			const auto id = backend->addNode({"Seed"}, {{"identity", PropertyValue(1)}});
			backend->setShapedPropertyAdmission(true);
			LogicalReadExecutionOptions options;
			options.bounds.maximum_property_entries = 1U;
			QueryEngine engine(backend, backend, options);
			EXPECT_THROW(static_cast<void>(engine.execute(query)), std::runtime_error);
			ASSERT_EQ(backend->graph().nodes.size(), 1U);
			ASSERT_EQ(backend->graph().nodes.at(id).properties.size(), 1U);
			EXPECT_EQ(backend->graph().nodes.at(id).properties.at("identity"), PropertyValue(1));
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, MapMergeRemovalMakesRoomWithoutDependingOnFieldOrder) {
		for (const std::string map : {"{identity: null, extra: 2}", "{extra: 2, identity: null}"}) {
			auto backend = std::make_shared<TestLogicalGraph>();
			const auto id = backend->addNode({"Seed"}, {{"identity", PropertyValue(1)}});
			backend->setShapedPropertyAdmission(true);
			LogicalReadExecutionOptions options;
			options.bounds.maximum_property_entries = 1U;
			QueryEngine engine(backend, backend, options);
			static_cast<void>(engine.execute("MATCH (n:Seed) SET n += " + map + " RETURN n.extra"));
			ASSERT_EQ(backend->graph().nodes.at(id).properties.size(), 1U);
			EXPECT_EQ(backend->graph().nodes.at(id).properties.at("extra"), PropertyValue(2));
			EXPECT_EQ(backend->graph().mutation_batches, 1U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, MergePersistedNodeUsesPropertyPatchesAndRebuildsAfterConflict) {
		for (const bool conflict : {false, true}) {
			auto backend = std::make_shared<TestLogicalGraph>();
			const auto id = backend->addNode({"Seed"}, {{"identity", PropertyValue(1)}, {"rank", PropertyValue(1)}, {"keep", PropertyValue(9)}});
			backend->setShapedPropertyAdmission(true);
			if (conflict) backend->rejectNextMutation();
			QueryEngine engine(backend, backend);
			const auto result = engine.execute("MERGE (n:Seed {identity: 1}) ON MATCH SET n += {rank: n.rank + 1} SET n:Updated RETURN n.rank AS rank");
			ASSERT_EQ(result.rowCount(), 1U);
			EXPECT_EQ(result.getRows().front().at("rank").asPrimitive(), PropertyValue(2));
			EXPECT_EQ(backend->graph().nodes.at(id).properties.at("keep"), PropertyValue(9));
			EXPECT_EQ(backend->countNodesWithLabel("Updated"), 1U);
			EXPECT_EQ(backend->graph().mutation_batches, 1U);
			EXPECT_EQ(backend->read_control_calls, conflict ? 2U : 1U);
			EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, MergePropertyPatchRestoringOriginalValueSkipsPublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"identity", PropertyValue(1)}, {"rank", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		const auto result = engine.execute("MERGE (n:Seed {identity: 1}) ON MATCH SET n.rank = 2 SET n.rank = 1 RETURN n.rank AS rank");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("rank").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(1));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
		EXPECT_EQ(backend->read_control_calls, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergePropertyPatchAdmissionFailureCannotPublishLabelChanges) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"identity", PropertyValue(1)}, {"rank", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		backend->setShapedAdmissionHook([](size_t) { throw std::runtime_error("test MERGE source admission failure"); });
		QueryEngine engine(backend, backend);
		EXPECT_THROW(static_cast<void>(engine.execute("MERGE (n:Seed {identity: 1}) ON MATCH SET n.rank = 2 SET n:Updated RETURN n")), std::runtime_error);
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(1));
		EXPECT_EQ(backend->countNodesWithLabel("Updated"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
		EXPECT_EQ(backend->read_control_calls, 0U);
		EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergePersistedEdgeUsesPatchForCompleteMapReplacement) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto source = backend->addNode({"Source"}, {});
		const auto target = backend->addNode({"Target"}, {});
		const auto edge = backend->addEdge(source, target, "LINK", {{"weight", PropertyValue(1)}, {"old", PropertyValue(9)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		const auto result = engine.execute("MATCH (a:Source), (b:Target) MERGE (a)-[r:LINK]->(b) ON MATCH SET r = {weight: 3} RETURN r.weight AS weight");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("weight").asPrimitive(), PropertyValue(3));
		ASSERT_EQ(backend->graph().edges.at(edge).properties.size(), 1U);
		EXPECT_EQ(backend->graph().edges.at(edge).properties.at("weight"), PropertyValue(3));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
		EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateChainPublishesNewNodesAndPersistedPropertyPatchesTogether) {
		for (const bool conflict : {false, true}) {
			auto backend = std::make_shared<TestLogicalGraph>();
			const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}, {"keep", PropertyValue(9)}});
			backend->setShapedPropertyAdmission(true);
			if (conflict) backend->rejectNextMutation();
			QueryEngine engine(backend, backend);
			const auto result = engine.execute("MATCH (n:Seed) CREATE (m:Created {name: 'new'}) SET n.rank = n.rank + 1 RETURN m, n.rank AS rank");
			ASSERT_EQ(result.rowCount(), 1U);
			EXPECT_EQ(result.getRows().front().at("rank").asPrimitive(), PropertyValue(2));
			EXPECT_TRUE(result.getRows().front().at("m").isLogicalNode());
			EXPECT_EQ(backend->graph().nodes.at(id).properties.at("keep"), PropertyValue(9));
			EXPECT_EQ(backend->countNodesWithLabel("Created"), 1U);
			EXPECT_EQ(backend->graph().mutation_batches, 1U);
			EXPECT_EQ(backend->read_control_calls, conflict ? 2U : 1U);
			EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachCollapsesOrderedPersistedChangesIntoOnePropertyPatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}, {"keep", PropertyValue(9)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		const auto result = engine.execute("MATCH (n:Seed) FOREACH (value IN [1, 2] | SET n.rank = n.rank + value) RETURN n.rank AS rank");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("rank").asPrimitive(), PropertyValue(4));
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(4));
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("keep"), PropertyValue(9));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
		EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateChainPatchAdmissionFailureRejectsCompanionCreations) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		backend->setShapedAdmissionHook([](size_t) { throw std::runtime_error("test CREATE patch source admission failure"); });
		QueryEngine engine(backend, backend);
		EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) CREATE (:Created) SET n.rank = 2 RETURN n")), std::runtime_error);
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(1));
		EXPECT_EQ(backend->countNodesWithLabel("Created"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
		EXPECT_EQ(backend->read_control_calls, 0U);
		EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachCreateReadsPreviousIterationPropertyChangesBeforeCurrentSet) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		static_cast<void>(engine.execute("MATCH (n:Seed) FOREACH (value IN [1, 2] | CREATE (:Observed {seen: n.rank}) SET n.rank = n.rank + 1) RETURN n"));
		std::vector<int64_t> seen;
		for (const auto &[nodeId, node] : backend->graph().nodes) {
			if (nodeId != id) seen.push_back(std::get<int64_t>(node.properties.at("seen").getVariant()));
		}
		std::sort(seen.begin(), seen.end());
		EXPECT_EQ(seen, (std::vector<int64_t>{1, 2}));
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(3));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
		EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachFailureAfterCrossEntityReadsDoesNotPublishStagedValues) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) FOREACH (value IN [1, 0] | CREATE (:Observed {seen: n.rank, quotient: 1 / value}) SET n.rank = n.rank + 1) RETURN n")), std::runtime_error);
		EXPECT_EQ(backend->graph().nodes.size(), 1U);
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(1));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
		EXPECT_EQ(backend->read_control_calls, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachUpdatesAllAliasesBeforeCreatingDependentEntities) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		static_cast<void>(engine.execute("MATCH (n:Seed), (alias:Seed) FOREACH (value IN [1, 2] | SET n.rank = n.rank + 1 CREATE (:Observed {seen: alias.rank})) RETURN n"));
		std::vector<int64_t> seen;
		for (const auto &[nodeId, node] : backend->graph().nodes) {
			if (nodeId != id) seen.push_back(std::get<int64_t>(node.properties.at("seen").getVariant()));
		}
		std::sort(seen.begin(), seen.end());
		EXPECT_EQ(seen, (std::vector<int64_t>{2, 3}));
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(3));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachCreatedEntitiesReadTheirFinalStagedPropertiesWithinEachIteration) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);
		static_cast<void>(engine.execute("FOREACH (value IN [1, 2] | CREATE (n:Created {rank: 1}) SET n += {rank: n.rank + value, previous: n.rank} CREATE (:Observed {seen: n.rank}))"));
		std::vector<int64_t> ranks, seen;
		for (const auto &[_, node] : backend->graph().nodes) {
			if (node.properties.contains("rank")) {
				ranks.push_back(std::get<int64_t>(node.properties.at("rank").getVariant()));
				EXPECT_EQ(node.properties.at("previous"), PropertyValue(1));
			} else seen.push_back(std::get<int64_t>(node.properties.at("seen").getVariant()));
		}
		std::sort(ranks.begin(), ranks.end());
		std::sort(seen.begin(), seen.end());
		EXPECT_EQ(ranks, (std::vector<int64_t>{2, 3}));
		EXPECT_EQ(seen, ranks);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, InitialCreatePropertiesRespectEntryBoundsForNodesAndRelationships) {
		for (const std::string query : {
			"CREATE (:Created {a: 1, b: 2})",
			"CREATE (a)-[:LINK {a: 1, b: 2}]->(b)",
			"UNWIND [1] AS value CREATE (:Created {a: 1, b: value})",
			"UNWIND [1] AS value CREATE (a)-[:LINK {a: 1, b: value}]->(b)"}) {
			SCOPED_TRACE(query);
			auto backend = std::make_shared<TestLogicalGraph>();
			LogicalReadExecutionOptions options;
			options.bounds.maximum_property_entries = 1U;
			QueryEngine engine(backend, backend, options);
			EXPECT_THROW(static_cast<void>(engine.execute(query)), std::runtime_error);
			EXPECT_TRUE(backend->graph().nodes.empty());
			EXPECT_TRUE(backend->graph().edges.empty());
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
			EXPECT_EQ(backend->read_control_calls, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeIdentityEntryAdmissionPrecedesDynamicExpressionEvaluation) {
		registerCountingMergeResultExpression();
		for (const std::string query : {
			"MERGE (:Created {a: 1, b: 2})",
			"MERGE (:Created {a: 1, b: testMergeResultCounter()})",
			"MERGE (a)-[:LINK {a: 1, b: 2}]->(b)",
			"MERGE (a)-[:LINK {a: 1, b: testMergeResultCounter()}]->(b)"}) {
			SCOPED_TRACE(query);
			mergeResultExpressionCalls.store(0, std::memory_order_relaxed);
			auto backend = std::make_shared<TestLogicalGraph>();
			LogicalReadExecutionOptions options;
			options.bounds.maximum_property_entries = 1U;
			QueryEngine engine(backend, backend, options);
			EXPECT_THROW(static_cast<void>(engine.execute(query)), std::runtime_error);
			EXPECT_EQ(mergeResultExpressionCalls.load(std::memory_order_relaxed), 0);
			EXPECT_TRUE(backend->graph().nodes.empty());
			EXPECT_TRUE(backend->graph().edges.empty());
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
			EXPECT_EQ(backend->read_control_calls, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeIdentityEntryAdmissionAllowsItsExactBoundary) {
		auto backend = std::make_shared<TestLogicalGraph>();
		LogicalReadExecutionOptions options;
		options.bounds.maximum_property_entries = 1U;
		QueryEngine engine(backend, backend, options);
		const auto result = engine.execute("UNWIND [3] AS value MERGE (n:Created {identity: value}) RETURN n.identity AS identity");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("identity").asPrimitive(), PropertyValue(3));
		EXPECT_EQ(backend->graph().nodes.size(), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergePropertyChangesSynchronizeAliasesBeforeLaterAssignments) {
		for (const std::string query : {
			"MATCH (n:Seed), (alias:Seed) MERGE (n) ON MATCH SET n.rank = 2, n.copied = alias.rank RETURN n.copied AS copied",
			"MATCH (n:Seed), (alias:Seed) MERGE (n) SET n.rank = 2 SET n.copied = alias.rank RETURN n.copied AS copied"}) {
			SCOPED_TRACE(query);
			auto backend = std::make_shared<TestLogicalGraph>();
			const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}});
			backend->setShapedPropertyAdmission(true);
			QueryEngine engine(backend, backend);
			const auto result = engine.execute(query);
			ASSERT_EQ(result.rowCount(), 1U);
			EXPECT_EQ(result.getRows().front().at("copied").asPrimitive(), PropertyValue(2));
			EXPECT_EQ(backend->graph().nodes.at(id).properties.at("copied"), PropertyValue(2));
			EXPECT_EQ(backend->graph().mutation_batches, 1U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, MergePropertyRemovalIsVisibleThroughAliasesInLaterAssignments) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}, {"keep", PropertyValue(9)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		const auto result = engine.execute("MATCH (n:Seed), (alias:Seed) MERGE (n) REMOVE n.rank SET n.copied = alias.rank RETURN n.copied AS copied");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("copied").asPrimitive().getType(), PropertyType::NULL_TYPE);
		const auto &properties = backend->graph().nodes.at(id).properties;
		EXPECT_FALSE(properties.contains("rank"));
		EXPECT_FALSE(properties.contains("copied"));
		EXPECT_EQ(properties.at("keep"), PropertyValue(9));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
		EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeAliasExpressionFailureCannotPublishEarlierRemoval) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"rank", PropertyValue(1)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed), (alias:Seed) MERGE (n) REMOVE n.rank SET n.copied = CASE WHEN alias.rank IS NULL THEN 1 / 0 ELSE 1 END RETURN n")), std::runtime_error);
		EXPECT_EQ(backend->graph().nodes.at(id).properties.at("rank"), PropertyValue(1));
		EXPECT_FALSE(backend->graph().nodes.at(id).properties.contains("copied"));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
		EXPECT_EQ(backend->read_control_calls, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, PropertyPatchLookupHandlesUnorderedFieldsAndMixedChanges) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const auto id = backend->addNode({"Seed"}, {{"z", PropertyValue(1)}, {"m", PropertyValue(2)}, {"a", PropertyValue(3)}});
		backend->setShapedPropertyAdmission(true);
		QueryEngine engine(backend, backend);
		static_cast<void>(engine.execute("MATCH (n:Seed) SET n.m = 7, n.b = 8 REMOVE n.z RETURN n"));
		const auto &properties = backend->graph().nodes.at(id).properties;
		ASSERT_EQ(properties.size(), 3U);
		EXPECT_EQ(properties.at("a"), PropertyValue(3));
		EXPECT_EQ(properties.at("b"), PropertyValue(8));
		EXPECT_EQ(properties.at("m"), PropertyValue(7));
		EXPECT_FALSE(properties.contains("z"));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
		EXPECT_GT(backend->readMetrics().shaped_property_admission_reads, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, BackendReadControlsRejectGuardAndConstraintSourceGrowthBeforePublication) {
		for (const std::string query: {
			"CREATE (n:Guarded {name: 'new'}) RETURN n.name AS value",
			"MATCH (n:Guarded {name: 'old'}) SET n.name = 'changed' RETURN n.name AS value",
			"MATCH (n:Guarded {name: 'old'}) REMOVE n.name RETURN n",
			"MATCH (n:Guarded {name: 'old'}) DELETE n",
			"MERGE (n:Guarded {name: 'new'}) RETURN n.name AS value"}) {
			SCOPED_TRACE(query);
			for (bool memory: {false, true}) {
				auto backend = std::make_shared<TestLogicalGraph>();
				static_cast<void>(backend->addNode({"Guarded"}, {{"name", PropertyValue("old")}}));
				LogicalReadExecutionOptions options;
				options.bounds.maximum_materialized_bytes = 4096U;
				QueryEngine engine(backend, backend, options);
				size_t resident = 0U;
				QueryGuard guard(0, 1, [&] { return resident; });
				QueryContext context; context.guard = &guard;
				backend->read_control_hook = [&](const storage::LogicalGraphMutationReadControl &control) {
					ASSERT_TRUE(control.checkpoint);
					ASSERT_TRUE(control.property_admission);
					if (memory) { resident = 2U * 1024U * 1024U; control.checkpoint(); }
					else control.property_admission(4097U);
				};
				if (memory) EXPECT_THROW(static_cast<void>(engine.execute(query, context)), QueryMemoryExceededException);
				else EXPECT_THROW(static_cast<void>(engine.execute(query, context)), std::runtime_error);
				EXPECT_EQ(backend->read_control_calls, 1U);
				EXPECT_EQ(backend->graph().mutation_batches, 0U);
				EXPECT_EQ(backend->countNodesWithLabel("Guarded"), 1U);
				backend->read_control_hook = {}; resident = 0U;
				EXPECT_NO_THROW(static_cast<void>(engine.execute(query, context)));
				EXPECT_EQ(backend->graph().mutation_batches, 1U);
			}
		}
	}

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
				"person", std::vector<std::string>{"Person"}, PropertyMap{{"name", PropertyValue("Ada")}},
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
