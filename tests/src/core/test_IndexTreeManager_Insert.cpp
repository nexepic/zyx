/**
 * @file test_IndexTreeManager_Insert.cpp
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
// Insert & Split Tests
// ============================================================================

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

TEST_F(IndexTreeManagerTest, InsertWithEmptyRootId) {
	// Tests that insert initializes tree when rootId is 0
	int64_t newRootId = stringTreeManager_->insert(0, PropertyValue("test_key"), 100);
	ASSERT_NE(newRootId, 0);

	auto results = stringTreeManager_->find(newRootId, PropertyValue("test_key"));
	ASSERT_EQ(results.size(), 1UL);
	ASSERT_EQ(results[0], 100);
}

TEST_F(IndexTreeManagerTest, InsertBatchEmpty) {
	std::vector<std::pair<PropertyValue, int64_t>> emptyBatch;
	int64_t result = stringTreeManager_->insertBatch(stringRootId_, emptyBatch);
	ASSERT_EQ(result, stringRootId_);
}

TEST_F(IndexTreeManagerTest, InsertBatchWithDuplicateValues) {
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	// Same key and same value (should be deduplicated)
	batch.emplace_back(PropertyValue("dup_val_key"), 100);
	batch.emplace_back(PropertyValue("dup_val_key"), 100);
	batch.emplace_back(PropertyValue("dup_val_key"), 200);

	stringRootId_ = stringTreeManager_->insertBatch(stringRootId_, batch);

	auto results = stringTreeManager_->find(stringRootId_, PropertyValue("dup_val_key"));
	// Should have 2 unique values (100, 200), not 3
	ASSERT_EQ(results.size(), 2UL);

	std::ranges::sort(results);
	ASSERT_EQ(results[0], 100);
	ASSERT_EQ(results[1], 200);
}

TEST_F(IndexTreeManagerTest, InsertBatchMergesWithExistingKeys) {
	// First insert a key
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("merge_key"), 1);

	// Now batch insert the same key with a different value
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue("merge_key"), 2);
	batch.emplace_back(PropertyValue("merge_key"), 3);

	stringRootId_ = stringTreeManager_->insertBatch(stringRootId_, batch);

	auto results = stringTreeManager_->find(stringRootId_, PropertyValue("merge_key"));
	ASSERT_EQ(results.size(), 3UL);

	std::ranges::sort(results);
	ASSERT_EQ(results[0], 1);
	ASSERT_EQ(results[1], 2);
	ASSERT_EQ(results[2], 3);
}

TEST_F(IndexTreeManagerTest, ComplexInsertDeleteInterleavedVerification) {
	// Interleaved insert and delete operations
	for (int i = 0; i < 40; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Delete even numbers
	for (int i = 0; i < 40; i += 2) {
		ASSERT_TRUE(intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i));
	}

	// Insert new values in gaps
	for (int i = 40; i < 60; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Delete odd numbers from first batch
	for (int i = 1; i < 40; i += 2) {
		ASSERT_TRUE(intTreeManager_->remove(intRootId_, PropertyValue(static_cast<int64_t>(i)), i));
	}

	// Verify only 40-59 remain
	for (int i = 40; i < 60; ++i) {
		auto r = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(i)));
		ASSERT_EQ(r.size(), 1UL) << "Key " << i << " should exist";
	}

	for (int i = 0; i < 40; ++i) {
		auto r = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(i)));
		ASSERT_TRUE(r.empty()) << "Key " << i << " should be deleted";
	}
}

TEST_F(IndexTreeManagerTest, InsertBatchSingleEntry) {
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue("single"), 1);

	stringRootId_ = stringTreeManager_->insertBatch(stringRootId_, batch);

	auto results = stringTreeManager_->find(stringRootId_, PropertyValue("single"));
	ASSERT_EQ(results.size(), 1UL);
	ASSERT_EQ(results[0], 1);
}

TEST_F(IndexTreeManagerTest, BooleanKeyInsertAndRemove) {
	auto boolTreeManager = std::make_shared<IndexTreeManager>(
		dataManager_, IndexTypes::NODE_PROPERTY_TYPE, PropertyType::BOOLEAN);
	int64_t boolRootId = boolTreeManager->initialize();

	// Insert multiple values for same boolean keys
	boolRootId = boolTreeManager->insert(boolRootId, PropertyValue(false), 1);
	boolRootId = boolTreeManager->insert(boolRootId, PropertyValue(false), 2);
	boolRootId = boolTreeManager->insert(boolRootId, PropertyValue(true), 3);
	boolRootId = boolTreeManager->insert(boolRootId, PropertyValue(true), 4);

	auto falseResults = boolTreeManager->find(boolRootId, PropertyValue(false));
	ASSERT_EQ(falseResults.size(), 2UL);

	auto trueResults = boolTreeManager->find(boolRootId, PropertyValue(true));
	ASSERT_EQ(trueResults.size(), 2UL);

	// Remove one value
	ASSERT_TRUE(boolTreeManager->remove(boolRootId, PropertyValue(false), 1));
	falseResults = boolTreeManager->find(boolRootId, PropertyValue(false));
	ASSERT_EQ(falseResults.size(), 1UL);

	boolTreeManager->clear(boolRootId);
}

TEST_F(IndexTreeManagerTest, InsertBatchMergesValuesWithExistingEntriesInLeaf) {
	// Insert individual entries first
	for (int i = 0; i < 5; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	// Batch insert with same keys but different values
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	for (int i = 0; i < 5; ++i) {
		batch.emplace_back(PropertyValue(generatePaddedKey(i)), i + 100);
	}

	stringRootId_ = stringTreeManager_->insertBatch(stringRootId_, batch);

	// Each key should now have 2 values
	for (int i = 0; i < 5; ++i) {
		auto results = stringTreeManager_->find(stringRootId_, PropertyValue(generatePaddedKey(i)));
		ASSERT_EQ(results.size(), 2UL) << "Key " << i << " should have 2 values after batch merge";
	}
}

TEST_F(IndexTreeManagerTest, InsertBatchDuplicateKeyAndValue) {
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	// Same key, same value, multiple times
	batch.emplace_back(PropertyValue("dup"), 42);
	batch.emplace_back(PropertyValue("dup"), 42);
	batch.emplace_back(PropertyValue("dup"), 42);

	stringRootId_ = stringTreeManager_->insertBatch(stringRootId_, batch);

	auto results = stringTreeManager_->find(stringRootId_, PropertyValue("dup"));
	// Should have only 1 unique value despite 3 duplicate entries
	ASSERT_EQ(results.size(), 1UL);
	ASSERT_EQ(results[0], 42);
}

TEST_F(IndexTreeManagerTest, InsertBatchMergesExistingEqualKeysWithDedup) {
	// Pre-insert a key with value 1
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("shared_key"), 1);

	// Batch insert same key with same value (1) and new value (2)
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue("shared_key"), 1); // duplicate value
	batch.emplace_back(PropertyValue("shared_key"), 2); // new value

	stringRootId_ = stringTreeManager_->insertBatch(stringRootId_, batch);

	auto results = stringTreeManager_->find(stringRootId_, PropertyValue("shared_key"));
	// Should have 2 unique values (1, 2), not 3
	ASSERT_EQ(results.size(), 2UL);
	std::ranges::sort(results);
	ASSERT_EQ(results[0], 1);
	ASSERT_EQ(results[1], 2);
}

TEST_F(IndexTreeManagerTest, InsertBatchEmptyEntries) {
	std::vector<std::pair<PropertyValue, int64_t>> emptyBatch;
	int64_t result = intTreeManager_->insertBatch(intRootId_, emptyBatch);
	EXPECT_EQ(result, intRootId_);
}

TEST_F(IndexTreeManagerTest, InsertBatchBulkSplitWithOldNextPointer) {
	// First, insert enough data one-by-one to create a multi-leaf tree
	for (int i = 0; i < 20; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i * 100)), i);
	}

	// Now batch-insert into the middle of the tree. This will overfill an existing
	// leaf that already has a next pointer (oldNextId != 0), triggering the bulk split
	// path that needs to re-link the old next leaf.
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	for (int i = 0; i < 30; ++i) {
		batch.emplace_back(PropertyValue(static_cast<int64_t>(500 + i)), 1000 + i);
	}
	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	// Verify all original and new entries are findable
	for (int i = 0; i < 20; ++i) {
		auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(i * 100)));
		ASSERT_FALSE(results.empty()) << "Original key " << i * 100 << " not found";
	}
	for (int i = 0; i < 30; ++i) {
		auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(500 + i)));
		ASSERT_FALSE(results.empty()) << "Batch key " << (500 + i) << " not found";
	}
}

TEST_F(IndexTreeManagerTest, InsertBatchExistingEntriesAfterNew) {
	// Insert high keys first
	stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("zzz_key"), 100);

	// Batch insert lower keys - the merge loop should exhaust new entries
	// before existing entries
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue("aaa_key"), 1);
	batch.emplace_back(PropertyValue("bbb_key"), 2);

	stringRootId_ = stringTreeManager_->insertBatch(stringRootId_, batch);

	// Verify all entries
	auto r1 = stringTreeManager_->find(stringRootId_, PropertyValue("aaa_key"));
	ASSERT_EQ(r1.size(), 1UL);
	auto r2 = stringTreeManager_->find(stringRootId_, PropertyValue("bbb_key"));
	ASSERT_EQ(r2.size(), 1UL);
	auto r3 = stringTreeManager_->find(stringRootId_, PropertyValue("zzz_key"));
	ASSERT_EQ(r3.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, InsertBatchBulkSplitFillLimit) {
	// Pre-fill a leaf with some data
	for (int i = 0; i < 5; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i * 1000)), i);
	}

	// Now batch-insert a large number of entries that will overflow the target leaf
	// and trigger bulk split with fill limit logic
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	for (int i = 0; i < 40; ++i) {
		batch.emplace_back(PropertyValue(static_cast<int64_t>(i * 10)), 500 + i);
	}
	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	// Verify all entries are findable
	for (int i = 0; i < 40; ++i) {
		auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(i * 10)));
		ASSERT_FALSE(results.empty()) << "Key " << (i * 10) << " not found";
	}
}

TEST_F(IndexTreeManagerTest, InsertBatchEqualKeyValueDedup) {
	// Insert key=100 with value=1
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(100)), 1);

	// Batch insert key=100 with value=1 (duplicate) and value=2 (new)
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue(static_cast<int64_t>(100)), 1);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(100)), 2);

	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(100)));
	ASSERT_EQ(results.size(), 2UL); // 1 and 2 only (deduped)
	std::ranges::sort(results);
	ASSERT_EQ(results[0], 1);
	ASSERT_EQ(results[1], 2);
}

TEST_F(IndexTreeManagerTest, SplitLeafWithExistingNextPointer) {
	// Insert enough to create 2 leaves, then insert more into the first leaf
	// to cause it to split, creating a new leaf between the two existing ones
	for (int i = 0; i < 20; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	// Now insert keys that sort between existing keys to force splits
	// on leaves that already have next pointers
	for (int i = 20; i < 40; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	// Verify linked list integrity by scanning all keys
	auto allKeys = stringTreeManager_->scanKeys(stringRootId_, 40);
	ASSERT_EQ(allKeys.size(), 40UL);

	// Verify sorted order
	for (size_t i = 1; i < allKeys.size(); ++i) {
		ASSERT_LE(std::get<std::string>(allKeys[i - 1].getVariant()),
				  std::get<std::string>(allKeys[i].getVariant()));
	}
}

TEST_F(IndexTreeManagerTest, InsertBatchLargeBulkSplit) {
	// Pre-populate with scattered keys
	for (int i = 0; i < 10; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i * 1000)), i);
	}

	// Batch insert a concentrated range that will overflow a single leaf
	// and require multiple new leaves in bulk split
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	for (int i = 0; i < 50; ++i) {
		batch.emplace_back(PropertyValue(static_cast<int64_t>(5000 + i)), 2000 + i);
	}
	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	// Verify all data
	for (int i = 0; i < 50; ++i) {
		auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(5000 + i)));
		ASSERT_FALSE(results.empty()) << "Batch key " << (5000 + i) << " not found";
	}
	for (int i = 0; i < 10; ++i) {
		auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(i * 1000)));
		ASSERT_FALSE(results.empty()) << "Original key " << (i * 1000) << " not found";
	}
}

TEST_F(IndexTreeManagerTest, InsertBatchEqualKeysInLeafMerge) {
	// Insert several individual entries
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(10)), 1);
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(20)), 2);
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(30)), 3);

	// Batch insert with EXACTLY the same keys but different values
	// This should trigger the equal-keys merge in the leaf merge loop
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue(static_cast<int64_t>(10)), 11);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(20)), 22);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(30)), 33);

	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	// Each key should now have 2 values
	auto r10 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(10)));
	ASSERT_EQ(r10.size(), 2UL);
	auto r20 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(20)));
	ASSERT_EQ(r20.size(), 2UL);
	auto r30 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(30)));
	ASSERT_EQ(r30.size(), 2UL);
}

TEST_F(IndexTreeManagerTest, InsertBatchNewKeysAllBeforeExisting) {
	// Insert high keys first
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(900)), 900);
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(950)), 950);

	// Batch insert only low keys -- all new keys come before existing keys
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue(static_cast<int64_t>(10)), 10);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(20)), 20);

	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	// Verify all present
	ASSERT_EQ(intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(10))).size(), 1UL);
	ASSERT_EQ(intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(20))).size(), 1UL);
	ASSERT_EQ(intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(900))).size(), 1UL);
	ASSERT_EQ(intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(950))).size(), 1UL);
}

TEST_F(IndexTreeManagerTest, InsertWithFindChildReturningZeroFallback) {
	// This is extremely difficult to trigger via public API since findChild
	// normally always finds a valid child. Instead, verify insert works correctly
	// with a large variety of key patterns that exercise all traversal paths.
	for (int i = 0; i < 100; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i * 7)), i);
	}
	// Insert keys that are smaller than all existing keys - exercises the
	// "key < all separators" path in findChild which returns first child
	for (int i = -100; i < 0; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i + 1000);
	}
	// Verify integrity
	auto res = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(-50)));
	ASSERT_EQ(res.size(), 1UL);
	ASSERT_EQ(res[0], 950);
}

TEST_F(IndexTreeManagerTest, InsertSingleEntryNoSplit) {
	int64_t originalRoot = intRootId_;
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(42)), 42);
	// Root should not have changed for a single insert into an empty leaf
	ASSERT_EQ(intRootId_, originalRoot);

	auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(42)));
	ASSERT_EQ(results.size(), 1UL);
}

TEST_F(IndexTreeManagerTest, InsertBatchPreSortedSameKey) {
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	for (int i = 0; i < 10; ++i) {
		batch.emplace_back(PropertyValue(static_cast<int64_t>(1)), i);
	}

	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(1)));
	ASSERT_EQ(results.size(), 10UL);
}

TEST_F(IndexTreeManagerTest, InsertBatchWithZeroRootIdFixed) {
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue(static_cast<int64_t>(10)), 10);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(20)), 20);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(30)), 30);

	// insertBatch with rootId=0 should auto-initialize the tree
	int64_t newRootId = intTreeManager_->insertBatch(0, batch);
	ASSERT_NE(newRootId, 0);

	// Verify all entries were inserted
	auto r10 = intTreeManager_->find(newRootId, PropertyValue(static_cast<int64_t>(10)));
	ASSERT_EQ(r10.size(), 1UL);
	ASSERT_EQ(r10[0], 10);

	auto r20 = intTreeManager_->find(newRootId, PropertyValue(static_cast<int64_t>(20)));
	ASSERT_EQ(r20.size(), 1UL);
	ASSERT_EQ(r20[0], 20);

	auto r30 = intTreeManager_->find(newRootId, PropertyValue(static_cast<int64_t>(30)));
	ASSERT_EQ(r30.size(), 1UL);
	ASSERT_EQ(r30[0], 30);

	intTreeManager_->clear(newRootId);
}

TEST_F(IndexTreeManagerTest, InsertBatchWithZeroRootIdLargeBatch) {
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	for (int i = 0; i < 100; ++i) {
		batch.emplace_back(PropertyValue(generatePaddedKey(i)), i);
	}

	int64_t newRootId = stringTreeManager_->insertBatch(0, batch);
	ASSERT_NE(newRootId, 0);

	// Verify all data
	for (int i = 0; i < 100; ++i) {
		auto results = stringTreeManager_->find(newRootId, PropertyValue(generatePaddedKey(i)));
		ASSERT_EQ(results.size(), 1UL) << "Key " << i << " not found";
	}

	stringTreeManager_->clear(newRootId);
}

TEST_F(IndexTreeManagerTest, InsertBatchSameKeyDescendingValues) {
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue(static_cast<int64_t>(1)), 300);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(1)), 100);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(1)), 200);

	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(1)));
	ASSERT_EQ(results.size(), 3UL);
	std::ranges::sort(results);
	ASSERT_EQ(results[0], 100);
	ASSERT_EQ(results[1], 200);
	ASSERT_EQ(results[2], 300);
}

TEST_F(IndexTreeManagerTest, InsertBatchInterleavedKeys) {
	// Insert odd keys
	for (int i = 1; i < 20; i += 2) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Batch insert even keys
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	for (int i = 0; i < 20; i += 2) {
		batch.emplace_back(PropertyValue(static_cast<int64_t>(i)), i);
	}
	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	// Verify all 20 keys exist
	for (int i = 0; i < 20; ++i) {
		auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(i)));
		ASSERT_EQ(results.size(), 1UL) << "Key " << i << " not found";
	}
}

TEST_F(IndexTreeManagerTest, InsertBatchSpreadAcrossMultipleLeaves) {
	// Build a multi-leaf tree first
	for (int i = 0; i < 40; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i * 100)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf()) << "Need multi-leaf tree";

	// Now batch-insert entries that will land in DIFFERENT leaves
	// because their keys span the entire key range
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue(static_cast<int64_t>(50)), 1000);    // between 0 and 100
	batch.emplace_back(PropertyValue(static_cast<int64_t>(1550)), 1001);  // between 1500 and 1600
	batch.emplace_back(PropertyValue(static_cast<int64_t>(3050)), 1002);  // between 3000 and 3100

	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	// Verify all batch entries were inserted
	auto r1 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(50)));
	ASSERT_EQ(r1.size(), 1UL);
	ASSERT_EQ(r1[0], 1000);
	auto r2 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(1550)));
	ASSERT_EQ(r2.size(), 1UL);
	ASSERT_EQ(r2[0], 1001);
	auto r3 = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(3050)));
	ASSERT_EQ(r3.size(), 1UL);
	ASSERT_EQ(r3[0], 1002);
}

TEST_F(IndexTreeManagerTest, InsertBatchManyDifferentKeys) {
	// Batch with many distinct keys to ensure the coalesceRawEntries
	// inner loop hits the "keys differ" path repeatedly
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	for (int i = 0; i < 20; ++i) {
		batch.emplace_back(PropertyValue(static_cast<int64_t>(i * 10)), i);
	}

	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	for (int i = 0; i < 20; ++i) {
		auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(i * 10)));
		ASSERT_EQ(results.size(), 1UL) << "Key " << (i * 10) << " not found";
	}
}

TEST_F(IndexTreeManagerTest, InsertBatchSameKeyMixedDuplicateValues) {
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	// After sort by key then value, order will be:
	// (1,1), (1,1), (1,2), (1,3), (1,3)
	batch.emplace_back(PropertyValue(static_cast<int64_t>(1)), 3);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(1)), 1);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(1)), 1);  // duplicate
	batch.emplace_back(PropertyValue(static_cast<int64_t>(1)), 2);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(1)), 3);  // duplicate

	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(1)));
	ASSERT_EQ(results.size(), 3UL); // 1, 2, 3 (deduped)
	std::ranges::sort(results);
	ASSERT_EQ(results[0], 1);
	ASSERT_EQ(results[1], 2);
	ASSERT_EQ(results[2], 3);
}

TEST_F(IndexTreeManagerTest, InsertBatchNewKeysBeforeExistingInLeaf) {
	// Insert keys 10, 30, 50 individually
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(10)), 10);
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(30)), 30);
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(50)), 50);

	// Batch insert keys 5, 20, 40 - these interleave with existing keys
	// In the merge loop: 5 < 10 (new first), then 10 < 20 (exist first),
	// then 20 < 30 (new first), then 30 < 40 (exist first), etc.
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue(static_cast<int64_t>(5)), 5);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(20)), 20);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(40)), 40);

	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	// Verify all 6 keys
	for (int key : {5, 10, 20, 30, 40, 50}) {
		auto results = intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(key)));
		ASSERT_EQ(results.size(), 1UL) << "Key " << key << " not found";
	}
}

TEST_F(IndexTreeManagerTest, InsertBatchOverflowMultipleChunks) {
	// Create a batch large enough to overflow a single leaf into 3+ leaves
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	// Use long keys to increase entry size and cause overflow sooner
	for (int i = 0; i < 60; ++i) {
		std::string longKey = generatePaddedKey(i) + std::string(50, 'A');
		batch.emplace_back(PropertyValue(longKey), i);
	}

	stringRootId_ = stringTreeManager_->insertBatch(stringRootId_, batch);

	// Verify all entries
	for (int i = 0; i < 60; ++i) {
		std::string longKey = generatePaddedKey(i) + std::string(50, 'A');
		auto results = stringTreeManager_->find(stringRootId_, PropertyValue(longKey));
		ASSERT_EQ(results.size(), 1UL) << "Key " << i << " not found after bulk split";
	}
}

TEST_F(IndexTreeManagerTest, InsertBatchAllNewKeysAfterExisting) {
	// Insert low keys first
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(1)), 1);
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(2)), 2);

	// Batch insert only high keys - all after existing
	std::vector<std::pair<PropertyValue, int64_t>> batch;
	batch.emplace_back(PropertyValue(static_cast<int64_t>(100)), 100);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(200)), 200);
	batch.emplace_back(PropertyValue(static_cast<int64_t>(300)), 300);

	intRootId_ = intTreeManager_->insertBatch(intRootId_, batch);

	// Verify all entries
	ASSERT_EQ(intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(1))).size(), 1UL);
	ASSERT_EQ(intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(2))).size(), 1UL);
	ASSERT_EQ(intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(100))).size(), 1UL);
	ASSERT_EQ(intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(200))).size(), 1UL);
	ASSERT_EQ(intTreeManager_->find(intRootId_, PropertyValue(static_cast<int64_t>(300))).size(), 1UL);
}
