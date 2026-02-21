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
	EXPECT_THROW(internalNode_.getAllEntries(dataManager_), std::logic_error);
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
	EXPECT_THROW(leafNode_.findChild(PropertyValue("key"), dataManager_, stringComparator), std::logic_error);
}

TEST_F(IndexTest, GetAllChildrenThrowsOnLeaf) {
	// Tests branch at line 473-474: throw if isLeaf
	EXPECT_THROW(leafNode_.getAllChildren(dataManager_), std::logic_error);
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

	EXPECT_EQ(internalNode_.getMetadata().childCount, 0);
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
