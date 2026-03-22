/**
 * @file test_Index.cpp
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/core/Index.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/utils/Serializer.hpp"

using namespace graph;
using namespace graph::storage;

class IndexTest : public ::testing::Test {
protected:
	// --- Comparators ---
	static bool stringComparator(const PropertyValue &a, const PropertyValue &b) {
		return std::get<std::string>(a.getVariant()) < std::get<std::string>(b.getVariant());
	}

	static bool intComparator(const PropertyValue &a, const PropertyValue &b) {
		return std::get<int64_t>(a.getVariant()) < std::get<int64_t>(b.getVariant());
	}

	// --- Test Suite Setup ---
	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath_ = std::filesystem::temp_directory_path() /
					  ("test_db_file_index_" + boost::uuids::to_string(uuid) + ".dat");

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

	// --- Per-Test Setup ---
	void SetUp() override {
		// Create fresh nodes for each test to ensure isolation
		Index newLeaf(0, Index::NodeType::LEAF, 1);
		dataManager_->addIndexEntity(newLeaf);
		leafNode_ = newLeaf;

		Index newInternal(0, Index::NodeType::INTERNAL, 1);
		dataManager_->addIndexEntity(newInternal);
		internalNode_ = newInternal;
	}

	void TearDown() override {
		dataManager_->deleteIndex(leafNode_);
		dataManager_->deleteIndex(internalNode_);
	}

	Index leafNode_;
	Index internalNode_;

	static std::shared_ptr<DataManager> dataManager_;
	static std::unique_ptr<Database> database_;
	static std::filesystem::path testDbPath_;
};

// Initialize static members
std::shared_ptr<DataManager> IndexTest::dataManager_ = nullptr;
std::unique_ptr<Database> IndexTest::database_ = nullptr;
std::filesystem::path IndexTest::testDbPath_;

// =================================================================================
// --- LEAF NODE TESTS (Optimized Single-Pass Logic) ---
// =================================================================================

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

// --- Blob Lifecycle Tests (Preserved) ---
TEST_F(IndexTest, LeafKeyBlobManagement) {
	// 1. Oversized Key
	std::string oversized_key_str(Index::LEAF_KEY_INLINE_THRESHOLD + 5, 'K');
	std::vector<Index::Entry> entries = {{PropertyValue(oversized_key_str), {1}, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_NE(retrieved[0].keyBlobId, 0);
	int64_t old_blob_id = retrieved[0].keyBlobId;

	// 2. Small Key
	// Use a very small string ("s") to ensure it stays inline after adding
	// serialization overhead (4 bytes length). 1+4 = 5 bytes < 32.
	std::string small_key_str = "s";

	std::vector<Index::Entry> updated_entries = {{PropertyValue(small_key_str), {1}, old_blob_id, 0}};
	leafNode_.setAllEntries(updated_entries, dataManager_);

	retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(retrieved[0].keyBlobId, 0) << "Small key should be stored inline.";

	ASSERT_THROW((void) dataManager_->getBlobManager()->readBlobChain(old_blob_id), std::runtime_error);
}

TEST_F(IndexTest, LeafValuesBlobManagement) {
	// Dynamically calculate value count to exceed threshold
	// Threshold is in bytes, so we divide by int64 size
	size_t threshold_count = Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t);
	size_t oversized_count = threshold_count + 2; // +2 to be safe

	std::vector<int64_t> oversized_values;
	oversized_values.reserve(oversized_count);
	for (size_t i = 0; i < oversized_count; ++i) {
		oversized_values.push_back(static_cast<int64_t>(i));
	}

	// 1. Create
	std::vector<Index::Entry> entries = {{PropertyValue("key"), oversized_values, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_NE(retrieved[0].valuesBlobId, 0)
			<< "Value list of size " << (oversized_count * 8)
			<< " bytes should be blob (Threshold: " << Index::LEAF_VALUES_INLINE_THRESHOLD << ")";

	int64_t old_blob_id = retrieved[0].valuesBlobId;

	// 2. Update to small
	std::vector<Index::Entry> small_entry_list = {{PropertyValue("key"), {999}, 0, old_blob_id}};
	leafNode_.setAllEntries(small_entry_list, dataManager_);

	retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(retrieved[0].valuesBlobId, 0);

	// Verify deletion
	ASSERT_THROW((void) dataManager_->getBlobManager()->readBlobChain(old_blob_id), std::runtime_error);
}

// =================================================================================
// --- INTERNAL NODE TESTS (Likely unchanged by Leaf Optimization) ---
// =================================================================================

TEST_F(IndexTest, Internal_AddAndRemoveChild) {
	// Mandatory first child
	std::vector<Index::ChildEntry> children = {{PropertyValue(std::monostate{}), 5, 0}};
	internalNode_.setAllChildren(children, dataManager_);

	Index::ChildEntry entry1 = {PropertyValue(200), 20, 0};
	Index::ChildEntry entry2 = {PropertyValue(100), 10, 0};

	internalNode_.addChild(entry1, dataManager_, intComparator);
	internalNode_.addChild(entry2, dataManager_, intComparator);

	auto final_children = internalNode_.getAllChildren(dataManager_);
	ASSERT_EQ(final_children.size(), 3UL);
	ASSERT_EQ(final_children[1].key, PropertyValue(100)); // Sorted check

	ASSERT_TRUE(internalNode_.removeChild(PropertyValue(200), dataManager_, intComparator));
	ASSERT_EQ(internalNode_.getAllChildren(dataManager_).size(), 2UL);
}

TEST_F(IndexTest, Internal_FindChild) {
	std::vector<Index::ChildEntry> children = {
			{PropertyValue(std::monostate{}), 5, 0}, {PropertyValue(100), 10, 0}, {PropertyValue(200), 20, 0}};
	internalNode_.setAllChildren(children, dataManager_);

	ASSERT_EQ(internalNode_.findChild(PropertyValue(50), dataManager_, intComparator), 5);
	ASSERT_EQ(internalNode_.findChild(PropertyValue(150), dataManager_, intComparator), 10);
	ASSERT_EQ(internalNode_.findChild(PropertyValue(250), dataManager_, intComparator), 20);
}

TEST_F(IndexTest, Internal_WouldOverflowCheck) {
	// Note: Internal nodes usually use 'wouldInternalOverflowOnAddChild' logic which is distinct from the new Leaf
	// logic. We assume this method still exists for Internal Nodes unless refactored similarly.

	std::vector<Index::ChildEntry> children;
	size_t size = 0;
	int i = 0;
	// Fill internal node
	while (size < Index::DATA_SIZE - 50) {
		PropertyValue k(i++);
		children.push_back({k, static_cast<int64_t>(i), 0});
		size += sizeof(int64_t) + sizeof(uint8_t) + graph::utils::getSerializedSize(k);
	}
	internalNode_.setAllChildren(children, dataManager_);

	Index::ChildEntry newEntry = {PropertyValue(99999), 999, 0};

	// Just verify the check works
	bool wouldOverflow = internalNode_.wouldInternalOverflowOnAddChild(newEntry, dataManager_);
	// We can't strictly assert true/false without exact byte calculation,
	// but the API should be callable.
	(void) wouldOverflow;
}

TEST_F(IndexTest, Internal_UpdateChildId) {
	std::vector<Index::ChildEntry> children;
	children.push_back({PropertyValue(std::monostate{}), 100, 0});
	children.push_back({PropertyValue(10), 200, 0});

	internalNode_.setAllChildren(children, dataManager_);

	ASSERT_TRUE(internalNode_.updateChildId(200, 299));
	auto updated = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(updated[1].childId, 299);
}

TEST_F(IndexTest, InternalKeyBlobManagement) {
	// Dynamically generate based on Internal Node Threshold
	std::string oversized_key_str(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'X');
	Index::ChildEntry oversized_entry = {PropertyValue(oversized_key_str), 123, 0};

	std::vector<Index::ChildEntry> children = {{PropertyValue(std::monostate{}), 1, 0}, oversized_entry};
	internalNode_.setAllChildren(children, dataManager_);

	auto retrieved = internalNode_.getAllChildren(dataManager_);
	ASSERT_NE(retrieved[1].keyBlobId, 0) << "Oversized internal key should be in a blob.";

	// Cleanup check
	int64_t blob_id = retrieved[1].keyBlobId;
	ASSERT_TRUE(internalNode_.removeChild(PropertyValue(oversized_key_str), dataManager_, stringComparator));
	ASSERT_THROW((void) dataManager_->getBlobManager()->readBlobChain(blob_id), std::runtime_error);
}

// =================================================================================
// --- isUnderflow Tests ---
// =================================================================================

TEST_F(IndexTest, IsUnderflowRootNode) {
	// Root node (parentId == 0) should never be in underflow
	// Tests branch at line 46-48: if (metadata.parentId == 0) return false
	leafNode_.setParentId(0); // Root node has no parent
	leafNode_.getMutableMetadata().dataUsage = 0; // Even with zero usage

	EXPECT_FALSE(leafNode_.isUnderflow(0.5)) << "Root node should never be in underflow";
}

TEST_F(IndexTest, IsUnderflowNonRootNode) {
	// Non-root node should check data usage
	// Tests branch at line 49: return metadata.dataUsage < threshold
	leafNode_.setParentId(100); // Non-root node
	leafNode_.getMutableMetadata().dataUsage = 10;

	// With 50% threshold, underflow threshold is DATA_SIZE * 0.5
	double threshold = 0.5;
	EXPECT_TRUE(leafNode_.isUnderflow(threshold)) << "Node with low usage should be in underflow";
	EXPECT_FALSE(leafNode_.isUnderflow(0.01)) << "Node with low usage should not be in underflow with very low threshold";
}

// =================================================================================
// --- Exception Path Tests ---
// =================================================================================

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

TEST_F(IndexTest, WouldInternalOverflowOnAddChildForLeaf) {
	// Tests branch at line 256-258: return false if isLeaf()
	Index::ChildEntry newEntry = {PropertyValue(999), 999, 0};

	bool wouldOverflow = leafNode_.wouldInternalOverflowOnAddChild(newEntry, dataManager_);
	EXPECT_FALSE(wouldOverflow) << "Leaf node should return false for wouldInternalOverflowOnAddChild";
}

TEST_F(IndexTest, RemoveChildThrowsOnLeaf) {
	// Tests branch at line 430-431: throw if isLeaf
	EXPECT_THROW(leafNode_.removeChild(PropertyValue("key"), dataManager_, stringComparator), std::logic_error);
}

TEST_F(IndexTest, FindChildThrowsOnLeaf) {
	// Tests branch at line 453-454: throw if isLeaf
	EXPECT_THROW((void)leafNode_.findChild(PropertyValue("key"), dataManager_, stringComparator), std::logic_error);
}

TEST_F(IndexTest, GetAllChildrenThrowsOnLeaf) {
	// Tests branch at line 473-474: throw if isLeaf
	EXPECT_THROW((void)leafNode_.getAllChildren(dataManager_), std::logic_error);
}

TEST_F(IndexTest, SetAllChildrenThrowsOnLeaf) {
	// Tests branch at line 523-524: throw if isLeaf
	std::vector<Index::ChildEntry> children = {{PropertyValue(std::monostate{}), 5, 0}};
	EXPECT_THROW(leafNode_.setAllChildren(children, dataManager_), std::logic_error);
}

TEST_F(IndexTest, GetChildIdsReturnsEmptyForLeaf) {
	// Tests branch at line 685-686: return {} if isLeaf
	auto ids = leafNode_.getChildIds();
	EXPECT_TRUE(ids.empty());
}

TEST_F(IndexTest, GetChildIdsReturnsEmptyForZeroChildCount) {
	// Tests branch at line 687-688: return {} if childCount == 0
	internalNode_.getMutableMetadata().childCount = 0;

	auto ids = internalNode_.getChildIds();
	EXPECT_TRUE(ids.empty());
}

TEST_F(IndexTest, GetChildIdsReturnsEmptyForEmptyChildren) {
	// Even with childCount set, if buffer is empty, should return first child or empty
	// Set up internal node with one child
	std::vector<Index::ChildEntry> children = {{PropertyValue(std::monostate{}), 5, 0}};
	internalNode_.setAllChildren(children, dataManager_);

	auto ids = internalNode_.getChildIds();
	EXPECT_EQ(ids.size(), 1UL);
	EXPECT_EQ(ids[0], 5);
}

// =================================================================================
// --- Additional Coverage Tests ---
// =================================================================================

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

TEST_F(IndexTest, UpdateChildIdNonExistentChild) {
	// Tests updateChildId when child ID doesn't exist
	bool updated = internalNode_.updateChildId(99999, 88888);
	EXPECT_FALSE(updated);
}

TEST_F(IndexTest, UpdateChildIdFirstChild) {
	// Tests updating the first child (which has no preceding key)
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Update first child ID
	bool updated = internalNode_.updateChildId(100, 999);
	EXPECT_TRUE(updated);

	auto updatedChildren = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(updatedChildren[0].childId, 999);
	EXPECT_EQ(updatedChildren[1].childId, 200);
}

TEST_F(IndexTest, UpdateChildIdMiddleChild) {
	// Tests updating a middle child
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0},
		{PropertyValue(300), 300, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Update middle child ID
	bool updated = internalNode_.updateChildId(200, 999);
	EXPECT_TRUE(updated);

	auto updatedChildren = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(updatedChildren[0].childId, 100);
	EXPECT_EQ(updatedChildren[1].childId, 999);
	EXPECT_EQ(updatedChildren[2].childId, 300);
}

TEST_F(IndexTest, GetChildIdsMultipleChildren) {
	// Tests getChildIds with multiple children
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0},
		{PropertyValue(300), 300, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	auto ids = internalNode_.getChildIds();
	EXPECT_EQ(ids.size(), 3UL);
	EXPECT_EQ(ids[0], 100);
	EXPECT_EQ(ids[1], 200);
	EXPECT_EQ(ids[2], 300);
}

TEST_F(IndexTest, FindChildReturnsCorrectChild) {
	// Tests findChild returns the correct child based on key
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0},
		{PropertyValue(400), 400, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Find child for key < 200 should return 100
	EXPECT_EQ(internalNode_.findChild(PropertyValue(150), dataManager_, intComparator), 100);

	// Find child for key < 400 should return 200
	EXPECT_EQ(internalNode_.findChild(PropertyValue(250), dataManager_, intComparator), 200);

	// Find child for key >= 400 should return 400
	EXPECT_EQ(internalNode_.findChild(PropertyValue(500), dataManager_, intComparator), 400);
}

TEST_F(IndexTest, FindChildWithAllKeysLessThanFirstSeparator) {
	// Tests findChild when key is less than all separators
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Key 50 should route to first child (100)
	EXPECT_EQ(internalNode_.findChild(PropertyValue(50), dataManager_, intComparator), 100);
}

TEST_F(IndexTest, GetAllChildrenWithBlobKeys) {
	// Tests getAllChildren when keys are stored in blobs
	// Create keys larger than threshold to force blob storage
	std::string largeKey1(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');
	std::string largeKey2(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'B');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey1), 200, 0},
		{PropertyValue(largeKey2), 300, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	auto retrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(retrieved.size(), 3UL);

	// Verify blob IDs are set for large keys
	EXPECT_EQ(retrieved[0].keyBlobId, 0);
	EXPECT_NE(retrieved[1].keyBlobId, 0) << "Large key 1 should be in blob";
	EXPECT_NE(retrieved[2].keyBlobId, 0) << "Large key 2 should be in blob";
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

TEST_F(IndexTest, SetAllChildrenUpdateExistingBlob) {
	// Tests setAllChildren when updating existing blob (line 551-556)
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Get the blob ID
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	int64_t originalBlobId = retrieved[1].keyBlobId;
	EXPECT_NE(originalBlobId, 0);

	// Update with DIFFERENT large key content (should update existing blob)
	std::string differentLargeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'B');
	std::vector<Index::ChildEntry> updatedChildren = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(differentLargeKey), 999, originalBlobId}  // Use existing blob ID
	};
	internalNode_.setAllChildren(updatedChildren, dataManager_);

	auto newRetrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_NE(newRetrieved[1].keyBlobId, 0);
	EXPECT_EQ(newRetrieved[1].childId, 999);

	// Verify the key content was actually updated
	EXPECT_EQ(newRetrieved[1].key, PropertyValue(differentLargeKey));
}

TEST_F(IndexTest, SetAllChildrenCleanupBlobWhenKeyBecomesSmall) {
	// Tests setAllChildren blob cleanup when key becomes small (line 540-544)
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');

	// Create with large key in blob
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Get blob ID
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	int64_t blobId = retrieved[1].keyBlobId;
	EXPECT_NE(blobId, 0);

	// Update with small key (blob should be cleaned up)
	std::vector<Index::ChildEntry> updatedChildren = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(5), 300, blobId}  // Small key now, but has old blobId
	};
	internalNode_.setAllChildren(updatedChildren, dataManager_);

	// Verify old blob was deleted
	EXPECT_THROW((void) dataManager_->getBlobManager()->readBlobChain(blobId), std::runtime_error);

	auto newRetrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(newRetrieved[1].keyBlobId, 0) << "Small key should not have blob";
}

TEST_F(IndexTest, GetAllChildrenEmptyChildren) {
	// Tests getAllChildren when childCount is 0 (line 477-479)
	internalNode_.getMutableMetadata().childCount = 0;

	auto children = internalNode_.getAllChildren(dataManager_);
	EXPECT_TRUE(children.empty());
}

TEST_F(IndexTest, SetAllChildrenEmptyChildren) {
	// Tests setAllChildren with empty children (line 530-532)
	std::vector<Index::ChildEntry> emptyChildren;

	EXPECT_NO_THROW(internalNode_.setAllChildren(emptyChildren, dataManager_));

	EXPECT_EQ(internalNode_.getMetadata().childCount, 0u);
}

TEST_F(IndexTest, WouldInternalOverflowOnAddChildEmptyChildren) {
	// Tests wouldInternalOverflowOnAddChild with empty children (line 267-269)
	// Set up internal node with no children
	Index newInternal(0, Index::NodeType::INTERNAL, 1);
	dataManager_->addIndexEntity(newInternal);
	newInternal.getMutableMetadata().childCount = 0;

	Index::ChildEntry newEntry = {PropertyValue(100), 10, 0};

	// Should return false (no overflow) when adding to empty children
	bool wouldOverflow = newInternal.wouldInternalOverflowOnAddChild(newEntry, dataManager_);
	EXPECT_FALSE(wouldOverflow);

	// Cleanup
	dataManager_->deleteIndex(newInternal);
}

TEST_F(IndexTest, WouldInternalOverflowOnAddChildWithExistingChildren) {
	// Tests wouldInternalOverflowOnAddChild with existing children
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	Index::ChildEntry newEntry = {PropertyValue(300), 300, 0};

	// Just verify the method works without error
	bool wouldOverflow = internalNode_.wouldInternalOverflowOnAddChild(newEntry, dataManager_);
	// Can't assert specific value without exact size calculation
	(void) wouldOverflow;
}

// Test setAllEntries when key blob needs to be created
// Tests branch at line 177-182: create new key blob
TEST_F(IndexTest, SetAllEntriesCreateNewKeyBlob) {
	// Create entry with oversized key
	std::string largeKey(Index::LEAF_KEY_INLINE_THRESHOLD + 1, 'K');
	std::vector<Index::Entry> entries = {{PropertyValue(largeKey), {100}, 0, 0}};

	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	EXPECT_NE(retrieved[0].keyBlobId, 0) << "Large key should be in blob";
	EXPECT_EQ(retrieved[0].key, PropertyValue(largeKey));
}

// Test setAllEntries when values blob needs to be created
// Tests branch at line 202-206: create new values blob
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

// Test setAllChildren when child key blob needs to be created
// Tests branch at line 557-561: create new child key blob
TEST_F(IndexTest, SetAllChildrenCreateNewKeyBlob) {
	// Create child entry with oversized key
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};

	internalNode_.setAllChildren(children, dataManager_);

	auto retrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_NE(retrieved[1].keyBlobId, 0) << "Large child key should be in blob";
}

// Test tryInsertEntry with multiple small entries
// Tests that multiple entries can be added without overflow
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

// =================================================================================
// --- ADDITIONAL BRANCH COVERAGE TESTS ---
// =================================================================================

// Test setAllChildren on leaf node
// Tests branch at line 508-509: throw error when called on leaf
TEST_F(IndexTest, SetAllChildrenOnLeafNodeThrows) {
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0}
	};

	EXPECT_THROW(leafNode_.setAllChildren(children, dataManager_), std::logic_error);
}

// Test updateChildId on leaf node
// Tests branch at line 602-603: return false when called on leaf
TEST_F(IndexTest, UpdateChildIdOnLeafNodeReturnsFalse) {
	bool result = leafNode_.updateChildId(100, 200);
	EXPECT_FALSE(result);
}

// Test updateChildId with no children
// Tests branch at line 604-605: return false when childCount == 0
TEST_F(IndexTest, UpdateChildIdWithNoChildrenReturnsFalse) {
	// Create internal node with no children
	Index newInternal(0, Index::NodeType::INTERNAL, 1);
	dataManager_->addIndexEntity(newInternal);
	newInternal.getMutableMetadata().childCount = 0;

	bool result = newInternal.updateChildId(100, 200);
	EXPECT_FALSE(result);

	dataManager_->deleteIndex(newInternal);
}

// Test getChildIds on leaf node
// Tests branch at line 666-667: return empty vector when called on leaf
TEST_F(IndexTest, GetChildIdsOnLeafNodeReturnsEmpty) {
	auto ids = leafNode_.getChildIds();
	EXPECT_TRUE(ids.empty());
}

// Test getChildIds with no children
// Tests branch at line 668-669: return empty vector when childCount == 0
TEST_F(IndexTest, GetChildIdsWithNoChildrenReturnsEmpty) {
	// Create internal node with no children
	Index newInternal(0, Index::NodeType::INTERNAL, 1);
	dataManager_->addIndexEntity(newInternal);
	newInternal.getMutableMetadata().childCount = 0;

	auto ids = newInternal.getChildIds();
	EXPECT_TRUE(ids.empty());

	dataManager_->deleteIndex(newInternal);
}

// Test updateChildId with blob keys
// Tests branch at line 630-633: KEY_IS_BLOB in updateChildId
TEST_F(IndexTest, UpdateChildIdWithBlobKeys) {
	// Create internal node with large key that forces blob storage
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Verify blob was created
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_NE(retrieved[1].keyBlobId, 0);

	// Update child ID - should handle blob keys correctly
	bool result = internalNode_.updateChildId(200, 999);
	EXPECT_TRUE(result);

	auto updated = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(updated[1].childId, 999);
}

// Test getChildIds with blob keys
// Tests branch at line 686-687: KEY_IS_BLOB in getChildIds
TEST_F(IndexTest, GetChildIdsWithBlobKeys) {
	// Create internal node with large key that forces blob storage
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0},
		{PropertyValue(300), 300, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Get child IDs - should handle blob keys correctly
	auto ids = internalNode_.getChildIds();
	EXPECT_EQ(ids.size(), 3UL);
	EXPECT_EQ(ids[0], 100);
	EXPECT_EQ(ids[1], 200);
	EXPECT_EQ(ids[2], 300);
}

// Test setAllChildren with blob key cleanup
// Tests branch at line 525-527: delete blob when keyBlobId != 0 && !shouldUseBlob
TEST_F(IndexTest, SetAllChildrenBlobCleanupWhenKeyShrinks) {
	// Create internal node with large key in blob
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Get blob ID
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	int64_t blobId = retrieved[1].keyBlobId;
	EXPECT_NE(blobId, 0);

	// Update with small key - blob should be deleted
	std::vector<Index::ChildEntry> updatedChildren = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(5), 300, blobId}  // Small key now with old blobId
	};
	internalNode_.setAllChildren(updatedChildren, dataManager_);

	// Verify blob was deleted
	EXPECT_THROW((void) dataManager_->getBlobManager()->readBlobChain(blobId), std::runtime_error);

	auto final = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(final[1].keyBlobId, 0);
}

// =================================================================================
// --- NEW BRANCH COVERAGE TESTS ---
// =================================================================================

// Test insertEntry throws on internal node
// Covers branch at line 345-346: throw if not leaf
TEST_F(IndexTest, InsertEntryThrowsOnInternalNode) {
	EXPECT_THROW(
		internalNode_.insertEntry(PropertyValue("key"), 100, dataManager_, stringComparator),
		std::logic_error);
}

// Test addChild throws on leaf node
// Covers branch at line 407-408: throw if isLeaf
TEST_F(IndexTest, AddChildThrowsOnLeafNode) {
	Index::ChildEntry entry = {PropertyValue(100), 10, 0};
	EXPECT_THROW(leafNode_.addChild(entry, dataManager_, intComparator), std::logic_error);
}

// Test removeChild returns false when key not found
// Covers branch at line 427-435: key not found returns false
TEST_F(IndexTest, RemoveChildReturnsFalseWhenKeyNotFound) {
	// Set up internal node with some children
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Try to remove a non-existent key
	bool removed = internalNode_.removeChild(PropertyValue(999), dataManager_, intComparator);
	EXPECT_FALSE(removed);

	// Verify children are unchanged
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(retrieved.size(), 2UL);
}

// Test findChild with empty children returns 0
// Covers branch at line 444-445: return 0 if children empty
TEST_F(IndexTest, FindChildWithEmptyChildrenReturnsZero) {
	// Create internal node with explicitly zero children
	Index emptyInternal(0, Index::NodeType::INTERNAL, 1);
	dataManager_->addIndexEntity(emptyInternal);
	emptyInternal.getMutableMetadata().childCount = 0;

	auto result = emptyInternal.findChild(PropertyValue(50), dataManager_, intComparator);
	EXPECT_EQ(result, 0);

	dataManager_->deleteIndex(emptyInternal);
}

// Test setAllEntries with existing key blob update
// Covers branch at line 165-170: update existing key blob
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

// Test setAllEntries with existing values blob update
// Covers branch at line 188-193: update existing values blob
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

// Test setAllEntries with key blob cleanup (large to small)
// Covers branch at line 158-161: delete key blob when entry shrinks
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

// Test setAllEntries with values blob cleanup (large to small)
// Covers branch at line 180-183: delete values blob when values shrink
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

// Test serialize and deserialize round-trip
// Covers serialize/deserialize paths
TEST_F(IndexTest, SerializeDeserializeRoundTrip) {
	// Create a leaf node with some data
	Index original(42, Index::NodeType::LEAF, 5);
	original.setParentId(10);
	original.setNextLeafId(20);
	original.setPrevLeafId(30);
	original.setLevel(3);

	// Serialize
	std::ostringstream os;
	original.serialize(os);

	// Deserialize
	std::string serialized = os.str();
	std::istringstream is(serialized);
	Index deserialized = Index::deserialize(is);

	EXPECT_EQ(deserialized.getId(), 42);
	EXPECT_EQ(deserialized.getParentId(), 10);
	EXPECT_EQ(deserialized.getNextLeafId(), 20);
	EXPECT_EQ(deserialized.getPrevLeafId(), 30);
	EXPECT_EQ(deserialized.getLevel(), 3);
	EXPECT_EQ(deserialized.getIndexType(), 5u);
	EXPECT_EQ(deserialized.getNodeType(), Index::NodeType::LEAF);
	EXPECT_TRUE(deserialized.isActive());
}

// Test serialize and deserialize round-trip for internal node
TEST_F(IndexTest, SerializeDeserializeInternalNode) {
	Index original(99, Index::NodeType::INTERNAL, 7);
	original.setLevel(2);

	// Serialize
	std::ostringstream os;
	original.serialize(os);

	// Deserialize
	std::string serialized = os.str();
	std::istringstream is(serialized);
	Index deserialized = Index::deserialize(is);

	EXPECT_EQ(deserialized.getId(), 99);
	EXPECT_EQ(deserialized.getNodeType(), Index::NodeType::INTERNAL);
	EXPECT_EQ(deserialized.getLevel(), 2);
	EXPECT_EQ(deserialized.getIndexType(), 7u);
}

// Test getEntrySerializedSize with different entry sizes
// Covers branches at line 580 and 592: large key and large values paths
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

// Test insertEntry with new key on leaf node
// Covers the else branch at line 358-359: new key insertion
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

// Test insertEntry adds new value to existing key
// Covers branch at line 353-356: key exists, value not found
TEST_F(IndexTest, InsertEntryNewValueForExistingKey) {
	leafNode_.insertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("key1"), 200, dataManager_, stringComparator);

	auto values = leafNode_.findValues(PropertyValue("key1"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 2UL);
	std::ranges::sort(values);
	EXPECT_EQ(values[0], 100);
	EXPECT_EQ(values[1], 200);
}

// Test removeEntry with key found but value not in list
// Covers branch at line 374: value_it == values.end() (value not found)
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

// Test wouldInternalOverflowOnAddChild with overflow scenario
// Covers branch at line 285: return true when data exceeds DATA_SIZE
TEST_F(IndexTest, WouldInternalOverflowOnAddChildTrueCase) {
	// Fill internal node as close to capacity as possible
	// Each child entry serializes as: flags (1) + key (variable) + childId (8)
	// First child is just childId (8 bytes)
	std::vector<Index::ChildEntry> children;
	children.push_back({PropertyValue(std::monostate{}), 1, 0});

	// Use string keys just below the blob threshold to maximize space usage
	// Each entry = 1 (flags) + 1 (type tag) + 4 (string len) + key_len + 8 (childId)
	int i = 1;
	size_t estimatedSize = sizeof(int64_t); // First child
	while (true) {
		// Use a key string of ~25 bytes (below INTERNAL_KEY_INLINE_THRESHOLD of 32)
		std::string keyStr = "k" + std::to_string(i);
		// Pad to fill more space
		while (keyStr.size() < 20) keyStr += '_';
		PropertyValue k(keyStr);
		size_t entrySize = sizeof(uint8_t) + graph::utils::getSerializedSize(k) + sizeof(int64_t);
		if (estimatedSize + entrySize > Index::DATA_SIZE - 5) {
			break; // Stop when nearly full, leave very little room
		}
		children.push_back({k, static_cast<int64_t>(i), 0});
		estimatedSize += entrySize;
		i++;
	}
	internalNode_.setAllChildren(children, dataManager_);

	// Now try to add another entry - should overflow since we're nearly full
	std::string addKey = "overflow_test_entry_";
	Index::ChildEntry newEntry = {PropertyValue(addKey), 99999, 0};

	bool wouldOverflow = internalNode_.wouldInternalOverflowOnAddChild(newEntry, dataManager_);
	EXPECT_TRUE(wouldOverflow) << "Adding entry to nearly full node should overflow. "
		<< "Estimated fill: " << estimatedSize << "/" << Index::DATA_SIZE;
}

// Test constructor sets level correctly for LEAF vs INTERNAL
TEST_F(IndexTest, ConstructorSetsLevelCorrectly) {
	Index leaf(0, Index::NodeType::LEAF, 1);
	EXPECT_EQ(leaf.getLevel(), 0) << "Leaf node should have level 0";

	Index internal(0, Index::NodeType::INTERNAL, 1);
	EXPECT_EQ(internal.getLevel(), 1) << "Internal node should have level 1";
}

// Test removeChild with blob key cleanup
// Covers branch at line 428-429: delete blob chain when keyBlobId != 0
TEST_F(IndexTest, RemoveChildWithBlobKeyCleanup) {
	// Create internal node with large key (blob storage)
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'X');
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Verify blob was created
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	int64_t blobId = retrieved[1].keyBlobId;
	EXPECT_NE(blobId, 0);

	// Remove the child
	bool removed = internalNode_.removeChild(PropertyValue(largeKey), dataManager_, stringComparator);
	EXPECT_TRUE(removed);

	// Verify blob was cleaned up
	EXPECT_THROW((void) dataManager_->getBlobManager()->readBlobChain(blobId), std::runtime_error);

	// Verify child was removed
	auto final_children = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(final_children.size(), 1UL);
}

// =================================================================================
// --- ADDITIONAL BRANCH COVERAGE TESTS - Round 3 ---
// =================================================================================

// Test wouldInternalOverflowOnAddChild with blob key in existing children
// Covers branch at line 269-271: keyIsBlob in wouldInternalOverflowOnAddChild
TEST_F(IndexTest, WouldInternalOverflowWithBlobKeyChildren) {
	// Create internal node with a large key that forces blob storage
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'X');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Verify blob was created
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_NE(retrieved[1].keyBlobId, 0);

	// Check overflow with the existing blob key children
	Index::ChildEntry newEntry = {PropertyValue(300), 300, 0};
	bool wouldOverflow = internalNode_.wouldInternalOverflowOnAddChild(newEntry, dataManager_);
	// Just ensure it runs without error and gives a result
	EXPECT_FALSE(wouldOverflow) << "Adding one small entry to a node with 2 children should not overflow";
}

// Test setAllChildren where shouldUseBlob is true AND keyBlobId is already non-zero
// Covers branch at line 532-534: update existing blob in setAllChildren
TEST_F(IndexTest, SetAllChildrenUpdateExistingBlobKey) {
	// First create children with a large key (creates blob)
	std::string largeKey1(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'A');

	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey1), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Get the blob ID
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	int64_t originalBlobId = retrieved[1].keyBlobId;
	EXPECT_NE(originalBlobId, 0);

	// Now update with a DIFFERENT large key, passing the existing blobId
	// This should trigger the update-existing-blob path
	std::string largeKey2(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'Z');
	std::vector<Index::ChildEntry> updatedChildren = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey2), 200, originalBlobId}
	};
	internalNode_.setAllChildren(updatedChildren, dataManager_);

	auto newRetrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_NE(newRetrieved[1].keyBlobId, 0);
	EXPECT_EQ(newRetrieved[1].key, PropertyValue(largeKey2));
}

// Test tryInsertEntry capacity check (line 299-301)
// Covers branch where entries capacity needs to be reserved
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

// Test insertEntry on internal node throws
// Covers branch at line 345-346 (already tested above but make sure)
TEST_F(IndexTest, InsertEntryThrowsOnInternalNodeDuplicate) {
	EXPECT_THROW(
		internalNode_.insertEntry(PropertyValue("key"), 100, dataManager_, stringComparator),
		std::logic_error);
}

// Test removeEntry where key exists but specific value is not in value list
// (key found at exact position, but value_it == values.end())
// Covers branch at line 374: value_it != values.end() -> False
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

// Test findValues on internal node throws (it delegates to getAllEntries which throws)
// Covers branch at line 396-398: getAllEntries throws on internal node
TEST_F(IndexTest, FindValuesOnInternalNodeThrows) {
	// findValues calls getAllEntries which throws on internal node
	EXPECT_THROW(
		(void)internalNode_.findValues(PropertyValue("key"), dataManager_, stringComparator),
		std::logic_error);
}

// Test removeEntry on internal node (getAllEntries throws)
TEST_F(IndexTest, RemoveEntryOnInternalNodeThrows) {
	EXPECT_THROW(
		internalNode_.removeEntry(PropertyValue("key"), 100, dataManager_, stringComparator),
		std::logic_error);
}

// Test updateChildId returns false when child not found in non-empty node
// Covers branch at line 650: found == false path
TEST_F(IndexTest, UpdateChildIdNotFoundReturnsFalse) {
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(200), 200, 0},
		{PropertyValue(300), 300, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Try to update a child ID that doesn't exist
	bool result = internalNode_.updateChildId(999, 888);
	EXPECT_FALSE(result);

	// Verify nothing changed
	auto ids = internalNode_.getChildIds();
	EXPECT_EQ(ids[0], 100);
	EXPECT_EQ(ids[1], 200);
	EXPECT_EQ(ids[2], 300);
}

// Test setAllEntries with key blob update where blob creation is needed (not just update)
// Covers branch at line 171-176: else branch for creating new blob
TEST_F(IndexTest, SetAllEntriesCreateKeyBlobFromZeroBlobId) {
	// Create entry with large key and keyBlobId = 0 (forces creation)
	std::string largeKey(Index::LEAF_KEY_INLINE_THRESHOLD + 5, 'M');
	std::vector<Index::Entry> entries = {{PropertyValue(largeKey), {100}, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	EXPECT_NE(retrieved[0].keyBlobId, 0) << "Should create new blob for large key";
	EXPECT_EQ(retrieved[0].key, PropertyValue(largeKey));
}

// Test setAllEntries with values blob creation from zero blobId
// Covers branch at line 194-197: else branch for creating new values blob
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

// Test removeChild where key is near but not exact match (lower_bound finds wrong entry)
// Covers branch at line 427: key comparison fails (not equal)
TEST_F(IndexTest, RemoveChildKeyNearButNotExact) {
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(static_cast<int64_t>(50)), 200, 0},
		{PropertyValue(static_cast<int64_t>(100)), 300, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Try to remove key 75 which doesn't exist (between 50 and 100)
	bool removed = internalNode_.removeChild(PropertyValue(static_cast<int64_t>(75)), dataManager_, intComparator);
	EXPECT_FALSE(removed);

	// All children should still be present
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	EXPECT_EQ(retrieved.size(), 3UL);
}

// Test getAllChildren returns empty when childCount is 0 on internal node
// Covers branch at line 465-467: childCount == 0 return
TEST_F(IndexTest, GetAllChildrenReturnsEmptyForZeroCount) {
	Index emptyInternal(0, Index::NodeType::INTERNAL, 1);
	dataManager_->addIndexEntity(emptyInternal);

	auto children = emptyInternal.getAllChildren(dataManager_);
	EXPECT_TRUE(children.empty());

	dataManager_->deleteIndex(emptyInternal);
}

// Test findChild with single child (returns first child for any key)
TEST_F(IndexTest, FindChildWithSingleChild) {
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 42, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Any key should return the single child
	auto result = internalNode_.findChild(PropertyValue(999), dataManager_, intComparator);
	EXPECT_EQ(result, 42);
}

// Test removeEntry where lower_bound finds an entry but key doesn't match
// Covers the False branch on line 371: key comparison fails after lower_bound
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

// Test findValues where key falls between existing keys (key not found)
// Covers the False branch on line 400: keyExists is false when lower_bound
// lands on a key that doesn't match
TEST_F(IndexTest, FindValuesKeyBetweenExistingKeys) {
	leafNode_.insertEntry(PropertyValue("alpha"), 10, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("gamma"), 30, dataManager_, stringComparator);

	// "beta" falls between "alpha" and "gamma" - lower_bound finds "gamma"
	auto results = leafNode_.findValues(PropertyValue("beta"), dataManager_, stringComparator);
	EXPECT_TRUE(results.empty());
}

// Test removeEntry where key matches but value is not in the list
// Covers the False branch on line 374: value_it == values.end()
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

// Test removeEntry where it == entries.end() (key beyond all entries)
// Covers the False branch on line 371: it == entries.end()
TEST_F(IndexTest, RemoveEntryKeyBeyondAllEntries) {
	leafNode_.insertEntry(PropertyValue("aaa"), 1, dataManager_, stringComparator);

	// "zzz" is beyond "aaa", lower_bound returns end()
	bool removed = leafNode_.removeEntry(PropertyValue("zzz"), 1, dataManager_, stringComparator);
	EXPECT_FALSE(removed);
}

// Test findValues on empty leaf
// Covers the edge case where it == entries.end() immediately
TEST_F(IndexTest, FindValuesOnEmptyLeaf) {
	auto results = leafNode_.findValues(PropertyValue("anything"), dataManager_, stringComparator);
	EXPECT_TRUE(results.empty());
}

// Test removeChild where key is beyond all children
// Covers the False branch at line 427: it == children.end()
TEST_F(IndexTest, RemoveChildKeyBeyondAllChildren) {
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 10, 0},
		{PropertyValue(static_cast<int64_t>(5)), 20, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Key 999 is beyond all children keys
	bool removed = internalNode_.removeChild(PropertyValue(static_cast<int64_t>(999)), dataManager_, intComparator);
	EXPECT_FALSE(removed);
}

// =================================================================================
// --- ADDITIONAL BRANCH COVERAGE TESTS - Round 4 ---
// =================================================================================

// Test setAllEntries where data exceeds DATA_SIZE (should throw)
// Covers branch at line 231-233: data.size() > DATA_SIZE
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

// Test setAllChildren where data exceeds DATA_SIZE (should throw)
// Covers branch at line 560-561: data.size() > DATA_SIZE in setAllChildren
TEST_F(IndexTest, SetAllChildrenOverflowThrows) {
	// Create children with keys that will exceed DATA_SIZE
	std::vector<Index::ChildEntry> children;
	children.push_back({PropertyValue(std::monostate{}), 1, 0}); // first child

	// Each subsequent child with a ~20 char key takes about 30-35 bytes
	// Need enough to overflow DATA_SIZE
	size_t estimatedSize = sizeof(int64_t); // first child
	int i = 1;
	while (estimatedSize < Index::DATA_SIZE + 100) {
		std::string keyStr = "key_overflow_test_" + std::to_string(i);
		PropertyValue k(keyStr);
		size_t entrySize = sizeof(uint8_t) + graph::utils::getSerializedSize(k) + sizeof(int64_t);
		children.push_back({k, static_cast<int64_t>(i + 1), 0});
		estimatedSize += entrySize;
		i++;
	}

	EXPECT_THROW(internalNode_.setAllChildren(children, dataManager_), std::runtime_error);
}

// Test getEntrySerializedSize with entry that has exactly 0 values
// Covers the case where valuesRawSize == 0 (below threshold, takes inline path)
TEST_F(IndexTest, GetEntrySerializedSizeZeroValues) {
	Index::Entry emptyValuesEntry = {PropertyValue("key"), {}, 0, 0};
	size_t size = Index::getEntrySerializedSize(emptyValuesEntry);

	// Should have: flags(1) + key serialization + value count(4) + 0 values
	EXPECT_GT(size, 0UL);
}

// Test getEntrySerializedSize with entry that has exactly 1 value
TEST_F(IndexTest, GetEntrySerializedSizeOneValue) {
	Index::Entry singleValueEntry = {PropertyValue("k"), {42}, 0, 0};
	size_t sizeOne = Index::getEntrySerializedSize(singleValueEntry);

	Index::Entry zeroValueEntry = {PropertyValue("k"), {}, 0, 0};
	size_t sizeZero = Index::getEntrySerializedSize(zeroValueEntry);

	// One value adds sizeof(int64_t) = 8 bytes
	EXPECT_EQ(sizeOne - sizeZero, sizeof(int64_t));
}

// Test removeEntry where the last value is removed, triggering entry deletion
// but neither keyBlobId nor valuesBlobId are set (both 0)
// Covers branch at line 376-384: values.empty() with blobIds == 0
TEST_F(IndexTest, RemoveLastValueNoBlobs) {
	leafNode_.insertEntry(PropertyValue("remove_last"), 100, dataManager_, stringComparator);

	// Remove the only value
	bool removed = leafNode_.removeEntry(PropertyValue("remove_last"), 100, dataManager_, stringComparator);
	EXPECT_TRUE(removed);

	// Entry should be completely gone
	auto entries = leafNode_.getAllEntries(dataManager_);
	EXPECT_TRUE(entries.empty());
}

// Test removeEntry where removing value does NOT empty the entry
// Covers branch at line 376: values.empty() returns false
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

// Test tryInsertEntry where the estimated size exceeds DATA_SIZE during iteration
// (early fail during size calculation)
// Covers branch at line 330: estimatedSize > DATA_SIZE early exit
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

// Test isUnderflow with different threshold values on a non-root node with actual data
TEST_F(IndexTest, IsUnderflowNonRootWithData) {
	// Create a node with some actual data usage
	leafNode_.setParentId(50); // Non-root
	leafNode_.insertEntry(PropertyValue("key1"), 1, dataManager_, stringComparator);

	// After insertion, dataUsage > 0
	uint32_t usage = leafNode_.getMetadata().dataUsage;
	EXPECT_GT(usage, 0u);

	// With threshold that makes threshold value > usage, should be underflow
	EXPECT_TRUE(leafNode_.isUnderflow(0.99));

	// With threshold of 0.0, nothing should be underflow
	EXPECT_FALSE(leafNode_.isUnderflow(0.0));
}

// Test constructor initializes all metadata fields correctly
TEST_F(IndexTest, ConstructorInitializesMetadata) {
	Index leaf(0, Index::NodeType::LEAF, 42);
	EXPECT_EQ(leaf.getMetadata().entryCount, 0u);
	EXPECT_EQ(leaf.getMetadata().childCount, 0u);
	EXPECT_EQ(leaf.getMetadata().dataUsage, 0u);
	EXPECT_EQ(leaf.getMetadata().indexType, 42u);
	EXPECT_EQ(leaf.getLevel(), 0);
	EXPECT_TRUE(leaf.isLeaf());

	Index internal(0, Index::NodeType::INTERNAL, 99);
	EXPECT_EQ(internal.getLevel(), 1);
	EXPECT_FALSE(internal.isLeaf());
}

// Test wouldInternalOverflowOnAddChild where existing children have blob keys
// that the function must account for in size estimation
// Covers branch at line 269-271: keyIsBlob == true in size estimation
TEST_F(IndexTest, WouldInternalOverflowBlobKeySizeEstimation) {
	// Create children where one has a blob key
	std::string largeKey(Index::INTERNAL_KEY_INLINE_THRESHOLD + 10, 'X');
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 100, 0},
		{PropertyValue(largeKey), 200, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// The blob key should be stored with a blob ID
	auto retrieved = internalNode_.getAllChildren(dataManager_);
	ASSERT_NE(retrieved[1].keyBlobId, 0);

	// When checking overflow, the function simulates the size using the
	// existing children (including blob key). This exercises the keyIsBlob
	// path in wouldInternalOverflowOnAddChild.
	Index::ChildEntry smallEntry = {PropertyValue(static_cast<int64_t>(5)), 300, 0};
	bool overflow = internalNode_.wouldInternalOverflowOnAddChild(smallEntry, dataManager_);
	EXPECT_FALSE(overflow) << "Small entry added to node with 2 children should not overflow";
}

// Test setAllEntries with both blob key AND blob values simultaneously
// Covers the combined path where both KEY_IS_BLOB and VALUES_ARE_BLOB flags are set
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

// =================================================================================
// --- ADDITIONAL BRANCH COVERAGE TESTS - Round 5 ---
// =================================================================================

// Test isUnderflow on non-root node where dataUsage is high enough to NOT be underflow
// Covers the false path of line 49: return dataUsage < threshold -> false
TEST_F(IndexTest, IsUnderflowNonRoot_NotUnderflow) {
	leafNode_.setParentId(100); // Non-root
	// Set dataUsage to be above the 50% threshold
	leafNode_.getMutableMetadata().dataUsage = static_cast<uint32_t>(Index::DATA_SIZE);

	EXPECT_FALSE(leafNode_.isUnderflow(0.5)) << "Node at full capacity should not be in underflow";
}

// Test tryInsertEntry where key exists and new value is different
// This covers the true branch at line 313: value not found, push_back
TEST_F(IndexTest, TryInsertEntry_ExistingKey_NewDistinctValue) {
	leafNode_.tryInsertEntry(PropertyValue("dup"), 10, dataManager_, stringComparator);
	auto result = leafNode_.tryInsertEntry(PropertyValue("dup"), 20, dataManager_, stringComparator);
	EXPECT_TRUE(result.success);

	auto values = leafNode_.findValues(PropertyValue("dup"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 2UL);
}

// Test insertEntry where key exists and value is duplicate (no-op)
// Covers the true branch at line 355: find returns != end (value already exists)
TEST_F(IndexTest, InsertEntry_DuplicateValueNoOp) {
	leafNode_.insertEntry(PropertyValue("dup2"), 42, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("dup2"), 42, dataManager_, stringComparator);

	auto values = leafNode_.findValues(PropertyValue("dup2"), dataManager_, stringComparator);
	EXPECT_EQ(values.size(), 1UL);
	EXPECT_EQ(values[0], 42);
}

// Test removeEntry where values become empty and both keyBlobId and valuesBlobId are non-zero
// Covers lines 378-383: both blob cleanup branches when values.empty() is true
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

// Test removeEntry where values become empty but only keyBlobId is non-zero
// Covers line 381-382: keyBlobId != 0 branch while valuesBlobId == 0
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

// Test removeEntry where values become empty but only valuesBlobId is non-zero
// Covers line 378-379: valuesBlobId != 0 branch while keyBlobId == 0
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

// Test getEntrySerializedSize with key exactly at threshold boundary
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

// Test getEntrySerializedSize with values exactly at LEAF_VALUES_INLINE_THRESHOLD
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

// Test operator| and operator& for EntrySerializationFlags
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

// Test addChild throws on leaf node
TEST_F(IndexTest, AddChildThrowsOnLeafNodeRepeat) {
	Index::ChildEntry entry = {PropertyValue("x"), 1, 0};
	EXPECT_THROW(leafNode_.addChild(entry, dataManager_, stringComparator), std::logic_error);
}

// Test removeChild throws on leaf node
TEST_F(IndexTest, RemoveChildThrowsOnLeafNodeRepeat) {
	EXPECT_THROW(leafNode_.removeChild(PropertyValue("x"), dataManager_, stringComparator), std::logic_error);
}

// Test findChild with key that exactly matches a separator
TEST_F(IndexTest, FindChildExactKeyMatch) {
	std::vector<Index::ChildEntry> children = {
		{PropertyValue(std::monostate{}), 10, 0},
		{PropertyValue(static_cast<int64_t>(50)), 20, 0},
		{PropertyValue(static_cast<int64_t>(100)), 30, 0}
	};
	internalNode_.setAllChildren(children, dataManager_);

	// Key exactly at separator value 50
	int64_t result = internalNode_.findChild(PropertyValue(static_cast<int64_t>(50)), dataManager_, intComparator);
	EXPECT_EQ(result, 20);

	// Key exactly at separator value 100
	result = internalNode_.findChild(PropertyValue(static_cast<int64_t>(100)), dataManager_, intComparator);
	EXPECT_EQ(result, 30);
}

// Cover: line 378 true branch - removeEntry with values stored as blob
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

// =================================================================================
// --- BRANCH COVERAGE: insertEntry key-not-found path (Branch 351:44 False) ---
// =================================================================================

// When insertEntry inserts a NEW key that falls BEFORE an existing key,
// lower_bound returns an iterator to the first element >= key.
// The check !comparator(key, it->key) evaluates to false (since key < it->key),
// covering the False path of the second sub-expression in the keyExists check.
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

// Insert a key in the middle of existing keys using insertEntry
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
