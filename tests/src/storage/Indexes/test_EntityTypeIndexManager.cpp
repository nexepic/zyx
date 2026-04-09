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

		// Ensure TokenRegistry is ready (Critical for resolveTokenName to work in tests)
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
				dataManager, fileStorage->getSystemStateManager(), graph::query::indexes::IndexTypes::EDGE_TYPE_INDEX,
				graph::storage::state::keys::Edge::LABEL_ROOT, graph::query::indexes::IndexTypes::EDGE_PROPERTY_TYPE,
				graph::storage::state::keys::Edge::PROPERTY_PREFIX);
	}

	void TearDown() override {
		// Reset managers before closing DB to release shared_ptrs
		nodeIndexManager.reset();
		edgeIndexManager.reset();
		dataManager.reset();
		fileStorage.reset();

		if (database) {
			database->close();
		}
		database.reset();

		std::error_code ec;
		fs::remove(testFilePath, ec);
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
	int64_t labelId = dataManager->getOrCreateTokenId("TestLabel");
	ASSERT_GT(labelId, 0);
	// Sanity check: Ensure label resolves correctly immediately
	ASSERT_EQ(dataManager->resolveTokenName(labelId), "TestLabel");

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
	int64_t newLabelId = dataManager->getOrCreateTokenId("NewLabel");
	ASSERT_GT(newLabelId, 0);

	graph::Node updatedNode = node;
	updatedNode.setLabelId(newLabelId); // Change Label
	updatedNode.addProperty("key1", "value2"); // Change Property Value

	// Sanity Check: Ensure old label still resolves correctly before update
	// If this fails, resolveTokenName returns empty, and updateLabelIndex SKIPS removal.
	ASSERT_EQ(dataManager->resolveTokenName(node.getLabelId()), "TestLabel");
	ASSERT_EQ(dataManager->resolveTokenName(updatedNode.getLabelId()), "NewLabel");

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
	int64_t knowsLabel = dataManager->getOrCreateTokenId("KNOWS");
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
	updatedEdge.setTypeId(dataManager->getOrCreateTokenId("LIKES"));
	updatedEdge.addProperty("weight", 1.0);

	// Sanity Check
	ASSERT_EQ(dataManager->resolveTokenName(edge.getTypeId()), "KNOWS");

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
	int64_t lbl = dataManager->getOrCreateTokenId("ZeroNode");
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
	// Note: getOrCreateTokenId("") returns 0.
	graph::Node node(1, 0);
	nodeIndexManager->onEntityAdded(node);

	// Verify nothing indexed
	auto labelIdx = nodeIndexManager->getLabelIndex();
	// No label string to query, but internal tree shouldn't have entry.
	// We check implicitly by seeing if the next update works cleanly.

	// 2. Update to valid label
	graph::Node validNode = node;
	validNode.setLabelId(dataManager->getOrCreateTokenId("Valid"));

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
	graph::Node node(1, dataManager->getOrCreateTokenId("Node"));
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

	int64_t lbl = dataManager->getOrCreateTokenId("BatchLabel");

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

// ============================================================================
// Additional Coverage Tests
// ============================================================================

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_WithEmptyLabelIndex) {
	// Cover branch: if (!nodesByLabelId.empty() && !labelIndex_->isEmpty()) -> labelIndex_->isEmpty() == true
	// Don't create label index, only property index
	(void) nodeIndexManager->createPropertyIndex("batch_key", []() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("BatchOnly");
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 3; ++i) {
		graph::Node n(i + 200, lbl);
		n.addProperty("batch_key", i);
		nodes.push_back(n);
	}

	// Should not crash even though label index is empty
	nodeIndexManager->onEntitiesAdded(nodes);

	auto propIdx = nodeIndexManager->getPropertyIndex();
	EXPECT_EQ(propIdx->findExactMatch("batch_key", 1).size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_WithEmptyPropertyBatch) {
	// Cover branch: if (!propBatch.empty() && !propertyIndex_->isEmpty()) -> propBatch is empty
	// Create label index only, no property index
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("NoPropBatch");
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 3; ++i) {
		// Nodes with no properties
		graph::Node n(i + 300, lbl);
		nodes.push_back(n);
	}

	nodeIndexManager->onEntitiesAdded(nodes);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("NoPropBatch").size(), 3UL);
}

TEST_F(EntityTypeIndexManagerTest, UpdatePropertyIndex_PropertyUnchanged) {
	// Cover branch: newIt->second != oldIt->second -> False (property unchanged)
	(void) nodeIndexManager->createPropertyIndex("unchanged", []() { return true; });

	graph::Node oldN(1, 1);
	oldN.addProperty("unchanged", "same_value");

	graph::Node newN(1, 1);
	newN.addProperty("unchanged", "same_value"); // Same value

	// Add to index first
	nodeIndexManager->onEntityAdded(oldN);

	auto propIdx = nodeIndexManager->getPropertyIndex();
	EXPECT_EQ(propIdx->findExactMatch("unchanged", "same_value").size(), 1UL);

	// Update with same value - should not remove or re-add
	nodeIndexManager->onEntityUpdated(oldN, newN);

	EXPECT_EQ(propIdx->findExactMatch("unchanged", "same_value").size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, DropIndex_InvalidType) {
	// Cover branch: return false in dropIndex (line 66)
	bool result = nodeIndexManager->dropIndex("invalid_type", "");
	EXPECT_FALSE(result);
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_NodesWithEmptyProperties) {
	// Cover branch: if (!props.empty()) -> False
	(void) nodeIndexManager->createLabelIndex([]() { return true; });
	(void) nodeIndexManager->createPropertyIndex("some_key", []() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("EmptyProps");
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 5; ++i) {
		graph::Node n(i + 400, lbl);
		// No properties added to nodes
		nodes.push_back(n);
	}

	nodeIndexManager->onEntitiesAdded(nodes);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("EmptyProps").size(), 5UL);
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_EdgesWithLabelsAndProperties) {
	// Cover Edge template instantiation paths for onEntitiesAdded
	(void) edgeIndexManager->createLabelIndex([]() { return true; });
	(void) edgeIndexManager->createPropertyIndex("edge_prop", []() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("BATCH_EDGE");
	std::vector<graph::Edge> edges;
	for (int i = 0; i < 3; ++i) {
		graph::Edge e(i + 500, 1, 2, lbl);
		e.addProperty("edge_prop", i * 10);
		edges.push_back(e);
	}

	edgeIndexManager->onEntitiesAdded(edges);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("BATCH_EDGE").size(), 3UL);

	auto propIdx = edgeIndexManager->getPropertyIndex();
	EXPECT_EQ(propIdx->findExactMatch("edge_prop", 10).size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, UpdatePropertyIndex_PropertyRemovedInNew) {
	// Cover branch: inOld && !inNew -> removal path
	(void) nodeIndexManager->createPropertyIndex("removable", []() { return true; });

	graph::Node oldN(1, 1);
	oldN.addProperty("removable", "to_remove");

	graph::Node newN(1, 1);
	// No "removable" property in new node

	nodeIndexManager->onEntityAdded(oldN);
	auto propIdx = nodeIndexManager->getPropertyIndex();
	EXPECT_EQ(propIdx->findExactMatch("removable", "to_remove").size(), 1UL);

	nodeIndexManager->onEntityUpdated(oldN, newN);
	EXPECT_TRUE(propIdx->findExactMatch("removable", "to_remove").empty());
}

TEST_F(EntityTypeIndexManagerTest, UpdatePropertyIndex_PropertyAddedInNew) {
	// Cover branch: !inOld && inNew -> addition path
	(void) nodeIndexManager->createPropertyIndex("new_prop", []() { return true; });

	graph::Node oldN(1, 1);
	// No "new_prop" property in old node

	graph::Node newN(1, 1);
	newN.addProperty("new_prop", "added_value");

	nodeIndexManager->onEntityAdded(oldN);
	auto propIdx = nodeIndexManager->getPropertyIndex();
	EXPECT_TRUE(propIdx->findExactMatch("new_prop", "added_value").empty());

	nodeIndexManager->onEntityUpdated(oldN, newN);
	EXPECT_EQ(propIdx->findExactMatch("new_prop", "added_value").size(), 1UL);
}

// ============================================================================
// Additional Branch Coverage Tests
// ============================================================================

TEST_F(EntityTypeIndexManagerTest, UpdateLabelIndex_DeleteWithEmptyLabel) {
	// Cover branch: isDeleted && newLabel.empty() -> removeNode NOT called (line 90)
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	// Add node with a label
	int64_t labelId = dataManager->getOrCreateTokenId("ToDelete");
	graph::Node node(10, labelId);
	nodeIndexManager->onEntityAdded(node);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("ToDelete").size(), 1UL);

	// Delete a node with labelId=0 (empty label) - removeNode should not be called
	graph::Node noLabelNode(11, 0);
	nodeIndexManager->onEntityDeleted(noLabelNode);

	// Original node should still be in the index
	EXPECT_EQ(labelIdx->findNodes("ToDelete").size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, UpdateLabelIndex_UpdateSameLabel) {
	// Cover branch: oldLabel != newLabel -> False (same label, no update needed)
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t labelId = dataManager->getOrCreateTokenId("SameLabel");
	graph::Node oldNode(20, labelId);
	nodeIndexManager->onEntityAdded(oldNode);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("SameLabel").size(), 1UL);

	// Update with same label - no remove/add should happen
	graph::Node newNode(20, labelId);
	nodeIndexManager->onEntityUpdated(oldNode, newNode);

	EXPECT_EQ(labelIdx->findNodes("SameLabel").size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, UpdateLabelIndex_UpdateFromEmptyToValid) {
	// Cover: oldLabel.empty() -> True (skip remove), newLabel not empty (add)
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	// Node starts with no label
	graph::Node oldNode(30, 0);
	nodeIndexManager->onEntityAdded(oldNode);

	auto labelIdx = nodeIndexManager->getLabelIndex();

	// Update to a valid label
	int64_t labelId = dataManager->getOrCreateTokenId("NewLabel");
	graph::Node newNode(30, labelId);

	// oldLabel is "", so resolveTokenName(0) returns empty, skip remove
	nodeIndexManager->onEntityUpdated(oldNode, newNode);

	EXPECT_EQ(labelIdx->findNodes("NewLabel").size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_EmptyLabelString) {
	// Cover: !labelStr.empty() -> False in onEntitiesAdded (line 209)
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	// Nodes with labelId that resolves to empty string (labelId=0)
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 3; ++i) {
		graph::Node n(i + 700, 0); // labelId=0 -> resolveTokenName returns ""
		nodes.push_back(n);
	}

	// Should not crash, nodes with empty labels are skipped in batch
	nodeIndexManager->onEntitiesAdded(nodes);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	// No labels should have been added
	SUCCEED();
}

TEST_F(EntityTypeIndexManagerTest, PersistState_NullChecks) {
	// Cover: persistState when labelIndex_ and propertyIndex_ are valid (lines 71-74)
	// This is always true in our setup, but exercises the branches
	EXPECT_NO_THROW(nodeIndexManager->persistState());
	EXPECT_NO_THROW(edgeIndexManager->persistState());
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_PropertyNotIndexed) {
	// Cover: hasKeyIndexed returns false in batch (line 191)
	(void) nodeIndexManager->createLabelIndex([]() { return true; });
	// Note: We do NOT create a property index

	int64_t lbl = dataManager->getOrCreateTokenId("PropTest");
	std::vector<graph::Node> nodes;
	graph::Node n(800, lbl);
	n.addProperty("unindexed_prop", 42);
	nodes.push_back(n);

	// Property should be skipped since it's not indexed
	nodeIndexManager->onEntitiesAdded(nodes);

	// Label should still be indexed
	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("PropTest").size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, UpdatePropertyIndexes_EmptyIndexedKeys) {
	// Cover: indexedKeys.empty() -> True in updatePropertyIndexes (line 116-117)
	// Don't create any property index - indexedKeys will be empty

	graph::Node oldN(900, 1);
	oldN.addProperty("some_key", "some_value");

	graph::Node newN(900, 1);
	newN.addProperty("some_key", "new_value");

	// Should return early since no property indexes exist
	EXPECT_NO_THROW(nodeIndexManager->onEntityUpdated(oldN, newN));
}

TEST_F(EntityTypeIndexManagerTest, CreatePropertyIndex_AlreadyExists) {
	// Cover: propertyIndex_->hasKeyIndexed(key) -> True in createPropertyIndex (line 49-50)
	bool buildCalled = false;
	EXPECT_TRUE(nodeIndexManager->createPropertyIndex("dup_key", [&]() {
		buildCalled = true;
		return true;
	}));
	EXPECT_TRUE(buildCalled);

	// Try creating same key again - should return false without calling build
	buildCalled = false;
	EXPECT_FALSE(nodeIndexManager->createPropertyIndex("dup_key", [&]() {
		buildCalled = true;
		return true;
	}));
	EXPECT_FALSE(buildCalled);
}

TEST_F(EntityTypeIndexManagerTest, DropIndex_LabelType) {
	// Cover: indexType == "label" branch in dropIndex (line 58-61)
	(void) nodeIndexManager->createLabelIndex([]() { return true; });
	EXPECT_TRUE(nodeIndexManager->dropIndex("label", ""));
}

TEST_F(EntityTypeIndexManagerTest, DropIndex_PropertyType) {
	// Cover: indexType == "property" branch in dropIndex (line 62-65)
	(void) nodeIndexManager->createPropertyIndex("drop_prop", []() { return true; });
	EXPECT_TRUE(nodeIndexManager->dropIndex("property", "drop_prop"));
}

TEST_F(EntityTypeIndexManagerTest, PersistState_NullIndexes) {
	// Cover: labelIndex_ and propertyIndex_ null-check branches (lines 71, 73)
	// In normal setup they are always non-null, but this still exercises the true path
	EXPECT_NO_THROW(nodeIndexManager->persistState());
}

TEST_F(EntityTypeIndexManagerTest, UpdateLabelIndex_IsDeleted_WithLabel) {
	// Cover: isDeleted=true && !newLabel.empty() -> removeNode called (line 90)
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t labelId = dataManager->getOrCreateTokenId("DeleteMe");
	graph::Node node(50, labelId);
	nodeIndexManager->onEntityAdded(node);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("DeleteMe").size(), 1UL);

	// Delete - should call removeNode
	nodeIndexManager->onEntityDeleted(node);
	EXPECT_TRUE(labelIdx->findNodes("DeleteMe").empty());
}

TEST_F(EntityTypeIndexManagerTest, UpdateLabelIndex_LabelChangedBothNonEmpty) {
	// Cover: oldLabel != newLabel where both are non-empty (lines 93, 96)
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t labelA = dataManager->getOrCreateTokenId("LabelA");
	int64_t labelB = dataManager->getOrCreateTokenId("LabelB");

	graph::Node oldNode(60, labelA);
	nodeIndexManager->onEntityAdded(oldNode);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("LabelA").size(), 1UL);

	graph::Node newNode(60, labelB);
	nodeIndexManager->onEntityUpdated(oldNode, newNode);

	EXPECT_TRUE(labelIdx->findNodes("LabelA").empty());
	EXPECT_EQ(labelIdx->findNodes("LabelB").size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, UpdatePropertyIndexes_OldHasNew_ValueChanged) {
	// Cover: inOld && inNew && value changed (lines 132-136)
	(void) nodeIndexManager->createPropertyIndex("change_prop", []() { return true; });

	graph::Node oldN(70, 1);
	oldN.addProperty("change_prop", "old_val");

	graph::Node newN(70, 1);
	newN.addProperty("change_prop", "new_val");

	nodeIndexManager->onEntityAdded(oldN);
	auto propIdx = nodeIndexManager->getPropertyIndex();
	EXPECT_EQ(propIdx->findExactMatch("change_prop", "old_val").size(), 1UL);

	nodeIndexManager->onEntityUpdated(oldN, newN);
	EXPECT_TRUE(propIdx->findExactMatch("change_prop", "old_val").empty());
	EXPECT_EQ(propIdx->findExactMatch("change_prop", "new_val").size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, UpdateLabelIndex_EntityIdZero_SkipsUpdate) {
	// Cover: entity.getId() == 0 -> early return (line 80)
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t labelId = dataManager->getOrCreateTokenId("ZeroIdNode");
	graph::Node zeroNode(0, labelId);

	// Should return early without crashing
	nodeIndexManager->onEntityAdded(zeroNode);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_TRUE(labelIdx->findNodes("ZeroIdNode").empty());
}

TEST_F(EntityTypeIndexManagerTest, UpdatePropertyIndexes_EmptyPropertyIndex) {
	// Cover: propertyIndex_->isEmpty() -> True (line 105)
	// Don't enable any property index
	graph::Node oldN(80, 1);
	oldN.addProperty("p", "old");
	graph::Node newN(80, 1);
	newN.addProperty("p", "new");

	// Should return early because property index is empty/disabled
	EXPECT_NO_THROW(nodeIndexManager->onEntityUpdated(oldN, newN));
}

TEST_F(EntityTypeIndexManagerTest, GetPropertyIndex_Accessor) {
	// Cover: getPropertyIndex() accessor (line 38)
	auto propIdx = nodeIndexManager->getPropertyIndex();
	ASSERT_NE(propIdx, nullptr);

	auto edgePropIdx = edgeIndexManager->getPropertyIndex();
	ASSERT_NE(edgePropIdx, nullptr);
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_WithBothIndexesEnabled) {
	// Cover: onEntitiesAdded where BOTH label and property indexes are enabled
	// so that the batch goes through the full path including label resolution and property batching
	(void) nodeIndexManager->createLabelIndex([]() { return true; });
	(void) nodeIndexManager->createPropertyIndex("indexed_key", []() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("FullBatch");
	std::vector<graph::Node> nodes;
	for (int i = 0; i < 5; ++i) {
		graph::Node n(i + 1000, lbl);
		n.addProperty("indexed_key", i * 100);
		nodes.push_back(n);
	}

	nodeIndexManager->onEntitiesAdded(nodes);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("FullBatch").size(), 5UL);

	auto propIdx = nodeIndexManager->getPropertyIndex();
	EXPECT_EQ(propIdx->findExactMatch("indexed_key", 200).size(), 1UL);
}

// ============================================================================
// Edge Template Branch Coverage Tests
// ============================================================================

TEST_F(EntityTypeIndexManagerTest, EdgeUpdateLabelIndex_EntityIdZero_SkipsUpdate) {
	// Cover Edge template: entity.getId() == 0 -> early return (line 80)
	(void) edgeIndexManager->createLabelIndex([]() { return true; });

	int64_t labelId = dataManager->getOrCreateTokenId("ZeroEdge");
	graph::Edge zeroEdge(0, 1, 2, labelId);

	// Should return early without crashing since edge ID is 0
	edgeIndexManager->onEntityAdded(zeroEdge);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_TRUE(labelIdx->findNodes("ZeroEdge").empty());
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_EmptyEdgeVector) {
	// Cover Edge template: entities.empty() in onEntitiesAdded (line 165)
	std::vector<graph::Edge> emptyEdges;
	edgeIndexManager->onEntitiesAdded(emptyEdges);
	SUCCEED();
}

TEST_F(EntityTypeIndexManagerTest, EdgeUpdatePropertyIndexes_IndexedKeysEmpty) {
	// Cover: indexedKeys.empty() -> True via Edge path (line 116-117)
	// Create a property index then drop it, or simply don't create any property index
	// but ensure the property index itself is not "isEmpty" (i.e., it has been created).
	// Actually, indexedKeys.empty() is separate from propertyIndex_->isEmpty().
	// We need propertyIndex not empty but indexedKeys empty.
	// Looking at the code: propertyIndex_->isEmpty() returns early at line 105.
	// indexedKeys.empty() at line 116 is after that.
	// So we need: propertyIndex not empty AND indexedKeys empty.
	// Create a property index then drop the key.
	(void) edgeIndexManager->createPropertyIndex("temp_key", []() { return true; });
	edgeIndexManager->dropIndex("property", "temp_key");

	graph::Edge oldEdge(10, 1, 2, 0);
	oldEdge.addProperty("some_key", "old_val");

	graph::Edge newEdge(10, 1, 2, 0);
	newEdge.addProperty("some_key", "new_val");

	// Should return early at indexedKeys.empty() without error
	EXPECT_NO_THROW(edgeIndexManager->onEntityUpdated(oldEdge, newEdge));
}

// ============================================================================
// Additional Branch Coverage Tests
// ============================================================================

TEST_F(EntityTypeIndexManagerTest, UpdateLabelIndex_DeletedEntityWithValidLabel) {
	// Cover: isDeleted && !newLabel.empty() -> true path (line 90)
	// where we delete an entity that actually has a valid label
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t labelId = dataManager->getOrCreateTokenId("DeleteTarget");
	graph::Node node(100, labelId);
	nodeIndexManager->onEntityAdded(node);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("DeleteTarget").size(), 1UL);

	// Delete the node - this should call removeNode for the label
	nodeIndexManager->onEntityDeleted(node);
	EXPECT_TRUE(labelIdx->findNodes("DeleteTarget").empty());
}

TEST_F(EntityTypeIndexManagerTest, UpdateLabelIndex_NotDeletedOldLabelEmptyNewLabelValid) {
	// Cover: !isDeleted, oldLabel.empty(), newLabel not empty (line 96)
	// oldLabel != newLabel is true when old is empty and new is non-empty
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	// Node with no label -> node with label
	graph::Node oldNode(200, 0);
	nodeIndexManager->onEntityAdded(oldNode);

	int64_t labelId = dataManager->getOrCreateTokenId("AddedLabel");
	graph::Node newNode(200, labelId);
	nodeIndexManager->onEntityUpdated(oldNode, newNode);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("AddedLabel").size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, UpdateLabelIndex_NotDeletedOldLabelValidNewLabelEmpty) {
	// Cover: !isDeleted, oldLabel not empty, newLabel empty
	// oldLabel != newLabel -> removeNode for old, skip addNode for empty new
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t labelId = dataManager->getOrCreateTokenId("RemoveLabel");
	graph::Node oldNode(300, labelId);
	nodeIndexManager->onEntityAdded(oldNode);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("RemoveLabel").size(), 1UL);

	// Change to no label
	graph::Node newNode(300, 0);
	nodeIndexManager->onEntityUpdated(oldNode, newNode);

	EXPECT_TRUE(labelIdx->findNodes("RemoveLabel").empty());
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_MixedLabelAndNoLabelNodes) {
	// Cover: nodesByLabelId handling where some nodes have labels and some don't
	// Also covers the case where labelId == 0 is skipped (line 182)
	(void) nodeIndexManager->createLabelIndex([]() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("MixedBatch");
	std::vector<graph::Node> nodes;

	// Some nodes with labels, some without
	nodes.emplace_back(graph::Node(1100, lbl));
	nodes.emplace_back(graph::Node(1101, 0));  // No label
	nodes.emplace_back(graph::Node(1102, lbl));
	nodes.emplace_back(graph::Node(1103, 0));  // No label

	nodeIndexManager->onEntitiesAdded(nodes);

	auto labelIdx = nodeIndexManager->getLabelIndex();
	// Only nodes with labels should be indexed
	EXPECT_EQ(labelIdx->findNodes("MixedBatch").size(), 2UL);
}

TEST_F(EntityTypeIndexManagerTest, OnEntityDeleted_EdgeWithLabel) {
	// Cover: onEntityDeleted Edge template with valid label
	(void) edgeIndexManager->createLabelIndex([]() { return true; });
	(void) edgeIndexManager->createPropertyIndex("edge_weight", []() { return true; });

	int64_t labelId = dataManager->getOrCreateTokenId("DEL_EDGE");
	graph::Edge edge(600, 1, 2, labelId);
	edge.addProperty("edge_weight", 5.0);

	edgeIndexManager->onEntityAdded(edge);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	auto propIdx = edgeIndexManager->getPropertyIndex();
	EXPECT_EQ(labelIdx->findNodes("DEL_EDGE").size(), 1UL);
	EXPECT_EQ(propIdx->findExactMatch("edge_weight", 5.0).size(), 1UL);

	edgeIndexManager->onEntityDeleted(edge);
	EXPECT_TRUE(labelIdx->findNodes("DEL_EDGE").empty());
	EXPECT_TRUE(propIdx->findExactMatch("edge_weight", 5.0).empty());
}

TEST_F(EntityTypeIndexManagerTest, OnEntityUpdated_EdgeLabelChange) {
	// Cover: onEntityUpdated Edge template where label changes
	(void) edgeIndexManager->createLabelIndex([]() { return true; });

	int64_t labelA = dataManager->getOrCreateTokenId("EDGE_A");
	int64_t labelB = dataManager->getOrCreateTokenId("EDGE_B");

	graph::Edge oldEdge(700, 1, 2, labelA);
	edgeIndexManager->onEntityAdded(oldEdge);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("EDGE_A").size(), 1UL);

	graph::Edge newEdge(700, 1, 2, labelB);
	edgeIndexManager->onEntityUpdated(oldEdge, newEdge);

	EXPECT_TRUE(labelIdx->findNodes("EDGE_A").empty());
	EXPECT_EQ(labelIdx->findNodes("EDGE_B").size(), 1UL);
}

// ============================================================================
// Additional Branch Coverage Tests - Edge template uncovered False paths
// ============================================================================

TEST_F(EntityTypeIndexManagerTest, EdgeUpdateLabelIndex_EdgeWithLabelIdZero) {
	// Cover Edge template: entity.getTypeId() != 0 -> False (line 85)
	// An edge with labelId=0 should skip label index update
	(void) edgeIndexManager->createLabelIndex([]() { return true; });

	graph::Edge edgeNoLabel(800, 1, 2, 0); // labelId = 0
	edgeIndexManager->onEntityAdded(edgeNoLabel);

	// No labels should be indexed for this edge
	auto labelIdx = edgeIndexManager->getLabelIndex();
	// Can't query empty label, just verify no crash
	SUCCEED();
}

TEST_F(EntityTypeIndexManagerTest, EdgeDeleteWithEmptyLabel) {
	// Cover Edge template: isDeleted && newLabel.empty() -> removeNode NOT called (line 90)
	// Delete an edge with labelId=0 - the label is empty so removeNode should be skipped
	(void) edgeIndexManager->createLabelIndex([]() { return true; });

	// Add an edge with a label
	int64_t labelId = dataManager->getOrCreateTokenId("KeepEdge");
	graph::Edge edgeWithLabel(810, 1, 2, labelId);
	edgeIndexManager->onEntityAdded(edgeWithLabel);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("KeepEdge").size(), 1UL);

	// Delete an edge with no label (labelId=0)
	graph::Edge edgeNoLabel(811, 3, 4, 0);
	edgeIndexManager->onEntityDeleted(edgeNoLabel);

	// The existing edge should still be in the index (the delete was for a different edge)
	EXPECT_EQ(labelIdx->findNodes("KeepEdge").size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, EdgeUpdateSameLabel) {
	// Cover Edge template: oldLabel != newLabel -> False (line 96, same label)
	// Update an edge where old and new label are the same
	(void) edgeIndexManager->createLabelIndex([]() { return true; });

	int64_t labelId = dataManager->getOrCreateTokenId("SameEdgeLabel");
	graph::Edge oldEdge(820, 1, 2, labelId);
	edgeIndexManager->onEntityAdded(oldEdge);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("SameEdgeLabel").size(), 1UL);

	// Update with same label - no add/remove should happen
	graph::Edge newEdge(820, 1, 2, labelId);
	edgeIndexManager->onEntityUpdated(oldEdge, newEdge);

	EXPECT_EQ(labelIdx->findNodes("SameEdgeLabel").size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, EdgeUpdateFromValidToEmptyLabel) {
	// Cover Edge template: !newLabel.empty() -> False in non-delete path (line 96)
	// Update an edge from valid label to no label
	(void) edgeIndexManager->createLabelIndex([]() { return true; });

	int64_t labelId = dataManager->getOrCreateTokenId("RemoveEdgeLabel");
	graph::Edge oldEdge(830, 1, 2, labelId);
	edgeIndexManager->onEntityAdded(oldEdge);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("RemoveEdgeLabel").size(), 1UL);

	// Update to no label (labelId=0)
	graph::Edge newEdge(830, 1, 2, 0);
	edgeIndexManager->onEntityUpdated(oldEdge, newEdge);

	// Old label should be removed
	EXPECT_TRUE(labelIdx->findNodes("RemoveEdgeLabel").empty());
}

TEST_F(EntityTypeIndexManagerTest, UpdatePropertyIndexes_IndexedKeysEmptyPath) {
	// Cover: indexedKeys.empty() -> True (line 116)
	// Create a property index then drop all keys from it so indexedKeys is empty
	// but propertyIndex itself is not empty (has been initialized)
	(void) nodeIndexManager->createPropertyIndex("temp_indexed_key", []() { return true; });

	// Drop the key - indexedKeys should now be empty but property index is not "empty"
	nodeIndexManager->dropIndex("property", "temp_indexed_key");

	// Now trigger updatePropertyIndexes through an entity update
	graph::Node oldN(2000, 1);
	oldN.addProperty("temp_indexed_key", "old_value");

	graph::Node newN(2000, 1);
	newN.addProperty("temp_indexed_key", "new_value");

	// Should hit indexedKeys.empty() -> True and return early
	EXPECT_NO_THROW(nodeIndexManager->onEntityUpdated(oldN, newN));
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_EdgesAllWithoutLabels) {
	// Cover Edge template: labelStr.empty() -> True in onEntitiesAdded batch (line 209 False path)
	// This ensures all edges resolve to empty label strings, so nodesByLabelStr stays empty
	(void) edgeIndexManager->createLabelIndex([]() { return true; });

	std::vector<graph::Edge> edges;
	for (int i = 0; i < 3; ++i) {
		graph::Edge e(i + 2000, 1, 2, 0); // labelId=0 -> resolveTokenName returns ""
		edges.push_back(e);
	}

	// Should not crash - edges with empty labels are skipped in batch
	edgeIndexManager->onEntitiesAdded(edges);
	SUCCEED();
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_EdgesBatchPropertyNotEmpty_PropertyIndexEmpty) {
	// Cover Edge template: !propBatch.empty() && !propertyIndex_->isEmpty() where
	// propertyIndex_->isEmpty() returns True (line 220 Branch 220:30 False path)
	// Create label index only, no property index. But edges have properties.
	(void) edgeIndexManager->createLabelIndex([]() { return true; });
	// Note: Do NOT create a property index for edges

	int64_t lbl = dataManager->getOrCreateTokenId("BATCH_EDGE_NP");
	std::vector<graph::Edge> edges;
	for (int i = 0; i < 3; ++i) {
		graph::Edge e(i + 2100, 1, 2, lbl);
		e.addProperty("unindexed_edge_prop", i * 5);
		edges.push_back(e);
	}

	// propBatch will be empty because hasKeyIndexed returns false for all props.
	// But this exercises the batch code path for edges with properties when no property index exists.
	edgeIndexManager->onEntitiesAdded(edges);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("BATCH_EDGE_NP").size(), 3UL);
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_EdgesMixedLabelsAndNoLabels) {
	// Cover Edge template: mixed batch where some edges have labels and some don't
	// Exercises nodesByLabelId skip for labelId==0 in Edge template
	(void) edgeIndexManager->createLabelIndex([]() { return true; });

	int64_t lbl = dataManager->getOrCreateTokenId("MIXED_EDGE");
	std::vector<graph::Edge> edges;

	// Edges with labels
	edges.emplace_back(graph::Edge(2200, 1, 2, lbl));
	edges.emplace_back(graph::Edge(2201, 1, 2, lbl));
	// Edges without labels (labelId=0)
	edges.emplace_back(graph::Edge(2202, 1, 2, 0));
	edges.emplace_back(graph::Edge(2203, 1, 2, 0));

	edgeIndexManager->onEntitiesAdded(edges);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("MIXED_EDGE").size(), 2UL);
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_EdgesWithNoLabels) {
	// Cover Edge template: entity.getTypeId() != 0 -> False in onEntitiesAdded (line 182)
	(void) edgeIndexManager->createLabelIndex([]() { return true; });
	(void) edgeIndexManager->createPropertyIndex("ep", []() { return true; });

	std::vector<graph::Edge> edges;
	for (int i = 0; i < 3; ++i) {
		graph::Edge e(i + 900, 1, 2, 0); // labelId = 0
		e.addProperty("ep", i);
		edges.push_back(e);
	}

	// Edges without labels - labelId=0 path should be skipped in batch
	edgeIndexManager->onEntitiesAdded(edges);

	// Properties should still be indexed
	auto propIdx = edgeIndexManager->getPropertyIndex();
	EXPECT_EQ(propIdx->findExactMatch("ep", 1).size(), 1UL);
}

TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_EdgesWithPropertyNotIndexed) {
	// Cover Edge template: hasKeyIndexed -> False in batch (line 191)
	(void) edgeIndexManager->createLabelIndex([]() { return true; });
	// Do NOT create property index for "unindexed_edge_prop"

	int64_t lbl = dataManager->getOrCreateTokenId("EdgePropSkip");
	std::vector<graph::Edge> edges;
	graph::Edge e(950, 1, 2, lbl);
	e.addProperty("unindexed_edge_prop", 42);
	edges.push_back(e);

	edgeIndexManager->onEntitiesAdded(edges);

	auto labelIdx = edgeIndexManager->getLabelIndex();
	EXPECT_EQ(labelIdx->findNodes("EdgePropSkip").size(), 1UL);
}

// Cover branch 203:35 False path: !labelIndex_->isEmpty() -> False
// when nodesByLabelId is non-empty but label index was never created
TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_NodeBatch_LabelIndexNotCreated) {
	// Do NOT create label index - labelIndex_->isEmpty() should return true
	// But entities have labels, so nodesByLabelId will be non-empty
	int64_t lbl = dataManager->getOrCreateTokenId("NoIndexLabel");
	std::vector<graph::Node> nodes;
	graph::Node n(500, lbl);
	n.addProperty("somekey", 42);
	nodes.push_back(n);

	// This should exercise the path where nodesByLabelId is non-empty
	// but labelIndex is empty, so the label batch is skipped
	nodeIndexManager->onEntitiesAdded(nodes);
	SUCCEED();
}

// Same for Edge template
TEST_F(EntityTypeIndexManagerTest, OnEntitiesAdded_EdgeBatch_LabelIndexNotCreated) {
	// Do NOT create label index for edges
	int64_t lbl = dataManager->getOrCreateTokenId("NoEdgeIndexLabel");
	std::vector<graph::Edge> edges;
	graph::Edge e(501, 1, 2, lbl);
	e.addProperty("edgekey", 99);
	edges.push_back(e);

	edgeIndexManager->onEntitiesAdded(edges);
	SUCCEED();
}
