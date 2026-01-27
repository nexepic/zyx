/**
 * @file test_EntityTypeIndexManager.cpp
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
		// Note: Database::open() initializes FileStorage, DataManager, and the internal IndexManager.
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();

		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();

		// Ensure LabelRegistry is ready (Critical for resolveLabel to work in tests)
		// With the previous fix, this should happen automatically in DataManager initialization.

		// Manually instantiate EntityTypeIndexManager for white-box testing.
		// NOTE: This instance is SEPARATE from the one inside database->indexManager.
		// They share the underlying DataManager and SystemStateManager (and thus the KV storage keys),
		// but they maintain their own in-memory cache of 'rootId_'.
		// Since we only operate on this instance in the tests, it should be self-consistent.

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
		// Reset managers before closing DB to release shared_ptrs
		nodeIndexManager.reset();
		edgeIndexManager.reset();

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

// TEST_F(EntityTypeIndexManagerTest, Constructor) {
// 	EXPECT_NE(nodeIndexManager->getLabelIndex(), nullptr);
// 	EXPECT_NE(nodeIndexManager->getPropertyIndex(), nullptr);
//
// 	// Default is disabled until created/data added
// 	EXPECT_FALSE(nodeIndexManager->hasLabelIndex());
// 	EXPECT_FALSE(nodeIndexManager->hasPropertyIndex("any_key"));
// }
//
// // ============================================================================
// // Lifecycle Tests (Create / Drop)
// // ============================================================================
//
// TEST_F(EntityTypeIndexManagerTest, CreateAndDropIndexes) {
// 	// 1. Create Label Index
// 	bool resLabel = nodeIndexManager->createLabelIndex([]() { return true; });
// 	EXPECT_TRUE(resLabel);
//
// 	// Trigger state change to ensure hasLabelIndex becomes true
// 	nodeIndexManager->getLabelIndex()->addNode(1, "Test");
// 	EXPECT_TRUE(nodeIndexManager->hasLabelIndex());
//
// 	// 2. Create Property Index
// 	bool resProp = nodeIndexManager->createPropertyIndex("name", []() { return true; });
// 	EXPECT_TRUE(resProp);
// 	EXPECT_TRUE(nodeIndexManager->hasPropertyIndex("name"));
//
// 	// 3. Drop Label Index
// 	EXPECT_TRUE(nodeIndexManager->dropIndex("label", ""));
// 	EXPECT_FALSE(nodeIndexManager->hasLabelIndex());
//
// 	// 4. Drop Property Index
// 	EXPECT_TRUE(nodeIndexManager->dropIndex("property", "name"));
// 	EXPECT_FALSE(nodeIndexManager->hasPropertyIndex("name"));
// }
//
// TEST_F(EntityTypeIndexManagerTest, CreatePropertyIndex_Idempotency) {
// 	// 1. Create first time
// 	EXPECT_TRUE(nodeIndexManager->createPropertyIndex("age", []() { return true; }));
//
// 	// 2. Create again (Should return false because it exists)
// 	bool buildCalled = false;
// 	bool res = nodeIndexManager->createPropertyIndex("age", [&]() {
// 		buildCalled = true;
// 		return true;
// 	});
//
// 	EXPECT_FALSE(res);
// 	EXPECT_FALSE(buildCalled);
// }

// ============================================================================
// Event Handler Tests (Node)
// ============================================================================

TEST_F(EntityTypeIndexManagerTest, NodeEventHandlers) {
	// Setup: Enable indexes first
	(void) nodeIndexManager->createLabelIndex([]() { return true; });
	(void) nodeIndexManager->createPropertyIndex("key1", []() { return true; });

	// 1. Add Node
	int64_t labelId = dataManager->getOrCreateLabelId("TestLabel");
	ASSERT_GT(labelId, 0);
	// Sanity check: Ensure label resolves correctly immediately
	ASSERT_EQ(dataManager->resolveLabel(labelId), "TestLabel");

	graph::Node node(1, labelId);
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
	int64_t newLabelId = dataManager->getOrCreateLabelId("NewLabel");
	ASSERT_GT(newLabelId, 0);

	graph::Node updatedNode = node;
	updatedNode.setLabelId(newLabelId); // Change Label
	updatedNode.addProperty("key1", "value2"); // Change Property Value

	// Sanity Check: Ensure old label still resolves correctly before update
	// If this fails, resolveLabel returns empty, and updateLabelIndex SKIPS removal.
	ASSERT_EQ(dataManager->resolveLabel(node.getLabelId()), "TestLabel");
	ASSERT_EQ(dataManager->resolveLabel(updatedNode.getLabelId()), "NewLabel");

	// Simulate Update Event
	nodeIndexManager->onEntityUpdated(node, updatedNode);

	// Verify Label Update
	auto oldLabelResults = labelIdx->findNodes("TestLabel");
	EXPECT_TRUE(oldLabelResults.empty()) << "Old label 'TestLabel' should be empty, but has size "
										 << oldLabelResults.size();

	auto newLabelResults = labelIdx->findNodes("NewLabel");
	ASSERT_EQ(newLabelResults.size(), 1UL)
			<< "New label 'NewLabel' should have 1 node, found " << newLabelResults.size();
	EXPECT_EQ(newLabelResults[0], 1);

	// Verify Property Update
	EXPECT_TRUE(propIdx->findExactMatch("key1", "value1").empty()); // Old value gone
	auto newPropResults = propIdx->findExactMatch("key1", "value2");
	ASSERT_EQ(newPropResults.size(), 1UL); // New value present
	EXPECT_EQ(newPropResults[0], 1);

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
	int64_t knowsLabel = dataManager->getOrCreateLabelId("KNOWS");
	graph::Edge edge(10, 1, 2, knowsLabel);
	edge.addProperty("weight", 0.5);

	edgeIndexManager->onEntityAdded(edge);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	auto propIdx = edgeIndexManager->getPropertyIndex();

	// Verify Add
	EXPECT_EQ(labelIdx->findNodes("KNOWS").size(), 1UL);
	EXPECT_EQ(propIdx->findExactMatch("weight", 0.5).size(), 1UL);

	// 2. Update Edge
	graph::Edge updatedEdge = edge;
	updatedEdge.setLabelId(dataManager->getOrCreateLabelId("LIKES"));
	updatedEdge.addProperty("weight", 1.0);

	// Sanity Check
	ASSERT_EQ(dataManager->resolveLabel(edge.getLabelId()), "KNOWS");

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
	int64_t lbl = dataManager->getOrCreateLabelId("ZeroNode");
	graph::Node node(0, lbl);

	// Try to add
	nodeIndexManager->onEntityAdded(node);

	// Verify no index entry created (Implementation should guard against ID 0)
	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_TRUE(labelIdx->findNodes("ZeroNode").empty());
}

TEST_F(EntityTypeIndexManagerTest, EmptyLabelHandling) {
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	// 1. Add node with empty label (ID 0)
	// Note: getOrCreateLabelId("") returns 0.
	graph::Node node(1, 0);
	nodeIndexManager->onEntityAdded(node);

	// Verify nothing indexed
	auto labelIdx = nodeIndexManager->getLabelIndex();
	// No label string to query, but internal tree shouldn't have entry.
	// We check implicitly by seeing if the next update works cleanly.

	// 2. Update to valid label
	graph::Node validNode = node;
	validNode.setLabelId(dataManager->getOrCreateLabelId("Valid"));

	// updateNode(old=Empty, new=Valid)
	nodeIndexManager->onEntityUpdated(node, validNode);

	EXPECT_EQ(labelIdx->findNodes("Valid").size(), 1UL);

	// 3. Update back to empty
	// updateNode(old=Valid, new=Empty)
	// Should REMOVE "Valid"
	nodeIndexManager->onEntityUpdated(validNode, node);

	auto result = labelIdx->findNodes("Valid");
	EXPECT_TRUE(result.empty()) << "Label 'Valid' should be removed, but found size " << result.size();
}

TEST_F(EntityTypeIndexManagerTest, PropertyChangeHandling_AddRemoveFields) {
	// Setup
	(void) nodeIndexManager->createPropertyIndex("dynamic", []() { return true; });
	auto propIdx = nodeIndexManager->getPropertyIndex();

	// 1. Node without property
	graph::Node node(1, dataManager->getOrCreateLabelId("Node"));
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

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_Empty) {
	std::vector<graph::Node> empty;
	// Should return early
	nodeIndexManager->onEntitiesAdded(empty);
	SUCCEED();
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_BatchLogic) {
	// Setup indexes
	(void) nodeIndexManager->createLabelIndex([]() { return true; });
	(void) nodeIndexManager->createPropertyIndex("batch_prop", []() { return true; });

	int64_t lbl = dataManager->getOrCreateLabelId("BatchLabel");

	std::vector<graph::Node> nodes;
	for (int i = 0; i < 5; ++i) {
		graph::Node n(i + 100, lbl);
		n.addProperty("batch_prop", i);
		nodes.push_back(n);
	}

	// Batch Insert
	nodeIndexManager->onEntitiesAdded(nodes);

	// Verify
	auto labelIdx = nodeIndexManager->getLabelIndex();
	auto propIdx = nodeIndexManager->getPropertyIndex();

	EXPECT_EQ(labelIdx->findNodes("BatchLabel").size(), 5UL);
	EXPECT_EQ(propIdx->findExactMatch("batch_prop", 2).size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, UpdatePropertyIndex_AddRemoveBatch) {
	// Use onEntityUpdated to trigger updatePropertyIndexes logic with diff
	(void) nodeIndexManager->createPropertyIndex("p1", []() { return true; });

	graph::Node oldN(1, 1);
	oldN.addProperty("p1", "old"); // In Old, Not New -> Remove

	graph::Node newN(1, 1);
	newN.addProperty("p1", "new"); // In New, Not Old -> Add

	nodeIndexManager->onEntityUpdated(oldN, newN);

	auto propIdx = nodeIndexManager->getPropertyIndex();
	EXPECT_TRUE(propIdx->findExactMatch("p1", "old").empty());
	EXPECT_EQ(propIdx->findExactMatch("p1", "new").size(), 1UL);
}
