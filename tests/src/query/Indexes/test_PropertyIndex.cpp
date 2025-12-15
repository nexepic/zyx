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
#include <numeric>
#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/PropertyIndex.hpp"

class PropertyIndexTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() /
					   ("test_propertyIndex" + boost::uuids::to_string(uuid) + ".dat");

		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		dataManager = database->getStorage()->getDataManager();

		constexpr uint32_t indexType = graph::query::indexes::IndexTypes::NODE_PROPERTY_TYPE;
		const std::string stateKeyPrefix = "test.node.properties";
		propertyIndex = database->getQueryEngine()->getIndexManager()->getNodeIndexManager()->getPropertyIndex();
	}

	void TearDown() override {
		if (database) {
			database->close();
		}
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	static bool vectorContains(const std::vector<int64_t> &vec, int64_t val) {
		return std::ranges::find(vec, val) != vec.end();
	}

	static std::string generatePaddedUuid(int i, int totalItems) {
		std::ostringstream ss;
		// Calculate the required width for padding based on the total number of items.
		// e.g., if totalItems is 2500, width is 4.
		const int width = static_cast<int>(std::to_string(totalItems).length());
		ss << "user-" << std::setw(width) << std::setfill('0') << i;
		return ss.str();
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::PropertyIndex> propertyIndex;
};


TEST_F(PropertyIndexTest, IsEmptyInitiallyAndAfterDrop) {
	EXPECT_TRUE(propertyIndex->isEmpty()) << "Index should be empty upon creation.";
	propertyIndex->addProperty(1, "test", "value");
	EXPECT_FALSE(propertyIndex->isEmpty()) << "Index should not be empty after adding a property.";
	propertyIndex->drop();
	EXPECT_TRUE(propertyIndex->isEmpty()) << "Index should be empty again after dropping.";
}

TEST_F(PropertyIndexTest, AddAndFindPropertiesOfAllTypes) {
	// Arrange: Add properties of all supported scalar types.
	propertyIndex->addProperty(1, "name", std::string("Alice"));
	propertyIndex->addProperty(2, "age", 30); // Robust: no cast needed
	propertyIndex->addProperty(3, "score", 99.5);
	propertyIndex->addProperty(4, "active", true);

	EXPECT_FALSE(propertyIndex->isEmpty());

	// Act & Assert: Verify each property can be found with its correct value.
	auto foundStr = propertyIndex->findExactMatch("name", std::string("Alice"));
	ASSERT_EQ(foundStr.size(), 1);
	EXPECT_EQ(foundStr[0], 1);

	auto foundInt = propertyIndex->findExactMatch("age", 30);
	ASSERT_EQ(foundInt.size(), 1);
	EXPECT_EQ(foundInt[0], 2);

	auto foundDouble = propertyIndex->findExactMatch("score", 99.5);
	ASSERT_EQ(foundDouble.size(), 1);
	EXPECT_EQ(foundDouble[0], 3);

	auto foundBool = propertyIndex->findExactMatch("active", true);
	ASSERT_EQ(foundBool.size(), 1);
	EXPECT_EQ(foundBool[0], 4);
}

TEST_F(PropertyIndexTest, RemoveProperty) {
	// Arrange
	propertyIndex->addProperty(6, "status", "pending");
	ASSERT_TRUE(vectorContains(propertyIndex->findExactMatch("status", "pending"), 6));

	// Act
	propertyIndex->removeProperty(6, "status", "pending");

	// Assert
	EXPECT_TRUE(propertyIndex->findExactMatch("status", "pending").empty())
			<< "The property should not be findable after being removed.";
}

TEST_F(PropertyIndexTest, UpdatePropertySimulation) {
	// Arrange: An "update" is a remove followed by an add.
	propertyIndex->addProperty(7, "status", "pending");
	ASSERT_TRUE(vectorContains(propertyIndex->findExactMatch("status", "pending"), 7));

	// Act
	propertyIndex->removeProperty(7, "status", "pending");
	propertyIndex->addProperty(7, "status", "approved");

	// Assert
	EXPECT_TRUE(propertyIndex->findExactMatch("status", "pending").empty());
	ASSERT_TRUE(vectorContains(propertyIndex->findExactMatch("status", "approved"), 7));
}

TEST_F(PropertyIndexTest, TypeEnforcementPreventsMixing) {
	// Arrange: Index a key with a specific type (string).
	propertyIndex->addProperty(1, "mixedKey", std::string("string value"));
	ASSERT_EQ(propertyIndex->getIndexedKeyType("mixedKey"), graph::PropertyType::STRING);

	// Act: Attempt to add a property with the same key but a different type (integer).
	// This action should be silently ignored by the index to maintain type integrity.
	propertyIndex->addProperty(2, "mixedKey", 12345);

	// Assert: The original value is still present, and the mismatched value was not added.
	EXPECT_TRUE(vectorContains(propertyIndex->findExactMatch("mixedKey", std::string("string value")), 1));
	EXPECT_TRUE(propertyIndex->findExactMatch("mixedKey", 12345).empty());

	// The indexed type for the key must remain unchanged.
	EXPECT_EQ(propertyIndex->getIndexedKeyType("mixedKey"), graph::PropertyType::STRING);
}

TEST_F(PropertyIndexTest, FindRangeForNumericTypes) {
	// Arrange
	propertyIndex->addProperty(8, "level", 10);
	propertyIndex->addProperty(9, "level", 20);
	propertyIndex->addProperty(10, "rating", 4.5);
	propertyIndex->addProperty(11, "rating", 5.0);
	propertyIndex->addProperty(12, "name", "a string"); // Non-numeric for boundary check

	// Act & Assert: Test integer range query.
	auto foundInts = propertyIndex->findRange("level", 15.0, 25.0);
	ASSERT_EQ(foundInts.size(), 1);
	EXPECT_EQ(foundInts[0], 9);

	// Act & Assert: Test double range query.
	auto foundDoubles = propertyIndex->findRange("rating", 4.0, 5.0);
	ASSERT_EQ(foundDoubles.size(), 2);
	EXPECT_TRUE(vectorContains(foundDoubles, 10));
	EXPECT_TRUE(vectorContains(foundDoubles, 11));

	// Act & Assert: A range query on a key indexed with a non-numeric type should return nothing.
	EXPECT_TRUE(propertyIndex->findRange("name", 0, 100).empty());
}

TEST_F(PropertyIndexTest, GetIndexedKeys) {
	// Arrange
	propertyIndex->addProperty(11, "k1", "v1");
	propertyIndex->addProperty(12, "k2", 123);
	propertyIndex->addProperty(13, "k3", 1.23);
	propertyIndex->addProperty(14, "k4", true);
	propertyIndex->addProperty(15, "k1", 456); // This should be ignored (type mismatch).

	// Act
	auto keys = propertyIndex->getIndexedKeys();
	std::ranges::sort(keys);

	// Assert
	const std::vector<std::string> expected_keys = {"k1", "k2", "k3", "k4"};
	ASSERT_EQ(keys.size(), 4);
	EXPECT_EQ(keys, expected_keys);
}

TEST_F(PropertyIndexTest, ClearAndDropKey) {
	// Arrange
	propertyIndex->addProperty(20, "key_to_clear", "v");
	propertyIndex->addProperty(21, "another_key", 1);
	ASSERT_TRUE(propertyIndex->hasKeyIndexed("key_to_clear"));
	ASSERT_TRUE(propertyIndex->hasKeyIndexed("another_key"));

	// Act: Clear one key, the other should remain.
	propertyIndex->clearKey("key_to_clear");

	// Assert
	EXPECT_FALSE(propertyIndex->isEmpty());
	EXPECT_FALSE(propertyIndex->hasKeyIndexed("key_to_clear"));
	EXPECT_TRUE(propertyIndex->hasKeyIndexed("another_key"));
	EXPECT_FALSE(vectorContains(propertyIndex->findExactMatch("another_key", 1), 0)); // Check other key is still valid

	// Act: Drop the last key.
	propertyIndex->dropKey("another_key");

	// Assert
	EXPECT_TRUE(propertyIndex->isEmpty());
	EXPECT_FALSE(propertyIndex->hasKeyIndexed("another_key"));
}

TEST_F(PropertyIndexTest, IgnoresNullAndUnknownTypes) {
	// Arrange: Add a property with a NULL value (monostate).
	propertyIndex->addProperty(50, "nullable_key", std::monostate{});

	// Assert: The index should remain empty as NULLs are not indexed.
	EXPECT_TRUE(propertyIndex->isEmpty());
	EXPECT_FALSE(propertyIndex->hasKeyIndexed("nullable_key"));
	EXPECT_EQ(propertyIndex->getIndexedKeyType("nullable_key"), graph::PropertyType::UNKNOWN);
}

TEST_F(PropertyIndexTest, SaveAndLoadState) {
	// Arrange: Add properties and flush the state to disk.
	propertyIndex->addProperty(101, "name", std::string("persisted_user"));
	propertyIndex->addProperty(102, "value", 999);
	propertyIndex->addProperty(103, "active", true);
	propertyIndex->flush();

	// Act: Create a new PropertyIndex instance.
	constexpr uint32_t indexType = graph::query::indexes::IndexTypes::NODE_PROPERTY_TYPE;
	// Use the correct prefix that matched the one in SetUp (via IndexManager)
	const std::string stateKeyPrefix = graph::query::indexes::StateKeys::NODE_PROPERTY_PREFIX;

	auto reloadedIndex = std::make_unique<graph::query::indexes::PropertyIndex>(
			dataManager, database->getStorage()->getSystemStateManager(), indexType, stateKeyPrefix);

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

TEST_F(PropertyIndexTest, HandlesLargeNumberOfPropertiesAndReloads) {
	constexpr int numItems = 2500;
	const std::string stateKeyPrefix = graph::query::indexes::StateKeys::NODE_PROPERTY_PREFIX;
	constexpr uint32_t indexType = graph::query::indexes::IndexTypes::NODE_PROPERTY_TYPE;

	for (int i = 1; i <= numItems; ++i) {
		propertyIndex->addProperty(i, "id", i);
		propertyIndex->addProperty(i, "uuid", generatePaddedUuid(i, numItems));
		propertyIndex->addProperty(numItems + i, "active", (i % 2 == 0));
	}

	// Act I: Flush the state to disk.
	propertyIndex->flush();

	// Drop the current instance to ensure the next one reads from disk.
	propertyIndex.reset();

	// Act II: Create a new instance, which should load the state from the DataManager.
	auto reloadedIndex = std::make_unique<graph::query::indexes::PropertyIndex>(
			dataManager, database->getStorage()->getSystemStateManager(), indexType, stateKeyPrefix);

	// Assert: Verify that the reloaded index contains the correct data.
	ASSERT_FALSE(reloadedIndex->isEmpty());
	ASSERT_EQ(reloadedIndex->getIndexedKeyType("id"), graph::PropertyType::INTEGER);
	ASSERT_EQ(reloadedIndex->getIndexedKeyType("uuid"), graph::PropertyType::STRING);
	ASSERT_EQ(reloadedIndex->getIndexedKeyType("active"), graph::PropertyType::BOOLEAN);

	// Verify a subset of the data to ensure correctness.
	for (int i = 1; i <= numItems; i += 250) { // Check a sample of items.
		// Check integer property
		auto foundById = reloadedIndex->findExactMatch("id", i);
		ASSERT_EQ(foundById.size(), 1) << "Failed to find integer property for ID: " << i;
		EXPECT_EQ(foundById[0], i);

		// Check string property using the same padded format.
		auto foundByUuid = reloadedIndex->findExactMatch("uuid", generatePaddedUuid(i, numItems));
		ASSERT_EQ(foundByUuid.size(), 1) << "Failed to find string property for ID: " << i;
		EXPECT_EQ(foundByUuid[0], i);

		// Check boolean property
		auto foundByActive = reloadedIndex->findExactMatch("active", (i % 2 == 0));
		EXPECT_TRUE(vectorContains(foundByActive, numItems + i))
				<< "Failed to find boolean property for ID: " << (numItems + i);
	}

	// Check the last item to be sure.
	auto lastItem = reloadedIndex->findExactMatch("id", numItems);
	ASSERT_EQ(lastItem.size(), 1);
	EXPECT_EQ(lastItem[0], numItems);
}

TEST_F(PropertyIndexTest, HandlesRandomInsertionsAndDeletions) {
	// Arrange: Create a list of IDs to be inserted randomly.
	constexpr int numItems = 1500; // A size large enough to cause multiple splits.
	std::vector<int64_t> ids(numItems);
	std::iota(ids.begin(), ids.end(), 1); // Fills the vector with 1, 2, 3, ..., numItems

	// Use a deterministic random engine for reproducibility of the test.
	std::mt19937 g(42); // Use a fixed seed.
	std::ranges::shuffle(ids, g);

	// Act I: Insert properties in a random order.
	for (int64_t id: ids) {
		propertyIndex->addProperty(id, "random_id", id);
	}
	propertyIndex->flush();

	// Assert I: Verify all items can be found after random insertion.
	for (int64_t id = 1; id <= numItems; ++id) {
		auto result = propertyIndex->findExactMatch("random_id", id);
		ASSERT_EQ(result.size(), 1) << "Failed to find item " << id << " after random insertion.";
		EXPECT_EQ(result[0], id);
	}

	// Arrange II: Shuffle again for random deletion.
	std::ranges::shuffle(ids, g);
	std::vector idsToDelete(ids.begin(), ids.begin() + numItems / 2);
	std::vector idsToKeep(ids.begin() + numItems / 2, ids.end());

	// Act II: Remove half of the properties in a random order.
	for (int64_t id: idsToDelete) {
		propertyIndex->removeProperty(id, "random_id", id);
	}
	propertyIndex->flush();

	// Assert II: Verify that deleted items are gone and remaining items are still present.
	// Check deleted items
	for (int64_t id: idsToDelete) {
		auto result = propertyIndex->findExactMatch("random_id", id);
		EXPECT_TRUE(result.empty()) << "Item " << id << " should have been deleted.";
	}
	// Check kept items
	for (int64_t id: idsToKeep) {
		auto result = propertyIndex->findExactMatch("random_id", id);
		ASSERT_EQ(result.size(), 1) << "Failed to find remaining item " << id;
		EXPECT_EQ(result[0], id);
	}
}

TEST_F(PropertyIndexTest, CreateIndex_RegistersKeyWithoutData) {
	const std::string key = "future_key";

	// 1. Initially Empty
	EXPECT_FALSE(propertyIndex->hasKeyIndexed(key));
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::UNKNOWN);

	// 2. Call createIndex explicitly (simulates what IndexManager does)
	// This puts the key into indexedKeyTypes_ with UNKNOWN type.
	propertyIndex->createIndex(key);

	// 3. Verify it is now "Indexed" even without data
	EXPECT_TRUE(propertyIndex->hasKeyIndexed(key)) << "createIndex should mark key as indexed immediately";
	// It should be listed in getIndexedKeys
	auto keys = propertyIndex->getIndexedKeys();
	EXPECT_NE(std::find(keys.begin(), keys.end(), key), keys.end());

	// 4. Now add data
	propertyIndex->addProperty(1, key, 123);

	// 5. Type should transition from UNKNOWN to INTEGER
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::INTEGER);
	auto results = propertyIndex->findExactMatch(key, 123);
	EXPECT_EQ(results.size(), 1);
}

TEST_F(PropertyIndexTest, Persistence_MapKeyRemoval) {
	const std::string key1 = "to_be_deleted";
	const std::string key2 = "to_remain";

	propertyIndex->addProperty(1, key1, 100);
	propertyIndex->addProperty(1, key2, 200);
	propertyIndex->flush();
	database->getStorage()->flush();

	// 1. Manually drop one key
	propertyIndex->dropKey(key1);
	propertyIndex->flush();
	database->getStorage()->flush();

	// 2. Reload
	database->close();
	database.reset();

	auto newDatabase = std::make_unique<graph::Database>(testFilePath.string());
	newDatabase->open();

	// 3. Verify key1 is totally gone from persisted state
	auto reloadedIdx = newDatabase->getQueryEngine()->getIndexManager()->getNodeIndexManager()->getPropertyIndex();
	EXPECT_FALSE(reloadedIdx->hasKeyIndexed(key1));
	EXPECT_TRUE(reloadedIdx->hasKeyIndexed(key2));
}
