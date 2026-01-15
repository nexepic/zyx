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
