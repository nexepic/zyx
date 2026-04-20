/**
 * @file test_IndexTreeManager.cpp
 * @author ZYX Contributors
 * @date 2026
 *
 * @copyright Copyright (c) 2026
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

#include "core/IndexTreeManagerTestFixture.hpp"

// ============================================================================
// Core IndexTreeManager Tests
// ============================================================================

TEST_F(IndexTreeManagerTest, OperationsOnEmptyTree) {
	ASSERT_TRUE(stringTreeManager_->find(stringRootId_, PropertyValue("any_key")).empty());
	ASSERT_FALSE(stringTreeManager_->remove(stringRootId_, PropertyValue("any_key"), 123));

	auto range_results = stringTreeManager_->findRange(stringRootId_, PropertyValue("A"), PropertyValue("Z"));
	ASSERT_TRUE(range_results.empty());
}

TEST_F(IndexTreeManagerTest, LeafValuesOverflowToBlob_EndToEnd) {
	const std::string testKey = "key_with_many_values";
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

TEST_F(IndexTreeManagerTest, DoubleKeyComparator) {
	auto doubleTreeManager = std::make_shared<IndexTreeManager>(
		dataManager_, IndexTypes::NODE_PROPERTY_TYPE, PropertyType::DOUBLE);
	int64_t doubleRootId = doubleTreeManager->initialize();

	doubleRootId = doubleTreeManager->insert(doubleRootId, PropertyValue(3.14), 1);
	doubleRootId = doubleTreeManager->insert(doubleRootId, PropertyValue(2.72), 2);
	doubleRootId = doubleTreeManager->insert(doubleRootId, PropertyValue(1.41), 3);

	auto results = doubleTreeManager->find(doubleRootId, PropertyValue(3.14));
	ASSERT_EQ(results.size(), 1UL);
	ASSERT_EQ(results[0], 1);

	// Range query should work with doubles
	auto range = doubleTreeManager->findRange(doubleRootId, PropertyValue(1.0), PropertyValue(4.0));
	ASSERT_EQ(range.size(), 3UL);

	doubleTreeManager->clear(doubleRootId);
}

TEST_F(IndexTreeManagerTest, BooleanKeyComparator) {
	auto boolTreeManager = std::make_shared<IndexTreeManager>(
		dataManager_, IndexTypes::NODE_PROPERTY_TYPE, PropertyType::BOOLEAN);
	int64_t boolRootId = boolTreeManager->initialize();

	boolRootId = boolTreeManager->insert(boolRootId, PropertyValue(false), 1);
	boolRootId = boolTreeManager->insert(boolRootId, PropertyValue(true), 2);

	auto falseResults = boolTreeManager->find(boolRootId, PropertyValue(false));
	ASSERT_EQ(falseResults.size(), 1UL);
	ASSERT_EQ(falseResults[0], 1);

	auto trueResults = boolTreeManager->find(boolRootId, PropertyValue(true));
	ASSERT_EQ(trueResults.size(), 1UL);
	ASSERT_EQ(trueResults[0], 2);

	boolTreeManager->clear(boolRootId);
}

TEST_F(IndexTreeManagerTest, ClearWithZeroRootId) {
	EXPECT_NO_THROW(stringTreeManager_->clear(0));
}

TEST_F(IndexTreeManagerTest, ClearHandlesInvalidEntities) {
	// Insert some data and then clear
	for (int i = 0; i < 10; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	EXPECT_NO_THROW(stringTreeManager_->clear(stringRootId_));
	stringRootId_ = 0; // Prevent double-clear in TearDown
}

TEST_F(IndexTreeManagerTest, ClearTreeWithBlobStorage) {
	// Use long keys to force blob storage in internal nodes
	const std::string suffix(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'Z');

	for (int i = 0; i < 30; ++i) {
		std::string longKey = generatePaddedKey(i) + suffix;
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(longKey), i);
	}

	// Clear should properly clean up all blobs
	EXPECT_NO_THROW(stringTreeManager_->clear(stringRootId_));
	stringRootId_ = 0; // Prevent double-clear in TearDown
}

TEST_F(IndexTreeManagerTest, ClearTreeWithValuesBlobEntries) {
	const std::string testKey = "vals_blob_key";
	size_t valueCount = (Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t)) + 5;

	for (size_t i = 0; i < valueCount; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(testKey), static_cast<int64_t>(i));
	}

	// Verify values are in blob
	int64_t leafId = stringTreeManager_->findLeafNode(stringRootId_, PropertyValue(testKey));
	auto leaf = dataManager_->getIndex(leafId);
	auto entries = leaf.getAllEntries(dataManager_);
	ASSERT_EQ(entries.size(), 1UL);
	ASSERT_NE(entries[0].valuesBlobId, 0);

	// Clear should handle blob cleanup
	EXPECT_NO_THROW(stringTreeManager_->clear(stringRootId_));
	stringRootId_ = 0;
}

TEST_F(IndexTreeManagerTest, MakeComparatorUnsupportedType) {
	// PropertyType that is not INTEGER, DOUBLE, STRING, or BOOLEAN
	// This should throw std::logic_error
	EXPECT_THROW(
		std::make_shared<IndexTreeManager>(
			dataManager_, IndexTypes::NODE_PROPERTY_TYPE, PropertyType::LIST),
		std::logic_error);
}

TEST_F(IndexTreeManagerTest, ClearInternalNodesWithBlobKeys) {
	// Use long keys to force blob storage in internal node separator keys
	const std::string suffix(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'Y');

	for (int i = 0; i < 20; ++i) {
		std::string longKey = generatePaddedKey(i) + suffix;
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(longKey), i);
	}

	// Verify tree has internal nodes
	auto root = dataManager_->getIndex(stringRootId_);
	ASSERT_FALSE(root.isLeaf()) << "Need internal nodes for this test";

	// Clear should handle blob cleanup in internal nodes too
	EXPECT_NO_THROW(stringTreeManager_->clear(stringRootId_));
	stringRootId_ = 0;
}

TEST_F(IndexTreeManagerTest, ClearInternalNodesWithoutBlobKeys) {
	// Use short keys that won't trigger blob storage
	for (int i = 0; i < 30; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf()) << "Need internal nodes for this test";

	// Clear should skip blob cleanup for short keys
	EXPECT_NO_THROW(intTreeManager_->clear(intRootId_));
	intRootId_ = 0;
}

TEST_F(IndexTreeManagerTest, ClearLeafEntriesWithoutBlobs) {
	for (int i = 0; i < 5; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// All entries should have inline keys and values (no blobs)
	int64_t leafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(0)));
	auto leaf = dataManager_->getIndex(leafId);
	auto entries = leaf.getAllEntries(dataManager_);
	for (const auto &entry : entries) {
		ASSERT_EQ(entry.keyBlobId, 0);
		ASSERT_EQ(entry.valuesBlobId, 0);
	}

	EXPECT_NO_THROW(intTreeManager_->clear(intRootId_));
	intRootId_ = 0;
}

TEST_F(IndexTreeManagerTest, ClearWithAlreadyDeletedNodes) {
	// Build a tree with internal nodes
	for (int i = 0; i < 50; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf()) << "Need internal nodes for this test";

	// Get a child node and manually delete it before clearing the tree
	auto children = root.getAllChildren(dataManager_);
	ASSERT_GE(children.size(), 2u);
	int64_t childToDelete = children[1].childId;
	auto childNode = dataManager_->getIndex(childToDelete);

	// Delete the child node directly - this simulates a partially corrupted tree
	dataManager_->deleteIndex(childNode);

	// clear() should handle the deleted node gracefully (entity.getId() == 0 -> continue)
	EXPECT_NO_THROW(intTreeManager_->clear(intRootId_));
	intRootId_ = 0; // Prevent double-clear in TearDown
}

TEST_F(IndexTreeManagerTest, GetDataManagerReturnsValidPointer) {
	auto dm = stringTreeManager_->getDataManager();
	ASSERT_NE(dm, nullptr);
	// Verify it's the same data manager we constructed with
	EXPECT_EQ(dm.get(), dataManager_.get());
}
