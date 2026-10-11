/**
 * @file test_RelocatableStorageBackend_DirectReadContracts.cpp
 * @brief Direct native-backend read and schema convenience contracts.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

using namespace graph::storage;
using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, DirectBackendReadPagesRetainThePinnedLogicalSnapshot) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	RelocatableReadSnapshot snapshot = backend.openSnapshot();

	EXPECT_EQ(backend.readNode(snapshot, fixture.person_node_id).node_id, fixture.person_node_id);
	EXPECT_EQ(backend.readEdge(snapshot, fixture.knows_edge_id).type, "KNOWS");
	EXPECT_TRUE(backend.containsLogicalNode(snapshot, fixture.unlabeled_node_id));
	EXPECT_FALSE(backend.containsLogicalNode(snapshot, fixture.mentions_edge_id + 100U));
	EXPECT_THROW(static_cast<void>(backend.enqueueLargeValuePageCompaction(SegmentId(1U), SegmentId(2U), 1U)),
			 std::logic_error);

	const LogicalGraphNodePage first = backend.scanNodesAfter(snapshot, 0U, 1U, 8U);
	ASSERT_EQ(first.nodes.size(), 1U);
	EXPECT_EQ(first.nodes.front().node_id, fixture.person_node_id);
	ASSERT_TRUE(first.may_have_more);
	const LogicalGraphNodePage second = backend.scanNodesAfter(snapshot, fixture.person_node_id, 1U, 8U);
	ASSERT_EQ(second.nodes.size(), 1U);
	EXPECT_EQ(second.nodes.front().node_id, fixture.unlabeled_node_id);
	EXPECT_THROW(static_cast<void>(backend.scanNodesAfter(snapshot, 0U, 0U, 8U)), FormatException);

	const LogicalGraphAdjacencyPage firstAdjacency = backend.scanAdjacentEdges(
			snapshot, fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
	ASSERT_EQ(firstAdjacency.edges.size(), 1U);
	ASSERT_TRUE(firstAdjacency.continuation.has_value());
	const LogicalGraphAdjacencyPage secondAdjacency = backend.scanAdjacentEdges(
			snapshot, fixture.person_node_id, LogicalGraphReadDirection::LGRD_OUTGOING, firstAdjacency.continuation, 1U);
	ASSERT_EQ(secondAdjacency.edges.size(), 1U);
	EXPECT_NE(firstAdjacency.edges.front().edge_id, secondAdjacency.edges.front().edge_id);

	std::vector<uint64_t> visited;
	EXPECT_FALSE(backend.visitNodesUntil(snapshot, 8U, [&visited](const LogicalGraphNode &node) {
		visited.push_back(node.node_id);
		return false;
	}));
	EXPECT_EQ(visited, std::vector<uint64_t>{fixture.person_node_id});
	snapshot.release();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, CloseRejectsPinnedSnapshotUntilItIsReleased) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend backend(backendOptions());
	backend.open();
	RelocatableReadSnapshot snapshot = backend.openSnapshot();

	// Closing while a snapshot remains pinned would invalidate its immutable page
	// routes and permit premature reclamation. The rejected close must leave the
	// backend and snapshot usable, rather than partially releasing either one.
	EXPECT_THROW(backend.close(), std::logic_error);
	EXPECT_TRUE(backend.isOpen());
	EXPECT_EQ(backend.readNode(snapshot, fixture.person_node_id).node_id, fixture.person_node_id);

	snapshot.release();
	EXPECT_NO_THROW(backend.close());

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	RelocatableReadSnapshot reopenedSnapshot = reopened.openSnapshot();
	EXPECT_EQ(reopened.readNode(reopenedSnapshot, fixture.person_node_id).node_id, fixture.person_node_id);
	reopenedSnapshot.release();
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, DirectReadRejectsSnapshotOwnedByAnotherDatabase) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackend first(backendOptions());
	first.open();
	RelocatableReadSnapshot foreignSnapshot = first.openSnapshot();

	const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path secondPath =
			std::filesystem::temp_directory_path() / ("zyx_foreign_snapshot_" + std::to_string(tick) + ".zyx");
	std::error_code removeError;
	std::filesystem::remove(secondPath, removeError);
	RelocatableStorageBackendOptions secondOptions;
	secondOptions.data_path = secondPath;
	secondOptions.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
	auto initialized = RelocatableStorageBackend::initializeNew(std::move(secondOptions));
	ASSERT_NE(initialized, nullptr);
	initialized->open();
	initialized->close();
	secondOptions = backendOptions();
	secondOptions.data_path = secondPath;
	secondOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend second(std::move(secondOptions));
	second.open();

	// A snapshot is a capability bound to one backend instance. Accepting a
	// foreign snapshot could read a page through unrelated catalog roots.
	EXPECT_THROW(static_cast<void>(second.readNode(foreignSnapshot, fixture.person_node_id)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(second.containsLogicalNode(foreignSnapshot, fixture.person_node_id)),
			 std::invalid_argument);

	foreignSnapshot.release();
	first.close();
	second.close();
	std::filesystem::remove(secondPath, removeError);
}

TEST_F(RelocatableStorageBackendTest, DirectExactLookupAndSchemaDropUseTheCanonicalNativeCatalog) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	static_cast<void>(backend.createLogicalConstraint(
			{"user_email_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE,
			 LogicalGraphConstraintKind::LGCK_UNIQUE, "User", {"email"},
			 LogicalGraphConstraintPropertyType::LGCPT_NONE}));
	const std::vector<uint64_t> users = backend.createLogicalNodes(
			{{{"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))},
						 {"display_name", graph::PropertyValue(std::string("Ada"))}}}});
	ASSERT_EQ(users.size(), 1U);

	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	EXPECT_EQ(backend.findNodesByExactProperties(snapshot, {}, {}, 1U, 1U), std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(
			  snapshot, {"User", "Other"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 1U, 2U),
			  std::nullopt);
	EXPECT_EQ(backend.findNodesByExactProperties(
			  snapshot, {"User"},
			  {{"email", graph::PropertyValue(std::string("ada@example.test"))},
			   {"email", graph::PropertyValue(std::string("ada@example.test"))}},
			  1U, 2U),
			  std::nullopt);
	const auto unknownLabel = backend.findNodesByExactProperties(
			snapshot, {"Unknown"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 1U, 2U);
	ASSERT_TRUE(unknownLabel.has_value());
	EXPECT_TRUE(unknownLabel->empty());
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 0U, 2U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 1U, 0U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(backend.findNodesByExactProperties(
				snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 1U, 1U)),
			 FormatException);
	const auto matched = backend.findNodesByExactProperties(
			snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}, 1U, 2U);
	ASSERT_TRUE(matched.has_value());
	ASSERT_EQ(matched->size(), 1U);
	EXPECT_EQ(matched->front().node_id, users.front());
	const auto missing = backend.findNodesByExactProperties(
			snapshot, {"User"}, {{"email", graph::PropertyValue(std::string("missing@example.test"))}}, 1U, 2U);
	ASSERT_TRUE(missing.has_value());
	EXPECT_TRUE(missing->empty());
	snapshot.release();

	static_cast<void>(backend.createLogicalNodeIndex({"user_email_index", "User", {"email"}}));
	EXPECT_TRUE(backend.dropLogicalNodeIndexBySchema("User", "email"));
	EXPECT_FALSE(backend.dropLogicalNodeIndexBySchema("User", "email"));
	EXPECT_THROW(static_cast<void>(backend.dropLogicalNodeIndexBySchema("", "email")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(backend.dropLogicalNodeIndexBySchema("User", "")), std::invalid_argument);
	EXPECT_TRUE(backend.listLogicalIndexes(8U).empty());
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, DirectRangeLookupsUseTheCanonicalNativeIndexes) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", graph::PropertyValue(int64_t{7})},
							 {"city", graph::PropertyValue("Shanghai")},
							 {"age", graph::PropertyValue(int64_t{30})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{42})},
							 {"city", graph::PropertyValue("Beijing")},
							 {"age", graph::PropertyValue(int64_t{31})}}},
			 {{"Person"}, {{"score", graph::PropertyValue(int64_t{42})},
							 {"city", graph::PropertyValue("Shanghai")},
							 {"age", graph::PropertyValue(int64_t{29})}}}});
	ASSERT_EQ(nodes.size(), 3U);
	static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
	static_cast<void>(backend.createLogicalNodeIndex({"person_city_age", "Person", {"city", "age"}}));

	RelocatableReadSnapshot snapshot = backend.openSnapshot();
	const auto scalarRange = backend.findNodesByPropertyRange(
			snapshot, {"Person"}, {"score", graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{42}), true, true},
			3U, 3U);
	ASSERT_TRUE(scalarRange.has_value());
	ASSERT_EQ(scalarRange->size(), 3U);
	EXPECT_EQ(scalarRange->at(0).node_id, nodes.at(0));
	EXPECT_EQ(scalarRange->at(1).node_id, nodes.at(1));
	EXPECT_EQ(scalarRange->at(2).node_id, nodes.at(2));

	const LogicalGraphCompositePropertyRange compositeRange{
			{{"city", graph::PropertyValue("Shanghai")}},
			{"age", graph::PropertyValue(int64_t{29}), graph::PropertyValue(int64_t{31}), true, false}};
	const auto compositeMatches =
			backend.findNodesByCompositePropertyRange(snapshot, {"Person"}, compositeRange, 3U, 3U);
	ASSERT_TRUE(compositeMatches.has_value());
	ASSERT_EQ(compositeMatches->size(), 2U);
	EXPECT_EQ(compositeMatches->at(0).node_id, nodes.at(0));
	EXPECT_EQ(compositeMatches->at(1).node_id, nodes.at(2));
	snapshot.release();
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionRejectsForgedEntityReferenceKindsBeforePublication) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Source"}, {}}, {{"Target"}, {}}};
	batch.edge_creates = {{
			{LogicalGraphMutationEntityKind::LGMK_NODE,
			 static_cast<LogicalGraphMutationReferenceKind>(99U),
			 0U},
			{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			"REJECTED",
			{},
	}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(batch))), std::invalid_argument);
	transaction->rollback();
	transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch forgedEntity;
	forgedEntity.node_creates = {{{"Target"}, {}}};
	forgedEntity.edge_creates = {{
			{static_cast<LogicalGraphMutationEntityKind>(99U),
			 LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE,
			 0U},
			{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			"REJECTED",
			{},
	}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(forgedEntity))), std::invalid_argument);
	transaction->rollback();

	// Final-state folding is part of staging, not a deferred commit-only check.
	// Otherwise an invalid enum could remain in the transaction after the caller
	// believes the rejected batch had no effect.
	transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch forgedProperty;
	forgedProperty.node_creates = {{{"Target"}, {}}};
	forgedProperty.property_replacements = {{
			{LogicalGraphMutationEntityKind::LGMK_NODE,
			 static_cast<LogicalGraphMutationReferenceKind>(99U),
			 0U},
			{{"rejected", graph::PropertyValue(int64_t{1})}},
	}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(forgedProperty))), std::invalid_argument);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
	transaction->rollback();

	transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch forgedLabel;
	forgedLabel.node_creates = {{{"Target"}, {}}};
	forgedLabel.node_label_replacements = {{
			{LogicalGraphMutationEntityKind::LGMK_NODE,
			 static_cast<LogicalGraphMutationReferenceKind>(99U),
			 0U},
			{"Rejected"},
	}};
	EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(forgedLabel))), std::invalid_argument);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
	transaction->rollback();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, LogicalTransactionRejectsInvalidCreatedEntityFinalStateBeforePublication) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();

	const LogicalGraphMutationEntityReference createdNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
	{
		std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphMutationBatch batch;
		batch.node_creates = {{{"DuplicateProperties"}, {}}};
		batch.property_replacements = {{createdNode, {{"first", graph::PropertyValue(int64_t{1})}}},
								   {createdNode, {{"second", graph::PropertyValue(int64_t{2})}}}};
		EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(batch))), std::invalid_argument);
		EXPECT_NO_THROW(transaction->rollback());
	}

	{
		std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
		LogicalGraphMutationBatch batch;
		batch.node_creates = {{{"InvalidLabelTarget"}, {}}};
		batch.node_label_replacements = {{
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
				{"InvalidLabelTarget"},
		}};
		EXPECT_THROW(static_cast<void>(transaction->stageMutationBatch(std::move(batch))), std::invalid_argument);
		EXPECT_NO_THROW(transaction->rollback());
	}

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"DeleteSource"}, {}}, {{"DeleteTarget"}, {}}};
	create.edge_creates = {{
			createdNode,
			{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			"LIVE_EDGE",
			{},
	}};
	const LogicalGraphMutationBatchResult staged = transaction->stageMutationBatch(std::move(create));
	ASSERT_EQ(staged.created_node_ids.size(), 2U);
	ASSERT_EQ(staged.created_edge_ids.size(), 1U);
	LogicalGraphMutationBatch erase;
	erase.node_deletes = {{staged.created_node_ids.front(), false, 0U}};
	EXPECT_TRUE(transaction->stageMutationBatch(std::move(erase)).created_node_ids.empty());
	EXPECT_THROW(static_cast<void>(transaction->commit()), std::invalid_argument);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(staged.created_node_ids.front())), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readNode(staged.created_node_ids.back())), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(staged.created_edge_ids.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}
