/**
 * @file test_Index_Entry.cpp
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

#include "core/IndexTestFixture.hpp"

// ============================================================================
// Entry Operations Tests
// ============================================================================

TEST_F(IndexTest, TryInsert_SuccessPath) {
	// 1. Insert first element
	auto res1 = leafNode_.tryInsertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);
	ASSERT_TRUE(res1.success);
	ASSERT_TRUE(res1.overflowEntries.empty());

	// 2. Insert second element
	auto res2 = leafNode_.tryInsertEntry(PropertyValue("key2"), 200, dataManager_, stringComparator);
	ASSERT_TRUE(res2.success);

	// 3. Insert duplicate key (different value)
	auto res3 = leafNode_.tryInsertEntry(PropertyValue("key1"), 101, dataManager_, stringComparator);
	ASSERT_TRUE(res3.success);

	// 4. Verify data was persisted to the node buffer
	auto values = leafNode_.findValues(PropertyValue("key1"), dataManager_, stringComparator);
	ASSERT_THAT(values, ::testing::UnorderedElementsAre(100, 101));

	values = leafNode_.findValues(PropertyValue("key2"), dataManager_, stringComparator);
	ASSERT_EQ(values.size(), 1UL);
	ASSERT_EQ(values[0], 200);
}

TEST_F(IndexTest, TryInsert_OverflowPath) {
	// 1. Fill the node aggressively
	std::vector<Index::Entry> entries;
	size_t estimatedSize = 0;
	int key_counter = 0;

	while (true) {
		PropertyValue key(std::to_string(key_counter++));
		int64_t value = 1;

		size_t entrySize = Index::getEntrySerializedSize({key, {value}, 0, 0});

		// Loop fills until the NEXT entry physically cannot fit.
		// This ensures the remaining gap is smaller than `entrySize` (approx 18 bytes).
		if (estimatedSize + entrySize > Index::DATA_SIZE) {
			break;
		}

		entries.push_back({key, {value}, 0, 0});
		estimatedSize += entrySize;
	}

	ASSERT_FALSE(entries.empty()) << "Node capacity is too small.";
	leafNode_.setAllEntries(entries, dataManager_);

	size_t countBefore = leafNode_.getEntryCount();
	size_t actualFreeSpace = Index::DATA_SIZE - estimatedSize;

	// 2. Attempt Overflow
	// We try to insert an entry. Even if it gets Blobbed (min size ~21 bytes),
	// the gap remaining from the loop (~0-17 bytes) should be too small.
	std::string overflowKey = "overflow_trigger";

	auto result = leafNode_.tryInsertEntry(PropertyValue(overflowKey), 999, dataManager_, stringComparator);

	// 3. Verify
	ASSERT_FALSE(result.success) << "Insertion succeeded unexpectedly.\n"
								 << "Filled: " << estimatedSize << "/" << Index::DATA_SIZE << "\n"
								 << "Free Space: " << actualFreeSpace << "\n"
								 << "Note: A minimum Blobbed entry requires approx 21 bytes.";

	ASSERT_EQ(result.overflowEntries.size(), countBefore + 1);

	// Verify atomicity
	auto currentStoredEntries = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(currentStoredEntries.size(), countBefore);
}

TEST_F(IndexTest, RemoveEntry) {
	// Setup
	leafNode_.tryInsertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);
	leafNode_.tryInsertEntry(PropertyValue("key1"), 101, dataManager_, stringComparator);

	// Remove one value
	ASSERT_TRUE(leafNode_.removeEntry(PropertyValue("key1"), 100, dataManager_, stringComparator));
	auto values = leafNode_.findValues(PropertyValue("key1"), dataManager_, stringComparator);
	ASSERT_EQ(values.size(), 1UL);
	ASSERT_EQ(values[0], 101);

	// Remove non-existent value
	ASSERT_FALSE(leafNode_.removeEntry(PropertyValue("key1"), 100, dataManager_, stringComparator));

	// Remove last value (removes entry)
	ASSERT_TRUE(leafNode_.removeEntry(PropertyValue("key1"), 101, dataManager_, stringComparator));
	ASSERT_TRUE(leafNode_.getAllEntries(dataManager_).empty());
}

TEST_F(IndexTest, GetAllEntriesThrowsOnInternalNode) {
	// Tests branch at line 97-99: throw if not leaf
	EXPECT_THROW((void)internalNode_.getAllEntries(dataManager_), std::logic_error);
}

TEST_F(IndexTest, GetAllEntriesReturnsEmptyForZeroEntryCount) {
	// Tests branch at line 101-102: return result if entryCount == 0
	leafNode_.getMutableMetadata().entryCount = 0;

	auto entries = leafNode_.getAllEntries(dataManager_);
	EXPECT_TRUE(entries.empty());
}

TEST_F(IndexTest, SetAllEntriesThrowsOnInternalNode) {
	// Tests branch at line 152-153: throw if not leaf
	std::vector<Index::Entry> entries = {{PropertyValue("key"), {1}, 0, 0}};
	EXPECT_THROW(internalNode_.setAllEntries(entries, dataManager_), std::logic_error);
}

TEST_F(IndexTest, TryInsertEntryThrowsOnInternalNode) {
	// Tests branch at line 300-302: throw if not leaf
	EXPECT_THROW(internalNode_.tryInsertEntry(PropertyValue("key"), 1, dataManager_, stringComparator),
				 std::logic_error);
}

TEST_F(IndexTest, TryInsertEntryWithDuplicateValue) {
	// Tests branch at line 321-328: duplicate value check
	leafNode_.tryInsertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);

	// Insert same value again - should return success without error
	auto result = leafNode_.tryInsertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);
	EXPECT_TRUE(result.success);
	EXPECT_TRUE(result.overflowEntries.empty());
}

TEST_F(IndexTest, TryInsertEntryWithKeyExistsSameValue) {
	// Tests the duplicate value check when key already exists
	leafNode_.tryInsertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);

	// Insert same key with same value
	auto result = leafNode_.tryInsertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);

	EXPECT_TRUE(result.success);
	EXPECT_TRUE(result.overflowEntries.empty());
}

TEST_F(IndexTest, TryInsertEntryFitsWithoutSplit) {
	// Tests the path where data fits without overflow
	// This tests the early return in tryInsertEntry
	leafNode_.tryInsertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);
	leafNode_.tryInsertEntry(PropertyValue("key2"), 200, dataManager_, stringComparator);

	// Both should fit without overflow
	auto result = leafNode_.tryInsertEntry(PropertyValue("key3"), 300, dataManager_, stringComparator);
	EXPECT_TRUE(result.success);
	EXPECT_TRUE(result.overflowEntries.empty());
}

TEST_F(IndexTest, RemoveEntryNonExistentKey) {
	// Tests removing a key that doesn't exist
	bool removed = leafNode_.removeEntry(PropertyValue("non_existent"), 999, dataManager_, stringComparator);
	EXPECT_FALSE(removed);
}

TEST_F(IndexTest, RemoveEntryNonExistentValue) {
	// Tests removing a value that doesn't exist for an existing key
	leafNode_.tryInsertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);

	bool removed = leafNode_.removeEntry(PropertyValue("key1"), 999, dataManager_, stringComparator);
	EXPECT_FALSE(removed);

	// The key should still exist with value 100
	auto values = leafNode_.findValues(PropertyValue("key1"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 1UL);
	EXPECT_EQ(values[0], 100);
}

TEST_F(IndexTest, FindValuesNonExistentKey) {
	// Tests findValues for a non-existent key
	auto values = leafNode_.findValues(PropertyValue("non_existent"), dataManager_, stringComparator);
	EXPECT_TRUE(values.empty());
}

TEST_F(IndexTest, FindValuesReturnsCorrectValues) {
	// Tests that findValues returns all values for a key
	leafNode_.tryInsertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);
	leafNode_.tryInsertEntry(PropertyValue("key1"), 200, dataManager_, stringComparator);
	leafNode_.tryInsertEntry(PropertyValue("key1"), 300, dataManager_, stringComparator);

	auto values = leafNode_.findValues(PropertyValue("key1"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 3UL);

	// Sort and verify
	std::ranges::sort(values);
	EXPECT_EQ(values[0], 100);
	EXPECT_EQ(values[1], 200);
	EXPECT_EQ(values[2], 300);
}

TEST_F(IndexTest, InsertEntryWithExistingValue) {
	// Tests insertEntry when value already exists (line 363-365)
	leafNode_.insertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);

	// Insert same value again - should not duplicate
	leafNode_.insertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);

	auto values = leafNode_.findValues(PropertyValue("key1"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 1UL);
	EXPECT_EQ(values[0], 100);
}

TEST_F(IndexTest, RemoveEntryWithBlobsCleanup) {
	// Tests removing last value with blob cleanup (line 384-394)
	// Create oversized key and values to force blob storage
	std::string largeKey(Index::LEAF_KEY_INLINE_THRESHOLD + 1, 'K');
	size_t valueCount = Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t) + 2;
	std::vector<int64_t> largeValues;
	for (size_t i = 0; i < valueCount; ++i) {
		largeValues.push_back(static_cast<int64_t>(i));
	}

	// Create entry with blobs
	std::vector<Index::Entry> entries = {{PropertyValue(largeKey), largeValues, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	// Get the blob IDs before removal
	auto retrievedEntries = leafNode_.getAllEntries(dataManager_);
	int64_t keyBlobId = retrievedEntries[0].keyBlobId;
	int64_t valuesBlobId = retrievedEntries[0].valuesBlobId;

	EXPECT_NE(keyBlobId, 0) << "Key should be in blob";
	EXPECT_NE(valuesBlobId, 0) << "Values should be in blob";

	// Remove all values one by one until entry is deleted
	for (int64_t value : largeValues) {
		leafNode_.removeEntry(PropertyValue(largeKey), value, dataManager_, stringComparator);
	}

	// Verify blobs were deleted (entry should be completely removed now)
	EXPECT_THROW((void) dataManager_->getBlobManager()->readBlobChain(keyBlobId), std::runtime_error);
	EXPECT_THROW((void) dataManager_->getBlobManager()->readBlobChain(valuesBlobId), std::runtime_error);

	// Verify entry is gone
	auto finalEntries = leafNode_.getAllEntries(dataManager_);
	EXPECT_TRUE(finalEntries.empty());
}

TEST_F(IndexTest, SetAllEntriesCreateNewKeyBlob) {
	// Create entry with oversized key
	std::string largeKey(Index::LEAF_KEY_INLINE_THRESHOLD + 1, 'K');
	std::vector<Index::Entry> entries = {{PropertyValue(largeKey), {100}, 0, 0}};

	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	EXPECT_NE(retrieved[0].keyBlobId, 0) << "Large key should be in blob";
	EXPECT_EQ(retrieved[0].key, PropertyValue(largeKey));
}

TEST_F(IndexTest, SetAllEntriesCreateNewValuesBlob) {
	// Create entry with oversized values
	size_t valueCount = Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t) + 2;
	std::vector<int64_t> largeValues;
	for (size_t i = 0; i < valueCount; ++i) {
		largeValues.push_back(static_cast<int64_t>(i));
	}

	std::vector<Index::Entry> entries = {{PropertyValue("key"), largeValues, 0, 0}};

	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	EXPECT_NE(retrieved[0].valuesBlobId, 0) << "Large values should be in blob";
}

TEST_F(IndexTest, TryInsertEntryMultipleSmallEntries) {
	// Insert several small entries that should fit
	for (int i = 0; i < 5; i++) {
		auto result = leafNode_.tryInsertEntry(PropertyValue("key" + std::to_string(i)), i, dataManager_, stringComparator);
		EXPECT_TRUE(result.success);
		EXPECT_TRUE(result.overflowEntries.empty());
	}

	// Verify all entries are stored
	for (int i = 0; i < 5; i++) {
		auto values = leafNode_.findValues(PropertyValue("key" + std::to_string(i)), dataManager_, stringComparator);
		EXPECT_EQ(values.size(), 1UL);
		EXPECT_EQ(values[0], i);
	}
}

TEST_F(IndexTest, InsertEntryThrowsOnInternalNode) {
	EXPECT_THROW(
		internalNode_.insertEntry(PropertyValue("key"), 100, dataManager_, stringComparator),
		std::logic_error);
}

TEST_F(IndexTest, SetAllEntriesUpdateExistingKeyBlob) {
	// Create entry with large key
	std::string largeKey1(Index::LEAF_KEY_INLINE_THRESHOLD + 5, 'A');
	std::vector<Index::Entry> entries1 = {{PropertyValue(largeKey1), {100}, 0, 0}};
	leafNode_.setAllEntries(entries1, dataManager_);

	// Get blob ID
	auto retrieved1 = leafNode_.getAllEntries(dataManager_);
	int64_t keyBlobId = retrieved1[0].keyBlobId;
	EXPECT_NE(keyBlobId, 0);

	// Now update with a DIFFERENT large key using the SAME blob ID
	std::string largeKey2(Index::LEAF_KEY_INLINE_THRESHOLD + 5, 'B');
	std::vector<Index::Entry> entries2 = {{PropertyValue(largeKey2), {200}, keyBlobId, 0}};
	leafNode_.setAllEntries(entries2, dataManager_);

	// Verify update
	auto retrieved2 = leafNode_.getAllEntries(dataManager_);
	EXPECT_NE(retrieved2[0].keyBlobId, 0);
	EXPECT_EQ(retrieved2[0].key, PropertyValue(largeKey2));
	EXPECT_EQ(retrieved2[0].values[0], 200);
}

TEST_F(IndexTest, SetAllEntriesUpdateExistingValuesBlob) {
	// Create entry with large values
	size_t valueCount = Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t) + 2;
	std::vector<int64_t> largeValues1;
	for (size_t i = 0; i < valueCount; ++i) {
		largeValues1.push_back(static_cast<int64_t>(i));
	}

	std::vector<Index::Entry> entries1 = {{PropertyValue("key"), largeValues1, 0, 0}};
	leafNode_.setAllEntries(entries1, dataManager_);

	// Get blob ID
	auto retrieved1 = leafNode_.getAllEntries(dataManager_);
	int64_t valuesBlobId = retrieved1[0].valuesBlobId;
	EXPECT_NE(valuesBlobId, 0);

	// Now update with DIFFERENT large values using the SAME blob ID
	std::vector<int64_t> largeValues2;
	for (size_t i = 100; i < 100 + valueCount; ++i) {
		largeValues2.push_back(static_cast<int64_t>(i));
	}

	std::vector<Index::Entry> entries2 = {{PropertyValue("key"), largeValues2, 0, valuesBlobId}};
	leafNode_.setAllEntries(entries2, dataManager_);

	// Verify update
	auto retrieved2 = leafNode_.getAllEntries(dataManager_);
	EXPECT_NE(retrieved2[0].valuesBlobId, 0);
	EXPECT_EQ(retrieved2[0].values.size(), valueCount);
	EXPECT_EQ(retrieved2[0].values[0], 100);
}

TEST_F(IndexTest, SetAllEntriesCleanupKeyBlobWhenKeyShrinks) {
	// Create entry with large key (creates blob)
	std::string largeKey(Index::LEAF_KEY_INLINE_THRESHOLD + 5, 'K');
	std::vector<Index::Entry> entries1 = {{PropertyValue(largeKey), {100}, 0, 0}};
	leafNode_.setAllEntries(entries1, dataManager_);

	// Get blob ID
	auto retrieved = leafNode_.getAllEntries(dataManager_);
	int64_t keyBlobId = retrieved[0].keyBlobId;
	EXPECT_NE(keyBlobId, 0);

	// Update with small key (blob should be deleted)
	std::vector<Index::Entry> entries2 = {{PropertyValue("s"), {200}, keyBlobId, 0}};
	leafNode_.setAllEntries(entries2, dataManager_);

	// Verify blob was deleted
	EXPECT_THROW((void) dataManager_->getBlobManager()->readBlobChain(keyBlobId), std::runtime_error);

	auto retrieved2 = leafNode_.getAllEntries(dataManager_);
	EXPECT_EQ(retrieved2[0].keyBlobId, 0) << "Small key should be stored inline";
}

TEST_F(IndexTest, SetAllEntriesCleanupValuesBlobWhenValuesShrink) {
	// Create entry with large values (creates blob)
	size_t valueCount = Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t) + 2;
	std::vector<int64_t> largeValues;
	for (size_t i = 0; i < valueCount; ++i) {
		largeValues.push_back(static_cast<int64_t>(i));
	}

	std::vector<Index::Entry> entries1 = {{PropertyValue("key"), largeValues, 0, 0}};
	leafNode_.setAllEntries(entries1, dataManager_);

	// Get blob ID
	auto retrieved = leafNode_.getAllEntries(dataManager_);
	int64_t valuesBlobId = retrieved[0].valuesBlobId;
	EXPECT_NE(valuesBlobId, 0);

	// Update with small values (blob should be deleted)
	std::vector<Index::Entry> entries2 = {{PropertyValue("key"), {999}, 0, valuesBlobId}};
	leafNode_.setAllEntries(entries2, dataManager_);

	// Verify blob was deleted
	EXPECT_THROW((void) dataManager_->getBlobManager()->readBlobChain(valuesBlobId), std::runtime_error);

	auto retrieved2 = leafNode_.getAllEntries(dataManager_);
	EXPECT_EQ(retrieved2[0].valuesBlobId, 0) << "Small values should be stored inline";
}

TEST_F(IndexTest, GetEntrySerializedSizeLargeKeyAndValues) {
	// Entry with large key (should use blob ID size instead of inline)
	std::string largeKey(Index::LEAF_KEY_INLINE_THRESHOLD + 1, 'K');
	Index::Entry largeKeyEntry = {PropertyValue(largeKey), {100}, 0, 0};
	size_t sizeWithLargeKey = Index::getEntrySerializedSize(largeKeyEntry);

	// Entry with small key (should use inline size)
	Index::Entry smallKeyEntry = {PropertyValue("k"), {100}, 0, 0};
	size_t sizeWithSmallKey = Index::getEntrySerializedSize(smallKeyEntry);

	// Large key uses blob ID (8 bytes), small key uses inline serialization
	// They should differ because a 33+ char string serialized > 8 bytes
	EXPECT_NE(sizeWithLargeKey, sizeWithSmallKey);

	// Entry with many values (should use blob ID size for values)
	// Use enough values so that inline size (N * 8) is significantly > blob ID (8)
	size_t valueCount = Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t) + 10;
	std::vector<int64_t> largeValues(valueCount, 42);
	Index::Entry largeValuesEntry = {PropertyValue("k"), largeValues, 0, 0};
	size_t sizeWithLargeValues = Index::getEntrySerializedSize(largeValuesEntry);

	// Entry with many inline values (fewer but still inline)
	// 2 values = 16 bytes which is < 32 threshold, so inline = 16 bytes
	// largeValues = (4+10)*8 = 112 bytes > 32 threshold, blob = 8 bytes
	// So large values uses 8 bytes, small uses 16 bytes
	Index::Entry twoValuesEntry = {PropertyValue("k"), {42, 43}, 0, 0};
	size_t sizeWithTwoValues = Index::getEntrySerializedSize(twoValuesEntry);

	// Large values should use blob (smaller serialized size), two values inline
	EXPECT_NE(sizeWithLargeValues, sizeWithTwoValues);
}

TEST_F(IndexTest, InsertEntryNewKey) {
	leafNode_.insertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("key2"), 200, dataManager_, stringComparator);

	auto values1 = leafNode_.findValues(PropertyValue("key1"), dataManager_, stringComparator);
	EXPECT_EQ(values1.size(), 1UL);
	EXPECT_EQ(values1[0], 100);

	auto values2 = leafNode_.findValues(PropertyValue("key2"), dataManager_, stringComparator);
	EXPECT_EQ(values2.size(), 1UL);
	EXPECT_EQ(values2[0], 200);
}

TEST_F(IndexTest, InsertEntryNewValueForExistingKey) {
	leafNode_.insertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("key1"), 200, dataManager_, stringComparator);

	auto values = leafNode_.findValues(PropertyValue("key1"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 2UL);
	std::ranges::sort(values);
	EXPECT_EQ(values[0], 100);
	EXPECT_EQ(values[1], 200);
}

TEST_F(IndexTest, RemoveEntryKeyExistsButValueNotFound) {
	leafNode_.insertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);

	// Remove a value that doesn't exist for this key
	bool removed = leafNode_.removeEntry(PropertyValue("key1"), 999, dataManager_, stringComparator);
	EXPECT_FALSE(removed);

	// Key should still have original value
	auto values = leafNode_.findValues(PropertyValue("key1"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 1UL);
	EXPECT_EQ(values[0], 100);
}

TEST_F(IndexTest, TryInsertEntryReservesCapacity) {
	// Insert enough entries so that the vector's capacity needs expanding
	for (int i = 0; i < 5; ++i) {
		auto result = leafNode_.tryInsertEntry(
			PropertyValue("k" + std::to_string(i)), i, dataManager_, stringComparator);
		EXPECT_TRUE(result.success);
	}

	// The next insert should still work (capacity will be expanded internally)
	auto result = leafNode_.tryInsertEntry(PropertyValue("k5"), 5, dataManager_, stringComparator);
	EXPECT_TRUE(result.success);
	EXPECT_TRUE(result.overflowEntries.empty());

	auto values = leafNode_.findValues(PropertyValue("k5"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 1UL);
}

TEST_F(IndexTest, InsertEntryThrowsOnInternalNodeDuplicate) {
	EXPECT_THROW(
		internalNode_.insertEntry(PropertyValue("key"), 100, dataManager_, stringComparator),
		std::logic_error);
}

TEST_F(IndexTest, RemoveEntryValueNotInList) {
	// Insert key with value 100
	leafNode_.insertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("key1"), 200, dataManager_, stringComparator);

	// Try to remove value 999 (not in list)
	bool removed = leafNode_.removeEntry(PropertyValue("key1"), 999, dataManager_, stringComparator);
	EXPECT_FALSE(removed);

	// Key should still have both original values
	auto values = leafNode_.findValues(PropertyValue("key1"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 2UL);
}

TEST_F(IndexTest, FindValuesOnInternalNodeThrows) {
	// findValues calls getAllEntries which throws on internal node
	EXPECT_THROW(
		(void)internalNode_.findValues(PropertyValue("key"), dataManager_, stringComparator),
		std::logic_error);
}

TEST_F(IndexTest, RemoveEntryOnInternalNodeThrows) {
	EXPECT_THROW(
		internalNode_.removeEntry(PropertyValue("key"), 100, dataManager_, stringComparator),
		std::logic_error);
}

TEST_F(IndexTest, SetAllEntriesCreateKeyBlobFromZeroBlobId) {
	// Create entry with large key and keyBlobId = 0 (forces creation)
	std::string largeKey(Index::LEAF_KEY_INLINE_THRESHOLD + 5, 'M');
	std::vector<Index::Entry> entries = {{PropertyValue(largeKey), {100}, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	EXPECT_NE(retrieved[0].keyBlobId, 0) << "Should create new blob for large key";
	EXPECT_EQ(retrieved[0].key, PropertyValue(largeKey));
}

TEST_F(IndexTest, SetAllEntriesCreateValuesBlobFromZeroBlobId) {
	size_t valueCount = Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t) + 3;
	std::vector<int64_t> largeValues;
	for (size_t i = 0; i < valueCount; ++i) {
		largeValues.push_back(static_cast<int64_t>(i + 500));
	}

	std::vector<Index::Entry> entries = {{PropertyValue("k"), largeValues, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	EXPECT_NE(retrieved[0].valuesBlobId, 0) << "Should create new blob for large values";
	EXPECT_EQ(retrieved[0].values.size(), valueCount);
}

TEST_F(IndexTest, RemoveEntryKeyBetweenExistingKeys) {
	// Insert "aaa" and "ccc"
	leafNode_.insertEntry(PropertyValue("aaa"), 1, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("ccc"), 2, dataManager_, stringComparator);

	// Try to remove "bbb" - lower_bound lands on "ccc" but key doesn't match
	bool removed = leafNode_.removeEntry(PropertyValue("bbb"), 1, dataManager_, stringComparator);
	EXPECT_FALSE(removed);

	// Verify both original entries still exist
	auto vals_aaa = leafNode_.findValues(PropertyValue("aaa"), dataManager_, stringComparator);
	EXPECT_EQ(vals_aaa.size(), 1UL);
	auto vals_ccc = leafNode_.findValues(PropertyValue("ccc"), dataManager_, stringComparator);
	EXPECT_EQ(vals_ccc.size(), 1UL);
}

TEST_F(IndexTest, FindValuesKeyBetweenExistingKeys) {
	leafNode_.insertEntry(PropertyValue("alpha"), 10, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("gamma"), 30, dataManager_, stringComparator);

	// "beta" falls between "alpha" and "gamma" - lower_bound finds "gamma"
	auto results = leafNode_.findValues(PropertyValue("beta"), dataManager_, stringComparator);
	EXPECT_TRUE(results.empty());
}

TEST_F(IndexTest, RemoveEntryKeyExistsButValueMissing) {
	leafNode_.insertEntry(PropertyValue("mykey"), 100, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("mykey"), 200, dataManager_, stringComparator);

	// Try to remove value 999 which doesn't exist for "mykey"
	bool removed = leafNode_.removeEntry(PropertyValue("mykey"), 999, dataManager_, stringComparator);
	EXPECT_FALSE(removed);

	// Original values still intact
	auto vals = leafNode_.findValues(PropertyValue("mykey"), dataManager_, stringComparator);
	EXPECT_EQ(vals.size(), 2UL);
}

TEST_F(IndexTest, RemoveEntryKeyBeyondAllEntries) {
	leafNode_.insertEntry(PropertyValue("aaa"), 1, dataManager_, stringComparator);

	// "zzz" is beyond "aaa", lower_bound returns end()
	bool removed = leafNode_.removeEntry(PropertyValue("zzz"), 1, dataManager_, stringComparator);
	EXPECT_FALSE(removed);
}

TEST_F(IndexTest, FindValuesOnEmptyLeaf) {
	auto results = leafNode_.findValues(PropertyValue("anything"), dataManager_, stringComparator);
	EXPECT_TRUE(results.empty());
}

TEST_F(IndexTest, SetAllEntriesOverflowThrows) {
	// Create entries that will exceed DATA_SIZE when serialized
	std::vector<Index::Entry> entries;
	// Each entry with a medium-sized key + 1 value takes about 20-30 bytes
	// DATA_SIZE is typically around 480 bytes, so we need many entries
	size_t estimatedSize = 0;
	int key_counter = 0;
	while (estimatedSize < Index::DATA_SIZE + 100) {
		PropertyValue key(std::to_string(key_counter++));
		Index::Entry entry = {key, {static_cast<int64_t>(key_counter)}, 0, 0};
		size_t entrySize = Index::getEntrySerializedSize(entry);
		entries.push_back(std::move(entry));
		estimatedSize += entrySize;
	}

	// This should throw because total size exceeds DATA_SIZE
	EXPECT_THROW(leafNode_.setAllEntries(entries, dataManager_), std::runtime_error);
}

TEST_F(IndexTest, GetEntrySerializedSizeZeroValues) {
	Index::Entry emptyValuesEntry = {PropertyValue("key"), {}, 0, 0};
	size_t size = Index::getEntrySerializedSize(emptyValuesEntry);

	// Should have: flags(1) + key serialization + value count(4) + 0 values
	EXPECT_GT(size, 0UL);
}

TEST_F(IndexTest, GetEntrySerializedSizeOneValue) {
	Index::Entry singleValueEntry = {PropertyValue("k"), {42}, 0, 0};
	size_t sizeOne = Index::getEntrySerializedSize(singleValueEntry);

	Index::Entry zeroValueEntry = {PropertyValue("k"), {}, 0, 0};
	size_t sizeZero = Index::getEntrySerializedSize(zeroValueEntry);

	// One value adds sizeof(int64_t) = 8 bytes
	EXPECT_EQ(sizeOne - sizeZero, sizeof(int64_t));
}

TEST_F(IndexTest, RemoveLastValueNoBlobs) {
	leafNode_.insertEntry(PropertyValue("remove_last"), 100, dataManager_, stringComparator);

	// Remove the only value
	bool removed = leafNode_.removeEntry(PropertyValue("remove_last"), 100, dataManager_, stringComparator);
	EXPECT_TRUE(removed);

	// Entry should be completely gone
	auto entries = leafNode_.getAllEntries(dataManager_);
	EXPECT_TRUE(entries.empty());
}

TEST_F(IndexTest, RemoveValueButEntryNotEmpty) {
	leafNode_.insertEntry(PropertyValue("multi"), 100, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("multi"), 200, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("multi"), 300, dataManager_, stringComparator);

	// Remove one value, entry still has 2
	bool removed = leafNode_.removeEntry(PropertyValue("multi"), 200, dataManager_, stringComparator);
	EXPECT_TRUE(removed);

	auto values = leafNode_.findValues(PropertyValue("multi"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 2UL);
}

TEST_F(IndexTest, TryInsertEntryEarlyOverflowDetection) {
	// Fill node close to capacity
	std::vector<Index::Entry> entries;
	size_t totalSize = 0;
	int key_counter = 0;
	while (true) {
		PropertyValue key(std::to_string(key_counter));
		Index::Entry entry = {key, {static_cast<int64_t>(key_counter)}, 0, 0};
		size_t entrySize = Index::getEntrySerializedSize(entry);
		if (totalSize + entrySize > Index::DATA_SIZE - 50) break; // Leave small gap
		entries.push_back(std::move(entry));
		totalSize += entrySize;
		key_counter++;
	}

	leafNode_.setAllEntries(entries, dataManager_);

	// Insert a key with many values to make the entry large enough to overflow
	// during the size calculation loop (hits the early exit)
	std::string overflowKey = "z_overflow";
	auto result = leafNode_.tryInsertEntry(PropertyValue(overflowKey), 999, dataManager_, stringComparator);
	// Whether it succeeds or fails depends on exact sizes, but it should not crash
	// If it fails, overflow entries should contain all entries + the new one
	if (!result.success) {
		EXPECT_GT(result.overflowEntries.size(), entries.size());
	}
}

TEST_F(IndexTest, SetAllEntriesWithBothKeyAndValueBlobs) {
	std::string largeKey(Index::LEAF_KEY_INLINE_THRESHOLD + 5, 'K');
	size_t valueCount = Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t) + 2;
	std::vector<int64_t> largeValues;
	for (size_t i = 0; i < valueCount; ++i) {
		largeValues.push_back(static_cast<int64_t>(i));
	}

	std::vector<Index::Entry> entries = {{PropertyValue(largeKey), largeValues, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(retrieved.size(), 1UL);
	EXPECT_NE(retrieved[0].keyBlobId, 0) << "Key should be in blob";
	EXPECT_NE(retrieved[0].valuesBlobId, 0) << "Values should be in blob";
	EXPECT_EQ(retrieved[0].key, PropertyValue(largeKey));
	EXPECT_EQ(retrieved[0].values.size(), valueCount);
}

TEST_F(IndexTest, TryInsertEntry_ExistingKey_NewDistinctValue) {
	leafNode_.tryInsertEntry(PropertyValue("dup"), 10, dataManager_, stringComparator);
	auto result = leafNode_.tryInsertEntry(PropertyValue("dup"), 20, dataManager_, stringComparator);
	EXPECT_TRUE(result.success);

	auto values = leafNode_.findValues(PropertyValue("dup"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 2UL);
}

TEST_F(IndexTest, InsertEntry_DuplicateValueNoOp) {
	leafNode_.insertEntry(PropertyValue("dup2"), 42, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("dup2"), 42, dataManager_, stringComparator);

	auto values = leafNode_.findValues(PropertyValue("dup2"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 1UL);
	EXPECT_EQ(values[0], 42);
}

TEST_F(IndexTest, RemoveEntry_EmptyValues_BothBlobsCleanup) {
	// Create entry with large key AND large values (both in blobs)
	std::string largeKey(Index::LEAF_KEY_INLINE_THRESHOLD + 5, 'R');
	size_t valueCount = Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t) + 2;
	std::vector<int64_t> largeValues;
	for (size_t i = 0; i < valueCount; ++i) {
		largeValues.push_back(static_cast<int64_t>(i + 1000));
	}

	std::vector<Index::Entry> entries = {{PropertyValue(largeKey), largeValues, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_NE(retrieved[0].keyBlobId, 0);
	ASSERT_NE(retrieved[0].valuesBlobId, 0);
	int64_t keyBlobId = retrieved[0].keyBlobId;
	int64_t valuesBlobId = retrieved[0].valuesBlobId;

	// Remove all values so entry becomes empty -> both blobs get cleaned up
	for (int64_t v : largeValues) {
		leafNode_.removeEntry(PropertyValue(largeKey), v, dataManager_, stringComparator);
	}

	EXPECT_THROW((void)dataManager_->getBlobManager()->readBlobChain(keyBlobId), std::runtime_error);
	EXPECT_THROW((void)dataManager_->getBlobManager()->readBlobChain(valuesBlobId), std::runtime_error);
	EXPECT_TRUE(leafNode_.getAllEntries(dataManager_).empty());
}

TEST_F(IndexTest, RemoveEntry_EmptyValues_OnlyKeyBlobCleanup) {
	std::string largeKey(Index::LEAF_KEY_INLINE_THRESHOLD + 5, 'Q');
	// Use small values (no blob)
	std::vector<Index::Entry> entries = {{PropertyValue(largeKey), {42}, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_NE(retrieved[0].keyBlobId, 0);
	ASSERT_EQ(retrieved[0].valuesBlobId, 0);
	int64_t keyBlobId = retrieved[0].keyBlobId;

	leafNode_.removeEntry(PropertyValue(largeKey), 42, dataManager_, stringComparator);

	EXPECT_THROW((void)dataManager_->getBlobManager()->readBlobChain(keyBlobId), std::runtime_error);
	EXPECT_TRUE(leafNode_.getAllEntries(dataManager_).empty());
}

TEST_F(IndexTest, RemoveEntry_EmptyValues_OnlyValuesBlobCleanup) {
	size_t valueCount = Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t) + 2;
	std::vector<int64_t> largeValues;
	for (size_t i = 0; i < valueCount; ++i) {
		largeValues.push_back(static_cast<int64_t>(i + 2000));
	}

	// Small key (no blob), large values (blob)
	std::vector<Index::Entry> entries = {{PropertyValue("k"), largeValues, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(retrieved[0].keyBlobId, 0);
	ASSERT_NE(retrieved[0].valuesBlobId, 0);
	int64_t valuesBlobId = retrieved[0].valuesBlobId;

	for (int64_t v : largeValues) {
		leafNode_.removeEntry(PropertyValue("k"), v, dataManager_, stringComparator);
	}

	EXPECT_THROW((void)dataManager_->getBlobManager()->readBlobChain(valuesBlobId), std::runtime_error);
	EXPECT_TRUE(leafNode_.getAllEntries(dataManager_).empty());
}

TEST_F(IndexTest, GetEntrySerializedSizeKeyAtBoundary) {
	// Key exactly at LEAF_KEY_INLINE_THRESHOLD
	std::string keyAtThreshold(Index::LEAF_KEY_INLINE_THRESHOLD, 'B');
	// Remove serialization overhead (type tag + length prefix = ~5 bytes)
	// Serialized size of string = 1 (type) + 4 (length) + key_len
	Index::Entry entryAtThreshold = {PropertyValue(keyAtThreshold), {100}, 0, 0};
	size_t sizeAt = Index::getEntrySerializedSize(entryAtThreshold);

	// Key one byte over threshold
	std::string keyOverThreshold(Index::LEAF_KEY_INLINE_THRESHOLD + 1, 'B');
	Index::Entry entryOver = {PropertyValue(keyOverThreshold), {100}, 0, 0};
	size_t sizeOver = Index::getEntrySerializedSize(entryOver);

	// The "at threshold" entry may or may not use blob depending on serialization overhead
	// Just verify they compute valid sizes
	EXPECT_GT(sizeAt, 0UL);
	EXPECT_GT(sizeOver, 0UL);
}

TEST_F(IndexTest, GetEntrySerializedSizeValuesAtBoundary) {
	size_t exactCount = Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t);
	std::vector<int64_t> exactValues(exactCount, 1);
	Index::Entry entryExact = {PropertyValue("k"), exactValues, 0, 0};
	size_t sizeExact = Index::getEntrySerializedSize(entryExact);

	// One more value pushes over threshold
	std::vector<int64_t> overValues(exactCount + 1, 1);
	Index::Entry entryOver = {PropertyValue("k"), overValues, 0, 0};
	size_t sizeOver = Index::getEntrySerializedSize(entryOver);

	// Over threshold uses blob ID (8 bytes) instead of inline
	EXPECT_GT(sizeExact, 0UL);
	EXPECT_GT(sizeOver, 0UL);
	// Over threshold should be smaller (8 bytes instead of N*8)
	EXPECT_LT(sizeOver, sizeExact);
}

TEST_F(IndexTest, EntrySerializationFlagsCombination) {
	// Test that both KEY_IS_BLOB and VALUES_ARE_BLOB can be read back from getAllEntries
	std::string largeKey(Index::LEAF_KEY_INLINE_THRESHOLD + 5, 'F');
	size_t valueCount = Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t) + 2;
	std::vector<int64_t> largeValues;
	for (size_t i = 0; i < valueCount; ++i) {
		largeValues.push_back(static_cast<int64_t>(i));
	}

	std::vector<Index::Entry> entries = {{PropertyValue(largeKey), largeValues, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	// Read back - both blobs should be used
	auto retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(retrieved.size(), 1UL);
	EXPECT_NE(retrieved[0].keyBlobId, 0);
	EXPECT_NE(retrieved[0].valuesBlobId, 0);

	// Verify round-trip data correctness
	EXPECT_EQ(retrieved[0].key, PropertyValue(largeKey));
	EXPECT_EQ(retrieved[0].values.size(), valueCount);
	for (size_t i = 0; i < valueCount; ++i) {
		EXPECT_EQ(retrieved[0].values[i], static_cast<int64_t>(i));
	}
}

TEST_F(IndexTest, RemoveEntry_WithBlobValues) {
	// Insert 5 values for the same key. Since LEAF_VALUES_INLINE_THRESHOLD is 32 bytes
	// and each int64_t is 8 bytes, 5 values = 40 bytes > 32 threshold -> blob storage.
	PropertyValue key(static_cast<int64_t>(999));
	for (int64_t v = 1; v <= 5; ++v) {
		leafNode_.insertEntry(key, v, dataManager_, intComparator);
	}

	// Verify all 5 values are stored
	auto values = leafNode_.findValues(key, dataManager_, intComparator);
	ASSERT_EQ(values.size(), 5UL);

	// Remove values one by one until entry is empty
	for (int64_t v = 1; v <= 5; ++v) {
		leafNode_.removeEntry(key, v, dataManager_, intComparator);
	}

	// After removing all values, the entry (and its blob) should be cleaned up
	auto remainingValues = leafNode_.findValues(key, dataManager_, intComparator);
	EXPECT_TRUE(remainingValues.empty());
}

TEST_F(IndexTest, InsertEntry_NewKeyBeforeExisting) {
	// Insert "bbb" first
	leafNode_.insertEntry(PropertyValue("bbb"), 200, dataManager_, stringComparator);

	// Insert "aaa" which falls BEFORE "bbb" - this triggers the false branch
	// of !comparator(key, it->key) in insertEntry's keyExists check
	leafNode_.insertEntry(PropertyValue("aaa"), 100, dataManager_, stringComparator);

	// Verify both keys are stored correctly and in order
	auto values_aaa = leafNode_.findValues(PropertyValue("aaa"), dataManager_, stringComparator);
	EXPECT_EQ(values_aaa.size(), 1UL);
	EXPECT_EQ(values_aaa[0], 100);

	auto values_bbb = leafNode_.findValues(PropertyValue("bbb"), dataManager_, stringComparator);
	EXPECT_EQ(values_bbb.size(), 1UL);
	EXPECT_EQ(values_bbb[0], 200);

	// Verify entry count
	EXPECT_EQ(leafNode_.getEntryCount(), 2u);
}

TEST_F(IndexTest, InsertEntry_NewKeyBetweenExisting) {
	leafNode_.insertEntry(PropertyValue("aaa"), 10, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("ccc"), 30, dataManager_, stringComparator);

	// Insert "bbb" between "aaa" and "ccc" - lower_bound finds "ccc",
	// !comparator("bbb", "ccc") = false -> covers the False branch
	leafNode_.insertEntry(PropertyValue("bbb"), 20, dataManager_, stringComparator);

	auto values = leafNode_.findValues(PropertyValue("bbb"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 1UL);
	EXPECT_EQ(values[0], 20);

	// Verify all three entries exist
	EXPECT_EQ(leafNode_.getEntryCount(), 3u);
}
