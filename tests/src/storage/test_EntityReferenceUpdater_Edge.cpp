/**
 * @file test_EntityReferenceUpdater_Edge.cpp
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
// 2. Edge Reference Updates
// =========================================================================

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_UpdatesNodePointers) {
	// Scenario: Node A -> Edge E -> Node B
	// Move Edge E. Node A's firstOut and Node B's firstIn should point to E_new.
	Node nodeA = createNode("A");
	Node nodeB = createNode("B");
	Edge edge = createEdge(nodeA.getId(), nodeB.getId(), "E");

	nodeA.setFirstOutEdgeId(edge.getId());
	nodeB.setFirstInEdgeId(edge.getId());
	dataManager->updateNode(nodeA);
	dataManager->updateNode(nodeB);

	int64_t oldEdgeId = edge.getId();
	int64_t newEdgeId = oldEdgeId + 200;
	Edge movedEdge = edge;
	movedEdge.getMutableMetadata().id = newEdgeId;

	// ACTION
	updater->updateEntityReferences(oldEdgeId, &movedEdge, toUnderlying(SegmentType::Edge));

	// VERIFY
	Node updatedA = dataManager->getNode(nodeA.getId());
	Node updatedB = dataManager->getNode(nodeB.getId());

	EXPECT_EQ(updatedA.getFirstOutEdgeId(), newEdgeId);
	EXPECT_EQ(updatedB.getFirstInEdgeId(), newEdgeId);
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_UpdatesEdgeChain) {
	// Scenario: Node -> E1 -> E2 -> E3
	// Move E2. E1's next should be E2_new. E3's prev should be E2_new.
	Node n = createNode("N");
	Edge e1 = createEdge(n.getId(), n.getId(), "1");
	Edge e2 = createEdge(n.getId(), n.getId(), "2");
	Edge e3 = createEdge(n.getId(), n.getId(), "3");

	e1.setNextOutEdgeId(e2.getId());
	e2.setPrevOutEdgeId(e1.getId());
	e2.setNextOutEdgeId(e3.getId());
	e3.setPrevOutEdgeId(e2.getId());

	dataManager->updateEdge(e1);
	dataManager->updateEdge(e2);
	dataManager->updateEdge(e3);

	int64_t oldE2 = e2.getId();
	int64_t newE2 = oldE2 + 999;
	Edge movedE2 = e2;
	movedE2.getMutableMetadata().id = newE2;

	// ACTION
	updater->updateEntityReferences(oldE2, &movedE2, toUnderlying(SegmentType::Edge));

	// VERIFY
	Edge updatedE1 = dataManager->getEdge(e1.getId());
	Edge updatedE3 = dataManager->getEdge(e3.getId());

	EXPECT_EQ(updatedE1.getNextOutEdgeId(), newE2);
	EXPECT_EQ(updatedE3.getPrevOutEdgeId(), newE2);
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_UpdatesInEdgeChain) {
	// Scenario: Multiple edges pointing TO the same node (Incoming Edges).
	// Node T (Target).
	// Node S1 -> T (Edge E1).
	// Node S2 -> T (Edge E2).
	// Node S3 -> T (Edge E3).
	// Chain: E1 <-> E2 <-> E3.
	// Move E2. Verify E1.nextIn and E3.prevIn point to E2_new.

	Node t = createNode("Target");
	Node s1 = createNode("S1");
	Node s2 = createNode("S2");
	Node s3 = createNode("S3");

	Edge e1 = createEdge(s1.getId(), t.getId(), "E1");
	Edge e2 = createEdge(s2.getId(), t.getId(), "E2");
	Edge e3 = createEdge(s3.getId(), t.getId(), "E3");

	// Manually setup Incoming Chain on edges
	// Note: In real logic, DataManager maintains this. Here we force it on disk entities.
	e1.setNextInEdgeId(e2.getId());

	e2.setPrevInEdgeId(e1.getId());
	e2.setNextInEdgeId(e3.getId());

	e3.setPrevInEdgeId(e2.getId());

	dataManager->updateEdge(e1);
	dataManager->updateEdge(e2);
	dataManager->updateEdge(e3);

	// MOVE E2
	int64_t oldE2 = e2.getId();
	int64_t newE2 = oldE2 + 555;
	Edge movedE2 = e2;
	movedE2.getMutableMetadata().id = newE2;

	// ACTION
	updater->updateEntityReferences(oldE2, &movedE2, toUnderlying(SegmentType::Edge));

	// VERIFY
	Edge updatedE1 = dataManager->getEdge(e1.getId());
	Edge updatedE3 = dataManager->getEdge(e3.getId());

	EXPECT_EQ(updatedE1.getNextInEdgeId(), newE2) << "E1 nextIn pointer failed to update";
	EXPECT_EQ(updatedE3.getPrevInEdgeId(), newE2) << "E3 prevIn pointer failed to update";
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_SourceNodeWithNoEdges) {
	// Scenario: Edge E points to Node S (source).
	// Move E. S should be updated to point to new edge ID.
	// Note: DataManager automatically links S.firstOutEdgeId to E when E is created.
	Node s = createNode("S");
	Node t = createNode("T");
	Edge e = createEdge(s.getId(), t.getId(), "E");

	// FirstOutEdgeId is automatically set by DataManager when edge is added
	int64_t oldEdgeId = e.getId();
	int64_t newEdgeId = oldEdgeId + 200;
	Edge movedEdge = e;
	movedEdge.getMutableMetadata().id = newEdgeId;

	// ACTION
	updater->updateEntityReferences(oldEdgeId, &movedEdge, toUnderlying(SegmentType::Edge));

	// VERIFY - source node should be updated to new edge ID
	Node updatedS = dataManager->getNode(s.getId());
	EXPECT_EQ(updatedS.getFirstOutEdgeId(), newEdgeId) << "Source node should point to new edge ID";
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_NeighborWithDifferentId) {
	// Scenario: E1 -> E2 -> E3.
	// Move E2. E1's next pointer should ONLY update if it points to oldE2.
	// If E1.next points to something else (not E2), it should not change.
	Node n = createNode("N");
	Edge e1 = createEdge(n.getId(), n.getId(), "1");
	Edge e2 = createEdge(n.getId(), n.getId(), "2");
	Edge e3 = createEdge(n.getId(), n.getId(), "3");

	// Setup: e1 -> e3 (e1's next is e3, NOT e2)
	e1.setNextOutEdgeId(e3.getId());
	e3.setPrevOutEdgeId(e1.getId());

	// e2 is isolated
	e2.setPrevOutEdgeId(0);
	e2.setNextOutEdgeId(0);

	dataManager->updateEdge(e1);
	dataManager->updateEdge(e2);
	dataManager->updateEdge(e3);

	int64_t oldE2 = e2.getId();
	int64_t newE2 = oldE2 + 999;
	Edge movedE2 = e2;
	movedE2.getMutableMetadata().id = newE2;

	// ACTION
	updater->updateEntityReferences(oldE2, &movedE2, toUnderlying(SegmentType::Edge));

	// VERIFY - e1's next should still point to e3 (not updated)
	Edge updatedE1 = dataManager->getEdge(e1.getId());
	EXPECT_EQ(updatedE1.getNextOutEdgeId(), e3.getId()) << "E1's next should not have changed";
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_WithNoNeighbors) {
	// Scenario: Edge E has no neighbors (isolated edge).
	// Move E. Should handle gracefully without updating non-existent neighbors.
	Node n = createNode("N");
	Edge e = createEdge(n.getId(), n.getId(), "E");

	// No neighbors set (all 0)
	e.setPrevOutEdgeId(0);
	e.setNextOutEdgeId(0);
	e.setPrevInEdgeId(0);
	e.setNextInEdgeId(0);
	dataManager->updateEdge(e);

	int64_t oldEdgeId = e.getId();
	int64_t newEdgeId = oldEdgeId + 111;
	Edge movedEdge = e;
	movedEdge.getMutableMetadata().id = newEdgeId;

	// ACTION - should not crash
	updater->updateEntityReferences(oldEdgeId, &movedEdge, toUnderlying(SegmentType::Edge));

	// VERIFY
	Edge updatedEdge = dataManager->getEdge(newEdgeId);
	EXPECT_EQ(updatedEdge.getPrevOutEdgeId(), 0);
	EXPECT_EQ(updatedEdge.getNextOutEdgeId(), 0);
	EXPECT_EQ(updatedEdge.getPrevInEdgeId(), 0);
	EXPECT_EQ(updatedEdge.getNextInEdgeId(), 0);
}

// Test Edge update with inactive neighbor
TEST_F(EntityReferenceUpdaterTest, UpdateEdge_NeighborInactive) {
	Node n = createNode("N");
	Edge e1 = createEdge(n.getId(), n.getId(), "1");
	Edge e2 = createEdge(n.getId(), n.getId(), "2");

	// Set up chain: e1 -> e2
	e1.setNextOutEdgeId(e2.getId());
	e2.setPrevOutEdgeId(e1.getId());
	dataManager->updateEdge(e1);
	dataManager->updateEdge(e2);

	// Note: Both edges are active in the DB at this point
	// The updater will successfully update e2's prev pointer to newE1

	int64_t oldE1 = e1.getId();
	int64_t newE1 = oldE1 + 100;
	Edge movedE1 = e1;
	movedE1.getMutableMetadata().id = newE1;

	// Should handle gracefully - when updater checks e2's prev pointer,
	// it will try to update e2, but e2 is still active in DB
	// The key is: the updater checks if e2.prev == oldE1, if so updates to newE1
	EXPECT_NO_THROW(updater->updateEntityReferences(oldE1, &movedE1, toUnderlying(SegmentType::Edge)));

	// Verify e2's prev pointer was updated to newE1
	Edge updatedE2 = dataManager->getEdge(e2.getId());
	EXPECT_EQ(updatedE2.getPrevOutEdgeId(), newE1);
}

// Test Edge update with non-existent neighbor
TEST_F(EntityReferenceUpdaterTest, UpdateEdge_NeighborDoesNotExist) {
	Node n = createNode("N");
	Edge e1 = createEdge(n.getId(), n.getId(), "1");

	// Set nextOut to non-existent edge ID
	e1.setNextOutEdgeId(99999);
	dataManager->updateEdge(e1);

	int64_t oldE1 = e1.getId();
	int64_t newE1 = oldE1 + 100;
	Edge movedE1 = e1;
	movedE1.getMutableMetadata().id = newE1;

	// Should handle gracefully - the updater tries to get edge 99999
	// When it doesn't exist (getId() == 0), it breaks the loop
	EXPECT_NO_THROW(updater->updateEntityReferences(oldE1, &movedE1, toUnderlying(SegmentType::Edge)));

	// The non-existent neighbor remains unchanged (still 99999)
	// The updater doesn't clear pointers to non-existent entities
	Edge updatedE1 = dataManager->getEdge(oldE1);
	EXPECT_EQ(updatedE1.getNextOutEdgeId(), 99999) << "Non-existent neighbor should remain unchanged";
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_SourceNodeFirstOutNotOldEdge) {
	// Test updateEdgeReferences when srcNode.getFirstOutEdgeId() != oldEdgeId
	// (line 102 false branch)
	Node src = createNode("S");
	Node dst = createNode("D");
	Edge e1 = createEdge(src.getId(), dst.getId(), "E1");
	Edge e2 = createEdge(src.getId(), dst.getId(), "E2");

	// src's firstOut points to e1, not e2
	src.setFirstOutEdgeId(e1.getId());
	dataManager->updateNode(src);

	int64_t oldE2 = e2.getId();
	int64_t newE2 = oldE2 + 200;
	Edge movedE2 = e2;
	movedE2.getMutableMetadata().id = newE2;

	updater->updateEntityReferences(oldE2, &movedE2, toUnderlying(SegmentType::Edge));

	// src's firstOut should NOT have been updated (it points to e1, not e2)
	Node updatedSrc = dataManager->getNode(src.getId());
	EXPECT_EQ(updatedSrc.getFirstOutEdgeId(), e1.getId());
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_TargetNodeFirstInNotOldEdge) {
	// Test updateEdgeReferences when dstNode.getFirstInEdgeId() != oldEdgeId
	// (line 110 false branch)
	Node src = createNode("S");
	Node dst = createNode("D");
	Edge e1 = createEdge(src.getId(), dst.getId(), "E1");
	Edge e2 = createEdge(src.getId(), dst.getId(), "E2");

	// dst's firstIn points to e1, not e2
	dst.setFirstInEdgeId(e1.getId());
	dataManager->updateNode(dst);

	int64_t oldE2 = e2.getId();
	int64_t newE2 = oldE2 + 200;
	Edge movedE2 = e2;
	movedE2.getMutableMetadata().id = newE2;

	updater->updateEntityReferences(oldE2, &movedE2, toUnderlying(SegmentType::Edge));

	// dst's firstIn should NOT have been updated
	Node updatedDst = dataManager->getNode(dst.getId());
	EXPECT_EQ(updatedDst.getFirstInEdgeId(), e1.getId());
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_BlobPropertyOnEdge) {
	// Test updatePropertiesPointingToEntity for Edge with BLOB storage
	// (line 391 branch in updatePropertiesPointingToEntity)
	Node n = createNode("N");
	Edge edge = createEdge(n.getId(), n.getId(), "E");
	Blob blob = createBlob(edge.getId(), toUnderlying(SegmentType::Edge));

	edge.setPropertyEntityId(blob.getId(), PropertyStorageType::BLOB_ENTITY);
	dataManager->updateEdge(edge);

	int64_t oldEdgeId = edge.getId();
	int64_t newEdgeId = oldEdgeId + 100;
	Edge movedEdge = edge;
	movedEdge.getMutableMetadata().id = newEdgeId;

	updater->updateEntityReferences(oldEdgeId, &movedEdge, toUnderlying(SegmentType::Edge));

	// Blob should now point to the new edge ID
	Blob updatedBlob = dataManager->getBlob(blob.getId());
	EXPECT_EQ(updatedBlob.getEntityId(), newEdgeId);
}

// Test updateEdgeReferences when old == new ID (no-op)
// Covers line 94: if (oldEdgeId == newEdgeId) return
TEST_F(EntityReferenceUpdaterTest, UpdateEdgeReferences_SameId) {
	Node n = createNode("N");
	Edge edge = createEdge(n.getId(), n.getId(), "E");
	int64_t id = edge.getId();

	EXPECT_NO_THROW(updater->updateEntityReferences(id, &edge, toUnderlying(SegmentType::Edge)));
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_PropertyStorageNone) {
	// Test updatePropertiesPointingToEntity for Edge with NONE storage type
	// (the branch where propertyEntityId == 0 is already tested for Node,
	//  but this tests the Edge code path specifically)
	Node n = createNode("N");
	Edge edge = createEdge(n.getId(), n.getId(), "E");

	// Edge has no property (propertyEntityId == 0)
	EXPECT_EQ(edge.getPropertyEntityId(), 0);

	int64_t oldEdgeId = edge.getId();
	int64_t newEdgeId = oldEdgeId + 100;
	Edge movedEdge = edge;
	movedEdge.getMutableMetadata().id = newEdgeId;

	// Should handle gracefully
	EXPECT_NO_THROW(updater->updateEntityReferences(oldEdgeId, &movedEdge, toUnderlying(SegmentType::Edge)));
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_NeighborOutNextMismatch) {
	// Cover: line 123 false branch - neighbor.getNextOutEdgeId() != oldEdgeId
	// Create an edge chain and then break the back-reference before moving
	Node n1 = createNode("MismatchNode1");
	Node n2 = createNode("MismatchNode2");

	// Create two edges from n1 to n2: chain is [edgeB, edgeA] (LIFO)
	Edge edgeA = createEdge(n1.getId(), n2.getId(), "MIS_LINK");
	Edge edgeB = createEdge(n1.getId(), n2.getId(), "MIS_LINK");

	// edgeB is head of outgoing chain, edgeB.nextOutEdge = edgeA
	// edgeA.prevOutEdge = edgeB
	Edge storedA = dataManager->getEdge(edgeA.getId());
	Edge storedB = dataManager->getEdge(edgeB.getId());
	ASSERT_EQ(storedA.getPrevOutEdgeId(), edgeB.getId());
	ASSERT_EQ(storedB.getNextOutEdgeId(), edgeA.getId());

	// Break the back-reference: set edgeB.nextOutEdge to 0
	storedB.setNextOutEdgeId(0);
	dataManager->updateEdge(storedB);

	// Now move edgeA to new ID. The updater will look at edgeA.prevOutEdge (edgeB),
	// check if edgeB.nextOutEdge == oldEdgeAId. Since we changed it to 0, the check fails.
	int64_t oldEdgeAId = edgeA.getId();
	int64_t newEdgeAId = oldEdgeAId + 500;
	Edge movedA = dataManager->getEdge(edgeA.getId());
	movedA.setId(newEdgeAId);

	updater->updateEntityReferences(oldEdgeAId, &movedA, toUnderlying(SegmentType::Edge));

	// Verify edgeB.nextOutEdge was NOT updated (since it didn't match)
	Edge checkB = dataManager->getEdge(edgeB.getId());
	EXPECT_EQ(checkB.getNextOutEdgeId(), 0);
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_NeighborInNextMismatch) {
	// Cover: line 135 false branch - neighbor.getNextInEdgeId() != oldEdgeId
	Node n1 = createNode("InMisNode1");
	Node n2 = createNode("InMisNode2");
	Node n3 = createNode("InMisNode3");

	// Create two edges targeting n2 from different sources
	Edge edgeA = createEdge(n1.getId(), n2.getId(), "IN_MIS");
	Edge edgeB = createEdge(n3.getId(), n2.getId(), "IN_MIS");

	// Incoming chain for n2: [edgeB, edgeA] (LIFO)
	Edge storedA = dataManager->getEdge(edgeA.getId());
	Edge storedB = dataManager->getEdge(edgeB.getId());
	ASSERT_EQ(storedA.getPrevInEdgeId(), edgeB.getId());
	ASSERT_EQ(storedB.getNextInEdgeId(), edgeA.getId());

	// Break the in-chain back-reference
	storedB.setNextInEdgeId(0);
	dataManager->updateEdge(storedB);

	// Move edgeA
	int64_t oldId = edgeA.getId();
	int64_t newId = oldId + 600;
	Edge movedA = dataManager->getEdge(edgeA.getId());
	movedA.setId(newId);

	updater->updateEntityReferences(oldId, &movedA, toUnderlying(SegmentType::Edge));

	// edgeB.nextInEdge should NOT have been updated
	Edge checkB = dataManager->getEdge(edgeB.getId());
	EXPECT_EQ(checkB.getNextInEdgeId(), 0);
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_PropertyOnEdgeStorageBlob) {
	// Test updatePropertiesPointingToEntity for Edge with BLOB storage
	// Covers line 391-394 for Edge type specifically
	Node n = createNode("N");
	Edge edge = createEdge(n.getId(), n.getId(), "E");
	Blob blob = createBlob(edge.getId(), toUnderlying(SegmentType::Edge));

	edge.setPropertyEntityId(blob.getId(), PropertyStorageType::BLOB_ENTITY);
	dataManager->updateEdge(edge);

	int64_t oldEdgeId = edge.getId();
	int64_t newEdgeId = oldEdgeId + 400;
	Edge movedEdge = edge;
	movedEdge.getMutableMetadata().id = newEdgeId;

	updater->updateEntityReferences(oldEdgeId, &movedEdge, toUnderlying(SegmentType::Edge));

	// Blob entity info should now point to new edge ID
	Blob updatedBlob = dataManager->getBlob(blob.getId());
	EXPECT_EQ(updatedBlob.getEntityId(), newEdgeId);
	EXPECT_EQ(updatedBlob.getEntityType(), toUnderlying(SegmentType::Edge));
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_PropertyOnEdgeStorageProperty) {
	// Test updatePropertiesPointingToEntity for Edge with PROPERTY storage
	// Covers line 387-390 for Edge type specifically
	Node n = createNode("N");
	Edge edge = createEdge(n.getId(), n.getId(), "E");
	Property prop = createProperty(edge.getId(), toUnderlying(SegmentType::Edge));

	edge.setPropertyEntityId(prop.getId(), PropertyStorageType::PROPERTY_ENTITY);
	dataManager->updateEdge(edge);

	int64_t oldEdgeId = edge.getId();
	int64_t newEdgeId = oldEdgeId + 500;
	Edge movedEdge = edge;
	movedEdge.getMutableMetadata().id = newEdgeId;

	updater->updateEntityReferences(oldEdgeId, &movedEdge, toUnderlying(SegmentType::Edge));

	// Property entity info should now point to new edge ID
	Property updatedProp = dataManager->getProperty(prop.getId());
	EXPECT_EQ(updatedProp.getEntityId(), newEdgeId);
	EXPECT_EQ(updatedProp.getEntityType(), toUnderlying(SegmentType::Edge));
}

TEST_F(EntityReferenceUpdaterTest, UpdateEdge_NoChainLinks) {
	Node nodeA = createNode("ChainA");
	Node nodeB = createNode("ChainB");
	Edge edge = createEdge(nodeA.getId(), nodeB.getId(), "ISOLATED_EDGE");

	// Do not set any edge chain pointers (prevOut, nextOut, prevIn, nextIn all 0)
	// But do set node pointers to this edge
	nodeA.setFirstOutEdgeId(edge.getId());
	nodeB.setFirstInEdgeId(edge.getId());
	dataManager->updateNode(nodeA);
	dataManager->updateNode(nodeB);

	int64_t oldId = edge.getId();
	int64_t newId = oldId + 5000;
	Edge movedEdge = edge;
	movedEdge.getMutableMetadata().id = newId;

	EXPECT_NO_THROW(updater->updateEntityReferences(oldId, &movedEdge, toUnderlying(SegmentType::Edge)));

	// Verify node pointers updated
	Node updatedA = dataManager->getNode(nodeA.getId());
	Node updatedB = dataManager->getNode(nodeB.getId());
	EXPECT_EQ(updatedA.getFirstOutEdgeId(), newId);
	EXPECT_EQ(updatedB.getFirstInEdgeId(), newId);
}
