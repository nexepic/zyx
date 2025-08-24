/**
 * @file test_BaseEntityManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/8/15
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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
#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/BaseEntityManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/EdgeManager.hpp"
#include "graph/storage/data/EntityTraits.hpp"
#include "graph/storage/data/NodeManager.hpp"

class BaseEntityManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_baseEntityManager_" + to_string(uuid) + ".dat");

		// Initialize Database and FileStorage
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();

		// Get the entity managers
		nodeManager = dataManager->getNodeManager();
		edgeManager = dataManager->getEdgeManager();
	}

	void TearDown() override {
		database->close();
		database.reset();
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	// Helper to create a Node using the correct API
	graph::Node createTestNode(const std::string &label) {
		graph::Node node;
		node.setLabel(label);
		return node;
	}

	// Helper to create an Edge using the correct API
	graph::Edge createTestEdge(int64_t sourceId, int64_t targetId, const std::string &label) {
		graph::Edge edge;
		edge.setSourceNodeId(sourceId);
		edge.setTargetNodeId(targetId);
		edge.setLabel(label);
		return edge;
	}


	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::storage::NodeManager> nodeManager;
	std::shared_ptr<graph::storage::EdgeManager> edgeManager;
};

// Tests for add method
TEST_F(BaseEntityManagerTest, AddNodeEntity) {
	graph::Node node = createTestNode("TestNode");
	nodeManager->add(node);

	// Verify ID was allocated
	EXPECT_NE(node.getId(), 0);

	// Verify entity can be retrieved
	graph::Node retrievedNode = nodeManager->get(node.getId());
	EXPECT_EQ(retrievedNode.getId(), node.getId());
	EXPECT_EQ(retrievedNode.getLabel(), "TestNode");
	EXPECT_TRUE(retrievedNode.isActive());
}

// Tests for update method
TEST_F(BaseEntityManagerTest, UpdateNodeEntity) {
	// Create and add a node
	graph::Node node = createTestNode("OriginalLabel");
	nodeManager->add(node);
	int64_t nodeId = node.getId();

	// Modify and update the node
	node.setLabel("UpdatedLabel");
	nodeManager->update(node);

	// Verify changes were applied
	graph::Node retrievedNode = nodeManager->get(nodeId);
	EXPECT_EQ(retrievedNode.getLabel(), "UpdatedLabel");
}

// Tests for remove method
TEST_F(BaseEntityManagerTest, RemoveNodeEntity) {
	// Create and add a node
	graph::Node node = createTestNode("NodeToRemove");
	nodeManager->add(node);
	int64_t nodeId = node.getId();

	// Remove the node, which should mark it as inactive
	nodeManager->remove(node);

	// Getting the node from memory might still return the object, but it should be inactive
	graph::Node retrievedNode = nodeManager->get(nodeId);
	// The entity itself is marked inactive, but a "get" might still return the object
	// A better test is to clear cache and see it is not loaded from disk
	dataManager->clearCache();
	graph::Node retrievedFromDisk = dataManager->getNode(nodeId);
	EXPECT_EQ(retrievedFromDisk.getId(), 0) << "Deleted node should not be retrievable from disk.";
}

// Tests for get method
TEST_F(BaseEntityManagerTest, GetNonExistentNode) {
	graph::Node node = nodeManager->get(999999);

	// Should return a default-constructed node with ID 0
	EXPECT_EQ(node.getId(), 0);
}

// Tests for getBatch method
TEST_F(BaseEntityManagerTest, GetBatchOfNodes) {
	// Create and add multiple nodes
	std::vector<int64_t> nodeIds;
	for (int i = 0; i < 5; i++) {
		graph::Node node = createTestNode("BatchNode" + std::to_string(i));
		nodeManager->add(node);
		nodeIds.push_back(node.getId());
	}

	// Mark one node as inactive using the correct remove method
	graph::Node nodeToRemove = nodeManager->get(nodeIds[2]);
	nodeManager->remove(nodeToRemove);

	// Get batch of nodes
	std::vector<graph::Node> nodes = nodeManager->getBatch(nodeIds);

	// Should only return active nodes
	EXPECT_EQ(nodes.size(), 4);

	// Verify correct nodes were returned
	for (const auto &node: nodes) {
		EXPECT_TRUE(node.isActive());
		EXPECT_NE(node.getId(), nodeIds[2]);
	}
}

// Tests for getInRange method
TEST_F(BaseEntityManagerTest, GetNodesInRange) {
	// Create nodes with sequential IDs. We can't guarantee sequential IDs from the allocator,
	// so we create a known range first.
	std::vector<int64_t> nodeIds;
	int64_t startId = 0;
	for (int i = 0; i < 10; i++) {
		graph::Node node = createTestNode("RangeNode" + std::to_string(i));
		nodeManager->add(node);
		nodeIds.push_back(node.getId());
		if (i == 0)
			startId = node.getId();
	}

	// Get nodes in range
	std::vector<graph::Node> nodes = nodeManager->getInRange(startId + 2, startId + 6, 10);

	// Verify correct range was returned
	EXPECT_EQ(nodes.size(), 5);
	for (const auto &node: nodes) {
		EXPECT_GE(node.getId(), startId + 2);
		EXPECT_LE(node.getId(), startId + 6);
	}

	// Test with limit
	nodes = nodeManager->getInRange(startId + 2, startId + 6, 2);
	EXPECT_EQ(nodes.size(), 2);
}

// Tests for property management
TEST_F(BaseEntityManagerTest, NodePropertyManagement) {
	// Create a node
	graph::Node node = createTestNode("PropertyNode");
	nodeManager->add(node);
	int64_t nodeId = node.getId();

	// Add properties
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["name"] = std::string("TestName");
	props["age"] = int64_t(30);
	props["active"] = true;
	nodeManager->addProperties(nodeId, props);

	// Get properties
	auto retrievedProps = nodeManager->getProperties(nodeId);

	// Verify properties
	EXPECT_EQ(retrievedProps.size(), 3);
	EXPECT_EQ(std::get<std::string>(retrievedProps["name"].getVariant()), "TestName");
	EXPECT_EQ(std::get<int64_t>(retrievedProps["age"].getVariant()), 30);
	EXPECT_EQ(std::get<bool>(retrievedProps["active"].getVariant()), true);

	// Remove a property
	nodeManager->removeProperty(nodeId, "age");

	// Verify property was removed
	retrievedProps = nodeManager->getProperties(nodeId);
	EXPECT_EQ(retrievedProps.size(), 2);
	EXPECT_EQ(retrievedProps.count("age"), 0);
}

// Tests for Edge entity to verify template works with different entity types
TEST_F(BaseEntityManagerTest, EdgeEntityOperations) {
	// Create source and target nodes
	graph::Node sourceNode = createTestNode("Source");
	graph::Node targetNode = createTestNode("Target");
	nodeManager->add(sourceNode);
	nodeManager->add(targetNode);

	// Create an edge
	graph::Edge edge = createTestEdge(sourceNode.getId(), targetNode.getId(), "CONNECTS_TO");
	edgeManager->add(edge);

	// Verify edge was added correctly
	graph::Edge retrievedEdge = edgeManager->get(edge.getId());
	EXPECT_EQ(retrievedEdge.getId(), edge.getId());
	EXPECT_EQ(retrievedEdge.getSourceNodeId(), sourceNode.getId());
	EXPECT_EQ(retrievedEdge.getTargetNodeId(), targetNode.getId());
	EXPECT_EQ(retrievedEdge.getLabel(), "CONNECTS_TO");

	// Update edge
	edge.setLabel("RELATED_TO");
	edgeManager->update(edge);

	// Verify edge was updated
	retrievedEdge = edgeManager->get(edge.getId());
	EXPECT_EQ(retrievedEdge.getLabel(), "RELATED_TO");

	// Remove edge
	edgeManager->remove(edge);

	// Getting the edge from memory might still return it, but it should be inactive
	retrievedEdge = edgeManager->get(edge.getId());
	// A better test for removal is checking if it's gone from disk after clearing cache
	dataManager->clearCache();
	auto retrievedFromDisk = dataManager->getEdge(edge.getId());
	EXPECT_EQ(retrievedFromDisk.getId(), 0) << "Deleted edge should not be retrievable from disk.";
}

// Tests for markAllSaved method
TEST_F(BaseEntityManagerTest, MarkAllSaved) {
	// Create and add multiple nodes
	for (int i = 0; i < 3; i++) {
		graph::Node node = createTestNode("SavedNode" + std::to_string(i));
		nodeManager->add(node);
	}

	// Mark all as saved
	dataManager->markAllSaved();

	// Verify dirty list is empty
	auto dirtyNodes = dataManager->getDirtyEntitiesWithChangeTypes<graph::Node>(
			{graph::storage::EntityChangeType::ADDED, graph::storage::EntityChangeType::MODIFIED,
			 graph::storage::EntityChangeType::DELETED});

	EXPECT_TRUE(dirtyNodes.empty());
}

// Tests for getDirtyEntitiesWithChangeTypes method
TEST_F(BaseEntityManagerTest, GetDirtyWithChangeTypes) {
	// --- Setup ---

	// 1. Create and add a node. Its state should be ADDED.
	graph::Node addedNode = createTestNode("AddedNode");
	nodeManager->add(addedNode);

	// 2. Create, add, then update a node. Its state should remain ADDED because
	//    it's still a new entity within this transaction and has no on-disk counterpart.
	graph::Node modifiedNode = createTestNode("OriginalLabel");
	nodeManager->add(modifiedNode);
	modifiedNode.setLabel("ModifiedLabel");
	nodeManager->update(modifiedNode);

	// 3. Create, add, then remove a node. This entity should effectively vanish
	//    from the dirty map, as its creation and deletion cancel each other out
	//    within the same transaction.
	graph::Node removedNode = createTestNode("RemovedNode");
	nodeManager->add(removedNode);
	nodeManager->remove(removedNode);

	// --- Assertions ---

	// Get only ADDED nodes. We expect two: addedNode and modifiedNode.
	auto addedNodes =
			dataManager->getDirtyEntitiesWithChangeTypes<graph::Node>({graph::storage::EntityChangeType::ADDED});
	EXPECT_EQ(addedNodes.size(), 2);

	// Verify the correct nodes are in the added list. The order is not guaranteed.
	bool foundAddedNode = false;
	bool foundModifiedNode = false;
	for (const auto &node: addedNodes) {
		if (node.getId() == addedNode.getId())
			foundAddedNode = true;
		if (node.getId() == modifiedNode.getId())
			foundModifiedNode = true;
	}
	EXPECT_TRUE(foundAddedNode);
	EXPECT_TRUE(foundModifiedNode);


	// Get only MODIFIED nodes. We expect zero, as no pre-existing entity was updated.
	auto modifiedNodes =
			dataManager->getDirtyEntitiesWithChangeTypes<graph::Node>({graph::storage::EntityChangeType::MODIFIED});
	EXPECT_EQ(modifiedNodes.size(), 0);


	// Get only DELETED nodes. We expect zero, as removedNode never existed on disk.
	auto deletedNodes =
			dataManager->getDirtyEntitiesWithChangeTypes<graph::Node>({graph::storage::EntityChangeType::DELETED});
	EXPECT_EQ(deletedNodes.size(), 0);


	// Get all dirty nodes. Should only contain the two ADDED nodes.
	auto allDirtyNodes = dataManager->getDirtyEntitiesWithChangeTypes<graph::Node>(
			{graph::storage::EntityChangeType::ADDED, graph::storage::EntityChangeType::MODIFIED,
			 graph::storage::EntityChangeType::DELETED});
	EXPECT_EQ(allDirtyNodes.size(), 2);
}
