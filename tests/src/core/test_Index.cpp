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
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/core/Index.hpp"
#include "graph/storage/data/BlobManager.hpp"

class IndexTest : public ::testing::Test {
protected:
	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		std::filesystem::path testFilePath = std::filesystem::temp_directory_path() /
											 ("test_db_file_index_" + boost::uuids::to_string(uuid) + ".dat");

		database_ = std::make_unique<graph::Database>(testFilePath.string());
		database_->open();
		dataManager_ = database_->getStorage()->getDataManager();
	}

	static void TearDownTestSuite() {
		if (database_) {
			// This will also trigger DataManager destruction via FileStorage
			database_->close();
			database_.reset();
		}
		dataManager_.reset();
	}

	void SetUp() override {
		// Create fresh, empty nodes for each test.
		leafNode_ = graph::Index(0, graph::Index::NodeType::LEAF, 1);
		internalNode_ = graph::Index(0, graph::Index::NodeType::INTERNAL, 1);
	}

	void TearDown() override {
		// Clean up any blobs created during tests
		if (leafNode_.hasBlobStorage()) {
			dataManager_->getBlobManager()->deleteBlobChain(leafNode_.getBlobId());
		}
		auto children = internalNode_.getAllChildren();
		for (const auto &entry: children) {
			if (entry.keyBlobId != 0) {
				dataManager_->getBlobManager()->deleteBlobChain(entry.keyBlobId);
			}
		}
	}

	graph::Index leafNode_;
	graph::Index internalNode_;

	static std::shared_ptr<graph::storage::DataManager> dataManager_;
	static std::unique_ptr<graph::Database> database_;
};

// Initialize static members
std::shared_ptr<graph::storage::DataManager> IndexTest::dataManager_ = nullptr;
std::unique_ptr<graph::Database> IndexTest::database_ = nullptr;

// --- LEAF NODE TESTS ---

TEST_F(IndexTest, InsertStringKey) {
	leafNode_.insertStringKey("key1", 100, dataManager_);
	leafNode_.insertStringKey("key2", 200, dataManager_);
	leafNode_.insertStringKey("key1", 101, dataManager_); // Add another value to same key

	auto values = leafNode_.findStringValues("key1", dataManager_);
	ASSERT_EQ(values.size(), 2);
	ASSERT_TRUE(std::find(values.begin(), values.end(), 100) != values.end());
	ASSERT_TRUE(std::find(values.begin(), values.end(), 101) != values.end());

	auto allKVs = leafNode_.getAllKeyValues(dataManager_);
	ASSERT_EQ(allKVs.size(), 2); // Should have two distinct keys
}

TEST_F(IndexTest, RemoveStringKey) {
	leafNode_.insertStringKey("key1", 100, dataManager_);
	leafNode_.insertStringKey("key1", 101, dataManager_);

	ASSERT_TRUE(leafNode_.removeStringKey("key1", 100, dataManager_));
	ASSERT_FALSE(leafNode_.removeStringKey("key1", 100, dataManager_)); // Already removed

	auto values = leafNode_.findStringValues("key1", dataManager_);
	ASSERT_EQ(values.size(), 1);
	ASSERT_EQ(values[0], 101);

	ASSERT_TRUE(leafNode_.removeStringKey("key1", 101, dataManager_));
	values = leafNode_.findStringValues("key1", dataManager_);
	ASSERT_TRUE(values.empty());
}

TEST_F(IndexTest, DataOverflowToBlob) {
	std::string base_key = "a_long_key_to_ensure_we_take_up_space_";
	std::vector<graph::Index::KeyValuePair> kvs;
	std::ostringstream temp_os;

	// Fill kvs until the serialized size exceeds DATA_SIZE
	for (int i = 0; temp_os.str().size() <= graph::Index::DATA_SIZE; ++i) {
		std::string key = base_key + std::to_string(i);
		graph::Index::KeyValuePair kvp = {key, {100L + i}};
		kvs.push_back(kvp);
		// Simulate serialization to check size
		graph::utils::Serializer::writeString(temp_os, kvp.key);
		graph::utils::Serializer::writePOD(temp_os, static_cast<uint32_t>(kvp.values.size()));
		graph::utils::Serializer::writePOD(temp_os, kvp.values[0]);
	}

	leafNode_.setAllKeyValues(kvs, dataManager_);

	ASSERT_TRUE(leafNode_.hasBlobStorage()) << "Node should have switched to blob storage.";
	ASSERT_NE(leafNode_.getBlobId(), 0) << "Blob ID should be valid.";

	auto retrievedKVs = leafNode_.getAllKeyValues(dataManager_);
	ASSERT_EQ(retrievedKVs.size(), kvs.size());
	ASSERT_EQ(retrievedKVs[0].key, kvs[0].key);
	ASSERT_EQ(retrievedKVs.back().values[0], kvs.back().values[0]);

	std::vector<graph::Index::KeyValuePair> small_kvs = {{"small_key", {999}}};
	leafNode_.setAllKeyValues(small_kvs, dataManager_);

	ASSERT_FALSE(leafNode_.hasBlobStorage()) << "Node should have reverted to inline storage.";
	ASSERT_EQ(leafNode_.getBlobId(), 0) << "Blob ID should be reset to 0.";

	auto finalKVs = leafNode_.getAllKeyValues(dataManager_);
	ASSERT_EQ(finalKVs.size(), 1);
	ASSERT_EQ(finalKVs[0].key, "small_key");
}


// --- INTERNAL NODE TESTS ---

TEST_F(IndexTest, AddChildSorted) {
	internalNode_.addChild({"keyB", 200, 0}, dataManager_);
	internalNode_.addChild({"keyA", 100, 0}, dataManager_);
	internalNode_.addChild({"keyC", 300, 0}, dataManager_);

	auto children = internalNode_.getAllChildren();
	ASSERT_EQ(children.size(), 3);

	// Verify they are stored in sorted order by key
	ASSERT_EQ(children[0].key, "keyA");
	ASSERT_EQ(children[0].childId, 100);
	ASSERT_EQ(children[1].key, "keyB");
	ASSERT_EQ(children[1].childId, 200);
	ASSERT_EQ(children[2].key, "keyC");
	ASSERT_EQ(children[2].childId, 300);
}

TEST_F(IndexTest, RemoveChild) {
	internalNode_.addChild({"keyA", 100, 0}, dataManager_);
	internalNode_.addChild({"keyB", 200, 0}, dataManager_);
	ASSERT_TRUE(internalNode_.removeChild("keyA", dataManager_));

	auto children = internalNode_.getAllChildren();
	ASSERT_EQ(children.size(), 1);
	ASSERT_EQ(children[0].key, "keyB");
	ASSERT_EQ(children[0].childId, 200);

	ASSERT_FALSE(internalNode_.removeChild("keyC", dataManager_)); // Non-existent key
}

TEST_F(IndexTest, AddChildWithBlobKey) {
	// A key longer than INTERNAL_KEY_BLOB_THRESHOLD (32)
	std::string longKey = "this_is_a_very_long_key_that_must_be_stored_in_a_blob";
	ASSERT_GT(longKey.length(), graph::Index::INTERNAL_KEY_BLOB_THRESHOLD);

	// IndexTreeManager would be responsible for creating the blob. Here we simulate it.
	auto blobChain =
			dataManager_->getBlobManager()->createBlobChain(internalNode_.getId(), graph::Index::typeId, longKey);
	ASSERT_FALSE(blobChain.empty());
	int64_t blobId = blobChain.front().getId();

	internalNode_.addChild({"keyA", 100, 0}, dataManager_);
	internalNode_.addChild({"", blobId, blobId}, dataManager_); // Add blob key

	auto children = internalNode_.getAllChildren();
	ASSERT_EQ(children.size(), 2);

	// Find the entry for the blob key
	auto it = std::find_if(children.begin(), children.end(), [](const auto &entry) { return entry.keyBlobId != 0; });

	ASSERT_NE(it, children.end());
	EXPECT_EQ(it->keyBlobId, blobId);
	EXPECT_TRUE(it->key.empty());

	// Verify findChild can resolve the blob key
	int64_t foundChildId = internalNode_.findChild(longKey, dataManager_);
	EXPECT_EQ(foundChildId, blobId); // We stored blobId as childId for this test
}

// --- GENERAL/STATIC TESTS ---

TEST_F(IndexTest, SerializationDeserialization) {
	leafNode_.insertStringKey("key1", 100, dataManager_);
	leafNode_.insertStringKey("key2", 200, dataManager_);

	std::ostringstream os;
	leafNode_.serialize(os);

	std::istringstream is(os.str());
	auto deserializedNode = graph::Index::deserialize(is);

	auto values1 = deserializedNode.findStringValues("key1", dataManager_);
	ASSERT_EQ(values1.size(), 1);
	ASSERT_EQ(values1[0], 100);

	auto values2 = deserializedNode.findStringValues("key2", dataManager_);
	ASSERT_EQ(values2.size(), 1);
	ASSERT_EQ(values2[0], 200);
}

/**
 * @brief Tests that removing the last value for a key also removes the key itself.
 */
TEST_F(IndexTest, RemoveLastValueRemovesKey) {
	leafNode_.insertStringKey("test_key", 1, dataManager_);
	leafNode_.insertStringKey("another_key", 2, dataManager_);

	// Remove the only value for "test_key"
	ASSERT_TRUE(leafNode_.removeStringKey("test_key", 1, dataManager_));

	// The key itself should now be gone.
	auto allKVs = leafNode_.getAllKeyValues(dataManager_);
	ASSERT_EQ(allKVs.size(), 1);
	ASSERT_EQ(allKVs[0].key, "another_key");

	// Finding the removed key should yield no results.
	ASSERT_TRUE(leafNode_.findStringValues("test_key", dataManager_).empty());
}

/**
 * @brief Tests edge cases for finding a child pointer in an internal node.
 */
TEST_F(IndexTest, FindChildEdgeCases) {
	internalNode_.addChild({"key_100", 100, 0}, dataManager_);
	internalNode_.addChild({"key_200", 200, 0}, dataManager_);

	// The first child has an implicit key of -infinity.
	// We need to add it to simulate a real internal node post-split.
	auto children = internalNode_.getAllChildren();
	children.insert(children.begin(), {"", 50, 0});
	internalNode_.setAllChildren(children);

	// Case 1: Key is smaller than all known keys, should go to the first child.
	ASSERT_EQ(internalNode_.findChild("aaa_before_all", dataManager_), 50);

	// Case 2: Key is between two keys.
	ASSERT_EQ(internalNode_.findChild("key_150", dataManager_), 100);

	// Case 3: Key is larger than all known keys.
	ASSERT_EQ(internalNode_.findChild("zzz_after_all", dataManager_), 200);
}

/**
 * @brief Verifies that methods requiring a DataManager for blob operations
 * throw an exception if one is not provided.
 */
TEST_F(IndexTest, OperationsWithNullDataManager) {
	// Create a key-value structure that is GUARANTEED to be larger than DATA_SIZE
	// to force the code path that requires blob storage.
	std::vector<graph::Index::KeyValuePair> kvs;
	std::string largeKey(graph::Index::DATA_SIZE, 'X'); // Create a key that alone fills the buffer
	kvs.push_back({largeKey, {123}}); // Adding the value makes it overflow.

	// This will now attempt to create a blob because data size > DATA_SIZE,
	// and it must fail because the dataManager is null.
	ASSERT_THROW(leafNode_.setAllKeyValues(kvs, nullptr), std::runtime_error);

	// Now, test the retrieval side.
	// First, successfully store the data with a valid manager.
	leafNode_.setAllKeyValues(kvs, dataManager_);
	ASSERT_TRUE(leafNode_.hasBlobStorage());

	// Attempting to retrieve the data with a null manager must also fail.
	ASSERT_THROW(leafNode_.getAllKeyValues(nullptr), std::runtime_error);
}

TEST_F(IndexTest, ConstantsAndSize) {
	EXPECT_EQ(graph::Index::TOTAL_INDEX_SIZE, 256u);
	EXPECT_EQ(graph::Index::METADATA_SIZE,
			  offsetof(graph::Index::Metadata, isActive) + sizeof(graph::Index::Metadata::isActive));
	EXPECT_EQ(graph::Index::DATA_SIZE, graph::Index::TOTAL_INDEX_SIZE - (offsetof(graph::Index::Metadata, isActive) +
																		 sizeof(graph::Index::Metadata::isActive)));
}
