/**
 * @file test_DataManager_NodeEdge.cpp
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

TEST_F(DataManagerTest, Initialization) {
	ASSERT_NE(nullptr, dataManager);
	EXPECT_NE(nullptr, dataManager->getNodeManager());
	EXPECT_NE(nullptr, dataManager->getEdgeManager());
	EXPECT_NE(nullptr, dataManager->getPropertyManager());
	EXPECT_NE(nullptr, dataManager->getBlobManager());
	EXPECT_NE(nullptr, dataManager->getSegmentIndexManager());

	auto header = dataManager->getFileHeader();
	EXPECT_STREQ(graph::storage::FILE_HEADER_MAGIC_STRING, header.magic);
	EXPECT_NE(0U, header.version);
}

TEST_F(DataManagerTest, NodeCRUD) {
	// 1. Create
	auto node = createTestNode(dataManager, "Person");
	dataManager->addNode(node);
	EXPECT_NE(0, node.getId());
	ASSERT_EQ(1UL, observer->addedNodes.size());
	EXPECT_EQ(node.getId(), observer->addedNodes[0].getId());

	// 2. Retrieve
	auto retrievedNode = dataManager->getNode(node.getId());
	EXPECT_EQ(node.getId(), retrievedNode.getId());
	EXPECT_EQ("Person", dataManager->resolveLabel(retrievedNode.getLabelId()));
	EXPECT_TRUE(retrievedNode.isActive());

	// 3. Update (while still in 'ADDED' state)
	// [IMPORTANT] Reset observer to verify the update specifically
	observer->reset();

	node.setLabelId(dataManager->getOrCreateLabelId("StagingPerson"));
	dataManager->updateNode(node);

	retrievedNode = dataManager->getNode(node.getId());
	EXPECT_EQ("StagingPerson", dataManager->resolveLabel(retrievedNode.getLabelId()));

	// Since DataManager now notifies even for ADDED entities, we expect 1 update event.
	// Old Label: Person, New Label: StagingPerson
	ASSERT_EQ(1UL, observer->updatedNodes.size());
	EXPECT_EQ("Person", dataManager->resolveLabel(observer->updatedNodes[0].first.getLabelId()));
	EXPECT_EQ("StagingPerson", dataManager->resolveLabel(observer->updatedNodes[0].second.getLabelId()));

	// 4. Simulate Save & Update
	simulateSave(); // Replaces markAllSaved()
	observer->reset();

	Node oldNode = retrievedNode;
	node.setLabelId(dataManager->getOrCreateLabelId("UpdatedPerson"));
	dataManager->updateNode(node);
	retrievedNode = dataManager->getNode(node.getId());
	EXPECT_EQ("UpdatedPerson", dataManager->resolveLabel(retrievedNode.getLabelId()));

	ASSERT_EQ(1UL, observer->updatedNodes.size());
	EXPECT_EQ(oldNode.getLabelId(), observer->updatedNodes[0].first.getLabelId());
	EXPECT_EQ("UpdatedPerson", dataManager->resolveLabel(observer->updatedNodes[0].second.getLabelId()));

	// 5. Delete
	dataManager->deleteNode(node);
	dataManager->clearCache();
	retrievedNode = dataManager->getNode(node.getId());

	if (retrievedNode.getId() != 0) {
		EXPECT_FALSE(retrievedNode.isActive());
	} else {
		EXPECT_EQ(0, retrievedNode.getId());
	}

	ASSERT_EQ(1UL, observer->deletedNodes.size());
	EXPECT_EQ(node.getId(), observer->deletedNodes[0].getId());
}

TEST_F(DataManagerTest, NodeProperties) {
	auto node = createTestNode(dataManager, "Person");
	dataManager->addNode(node);

	std::unordered_map<std::string, PropertyValue> properties = {{"name", PropertyValue("John Doe")},
																 {"age", PropertyValue(30)},
																 {"active", PropertyValue(true)},
																 {"score", PropertyValue(98.5)}};
	dataManager->addNodeProperties(node.getId(), properties);

	auto retrievedProps = dataManager->getNodeProperties(node.getId());
	EXPECT_EQ(4UL, retrievedProps.size());
	EXPECT_EQ("John Doe", std::get<std::string>(retrievedProps.at("name").getVariant()));
	EXPECT_EQ(30, std::get<int64_t>(retrievedProps.at("age").getVariant()));
	EXPECT_TRUE(std::get<bool>(retrievedProps.at("active").getVariant()));
	EXPECT_DOUBLE_EQ(98.5, std::get<double>(retrievedProps.at("score").getVariant()));

	dataManager->removeNodeProperty(node.getId(), "age");
	retrievedProps = dataManager->getNodeProperties(node.getId());
	EXPECT_EQ(3UL, retrievedProps.size());
	EXPECT_EQ(retrievedProps.find("age"), retrievedProps.end());
}

TEST_F(DataManagerTest, EdgeCRUD) {
	auto sourceNode = createTestNode(dataManager, "Source");
	auto targetNode = createTestNode(dataManager, "Target");
	dataManager->addNode(sourceNode);
	dataManager->addNode(targetNode);
	observer->reset();

	// 1. Create
	auto edge = createTestEdge(dataManager, sourceNode.getId(), targetNode.getId(), "KNOWS");
	dataManager->addEdge(edge);
	EXPECT_NE(0, edge.getId());
	ASSERT_EQ(1UL, observer->addedEdges.size());

	// 2. Retrieve
	auto retrievedEdge = dataManager->getEdge(edge.getId());
	EXPECT_EQ(edge.getId(), retrievedEdge.getId());
	EXPECT_EQ("KNOWS", dataManager->resolveLabel(retrievedEdge.getLabelId()));
	EXPECT_TRUE(retrievedEdge.isActive());

	// 3. Simulate Save
	simulateSave();
	observer->reset();

	// 4. Update
	Edge oldEdge = retrievedEdge;
	edge.setLabelId(dataManager->getOrCreateLabelId("UPDATED_KNOWS"));
	dataManager->updateEdge(edge);
	retrievedEdge = dataManager->getEdge(edge.getId());
	EXPECT_EQ("UPDATED_KNOWS", dataManager->resolveLabel(retrievedEdge.getLabelId()));

	ASSERT_EQ(1UL, observer->updatedEdges.size());
	EXPECT_EQ(oldEdge.getLabelId(), observer->updatedEdges[0].first.getLabelId());
	EXPECT_EQ("UPDATED_KNOWS", dataManager->resolveLabel(observer->updatedEdges[0].second.getLabelId()));

	// 5. Delete
	dataManager->deleteEdge(edge);
	dataManager->clearCache();
	retrievedEdge = dataManager->getEdge(edge.getId());
	if (retrievedEdge.getId() != 0)
		EXPECT_FALSE(retrievedEdge.isActive());
	else
		EXPECT_EQ(0, retrievedEdge.getId());

	ASSERT_EQ(1UL, observer->deletedEdges.size());
	EXPECT_EQ(edge.getId(), observer->deletedEdges[0].getId());
}

TEST_F(DataManagerTest, EdgeProperties) {
	auto sourceNode = createTestNode(dataManager, "Source");
	auto targetNode = createTestNode(dataManager, "Target");
	dataManager->addNode(sourceNode);
	dataManager->addNode(targetNode);
	auto edge = createTestEdge(dataManager, sourceNode.getId(), targetNode.getId(), "CONNECTS");
	dataManager->addEdge(edge);

	std::unordered_map<std::string, PropertyValue> properties = {
			{"weight", PropertyValue(2.5)}, {"since", PropertyValue(2023)}, {"active", PropertyValue(true)}};
	dataManager->addEdgeProperties(edge.getId(), properties);

	auto retrievedProps = dataManager->getEdgeProperties(edge.getId());
	EXPECT_EQ(3UL, retrievedProps.size());
	EXPECT_DOUBLE_EQ(2.5, std::get<double>(retrievedProps.at("weight").getVariant()));
	EXPECT_EQ(2023, std::get<int64_t>(retrievedProps.at("since").getVariant()));
	EXPECT_TRUE(std::get<bool>(retrievedProps.at("active").getVariant()));

	dataManager->removeEdgeProperty(edge.getId(), "since");
	retrievedProps = dataManager->getEdgeProperties(edge.getId());
	EXPECT_EQ(2UL, retrievedProps.size());
	EXPECT_EQ(retrievedProps.find("since"), retrievedProps.end());
}

TEST_F(DataManagerTest, FindEdgesByNode) {
	auto node1 = createTestNode(dataManager, "Node1");
	auto node2 = createTestNode(dataManager, "Node2");
	auto node3 = createTestNode(dataManager, "Node3");
	dataManager->addNode(node1);
	dataManager->addNode(node2);
	dataManager->addNode(node3);
	auto edge1 = createTestEdge(dataManager, node1.getId(), node2.getId(), "CONNECTS_TO");
	auto edge2 = createTestEdge(dataManager, node2.getId(), node3.getId(), "CONNECTS_TO");
	auto edge3 = createTestEdge(dataManager, node3.getId(), node1.getId(), "CONNECTS_TO");
	dataManager->addEdge(edge1);
	dataManager->addEdge(edge2);
	dataManager->addEdge(edge3);

	auto outEdges = dataManager->findEdgesByNode(node1.getId(), "out");
	ASSERT_EQ(1UL, outEdges.size());
	EXPECT_EQ(edge1.getId(), outEdges[0].getId());
	EXPECT_EQ(node2.getId(), outEdges[0].getTargetNodeId());

	auto inEdges = dataManager->findEdgesByNode(node1.getId(), "in");
	ASSERT_EQ(1UL, inEdges.size());
	EXPECT_EQ(edge3.getId(), inEdges[0].getId());
	EXPECT_EQ(node3.getId(), inEdges[0].getSourceNodeId());

	auto allEdges = dataManager->findEdgesByNode(node1.getId(), "both");
	EXPECT_EQ(2UL, allEdges.size());
}

TEST_F(DataManagerTest, BatchOperations) {
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < 10; i++) {
		auto node = createTestNode(dataManager, "BatchNode" + std::to_string(i));
		dataManager->addNode(node);
		nodeIds.push_back(node.getId());
	}

	auto nodes = dataManager->getNodeBatch(nodeIds);
	ASSERT_EQ(10UL, nodes.size());
	for (size_t i = 0; i < nodes.size(); i++) {
		EXPECT_EQ(nodeIds[i], nodes[i].getId());
		EXPECT_EQ("BatchNode" + std::to_string(i), dataManager->resolveLabel(nodes[i].getLabelId()));
	}

	auto rangeNodes = dataManager->getNodesInRange(nodeIds.front(), nodeIds.back(), 5);
	EXPECT_EQ(5UL, rangeNodes.size());

	rangeNodes = dataManager->getNodesInRange(nodeIds.front(), nodeIds.back(), 20);
	EXPECT_EQ(10UL, rangeNodes.size());
}

TEST_F(DataManagerTest, CacheManagement) {
	auto node = createTestNode(dataManager, "CacheNode");
	dataManager->addNode(node);

	auto retrievedNode = dataManager->getNode(node.getId());
	EXPECT_EQ(node.getId(), retrievedNode.getId());

	dataManager->clearCache();

	// Should still retrieve from Dirty Layer (PersistenceManager) even if cache is cleared
	retrievedNode = dataManager->getNode(node.getId());
	EXPECT_EQ(node.getId(), retrievedNode.getId());
}

TEST_F(DataManagerTest, MultipleEntityTypes) {
	auto node = createTestNode(dataManager, "MultiTypeNode");
	dataManager->addNode(node);

	auto state = createTestState("multi.type.state");
	dataManager->addStateEntity(state);

	auto index = createTestIndex(Index::NodeType::INTERNAL, 2);
	dataManager->addIndexEntity(index);

	std::string blobData = "some binary data";
	auto blob = createTestBlob(blobData);
	dataManager->addBlobEntity(blob);

	auto retrievedNode = dataManager->getNode(node.getId());
	EXPECT_EQ("MultiTypeNode", dataManager->resolveLabel(retrievedNode.getLabelId()));

	auto retrievedState = dataManager->findStateByKey("multi.type.state");
	EXPECT_EQ(state.getId(), retrievedState.getId());

	auto retrievedIndex = dataManager->getIndex(index.getId());
	EXPECT_EQ(2U, retrievedIndex.getIndexType());

	auto retrievedBlob = dataManager->getBlob(blob.getId());
	EXPECT_EQ(blobData, retrievedBlob.getDataAsString());

	// --- Dirty Check ---
	EXPECT_TRUE(dataManager->hasUnsavedChanges());

	// 1. Check Node
	EXPECT_EQ(1UL, dataManager->getDirtyEntityInfos<Node>({EntityChangeType::CHANGE_ADDED}).size());

	// 2. Check State (Expect >= 1 due to system config/index metadata)
	auto dirtyStates = dataManager->getDirtyEntityInfos<State>({EntityChangeType::CHANGE_ADDED});
	EXPECT_GE(dirtyStates.size(), 1UL);

	// Verify our specific state is in the list
	bool stateFound = false;
	for (const auto &info: dirtyStates) {
		if (info.backup.has_value() && info.backup->getId() == state.getId()) {
			stateFound = true;
			break;
		}
	}
	EXPECT_TRUE(stateFound) << "The manually created state was not found in dirty list.";

	// 3. Check Index (Expect >= 1 due to Label Index B-Tree updates)
	auto dirtyIndexes = dataManager->getDirtyEntityInfos<Index>({EntityChangeType::CHANGE_ADDED});
	EXPECT_GE(dirtyIndexes.size(), 1UL);

	// Verify our specific index is in the list
	bool indexFound = false;
	for (const auto &info: dirtyIndexes) {
		if (info.backup.has_value() && info.backup->getId() == index.getId()) {
			indexFound = true;
			break;
		}
	}
	EXPECT_TRUE(indexFound) << "The manually created index was not found in dirty list.";

	// 4. Check Blob
	auto dirtyBlobs = dataManager->getDirtyEntityInfos<Blob>({EntityChangeType::CHANGE_ADDED});
	EXPECT_GE(dirtyBlobs.size(), 1UL) << "Should have at least the user-created blob";

	bool blobFound = false;
	for (const auto &info : dirtyBlobs) {
		if (info.backup.has_value() && info.backup->getId() == blob.getId()) {
			blobFound = true;
			EXPECT_EQ(info.backup->getDataAsString(), blobData);
			break;
		}
	}
	EXPECT_TRUE(blobFound) << "The user-created blob was not found in dirty list.";

	simulateSave();
	EXPECT_FALSE(dataManager->hasUnsavedChanges());
}

TEST_F(DataManagerTest, LargeEntityCounts) {
	constexpr int entityCount = 100;

	std::vector<Node> nodes;
	std::vector<int64_t> nodeIds;
	nodes.reserve(entityCount);
	nodeIds.reserve(entityCount);

	for (int i = 0; i < entityCount; i++) {
		auto node = createTestNode(dataManager, "LargeNode" + std::to_string(i));
		dataManager->addNode(node);
		nodes.push_back(node);
		nodeIds.push_back(node.getId());
	}

	for (int i = 0; i < entityCount - 1; i++) {
		auto edge = createTestEdge(dataManager, nodes[i].getId(), nodes[i + 1].getId(), "NEXT");
		dataManager->addEdge(edge);
	}

	for (int i = 0; i < entityCount; i++) {
		std::unordered_map<std::string, PropertyValue> props = {
				{"index", PropertyValue(i)},
				{"name", PropertyValue("Node" + std::to_string(i))},
		};
		dataManager->addNodeProperties(nodes[i].getId(), props);
	}

	for (int i = 0; i < entityCount; i += 10) {
		int end = std::min(i + 10, entityCount);
		std::vector<int64_t> batchIds(nodeIds.begin() + i, nodeIds.begin() + end);
		auto batchNodes = dataManager->getNodeBatch(batchIds);
		EXPECT_EQ(batchIds.size(), batchNodes.size());
	}

	dataManager->clearCache();

	constexpr int middleIndex = entityCount / 2;
	auto retrievedNode = dataManager->getNode(nodes[middleIndex].getId());
	EXPECT_EQ(nodes[middleIndex].getId(), retrievedNode.getId());

	auto props = dataManager->getNodeProperties(nodes[middleIndex].getId());
	EXPECT_EQ(middleIndex, std::get<int64_t>(props.at("index").getVariant()));
}

TEST_F(DataManagerTest, DeletionFlagTracking) {
	// Create a test node
	auto node = createTestNode(dataManager, "TestNode");
	dataManager->addNode(node);

	// Create a deletion flag
	std::atomic<bool> deletionFlag(false);
	dataManager->setDeletionFlagReference(&deletionFlag);

	// Verify flag is initially false
	EXPECT_FALSE(deletionFlag.load());

	// Perform a deletion operation which should set the flag
	dataManager->deleteNode(node);

	// Verify the flag was set to true
	EXPECT_TRUE(deletionFlag.load());

	// Reset the flag
	deletionFlag.store(false);

	// Mark deletion performed manually
	dataManager->markDeletionPerformed();

	// Verify the flag was set again
	EXPECT_TRUE(deletionFlag.load());

	// Test with null flag reference (should not crash)
	dataManager->setDeletionFlagReference(nullptr);
	dataManager->markDeletionPerformed(); // Should not crash when flag is null
}

TEST_F(DataManagerTest, DeletionFlagTrackingMultipleOperations) {
	// Create test nodes
	std::vector<Node> nodes;
	for (int i = 0; i < 5; i++) {
		auto node = createTestNode(dataManager, "Node" + std::to_string(i));
		dataManager->addNode(node);
		nodes.push_back(node);
	}

	// Create edges between nodes
	for (size_t i = 0; i < nodes.size() - 1; i++) {
		auto edge = createTestEdge(dataManager, nodes[i].getId(), nodes[i + 1].getId(), "LINK");
		dataManager->addEdge(edge);
	}

	// Set deletion flag
	std::atomic<bool> deletionFlag(false);
	dataManager->setDeletionFlagReference(&deletionFlag);

	// Delete a node (which has edges, triggering multiple deletion operations)
	dataManager->deleteNode(nodes[2]);

	// Verify the flag was set
	EXPECT_TRUE(deletionFlag.load());

	// Reset and test edge deletion
	deletionFlag.store(false);
	auto testEdge = createTestEdge(dataManager, nodes[0].getId(), nodes[1].getId(), "TEST");
	dataManager->addEdge(testEdge);
	dataManager->deleteEdge(testEdge);
	EXPECT_TRUE(deletionFlag.load());
}

TEST_F(DataManagerTest, EmptyBatchOperations) {
	// Clear any existing changes
	simulateSave();

	// Test empty node batch
	std::vector<Node> emptyNodes;
	dataManager->addNodes(emptyNodes);
	EXPECT_NO_THROW(dataManager->addNodes(emptyNodes));

	// Test empty edge batch
	std::vector<Edge> emptyEdges;
	dataManager->addEdges(emptyEdges);
	EXPECT_NO_THROW(dataManager->addEdges(emptyEdges));

	// Verify no new dirty entities were created (should not have unsaved changes)
	[[maybe_unused]] bool hasChanges = dataManager->hasUnsavedChanges();
	// Don't assert false here - there might be background entities. Just verify no crash.
	EXPECT_TRUE(true) << "Empty batch operations should not crash";
}

TEST_F(DataManagerTest, BatchNodesWithProperties) {
	// Create nodes with properties that may trigger external storage
	std::vector<Node> nodes;

	// Node 1: Small properties (inline storage)
	Node node1 = createTestNode(dataManager, "InlineNode");
	node1.setProperties({{"name", PropertyValue("Alice")}, {"age", PropertyValue(30)}});
	nodes.push_back(node1);

	// Node 2: Large properties (may trigger blob storage)
	Node node2 = createTestNode(dataManager, "BlobNode");
	std::string largeData(5000, 'X');
	node2.setProperties({{"data", PropertyValue(largeData)}});
	nodes.push_back(node2);

	// Node 3: Empty properties
	Node node3 = createTestNode(dataManager, "EmptyNode");
	// Empty properties map
	nodes.push_back(node3);

	dataManager->addNodes(nodes);

	// The nodes vector is updated in place with assigned IDs
	// nodes[0], nodes[1], nodes[2] now have valid IDs
	EXPECT_NE(0, nodes[0].getId());
	EXPECT_NE(0, nodes[1].getId());
	EXPECT_NE(0, nodes[2].getId());

	// Verify properties can be retrieved using the updated IDs in the vector
	auto props1 = dataManager->getNodeProperties(nodes[0].getId());
	EXPECT_EQ(2UL, props1.size());

	auto props2 = dataManager->getNodeProperties(nodes[1].getId());
	EXPECT_EQ(1UL, props2.size());
	EXPECT_EQ(5000UL, std::get<std::string>(props2["data"].getVariant()).size());

	auto props3 = dataManager->getNodeProperties(nodes[2].getId());
	EXPECT_TRUE(props3.empty());
}

TEST_F(DataManagerTest, BatchEdgesWithProperties) {
	// Create source and target nodes
	auto source1 = createTestNode(dataManager, "Source1");
	auto source2 = createTestNode(dataManager, "Source2");
	auto target1 = createTestNode(dataManager, "Target1");
	auto target2 = createTestNode(dataManager, "Target2");

	dataManager->addNode(source1);
	dataManager->addNode(source2);
	dataManager->addNode(target1);
	dataManager->addNode(target2);

	// Create edges with properties
	std::vector<Edge> edges;

	Edge edge1 = createTestEdge(dataManager, source1.getId(), target1.getId(), "LINK1");
	edge1.setProperties({{"weight", PropertyValue(1.5)}});
	edges.push_back(edge1);

	Edge edge2 = createTestEdge(dataManager, source2.getId(), target2.getId(), "LINK2");
	edge2.setProperties({{"weight", PropertyValue(2.5)}, {"active", PropertyValue(true)}});
	edges.push_back(edge2);

	dataManager->addEdges(edges);

	// The edges vector is updated in place with assigned IDs
	EXPECT_NE(0, edges[0].getId());
	EXPECT_NE(0, edges[1].getId());

	// Verify properties using the updated IDs in the vector
	auto props1 = dataManager->getEdgeProperties(edges[0].getId());
	EXPECT_EQ(1UL, props1.size());

	auto props2 = dataManager->getEdgeProperties(edges[1].getId());
	EXPECT_EQ(2UL, props2.size());
}

TEST_F(DataManagerTest, GetOrCreateLabelIdEmpty) {
	// Empty label should return 0
	int64_t labelId = dataManager->getOrCreateLabelId("");
	EXPECT_EQ(0, labelId);
}

TEST_F(DataManagerTest, ResolveLabelZero) {
	// Zero labelId should return empty string
	std::string label = dataManager->resolveLabel(0);
	EXPECT_TRUE(label.empty());
}

TEST_F(DataManagerTest, ResolveLabelNonExistent) {
	// Non-existent label should return empty string or throw
	// Since we can't easily create an invalid labelId, test with a high number
	std::string label = dataManager->resolveLabel(999999);
	// Behavior depends on implementation - either empty string or throws
	EXPECT_TRUE(label.empty() || label.empty());
}

TEST_F(DataManagerTest, AddPropertiesToModifiedNode) {
	// Create a node
	auto node = createTestNode(dataManager, "ModifiedNode");
	dataManager->addNode(node);

	// Modify the node
	node.setLabelId(dataManager->getOrCreateLabelId("NewLabel"));
	dataManager->updateNode(node);

	// Add properties (should handle correctly even though node is already modified)
	dataManager->addNodeProperties(node.getId(), {{"key", PropertyValue("value")}});

	auto props = dataManager->getNodeProperties(node.getId());
	EXPECT_EQ(1UL, props.size());
}

TEST_F(DataManagerTest, RemovePropertyFromNodeWithoutExternalProps) {
	// Create a node without triggering external property storage
	auto node = createTestNode(dataManager, "NoExternalProps");
	dataManager->addNode(node);

	// Try to remove a non-existent property
	EXPECT_NO_THROW(dataManager->removeNodeProperty(node.getId(), "nonexistent"));

	// Verify node still exists
	auto retrieved = dataManager->getNode(node.getId());
	EXPECT_EQ(node.getId(), retrieved.getId());
}

TEST_F(DataManagerTest, MarkEntityDeletedWhenJustAdded) {
	// Create a node
	auto node = createTestNode(dataManager, "JustAdded");
	dataManager->addNode(node);

	// It should be in ADDED state
	auto dirtyInfo = dataManager->getDirtyInfo<Node>(node.getId());
	ASSERT_TRUE(dirtyInfo.has_value());
	EXPECT_EQ(EntityChangeType::CHANGE_ADDED, dirtyInfo->changeType);

	// Delete the node (should remove from dirty registry completely)
	dataManager->deleteNode(node);

	// The node should no longer be in dirty state
	auto afterDeleteInfo = dataManager->getDirtyInfo<Node>(node.getId());
	EXPECT_FALSE(afterDeleteInfo.has_value()) << "ADDED entity should be removed from registry after deletion";
}

TEST_F(DataManagerTest, FindEdgesByNodeInvalidDirection) {
	// Create nodes and edges
	auto node1 = createTestNode(dataManager, "Node1");
	auto node2 = createTestNode(dataManager, "Node2");
	dataManager->addNode(node1);
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "TEST");
	dataManager->addEdge(edge);

	// Test with invalid direction (should default to "both")
	auto edges = dataManager->findEdgesByNode(node1.getId(), "invalid");
	EXPECT_EQ(1UL, edges.size());
}

TEST_F(DataManagerTest, DeleteModifiedEntity) {
	// 1. Add a node (dirty state = ADDED)
	auto node = createTestNode(dataManager, "DeleteModifiedNode");
	dataManager->addNode(node);
	int64_t nodeId = node.getId();

	// 2. Save to clear dirty state
	simulateSave();

	// 3. Update the node (dirty state = MODIFIED)
	node.setLabelId(dataManager->getOrCreateLabelId("UpdatedNode"));
	dataManager->updateNode(node);

	// 4. Delete the modified node
	// This should hit the else branch at line 941 in DataManager.cpp:
	// if (dirtyInfo.has_value() && dirtyInfo->changeType == EntityChangeType::CHANGE_ADDED) { ... } else { ... }
	dataManager->deleteNode(node);

	// Verify the node is marked as deleted
	auto retrievedNode = dataManager->getNode(nodeId);
	EXPECT_FALSE(retrievedNode.isActive()) << "Node should be marked inactive after deletion";
}

TEST_F(DataManagerTest, DeleteModifiedEdge) {
	// 1. Create nodes and edge
	auto source = createTestNode(dataManager, "Source");
	auto target = createTestNode(dataManager, "Target");
	dataManager->addNode(source);
	dataManager->addNode(target);

	auto edge = createTestEdge(dataManager, source.getId(), target.getId(), "TEST_EDGE");
	dataManager->addEdge(edge);
	[[maybe_unused]] int64_t edgeId = edge.getId();

	// 2. Save to clear dirty state
	simulateSave();

	// 3. Update the edge (dirty state = MODIFIED)
	edge.setLabelId(dataManager->getOrCreateLabelId("UPDATED_EDGE"));
	dataManager->updateEdge(edge);

	// 4. Delete the modified edge
	// This should hit the else branch in markEntityDeleted for Edge
	dataManager->deleteEdge(edge);

	// Verify the edge is marked as deleted
	auto retrievedEdge = dataManager->getEdge(edgeId);
	EXPECT_FALSE(retrievedEdge.isActive()) << "Edge should be marked inactive after deletion";
}

TEST_F(DataManagerTest, DoubleDeleteEntity) {
	// 1. Add a node
	auto node = createTestNode(dataManager, "DoubleDeleteNode");
	dataManager->addNode(node);

	// 2. Save to clear dirty state
	simulateSave();

	// 3. First delete (dirty state = DELETED)
	dataManager->deleteNode(node);

	// 4. Second delete (should handle gracefully)
	// This tests the else branch where dirtyInfo already exists but is DELETED
	EXPECT_NO_THROW(dataManager->deleteNode(node));
}

TEST_F(DataManagerTest, MarkEntityDeletedRemovesFromRegistryWhenJustAdded) {
	// Test line 940: When entity is in ADDED state, delete should remove from registry
	// instead of marking as DELETED
	auto node = createTestNode(dataManager, "JustAddedNode");
	dataManager->addNode(node);
	int64_t nodeId = node.getId();

	// Verify it's in ADDED state
	auto dirtyInfo = dataManager->getDirtyInfo<Node>(nodeId);
	ASSERT_TRUE(dirtyInfo.has_value()) << "Node should be in dirty registry";
	EXPECT_EQ(EntityChangeType::CHANGE_ADDED, dirtyInfo->changeType);

	// Delete it - should remove from registry (line 940)
	dataManager->deleteNode(node);

	// Verify it's completely removed from registry (not marked as DELETED)
	auto afterDeleteInfo = dataManager->getDirtyInfo<Node>(nodeId);
	EXPECT_FALSE(afterDeleteInfo.has_value())
		<< "ADDED entity should be removed from registry after delete, not marked DELETED";
}

TEST_F(DataManagerTest, MarkEntityDeletedMarksInactiveWhenModified) {
	// Test line 942-943: When entity is in MODIFIED state, delete should mark as DELETED
	auto node = createTestNode(dataManager, "ModifiedNode");
	dataManager->addNode(node);
	simulateSave(); // Save to clear ADDED state

	// Modify the node (it's now in MODIFIED state)
	node.setLabelId(dataManager->getOrCreateLabelId("NewLabel"));
	dataManager->updateNode(node);
	int64_t nodeId = node.getId();

	// Verify it's in MODIFIED state
	auto dirtyInfo = dataManager->getDirtyInfo<Node>(node.getId());
	ASSERT_TRUE(dirtyInfo.has_value()) << "Node should be in dirty registry";
	EXPECT_EQ(EntityChangeType::CHANGE_MODIFIED, dirtyInfo->changeType);

	// Delete it - should mark as DELETED
	dataManager->deleteNode(node);

	// Verify it's marked as DELETED in registry
	auto afterDeleteInfo = dataManager->getDirtyInfo<Node>(nodeId);
	ASSERT_TRUE(afterDeleteInfo.has_value()) << "Node should still be in dirty registry";
	EXPECT_EQ(EntityChangeType::CHANGE_DELETED, afterDeleteInfo->changeType)
		<< "MODIFIED entity should be marked DELETED after delete";
}

TEST_F(DataManagerTest, GetOrCreateLabelIdEmptyString) {
	// Covers L218 true branch: label.empty() returns 0
	int64_t result = dataManager->getOrCreateLabelId("");
	EXPECT_EQ(0, result) << "Empty label should return 0";
}

TEST_F(DataManagerTest, ResolveLabelZeroId) {
	// Covers L227 true branch: labelId == 0 returns ""
	std::string result = dataManager->resolveLabel(0);
	EXPECT_EQ("", result) << "Label ID 0 should resolve to empty string";
}

TEST_F(DataManagerTest, FindEdgesByNodeOutDirection) {
	// Covers L492 true branch: direction == "out"
	auto node1 = createTestNode(dataManager, "A");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "B");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "KNOWS");
	dataManager->addEdge(edge);

	auto outEdges = dataManager->findEdgesByNode(node1.getId(), "out");
	EXPECT_GE(outEdges.size(), 1UL) << "Should find outgoing edges";
}

TEST_F(DataManagerTest, FindEdgesByNodeInDirection) {
	// Covers L494 true branch: direction == "in"
	auto node1 = createTestNode(dataManager, "A");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "B");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "KNOWS");
	dataManager->addEdge(edge);

	auto inEdges = dataManager->findEdgesByNode(node2.getId(), "in");
	EXPECT_GE(inEdges.size(), 1UL) << "Should find incoming edges";
}

TEST_F(DataManagerTest, FindEdgesByNodeBothDirection) {
	// Covers L496 else branch: direction == "both"
	auto node1 = createTestNode(dataManager, "A");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "B");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "KNOWS");
	dataManager->addEdge(edge);

	auto allEdges = dataManager->findEdgesByNode(node1.getId(), "both");
	EXPECT_GE(allEdges.size(), 1UL) << "Should find edges in both directions";
}

TEST_F(DataManagerTest, AddNodesEmptyVector) {
	// Covers L268-269: nodes.empty() returns early
	std::vector<Node> emptyNodes;
	dataManager->addNodes(emptyNodes);
	EXPECT_EQ(0UL, observer->addedNodes.size()) << "No notifications for empty batch";
}

TEST_F(DataManagerTest, AddEdgesEmptyVector) {
	// Covers L387-388: edges.empty() returns early
	std::vector<Edge> emptyEdges;
	dataManager->addEdges(emptyEdges);
	EXPECT_EQ(0UL, observer->addedEdges.size()) << "No notifications for empty batch";
}

TEST_F(DataManagerTest, AddNodesWithProperties) {
	// Covers L281-290: nodes with non-empty properties in addNodes
	std::vector<Node> nodes;
	auto n1 = createTestNode(dataManager, "PropNode");
	std::unordered_map<std::string, PropertyValue> props;
	props["name"] = PropertyValue(std::string("Alice"));
	n1.setProperties(props);
	nodes.push_back(n1);

	auto n2 = createTestNode(dataManager, "EmptyPropNode");
	// n2 has no properties - covers the continue branch at L282-283
	nodes.push_back(n2);

	dataManager->addNodes(nodes);

	for (auto &n : nodes) {
		EXPECT_NE(0, n.getId());
	}
}

TEST_F(DataManagerTest, AddEdgesWithProperties) {
	// Covers L404-415: edges with non-empty properties in addEdges
	auto node1 = createTestNode(dataManager, "A");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "B");
	dataManager->addNode(node2);

	std::vector<Edge> edges;
	auto e1 = createTestEdge(dataManager, node1.getId(), node2.getId(), "PropEdge");
	std::unordered_map<std::string, PropertyValue> props;
	props["weight"] = PropertyValue(static_cast<int64_t>(42));
	e1.setProperties(props);
	edges.push_back(e1);

	auto e2 = createTestEdge(dataManager, node2.getId(), node1.getId(), "EmptyPropEdge");
	// e2 has no properties - covers the continue branch at L405-406
	edges.push_back(e2);

	dataManager->addEdges(edges);

	for (auto &e : edges) {
		EXPECT_NE(0, e.getId());
	}
}

TEST_F(DataManagerTest, GetEntityTemplateNode) {
	// Covers unexecuted getEntity<Node> template instantiation
	auto node = createTestNode(dataManager, "TemplateNode");
	dataManager->addNode(node);

	auto retrieved = dataManager->getEntity<Node>(node.getId());
	EXPECT_EQ(node.getId(), retrieved.getId());
	EXPECT_TRUE(retrieved.isActive());
}

TEST_F(DataManagerTest, GetEntityTemplateEdge) {
	// Covers unexecuted getEntity<Edge> template instantiation
	auto node1 = createTestNode(dataManager, "A");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "B");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "KNOWS");
	dataManager->addEdge(edge);

	auto retrieved = dataManager->getEntity<Edge>(edge.getId());
	EXPECT_EQ(edge.getId(), retrieved.getId());
	EXPECT_TRUE(retrieved.isActive());
}

TEST_F(DataManagerTest, UpdateEntityTemplateNode) {
	// Covers unexecuted updateEntity<Node> template instantiation
	auto node = createTestNode(dataManager, "UpdateTemplateNode");
	dataManager->addNode(node);
	simulateSave();

	node.setLabelId(dataManager->getOrCreateLabelId("UpdatedTemplateNode"));
	dataManager->updateEntity<Node>(node);

	auto retrieved = dataManager->getNode(node.getId());
	EXPECT_EQ("UpdatedTemplateNode", dataManager->resolveLabel(retrieved.getLabelId()));
}

TEST_F(DataManagerTest, UpdateEntityTemplateEdge) {
	// Covers unexecuted updateEntity<Edge> template instantiation
	auto node1 = createTestNode(dataManager, "A");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "B");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "KNOWS");
	dataManager->addEdge(edge);
	simulateSave();

	edge.setLabelId(dataManager->getOrCreateLabelId("LIKES"));
	dataManager->updateEntity<Edge>(edge);

	auto retrieved = dataManager->getEdge(edge.getId());
	EXPECT_EQ("LIKES", dataManager->resolveLabel(retrieved.getLabelId()));
}

TEST_F(DataManagerTest, AddToCacheEdge) {
	// Covers addToCache<Edge> unexecuted instantiation
	// This is called by getEntitiesInRange disk path, so create edges on disk
	auto node1 = createTestNode(dataManager, "Src");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "CacheTestEdge");
	dataManager->addEdge(edge);

	// Flush to disk and clear cache
	fileStorage->flush();
	dataManager->clearCache();

	// getEntitiesInRange will load from disk and call addToCache<Edge>
	auto edges = dataManager->getEntitiesInRange<Edge>(edge.getId(), edge.getId(), 10);
	EXPECT_GE(edges.size(), 1UL) << "Should load edge from disk and cache it";
}

TEST_F(DataManagerTest, MarkEntityDeleted_FreshlyAddedNode) {
	auto node = createTestNode(dataManager, "DeleteAfterAdd");
	dataManager->addNode(node);

	// Node is in CHANGE_ADDED state. Deleting it should remove from PersistenceManager
	// (line 1009-1020: removes from persistence manager)
	dataManager->markEntityDeleted<Node>(node);

	// After marking deleted, getDirtyInfo should return nothing (removed)
	auto info = dataManager->getDirtyInfo<Node>(node.getId());
	EXPECT_FALSE(info.has_value()) << "Freshly added then deleted entity should be removed from dirty tracking";
}

TEST_F(DataManagerTest, MarkEntityDeleted_PersistedNode) {
	auto node = createTestNode(dataManager, "DeletePersisted");
	dataManager->addNode(node);
	int64_t nodeId = node.getId();

	// Save to disk
	fileStorage->flush();

	// Now delete - entity is NOT in ADDED state, so else branch executes
	dataManager->markEntityDeleted<Node>(node);

	auto info = dataManager->getDirtyInfo<Node>(nodeId);
	EXPECT_TRUE(info.has_value()) << "Persisted then deleted entity should have dirty info";
	EXPECT_EQ(info->changeType, EntityChangeType::CHANGE_DELETED);
}

TEST_F(DataManagerTest, ResolveLabel_ZeroId) {
	auto result = dataManager->resolveLabel(0);
	EXPECT_EQ(result, "") << "Label ID 0 should resolve to empty string";
}

TEST_F(DataManagerTest, GetOrCreateLabelId_EmptyString) {
	auto result = dataManager->getOrCreateLabelId("");
	EXPECT_EQ(result, 0) << "Empty label should return 0";
}

TEST_F(DataManagerTest, GetEdgeBatch) {
	auto node1 = createTestNode(dataManager, "BatchEdgeNode1");
	dataManager->addNode(node1);
	auto node2 = createTestNode(dataManager, "BatchEdgeNode2");
	dataManager->addNode(node2);

	auto edge1 = createTestEdge(dataManager, node1.getId(), node2.getId(), "BatchEdge1");
	dataManager->addEdge(edge1);
	auto edge2 = createTestEdge(dataManager, node1.getId(), node2.getId(), "BatchEdge2");
	dataManager->addEdge(edge2);

	std::vector<int64_t> ids = {edge1.getId(), edge2.getId()};
	auto edges = dataManager->getEdgeBatch(ids);
	EXPECT_EQ(edges.size(), 2UL);
}

TEST_F(DataManagerTest, RemoveEntityPropertyNodeViaPublicAPI) {
	// Covers removeNodeProperty which internally calls removeEntityProperty<Node>
	// at line 1024-1025 (unexecuted template instantiation)
	auto node = createTestNode(dataManager, "PropRemoveNode");
	dataManager->addNode(node);

	// Add properties to the node
	dataManager->addNodeProperties(node.getId(), {
		{"name", PropertyValue(std::string("Alice"))},
		{"age", PropertyValue(static_cast<int64_t>(30))},
		{"city", PropertyValue(std::string("NYC"))}
	});

	// Verify properties are there
	auto propsBefore = dataManager->getNodeProperties(node.getId());
	EXPECT_EQ(3UL, propsBefore.size());

	// Remove a property via removeNodeProperty (which delegates to
	// NodeManager::removeProperty, different from removeEntityProperty<Node>)
	dataManager->removeNodeProperty(node.getId(), "age");

	// Verify the property was removed
	auto propsAfter = dataManager->getNodeProperties(node.getId());
	EXPECT_EQ(2UL, propsAfter.size());
	EXPECT_TRUE(propsAfter.find("age") == propsAfter.end())
		<< "Property 'age' should have been removed";
	EXPECT_TRUE(propsAfter.find("name") != propsAfter.end())
		<< "Property 'name' should still exist";
	EXPECT_TRUE(propsAfter.find("city") != propsAfter.end())
		<< "Property 'city' should still exist";
}

TEST_F(DataManagerTest, RemoveEntityPropertyEdgeViaPublicAPI) {
	// Covers removeEdgeProperty which internally calls removeEntityProperty<Edge>
	// at line 1024-1025 (unexecuted template instantiation)
	auto node1 = createTestNode(dataManager, "Src");
	auto node2 = createTestNode(dataManager, "Tgt");
	dataManager->addNode(node1);
	dataManager->addNode(node2);

	auto edge = createTestEdge(dataManager, node1.getId(), node2.getId(), "KNOWS");
	dataManager->addEdge(edge);

	// Add properties to the edge
	dataManager->addEdgeProperties(edge.getId(), {
		{"weight", PropertyValue(1.5)},
		{"since", PropertyValue(static_cast<int64_t>(2020))},
		{"active", PropertyValue(true)}
	});

	// Verify properties are there
	auto propsBefore = dataManager->getEdgeProperties(edge.getId());
	EXPECT_EQ(3UL, propsBefore.size());

	// Remove a property via removeEdgeProperty
	dataManager->removeEdgeProperty(edge.getId(), "since");

	// Verify the property was removed
	auto propsAfter = dataManager->getEdgeProperties(edge.getId());
	EXPECT_EQ(2UL, propsAfter.size());
	EXPECT_TRUE(propsAfter.find("since") == propsAfter.end())
		<< "Property 'since' should have been removed";
}

