/**
 * @file test_BaseEntityManager.cpp
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
	static graph::Node createTestNode(const std::shared_ptr<graph::storage::DataManager> &dm, const std::string &label) {
		graph::Node node;
		node.setLabelId(dm->getOrCreateLabelId(label));
		return node;
	}

	// Helper to create an Edge using the correct API
	static graph::Edge createTestEdge(const std::shared_ptr<graph::storage::DataManager> &dm, int64_t sourceId, int64_t targetId, const std::string &label) {
		graph::Edge edge;
		edge.setSourceNodeId(sourceId);
		edge.setTargetNodeId(targetId);
		edge.setLabelId(dm->getOrCreateLabelId(label));
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
	graph::Node node = createTestNode(dataManager, "TestNode");
	nodeManager->add(node);

	EXPECT_NE(node.getId(), 0);

	graph::Node retrievedNode = nodeManager->get(node.getId());
	EXPECT_EQ(retrievedNode.getId(), node.getId());

	EXPECT_EQ(retrievedNode.getLabelId(), node.getLabelId());
	EXPECT_EQ(dataManager->resolveLabel(retrievedNode.getLabelId()), "TestNode");

	EXPECT_TRUE(retrievedNode.isActive());
}

// Tests for update method
TEST_F(BaseEntityManagerTest, UpdateNodeEntity) {
	graph::Node node = createTestNode(dataManager, "OriginalLabel");
	nodeManager->add(node);
	int64_t nodeId = node.getId();

	node.setLabelId(dataManager->getOrCreateLabelId("UpdatedLabel"));
	nodeManager->update(node);

	graph::Node retrievedNode = nodeManager->get(nodeId);
	EXPECT_EQ(dataManager->resolveLabel(retrievedNode.getLabelId()), "UpdatedLabel");
}

// Tests for remove method
TEST_F(BaseEntityManagerTest, RemoveNodeEntity) {
	// Create and add a node
	graph::Node node = createTestNode(dataManager, "NodeToRemove");
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
		graph::Node node = createTestNode(dataManager, "BatchNode" + std::to_string(i));
		nodeManager->add(node);
		nodeIds.push_back(node.getId());
	}

	// Mark one node as inactive
	graph::Node nodeToRemove = nodeManager->get(nodeIds[2]);
	nodeManager->remove(nodeToRemove);

	// Get batch of nodes
	std::vector<graph::Node> nodes = nodeManager->getBatch(nodeIds);

	// Should only return active nodes
	EXPECT_EQ(nodes.size(), 4UL);

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
		graph::Node node = createTestNode(dataManager, "RangeNode" + std::to_string(i));
		nodeManager->add(node);
		nodeIds.push_back(node.getId());
		if (i == 0)
			startId = node.getId();
	}

	// Get nodes in range
	std::vector<graph::Node> nodes = nodeManager->getInRange(startId + 2, startId + 6, 10);

	// Verify correct range was returned
	EXPECT_EQ(nodes.size(), 5UL);
	for (const auto &node: nodes) {
		EXPECT_GE(node.getId(), startId + 2);
		EXPECT_LE(node.getId(), startId + 6);
	}

	// Test with limit
	nodes = nodeManager->getInRange(startId + 2, startId + 6, 2);
	EXPECT_EQ(nodes.size(), 2UL);
}

// Tests for property management
TEST_F(BaseEntityManagerTest, NodePropertyManagement) {
	graph::Node node = createTestNode(dataManager, "PropertyNode");
	nodeManager->add(node);
	int64_t nodeId = node.getId();

	std::unordered_map<std::string, graph::PropertyValue> props;
	props["name"] = std::string("TestName");
	props["age"] = 30;
	props["active"] = true;
	nodeManager->addProperties(nodeId, props);

	auto retrievedProps = nodeManager->getProperties(nodeId);

	EXPECT_EQ(retrievedProps.size(), 3UL);
	EXPECT_EQ(std::get<std::string>(retrievedProps["name"].getVariant()), "TestName");
	EXPECT_EQ(std::get<int64_t>(retrievedProps["age"].getVariant()), 30);
	EXPECT_EQ(std::get<bool>(retrievedProps["active"].getVariant()), true);

	nodeManager->removeProperty(nodeId, "age");

	retrievedProps = nodeManager->getProperties(nodeId);
	EXPECT_EQ(retrievedProps.size(), 2UL);
	EXPECT_EQ(retrievedProps.count("age"), 0UL);
}

// Tests for Edge entity
TEST_F(BaseEntityManagerTest, EdgeEntityOperations) {
	graph::Node sourceNode = createTestNode(dataManager, "Source");
	graph::Node targetNode = createTestNode(dataManager, "Target");
	nodeManager->add(sourceNode);
	nodeManager->add(targetNode);

	graph::Edge edge = createTestEdge(dataManager, sourceNode.getId(), targetNode.getId(), "CONNECTS_TO");
	edgeManager->add(edge);

	graph::Edge retrievedEdge = edgeManager->get(edge.getId());
	EXPECT_EQ(retrievedEdge.getId(), edge.getId());
	EXPECT_EQ(retrievedEdge.getSourceNodeId(), sourceNode.getId());
	EXPECT_EQ(retrievedEdge.getTargetNodeId(), targetNode.getId());
	EXPECT_EQ(dataManager->resolveLabel(retrievedEdge.getLabelId()), "CONNECTS_TO");

	edge.setLabelId(dataManager->getOrCreateLabelId("RELATED_TO"));
	edgeManager->update(edge);

	retrievedEdge = edgeManager->get(edge.getId());
	EXPECT_EQ(dataManager->resolveLabel(retrievedEdge.getLabelId()), "RELATED_TO");

	edgeManager->remove(edge);

	// Check deletion status
	retrievedEdge = edgeManager->get(edge.getId());
	EXPECT_EQ(retrievedEdge.getId(), 0) << "Removed edge should return empty entity (ID 0).";

	dataManager->clearCache();
	auto retrievedFromDisk = dataManager->loadEdgeFromDisk(edge.getId());
	EXPECT_EQ(retrievedFromDisk.getId(), 0);
}

// Tests for PersistenceManager Snapshot Commit (Replacing MarkAllSaved)
TEST_F(BaseEntityManagerTest, SnapshotCommitClearsDirty) {
	// Create and add multiple nodes
	for (int i = 0; i < 3; i++) {
		graph::Node node = createTestNode(dataManager, "SavedNode" + std::to_string(i));
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

// Tests for getDirtyEntityInfos (Replacing getDirtyWithChangeTypes)
TEST_F(BaseEntityManagerTest, GetDirtyWithChangeTypes) {
	// --- Setup ---

	// 1. ADDED
    graph::Node addedNode = createTestNode(dataManager, "AddedNode");
    nodeManager->add(addedNode);

    // 2. ADDED then MODIFIED
    graph::Node modifiedNode = createTestNode(dataManager, "OriginalLabel");
    nodeManager->add(modifiedNode);

    modifiedNode.setLabelId(dataManager->getOrCreateLabelId("ModifiedLabel"));
    nodeManager->update(modifiedNode);

    // 3. ADDED then REMOVED
    graph::Node removedNode = createTestNode(dataManager, "RemovedNode");
    nodeManager->add(removedNode);
    nodeManager->remove(removedNode);

	// --- Assertions ---

	// Get only ADDED nodes
	auto addedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::ADDED});
	// Depending on strict logic:
	// addedNode -> ADDED
	// modifiedNode -> ADDED (because updateEntityImpl preserved ADDED state)
	EXPECT_EQ(addedNodes.size(), 2UL);

	// Verify IDs
	std::set<int64_t> addedIds;
	for (auto &n: addedNodes)
		addedIds.insert(n.getId());
	EXPECT_TRUE(addedIds.count(addedNode.getId()));
	EXPECT_TRUE(addedIds.count(modifiedNode.getId()));


	// Get only MODIFIED nodes
	auto modifiedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::MODIFIED});
	EXPECT_EQ(modifiedNodes.size(), 0UL);


	// Since 'removedNode' was only in memory (ADDED), deleting it simply removes it
	// from the dirty registry completely. It does NOT create a DELETED record.
	auto deletedNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::DELETED});
	// In current implementation, we upsert a DELETED record.
	EXPECT_EQ(deletedNodes.size(), 0UL);

	// Get all dirty nodes. Total = 3 (2 Added + 1 Deleted)
	auto allDirtyNodes = getDirtyEntities<graph::Node>({graph::storage::EntityChangeType::ADDED,
														graph::storage::EntityChangeType::MODIFIED,
														graph::storage::EntityChangeType::DELETED});
	EXPECT_EQ(allDirtyNodes.size(), 2UL);
}
