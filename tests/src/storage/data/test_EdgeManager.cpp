/**
 * @file test_EdgeManager.cpp
 * @author Nexepic
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EdgeManager.hpp"
#include "graph/storage/data/NodeManager.hpp"

class EdgeManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_edgeManager_" + to_string(uuid) + ".dat");

		// Initialize Database and FileStorage
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
		deletionManager = dataManager->getDeletionManager();

		// Create edge and node managers
		edgeManager = dataManager->getEdgeManager();
		nodeManager = dataManager->getNodeManager();

		// Create some test nodes
		createTestNodes();
	}

	void TearDown() override {
		database->close();
		database.reset();
		std::filesystem::remove(testFilePath);
	}

	void createTestNodes() {
		// Create source and target nodes for edges
		sourceNode = graph::Node(0, "SourceNode");
		targetNode = graph::Node(0, "TargetNode");

		// Add nodes to storage
		nodeManager->add(sourceNode);
		nodeManager->add(targetNode);
	}

	// Helper to get dirty edges
	[[nodiscard]] std::vector<graph::storage::DirtyEntityInfo<graph::Edge>>
	getDirtyInfo(const std::vector<graph::storage::EntityChangeType> &types) const {
		return dataManager->getDirtyEntityInfos<graph::Edge>(types);
	}

	// Helper to simulate "Save" (Flush to disk and clear dirty state)
	void simulateSave() const {
		(void) dataManager->prepareFlushSnapshot();
		dataManager->commitFlushSnapshot();
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::storage::DeletionManager> deletionManager;
	std::shared_ptr<graph::storage::EdgeManager> edgeManager;
	std::shared_ptr<graph::storage::NodeManager> nodeManager;

	graph::Node sourceNode;
	graph::Node targetNode;
};

// Test basic edge creation and retrieval
TEST_F(EdgeManagerTest, AddAndGetEdge) {
	// Create an edge between source and target nodes
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), "CONNECTS_TO");

	// Add the edge
	edgeManager->add(edge);
	EXPECT_NE(edge.getId(), 0) << "Edge should have been assigned an ID";

	// Retrieve the edge
	graph::Edge retrievedEdge = edgeManager->get(edge.getId());

	// Verify edge properties
	EXPECT_EQ(retrievedEdge.getId(), edge.getId());
	EXPECT_EQ(retrievedEdge.getSourceNodeId(), sourceNode.getId());
	EXPECT_EQ(retrievedEdge.getTargetNodeId(), targetNode.getId());
	EXPECT_EQ(retrievedEdge.getLabel(), "CONNECTS_TO");
	EXPECT_TRUE(retrievedEdge.isActive());
}

// Test edge removal for an entity that was added and removed in the same context
TEST_F(EdgeManagerTest, RemoveEdgeInSameContext) {
	// Create and add an edge
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), "CONNECTS_TO");
	edgeManager->add(edge);
	int64_t edgeId = edge.getId();

	// Immediately remove the edge
	edgeManager->remove(edge);

	// Verify the edge is no longer retrievable
	graph::Edge retrievedEdge = edgeManager->get(edgeId);
	// get() may return an inactive object or an empty object.
	// If it's effectively "gone" from both maps (added then deleted), it might be unretrievable (ID=0)
	// OR it might be in map marked as DELETED.
	// Assuming DataManager logic: upsert(DELETED).
	// If it's DELETED, isActive() should be false.
	if (retrievedEdge.getId() != 0) {
		EXPECT_FALSE(retrievedEdge.isActive());
	} else {
		EXPECT_EQ(retrievedEdge.getId(), 0);
	}

	auto deletedEdges = getDirtyInfo({graph::storage::EntityChangeType::DELETED});

	EXPECT_EQ(deletedEdges.size(), 0UL);
}

// Test edge removal for an entity that is considered "persisted"
TEST_F(EdgeManagerTest, RemovePersistedEdge) {
	// Create and add an edge
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), "CONNECTS_TO");
	edgeManager->add(edge);
	int64_t edgeId = edge.getId();
	int64_t sourceId = sourceNode.getId();

	// --- Simulate the entity being in a "persisted" state ---
	simulateSave(); // Replaces markAllSaved()

	// --- State after simulated persistence ---
	// The edge now exists in the cache (or disk), but is not in the dirty map.

	// Remove the "persisted" edge
	graph::Edge persistedEdge = edgeManager->get(edgeId);
	EXPECT_EQ(persistedEdge.getId(), edgeId) << "Edge should be retrievable.";
	edgeManager->remove(persistedEdge);

	// Verify it is now in the DELETED dirty map
	auto deletedEdgesInfo = getDirtyInfo({graph::storage::EntityChangeType::DELETED});
	EXPECT_EQ(deletedEdgesInfo.size(), 1UL) << "One edge should be marked as DELETED.";
	EXPECT_EQ(deletedEdgesInfo[0].backup->getId(), edgeId);
	EXPECT_FALSE(deletedEdgesInfo[0].backup->isActive()) << "The backup of the deleted edge should be inactive.";

	// Verify that the source node's reference to the edge is cleared
	graph::Node updatedSource = nodeManager->get(sourceId);
	EXPECT_EQ(updatedSource.getFirstOutEdgeId(), 0) << "Source node should no longer reference the removed edge.";
}

// Test findByNode functionality
TEST_F(EdgeManagerTest, FindEdgesByNode) {
	// Create multiple edges from source to different targets
	graph::Node target2(0, "Target2");
	nodeManager->add(target2);

	graph::Edge edge1(0, sourceNode.getId(), targetNode.getId(), "CONNECTS_TO");
	graph::Edge edge2(0, sourceNode.getId(), target2.getId(), "REFERS_TO");

	edgeManager->add(edge1);
	edgeManager->add(edge2);

	// Find outgoing edges from source node
	auto outgoingEdges = edgeManager->findByNode(sourceNode.getId(), "out");
	EXPECT_EQ(outgoingEdges.size(), 2UL) << "Should find 2 outgoing edges from source node";

	// Find incoming edges to target node
	auto incomingEdges = edgeManager->findByNode(targetNode.getId(), "in");
	EXPECT_EQ(incomingEdges.size(), 1UL) << "Should find 1 incoming edge to target node";
	EXPECT_EQ(incomingEdges[0].getSourceNodeId(), sourceNode.getId());
}

// Test edge updating
TEST_F(EdgeManagerTest, UpdateEdge) {
	// Create and add an edge
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), "INITIAL_LABEL");
	edgeManager->add(edge);

	// Update the edge label
	edge.setLabel("UPDATED_LABEL");
	edgeManager->update(edge);

	// Verify the update
	graph::Edge retrievedEdge = edgeManager->get(edge.getId());
	EXPECT_EQ(retrievedEdge.getLabel(), "UPDATED_LABEL") << "Edge label should be updated";
}

// Test edge properties
TEST_F(EdgeManagerTest, EdgeProperties) {
	// Create and add an edge
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), "HAS_PROPERTY");
	edgeManager->add(edge);

	// Add properties
	std::unordered_map<std::string, graph::PropertyValue> properties;
	properties["weight"] = 2.5;
	properties["active"] = true;
	properties["name"] = std::string("Connection");

	edgeManager->addProperties(edge.getId(), properties);

	// Retrieve properties
	auto retrievedProps = edgeManager->getProperties(edge.getId());

	// Verify properties
	EXPECT_EQ(std::get<double>(retrievedProps["weight"].getVariant()), 2.5);
	EXPECT_EQ(std::get<bool>(retrievedProps["active"].getVariant()), true);
	EXPECT_EQ(std::get<std::string>(retrievedProps["name"].getVariant()), "Connection");

	// Remove a property
	edgeManager->removeProperty(edge.getId(), "weight");
	retrievedProps = edgeManager->getProperties(edge.getId());
	EXPECT_FALSE(retrievedProps.contains("weight")) << "Property should be removed";
}

// Test relationship bidirectionality
TEST_F(EdgeManagerTest, RelationshipBidirectionality) {
	// Create and add an edge
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), "CONNECTS_TO");
	edgeManager->add(edge);

	// Verify the source node has the edge as outgoing
	graph::Node updatedSource = nodeManager->get(sourceNode.getId());
	EXPECT_EQ(updatedSource.getFirstOutEdgeId(), edge.getId()) << "Source node should reference edge as outgoing";

	// Verify the target node has the edge as incoming
	graph::Node updatedTarget = nodeManager->get(targetNode.getId());
	EXPECT_EQ(updatedTarget.getFirstInEdgeId(), edge.getId()) << "Target node should reference edge as incoming";
}

// Test multiple edges between same nodes
TEST_F(EdgeManagerTest, MultipleEdgesBetweenSameNodes) {
	// Create multiple edges between the same nodes
	graph::Edge edge1(0, sourceNode.getId(), targetNode.getId(), "KNOWS");
	graph::Edge edge2(0, sourceNode.getId(), targetNode.getId(), "LIKES");

	edgeManager->add(edge1);
	edgeManager->add(edge2);

	// The linking logic PREPENDS new edges to the list.
	// So, after adding edge2, the chain should be: sourceNode -> edge2 -> edge1
	// We need to fetch the updated states of both edges from the manager.

	graph::Edge updatedEdge1 = edgeManager->get(edge1.getId());
	graph::Edge updatedEdge2 = edgeManager->get(edge2.getId());

	// Verify the outgoing chain: edge2 should point to edge1.
	EXPECT_EQ(updatedEdge2.getNextOutEdgeId(), edge1.getId())
			<< "Second edge should link to the first edge (outgoing).";
	EXPECT_EQ(updatedEdge1.getPrevOutEdgeId(), edge2.getId())
			<< "First edge's prev should be the second edge (outgoing).";
	EXPECT_EQ(updatedEdge1.getNextOutEdgeId(), 0) << "First edge should be the end of the outgoing chain.";

	// Verify the incoming chain: edge2 should point to edge1.
	EXPECT_EQ(updatedEdge2.getNextInEdgeId(), edge1.getId()) << "Second edge should link to the first edge (incoming).";
	EXPECT_EQ(updatedEdge1.getPrevInEdgeId(), edge2.getId())
			<< "First edge's prev should be the second edge (incoming).";
	EXPECT_EQ(updatedEdge1.getNextInEdgeId(), 0) << "First edge should be the end of the incoming chain.";

	// Also verify the node now points to edge2 as the head of the list.
	graph::Node updatedSource = nodeManager->get(sourceNode.getId());
	EXPECT_EQ(updatedSource.getFirstOutEdgeId(), edge2.getId()) << "Source node should point to the newest edge.";
}

// Test batch operations
TEST_F(EdgeManagerTest, BatchOperations) {
	// Create multiple edges
	std::vector<graph::Edge> edges;
	std::vector<int64_t> edgeIds;

	for (int i = 0; i < 5; i++) {
		graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), "BATCH_EDGE_" + std::to_string(i));
		edgeManager->add(edge);
		edges.push_back(edge);
		edgeIds.push_back(edge.getId());
	}

	// Test getBatch
	auto retrievedEdges = edgeManager->getBatch(edgeIds);
	EXPECT_EQ(retrievedEdges.size(), 5UL) << "Should retrieve all 5 edges";

	// Test with removing one edge
	edgeManager->remove(edges[2]);
	retrievedEdges = edgeManager->getBatch(edgeIds);
	EXPECT_EQ(retrievedEdges.size(), 4UL) << "Should retrieve only active edges";
}

// Test edge ID allocation
TEST_F(EdgeManagerTest, EdgeIdAllocation) {
	// Create multiple edges and verify IDs are unique and allocated sequentially.
	graph::Edge edge1(0, sourceNode.getId(), targetNode.getId(), "EDGE1");
	graph::Edge edge2(0, sourceNode.getId(), targetNode.getId(), "EDGE2");

	edgeManager->add(edge1);
	edgeManager->add(edge2);

	// --- Verification ---

	// 1. Check that IDs are valid (non-zero).
	EXPECT_NE(edge1.getId(), 0);
	EXPECT_NE(edge2.getId(), 0);

	// 2. Check for uniqueness.
	EXPECT_NE(edge1.getId(), edge2.getId());

	// 3. Check for sequential allocation.
	EXPECT_EQ(edge2.getId(), edge1.getId() + 1);
}

// Test error handling for invalid operations
TEST_F(EdgeManagerTest, ErrorHandling) {
	// Try to get a non-existent edge
	graph::Edge nonExistentEdge = edgeManager->get(999999);
	EXPECT_EQ(nonExistentEdge.getId(), 0) << "Non-existent edge should return default edge with ID 0";

	// This test should now pass because of the new validation in BaseEntityManager::update.
	graph::Edge invalidEdge(999999, sourceNode.getId(), targetNode.getId(), "INVALID");
	invalidEdge.markInactive(false); // Ensure it's active to bypass the first check
	EXPECT_THROW(edgeManager->update(invalidEdge), std::runtime_error);
}
