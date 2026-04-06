/**
 * @file test_NodeManager.cpp
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

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include "boost/uuid/uuid.hpp"
#include "boost/uuid/uuid_generators.hpp"
#include "boost/uuid/uuid_io.hpp"
#include "graph/core/Database.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/NodeManager.hpp"

class NodeManagerTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::storage::NodeManager> nodeManager;

	void SetUp() override {
		// Create a unique temporary database file using Boost UUID
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_nodeManager_" + to_string(uuid) + ".dat");

		// Initialize database and get managers from the DataManager
		db = std::make_unique<graph::Database>(testFilePath.string());
		db->open();
		storage = db->getStorage();
		dataManager = storage->getDataManager();
		// Get the manager instance that was created by the DataManager
		nodeManager = dataManager->getNodeManager();
	}

	void TearDown() override {
		// Release shared_ptrs before closing database
		nodeManager.reset();
		dataManager.reset();
		storage.reset();

		if (db) {
			db->close();
		}
		db.reset();

		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	// Helper function to create a valid Node entity
	static graph::Node createTestNode(const std::shared_ptr<graph::storage::DataManager> &dm,
									  const std::string &label) {
		graph::Node node;
		int64_t labelId = dm->getOrCreateLabelId(label);
		node.setLabelId(labelId);
		return node;
	}
};

// Test adding a node and then its properties
TEST_F(NodeManagerTest, AddAndGetNodeWithProperties) {
	graph::Node node = createTestNode(dataManager, "Person");

	nodeManager->add(node);
	EXPECT_NE(node.getId(), 0);

	std::unordered_map<std::string, graph::PropertyValue> props;
	props["name"] = graph::PropertyValue("John");
	props["age"] = graph::PropertyValue(30);
	nodeManager->addProperties(node.getId(), props);

	graph::Node retrievedNode = nodeManager->get(node.getId());
	auto retrievedProps = nodeManager->getProperties(node.getId());

	EXPECT_EQ(retrievedNode.getId(), node.getId());
	EXPECT_EQ(retrievedNode.getLabelId(), node.getLabelId());
	EXPECT_EQ(dataManager->resolveLabel(retrievedNode.getLabelId()), "Person");

	ASSERT_EQ(retrievedProps.size(), 2UL);
	EXPECT_EQ(std::get<std::string>(retrievedProps.at("name").getVariant()), "John");
}

// Test updating a node's label and properties
TEST_F(NodeManagerTest, UpdateNode) {
	// Create and add a node
	graph::Node node = createTestNode(dataManager, "Person");
	nodeManager->add(node);
	nodeManager->addProperties(node.getId(), {{"name", graph::PropertyValue("John")}});

	// Update the node's label
	int64_t newLabelId = dataManager->getOrCreateLabelId("Employee");
	node.setLabelId(newLabelId);
	nodeManager->update(node);

	// Add/update properties separately
	nodeManager->addProperties(node.getId(), {
													 {"name", graph::PropertyValue("John Doe")}, // Update existing
													 {"salary", graph::PropertyValue(50000)} // Add new
											 });

	// Get the updated node and its properties
	graph::Node retrievedNode = nodeManager->get(node.getId());
	auto retrievedProps = nodeManager->getProperties(node.getId());

	EXPECT_EQ(retrievedNode.getLabelId(), newLabelId) << "Node label should be updated";
	EXPECT_EQ(dataManager->resolveLabel(retrievedNode.getLabelId()), "Employee");
	ASSERT_EQ(retrievedProps.size(), 2UL);
	EXPECT_EQ(std::get<std::string>(retrievedProps.at("name").getVariant()), "John Doe")
			<< "Node name should be updated";
	EXPECT_EQ(std::get<int64_t>(retrievedProps.at("salary").getVariant()), 50000)
			<< "Node should have the new salary property";
}

// Test removing a node
TEST_F(NodeManagerTest, RemoveNode) {
	// Create and add a node
	graph::Node node = createTestNode(dataManager, "Person");
	nodeManager->add(node);
	int64_t nodeId = node.getId();

	// Verify the node was added
	graph::Node retrievedNode = nodeManager->get(nodeId);
	EXPECT_EQ(retrievedNode.getId(), nodeId) << "Node should be retrievable after adding";

	// Remove the node
	nodeManager->remove(node);

	// Clear cache to ensure we are reading from disk
	dataManager->clearCache();

	// Verify the node was removed
	graph::Node removedNode = nodeManager->get(nodeId);
	EXPECT_EQ(removedNode.getId(), 0) << "Node should not be retrievable after removal";
}

// Test batch operations
TEST_F(NodeManagerTest, BatchOperations) {
	std::vector<graph::Node> nodes;
	std::vector<int64_t> nodeIds;

	for (int i = 0; i < 5; i++) {
		graph::Node node = createTestNode(dataManager, "Person_" + std::to_string(i));
		nodeManager->add(node);
		nodes.push_back(node);
		nodeIds.push_back(node.getId());
	}

	auto retrievedNodes = nodeManager->getBatch(nodeIds);
	EXPECT_EQ(retrievedNodes.size(), 5UL);

	nodeManager->remove(nodes[2]);
	retrievedNodes = nodeManager->getBatch(nodeIds);
	EXPECT_EQ(retrievedNodes.size(), 4UL);
}

// Test property management
TEST_F(NodeManagerTest, PropertyManagement) {
	// Create a node
	graph::Node node = createTestNode(dataManager, "TestNode");
	nodeManager->add(node);
	int64_t nodeId = node.getId();

	// Add properties of various types using the manager
	nodeManager->addProperties(nodeId, {{"string", graph::PropertyValue("text value")},
										{"integer", graph::PropertyValue(42)},
										{"double", graph::PropertyValue(3.14)},
										{"boolean", graph::PropertyValue(true)}});

	// Retrieve and verify properties
	auto retrievedProps = nodeManager->getProperties(nodeId);
	ASSERT_EQ(retrievedProps.size(), 4UL);
	EXPECT_EQ(std::get<std::string>(retrievedProps.at("string").getVariant()), "text value");
	EXPECT_EQ(std::get<int64_t>(retrievedProps.at("integer").getVariant()), 42);
	EXPECT_DOUBLE_EQ(std::get<double>(retrievedProps.at("double").getVariant()), 3.14);
	EXPECT_EQ(std::get<bool>(retrievedProps.at("boolean").getVariant()), true);

	// Test property removal using the manager
	nodeManager->removeProperty(nodeId, "string");

	auto updatedProps = nodeManager->getProperties(nodeId);
	EXPECT_EQ(updatedProps.find("string"), updatedProps.end()) << "Property should be removed";
	EXPECT_NE(updatedProps.find("integer"), updatedProps.end()) << "Other properties should remain";
	EXPECT_EQ(updatedProps.size(), 3UL);
}

// Test node ID allocation
TEST_F(NodeManagerTest, NodeIdAllocation) {
	graph::Node node1 = createTestNode(dataManager, "Node1");
	graph::Node node2 = createTestNode(dataManager, "Node2");

	nodeManager->add(node1);
	nodeManager->add(node2);

	EXPECT_NE(node1.getId(), 0);
	EXPECT_NE(node2.getId(), 0);
	EXPECT_NE(node1.getId(), node2.getId());
}
