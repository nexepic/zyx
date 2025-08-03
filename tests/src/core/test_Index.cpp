/**
 * @file test_Index.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/13
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <filesystem>
#include <gtest/gtest.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "graph/core/Database.hpp"
#include "graph/core/Index.hpp"

class IndexTest : public ::testing::Test {
protected:
	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		std::filesystem::path testFilePath =
				std::filesystem::temp_directory_path() / ("test_db_file_index_" + boost::uuids::to_string(uuid) + ".dat");

		database_ = std::make_unique<graph::Database>(testFilePath.string());
		database_->open();
        dataManager_ = database_->getStorage()->getDataManager();
	}

    static void TearDownTestSuite() {
        if (database_) {
            database_->close();
            database_.reset();
        }
        dataManager_.reset();
    }

	void SetUp() override {
		// Re-initialize node objects before each test
		leafNode_ = graph::Index(1, graph::Index::NodeType::LEAF, 0);
		internalNode_ = graph::Index(2, graph::Index::NodeType::INTERNAL, 0);
	}

	graph::Index leafNode_;
	graph::Index internalNode_;

	static std::shared_ptr<graph::storage::DataManager> dataManager_;
    static std::unique_ptr<graph::Database> database_;
};

// Initialize static members
std::shared_ptr<graph::storage::DataManager> IndexTest::dataManager_ = nullptr;
std::unique_ptr<graph::Database> IndexTest::database_ = nullptr;

TEST_F(IndexTest, InsertStringKey) {
	leafNode_.insertStringKey("key1", 100, dataManager_);
	leafNode_.insertStringKey("key2", 200, dataManager_);
	leafNode_.insertStringKey("key1", 101, dataManager_); // Add another value to same key

    auto values = leafNode_.findStringValues("key1", dataManager_);
	ASSERT_EQ(values.size(), 2);
    // Values are not guaranteed to be sorted within the vector
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

    // Remove the last value, which should remove the key itself
    ASSERT_TRUE(leafNode_.removeStringKey("key1", 101, dataManager_));
    values = leafNode_.findStringValues("key1", dataManager_);
    ASSERT_TRUE(values.empty());
}

TEST_F(IndexTest, AddChild) {
	internalNode_.addChild("keyB", 200);
	internalNode_.addChild("keyA", 100); // Insert out of order

    auto children = internalNode_.getAllChildren();
	ASSERT_EQ(children.size(), 2);
    // Verify they are stored in sorted order
	ASSERT_EQ(children[0].key, "keyA");
	ASSERT_EQ(children[0].childId, 100);
    ASSERT_EQ(children[1].key, "keyB");
	ASSERT_EQ(children[1].childId, 200);
}

TEST_F(IndexTest, RemoveChild) {
	internalNode_.addChild("keyA", 100);
	internalNode_.addChild("keyB", 200);
	ASSERT_TRUE(internalNode_.removeChild("keyA"));

    auto children = internalNode_.getAllChildren();
	ASSERT_EQ(children.size(), 1);
	ASSERT_EQ(children[0].key, "keyB");
	ASSERT_EQ(children[0].childId, 200);

    ASSERT_FALSE(internalNode_.removeChild("keyC")); // Non-existent key
}

TEST_F(IndexTest, SerializationDeserialization) {
	leafNode_.insertStringKey("key1", 100, dataManager_);
	leafNode_.insertStringKey("key2", 200, dataManager_);

	std::ostringstream os;
	leafNode_.serialize(os);

	std::istringstream is(os.str());
	auto deserializedNode = graph::Index::deserialize(is);

    // The deserialized node also needs a dataManager to interpret its data buffer.
	auto values1 = deserializedNode.findStringValues("key1", dataManager_);
	ASSERT_EQ(values1.size(), 1);
	ASSERT_EQ(values1[0], 100);

    auto values2 = deserializedNode.findStringValues("key2", dataManager_);
	ASSERT_EQ(values2.size(), 1);
	ASSERT_EQ(values2[0], 200);
}

TEST_F(IndexTest, DataOverflowToBlob) {
    // Generate enough data to overflow the inline buffer
    std::vector<graph::Index::KeyValuePair> kvs;
    std::string long_key = "a_very_long_key_to_ensure_we_take_up_space_";
    size_t total_size = 0;
    int i = 0;
    while(total_size < graph::Index::DATA_SIZE) {
        std::string key = long_key + std::to_string(i);
        kvs.push_back({key, {100L + i}});
        total_size += key.length() + sizeof(uint32_t) * 2 + sizeof(int64_t);
        i++;
    }

    // This should trigger blob storage
    leafNode_.setAllKeyValues(kvs, dataManager_);

    // Verify that blob storage was used
    ASSERT_TRUE(leafNode_.hasBlobStorage());
    ASSERT_NE(leafNode_.getBlobId(), 0);

    // Verify we can read the data back correctly
    auto retrievedKVs = leafNode_.getAllKeyValues(dataManager_);
    ASSERT_EQ(retrievedKVs.size(), kvs.size());
    ASSERT_EQ(retrievedKVs[0].key, kvs[0].key);
    ASSERT_EQ(retrievedKVs.back().values[0], kvs.back().values[0]);

    // Now, update it with small data, it should switch back to inline
    std::vector<graph::Index::KeyValuePair> small_kvs = {{"small_key", {999}}};
    leafNode_.setAllKeyValues(small_kvs, dataManager_);

    ASSERT_FALSE(leafNode_.hasBlobStorage());
    ASSERT_EQ(leafNode_.getBlobId(), 0);

    auto finalKVs = leafNode_.getAllKeyValues(dataManager_);
    ASSERT_EQ(finalKVs.size(), 1);
    ASSERT_EQ(finalKVs[0].key, "small_key");
}

TEST_F(IndexTest, ConstantsAndSize) {
	EXPECT_EQ(graph::Index::TOTAL_INDEX_SIZE, 256u);
	EXPECT_EQ(graph::Index::METADATA_SIZE, sizeof(graph::Index::Metadata));
	EXPECT_EQ(graph::Index::DATA_SIZE, graph::Index::TOTAL_INDEX_SIZE - sizeof(graph::Index::Metadata));
}