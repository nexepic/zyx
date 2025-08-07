/**
 * @file test_PropertyIndex.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/29
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include "graph/core/Database.hpp"
#include "graph/query/indexes/PropertyIndex.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"

class PropertyIndexTest : public ::testing::Test {
protected:
	// SetUpTestSuite is run ONCE for the entire test suite.
	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath_ = std::filesystem::temp_directory_path() /
						("test_propertyIndex_" + boost::uuids::to_string(uuid) + ".dat");

		database_ = std::make_unique<graph::Database>(testFilePath_.string());
		database_->open();
		dataManager_ = database_->getStorage()->getDataManager();

		graph::StateRegistry::initialize(dataManager_);
	}

	// TearDownTestSuite is run ONCE after all tests in the suite have finished.
	static void TearDownTestSuite() {
		if (database_) {
			database_->close();
			database_.reset();
		}
		if (std::filesystem::exists(testFilePath_)) {
			std::filesystem::remove(testFilePath_);
		}
	}

	// SetUp is run before EACH TEST_F.
	void SetUp() override {
		constexpr uint32_t indexType = graph::query::indexes::IndexTypes::NODE_PROPERTY_TYPE;
		const std::string stateKeyPrefix = "test.node.properties";

		propertyIndex_ =
				std::make_unique<graph::query::indexes::PropertyIndex>(dataManager_, indexType, stateKeyPrefix);
		propertyIndex_->drop(); // Use drop to ensure a completely clean slate, including state.
		propertyIndex_->flush();
	}

	// TearDown is run after EACH TEST_F.
	void TearDown() override {
		if (propertyIndex_) {
			propertyIndex_->drop();
		}
	}

	static bool vectorContains(const std::vector<int64_t> &vec, int64_t val) {
		return std::ranges::find(vec, val) != vec.end();
	}

	static inline std::filesystem::path testFilePath_;
	static inline std::unique_ptr<graph::Database> database_;
	static inline std::shared_ptr<graph::storage::DataManager> dataManager_;

	std::unique_ptr<graph::query::indexes::PropertyIndex> propertyIndex_;
};


TEST_F(PropertyIndexTest, IsEmptyInitiallyAndAfterDrop) {
	EXPECT_TRUE(propertyIndex_->isEmpty());
	propertyIndex_->addProperty(1, "test", "value");
	EXPECT_FALSE(propertyIndex_->isEmpty());
	propertyIndex_->drop();
	EXPECT_TRUE(propertyIndex_->isEmpty());
}

TEST_F(PropertyIndexTest, AddAndFindPropertiesOfAllTypes) {
	// Add properties of all supported types
	propertyIndex_->addProperty(1, "name", std::string("Alice"));
	propertyIndex_->addProperty(2, "age", 30);
	propertyIndex_->addProperty(3, "score", 99.5);
	propertyIndex_->addProperty(4, "active", true);

	EXPECT_FALSE(propertyIndex_->isEmpty());

	// Verify each can be found
	auto foundStr = propertyIndex_->findExactMatch("name", std::string("Alice"));
	ASSERT_EQ(foundStr.size(), 1);
	EXPECT_EQ(foundStr[0], 1);

	auto foundInt = propertyIndex_->findExactMatch("age", 30);
	ASSERT_EQ(foundInt.size(), 1);
	EXPECT_EQ(foundInt[0], 2);

	auto foundDouble = propertyIndex_->findExactMatch("score", 99.5);
	ASSERT_EQ(foundDouble.size(), 1);
	EXPECT_EQ(foundDouble[0], 3);

	auto foundBool = propertyIndex_->findExactMatch("active", true);
	ASSERT_EQ(foundBool.size(), 1);
	EXPECT_EQ(foundBool[0], 4);
}

TEST_F(PropertyIndexTest, RemoveProperty) {
	int64_t nodeId = 6;
	std::string key = "status";
	std::string value = "pending";
	propertyIndex_->addProperty(nodeId, key, value);

	// Verify it exists before removal
	auto found_before = propertyIndex_->findExactMatch(key, value);
	ASSERT_TRUE(vectorContains(found_before, nodeId));

	// Remove and verify it's gone
	propertyIndex_->removeProperty(nodeId, key, value);
	auto found_after = propertyIndex_->findExactMatch(key, value);
	EXPECT_FALSE(vectorContains(found_after, nodeId));
}

TEST_F(PropertyIndexTest, UpdateProperty) {
	int64_t nodeId = 7;
	std::string key = "status";
	std::string oldValue = "pending";
	std::string newValue = "approved";

	// Add old value
	propertyIndex_->addProperty(nodeId, key, oldValue);
	ASSERT_TRUE(vectorContains(propertyIndex_->findExactMatch(key, oldValue), nodeId));

	// Simulate an update by removing old and adding new
	propertyIndex_->removeProperty(nodeId, key, oldValue);
	propertyIndex_->addProperty(nodeId, key, newValue);

	// Verify old value is gone and new value is present
	ASSERT_TRUE(propertyIndex_->findExactMatch(key, oldValue).empty());
	ASSERT_TRUE(vectorContains(propertyIndex_->findExactMatch(key, newValue), nodeId));
}

TEST_F(PropertyIndexTest, FindRangeForSeparateTypes) {
	// Arrange: Index an integer-based key and a double-based key.
	std::string intKey = "level";
	propertyIndex_->addProperty(8, intKey, 10);
	propertyIndex_->addProperty(9, intKey, 20);
	propertyIndex_->addProperty(10, intKey, 30);

	std::string doubleKey = "rating";
	propertyIndex_->addProperty(11, doubleKey, 3.5);
	propertyIndex_->addProperty(12, doubleKey, 4.5);
	propertyIndex_->addProperty(13, doubleKey, 5.0);

	// Act & Assert: Test integer range
	auto foundInts = propertyIndex_->findRange(intKey, 15, 25);
	ASSERT_EQ(foundInts.size(), 1);
	EXPECT_EQ(foundInts[0], 9);

	// Act & Assert: Test double range
	auto foundDoubles = propertyIndex_->findRange(doubleKey, 4.0, 5.0);
	ASSERT_EQ(foundDoubles.size(), 2);
	EXPECT_TRUE(vectorContains(foundDoubles, 12));
	EXPECT_TRUE(vectorContains(foundDoubles, 13));

	// Act & Assert: A range query on a non-numeric-indexed key should return nothing.
	propertyIndex_->addProperty(14, "name", std::string("a string"));
	auto foundStrings = propertyIndex_->findRange("name", 0, 100);
	EXPECT_TRUE(foundStrings.empty());
}

TEST_F(PropertyIndexTest, TypeEnforcementPreventsMixing) {
	// Arrange: Index a key with a specific type (string).
	std::string key = "mixedKey";
	propertyIndex_->addProperty(1, key, std::string("string value"));
	EXPECT_EQ(propertyIndex_->getIndexedKeyType(key), graph::query::indexes::PropertyType::STRING);

	// Act: Attempt to add a property with the same key but a different type (int).
	// This should be ignored by the index and ideally log a warning.
	propertyIndex_->addProperty(2, key, 12345);

	// Assert:
	// 1. The original string value can still be found.
	auto foundStr = propertyIndex_->findExactMatch(key, std::string("string value"));
	ASSERT_EQ(foundStr.size(), 1);
	EXPECT_EQ(foundStr[0], 1);

	// 2. The mismatched integer value cannot be found.
	auto foundInt = propertyIndex_->findExactMatch(key, 12345);
	EXPECT_TRUE(foundInt.empty());

	// 3. The indexed type for the key remains STRING.
	EXPECT_EQ(propertyIndex_->getIndexedKeyType(key), graph::query::indexes::PropertyType::STRING);
}


TEST_F(PropertyIndexTest, GetIndexedKeys) {
	propertyIndex_->addProperty(11, "k1", std::string("v1"));
	propertyIndex_->addProperty(12, "k2", 123);
	propertyIndex_->addProperty(13, "k3", 1.23);
	propertyIndex_->addProperty(14, "k4", true);
	// This one should be ignored due to type mismatch.
	propertyIndex_->addProperty(15, "k1", 456);

	auto keys = propertyIndex_->getIndexedKeys();
	std::ranges::sort(keys);

	// Expecting 4 keys because the second "k1" was a type mismatch and not indexed.
	ASSERT_EQ(keys.size(), 4);
	EXPECT_EQ(keys[0], "k1");
	EXPECT_EQ(keys[1], "k2");
	EXPECT_EQ(keys[2], "k3");
	EXPECT_EQ(keys[3], "k4");
}

TEST_F(PropertyIndexTest, ClearAndDropKey) {
	propertyIndex_->addProperty(20, "key_to_clear", std::string("v"));
	propertyIndex_->addProperty(21, "another_key", 1);
	ASSERT_TRUE(propertyIndex_->hasKeyIndexed("key_to_clear"));
	ASSERT_TRUE(propertyIndex_->hasKeyIndexed("another_key"));

	// Clear one key, the other should remain.
	propertyIndex_->clearKey("key_to_clear");
	EXPECT_FALSE(propertyIndex_->isEmpty());
	EXPECT_FALSE(propertyIndex_->hasKeyIndexed("key_to_clear"));
	EXPECT_TRUE(propertyIndex_->hasKeyIndexed("another_key"));
	EXPECT_TRUE(propertyIndex_->findExactMatch("key_to_clear", std::string("v")).empty());
	EXPECT_FALSE(propertyIndex_->findExactMatch("another_key", 1).empty());

	// Drop the last key, index should become empty.
	propertyIndex_->dropKey("another_key");
	EXPECT_TRUE(propertyIndex_->isEmpty());
	EXPECT_FALSE(propertyIndex_->hasKeyIndexed("another_key"));
}

TEST_F(PropertyIndexTest, SaveAndLoadState) {
	// Arrange: Add properties and flush the state to disk.
	propertyIndex_->addProperty(101, "name", std::string("persisted_user"));
	propertyIndex_->addProperty(102, "value", 999);
	propertyIndex_->addProperty(103, "active", true);
	propertyIndex_->flush();

	// Act: Create a new PropertyIndex instance with the same parameters.
	// This will force it to load the previously saved state from disk.
	constexpr uint32_t indexType = graph::query::indexes::IndexTypes::NODE_PROPERTY_TYPE;
	const std::string stateKeyPrefix = "test.node.properties";
	auto reloadedIndex =
			std::make_unique<graph::query::indexes::PropertyIndex>(dataManager_, indexType, stateKeyPrefix);

	// Assert: The reloaded index should contain all the data and type information.
	EXPECT_FALSE(reloadedIndex->isEmpty());
	EXPECT_EQ(reloadedIndex->getIndexedKeyType("name"), graph::query::indexes::PropertyType::STRING);
	EXPECT_EQ(reloadedIndex->getIndexedKeyType("value"), graph::query::indexes::PropertyType::INTEGER);
	EXPECT_EQ(reloadedIndex->getIndexedKeyType("active"), graph::query::indexes::PropertyType::BOOLEAN);

	auto foundStr = reloadedIndex->findExactMatch("name", std::string("persisted_user"));
	ASSERT_EQ(foundStr.size(), 1);
	EXPECT_EQ(foundStr[0], 101);

	auto foundInt = reloadedIndex->findExactMatch("value", 999);
	ASSERT_EQ(foundInt.size(), 1);
	EXPECT_EQ(foundInt[0], 102);

	auto foundBool = reloadedIndex->findExactMatch("active", true);
	ASSERT_EQ(foundBool.size(), 1);
	EXPECT_EQ(foundBool[0], 103);
}
