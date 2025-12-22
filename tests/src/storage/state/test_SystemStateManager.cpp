/**
 * @file test_SystemStateManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/15
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
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

class SystemStateManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_stateMgr_" + to_string(uuid) + ".dat");

		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();

		// Initialize the manager under test
		stateManager =
				std::make_unique<graph::storage::state::SystemStateManager>(database->getStorage()->getDataManager());
	}

	void TearDown() override {
		database->close();
		database.reset();
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath);
		}
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::unique_ptr<graph::storage::state::SystemStateManager> stateManager;
};

// Test 1: Scalar Types (Int64)
TEST_F(SystemStateManagerTest, SetAndGetInt64) {
	const std::string key = "config.test.int";
	const std::string field = "max_size";
	const int64_t expectedValue = 1024;

	// Default value check
	EXPECT_EQ(stateManager->get<int64_t>(key, field, -1), -1);

	// Set value
	stateManager->set<int64_t>(key, field, expectedValue);

	// Get value
	EXPECT_EQ(stateManager->get<int64_t>(key, field, -1), expectedValue);
}

// Test 2: Scalar Types (Bool)
TEST_F(SystemStateManagerTest, SetAndGetBool) {
	const std::string key = "config.test.bool";
	const std::string field = "is_enabled";

	EXPECT_FALSE(stateManager->get<bool>(key, field, false));

	stateManager->set<bool>(key, field, true);
	EXPECT_TRUE(stateManager->get<bool>(key, field, false));

	stateManager->set<bool>(key, field, false);
	EXPECT_FALSE(stateManager->get<bool>(key, field, true));
}

// Test 3: Scalar Types (String)
TEST_F(SystemStateManagerTest, SetAndGetString) {
	const std::string key = "config.test.string";
	const std::string field = "db_name";
	const std::string value = "my_graph_db";

	EXPECT_EQ(stateManager->get<std::string>(key, field, "default"), "default");

	stateManager->set<std::string>(key, field, value);
	EXPECT_EQ(stateManager->get<std::string>(key, field, "default"), value);
}

// Test 4: Scalar Types (Double)
TEST_F(SystemStateManagerTest, SetAndGetDouble) {
	const std::string key = "config.test.double";
	const std::string field = "ratio";
	const double value = 0.75;

	// Use epsilon for float comparison just in case, though direct double storage should be exact
	EXPECT_DOUBLE_EQ(stateManager->get<double>(key, field, 0.0), 0.0);

	stateManager->set<double>(key, field, value);
	EXPECT_DOUBLE_EQ(stateManager->get<double>(key, field, 0.0), value);
}

// Test 5: Map Operations (String -> Int64)
TEST_F(SystemStateManagerTest, SetAndGetMap) {
	const std::string key = "index.roots";
	std::unordered_map<std::string, int64_t> inputMap = {{"age", 100}, {"salary", 200}, {"id", 300}};

	// Initial state check
	auto emptyMap = stateManager->getMap<int64_t>(key);
	EXPECT_TRUE(emptyMap.empty());

	// Save map
	stateManager->setMap(key, inputMap);

	// Load map
	auto loadedMap = stateManager->getMap<int64_t>(key);
	ASSERT_EQ(loadedMap.size(), 3UL);
	EXPECT_EQ(loadedMap["age"], 100);
	EXPECT_EQ(loadedMap["salary"], 200);
	EXPECT_EQ(loadedMap["id"], 300);
}

// Test 6: Map Update and Removal
TEST_F(SystemStateManagerTest, UpdateAndRemoveMap) {
	const std::string key = "index.roots.update";
	std::unordered_map<std::string, int64_t> initialMap = {{"A", 1}, {"B", 2}};

	stateManager->setMap(key, initialMap);

	// Update: Remove B, Add C
	std::unordered_map<std::string, int64_t> updatedMap = {{"A", 1}, {"C", 3}};
	stateManager->setMap(key, updatedMap);

	auto result = stateManager->getMap<int64_t>(key);
	ASSERT_EQ(result.size(), 2UL);
	EXPECT_EQ(result["A"], 1);
	EXPECT_EQ(result["C"], 3);
	EXPECT_FALSE(result.contains("B")); // Important: Verify B is gone (map replacement logic)

	// Set empty map -> Should remove state
	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{});
	EXPECT_TRUE(stateManager->getMap<int64_t>(key).empty());
}

// Test 7: Persistence (Write to disk -> Reload)
TEST_F(SystemStateManagerTest, Persistence) {
	const std::string key = "persistent.config";
	const std::string field = "ver";
	const int64_t val = 12345;

	stateManager->set<int64_t>(key, field, val);

	// Flush to disk
	database->getStorage()->flush();
	database->close();

	// Reload
	auto newDb = std::make_unique<graph::Database>(testFilePath.string());
	newDb->open();
	auto newManager =
			std::make_unique<graph::storage::state::SystemStateManager>(newDb->getStorage()->getDataManager());

	// Verify
	EXPECT_EQ(newManager->get<int64_t>(key, field, 0), val);
}

// Test 8: Type Mismatch Safety
TEST_F(SystemStateManagerTest, TypeMismatchReturnsDefault) {
	const std::string key = "mismatch.test";
	const std::string field = "val";

	// Store as String
	stateManager->set<std::string>(key, field, "not a number");

	// Try to read as Int64
	int64_t result = stateManager->get<int64_t>(key, field, 999);

	// Should return default because underlying type is string, not int/variant compatible
	EXPECT_EQ(result, 999);
}

// Test 9: Lifecycle Remove
TEST_F(SystemStateManagerTest, RemoveState) {
	const std::string key = "remove.me";
	stateManager->set<int64_t>(key, "val", 1);
	EXPECT_EQ(stateManager->get<int64_t>(key, "val", 0), 1);

	stateManager->remove(key);

	// Should be gone
	EXPECT_EQ(stateManager->get<int64_t>(key, "val", 0), 0);
}

TEST_F(SystemStateManagerTest, MergeVsReplace) {
	const std::string key = "test.mode";

	// Initial Setup
	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{{"A", 1}, {"B", 2}},
						 graph::storage::state::UpdateMode::REPLACE);

	// Test MERGE: Update A, Add C, Keep B
	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{{"A", 10}, {"C", 30}},
						 graph::storage::state::UpdateMode::MERGE);

	auto res = stateManager->getMap<int64_t>(key);
	EXPECT_EQ(res["A"], 10); // Updated
	EXPECT_EQ(res["B"], 2); // Kept
	EXPECT_EQ(res["C"], 30); // Added

	// Test REPLACE: Only D remains
	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{{"D", 40}},
						 graph::storage::state::UpdateMode::REPLACE);

	res = stateManager->getMap<int64_t>(key);
	EXPECT_EQ(res.size(), 1UL);
	EXPECT_EQ(res["D"], 40);
	EXPECT_FALSE(res.contains("A"));
}

// Test 10: Verify "No-Op" Optimization (Dirty Check)
// Ensures that writing the same value does not trigger a disk write.
TEST_F(SystemStateManagerTest, Optimization_NoWriteIfSame) {
	const std::string key = "opt.test";

	// 1. Initial Write
	stateManager->set<int64_t>(key, "val", 100);

	// Flush to clear dirty flags in DataManager/PersistenceManager
	database->getStorage()->flush();
	ASSERT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// 2. Set SAME value (Scalar)
	// Should be optimized out
	stateManager->set<int64_t>(key, "val", 100);
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges())
		<< "Optimization Failed: Setting same scalar value triggered a dirty state.";

	// 3. Set DIFFERENT value
	stateManager->set<int64_t>(key, "val", 101);
	EXPECT_TRUE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Flush again
	database->getStorage()->flush();

	// 4. Test Map Optimization (MERGE mode)
	std::unordered_map<std::string, int64_t> sameMap = {{"val", 101}};
	stateManager->setMap(key, sameMap, graph::storage::state::UpdateMode::MERGE);
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges())
		<< "Optimization Failed: Merging identical map triggered a dirty state.";

	// 5. Test Map Optimization (REPLACE mode)
	// In REPLACE mode, same content should also be optimized
	stateManager->setMap(key, sameMap, graph::storage::state::UpdateMode::REPLACE);
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges())
		<< "Optimization Failed: Replacing with identical map triggered a dirty state.";
}