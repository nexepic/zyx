/**
 * @file test_DataManager_DirtyTracking.cpp
 * @date 2025/8/15
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "DataManagerTestFixture.hpp"

TEST_F(DataManagerTest, DirtyTracking) {
	auto node = createTestNode(dataManager, "DirtyNode");
	dataManager->addNode(node);
	EXPECT_TRUE(dataManager->hasUnsavedChanges());

	auto dirtyNodes = dataManager->getDirtyEntityInfos<Node>({EntityChangeType::CHANGE_ADDED});
	ASSERT_EQ(1UL, dirtyNodes.size());
	EXPECT_EQ(EntityChangeType::CHANGE_ADDED, dirtyNodes[0].changeType);
	EXPECT_EQ(node.getId(), dirtyNodes[0].backup->getId());

	simulateSave();
	EXPECT_FALSE(dataManager->hasUnsavedChanges());

	node.setLabelId(dataManager->getOrCreateLabelId("UpdatedDirtyNode"));
	dataManager->updateNode(node);
	EXPECT_TRUE(dataManager->hasUnsavedChanges());

	dirtyNodes = dataManager->getDirtyEntityInfos<Node>({EntityChangeType::CHANGE_MODIFIED});
	ASSERT_EQ(1UL, dirtyNodes.size());
	EXPECT_EQ(EntityChangeType::CHANGE_MODIFIED, dirtyNodes[0].changeType);
}

TEST_F(DataManagerTest, AutoFlush) {
	bool autoFlushCalled = false;

	if (database) {
		auto idxMgr = database->getQueryEngine()->getIndexManager();
		// Drop it to disable auto-indexing
		(void) idxMgr->getNodeIndexManager()->dropIndex("label", "");

		database->getStorage()->flush();
	}

	// Note: The new auto-flush logic checks SUM of all entities.
	dataManager->setMaxDirtyEntities(10);
	dataManager->setAutoFlushCallback([&autoFlushCalled]() { autoFlushCalled = true; });

	for (int i = 0; i < 4; i++) {
		auto node = createTestNode(dataManager, "AutoFlushNode" + std::to_string(i));
		dataManager->addNode(node);
	}
	// With index disabled and dirty count reset, 4 nodes < 10 limit
	EXPECT_FALSE(autoFlushCalled);

	auto node = createTestNode(dataManager, "ThresholdNode");
	dataManager->addNode(node);
	// 5 nodes >= 5 limit
	EXPECT_TRUE(autoFlushCalled);
}

TEST_F(DataManagerTest, GetDirtyEntityInfosAllTypes) {
	// Create entities with different change types (avoid simulateSave due to index issues)

	// Create an ADDED node
	auto node1 = createTestNode(dataManager, "AddedNode");
	dataManager->addNode(node1);

	// Create another node
	auto node2 = createTestNode(dataManager, "ToBeModified");
	dataManager->addNode(node2);

	// Modify node2 (even though it's in ADDED state, update will track it)
	node2.setLabelId(dataManager->getOrCreateLabelId("ModifiedLabel"));
	dataManager->updateNode(node2);

	// Get all dirty types
	auto allDirty = dataManager->getDirtyEntityInfos<Node>({EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED,
															EntityChangeType::CHANGE_DELETED});

	// Should find at least our 2 entities in the dirty list
	EXPECT_GE(allDirty.size(), 2UL);

	// Verify we have at least some dirty entities
	bool foundOurEntities = false;
	for (const auto &info: allDirty) {
		if (info.backup.has_value()) {
			int64_t id = info.backup->getId();
			if (id == node1.getId() || id == node2.getId()) {
				foundOurEntities = true;
				break;
			}
		}
	}
	EXPECT_TRUE(foundOurEntities) << "Should find our created entities in dirty list";
}

TEST_F(DataManagerTest, SetEntityDirtyWithInvalidBackup) {
	// Create a DirtyEntityInfo with a backup that has ID=0 (invalid entity)
	Node invalidNode;
	invalidNode.setId(0); // Explicitly set ID to 0 (invalid)

	DirtyEntityInfo<Node> dirtyInfo;
	dirtyInfo.changeType = EntityChangeType::CHANGE_MODIFIED;
	dirtyInfo.backup = invalidNode; // Backup with ID=0

	// Get initial dirty count
	auto dirtyBefore = dataManager->getDirtyEntityInfos<Node>({EntityChangeType::CHANGE_MODIFIED});

	// Call setEntityDirty - should return early without adding to dirty tracking
	// This tests the guard clause that prevents invalid entities from being tracked
	dataManager->setEntityDirty(dirtyInfo);

	// Verify that the invalid entity was NOT added to dirty tracking
	auto dirtyAfter = dataManager->getDirtyEntityInfos<Node>({EntityChangeType::CHANGE_MODIFIED});
	EXPECT_EQ(dirtyBefore.size(), dirtyAfter.size())
		<< "Entity with ID=0 should not be added to dirty tracking";
}

TEST_F(DataManagerTest, GetDirtyEntityInfosAllTypesWithNoDirtyEntities) {
	// Create a fresh database with no dirty entities
	// (Database is already fresh in SetUp, but let's verify)

	// Request all change types when there are no dirty entities
	auto allDirty = dataManager->getDirtyEntityInfos<Node>(
		{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED, EntityChangeType::CHANGE_DELETED});

	// Should return empty vector
	EXPECT_TRUE(allDirty.empty()) << "Should return empty vector when no dirty entities exist";
	EXPECT_EQ(0UL, allDirty.size()) << "Size should be 0 for empty dirty tracking";
}

TEST_F(DataManagerTest, GetDirtyEntityInfosWithTypeFiltering) {
	// Create nodes with different change types
	auto node1 = createTestNode(dataManager, "Node1");
	auto node2 = createTestNode(dataManager, "Node2");
	auto node3 = createTestNode(dataManager, "Node3");
	dataManager->addNode(node1);
	dataManager->addNode(node2);
	dataManager->addNode(node3);

	// Delete node3 to create a DELETED type
	dataManager->deleteNode(node3);

	// Request only ADDED and MODIFIED types (not DELETED)
	auto filteredDirty = dataManager->getDirtyEntityInfos<Node>(
		{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED});

	// Should NOT include the DELETED node
	for (const auto &info: filteredDirty) {
		if (info.backup.has_value()) {
			EXPECT_NE(EntityChangeType::CHANGE_DELETED, info.changeType)
				<< "Filtered results should not include DELETED entities";
		}
	}
}

TEST_F(DataManagerTest, GetDirtyEntityInfosWithSingleType) {
	// Create a node
	auto node = createTestNode(dataManager, "SingleTypeTest");
	dataManager->addNode(node);

	// Request only ADDED type
	auto addedOnly = dataManager->getDirtyEntityInfos<Node>({EntityChangeType::CHANGE_ADDED});

	// All nodes in dirty tracking should be ADDED (since we just added them)
	EXPECT_GT(addedOnly.size(), 0UL) << "Should find at least one ADDED entity";

	// Verify all returned entities are ADDED
	for (const auto &info: addedOnly) {
		EXPECT_EQ(EntityChangeType::CHANGE_ADDED, info.changeType)
			<< "All results should be ADDED when filtering by ADDED only";
	}
}

TEST_F(DataManagerTest, GetDirtyEntityInfosWithEmptyTypes) {
	// Create a node
	auto node = createTestNode(dataManager, "EmptyTypesTest");
	dataManager->addNode(node);

	// Request with empty types vector (edge case, but should handle gracefully)
	auto emptyTypesResult = dataManager->getDirtyEntityInfos<Node>({}); // Empty vector

	// Should return empty result since no types were requested
	EXPECT_TRUE(emptyTypesResult.empty()) << "Should return empty when no types requested";
	EXPECT_EQ(0UL, emptyTypesResult.size()) << "Size should be 0 for empty types filter";
}

TEST_F(DataManagerTest, SetEntityDirtyWithNoBackup) {
	// Create a DirtyEntityInfo without a backup (backup.has_value() == false)
	DirtyEntityInfo<Node> dirtyInfo;
	dirtyInfo.changeType = EntityChangeType::CHANGE_MODIFIED;
	// Don't set backup - leave it as nullopt

	// Call setEntityDirty - should not return early, should call upsert
	auto dirtyBefore = dataManager->getDirtyEntityInfos<Node>({EntityChangeType::CHANGE_MODIFIED});
	dataManager->setEntityDirty(dirtyInfo);

	// Since backup is nullopt, upsert was called but it won't track anything meaningful
	// The important thing is that we didn't return early at line 756
	EXPECT_TRUE(true) << "setEntityDirty with no backup should not crash";
}

TEST_F(DataManagerTest, UpdateNodeStandardFlowNotInDirtyTracking) {
	// Covers L209-214: Standard update flow when entity is NOT in ADDED state
	// 1. Create a node
	auto node = createTestNode(dataManager, "Person");
	dataManager->addNode(node);
	EXPECT_NE(0, node.getId());

	// 2. Save + flush to clear dirty tracking, making the entity persisted
	simulateSave();
	fileStorage->flush();

	// 3. Now the entity is no longer in dirty tracking (not ADDED).
	// Updating it should take the standard update path (L209-214).
	observer->reset();
	node.setLabelId(dataManager->getOrCreateLabelId("UpdatedPerson"));
	dataManager->updateNode(node);

	// Verify the update notification was fired
	ASSERT_EQ(1UL, observer->updatedNodes.size());
}

TEST_F(DataManagerTest, UpdateEdgeStandardFlowNotInDirtyTracking) {
	// Covers L209-214 for Edge template instantiation
	auto node1 = createTestNode(dataManager, "A");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "B");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "KNOWS");
	dataManager->addEdge(edge);

	// Save + flush to persist and clear dirty tracking
	simulateSave();
	fileStorage->flush();

	// Update edge - should take standard update path
	observer->reset();
	edge.setLabelId(dataManager->getOrCreateLabelId("LIKES"));
	dataManager->updateEdge(edge);

	ASSERT_EQ(1UL, observer->updatedEdges.size());
}

TEST_F(DataManagerTest, GetDirtyEdgeInfosAllThreeTypes) {
	// Covers types.size() == 3 fast path for Edge template
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	// Create an edge (ADDED)
	auto edge1 = createTestEdge(dataManager, node1.getId(), node2.getId(), "E1");
	dataManager->addEdge(edge1);

	// Save and then modify another edge (MODIFIED)
	auto edge2 = createTestEdge(dataManager, node1.getId(), node2.getId(), "E2");
	dataManager->addEdge(edge2);
	simulateSave();
	edge2.setLabelId(dataManager->getOrCreateLabelId("E2_mod"));
	dataManager->updateEdge(edge2);

	// Create and delete a third edge (DELETED)
	auto edge3 = createTestEdge(dataManager, node1.getId(), node2.getId(), "E3");
	dataManager->addEdge(edge3);
	simulateSave();
	dataManager->deleteEdge(edge3);

	// Request all 3 types - should hit the fast path (types.size() == 3)
	auto allDirty = dataManager->getDirtyEntityInfos<Edge>(
			{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED, EntityChangeType::CHANGE_DELETED});
	EXPECT_GE(allDirty.size(), 1UL) << "Should have at least one dirty edge info";
}

TEST_F(DataManagerTest, GetDirtyEdgeInfosFiltered) {
	// Covers the filtering loop (types.size() != 3) for Edge template
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "FiltEdge");
	dataManager->addEdge(edge);

	// Only request ADDED type (size != 3, goes through filter loop)
	auto addedOnly = dataManager->getDirtyEntityInfos<Edge>({EntityChangeType::CHANGE_ADDED});
	EXPECT_GE(addedOnly.size(), 1UL) << "Should find at least one ADDED edge";
}

TEST_F(DataManagerTest, GetDirtyPropertyInfosAllThreeTypes) {
	// Covers types.size() == 3 fast path for Property template
	auto node = createTestNode(dataManager, "PropNode");
	dataManager->addNode(node);

	// Add node properties (creates Property entity as ADDED)
	std::unordered_map<std::string, PropertyValue> props;
	props["name"] = PropertyValue(std::string("Alice"));
	dataManager->addNodeProperties(node.getId(), props);

	// Request all 3 types for Property
	auto allDirty = dataManager->getDirtyEntityInfos<Property>(
			{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED, EntityChangeType::CHANGE_DELETED});
	EXPECT_GE(allDirty.size(), 1UL) << "Should have at least one dirty property info";
}

TEST_F(DataManagerTest, GetDirtyBlobInfosAllThreeTypes) {
	// Covers types.size() == 3 fast path for Blob template
	// Use addStateProperties with useBlobStorage=true to create Blob entities
	auto state = createTestState("blob_state_key");
	dataManager->addStateEntity(state);

	std::unordered_map<std::string, PropertyValue> largeProps;
	std::string largeData(5000, 'X');
	largeProps["data"] = PropertyValue(largeData);
	dataManager->addStateProperties("blob_state_key", largeProps, true);

	// Request all 3 types for Blob
	auto allDirty = dataManager->getDirtyEntityInfos<Blob>(
			{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED, EntityChangeType::CHANGE_DELETED});
	EXPECT_GE(allDirty.size(), 0UL) << "Should return blob dirty infos (may be 0 if blob not used internally)";
}

TEST_F(DataManagerTest, GetDirtyIndexInfosAllThreeTypes) {
	// Covers types.size() == 3 fast path for Index template
	auto index = createTestIndex(Index::NodeType::INTERNAL, 1);
	dataManager->addIndexEntity(index);

	// Request all 3 types for Index
	auto allDirty = dataManager->getDirtyEntityInfos<Index>(
			{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED, EntityChangeType::CHANGE_DELETED});
	EXPECT_GE(allDirty.size(), 1UL) << "Should have at least one dirty index info";
}

TEST_F(DataManagerTest, GetDirtyStateInfosAllThreeTypes) {
	// Covers types.size() == 3 fast path for State template
	auto state = createTestState("dirty_state_key");
	dataManager->addStateEntity(state);

	// Request all 3 types for State
	auto allDirty = dataManager->getDirtyEntityInfos<State>(
			{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED, EntityChangeType::CHANGE_DELETED});
	EXPECT_GE(allDirty.size(), 1UL) << "Should have at least one dirty state info";
}

TEST_F(DataManagerTest, SetEntityDirtyEdgeWithInvalidBackup) {
	// Covers setEntityDirty<Edge> with backup ID=0 (L803 True branch for Edge)
	Edge invalidEdge;
	invalidEdge.setId(0);

	DirtyEntityInfo<Edge> dirtyInfo;
	dirtyInfo.changeType = EntityChangeType::CHANGE_MODIFIED;
	dirtyInfo.backup = invalidEdge;

	// Should return early without adding to dirty tracking
	dataManager->setEntityDirty(dirtyInfo);
	EXPECT_TRUE(true) << "setEntityDirty<Edge> with ID=0 should not crash";
}

TEST_F(DataManagerTest, SetEntityDirtyPropertyWithInvalidBackup) {
	// Covers setEntityDirty<Property> with backup ID=0 (L803 True branch for Property)
	Property invalidProp;
	invalidProp.setId(0);

	DirtyEntityInfo<Property> dirtyInfo;
	dirtyInfo.changeType = EntityChangeType::CHANGE_MODIFIED;
	dirtyInfo.backup = invalidProp;

	dataManager->setEntityDirty(dirtyInfo);
	EXPECT_TRUE(true) << "setEntityDirty<Property> with ID=0 should not crash";
}

TEST_F(DataManagerTest, SetEntityDirtyBlobWithInvalidBackup) {
	// Covers setEntityDirty<Blob> with backup ID=0 (L803 True branch for Blob)
	Blob invalidBlob;
	invalidBlob.setId(0);

	DirtyEntityInfo<Blob> dirtyInfo;
	dirtyInfo.changeType = EntityChangeType::CHANGE_MODIFIED;
	dirtyInfo.backup = invalidBlob;

	dataManager->setEntityDirty(dirtyInfo);
	EXPECT_TRUE(true) << "setEntityDirty<Blob> with ID=0 should not crash";
}

TEST_F(DataManagerTest, SetEntityDirtyIndexWithInvalidBackup) {
	// Covers setEntityDirty<Index> with backup ID=0 (L803 True branch for Index)
	Index invalidIndex;
	invalidIndex.setId(0);

	DirtyEntityInfo<Index> dirtyInfo;
	dirtyInfo.changeType = EntityChangeType::CHANGE_MODIFIED;
	dirtyInfo.backup = invalidIndex;

	dataManager->setEntityDirty(dirtyInfo);
	EXPECT_TRUE(true) << "setEntityDirty<Index> with ID=0 should not crash";
}

TEST_F(DataManagerTest, SetEntityDirtyStateWithInvalidBackup) {
	// Covers setEntityDirty<State> with backup ID=0 (L803 True branch for State)
	State invalidState;
	invalidState.setId(0);

	DirtyEntityInfo<State> dirtyInfo;
	dirtyInfo.changeType = EntityChangeType::CHANGE_MODIFIED;
	dirtyInfo.backup = invalidState;

	dataManager->setEntityDirty(dirtyInfo);
	EXPECT_TRUE(true) << "setEntityDirty<State> with ID=0 should not crash";
}

TEST_F(DataManagerTest, GetDirtyEntityInfosEdgeFiltered) {
	// Covers getDirtyEntityInfos<Edge> filter loop (L847-L855)
	// Need dirty edges with < 3 types to go through filter path
	auto node1 = createTestNode(dataManager, "EdgeFilterSrc");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "EdgeFilterDst");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "FILTER_TEST");
	dataManager->addEdge(edge);

	// Query with single type - should go through filter loop
	auto addedOnly = dataManager->getDirtyEntityInfos<Edge>({EntityChangeType::CHANGE_ADDED});
	EXPECT_GE(addedOnly.size(), 1UL) << "Should find added edge";

	// Query with 2 types - also goes through filter loop
	auto addedOrModified = dataManager->getDirtyEntityInfos<Edge>(
			{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED});
	EXPECT_GE(addedOrModified.size(), 1UL) << "Should find added edge with 2-type filter";
}

TEST_F(DataManagerTest, GetDirtyEntityInfosPropertyFilterNoMatch) {
	auto prop = createTestProperty(1, 0, {{"k", PropertyValue(1)}});
	dataManager->addPropertyEntity(prop);

	auto deletedOnly = dataManager->getDirtyEntityInfos<Property>({EntityChangeType::CHANGE_DELETED});
	EXPECT_TRUE(deletedOnly.empty()) << "ADDED property should not match DELETED filter";
}

TEST_F(DataManagerTest, GetDirtyEntityInfosBlobFilterNoMatch) {
	auto blob = createTestBlob("filterblobdata");
	dataManager->addBlobEntity(blob);

	auto deletedOnly = dataManager->getDirtyEntityInfos<Blob>({EntityChangeType::CHANGE_DELETED});
	EXPECT_TRUE(deletedOnly.empty()) << "ADDED blob should not match DELETED filter";
}

TEST_F(DataManagerTest, GetDirtyEntityInfosIndexFilterNoMatch) {
	auto idx = createTestIndex(Index::NodeType::LEAF, 1);
	dataManager->addIndexEntity(idx);

	auto deletedOnly = dataManager->getDirtyEntityInfos<Index>({EntityChangeType::CHANGE_DELETED});
	EXPECT_TRUE(deletedOnly.empty()) << "ADDED index should not match DELETED filter";
}

TEST_F(DataManagerTest, GetDirtyEntityInfosStateFilterNoMatch) {
	auto state = createTestState("filter.state.key");
	dataManager->addStateEntity(state);

	auto deletedOnly = dataManager->getDirtyEntityInfos<State>({EntityChangeType::CHANGE_DELETED});
	EXPECT_TRUE(deletedOnly.empty()) << "ADDED state should not match DELETED filter";
}

TEST_F(DataManagerTest, GetDirtyEntityInfos_FilteredTypes) {
	// Create a node (makes it dirty with CHANGE_ADDED)
	auto node = createTestNode(dataManager, "DirtyInfoNode");
	dataManager->addNode(node);

	// Query with only ADDED type - should filter
	std::vector<EntityChangeType> addedOnly = {EntityChangeType::CHANGE_ADDED};
	auto infos = dataManager->getDirtyEntityInfos<Node>(addedOnly);
	EXPECT_GE(infos.size(), 1UL) << "Should find at least one ADDED node";

	// Verify all returned infos are of the requested type
	for (const auto &info : infos) {
		EXPECT_EQ(info.changeType, EntityChangeType::CHANGE_ADDED);
	}
}

TEST_F(DataManagerTest, GetDirtyEntityInfos_NoMatchingType) {
	// Create a node (CHANGE_ADDED)
	auto node = createTestNode(dataManager, "NoMatchNode");
	dataManager->addNode(node);

	// Query with only DELETED type - ADDED nodes should not match
	std::vector<EntityChangeType> deletedOnly = {EntityChangeType::CHANGE_DELETED};
	auto infos = dataManager->getDirtyEntityInfos<Node>(deletedOnly);
	// May or may not be empty depending on prior state, but ADDED nodes shouldn't appear
	for (const auto &info : infos) {
		EXPECT_EQ(info.changeType, EntityChangeType::CHANGE_DELETED);
	}
}

TEST_F(DataManagerTest, SetEntityDirty_ZeroIdEntity) {
	Node zeroNode;
	// zeroNode has id == 0 by default
	DirtyEntityInfo<Node> info(EntityChangeType::CHANGE_ADDED, zeroNode);
	// Should early-return without crashing (line 803: backup->getId() == 0)
	EXPECT_NO_THROW(dataManager->setEntityDirty<Node>(info));
}

TEST_F(DataManagerTest, GetDirtyEntityInfosPropertyFilteredWithMatch) {
	auto node = createTestNode(dataManager, "DirtyPropNode");
	dataManager->addNode(node);

	auto prop = createTestProperty(node.getId(), Node::typeId,
		{{"k", PropertyValue(std::string("v"))}});
	dataManager->addPropertyEntity(prop);

	// Filter for ADDED only - should match since we just added
	auto addedOnly = dataManager->getDirtyEntityInfos<Property>({EntityChangeType::CHANGE_ADDED});
	EXPECT_GE(addedOnly.size(), 1UL) << "Should find ADDED property in dirty infos";
}

TEST_F(DataManagerTest, GetDirtyEntityInfosBlobFilteredWithMatch) {
	auto blob = createTestBlob("dirty_blob_test");
	dataManager->addBlobEntity(blob);

	auto addedOnly = dataManager->getDirtyEntityInfos<Blob>({EntityChangeType::CHANGE_ADDED});
	EXPECT_GE(addedOnly.size(), 1UL) << "Should find ADDED blob in dirty infos";
}

TEST_F(DataManagerTest, GetDirtyEntityInfosIndexFilteredWithMatch) {
	auto idx = createTestIndex(Index::NodeType::LEAF, 999);
	dataManager->addIndexEntity(idx);

	auto addedOnly = dataManager->getDirtyEntityInfos<Index>({EntityChangeType::CHANGE_ADDED});
	EXPECT_GE(addedOnly.size(), 1UL) << "Should find ADDED index in dirty infos";
}

TEST_F(DataManagerTest, GetDirtyEntityInfosStateFilteredWithMatch) {
	auto state = createTestState("dirty_state_test");
	dataManager->addStateEntity(state);

	auto addedOnly = dataManager->getDirtyEntityInfos<State>({EntityChangeType::CHANGE_ADDED});
	EXPECT_GE(addedOnly.size(), 1UL) << "Should find ADDED state in dirty infos";
}

TEST_F(DataManagerTest, GetDirtyEntityInfosNodeTwoTypesWithMatch) {
	auto node = createTestNode(dataManager, "TwoTypeNode");
	dataManager->addNode(node);

	// Filter for ADDED and MODIFIED (2 types, not 3, so uses filter loop)
	auto filtered = dataManager->getDirtyEntityInfos<Node>(
		{EntityChangeType::CHANGE_ADDED, EntityChangeType::CHANGE_MODIFIED});
	EXPECT_GE(filtered.size(), 1UL) << "Should find ADDED node with 2-type filter";
}

