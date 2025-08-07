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
	EXPECT_TRUE(propertyIndex_->isEmpty()) << "Index should be empty upon creation.";
	propertyIndex_->addProperty(1, "test", "value");
	EXPECT_FALSE(propertyIndex_->isEmpty()) << "Index should not be empty after adding a property.";
	propertyIndex_->drop();
	EXPECT_TRUE(propertyIndex_->isEmpty()) << "Index should be empty again after dropping.";
}

TEST_F(PropertyIndexTest, AddAndFindPropertiesOfAllTypes) {
	// Arrange: Add properties of all supported scalar types.
	propertyIndex_->addProperty(1, "name", std::string("Alice"));
	propertyIndex_->addProperty(2, "age", 30); // Robust: no cast needed
	propertyIndex_->addProperty(3, "score", 99.5);
	propertyIndex_->addProperty(4, "active", true);

	EXPECT_FALSE(propertyIndex_->isEmpty());

	// Act & Assert: Verify each property can be found with its correct value.
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
	// Arrange
	propertyIndex_->addProperty(6, "status", "pending");
	ASSERT_TRUE(vectorContains(propertyIndex_->findExactMatch("status", "pending"), 6));

	// Act
	propertyIndex_->removeProperty(6, "status", "pending");

	// Assert
	EXPECT_TRUE(propertyIndex_->findExactMatch("status", "pending").empty())
		<< "The property should not be findable after being removed.";
}

TEST_F(PropertyIndexTest, UpdatePropertySimulation) {
	// Arrange: An "update" is a remove followed by an add.
	propertyIndex_->addProperty(7, "status", "pending");
	ASSERT_TRUE(vectorContains(propertyIndex_->findExactMatch("status", "pending"), 7));

	// Act
	propertyIndex_->removeProperty(7, "status", "pending");
	propertyIndex_->addProperty(7, "status", "approved");

	// Assert
	EXPECT_TRUE(propertyIndex_->findExactMatch("status", "pending").empty());
	ASSERT_TRUE(vectorContains(propertyIndex_->findExactMatch("status", "approved"), 7));
}

TEST_F(PropertyIndexTest, TypeEnforcementPreventsMixing) {
	// Arrange: Index a key with a specific type (string).
	propertyIndex_->addProperty(1, "mixedKey", std::string("string value"));
	ASSERT_EQ(propertyIndex_->getIndexedKeyType("mixedKey"), graph::PropertyType::STRING);

	// Act: Attempt to add a property with the same key but a different type (integer).
	// This action should be silently ignored by the index to maintain type integrity.
	propertyIndex_->addProperty(2, "mixedKey", 12345);

	// Assert: The original value is still present, and the mismatched value was not added.
	EXPECT_TRUE(vectorContains(propertyIndex_->findExactMatch("mixedKey", std::string("string value")), 1));
	EXPECT_TRUE(propertyIndex_->findExactMatch("mixedKey", 12345).empty());

	// The indexed type for the key must remain unchanged.
	EXPECT_EQ(propertyIndex_->getIndexedKeyType("mixedKey"), graph::PropertyType::STRING);
}

TEST_F(PropertyIndexTest, FindRangeForNumericTypes) {
	// Arrange
	propertyIndex_->addProperty(8, "level", 10);
	propertyIndex_->addProperty(9, "level", 20);
	propertyIndex_->addProperty(10, "rating", 4.5);
	propertyIndex_->addProperty(11, "rating", 5.0);
	propertyIndex_->addProperty(12, "name", "a string"); // Non-numeric for boundary check

	// Act & Assert: Test integer range query.
	auto foundInts = propertyIndex_->findRange("level", 15.0, 25.0);
	ASSERT_EQ(foundInts.size(), 1);
	EXPECT_EQ(foundInts[0], 9);

	// Act & Assert: Test double range query.
	auto foundDoubles = propertyIndex_->findRange("rating", 4.0, 5.0);
	ASSERT_EQ(foundDoubles.size(), 2);
	EXPECT_TRUE(vectorContains(foundDoubles, 10));
	EXPECT_TRUE(vectorContains(foundDoubles, 11));

	// Act & Assert: A range query on a key indexed with a non-numeric type should return nothing.
	EXPECT_TRUE(propertyIndex_->findRange("name", 0, 100).empty());
}

TEST_F(PropertyIndexTest, GetIndexedKeys) {
	// Arrange
	propertyIndex_->addProperty(11, "k1", "v1");
	propertyIndex_->addProperty(12, "k2", 123);
	propertyIndex_->addProperty(13, "k3", 1.23);
	propertyIndex_->addProperty(14, "k4", true);
	propertyIndex_->addProperty(15, "k1", 456); // This should be ignored (type mismatch).

	// Act
	auto keys = propertyIndex_->getIndexedKeys();
	std::ranges::sort(keys);

	// Assert
	const std::vector<std::string> expected_keys = {"k1", "k2", "k3", "k4"};
	ASSERT_EQ(keys.size(), 4);
	EXPECT_EQ(keys, expected_keys);
}

TEST_F(PropertyIndexTest, ClearAndDropKey) {
	// Arrange
	propertyIndex_->addProperty(20, "key_to_clear", "v");
	propertyIndex_->addProperty(21, "another_key", 1);
	ASSERT_TRUE(propertyIndex_->hasKeyIndexed("key_to_clear"));
	ASSERT_TRUE(propertyIndex_->hasKeyIndexed("another_key"));

	// Act: Clear one key, the other should remain.
	propertyIndex_->clearKey("key_to_clear");

	// Assert
	EXPECT_FALSE(propertyIndex_->isEmpty());
	EXPECT_FALSE(propertyIndex_->hasKeyIndexed("key_to_clear"));
	EXPECT_TRUE(propertyIndex_->hasKeyIndexed("another_key"));
	EXPECT_FALSE(vectorContains(propertyIndex_->findExactMatch("another_key", 1), 0)); // Check other key is still valid

	// Act: Drop the last key.
	propertyIndex_->dropKey("another_key");

	// Assert
	EXPECT_TRUE(propertyIndex_->isEmpty());
	EXPECT_FALSE(propertyIndex_->hasKeyIndexed("another_key"));
}

TEST_F(PropertyIndexTest, IgnoresNullAndUnknownTypes) {
	// Arrange: Add a property with a NULL value (monostate).
	propertyIndex_->addProperty(50, "nullable_key", std::monostate{});

	// Assert: The index should remain empty as NULLs are not indexed.
	EXPECT_TRUE(propertyIndex_->isEmpty());
	EXPECT_FALSE(propertyIndex_->hasKeyIndexed("nullable_key"));
	EXPECT_EQ(propertyIndex_->getIndexedKeyType("nullable_key"), graph::PropertyType::UNKNOWN);
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
	EXPECT_EQ(reloadedIndex->getIndexedKeyType("name"), graph::PropertyType::STRING);
	EXPECT_EQ(reloadedIndex->getIndexedKeyType("value"), graph::PropertyType::INTEGER);
	EXPECT_EQ(reloadedIndex->getIndexedKeyType("active"), graph::PropertyType::BOOLEAN);

	// Verify data can be found in the reloaded index.
	EXPECT_TRUE(vectorContains(reloadedIndex->findExactMatch("name", std::string("persisted_user")), 101));
	EXPECT_TRUE(vectorContains(reloadedIndex->findExactMatch("value", 999), 102));
	EXPECT_TRUE(vectorContains(reloadedIndex->findExactMatch("active", true), 103));
}