/**
 * @file test_RelationshipTraversal.cpp
 * @author Nexepic
 * @date 2025/7/30
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <random>
#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/DeletionManager.hpp"
#include "graph/storage/data/DirtyEntityInfo.hpp"
#include "graph/traversal/RelationshipTraversal.hpp"

// Test Fixture for basic, single-edge scenarios
class RelationshipTraversalTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Create a unique database file for each test run to ensure isolation
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_db_" + boost::uuids::to_string(uuid) + ".dat");

		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
		traversal = dataManager->getRelationshipTraversal();

		// Create and add two nodes
		node1 = graph::Node(1, dataManager->getOrCreateLabelId("Node1"));
		node2 = graph::Node(2, dataManager->getOrCreateLabelId("Node2"));
		dataManager->addNode(node1);
		dataManager->addNode(node2);

		// Create and add a single edge between them.
		// dataManager->addEdge() internally calls linkEdge(), establishing the initial state.
		edge = graph::Edge(10, node1.getId(), node2.getId(), dataManager->getOrCreateLabelId("RELATED_TO"));
		dataManager->addEdge(edge);
	}

	void TearDown() override {
		database->close();
		database.reset(); // Ensure database is destroyed before file deletion
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::traversal::RelationshipTraversal> traversal;

	graph::Node node1, node2;
	graph::Edge edge;
};

TEST_F(RelationshipTraversalTest, GetOutgoingAndIncomingEdgesForSingleEdge) {
	auto outEdges = traversal->getOutgoingEdges(node1.getId());
	ASSERT_EQ(outEdges.size(), 1UL) << "Node1 should have one outgoing edge.";
	EXPECT_EQ(outEdges[0].getId(), edge.getId());

	auto inEdges = traversal->getIncomingEdges(node2.getId());
	ASSERT_EQ(inEdges.size(), 1UL) << "Node2 should have one incoming edge.";
	EXPECT_EQ(inEdges[0].getId(), edge.getId());
}

TEST_F(RelationshipTraversalTest, GetAllConnectedEdgesForSingleEdge) {
	auto allEdgesA = traversal->getAllConnectedEdges(node1.getId());
	EXPECT_EQ(allEdgesA.size(), 1UL);

	auto allEdgesB = traversal->getAllConnectedEdges(node2.getId());
	EXPECT_EQ(allEdgesB.size(), 1UL);
}

TEST_F(RelationshipTraversalTest, GetConnectedNodesForSingleEdge) {
	auto targets = traversal->getConnectedTargetNodes(node1.getId());
	ASSERT_EQ(targets.size(), 1UL);
	EXPECT_EQ(targets[0].getId(), node2.getId());

	auto sources = traversal->getConnectedSourceNodes(node2.getId());
	ASSERT_EQ(sources.size(), 1UL);
	EXPECT_EQ(sources[0].getId(), node1.getId());
}

TEST_F(RelationshipTraversalTest, GetAllConnectedNodesForSingleEdge) {
	auto connectedA = traversal->getAllConnectedNodes(node1.getId());
	ASSERT_EQ(connectedA.size(), 1UL);
	EXPECT_EQ(connectedA[0].getId(), node2.getId());

	auto connectedB = traversal->getAllConnectedNodes(node2.getId());
	ASSERT_EQ(connectedB.size(), 1UL);
	EXPECT_EQ(connectedB[0].getId(), node1.getId());
}

TEST_F(RelationshipTraversalTest, UnlinkSingleEdge) {
	// First, confirm the edge exists
	ASSERT_FALSE(traversal->getOutgoingEdges(node1.getId()).empty());

	// Perform the unlink operation
	traversal->unlinkEdge(edge);

	// Verify the traversal now returns empty lists
	EXPECT_TRUE(traversal->getOutgoingEdges(node1.getId()).empty());
	EXPECT_TRUE(traversal->getIncomingEdges(node2.getId()).empty());

	// Verify the edge object itself has its pointers cleared
	EXPECT_EQ(edge.getNextOutEdgeId(), 0);
	EXPECT_EQ(edge.getPrevOutEdgeId(), 0);
	EXPECT_EQ(edge.getNextInEdgeId(), 0);
	EXPECT_EQ(edge.getPrevInEdgeId(), 0);
}

TEST_F(RelationshipTraversalTest, TraversalOnNodeWithNoEdges) {
	// Add a new node that has no connections
	graph::Node node3(3, dataManager->getOrCreateLabelId("IsolatedNode"));
	dataManager->addNode(node3);

	EXPECT_TRUE(traversal->getOutgoingEdges(node3.getId()).empty());
	EXPECT_TRUE(traversal->getIncomingEdges(node3.getId()).empty());
	EXPECT_TRUE(traversal->getAllConnectedEdges(node3.getId()).empty());
	EXPECT_TRUE(traversal->getAllConnectedNodes(node3.getId()).empty());
}

TEST_F(RelationshipTraversalTest, TraversalIgnoresInactiveEdges) {
	// Mark the edge as inactive and update it in storage
	dataManager->deleteEdge(edge);

	EXPECT_TRUE(traversal->getOutgoingEdges(node1.getId()).empty())
			<< "Traversal should ignore inactive outgoing edges.";
	EXPECT_TRUE(traversal->getIncomingEdges(node2.getId()).empty())
			<< "Traversal should ignore inactive incoming edges.";
}

TEST_F(RelationshipTraversalTest, GetAllConnectedNodesReturnsUniqueNodes) {
	// Add a second edge from node1 to node2
	graph::Edge edge2(11, node1.getId(), node2.getId(), dataManager->getOrCreateLabelId("KNOWS"));
	dataManager->addEdge(edge2);

	// We have two edges pointing to node2, but should only get one node back
	auto connectedNodes = traversal->getAllConnectedNodes(node1.getId());
	ASSERT_EQ(connectedNodes.size(), 1UL);
	EXPECT_EQ(connectedNodes[0].getId(), node2.getId());
}

TEST_F(RelationshipTraversalTest, SelfReferencingEdge) {
	graph::Node node3(3, dataManager->getOrCreateLabelId("SelfReferencingNode"));
	dataManager->addNode(node3);

	graph::Edge selfEdge(12, node3.getId(), node3.getId(), dataManager->getOrCreateLabelId("LOOPS_ON"));
	dataManager->addEdge(selfEdge);

	auto outEdges = traversal->getOutgoingEdges(node3.getId());
	ASSERT_EQ(outEdges.size(), 1UL);
	EXPECT_EQ(outEdges[0].getId(), selfEdge.getId());

	auto inEdges = traversal->getIncomingEdges(node3.getId());
	ASSERT_EQ(inEdges.size(), 1UL);
	EXPECT_EQ(inEdges[0].getId(), selfEdge.getId());

	auto allNodes = traversal->getAllConnectedNodes(node3.getId());
	ASSERT_EQ(allNodes.size(), 1UL);
	EXPECT_EQ(allNodes[0].getId(), node3.getId());
}

TEST_F(RelationshipTraversalTest, CycleDetectionThrowsExceptionOnTraversal) {
	// Step 1: Get the latest state of the edge and call linkEdge again to intentionally create a cycle.
	// We expect this call to succeed because it only writes data, not traverses.
	auto edgeToCreateCycleWith = dataManager->getEdge(edge.getId());
	// Use EXPECT_NO_THROW to explicitly indicate that we do not expect an exception here.
	EXPECT_NO_THROW(traversal->linkEdge(edgeToCreateCycleWith));

	// Step 2: Now, try to traverse the corrupted, cyclic linked list.
	// This getOutgoingEdges call should detect the cycle and throw an exception.
	ASSERT_THROW(traversal->getOutgoingEdges(node1.getId()), std::runtime_error);

	// You can also verify the same for incoming edges
	ASSERT_THROW(traversal->getIncomingEdges(node2.getId()), std::runtime_error);
}

// A new Test Fixture for more complex scenarios involving multiple edges
class RelationshipTraversalAdvancedTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath =
				std::filesystem::temp_directory_path() / ("test_db_adv_" + boost::uuids::to_string(uuid) + ".dat");
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		dataManager = database->getStorage()->getDataManager();
		traversal = dataManager->getRelationshipTraversal();

		// Create nodes: n1, n2, n3, n4
		n1 = graph::Node(1, dataManager->getOrCreateLabelId("N1"));
		n2 = graph::Node(2, dataManager->getOrCreateLabelId("N2"));
		n3 = graph::Node(3, dataManager->getOrCreateLabelId("N3"));
		n4 = graph::Node(4, dataManager->getOrCreateLabelId("N4"));
		dataManager->addNode(n1);
		dataManager->addNode(n2);
		dataManager->addNode(n3);
		dataManager->addNode(n4);

		// Create a chain of outgoing edges from n1:
		// n1 -> n2 (edge10)
		// n1 -> n3 (edge11)
		// n1 -> n4 (edge12)
		// Since linkEdge adds to the front, the final linked-list order for out-edges will be: edge12 -> edge11 ->
		// edge10
		edge10 = graph::Edge(10, n1.getId(), n2.getId(), dataManager->getOrCreateLabelId("points_to"));
		edge11 = graph::Edge(11, n1.getId(), n3.getId(), dataManager->getOrCreateLabelId("points_to"));
		edge12 = graph::Edge(12, n1.getId(), n4.getId(), dataManager->getOrCreateLabelId("points_to"));
		dataManager->addEdge(edge10);
		dataManager->addEdge(edge11);
		dataManager->addEdge(edge12);
	}

	void TearDown() override {
		database->close();
		database.reset();
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::traversal::RelationshipTraversal> traversal;
	graph::Node n1, n2, n3, n4;
	graph::Edge edge10, edge11, edge12;
};

TEST_F(RelationshipTraversalAdvancedTest, GetMultipleOutgoingEdges) {
	auto outEdges = traversal->getOutgoingEdges(n1.getId());
	ASSERT_EQ(outEdges.size(), 3UL);

	// Verify the order is LIFO (Last-In, First-Out) due to linking at the head
	EXPECT_EQ(outEdges[0].getId(), edge12.getId());
	EXPECT_EQ(outEdges[1].getId(), edge11.getId());
	EXPECT_EQ(outEdges[2].getId(), edge10.getId());
}

TEST_F(RelationshipTraversalAdvancedTest, UnlinkFirstEdgeInChain) {
	// The first edge in the chain is edge12
	auto edgeToUnlink = dataManager->getEdge(edge12.getId());
	traversal->unlinkEdge(edgeToUnlink);

	auto outEdges = traversal->getOutgoingEdges(n1.getId());
	ASSERT_EQ(outEdges.size(), 2UL);
	EXPECT_EQ(outEdges[0].getId(), edge11.getId());
	EXPECT_EQ(outEdges[1].getId(), edge10.getId());

	// Verify the new head of the list (edge11) has no previous edge
	auto newFirstEdge = dataManager->getEdge(edge11.getId());
	EXPECT_EQ(newFirstEdge.getPrevOutEdgeId(), 0);
}

TEST_F(RelationshipTraversalAdvancedTest, UnlinkMiddleEdgeInChain) {
	// The middle edge in the chain is edge11
	auto edgeToUnlink = dataManager->getEdge(edge11.getId());
	traversal->unlinkEdge(edgeToUnlink);

	auto outEdges = traversal->getOutgoingEdges(n1.getId());
	ASSERT_EQ(outEdges.size(), 2UL);

	// Verify the remaining edges are edge12 and edge10
	EXPECT_EQ(outEdges[0].getId(), edge12.getId());
	EXPECT_EQ(outEdges[1].getId(), edge10.getId());

	// Verify that the link was correctly repaired
	auto firstEdge = dataManager->getEdge(edge12.getId());
	auto secondEdge = dataManager->getEdge(edge10.getId());

	EXPECT_EQ(firstEdge.getNextOutEdgeId(), secondEdge.getId());
	EXPECT_EQ(secondEdge.getPrevOutEdgeId(), firstEdge.getId());
}

TEST_F(RelationshipTraversalAdvancedTest, UnlinkLastEdgeInChain) {
	// The last edge in the chain is edge10
	auto edgeToUnlink = dataManager->getEdge(edge10.getId());
	traversal->unlinkEdge(edgeToUnlink);

	auto outEdges = traversal->getOutgoingEdges(n1.getId());
	ASSERT_EQ(outEdges.size(), 2UL);
	EXPECT_EQ(outEdges[0].getId(), edge12.getId());
	EXPECT_EQ(outEdges[1].getId(), edge11.getId());

	// Verify the new last edge in the list (edge11) has no next edge
	auto newLastEdge = dataManager->getEdge(edge11.getId());
	EXPECT_EQ(newLastEdge.getNextOutEdgeId(), 0);
}

TEST_F(RelationshipTraversalAdvancedTest, UnlinkMiddleEdgeFromIncomingChain) {
	graph::Node n5(5, dataManager->getOrCreateLabelId("N5"));
	dataManager->addNode(n5);

	graph::Edge edge14(14, n1.getId(), n5.getId(), dataManager->getOrCreateLabelId("links_to"));
	dataManager->addEdge(edge14);

	graph::Edge edge24(24, n2.getId(), n5.getId(), dataManager->getOrCreateLabelId("links_to"));
	dataManager->addEdge(edge24);

	graph::Edge edge34(34, n3.getId(), n5.getId(), dataManager->getOrCreateLabelId("links_to"));
	dataManager->addEdge(edge34);

	auto initialInEdges = traversal->getIncomingEdges(n5.getId());
	ASSERT_EQ(initialInEdges.size(), 3UL);
	ASSERT_EQ(initialInEdges[0].getId(), edge34.getId());
	ASSERT_EQ(initialInEdges[1].getId(), edge24.getId());
	ASSERT_EQ(initialInEdges[2].getId(), edge14.getId());

	// Remove the middle edge edge24.
	auto edgeToUnlink = dataManager->getEdge(edge24.getId());
	traversal->unlinkEdge(edgeToUnlink);

	// a. Check the edges currently in the chain
	auto finalInEdges = traversal->getIncomingEdges(n5.getId());
	ASSERT_EQ(finalInEdges.size(), 2UL);
	EXPECT_EQ(finalInEdges[0].getId(), edge34.getId()); // New head of the list
	EXPECT_EQ(finalInEdges[1].getId(), edge14.getId()); // New tail of the list

	// b. Key verification: check if the pointers are correctly reconnected
	auto newFirstEdge = dataManager->getEdge(edge34.getId());
	auto newSecondEdge = dataManager->getEdge(edge14.getId());

	// Verify that `prevInEdge.setNextInEdgeId(nextInEdgeId)` succeeded
	EXPECT_EQ(newFirstEdge.getNextInEdgeId(), newSecondEdge.getId());

	// Verify that `nextInEdge.setPrevInEdgeId(prevInEdgeId)` succeeded
	EXPECT_EQ(newSecondEdge.getPrevInEdgeId(), newFirstEdge.getId());
}

TEST(RelationshipTraversalLifetimeTest, HandlesExpiredDataManagerGracefully) {
	// Use a null shared_ptr to guarantee dataManager_.lock() returns null
	std::shared_ptr<graph::storage::DataManager> nullDm;
	graph::traversal::RelationshipTraversal traversal(nullDm);

	// All methods should return empty results when DataManager is null
	std::vector<graph::Edge> outEdges;
	EXPECT_NO_THROW(outEdges = traversal.getOutgoingEdges(123));
	EXPECT_TRUE(outEdges.empty());

	std::vector<graph::Node> connectedNodes;
	EXPECT_NO_THROW(connectedNodes = traversal.getAllConnectedNodes(123));
	EXPECT_TRUE(connectedNodes.empty());
}

// ============================================================================
// Coverage Tests: Expired weak_ptr in getConnectedTargetNodes, getConnectedSourceNodes,
// getIncomingEdges, linkEdge, unlinkEdge
// ============================================================================

TEST(RelationshipTraversalLifetimeTest, ExpiredDataManager_GetIncomingEdges) {
	// Cover: dataManager_.lock() -> null in getIncomingEdges (line 63)
	std::shared_ptr<graph::storage::DataManager> nullDm;
	graph::traversal::RelationshipTraversal traversal(nullDm);

	std::vector<graph::Edge> inEdges;
	EXPECT_NO_THROW(inEdges = traversal.getIncomingEdges(123));
	EXPECT_TRUE(inEdges.empty());
}

TEST(RelationshipTraversalLifetimeTest, ExpiredDataManager_GetConnectedTargetNodes) {
	// Cover: dataManager_.lock() -> null in getConnectedTargetNodes (line 104-106)
	std::shared_ptr<graph::storage::DataManager> nullDm;
	graph::traversal::RelationshipTraversal traversal(nullDm);

	std::vector<graph::Node> targetNodes;
	EXPECT_NO_THROW(targetNodes = traversal.getConnectedTargetNodes(123));
	EXPECT_TRUE(targetNodes.empty());
}

TEST(RelationshipTraversalLifetimeTest, ExpiredDataManager_GetConnectedSourceNodes) {
	// Cover: dataManager_.lock() -> null in getConnectedSourceNodes (line 120-122)
	std::shared_ptr<graph::storage::DataManager> nullDm;
	graph::traversal::RelationshipTraversal traversal(nullDm);

	std::vector<graph::Node> sourceNodes;
	EXPECT_NO_THROW(sourceNodes = traversal.getConnectedSourceNodes(123));
	EXPECT_TRUE(sourceNodes.empty());
}

TEST(RelationshipTraversalLifetimeTest, ExpiredDataManager_LinkEdge) {
	// Cover: dataManager_.lock() -> null in linkEdge (line 161-163)
	std::shared_ptr<graph::storage::DataManager> nullDm;
	graph::traversal::RelationshipTraversal traversal(nullDm);

	graph::Edge edge(1, 1, 2, 1);
	EXPECT_NO_THROW(traversal.linkEdge(edge));
}

TEST(RelationshipTraversalLifetimeTest, ExpiredDataManager_UnlinkEdge) {
	// Cover: dataManager_.lock() -> null in unlinkEdge (line 204-207)
	std::shared_ptr<graph::storage::DataManager> nullDm;
	graph::traversal::RelationshipTraversal traversal(nullDm);

	graph::Edge edge(1, 1, 2, 1);
	EXPECT_NO_THROW(traversal.unlinkEdge(edge));
}

// ============================================================================
// Additional Branch Coverage Tests for RelationshipTraversal
// ============================================================================

TEST(RelationshipTraversalLifetimeTest, ExpiredDataManager_GetAllConnectedNodes) {
	// Cover: dataManager_.lock() -> null in getAllConnectedNodes (line 134-136)
	std::shared_ptr<graph::storage::DataManager> nullDm;
	graph::traversal::RelationshipTraversal traversal(nullDm);

	std::vector<graph::Node> allNodes;
	EXPECT_NO_THROW(allNodes = traversal.getAllConnectedNodes(123));
	EXPECT_TRUE(allNodes.empty());
}

TEST_F(RelationshipTraversalTest, LinkEdge_TargetNodeFirstInEdge) {
	// Cover: linkEdge when target node has firstInEdgeId == 0 (line 186-188)
	// and when it's not 0 (lines 189-198)
	// The basic SetUp already creates one edge (node1->node2), so node2's firstInEdgeId is non-zero

	// Add a new isolated node
	graph::Node node3(3, dataManager->getOrCreateLabelId("Node3"));
	dataManager->addNode(node3);

	// Add edge to node3 (node3 has no incoming edges, firstInEdgeId == 0)
	graph::Edge edge2(20, node1.getId(), node3.getId(), dataManager->getOrCreateLabelId("CONNECTS"));
	dataManager->addEdge(edge2);

	auto inEdges = traversal->getIncomingEdges(node3.getId());
	ASSERT_EQ(inEdges.size(), 1UL);
	EXPECT_EQ(inEdges[0].getId(), edge2.getId());
}

TEST_F(RelationshipTraversalTest, UnlinkEdge_MiddleInChain_IncomingEdges) {
	// Cover: unlinkEdge where prevInEdgeId != 0 and nextInEdgeId != 0 (lines 237-248)
	graph::Node node3(3, dataManager->getOrCreateLabelId("Node3"));
	graph::Node node4(4, dataManager->getOrCreateLabelId("Node4"));
	dataManager->addNode(node3);
	dataManager->addNode(node4);

	// Create multiple incoming edges to node2
	graph::Edge edge2(20, node3.getId(), node2.getId(), dataManager->getOrCreateLabelId("LINK"));
	dataManager->addEdge(edge2);

	graph::Edge edge3(30, node4.getId(), node2.getId(), dataManager->getOrCreateLabelId("LINK"));
	dataManager->addEdge(edge3);

	// Now node2 has incoming edges: [edge3, edge2, edge(10)]
	auto inBefore = traversal->getIncomingEdges(node2.getId());
	ASSERT_EQ(inBefore.size(), 3UL);

	// Unlink the middle edge (edge2)
	auto edgeToUnlink = dataManager->getEdge(edge2.getId());
	traversal->unlinkEdge(edgeToUnlink);

	auto inAfter = traversal->getIncomingEdges(node2.getId());
	ASSERT_EQ(inAfter.size(), 2UL);
}

TEST_F(RelationshipTraversalTest, GetConnectedSourceNodes_MultipleEdges) {
	// Cover: getConnectedSourceNodes loop with multiple incoming edges
	graph::Node node3(3, dataManager->getOrCreateLabelId("Node3"));
	graph::Node node4(4, dataManager->getOrCreateLabelId("Node4"));
	dataManager->addNode(node3);
	dataManager->addNode(node4);

	graph::Edge edge2(20, node3.getId(), node2.getId(), dataManager->getOrCreateLabelId("LINK"));
	dataManager->addEdge(edge2);

	graph::Edge edge3(30, node4.getId(), node2.getId(), dataManager->getOrCreateLabelId("LINK"));
	dataManager->addEdge(edge3);

	auto sourceNodes = traversal->getConnectedSourceNodes(node2.getId());
	ASSERT_EQ(sourceNodes.size(), 3UL);
}

TEST_F(RelationshipTraversalTest, GetAllConnectedNodes_DeduplicatesFromBothDirections) {
	// Cover: getAllConnectedNodes when incoming edges produce duplicate node IDs (line 149)
	// node1 -> node2 (existing edge)
	// Also add node2 -> node1 (reverse edge)
	graph::Edge reverseEdge(40, node2.getId(), node1.getId(), dataManager->getOrCreateLabelId("REVERSE"));
	dataManager->addEdge(reverseEdge);

	// From node1's perspective: outgoing to node2, incoming from node2
	// node2 should appear only once
	auto connectedNodes = traversal->getAllConnectedNodes(node1.getId());
	ASSERT_EQ(connectedNodes.size(), 1UL);
	EXPECT_EQ(connectedNodes[0].getId(), node2.getId());
}

// ============================================================================
// Additional Branch Coverage: Unlink tail edge from incoming chain
// ============================================================================

TEST_F(RelationshipTraversalAdvancedTest, UnlinkLastEdgeInIncomingChain) {
	// Cover: unlinkEdge where prevInEdgeId != 0 but nextInEdgeId == 0
	// Create multiple incoming edges to n2
	graph::Node n5(5, dataManager->getOrCreateLabelId("N5"));
	dataManager->addNode(n5);

	graph::Edge edgeA(50, n2.getId(), n5.getId(), dataManager->getOrCreateLabelId("TO"));
	dataManager->addEdge(edgeA);

	graph::Edge edgeB(51, n3.getId(), n5.getId(), dataManager->getOrCreateLabelId("TO"));
	dataManager->addEdge(edgeB);

	// n5 has incoming: [edgeB, edgeA] (LIFO order)
	auto inBefore = traversal->getIncomingEdges(n5.getId());
	ASSERT_EQ(inBefore.size(), 2UL);

	// Unlink the last edge in the chain (edgeA)
	auto edgeToUnlink = dataManager->getEdge(edgeA.getId());
	traversal->unlinkEdge(edgeToUnlink);

	auto inAfter = traversal->getIncomingEdges(n5.getId());
	ASSERT_EQ(inAfter.size(), 1UL);
	EXPECT_EQ(inAfter[0].getId(), edgeB.getId());

	// Verify the new tail has no next
	auto tailEdge = dataManager->getEdge(edgeB.getId());
	EXPECT_EQ(tailEdge.getNextInEdgeId(), 0);
}

TEST_F(RelationshipTraversalAdvancedTest, UnlinkFirstEdgeInIncomingChain) {
	// Cover: unlinkEdge where prevInEdgeId == 0 (head of incoming chain)
	graph::Node n5(5, dataManager->getOrCreateLabelId("N5"));
	dataManager->addNode(n5);

	graph::Edge edgeA(50, n2.getId(), n5.getId(), dataManager->getOrCreateLabelId("TO"));
	dataManager->addEdge(edgeA);

	graph::Edge edgeB(51, n3.getId(), n5.getId(), dataManager->getOrCreateLabelId("TO"));
	dataManager->addEdge(edgeB);

	// n5 has incoming: [edgeB, edgeA] (LIFO order)
	// Unlink the first (head) edge (edgeB)
	auto edgeToUnlink = dataManager->getEdge(edgeB.getId());
	traversal->unlinkEdge(edgeToUnlink);

	auto inAfter = traversal->getIncomingEdges(n5.getId());
	ASSERT_EQ(inAfter.size(), 1UL);
	EXPECT_EQ(inAfter[0].getId(), edgeA.getId());
}

TEST_F(RelationshipTraversalTest, GetConnectedTargetNodes_MultipleOutgoing) {
	// Cover: getConnectedTargetNodes loop with multiple outgoing edges
	graph::Node node3(3, dataManager->getOrCreateLabelId("Node3"));
	graph::Node node4(4, dataManager->getOrCreateLabelId("Node4"));
	dataManager->addNode(node3);
	dataManager->addNode(node4);

	graph::Edge edge2(20, node1.getId(), node3.getId(), dataManager->getOrCreateLabelId("LINK"));
	dataManager->addEdge(edge2);

	graph::Edge edge3(30, node1.getId(), node4.getId(), dataManager->getOrCreateLabelId("LINK"));
	dataManager->addEdge(edge3);

	auto targets = traversal->getConnectedTargetNodes(node1.getId());
	// Should have node2 (from setup edge) + node3 + node4
	ASSERT_EQ(targets.size(), 3UL);
}

TEST_F(RelationshipTraversalTest, GetAllConnectedEdges_BothDirections) {
	// Cover: getAllConnectedEdges with edges in both directions
	graph::Edge reverseEdge(50, node2.getId(), node1.getId(), dataManager->getOrCreateLabelId("BACK"));
	dataManager->addEdge(reverseEdge);

	auto allEdges = traversal->getAllConnectedEdges(node1.getId());
	// node1 has 1 outgoing (edge from setup) + 1 incoming (reverseEdge)
	ASSERT_EQ(allEdges.size(), 2UL);
}

// ============================================================================
// Additional Branch Coverage Tests for RelationshipTraversal.cpp
// ============================================================================

TEST_F(RelationshipTraversalTest, GetOutgoingEdges_MixActiveAndInactive) {
	// Cover: getOutgoingEdges where some edges in the chain are inactive (line 52-54)
	// The traversal should skip inactive edges but continue following the chain
	graph::Node node3(3, dataManager->getOrCreateLabelId("Node3"));
	dataManager->addNode(node3);

	graph::Edge edge2(20, node1.getId(), node3.getId(), dataManager->getOrCreateLabelId("LINK"));
	dataManager->addEdge(edge2);

	// Now node1 has outgoing edges: [edge2, edge(10)]
	auto outBefore = traversal->getOutgoingEdges(node1.getId());
	ASSERT_EQ(outBefore.size(), 2UL);

	// Delete edge2 (marks it inactive) - it remains in the chain but is inactive
	dataManager->deleteEdge(edge2);

	// Now traversal should skip the inactive edge2 and only return edge(10)
	auto outAfter = traversal->getOutgoingEdges(node1.getId());
	ASSERT_EQ(outAfter.size(), 1UL);
	EXPECT_EQ(outAfter[0].getId(), edge.getId());
}

TEST_F(RelationshipTraversalTest, GetIncomingEdges_MixActiveAndInactive) {
	// Cover: getIncomingEdges where some edges are inactive (line 82-84)
	graph::Node node3(3, dataManager->getOrCreateLabelId("Node3"));
	dataManager->addNode(node3);

	graph::Edge edge2(20, node3.getId(), node2.getId(), dataManager->getOrCreateLabelId("LINK"));
	dataManager->addEdge(edge2);

	// Now node2 has incoming edges: [edge2, edge(10)]
	auto inBefore = traversal->getIncomingEdges(node2.getId());
	ASSERT_EQ(inBefore.size(), 2UL);

	// Delete edge2 (marks it inactive)
	dataManager->deleteEdge(edge2);

	// Only the original edge should remain active
	auto inAfter = traversal->getIncomingEdges(node2.getId());
	ASSERT_EQ(inAfter.size(), 1UL);
	EXPECT_EQ(inAfter[0].getId(), edge.getId());
}

TEST_F(RelationshipTraversalTest, LinkEdge_SourceNodeFirstOutIsZero) {
	// Cover: linkEdge when source node's firstOutEdgeId == 0 (line 169-171)
	// Create a new isolated node with no edges
	graph::Node node3(3, dataManager->getOrCreateLabelId("Isolated"));
	dataManager->addNode(node3);

	// Create an edge from node3 (which has firstOutEdgeId == 0)
	graph::Edge newEdge(20, node3.getId(), node2.getId(), dataManager->getOrCreateLabelId("NEW"));
	dataManager->addEdge(newEdge); // internally calls linkEdge

	auto outEdges = traversal->getOutgoingEdges(node3.getId());
	ASSERT_EQ(outEdges.size(), 1UL);
	EXPECT_EQ(outEdges[0].getId(), newEdge.getId());

	// Verify node3's firstOutEdgeId was set
	auto updatedNode3 = dataManager->getNode(node3.getId());
	EXPECT_EQ(updatedNode3.getFirstOutEdgeId(), newEdge.getId());
}

TEST_F(RelationshipTraversalTest, LinkEdge_SourceNodeFirstOutIsNonZero) {
	// Cover: linkEdge when source node already has firstOutEdgeId != 0 (line 172-181)
	// node1 already has an outgoing edge, so adding another should trigger the else branch

	graph::Node node3(3, dataManager->getOrCreateLabelId("Node3"));
	dataManager->addNode(node3);

	graph::Edge newEdge(20, node1.getId(), node3.getId(), dataManager->getOrCreateLabelId("SECOND"));
	dataManager->addEdge(newEdge); // internally calls linkEdge

	auto outEdges = traversal->getOutgoingEdges(node1.getId());
	ASSERT_EQ(outEdges.size(), 2UL);

	// The new edge should be the first (LIFO)
	EXPECT_EQ(outEdges[0].getId(), newEdge.getId());
	EXPECT_EQ(outEdges[1].getId(), edge.getId());
}

TEST_F(RelationshipTraversalTest, UnlinkEdge_HeadOfOutgoingChain) {
	// Cover: unlinkEdge where prevOutEdgeId == 0 (head of outgoing chain, line 215-218)
	// node1 has one outgoing edge (the setup edge)
	// Unlinking it should set node1's firstOutEdgeId to nextOutEdgeId (0)
	traversal->unlinkEdge(edge);

	auto updatedNode1 = dataManager->getNode(node1.getId());
	EXPECT_EQ(updatedNode1.getFirstOutEdgeId(), 0);
}

TEST_F(RelationshipTraversalTest, UnlinkEdge_HeadOfIncomingChain) {
	// Cover: unlinkEdge where prevInEdgeId == 0 (head of incoming chain, line 234-236)
	// node2 has one incoming edge (the setup edge)
	// Unlinking it should set node2's firstInEdgeId to nextInEdgeId (0)
	traversal->unlinkEdge(edge);

	auto updatedNode2 = dataManager->getNode(node2.getId());
	EXPECT_EQ(updatedNode2.getFirstInEdgeId(), 0);
}

TEST_F(RelationshipTraversalTest, GetConnectedSourceNodes_EmptyOutgoing) {
	// Cover: getConnectedSourceNodes when node has no incoming edges
	// node1 has no incoming edges
	auto sourceNodes = traversal->getConnectedSourceNodes(node1.getId());
	EXPECT_TRUE(sourceNodes.empty());
}

TEST_F(RelationshipTraversalTest, GetConnectedTargetNodes_NoOutgoing) {
	// Cover: getConnectedTargetNodes when node has no outgoing edges
	// node2 has no outgoing edges
	auto targetNodes = traversal->getConnectedTargetNodes(node2.getId());
	EXPECT_TRUE(targetNodes.empty());
}

TEST_F(RelationshipTraversalTest, GetAllConnectedNodes_NoEdges) {
	// Cover: getAllConnectedNodes when node has no edges at all
	graph::Node node3(3, dataManager->getOrCreateLabelId("Isolated"));
	dataManager->addNode(node3);

	auto connectedNodes = traversal->getAllConnectedNodes(node3.getId());
	EXPECT_TRUE(connectedNodes.empty());
}

TEST_F(RelationshipTraversalAdvancedTest, UnlinkEdge_WithNextOutEdge) {
	// Cover: unlinkEdge where nextOutEdgeId != 0 (line 225-229)
	// In the advanced fixture, we have 3 edges: e12 -> e11 -> e10
	// Unlinking e12 (head) means nextOutEdgeId = e11 != 0
	// e11 should have its prevOutEdgeId set to 0
	auto edgeToUnlink = dataManager->getEdge(edge12.getId());
	traversal->unlinkEdge(edgeToUnlink);

	auto updatedE11 = dataManager->getEdge(edge11.getId());
	EXPECT_EQ(updatedE11.getPrevOutEdgeId(), 0) << "After unlinking head, next edge prev should be 0";
}

TEST_F(RelationshipTraversalAdvancedTest, UnlinkEdge_MiddleWithBothNeighbors) {
	// Cover: unlinkEdge where prevOutEdgeId != 0 AND nextOutEdgeId != 0 (lines 219-222, 225-229)
	// Unlink e11 (middle): e12 -> e11 -> e10 becomes e12 -> e10
	auto edgeToUnlink = dataManager->getEdge(edge11.getId());
	traversal->unlinkEdge(edgeToUnlink);

	auto updatedE12 = dataManager->getEdge(edge12.getId());
	auto updatedE10 = dataManager->getEdge(edge10.getId());

	EXPECT_EQ(updatedE12.getNextOutEdgeId(), edge10.getId());
	EXPECT_EQ(updatedE10.getPrevOutEdgeId(), edge12.getId());
}

// ============================================================================
// Additional Branch Coverage Tests - Round 5
// ============================================================================

TEST_F(RelationshipTraversalTest, GetOutgoingEdges_NoEdgesFromTarget) {
	// node2 has no outgoing edges - tests while loop not entered (line 43 false)
	auto outEdges = traversal->getOutgoingEdges(node2.getId());
	EXPECT_TRUE(outEdges.empty());
}

TEST_F(RelationshipTraversalTest, GetIncomingEdges_NoEdgesToSource) {
	// node1 has no incoming edges - tests while loop not entered (line 73 false)
	auto inEdges = traversal->getIncomingEdges(node1.getId());
	EXPECT_TRUE(inEdges.empty());
}

TEST_F(RelationshipTraversalTest, LinkEdge_TargetNodeAlreadyHasIncomingEdge) {
	// Cover: linkEdge else branch for target in-edge chain (lines 189-198)
	// node2 already has an incoming edge from setUp, add another
	graph::Node node3(3, dataManager->getOrCreateLabelId("Node3"));
	dataManager->addNode(node3);

	graph::Edge edge2(20, node3.getId(), node2.getId(), dataManager->getOrCreateLabelId("ALSO"));
	dataManager->addEdge(edge2); // internally calls linkEdge

	auto inEdges = traversal->getIncomingEdges(node2.getId());
	ASSERT_EQ(inEdges.size(), 2UL);

	// LIFO: edge2 should be first, original edge second
	EXPECT_EQ(inEdges[0].getId(), edge2.getId());
	EXPECT_EQ(inEdges[1].getId(), edge.getId());
}

TEST_F(RelationshipTraversalAdvancedTest, UnlinkEdge_LastEdge_NoNextOutEdge) {
	// Cover: unlinkEdge where nextOutEdgeId == 0 (line 225 false branch)
	// Unlink edge10, the tail of the chain
	auto edgeToUnlink = dataManager->getEdge(edge10.getId());
	traversal->unlinkEdge(edgeToUnlink);

	// The next-to-last edge (edge11) should now have nextOutEdgeId == 0
	auto updatedE11 = dataManager->getEdge(edge11.getId());
	EXPECT_EQ(updatedE11.getNextOutEdgeId(), 0);
}

TEST_F(RelationshipTraversalAdvancedTest, UnlinkEdge_LastEdge_NoNextInEdge) {
	// Cover: unlinkEdge where nextInEdgeId == 0 (line 244 false branch)
	// for incoming chain - n2 has edge10 as its only incoming edge
	auto edgeToUnlink = dataManager->getEdge(edge10.getId());
	traversal->unlinkEdge(edgeToUnlink);

	// After unlinking, n2 should have no incoming edges
	auto inEdges = traversal->getIncomingEdges(n2.getId());
	EXPECT_TRUE(inEdges.empty());
}

// ============================================================================
// Branch coverage: null weak_ptr (expired DataManager) for all methods
// ============================================================================

TEST(RelationshipTraversalNullPtrTest, GetOutgoingEdges_NullDataManager) {
	// Cover: dataManager_.lock() returns null in getOutgoingEdges (line 33 true branch)
	std::shared_ptr<graph::storage::DataManager> nullPtr;
	graph::traversal::RelationshipTraversal traversal(nullPtr);

	auto result = traversal.getOutgoingEdges(1);
	EXPECT_TRUE(result.empty());
}

TEST(RelationshipTraversalNullPtrTest, GetIncomingEdges_NullDataManager) {
	// Cover: dataManager_.lock() returns null in getIncomingEdges (line 63 true branch)
	std::shared_ptr<graph::storage::DataManager> nullPtr;
	graph::traversal::RelationshipTraversal traversal(nullPtr);

	auto result = traversal.getIncomingEdges(1);
	EXPECT_TRUE(result.empty());
}

TEST(RelationshipTraversalNullPtrTest, GetConnectedTargetNodes_NullDataManager) {
	// Cover: dataManager_.lock() returns null in getConnectedTargetNodes (line 105 true branch)
	std::shared_ptr<graph::storage::DataManager> nullPtr;
	graph::traversal::RelationshipTraversal traversal(nullPtr);

	auto result = traversal.getConnectedTargetNodes(1);
	EXPECT_TRUE(result.empty());
}

TEST(RelationshipTraversalNullPtrTest, GetConnectedSourceNodes_NullDataManager) {
	// Cover: dataManager_.lock() returns null in getConnectedSourceNodes (line 121 true branch)
	std::shared_ptr<graph::storage::DataManager> nullPtr;
	graph::traversal::RelationshipTraversal traversal(nullPtr);

	auto result = traversal.getConnectedSourceNodes(1);
	EXPECT_TRUE(result.empty());
}

TEST(RelationshipTraversalNullPtrTest, GetAllConnectedNodes_NullDataManager) {
	// Cover: dataManager_.lock() returns null in getAllConnectedNodes (line 135 true branch)
	std::shared_ptr<graph::storage::DataManager> nullPtr;
	graph::traversal::RelationshipTraversal traversal(nullPtr);

	auto result = traversal.getAllConnectedNodes(1);
	EXPECT_TRUE(result.empty());
}

TEST(RelationshipTraversalNullPtrTest, LinkEdge_NullDataManager) {
	// Cover: dataManager_.lock() returns null in linkEdge (line 162 true branch)
	std::shared_ptr<graph::storage::DataManager> nullPtr;
	graph::traversal::RelationshipTraversal traversal(nullPtr);

	graph::Edge e(1, 1, 2, 1);
	EXPECT_NO_THROW(traversal.linkEdge(e));
}

TEST(RelationshipTraversalNullPtrTest, UnlinkEdge_NullDataManager) {
	// Cover: dataManager_.lock() returns null in unlinkEdge (line 205 true branch)
	std::shared_ptr<graph::storage::DataManager> nullPtr;
	graph::traversal::RelationshipTraversal traversal(nullPtr);

	graph::Edge e(1, 1, 2, 1);
	EXPECT_NO_THROW(traversal.unlinkEdge(e));
}

// ============================================================================
// Branch coverage: inactive edge in incoming chain (line 82 false branch)
// ============================================================================

TEST_F(RelationshipTraversalTest, GetIncomingEdges_InactiveEdgeInChainSkipped) {
	// Cover: getIncomingEdges where edge.isActive() is false (line 82 false branch)
	// We need an inactive edge that is still linked in the incoming chain.
	// To achieve this, we retrieve the edge preserving its linked list pointers,
	// mark it inactive, then register it as a dirty "modified" entity with the
	// inactive copy as the backup. When getEdge is called during traversal,
	// the dirty info returns this backup, which is inactive but has valid pointers.

	// Add a second incoming edge to node2
	graph::Node node3(3, dataManager->getOrCreateLabelId("Node3"));
	dataManager->addNode(node3);

	graph::Edge edge2(20, node3.getId(), node2.getId(), dataManager->getOrCreateLabelId("LINK"));
	dataManager->addEdge(edge2);

	// Incoming chain for node2: [edge2(20), edge(10)]
	auto inBefore = traversal->getIncomingEdges(node2.getId());
	ASSERT_EQ(inBefore.size(), 2UL);

	// Get the edge from storage (preserves linked list pointers), mark inactive,
	// and register it as a dirty modified entity so getEdge returns it with pointers intact.
	auto storedEdge2 = dataManager->getEdge(edge2.getId());
	storedEdge2.markInactive();
	graph::storage::DirtyEntityInfo<graph::Edge> dirtyInfo(
			graph::storage::EntityChangeType::CHANGE_MODIFIED, storedEdge2);
	dataManager->setEntityDirty<graph::Edge>(dirtyInfo);

	// Now traverse: edge2 is still in the chain but inactive, should be skipped
	auto inAfter = traversal->getIncomingEdges(node2.getId());
	ASSERT_EQ(inAfter.size(), 1UL);
	EXPECT_EQ(inAfter[0].getId(), edge.getId());
}

