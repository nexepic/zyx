/**
 * @file test_IndexEntityManager.cpp
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
#include "graph/core/Database.hpp"
#include "graph/core/Index.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/IndexEntityManager.hpp"

class IndexEntityManagerTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::storage::IndexEntityManager> indexEntityManager;
	std::shared_ptr<graph::storage::DeletionManager> deletionManager;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		// Create a unique temporary database file for each test
		testFilePath = std::filesystem::temp_directory_path() / ("test_indexEntityManager_" + to_string(uuid) + ".dat");

		// Initialize database and get the necessary manager
		db = std::make_unique<graph::Database>(testFilePath.string());
		db->open();
		storage = db->getStorage();
		dataManager = storage->getDataManager();
		deletionManager = dataManager->getDeletionManager();
		// Get the manager instance that was created by the DataManager
		indexEntityManager = dataManager->getIndexEntityManager();
	}

	void TearDown() override {
		// Release shared_ptrs before closing database
		indexEntityManager.reset();
		deletionManager.reset();
		dataManager.reset();
		storage.reset();

		if (db) {
			db->close();
		}
		db.reset();

		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}

	// Helper function to create a valid Index entity
	static graph::Index createTestIndex(graph::Index::NodeType nodeType, uint32_t indexType) {
		// ID is 0 before being added to the database
		graph::Index index(0, nodeType, indexType);
		return index;
	}
};

// Test adding and retrieving an index
TEST_F(IndexEntityManagerTest, AddAndGetIndex) {
	// Create an index using the correct API
	graph::Index index = createTestIndex(graph::Index::NodeType::LEAF, 1);

	// Add the index
	indexEntityManager->add(index);
	EXPECT_NE(index.getId(), 0) << "Index should have a non-zero ID after adding";

	// Get the index
	graph::Index retrievedIndex = indexEntityManager->get(index.getId());
	EXPECT_EQ(retrievedIndex.getId(), index.getId()) << "Retrieved index should have the same ID";
	EXPECT_EQ(retrievedIndex.getNodeType(), graph::Index::NodeType::LEAF)
			<< "Retrieved index should have the same node type";
	EXPECT_EQ(retrievedIndex.getIndexType(), 1U) << "Retrieved index should have the same index type";
}

// Test updating an index
TEST_F(IndexEntityManagerTest, UpdateIndex) {
	// Create and add an index
	graph::Index index = createTestIndex(graph::Index::NodeType::LEAF, 1);
	indexEntityManager->add(index);
	int64_t indexId = index.getId();

	// Update a mutable property of the index
	index.setLevel(5);
	indexEntityManager->update(index);

	// Get the updated index
	graph::Index retrievedIndex = indexEntityManager->get(indexId);
	EXPECT_EQ(retrievedIndex.getLevel(), 5) << "Index level should be updated";
}

// Test removing an index
TEST_F(IndexEntityManagerTest, RemoveIndex) {
	// Create and add an index
	graph::Index index = createTestIndex(graph::Index::NodeType::INTERNAL, 2);
	indexEntityManager->add(index);
	int64_t indexId = index.getId();

	// Verify the index was added
	graph::Index retrievedIndex = indexEntityManager->get(indexId);
	EXPECT_EQ(retrievedIndex.getId(), indexId) << "Index should be retrievable after adding";

	// Remove the index
	indexEntityManager->remove(index);

	// Clear cache to ensure we are reading from disk, not an in-memory copy
	dataManager->clearCache();

	// Verify the index was removed
	graph::Index removedIndex = indexEntityManager->get(indexId);
	EXPECT_EQ(removedIndex.getId(), 0) << "Index should not be retrievable after removal";
}

// Test handling of non-existent index
TEST_F(IndexEntityManagerTest, GetNonExistentIndex) {
	// Try to get a non-existent index
	graph::Index nonExistentIndex = indexEntityManager->get(999999);
	EXPECT_EQ(nonExistentIndex.getId(), 0) << "Non-existent index should return default index with ID 0";
}

// Test batch operations
TEST_F(IndexEntityManagerTest, BatchOperations) {
	// Create multiple indexes
	std::vector<graph::Index> indexes;
	std::vector<int64_t> indexIds;

	for (int i = 0; i < 5; i++) {
		graph::Index index = createTestIndex(graph::Index::NodeType::LEAF, i);
		indexEntityManager->add(index);
		indexes.push_back(index);
		indexIds.push_back(index.getId());
	}

	// Test getBatch
	auto retrievedIndexes = indexEntityManager->getBatch(indexIds);
	EXPECT_EQ(retrievedIndexes.size(), 5UL) << "Should retrieve all 5 indexes";

	// Test after removing one index
	indexEntityManager->remove(indexes[2]);
	retrievedIndexes = indexEntityManager->getBatch(indexIds);
	EXPECT_EQ(retrievedIndexes.size(), 4UL) << "Should retrieve only active indexes";
}

// Test index ID allocation
TEST_F(IndexEntityManagerTest, IndexIdAllocation) {
	// Create multiple indexes and verify IDs are unique
	graph::Index index1 = createTestIndex(graph::Index::NodeType::LEAF, 1);
	graph::Index index2 = createTestIndex(graph::Index::NodeType::LEAF, 1);

	indexEntityManager->add(index1);
	indexEntityManager->add(index2);

	EXPECT_NE(index1.getId(), 0);
	EXPECT_NE(index2.getId(), 0);
	EXPECT_NE(index1.getId(), index2.getId()) << "Index IDs should be unique";
}
