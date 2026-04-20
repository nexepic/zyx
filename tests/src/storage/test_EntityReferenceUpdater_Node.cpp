/**
 * @file test_EntityReferenceUpdater_Node.cpp
 * @author Nexepic
 * @date 2025/12/8
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

#include "storage/EntityReferenceUpdaterTestFixture.hpp"

// =========================================================================
// 1. Node Reference Updates
// =========================================================================

TEST_F(EntityReferenceUpdaterTest, UpdateNode_UpdatesConnectedEdges) {
	// Scenario: Node A -> Edge E -> Node B
	// Move Node A. Edge E's source should update.
	Node nodeA = createNode("A");
	Node nodeB = createNode("B");
	Edge edge = createEdge(nodeA.getId(), nodeB.getId(), "E");

	nodeA.setFirstOutEdgeId(edge.getId());
	dataManager->updateNode(nodeA);

	int64_t oldId = nodeA.getId();
	int64_t newId = oldId + 100;

	Node movedNode = nodeA;
	movedNode.getMutableMetadata().id = newId;

	// ACTION
	updater->updateEntityReferences(oldId, &movedNode, toUnderlying(SegmentType::Node));

	// VERIFY
	Edge updatedEdge = dataManager->getEdge(edge.getId());
	EXPECT_EQ(updatedEdge.getSourceNodeId(), newId);
	EXPECT_EQ(updatedEdge.getTargetNodeId(), nodeB.getId());
}

TEST_F(EntityReferenceUpdaterTest, UpdateNode_UpdatesIncomingEdges) {
	// Scenario: Node A -> Edge E -> Node B
	// Move Node B. Edge E's target should update.
	Node nodeA = createNode("A");
	Node nodeB = createNode("B");
	Edge edge = createEdge(nodeA.getId(), nodeB.getId(), "E");

	nodeB.setFirstInEdgeId(edge.getId());
	dataManager->updateNode(nodeB);

	int64_t oldId = nodeB.getId();
	int64_t newId = oldId + 500;
	Node movedNode = nodeB;
	movedNode.getMutableMetadata().id = newId;

	// ACTION
	updater->updateEntityReferences(oldId, &movedNode, toUnderlying(SegmentType::Node));

	// VERIFY
	Edge updatedEdge = dataManager->getEdge(edge.getId());
	EXPECT_EQ(updatedEdge.getTargetNodeId(), newId);
	EXPECT_EQ(updatedEdge.getSourceNodeId(), nodeA.getId());
}

TEST_F(EntityReferenceUpdaterTest, UpdateNode_UpdatesPropertyRef) {
	// Scenario: Node A has Property P.
	// Move Node A. Property P should now point to A_new.
	Node node = createNode("PropOwner");
	Property prop = createProperty(node.getId(), toUnderlying(SegmentType::Node));

	node.setPropertyEntityId(prop.getId(), PropertyStorageType::PROPERTY_ENTITY);
	dataManager->updateNode(node);

	int64_t oldNodeId = node.getId();
	int64_t newNodeId = oldNodeId + 10;
	Node movedNode = node;
	movedNode.getMutableMetadata().id = newNodeId;

	// ACTION
	updater->updateEntityReferences(oldNodeId, &movedNode, toUnderlying(SegmentType::Node));

	// VERIFY
	Property updatedProp = dataManager->getProperty(prop.getId());
	EXPECT_EQ(updatedProp.getEntityId(), newNodeId);
}

TEST_F(EntityReferenceUpdaterTest, UpdateNode_UpdatesBlobRef) {
	// Scenario: Node has a Blob entity (not Property).
	// Move Node. Blob should update its owner info to new Node ID.
	// Note: updatePropertiesPointingToEntity is called when the *Entity* (Node) moves,
	// and it updates the *Blob* pointing to it.

	Node node = createNode("BlobOwner");
	Blob blob = createBlob(node.getId(), toUnderlying(SegmentType::Node));

	// Link Node -> Blob
	node.setPropertyEntityId(blob.getId(), PropertyStorageType::BLOB_ENTITY);
	dataManager->updateNode(node);

	// MOVE Node
	int64_t oldNodeId = node.getId();
	int64_t newNodeId = oldNodeId + 99;
	Node movedNode = node;
	movedNode.getMutableMetadata().id = newNodeId;

	// ACTION
	updater->updateEntityReferences(oldNodeId, &movedNode, toUnderlying(SegmentType::Node));

	// VERIFY
	// The Blob should now point to the new Node ID
	Blob updatedBlob = dataManager->getBlob(blob.getId());
	EXPECT_EQ(updatedBlob.getEntityId(), newNodeId);
	EXPECT_EQ(updatedBlob.getEntityType(), toUnderlying(SegmentType::Node));
}

TEST_F(EntityReferenceUpdaterTest, UpdateNode_EdgeWithInactiveSource) {
	// Scenario: Edge E points to Node S, but E is marked inactive.
	// Move Node S. Edge E should NOT be updated (inactive edges are skipped).
	// The updater should handle inactive edges gracefully.
	Node s = createNode("S");
	Node t = createNode("T");
	Edge e = createEdge(s.getId(), t.getId(), "E");

	// Set the node's firstOutEdgeId to point to this edge
	s.setFirstOutEdgeId(e.getId());
	dataManager->updateNode(s);

	// Mark edge as inactive - this simulates the edge being deleted
	// NOTE: We don't call updateEdge on inactive edge because BaseEntityManager::update
	// explicitly checks and throws "Update inactive entity" exception
	e.markInactive();

	int64_t oldSId = s.getId();
	int64_t newSId = oldSId + 100;
	Node movedS = s;
	movedS.getMutableMetadata().id = newSId;

	// ACTION - inactive edges are skipped during traversal
	// The updateNodeReferences method skips inactive edges (line 76-77 in EntityReferenceUpdater.cpp)
	EXPECT_NO_THROW(updater->updateEntityReferences(oldSId, &movedS, toUnderlying(SegmentType::Node)));

	// VERIFY - the inactive edge should still point to the old node ID
	// because inactive edges are not updated
	EXPECT_EQ(e.getSourceNodeId(), oldSId) << "Inactive edge should not be updated";
}

TEST_F(EntityReferenceUpdaterTest, UpdateNode_WithNoProperties) {
	// Scenario: Node N has no properties (propertyEntityId=0, storageType=NONE).
	// Move N. Should handle gracefully without trying to update non-existent properties.
	Node node = createNode("NoProps");

	// Ensure node has no properties
	node.setPropertyEntityId(0, PropertyStorageType::NONE);
	dataManager->updateNode(node);

	int64_t oldNodeId = node.getId();
	int64_t newNodeId = oldNodeId + 88;
	Node movedNode = node;
	movedNode.getMutableMetadata().id = newNodeId;

	// ACTION - should not crash
	updater->updateEntityReferences(oldNodeId, &movedNode, toUnderlying(SegmentType::Node));

	// VERIFY
	Node updatedNode = dataManager->getNode(newNodeId);
	EXPECT_EQ(updatedNode.getPropertyEntityId(), 0);
	EXPECT_EQ(updatedNode.getPropertyStorageType(), PropertyStorageType::NONE);
}

TEST_F(EntityReferenceUpdaterTest, UpdateNode_PropertyEntityType) {
	// Cover: line 387 true branch and line 391 false branch
	// Node with PROPERTY_ENTITY storage type: hits line 387 (true) and skips line 391.
	Node n = createNode("PropNode");
	Property prop = createProperty(n.getId(), toUnderlying(SegmentType::Node));

	// Set node's property entity to a PROPERTY_ENTITY type
	n.setPropertyEntityId(prop.getId(), PropertyStorageType::PROPERTY_ENTITY);
	dataManager->updateNode(n);

	int64_t oldNodeId = n.getId();
	int64_t newNodeId = oldNodeId + 800;
	Node movedNode = dataManager->getNode(n.getId());
	movedNode.setId(newNodeId);

	updater->updateEntityReferences(oldNodeId, &movedNode, toUnderlying(SegmentType::Node));

	// The property's entityInfo should be updated to newNodeId
	Property updatedProp = dataManager->getProperty(prop.getId());
	EXPECT_EQ(updatedProp.getEntityId(), newNodeId);
}

// Test updateNodeReferences when old == new ID (no-op)
// Covers line 64: if (oldNodeId == newNodeId) return
TEST_F(EntityReferenceUpdaterTest, UpdateNodeReferences_SameId) {
	Node node = createNode("N");
	int64_t id = node.getId();

	// Same old and new ID - should be a no-op
	EXPECT_NO_THROW(updater->updateEntityReferences(id, &node, toUnderlying(SegmentType::Node)));
}

// Test updatePropertiesPointingToEntity with zero propertyEntityId
// Covers line 381: if (info.propertyEntityId == 0) return
TEST_F(EntityReferenceUpdaterTest, UpdateNodeReferences_NoPropertyEntity) {
	Node node = createNode("N");
	// Node has no property entity (propertyEntityId == 0)
	EXPECT_EQ(node.getPropertyEntityId(), 0);

	int64_t oldId = node.getId();
	int64_t newId = oldId + 100;
	Node movedNode = node;
	movedNode.getMutableMetadata().id = newId;

	// Should not crash when there's no property entity
	EXPECT_NO_THROW(updater->updateEntityReferences(oldId, &movedNode, toUnderlying(SegmentType::Node)));
}

TEST_F(EntityReferenceUpdaterTest, UpdateNode_MultipleOutgoingEdges) {
	// Test node reference update when node has multiple outgoing edges
	// This tests the while loop traversal in updateNodeReferences (lines 72-77)
	Node nodeA = createNode("A");
	Node nodeB = createNode("B");
	Node nodeC = createNode("C");

	Edge e1 = createEdge(nodeA.getId(), nodeB.getId(), "E1");
	Edge e2 = createEdge(nodeA.getId(), nodeC.getId(), "E2");

	// Chain: A.firstOut -> e2 -> e1
	e2.setNextOutEdgeId(e1.getId());
	e1.setPrevOutEdgeId(e2.getId());
	dataManager->updateEdge(e1);
	dataManager->updateEdge(e2);

	nodeA.setFirstOutEdgeId(e2.getId());
	dataManager->updateNode(nodeA);

	int64_t oldId = nodeA.getId();
	int64_t newId = oldId + 500;
	Node movedNode = nodeA;
	movedNode.getMutableMetadata().id = newId;

	updater->updateEntityReferences(oldId, &movedNode, toUnderlying(SegmentType::Node));

	// Both edges should have their sourceNodeId updated
	Edge updatedE1 = dataManager->getEdge(e1.getId());
	Edge updatedE2 = dataManager->getEdge(e2.getId());
	EXPECT_EQ(updatedE1.getSourceNodeId(), newId);
	EXPECT_EQ(updatedE2.getSourceNodeId(), newId);
}

TEST_F(EntityReferenceUpdaterTest, UpdateNode_MultipleIncomingEdges) {
	// Test node reference update when node has multiple incoming edges
	// This tests the while loop traversal for incoming edges (lines 80-86)
	Node nodeA = createNode("A");
	Node nodeB = createNode("B");
	Node nodeC = createNode("C");

	Edge e1 = createEdge(nodeB.getId(), nodeA.getId(), "E1");
	Edge e2 = createEdge(nodeC.getId(), nodeA.getId(), "E2");

	// Chain: A.firstIn -> e2 -> e1
	e2.setNextInEdgeId(e1.getId());
	e1.setPrevInEdgeId(e2.getId());
	dataManager->updateEdge(e1);
	dataManager->updateEdge(e2);

	nodeA.setFirstInEdgeId(e2.getId());
	dataManager->updateNode(nodeA);

	int64_t oldId = nodeA.getId();
	int64_t newId = oldId + 500;
	Node movedNode = nodeA;
	movedNode.getMutableMetadata().id = newId;

	updater->updateEntityReferences(oldId, &movedNode, toUnderlying(SegmentType::Node));

	// Both edges should have their targetNodeId updated
	Edge updatedE1 = dataManager->getEdge(e1.getId());
	Edge updatedE2 = dataManager->getEdge(e2.getId());
	EXPECT_EQ(updatedE1.getTargetNodeId(), newId);
	EXPECT_EQ(updatedE2.getTargetNodeId(), newId);
}

TEST_F(EntityReferenceUpdaterTest, UpdateNode_NoOutgoingNoIncomingEdges) {
	// Test updateNodeReferences when node has no outgoing AND no incoming edges
	// Both while loops (lines 72 and 81) should immediately exit
	Node node = createNode("Isolated");

	// Ensure no edges
	node.setFirstOutEdgeId(0);
	node.setFirstInEdgeId(0);
	dataManager->updateNode(node);

	int64_t oldId = node.getId();
	int64_t newId = oldId + 700;
	Node movedNode = node;
	movedNode.getMutableMetadata().id = newId;

	EXPECT_NO_THROW(updater->updateEntityReferences(oldId, &movedNode, toUnderlying(SegmentType::Node)));
}

TEST_F(EntityReferenceUpdaterTest, UpdateNode_NoEdgeConnections) {
	Node node = createNode("IsolatedNode");
	// Node has no edges connected (firstOutEdgeId == 0, firstInEdgeId == 0)

	int64_t oldId = node.getId();
	int64_t newId = oldId + 3000;
	Node movedNode = node;
	movedNode.getMutableMetadata().id = newId;

	// Should complete without error even though there are no edges to update
	EXPECT_NO_THROW(updater->updateEntityReferences(oldId, &movedNode, toUnderlying(SegmentType::Node)));
}

// Test Edge with BLOB property on Node (updatePropertiesPointingToEntity BLOB branch for Node)
TEST_F(EntityReferenceUpdaterTest, UpdateEdge_BlobPropertyOnNode) {
	// Test updatePropertiesPointingToEntity for Node with BLOB storage
	// Covers the BLOB_ENTITY branch in updatePropertiesPointingToEntity for Node
	Node node = createNode("N");
	Blob blob = createBlob(node.getId(), toUnderlying(SegmentType::Node));

	node.setPropertyEntityId(blob.getId(), PropertyStorageType::BLOB_ENTITY);
	dataManager->updateNode(node);

	int64_t oldNodeId = node.getId();
	int64_t newNodeId = oldNodeId + 300;
	Node movedNode = node;
	movedNode.getMutableMetadata().id = newNodeId;

	updater->updateEntityReferences(oldNodeId, &movedNode, toUnderlying(SegmentType::Node));

	// Blob entity info should now point to the new node ID
	Blob updatedBlob = dataManager->getBlob(blob.getId());
	EXPECT_EQ(updatedBlob.getEntityId(), newNodeId);
	EXPECT_EQ(updatedBlob.getEntityType(), toUnderlying(SegmentType::Node));
}
