/**
 * @file test_LogicalPlanMutationRuntime_Validation.cpp
 * @brief Validation contracts for direct logical mutation plans.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "query/execution/LogicalPlanMutationRuntimeTestFixture.hpp"

#include "graph/query/logical/operators/LogicalTraversal.hpp"

namespace graph::query {

	TEST(LogicalPlanMutationRuntimeTest, RejectsLabelRemovalFromRelationshipsBeforePublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t sourceId = backend->addNode({"Source"}, {});
		const uint64_t targetId = backend->addNode({"Target"}, {});
		QueryEngine engine(backend, backend);
		static_cast<void>(engine.execute("MATCH (source:Source), (target:Target) CREATE "
										 "(source)-[relationship:LINK {weight: 1}]->(target)"));

		const uint64_t relationshipId = backend->graph().edges.begin()->first;
		const size_t publicationsBefore = backend->graph().mutation_batches;
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);

		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source",
				"relationship", "target", "LINK", "out", std::vector<std::string>{"Target"});
		logical::LogicalRemove remove({{logical::LogicalRemoveActionType::LREM_LABEL, "relationship", "NotALabel"}},
									  std::move(traversal));

		EXPECT_THROW(static_cast<void>(runtime.execute(remove, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		ASSERT_TRUE(backend->graph().edges.contains(relationshipId));
		EXPECT_EQ(backend->graph().edges.at(relationshipId).properties.at("weight"), PropertyValue(1));
		EXPECT_TRUE(backend->graph().nodes.contains(sourceId));
		EXPECT_TRUE(backend->graph().nodes.contains(targetId));
		EXPECT_EQ(backend->graph().mutation_batches, publicationsBefore);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsCreatedRelationshipLabelRemovalBeforePublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);

		auto source = std::make_unique<logical::LogicalCreateNode>(
				"source", std::vector<std::string>{"Source"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		auto target = std::make_unique<logical::LogicalCreateNode>(
				"target", std::vector<std::string>{"Target"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{}, std::move(source));
		auto relationship = std::make_unique<logical::LogicalCreateEdge>("relationship", "LINK", PropertyMap{},
																		 "source", "target", std::move(target));
		logical::LogicalRemove remove({{logical::LogicalRemoveActionType::LREM_LABEL, "relationship", "NotALabel"}},
									  std::move(relationship));

		EXPECT_THROW(static_cast<void>(runtime.execute(remove, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		EXPECT_EQ(backend->graph().nodes.size(), 0U);
		EXPECT_EQ(backend->edgeCount(), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsUnknownSetAndRemoveItemKindsBeforePublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);

		// The operator classes are part of the public logical-plan API. Treat an
		// out-of-range enum value as malformed input rather than silently applying
		// a no-op or treating it as a Property mutation.
		logical::LogicalSet invalidSet(
				{{static_cast<logical::SetActionType>(255), "node", "value",
				  std::make_shared<expressions::LiteralExpression>(PropertyValue(2))}},
				std::make_unique<logical::LogicalNodeScan>("node", std::vector<std::string>{"Seed"}));
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidSet, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		logical::LogicalRemove invalidRemove(
				{{static_cast<logical::LogicalRemoveActionType>(255), "node", "value"}},
				std::make_unique<logical::LogicalNodeScan>("node", std::vector<std::string>{"Seed"}));
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidRemove, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		EXPECT_EQ(backend->graph().nodes.at(nodeId).properties.at("value"), PropertyValue(1));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsMalformedSetAndRemoveItemShapesBeforeOpeningASnapshot) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
		backend->setSnapshotMode(TestSnapshotMode::TSSM_NULL);
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const auto executeSet = [&](logical::LogicalSetItem item) {
			logical::LogicalSet set({std::move(item)},
					std::make_unique<logical::LogicalNodeScan>("node", std::vector<std::string>{"Seed"}));
			EXPECT_THROW(static_cast<void>(runtime.execute(set, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		};
		const auto executeRemove = [&](logical::LogicalRemoveItem item) {
			logical::LogicalRemove remove({std::move(item)},
					std::make_unique<logical::LogicalNodeScan>("node", std::vector<std::string>{"Seed"}));
			EXPECT_THROW(static_cast<void>(runtime.execute(remove, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		};

		// Direct logical-plan callers can bypass parser construction. Every SET
		// variant must reject malformed fields before it reads or publishes data.
		executeSet({logical::SetActionType::LSET_PROPERTY, "", "value",
				 std::make_shared<expressions::LiteralExpression>(PropertyValue(true))});
		executeSet({logical::SetActionType::LSET_PROPERTY, "node", "",
				 std::make_shared<expressions::LiteralExpression>(PropertyValue(2))});
		executeSet({logical::SetActionType::LSET_PROPERTY, "node", "value", nullptr});
		executeSet({logical::SetActionType::LSET_LABEL, "", "Unexpected", nullptr});
		executeSet({logical::SetActionType::LSET_LABEL, "node", "", nullptr});
		executeSet({logical::SetActionType::LSET_LABEL, "node", "Unexpected",
				 std::make_shared<expressions::LiteralExpression>(PropertyValue(true))});
		executeSet({logical::SetActionType::LSET_MAP_MERGE, "", "",
				 std::make_shared<expressions::LiteralExpression>(PropertyValue(2))});
		executeSet({logical::SetActionType::LSET_MAP_MERGE, "node", "", nullptr});
		executeRemove({logical::LogicalRemoveActionType::LREM_PROPERTY, "", "value"});
		executeRemove({logical::LogicalRemoveActionType::LREM_LABEL, "node", ""});

		EXPECT_EQ(backend->graph().nodes.at(nodeId).properties.at("value"), PropertyValue(1));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsUnknownCreatedRemoveItemKindBeforePublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);

		auto create = std::make_unique<logical::LogicalCreateNode>(
				"node", std::vector<std::string>{"Created"}, PropertyMap{{"value", PropertyValue(1)}},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		logical::LogicalRemove invalidRemove(
				{{static_cast<logical::LogicalRemoveActionType>(255), "node", "value"}}, std::move(create));

		EXPECT_THROW(static_cast<void>(runtime.execute(invalidRemove, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		EXPECT_EQ(backend->countNodesWithLabel("Created"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsMalformedCallSubqueryShapesBeforeOpeningASnapshot) {
		auto backend = std::make_shared<TestLogicalGraph>();
		// A null snapshot distinguishes validation performed at the public plan
		// boundary from a later failure after a mutation path has begun execution.
		backend->setSnapshotMode(TestSnapshotMode::TSSM_NULL);
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		logical::LogicalSingleRow seed;

		MalformedLogicalOperator forgedProject(logical::LogicalOpType::LOP_PROJECT, {&seed});
		EXPECT_THROW(static_cast<void>(runtime.execute(forgedProject, nativeMutationBounds(), QueryContext{})),
				 std::logic_error);

		MalformedLogicalOperator forgedCall(logical::LogicalOpType::LOP_CALL_SUBQUERY, {&seed});
		EXPECT_THROW(static_cast<void>(runtime.execute(forgedCall, nativeMutationBounds(), QueryContext{})),
				 std::logic_error);

		auto forgedWriteOnlyBody = std::make_unique<MalformedLogicalOperator>(
				logical::LogicalOpType::LOP_CREATE_NODE, std::vector<logical::LogicalOperator *>{&seed});
		logical::LogicalCallSubquery writeOnlyCall(std::make_unique<logical::LogicalSingleRow>(),
				std::move(forgedWriteOnlyBody), std::vector<std::string>{}, std::vector<std::string>{});
		EXPECT_THROW(static_cast<void>(runtime.execute(writeOnlyCall, nativeMutationBounds(), QueryContext{})),
				 std::logic_error);

		auto forgedBatchedBody = std::make_unique<MalformedLogicalOperator>(
				logical::LogicalOpType::LOP_CREATE_NODE, std::vector<logical::LogicalOperator *>{&seed});
		logical::LogicalCallSubquery batchedCall(std::make_unique<logical::LogicalSingleRow>(),
				std::move(forgedBatchedBody), std::vector<std::string>{}, std::vector<std::string>{}, true, 1);
		EXPECT_THROW(static_cast<void>(runtime.execute(batchedCall, nativeMutationBounds(), QueryContext{})),
				 std::logic_error);

		EXPECT_EQ(backend->graph().mutation_batches, 0U);
		EXPECT_EQ(backend->graph().nodes.size(), 0U);
		EXPECT_EQ(backend->edgeCount(), 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsPropertyMutationWhenTheCompletePropertyReadExceedsItsBound) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId =
				backend->addNode({"Bounded"}, {{"first", PropertyValue(1)}, {"second", PropertyValue(2)}});
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		execution::LogicalPlanReadRuntimeBounds bounds = nativeMutationBounds();
		bounds.maximum_property_entries = 1U;

		logical::LogicalSet update(
				{{logical::SetActionType::LSET_PROPERTY, "node", "third",
				  std::make_shared<expressions::LiteralExpression>(PropertyValue(3))}},
				std::make_unique<logical::LogicalNodeScan>("node", std::vector<std::string>{"Bounded"}));
		EXPECT_THROW(static_cast<void>(runtime.execute(update, bounds, QueryContext{})), std::runtime_error);

		const TestNode &unchanged = backend->graph().nodes.at(nodeId);
		EXPECT_EQ(unchanged.properties.at("first"), PropertyValue(1));
		EXPECT_EQ(unchanged.properties.at("second"), PropertyValue(2));
		EXPECT_FALSE(unchanged.properties.contains("third"));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

} // namespace graph::query
