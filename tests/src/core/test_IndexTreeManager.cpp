/**
 * @file test_IndexTreeManager.cpp
 * @author Nexepic
 * @date 2025/6/13
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

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/core/IndexTreeManager.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

using namespace graph;
using namespace graph::storage;
using namespace graph::query::indexes;

class IndexTreeManagerTest : public ::testing::Test {
protected:
	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath_ = std::filesystem::temp_directory_path() /
					  ("test_indexTreeManager_" + boost::uuids::to_string(uuid) + ".dat");

		database_ = std::make_unique<Database>(testDbPath_.string());
		database_->open();
		dataManager_ = database_->getStorage()->getDataManager();
	}

	static void TearDownTestSuite() {
		if (database_) {
			database_->close();
			database_.reset();
			if (std::filesystem::exists(testDbPath_)) {
				std::filesystem::remove(testDbPath_);
			}
		}
	}

	void SetUp() override {
		// Create managers for specific key types as needed by tests.
		stringTreeManager_ =
				std::make_shared<IndexTreeManager>(dataManager_, IndexTypes::NODE_PROPERTY_TYPE, PropertyType::STRING);
		stringRootId_ = stringTreeManager_->initialize();

		intTreeManager_ =
				std::make_shared<IndexTreeManager>(dataManager_, IndexTypes::NODE_PROPERTY_TYPE, PropertyType::INTEGER);
		intRootId_ = intTreeManager_->initialize();
	}

	void TearDown() override {
		if (stringRootId_ != 0)
			stringTreeManager_->clear(stringRootId_);
		if (intRootId_ != 0)
			intTreeManager_->clear(intRootId_);
	}

	static std::string generatePaddedKey(int i, int width = 5) {
		std::ostringstream ss;
		ss << "key_" << std::setw(width) << std::setfill('0') << i;
		return ss.str();
	}

	static std::unique_ptr<Database> database_;
	static std::shared_ptr<DataManager> dataManager_;
	static std::filesystem::path testDbPath_;

	std::shared_ptr<IndexTreeManager> stringTreeManager_;
	int64_t stringRootId_{};
	std::shared_ptr<IndexTreeManager> intTreeManager_;
	int64_t intRootId_{};
};

std::unique_ptr<Database> IndexTreeManagerTest::database_ = nullptr;
std::shared_ptr<DataManager> IndexTreeManagerTest::dataManager_ = nullptr;
std::filesystem::path IndexTreeManagerTest::testDbPath_;


// --- Core B+Tree Functionality Tests ---

TEST_F(IndexTreeManagerTest, OperationsOnEmptyTree) {
	ASSERT_TRUE(stringTreeManager_->find(stringRootId_, PropertyValue("any_key")).empty());
	ASSERT_FALSE(stringTreeManager_->remove(stringRootId_, PropertyValue("any_key"), 123));

	auto range_results = stringTreeManager_->findRange(stringRootId_, PropertyValue("A"), PropertyValue("Z"));
	ASSERT_TRUE(range_results.empty());
}

TEST_F(IndexTreeManagerTest, InsertDuplicateAndMultipleValues) {
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("multi_key"), 100);
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("multi_key"), 200);
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("multi_key"), 300);
	stringRootId_ =
			stringTreeManager_->insert(stringRootId_, PropertyValue("multi_key"), 200); // Duplicate should be ignored.

	auto results = stringTreeManager_->find(stringRootId_, PropertyValue("multi_key"));
	ASSERT_EQ(results.size(), 3UL);
	std::ranges::sort(results);
	ASSERT_EQ(results[0], 100);
	ASSERT_EQ(results[1], 200);
	ASSERT_EQ(results[2], 300);
}

TEST_F(IndexTreeManagerTest, AdvancedRemoveScenarios) {
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("test_key"), 1);
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("test_key"), 2);
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("test_key"), 3);

	ASSERT_FALSE(stringTreeManager_->remove(stringRootId_, PropertyValue("test_key"), 99));
	ASSERT_EQ(stringTreeManager_->find(stringRootId_, PropertyValue("test_key")).size(), 3UL);

	ASSERT_TRUE(stringTreeManager_->remove(stringRootId_, PropertyValue("test_key"), 2));
	auto results1 = stringTreeManager_->find(stringRootId_, PropertyValue("test_key"));
	ASSERT_EQ(results1.size(), 2UL);
	ASSERT_EQ(std::ranges::find(results1, 2), results1.end());
	ASSERT_TRUE(stringTreeManager_->remove(stringRootId_, PropertyValue("test_key"), 1));
	ASSERT_TRUE(stringTreeManager_->remove(stringRootId_, PropertyValue("test_key"), 3));
	ASSERT_TRUE(stringTreeManager_->find(stringRootId_, PropertyValue("test_key")).empty());

	ASSERT_FALSE(stringTreeManager_->remove(stringRootId_, PropertyValue("non_existent_key"), 1));
}

// --- B+Tree Structural Integrity and Split Tests ---

TEST_F(IndexTreeManagerTest, InsertCausesLeafSplit) {
	auto rootNode = dataManager_->getIndex(stringRootId_);
	ASSERT_TRUE(rootNode.isLeaf());

	for (int i = 0; i < 500; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("key_" + std::to_string(i)), i);
		rootNode = dataManager_->getIndex(stringRootId_);
		if (!rootNode.isLeaf())
			break;
	}

	ASSERT_FALSE(rootNode.isLeaf()) << "A leaf split should have turned the root into an internal node.";
	ASSERT_EQ(rootNode.getChildCount(), 2u);

	auto results = stringTreeManager_->find(stringRootId_, PropertyValue("key_0"));
	ASSERT_EQ(results[0], 0);
}

TEST_F(IndexTreeManagerTest, LeafNodeLinkedListIsCorrectAfterSplit) {
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("C"), 3);
	for (int i = 0; i < 500; ++i) { // Force a split
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	int64_t leaf1_id = stringTreeManager_->findLeafNode(stringRootId_, PropertyValue("C"));
	ASSERT_NE(leaf1_id, 0);
	auto leaf1 = dataManager_->getIndex(leaf1_id);

	int64_t leaf2_id = leaf1.getNextLeafId();
	ASSERT_NE(leaf2_id, 0);
	auto leaf2 = dataManager_->getIndex(leaf2_id);
	ASSERT_EQ(leaf2.getPrevLeafId(), leaf1_id);
}

TEST_F(IndexTreeManagerTest, InsertCausesInternalSplit) {
	auto rootNode = dataManager_->getIndex(stringRootId_);
	uint8_t initialLevel = rootNode.getLevel();

	int insertedCount = 0;
	for (int i = 0; i < 10000; ++i) { // Need many insertions to split internal nodes
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
		insertedCount++;
		rootNode = dataManager_->getIndex(stringRootId_);
		if (rootNode.getLevel() > initialLevel + 1)
			break; // Height increased by 2 (e.g., L0 -> L2)
	}

	ASSERT_GT(rootNode.getLevel(), static_cast<uint8_t>(initialLevel + 1))
			<< "An internal root split should increase the tree height by at least 2 levels.";

	auto results = stringTreeManager_->findRange(stringRootId_, PropertyValue(generatePaddedKey(0)),
												 PropertyValue(generatePaddedKey(insertedCount - 1)));
	ASSERT_EQ(results.size(), static_cast<size_t>(insertedCount));
}

TEST_F(IndexTreeManagerTest, RangeQuerySpansMultipleLeaves) {
	for (int i = 0; i < 600; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	auto results = stringTreeManager_->findRange(stringRootId_, PropertyValue(generatePaddedKey(100)),
												 PropertyValue(generatePaddedKey(499)));
	ASSERT_EQ(results.size(), 400UL);
	std::ranges::sort(results);
	ASSERT_EQ(results[0], 100);
	ASSERT_EQ(results.back(), 499);
}

// --- End-to-End Blob Storage Tests ---

TEST_F(IndexTreeManagerTest, LeafValuesOverflowToBlob_EndToEnd) {
	constexpr std::string testKey = "key_with_many_values";
	size_t valueCount = (Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t)) + 5;

	for (size_t i = 0; i < valueCount; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(testKey), static_cast<int64_t>(i));
	}

	int64_t leafId = stringTreeManager_->findLeafNode(stringRootId_, PropertyValue(testKey));
	ASSERT_NE(leafId, 0);
	auto leafNode = dataManager_->getIndex(leafId);
	auto entries = leafNode.getAllEntries(dataManager_);
	ASSERT_EQ(entries.size(), 1UL);
	ASSERT_NE(entries[0].valuesBlobId, 0) << "The entry's value list should have been offloaded to a blob.";

	auto results = stringTreeManager_->find(stringRootId_, PropertyValue(testKey));
	ASSERT_EQ(results.size(), valueCount);
}

TEST_F(IndexTreeManagerTest, InternalKeyPromotionToBlob_EndToEnd) {
	// This test ensures that all promoted keys are long enough to force Blob storage.
	const std::string suffix(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'X');
	int splitTriggerCount =
			500; // A number of insertions sufficient to cause multiple splits (including internal node splits).

	// Insert a large number of long keys to force multiple splits in the tree.
	for (int i = 0; i < splitTriggerCount; ++i) {
		// Make each key a long key.
		std::string currentLongKey = generatePaddedKey(i) + suffix;
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(currentLongKey), i);
	}

	// At this point, at least one promoted key in the tree must have been stored in a Blob.
	// Traversal and checking logic remain unchanged.
	bool foundBlobKeyInTree = false;
	std::vector<int64_t> nodesToVisit = {stringRootId_};
	std::set<int64_t> visited;

	while (!nodesToVisit.empty()) {
		int64_t currentId = nodesToVisit.back();
		nodesToVisit.pop_back();
		if (visited.contains(currentId))
			continue;
		visited.insert(currentId);

		auto currentNode = dataManager_->getIndex(currentId);
		if (!currentNode.isLeaf()) {
			auto children = currentNode.getAllChildren(dataManager_);
			for (const auto &child: children) {
				if (child.keyBlobId != 0) {
					foundBlobKeyInTree = true;
					break;
				}
				nodesToVisit.push_back(child.childId);
			}
		}
		if (foundBlobKeyInTree)
			break;
	}

	ASSERT_TRUE(foundBlobKeyInTree) << "After multiple splits, no keys stored in Blob were found in internal nodes.";

	// Finally, verify that we can still find the correct value using this long key.
	std::string finalKeyToFind = generatePaddedKey(splitTriggerCount - 1) + suffix;
	auto results = stringTreeManager_->find(stringRootId_, PropertyValue(finalKeyToFind));
	ASSERT_EQ(results.size(), 1UL);
	ASSERT_EQ(results[0], splitTriggerCount - 1);
}

// --- B+Tree Deletion, Redistribution, and Merging Tests ---

/**
 * @brief Tests that the clear() function correctly deletes all nodes except a new root.
 */
TEST_F(IndexTreeManagerTest, ClearFunctionality) {
	// Insert a significant amount of data to create many nodes.
	for (int i = 0; i < 500; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	// The clear operation should delete all nodes and create a new, single leaf root.
	stringTreeManager_->clear(stringRootId_);

	// After clearing, the database should not contain any old index nodes.
	// We can verify this by checking if we can still access an old node ID.
	// Note: This is a conceptual check. A more robust way would be to count index entities.
	// For now, let's re-initialize and check the state.

	stringRootId_ = stringTreeManager_->initialize();
	auto rootNode = dataManager_->getIndex(stringRootId_);
	ASSERT_TRUE(rootNode.isLeaf()) << "Root should be a leaf after clear and re-initialize.";
	ASSERT_EQ(rootNode.getEntryCount(), 0u) << "New root should be empty.";

	// A simple check to ensure the old data is gone.
	auto results = stringTreeManager_->find(stringRootId_, PropertyValue(generatePaddedKey(0)));
	ASSERT_TRUE(results.empty());
}


/**
 * @brief Tests redistribution from a right sibling to a left sibling on a leaf node.
 *
 * 1. Create a split, resulting in [L_Node_1 (few keys)] [L_Node_2 (many keys)]
 * 2. Delete a key from L_Node_1 to trigger underflow.
 * 3. L_Node_2 has extra keys, so it should lend one to L_Node_1 (redistribute).
 * 4. Verify the node counts, key distribution, and parent's separator key.
 */
TEST_F(IndexTreeManagerTest, RemoveCausesLeafRedistributionFromRight) {
	// Setup: Insert keys to cause a split. We need precise control.
	// Assuming a split promotes the median key, we can craft the insertions.
	// For simplicity, let's just insert enough to cause a split, then add more to the right.
	for (int i = 0; i < 300; ++i) { // Creates at least one split
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(i), i);
	}

	// At this point, keys are likely distributed among several leaves.
	// Find two adjacent leaves. Let's find leaf for key 50.
	int64_t leftLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(50));
	auto leftLeaf = dataManager_->getIndex(leftLeafId);
	int64_t rightLeafId = leftLeaf.getNextLeafId();
	ASSERT_NE(rightLeafId, 0) << "Setup failed: Could not find two adjacent leaf nodes.";

	// To reliably trigger redistribution, let's empty the left leaf and fill the right one.
	auto leftEntries = leftLeaf.getAllEntries(dataManager_);
	auto rightLeaf = dataManager_->getIndex(rightLeafId);
	auto rightEntries = rightLeaf.getAllEntries(dataManager_);

	// Move all but one entry from left to right.
	rightEntries.insert(rightEntries.begin(), std::make_move_iterator(leftEntries.begin() + 1),
						std::make_move_iterator(leftEntries.end()));
	leftEntries.resize(1);

	leftLeaf.setAllEntries(leftEntries, dataManager_);
	rightLeaf.setAllEntries(rightEntries, dataManager_);
	dataManager_->updateIndexEntity(leftLeaf);
	dataManager_->updateIndexEntity(rightLeaf);

	// Action: Delete the last entry from the left leaf to trigger underflow.
	int64_t keyToDelete = std::get<int64_t>(leftEntries[0].key.getVariant());
	ASSERT_TRUE(intTreeManager_->remove(intRootId_, PropertyValue(keyToDelete), keyToDelete));

	// Verification
	auto finalLeftLeaf = dataManager_->getIndex(leftLeafId);
	auto finalRightLeaf = dataManager_->getIndex(rightLeafId);

	ASSERT_GT(finalLeftLeaf.getEntryCount(), 0u) << "Left leaf should have received an entry.";
	ASSERT_LT(finalRightLeaf.getEntryCount(), rightEntries.size()) << "Right leaf should have given an entry.";

	// Check that the data is still correct.
	auto result = intTreeManager_->find(intRootId_, PropertyValue(keyToDelete + 1));
	ASSERT_FALSE(result.empty()) << "Data should still be findable after redistribution.";
}

TEST_F(IndexTreeManagerTest, RemoveCausesLeafMerge) {
	// Start with a fresh tree
	intTreeManager_->clear(intRootId_);
	intRootId_ = intTreeManager_->initialize();

	// 1. Create a simple tree with a small, controlled set of entries
	// Use a distribution that forces multiple leaf nodes
	std::vector<int64_t> values;
	// First leaf should contain values 10-50
	for (int i = 10; i <= 50; i += 10) {
		values.push_back(i);
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Force a split by adding more values
	// Second leaf should contain values 60-100
	for (int i = 60; i <= 100; i += 10) {
		values.push_back(i);
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// 2. Verify the tree structure - we should have at least two leaf nodes
	int64_t firstLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(10)));
	auto firstLeaf = dataManager_->getIndex(firstLeafId);
	ASSERT_NE(firstLeaf.getNextLeafId(), 0) << "Test requires at least two leaf nodes";

	int64_t secondLeafId = firstLeaf.getNextLeafId();
	auto secondLeaf = dataManager_->getIndex(secondLeafId);

	// Both leaves should have the same parent
	ASSERT_EQ(firstLeaf.getParentId(), secondLeaf.getParentId());
	int64_t parentId = firstLeaf.getParentId();
	auto parent = dataManager_->getIndex(parentId);

	// Save parent's child count before operation
	size_t parentChildCount_before = parent.getChildCount();
	ASSERT_GE(parentChildCount_before, 2UL) << "Parent must have at least 2 children for merge test";

	// 3. Now remove all entries from first leaf to force a merge
	for (int i = 10; i <= 50; i += 10) {
		ASSERT_TRUE(intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i));
	}

	// 4. Verification logic must handle Root Collapse
	auto currentRoot = dataManager_->getIndex(intRootId_);

	if (currentRoot.getId() == parentId) {
		// Case A: Root didn't collapse (e.g. if parent had > 2 children originally)
		ASSERT_EQ(currentRoot.getChildCount(), parentChildCount_before - 1)
				<< "Parent should have one less child after merge";
	} else {
		// Case B: Root collapsed! (This is what actually happens)
		// The new root should be the merged leaf itself.
		// The old parent is gone.
		ASSERT_NE(currentRoot.getId(), parentId);
		ASSERT_TRUE(currentRoot.isLeaf()) << "New root should be the merged leaf";
		// We can't check parent.getChildCount() because parent is deleted.
	}

	// 5. Verify the merged node contains the remaining values
	// If collapsed, intRootId_ IS the remaining leaf.
	// If not collapsed, findLeafNode still works.
	int64_t remainingLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(60)));
	auto remainingLeaf = dataManager_->getIndex(remainingLeafId);
	auto entries = remainingLeaf.getAllEntries(dataManager_);

	// Should have entries for 60, 70, 80, 90, 100
	ASSERT_EQ(entries.size(), 5UL) << "Merged leaf should contain all remaining entries";

	// Verify each expected value is present
	for (int i = 60; i <= 100; i += 10) {
		bool found = false;
		for (const auto &entry: entries) {
			if (std::get<int64_t>(entry.key.getVariant()) == i) {
				found = true;
				break;
			}
		}
		ASSERT_TRUE(found) << "Value " << i << " not found in merged leaf";
	}
}

/**
 * @brief Tests a recursive merge, where a leaf merge causes its parent to underflow and merge.
 *
 * This is the most complex scenario. We need to create a tree structure where a parent (P1)
 * and its sibling (P2) both have the minimum number of children. Then, by merging P1's
 * children, P1 itself underflows and is forced to merge with P2.
 */
TEST_F(IndexTreeManagerTest, RemoveCausesRecursiveMerge) {
	// Setup: This requires creating a very specific tree structure.
	// We need a tree of height > 1, with at least two internal nodes at the same level,
	// both of which have the minimum number of children (e.g., 2), and those children
	// are also minimally filled leaves.

	// This setup can be very sensitive to the fanout factor of nodes.
	// We'll insert and then delete strategically to create the desired state.
	constexpr int initial_insert_count = 5000;
	for (int i = 0; i < initial_insert_count; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(i), i);
	}

	auto root_before = dataManager_->getIndex(intRootId_);
	uint8_t height_before = root_before.getLevel();
	ASSERT_GE(height_before, 2u) << "Test requires a tree of at least height 2.";

	// Now, delete a large chunk of keys from the middle, which tends to cause
	// many merges and can trigger recursive merges.
	for (int i = 100; i < 4800; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(i), i);
	}

	// Verification
	auto root_after = dataManager_->getIndex(intRootId_);
	uint8_t height_after = root_after.getLevel();

	// It's possible the tree height decreased.
	ASSERT_LE(height_after, height_before) << "Tree height should not increase after massive deletions.";

	// The most important check: data integrity. The remaining data should be findable.
	auto result1 = intTreeManager_->find(intRootId_, PropertyValue(50));
	ASSERT_EQ(result1.size(), 1UL);
	ASSERT_EQ(result1[0], 50);

	auto result2 = intTreeManager_->find(intRootId_, PropertyValue(4900));
	ASSERT_EQ(result2.size(), 1UL);
	ASSERT_EQ(result2[0], 4900);

	// A specific check for a key that was deleted.
	auto result3 = intTreeManager_->find(intRootId_, PropertyValue(2500));
	ASSERT_TRUE(result3.empty());
}

// --- Batch Operations Tests ---

TEST_F(IndexTreeManagerTest, InsertBatchSorted) {
	// Prepare sorted batch
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.reserve(100);
	for (int i = 0; i < 100; ++i) {
		batch.emplace_back(PropertyValue(generatePaddedKey(i)), i);
	}

	// Execute batch insert
	stringRootId_ = stringTreeManager_->insertBatch(stringRootId_, batch);

	// Verify all data is present
	for (int i = 0; i < 100; ++i) {
		auto results = stringTreeManager_->find(stringRootId_, PropertyValue(generatePaddedKey(i)));
		ASSERT_EQ(results.size(), 1UL);
		ASSERT_EQ(results[0], i);
	}
}

TEST_F(IndexTreeManagerTest, InsertBatchUnsorted) {
	// Prepare unsorted batch
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue("C"), 3);
	batch.emplace_back(PropertyValue("A"), 1);
	batch.emplace_back(PropertyValue("D"), 4);
	batch.emplace_back(PropertyValue("B"), 2);

	// Execute batch insert
	stringRootId_ = stringTreeManager_->insertBatch(stringRootId_, batch);

	// Verify data and order
	auto resultsA = stringTreeManager_->find(stringRootId_, PropertyValue("A"));
	ASSERT_EQ(resultsA.size(), 1UL);
	ASSERT_EQ(resultsA[0], 1);

	auto resultsD = stringTreeManager_->find(stringRootId_, PropertyValue("D"));
	ASSERT_EQ(resultsD.size(), 1UL);
	ASSERT_EQ(resultsD[0], 4);

	// Verify range query gives correct order
	auto range = stringTreeManager_->findRange(stringRootId_, PropertyValue("A"), PropertyValue("Z"));
	ASSERT_EQ(range.size(), 4UL);
	ASSERT_EQ(range[0], 1); // A
	ASSERT_EQ(range[1], 2); // B
	ASSERT_EQ(range[2], 3); // C
	ASSERT_EQ(range[3], 4); // D
}

TEST_F(IndexTreeManagerTest, InsertBatchIntoExistingTree) {
	// 1. Initial manual inserts
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("key_050"), 50);
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("key_150"), 150);

	// 2. Prepare batch that interleaves with existing keys
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue("key_025"), 25); // Before existing
	batch.emplace_back(PropertyValue("key_100"), 100); // Between existing
	batch.emplace_back(PropertyValue("key_200"), 200); // After existing

	// 3. Batch insert
	stringRootId_ = stringTreeManager_->insertBatch(stringRootId_, batch);

	// 4. Verify
	ASSERT_EQ(stringTreeManager_->find(stringRootId_, PropertyValue("key_025")).size(), 1UL);
	ASSERT_EQ(stringTreeManager_->find(stringRootId_, PropertyValue("key_050")).size(), 1UL);
	ASSERT_EQ(stringTreeManager_->find(stringRootId_, PropertyValue("key_100")).size(), 1UL);
	ASSERT_EQ(stringTreeManager_->find(stringRootId_, PropertyValue("key_150")).size(), 1UL);
	ASSERT_EQ(stringTreeManager_->find(stringRootId_, PropertyValue("key_200")).size(), 1UL);
}

TEST_F(IndexTreeManagerTest, InsertBatchMassiveSplit) {
	// Prepare a large batch to trigger multiple leaf and internal splits
	// Assuming fanout is small enough that 1000 items causes splits
	constexpr int count = 2000;
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.reserve(count);
	for (int i = 0; i < count; ++i) {
		batch.emplace_back(PropertyValue(generatePaddedKey(i)), i);
	}

	// Execute batch insert
	stringRootId_ = stringTreeManager_->insertBatch(stringRootId_, batch);

	// Verify structure (should have height > 0)
	auto rootNode = dataManager_->getIndex(stringRootId_);
	ASSERT_GT(rootNode.getLevel(), 0);

	// Verify first and last
	ASSERT_EQ(stringTreeManager_->find(stringRootId_, PropertyValue(generatePaddedKey(0)))[0], 0);
	ASSERT_EQ(stringTreeManager_->find(stringRootId_, PropertyValue(generatePaddedKey(count - 1)))[0], count - 1);

	// Verify range size
	auto all = stringTreeManager_->findRange(stringRootId_, PropertyValue(generatePaddedKey(0)),
											 PropertyValue(generatePaddedKey(count)));
	ASSERT_EQ(all.size(), static_cast<size_t>(count));
}

TEST_F(IndexTreeManagerTest, InsertBatchWithDuplicates) {
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	// Same key, different values
	batch.emplace_back(PropertyValue("dup_key"), 10);
	batch.emplace_back(PropertyValue("dup_key"), 20);
	batch.emplace_back(PropertyValue("dup_key"), 30);
	// Another key
	batch.emplace_back(PropertyValue("other_key"), 99);

	stringRootId_ = stringTreeManager_->insertBatch(stringRootId_, batch);

	auto dupResults = stringTreeManager_->find(stringRootId_, PropertyValue("dup_key"));
	ASSERT_EQ(dupResults.size(), 3UL);

	// Sort to verify contents regardless of internal order
	std::ranges::sort(dupResults);
	ASSERT_EQ(dupResults[0], 10);
	ASSERT_EQ(dupResults[1], 20);
	ASSERT_EQ(dupResults[2], 30);

	auto otherResults = stringTreeManager_->find(stringRootId_, PropertyValue("other_key"));
	ASSERT_EQ(otherResults.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, InsertBatchMixedTypesInteger) {
	// Testing the Integer Tree Manager specifically
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue(static_cast<int64_t>(100)), 1);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(50)), 2);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(150)), 3);

	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	auto results = intTreeManager_->findRange(intRootId_, PropertyValue(static_cast<int64_t>(0)),
											  PropertyValue(static_cast<int64_t>(200)));
	ASSERT_EQ(results.size(), 3UL);
	ASSERT_EQ(results[0], 2); // Value for 50
	ASSERT_EQ(results[1], 1); // Value for 100
	ASSERT_EQ(results[2], 3); // Value for 150
}

// --- Advanced Underflow & Structure Tests ---

/**
 * @brief Tests the "Root Collapse" scenario.
 *
 * Logic being tested:
 * If the root is an internal node and has only ONE child after deletion,
 * the root should be deleted and that single child should become the new root.
 * This reduces the tree height.
 */
TEST_F(IndexTreeManagerTest, RootCollapse_HeightReduction) {
	// 1. Build a tree with height at least 1 (Root -> Leaves)
	// We need enough entries to force a split at the root.
	// Assuming fanout is small, 50 entries usually triggers splits.
	for (int i = 0; i < 50; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	auto rootBefore = dataManager_->getIndex(stringRootId_);
	uint8_t initialLevel = rootBefore.getLevel();
	ASSERT_GE(initialLevel, 1) << "Setup failed: Tree must have height > 0 (internal root).";

	// 2. Delete almost everything, leaving just enough to fit in one node
	// We keep keys 0 and 1.
	for (int i = 2; i < 50; ++i) {
		stringTreeManager_->remove(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	// 3. Verify Height Reduction
	auto rootAfter = dataManager_->getIndex(stringRootId_);
	ASSERT_LT(rootAfter.getLevel(), initialLevel) << "Root should have collapsed to a lower level.";
	ASSERT_TRUE(rootAfter.isLeaf()) << "With only 2 items, the root should now be a leaf.";

	// 4. Verify Data Integrity
	auto res0 = stringTreeManager_->find(stringRootId_, PropertyValue(generatePaddedKey(0)));
	ASSERT_EQ(res0.size(), 1UL);
	auto res1 = stringTreeManager_->find(stringRootId_, PropertyValue(generatePaddedKey(1)));
	ASSERT_EQ(res1.size(), 1UL);
}

/**
 * @brief Tests redistribution specifically from a LEFT sibling.
 *
 * Setup: [Leaf A (Left, Full)] [Leaf B (Right, Near Empty)]
 * Action: Delete from Leaf B.
 * Expectation: Leaf B borrows the largest key from Leaf A.
 */
TEST_F(IndexTreeManagerTest, LeafRedistributeFromLeft) {
	// 1. Manually construct two leaves to control the state precisely.
	// We assume the implementation uses the standard logic where we can create this state via inserts.

	// Insert 0..30. Assuming capacity allows this to split into at least two nodes.
	for (int i = 0; i <= 30; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Identify the leaf containing '30' (The rightmost leaf)
	int64_t rightLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(30)));
	auto rightLeaf = dataManager_->getIndex(rightLeafId);

	int64_t leftLeafId = rightLeaf.getPrevLeafId();
	ASSERT_NE(leftLeafId, 0) << "Setup failed: Right leaf must have a left sibling.";

	// 2. Manipulate state to force Left-to-Right redistribution condition.
	// We want Left Leaf to be FULL and Right Leaf to be almost EMPTY.

	// Remove almost all entries from Right Leaf except '30'.
	// We iterate backwards from 29 down to whatever is in that leaf.
	// Note: This relies on knowing the split point, but we can just blindly remove
	// numbers and check if they were in the right leaf.
	for (int i = 29; i >= 15; --i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Now, verify Right Leaf has very few items (likely just 30)
	// And Left Leaf should still have plenty (0..14 or so).

	// 3. Trigger Underflow on Right Leaf
	// Remove '30'. The right leaf becomes empty/underflow.
	// It should look to its Left Sibling. Left sibling has data.
	ASSERT_TRUE(intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(30)), 30));

	// 4. Verify Data Integrity
	// If redistribution worked, the Right Leaf (or what replaced it) should now contain
	// the largest value from the Left Leaf (e.g., 14).
	// The key '14' should still be findable.
	auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(14)));
	ASSERT_EQ(res.size(), 1UL);
	ASSERT_EQ(res[0], 14);
}

/**
 * @brief Tests merging of Internal Nodes.
 *
 * This is distinct from leaf merging because internal nodes manage separators
 * differently during a merge.
 */
TEST_F(IndexTreeManagerTest, InternalNodeMerge) {
	// 1. Create a 3-level tree (Root -> Internal -> Leaf)
	// Insert enough data to ensure we have multiple internal nodes at level 1.
	constexpr int count = 1000;
	for (int i = 0; i < count; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_GE(root.getLevel(), 2) << "Setup failed: Need a tree with at least 3 levels (Height 2).";

	// 2. Mass delete from a specific range to hollow out a subtree.
	// This forces leaves to merge, which forces their parent (Internal Node) to underflow.
	// If neighbors are also sparse, the Internal Nodes will merge.

	// Delete the first 80% of data. This usually causes cascading merges from left to right.
	for (int i = 0; i < 800; ++i) {
		intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// 3. Verify the tree didn't collapse into an invalid state
	// We should still find the remaining data.
	auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(999)));
	ASSERT_EQ(res.size(), 1UL);
	ASSERT_EQ(res[0], 999);

	// Verify root is still valid (it might have lowered in height)
	auto rootAfter = dataManager_->getIndex(intRootId_);
	ASSERT_GT(rootAfter.getEntryCount() + rootAfter.getChildCount(), 0u);
}

/**
 * @brief Stress Test: Randomized Insertions and Deletions.
 *
 * This effectively fuzz-tests the B+Tree logic, hitting many combinations
 * of split, merge, and redistribution.
 */
TEST_F(IndexTreeManagerTest, StressTest_RandomOps) {
	std::srand(42); // Fixed seed for reproducibility
	std::vector<int> keys;
	std::set<int> activeKeys;
	constexpr int iterations = 5000;

	// Phase 1: Random Insertions
	for (int i = 0; i < iterations; ++i) {
		int key = std::rand() % 100000;
		if (!activeKeys.contains(key)) {
			stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(std::to_string(key)), key);
			activeKeys.insert(key);
			keys.push_back(key);
		}
	}

	// Verify all exist
	for (int key: activeKeys) {
		auto res = stringTreeManager_->find(stringRootId_, PropertyValue(std::to_string(key)));
		ASSERT_FALSE(res.empty()) << "Key " << key << " missing after inserts.";
	}

	// Phase 2: Random Deletions (Delete 50% of them)
	// We shuffle the vector to delete in random order, triggering various merge scenarios.
	std::random_device rd;
	std::mt19937 g(rd());
	std::ranges::shuffle(keys, g);

	size_t deleteCount = keys.size() / 2;
	for (size_t i = 0; i < deleteCount; ++i) {
		int key = keys[i];
		bool removed = stringTreeManager_->remove(stringRootId_, PropertyValue(std::to_string(key)), key);
		ASSERT_TRUE(removed) << "Failed to remove existing key " << key;
		activeKeys.erase(key);
	}

	// Phase 3: Verify Remaining
	for (int key: activeKeys) {
		auto res = stringTreeManager_->find(stringRootId_, PropertyValue(std::to_string(key)));
		ASSERT_FALSE(res.empty()) << "Key " << key << " missing after partial deletion.";
	}

	// Phase 4: Delete Everything
	for (size_t i = deleteCount; i < keys.size(); ++i) {
		int key = keys[i];
		stringTreeManager_->remove(stringRootId_, PropertyValue(std::to_string(key)), key);
	}

	// Phase 5: Verify Empty
	auto rootNode = dataManager_->getIndex(stringRootId_);
	ASSERT_TRUE(rootNode.isLeaf());
	ASSERT_EQ(rootNode.getEntryCount(), 0u);
}
