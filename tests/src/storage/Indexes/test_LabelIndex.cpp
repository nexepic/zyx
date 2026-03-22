/**
 * @file test_LabelIndex.cpp
 * @author Nexepic
 * @date 2025/7/29
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
#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/LabelIndex.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"

class LabelIndexTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_labelIndex_" + to_string(uuid) + ".dat");
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();

		// Updated: Explicitly get the LabelIndex for nodes to make the test target clear.
		labelIndex = database->getQueryEngine()->getIndexManager()->getNodeIndexManager()->getLabelIndex();
	}

	void TearDown() override {
		database->close();
		database.reset();
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::LabelIndex> labelIndex;
};

TEST_F(LabelIndexTest, AddAndFindNode) {
	labelIndex->createIndex();
	int64_t nodeId = 123;
	std::string label = "Person";
	labelIndex->addNode(nodeId, label);
	auto found = labelIndex->findNodes(label);
	ASSERT_EQ(found.size(), 1UL);
	EXPECT_EQ(found[0], nodeId);
	EXPECT_FALSE(labelIndex->isEmpty());
}

TEST_F(LabelIndexTest, HasLabel) {
	int64_t nodeId = 456;
	std::string label = "Company";
	labelIndex->addNode(nodeId, label);
	EXPECT_TRUE(labelIndex->hasLabel(nodeId, label));
	EXPECT_FALSE(labelIndex->hasLabel(nodeId, "Other"));
}

TEST_F(LabelIndexTest, IsEmptyInitially) {
	// Default is disabled -> considered empty
	EXPECT_TRUE(labelIndex->isEmpty());
}

TEST_F(LabelIndexTest, RemoveNode) {
	int64_t nodeId = 789;
	std::string label = "City";
	labelIndex->addNode(nodeId, label);
	EXPECT_TRUE(labelIndex->hasLabel(nodeId, label));
	labelIndex->removeNode(nodeId, label);
	EXPECT_FALSE(labelIndex->hasLabel(nodeId, label));
	auto found = labelIndex->findNodes(label);
	EXPECT_TRUE(found.empty());
}

TEST_F(LabelIndexTest, ClearAndDrop) {
	labelIndex->createIndex();

	labelIndex->addNode(1, "A");
	labelIndex->addNode(2, "B");
	EXPECT_FALSE(labelIndex->isEmpty());

	labelIndex->clear();
	EXPECT_FALSE(labelIndex->isEmpty());

	// Act & Assert: drop() should work correctly and not throw.
	EXPECT_NO_THROW(labelIndex->drop());
	// After drop, enabled_ is false.
	EXPECT_TRUE(labelIndex->isEmpty());
}

TEST_F(LabelIndexTest, SaveAndLoadState) {
	// Arrange
	labelIndex->createIndex();

	int64_t nodeId = 1;
	std::string label = "Reload";

	// Act: Add a node directly to the index and explicitly save its state.
	labelIndex->addNode(nodeId, label);

	labelIndex->flush();

	// Close and reopen the database to simulate a restart.
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();

	// Get the new index instance after reloading.
	auto labelIndexNew = database->getQueryEngine()->getIndexManager()->getNodeIndexManager()->getLabelIndex();

	// Assert: The reloaded index should contain the persisted data.
	EXPECT_FALSE(labelIndexNew->isEmpty());
	auto found = labelIndexNew->findNodes(label);
	ASSERT_EQ(found.size(), 1UL);
	EXPECT_EQ(found[0], nodeId);
}

TEST_F(LabelIndexTest, FlushNoThrow) {
	labelIndex->addNode(1, "Flush");
	// A simple sanity check that flush can be called without error.
	EXPECT_NO_THROW(labelIndex->flush());
}

TEST_F(LabelIndexTest, AddNodesBatch_MixedScenario) {
	labelIndex->createIndex();

	// Prepare batch data
	std::unordered_map<std::string, std::vector<int64_t>> batchData;

	// Group 1: Standard batch
	batchData["GroupA"] = {1, 2, 3};

	// Group 2: Single item batch
	batchData["GroupB"] = {4};

	// Group 3: Empty vector (Coverage for "if (entityIds.empty()) continue")
	batchData["GroupEmpty"] = {};

	// Execute Batch Add
	// This covers the loop, empty check, and batch insertion logic.
	labelIndex->addNodesBatch(batchData);

	// Verify Results
	auto resA = labelIndex->findNodes("GroupA");
	EXPECT_EQ(resA.size(), 3UL);
	EXPECT_TRUE(labelIndex->hasLabel(1, "GroupA"));
	EXPECT_TRUE(labelIndex->hasLabel(2, "GroupA"));
	EXPECT_TRUE(labelIndex->hasLabel(3, "GroupA"));

	auto resB = labelIndex->findNodes("GroupB");
	EXPECT_EQ(resB.size(), 1UL);
	EXPECT_TRUE(labelIndex->hasLabel(4, "GroupB"));

	// Verify Empty group was skipped
	auto resEmpty = labelIndex->findNodes("GroupEmpty");
	EXPECT_TRUE(resEmpty.empty());
}

TEST_F(LabelIndexTest, AddNodesBatch_InitializeRoot) {
	// Ensure index is enabled but physically empty (rootId = 0)
	labelIndex->createIndex();
	labelIndex->clear(); // Force rootId_ = 0

	// This should trigger: if (rootId_ == 0) rootId_ = treeManager_->initialize();
	std::unordered_map<std::string, std::vector<int64_t>> data;
	data["InitLabel"] = {100};

	labelIndex->addNodesBatch(data);

	EXPECT_FALSE(labelIndex->isEmpty());
	EXPECT_TRUE(labelIndex->hasPhysicalData()); // Internal check if rootId != 0
	EXPECT_TRUE(labelIndex->hasLabel(100, "InitLabel"));
}

TEST_F(LabelIndexTest, AddNodesBatch_TriggerRootSplit) {
	labelIndex->createIndex();

	// Create a large batch to force tree growth/splits
	// The exact number depends on B+Tree node size constants,
	// but usually a few hundred or thousand entries will suffice.
	std::unordered_map<std::string, std::vector<int64_t>> largeBatch;
	std::vector<int64_t> ids;
	for (int i = 0; i < 2000; ++i) {
		ids.push_back(i + 1000);
	}
	largeBatch["LargeGroup"] = ids;

	// This should trigger inserts that cause splits, potentially changing rootId_
	// Covering: if (newRootId != rootId_) rootId_ = newRootId;
	labelIndex->addNodesBatch(largeBatch);

	auto results = labelIndex->findNodes("LargeGroup");
	EXPECT_EQ(results.size(), 2000UL);
}

TEST_F(LabelIndexTest, Initialize_DisabledByDefault) {
	// Manually create a fresh index that hasn't been enabled
	auto tempIndex = std::make_shared<graph::query::indexes::LabelIndex>(
			dataManager, fileStorage->getSystemStateManager(), 100, "temp.idx");

	// Should be empty/disabled
	EXPECT_TRUE(tempIndex->isEmpty());
}

TEST_F(LabelIndexTest, AddNodesBatch_EmptyInput) {
	labelIndex->createIndex();
	std::unordered_map<std::string, std::vector<int64_t>> emptyBatch;

	// Should do nothing, return immediately
	labelIndex->addNodesBatch(emptyBatch);
	EXPECT_TRUE(labelIndex->findNodes("Any").empty());
}

TEST_F(LabelIndexTest, HasLabel_EmptyRoot) {
	// Ensure rootId is 0
	labelIndex->clear();
	EXPECT_FALSE(labelIndex->hasLabel(1, "Test"));
}

TEST_F(LabelIndexTest, RemoveNode_EmptyRoot) {
	labelIndex->clear();
	// Should simply return
	labelIndex->removeNode(1, "Test");
	SUCCEED();
}

TEST_F(LabelIndexTest, SaveState_WithDataAndEnabled) {
	// Cover: rootId_ != 0 branch in saveState (line 101) and enabled_ branch (line 109)
	labelIndex->createIndex();
	labelIndex->addNode(1, "SaveStateTest");

	// Now rootId_ != 0 and enabled_ == true
	EXPECT_NO_THROW(labelIndex->saveState());

	// Verify persistence by re-creating
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	auto newIndex = database->getQueryEngine()->getIndexManager()->getNodeIndexManager()->getLabelIndex();
	EXPECT_TRUE(newIndex->hasLabel(1, "SaveStateTest"));
}

TEST_F(LabelIndexTest, ClearWithPhysicalData) {
	// Cover: rootId_ != 0 branch in clear() (line 76)
	labelIndex->createIndex();
	labelIndex->addNode(1, "ClearMe");
	EXPECT_TRUE(labelIndex->hasPhysicalData());

	labelIndex->clear();
	// After clear, rootId_ should be 0
	EXPECT_FALSE(labelIndex->hasPhysicalData());

	// But it should still be enabled
	EXPECT_FALSE(labelIndex->isEmpty());
}

TEST_F(LabelIndexTest, Initialize_EnabledFallback_WithPhysicalData) {
	// Cover: !enabled_ && rootId_ != 0 -> force enable (line 52-54)
	// This requires rootId != 0 but enabled = false in the state
	// We achieve this by saving data, then removing the config key
	labelIndex->createIndex();
	labelIndex->addNode(1, "FallbackTest");
	labelIndex->saveState();

	// Manually remove the "enabled" config but keep the root ID
	auto sysState = fileStorage->getSystemStateManager();
	std::string configKey = std::string(graph::storage::state::keys::Node::LABEL_ROOT)
		+ graph::storage::state::keys::SUFFIX_CONFIG;
	sysState->remove(configKey);

	// Re-open database - initialize should detect rootId != 0 and force enable
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	auto newIndex = database->getQueryEngine()->getIndexManager()->getNodeIndexManager()->getLabelIndex();

	// Should be enabled due to fallback
	EXPECT_FALSE(newIndex->isEmpty());
	EXPECT_TRUE(newIndex->hasPhysicalData());
}

TEST_F(LabelIndexTest, FindNodes_WithEmptyRoot) {
	// Cover: rootId_ == 0 in findNodes (line 180)
	labelIndex->createIndex();
	labelIndex->clear(); // Force rootId_ = 0

	auto result = labelIndex->findNodes("Any");
	EXPECT_TRUE(result.empty());
}

TEST_F(LabelIndexTest, AddNode_InitializesTreeWhenRootZero) {
	// Cover: rootId_ == 0 in addNode -> initialize (line 123-124)
	labelIndex->createIndex();
	labelIndex->clear(); // rootId_ = 0

	// addNode should initialize the tree
	labelIndex->addNode(42, "NewNode");
	EXPECT_TRUE(labelIndex->hasPhysicalData());
	EXPECT_TRUE(labelIndex->hasLabel(42, "NewNode"));
}

TEST_F(LabelIndexTest, AddNode_RootChanged) {
	// Cover: newRootId != rootId_ in addNode (line 127-128)
	labelIndex->createIndex();

	// Insert many entries to potentially trigger root split
	for (int i = 1; i <= 50; ++i) {
		labelIndex->addNode(i, "Label_" + std::to_string(i));
	}

	// Verify entries are findable
	EXPECT_TRUE(labelIndex->hasLabel(1, "Label_1"));
	EXPECT_TRUE(labelIndex->hasLabel(50, "Label_50"));
}

TEST_F(LabelIndexTest, RemoveNode_WithData) {
	// Cover: rootId_ != 0 in removeNode (line 169)
	labelIndex->createIndex();
	labelIndex->addNode(1, "ToRemove");
	EXPECT_TRUE(labelIndex->hasLabel(1, "ToRemove"));

	labelIndex->removeNode(1, "ToRemove");
	EXPECT_FALSE(labelIndex->hasLabel(1, "ToRemove"));
}

TEST_F(LabelIndexTest, HasLabel_WithData) {
	// Cover: rootId_ != 0 in hasLabel (line 190)
	labelIndex->createIndex();
	labelIndex->addNode(1, "Exists");

	EXPECT_TRUE(labelIndex->hasLabel(1, "Exists"));
	EXPECT_FALSE(labelIndex->hasLabel(1, "NotExists"));
	EXPECT_FALSE(labelIndex->hasLabel(999, "Exists"));
}

// =========================================================================
// Additional Branch Coverage Tests for LabelIndex
// =========================================================================

/**
 * @brief Test addNodesBatch with many distinct labels to trigger root split.
 * Covers: addNodesBatch line 161 (newRootId != rootId_ true branch).
 *
 * Using many distinct labels ensures many B-tree insertions happen,
 * increasing the chance of root splits during batch operations.
 */
TEST_F(LabelIndexTest, AddNodesBatch_ManyDistinctLabelsForRootSplit) {
	labelIndex->createIndex();

	// Create many distinct label groups to force root splits
	std::unordered_map<std::string, std::vector<int64_t>> batchData;
	for (int i = 0; i < 500; ++i) {
		std::string label = "Label_" + std::to_string(i);
		batchData[label] = {static_cast<int64_t>(i + 1)};
	}

	labelIndex->addNodesBatch(batchData);

	// Verify a sample of entries
	EXPECT_TRUE(labelIndex->hasLabel(1, "Label_0"));
	EXPECT_TRUE(labelIndex->hasLabel(250, "Label_249"));
	EXPECT_TRUE(labelIndex->hasLabel(500, "Label_499"));
}

/**
 * @brief Test addNodesBatch with large batch per label to trigger root split.
 * Covers: addNodesBatch line 161 (newRootId != rootId_ true branch)
 * by inserting many entries under many labels in a single batch call.
 */
TEST_F(LabelIndexTest, AddNodesBatch_MultipleLabelsLargeBatch) {
	labelIndex->createIndex();

	std::unordered_map<std::string, std::vector<int64_t>> batchData;
	// Create 100 labels with 50 nodes each
	int64_t idCounter = 1;
	for (int labelIdx = 0; labelIdx < 100; ++labelIdx) {
		std::string label = "MultiLabel_" + std::to_string(labelIdx);
		std::vector<int64_t> ids;
		for (int j = 0; j < 50; ++j) {
			ids.push_back(idCounter++);
		}
		batchData[label] = ids;
	}

	labelIndex->addNodesBatch(batchData);

	// Verify data integrity
	EXPECT_TRUE(labelIndex->hasLabel(1, "MultiLabel_0"));
	auto results = labelIndex->findNodes("MultiLabel_50");
	EXPECT_EQ(results.size(), 50UL);
}

/**
 * @brief Test saveState when enabled is false (not enabled, no data).
 * Covers: saveState line 109 (enabled_ false branch - does not persist config).
 */
TEST_F(LabelIndexTest, SaveState_NotEnabled) {
	// labelIndex starts disabled (isEmpty() returns true)
	EXPECT_TRUE(labelIndex->isEmpty());

	// Call saveState when not enabled - should not persist enabled config
	EXPECT_NO_THROW(labelIndex->saveState());

	// Still disabled
	EXPECT_TRUE(labelIndex->isEmpty());
}

/**
 * @brief Test saveState when enabled but no data (rootId_ == 0).
 * Covers: saveState line 101 false branch (rootId_ == 0, skip root ID persistence).
 */
TEST_F(LabelIndexTest, SaveState_EnabledButNoData) {
	labelIndex->createIndex();

	// Enabled but rootId_ == 0 (no nodes added)
	EXPECT_FALSE(labelIndex->isEmpty()); // enabled
	EXPECT_FALSE(labelIndex->hasPhysicalData()); // but no physical data

	// saveState should skip the rootId persistence (line 101 false branch)
	EXPECT_NO_THROW(labelIndex->saveState());
}

/**
 * @brief Direct test for initialize fallback: rootId_ != 0 when !enabled_.
 * Covers: LabelIndex.cpp line 52 (!enabled_ && rootId_ != 0 -> force enable).
 *
 * This creates a scenario where the persisted state has physical data
 * (rootId != 0) but the enabled config was removed/missing.
 */
TEST_F(LabelIndexTest, Initialize_ForcesEnableWhenRootExists) {
	// Step 1: Create index and add data
	labelIndex->createIndex();
	labelIndex->addNode(1, "ForceEnable");
	labelIndex->addNode(2, "ForceEnable");
	labelIndex->saveState();

	// Step 2: Get the state key and remove the config but keep root
	auto sysState = fileStorage->getSystemStateManager();
	std::string stateKey = std::string(graph::storage::state::keys::Node::LABEL_ROOT);

	// Remove enabled config
	std::string configKey = stateKey + graph::storage::state::keys::SUFFIX_CONFIG;
	sysState->remove(configKey);

	// Step 3: Create a fresh LabelIndex instance with this inconsistent state
	// Use a different index type to avoid interference with the main index
	auto freshIndex = std::make_shared<graph::query::indexes::LabelIndex>(
			dataManager, sysState,
			graph::query::indexes::IndexTypes::NODE_LABEL_TYPE,
			stateKey);

	// The fallback at line 52 should force enable since rootId_ != 0
	EXPECT_FALSE(freshIndex->isEmpty()); // Should be enabled
	EXPECT_TRUE(freshIndex->hasPhysicalData());
}

/**
 * @brief Test addNodesBatch followed by persistence and reload to verify root changes.
 * Covers: addNodesBatch line 161 true branch (root changes persisted correctly).
 */
TEST_F(LabelIndexTest, AddNodesBatch_PersistAfterRootSplit) {
	labelIndex->createIndex();

	// Insert a large batch that should trigger root splits
	std::unordered_map<std::string, std::vector<int64_t>> batchData;
	std::vector<int64_t> ids;
	for (int i = 1; i <= 3000; ++i) {
		ids.push_back(i);
	}
	batchData["SplitLabel"] = ids;

	// Also add many distinct labels
	for (int i = 0; i < 200; ++i) {
		batchData["Dist_" + std::to_string(i)] = {static_cast<int64_t>(10000 + i)};
	}

	labelIndex->addNodesBatch(batchData);
	labelIndex->saveState();

	// Reload and verify
	database->close();
	database.reset();

	database = std::make_unique<graph::Database>(testFilePath.string());
	database->open();
	auto newIndex = database->getQueryEngine()->getIndexManager()->getNodeIndexManager()->getLabelIndex();

	EXPECT_FALSE(newIndex->isEmpty());
	auto results = newIndex->findNodes("SplitLabel");
	EXPECT_EQ(results.size(), 3000UL);
	EXPECT_TRUE(newIndex->hasLabel(10000, "Dist_0"));
	EXPECT_TRUE(newIndex->hasLabel(10199, "Dist_199"));
}
