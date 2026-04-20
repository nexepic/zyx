/**
 * @file test_IndexTreeManager_Query.cpp
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
// Find & Scan Tests
// ============================================================================

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

TEST_F(IndexTreeManagerTest, FindReturnsEmptyForNonExistentKey) {
	// Tests that find returns empty vector for non-existent keys
	auto results = stringTreeManager_->find(stringRootId_, PropertyValue("non_existent_key"));
	ASSERT_TRUE(results.empty());
}

TEST_F(IndexTreeManagerTest, FindRangeWithInvalidRange) {
	// Tests findRange when min > max (should return empty)
	auto results = stringTreeManager_->findRange(stringRootId_, PropertyValue("Z"), PropertyValue("A"));
	ASSERT_TRUE(results.empty());
}

TEST_F(IndexTreeManagerTest, ScanKeysWithZeroLimit) {
	// Tests scanKeys with limit = 0 (should return empty)
	auto keys = stringTreeManager_->scanKeys(stringRootId_, 0);
	ASSERT_TRUE(keys.empty());
}

TEST_F(IndexTreeManagerTest, ScanKeysWithLimitSmallerThanData) {
	// Insert some data
	for (int i = 0; i < 10; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue("key_" + std::to_string(i)), i);
	}

	// Scan with limit smaller than total entries
	auto keys = stringTreeManager_->scanKeys(stringRootId_, 5);
	ASSERT_EQ(keys.size(), 5UL);
}

TEST_F(IndexTreeManagerTest, FindWithZeroRootId) {
	// Tests find with rootId = 0 returns empty
	auto results = stringTreeManager_->find(0, PropertyValue("key"));
	ASSERT_TRUE(results.empty());
}

TEST_F(IndexTreeManagerTest, FindRangeWithZeroRootId) {
	// Tests findRange with rootId = 0 returns empty
	auto results = stringTreeManager_->findRange(0, PropertyValue("A"), PropertyValue("Z"));
	ASSERT_TRUE(results.empty());
}

TEST_F(IndexTreeManagerTest, ScanKeysWithZeroRootId) {
	auto keys = stringTreeManager_->scanKeys(0, 100);
	ASSERT_TRUE(keys.empty());
}

TEST_F(IndexTreeManagerTest, ScanKeysAcrossMultipleLeaves) {
	// Insert enough data to cause multiple leaf nodes
	for (int i = 0; i < 50; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	// Scan all keys
	auto allKeys = stringTreeManager_->scanKeys(stringRootId_, 50);
	ASSERT_EQ(allKeys.size(), 50UL);

	// Verify keys are in sorted order
	for (size_t i = 1; i < allKeys.size(); ++i) {
		ASSERT_LE(std::get<std::string>(allKeys[i - 1].getVariant()),
				  std::get<std::string>(allKeys[i].getVariant()));
	}
}

TEST_F(IndexTreeManagerTest, FindRangeExactBounds) {
	for (int i = 0; i < 10; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i * 10)), i);
	}

	// Range [20, 50] should include keys 20, 30, 40, 50
	auto results = intTreeManager_->findRange(intRootId_,
		PropertyValue(static_cast<int64_t>(20)),
		PropertyValue(static_cast<int64_t>(50)));
	ASSERT_EQ(results.size(), 4UL);
}

TEST_F(IndexTreeManagerTest, FindRangeStartsBelowAllEntries) {
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(50)), 50);
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(60)), 60);

	// Range starts way below entries
	auto results = intTreeManager_->findRange(intRootId_,
		PropertyValue(static_cast<int64_t>(10)),
		PropertyValue(static_cast<int64_t>(55)));
	ASSERT_EQ(results.size(), 1UL);
	ASSERT_EQ(results[0], 50);
}

TEST_F(IndexTreeManagerTest, FindLeafNodeWithZeroRootId) {
	int64_t leafId = stringTreeManager_->findLeafNode(0, PropertyValue("any"));
	ASSERT_EQ(leafId, 0);
}

TEST_F(IndexTreeManagerTest, FindRangeScansToEndOfLeafList) {
	// Insert a few values
	for (int i = 0; i < 5; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i * 10)), i);
	}

	// Range query that covers all values and continues past the end
	auto results = intTreeManager_->findRange(intRootId_,
		PropertyValue(static_cast<int64_t>(0)),
		PropertyValue(static_cast<int64_t>(999)));
	ASSERT_EQ(results.size(), 5UL);
}

TEST_F(IndexTreeManagerTest, ScanKeysLimitExceedsData) {
	for (int i = 0; i < 5; ++i) {
		stringRootId_ = stringTreeManager_->insert(stringRootId_, PropertyValue(generatePaddedKey(i)), i);
	}

	// Request more keys than exist
	auto keys = stringTreeManager_->scanKeys(stringRootId_, 100);
	ASSERT_EQ(keys.size(), 5UL);
}

TEST_F(IndexTreeManagerTest, DoubleKeyRangeQuery) {
	auto doubleTreeManager = std::make_shared<IndexTreeManager>(
		dataManager_, IndexTypes::NODE_PROPERTY_TYPE, PropertyType::DOUBLE);
	int64_t doubleRootId = doubleTreeManager->initialize();

	for (int i = 0; i < 10; ++i) {
		doubleRootId = doubleTreeManager->insert(doubleRootId, PropertyValue(static_cast<double>(i) * 1.5), i);
	}

	// Range query [3.0, 9.0]
	auto results = doubleTreeManager->findRange(doubleRootId, PropertyValue(3.0), PropertyValue(9.0));
	ASSERT_GE(results.size(), 3UL);

	// Remove and verify
	ASSERT_TRUE(doubleTreeManager->remove(doubleRootId, PropertyValue(0.0), 0));
	auto res0 = doubleTreeManager->find(doubleRootId, PropertyValue(0.0));
	ASSERT_TRUE(res0.empty());

	doubleTreeManager->clear(doubleRootId);
}

TEST_F(IndexTreeManagerTest, ScanKeysFromInternalRoot) {
	// Build a tree with enough data to have internal root
	for (int i = 0; i < 30; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf()) << "Need internal root for this test";

	// scanKeys should navigate through internal nodes to find leftmost leaf
	auto keys = intTreeManager_->scanKeys(intRootId_, 5);
	ASSERT_EQ(keys.size(), 5UL);
}

TEST_F(IndexTreeManagerTest, FindRangeBreaksMidLeaf) {
	for (int i = 0; i < 10; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i * 10)), i);
	}

	// Range [25, 45] should only get key 30 and 40 (skipping 50+)
	auto results = intTreeManager_->findRange(intRootId_,
		PropertyValue(static_cast<int64_t>(25)),
		PropertyValue(static_cast<int64_t>(45)));
	ASSERT_EQ(results.size(), 2UL);
}

TEST_F(IndexTreeManagerTest, FindRangeExhaustsLeafChain) {
	// Insert a few keys, then do a range query that goes past last leaf
	for (int i = 0; i < 20; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}
	// Range [0, 1000000] should scan all leaves until chain ends
	auto results = intTreeManager_->findRange(intRootId_,
		PropertyValue(static_cast<int64_t>(0)),
		PropertyValue(static_cast<int64_t>(1000000)));
	ASSERT_EQ(results.size(), 20UL);
}

TEST_F(IndexTreeManagerTest, ScanKeysLimitExactlyAtLeafBoundary) {
	// Insert enough data to have multiple leaves
	for (int i = 0; i < 50; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	// Find actual leaf sizes to pick a limit that matches
	int64_t firstLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(0)));
	auto firstLeaf = dataManager_->getIndex(firstLeafId);
	auto entries = firstLeaf.getAllEntries(dataManager_);
	size_t firstLeafSize = entries.size();

	// Scan exactly as many keys as the first leaf has
	auto keys = intTreeManager_->scanKeys(intRootId_, firstLeafSize);
	ASSERT_EQ(keys.size(), firstLeafSize);
}

TEST_F(IndexTreeManagerTest, FindRangeSingleKeyRange) {
	for (int i = 0; i < 10; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i * 10)), i);
	}

	// Range [30, 30] should only get key 30
	auto results = intTreeManager_->findRange(intRootId_,
		PropertyValue(static_cast<int64_t>(30)),
		PropertyValue(static_cast<int64_t>(30)));
	ASSERT_EQ(results.size(), 1UL);
	ASSERT_EQ(results[0], 3);
}

TEST_F(IndexTreeManagerTest, ScanKeysSingleLeafTree) {
	// Insert just a few entries that fit in one leaf
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(5)), 5);
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(3)), 3);
	intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(7)), 7);

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_TRUE(root.isLeaf()) << "Root should be a leaf for this test";

	auto keys = intTreeManager_->scanKeys(intRootId_, 10);
	ASSERT_EQ(keys.size(), 3UL);
}

TEST_F(IndexTreeManagerTest, FindRangeSkipsEntriesBelowMinKeyAcrossLeaves) {
	// Build a multi-leaf tree
	for (int i = 0; i < 50; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf()) << "Need multi-leaf tree";

	// Find a leaf that contains entries below our min key
	// Range query starting from the middle of the first leaf's entries
	int64_t firstLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(0)));
	auto firstLeaf = dataManager_->getIndex(firstLeafId);
	auto entries = firstLeaf.getAllEntries(dataManager_);

	if (entries.size() >= 3) {
		// Use a min key that's in the middle of the first leaf
		int64_t midKey = std::get<int64_t>(entries[entries.size() / 2].key.getVariant());
		auto results = intTreeManager_->findRange(intRootId_,
			PropertyValue(static_cast<int64_t>(midKey)),
			PropertyValue(static_cast<int64_t>(49)));

		// Should skip entries below midKey in the first leaf, then get the rest
		ASSERT_GT(results.size(), 0UL);
		// All results should be >= midKey
		for (int64_t val : results) {
			ASSERT_GE(val, midKey) << "Found value " << val << " below min key " << midKey;
		}
	}
}

TEST_F(IndexTreeManagerTest, FindRangeStopsBeforeNextLeaf) {
	// Build a multi-leaf tree
	for (int i = 0; i < 40; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i * 10)), i);
	}

	// Find range that stops in the middle of the first leaf
	// This ensures continueScan=false before we reach leaf.getNextLeafId()
	auto results = intTreeManager_->findRange(intRootId_,
		PropertyValue(static_cast<int64_t>(0)),
		PropertyValue(static_cast<int64_t>(25)));

	// Should get keys 0, 10, 20 (stop before 30)
	ASSERT_EQ(results.size(), 3UL);
}

TEST_F(IndexTreeManagerTest, ScanKeysHitsLimitMidLeaf) {
	// Build a tree with many entries across leaves
	for (int i = 0; i < 40; ++i) {
		intRootId_ = intTreeManager_->insert(intRootId_, PropertyValue(static_cast<int64_t>(i)), i);
	}

	auto root = dataManager_->getIndex(intRootId_);
	ASSERT_FALSE(root.isLeaf()) << "Need multi-leaf tree";

	// Find first leaf size and request limit = firstLeafSize + 1
	// so we start scanning the second leaf and hit the limit mid-way
	int64_t firstLeafId = intTreeManager_->findLeafNode(intRootId_, PropertyValue(static_cast<int64_t>(0)));
	auto firstLeaf = dataManager_->getIndex(firstLeafId);
	auto entries = firstLeaf.getAllEntries(dataManager_);
	size_t limit = entries.size() + 1; // one more than first leaf

	auto keys = intTreeManager_->scanKeys(intRootId_, limit);
	ASSERT_EQ(keys.size(), limit);
}
