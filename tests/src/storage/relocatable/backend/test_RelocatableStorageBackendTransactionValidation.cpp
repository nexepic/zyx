/**
 * @file test_RelocatableStorageBackendTransactionValidation.cpp
 * @brief Fail-closed logical transaction validation contracts.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include <limits>

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest,
	   LogicalTransactionRejectsMalformedFinalStateReferencesWithoutPublishingEntities) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const auto persistedNode = [&fixture] {
		return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
																LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
																fixture.person_node_id};
	};
	const auto reject = [&backend](LogicalGraphMutationBatch batch) {
		std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
		EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(batch))), std::invalid_argument);
		EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
		transaction->rollback();
	};

	LogicalGraphMutationBatch unknownEntityKind;
	unknownEntityKind.property_replacements = {{
			{static_cast<LogicalGraphMutationEntityKind>(0xFFU),
			 LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
			 fixture.person_node_id},
			{{"ignored", PropertyValue(int64_t{1})}},
	}};
	reject(std::move(unknownEntityKind));

	LogicalGraphMutationBatch unknownReferenceKind;
	unknownReferenceKind.property_replacements = {{
			{LogicalGraphMutationEntityKind::LGMK_NODE,
			 static_cast<LogicalGraphMutationReferenceKind>(0xFFU),
			 fixture.person_node_id},
			{{"ignored", PropertyValue(int64_t{1})}},
	}};
	reject(std::move(unknownReferenceKind));

	LogicalGraphMutationBatch unknownLabelReferenceKind;
	unknownLabelReferenceKind.node_label_replacements = {{
			{LogicalGraphMutationEntityKind::LGMK_NODE,
			 static_cast<LogicalGraphMutationReferenceKind>(0xFFU),
			 fixture.person_node_id},
			{"Ignored"},
	}};
	reject(std::move(unknownLabelReferenceKind));

	LogicalGraphMutationBatch invalidCreatedNodeProperty;
	invalidCreatedNodeProperty.node_creates = {{{"MalformedPropertyTarget"}, {}}};
	invalidCreatedNodeProperty.property_replacements = {{
			{LogicalGraphMutationEntityKind::LGMK_EDGE,
			 LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE,
			 0U},
			{{"ignored", PropertyValue(int64_t{2})}},
	}};
	reject(std::move(invalidCreatedNodeProperty));

	LogicalGraphMutationBatch duplicatePersistedProperties;
	duplicatePersistedProperties.property_replacements = {{persistedNode(), {{"first", PropertyValue(int64_t{1})}}},
															{persistedNode(), {{"second", PropertyValue(int64_t{2})}}}};
	reject(std::move(duplicatePersistedProperties));

	LogicalGraphMutationBatch duplicatePersistedLabels;
	duplicatePersistedLabels.node_label_replacements = {{persistedNode(), {"First"}},
															 {persistedNode(), {"Second"}}};
	reject(std::move(duplicatePersistedLabels));

	LogicalGraphMutationBatch invalidNodeLabelTarget;
	invalidNodeLabelTarget.node_label_replacements = {{
			{LogicalGraphMutationEntityKind::LGMK_EDGE,
			 LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE,
			 0U},
			{"Ignored"},
	}};
	reject(std::move(invalidNodeLabelTarget));

	// A rejected statement must not poison the writer: a subsequent public
	// transaction still creates and publishes a durable Node.
	const std::vector<uint64_t> committed = backend.createLogicalNodes({{{"AfterRejectedFinalState"}, {}}});
	ASSERT_EQ(committed.size(), 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(committed.front()).labels, (std::vector<std::string>{"AfterRejectedFinalState"}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest,
	   LogicalTransactionRejectsDuplicateCreatedEntityFinalStateMutationsWithoutPublishing) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const auto createdNode = [](uint64_t ordinal) {
		return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
																LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, ordinal};
	};
	const auto createdEdge = [](uint64_t ordinal) {
		return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_EDGE,
																LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, ordinal};
	};
	const auto reject = [&backend](LogicalGraphMutationBatch batch) {
		std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
		EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(batch))), std::invalid_argument);
		transaction->rollback();
	};

	LogicalGraphMutationBatch duplicateNodeProperties;
	duplicateNodeProperties.node_creates = {{{"DuplicateProperties"}, {}}};
	duplicateNodeProperties.property_replacements = {{createdNode(0U), {{"first", PropertyValue(int64_t{1})}}},
															 {createdNode(0U), {{"second", PropertyValue(int64_t{2})}}}};
	reject(std::move(duplicateNodeProperties));

	LogicalGraphMutationBatch duplicateEdgeProperties;
	duplicateEdgeProperties.node_creates = {{{"Source"}, {}}, {{"Target"}, {}}};
	duplicateEdgeProperties.edge_creates = {{createdNode(0U), createdNode(1U), "DUPLICATE_PROPERTIES", {}}};
	duplicateEdgeProperties.property_replacements = {{createdEdge(0U), {{"first", PropertyValue(int64_t{1})}}},
															 {createdEdge(0U), {{"second", PropertyValue(int64_t{2})}}}};
	reject(std::move(duplicateEdgeProperties));

	LogicalGraphMutationBatch duplicateNodeLabels;
	duplicateNodeLabels.node_creates = {{{"DuplicateLabels"}, {}}};
	duplicateNodeLabels.node_label_replacements = {{createdNode(0U), {"First"}}, {createdNode(0U), {"Second"}}};
	reject(std::move(duplicateNodeLabels));

	const std::vector<uint64_t> committed = backend.createLogicalNodes({{{"AfterDuplicateValidation"}, {}}});
	ASSERT_EQ(committed.size(), 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(committed.front()).labels, (std::vector<std::string>{"AfterDuplicateValidation"}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest,
	   LogicalTransactionRejectsInvalidDeleteFinalStatesBeforePublishingTheGraphGeneration) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const auto createdNode = [](uint64_t ordinal) {
		return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
																LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, ordinal};
	};
	const auto persistedNode = [](uint64_t nodeId) {
		return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
																LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId};
	};
	const auto rejectOnCommit = [&backend](LogicalGraphMutationBatch batch) {
		std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
		ASSERT_NO_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(batch))));
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::invalid_argument);
		EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	};
	const auto rejectCreatedDeleteOnCommit = [&backend](LogicalGraphMutationBatch batch, size_t ordinal, bool detach,
																							 size_t maximumDetachEdges) {
		std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
		const LogicalGraphMutationBatchResult preview = transaction->previewMutationBatch(batch);
		ASSERT_LT(ordinal, preview.created_node_ids.size());
		batch.node_deletes = {{preview.created_node_ids[ordinal], detach, maximumDetachEdges}};
		ASSERT_NO_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(batch))));
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::invalid_argument);
		EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	};

	LogicalGraphMutationBatch zeroNodeDelete;
	zeroNodeDelete.node_deletes = {{0U, false, 0U}};
	rejectOnCommit(std::move(zeroNodeDelete));

	LogicalGraphMutationBatch duplicateEdgeDelete;
	duplicateEdgeDelete.edge_deletes = {{fixture.knows_edge_id}, {fixture.knows_edge_id}};
	rejectOnCommit(std::move(duplicateEdgeDelete));

	LogicalGraphMutationBatch createdNodeWithLiveEdge;
	createdNodeWithLiveEdge.node_creates = {{{"DeletedSource"}, {}}, {{"LiveTarget"}, {}}};
	createdNodeWithLiveEdge.edge_creates = {{createdNode(0U), createdNode(1U), "LIVE", {}}};
	// A create/delete pair is valid only when every surviving incident Edge is
	// explicitly covered by a bounded detach request.
	rejectCreatedDeleteOnCommit(std::move(createdNodeWithLiveEdge), 0U, false, 0U);

	LogicalGraphMutationBatch detachBoundTooSmall;
	detachBoundTooSmall.node_creates = {{{"BoundedSource"}, {}}, {{"FirstTarget"}, {}}, {{"SecondTarget"}, {}}};
	detachBoundTooSmall.edge_creates = {{createdNode(0U), createdNode(1U), "FIRST", {}},
										  {createdNode(0U), createdNode(2U), "SECOND", {}}};
	rejectCreatedDeleteOnCommit(std::move(detachBoundTooSmall), 0U, true, 1U);

	// A detach delete must retain a finite scan budget. Accepting the sentinel
	// maximum would permit an unbounded adjacency read during publication.
	LogicalGraphMutationBatch unboundedDetach;
	unboundedDetach.node_deletes = {{fixture.person_node_id, true, std::numeric_limits<size_t>::max()}};
	rejectOnCommit(std::move(unboundedDetach));

	LogicalGraphMutationBatch persistedDeleteWithLiveCreatedEdge;
	persistedDeleteWithLiveCreatedEdge.node_creates = {{{"NewNeighbor"}, {}}};
	persistedDeleteWithLiveCreatedEdge.edge_creates = {{persistedNode(fixture.unlabeled_node_id), createdNode(0U),
																				 "PERSISTED_LIVE", {}}};
	persistedDeleteWithLiveCreatedEdge.node_deletes = {{fixture.unlabeled_node_id, false, 0U}};
	rejectOnCommit(std::move(persistedDeleteWithLiveCreatedEdge));

	const std::vector<uint64_t> committed = backend.createLogicalNodes({{{"AfterDeleteValidation"}, {}}});
	ASSERT_EQ(committed.size(), 1U);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(committed.front()).labels, (std::vector<std::string>{"AfterDeleteValidation"}));
	EXPECT_NO_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)));
	EXPECT_NO_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest,
	   LogicalTransactionRejectsMalformedCreatedEdgeEndpointAtFinalCanonicalization) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const auto createdNode = [](uint64_t ordinal) {
		return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
																LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, ordinal};
	};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"MalformedSource"}, {}}, {{"ValidTarget"}, {}}};
	batch.edge_creates = {{
			{LogicalGraphMutationEntityKind::LGMK_EDGE,
			 LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE,
			 0U},
			createdNode(1U),
			"MALFORMED_ENDPOINT",
			{},
	}};

	ASSERT_NO_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(batch))));
	EXPECT_THROW(static_cast<void>(transaction->commit()), std::invalid_argument);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	std::vector<uint64_t> nodeIds;
	snapshot->visitNodes(8U, [&nodeIds](const LogicalGraphNode &node) {
		nodeIds.push_back(node.node_id);
		return true;
	});
	EXPECT_TRUE(nodeIds.empty());
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest,
	   LogicalTransactionCancelsAnIsolatedCreatedNodeWithoutRejectingAnUnrelatedCreatedEdge) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const auto createdNode = [](uint64_t ordinal) {
		return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
																LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, ordinal};
	};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"EdgeSource"}, {}}, {{"EdgeTarget"}, {}}, {{"Isolated"}, {}}};
	create.edge_creates = {{createdNode(0U), createdNode(1U), "UNRELATED", {}}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(staged.created_node_ids.size(), 3U);
	ASSERT_EQ(staged.created_edge_ids.size(), 1U);

	LogicalGraphMutationBatch erase;
	erase.node_deletes = {{staged.created_node_ids[2U], false, 0U}};
	ASSERT_TRUE(transaction->stageMutationBatch(std::move(erase)).created_node_ids.empty());
	const LogicalGraphMutationBatchResult committed = transaction->commit();
	ASSERT_EQ(committed.created_node_ids,
			  (std::vector<uint64_t>{staged.created_node_ids[0U], staged.created_node_ids[1U]}));
	ASSERT_EQ(committed.created_edge_ids, staged.created_edge_ids);

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_NO_THROW(static_cast<void>(snapshot->readNode(staged.created_node_ids[0U])));
	EXPECT_NO_THROW(static_cast<void>(snapshot->readNode(staged.created_node_ids[1U])));
	EXPECT_NO_THROW(static_cast<void>(snapshot->readEdge(staged.created_edge_ids[0U])));
	EXPECT_THROW(static_cast<void>(snapshot->readNode(staged.created_node_ids[2U])), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest,
	   LogicalTransactionFoldsExplicitPersistedEdgeDeletesIntoAnIsolatedNodeDelete) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch finalState;
	// The fixture gives this Node two self-loop Edges. Explicitly removing both
	// lets the bounded detach scan account for the final-state removals before
	// the one atomic publication deletes the now-isolated Node.
	finalState.edge_deletes = {{fixture.knows_edge_id}, {fixture.mentions_edge_id}};
	finalState.node_deletes = {{fixture.person_node_id, true, 2U}};
	ASSERT_NO_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(finalState))));
	EXPECT_TRUE(transaction->commit().created_node_ids.empty());
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_NO_THROW(static_cast<void>(snapshot->readNode(fixture.unlabeled_node_id)));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest,
	   LogicalTransactionDropsPersistedFinalStateReplacementsForAnEntityDeletedInTheSameCommit) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const LogicalGraphMutationEntityReference persistedNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
			fixture.person_node_id};
	const LogicalGraphMutationEntityReference persistedEdge{
			LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
			fixture.knows_edge_id};
	LogicalGraphMutationBatch finalState;
	finalState.property_replacements = {{persistedNode, {{"must_not_publish", PropertyValue(int64_t{1})}}}};
	finalState.property_replacements.push_back(
			{persistedEdge, {{"must_not_publish", PropertyValue(int64_t{2})}}});
	finalState.node_label_replacements = {{persistedNode, {"MustNotPublish"}}};
	finalState.edge_deletes = {{fixture.knows_edge_id}, {fixture.mentions_edge_id}};
	finalState.node_deletes = {{fixture.person_node_id, false, 0U}};

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	ASSERT_NO_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(finalState))));
	EXPECT_TRUE(transaction->commit().created_node_ids.empty());

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	EXPECT_NO_THROW(static_cast<void>(snapshot->readNode(fixture.unlabeled_node_id)));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionTerminalStateRejectsEveryFurtherPublicOperation) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	std::unique_ptr<LogicalGraphTransaction> committed = backend.beginLogicalGraphTransaction(true);
	EXPECT_TRUE(committed->isReadOnly());
	EXPECT_EQ(committed->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
	EXPECT_TRUE(committed->commit().created_node_ids.empty());
	EXPECT_EQ(committed->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	EXPECT_THROW(static_cast<void>(committed->readSnapshot()), std::logic_error);
	EXPECT_THROW(static_cast<void>(committed->previewMutationBatch({})), std::logic_error);
	EXPECT_THROW(static_cast<void>(committed->stageMutationBatch({})), std::logic_error);
	EXPECT_THROW(static_cast<void>(committed->commit()), std::logic_error);
	EXPECT_THROW(committed->rollback(), std::logic_error);

	std::unique_ptr<LogicalGraphTransaction> rolledBack = backend.beginLogicalGraphTransaction(false);
	rolledBack->rollback();
	EXPECT_EQ(rolledBack->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	EXPECT_THROW(static_cast<void>(rolledBack->readSnapshot()), std::logic_error);
	EXPECT_THROW(static_cast<void>(rolledBack->stageMutationBatch({})), std::logic_error);
	EXPECT_THROW(rolledBack->rollback(), std::logic_error);

	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest,
	   LogicalTransactionCancelsCreatedEdgesBeforeDeletingTheirCreatedEndpoints) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const auto createdNode = [](uint64_t ordinal) {
		return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
																	LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, ordinal};
	};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"TransientSource"}, {}}, {{"TransientTarget"}, {}}};
	create.edge_creates = {{createdNode(0U), createdNode(1U), "TRANSIENT", {}}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(staged.created_node_ids.size(), 2U);
	ASSERT_EQ(staged.created_edge_ids.size(), 1U);

	// Removing the Edge first leaves both created Nodes isolated in the final
	// state, so they can be cancelled without detach semantics.
	LogicalGraphMutationBatch finalState;
	finalState.edge_deletes = {{staged.created_edge_ids.front()}};
	finalState.node_deletes = {{staged.created_node_ids[0], false, 0U}, {staged.created_node_ids[1], false, 0U}};
	ASSERT_TRUE(transaction->stageMutationBatch(std::move(finalState)).created_node_ids.empty());
	const LogicalGraphMutationBatchResult committed = transaction->commit();
	EXPECT_TRUE(committed.created_node_ids.empty());
	EXPECT_TRUE(committed.created_edge_ids.empty());
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	for (const uint64_t nodeId: staged.created_node_ids) {
		EXPECT_THROW(static_cast<void>(snapshot->readNode(nodeId)), FormatException);
	}
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(staged.created_edge_ids.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest,
	   LogicalTransactionDiscardsFinalStateMutationsForEntitiesCancelledBeforePublication) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const auto persisted = [](LogicalGraphMutationEntityKind kind, uint64_t id) {
		return LogicalGraphMutationEntityReference{kind, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id};
	};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"DiscardedSource"}, {}}, {{"DiscardedTarget"}, {}}};
	create.edge_creates = {{
			{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			"DISCARDED",
			{},
	}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(staged.created_node_ids.size(), 2U);
	ASSERT_EQ(staged.created_edge_ids.size(), 1U);

	// Callers receive durable identities during staging. A later statement may
	// still target them, but deleting the full created subgraph must suppress
	// every folded Property and label update with the cancelled entities.
	LogicalGraphMutationBatch finalState;
	finalState.property_replacements = {
			{persisted(LogicalGraphMutationEntityKind::LGMK_NODE, staged.created_node_ids[0U]),
			 {{"must_not_publish", PropertyValue(int64_t{1})}}},
			{persisted(LogicalGraphMutationEntityKind::LGMK_EDGE, staged.created_edge_ids[0U]),
			 {{"must_not_publish", PropertyValue(int64_t{2})}}},
	};
	finalState.node_label_replacements = {{
			persisted(LogicalGraphMutationEntityKind::LGMK_NODE, staged.created_node_ids[1U]), {"DiscardedTarget", "Ignored"},
	}};
	finalState.edge_deletes = {{staged.created_edge_ids[0U]}};
	finalState.node_deletes = {{staged.created_node_ids[0U], false, 0U}, {staged.created_node_ids[1U], false, 0U}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(finalState)).created_node_ids.empty());
	const LogicalGraphMutationBatchResult committed = transaction->commit();
	EXPECT_TRUE(committed.created_node_ids.empty());
	EXPECT_TRUE(committed.created_edge_ids.empty());

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	for (const uint64_t nodeId: staged.created_node_ids) {
		EXPECT_THROW(static_cast<void>(snapshot->readNode(nodeId)), FormatException);
	}
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(staged.created_edge_ids.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest,
	   LogicalTransactionPublishesFinalStateReplacementsForCreatedNodesAndEdges) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const auto createdNode = [](uint64_t ordinal) {
		return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
																		LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, ordinal};
	};
	const auto createdEdge = [](uint64_t ordinal) {
		return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_EDGE,
																		LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, ordinal};
	};

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch finalState;
	finalState.node_creates = {{{"CreatedSource"}, {}}, {{"CreatedTarget"}, {}}};
	finalState.edge_creates = {{createdNode(0U), createdNode(1U), "CREATED", {}}};
	finalState.property_replacements = {{createdNode(0U), {{"node_value", PropertyValue(int64_t{7})}}},
												 {createdEdge(0U), {{"edge_value", PropertyValue("kept")}}}};
	finalState.node_label_replacements = {{createdNode(1U), {"CreatedTarget", "Published"}}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(finalState));
	ASSERT_EQ(staged.created_node_ids.size(), 2U);
	ASSERT_EQ(staged.created_edge_ids.size(), 1U);
	const LogicalGraphMutationBatchResult committed = transaction->commit();
	ASSERT_EQ(committed.created_node_ids.size(), 2U);
	ASSERT_EQ(committed.created_edge_ids.size(), 1U);
	EXPECT_EQ(committed.created_node_ids, staged.created_node_ids);
	EXPECT_EQ(committed.created_edge_ids, staged.created_edge_ids);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphNode source = snapshot->readNode(committed.created_node_ids[0U]);
	const LogicalGraphNode target = snapshot->readNode(committed.created_node_ids[1U]);
	const LogicalGraphEdge edge = snapshot->readEdge(committed.created_edge_ids[0U]);
	ASSERT_EQ(source.properties.size(), 1U);
	EXPECT_EQ(source.properties.front().key, "node_value");
	EXPECT_EQ(source.properties.front().value, PropertyValue(int64_t{7}));
	EXPECT_EQ(target.labels, (std::vector<std::string>{"CreatedTarget", "Published"}));
	EXPECT_EQ(edge.source_node_id, committed.created_node_ids[0U]);
	EXPECT_EQ(edge.target_node_id, committed.created_node_ids[1U]);
	EXPECT_EQ(edge.type, "CREATED");
	ASSERT_EQ(edge.properties.size(), 1U);
	EXPECT_EQ(edge.properties.front().key, "edge_value");
	EXPECT_EQ(edge.properties.front().value, PropertyValue("kept"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenedOptions = backendOptions();
	reopenedOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenedOptions));
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> recovered = reopened.openLogicalGraphReadSnapshot();
	const LogicalGraphNode recoveredSource = recovered->readNode(committed.created_node_ids[0U]);
	const LogicalGraphEdge recoveredEdge = recovered->readEdge(committed.created_edge_ids[0U]);
	ASSERT_EQ(recoveredSource.properties.size(), 1U);
	ASSERT_EQ(recoveredEdge.properties.size(), 1U);
	EXPECT_EQ(recoveredSource.properties.front().value, PropertyValue(int64_t{7}));
	EXPECT_EQ(recoveredEdge.properties.front().value, PropertyValue("kept"));
	recovered.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
	   LogicalTransactionRebasesReturnedCreatedIdsForLaterFinalStateReplacements) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const auto createdNode = [](uint64_t ordinal) {
		return LogicalGraphMutationEntityReference{LogicalGraphMutationEntityKind::LGMK_NODE,
																		LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, ordinal};
	};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"RebasedSource"}, {}}, {{"RebasedTarget"}, {}}};
	create.edge_creates = {{createdNode(0U), createdNode(1U), "REBASED", {}}};
	const LogicalGraphMutationBatchResult created = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(created.created_node_ids.size(), 2U);
	ASSERT_EQ(created.created_edge_ids.size(), 1U);

	// Public mutation callers receive durable IDs from the first statement. A
	// later statement in the same transaction uses those IDs, but the final
	// publication must still fold the changes into the not-yet-published creates.
	const LogicalGraphMutationEntityReference createdSource{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
			created.created_node_ids[0U]};
	const LogicalGraphMutationEntityReference createdTarget{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
			created.created_node_ids[1U]};
	const LogicalGraphMutationEntityReference createdRelationship{
			LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
			created.created_edge_ids[0U]};
	const LogicalGraphMutationEntityReference persistedNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
			fixture.person_node_id};
	LogicalGraphMutationBatch laterFinalState;
	laterFinalState.property_replacements = {{createdSource, {{"source_value", PropertyValue(int64_t{11})}}},
														  {createdRelationship, {{"edge_value", PropertyValue(int64_t{12})}}},
														  {persistedNode, {{"persisted_value", PropertyValue(int64_t{13})}}}};
	laterFinalState.node_label_replacements = {{createdTarget, {"RebasedTarget", "Rebased"}}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(laterFinalState)).created_node_ids.empty());

	const LogicalGraphMutationBatchResult committed = transaction->commit();
	EXPECT_EQ(committed, created);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphNode source = snapshot->readNode(created.created_node_ids[0U]);
	const LogicalGraphNode target = snapshot->readNode(created.created_node_ids[1U]);
	const LogicalGraphEdge edge = snapshot->readEdge(created.created_edge_ids[0U]);
	const LogicalGraphNode persisted = snapshot->readNode(fixture.person_node_id);
	ASSERT_EQ(source.properties.size(), 1U);
	ASSERT_EQ(edge.properties.size(), 1U);
	ASSERT_EQ(persisted.properties.size(), 1U);
	EXPECT_EQ(source.properties.front().value, PropertyValue(int64_t{11}));
	EXPECT_EQ(target.labels, (std::vector<std::string>{"Rebased", "RebasedTarget"}));
	EXPECT_EQ(edge.properties.front().value, PropertyValue(int64_t{12}));
	EXPECT_EQ(persisted.properties.front().key, "persisted_value");
	EXPECT_EQ(persisted.properties.front().value, PropertyValue(int64_t{13}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}
