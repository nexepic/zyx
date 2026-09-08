/**
 * @file test_LogicalPlanMutationRuntime_Validation.cpp
 * @brief Validation contracts for direct logical mutation plans.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "query/execution/LogicalPlanMutationRuntimeTestFixture.hpp"

#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalDelete.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalMergeEdge.hpp"
#include "graph/query/logical/operators/LogicalMergeNode.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
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

	TEST(LogicalPlanMutationRuntimeTest, RejectsLabelAssignmentToRelationshipsBeforePublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t sourceId = backend->addNode({"Source"}, {});
		const uint64_t targetId = backend->addNode({"Target"}, {});
		QueryEngine engine(backend, backend);
		static_cast<void>(engine.execute("MATCH (source:Source), (target:Target) CREATE "
										 "(source)-[relationship:LINK]->(target)"));

		const uint64_t relationshipId = backend->graph().edges.begin()->first;
		const size_t publicationsBefore = backend->graph().mutation_batches;
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
				std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source",
				"relationship", "target", "LINK", "out", std::vector<std::string>{"Target"});
		logical::LogicalSet set({{logical::SetActionType::LSET_LABEL, "relationship", "Unexpected", nullptr}},
				std::move(traversal));

		EXPECT_THROW(static_cast<void>(runtime.execute(set, nativeMutationBounds(), QueryContext{})), std::logic_error);
		EXPECT_TRUE(backend->graph().edges.contains(relationshipId));
		EXPECT_TRUE(backend->graph().nodes.contains(sourceId));
		EXPECT_TRUE(backend->graph().nodes.contains(targetId));
		EXPECT_EQ(backend->graph().mutation_batches, publicationsBefore);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsInvalidRelationshipNodeEndpointsBeforePublication) {
		const auto executeAndReject = [](std::unique_ptr<logical::LogicalOperator> root) {
			auto backend = std::make_shared<TestLogicalGraph>();
			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
			EXPECT_THROW(static_cast<void>(runtime.execute(*root, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
			EXPECT_TRUE(backend->graph().nodes.empty());
			EXPECT_EQ(backend->edgeCount(), 0U);
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		};

		// A missing endpoint must not turn into a partially staged relationship.
		auto source = std::make_unique<logical::LogicalCreateNode>(
				"source", std::vector<std::string>{"Source"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		auto missingTargetEdge = std::make_unique<logical::LogicalCreateEdge>(
				"relationship", "LINK", PropertyMap{}, "source", "missing", std::move(source));
		executeAndReject(std::move(missingTargetEdge));

		// An endpoint bound to a relationship is also invalid: CREATE Edge accepts
		// only Node references for its source and target fields.
		auto firstSource = std::make_unique<logical::LogicalCreateNode>(
				"firstSource", std::vector<std::string>{"Source"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		auto firstTarget = std::make_unique<logical::LogicalCreateNode>(
				"firstTarget", std::vector<std::string>{"Target"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{}, std::move(firstSource));
		auto firstRelationship = std::make_unique<logical::LogicalCreateEdge>(
				"firstRelationship", "LINK", PropertyMap{}, "firstSource", "firstTarget", std::move(firstTarget));
		auto relationshipWithEdgeEndpoint = std::make_unique<logical::LogicalCreateEdge>(
				"relationship", "LINK", PropertyMap{}, "firstRelationship", "firstTarget",
				std::move(firstRelationship));
		executeAndReject(std::move(relationshipWithEdgeEndpoint));
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

	TEST(LogicalPlanMutationRuntimeTest, RejectsEmptyAndForgedNestedPropertyMutationChainsBeforeOpeningASnapshot) {
		auto backend = std::make_shared<TestLogicalGraph>();
		// These public logical-plan shapes are rejected by chain collection, before
		// the runtime would need a snapshot to resolve a read binding.
		backend->setSnapshotMode(TestSnapshotMode::TSSM_NULL);
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const auto propertyItem = [] {
			return logical::LogicalSetItem{logical::SetActionType::LSET_PROPERTY, "node", "value",
					std::make_shared<expressions::LiteralExpression>(PropertyValue(1))};
		};

		logical::LogicalSet emptySet({}, std::make_unique<logical::LogicalSingleRow>());
		EXPECT_THROW(static_cast<void>(runtime.execute(emptySet, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		logical::LogicalRemove emptyRemove({}, std::make_unique<logical::LogicalSingleRow>());
		EXPECT_THROW(static_cast<void>(runtime.execute(emptyRemove, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		logical::LogicalSet forgedSetChild(
				{propertyItem()},
				std::make_unique<MalformedLogicalOperator>(logical::LogicalOpType::LOP_SET,
															 std::vector<logical::LogicalOperator *>{}));
		EXPECT_THROW(static_cast<void>(runtime.execute(forgedSetChild, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		logical::LogicalRemove forgedRemoveChild(
				{{logical::LogicalRemoveActionType::LREM_PROPERTY, "node", "value"}},
				std::make_unique<MalformedLogicalOperator>(logical::LogicalOpType::LOP_REMOVE,
															 std::vector<logical::LogicalOperator *>{}));
		EXPECT_THROW(static_cast<void>(runtime.execute(forgedRemoveChild, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		EXPECT_EQ(backend->graph().mutation_batches, 0U);
		EXPECT_TRUE(backend->graph().nodes.empty());
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

	TEST(LogicalPlanMutationRuntimeTest, RejectsUnboundCreateChainUpdatesBeforePublication) {
		const auto createdNode = [] {
			return std::make_unique<logical::LogicalCreateNode>(
					"created", std::vector<std::string>{"Created"}, PropertyMap{},
					std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
					std::make_unique<logical::LogicalSingleRow>());
		};
		const auto rejects = [&](std::unique_ptr<logical::LogicalOperator> root) {
			auto backend = std::make_shared<TestLogicalGraph>();
			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);

			EXPECT_THROW(static_cast<void>(runtime.execute(*root, nativeMutationBounds(), QueryContext{})),
						 std::logic_error);
			EXPECT_EQ(backend->countNodesWithLabel("Created"), 0U);
			EXPECT_EQ(backend->edgeCount(), 0U);
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		};

		// Parser-built plans always bind these variables, but callers of the public
		// logical-plan API can construct this shape directly. The failed update must
		// not publish the preceding CREATE work.
		rejects(std::make_unique<logical::LogicalSet>(
				std::vector<logical::LogicalSetItem>{{logical::SetActionType::LSET_PROPERTY, "missing", "value",
															 std::make_shared<expressions::LiteralExpression>(PropertyValue(1))}},
				createdNode()));
		rejects(std::make_unique<logical::LogicalRemove>(
				std::vector<logical::LogicalRemoveItem>{{logical::LogicalRemoveActionType::LREM_PROPERTY,
																	  "missing", "value"}},
				createdNode()));
		rejects(std::make_unique<logical::LogicalDelete>(std::vector<std::string>{"missing"}, false, createdNode()));
	}

	TEST(LogicalPlanMutationRuntimeTest, WriteOnlyCallResultPaginationDoesNotReduceItsAtomicMutationInput) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(10)}}));
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(20)}}));
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(30)}}));
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);

		// Result ORDER BY/SKIP/LIMIT belongs outside the write-only CALL. It
		// narrows visible rows only after all outer rows have staged one batch.
		auto body = std::make_unique<logical::LogicalCreateNode>(
				"child", std::vector<std::string>{"PagedChild"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		auto call = std::make_unique<logical::LogicalCallSubquery>(
				std::make_unique<logical::LogicalNodeScan>("seed", std::vector<std::string>{"Seed"}),
				std::move(body), std::vector<std::string>{}, std::vector<std::string>{});
		auto sort = std::make_unique<logical::LogicalSort>(
				std::move(call), std::vector<logical::LogicalSortItem>{
						{std::make_shared<expressions::VariableReferenceExpression>("seed", "value"), false}});
		auto project = std::make_unique<logical::LogicalProject>(
				std::move(sort), std::vector<logical::LogicalProjectItem>{
						{std::make_shared<expressions::VariableReferenceExpression>("seed", "value"), "value"}});
		auto skip = std::make_unique<logical::LogicalSkip>(std::move(project), 1);
		logical::LogicalLimit root(std::move(skip), 1);

		const QueryResult result = runtime.execute(root, nativeMutationBounds(), QueryContext{});
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getPipelineRowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(20));
		EXPECT_EQ(backend->countNodesWithLabel("PagedChild"), 3U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsResultPaginationForNonCreateWriteOnlyCallBeforePublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);

		// A SET-only CALL has a separate fallback path. Result pagination would
		// make its mutation cardinality ambiguous, so direct public plans must
		// fail before they can alter the matched Node.
		auto set = std::make_unique<logical::LogicalSet>(
				std::vector<logical::LogicalSetItem>{{logical::SetActionType::LSET_PROPERTY, "seed", "value",
															std::make_shared<expressions::LiteralExpression>(PropertyValue(2))}},
				std::make_unique<logical::LogicalSingleRow>());
		auto call = std::make_unique<logical::LogicalCallSubquery>(
				std::make_unique<logical::LogicalNodeScan>("seed", std::vector<std::string>{"Seed"}), std::move(set),
				std::vector<std::string>{}, std::vector<std::string>{});
		auto sort = std::make_unique<logical::LogicalSort>(
				std::move(call), std::vector<logical::LogicalSortItem>{
						{std::make_shared<expressions::VariableReferenceExpression>("seed", "value"), true}});
		logical::LogicalLimit root(std::move(sort), 1);

		EXPECT_THROW(static_cast<void>(runtime.execute(root, nativeMutationBounds(), QueryContext{})), std::runtime_error);
		EXPECT_EQ(backend->graph().nodes.at(seedId).properties.at("value"), PropertyValue(1));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsMalformedWriteOnlyCallExportItemsBeforePublication) {
		const auto rejects = [](std::vector<logical::LogicalProjectItem> items,
									std::vector<std::string> returnedVariables) {
			auto backend = std::make_shared<TestLogicalGraph>();
			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
			auto create = std::make_unique<logical::LogicalCreateNode>(
					"created", std::vector<std::string>{"RejectedExport"}, PropertyMap{},
					std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
					std::make_unique<logical::LogicalSingleRow>());
			auto exported = std::make_unique<logical::LogicalProject>(std::move(create), std::move(items));
			logical::LogicalCallSubquery call(std::make_unique<logical::LogicalSingleRow>(), std::move(exported),
					std::vector<std::string>{}, std::move(returnedVariables));

			EXPECT_THROW(static_cast<void>(runtime.execute(call, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
			EXPECT_EQ(backend->countNodesWithLabel("RejectedExport"), 0U);
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		};

		// The CALL constructor is intentionally permissive while the optimizer is
		// assembling a plan. Its execution boundary must reject every malformed
		// export shape before the inner CREATE is published.
		rejects({{std::make_shared<expressions::LiteralExpression>(PropertyValue(1)), ""}}, {""});
		rejects({{std::shared_ptr<expressions::Expression>{}, "value"}}, {"value"});
		rejects({{std::make_shared<expressions::LiteralExpression>(PropertyValue(1)), "value"},
				 {std::make_shared<expressions::LiteralExpression>(PropertyValue(2)), "value"}},
				{"value", "value"});
		// Export aliases are public logical-plan input. Self-references and cycles
		// must fail while preparing the result, before the inner CREATE commits.
		rejects({{std::make_shared<expressions::VariableReferenceExpression>("value"), "value"}}, {"value"});
		rejects({{std::make_shared<expressions::VariableReferenceExpression>("right"), "left"},
				 {std::make_shared<expressions::VariableReferenceExpression>("left"), "right"}},
				{"left", "right"});
	}

	TEST(LogicalPlanMutationRuntimeTest, WriteOnlyCallExportsCreatedNodeAfterItsAtomicPublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		auto create = std::make_unique<logical::LogicalCreateNode>(
				"child", std::vector<std::string>{"ExportedChild"}, PropertyMap{{"value", PropertyValue(7)}},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		auto exported = std::make_unique<logical::LogicalProject>(
				std::move(create),
				std::vector<logical::LogicalProjectItem>{{
						std::make_shared<expressions::VariableReferenceExpression>("child"), "child"}});
		logical::LogicalCallSubquery call(std::make_unique<logical::LogicalSingleRow>(), std::move(exported),
				std::vector<std::string>{}, std::vector<std::string>{"child"});

		const QueryResult result = runtime.execute(call, nativeMutationBounds(), QueryContext{});
		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalNodeResult &child = result.getRows().front().at("child").asLogicalNode();
		EXPECT_TRUE(std::ranges::find(child.labels, "ExportedChild") != child.labels.end());
		EXPECT_EQ(child.properties.at("value"), PropertyValue(7));
		EXPECT_EQ(backend->countNodesWithLabel("ExportedChild"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, WriteOnlyCallPassesExportedNodesThroughAnOuterProjection) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		auto create = std::make_unique<logical::LogicalCreateNode>(
				"child", std::vector<std::string>{"OuterExportedChild"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		auto exported = std::make_unique<logical::LogicalProject>(
				std::move(create),
				std::vector<logical::LogicalProjectItem>{{
						std::make_shared<expressions::VariableReferenceExpression>("child"), "child"}});
		auto call = std::make_unique<logical::LogicalCallSubquery>(
				std::make_unique<logical::LogicalSingleRow>(), std::move(exported), std::vector<std::string>{},
				std::vector<std::string>{"child"});
		logical::LogicalProject root(
				std::move(call),
				std::vector<logical::LogicalProjectItem>{{
						std::make_shared<expressions::VariableReferenceExpression>("child"), "result"}});

		const QueryResult result = runtime.execute(root, nativeMutationBounds(), QueryContext{});
		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalNodeResult &child = result.getRows().front().at("result").asLogicalNode();
		EXPECT_TRUE(std::ranges::find(child.labels, "OuterExportedChild") != child.labels.end());
		EXPECT_EQ(backend->countNodesWithLabel("OuterExportedChild"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsRedeclaredCreatedEntityVariablesBeforePublication) {
		const auto execute = [](std::unique_ptr<logical::LogicalOperator> root) {
			auto backend = std::make_shared<TestLogicalGraph>();
			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
			EXPECT_THROW(static_cast<void>(runtime.execute(*root, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
			EXPECT_TRUE(backend->graph().nodes.empty());
			EXPECT_EQ(backend->edgeCount(), 0U);
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		};

		// Public logical plans can be assembled without parser validation. A second
		// declaration must not overwrite an existing CREATE binding or publish a
		// partial batch.
		auto firstNode = std::make_unique<logical::LogicalCreateNode>(
				"node", std::vector<std::string>{"First"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		execute(std::make_unique<logical::LogicalCreateNode>(
				"node", std::vector<std::string>{"Second"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{}, std::move(firstNode)));

		auto source = std::make_unique<logical::LogicalCreateNode>(
				"source", std::vector<std::string>{"Source"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		auto target = std::make_unique<logical::LogicalCreateNode>(
				"target", std::vector<std::string>{"Target"}, PropertyMap{},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{}, std::move(source));
		auto firstEdge = std::make_unique<logical::LogicalCreateEdge>(
				"relationship", "FIRST", PropertyMap{}, "source", "target", std::move(target));
		execute(std::make_unique<logical::LogicalCreateEdge>(
				"relationship", "SECOND", PropertyMap{}, "source", "target", std::move(firstEdge)));
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsMergeVariableConflictsBeforePublication) {
		const auto executeAndReject = [](std::unique_ptr<logical::LogicalOperator> root) {
			auto backend = std::make_shared<TestLogicalGraph>();
			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);

			EXPECT_THROW(static_cast<void>(runtime.execute(*root, nativeMutationBounds(), QueryContext{})),
						 std::logic_error);
			EXPECT_TRUE(backend->graph().nodes.empty());
			EXPECT_EQ(backend->edgeCount(), 0U);
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		};

		// Typed logical plans can be assembled directly without parser-level
		// variable-declaration validation. A second relationship binding must fail
		// before the first staged entity reaches the backend.
		auto source = std::make_unique<logical::LogicalMergeNode>(
				"source", std::vector<std::string>{"Source"}, PropertyMap{}, std::vector<logical::MergeSetAction>{},
				std::vector<logical::MergeSetAction>{}, std::make_unique<logical::LogicalSingleRow>());
		auto target = std::make_unique<logical::LogicalMergeNode>(
				"target", std::vector<std::string>{"Target"}, PropertyMap{}, std::vector<logical::MergeSetAction>{},
				std::vector<logical::MergeSetAction>{}, std::move(source));
		auto firstEdge = std::make_unique<logical::LogicalMergeEdge>(
				"source", "relationship", "target", "FIRST", "out", PropertyMap{},
				std::vector<logical::MergeSetAction>{}, std::vector<logical::MergeSetAction>{}, std::move(target));
		executeAndReject(std::make_unique<logical::LogicalMergeEdge>(
				"source", "relationship", "target", "SECOND", "out", PropertyMap{},
				std::vector<logical::MergeSetAction>{}, std::vector<logical::MergeSetAction>{}, std::move(firstEdge)));

		auto scalarInput = std::make_unique<logical::LogicalProject>(
				std::make_unique<logical::LogicalSingleRow>(), std::vector<logical::LogicalProjectItem>{{
						std::make_shared<expressions::LiteralExpression>(PropertyValue(1)), "relationship"}});
		auto scalarSource = std::make_unique<logical::LogicalMergeNode>(
				"source", std::vector<std::string>{"Source"}, PropertyMap{}, std::vector<logical::MergeSetAction>{},
				std::vector<logical::MergeSetAction>{}, std::move(scalarInput));
		auto scalarTarget = std::make_unique<logical::LogicalMergeNode>(
				"target", std::vector<std::string>{"Target"}, PropertyMap{}, std::vector<logical::MergeSetAction>{},
				std::vector<logical::MergeSetAction>{}, std::move(scalarSource));
		executeAndReject(std::make_unique<logical::LogicalMergeEdge>(
				"source", "relationship", "target", "LINK", "out", PropertyMap{},
				std::vector<logical::MergeSetAction>{}, std::vector<logical::MergeSetAction>{}, std::move(scalarTarget)));

		auto scalarNodeInput = std::make_unique<logical::LogicalProject>(
				std::make_unique<logical::LogicalSingleRow>(), std::vector<logical::LogicalProjectItem>{{
						std::make_shared<expressions::LiteralExpression>(PropertyValue(1)), "node"}});
		executeAndReject(std::make_unique<logical::LogicalMergeNode>(
				"node", std::vector<std::string>{"Node"}, PropertyMap{}, std::vector<logical::MergeSetAction>{},
				std::vector<logical::MergeSetAction>{}, std::move(scalarNodeInput)));

		auto edgeSource = std::make_unique<logical::LogicalMergeNode>(
				"source", std::vector<std::string>{"Source"}, PropertyMap{}, std::vector<logical::MergeSetAction>{},
				std::vector<logical::MergeSetAction>{}, std::make_unique<logical::LogicalSingleRow>());
		auto edgeTarget = std::make_unique<logical::LogicalMergeNode>(
				"target", std::vector<std::string>{"Target"}, PropertyMap{}, std::vector<logical::MergeSetAction>{},
				std::vector<logical::MergeSetAction>{}, std::move(edgeSource));
		auto edgeBinding = std::make_unique<logical::LogicalMergeEdge>(
				"source", "binding", "target", "LINK", "out", PropertyMap{}, std::vector<logical::MergeSetAction>{},
				std::vector<logical::MergeSetAction>{}, std::move(edgeTarget));
		executeAndReject(std::make_unique<logical::LogicalMergeNode>(
				"binding", std::vector<std::string>{"Node"}, PropertyMap{}, std::vector<logical::MergeSetAction>{},
				std::vector<logical::MergeSetAction>{}, std::move(edgeBinding)));
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

	TEST(LogicalPlanMutationRuntimeTest, RejectsMalformedAggregateMutationChildrenBeforeOpeningASnapshot) {
		auto backend = std::make_shared<TestLogicalGraph>();
		backend->setSnapshotMode(TestSnapshotMode::TSSM_NULL);
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);

		// Aggregate execution has a narrower mutation child contract than the
		// general dispatcher. Exercise both concrete-shape checks before a read
		// snapshot can be acquired or a write can be published.
		auto forgedProject = std::make_unique<MalformedLogicalOperator>(
				logical::LogicalOpType::LOP_PROJECT, std::vector<logical::LogicalOperator *>{});
		logical::LogicalAggregate invalidProject(std::move(forgedProject), {}, {{"count", nullptr, "count"}});
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidProject, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		auto missingMutationChild = std::make_unique<logical::LogicalProject>(nullptr,
				std::vector<logical::LogicalProjectItem>{});
		logical::LogicalAggregate invalidChild(std::move(missingMutationChild), {}, {{"count", nullptr, "count"}});
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidChild, nativeMutationBounds(), QueryContext{})),
					 std::runtime_error);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
		EXPECT_TRUE(backend->graph().nodes.empty());
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsIncompletePublicCallPipelineShapesBeforeOpeningASnapshot) {
		auto backend = std::make_shared<TestLogicalGraph>();
		backend->setSnapshotMode(TestSnapshotMode::TSSM_NULL);
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const auto rejectsWithoutPublishing = [&](const logical::LogicalOperator &root, const char *expectedMessage) {
			try {
				static_cast<void>(runtime.execute(root, nativeMutationBounds(), QueryContext{}));
				FAIL() << "expected a malformed public CALL pipeline to be rejected";
			} catch (const std::exception &error) {
				EXPECT_NE(std::string(error.what()).find(expectedMessage), std::string::npos);
			}
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
			EXPECT_TRUE(backend->graph().nodes.empty());
			EXPECT_EQ(backend->edgeCount(), 0U);
		};

		// The public constructors permit an incomplete plan while an optimizer is
		// assembling it. The execution boundary must reject every such plan before
		// it can acquire a snapshot or publish any portion of a mutation.
		logical::LogicalLimit missingLimitChild(nullptr, 1);
		rejectsWithoutPublishing(missingLimitChild, "requires unary CREATE operators");
		logical::LogicalSkip missingSkipChild(nullptr, 1);
		rejectsWithoutPublishing(missingSkipChild, "requires unary CREATE operators");
		logical::LogicalProject missingProjectChild(nullptr, {});
		rejectsWithoutPublishing(missingProjectChild, "requires unary CREATE operators");
		logical::LogicalSort missingSortChild(nullptr, {});
		rejectsWithoutPublishing(missingSortChild, "requires unary CREATE operators");

		logical::LogicalCallSubquery missingWriteOnlyBody(nullptr, nullptr, {}, {});
		rejectsWithoutPublishing(missingWriteOnlyBody, "requires an update body");
		logical::LogicalCallSubquery missingBatchedBody(nullptr, nullptr, {}, {}, true, 1);
		rejectsWithoutPublishing(missingBatchedBody, "requires a write-only subquery body");

		logical::LogicalCallSubquery unsupportedBatchSize(
				nullptr, std::make_unique<logical::LogicalSingleRow>(), {}, {}, false, 1);
		rejectsWithoutPublishing(unsupportedBatchSize, "does not support CALL subqueries with transaction batching");
		logical::LogicalCallSubquery unsupportedBody(
				nullptr, std::make_unique<logical::LogicalNodeScan>("node", std::vector<std::string>{}), {}, {});
		rejectsWithoutPublishing(unsupportedBody, "supports only CREATE, SET, REMOVE, and DELETE update chains");

		logical::LogicalCallSubquery exportedWithoutProjection(
				nullptr, std::make_unique<logical::LogicalSingleRow>(), {}, {"result"});
		rejectsWithoutPublishing(exportedWithoutProjection, "invalid exported-variable projection");
		logical::LogicalCallSubquery exportedWithWrongColumns(
				nullptr,
				std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(),
						std::vector<logical::LogicalProjectItem>{}),
				{}, {"result"});
		rejectsWithoutPublishing(exportedWithWrongColumns, "exported-variable projection has invalid columns");
		logical::LogicalCallSubquery exportedWithoutMutationChild(
				nullptr,
				std::make_unique<logical::LogicalProject>(
						nullptr,
						std::vector<logical::LogicalProjectItem>{{
								std::make_shared<expressions::LiteralExpression>(PropertyValue(1)), "result"}}),
				{}, {"result"});
		rejectsWithoutPublishing(exportedWithoutMutationChild, "projection requires a mutation child");

		logical::LogicalCallSubquery importedWithoutWith(
				nullptr, std::make_unique<logical::LogicalCreateNode>(
						"created", std::vector<std::string>{}, PropertyMap{},
						std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
						std::make_unique<logical::LogicalSingleRow>()),
				{"outer"}, {});
		rejectsWithoutPublishing(importedWithoutWith, "imports require a WITH projection");

		logical::LogicalSet bodyWithoutUnarySeed(
				{{logical::SetActionType::LSET_PROPERTY, "node", "value",
				  std::make_shared<expressions::LiteralExpression>(PropertyValue(1))}},
				nullptr);
		logical::LogicalCallSubquery incompleteUpdateBody(nullptr, bodyWithoutUnarySeed.clone(), {}, {});
		rejectsWithoutPublishing(incompleteUpdateBody, "update body must be a unary chain ending in SingleRow");
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsForgedMutationPipelineOperatorsBeforeOpeningASnapshot) {
		auto backend = std::make_shared<TestLogicalGraph>();
		backend->setSnapshotMode(TestSnapshotMode::TSSM_NULL);
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const auto expectsMalformedOperatorRejection = [&](logical::LogicalOpType type) {
			MalformedLogicalOperator forged(type, {});
			EXPECT_THROW(static_cast<void>(runtime.execute(forged, nativeMutationBounds(), QueryContext{})),
						 std::logic_error);
		};

		// The logical-plan API is public. A caller can forge an operator whose
		// advertised enum does not match its concrete class; every mutation entry
		// point must reject that before opening a snapshot or staging a write.
		expectsMalformedOperatorRejection(logical::LogicalOpType::LOP_AGGREGATE);
		expectsMalformedOperatorRejection(logical::LogicalOpType::LOP_LIMIT);
		expectsMalformedOperatorRejection(logical::LogicalOpType::LOP_SKIP);
		expectsMalformedOperatorRejection(logical::LogicalOpType::LOP_PROJECT);
		expectsMalformedOperatorRejection(logical::LogicalOpType::LOP_SORT);
		expectsMalformedOperatorRejection(logical::LogicalOpType::LOP_CALL_SUBQUERY);
		expectsMalformedOperatorRejection(logical::LogicalOpType::LOP_FOREACH);
		expectsMalformedOperatorRejection(logical::LogicalOpType::LOP_CREATE_NODE);
		expectsMalformedOperatorRejection(logical::LogicalOpType::LOP_CREATE_EDGE);
		expectsMalformedOperatorRejection(logical::LogicalOpType::LOP_SET);
		expectsMalformedOperatorRejection(logical::LogicalOpType::LOP_REMOVE);
		expectsMalformedOperatorRejection(logical::LogicalOpType::LOP_DELETE);
		EXPECT_THROW(static_cast<void>(runtime.execute(
								MalformedLogicalOperator(logical::LogicalOpType::LOP_SINGLE_ROW, {}),
								nativeMutationBounds(), QueryContext{})),
				 std::logic_error);

		execution::LogicalPlanReadRuntimeBounds zeroBound = nativeMutationBounds();
		zeroBound.maximum_rows = 0U;
		logical::LogicalSingleRow seed;
		EXPECT_THROW(static_cast<void>(runtime.execute(seed, zeroBound, QueryContext{})), std::invalid_argument);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
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

	TEST(LogicalPlanMutationRuntimeTest, RejectsMutationWhenNodeOrAdjacencyReadsReachTheirBounds) {
		// A mutation must prove that its complete read input was exhausted. Stopping
		// at either a Node or adjacency bound cannot publish a partial update.
		{
			auto backend = std::make_shared<TestLogicalGraph>();
			const uint64_t first = backend->addNode({"Bounded"}, {{"value", PropertyValue(1)}});
			const uint64_t second = backend->addNode({"Bounded"}, {{"value", PropertyValue(2)}});
			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
			execution::LogicalPlanReadRuntimeBounds bounds = nativeMutationBounds();
			bounds.maximum_node_records = 1U;
			bounds.maximum_node_page_records = 1U;
			logical::LogicalSet update(
					{{logical::SetActionType::LSET_PROPERTY, "node", "updated",
					  std::make_shared<expressions::LiteralExpression>(PropertyValue(true))}},
					std::make_unique<logical::LogicalNodeScan>("node", std::vector<std::string>{"Bounded"}));

			EXPECT_THROW(static_cast<void>(runtime.execute(update, bounds, QueryContext{})), std::runtime_error);
			EXPECT_FALSE(backend->graph().nodes.at(first).properties.contains("updated"));
			EXPECT_FALSE(backend->graph().nodes.at(second).properties.contains("updated"));
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		}

		{
			auto backend = std::make_shared<TestLogicalGraph>();
			const uint64_t sourceId = backend->addNode({"Source"}, {});
			const uint64_t firstTarget = backend->addNode({"Target"}, {});
			const uint64_t secondTarget = backend->addNode({"Target"}, {});
			QueryEngine engine(backend, backend);
			static_cast<void>(engine.execute("MATCH (source:Source), (first:Target), (second:Target) "
											 "CREATE (source)-[:LINK]->(first), (source)-[:LINK]->(second)"));

			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
			execution::LogicalPlanReadRuntimeBounds bounds = nativeMutationBounds();
			bounds.maximum_edges_per_node = 1U;
			bounds.maximum_adjacency_page_records = 1U;
			auto traversal = std::make_unique<logical::LogicalTraversal>(
					std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source",
					"relationship", "target", "LINK", "out", std::vector<std::string>{"Target"});
			logical::LogicalSet update(
					{{logical::SetActionType::LSET_PROPERTY, "relationship", "updated",
					  std::make_shared<expressions::LiteralExpression>(PropertyValue(true))}},
					std::move(traversal));

			EXPECT_THROW(static_cast<void>(runtime.execute(update, bounds, QueryContext{})), std::runtime_error);
			EXPECT_TRUE(backend->graph().nodes.contains(sourceId));
			EXPECT_TRUE(backend->graph().nodes.contains(firstTarget));
			EXPECT_TRUE(backend->graph().nodes.contains(secondTarget));
			for (const auto &[_, edge]: backend->graph().edges) {
				EXPECT_FALSE(edge.properties.contains("updated"));
			}
			EXPECT_EQ(backend->graph().mutation_batches, 1U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsInvalidRuntimeMutationBindingsBeforePublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const auto seedScan = [] {
			return std::make_unique<logical::LogicalNodeScan>("node", std::vector<std::string>{"Seed"});
		};
		const auto rejectsSet = [&](logical::LogicalSetItem item) {
			logical::LogicalSet set({std::move(item)}, seedScan());
			EXPECT_THROW(static_cast<void>(runtime.execute(set, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
		};

		// A parsed plan normally cannot form these shapes, but public native callers
		// may supply them directly. They must fail after reading the binding but
		// before a replacement batch can be published.
		rejectsSet({logical::SetActionType::LSET_PROPERTY, "missing", "value",
					std::make_shared<expressions::LiteralExpression>(PropertyValue(2))});
		rejectsSet({logical::SetActionType::LSET_MAP_MERGE, "node", "",
					std::make_shared<expressions::LiteralExpression>(PropertyValue(2))});
		rejectsSet({logical::SetActionType::LSET_MAP_MERGE, "node", "",
					std::make_shared<expressions::LiteralExpression>(
							PropertyValue(PropertyValue::MapType{{"", PropertyValue(2)}}))});

		logical::LogicalDelete missingBinding({"missing"}, false, seedScan());
		EXPECT_THROW(static_cast<void>(runtime.execute(missingBinding, nativeMutationBounds(), QueryContext{})),
				 std::logic_error);
		logical::LogicalDelete scalarBinding(
				{"scalar"}, false,
				std::make_unique<logical::LogicalProject>(
						std::make_unique<logical::LogicalSingleRow>(),
						std::vector<logical::LogicalProjectItem>{
								{std::make_shared<expressions::LiteralExpression>(PropertyValue(1)), "scalar"}}));
		EXPECT_THROW(static_cast<void>(runtime.execute(scalarBinding, nativeMutationBounds(), QueryContext{})),
				 std::logic_error);

		EXPECT_EQ(backend->graph().nodes.at(nodeId).properties.at("value"), PropertyValue(1));
		EXPECT_EQ(backend->graph().nodes.size(), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, HandlesOnlyEmptyCreateNodeRedeclarations) {
		// Public native plans may reuse a CREATE variable while assembling a unary
		// chain. An empty redeclaration is the same created Node; adding any shape
		// to that redeclaration would make the request ambiguous and must fail.
		{
			auto backend = std::make_shared<TestLogicalGraph>();
			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
			auto initial = std::make_unique<logical::LogicalCreateNode>(
					"node", std::vector<std::string>{}, PropertyMap{},
					std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
					std::make_unique<logical::LogicalSingleRow>());
			logical::LogicalCreateNode redeclaration(
					"node", std::vector<std::string>{}, PropertyMap{},
					std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{}, std::move(initial));

			static_cast<void>(runtime.execute(redeclaration, nativeMutationBounds(), QueryContext{}));
			EXPECT_EQ(backend->graph().nodes.size(), 1U);
			EXPECT_EQ(backend->graph().mutation_batches, 1U);
		}

		{
			auto backend = std::make_shared<TestLogicalGraph>();
			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
			auto initial = std::make_unique<logical::LogicalCreateNode>(
					"node", std::vector<std::string>{}, PropertyMap{},
					std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
					std::make_unique<logical::LogicalSingleRow>());
			logical::LogicalCreateNode redeclaration(
					"node", std::vector<std::string>{"Unexpected"}, PropertyMap{},
					std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{}, std::move(initial));

			EXPECT_THROW(static_cast<void>(runtime.execute(redeclaration, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
			EXPECT_TRUE(backend->graph().nodes.empty());
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		}
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsMalformedInlinePropertiesBeforePublishingAMutation) {
		const auto runNodeMutation = [](TestInlinePropertyMode propertyMode) {
			auto backend = std::make_shared<TestLogicalGraph>();
			const uint64_t nodeId = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
			backend->setInlinePropertyMode(propertyMode);
			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
			logical::LogicalSet update(
					{{logical::SetActionType::LSET_PROPERTY, "node", "updated",
					  std::make_shared<expressions::LiteralExpression>(PropertyValue(2))}},
					std::make_unique<logical::LogicalNodeScan>("node", std::vector<std::string>{"Seed"}));

			EXPECT_THROW(static_cast<void>(runtime.execute(update, nativeMutationBounds(), QueryContext{})),
						 std::runtime_error);
			EXPECT_EQ(backend->graph().nodes.at(nodeId).properties.at("value"), PropertyValue(1));
			EXPECT_FALSE(backend->graph().nodes.at(nodeId).properties.contains("updated"));
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		};

		// Read backends are an ABI boundary. A direct property collection must be
		// canonical before the mutation runtime can calculate and publish a replacement.
		runNodeMutation(TestInlinePropertyMode::TIPM_EMPTY_KEY);
		runNodeMutation(TestInlinePropertyMode::TIPM_DUPLICATE_KEY);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsMalformedMergeActionsAndDirectionsWithoutPublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const auto literal = std::make_shared<expressions::LiteralExpression>(PropertyValue(7));
		const auto rejectsMalformedNodeAction = [&](logical::MergeSetAction action) {
			logical::LogicalMergeNode merge("node", {"Candidate"}, {}, {std::move(action)}, {},
									   std::make_unique<logical::LogicalSingleRow>());
			EXPECT_THROW(static_cast<void>(runtime.execute(merge, nativeMutationBounds(), QueryContext{})),
						 std::logic_error);
		};

		// Direct logical-plan callers can bypass parser validation. A malformed
		// MERGE action must fail before the batch can publish its staged entities.
		rejectsMalformedNodeAction({"other", "value", literal});
		rejectsMalformedNodeAction({"node", "", literal});
		rejectsMalformedNodeAction({"node", "value", nullptr});
		logical::LogicalMergeNode anonymous("", {"Candidate"}, {}, {{"", "value", literal}}, {},
								  std::make_unique<logical::LogicalSingleRow>());
		EXPECT_THROW(static_cast<void>(runtime.execute(anonymous, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		auto source = std::make_unique<logical::LogicalMergeNode>(
				"source", std::vector<std::string>{"Source"}, PropertyMap{}, std::vector<logical::MergeSetAction>{},
				std::vector<logical::MergeSetAction>{}, std::make_unique<logical::LogicalSingleRow>());
		auto target = std::make_unique<logical::LogicalMergeNode>(
				"target", std::vector<std::string>{"Target"}, PropertyMap{}, std::vector<logical::MergeSetAction>{},
				std::vector<logical::MergeSetAction>{}, std::move(source));
		logical::LogicalMergeEdge invalidDirection("source", "relationship", "target", "LINK", "sideways", {},
										 {}, {}, std::move(target));
		EXPECT_THROW(static_cast<void>(runtime.execute(invalidDirection, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		EXPECT_EQ(backend->countNodesWithLabel("Candidate"), 0U);
		EXPECT_EQ(backend->countNodesWithLabel("Source"), 0U);
		EXPECT_EQ(backend->countNodesWithLabel("Target"), 0U);
		EXPECT_EQ(backend->edgeCount(), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsMalformedMergeIdentityShapesBeforePublication) {
		const auto rejects = [](std::vector<std::string> labels, PropertyMap properties,
									std::unordered_map<std::string, std::shared_ptr<expressions::Expression>> expressions = {}) {
			auto backend = std::make_shared<TestLogicalGraph>();
			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
			logical::LogicalMergeNode merge("node", std::move(labels), std::move(properties), {}, {},
					std::make_unique<logical::LogicalSingleRow>(), std::move(expressions));

			EXPECT_THROW(static_cast<void>(runtime.execute(merge, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
			EXPECT_TRUE(backend->graph().nodes.empty());
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		};

		// MERGE identity values are both a match key and a potential CREATE value.
		// Direct public plans must reject malformed labels and static/dynamic key
		// collections before they can create an entity with ambiguous identity.
		rejects({""}, {});
		rejects({"Candidate", "Candidate"}, {});
		rejects({"Candidate"}, {{"", PropertyValue(1)}});
		rejects({"Candidate"}, {{"value", PropertyValue{}}});
		rejects({"Candidate"}, {}, {{"", std::make_shared<expressions::LiteralExpression>(PropertyValue(1))}});
		rejects({"Candidate"}, {}, {{"value", std::shared_ptr<expressions::Expression>{}}});
		rejects({"Candidate"}, {{"value", PropertyValue(1)}},
				{{"value", std::make_shared<expressions::LiteralExpression>(PropertyValue(2))}});
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsMalformedReadMutationResultItemsBeforePublication) {
		const auto rejects = [](std::vector<logical::LogicalProjectItem> items) {
			auto backend = std::make_shared<TestLogicalGraph>();
			const uint64_t nodeId = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
			auto set = std::make_unique<logical::LogicalSet>(
					std::vector<logical::LogicalSetItem>{{logical::SetActionType::LSET_PROPERTY, "node", "value",
															std::make_shared<expressions::LiteralExpression>(PropertyValue(2))}},
					std::make_unique<logical::LogicalNodeScan>("node", std::vector<std::string>{"Seed"}));
			logical::LogicalProject root(std::move(set), std::move(items));

			EXPECT_THROW(static_cast<void>(runtime.execute(root, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
			EXPECT_EQ(backend->graph().nodes.at(nodeId).properties.at("value"), PropertyValue(1));
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		};

		// A SET statement evaluates its complete RETURN surface before publication.
		// This protects direct logical-plan callers from durable writes followed by
		// a malformed result projection.
		rejects({{std::make_shared<expressions::VariableReferenceExpression>("node", "value"), ""}});
		rejects({{std::shared_ptr<expressions::Expression>{}, "value"}});
		rejects({{std::make_shared<expressions::VariableReferenceExpression>("node", "value"), "value"},
				 {std::make_shared<expressions::LiteralExpression>(PropertyValue(2)), "value"}});
	}

	TEST(LogicalPlanMutationRuntimeTest, RejectsMalformedCreatePropertyExpressionsBeforePublication) {
		const auto rejects = [](PropertyMap properties,
									std::unordered_map<std::string, std::shared_ptr<expressions::Expression>> expressions) {
			auto backend = std::make_shared<TestLogicalGraph>();
			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
			logical::LogicalCreateNode create("created", {"RejectedCreateProperty"}, std::move(properties),
					std::move(expressions), std::make_unique<logical::LogicalSingleRow>());

			EXPECT_THROW(static_cast<void>(runtime.execute(create, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
			EXPECT_EQ(backend->countNodesWithLabel("RejectedCreateProperty"), 0U);
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		};

		// Expression properties are folded into the same canonical collection as
		// literal CREATE properties. Reject bad keys, missing expressions, and
		// duplicate literal/dynamic declarations before the write batch exists.
		rejects({{"", PropertyValue(1)}}, {});
		rejects({}, {{"", std::make_shared<expressions::LiteralExpression>(PropertyValue(1))}});
		rejects({}, {{"value", std::shared_ptr<expressions::Expression>{}}});
		rejects({{"value", PropertyValue(1)}},
				{{"value", std::make_shared<expressions::LiteralExpression>(PropertyValue(2))}});
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateSetMapMergePreservesEqualValuesAndAppliesEachRealChange) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		auto create = std::make_unique<logical::LogicalCreateNode>(
				"created", std::vector<std::string>{"MergedCreate"},
				PropertyMap{{"same", PropertyValue(1)}, {"changed", PropertyValue(2)}, {"removed", PropertyValue(3)}},
				std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
				std::make_unique<logical::LogicalSingleRow>());
		logical::LogicalSet merge(
				{{logical::SetActionType::LSET_MAP_MERGE, "created", "",
				  std::make_shared<expressions::LiteralExpression>(PropertyValue(PropertyValue::MapType{
						  {"absent", PropertyValue{}},
						  {"same", PropertyValue(1)},
						  {"changed", PropertyValue(4)},
							  {"added", PropertyValue(5)},
							  {"removed", PropertyValue{}}}))}},
				std::move(create));

		static_cast<void>(runtime.execute(merge, nativeMutationBounds(), QueryContext{}));
		ASSERT_EQ(backend->countNodesWithLabel("MergedCreate"), 1U);
		const TestNode &node = backend->graph().nodes.begin()->second;
		EXPECT_EQ(node.properties.at("same"), PropertyValue(1));
		EXPECT_EQ(node.properties.at("changed"), PropertyValue(4));
		EXPECT_EQ(node.properties.at("added"), PropertyValue(5));
		EXPECT_FALSE(node.properties.contains("absent"));
		EXPECT_FALSE(node.properties.contains("removed"));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateSetMapMergeRejectsInvalidMapsBeforePublication) {
		const auto rejects = [](PropertyValue value) {
			auto backend = std::make_shared<TestLogicalGraph>();
			execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
			auto create = std::make_unique<logical::LogicalCreateNode>(
					"created", std::vector<std::string>{"RejectedMapMerge"}, PropertyMap{},
					std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
					std::make_unique<logical::LogicalSingleRow>());
			logical::LogicalSet merge(
					{{logical::SetActionType::LSET_MAP_MERGE, "created", "",
					  std::make_shared<expressions::LiteralExpression>(std::move(value))}},
					std::move(create));

			EXPECT_THROW(static_cast<void>(runtime.execute(merge, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);
			EXPECT_EQ(backend->countNodesWithLabel("RejectedMapMerge"), 0U);
			EXPECT_EQ(backend->graph().mutation_batches, 0U);
		};

		// These direct logical-plan inputs bypass the parser. The CREATE portion is
		// staged first, so each malformed map must still fail before publication.
		rejects(PropertyValue(1));
		rejects(PropertyValue(PropertyValue::MapType{{"", PropertyValue(1)}}));
	}

	TEST(LogicalPlanMutationRuntimeTest, ReadSetMapMergePreservesEqualValuesAndAppliesEachRealChange) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId = backend->addNode(
				{"Seed"}, {{"same", PropertyValue(1)}, {"changed", PropertyValue(2)}, {"removed", PropertyValue(3)}});
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		logical::LogicalSet merge(
				{{logical::SetActionType::LSET_MAP_MERGE, "node", "",
				  std::make_shared<expressions::LiteralExpression>(PropertyValue(PropertyValue::MapType{
						  {"absent", PropertyValue{}},
						  {"same", PropertyValue(1)},
						  {"changed", PropertyValue(4)},
							  {"added", PropertyValue(5)},
							  {"removed", PropertyValue{}}}))}},
				std::make_unique<logical::LogicalNodeScan>("node", std::vector<std::string>{"Seed"}));

		static_cast<void>(runtime.execute(merge, nativeMutationBounds(), QueryContext{}));
		const TestNode &node = backend->graph().nodes.at(nodeId);
		EXPECT_EQ(node.properties.at("same"), PropertyValue(1));
		EXPECT_EQ(node.properties.at("changed"), PropertyValue(4));
		EXPECT_EQ(node.properties.at("added"), PropertyValue(5));
		EXPECT_FALSE(node.properties.contains("absent"));
		EXPECT_FALSE(node.properties.contains("removed"));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergePropertyActionsAvoidNoOpPublicationAndApplyNullRemoval) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const auto sameValue = std::make_shared<expressions::LiteralExpression>(PropertyValue(1));
		logical::LogicalMergeNode unchanged(
				"node", {"Seed"}, {{"value", PropertyValue(1)}}, {}, {{"node", "value", sameValue}},
				std::make_unique<logical::LogicalSingleRow>());

		static_cast<void>(runtime.execute(unchanged, nativeMutationBounds(), QueryContext{}));
		EXPECT_EQ(backend->graph().nodes.at(nodeId).properties.at("value"), PropertyValue(1));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);

		const auto nullValue = std::make_shared<expressions::LiteralExpression>(PropertyValue{});
		logical::LogicalMergeNode removesValue(
				"node", {"Seed"}, {{"value", PropertyValue(1)}}, {}, {{"node", "value", nullValue}},
				std::make_unique<logical::LogicalSingleRow>());
		static_cast<void>(runtime.execute(removesValue, nativeMutationBounds(), QueryContext{}));

		EXPECT_FALSE(backend->graph().nodes.at(nodeId).properties.contains("value"));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergePostMutationChainUpdatesCreatedPropertiesAndLabelsAtomically) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const auto replacementMap = std::make_shared<expressions::LiteralExpression>(PropertyValue(
				PropertyValue::MapType{{"gone", PropertyValue{}}, {"replacement", PropertyValue(4)}}));
		auto merge = std::make_unique<logical::LogicalMergeNode>(
				"node", std::vector<std::string>{"Created"},
				PropertyMap{{"retained", PropertyValue(1)}, {"gone", PropertyValue(2)}},
				std::vector<logical::MergeSetAction>{}, std::vector<logical::MergeSetAction>{},
				std::make_unique<logical::LogicalSingleRow>());
		auto set = std::make_unique<logical::LogicalSet>(
				std::vector<logical::LogicalSetItem>{{logical::SetActionType::LSET_MAP_MERGE, "node", "",
																replacementMap},
															 {logical::SetActionType::LSET_LABEL, "node", "Added", nullptr}},
				std::move(merge));
		logical::LogicalRemove remove(
				{{logical::LogicalRemoveActionType::LREM_PROPERTY, "node", "retained"},
				 {logical::LogicalRemoveActionType::LREM_LABEL, "node", "Created"}},
				std::move(set));

		static_cast<void>(runtime.execute(remove, nativeMutationBounds(), QueryContext{}));
		ASSERT_EQ(backend->graph().nodes.size(), 1U);
		const TestNode &node = backend->graph().nodes.begin()->second;
		EXPECT_EQ(node.labels, (std::vector<std::string>{"Added"}));
		EXPECT_FALSE(node.properties.contains("retained"));
		EXPECT_FALSE(node.properties.contains("gone"));
		EXPECT_EQ(node.properties.at("replacement"), PropertyValue(4));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergePostMutationChainRejectsInvalidAssignmentsWithoutPublication) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const auto makeMerge = [] {
			return std::make_unique<logical::LogicalMergeNode>(
					"node", std::vector<std::string>{"Candidate"}, PropertyMap{},
					std::vector<logical::MergeSetAction>{}, std::vector<logical::MergeSetAction>{},
					std::make_unique<logical::LogicalSingleRow>());
		};
		const auto rejectsSet = [&](logical::LogicalSetItem item) {
			logical::LogicalSet set({std::move(item)}, makeMerge());
			EXPECT_THROW(static_cast<void>(runtime.execute(set, nativeMutationBounds(), QueryContext{})),
						 std::logic_error);
		};

		// Post-MERGE SET is also public logical-plan input. Verify malformed map
		// and entity targets cannot commit the already staged MERGE candidate.
		rejectsSet({logical::SetActionType::LSET_PROPERTY, "missing", "value",
					std::make_shared<expressions::LiteralExpression>(PropertyValue(1))});
		rejectsSet({logical::SetActionType::LSET_MAP_MERGE, "node", "",
					std::make_shared<expressions::LiteralExpression>(PropertyValue(1))});
		rejectsSet({logical::SetActionType::LSET_LABEL, "node", "", nullptr});
		logical::LogicalRemove remove(
				{{logical::LogicalRemoveActionType::LREM_PROPERTY, "missing", "value"}}, makeMerge());
		EXPECT_THROW(static_cast<void>(runtime.execute(remove, nativeMutationBounds(), QueryContext{})),
					 std::logic_error);

		EXPECT_EQ(backend->countNodesWithLabel("Candidate"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

} // namespace graph::query
