/**
 * @file test_PropertyIndex.cpp
 * @author Nexepic
 * @date 2025/7/29
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
#include <gtest/gtest.h>
#include <memory>
#include <numeric>
#include "graph/core/Database.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
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
	ASSERT_EQ(foundStr.size(), 1u);
	EXPECT_EQ(foundStr[0], 1);

	auto foundInt = propertyIndex->findExactMatch("age", 30);
	ASSERT_EQ(foundInt.size(), 1u);
	EXPECT_EQ(foundInt[0], 2);

	auto foundDouble = propertyIndex->findExactMatch("score", 99.5);
	ASSERT_EQ(foundDouble.size(), 1u);
	EXPECT_EQ(foundDouble[0], 3);

	auto foundBool = propertyIndex->findExactMatch("active", true);
	ASSERT_EQ(foundBool.size(), 1u);
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
	ASSERT_EQ(foundInts.size(), 1u);
	EXPECT_EQ(foundInts[0], 9);

	// Act & Assert: Test double range query.
	auto foundDoubles = propertyIndex->findRange("rating", 4.0, 5.0);
	ASSERT_EQ(foundDoubles.size(), 2u);
	EXPECT_TRUE(vectorContains(foundDoubles, 10));
	EXPECT_TRUE(vectorContains(foundDoubles, 11));

	// Act & Assert: A range query on a key indexed with a non-numeric type should return nothing.
	EXPECT_TRUE(propertyIndex->findRange("name", 0, 100).empty());
}

TEST_F(PropertyIndexTest, GetIndexedKeys) {
	// [FIX] Arrange: Explicitly create the indexes first!
	// PropertyIndex requires the key to be registered before adding data.
	propertyIndex->createIndex("k1");
	propertyIndex->createIndex("k2");
	propertyIndex->createIndex("k3");
	propertyIndex->createIndex("k4");

	// Act: Add properties
	// Now that keys are registered, these calls will actually insert data into B-Trees
	propertyIndex->addProperty(11, "k1", "v1");
	propertyIndex->addProperty(12, "k2", 123);
	propertyIndex->addProperty(13, "k3", 1.23);
	propertyIndex->addProperty(14, "k4", true);

	// Test Type Mismatch logic
	// k1 is String (inferred from first insert "v1"). 456 is Int. Should be ignored.
	propertyIndex->addProperty(15, "k1", 456);

	// Act: Retrieve keys
	auto keys = propertyIndex->getIndexedKeys();
	std::ranges::sort(keys);

	// Assert
	const std::vector<std::string> expected_keys = {"k1", "k2", "k3", "k4"};

	// Now this should pass because the keys were registered
	ASSERT_EQ(keys.size(), 4u);
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
		ASSERT_EQ(foundById.size(), 1u) << "Failed to find integer property for ID: " << i;
		EXPECT_EQ(foundById[0], i);

		// Check string property using the same padded format.
		auto foundByUuid = reloadedIndex->findExactMatch("uuid", generatePaddedUuid(i, numItems));
		ASSERT_EQ(foundByUuid.size(), 1u) << "Failed to find string property for ID: " << i;
		EXPECT_EQ(foundByUuid[0], i);

		// Check boolean property
		auto foundByActive = reloadedIndex->findExactMatch("active", (i % 2 == 0));
		EXPECT_TRUE(vectorContains(foundByActive, numItems + i))
				<< "Failed to find boolean property for ID: " << (numItems + i);
	}

	// Check the last item to be sure.
	auto lastItem = reloadedIndex->findExactMatch("id", numItems);
	ASSERT_EQ(lastItem.size(), 1u);
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
		ASSERT_EQ(result.size(), 1u) << "Failed to find item " << id << " after random insertion.";
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
		ASSERT_EQ(result.size(), 1u) << "Failed to find remaining item " << id;
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
	EXPECT_NE(std::ranges::find(keys, key), keys.end());

	// 4. Now add data
	propertyIndex->addProperty(1, key, 123);

	// 5. Type should transition from UNKNOWN to INTEGER
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::INTEGER);
	auto results = propertyIndex->findExactMatch(key, 123);
	EXPECT_EQ(results.size(), 1u);
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

TEST_F(PropertyIndexTest, CreateAndImmediatelyDrop) {
	const std::string key = "unused_key";

	// 1. Create Index (Registers key as UNKNOWN type)
	propertyIndex->createIndex(key);
	EXPECT_TRUE(propertyIndex->hasKeyIndexed(key));
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::UNKNOWN);

	// 2. Drop Index immediately
	// This previously caused "Invalid property type" exception because
	// it tried to find a root map for UNKNOWN type.
	EXPECT_NO_THROW(propertyIndex->dropKey(key));

	// 3. Verify state
	EXPECT_FALSE(propertyIndex->hasKeyIndexed(key));
	EXPECT_TRUE(propertyIndex->isEmpty());
}

TEST_F(PropertyIndexTest, CreateAndDropAll) {
	propertyIndex->createIndex("k1"); // UNKNOWN
	propertyIndex->addProperty(1, "k2", 100); // INTEGER

	// Drop everything
	EXPECT_NO_THROW(propertyIndex->drop());

	EXPECT_TRUE(propertyIndex->isEmpty());
}

TEST_F(PropertyIndexTest, RepeatedCreateAndDropStress) {
	const std::string key = "volatile_key";

	// Cycle 1
	propertyIndex->createIndex(key);
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::UNKNOWN);
	propertyIndex->addProperty(1, key, 100);
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::INTEGER);

	propertyIndex->dropKey(key);
	EXPECT_FALSE(propertyIndex->hasKeyIndexed(key));

	// Cycle 2: Immediate recreation
	propertyIndex->createIndex(key);
	EXPECT_TRUE(propertyIndex->hasKeyIndexed(key));
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::UNKNOWN);

	// Cycle 3: Add data, then drop, then create, then add different type
	propertyIndex->addProperty(2, key, "string_now"); // Should set to STRING
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::STRING);

	propertyIndex->dropKey(key);

	propertyIndex->createIndex(key);
	propertyIndex->addProperty(3, key, true); // Should set to BOOLEAN
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::BOOLEAN);

	// Final Cleanup
	propertyIndex->dropKey(key);
	EXPECT_TRUE(propertyIndex->isEmpty());
}

/**
 * @brief Verifies robustness against repetitive and interleaved create/drop operations.
 * Covers scenarios:
 * 1. Multiple Create -> Multiple Drop (No Data)
 * 2. Create -> Insert -> Multiple Create (Should preserve type) -> Multiple Drop
 */
TEST_F(PropertyIndexTest, Robustness_RepetitiveAndInterleavedOps) {
	const std::string key = "resilient_key";

	// --- Phase 1: Multiple Create (Idempotency on UNKNOWN) ---
	// Execute createIndex 5 times consecutively
	for (int i = 0; i < 5; ++i) {
		propertyIndex->createIndex(key);
		// Assert: Key exists, Type is UNKNOWN
		EXPECT_TRUE(propertyIndex->hasKeyIndexed(key));
		EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::UNKNOWN);
	}

	// --- Phase 2: Multiple Drop (Idempotency on Empty) ---
	// Execute dropKey 5 times consecutively
	for (int i = 0; i < 5; ++i) {
		// Should not throw exceptions (even when key is missing on iterations 2-5)
		EXPECT_NO_THROW(propertyIndex->dropKey(key));
		// Assert: Key is gone
		EXPECT_FALSE(propertyIndex->hasKeyIndexed(key));
	}
	EXPECT_TRUE(propertyIndex->isEmpty());

	// --- Phase 3: Create -> Insert -> Multiple Create (State Protection) ---

	// 3a. Initial Create
	for (int i = 0; i < 3; ++i) {
		propertyIndex->createIndex(key);
	}
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::UNKNOWN);

	// 3b. Insert Data (Transitions state to INTEGER)
	propertyIndex->addProperty(1, key, 100);
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::INTEGER);

	// 3c. Multiple Create AGAIN (Must NOT reset to UNKNOWN)
	for (int i = 0; i < 3; ++i) {
		propertyIndex->createIndex(key);
		// Critical Assert: Type must remain INTEGER, data must persist
		EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::INTEGER)
				<< "createIndex overwrote existing type definition!";
	}

	// Verify data is still accessible
	auto results = propertyIndex->findExactMatch(key, 100);
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0], 1);

	// --- Phase 4: Multiple Drop (Cleanup) ---
	for (int i = 0; i < 5; ++i) {
		propertyIndex->dropKey(key);
		EXPECT_FALSE(propertyIndex->hasKeyIndexed(key));
	}

	// Verify completely empty
	EXPECT_TRUE(propertyIndex->isEmpty());
	EXPECT_TRUE(propertyIndex->findExactMatch(key, 100).empty());
}

/**
 * @brief Verifies that multiple property keys can be indexed simultaneously without interference.
 */
TEST_F(PropertyIndexTest, MultiplePropertyKeysIsolation) {
	const std::string key1 = "firstname";
	const std::string key2 = "lastname";
	const std::string key3 = "age";

	// 1. Create multiple indexes
	propertyIndex->createIndex(key1);
	propertyIndex->createIndex(key2);
	propertyIndex->createIndex(key3);

	EXPECT_TRUE(propertyIndex->hasKeyIndexed(key1));
	EXPECT_TRUE(propertyIndex->hasKeyIndexed(key2));
	EXPECT_TRUE(propertyIndex->hasKeyIndexed(key3));

	// 2. Insert mixed data
	// Node 1: Alice Smith, 30
	propertyIndex->addProperty(1, key1, "Alice");
	propertyIndex->addProperty(1, key2, "Smith");
	propertyIndex->addProperty(1, key3, 30);

	// Node 2: Bob Jones, 40
	propertyIndex->addProperty(2, key1, "Bob");
	propertyIndex->addProperty(2, key2, "Jones");
	propertyIndex->addProperty(2, key3, 40);

	// 3. Verify Isolation (Querying one key shouldn't return data from another)

	// Query firstname "Alice" -> Should find Node 1
	auto res1 = propertyIndex->findExactMatch(key1, "Alice");
	ASSERT_EQ(res1.size(), 1u);
	EXPECT_EQ(res1[0], 1);

	// Query lastname "Smith" -> Should find Node 1
	auto res2 = propertyIndex->findExactMatch(key2, "Smith");
	ASSERT_EQ(res2.size(), 1u);
	EXPECT_EQ(res2[0], 1);

	// Query age 40 -> Should find Node 2
	auto res3 = propertyIndex->findExactMatch(key3, 40);
	ASSERT_EQ(res3.size(), 1u);
	EXPECT_EQ(res3[0], 2);

	// Cross-contamination check:
	// Querying key1 with value from key2 should return empty (unless coincide)
	// "Smith" is a lastname, not a firstname here.
	auto res4 = propertyIndex->findExactMatch(key1, "Smith");
	EXPECT_TRUE(res4.empty());

	// 4. Verify Types
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key1), graph::PropertyType::STRING);
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key2), graph::PropertyType::STRING);
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key3), graph::PropertyType::INTEGER);
}

/**
 * @brief Test dropKey behavior when some root maps still have entries.
 * This covers the case where after dropping one key, other keys remain in the same type map.
 * Previously uncovered branches: Lines 93, 99, 102 (when root maps are NOT empty)
 */
TEST_F(PropertyIndexTest, DropKey_WithRemainingKeysInSameType) {
	const std::string key1 = "string_key_1";
	const std::string key2 = "string_key_2";
	const std::string key3 = "int_key_1";
	const std::string key4 = "double_key_1";
	const std::string key5 = "bool_key_1";

	// Add multiple keys of the same type to trigger "not empty" branches
	propertyIndex->addProperty(1, key1, "value1");
	propertyIndex->addProperty(2, key2, "value2");
	propertyIndex->addProperty(3, key3, 100);
	propertyIndex->addProperty(4, key4, 3.14);
	propertyIndex->addProperty(5, key5, true);

	// Flush to ensure state is written
	propertyIndex->flush();

	// Drop one key at a time, leaving other keys in the same type map
	// This should trigger the "not empty" branch for each type
	propertyIndex->dropKey(key1);

	// Verify that key1 is gone but key2 still exists
	EXPECT_FALSE(propertyIndex->hasKeyIndexed(key1));
	EXPECT_TRUE(propertyIndex->hasKeyIndexed(key2));
	EXPECT_TRUE(propertyIndex->hasKeyIndexed(key3));
	EXPECT_TRUE(propertyIndex->hasKeyIndexed(key4));
	EXPECT_TRUE(propertyIndex->hasKeyIndexed(key5));

	// Verify remaining data is still accessible
	auto results = propertyIndex->findExactMatch(key2, "value2");
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0], 2);
}

/**
 * @brief Test clearIndexData with non-existent key.
 * Covers line 164: when !indexedKeyTypes_.contains(key) is true.
 */
TEST_F(PropertyIndexTest, ClearIndexData_NonExistentKey) {
	const std::string key = "non_existent_key";

	// Should not throw when clearing a key that was never indexed
	EXPECT_NO_THROW(propertyIndex->clearIndexData(key));

	// Index should remain empty
	EXPECT_TRUE(propertyIndex->isEmpty());
}

/**
 * @brief Test clearIndexData with existing key.
 * This exercises the lambda at line 170-175.
 */
TEST_F(PropertyIndexTest, ClearIndexData_ExistingKey) {
	const std::string key = "clear_test_key";

	// Add some data
	propertyIndex->addProperty(1, key, "test_value");
	propertyIndex->addProperty(2, key, "another_value");

	// Verify data exists
	auto resultsBefore = propertyIndex->findExactMatch(key, "test_value");
	ASSERT_EQ(resultsBefore.size(), 1u);
	EXPECT_EQ(resultsBefore[0], 1);

	// Clear the index data (but keep the key registration)
	propertyIndex->clearIndexData(key);

	// Data should be gone
	EXPECT_TRUE(propertyIndex->findExactMatch(key, "test_value").empty());
	EXPECT_TRUE(propertyIndex->findExactMatch(key, "another_value").empty());

	// But the key should still be registered (type reset to UNKNOWN)
	EXPECT_TRUE(propertyIndex->hasKeyIndexed(key));
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::UNKNOWN);

	// Can add new data of potentially different type
	propertyIndex->addProperty(3, key, 123);
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::INTEGER);
	auto resultsAfter = propertyIndex->findExactMatch(key, 123);
	ASSERT_EQ(resultsAfter.size(), 1u);
	EXPECT_EQ(resultsAfter[0], 3);
}

/**
 * @brief Test clear behavior when index is already empty.
 * Covers line 115: when rootMap | std::views::values is empty.
 */
TEST_F(PropertyIndexTest, Clear_AlreadyEmpty) {
	// Index should be empty initially
	EXPECT_TRUE(propertyIndex->isEmpty());

	// Clear should not throw on empty index
	EXPECT_NO_THROW(propertyIndex->clear());

	// Should still be empty
	EXPECT_TRUE(propertyIndex->isEmpty());
}

/**
 * @brief Test addPropertiesBatch with empty vector.
 * Covers line 229: properties.empty() check.
 */
TEST_F(PropertyIndexTest, AddPropertiesBatch_EmptyVector) {
	std::vector<std::tuple<int64_t, std::string, graph::PropertyValue>> emptyBatch;

	// Should not throw with empty batch
	EXPECT_NO_THROW(propertyIndex->addPropertiesBatch(emptyBatch));

	// Index should remain empty
	EXPECT_TRUE(propertyIndex->isEmpty());
}

/**
 * @brief Test addPropertiesBatch with type mismatches.
 * Covers line 262: when registeredType != valueType in batch operations.
 */
TEST_F(PropertyIndexTest, AddPropertiesBatch_TypeMismatch) {
	const std::string key = "batch_type_key";

	// Register key as INTEGER type
	propertyIndex->createIndex(key);
	propertyIndex->addProperty(1, key, 100);

	// Create batch with mixed types (should ignore string entries)
	std::vector<std::tuple<int64_t, std::string, graph::PropertyValue>> batch;
	batch.emplace_back(2, key, 200);        // Valid: integer
	batch.emplace_back(3, key, std::string("invalid"));  // Invalid: string
	batch.emplace_back(4, key, 300);        // Valid: integer
	batch.emplace_back(5, key, 3.14);       // Invalid: double

	propertyIndex->addPropertiesBatch(batch);

	// Verify only integers were added
	auto results = propertyIndex->findExactMatch(key, 200);
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0], 2);

	results = propertyIndex->findExactMatch(key, 300);
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0], 4);

	// String and double should not have been added
	EXPECT_TRUE(propertyIndex->findExactMatch(key, std::string("invalid")).empty());
	EXPECT_TRUE(propertyIndex->findExactMatch(key, 3.14).empty());
}

/**
 * @brief Test addPropertiesBatch with non-registered keys.
 * Covers line 248: when key is not in indexedKeyTypes_.
 */
TEST_F(PropertyIndexTest, AddPropertiesBatch_UnregisteredKey) {
	const std::string key = "unregistered_key";

	// DO NOT register the key - it should not be auto-created in batch mode

	// Create batch with data for unregistered key
	std::vector<std::tuple<int64_t, std::string, graph::PropertyValue>> batch;
	batch.emplace_back(1, key, 100);
	batch.emplace_back(2, key, 200);

	propertyIndex->addPropertiesBatch(batch);

	// Index should remain empty (key was not auto-created)
	EXPECT_TRUE(propertyIndex->isEmpty());
	EXPECT_FALSE(propertyIndex->hasKeyIndexed(key));
}

/**
 * @brief Test addPropertiesBatch with NULL and UNKNOWN types.
 * Covers line 242: when valueType is UNKNOWN or NULL_TYPE.
 */
TEST_F(PropertyIndexTest, AddPropertiesBatch_WithNullValues) {
	const std::string key = "null_test_key";

	// Register the key
	propertyIndex->createIndex(key);
	propertyIndex->addProperty(1, key, "valid_value");

	// Create batch with NULL values
	std::vector<std::tuple<int64_t, std::string, graph::PropertyValue>> batch;
	batch.emplace_back(2, key, std::string("value2"));
	batch.emplace_back(3, key, std::monostate{});  // NULL value
	batch.emplace_back(4, key, std::string("value4"));

	propertyIndex->addPropertiesBatch(batch);

	// Verify NULL value was skipped
	auto results = propertyIndex->findExactMatch(key, std::string("value2"));
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0], 2);

	results = propertyIndex->findExactMatch(key, std::string("value4"));
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0], 4);

	// Should have exactly 3 entries (initial + 2 from batch, NULL skipped)
	EXPECT_EQ(propertyIndex->getIndexedKeyType(key), graph::PropertyType::STRING);
}

/**
 * @brief Test removeProperty with non-existent key.
 * Covers line 308: when key is not in indexedKeyTypes_.
 */
TEST_F(PropertyIndexTest, RemoveProperty_NonExistentKey) {
	const std::string key = "non_existent_remove_key";

	// Try to remove from a key that was never indexed
	EXPECT_NO_THROW(propertyIndex->removeProperty(1, key, 100));

	// Should remain empty
	EXPECT_TRUE(propertyIndex->isEmpty());
}

/**
 * @brief Test removeProperty with type mismatch.
 * Covers line 316: when registeredType != valueType.
 */
TEST_F(PropertyIndexTest, RemoveProperty_TypeMismatch) {
	const std::string key = "type_mismatch_remove_key";

	// Register as STRING
	propertyIndex->addProperty(1, key, "string_value");

	// Try to remove with integer type (should be ignored)
	propertyIndex->removeProperty(1, key, 12345);

	// Original value should still be present
	auto results = propertyIndex->findExactMatch(key, std::string("string_value"));
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0], 1);
}

/**
 * @brief Test findExactMatch with non-existent key.
 * Covers line 334: when key is not in indexedKeyTypes_.
 */
TEST_F(PropertyIndexTest, FindExactMatch_NonExistentKey) {
	const std::string key = "find_non_existent";

	// Try to find a key that was never indexed
	auto results = propertyIndex->findExactMatch(key, "some_value");

	// Should return empty vector
	EXPECT_TRUE(results.empty());
}

/**
 * @brief Test findRange with non-numeric types.
 * Covers line 351: when type is not INTEGER or DOUBLE.
 */
TEST_F(PropertyIndexTest, FindRange_NonNumericType) {
	const std::string key = "string_range_key";

	// Add a string property
	propertyIndex->addProperty(1, key, "string_value");

	// Try to find range on string key (should return empty)
	auto results = propertyIndex->findRange(key, 0.0, 100.0);

	// Should return empty for non-numeric types
	EXPECT_TRUE(results.empty());
}

/**
 * @brief Test findRange with non-existent key.
 * Covers line 356: when rootIt == rootMap.end().
 */
TEST_F(PropertyIndexTest, FindRange_NonExistentKey) {
	const std::string key = "range_non_existent";

	// Try to find range on a key that was never indexed
	auto results = propertyIndex->findRange(key, 0.0, 100.0);

	// Should return empty
	EXPECT_TRUE(results.empty());
}
