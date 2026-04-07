/**
 * @file test_DataManager_EntityTypes.cpp
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

TEST_F(DataManagerTest, PropertyEntityCRUD) {
	auto node = createTestNode(dataManager, "PropertyHolder");
	dataManager->addNode(node);

	std::unordered_map<std::string, PropertyValue> props = {{"test_key", PropertyValue("test_value")}};
	auto property = createTestProperty(node.getId(), Node::typeId, props);
	dataManager->addPropertyEntity(property);
	EXPECT_NE(0, property.getId());

	auto retrievedProperty = dataManager->getProperty(property.getId());
	EXPECT_EQ(property.getId(), retrievedProperty.getId());
	EXPECT_EQ(node.getId(), retrievedProperty.getEntityId());
	ASSERT_TRUE(retrievedProperty.hasPropertyValue("test_key"));
	EXPECT_EQ("test_value", std::get<std::string>(retrievedProperty.getPropertyValues().at("test_key").getVariant()));

	props["test_key"] = PropertyValue(42);
	property.setProperties(props);
	dataManager->updatePropertyEntity(property);
	retrievedProperty = dataManager->getProperty(property.getId());
	EXPECT_EQ(42, std::get<int64_t>(retrievedProperty.getPropertyValues().at("test_key").getVariant()));

	dataManager->deleteProperty(property);
	dataManager->clearCache();
	retrievedProperty = dataManager->getProperty(property.getId());
	if (retrievedProperty.getId() != 0)
		EXPECT_FALSE(retrievedProperty.isActive());
	else
		EXPECT_EQ(0, retrievedProperty.getId());
}

TEST_F(DataManagerTest, BlobEntityCRUD) {
	std::string blobDataStr = "12345";
	auto blob = createTestBlob(blobDataStr);
	dataManager->addBlobEntity(blob);
	EXPECT_NE(0, blob.getId());

	auto retrievedBlob = dataManager->getBlob(blob.getId());
	EXPECT_EQ(blob.getId(), retrievedBlob.getId());
	EXPECT_EQ(blobDataStr, retrievedBlob.getDataAsString());

	std::string newBlobDataStr = "67890";
	blob.setData(newBlobDataStr);
	dataManager->updateBlobEntity(blob);
	retrievedBlob = dataManager->getBlob(blob.getId());
	EXPECT_EQ(newBlobDataStr, retrievedBlob.getDataAsString());

	dataManager->deleteBlob(blob);
	dataManager->clearCache();
	retrievedBlob = dataManager->getBlob(blob.getId());
	if (retrievedBlob.getId() != 0)
		EXPECT_FALSE(retrievedBlob.isActive());
	else
		EXPECT_EQ(0, retrievedBlob.getId());
}

TEST_F(DataManagerTest, IndexEntityCRUD) {
	auto index = createTestIndex(Index::NodeType::LEAF, 1);
	dataManager->addIndexEntity(index);
	EXPECT_NE(0, index.getId());

	auto retrievedIndex = dataManager->getIndex(index.getId());
	EXPECT_EQ(index.getId(), retrievedIndex.getId());
	EXPECT_EQ(1U, retrievedIndex.getIndexType());
	EXPECT_EQ(Index::NodeType::LEAF, retrievedIndex.getNodeType());

	index.setLevel(5);
	dataManager->updateIndexEntity(index);
	retrievedIndex = dataManager->getIndex(index.getId());
	EXPECT_EQ(5, retrievedIndex.getLevel());

	dataManager->deleteIndex(index);
	dataManager->clearCache();
	retrievedIndex = dataManager->getIndex(index.getId());
	if (retrievedIndex.getId() != 0)
		EXPECT_FALSE(retrievedIndex.isActive());
	else
		EXPECT_EQ(0, retrievedIndex.getId());
}

TEST_F(DataManagerTest, StateEntityCRUD) {
	auto state = createTestState("test.state.key");
	dataManager->addStateEntity(state);
	EXPECT_NE(0, state.getId());

	auto retrievedState = dataManager->getState(state.getId());
	EXPECT_EQ(state.getId(), retrievedState.getId());
	EXPECT_EQ("test.state.key", retrievedState.getKey());

	auto foundState = dataManager->findStateByKey("test.state.key");
	EXPECT_EQ(state.getId(), foundState.getId());

	state.setKey("updated.state.key");
	dataManager->updateStateEntity(state);
	auto notFoundState = dataManager->findStateByKey("test.state.key");
	EXPECT_EQ(0, notFoundState.getId());
	foundState = dataManager->findStateByKey("updated.state.key");
	EXPECT_EQ(state.getId(), foundState.getId());
	EXPECT_EQ("updated.state.key", foundState.getKey());

	dataManager->deleteState(state);
	dataManager->clearCache();
	retrievedState = dataManager->getState(state.getId());
	if (retrievedState.getId() != 0)
		EXPECT_FALSE(retrievedState.isActive());
	else
		EXPECT_EQ(0, retrievedState.getId());
}

TEST_F(DataManagerTest, StateProperties) {
	auto state = createTestState("config.state");
	dataManager->addStateEntity(state);

	std::unordered_map<std::string, PropertyValue> properties = {
			{"setting1", PropertyValue("value1")}, {"setting2", PropertyValue(42)}, {"setting3", PropertyValue(true)}};
	dataManager->addStateProperties("config.state", properties);

	auto retrievedProps = dataManager->getStateProperties("config.state");
	EXPECT_EQ(3UL, retrievedProps.size());
	EXPECT_EQ("value1", std::get<std::string>(retrievedProps.at("setting1").getVariant()));
	EXPECT_EQ(42, std::get<int64_t>(retrievedProps.at("setting2").getVariant()));
	EXPECT_TRUE(std::get<bool>(retrievedProps.at("setting3").getVariant()));

	dataManager->removeState("config.state");
	auto foundState = dataManager->findStateByKey("config.state");
	EXPECT_EQ(0, foundState.getId());

	auto emptyProps = dataManager->getStateProperties("config.state");
	EXPECT_TRUE(emptyProps.empty());
}

TEST_F(DataManagerTest, GetEntitiesInRangeInvalid) {
	// Test with start > end (should return empty)
	auto nodes = dataManager->getNodesInRange(100, 50, 10);
	EXPECT_TRUE(nodes.empty());

	// Test with limit = 0 (should return empty)
	auto nodes2 = dataManager->getNodesInRange(1, 100, 0);
	EXPECT_TRUE(nodes2.empty());

	// Test with start == end
	auto node = createTestNode(dataManager, "SingleRange");
	dataManager->addNode(node);

	auto nodes3 = dataManager->getNodesInRange(node.getId(), node.getId(), 10);
	EXPECT_EQ(1UL, nodes3.size());
}

TEST_F(DataManagerTest, GetEntitiesInRangeMemorySaturation) {
	// Create 20 nodes
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < 20; i++) {
		auto node = createTestNode(dataManager, "MemSat" + std::to_string(i));
		dataManager->addNode(node);
		nodeIds.push_back(node.getId());
	}

	// Request with limit smaller than available count
	auto nodes = dataManager->getNodesInRange(nodeIds.front(), nodeIds.back(), 5);
	EXPECT_EQ(5UL, nodes.size());

	// Verify they are the first 5 nodes
	EXPECT_EQ(nodeIds[0], nodes[0].getId());
	EXPECT_EQ(nodeIds[4], nodes[4].getId());
}

TEST_F(DataManagerTest, GetEntitiesInRangeAfterCacheClear) {
	// Create nodes
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < 5; i++) {
		auto node = createTestNode(dataManager, "CachedNode" + std::to_string(i));
		dataManager->addNode(node);
		nodeIds.push_back(node.getId());
	}

	// Verify nodes are in memory (from PersistenceManager)
	auto beforeClear = dataManager->getNodesInRange(nodeIds.front(), nodeIds.back(), 10);
	EXPECT_EQ(5UL, beforeClear.size()) << "Should find all nodes in memory";

	// Clear cache - nodes should still be accessible from dirty layer
	dataManager->clearCache();

	auto afterClear = dataManager->getNodesInRange(nodeIds.front(), nodeIds.back(), 10);
	EXPECT_EQ(5UL, afterClear.size()) << "Should still find nodes from dirty layer after cache clear";

	// Verify IDs match
	for (size_t i = 0; i < 5; i++) {
		EXPECT_EQ(nodeIds[i], afterClear[i].getId());
	}
}

TEST_F(DataManagerTest, StatePropertiesWithBlobStorage) {
	std::string stateKey = "blob.state";

	// Create large state properties that should trigger blob storage
	std::unordered_map<std::string, PropertyValue> largeProps;
	std::string largeData(5000, 'Y');
	largeProps["large_data"] = PropertyValue(largeData);

	// Add with explicit blob storage flag
	dataManager->addStateProperties(stateKey, largeProps, true);

	// Verify properties were stored
	auto retrievedProps = dataManager->getStateProperties(stateKey);
	EXPECT_EQ(1UL, retrievedProps.size());
	EXPECT_EQ(5000UL, std::get<std::string>(retrievedProps["large_data"].getVariant()).size());
}

TEST_F(DataManagerTest, StateChainBehavior) {
	// Create a state
	auto state = createTestState("chain.test");
	dataManager->addStateEntity(state);
	int64_t originalId = state.getId();
	EXPECT_NE(0, originalId);
	EXPECT_EQ(0, state.getPrevStateId()) << "First state should have no prev state";

	// Add properties to create a chain
	// The StateManager may or may not create a new state depending on implementation
	dataManager->addStateProperties(state.getKey(), {{"key", PropertyValue("value")}});

	// Get the current state
	auto currentState = dataManager->findStateByKey(state.getKey());
	EXPECT_NE(0, currentState.getId()) << "Should find state by key";

	// Verify properties were added
	auto props = dataManager->getStateProperties(state.getKey());
	EXPECT_EQ(1UL, props.size());
	EXPECT_EQ("value", std::get<std::string>(props["key"].getVariant()));
}

TEST_F(DataManagerTest, GetEntitiesInRangeWithDeletedEntities) {
	// Test lines 590, 608: getNodesInRange should skip deleted entities
	std::vector<int64_t> nodeIds;

	// Create 10 nodes
	for (int i = 0; i < 10; i++) {
		auto node = createTestNode(dataManager, "RangeNode" + std::to_string(i));
		dataManager->addNode(node);
		nodeIds.push_back(node.getId());
	}

	simulateSave(); // Save to disk

	// Delete some nodes
	auto node2 = dataManager->getNode(nodeIds[2]);
	auto node5 = dataManager->getNode(nodeIds[5]);
	auto node8 = dataManager->getNode(nodeIds[8]);
	dataManager->deleteNode(node2);
	dataManager->deleteNode(node5);
	dataManager->deleteNode(node8);

	// Request range that includes deleted nodes
	auto nodes = dataManager->getNodesInRange(nodeIds.front(), nodeIds.back(), 20);

	// Should return 7 nodes (excluding 3 deleted ones)
	EXPECT_EQ(7UL, nodes.size()) << "Should exclude deleted entities from range";

	// Verify deleted nodes are not in result
	for (const auto& node : nodes) {
		EXPECT_NE(nodeIds[2], node.getId()) << "Deleted node 2 should not be in result";
		EXPECT_NE(nodeIds[5], node.getId()) << "Deleted node 5 should not be in result";
		EXPECT_NE(nodeIds[8], node.getId()) << "Deleted node 8 should not be in result";
	}
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdgeFromDirtyLayer) {
	// Covers getEntitiesInRange<Edge> with dirty entities (Pass 1 dirtyInfo branch)
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	// Create edges (they will be in ADDED dirty state)
	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 5; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "DirtyEdge" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	// Query using getEntitiesInRange<Edge> directly - entities are in dirty layer
	auto edges = dataManager->getEntitiesInRange<Edge>(edgeIds.front(), edgeIds.back(), 100);
	EXPECT_EQ(5UL, edges.size()) << "Should find all edges from dirty layer";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdgeInvalidRange) {
	// Covers getEntitiesInRange<Edge> early return for invalid range
	auto edges = dataManager->getEntitiesInRange<Edge>(100, 50, 10);
	EXPECT_TRUE(edges.empty()) << "startId > endId should return empty";

	auto edges2 = dataManager->getEntitiesInRange<Edge>(1, 100, 0);
	EXPECT_TRUE(edges2.empty()) << "limit == 0 should return empty";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdgeFromDisk) {
	// Covers getEntitiesInRange<Edge> Pass 2 from disk
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 5; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "DiskEdge" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	// Flush to disk and clear cache to force disk read
	fileStorage->flush();
	dataManager->clearCache();

	auto edges = dataManager->getEntitiesInRange<Edge>(edgeIds.front(), edgeIds.back(), 100);
	EXPECT_EQ(5UL, edges.size()) << "Should find all edges from disk";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdgeLimitInPass1) {
	// Covers result.size() >= limit break in Pass 1 for Edge template
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	// Create 10 edges in dirty state
	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 10; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "LimitEdge" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	// Request with limit of 3 - should hit limit in Pass 1 (dirty layer)
	auto edges = dataManager->getEntitiesInRange<Edge>(edgeIds.front(), edgeIds.back(), 3);
	EXPECT_EQ(3UL, edges.size()) << "Should respect limit from dirty layer";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdgeDeletedInDirty) {
	// Covers CHANGE_DELETED check in getEntitiesInRange<Edge> dirty info branch
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

	// Save to disk, then delete one (creates DELETED dirty entry)
	fileStorage->flush();
	auto edgeToDel = dataManager->getEdge(edgeIds[2]);
	dataManager->deleteEdge(edgeToDel);

	// Query - the deleted edge should be excluded by dirty check
	auto edges = dataManager->getEntitiesInRange<Edge>(edgeIds.front(), edgeIds.back(), 100);
	EXPECT_EQ(4UL, edges.size()) << "Should skip deleted edge in dirty layer";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdgeCacheHit) {
	// Covers cache.contains() True branch for Edge in getEntitiesInRange
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 5; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "CacheEdge" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	// Flush and clear dirty state, but load edges into cache by accessing them
	fileStorage->flush();
	for (auto id : edgeIds) {
		auto e = dataManager->getEdge(id);
		EXPECT_TRUE(e.isActive());
	}

	// Now edges are in cache. Call getEntitiesInRange<Edge> to hit cache path.
	auto edges = dataManager->getEntitiesInRange<Edge>(edgeIds.front(), edgeIds.back(), 100);
	EXPECT_EQ(5UL, edges.size()) << "Should find edges from cache";
}

TEST_F(DataManagerTest, GetEntitiesInRangePropertyInvalidRange) {
	// Covers getEntitiesInRange<Property> early return
	auto props = dataManager->getEntitiesInRange<Property>(100, 50, 10);
	EXPECT_TRUE(props.empty());
}

TEST_F(DataManagerTest, GetEntitiesInRangeBlobInvalidRange) {
	// Covers getEntitiesInRange<Blob> early return
	auto blobs = dataManager->getEntitiesInRange<Blob>(100, 50, 10);
	EXPECT_TRUE(blobs.empty());
}

TEST_F(DataManagerTest, GetEntitiesInRangeIndexInvalidRange) {
	// Covers getEntitiesInRange<Index> early return
	auto indexes = dataManager->getEntitiesInRange<Index>(100, 50, 10);
	EXPECT_TRUE(indexes.empty());
}

TEST_F(DataManagerTest, GetEntitiesInRangeNodeLimitReachedInPass1) {
	// Covers result.size() >= limit early return at L640 for Node template
	// Create many nodes in dirty state
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < 20; i++) {
		auto node = createTestNode(dataManager, "P1Limit" + std::to_string(i));
		dataManager->addNode(node);
		nodeIds.push_back(node.getId());
	}

	// Request with small limit - all in dirty state, should hit limit in Pass 1
	auto nodes = dataManager->getEntitiesInRange<Node>(nodeIds.front(), nodeIds.back(), 3);
	EXPECT_EQ(3UL, nodes.size()) << "Should return exactly 3 from dirty layer";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdgeLimitReachedInPass1) {
	// Covers result.size() >= limit early return at L640 for Edge template
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 20; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "P1Edge" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	// All edges in dirty state, limit of 3
	auto edges = dataManager->getEntitiesInRange<Edge>(edgeIds.front(), edgeIds.back(), 3);
	EXPECT_EQ(3UL, edges.size()) << "Should return exactly 3 from dirty layer";
}

TEST_F(DataManagerTest, GetEntitiesInRangeStateDirtyEntities) {
	// Covers getEntitiesInRange<State> dirty info path (dirtyInfo.has_value() True)
	// State entities are created internally by the system, so query a range that
	// overlaps with system state IDs
	auto state1 = createTestState("range.state.1");
	dataManager->addStateEntity(state1);
	auto state2 = createTestState("range.state.2");
	dataManager->addStateEntity(state2);

	// These are in dirty ADDED state
	auto states = dataManager->getEntitiesInRange<State>(state1.getId(), state2.getId(), 100);
	EXPECT_GE(states.size(), 2UL) << "Should find dirty state entities";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdgeNoSegmentOverlap) {
	// Covers intersectStart > intersectEnd in getEntitiesInRange<Edge>
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "NoOverlapEdge");
	dataManager->addEdge(edge);

	fileStorage->flush();
	dataManager->clearCache();

	// Query a range far away from the actual edge IDs
	auto edges = dataManager->getEntitiesInRange<Edge>(edge.getId() + 10000, edge.getId() + 20000, 100);
	EXPECT_TRUE(edges.empty()) << "Should find nothing when range doesn't overlap";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdgeLimitInDiskPass) {
	// Covers result.size() >= limit break inside entity loop for Edge
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 10; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "DiskLimit" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	// Request only 2 from disk
	auto edges = dataManager->getEntitiesInRange<Edge>(edgeIds.front(), edgeIds.back(), 2);
	EXPECT_EQ(2UL, edges.size()) << "Should stop at limit during disk read";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdgeLimitAfterSegment) {
	// Covers result.size() >= limit break after segment loop for Edge
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 20; i++) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "SegLimit" + std::to_string(i));
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	auto edges = dataManager->getEntitiesInRange<Edge>(edgeIds.front(), edgeIds.back(), 5);
	EXPECT_EQ(5UL, edges.size()) << "Should stop after reaching limit";
}

TEST_F(DataManagerTest, GetEntitiesInRangeStateFromDisk) {
	// Covers getEntitiesInRange<State> disk pass (segments, cache, etc.)
	auto state1 = createTestState("disk.state.1");
	dataManager->addStateEntity(state1);
	auto state2 = createTestState("disk.state.2");
	dataManager->addStateEntity(state2);

	fileStorage->flush();
	dataManager->clearCache();

	// Query from disk
	auto states = dataManager->getEntitiesInRange<State>(state1.getId(), state2.getId(), 100);
	EXPECT_GE(states.size(), 2UL) << "Should find state entities from disk";
}

TEST_F(DataManagerTest, GetEntitiesInRangeNodeStartGreaterThanEnd) {
	// Covers startId > endId True branch at L591 for Node template
	auto nodes = dataManager->getEntitiesInRange<Node>(100, 50, 10);
	EXPECT_TRUE(nodes.empty()) << "startId > endId should return empty for Node";
}

TEST_F(DataManagerTest, GetEntitiesInRangeNodeLimitZero) {
	// Covers limit == 0 True branch at L591:26 for Node template
	// startId <= endId so the first condition is false, second is evaluated
	auto nodes = dataManager->getEntitiesInRange<Node>(1, 100, 0);
	EXPECT_TRUE(nodes.empty()) << "limit == 0 should return empty for Node";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdgeLimitZero) {
	// Covers limit == 0 True branch at L591:26 for Edge template
	auto edges = dataManager->getEntitiesInRange<Edge>(1, 100, 0);
	EXPECT_TRUE(edges.empty()) << "limit == 0 should return empty for Edge";
}

TEST_F(DataManagerTest, GetEntitiesInRangePropertyLimitZero) {
	// Covers limit == 0 True branch at L591:26 for Property template
	auto props = dataManager->getEntitiesInRange<Property>(1, 100, 0);
	EXPECT_TRUE(props.empty()) << "limit == 0 should return empty for Property";
}

TEST_F(DataManagerTest, GetEntitiesInRangeBlobLimitZero) {
	// Covers limit == 0 True branch at L591:26 for Blob template
	auto blobs = dataManager->getEntitiesInRange<Blob>(1, 100, 0);
	EXPECT_TRUE(blobs.empty()) << "limit == 0 should return empty for Blob";
}

TEST_F(DataManagerTest, GetEntitiesInRangeIndexLimitZero) {
	// Covers limit == 0 True branch at L591:26 for Index template
	auto indexes = dataManager->getEntitiesInRange<Index>(1, 100, 0);
	EXPECT_TRUE(indexes.empty()) << "limit == 0 should return empty for Index";
}

TEST_F(DataManagerTest, GetEntitiesInRangeStateLimitZero) {
	// Covers limit == 0 True branch at L591:26 for State template
	auto states = dataManager->getEntitiesInRange<State>(1, 100, 0);
	EXPECT_TRUE(states.empty()) << "limit == 0 should return empty for State";
}

TEST_F(DataManagerTest, GetEntitiesInRangeStateInvalidRange) {
	// Covers getEntitiesInRange<State> early return
	auto states = dataManager->getEntitiesInRange<State>(100, 50, 10);
	EXPECT_TRUE(states.empty()) << "startId > endId should return empty for State";
}

TEST_F(DataManagerTest, GetEntitiesInRangeNodeCacheHitAfterDiskLoad) {
	// Covers cache.contains() True branch for Node in getEntitiesInRange
	// Create nodes and flush to disk
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < 5; i++) {
		auto node = createTestNode(dataManager, "CacheHitNode" + std::to_string(i));
		dataManager->addNode(node);
		nodeIds.push_back(node.getId());
	}

	fileStorage->flush();

	// Load some nodes into cache by accessing them individually
	for (int i = 0; i < 3; i++) {
		auto n = dataManager->getNode(nodeIds[i]);
		EXPECT_TRUE(n.isActive());
	}

	// Now call getEntitiesInRange - should find 3 in cache (Pass 1) and 2 from disk (Pass 2)
	auto nodes = dataManager->getEntitiesInRange<Node>(nodeIds.front(), nodeIds.back(), 100);
	EXPECT_EQ(5UL, nodes.size()) << "Should find all nodes from cache + disk";
}

TEST_F(DataManagerTest, GetEntitiesInRangeNodeDeletedInDirty) {
	// Covers CHANGE_DELETED check in getEntitiesInRange<Node> dirty info branch
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < 5; i++) {
		auto node = createTestNode(dataManager, "DelNode" + std::to_string(i));
		dataManager->addNode(node);
		nodeIds.push_back(node.getId());
	}

	fileStorage->flush();

	// Delete one node
	auto nodeToDelete = dataManager->getNode(nodeIds[2]);
	dataManager->deleteNode(nodeToDelete);

	// Query range using getEntitiesInRange<Node>
	auto nodes = dataManager->getEntitiesInRange<Node>(nodeIds.front(), nodeIds.back(), 100);
	EXPECT_EQ(4UL, nodes.size()) << "Should skip deleted node";
}

TEST_F(DataManagerTest, GetEntitiesInRangeNodeLimitInDiskPass) {
	// Covers result.size() >= limit break inside entity loop for Node in Pass 2
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < 10; i++) {
		auto node = createTestNode(dataManager, "DiskLimitNode" + std::to_string(i));
		dataManager->addNode(node);
		nodeIds.push_back(node.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	// Request only 2 from disk
	auto nodes = dataManager->getEntitiesInRange<Node>(nodeIds.front(), nodeIds.back(), 2);
	EXPECT_EQ(2UL, nodes.size()) << "Should stop at limit during disk read";
}

TEST_F(DataManagerTest, GetEntitiesInRangeNodeLimitAfterSegment) {
	// Covers result.size() >= limit break after segment loop for Node in Pass 2
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < 20; i++) {
		auto node = createTestNode(dataManager, "SegLimitNode" + std::to_string(i));
		dataManager->addNode(node);
		nodeIds.push_back(node.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	auto nodes = dataManager->getEntitiesInRange<Node>(nodeIds.front(), nodeIds.back(), 5);
	EXPECT_EQ(5UL, nodes.size()) << "Should stop after reaching limit";
}

TEST_F(DataManagerTest, GetEntitiesInRangeNodeNoSegmentOverlap) {
	// Covers intersectStart > intersectEnd in getEntitiesInRange<Node>
	auto node = createTestNode(dataManager, "NoOverlapNode");
	dataManager->addNode(node);

	fileStorage->flush();
	dataManager->clearCache();

	// Query a range far away
	auto nodes = dataManager->getEntitiesInRange<Node>(node.getId() + 10000, node.getId() + 20000, 100);
	EXPECT_TRUE(nodes.empty()) << "Should find nothing when range doesn't overlap";
}

TEST_F(DataManagerTest, GetEntitiesInRangePropertyFullPass) {
	// Covers getEntitiesInRange<Property> Pass 1 dirty, cache, and Pass 2 disk branches
	// (L608-L683 for Property template)

	// Create a node first to attach properties to
	auto node = createTestNode(dataManager, "PropHost");
	dataManager->addNode(node);

	// Create properties and flush to disk
	std::vector<int64_t> propIds;
	for (int i = 0; i < 5; i++) {
		std::unordered_map<std::string, PropertyValue> props = {{"key" + std::to_string(i), PropertyValue(i)}};
		auto prop = createTestProperty(node.getId(), Node::typeId, props);
		dataManager->addPropertyEntity(prop);
		propIds.push_back(prop.getId());
	}

	fileStorage->flush();

	// Load some into cache
	for (int i = 0; i < 2; i++) {
		auto p = dataManager->getProperty(propIds[i]);
		EXPECT_TRUE(p.isActive());
	}

	// Modify one to put it in dirty layer
	auto dirtyProp = dataManager->getProperty(propIds[2]);
	std::unordered_map<std::string, PropertyValue> newProps = {{"updated", PropertyValue("yes")}};
	dirtyProp.setProperties(newProps);
	dataManager->updatePropertyEntity(dirtyProp);

	// Query range - should cover dirty, cache, and disk paths
	auto results = dataManager->getEntitiesInRange<Property>(propIds.front(), propIds.back(), 100);
	EXPECT_GE(results.size(), 3UL) << "Should find properties from dirty + cache + disk";
}

TEST_F(DataManagerTest, GetEntitiesInRangeBlobFullPass) {
	std::vector<int64_t> blobIds;
	for (int i = 0; i < 5; i++) {
		auto blob = createTestBlob("blobdata" + std::to_string(i));
		dataManager->addBlobEntity(blob);
		blobIds.push_back(blob.getId());
	}

	fileStorage->flush();

	// Load some into cache
	auto b = dataManager->getBlob(blobIds[0]);
	EXPECT_TRUE(b.isActive());

	// Query range
	auto blobs = dataManager->getEntitiesInRange<Blob>(blobIds.front(), blobIds.back(), 100);
	EXPECT_GE(blobs.size(), 1UL) << "Should find blobs from cache + disk";
}

TEST_F(DataManagerTest, GetEntitiesInRangeIndexFullPass) {
	std::vector<int64_t> indexIds;
	for (int i = 0; i < 5; i++) {
		auto idx = createTestIndex(Index::NodeType::LEAF, static_cast<uint32_t>(i + 1));
		dataManager->addIndexEntity(idx);
		indexIds.push_back(idx.getId());
	}

	fileStorage->flush();

	// Load some into cache
	auto ix = dataManager->getIndex(indexIds[0]);
	EXPECT_TRUE(ix.isActive());

	// Query range
	auto indexes = dataManager->getEntitiesInRange<Index>(indexIds.front(), indexIds.back(), 100);
	EXPECT_GE(indexes.size(), 1UL) << "Should find indexes from cache + disk";
}

TEST_F(DataManagerTest, GetEntitiesInRangeStateFullPass) {
	std::vector<int64_t> stateIds;
	for (int i = 0; i < 5; i++) {
		auto state = createTestState("state.key." + std::to_string(i));
		dataManager->addStateEntity(state);
		stateIds.push_back(state.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	// Query range - forces disk reads
	auto states = dataManager->getEntitiesInRange<State>(stateIds.front(), stateIds.back(), 100);
	EXPECT_GE(states.size(), 1UL) << "Should find states from disk";
}

TEST_F(DataManagerTest, GetEntitiesInRangePropertyLimitInDiskPass) {
	std::vector<int64_t> propIds;
	for (int i = 0; i < 10; i++) {
		auto prop = createTestProperty(static_cast<int64_t>(i + 1), 0, {{"k", PropertyValue(i)}});
		dataManager->addPropertyEntity(prop);
		propIds.push_back(prop.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	auto props = dataManager->getEntitiesInRange<Property>(propIds.front(), propIds.back(), 2);
	EXPECT_EQ(2UL, props.size()) << "Should stop at limit during disk read for Property";
}

TEST_F(DataManagerTest, GetEntitiesInRangeBlobLimitInDiskPass) {
	std::vector<int64_t> blobIds;
	for (int i = 0; i < 10; i++) {
		auto blob = createTestBlob("limitblob" + std::to_string(i));
		dataManager->addBlobEntity(blob);
		blobIds.push_back(blob.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	auto blobs = dataManager->getEntitiesInRange<Blob>(blobIds.front(), blobIds.back(), 2);
	EXPECT_EQ(2UL, blobs.size()) << "Should stop at limit during disk read for Blob";
}

TEST_F(DataManagerTest, GetEntitiesInRangeIndexLimitInDiskPass) {
	std::vector<int64_t> indexIds;
	for (int i = 0; i < 10; i++) {
		auto idx = createTestIndex(Index::NodeType::LEAF, static_cast<uint32_t>(i + 1));
		dataManager->addIndexEntity(idx);
		indexIds.push_back(idx.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	auto indexes = dataManager->getEntitiesInRange<Index>(indexIds.front(), indexIds.back(), 2);
	EXPECT_EQ(2UL, indexes.size()) << "Should stop at limit during disk read for Index";
}

TEST_F(DataManagerTest, GetEntitiesInRangePropertyDeletedInDirty) {
	std::vector<int64_t> propIds;
	for (int i = 0; i < 5; i++) {
		auto prop = createTestProperty(static_cast<int64_t>(i + 1), 0, {{"k", PropertyValue(i)}});
		dataManager->addPropertyEntity(prop);
		propIds.push_back(prop.getId());
	}

	fileStorage->flush();

	auto toDelete = dataManager->getProperty(propIds[2]);
	dataManager->deleteProperty(toDelete);

	auto props = dataManager->getEntitiesInRange<Property>(propIds.front(), propIds.back(), 100);
	EXPECT_EQ(4UL, props.size()) << "Should skip deleted property in range query";
}

TEST_F(DataManagerTest, GetEntitiesInRangePropertyNoSegmentOverlap) {
	auto prop = createTestProperty(1, 0, {{"k", PropertyValue(3)}});
	dataManager->addPropertyEntity(prop);
	fileStorage->flush();
	dataManager->clearCache();

	auto props = dataManager->getEntitiesInRange<Property>(prop.getId() + 10000, prop.getId() + 20000, 100);
	EXPECT_TRUE(props.empty()) << "Should find nothing when range doesn't overlap for Property";
}

TEST_F(DataManagerTest, GetEntitiesInRangeBlobNoSegmentOverlap) {
	auto blob = createTestBlob("overlapblob");
	dataManager->addBlobEntity(blob);
	fileStorage->flush();
	dataManager->clearCache();

	auto blobs = dataManager->getEntitiesInRange<Blob>(blob.getId() + 10000, blob.getId() + 20000, 100);
	EXPECT_TRUE(blobs.empty()) << "Should find nothing when range doesn't overlap for Blob";
}

TEST_F(DataManagerTest, GetEntitiesInRangeIndexNoSegmentOverlap) {
	auto idx = createTestIndex(Index::NodeType::LEAF, 1);
	dataManager->addIndexEntity(idx);
	fileStorage->flush();
	dataManager->clearCache();

	auto indexes = dataManager->getEntitiesInRange<Index>(idx.getId() + 10000, idx.getId() + 20000, 100);
	EXPECT_TRUE(indexes.empty()) << "Should find nothing when range doesn't overlap for Index";
}

TEST_F(DataManagerTest, GetEntitiesInRangePropertyCacheHitSkipInPass2) {
	std::vector<int64_t> propIds;
	for (int i = 0; i < 5; i++) {
		auto prop = createTestProperty(static_cast<int64_t>(i + 1), 0, {{"k", PropertyValue(i)}});
		dataManager->addPropertyEntity(prop);
		propIds.push_back(prop.getId());
	}

	fileStorage->flush();

	for (auto id : propIds) {
		auto p = dataManager->getProperty(id);
		EXPECT_TRUE(p.isActive());
	}

	auto props = dataManager->getEntitiesInRange<Property>(propIds.front(), propIds.back(), 100);
	EXPECT_EQ(5UL, props.size()) << "Should find all properties from cache";
}

TEST_F(DataManagerTest, GetEntitiesInRangePropertyLimitInPass1) {
	std::vector<int64_t> propIds;
	for (int i = 0; i < 10; i++) {
		auto prop = createTestProperty(static_cast<int64_t>(i + 1), 0, {{"k", PropertyValue(i)}});
		dataManager->addPropertyEntity(prop);
		propIds.push_back(prop.getId());
	}

	auto props = dataManager->getEntitiesInRange<Property>(propIds.front(), propIds.back(), 3);
	EXPECT_EQ(3UL, props.size()) << "Should stop at limit in Pass 1 for Property";
}

TEST_F(DataManagerTest, GetEntitiesInRangePropertyCacheInactive) {
	std::vector<int64_t> propIds;
	for (int i = 0; i < 5; i++) {
		auto prop = createTestProperty(static_cast<int64_t>(i + 1), 0, {{"k", PropertyValue(i)}});
		dataManager->addPropertyEntity(prop);
		propIds.push_back(prop.getId());
	}

	fileStorage->flush();

	auto toDelete = dataManager->getProperty(propIds[2]);
	dataManager->deleteProperty(toDelete);
	fileStorage->flush();

	dataManager->getProperty(propIds[2]);

	auto props = dataManager->getEntitiesInRange<Property>(propIds.front(), propIds.back(), 100);
	EXPECT_EQ(4UL, props.size()) << "Should skip inactive entity in cache";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdge_DirtyEntitiesInPass1) {
	// Create nodes for edges
	auto node1 = createTestNode(dataManager, "RangeEdgeNode1");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "RangeEdgeNode2");
	dataManager->addNode(node2);

	// Create edges (they become dirty/ADDED in PersistenceManager)
	auto edge1 = createTestEdge(dataManager, node1.getId(), node2.getId(), "RangeEdge1");
	dataManager->addEdge(edge1);
	auto edge2 = createTestEdge(dataManager, node1.getId(), node2.getId(), "RangeEdge2");
	dataManager->addEdge(edge2);
	auto edge3 = createTestEdge(dataManager, node1.getId(), node2.getId(), "RangeEdge3");
	dataManager->addEdge(edge3);

	// Query range that includes these edges - they should be found via dirty info (Pass 1)
	auto edges = dataManager->getEntitiesInRange<Edge>(edge1.getId(), edge3.getId(), 100);
	EXPECT_GE(edges.size(), 3UL) << "Should find dirty edges in range";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdge_LimitReachedInPass1) {
	auto node1 = createTestNode(dataManager, "LimitNode1");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "LimitNode2");
	dataManager->addNode(node2);

	// Create multiple edges
	std::vector<Edge> createdEdges;
	for (int i = 0; i < 5; ++i) {
		auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "LimitEdge" + std::to_string(i));
		dataManager->addEdge(edge);
		createdEdges.push_back(edge);
	}

	// Query with limit = 2 - should stop after finding 2 dirty entities in pass 1
	auto edges = dataManager->getEntitiesInRange<Edge>(
		createdEdges.front().getId(), createdEdges.back().getId(), 2);
	EXPECT_EQ(edges.size(), 2UL) << "Should respect limit during pass 1";
}

TEST_F(DataManagerTest, GetEntitiesInRangeNode_CachedEntitiesInPass1) {
	// Create nodes
	auto node1 = createTestNode(dataManager, "CacheNode1");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "CacheNode2");
	dataManager->addNode(node2);

	// Save to disk so they're no longer dirty
	fileStorage->flush();

	// Access them to populate cache
	auto retrieved1 = dataManager->getNode(node1.getId());
	auto retrieved2 = dataManager->getNode(node2.getId());
	EXPECT_TRUE(retrieved1.isActive());
	EXPECT_TRUE(retrieved2.isActive());

	// Now query range - they should be found via cache (Pass 1, line 629)
	auto nodes = dataManager->getEntitiesInRange<Node>(node1.getId(), node2.getId(), 100);
	EXPECT_GE(nodes.size(), 2UL) << "Should find cached nodes in range";
}

TEST_F(DataManagerTest, GetEntitiesInRangeNode_LimitReachedFromCache) {
	// Create several nodes
	std::vector<Node> createdNodes;
	for (int i = 0; i < 5; ++i) {
		auto node = createTestNode(dataManager, "LimitCacheNode" + std::to_string(i));
		dataManager->addNode(node);
		createdNodes.push_back(node);
	}

	// Save and access to populate cache
	fileStorage->flush();
	for (const auto &n : createdNodes) {
		(void)dataManager->getNode(n.getId());
	}

	// Query with limit = 2 from cache
	auto nodes = dataManager->getEntitiesInRange<Node>(
		createdNodes.front().getId(), createdNodes.back().getId(), 2);
	EXPECT_LE(nodes.size(), 2UL) << "Should respect limit from cache in pass 1";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdge_InvalidRange) {
	auto edges = dataManager->getEntitiesInRange<Edge>(100, 50, 10);
	EXPECT_TRUE(edges.empty()) << "Should return empty for inverted range";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdge_ZeroLimit) {
	auto edges = dataManager->getEntitiesInRange<Edge>(1, 100, 0);
	EXPECT_TRUE(edges.empty()) << "Should return empty for zero limit";
}

TEST_F(DataManagerTest, GetEntitiesInRangeProperty_InvalidRange) {
	auto props = dataManager->getEntitiesInRange<Property>(100, 50, 10);
	EXPECT_TRUE(props.empty()) << "Should return empty for inverted range";
}

TEST_F(DataManagerTest, GetEntitiesInRangeBlob_InvalidRange) {
	auto blobs = dataManager->getEntitiesInRange<Blob>(100, 50, 10);
	EXPECT_TRUE(blobs.empty()) << "Should return empty for inverted range";
}

TEST_F(DataManagerTest, GetEntitiesInRangeIndex_InvalidRange) {
	auto indexes = dataManager->getEntitiesInRange<Index>(100, 50, 10);
	EXPECT_TRUE(indexes.empty()) << "Should return empty for inverted range";
}

TEST_F(DataManagerTest, GetEntitiesInRangeState_InvalidRange) {
	auto states = dataManager->getEntitiesInRange<State>(100, 50, 10);
	EXPECT_TRUE(states.empty()) << "Should return empty for inverted range";
}

TEST_F(DataManagerTest, GetEntitiesInRangeBlobWithData) {
	// Covers getEntitiesInRange<Blob> interior branches:
	// - Line 600-601: for loop iterating over IDs
	// - Line 609: checking PersistenceManager for dirty info
	// - Line 614: checking if dirty entity is not deleted
	// - Line 622: checking cache
	// Create blobs and exercise the range query
	std::vector<int64_t> blobIds;
	for (int i = 0; i < 5; i++) {
		auto blob = createTestBlob("BlobData" + std::to_string(i));
		dataManager->addBlobEntity(blob);
		blobIds.push_back(blob.getId());
	}

	// Query blobs in range - entities should be in dirty layer (Pass 1)
	auto blobs = dataManager->getEntitiesInRange<Blob>(blobIds.front(), blobIds.back(), 100);
	EXPECT_GE(blobs.size(), 5UL)
		<< "Should find at least our 5 blobs from dirty layer";

	// Verify the blobs are active
	for (const auto &b : blobs) {
		EXPECT_TRUE(b.isActive()) << "Blob should be active";
	}
}

TEST_F(DataManagerTest, GetEntitiesInRangeBlobFromDisk) {
	// Covers getEntitiesInRange<Blob> Pass 2 (disk) branches:
	// - Line 644: segment iteration
	// - Line 652-653: overlap check
	// - Line 661-663: dedup check in disk pass
	std::vector<int64_t> blobIds;
	for (int i = 0; i < 5; i++) {
		auto blob = createTestBlob("DiskBlob" + std::to_string(i));
		dataManager->addBlobEntity(blob);
		blobIds.push_back(blob.getId());
	}

	// Flush to disk
	fileStorage->flush();
	dataManager->clearCache();

	// Query blobs from disk
	auto blobs = dataManager->getEntitiesInRange<Blob>(blobIds.front(), blobIds.back(), 100);
	EXPECT_GE(blobs.size(), 5UL) << "Should find blobs from disk";
}

TEST_F(DataManagerTest, GetEntitiesInRangeBlobWithLimit) {
	// Covers limit branches in getEntitiesInRange<Blob>
	std::vector<int64_t> blobIds;
	for (int i = 0; i < 10; i++) {
		auto blob = createTestBlob("LimitBlob" + std::to_string(i));
		dataManager->addBlobEntity(blob);
		blobIds.push_back(blob.getId());
	}

	// Request with limit smaller than available count
	auto blobs = dataManager->getEntitiesInRange<Blob>(blobIds.front(), blobIds.back(), 3);
	EXPECT_EQ(3UL, blobs.size()) << "Should respect limit of 3";
}

TEST_F(DataManagerTest, GetEntitiesInRangeIndexWithData) {
	// Covers getEntitiesInRange<Index> interior branches
	std::vector<int64_t> indexIds;
	for (int i = 0; i < 5; i++) {
		auto index = createTestIndex(Index::NodeType::LEAF, i + 1);
		dataManager->addIndexEntity(index);
		indexIds.push_back(index.getId());
	}

	// Query indexes in range from dirty layer
	auto indexes = dataManager->getEntitiesInRange<Index>(indexIds.front(), indexIds.back(), 100);
	EXPECT_GE(indexes.size(), 5UL)
		<< "Should find at least our 5 indexes from dirty layer";
}

TEST_F(DataManagerTest, GetEntitiesInRangeIndexFromDisk) {
	// Covers getEntitiesInRange<Index> Pass 2 (disk) branches
	std::vector<int64_t> indexIds;
	for (int i = 0; i < 5; i++) {
		auto index = createTestIndex(Index::NodeType::LEAF, i + 1);
		dataManager->addIndexEntity(index);
		indexIds.push_back(index.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	auto indexes = dataManager->getEntitiesInRange<Index>(indexIds.front(), indexIds.back(), 100);
	EXPECT_GE(indexes.size(), 5UL) << "Should find indexes from disk";
}

TEST_F(DataManagerTest, GetEntitiesInRangeIndexWithLimit) {
	// Covers limit branches in getEntitiesInRange<Index>
	std::vector<int64_t> indexIds;
	for (int i = 0; i < 10; i++) {
		auto index = createTestIndex(Index::NodeType::LEAF, i + 1);
		dataManager->addIndexEntity(index);
		indexIds.push_back(index.getId());
	}

	auto indexes = dataManager->getEntitiesInRange<Index>(indexIds.front(), indexIds.back(), 3);
	EXPECT_EQ(3UL, indexes.size()) << "Should respect limit of 3";
}

TEST_F(DataManagerTest, GetEntitiesInRangeStateWithData) {
	// Covers getEntitiesInRange<State> interior branches
	std::vector<int64_t> stateIds;
	for (int i = 0; i < 5; i++) {
		auto state = createTestState("range_state_" + std::to_string(i));
		dataManager->addStateEntity(state);
		stateIds.push_back(state.getId());
	}

	// Query states in range from dirty layer
	auto states = dataManager->getEntitiesInRange<State>(stateIds.front(), stateIds.back(), 100);
	EXPECT_GE(states.size(), 5UL)
		<< "Should find at least our 5 states from dirty layer";
}

TEST_F(DataManagerTest, GetEntitiesInRangeStateFromDiskPass2) {
	// Covers getEntitiesInRange<State> Pass 2 (disk) branches
	std::vector<int64_t> stateIds;
	for (int i = 0; i < 5; i++) {
		auto state = createTestState("disk_state2_" + std::to_string(i));
		dataManager->addStateEntity(state);
		stateIds.push_back(state.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	auto states = dataManager->getEntitiesInRange<State>(stateIds.front(), stateIds.back(), 100);
	EXPECT_GE(states.size(), 5UL) << "Should find states from disk";
}

TEST_F(DataManagerTest, GetEntitiesInRangeStateWithLimit) {
	// Covers limit branches in getEntitiesInRange<State>
	std::vector<int64_t> stateIds;
	for (int i = 0; i < 10; i++) {
		auto state = createTestState("limit_state_" + std::to_string(i));
		dataManager->addStateEntity(state);
		stateIds.push_back(state.getId());
	}

	auto states = dataManager->getEntitiesInRange<State>(stateIds.front(), stateIds.back(), 3);
	EXPECT_EQ(3UL, states.size()) << "Should respect limit of 3";
}

TEST_F(DataManagerTest, GetEntitiesInRangeBlobWithDeletedEntities) {
	// Create blobs, then delete some - exercises the CHANGE_DELETED branch
	std::vector<int64_t> blobIds;
	std::vector<Blob> blobs;
	for (int i = 0; i < 5; i++) {
		auto blob = createTestBlob("DelBlob" + std::to_string(i));
		dataManager->addBlobEntity(blob);
		blobIds.push_back(blob.getId());
		blobs.push_back(blob);
	}

	// Delete blob at index 1 and 3
	dataManager->deleteBlob(blobs[1]);
	dataManager->deleteBlob(blobs[3]);

	// Query range - should skip deleted blobs
	auto result = dataManager->getEntitiesInRange<Blob>(blobIds.front(), blobIds.back(), 100);
	// Should return fewer than 5 (the 2 deleted ones should be excluded)
	// But some system blobs may also be in range, so just verify deleted ones are absent
	for (const auto &b : result) {
		EXPECT_NE(blobIds[1], b.getId()) << "Deleted blob 1 should be excluded";
		EXPECT_NE(blobIds[3], b.getId()) << "Deleted blob 3 should be excluded";
	}
}

TEST_F(DataManagerTest, GetEntitiesInRangeIndexWithDeletedEntities) {
	// Covers CHANGE_DELETED skip branch for Index template
	std::vector<int64_t> indexIds;
	std::vector<Index> indexes;
	for (int i = 0; i < 5; i++) {
		auto index = createTestIndex(Index::NodeType::LEAF, i + 1);
		dataManager->addIndexEntity(index);
		indexIds.push_back(index.getId());
		indexes.push_back(index);
	}

	// Delete some indexes
	dataManager->deleteIndex(indexes[0]);
	dataManager->deleteIndex(indexes[4]);

	auto result = dataManager->getEntitiesInRange<Index>(indexIds.front(), indexIds.back(), 100);
	for (const auto &idx : result) {
		EXPECT_NE(indexIds[0], idx.getId()) << "Deleted index 0 should be excluded";
		EXPECT_NE(indexIds[4], idx.getId()) << "Deleted index 4 should be excluded";
	}
}

TEST_F(DataManagerTest, GetEntitiesInRangeStateWithDeletedEntities) {
	// Covers CHANGE_DELETED skip branch for State template
	std::vector<int64_t> stateIds;
	std::vector<State> states;
	for (int i = 0; i < 5; i++) {
		auto state = createTestState("del_state_" + std::to_string(i));
		dataManager->addStateEntity(state);
		stateIds.push_back(state.getId());
		states.push_back(state);
	}

	// Delete some states
	dataManager->deleteState(states[2]);

	auto result = dataManager->getEntitiesInRange<State>(stateIds.front(), stateIds.back(), 100);
	for (const auto &s : result) {
		EXPECT_NE(stateIds[2], s.getId()) << "Deleted state 2 should be excluded";
	}
}

TEST_F(DataManagerTest, GetEntitiesInRangeBlobFromCache) {
	// Covers cache check in Pass 1 for Blob template
	std::vector<int64_t> blobIds;
	for (int i = 0; i < 5; i++) {
		auto blob = createTestBlob("CacheBlob" + std::to_string(i));
		dataManager->addBlobEntity(blob);
		blobIds.push_back(blob.getId());
	}

	// Flush to disk and clear dirty state so entities are only on disk
	fileStorage->flush();

	// Load blobs into cache by accessing them individually
	for (auto id : blobIds) {
		(void)dataManager->getBlob(id);
	}

	// Now call getEntitiesInRange - should find them in cache (Pass 1)
	auto blobs = dataManager->getEntitiesInRange<Blob>(blobIds.front(), blobIds.back(), 100);
	EXPECT_GE(blobs.size(), 5UL) << "Should find blobs from cache";
}

TEST_F(DataManagerTest, GetEntitiesInRangeIndexFromCache) {
	// Covers cache check in Pass 1 for Index template
	std::vector<int64_t> indexIds;
	for (int i = 0; i < 5; i++) {
		auto index = createTestIndex(Index::NodeType::LEAF, i + 1);
		dataManager->addIndexEntity(index);
		indexIds.push_back(index.getId());
	}

	fileStorage->flush();

	// Load indexes into cache
	for (auto id : indexIds) {
		(void)dataManager->getIndex(id);
	}

	auto indexes = dataManager->getEntitiesInRange<Index>(indexIds.front(), indexIds.back(), 100);
	EXPECT_GE(indexes.size(), 5UL) << "Should find indexes from cache";
}

TEST_F(DataManagerTest, GetEntitiesInRangeStateFromCache) {
	// Covers cache check in Pass 1 for State template
	std::vector<int64_t> stateIds;
	for (int i = 0; i < 5; i++) {
		auto state = createTestState("cache_state_" + std::to_string(i));
		dataManager->addStateEntity(state);
		stateIds.push_back(state.getId());
	}

	fileStorage->flush();

	// Load states into cache
	for (auto id : stateIds) {
		(void)dataManager->getState(id);
	}

	auto states = dataManager->getEntitiesInRange<State>(stateIds.front(), stateIds.back(), 100);
	EXPECT_GE(states.size(), 5UL) << "Should find states from cache";
}

TEST_F(DataManagerTest, MarkPropertyEntityDeletedWhenModified) {
	// Covers markEntityDeleted<Property> else branch (not ADDED)
	// Create a node with properties to get a Property entity
	auto node = createTestNode(dataManager, "PropNode");
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {
		{"key1", PropertyValue(std::string("val1"))},
		{"key2", PropertyValue(std::string("val2"))}
	});

	// Flush to clear ADDED state
	fileStorage->flush();

	// Get the property entity linked to the node
	auto retrievedNode = dataManager->getNode(node.getId());
	int64_t propId = retrievedNode.getPropertyEntityId();
	if (propId != 0) {
		auto prop = dataManager->getProperty(propId);
		EXPECT_TRUE(prop.isActive());

		// Now update the property entity (puts it in MODIFIED state)
		dataManager->updatePropertyEntity(prop);

		// Delete it - should hit the else branch (not ADDED)
		dataManager->deleteProperty(prop);

		auto retrieved = dataManager->getProperty(propId);
		EXPECT_FALSE(retrieved.isActive())
			<< "Property should be inactive after deletion";
	}
}

TEST_F(DataManagerTest, MarkBlobEntityDeletedWhenModified) {
	// Covers markEntityDeleted<Blob> else branch (entity in MODIFIED state)
	auto blob = createTestBlob("ModifyThenDeleteBlob");
	dataManager->addBlobEntity(blob);
	int64_t blobId = blob.getId();

	// Flush to clear ADDED state
	fileStorage->flush();

	// Update the blob (puts it in MODIFIED state)
	auto loadedBlob = dataManager->getBlob(blobId);
	loadedBlob.setData("UpdatedData");
	dataManager->updateBlobEntity(loadedBlob);

	// Delete it - should hit the else branch
	dataManager->deleteBlob(loadedBlob);

	auto retrieved = dataManager->getBlob(blobId);
	EXPECT_FALSE(retrieved.isActive())
		<< "Blob should be inactive after deletion";
}

TEST_F(DataManagerTest, MarkIndexEntityDeletedWhenModified) {
	// Covers markEntityDeleted<Index> else branch (entity in MODIFIED state)
	auto index = createTestIndex(Index::NodeType::LEAF, 1);
	dataManager->addIndexEntity(index);
	int64_t indexId = index.getId();

	// Flush to clear ADDED state
	fileStorage->flush();

	// Update the index (puts it in MODIFIED state)
	auto loadedIndex = dataManager->getIndex(indexId);
	dataManager->updateIndexEntity(loadedIndex);

	// Delete it - should hit the else branch
	dataManager->deleteIndex(loadedIndex);

	auto retrieved = dataManager->getIndex(indexId);
	EXPECT_FALSE(retrieved.isActive())
		<< "Index should be inactive after deletion";
}

TEST_F(DataManagerTest, GetEntitiesInRangePropertyFromDisk) {
	auto node = createTestNode(dataManager, "PropRangeNode");
	dataManager->addNode(node);

	// Create property entities directly
	std::vector<int64_t> propIds;
	for (int i = 0; i < 3; i++) {
		auto property = createTestProperty(node.getId(), Node::typeId,
			{{"key" + std::to_string(i), PropertyValue(std::string("val" + std::to_string(i)))}});
		dataManager->addPropertyEntity(property);
		propIds.push_back(property.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	// Query property range from disk
	auto props = dataManager->getEntitiesInRange<Property>(propIds.front(), propIds.back(), 100);
	EXPECT_GE(props.size(), 3UL) << "Should find property entities from disk";
}

TEST_F(DataManagerTest, GetEntitiesInRangePropertyWithDeleted) {
	auto node = createTestNode(dataManager, "PropDelNode");
	dataManager->addNode(node);

	std::vector<int64_t> propIds;
	std::vector<Property> properties;
	for (int i = 0; i < 3; i++) {
		auto property = createTestProperty(node.getId(), Node::typeId,
			{{"key" + std::to_string(i), PropertyValue(std::string("val"))}});
		dataManager->addPropertyEntity(property);
		propIds.push_back(property.getId());
		properties.push_back(property);
	}

	// Delete one property
	dataManager->deleteProperty(properties[1]);

	auto result = dataManager->getEntitiesInRange<Property>(propIds.front(), propIds.back(), 100);
	for (const auto &p : result) {
		EXPECT_NE(propIds[1], p.getId()) << "Deleted property should be excluded";
	}
}

TEST_F(DataManagerTest, GetEntitiesInRangePropertyFromCache) {
	auto node = createTestNode(dataManager, "PropCacheNode");
	dataManager->addNode(node);

	std::vector<int64_t> propIds;
	for (int i = 0; i < 3; i++) {
		auto property = createTestProperty(node.getId(), Node::typeId,
			{{"k" + std::to_string(i), PropertyValue(std::string("v"))}});
		dataManager->addPropertyEntity(property);
		propIds.push_back(property.getId());
	}

	fileStorage->flush();

	// Load into cache
	for (auto id : propIds) {
		(void)dataManager->getProperty(id);
	}

	auto result = dataManager->getEntitiesInRange<Property>(propIds.front(), propIds.back(), 100);
	EXPECT_GE(result.size(), 3UL) << "Should find property entities from cache";
}

TEST_F(DataManagerTest, MarkStateEntityDeletedWhenJustAdded) {
	auto state = createTestState("just_added_state");
	dataManager->addStateEntity(state);
	int64_t stateId = state.getId();

	auto dirtyInfo = dataManager->getDirtyInfo<State>(stateId);
	ASSERT_TRUE(dirtyInfo.has_value());
	EXPECT_EQ(EntityChangeType::CHANGE_ADDED, dirtyInfo->changeType);

	dataManager->deleteState(state);

	auto afterDelete = dataManager->getDirtyInfo<State>(stateId);
	EXPECT_FALSE(afterDelete.has_value())
		<< "ADDED state should be removed from registry after deletion";
}

TEST_F(DataManagerTest, MarkStateEntityDeletedWhenPersisted) {
	auto state = createTestState("persisted_state");
	dataManager->addStateEntity(state);
	int64_t stateId = state.getId();

	fileStorage->flush();

	dataManager->deleteState(state);

	auto dirtyInfo = dataManager->getDirtyInfo<State>(stateId);
	ASSERT_TRUE(dirtyInfo.has_value());
	EXPECT_EQ(EntityChangeType::CHANGE_DELETED, dirtyInfo->changeType);
}

TEST_F(DataManagerTest, GetEntitiesInRangeNodeWithSmallLimitFromDisk) {
	// Create multiple nodes and flush to disk
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < 10; i++) {
		auto node = createTestNode(dataManager, "LimitTestNode");
		dataManager->addNode(node);
		nodeIds.push_back(node.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	// Query with small limit=2 so we hit the limit during disk pass
	auto result = dataManager->getEntitiesInRange<Node>(nodeIds.front(), nodeIds.back(), 2);
	EXPECT_EQ(2UL, result.size())
		<< "Should return exactly 2 nodes due to limit";
}

TEST_F(DataManagerTest, GetEntitiesInRangeEdgeWithLimitOneDisk) {
	auto n1 = createTestNode(dataManager, "LimSrc");
	dataManager->addNode(n1);
	auto n2 = createTestNode(dataManager, "LimTgt");
	dataManager->addNode(n2);

	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 5; i++) {
		auto edge = createTestEdge(dataManager, n1.getId(), n2.getId(), "LimitEdge");
		dataManager->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	auto result = dataManager->getEntitiesInRange<Edge>(edgeIds.front(), edgeIds.back(), 1);
	EXPECT_EQ(1UL, result.size())
		<< "Should return exactly 1 edge due to limit";
}

TEST_F(DataManagerTest, GetEntitiesInRangeNarrowRangeMissesEntities) {
	// Create nodes to get allocated IDs
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < 5; i++) {
		auto node = createTestNode(dataManager, "NarrowNode");
		dataManager->addNode(node);
		nodeIds.push_back(node.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	// Query with a range that starts AFTER all existing entities
	int64_t maxId = nodeIds.back();
	auto result = dataManager->getEntitiesInRange<Node>(maxId + 100, maxId + 200, 100);
	EXPECT_TRUE(result.empty()) << "Should return empty for range beyond all entities";
}

TEST_F(DataManagerTest, GetEntitiesInRangeBlobWithLimitFromDisk) {
	std::vector<int64_t> blobIds;
	for (int i = 0; i < 5; i++) {
		auto blob = createTestBlob("blob_data_" + std::to_string(i));
		dataManager->addBlobEntity(blob);
		blobIds.push_back(blob.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	auto result = dataManager->getEntitiesInRange<Blob>(blobIds.front(), blobIds.back(), 2);
	EXPECT_EQ(2UL, result.size()) << "Should return exactly 2 blobs due to limit";
}

TEST_F(DataManagerTest, GetEntitiesInRangeIndexWithLimitFromDisk) {
	std::vector<int64_t> indexIds;
	for (int i = 0; i < 5; i++) {
		auto idx = createTestIndex(Index::NodeType::LEAF, static_cast<uint32_t>(i + 100));
		dataManager->addIndexEntity(idx);
		indexIds.push_back(idx.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	auto result = dataManager->getEntitiesInRange<Index>(indexIds.front(), indexIds.back(), 2);
	EXPECT_EQ(2UL, result.size()) << "Should return exactly 2 indexes due to limit";
}

TEST_F(DataManagerTest, GetEntitiesInRangeStateWithLimitFromDisk) {
	std::vector<int64_t> stateIds;
	for (int i = 0; i < 5; i++) {
		auto state = createTestState("limit_state_" + std::to_string(i));
		dataManager->addStateEntity(state);
		stateIds.push_back(state.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	auto result = dataManager->getEntitiesInRange<State>(stateIds.front(), stateIds.back(), 2);
	EXPECT_EQ(2UL, result.size()) << "Should return exactly 2 states due to limit";
}

TEST_F(DataManagerTest, GetEntitiesInRangeMultiSegmentNonOverlap) {
	// Create enough nodes to fill at least 2 segments
	const int nodeCount = static_cast<int>(NODES_PER_SEGMENT) + 10;
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < nodeCount; i++) {
		auto node = createTestNode(dataManager, "MultiSeg");
		dataManager->addNode(node);
		nodeIds.push_back(node.getId());
	}

	fileStorage->flush();
	dataManager->clearCache();

	// Query only the last few IDs, which live in the second segment.
	// The first segment should NOT overlap with this range,
	// triggering the intersectStart > intersectEnd continue on line 660.
	int64_t queryStart = nodeIds[nodeCount - 4]; // 4th from the end
	int64_t queryEnd = nodeIds[nodeCount - 1];   // last node
	auto result = dataManager->getEntitiesInRange<Node>(queryStart, queryEnd, 100);
	EXPECT_GE(result.size(), 1UL) << "Should find nodes in later segment";
	for (const auto &n : result) {
		EXPECT_GE(n.getId(), queryStart);
		EXPECT_LE(n.getId(), queryEnd);
	}
}

TEST_F(DataManagerTest, GetEntitiesInRangeWithLimitZero) {
	auto node = createTestNode(dataManager, "ZeroLimit");
	dataManager->addNode(node);
	fileStorage->flush();

	auto result = dataManager->getEntitiesInRange<Node>(1, 100, 0);
	EXPECT_TRUE(result.empty()) << "Limit 0 should return empty result";
}

TEST_F(DataManagerTest, GetEntitiesInRangeInvertedRange) {
	auto node = createTestNode(dataManager, "InvertedRange");
	dataManager->addNode(node);
	fileStorage->flush();

	auto result = dataManager->getEntitiesInRange<Node>(100, 1, 100);
	EXPECT_TRUE(result.empty()) << "Inverted range should return empty result";
}

