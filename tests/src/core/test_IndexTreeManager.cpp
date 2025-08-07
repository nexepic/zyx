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
#include "graph/storage/data/BlobManager.hpp"

class IndexTreeManagerTest : public ::testing::Test {
protected:
	// SetUpTestSuite is run once for all tests in this file
	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		std::filesystem::path testFilePath = std::filesystem::temp_directory_path() /
											 ("test_indexTreeManager_" + boost::uuids::to_string(uuid) + ".dat");

		database_ = std::make_unique<graph::Database>(testFilePath.string());
		database_->open();
		dataManager_ = database_->getStorage()->getDataManager();
	}

	static void TearDownTestSuite() {
		if (database_) {
			database_->close();
			database_.reset();
		}
	}

	// SetUp is run before each individual TEST_F
	void SetUp() override {
		treeManager_ = std::make_shared<graph::query::indexes::IndexTreeManager>(
				dataManager_, graph::query::indexes::IndexTypes::NODE_LABEL_TYPE);
		rootId_ = treeManager_->initialize();
	}

	void TearDown() override {
		if (rootId_ != 0) {
			treeManager_->clear(rootId_);
		}
	}

	// Helper function to force a leaf split.
	void ensureSplitOccurs() {
		auto rootNode = dataManager_->getIndex(rootId_);
		if (!rootNode.isLeaf())
			return; // Split has already occurred.

		for (int i = 0; i < 500; ++i) {
			rootId_ = treeManager_->insert(rootId_, "split_key_" + std::to_string(i), i);
			rootNode = dataManager_->getIndex(rootId_);
			if (!rootNode.isLeaf()) {
				return; // Exit as soon as split is detected.
			}
		}
		FAIL() << "Setup failed: A leaf split did not occur within the loop.";
	}

	// Helper function to generate zero-padded keys for correct alphabetical sorting.
	static std::string generatePaddedKey(int i, int width = 4) {
		std::ostringstream ss;
		ss << "key_" << std::setw(width) << std::setfill('0') << i;
		return ss.str();
	}

	// Static members are shared by all tests in the suite
	static inline std::unique_ptr<graph::Database> database_;
	static inline std::shared_ptr<graph::storage::DataManager> dataManager_;

	// Member variables accessible by each TEST_F
	std::shared_ptr<graph::query::indexes::IndexTreeManager> treeManager_;
	int64_t rootId_{};
};

// --- Core B+Tree Functionality Tests ---

TEST_F(IndexTreeManagerTest, OperationsOnEmptyTree) {
	// An empty tree is created in SetUp.
	ASSERT_TRUE(treeManager_->find(rootId_, "any_key").empty());
	ASSERT_FALSE(treeManager_->remove(rootId_, "any_key", 123));

	auto range_results = treeManager_->findRange(rootId_, "A", "Z");
	ASSERT_TRUE(range_results.empty());
}

TEST_F(IndexTreeManagerTest, InsertDuplicateAndMultipleValues) {
	// 1. Insert multiple values for the same key.
	rootId_ = treeManager_->insert(rootId_, "multi_key", 100);
	rootId_ = treeManager_->insert(rootId_, "multi_key", 200);
	rootId_ = treeManager_->insert(rootId_, "multi_key", 300);

	// 2. Insert a duplicate key-value pair, which should be ignored (no-op).
	rootId_ = treeManager_->insert(rootId_, "multi_key", 200);

	auto results = treeManager_->find(rootId_, "multi_key");
	ASSERT_EQ(results.size(), 3);
	std::ranges::sort(results);
	ASSERT_EQ(results[0], 100);
	ASSERT_EQ(results[1], 200);
	ASSERT_EQ(results[2], 300);
}

TEST_F(IndexTreeManagerTest, AdvancedRemoveScenarios) {
	rootId_ = treeManager_->insert(rootId_, "test_key", 1);
	rootId_ = treeManager_->insert(rootId_, "test_key", 2);
	rootId_ = treeManager_->insert(rootId_, "test_key", 3);

	// Scenario 1: Remove a non-existent value from an existing key.
	ASSERT_FALSE(treeManager_->remove(rootId_, "test_key", 99));
	ASSERT_EQ(treeManager_->find(rootId_, "test_key").size(), 3);

	// Scenario 2: Remove an existing value from a key with multiple values.
	ASSERT_TRUE(treeManager_->remove(rootId_, "test_key", 2));
	auto results1 = treeManager_->find(rootId_, "test_key");
	ASSERT_EQ(results1.size(), 2);
	ASSERT_EQ(std::ranges::find(results1, 2), results1.end()); // Verify value 2 is GONE.
	ASSERT_NE(std::ranges::find(results1, 1), results1.end()); // Verify others remain.

	// Scenario 3: Remove the last value for a key, which should remove the key itself.
	ASSERT_TRUE(treeManager_->remove(rootId_, "test_key", 1));
	ASSERT_TRUE(treeManager_->remove(rootId_, "test_key", 3));
	ASSERT_TRUE(treeManager_->find(rootId_, "test_key").empty());

	// Scenario 4: Remove a non-existent key.
	ASSERT_FALSE(treeManager_->remove(rootId_, "non_existent_key", 1));
}

// --- B+Tree Structural Integrity and Split Tests ---

TEST_F(IndexTreeManagerTest, InsertCausesLeafSplit) {
	auto rootNode = dataManager_->getIndex(rootId_);
	ASSERT_TRUE(rootNode.isLeaf());

	int insertedCount = 0;
	// Insert enough distinct keys to overfill a single leaf node.
	for (int i = 0; i < 500; ++i) {
		rootId_ = treeManager_->insert(rootId_, "key_" + std::to_string(i), i);
		insertedCount++;
		rootNode = dataManager_->getIndex(rootId_);
		if (!rootNode.isLeaf())
			break; // Split detected
	}

	ASSERT_FALSE(rootNode.isLeaf()) << "A leaf split should have turned the root into an internal node.";
	ASSERT_EQ(rootNode.getChildCount(), 2);

	// Verify all data is still accessible after the split.
	auto results = treeManager_->findRange(rootId_, "key_0", "key_" + std::to_string(insertedCount - 1));
	ASSERT_EQ(results.size(), insertedCount);
}

TEST_F(IndexTreeManagerTest, LeafNodeLinkedListIsCorrectAfterSplit) {
	rootId_ = treeManager_->insert(rootId_, "C", 3);
	rootId_ = treeManager_->insert(rootId_, "A", 1);
	rootId_ = treeManager_->insert(rootId_, "B", 2);

	// Force a split by inserting enough data.
	ensureSplitOccurs();

	// After the split, the tree has an internal root and at least two leaves.
	int64_t leaf1_id = treeManager_->findLeafNode(rootId_, "A");
	ASSERT_NE(leaf1_id, 0);
	auto leaf1 = dataManager_->getIndex(leaf1_id);

	// The first leaf's 'prev' pointer should be null (0).
	ASSERT_EQ(leaf1.getPrevLeafId(), 0);
	int64_t leaf2_id = leaf1.getNextLeafId();
	ASSERT_NE(leaf2_id, 0) << "The first leaf node must point to a second leaf node after a split.";

	auto leaf2 = dataManager_->getIndex(leaf2_id);
	// The second leaf's 'prev' pointer should point back to the first leaf.
	ASSERT_EQ(leaf2.getPrevLeafId(), leaf1_id);
}

TEST_F(IndexTreeManagerTest, InsertCausesInternalSplit) {
	auto rootNode = dataManager_->getIndex(rootId_);
	uint8_t initialLevel = rootNode.getLevel();

	int insertedCount = 0;
	// Increase loop limit drastically. An internal root split requires filling the
	// root node with pointers from many leaf splits. This can take thousands of insertions.
	for (int i = 0; i < 50000; ++i) {
		rootId_ = treeManager_->insert(rootId_, generatePaddedKey(i), i);
		insertedCount++;
		rootNode = dataManager_->getIndex(rootId_);
		// A split of the root internal node creates a new root, increasing tree height.
		if (rootNode.getLevel() >= initialLevel + 2) {
			break;
		}
	}

	ASSERT_GE(rootNode.getLevel(), initialLevel + 2) << "An internal root split should increase the tree height.";

	// Verify data integrity after many splits.
	auto results = treeManager_->findRange(rootId_, generatePaddedKey(0), generatePaddedKey(insertedCount - 1));
	ASSERT_EQ(results.size(), insertedCount);
}

TEST_F(IndexTreeManagerTest, RangeQuerySpansMultipleLeaves) {
	// Insert enough data to guarantee multiple leaf splits.
	for (int i = 0; i < 600; ++i) {
		rootId_ = treeManager_->insert(rootId_, generatePaddedKey(i), i);
	}

	auto rootNode = dataManager_->getIndex(rootId_);
	ASSERT_FALSE(rootNode.isLeaf()) << "Multiple splits are required for this test.";

	// Query a range that is guaranteed to span across at least two leaves.
	auto results = treeManager_->findRange(rootId_, generatePaddedKey(100), generatePaddedKey(499));

	// We expect to get 400 values (from 100 to 499 inclusive).
	ASSERT_EQ(results.size(), 400);
	std::ranges::sort(results);
	ASSERT_EQ(results[0], 100);
	ASSERT_EQ(results.back(), 499);
}

// --- Blob Storage and Validation Tests ---

/**
 * @brief (NEW) Verifies that attempting to insert a key longer than the
 * system's absolute limit is correctly rejected by throwing an exception.
 */
TEST_F(IndexTreeManagerTest, InsertWithKeyExceedingAbsoluteLimitFails) {
	// Create a key that is intentionally longer than the allowed maximum length.
	std::string illegalKey(graph::Index::ABSOLUTE_MAX_KEY_LENGTH + 1, 'Z');

	// Expect the insert operation to throw a std::runtime_error.
	ASSERT_THROW(treeManager_->insert(rootId_, illegalKey, 999), std::runtime_error);
}

/**
 * @brief (REPLACEMENT) Verifies that a LEAF node's content moves to a blob
 * when a single key accumulates too many values to fit inline.
 */
TEST_F(IndexTreeManagerTest, LeafDataOverflowsToBlob) {
	const std::string testKey = "key_with_many_values";

	// Insert enough values for this single key to force its data into a blob.
	// e.g., 50 values * 8 bytes/value = 400 bytes, which should exceed DATA_SIZE.
	const int valueCount = 50;
	for (int i = 0; i < valueCount; ++i) {
		rootId_ = treeManager_->insert(rootId_, testKey, i);
	}

	// Find the leaf where our key is stored.
	int64_t leafId = treeManager_->findLeafNode(rootId_, testKey);
	ASSERT_NE(leafId, 0);
	auto leafNode = dataManager_->getIndex(leafId);

	// The leaf itself should have offloaded its oversized content to a blob.
	ASSERT_TRUE(leafNode.hasBlobStorage()) << "Node should be using blob storage for the large value list.";

	// Verify all data can be retrieved from the blob.
	auto results = treeManager_->find(rootId_, testKey);
	ASSERT_EQ(results.size(), valueCount);
}

/**
 * @brief (DETERMINISTIC VERSION) Verifies an INTERNAL node's key is stored in a blob.
 * This test works in two stages:
 * 1. Probe Stage: It first determines the exact key capacity of a leaf node.
 * 2. Execution Stage: It uses this capacity to precisely place a long key at the
 *    split point, guaranteeing it gets promoted and testing the blob storage mechanism.
 */
TEST_F(IndexTreeManagerTest, InternalNodeKeyIsStoredInBlob) {
	// --- STAGE 1: PROBE a leaf node's actual capacity ---
	int capacity = 0;
	{
		// Use a temporary, separate Tree Manager to avoid interfering with the main test.
		auto probeTreeManager = std::make_shared<graph::query::indexes::IndexTreeManager>(
				dataManager_, graph::query::indexes::IndexTypes::NODE_LABEL_TYPE);
		int64_t probeRootId = probeTreeManager->initialize();

		for (int i = 0; i < 5000; ++i) { // High safety limit
			probeRootId = probeTreeManager->insert(probeRootId, "probe_key_" + std::to_string(i), i);
			capacity++;
			auto probeRootNode = dataManager_->getIndex(probeRootId);
			if (!probeRootNode.isLeaf()) {
				// The first split just occurred. `capacity` now holds the number of
				// items that fit in a single leaf.
				break;
			}
		}
		probeTreeManager->clear(probeRootId); // Clean up the probe tree.
	}
	ASSERT_GT(capacity, 0) << "Probe stage failed: Could not determine node capacity.";


	// --- STAGE 2: EXECUTE the test with the known capacity ---

	// Create a key that is valid but long enough to require blob storage when promoted.
	std::string longButLegalKey(graph::Index::INTERNAL_KEY_BLOB_THRESHOLD + 10, 'Y');
	const int midpoint = capacity / 2;

	// Step 2.1: Fill the first half of the leaf with keys alphabetically BEFORE our long key.
	for (int i = 0; i < midpoint; ++i) {
		rootId_ = treeManager_->insert(rootId_, "A_key_" + std::to_string(i), i);
	}

	// Step 2.2: Insert our target long key. It is now positioned to be the first
	// key in the new "right" node after the split.
	rootId_ = treeManager_->insert(rootId_, longButLegalKey, 9999);

	// Step 2.3: Fill the rest of the leaf to trigger the split.
	// The total number of items will exceed `capacity` here.
	for (int i = 0; i < midpoint; ++i) {
		rootId_ = treeManager_->insert(rootId_, "Z_key_" + std::to_string(i), i);
	}

	auto rootNode = dataManager_->getIndex(rootId_);
	ASSERT_FALSE(rootNode.isLeaf()) << "A split must have occurred, creating an internal root.";

	// Step 2.4: Assertions. Now we can reliably check the promoted key.
	auto children = rootNode.getAllChildren();
	bool blobKeyFound = false;
	for (const auto &child: children) {
		if (child.keyBlobId != 0) {
			blobKeyFound = true;
			std::string keyFromBlob = dataManager_->getBlobManager()->readBlobChain(child.keyBlobId);
			ASSERT_EQ(keyFromBlob, longButLegalKey);
			break;
		}
	}
	ASSERT_TRUE(blobKeyFound)
			<< "The promoted internal key, which was at the split point, should have been stored in a blob.";

	// Finally, ensure we can still find the data using this blob-stored key.
	auto results = treeManager_->find(rootId_, longButLegalKey);
	ASSERT_EQ(results.size(), 1);
	ASSERT_EQ(results[0], 9999);
}
