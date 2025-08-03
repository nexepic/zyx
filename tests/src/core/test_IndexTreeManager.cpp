/**
 * @file test_IndexTreeManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/13
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"

class IndexTreeManagerTest : public ::testing::Test {
protected:
	// SetUpTestSuite is run once for all tests in this file
	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		std::filesystem::path testFilePath = std::filesystem::temp_directory_path() /
											 ("test_db_file_manager_" + boost::uuids::to_string(uuid) + ".dat");

		database_ = std::make_unique<graph::Database>(testFilePath.string());
		database_->open();
		dataManager_ = database_->getStorage()->getDataManager();
	}

	static void TearDownTestSuite() {
		if (database_) {
			database_->close();
			database_.reset();
		}
		dataManager_.reset();
	}

	// SetUp is run before each individual TEST_F
	void SetUp() override {
		// For each test, create a fresh tree manager with a SMALL capacity to test splits.
		treeManager_ = std::make_shared<graph::query::indexes::IndexTreeManager>(
				dataManager_, graph::query::indexes::LabelIndex::LABEL_INDEX_TYPE,
				3, // maxKeysPerLeaf: force leaf split after 3 insertions
				3 // maxKeysPerInternal: force internal node to split too
		);
		rootId_ = treeManager_->initialize();
	}

	void TearDown() override {
		// Clean up the tree created in SetUp to ensure tests are isolated
		if (rootId_ != 0) {
			treeManager_->clear(rootId_);
		}
	}

	// Member variables accessible by each TEST_F
	std::shared_ptr<graph::query::indexes::IndexTreeManager> treeManager_;
	int64_t rootId_{};

	// Static members shared by all tests in the suite
	static std::unique_ptr<graph::Database> database_;
	static std::shared_ptr<graph::storage::DataManager> dataManager_;
};

// Initialize static members
std::unique_ptr<graph::Database> IndexTreeManagerTest::database_ = nullptr;
std::shared_ptr<graph::storage::DataManager> IndexTreeManagerTest::dataManager_ = nullptr;


TEST_F(IndexTreeManagerTest, InsertCausesLeafSplit) {
	// With maxKeysPerLeaf = 3, the 4th insertion should trigger a split.
	rootId_ = treeManager_->insert(rootId_, "keyA", 1);
	rootId_ = treeManager_->insert(rootId_, "keyB", 2);
	rootId_ = treeManager_->insert(rootId_, "keyC", 3);

	auto rootNodeBeforeSplit = dataManager_->getIndex(rootId_);
	ASSERT_TRUE(rootNodeBeforeSplit.isLeaf());
	ASSERT_EQ(rootNodeBeforeSplit.getKeyCount(), 3);

	// This insertion MUST cause a split.
	rootId_ = treeManager_->insert(rootId_, "keyD", 4);

	// Verify the split happened
	auto newRootNode = dataManager_->getIndex(rootId_);
	ASSERT_FALSE(newRootNode.isLeaf()); // The root should now be an internal node
	ASSERT_EQ(newRootNode.getChildCount(), 2);

	// Verify all data is still findable
	auto results = treeManager_->findRange(rootId_, "keyA", "keyD");
	ASSERT_EQ(results.size(), 4);
}

TEST_F(IndexTreeManagerTest, InsertCausesInternalSplit) {
	// With maxKeysPerInternal = 3, we need to cause enough leaf splits
	// to over-fill the root internal node.
	// Each leaf split promotes one key. We need 4 keys promoted to split the root.
	// Each leaf holds 3 keys, so we need to fill 4 leaves.
	for (int i = 0; i < 10; ++i) {
		rootId_ = treeManager_->insert(rootId_, "key_" + std::to_string(i), i);
	}

	auto rootNode = dataManager_->getIndex(rootId_);
	ASSERT_FALSE(rootNode.isLeaf());
	// The specific structure depends on split keys, but we can verify all data exists.
	auto results = treeManager_->findRange(rootId_, "key_0", "key_9");
	ASSERT_EQ(results.size(), 10);
}

TEST_F(IndexTreeManagerTest, RemoveAndFind) {
	rootId_ = treeManager_->insert(rootId_, "keyA", 1);
	rootId_ = treeManager_->insert(rootId_, "keyB", 2);

	bool removed = treeManager_->remove(rootId_, "keyA", 1);
	ASSERT_TRUE(removed);

	removed = treeManager_->remove(rootId_, "keyA", 1);
	ASSERT_FALSE(removed);

	auto results = treeManager_->find(rootId_, "keyA");
	ASSERT_TRUE(results.empty());

	results = treeManager_->find(rootId_, "keyB");
	ASSERT_EQ(results.size(), 1);
	ASSERT_EQ(results[0], 2);
}

TEST_F(IndexTreeManagerTest, RangeQueryAfterSplit) {
	for (int i = 0; i < 7; ++i) {
		rootId_ = treeManager_->insert(rootId_, "key" + std::to_string(i), i);
	}

	auto results = treeManager_->findRange(rootId_, "key1", "key5");
	std::vector<int64_t> expected = {1, 2, 3, 4, 5};
	std::ranges::sort(results);
	ASSERT_EQ(results, expected);
}

TEST_F(IndexTreeManagerTest, Clear) {
	for (int i = 0; i < 7; ++i) {
		rootId_ = treeManager_->insert(rootId_, "key" + std::to_string(i), i);
	}

	int64_t rootToDelete = rootId_;
	treeManager_->clear(rootToDelete);

	// Check that a previously found key is no longer there.
	// A new tree would have a different rootId, so we can't search the old one.
	// The best way to test clear is to ensure no errors and that a new tree is empty.
	rootId_ = treeManager_->initialize();
	auto results = treeManager_->find(rootId_, "key1");
	ASSERT_TRUE(results.empty());
}
