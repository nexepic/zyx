/**
 * @file test_Index.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/13
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
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
		// This is now less critical since each test gets a fresh node, but good practice.
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
// --- LEAF NODE TESTS ---
// =================================================================================

TEST_F(IndexTest, InsertAndFindEntry) {
	leafNode_.insertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("key2"), 200, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("key1"), 101, dataManager_, stringComparator);

	auto values = leafNode_.findValues(PropertyValue("key1"), dataManager_, stringComparator);
	ASSERT_EQ(values.size(), 2);
	// Use gtest's container assertions for better output
	ASSERT_THAT(values, ::testing::UnorderedElementsAre(100, 101));

	values = leafNode_.findValues(PropertyValue("key2"), dataManager_, stringComparator);
	ASSERT_EQ(values.size(), 1);
	ASSERT_EQ(values[0], 200);

	auto allEntries = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(allEntries.size(), 2);
}

TEST_F(IndexTest, RemoveEntry) {
	leafNode_.insertEntry(PropertyValue("key1"), 100, dataManager_, stringComparator);
	leafNode_.insertEntry(PropertyValue("key1"), 101, dataManager_, stringComparator);

	// Remove one value from a key with multiple values
	ASSERT_TRUE(leafNode_.removeEntry(PropertyValue("key1"), 100, dataManager_, stringComparator));
	auto values = leafNode_.findValues(PropertyValue("key1"), dataManager_, stringComparator);
	ASSERT_EQ(values.size(), 1);
	ASSERT_EQ(values[0], 101);

	// Attempt to remove a value that is already gone
	ASSERT_FALSE(leafNode_.removeEntry(PropertyValue("key1"), 100, dataManager_, stringComparator));

	// Remove the last value for a key, which should remove the entire entry
	ASSERT_TRUE(leafNode_.removeEntry(PropertyValue("key1"), 101, dataManager_, stringComparator));
	values = leafNode_.findValues(PropertyValue("key1"), dataManager_, stringComparator);
	ASSERT_TRUE(values.empty());
	ASSERT_TRUE(leafNode_.getAllEntries(dataManager_).empty()) << "The entire entry should be gone.";
}

TEST_F(IndexTest, EmptyNodeOperations) {
	auto values = leafNode_.findValues(PropertyValue("nonexistent"), dataManager_, stringComparator);
	ASSERT_TRUE(values.empty());
	ASSERT_FALSE(leafNode_.removeEntry(PropertyValue("nonexistent"), 123, dataManager_, stringComparator));
	ASSERT_TRUE(leafNode_.getAllEntries(dataManager_).empty());
}

// --- NEW TEST: Verifies the full lifecycle of leaf key blobs ---
TEST_F(IndexTest, LeafKeyBlobManagement) {
	std::string oversized_key_str(Index::LEAF_KEY_INLINE_THRESHOLD + 1, 'K');

	// 1. Create with oversized key -> Should create a blob
	std::vector<Index::Entry> entries = {{PropertyValue(oversized_key_str), {1}, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(retrieved.size(), 1);
	ASSERT_NE(retrieved[0].keyBlobId, 0) << "Oversized key should be stored in a blob.";
	ASSERT_EQ(retrieved[0].key, PropertyValue(oversized_key_str));

	int64_t old_blob_id = retrieved[0].keyBlobId;

	// 2. Update with a small key -> Should delete the old blob and go inline
	std::vector<Index::Entry> updated_entries = {{PropertyValue("small_key"), {1}, old_blob_id, 0}};
	leafNode_.setAllEntries(updated_entries, dataManager_);

	retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(retrieved.size(), 1);
	ASSERT_EQ(retrieved[0].keyBlobId, 0) << "Small key should be stored inline.";
	ASSERT_EQ(retrieved[0].key, PropertyValue("small_key"));

	// Verify old blob was deleted
	ASSERT_THROW((void) dataManager_->getBlobManager()->readBlobChain(old_blob_id), std::runtime_error);
}

// --- REVISED TEST: Verifies the full lifecycle of leaf value blobs ---
TEST_F(IndexTest, LeafValuesBlobManagement) {
	std::vector<int64_t> oversized_values;
	size_t value_count = (Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t)) + 1;
	for (size_t i = 0; i < value_count; ++i) {
		oversized_values.push_back(static_cast<int64_t>(i));
	}

	// 1. Create with oversized values -> Should create a blob
	std::vector<Index::Entry> entries = {{PropertyValue("key"), oversized_values, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(retrieved.size(), 1);
	ASSERT_NE(retrieved[0].valuesBlobId, 0) << "Value list should be stored in a blob.";
	ASSERT_EQ(retrieved[0].values.size(), value_count);

	int64_t old_blob_id = retrieved[0].valuesBlobId;

	// 2. Update with small value list -> Should delete old blob and go inline
	std::vector<Index::Entry> small_entry_list = {{PropertyValue("key"), {999}, 0, old_blob_id}};
	leafNode_.setAllEntries(small_entry_list, dataManager_);

	retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(retrieved.size(), 1);
	ASSERT_EQ(retrieved[0].valuesBlobId, 0) << "Value list should revert to inline storage.";
	ASSERT_EQ(retrieved[0].values[0], 999);

	// Verify old blob was deleted
	ASSERT_THROW((void) dataManager_->getBlobManager()->readBlobChain(old_blob_id), std::runtime_error);
}

// =================================================================================
// --- INTERNAL NODE TESTS ---
// =================================================================================

TEST_F(IndexTest, AddAndRemoveChild) {
	// Start with the mandatory first child (implicit key)
	std::vector<Index::ChildEntry> children = {{PropertyValue(std::monostate{}), 5, 0}};
	internalNode_.setAllChildren(children, dataManager_);

	// Add children out of order
	Index::ChildEntry entry1 = {PropertyValue(200), 20, 0};
	Index::ChildEntry entry2 = {PropertyValue(100), 10, 0};
	Index::ChildEntry entry3 = {PropertyValue(300), 30, 0};
	internalNode_.addChild(entry1, dataManager_, intComparator);
	internalNode_.addChild(entry2, dataManager_, intComparator);
	internalNode_.addChild(entry3, dataManager_, intComparator);

	auto final_children = internalNode_.getAllChildren(dataManager_);
	ASSERT_EQ(final_children.size(), 4);
	// Check sorting
	ASSERT_EQ(final_children[1].key, PropertyValue(100));
	ASSERT_EQ(final_children[2].key, PropertyValue(200));
	ASSERT_EQ(final_children[3].key, PropertyValue(300));

	// Remove a middle child
	ASSERT_TRUE(internalNode_.removeChild(PropertyValue(200), dataManager_, intComparator));
	children = internalNode_.getAllChildren(dataManager_);
	ASSERT_EQ(children.size(), 3);
	ASSERT_EQ(children[1].key, PropertyValue(100));
	ASSERT_EQ(children[2].key, PropertyValue(300));

	// Attempt to remove non-existent child
	ASSERT_FALSE(internalNode_.removeChild(PropertyValue(999), dataManager_, intComparator));
}

TEST_F(IndexTest, FindChild) {
	std::vector<Index::ChildEntry> children = {
			{PropertyValue(std::monostate{}), 5, 0}, {PropertyValue(100), 10, 0}, {PropertyValue(200), 20, 0}};
	internalNode_.setAllChildren(children, dataManager_);

	// Test keys that fall between separators
	ASSERT_EQ(internalNode_.findChild(PropertyValue(50), dataManager_, intComparator), 5); // Before first key
	ASSERT_EQ(internalNode_.findChild(PropertyValue(150), dataManager_, intComparator), 10); // Between 100 and 200
	ASSERT_EQ(internalNode_.findChild(PropertyValue(250), dataManager_, intComparator), 20); // After last key
	// Test keys that exactly match separators
	ASSERT_EQ(internalNode_.findChild(PropertyValue(100), dataManager_, intComparator), 10);
	ASSERT_EQ(internalNode_.findChild(PropertyValue(200), dataManager_, intComparator), 20);
}

// --- REVISED TEST: Verifies the full lifecycle of internal key blobs ---
TEST_F(IndexTest, InternalKeyBlobManagement) {
	std::string oversized_key_str(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'X');
	Index::ChildEntry oversized_entry = {PropertyValue(oversized_key_str), 123, 0};

	// 1. Create with oversized key -> should use blob
	std::vector<Index::ChildEntry> children = {{PropertyValue(std::monostate{}), 1, 0}, oversized_entry};
	internalNode_.setAllChildren(children, dataManager_);

	auto retrieved = internalNode_.getAllChildren(dataManager_);
	ASSERT_EQ(retrieved.size(), 2);
	ASSERT_NE(retrieved[1].keyBlobId, 0) << "Oversized key should be in a blob.";
	ASSERT_EQ(retrieved[1].key, PropertyValue(oversized_key_str));

	// 2. Remove entry with blob key and verify blob is gone
	int64_t blob_id = retrieved[1].keyBlobId;
	ASSERT_TRUE(internalNode_.removeChild(PropertyValue(oversized_key_str), dataManager_, stringComparator));
	ASSERT_THROW((void) dataManager_->getBlobManager()->readBlobChain(blob_id), std::runtime_error);
}

// =================================================================================
// --- GENERAL, SERIALIZATION & FULLNESS TESTS ---
// =================================================================================

TEST_F(IndexTest, BlobGarbageCollectionOnRemove) {
	// 1. Setup an entry with both key and value blobs
	std::string oversized_key(Index::LEAF_KEY_INLINE_THRESHOLD + 1, 'K');
	std::vector<int64_t> oversized_values;
	// Keep the value count small but still enough to trigger a blob, makes the test faster
	size_t value_count = (Index::LEAF_VALUES_INLINE_THRESHOLD / sizeof(int64_t)) + 1;
	for (size_t i = 0; i < value_count; ++i)
		oversized_values.push_back(i);

	std::vector<Index::Entry> entries = {{PropertyValue(oversized_key), oversized_values, 0, 0}};
	leafNode_.setAllEntries(entries, dataManager_);

	auto retrieved = leafNode_.getAllEntries(dataManager_);
	ASSERT_EQ(retrieved.size(), 1);
	ASSERT_NE(retrieved[0].keyBlobId, 0);
	ASSERT_NE(retrieved[0].valuesBlobId, 0);
	int64_t key_blob_id = retrieved[0].keyBlobId;
	int64_t values_blob_id = retrieved[0].valuesBlobId;

	// 2. MODIFICATION: Remove ALL values, one by one.
	// The garbage collection should only happen after the LAST value is removed.
	for (size_t i = 0; i < oversized_values.size(); ++i) {
		bool is_last_element = (i == oversized_values.size() - 1);

		ASSERT_TRUE(leafNode_.removeEntry(PropertyValue(oversized_key), oversized_values[i], dataManager_,
										  stringComparator));

		// Before the last element is removed, the entry list should NOT be empty.
		if (!is_last_element) {
			ASSERT_FALSE(leafNode_.getAllEntries(dataManager_).empty())
					<< "Entry should not be deleted until its value list is empty.";
		}
	}

	// 3. After the loop, the entry should be gone.
	ASSERT_TRUE(leafNode_.getAllEntries(dataManager_).empty())
			<< "The entry should be deleted after its last value was removed.";

	// 4. Verify both blobs are gone from storage
	ASSERT_THROW((void) dataManager_->getBlobManager()->readBlobChain(key_blob_id), std::runtime_error);
	ASSERT_THROW((void) dataManager_->getBlobManager()->readBlobChain(values_blob_id), std::runtime_error);
}

TEST_F(IndexTest, WouldOverflowChecks) {
	// --- Leaf Node Test ---
	// 1. Fill the node almost to capacity
	std::vector<Index::Entry> entries;
	size_t estimatedSize = 0;
	int key_counter = 0;
	while (true) {
		PropertyValue key(std::to_string(key_counter++));
		int64_t value = 1;

		// 1 byte for flags, size of the key, 4 bytes for value count, size of the value.
		// No space is allocated for blob IDs when they are not used.
		size_t entrySize = sizeof(uint8_t) + // Flags byte
						   utils::getSerializedSize(key) + // Inline key
						   sizeof(uint32_t) + // Value count
						   sizeof(value); // Inline value

		if (estimatedSize + entrySize > Index::DATA_SIZE) {
			break;
		}
		entries.push_back({key, {value}, 0, 0});
		estimatedSize += entrySize;
	}

	// This check ensures we're actually testing something. If the loop didn't run,
	// DATA_SIZE might be too small for even one entry.
	ASSERT_FALSE(entries.empty()) << "Node could not fit even a single test entry.";
	leafNode_.setAllEntries(entries, dataManager_);

	// 2. Check that adding one more small entry would cause an overflow
	ASSERT_TRUE(leafNode_.wouldLeafOverflowOnInsert(PropertyValue("one_more"), 1, dataManager_, stringComparator))
			<< "Adding to a nearly full node should trigger overflow.";

	// 3. Check that an empty node does not overflow with a single small entry
	Index empty_leaf(0, Index::NodeType::LEAF, 1);
	dataManager_->addIndexEntity(empty_leaf);
	ASSERT_FALSE(empty_leaf.wouldLeafOverflowOnInsert(PropertyValue("a"), 1, dataManager_, stringComparator))
			<< "Adding a single entry to an empty node should not cause an overflow.";
	dataManager_->deleteIndex(empty_leaf);
}

TEST_F(IndexTest, OperationsWithNullDataManager) {
	// Test leaf node blob operations
	std::vector<int64_t> oversized_values(Index::LEAF_VALUES_INLINE_THRESHOLD + 1, 1);
	std::vector<Index::Entry> entries = {{PropertyValue("key"), oversized_values}};
	ASSERT_THROW(leafNode_.setAllEntries(entries, nullptr), std::runtime_error);

	leafNode_.setAllEntries(entries, dataManager_);
	ASSERT_THROW((void) leafNode_.getAllEntries(nullptr), std::runtime_error);

	// Test internal node blob operations
	std::string oversized_key_str(Index::INTERNAL_KEY_INLINE_THRESHOLD + 1, 'X');
	std::vector<Index::ChildEntry> children = {{PropertyValue(std::monostate{}), 1, 0},
											   {PropertyValue(oversized_key_str), 123, 0}};
	ASSERT_THROW(internalNode_.setAllChildren(children, nullptr), std::runtime_error);

	internalNode_.setAllChildren(children, dataManager_);
	ASSERT_THROW((void) internalNode_.getAllChildren(nullptr), std::runtime_error);
}
