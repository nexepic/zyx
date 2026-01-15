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

	auto deletedEdges = getDirtyInfo({graph::storage::EntityChangeType::DELETED});
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

	auto deletedEdgesInfo = getDirtyInfo({graph::storage::EntityChangeType::DELETED});
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
