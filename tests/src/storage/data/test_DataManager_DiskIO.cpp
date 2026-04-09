/**
 * @file test_DataManager_DiskIO.cpp
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

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskDeleted) {
	// Create and save a node
	auto node = createTestNode(dataManager, "DeletedTest");
	dataManager->addNode(node);
	simulateSave();

	// Delete the node
	dataManager->deleteNode(node);

	// Verify it returns inactive entity
	auto retrieved = dataManager->getNode(node.getId());
	EXPECT_FALSE(retrieved.isActive());
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskInactiveInCache) {
	// Test line 854: Inactive entities in cache should be handled correctly
	auto node = createTestNode(dataManager, "CachedInactiveNode");
	dataManager->addNode(node);
	simulateSave(); // Save to disk

	// Load into cache
	auto loaded1 = dataManager->getNode(node.getId());
	EXPECT_TRUE(loaded1.isActive()) << "Node should be active after save";

	// Delete it (marks inactive in cache)
	dataManager->deleteNode(loaded1);

	// Clear dirty state but node remains inactive in cache
	simulateSave();

	// Try to get it again - should return inactive entity from cache
	auto loaded2 = dataManager->getNode(node.getId());
	EXPECT_FALSE(loaded2.isActive()) << "Should return inactive entity from cache";
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskNonExistent) {
	// Try to get a node that doesn't exist
	// This will go through: dirtyInfo check (no) → cache check (no) → loadFromDisk (returns ID=0)
	auto nonExistent = dataManager->getNode(999999);
	EXPECT_FALSE(nonExistent.isActive()) << "Non-existent entity should be marked inactive";
	EXPECT_EQ(0, nonExistent.getId()) << "Non-existent entity should have ID=0";
}

TEST_F(DataManagerTest, LoadInactiveEntityFromDisk) {
	// Create and save a node
	auto node = createTestNode(dataManager, "InactiveNode");
	dataManager->addNode(node);
	simulateSave();

	// Clear cache by getting a lot of other nodes to force eviction
	// Or just create a new database with same file to ensure clean state
	// Actually, let's use the persistence API to verify the node exists on disk
	auto nodeId = node.getId();

	// Delete the node (marks as inactive in dirty tracking)
	dataManager->deleteNode(node);

	// Save to persist the deletion
	simulateSave();

	// Now the node on disk should be marked inactive
	// Try to load it again - should return inactive
	auto loaded = dataManager->getNode(nodeId);
	EXPECT_FALSE(loaded.isActive()) << "Deleted node should be inactive";
}

TEST_F(DataManagerTest, UpdateModifiedEntity) {
	// Create a node
	auto node = createTestNode(dataManager, "NodeToUpdate");
	dataManager->addNode(node);

	// First update - puts it in MODIFIED state (still ADDED in dirty tracking until save)
	node.setLabelId(dataManager->getOrCreateTokenId("FirstUpdate"));
	dataManager->updateNode(node);

	// Get the dirty info - should be ADDED or MODIFIED
	auto dirtyInfo1 = dataManager->getDirtyInfo<Node>(node.getId());
	ASSERT_TRUE(dirtyInfo1.has_value()) << "Node should be in dirty tracking";

	// Second update - now dirtyInfo exists and is in MODIFIED state
	node.setLabelId(dataManager->getOrCreateTokenId("SecondUpdate"));
	dataManager->updateNode(node);

	// Verify it's still in dirty tracking
	auto dirtyInfo2 = dataManager->getDirtyInfo<Node>(node.getId());
	ASSERT_TRUE(dirtyInfo2.has_value()) << "Node should still be in dirty tracking after second update";

	// Verify the update was applied
	auto retrieved = dataManager->getNode(node.getId());
	EXPECT_EQ(node.getLabelId(), retrieved.getLabelId());
}

TEST_F(DataManagerTest, LoadNonExistentEntities) {
	// Test non-existent Edge
	auto nonExistentEdge = dataManager->getEdge(999998);
	EXPECT_FALSE(nonExistentEdge.isActive()) << "Non-existent edge should be marked inactive";
	EXPECT_EQ(0, nonExistentEdge.getId()) << "Non-existent edge should have ID=0";

	// Test non-existent Blob
	auto nonExistentBlob = dataManager->getBlob(999997);
	EXPECT_FALSE(nonExistentBlob.isActive()) << "Non-existent blob should be marked inactive";
	EXPECT_EQ(0, nonExistentBlob.getId()) << "Non-existent blob should have ID=0";

	// Test non-existent Index
	auto nonExistentIndex = dataManager->getIndex(999996);
	EXPECT_FALSE(nonExistentIndex.isActive()) << "Non-existent index should be marked inactive";
	EXPECT_EQ(0, nonExistentIndex.getId()) << "Non-existent index should have ID=0";

	// Test non-existent State
	auto nonExistentState = dataManager->getState(999995);
	EXPECT_FALSE(nonExistentState.isActive()) << "Non-existent state should be marked inactive";
	EXPECT_EQ(0, nonExistentState.getId()) << "Non-existent state should have ID=0";
}

TEST_F(DataManagerTest, GetEdgesInRangeFromDisk) {
	// Covers Pass 2 disk loop in getEntitiesInRange<Edge>
	// Create source/target nodes first
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	// Create 10 edges and flush them to disk
	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 10; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "DiskEdge" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	// Flush to disk (writes dirty layer to disk and commits snapshot)
	fileStorage->flush();

	// Clear cache so entities must be loaded from disk
	dataManager->clearCache();

	// Query all edges in range - should trigger disk pass
	auto edges = dataManager->getEdgesInRange(edgeIds.front(), edgeIds.back(), 100);
	EXPECT_EQ(10UL, edges.size()) << "Should find all 10 edges from disk";

	// Verify all expected IDs are present (order may differ due to segment layout)
	std::unordered_set<int64_t> returnedIds;
	for (const auto &e : edges) {
		EXPECT_TRUE(e.isActive());
		returnedIds.insert(e.getId());
	}
	for (auto expectedId : edgeIds) {
		EXPECT_TRUE(returnedIds.contains(expectedId))
				<< "Expected edge ID " << expectedId << " not found in results";
	}
}

TEST_F(DataManagerTest, GetEdgesInRangeWithLimitFromDisk) {
	// Covers limit check in disk pass loop for Edge template
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 10; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "LimitEdge" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	// Request only 3 edges
	auto edges = dataManager->getEdgesInRange(edgeIds.front(), edgeIds.back(), 3);
	EXPECT_EQ(3UL, edges.size()) << "Should respect limit of 3";
}

TEST_F(DataManagerTest, GetEdgesInRangeNoOverlap) {
	// Covers intersectStart > intersectEnd (no overlap) branch for Edge template
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "SingleEdge");
	dataManager->addEdge(edge);

	fileStorage->flush();
	dataManager->clearCache();

	// Query a range far from the actual edge ID
	auto edges = dataManager->getEdgesInRange(edge.getId() + 10000, edge.getId() + 20000, 100);
	EXPECT_EQ(0UL, edges.size()) << "Should find nothing when range doesn't overlap";
}

TEST_F(DataManagerTest, GetEdgesInRangeMixedDirtyAndDisk) {
	// Covers interaction between Pass 1 (memory) and Pass 2 (disk) for Edge
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	// Create 8 edges and flush to disk
	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 8; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "MixEdge" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	fileStorage->flush();

	// Modify 3 edges (puts them in dirty layer as MODIFIED)
	for (int i = 0; i < 3; i++) {
		auto edge = dataManager->getEdge(edgeIds[i]);
		edge.setTypeId(dataManager->getOrCreateTokenId("ModifiedEdge" + std::to_string(i)));
		dataManager->updateEdge(edge);
	}

	dataManager->clearCache();

	// Query all - should get modified ones from dirty (Pass 1) and rest from disk (Pass 2)
	auto edges = dataManager->getEdgesInRange(edgeIds.front(), edgeIds.back(), 100);
	EXPECT_EQ(8UL, edges.size()) << "Should find all 8 edges from mixed sources";
}

TEST_F(DataManagerTest, GetEdgesInRangeLimitAfterSegment) {
	// Covers limit check after processing a full segment for Edge template
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 20; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "SegEdge" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	// Request 7 edges - should stop mid-way through disk read
	auto edges = dataManager->getEdgesInRange(edgeIds.front(), edgeIds.back(), 7);
	EXPECT_EQ(7UL, edges.size()) << "Should return exactly 7 edges";
}

TEST_F(DataManagerTest, GetEdgesInRangeCacheAndDiskInteraction) {
	// Covers cache check in Pass 1 and dedup in Pass 2 for Edge template
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 10; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "CacheEdge" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	// Pre-load 3 edges into cache by fetching them individually
	for (int i = 0; i < 3; i++) {
		auto edge = dataManager->getEdge(edgeIds[i]);
		EXPECT_TRUE(edge.isActive());
	}

	// Now query the full range - Pass 1 should find 3 in cache, Pass 2 gets rest from disk
	auto edges = dataManager->getEdgesInRange(edgeIds.front(), edgeIds.back(), 100);
	EXPECT_EQ(10UL, edges.size()) << "Should find all 10 without duplicates";
}

TEST_F(DataManagerTest, GetEdgesInRangeSkipsDeletedInDirtyLayer) {
	// Covers CHANGE_DELETED check in Pass 1 for Edge template
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 5; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "DelEdge" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	fileStorage->flush();

	// Delete one edge (marks it as CHANGE_DELETED in dirty layer)
	auto edgeToDel = dataManager->getEdge(edgeIds[2]);
	dataManager->deleteEdge(edgeToDel);

	dataManager->clearCache();

	// Query all - the deleted edge should be skipped in Pass 1
	auto edges = dataManager->getEdgesInRange(edgeIds.front(), edgeIds.back(), 100);
	EXPECT_EQ(4UL, edges.size()) << "Should skip deleted edge";
}

TEST_F(DataManagerTest, GetEdgesInRangeLimitOneFromDisk) {
	// Covers early break with limit=1 in disk pass for Edge template
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 5; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "OneEdge" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	auto edges = dataManager->getEdgesInRange(edgeIds.front(), edgeIds.back(), 1);
	EXPECT_EQ(1UL, edges.size()) << "Should return exactly 1 edge";
}

TEST_F(DataManagerTest, ReadEntityFromDiskInactiveEdge) {
	// Covers readEntityFromDisk<Edge> inactive entity branch (L711 True for Edge)
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "InactiveEdge");
	dataManager->addEdge(edge);
	int64_t edgeId = edge.getId();

	// Save to disk
	fileStorage->flush();

	// Delete and save again (marks inactive on disk)
	auto loadedEdge = dataManager->getEdge(edgeId);
	dataManager->deleteEdge(loadedEdge);
	fileStorage->flush();

	// Clear cache and dirty state, then try to load from disk
	dataManager->clearCache();

	auto result = dataManager->getEdge(edgeId);
	EXPECT_FALSE(result.isActive()) << "Deleted edge should be inactive when loaded from disk";
}

TEST_F(DataManagerTest, ReadEntityFromDiskInactiveState) {
	// Covers readEntityFromDisk<State> inactive entity branch (L711 True for State)
	auto state = createTestState("inactive.state");
	dataManager->addStateEntity(state);
	int64_t stateId = state.getId();

	// Save to disk
	fileStorage->flush();

	// Delete and save again
	dataManager->deleteState(state);
	fileStorage->flush();

	// Clear cache and try to load from disk
	dataManager->clearCache();

	auto result = dataManager->getState(stateId);
	EXPECT_FALSE(result.isActive()) << "Deleted state should be inactive when loaded from disk";
}

TEST_F(DataManagerTest, ReadEntityFromDiskInactiveNode) {
	// Covers readEntityFromDisk<Node> inactive entity branch (L711 True for Node)
	auto node = createTestNode(dataManager, "InactiveNode");
	dataManager->addNode(node);
	int64_t nodeId = node.getId();

	// Save to disk
	fileStorage->flush();

	// Delete and save again (marks inactive on disk)
	auto loadedNode = dataManager->getNode(nodeId);
	dataManager->deleteNode(loadedNode);
	fileStorage->flush();

	// Clear cache then try to load from disk
	dataManager->clearCache();

	auto result = dataManager->getNode(nodeId);
	EXPECT_FALSE(result.isActive()) << "Deleted node should be inactive when loaded from disk";
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskPropertyCacheInactive) {
	auto prop = createTestProperty(1, 0, {{"k", PropertyValue(42)}});
	dataManager->addPropertyEntity(prop);
	int64_t propId = prop.getId();

	fileStorage->flush();

	// Delete to mark inactive
	dataManager->deleteProperty(prop);
	fileStorage->flush();

	// Access the entity - it should be inactive
	auto result = dataManager->getProperty(propId);
	if (result.getId() != 0)
		EXPECT_FALSE(result.isActive()) << "Deleted property should be inactive";
	else
		EXPECT_EQ(0, result.getId());
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskBlobDiskLoad) {
	auto blob = createTestBlob("diskloadblob");
	dataManager->addBlobEntity(blob);
	int64_t blobId = blob.getId();

	fileStorage->flush();
	dataManager->clearCache();

	// Load from disk
	auto result = dataManager->getBlob(blobId);
	EXPECT_TRUE(result.isActive()) << "Blob loaded from disk should be active";
	EXPECT_EQ(blobId, result.getId());
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskIndexDiskLoad) {
	auto idx = createTestIndex(Index::NodeType::LEAF, 1);
	dataManager->addIndexEntity(idx);
	int64_t indexId = idx.getId();

	fileStorage->flush();
	dataManager->clearCache();

	auto result = dataManager->getIndex(indexId);
	EXPECT_TRUE(result.isActive()) << "Index loaded from disk should be active";
	EXPECT_EQ(indexId, result.getId());
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskStateDiskLoad) {
	auto state = createTestState("disk.load.state");
	dataManager->addStateEntity(state);
	int64_t stateId = state.getId();

	fileStorage->flush();
	dataManager->clearCache();

	auto result = dataManager->getState(stateId);
	EXPECT_TRUE(result.isActive()) << "State loaded from disk should be active";
	EXPECT_EQ(stateId, result.getId());
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskPropertyDiskNotFound) {
	// Covers getEntityFromMemoryOrDisk<Property> L898 False branch (entity not on disk)
	auto result = dataManager->getProperty(999999);
	EXPECT_FALSE(result.isActive()) << "Non-existent property should be inactive";
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskBlobNotFound) {
	// Covers getEntityFromMemoryOrDisk<Blob> L876 or L898 path for non-existent
	auto result = dataManager->getBlob(999999);
	EXPECT_FALSE(result.isActive()) << "Non-existent blob should be inactive";
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskIndexNotFound) {
	auto result = dataManager->getIndex(999999);
	EXPECT_FALSE(result.isActive()) << "Non-existent index should be inactive";
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskStateNotFound) {
	auto result = dataManager->getState(999999);
	EXPECT_FALSE(result.isActive()) << "Non-existent state should be inactive";
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskPropertyDeleted) {
	auto prop = createTestProperty(1, 0, {{"k", PropertyValue(99)}});
	dataManager->addPropertyEntity(prop);
	int64_t propId = prop.getId();

	fileStorage->flush();

	auto loaded = dataManager->getProperty(propId);
	dataManager->deleteProperty(loaded);

	auto result = dataManager->getProperty(propId);
	EXPECT_FALSE(result.isActive()) << "Deleted property should be inactive from dirty layer";
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskBlobDeleted) {
	auto blob = createTestBlob("deletedblob");
	dataManager->addBlobEntity(blob);
	int64_t blobId = blob.getId();

	fileStorage->flush();

	auto loaded = dataManager->getBlob(blobId);
	dataManager->deleteBlob(loaded);

	auto result = dataManager->getBlob(blobId);
	EXPECT_FALSE(result.isActive()) << "Deleted blob should be inactive from dirty layer";
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskIndexDeleted) {
	auto idx = createTestIndex(Index::NodeType::LEAF, 1);
	dataManager->addIndexEntity(idx);
	int64_t indexId = idx.getId();

	fileStorage->flush();

	auto loaded = dataManager->getIndex(indexId);
	dataManager->deleteIndex(loaded);

	auto result = dataManager->getIndex(indexId);
	EXPECT_FALSE(result.isActive()) << "Deleted index should be inactive from dirty layer";
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDiskStateDeleted) {
	auto state = createTestState("deleted.state");
	dataManager->addStateEntity(state);
	int64_t stateId = state.getId();

	fileStorage->flush();

	auto loaded = dataManager->getState(stateId);
	dataManager->deleteState(loaded);

	auto result = dataManager->getState(stateId);
	EXPECT_FALSE(result.isActive()) << "Deleted state should be inactive from dirty layer";
}

TEST_F(DataManagerTest, LoadNodeFromDiskNotFound) {
	// Covers L910 nodeOpt not has_value → returns inactive
	dataManager->clearCache();
	auto result = dataManager->getNode(999999);
	EXPECT_FALSE(result.isActive());
}

TEST_F(DataManagerTest, LoadEdgeFromDiskNotFound) {
	// Covers loadEdgeFromDisk not found
	dataManager->clearCache();
	auto result = dataManager->getEdge(999999);
	EXPECT_FALSE(result.isActive());
}

TEST_F(DataManagerTest, LoadPropertyFromDiskNotFound) {
	// Covers L924 propertyOpt not has_value
	dataManager->clearCache();
	auto result = dataManager->getProperty(999999);
	EXPECT_FALSE(result.isActive());
}

TEST_F(DataManagerTest, LoadBlobFromDiskNotFound) {
	// Covers L931 blobOpt not has_value
	dataManager->clearCache();
	auto result = dataManager->getBlob(999999);
	EXPECT_FALSE(result.isActive());
}

TEST_F(DataManagerTest, LoadIndexFromDiskNotFound) {
	// Covers L938 indexOpt not has_value
	dataManager->clearCache();
	auto result = dataManager->getIndex(999999);
	EXPECT_FALSE(result.isActive());
}

TEST_F(DataManagerTest, LoadStateFromDiskNotFound) {
	// Covers L945 stateOpt not has_value
	dataManager->clearCache();
	auto result = dataManager->getState(999999);
	EXPECT_FALSE(result.isActive());
}

TEST_F(DataManagerTest, ReadEntityFromDiskInactiveProperty) {
	auto prop = createTestProperty(1, 0, {{"k", PropertyValue(2)}});
	dataManager->addPropertyEntity(prop);
	int64_t propId = prop.getId();

	fileStorage->flush();

	auto loaded = dataManager->getProperty(propId);
	dataManager->deleteProperty(loaded);
	fileStorage->flush();
	dataManager->clearCache();

	auto result = dataManager->getProperty(propId);
	EXPECT_FALSE(result.isActive()) << "Deleted property should be inactive from disk";
}

TEST_F(DataManagerTest, ReadEntityFromDiskInactiveBlob) {
	auto blob = createTestBlob("inactiveblob");
	dataManager->addBlobEntity(blob);
	int64_t blobId = blob.getId();

	fileStorage->flush();

	auto loaded = dataManager->getBlob(blobId);
	dataManager->deleteBlob(loaded);
	fileStorage->flush();
	dataManager->clearCache();

	auto result = dataManager->getBlob(blobId);
	EXPECT_FALSE(result.isActive()) << "Deleted blob should be inactive from disk";
}

TEST_F(DataManagerTest, ReadEntityFromDiskInactiveIndex) {
	auto idx = createTestIndex(Index::NodeType::LEAF, 1);
	dataManager->addIndexEntity(idx);
	int64_t indexId = idx.getId();

	fileStorage->flush();

	auto loaded = dataManager->getIndex(indexId);
	dataManager->deleteIndex(loaded);
	fileStorage->flush();
	dataManager->clearCache();

	auto result = dataManager->getIndex(indexId);
	EXPECT_FALSE(result.isActive()) << "Deleted index should be inactive from disk";
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDisk_InactiveCachedEntity) {
	auto node = createTestNode(dataManager, "InactiveCacheNode");
	dataManager->addNode(node);
	int64_t nodeId = node.getId();

	// Save to disk
	fileStorage->flush();

	// Load into cache
	(void)dataManager->getNode(nodeId);

	// Delete and save - the cache might still have it but now it's inactive
	dataManager->deleteNode(node);
	fileStorage->flush();
	dataManager->clearCache();

	// Now try to get - should go through disk path and find inactive
	auto result = dataManager->getEntityFromMemoryOrDisk<Node>(nodeId);
	// The entity should be inactive after deletion
	EXPECT_FALSE(result.isActive());
}

TEST_F(DataManagerTest, GetEntityFromMemoryOrDisk_DeletedEntity) {
	auto node = createTestNode(dataManager, "DeletedMemNode");
	dataManager->addNode(node);
	int64_t nodeId = node.getId();
	fileStorage->flush();

	// Delete the node (marks as CHANGE_DELETED in dirty info)
	auto loaded = dataManager->getNode(nodeId);
	dataManager->deleteNode(loaded);

	// getEntityFromMemoryOrDisk should detect CHANGE_DELETED and return inactive
	auto result = dataManager->getEntityFromMemoryOrDisk<Node>(nodeId);
	EXPECT_FALSE(result.isActive()) << "Deleted entity should be returned as inactive";
}

