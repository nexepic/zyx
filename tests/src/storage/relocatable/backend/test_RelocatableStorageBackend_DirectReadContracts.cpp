/**
 * @file test_RelocatableStorageBackend_DirectReadContracts.cpp
 * @brief Direct native-backend read and schema convenience contracts.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
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
	EXPECT_THROW(static_cast<void>(backend.readState(snapshot, 1U)), FormatException);
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
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}
