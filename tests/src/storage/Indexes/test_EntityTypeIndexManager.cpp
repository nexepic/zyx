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
#include <string>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"

namespace fs = std::filesystem;

class EntityTypeIndexManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file path for the test database
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_entityTypeIndex_" + boost::uuids::to_string(uuid) + ".dat");

		// Initialize Database and FileStorage
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();

		// Manually instantiate EntityTypeIndexManager for white-box testing.

		// 1. Node Index Manager
		nodeIndexManager = std::make_shared<graph::query::indexes::EntityTypeIndexManager>(
				dataManager, fileStorage->getSystemStateManager(), graph::query::indexes::IndexTypes::NODE_LABEL_TYPE,
				graph::storage::state::keys::Node::LABEL_ROOT, graph::query::indexes::IndexTypes::NODE_PROPERTY_TYPE,
				graph::storage::state::keys::Node::PROPERTY_PREFIX);

		// 2. Edge Index Manager
		edgeIndexManager = std::make_shared<graph::query::indexes::EntityTypeIndexManager>(
				dataManager, fileStorage->getSystemStateManager(), graph::query::indexes::IndexTypes::EDGE_LABEL_TYPE,
				graph::storage::state::keys::Edge::LABEL_ROOT, graph::query::indexes::IndexTypes::EDGE_PROPERTY_TYPE,
				graph::storage::state::keys::Edge::PROPERTY_PREFIX);
	}

	void TearDown() override {
		if (database) {
			database->close();
		}
		database.reset();
		if (fs::exists(testFilePath)) {
			fs::remove(testFilePath);
		}
	}

	fs::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::EntityTypeIndexManager> nodeIndexManager;
	std::shared_ptr<graph::query::indexes::EntityTypeIndexManager> edgeIndexManager;
};

// ============================================================================
// Initialization Tests
// ============================================================================

TEST_F(EntityTypeIndexManagerTest, Constructor) {
	EXPECT_NE(nodeIndexManager->getLabelIndex(), nullptr);
	EXPECT_NE(nodeIndexManager->getPropertyIndex(), nullptr);

	// Default is disabled
	EXPECT_FALSE(nodeIndexManager->hasLabelIndex());

	// Property index works on specific keys, so it remains "false" for arbitrary keys
	EXPECT_FALSE(nodeIndexManager->hasPropertyIndex("any_key"));
}

// ============================================================================
// Lifecycle Tests (Create / Drop)
// ============================================================================

TEST_F(EntityTypeIndexManagerTest, CreateAndDropIndexes) {
	// 1. Create Label Index
	// Mock build function that returns true immediately
	bool resLabel = nodeIndexManager->createLabelIndex([]() { return true; });
	EXPECT_TRUE(resLabel);

	// Use hasLabelIndex instead of listIndexes
	// Inject some data to make sure it's "physically" present/active if implementation relies on emptiness
	nodeIndexManager->getLabelIndex()->addNode(1, "Test");
	EXPECT_TRUE(nodeIndexManager->hasLabelIndex());

	// 2. Create Property Index
	bool resProp = nodeIndexManager->createPropertyIndex("name", []() { return true; });
	EXPECT_TRUE(resProp);

	// Use hasPropertyIndex instead of listIndexes
	EXPECT_TRUE(nodeIndexManager->hasPropertyIndex("name"));

	// 3. Drop Label Index
	EXPECT_TRUE(nodeIndexManager->dropIndex("label", ""));
	EXPECT_FALSE(nodeIndexManager->hasLabelIndex());

	// 4. Drop Property Index
	EXPECT_TRUE(nodeIndexManager->dropIndex("property", "name"));
	EXPECT_FALSE(nodeIndexManager->hasPropertyIndex("name"));
}

TEST_F(EntityTypeIndexManagerTest, CreatePropertyIndex_Idempotency) {
	// 1. Create first time
	EXPECT_TRUE(nodeIndexManager->createPropertyIndex("age", []() { return true; }));

	// 2. Create again (Should return false because it exists)
	// The build function should NOT be called.
	bool buildCalled = false;
	bool res = nodeIndexManager->createPropertyIndex("age", [&]() {
		buildCalled = true;
		return true;
	});

	EXPECT_FALSE(res);
	EXPECT_FALSE(buildCalled);
}

// ============================================================================
// Event Handler Tests (Node)
// ============================================================================

TEST_F(EntityTypeIndexManagerTest, NodeEventHandlers) {
	// Setup: Enable indexes first
	(void) nodeIndexManager->createLabelIndex([]() { return true; });
	(void) nodeIndexManager->createPropertyIndex("key1", []() { return true; });

	// 1. Add Node
	graph::Node node(1, "TestLabel");
	node.addProperty("key1", "value1");

	// Simulate Add Event
	nodeIndexManager->onEntityAdded(node);

	// Verify Internal State
	auto labelIdx = nodeIndexManager->getLabelIndex();
	auto propIdx = nodeIndexManager->getPropertyIndex();

	// Check Label Index
	auto labelResults = labelIdx->findNodes("TestLabel");
	ASSERT_EQ(labelResults.size(), 1UL);
	EXPECT_EQ(labelResults[0], 1);

	// Check Property Index
	auto propResults = propIdx->findExactMatch("key1", "value1");
	ASSERT_EQ(propResults.size(), 1UL);
	EXPECT_EQ(propResults[0], 1);

	// 2. Update Node
	graph::Node updatedNode = node;
	updatedNode.setLabel("NewLabel"); // Change Label
	updatedNode.addProperty("key1", "value2"); // Change Property Value

	// Simulate Update Event
	nodeIndexManager->onEntityUpdated(node, updatedNode);

	// Verify Label Update
	EXPECT_TRUE(labelIdx->findNodes("TestLabel").empty()); // Old label gone
	labelResults = labelIdx->findNodes("NewLabel");
	ASSERT_EQ(labelResults.size(), 1UL); // New label present
	EXPECT_EQ(labelResults[0], 1);

	// Verify Property Update
	EXPECT_TRUE(propIdx->findExactMatch("key1", "value1").empty()); // Old value gone
	propResults = propIdx->findExactMatch("key1", "value2");
	ASSERT_EQ(propResults.size(), 1UL); // New value present
	EXPECT_EQ(propResults[0], 1);

	// 3. Delete Node
	nodeIndexManager->onEntityDeleted(updatedNode);

	// Verify Deletion
	EXPECT_TRUE(labelIdx->findNodes("NewLabel").empty());
	EXPECT_TRUE(propIdx->findExactMatch("key1", "value2").empty());
}

// ============================================================================
// Event Handler Tests (Edge)
// ============================================================================

TEST_F(EntityTypeIndexManagerTest, EdgeEventHandlers) {
	// Setup
	(void) edgeIndexManager->createLabelIndex([]() { return true; });
	(void) edgeIndexManager->createPropertyIndex("weight", []() { return true; });

	// 1. Add Edge
	graph::Edge edge(10, 1, 2, "KNOWS");
	edge.addProperty("weight", 0.5);

	edgeIndexManager->onEntityAdded(edge);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	auto propIdx = edgeIndexManager->getPropertyIndex();

	// Verify Add
	EXPECT_EQ(labelIdx->findNodes("KNOWS").size(), 1UL);
	EXPECT_EQ(propIdx->findExactMatch("weight", 0.5).size(), 1UL);

	// 2. Update Edge
	graph::Edge updatedEdge = edge;
	updatedEdge.setLabel("LIKES");
	updatedEdge.addProperty("weight", 1.0);

	edgeIndexManager->onEntityUpdated(edge, updatedEdge);

	// Verify Update
	EXPECT_TRUE(labelIdx->findNodes("KNOWS").empty());
	EXPECT_EQ(labelIdx->findNodes("LIKES").size(), 1UL);

	EXPECT_TRUE(propIdx->findExactMatch("weight", 0.5).empty());
	EXPECT_EQ(propIdx->findExactMatch("weight", 1.0).size(), 1UL);

	// 3. Delete Edge
	edgeIndexManager->onEntityDeleted(updatedEdge);
	EXPECT_TRUE(labelIdx->findNodes("LIKES").empty());
	EXPECT_TRUE(propIdx->findExactMatch("weight", 1.0).empty());
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(EntityTypeIndexManagerTest, HandleZeroEntityId) {
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	// Create a node with invalid ID 0
	graph::Node node(0, "ZeroNode");

	// Try to add
	nodeIndexManager->onEntityAdded(node);

	// Verify no index entry created
	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_TRUE(labelIdx->findNodes("ZeroNode").empty());
}

TEST_F(EntityTypeIndexManagerTest, EmptyLabelHandling) {
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	// 1. Add node with empty label
	graph::Node node(1, "");
	nodeIndexManager->onEntityAdded(node);

	// Verify nothing indexed
	auto labelIdx = nodeIndexManager->getLabelIndex();
	// Cannot query empty string usually, but let's check update flow

	// 2. Update to valid label
	graph::Node validNode = node;
	validNode.setLabel("Valid");
	nodeIndexManager->onEntityUpdated(node, validNode);

	EXPECT_EQ(labelIdx->findNodes("Valid").size(), 1UL);

	// 3. Update back to empty
	nodeIndexManager->onEntityUpdated(validNode, node);
	EXPECT_TRUE(labelIdx->findNodes("Valid").empty());
}

TEST_F(EntityTypeIndexManagerTest, PropertyChangeHandling_AddRemoveFields) {
	// Setup
	(void) nodeIndexManager->createPropertyIndex("dynamic", []() { return true; });
	auto propIdx = nodeIndexManager->getPropertyIndex();

	// 1. Node without property
	graph::Node node(1, "Node");
	nodeIndexManager->onEntityAdded(node);
	EXPECT_TRUE(propIdx->findExactMatch("dynamic", 100).empty());

	// 2. Add Property (Update)
	graph::Node withProp = node;
	withProp.addProperty("dynamic", 100);

	nodeIndexManager->onEntityUpdated(node, withProp);
	EXPECT_EQ(propIdx->findExactMatch("dynamic", 100).size(), 1UL);

	// 3. Remove Property (Update)
	nodeIndexManager->onEntityUpdated(withProp, node);
	EXPECT_TRUE(propIdx->findExactMatch("dynamic", 100).empty());
}
