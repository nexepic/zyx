/**
 * @file test_EntityTypeIndexManager.cpp
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
#include <gtest/gtest.h>
#include <memory>
#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"

class EntityTypeIndexManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_entityTypeIndex_" + to_string(uuid) + ".dat");

		// Initialize Database and FileStorage
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();

		// Create the index managers
		nodeIndexManager = std::make_shared<graph::query::indexes::EntityTypeIndexManager>(
				dataManager,
				fileStorage->getSystemStateManager(), // Get the manager instance
				graph::query::indexes::IndexTypes::NODE_LABEL_TYPE,
				graph::storage::state::keys::Node::LABEL_ROOT, // Use new group
				graph::query::indexes::IndexTypes::NODE_PROPERTY_TYPE,
				graph::storage::state::keys::Node::PROPERTY_PREFIX);

		edgeIndexManager = std::make_shared<graph::query::indexes::EntityTypeIndexManager>(
				dataManager, fileStorage->getSystemStateManager(), graph::query::indexes::IndexTypes::EDGE_LABEL_TYPE,
				graph::storage::state::keys::Edge::LABEL_ROOT, graph::query::indexes::IndexTypes::EDGE_PROPERTY_TYPE,
				graph::storage::state::keys::Edge::PROPERTY_PREFIX);
	}

	void TearDown() override {
		database->close();
		database.reset();
		std::filesystem::remove(testFilePath);
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::EntityTypeIndexManager> nodeIndexManager;
	std::shared_ptr<graph::query::indexes::EntityTypeIndexManager> edgeIndexManager;
};

TEST_F(EntityTypeIndexManagerTest, Constructor) {
	// Verify index retrieval methods
	EXPECT_NE(nodeIndexManager->getLabelIndex(), nullptr);
	EXPECT_NE(nodeIndexManager->getPropertyIndex(), nullptr);

	// Verify indexes are initially empty
	EXPECT_TRUE(nodeIndexManager->getLabelIndex()->isEmpty());
	EXPECT_TRUE(nodeIndexManager->getPropertyIndex()->isEmpty());
}

TEST_F(EntityTypeIndexManagerTest, BuildAndDropIndexes) {
	// Mock building indexes
	bool result = nodeIndexManager->buildAllIndexes([]() { return true; });
	EXPECT_TRUE(result);

	// Check listing returns empty because indexes are still empty
	auto indexes = nodeIndexManager->listIndexes();
	EXPECT_TRUE(indexes.empty());

	// Drop indexes
	result = nodeIndexManager->dropIndex("label", "");
	EXPECT_TRUE(result);

	result = nodeIndexManager->dropIndex("property", "");
	EXPECT_TRUE(result);

	// Invalid index type
	result = nodeIndexManager->dropIndex("invalid", "");
	EXPECT_FALSE(result);
}

TEST_F(EntityTypeIndexManagerTest, NodeEventHandlers) {
	nodeIndexManager->getLabelIndex()->createIndex();
	// For property index, we register the specific key we want to track
	nodeIndexManager->getPropertyIndex()->createIndex("key1");

	// Create a test node
	graph::Node node;
	node.setId(1);
	node.setLabel("TestLabel");
	node.addProperty("key1", "value1");

	// Initialize label index manually (to avoid requiring full build)
	auto labelIndex = nodeIndexManager->getLabelIndex();
	labelIndex->addNode(node.getId(), node.getLabel());

	// Initialize property index manually
	auto propertyIndex = nodeIndexManager->getPropertyIndex();
	propertyIndex->addProperty(node.getId(), "key1", node.getProperty("key1"));

	// Add node (Should update/confirm the indexes)
	nodeIndexManager->onEntityAdded(node);

	// Verify label index
	auto nodesWithLabel = labelIndex->findNodes("TestLabel");
	EXPECT_EQ(nodesWithLabel.size(), 1UL);
	EXPECT_EQ(nodesWithLabel[0], 1);

	// Verify property index
	auto nodesWithProperty = propertyIndex->findExactMatch("key1", "value1");
	EXPECT_EQ(nodesWithProperty.size(), 1UL);
	EXPECT_EQ(nodesWithProperty[0], 1);

	// Update node
	graph::Node updatedNode = node;
	updatedNode.setLabel("NewLabel");
	updatedNode.addProperty("key1", "value2");
	updatedNode.addProperty("key2", "value3");

	nodeIndexManager->getPropertyIndex()->createIndex("key2");

	nodeIndexManager->onEntityUpdated(node, updatedNode);

	// Verify label index update
	nodesWithLabel = labelIndex->findNodes("TestLabel");
	EXPECT_TRUE(nodesWithLabel.empty());

	nodesWithLabel = labelIndex->findNodes("NewLabel");
	EXPECT_EQ(nodesWithLabel.size(), 1UL);
	EXPECT_EQ(nodesWithLabel[0], 1);

	// Verify property index update
	nodesWithProperty = propertyIndex->findExactMatch("key1", "value1");
	EXPECT_TRUE(nodesWithProperty.empty());

	nodesWithProperty = propertyIndex->findExactMatch("key1", "value2");
	EXPECT_EQ(nodesWithProperty.size(), 1UL);
	EXPECT_EQ(nodesWithProperty[0], 1);

	// Delete node
	nodeIndexManager->onEntityDeleted(updatedNode);

	// Verify label index after deletion
	nodesWithLabel = labelIndex->findNodes("NewLabel");
	EXPECT_TRUE(nodesWithLabel.empty());

	// Verify property index after deletion
	nodesWithProperty = propertyIndex->findExactMatch("key1", "value2");
	EXPECT_TRUE(nodesWithProperty.empty());
}

TEST_F(EntityTypeIndexManagerTest, EdgeEventHandlers) {
	edgeIndexManager->getLabelIndex()->createIndex();
	edgeIndexManager->getPropertyIndex()->createIndex("key1");
	// Ensure "key2" (used in update) is also registered, otherwise updatePropertyIndexes ignores it
	edgeIndexManager->getPropertyIndex()->createIndex("key2");

	// Create a test edge
	graph::Edge edge;
	edge.setId(1);
	edge.setLabel("TestEdgeLabel");
	edge.addProperty("key1", "value1");

	// Initialize label index manually
	auto labelIndex = edgeIndexManager->getLabelIndex();
	labelIndex->addNode(edge.getId(), edge.getLabel());

	// Initialize property index manually
	auto propertyIndex = edgeIndexManager->getPropertyIndex();
	propertyIndex->addProperty(edge.getId(), "key1", edge.getProperty("key1"));

	// Add edge
	edgeIndexManager->onEntityAdded(edge);

	// Verify label index
	auto edgesWithLabel = labelIndex->findNodes("TestEdgeLabel");
	EXPECT_EQ(edgesWithLabel.size(), 1UL);
	EXPECT_EQ(edgesWithLabel[0], 1);

	// Verify property index
	auto edgesWithProperty = propertyIndex->findExactMatch("key1", "value1");
	EXPECT_EQ(edgesWithProperty.size(), 1UL);
	EXPECT_EQ(edgesWithProperty[0], 1);

	// Update edge
	graph::Edge updatedEdge = edge;
	updatedEdge.setLabel("NewEdgeLabel");
	updatedEdge.addProperty("key1", "value2");
	updatedEdge.addProperty("key2", "value3");

	edgeIndexManager->onEntityUpdated(edge, updatedEdge);

	// Verify label index update
	edgesWithLabel = labelIndex->findNodes("TestEdgeLabel");
	EXPECT_TRUE(edgesWithLabel.empty());

	edgesWithLabel = labelIndex->findNodes("NewEdgeLabel");
	EXPECT_EQ(edgesWithLabel.size(), 1UL);
	EXPECT_EQ(edgesWithLabel[0], 1);

	// Delete edge
	edgeIndexManager->onEntityDeleted(updatedEdge);

	// Verify label index after deletion
	edgesWithLabel = labelIndex->findNodes("NewEdgeLabel");
	EXPECT_TRUE(edgesWithLabel.empty());
}

TEST_F(EntityTypeIndexManagerTest, HandleZeroEntityId) {
	// Create a node with id 0
	graph::Node node;
	node.setId(0);
	node.setLabel("ZeroNode");

	// Initialize label index manually
	auto labelIndex = nodeIndexManager->getLabelIndex();
	labelIndex->addNode(1, "SomeOtherLabel"); // Add something to make index non-empty

	// Try to add entity with id 0
	nodeIndexManager->onEntityAdded(node);

	// Verify no effect on label index
	auto nodesWithLabel = labelIndex->findNodes("ZeroNode");
	EXPECT_TRUE(nodesWithLabel.empty());
}

TEST_F(EntityTypeIndexManagerTest, EmptyLabelHandling) {
	// [FIX] Explicitly enable the label index.
	// Otherwise onEntityAdded checks isEmpty() and returns early.
	nodeIndexManager->getLabelIndex()->createIndex();

	// Create a node with empty label
	graph::Node node;
	node.setId(1);
	node.setLabel(""); // Empty label

	// Initialize label index manually
	auto labelIndex = nodeIndexManager->getLabelIndex();
	labelIndex->addNode(2, "SomeOtherLabel"); // Add something to make index non-empty

	// Add node with empty label
	nodeIndexManager->onEntityAdded(node);

	// Update node with empty label to non-empty label
	graph::Node updatedNode = node;
	updatedNode.setLabel("NewLabel");

	nodeIndexManager->onEntityUpdated(node, updatedNode);

	// Verify label index
	auto nodesWithLabel = labelIndex->findNodes("NewLabel");
	EXPECT_EQ(nodesWithLabel.size(), 1UL);
	EXPECT_EQ(nodesWithLabel[0], 1);

	// Update back to empty label
	graph::Node emptyLabelNode = updatedNode;
	emptyLabelNode.setLabel("");

	nodeIndexManager->onEntityUpdated(updatedNode, emptyLabelNode);

	// Verify label was removed
	nodesWithLabel = labelIndex->findNodes("NewLabel");
	EXPECT_TRUE(nodesWithLabel.empty());
}

// Check for PropertyChangeHandling (No changes needed, logic confirms correct behavior)
TEST_F(EntityTypeIndexManagerTest, PropertyChangeHandling) {
	// Create a node with properties
	graph::Node node;
	node.setId(1);
	node.addProperty("key1", "value1");
	node.addProperty("key2", 42);

	// Initialize property index manually
	// Note: addProperty automatically registers the key type, making index non-empty.
	auto propertyIndex = nodeIndexManager->getPropertyIndex();
	propertyIndex->addProperty(node.getId(), "key1", node.getProperty("key1"));
	propertyIndex->addProperty(node.getId(), "key2", node.getProperty("key2"));

	// Create updated node with modified properties
	graph::Node updatedNode = node;
	updatedNode.addProperty("key1", "value2"); // Modified property
	updatedNode.removeProperty("key2"); // Removed property
	updatedNode.addProperty("key3", true); // Added property

	// Manually index the new property to simulate it being indexed (registers key3)
	propertyIndex->addProperty(node.getId(), "key3", updatedNode.getProperty("key3"));

	// Update the node
	// Because key1, key2, key3 are all registered (via addProperty calls above),
	// updatePropertyIndexes will process them.
	nodeIndexManager->onEntityUpdated(node, updatedNode);

	// Verify property changes
	auto nodesWithProperty = propertyIndex->findExactMatch("key1", "value1");
	EXPECT_TRUE(nodesWithProperty.empty());

	nodesWithProperty = propertyIndex->findExactMatch("key1", "value2");
	EXPECT_EQ(nodesWithProperty.size(), 1UL);
	EXPECT_EQ(nodesWithProperty[0], 1);

	nodesWithProperty = propertyIndex->findExactMatch("key2", 42);
	EXPECT_TRUE(nodesWithProperty.empty());

	nodesWithProperty = propertyIndex->findExactMatch("key3", true);
	EXPECT_EQ(nodesWithProperty.size(), 1UL);
	EXPECT_EQ(nodesWithProperty[0], 1);
}

// Check for BuildPropertyIndex_RegistersImmediately (No changes needed, correct)
TEST_F(EntityTypeIndexManagerTest, BuildPropertyIndex_RegistersImmediately) {
	const std::string key = "lazy_prop";

	// Check not present
	EXPECT_FALSE(nodeIndexManager->hasPropertyIndex(key));

	// Build (Create empty)
	bool success = nodeIndexManager->buildPropertyIndex(key, []() { return true; });
	EXPECT_TRUE(success);

	// Check present immediately
	EXPECT_TRUE(nodeIndexManager->hasPropertyIndex(key));

	// Check list
	auto list = nodeIndexManager->listIndexes();
	bool found = false;
	for (auto &p: list) {
		if (p.first == "property" && p.second == key)
			found = true;
	}
	EXPECT_TRUE(found);
}
