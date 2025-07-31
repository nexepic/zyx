/**
 * @file test_RelationshipTraversal.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/30
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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
		node1 = graph::Node(1, "Node1");
		node2 = graph::Node(2, "Node2");
		dataManager->addNode(node1);
		dataManager->addNode(node2);

		// Create and add a single edge between them.
		// dataManager->addEdge() internally calls linkEdge(), establishing the initial state.
		edge = graph::Edge(10, node1.getId(), node2.getId(), "RELATED_TO");
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
	ASSERT_EQ(outEdges.size(), 1) << "Node1 should have one outgoing edge.";
	EXPECT_EQ(outEdges[0].getId(), edge.getId());

	auto inEdges = traversal->getIncomingEdges(node2.getId());
	ASSERT_EQ(inEdges.size(), 1) << "Node2 should have one incoming edge.";
	EXPECT_EQ(inEdges[0].getId(), edge.getId());
}

TEST_F(RelationshipTraversalTest, GetAllConnectedEdgesForSingleEdge) {
	auto allEdgesA = traversal->getAllConnectedEdges(node1.getId());
	EXPECT_EQ(allEdgesA.size(), 1);

	auto allEdgesB = traversal->getAllConnectedEdges(node2.getId());
	EXPECT_EQ(allEdgesB.size(), 1);
}

TEST_F(RelationshipTraversalTest, GetConnectedNodesForSingleEdge) {
	auto targets = traversal->getConnectedTargetNodes(node1.getId());
	ASSERT_EQ(targets.size(), 1);
	EXPECT_EQ(targets[0].getId(), node2.getId());

	auto sources = traversal->getConnectedSourceNodes(node2.getId());
	ASSERT_EQ(sources.size(), 1);
	EXPECT_EQ(sources[0].getId(), node1.getId());
}

TEST_F(RelationshipTraversalTest, GetAllConnectedNodesForSingleEdge) {
	auto connectedA = traversal->getAllConnectedNodes(node1.getId());
	ASSERT_EQ(connectedA.size(), 1);
	EXPECT_EQ(connectedA[0].getId(), node2.getId());

	auto connectedB = traversal->getAllConnectedNodes(node2.getId());
	ASSERT_EQ(connectedB.size(), 1);
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
	graph::Node node3(3, "IsolatedNode");
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
	graph::Edge edge2(11, node1.getId(), node2.getId(), "KNOWS");
	dataManager->addEdge(edge2);

	// We have two edges pointing to node2, but should only get one node back
	auto connectedNodes = traversal->getAllConnectedNodes(node1.getId());
	ASSERT_EQ(connectedNodes.size(), 1);
	EXPECT_EQ(connectedNodes[0].getId(), node2.getId());
}

TEST_F(RelationshipTraversalTest, SelfReferencingEdge) {
	graph::Node node3(3, "SelfReferencingNode");
	dataManager->addNode(node3);

	graph::Edge selfEdge(12, node3.getId(), node3.getId(), "LOOPS_ON");
	dataManager->addEdge(selfEdge);

	auto outEdges = traversal->getOutgoingEdges(node3.getId());
	ASSERT_EQ(outEdges.size(), 1);
	EXPECT_EQ(outEdges[0].getId(), selfEdge.getId());

	auto inEdges = traversal->getIncomingEdges(node3.getId());
	ASSERT_EQ(inEdges.size(), 1);
	EXPECT_EQ(inEdges[0].getId(), selfEdge.getId());

	auto allNodes = traversal->getAllConnectedNodes(node3.getId());
	ASSERT_EQ(allNodes.size(), 1);
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
		n1 = graph::Node(1, "N1");
		n2 = graph::Node(2, "N2");
		n3 = graph::Node(3, "N3");
		n4 = graph::Node(4, "N4");
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
		edge10 = graph::Edge(10, n1.getId(), n2.getId(), "points_to");
		edge11 = graph::Edge(11, n1.getId(), n3.getId(), "points_to");
		edge12 = graph::Edge(12, n1.getId(), n4.getId(), "points_to");
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
	ASSERT_EQ(outEdges.size(), 3);

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
	ASSERT_EQ(outEdges.size(), 2);
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
	ASSERT_EQ(outEdges.size(), 2);

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
	ASSERT_EQ(outEdges.size(), 2);
	EXPECT_EQ(outEdges[0].getId(), edge12.getId());
	EXPECT_EQ(outEdges[1].getId(), edge11.getId());

	// Verify the new last edge in the list (edge11) has no next edge
	auto newLastEdge = dataManager->getEdge(edge11.getId());
	EXPECT_EQ(newLastEdge.getNextOutEdgeId(), 0);
}

TEST_F(RelationshipTraversalAdvancedTest, UnlinkMiddleEdgeFromIncomingChain) {
	graph::Node n5(5, "N5");
	dataManager->addNode(n5);

	graph::Edge edge14(14, n1.getId(), n5.getId(), "links_to");
	dataManager->addEdge(edge14);

	graph::Edge edge24(24, n2.getId(), n5.getId(), "links_to");
	dataManager->addEdge(edge24);

	graph::Edge edge34(34, n3.getId(), n5.getId(), "links_to");
	dataManager->addEdge(edge34);

	auto initialInEdges = traversal->getIncomingEdges(n5.getId());
	ASSERT_EQ(initialInEdges.size(), 3);
	ASSERT_EQ(initialInEdges[0].getId(), edge34.getId());
	ASSERT_EQ(initialInEdges[1].getId(), edge24.getId());
	ASSERT_EQ(initialInEdges[2].getId(), edge14.getId());

	// Remove the middle edge edge24.
	auto edgeToUnlink = dataManager->getEdge(edge24.getId());
	traversal->unlinkEdge(edgeToUnlink);

	// a. Check the edges currently in the chain
	auto finalInEdges = traversal->getIncomingEdges(n5.getId());
	ASSERT_EQ(finalInEdges.size(), 2);
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
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	auto testFilePath = std::filesystem::temp_directory_path() / ("test_lifetime_" + to_string(uuid) + ".dat");

	// 1. Create a DataManager whose lifetime we can fully control
	auto database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	std::shared_ptr<graph::storage::DataManager> dataManager = database->getStorage()->getDataManager();

	// 2. Create Traversal, whose internal weak_ptr now points to our DataManager
	auto traversal = std::make_shared<graph::traversal::RelationshipTraversal>(dataManager);

	// 3. Key step: destroy the DataManager.
	// We reset the shared_ptr to bring the reference count to zero, triggering its destructor.
	// Now, the weak_ptr inside traversal is expired (dangling).
	dataManager.reset();
	database->close();

	// 4. Call methods on Traversal.
	// Inside these methods, `dataManager_.lock()` will fail and return a null pointer.
	// We expect these calls to complete safely, throw no exceptions, and return empty results.
	std::vector<graph::Edge> outEdges;
	EXPECT_NO_THROW(outEdges = traversal->getOutgoingEdges(123));
	EXPECT_TRUE(outEdges.empty());

	std::vector<graph::Node> connectedNodes;
	EXPECT_NO_THROW(connectedNodes = traversal->getAllConnectedNodes(123));
	EXPECT_TRUE(connectedNodes.empty());

	std::filesystem::remove(testFilePath);
}
