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
	static graph::Node createTestNode(const std::string &label) {
		graph::Node node;
		node.setLabel(label);
		return node;
	}

	// Helper to create an Edge using the correct API
	static graph::Edge createTestEdge(int64_t sourceId, int64_t targetId, const std::string &label) {
		graph::Edge edge;
		edge.setSourceNodeId(sourceId);
		edge.setTargetNodeId(targetId);
		edge.setLabel(label);
		return edge;
	}

	// New Helper: Get dirty entities from DataManager using new API
	template<typename T>
	std::vector<T> getDirtyEntities(const std::vector<graph::storage::EntityChangeType> &types) {
		auto infos = dataManager->getDirtyEntityInfos<T>(types);
		std::vector<T> result;
		for (const auto &info: infos) {
			if (info.backup.has_value()) {
				result.push_back(*info.backup);
			}
		}
		return result;
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

	// Remove the node
	nodeManager->remove(node);

	// Test retrieval from memory (Dirty Layer)
	// The entity should still be retrievable from dirty memory but marked inactive
	// OR depending on implementation, get() might return empty/inactive if DELETED.
	// PersistenceManager::get() returns backup if present.
	// Let's check if it's considered "deleted" by checking active status
	graph::Node retrievedNode = nodeManager->get(nodeId);
	EXPECT_EQ(retrievedNode.getId(), 0) << "Removed node should return empty entity (ID 0) from memory.";

	// Test retrieval from disk after cache clear
	// Since we haven't flushed, disk should have nothing or old state.
	// But since it's a new node never saved, disk has nothing.
	dataManager->clearCache();
	graph::Node retrievedFromDisk = dataManager->loadNodeFromDisk(nodeId);
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

	// Mark one node as inactive
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
	graph::Node node = createTestNode("PropertyNode");
	nodeManager->add(node);
	int64_t nodeId = node.getId();

	std::unordered_map<std::string, graph::PropertyValue> props;
	props["name"] = std::string("TestName");
	props["age"] = 30;
	props["active"] = true;
	nodeManager->addProperties(nodeId, props);

	auto retrievedProps = nodeManager->getProperties(nodeId);

	EXPECT_EQ(retrievedProps.size(), 3);
	EXPECT_EQ(std::get<std::string>(retrievedProps["name"].getVariant()), "TestName");
	EXPECT_EQ(std::get<int64_t>(retrievedProps["age"].getVariant()), 30);
	EXPECT_EQ(std::get<bool>(retrievedProps["active"].getVariant()), true);

	nodeManager->removeProperty(nodeId, "age");

	retrievedProps = nodeManager->getProperties(nodeId);
	EXPECT_EQ(retrievedProps.size(), 2);
	EXPECT_EQ(retrievedProps.count("age"), 0);
}

// Tests for Edge entity
TEST_F(BaseEntityManagerTest, EdgeEntityOperations) {
	graph::Node sourceNode = createTestNode("Source");
	graph::Node targetNode = createTestNode("Target");
	nodeManager->add(sourceNode);
	nodeManager->add(targetNode);

	graph::Edge edge = createTestEdge(sourceNode.getId(), targetNode.getId(), "CONNECTS_TO");
	edgeManager->add(edge);

	graph::Edge retrievedEdge = edgeManager->get(edge.getId());
	EXPECT_EQ(retrievedEdge.getId(), edge.getId());
	EXPECT_EQ(retrievedEdge.getSourceNodeId(), sourceNode.getId());
	EXPECT_EQ(retrievedEdge.getTargetNodeId(), targetNode.getId());
	EXPECT_EQ(retrievedEdge.getLabel(), "CONNECTS_TO");

	edge.setLabel("RELATED_TO");
	edgeManager->update(edge);

	retrievedEdge = edgeManager->get(edge.getId());
	EXPECT_EQ(retrievedEdge.getLabel(), "RELATED_TO");

	edgeManager->remove(edge);

	// Check deletion status
	retrievedEdge = edgeManager->get(edge.getId());
	EXPECT_EQ(retrievedEdge.getId(), 0) << "Removed edge should return empty entity (ID 0).";

	dataManager->clearCache();
	auto retrievedFromDisk = dataManager->loadEdgeFromDisk(edge.getId());
	EXPECT_EQ(retrievedFromDisk.getId(), 0);
}

// UPDATED: Tests for PersistenceManager Snapshot Commit (Replacing MarkAllSaved)
TEST_F(BaseEntityManagerTest, SnapshotCommitClearsDirty) {
	// Create and add multiple nodes
	for (int i = 0; i < 3; i++) {
		graph::Node node = createTestNode("SavedNode" + std::to_string(i));
		nodeManager->add(node);
	}

	// 1. Prepare Snapshot (Move to Flushing Layer)
	auto snapshot = dataManager->prepareFlushSnapshot();

	// 2. Commit Snapshot (Clear Flushing Layer)
	dataManager->commitFlushSnapshot();

	// Verify dirty list is empty
	// Using new API helper getDirtyEntities
	auto dirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::ADDED,
													 graph::storage::EntityChangeType::MODIFIED,
													 graph::storage::EntityChangeType::DELETED});

	EXPECT_TRUE(dirtyNodes.empty());
}

// UPDATED: Tests for getDirtyEntityInfos (Replacing getDirtyWithChangeTypes)
TEST_F(BaseEntityManagerTest, GetDirtyWithChangeTypes) {
	// --- Setup ---

	// 1. ADDED
	graph::Node addedNode = createTestNode("AddedNode");
	nodeManager->add(addedNode);

	// 2. ADDED then MODIFIED (Should effectively be ADDED in dirty list for new entity)
	// Note: Our new PersistenceManager logic might report this as ADDED if logic handles it,
	// OR MODIFIED depending on implementation.
	// DataManager::updateEntityImpl logic: if already ADDED, keep as ADDED.
	graph::Node modifiedNode = createTestNode("OriginalLabel");
	nodeManager->add(modifiedNode);
	modifiedNode.setLabel("ModifiedLabel");
	nodeManager->update(modifiedNode);

	// 3. ADDED then REMOVED (Should effectively be DELETED or disappear)
	// DataManager::markEntityDeleted logic: if ADDED, mark DELETED (and maybe remove from map logic?)
	// In our implementation: upsert(DELETED).
	graph::Node removedNode = createTestNode("RemovedNode");
	nodeManager->add(removedNode);
	nodeManager->remove(removedNode);

	// --- Assertions ---

	// Get only ADDED nodes
	auto addedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::ADDED});
	// Depending on strict logic:
	// addedNode -> ADDED
	// modifiedNode -> ADDED (because updateEntityImpl preserved ADDED state)
	EXPECT_EQ(addedNodes.size(), 2);

	// Verify IDs
	std::set<int64_t> addedIds;
	for (auto &n: addedNodes)
		addedIds.insert(n.getId());
	EXPECT_TRUE(addedIds.count(addedNode.getId()));
	EXPECT_TRUE(addedIds.count(modifiedNode.getId()));


	// Get only MODIFIED nodes
	auto modifiedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::MODIFIED});
	EXPECT_EQ(modifiedNodes.size(), 0);


	// Get only DELETED nodes
	// removedNode -> Was ADDED then marked DELETED.
	auto deletedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::DELETED});
	// In current implementation, we upsert a DELETED record.
	EXPECT_EQ(deletedNodes.size(), 1);
	EXPECT_EQ(deletedNodes[0].getId(), removedNode.getId());

	// Get all dirty nodes. Total = 3 (2 Added + 1 Deleted)
	auto allDirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::ADDED,
														graph::storage::EntityChangeType::MODIFIED,
														graph::storage::EntityChangeType::DELETED});
	EXPECT_EQ(allDirtyNodes.size(), 3);
}
