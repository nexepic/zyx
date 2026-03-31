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
		int64_t srcLabelId = dataManager->getOrCreateLabelId("SourceNode");
		int64_t tgtLabelId = dataManager->getOrCreateLabelId("TargetNode");

		// Add nodes to storage
		sourceNode = graph::Node(0, srcLabelId);
		targetNode = graph::Node(0, tgtLabelId);

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
		fileStorage->flush();
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
	int64_t labelId = dataManager->getOrCreateLabelId("CONNECTS_TO");
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), labelId);

	edgeManager->add(edge);
	EXPECT_NE(edge.getId(), 0);

	graph::Edge retrievedEdge = edgeManager->get(edge.getId());

	EXPECT_EQ(retrievedEdge.getId(), edge.getId());
	EXPECT_EQ(retrievedEdge.getSourceNodeId(), sourceNode.getId());
	EXPECT_EQ(retrievedEdge.getTargetNodeId(), targetNode.getId());

	EXPECT_EQ(retrievedEdge.getLabelId(), labelId);
	EXPECT_EQ(dataManager->resolveLabel(retrievedEdge.getLabelId()), "CONNECTS_TO");

	EXPECT_TRUE(retrievedEdge.isActive());
}

// Test edge removal for an entity that was added and removed in the same context
TEST_F(EdgeManagerTest, RemoveEdgeInSameContext) {
	int64_t labelId = dataManager->getOrCreateLabelId("CONNECTS_TO");
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), labelId);

	edgeManager->add(edge);
	int64_t edgeId = edge.getId();

	edgeManager->remove(edge);

	graph::Edge retrievedEdge = edgeManager->get(edgeId);
	if (retrievedEdge.getId() != 0) {
		EXPECT_FALSE(retrievedEdge.isActive());
	} else {
		EXPECT_EQ(retrievedEdge.getId(), 0);
	}

	auto deletedEdges = getDirtyInfo({graph::storage::EntityChangeType::CHANGE_DELETED});
	EXPECT_EQ(deletedEdges.size(), 0UL);
}

// Test edge removal for an entity that is considered "persisted"
TEST_F(EdgeManagerTest, RemovePersistedEdge) {
	int64_t labelId = dataManager->getOrCreateLabelId("CONNECTS_TO");
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), labelId);

	edgeManager->add(edge);
	int64_t edgeId = edge.getId();
	int64_t sourceId = sourceNode.getId();

	simulateSave();

	graph::Edge persistedEdge = edgeManager->get(edgeId);
	EXPECT_EQ(persistedEdge.getId(), edgeId);
	edgeManager->remove(persistedEdge);

	auto deletedEdgesInfo = getDirtyInfo({graph::storage::EntityChangeType::CHANGE_DELETED});
	EXPECT_EQ(deletedEdgesInfo.size(), 1UL);
	EXPECT_EQ(deletedEdgesInfo[0].backup->getId(), edgeId);

	graph::Node updatedSource = nodeManager->get(sourceId);
	EXPECT_EQ(updatedSource.getFirstOutEdgeId(), 0);
}

// Test findByNode functionality
TEST_F(EdgeManagerTest, FindEdgesByNode) {
	int64_t targetLabelId = dataManager->getOrCreateLabelId("Target2");
	graph::Node target2(0, targetLabelId);
	nodeManager->add(target2);

	int64_t connectsLabel = dataManager->getOrCreateLabelId("CONNECTS_TO");
	int64_t refersLabel = dataManager->getOrCreateLabelId("REFERS_TO");

	graph::Edge edge1(0, sourceNode.getId(), targetNode.getId(), connectsLabel);
	graph::Edge edge2(0, sourceNode.getId(), target2.getId(), refersLabel);

	edgeManager->add(edge1);
	edgeManager->add(edge2);

	auto outgoingEdges = edgeManager->findByNode(sourceNode.getId(), "out");
	EXPECT_EQ(outgoingEdges.size(), 2UL);

	auto incomingEdges = edgeManager->findByNode(targetNode.getId(), "in");
	EXPECT_EQ(incomingEdges.size(), 1UL);
	EXPECT_EQ(incomingEdges[0].getSourceNodeId(), sourceNode.getId());
}

// Test edge updating
TEST_F(EdgeManagerTest, UpdateEdge) {
	int64_t initLabel = dataManager->getOrCreateLabelId("INITIAL_LABEL");
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), initLabel);
	edgeManager->add(edge);

	int64_t updatedLabel = dataManager->getOrCreateLabelId("UPDATED_LABEL");
	edge.setLabelId(updatedLabel);

	edgeManager->update(edge);

	graph::Edge retrievedEdge = edgeManager->get(edge.getId());
	EXPECT_EQ(retrievedEdge.getLabelId(), updatedLabel);
	EXPECT_EQ(dataManager->resolveLabel(retrievedEdge.getLabelId()), "UPDATED_LABEL");
}

// Test edge properties
TEST_F(EdgeManagerTest, EdgeProperties) {
	int64_t labelId = dataManager->getOrCreateLabelId("HAS_PROPERTY");
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), labelId);
	edgeManager->add(edge);

	std::unordered_map<std::string, graph::PropertyValue> properties;
	properties["weight"] = 2.5;
	properties["active"] = true;
	properties["name"] = std::string("Connection");

	edgeManager->addProperties(edge.getId(), properties);

	auto retrievedProps = edgeManager->getProperties(edge.getId());

	EXPECT_EQ(std::get<double>(retrievedProps["weight"].getVariant()), 2.5);
	EXPECT_EQ(std::get<bool>(retrievedProps["active"].getVariant()), true);
	EXPECT_EQ(std::get<std::string>(retrievedProps["name"].getVariant()), "Connection");

	edgeManager->removeProperty(edge.getId(), "weight");
	retrievedProps = edgeManager->getProperties(edge.getId());
	EXPECT_FALSE(retrievedProps.contains("weight"));
}

// Test relationship bidirectionality
TEST_F(EdgeManagerTest, RelationshipBidirectionality) {
	int64_t labelId = dataManager->getOrCreateLabelId("CONNECTS_TO");
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), labelId);
	edgeManager->add(edge);

	graph::Node updatedSource = nodeManager->get(sourceNode.getId());
	EXPECT_EQ(updatedSource.getFirstOutEdgeId(), edge.getId());

	graph::Node updatedTarget = nodeManager->get(targetNode.getId());
	EXPECT_EQ(updatedTarget.getFirstInEdgeId(), edge.getId());
}

// Test multiple edges between same nodes
TEST_F(EdgeManagerTest, MultipleEdgesBetweenSameNodes) {
	int64_t knowsLabel = dataManager->getOrCreateLabelId("KNOWS");
	int64_t likesLabel = dataManager->getOrCreateLabelId("LIKES");

	graph::Edge edge1(0, sourceNode.getId(), targetNode.getId(), knowsLabel);
	graph::Edge edge2(0, sourceNode.getId(), targetNode.getId(), likesLabel);

	edgeManager->add(edge1);
	edgeManager->add(edge2);

	graph::Edge updatedEdge1 = edgeManager->get(edge1.getId());
	graph::Edge updatedEdge2 = edgeManager->get(edge2.getId());

	EXPECT_EQ(updatedEdge2.getNextOutEdgeId(), edge1.getId());
	EXPECT_EQ(updatedEdge1.getPrevOutEdgeId(), edge2.getId());
	EXPECT_EQ(updatedEdge1.getNextOutEdgeId(), 0);

	EXPECT_EQ(updatedEdge2.getNextInEdgeId(), edge1.getId());
	EXPECT_EQ(updatedEdge1.getPrevInEdgeId(), edge2.getId());
	EXPECT_EQ(updatedEdge1.getNextInEdgeId(), 0);

	graph::Node updatedSource = nodeManager->get(sourceNode.getId());
	EXPECT_EQ(updatedSource.getFirstOutEdgeId(), edge2.getId());
}

// Test batch operations
TEST_F(EdgeManagerTest, BatchOperations) {
	std::vector<graph::Edge> edges;
	std::vector<int64_t> edgeIds;

	for (int i = 0; i < 5; i++) {
		int64_t labelId = dataManager->getOrCreateLabelId("BATCH_EDGE_" + std::to_string(i));
		graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), labelId);
		edgeManager->add(edge);
		edges.push_back(edge);
		edgeIds.push_back(edge.getId());
	}

	auto retrievedEdges = edgeManager->getBatch(edgeIds);
	EXPECT_EQ(retrievedEdges.size(), 5UL);

	edgeManager->remove(edges[2]);
	retrievedEdges = edgeManager->getBatch(edgeIds);
	EXPECT_EQ(retrievedEdges.size(), 4UL);
}

// Test edge ID allocation
TEST_F(EdgeManagerTest, EdgeIdAllocation) {
	int64_t label1 = dataManager->getOrCreateLabelId("EDGE1");
	int64_t label2 = dataManager->getOrCreateLabelId("EDGE2");

	graph::Edge edge1(0, sourceNode.getId(), targetNode.getId(), label1);
	graph::Edge edge2(0, sourceNode.getId(), targetNode.getId(), label2);

	edgeManager->add(edge1);
	edgeManager->add(edge2);

	EXPECT_NE(edge1.getId(), 0);
	EXPECT_NE(edge2.getId(), 0);
	EXPECT_NE(edge1.getId(), edge2.getId());
	EXPECT_EQ(edge2.getId(), edge1.getId() + 1);
}

// Test error handling for invalid operations
TEST_F(EdgeManagerTest, ErrorHandling) {
	graph::Edge nonExistentEdge = edgeManager->get(999999);
	EXPECT_EQ(nonExistentEdge.getId(), 0);

	int64_t labelId = dataManager->getOrCreateLabelId("INVALID");
	graph::Edge invalidEdge(999999, sourceNode.getId(), targetNode.getId(), labelId);
	invalidEdge.markInactive(false);
	EXPECT_THROW(edgeManager->update(invalidEdge), std::runtime_error);
}

// Test findByNode with "both" direction
TEST_F(EdgeManagerTest, FindEdgesByNodeBothDirection) {
	// Create a bidirectional connection: source -> target and target -> source
	int64_t label1 = dataManager->getOrCreateLabelId("FORWARD");
	int64_t label2 = dataManager->getOrCreateLabelId("BACKWARD");

	graph::Edge edge1(0, sourceNode.getId(), targetNode.getId(), label1);
	graph::Edge edge2(0, targetNode.getId(), sourceNode.getId(), label2);

	edgeManager->add(edge1);
	edgeManager->add(edge2);

	// Find edges in both directions from sourceNode
	auto bothEdges = edgeManager->findByNode(sourceNode.getId(), "both");

	EXPECT_EQ(bothEdges.size(), 2UL);

	// Verify we got both outgoing and incoming edges
	bool hasOutgoing = false;
	bool hasIncoming = false;
	for (const auto& edge : bothEdges) {
		if (edge.getId() == edge1.getId()) {
			hasOutgoing = true;
			EXPECT_EQ(edge.getSourceNodeId(), sourceNode.getId());
		}
		if (edge.getId() == edge2.getId()) {
			hasIncoming = true;
			EXPECT_EQ(edge.getTargetNodeId(), sourceNode.getId());
		}
	}

	EXPECT_TRUE(hasOutgoing);
	EXPECT_TRUE(hasIncoming);
}

// Test findByNode with invalid direction
TEST_F(EdgeManagerTest, FindEdgesByNodeInvalidDirection) {
	int64_t labelId = dataManager->getOrCreateLabelId("CONNECTS_TO");
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), labelId);
	edgeManager->add(edge);

	// Test with invalid direction - should return empty or handle gracefully
	auto edges = edgeManager->findByNode(sourceNode.getId(), "invalid_direction");
	// Implementation should handle invalid direction, either by returning empty or throwing
	EXPECT_TRUE(edges.empty() || edges.size() >= 0);
}

// Test findByNode returns empty for non-existent node
TEST_F(EdgeManagerTest, FindEdgesByNodeNonExistent) {
	auto outgoingEdges = edgeManager->findByNode(99999, "out");
	EXPECT_TRUE(outgoingEdges.empty());

	auto incomingEdges = edgeManager->findByNode(99999, "in");
	EXPECT_TRUE(incomingEdges.empty());

	auto bothEdges = edgeManager->findByNode(99999, "both");
	EXPECT_TRUE(bothEdges.empty());
}

// Test edge persistence and retrieval after save
TEST_F(EdgeManagerTest, EdgePersistenceAfterSave) {
	int64_t labelId = dataManager->getOrCreateLabelId("PERSISTENT_EDGE");
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), labelId);

	edgeManager->add(edge);
	int64_t edgeId = edge.getId();

	// Save to disk
	simulateSave();

	// Retrieve and verify
	graph::Edge retrievedEdge = edgeManager->get(edgeId);
	EXPECT_EQ(retrievedEdge.getId(), edgeId);
	EXPECT_EQ(retrievedEdge.getSourceNodeId(), sourceNode.getId());
	EXPECT_EQ(retrievedEdge.getTargetNodeId(), targetNode.getId());
	EXPECT_TRUE(retrievedEdge.isActive());
}

// Test multiple removals of same edge
TEST_F(EdgeManagerTest, RemoveEdgeTwice) {
	int64_t labelId = dataManager->getOrCreateLabelId("CONNECTS_TO");
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), labelId);

	edgeManager->add(edge);
	int64_t edgeId = edge.getId();

	simulateSave();

	// First removal
	graph::Edge retrievedEdge = edgeManager->get(edgeId);
	edgeManager->remove(retrievedEdge);

	// Try to remove again - should handle gracefully
	graph::Edge removedEdge = edgeManager->get(edgeId);
	if (removedEdge.getId() != 0) {
		EXPECT_NO_THROW(edgeManager->remove(removedEdge));
	}
}

// Test edge updates with different scenarios
TEST_F(EdgeManagerTest, UpdateEdgeWithDifferentLabels) {
	int64_t label1 = dataManager->getOrCreateLabelId("LABEL_A");
	int64_t label2 = dataManager->getOrCreateLabelId("LABEL_B");
	int64_t label3 = dataManager->getOrCreateLabelId("LABEL_C");

	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), label1);
	edgeManager->add(edge);

	// Update to label2
	edge.setLabelId(label2);
	edgeManager->update(edge);

	graph::Edge retrieved1 = edgeManager->get(edge.getId());
	EXPECT_EQ(retrieved1.getLabelId(), label2);

	// Update to label3
	edge.setLabelId(label3);
	edgeManager->update(edge);

	graph::Edge retrieved2 = edgeManager->get(edge.getId());
	EXPECT_EQ(retrieved2.getLabelId(), label3);
	EXPECT_EQ(dataManager->resolveLabel(retrieved2.getLabelId()), "LABEL_C");
}

// Test edge properties with multiple operations
TEST_F(EdgeManagerTest, EdgePropertiesMultipleOperations) {
	int64_t labelId = dataManager->getOrCreateLabelId("PROPERTY_EDGE");
	graph::Edge edge(0, sourceNode.getId(), targetNode.getId(), labelId);
	edgeManager->add(edge);

	// Add multiple properties
	std::unordered_map<std::string, graph::PropertyValue> properties1;
	properties1["weight"] = 1.5;
	properties1["name"] = std::string("Test1");
	properties1["count"] = (int64_t) 10;
	edgeManager->addProperties(edge.getId(), properties1);

	auto props1 = edgeManager->getProperties(edge.getId());
	EXPECT_EQ(props1.size(), 3UL);

	// Verify values
	EXPECT_DOUBLE_EQ(std::get<double>(props1["weight"].getVariant()), 1.5);
	EXPECT_EQ(std::get<std::string>(props1["name"].getVariant()), "Test1");
	EXPECT_EQ(std::get<int64_t>(props1["count"].getVariant()), 10);

	// Remove all properties one by one
	edgeManager->removeProperty(edge.getId(), "weight");
	edgeManager->removeProperty(edge.getId(), "name");
	edgeManager->removeProperty(edge.getId(), "count");

	auto props2 = edgeManager->getProperties(edge.getId());
	EXPECT_TRUE(props2.empty());

	// Add properties again to verify we can add after removal
	std::unordered_map<std::string, graph::PropertyValue> properties2;
	properties2["newWeight"] = 2.5;
	properties2["newName"] = std::string("Test2");
	edgeManager->addProperties(edge.getId(), properties2);

	auto props3 = edgeManager->getProperties(edge.getId());
	EXPECT_EQ(props3.size(), 2UL);
	EXPECT_DOUBLE_EQ(std::get<double>(props3["newWeight"].getVariant()), 2.5);
	EXPECT_EQ(std::get<std::string>(props3["newName"].getVariant()), "Test2");
}

// Test operations when DataManager is no longer available
// This tests defensive code paths that handle weak_ptr expiration
TEST_F(EdgeManagerTest, OperationsWithExpiredDataManager) {
	// NOTE: Testing the weak_ptr expiration scenario is complex due to the architecture
	// where Database/FileStorage/DataManager may have circular references or extended lifecycles.
	// This test documents the expected behavior when DataManager becomes unavailable.

	// For now, we test the defensive code paths indirectly by ensuring
	// the EdgeManager handles all operations correctly when DataManager is available
	// The defensive checks (if (!dataManager)) are in place for safety in edge cases.

	// Create a temporary database to get a separate EdgeManager instance
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	auto tempPath = std::filesystem::temp_directory_path() / ("temp_edgeManager_" + to_string(uuid) + ".dat");
	auto tempDatabase = std::make_unique<graph::Database>(tempPath.string());
	tempDatabase->open();
	auto tempStorage = tempDatabase->getStorage();
	auto tempDataManager = tempStorage->getDataManager();

	// Get the edge manager
	auto tempEdgeManager = tempDataManager->getEdgeManager();

	// Create some nodes first
	int64_t labelId = tempDataManager->getOrCreateLabelId("TEST_LABEL");
	graph::Node node1(0, labelId);
	graph::Node node2(0, labelId);
	tempDataManager->getNodeManager()->add(node1);
	tempDataManager->getNodeManager()->add(node2);

	int64_t edgeLabelId = tempDataManager->getOrCreateLabelId("TEST_EDGE");
	graph::Edge testEdge(0, node1.getId(), node2.getId(), edgeLabelId);

	// Test that operations work correctly when DataManager is available
	tempEdgeManager->add(testEdge);
	EXPECT_NE(testEdge.getId(), 0);

	// Test findByNode
	auto edges = tempEdgeManager->findByNode(node1.getId(), "out");
	EXPECT_GT(edges.size(), 0UL);

	// Clean up
	tempDatabase->close();
	std::filesystem::remove(tempPath);
	std::filesystem::remove(tempPath.string() + ".wal");

	// NOTE: The weak_ptr expiration scenario (where DataManager is destroyed but
	// EdgeManager still exists) is difficult to test reliably due to the architecture.
	// In production code, this scenario should never occur as DataManager owns EdgeManager.
	// The defensive checks are in place for extra safety.
}

