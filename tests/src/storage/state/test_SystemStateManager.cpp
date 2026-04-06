/**
 * @file test_SystemStateManager.cpp
 * @author Nexepic
 * @date 2025/12/15
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
		std::error_code ec;
		if (std::filesystem::exists(testFilePath)) {
			std::filesystem::remove(testFilePath, ec);
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

// Test 11: GetAll returns all properties
TEST_F(SystemStateManagerTest, GetAll) {
	const std::string key = "getall.test";

	stateManager->set<int64_t>(key, "a", 1);
	stateManager->set<std::string>(key, "b", "hello");

	auto all = stateManager->getAll(key);
	EXPECT_GE(all.size(), 2UL);
	EXPECT_TRUE(all.contains("a"));
	EXPECT_TRUE(all.contains("b"));
}

// Test 12: Map REPLACE with different sized maps
TEST_F(SystemStateManagerTest, ReplaceMode_DifferentSizeMap) {
	const std::string key = "replace.size";

	// Set a map with 3 entries
	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{{"A", 1}, {"B", 2}, {"C", 3}},
						 graph::storage::state::UpdateMode::REPLACE);

	// Replace with a different sized map (2 entries)
	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{{"X", 10}, {"Y", 20}},
						 graph::storage::state::UpdateMode::REPLACE);

	auto result = stateManager->getMap<int64_t>(key);
	EXPECT_EQ(result.size(), 2UL);
	EXPECT_EQ(result["X"], 10);
	EXPECT_EQ(result["Y"], 20);
	EXPECT_FALSE(result.contains("A"));
}

// Test 13: Empty map REPLACE with non-empty existing
TEST_F(SystemStateManagerTest, ReplaceMode_EmptyMapClearsExisting) {
	const std::string key = "replace.empty";

	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{{"A", 1}},
						 graph::storage::state::UpdateMode::REPLACE);

	// Replace with empty map
	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{},
						 graph::storage::state::UpdateMode::REPLACE);

	auto result = stateManager->getMap<int64_t>(key);
	EXPECT_TRUE(result.empty());
}

// Test 14: Empty map MERGE is a no-op
TEST_F(SystemStateManagerTest, MergeMode_EmptyMapIsNoop) {
	const std::string key = "merge.empty";

	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{{"A", 1}},
						 graph::storage::state::UpdateMode::REPLACE);

	// Merge with empty map - should not change anything
	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{},
						 graph::storage::state::UpdateMode::MERGE);

	auto result = stateManager->getMap<int64_t>(key);
	EXPECT_EQ(result.size(), 1UL);
	EXPECT_EQ(result["A"], 1);
}

// Test 15: GetMap with string values
TEST_F(SystemStateManagerTest, GetMap_StringValues) {
	const std::string key = "map.strings";

	std::unordered_map<std::string, std::string> map;
	map["name"] = "Alice";
	map["city"] = "Boston";
	stateManager->setMap(key, map);

	auto result = stateManager->getMap<std::string>(key);
	EXPECT_EQ(result.size(), 2UL);
	EXPECT_EQ(result["name"], "Alice");
	EXPECT_EQ(result["city"], "Boston");
}

// Test 16: Bool stored as int64_t retrieval
TEST_F(SystemStateManagerTest, BoolFromInt64) {
	const std::string key = "bool.int64";

	// Store as int64_t
	stateManager->set<int64_t>(key, "flag", 1);

	// Read as bool - should extract from int64_t
	bool result = stateManager->get<bool>(key, "flag", false);
	EXPECT_TRUE(result);

	// Store 0
	stateManager->set<int64_t>(key, "flag", 0);
	result = stateManager->get<bool>(key, "flag", true);
	EXPECT_FALSE(result);
}

// Test 17: REPLACE mode same-content different-key optimization
TEST_F(SystemStateManagerTest, ReplaceMode_SameContentSameSize) {
	const std::string key = "replace.samesize";

	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{{"A", 1}, {"B", 2}},
						 graph::storage::state::UpdateMode::REPLACE);

	database->getStorage()->flush();
	ASSERT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Replace with same size but different content
	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{{"A", 1}, {"B", 99}},
						 graph::storage::state::UpdateMode::REPLACE);

	EXPECT_TRUE(database->getStorage()->getDataManager()->hasUnsavedChanges());
}

// Test 18: Replace with empty existing state (noop)
TEST_F(SystemStateManagerTest, ReplaceMode_EmptyExistingState) {
	const std::string key = "replace.no.existing";

	// Replace empty state with empty map - should be no-op
	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{},
						 graph::storage::state::UpdateMode::REPLACE);

	auto result = stateManager->getMap<int64_t>(key);
	EXPECT_TRUE(result.empty());
}

// Test 19: Double type mismatch returns default (covers extract<double> nullopt branch)
TEST_F(SystemStateManagerTest, DoubleMismatchReturnsDefault) {
	const std::string key = "mismatch.double";
	const std::string field = "val";

	// Store as string, try to read as double
	stateManager->set<std::string>(key, field, "not_a_double");
	double result = stateManager->get<double>(key, field, 3.14);
	EXPECT_DOUBLE_EQ(result, 3.14);
}

// Test 20: String type mismatch returns default (covers extract<string> nullopt branch)
TEST_F(SystemStateManagerTest, StringMismatchReturnsDefault) {
	const std::string key = "mismatch.string";
	const std::string field = "val";

	// Store as int64_t, try to read as string
	stateManager->set<int64_t>(key, field, 42);
	std::string result = stateManager->get<std::string>(key, field, "fallback");
	EXPECT_EQ(result, "fallback");
}

// Test 21: Bool extract from non-bool non-int type returns default (covers extract<bool> final nullopt)
TEST_F(SystemStateManagerTest, BoolFromStringReturnsDefault) {
	const std::string key = "mismatch.bool";
	const std::string field = "val";

	// Store as string - neither bool nor int64_t
	stateManager->set<std::string>(key, field, "true_string");
	bool result = stateManager->get<bool>(key, field, false);
	EXPECT_FALSE(result);
}

// Test 22: Bool MERGE mode no-op when same value (covers setMap<bool> MERGE same-value branch)
TEST_F(SystemStateManagerTest, BoolMergeNoop) {
	const std::string key = "bool.merge.noop";
	const std::string field = "flag";

	// Set initial value
	stateManager->set<bool>(key, field, true);

	// Flush to clear dirty state
	database->getStorage()->flush();
	ASSERT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Set same value again - should be a no-op (covers the it->second != newVal false branch)
	stateManager->set<bool>(key, field, true);
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges())
			<< "Setting same bool value should not trigger dirty state.";
}

// Test 23: Double MERGE mode no-op when same value (covers setMap<double> MERGE same-value branch)
TEST_F(SystemStateManagerTest, DoubleMergeNoop) {
	const std::string key = "double.merge.noop";
	const std::string field = "ratio";

	// Set initial value
	stateManager->set<double>(key, field, 2.718);

	// Flush
	database->getStorage()->flush();
	ASSERT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Set same value again
	stateManager->set<double>(key, field, 2.718);
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges())
			<< "Setting same double value should not trigger dirty state.";
}

// Test 24: Double MERGE mode detects changed value
TEST_F(SystemStateManagerTest, DoubleMergeDetectsChange) {
	const std::string key = "double.merge.change";
	const std::string field = "ratio";

	stateManager->set<double>(key, field, 1.0);
	database->getStorage()->flush();

	// Set a different value
	stateManager->set<double>(key, field, 2.0);
	EXPECT_TRUE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Verify new value
	EXPECT_DOUBLE_EQ(stateManager->get<double>(key, field, 0.0), 2.0);
}

// Test 25: PropVec (vector<PropertyValue>) get/set (covers extract<PropVec> instantiation)
TEST_F(SystemStateManagerTest, SetAndGetPropVec) {
	const std::string key = "propvec.test";
	const std::string field = "items";

	using PropVec = std::vector<graph::PropertyValue>;
	PropVec input;
	input.emplace_back(static_cast<int64_t>(1));
	input.emplace_back(static_cast<int64_t>(2));
	input.emplace_back(static_cast<int64_t>(3));

	// Default empty vector
	PropVec defaultVec;
	auto result = stateManager->get<PropVec>(key, field, defaultVec);
	EXPECT_TRUE(result.empty());

	// Set and retrieve
	stateManager->set<PropVec>(key, field, input);
	result = stateManager->get<PropVec>(key, field, defaultVec);
	ASSERT_EQ(result.size(), 3UL);
}

// Test 26: PropVec map operations (covers getMap<PropVec> and setMap<PropVec>)
TEST_F(SystemStateManagerTest, PropVecMapOperations) {
	const std::string key = "propvec.map";

	using PropVec = std::vector<graph::PropertyValue>;

	PropVec vec1;
	vec1.emplace_back(static_cast<int64_t>(10));
	PropVec vec2;
	vec2.emplace_back(std::string("hello"));

	std::unordered_map<std::string, PropVec> inputMap;
	inputMap["nums"] = vec1;
	inputMap["strs"] = vec2;

	stateManager->setMap(key, inputMap, graph::storage::state::UpdateMode::REPLACE);

	auto loaded = stateManager->getMap<PropVec>(key);
	ASSERT_EQ(loaded.size(), 2UL);
	EXPECT_TRUE(loaded.contains("nums"));
	EXPECT_TRUE(loaded.contains("strs"));
}

// Test 27: PropVec MERGE mode (covers setMap<PropVec> MERGE path)
TEST_F(SystemStateManagerTest, PropVecMapMerge) {
	const std::string key = "propvec.merge";

	using PropVec = std::vector<graph::PropertyValue>;

	PropVec vec1;
	vec1.emplace_back(static_cast<int64_t>(1));
	PropVec vec2;
	vec2.emplace_back(static_cast<int64_t>(2));

	std::unordered_map<std::string, PropVec> initial;
	initial["a"] = vec1;
	stateManager->setMap(key, initial, graph::storage::state::UpdateMode::REPLACE);

	// Merge in a new key
	std::unordered_map<std::string, PropVec> merge;
	merge["b"] = vec2;
	stateManager->setMap(key, merge, graph::storage::state::UpdateMode::MERGE);

	auto loaded = stateManager->getMap<PropVec>(key);
	EXPECT_EQ(loaded.size(), 2UL);
	EXPECT_TRUE(loaded.contains("a"));
	EXPECT_TRUE(loaded.contains("b"));
}

// Test 28: String map REPLACE mode with different content same size
TEST_F(SystemStateManagerTest, StringMapReplaceSameSizeDifferentContent) {
	const std::string key = "string.replace.diff";

	std::unordered_map<std::string, std::string> map1;
	map1["k1"] = "v1";
	map1["k2"] = "v2";
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	database->getStorage()->flush();

	// Replace with same size but different values - triggers the REPLACE comparison loop
	std::unordered_map<std::string, std::string> map2;
	map2["k1"] = "v1";
	map2["k2"] = "changed";
	stateManager->setMap(key, map2, graph::storage::state::UpdateMode::REPLACE);

	EXPECT_TRUE(database->getStorage()->getDataManager()->hasUnsavedChanges());
	auto result = stateManager->getMap<std::string>(key);
	EXPECT_EQ(result["k2"], "changed");
}

// Test 29: String map REPLACE same content is no-op
TEST_F(SystemStateManagerTest, StringMapReplaceSameContentNoop) {
	const std::string key = "string.replace.same";

	std::unordered_map<std::string, std::string> map1;
	map1["k1"] = "v1";
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	database->getStorage()->flush();
	ASSERT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Replace with identical content
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges())
			<< "Replacing with identical string map should not trigger dirty state.";
}

// Test 30: String map REPLACE with different size map
TEST_F(SystemStateManagerTest, StringMapReplaceDifferentSize) {
	const std::string key = "string.replace.size";

	std::unordered_map<std::string, std::string> map1;
	map1["a"] = "1";
	map1["b"] = "2";
	map1["c"] = "3";
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	// Replace with smaller map
	std::unordered_map<std::string, std::string> map2;
	map2["x"] = "10";
	stateManager->setMap(key, map2, graph::storage::state::UpdateMode::REPLACE);

	auto result = stateManager->getMap<std::string>(key);
	EXPECT_EQ(result.size(), 1UL);
	EXPECT_EQ(result["x"], "10");
}

// Test 31: String map MERGE mode
TEST_F(SystemStateManagerTest, StringMapMerge) {
	const std::string key = "string.merge";

	std::unordered_map<std::string, std::string> initial;
	initial["name"] = "Alice";
	stateManager->setMap(key, initial, graph::storage::state::UpdateMode::REPLACE);

	// Merge additional key
	std::unordered_map<std::string, std::string> extra;
	extra["city"] = "Boston";
	stateManager->setMap(key, extra, graph::storage::state::UpdateMode::MERGE);

	auto result = stateManager->getMap<std::string>(key);
	EXPECT_EQ(result.size(), 2UL);
	EXPECT_EQ(result["name"], "Alice");
	EXPECT_EQ(result["city"], "Boston");
}

// Test 32: Empty map REPLACE with non-empty existing for string type
TEST_F(SystemStateManagerTest, StringMapEmptyReplaceClearsExisting) {
	const std::string key = "string.empty.replace";

	std::unordered_map<std::string, std::string> map1;
	map1["a"] = "1";
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	// Replace with empty
	stateManager->setMap(key, std::unordered_map<std::string, std::string>{},
						 graph::storage::state::UpdateMode::REPLACE);

	auto result = stateManager->getMap<std::string>(key);
	EXPECT_TRUE(result.empty());
}

// Test 33: Empty map MERGE for string type is no-op
TEST_F(SystemStateManagerTest, StringMapEmptyMergeNoop) {
	const std::string key = "string.empty.merge";

	std::unordered_map<std::string, std::string> map1;
	map1["a"] = "1";
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	// Merge with empty - should keep existing
	stateManager->setMap(key, std::unordered_map<std::string, std::string>{},
						 graph::storage::state::UpdateMode::MERGE);

	auto result = stateManager->getMap<std::string>(key);
	EXPECT_EQ(result.size(), 1UL);
	EXPECT_EQ(result["a"], "1");
}

// Test 34: REPLACE mode with key not found in existing map (covers it == currentProps.end() in REPLACE comparison)
TEST_F(SystemStateManagerTest, ReplaceModeMissingKeyInExisting) {
	const std::string key = "replace.missing.key";

	// Set initial map with 2 entries
	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{{"A", 1}, {"B", 2}},
						 graph::storage::state::UpdateMode::REPLACE);

	database->getStorage()->flush();

	// Replace with same size map but different keys - triggers key-not-found branch
	stateManager->setMap(key, std::unordered_map<std::string, int64_t>{{"A", 1}, {"C", 2}},
						 graph::storage::state::UpdateMode::REPLACE);

	EXPECT_TRUE(database->getStorage()->getDataManager()->hasUnsavedChanges());
	auto result = stateManager->getMap<int64_t>(key);
	EXPECT_EQ(result.size(), 2UL);
	EXPECT_TRUE(result.contains("A"));
	EXPECT_TRUE(result.contains("C"));
	EXPECT_FALSE(result.contains("B"));
}

// Test 35: PropVec REPLACE with same content is no-op
TEST_F(SystemStateManagerTest, PropVecReplaceNoop) {
	const std::string key = "propvec.replace.noop";
	using PropVec = std::vector<graph::PropertyValue>;

	PropVec vec1;
	vec1.emplace_back(static_cast<int64_t>(42));

	std::unordered_map<std::string, PropVec> map1;
	map1["data"] = vec1;
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	database->getStorage()->flush();
	ASSERT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Replace with identical content
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());
}

// Test 36: PropVec empty REPLACE clears existing
TEST_F(SystemStateManagerTest, PropVecEmptyReplace) {
	const std::string key = "propvec.empty.replace";
	using PropVec = std::vector<graph::PropertyValue>;

	PropVec vec1;
	vec1.emplace_back(static_cast<int64_t>(1));

	std::unordered_map<std::string, PropVec> map1;
	map1["data"] = vec1;
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	// Replace with empty
	stateManager->setMap(key, std::unordered_map<std::string, PropVec>{},
						 graph::storage::state::UpdateMode::REPLACE);

	auto result = stateManager->getMap<PropVec>(key);
	EXPECT_TRUE(result.empty());
}

// Test 37: PropVec type mismatch returns default (covers extract<PropVec> nullopt)
TEST_F(SystemStateManagerTest, PropVecMismatchReturnsDefault) {
	const std::string key = "propvec.mismatch";
	const std::string field = "val";
	using PropVec = std::vector<graph::PropertyValue>;

	// Store as int64_t, try to read as PropVec
	stateManager->set<int64_t>(key, field, 42);
	PropVec defaultVec;
	auto result = stateManager->get<PropVec>(key, field, defaultVec);
	EXPECT_TRUE(result.empty());
}

// Test 38: PropertyValue map operations (covers setMap<PropertyValue>)
TEST_F(SystemStateManagerTest, PropertyValueMapOperations) {
	const std::string key = "pv.map";

	std::unordered_map<std::string, graph::PropertyValue> inputMap;
	inputMap["int_val"] = graph::PropertyValue(static_cast<int64_t>(100));
	inputMap["str_val"] = graph::PropertyValue(std::string("hello"));
	inputMap["bool_val"] = graph::PropertyValue(true);

	stateManager->setMap(key, inputMap, graph::storage::state::UpdateMode::REPLACE);

	auto all = stateManager->getAll(key);
	EXPECT_EQ(all.size(), 3UL);
	EXPECT_TRUE(all.contains("int_val"));
	EXPECT_TRUE(all.contains("str_val"));
	EXPECT_TRUE(all.contains("bool_val"));
}

// Test 39: PropertyValue map MERGE mode
TEST_F(SystemStateManagerTest, PropertyValueMapMerge) {
	const std::string key = "pv.merge";

	std::unordered_map<std::string, graph::PropertyValue> initial;
	initial["a"] = graph::PropertyValue(static_cast<int64_t>(1));
	stateManager->setMap(key, initial, graph::storage::state::UpdateMode::REPLACE);

	std::unordered_map<std::string, graph::PropertyValue> merge;
	merge["b"] = graph::PropertyValue(std::string("two"));
	stateManager->setMap(key, merge, graph::storage::state::UpdateMode::MERGE);

	auto all = stateManager->getAll(key);
	EXPECT_EQ(all.size(), 2UL);
	EXPECT_TRUE(all.contains("a"));
	EXPECT_TRUE(all.contains("b"));
}

// Test 40: PropertyValue map empty REPLACE
TEST_F(SystemStateManagerTest, PropertyValueMapEmptyReplace) {
	const std::string key = "pv.empty.replace";

	std::unordered_map<std::string, graph::PropertyValue> initial;
	initial["x"] = graph::PropertyValue(static_cast<int64_t>(1));
	stateManager->setMap(key, initial, graph::storage::state::UpdateMode::REPLACE);

	// Replace with empty
	stateManager->setMap(key, std::unordered_map<std::string, graph::PropertyValue>{},
						 graph::storage::state::UpdateMode::REPLACE);

	auto all = stateManager->getAll(key);
	EXPECT_TRUE(all.empty());
}

// Test 41: getMap with mixed types filters out non-matching entries (covers val.has_value() false branch)
TEST_F(SystemStateManagerTest, GetMapFiltersMismatchedTypes) {
	const std::string key = "mixed.types.map";

	// Store mixed types using PropertyValue map
	std::unordered_map<std::string, graph::PropertyValue> mixedMap;
	mixedMap["int_key"] = graph::PropertyValue(static_cast<int64_t>(42));
	mixedMap["str_key"] = graph::PropertyValue(std::string("hello"));
	mixedMap["bool_key"] = graph::PropertyValue(true);
	stateManager->setMap(key, mixedMap, graph::storage::state::UpdateMode::REPLACE);

	// getMap<int64_t> should only return the int entry
	auto intMap = stateManager->getMap<int64_t>(key);
	EXPECT_EQ(intMap.size(), 1UL);
	EXPECT_EQ(intMap["int_key"], 42);

	// getMap<string> should only return the string entry
	auto strMap = stateManager->getMap<std::string>(key);
	EXPECT_EQ(strMap.size(), 1UL);
	EXPECT_EQ(strMap["str_key"], "hello");
}

// Test 42: PropertyValue map REPLACE same-size same-content is no-op
TEST_F(SystemStateManagerTest, PropertyValueMapReplaceSameContentNoop) {
	const std::string key = "pv.replace.noop";

	std::unordered_map<std::string, graph::PropertyValue> map1;
	map1["a"] = graph::PropertyValue(static_cast<int64_t>(1));
	map1["b"] = graph::PropertyValue(std::string("two"));
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	database->getStorage()->flush();
	ASSERT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Replace with identical content - should be no-op
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges())
			<< "Replacing PropertyValue map with identical content should not trigger dirty state.";
}

// Test 43: PropertyValue map REPLACE same-size different content triggers write
TEST_F(SystemStateManagerTest, PropertyValueMapReplaceSameSizeDifferent) {
	const std::string key = "pv.replace.diff";

	std::unordered_map<std::string, graph::PropertyValue> map1;
	map1["a"] = graph::PropertyValue(static_cast<int64_t>(1));
	map1["b"] = graph::PropertyValue(static_cast<int64_t>(2));
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	database->getStorage()->flush();

	// Replace with same size but different value
	std::unordered_map<std::string, graph::PropertyValue> map2;
	map2["a"] = graph::PropertyValue(static_cast<int64_t>(1));
	map2["b"] = graph::PropertyValue(static_cast<int64_t>(99));
	stateManager->setMap(key, map2, graph::storage::state::UpdateMode::REPLACE);

	EXPECT_TRUE(database->getStorage()->getDataManager()->hasUnsavedChanges());
}

// Test 44: PropertyValue map REPLACE different size triggers write
TEST_F(SystemStateManagerTest, PropertyValueMapReplaceDifferentSize) {
	const std::string key = "pv.replace.size";

	std::unordered_map<std::string, graph::PropertyValue> map1;
	map1["a"] = graph::PropertyValue(static_cast<int64_t>(1));
	map1["b"] = graph::PropertyValue(static_cast<int64_t>(2));
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	// Replace with different size
	std::unordered_map<std::string, graph::PropertyValue> map2;
	map2["x"] = graph::PropertyValue(static_cast<int64_t>(10));
	stateManager->setMap(key, map2, graph::storage::state::UpdateMode::REPLACE);

	auto all = stateManager->getAll(key);
	EXPECT_EQ(all.size(), 1UL);
	EXPECT_TRUE(all.contains("x"));
}

// Test 45: PropertyValue map empty MERGE is no-op
TEST_F(SystemStateManagerTest, PropertyValueMapEmptyMerge) {
	const std::string key = "pv.empty.merge";

	std::unordered_map<std::string, graph::PropertyValue> initial;
	initial["x"] = graph::PropertyValue(static_cast<int64_t>(1));
	stateManager->setMap(key, initial, graph::storage::state::UpdateMode::REPLACE);

	// Merge with empty - should keep existing
	stateManager->setMap(key, std::unordered_map<std::string, graph::PropertyValue>{},
						 graph::storage::state::UpdateMode::MERGE);

	auto all = stateManager->getAll(key);
	EXPECT_EQ(all.size(), 1UL);
}

// Test 46: PropVec REPLACE different size
TEST_F(SystemStateManagerTest, PropVecReplaceDifferentSize) {
	const std::string key = "propvec.replace.size";
	using PropVec = std::vector<graph::PropertyValue>;

	PropVec vec1;
	vec1.emplace_back(static_cast<int64_t>(1));
	PropVec vec2;
	vec2.emplace_back(static_cast<int64_t>(2));

	std::unordered_map<std::string, PropVec> map1;
	map1["a"] = vec1;
	map1["b"] = vec2;
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	// Replace with smaller map
	std::unordered_map<std::string, PropVec> map2;
	map2["x"] = vec1;
	stateManager->setMap(key, map2, graph::storage::state::UpdateMode::REPLACE);

	auto loaded = stateManager->getMap<PropVec>(key);
	EXPECT_EQ(loaded.size(), 1UL);
	EXPECT_TRUE(loaded.contains("x"));
}

// Test 47: PropVec REPLACE same-size different content
TEST_F(SystemStateManagerTest, PropVecReplaceSameSizeDifferent) {
	const std::string key = "propvec.replace.diff";
	using PropVec = std::vector<graph::PropertyValue>;

	PropVec vec1;
	vec1.emplace_back(static_cast<int64_t>(1));

	std::unordered_map<std::string, PropVec> map1;
	map1["a"] = vec1;
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	database->getStorage()->flush();

	// Replace with same size but different content
	PropVec vec2;
	vec2.emplace_back(static_cast<int64_t>(999));

	std::unordered_map<std::string, PropVec> map2;
	map2["a"] = vec2;
	stateManager->setMap(key, map2, graph::storage::state::UpdateMode::REPLACE);

	EXPECT_TRUE(database->getStorage()->getDataManager()->hasUnsavedChanges());
}

// Test 48: PropVec empty MERGE is no-op
TEST_F(SystemStateManagerTest, PropVecEmptyMerge) {
	const std::string key = "propvec.empty.merge";
	using PropVec = std::vector<graph::PropertyValue>;

	PropVec vec1;
	vec1.emplace_back(static_cast<int64_t>(1));

	std::unordered_map<std::string, PropVec> initial;
	initial["a"] = vec1;
	stateManager->setMap(key, initial, graph::storage::state::UpdateMode::REPLACE);

	// Merge with empty
	stateManager->setMap(key, std::unordered_map<std::string, PropVec>{},
						 graph::storage::state::UpdateMode::MERGE);

	auto loaded = stateManager->getMap<PropVec>(key);
	EXPECT_EQ(loaded.size(), 1UL);
}

// Test 49: PropVec MERGE no-op when same value (covers it->second != newVal false branch)
TEST_F(SystemStateManagerTest, PropVecMergeNoop) {
	const std::string key = "propvec.merge.noop";
	using PropVec = std::vector<graph::PropertyValue>;

	PropVec vec1;
	vec1.emplace_back(static_cast<int64_t>(42));

	std::unordered_map<std::string, PropVec> map1;
	map1["data"] = vec1;
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	database->getStorage()->flush();
	ASSERT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Merge same content
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::MERGE);
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges())
			<< "Merging PropVec with same content should not trigger dirty state.";
}

// Test 50: PropertyValue map MERGE no-op when same value
TEST_F(SystemStateManagerTest, PropertyValueMapMergeNoop) {
	const std::string key = "pv.merge.noop";

	std::unordered_map<std::string, graph::PropertyValue> map1;
	map1["a"] = graph::PropertyValue(static_cast<int64_t>(42));
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	database->getStorage()->flush();
	ASSERT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Merge same content
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::MERGE);
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges())
			<< "Merging PropertyValue map with same content should not trigger dirty state.";
}

// Test 51: PropertyValue map REPLACE with missing key in existing (key-not-found branch)
TEST_F(SystemStateManagerTest, PropertyValueMapReplaceMissingKey) {
	const std::string key = "pv.replace.missingkey";

	std::unordered_map<std::string, graph::PropertyValue> map1;
	map1["a"] = graph::PropertyValue(static_cast<int64_t>(1));
	map1["b"] = graph::PropertyValue(static_cast<int64_t>(2));
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	database->getStorage()->flush();

	// Replace with same size but different keys
	std::unordered_map<std::string, graph::PropertyValue> map2;
	map2["a"] = graph::PropertyValue(static_cast<int64_t>(1));
	map2["c"] = graph::PropertyValue(static_cast<int64_t>(2));
	stateManager->setMap(key, map2, graph::storage::state::UpdateMode::REPLACE);

	EXPECT_TRUE(database->getStorage()->getDataManager()->hasUnsavedChanges());
	auto all = stateManager->getAll(key);
	EXPECT_EQ(all.size(), 2UL);
	EXPECT_TRUE(all.contains("a"));
	EXPECT_TRUE(all.contains("c"));
	EXPECT_FALSE(all.contains("b"));
}

// Test 52: Int64 map REPLACE same-size same-content is no-op (covers isDirty=false in REPLACE)
TEST_F(SystemStateManagerTest, Int64MapReplaceSameContentNoop) {
	const std::string key = "int64.replace.noop";

	std::unordered_map<std::string, int64_t> map1 = {{"A", 1}, {"B", 2}};
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	database->getStorage()->flush();
	ASSERT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Replace with same content
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges())
			<< "Replacing int64 map with identical content should not trigger dirty state.";
}

// Test 53: Int64 MERGE same value no-op
TEST_F(SystemStateManagerTest, Int64MergeSameValueNoop) {
	const std::string key = "int64.merge.noop";

	stateManager->set<int64_t>(key, "val", 100);
	database->getStorage()->flush();
	ASSERT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Set same value
	stateManager->set<int64_t>(key, "val", 100);
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());
}

// Test 54: String MERGE same value no-op
TEST_F(SystemStateManagerTest, StringMergeSameValueNoop) {
	const std::string key = "string.merge.noop";

	stateManager->set<std::string>(key, "val", "hello");
	database->getStorage()->flush();
	ASSERT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Set same value
	stateManager->set<std::string>(key, "val", "hello");
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());
}

// Test 55: Empty map REPLACE when no existing state (covers getStateProperties empty + REPLACE empty branch)
TEST_F(SystemStateManagerTest, EmptyMapReplaceNoExistingForString) {
	const std::string key = "string.empty.no.existing";

	// Replace empty state with empty map
	stateManager->setMap(key, std::unordered_map<std::string, std::string>{},
						 graph::storage::state::UpdateMode::REPLACE);

	auto result = stateManager->getMap<std::string>(key);
	EXPECT_TRUE(result.empty());
}

// Test 56: Empty map REPLACE for PropVec when no existing state
TEST_F(SystemStateManagerTest, EmptyMapReplaceNoExistingForPropVec) {
	const std::string key = "propvec.empty.no.existing";
	using PropVec = std::vector<graph::PropertyValue>;

	stateManager->setMap(key, std::unordered_map<std::string, PropVec>{},
						 graph::storage::state::UpdateMode::REPLACE);

	auto result = stateManager->getMap<PropVec>(key);
	EXPECT_TRUE(result.empty());
}

// Test 57: Empty map REPLACE for PropertyValue when no existing state
TEST_F(SystemStateManagerTest, EmptyMapReplaceNoExistingForPropertyValue) {
	const std::string key = "pv.empty.no.existing";

	stateManager->setMap(key, std::unordered_map<std::string, graph::PropertyValue>{},
						 graph::storage::state::UpdateMode::REPLACE);

	auto all = stateManager->getAll(key);
	EXPECT_TRUE(all.empty());
}

// Test 58: extract<T> actually finds a matching value (covers True branch at line 52)
TEST_F(SystemStateManagerTest, ExtractFindsMatchingInt64Value) {
	const std::string key = "extract.int64.found";
	const std::string field = "count";

	// Store as int64_t and retrieve as int64_t - should hit extract<int64_t> True branch
	stateManager->set<int64_t>(key, field, 42);
	int64_t result = stateManager->get<int64_t>(key, field, -1);
	EXPECT_EQ(result, 42);
}

// Test 59: extract<T> for string finds matching value
TEST_F(SystemStateManagerTest, ExtractFindsMatchingStringValue) {
	const std::string key = "extract.string.found";
	const std::string field = "name";

	stateManager->set<std::string>(key, field, "test_value");
	std::string result = stateManager->get<std::string>(key, field, "default");
	EXPECT_EQ(result, "test_value");
}

// Test 60: extract<T> for double finds matching value
TEST_F(SystemStateManagerTest, ExtractFindsMatchingDoubleValue) {
	const std::string key = "extract.double.found";
	const std::string field = "ratio";

	stateManager->set<double>(key, field, 3.14);
	double result = stateManager->get<double>(key, field, 0.0);
	EXPECT_DOUBLE_EQ(result, 3.14);
}

// Test 61: extract<bool> from actual bool value (covers line 61 True branch)
TEST_F(SystemStateManagerTest, ExtractBoolFromActualBool) {
	const std::string key = "extract.bool.actual";
	const std::string field = "flag";

	stateManager->set<bool>(key, field, true);
	bool result = stateManager->get<bool>(key, field, false);
	EXPECT_TRUE(result);

	stateManager->set<bool>(key, field, false);
	result = stateManager->get<bool>(key, field, true);
	EXPECT_FALSE(result);
}

// Test 62: extract<bool> from int64_t zero value (covers line 63 with val==0)
TEST_F(SystemStateManagerTest, ExtractBoolFromInt64Zero) {
	const std::string key = "extract.bool.int.zero";
	const std::string field = "flag";

	// Store as int64_t with value 0
	stateManager->set<int64_t>(key, field, 0);

	// Read as bool - should extract from int64_t and return false
	bool result = stateManager->get<bool>(key, field, true);
	EXPECT_FALSE(result);
}

// Test 63: get<T> with field found but val not matching type (covers line 95 False branch)
TEST_F(SystemStateManagerTest, GetFieldFoundButTypeMismatch) {
	const std::string key = "get.mismatch.found";

	// Store as bool, try to read as int64_t
	stateManager->set<bool>(key, "mybool", true);
	// Reading as int64_t should find the field but extract fails
	int64_t result = stateManager->get<int64_t>(key, "mybool", -999);
	EXPECT_EQ(result, -999);
}

// Test 64: get<T> with field found and val matching (covers line 95 True branch)
TEST_F(SystemStateManagerTest, GetFieldFoundAndTypeMatches) {
	const std::string key = "get.match.found";

	stateManager->set<int64_t>(key, "myint", 777);
	int64_t result = stateManager->get<int64_t>(key, "myint", -1);
	EXPECT_EQ(result, 777);
}

// Test 65: Multiple fields in same key, get specific one (covers line 93 True branch)
TEST_F(SystemStateManagerTest, GetSpecificFieldFromMultiple) {
	const std::string key = "multi.fields";

	stateManager->set<int64_t>(key, "field_a", 100);
	stateManager->set<std::string>(key, "field_b", "hello");
	stateManager->set<double>(key, "field_c", 2.718);

	EXPECT_EQ(stateManager->get<int64_t>(key, "field_a", 0), 100);
	EXPECT_EQ(stateManager->get<std::string>(key, "field_b", ""), "hello");
	EXPECT_DOUBLE_EQ(stateManager->get<double>(key, "field_c", 0.0), 2.718);
}

// Test 66: extract<bool> from int64_t non-zero value
TEST_F(SystemStateManagerTest, ExtractBoolFromInt64NonZero) {
	const std::string key = "extract.bool.int.nonzero";

	stateManager->set<int64_t>(key, "flag", 42);
	bool result = stateManager->get<bool>(key, "flag", false);
	EXPECT_TRUE(result);
}

// Test 67: setMap with PropertyValue containing vector data
TEST_F(SystemStateManagerTest, SetMapPropertyValueVector) {
	const std::string key = "pvvec.map";

	// Use PropertyValue map to store a vector<PropertyValue> containing doubles
	std::vector<graph::PropertyValue> pvList;
	pvList.emplace_back(1.5);
	pvList.emplace_back(2.5);

	std::unordered_map<std::string, graph::PropertyValue> map;
	map["embedding"] = graph::PropertyValue(pvList);
	stateManager->setMap(key, map, graph::storage::state::UpdateMode::REPLACE);

	// Read back as PropVec
	using PropVec = std::vector<graph::PropertyValue>;
	auto result = stateManager->get<PropVec>(key, "embedding", PropVec{});
	ASSERT_EQ(result.size(), 2UL);
}

// Test 68: PropVec round trip covering extract<PropVec>
TEST_F(SystemStateManagerTest, PropVecExtractRoundTrip) {
	const std::string key = "propvec.roundtrip";
	const std::string field = "data";

	using PropVec = std::vector<graph::PropertyValue>;
	PropVec pvList;
	pvList.emplace_back(1.0);
	pvList.emplace_back(static_cast<int64_t>(42));
	pvList.emplace_back(std::string("hello"));

	stateManager->set<PropVec>(key, field, pvList);

	// Read back - covers extract<PropVec> path (get_if matches)
	PropVec result = stateManager->get<PropVec>(key, field, PropVec{});
	ASSERT_EQ(result.size(), 3UL);
}

// ============================================================================
// Tests targeting extract<std::vector<float>> specialization (lines 72-78)
// ============================================================================

// Test 69: Store doubles as vector<PropertyValue>, retrieve as vector<float>
// Covers extract<vector<float>> line 72-77 (double elements path)
TEST_F(SystemStateManagerTest, ExtractVectorFloatFromDoubles) {
	const std::string key = "vecfloat.doubles";
	const std::string field = "embedding";

	// Store a vector of doubles as PropertyValue vector
	using PropVec = std::vector<graph::PropertyValue>;
	PropVec pvList;
	pvList.emplace_back(1.5);   // double
	pvList.emplace_back(2.5);   // double
	pvList.emplace_back(3.75);  // double

	stateManager->set<PropVec>(key, field, pvList);

	// Read back as vector<float> - exercises extract<vector<float>> specialization
	std::vector<float> defaultVec;
	auto result = stateManager->get<std::vector<float>>(key, field, defaultVec);
	ASSERT_EQ(result.size(), 3UL);
	EXPECT_FLOAT_EQ(result[0], 1.5f);
	EXPECT_FLOAT_EQ(result[1], 2.5f);
	EXPECT_FLOAT_EQ(result[2], 3.75f);
}

// Test 70: Store integers as vector<PropertyValue>, retrieve as vector<float>
// Covers extract<vector<float>> line 78-79 (integer elements path)
TEST_F(SystemStateManagerTest, ExtractVectorFloatFromIntegers) {
	const std::string key = "vecfloat.integers";
	const std::string field = "embedding";

	using PropVec = std::vector<graph::PropertyValue>;
	PropVec pvList;
	pvList.emplace_back(static_cast<int64_t>(10));
	pvList.emplace_back(static_cast<int64_t>(20));
	pvList.emplace_back(static_cast<int64_t>(30));

	stateManager->set<PropVec>(key, field, pvList);

	// Read as vector<float>
	std::vector<float> defaultVec;
	auto result = stateManager->get<std::vector<float>>(key, field, defaultVec);
	ASSERT_EQ(result.size(), 3UL);
	EXPECT_FLOAT_EQ(result[0], 10.0f);
	EXPECT_FLOAT_EQ(result[1], 20.0f);
	EXPECT_FLOAT_EQ(result[2], 30.0f);
}

// Test 71: Store mixed doubles and ints, retrieve as vector<float>
TEST_F(SystemStateManagerTest, ExtractVectorFloatFromMixed) {
	const std::string key = "vecfloat.mixed";
	const std::string field = "embedding";

	using PropVec = std::vector<graph::PropertyValue>;
	PropVec pvList;
	pvList.emplace_back(1.5);                        // double
	pvList.emplace_back(static_cast<int64_t>(42));   // int64_t

	stateManager->set<PropVec>(key, field, pvList);

	std::vector<float> defaultVec;
	auto result = stateManager->get<std::vector<float>>(key, field, defaultVec);
	ASSERT_EQ(result.size(), 2UL);
	EXPECT_FLOAT_EQ(result[0], 1.5f);
	EXPECT_FLOAT_EQ(result[1], 42.0f);
}

// Test 72: Non-numeric elements in vector returns nullopt (line 81)
// Covers extract<vector<float>> non-numeric element branch
TEST_F(SystemStateManagerTest, ExtractVectorFloatNonNumericReturnsDefault) {
	const std::string key = "vecfloat.nonnumeric";
	const std::string field = "embedding";

	using PropVec = std::vector<graph::PropertyValue>;
	PropVec pvList;
	pvList.emplace_back(1.5);                        // double - ok
	pvList.emplace_back(std::string("not_a_number")); // string - will cause nullopt

	stateManager->set<PropVec>(key, field, pvList);

	std::vector<float> defaultVec = {-1.0f};
	auto result = stateManager->get<std::vector<float>>(key, field, defaultVec);
	// Should return default because of non-numeric element
	ASSERT_EQ(result.size(), 1UL);
	EXPECT_FLOAT_EQ(result[0], -1.0f);
}

// Test 73: extract<vector<float>> with non-vector PropertyValue returns nullopt (line 87)
TEST_F(SystemStateManagerTest, ExtractVectorFloatFromNonVector) {
	const std::string key = "vecfloat.nonvector";
	const std::string field = "val";

	// Store a scalar, try to extract as vector<float>
	stateManager->set<int64_t>(key, field, 42);

	std::vector<float> defaultVec = {-1.0f};
	auto result = stateManager->get<std::vector<float>>(key, field, defaultVec);
	ASSERT_EQ(result.size(), 1UL);
	EXPECT_FLOAT_EQ(result[0], -1.0f);
}

// Test 74: Empty vector<PropertyValue> extracted as empty vector<float>
TEST_F(SystemStateManagerTest, ExtractVectorFloatEmpty) {
	const std::string key = "vecfloat.empty";
	const std::string field = "embedding";

	using PropVec = std::vector<graph::PropertyValue>;
	PropVec emptyList;

	stateManager->set<PropVec>(key, field, emptyList);

	std::vector<float> defaultVec = {-1.0f};
	auto result = stateManager->get<std::vector<float>>(key, field, defaultVec);
	// Empty vector should be returned (not defaultVec)
	EXPECT_TRUE(result.empty());
}

// Test 75: vector<float> set and getMap operations
TEST_F(SystemStateManagerTest, VectorFloatSetAndGetMap) {
	const std::string key = "vecfloat.map";

	std::unordered_map<std::string, std::vector<float>> inputMap;
	inputMap["emb1"] = {1.0f, 2.0f, 3.0f};
	inputMap["emb2"] = {4.0f, 5.0f};

	stateManager->setMap(key, inputMap, graph::storage::state::UpdateMode::REPLACE);

	auto loaded = stateManager->getMap<std::vector<float>>(key);
	ASSERT_EQ(loaded.size(), 2UL);
	EXPECT_TRUE(loaded.contains("emb1"));
	EXPECT_TRUE(loaded.contains("emb2"));
	ASSERT_EQ(loaded["emb1"].size(), 3UL);
	EXPECT_FLOAT_EQ(loaded["emb1"][0], 1.0f);
	EXPECT_FLOAT_EQ(loaded["emb1"][1], 2.0f);
	EXPECT_FLOAT_EQ(loaded["emb1"][2], 3.0f);
	ASSERT_EQ(loaded["emb2"].size(), 2UL);
	EXPECT_FLOAT_EQ(loaded["emb2"][0], 4.0f);
	EXPECT_FLOAT_EQ(loaded["emb2"][1], 5.0f);
}

// Test 76: vector<float> MERGE mode
TEST_F(SystemStateManagerTest, VectorFloatMerge) {
	const std::string key = "vecfloat.merge";

	std::unordered_map<std::string, std::vector<float>> initial;
	initial["a"] = {1.0f, 2.0f};
	stateManager->setMap(key, initial, graph::storage::state::UpdateMode::REPLACE);

	std::unordered_map<std::string, std::vector<float>> merge;
	merge["b"] = {3.0f, 4.0f};
	stateManager->setMap(key, merge, graph::storage::state::UpdateMode::MERGE);

	auto loaded = stateManager->getMap<std::vector<float>>(key);
	EXPECT_EQ(loaded.size(), 2UL);
	EXPECT_TRUE(loaded.contains("a"));
	EXPECT_TRUE(loaded.contains("b"));
}

// Test 77: vector<float> MERGE same value is no-op (covers line 150 same-value branch)
TEST_F(SystemStateManagerTest, VectorFloatMergeSameValueNoop) {
	const std::string key = "vecfloat.merge.noop";

	std::unordered_map<std::string, std::vector<float>> map1;
	map1["emb"] = {1.0f, 2.0f, 3.0f};
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	database->getStorage()->flush();
	ASSERT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges());

	// Merge with same content
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::MERGE);
	EXPECT_FALSE(database->getStorage()->getDataManager()->hasUnsavedChanges())
			<< "Merging vector<float> with same content should not trigger dirty state.";
}

// Test 78: vector<float> empty REPLACE when existing data exists
TEST_F(SystemStateManagerTest, VectorFloatEmptyReplaceClearsExisting) {
	const std::string key = "vecfloat.empty.replace";

	std::unordered_map<std::string, std::vector<float>> map1;
	map1["emb"] = {1.0f, 2.0f};
	stateManager->setMap(key, map1, graph::storage::state::UpdateMode::REPLACE);

	// Replace with empty
	stateManager->setMap(key, std::unordered_map<std::string, std::vector<float>>{},
						 graph::storage::state::UpdateMode::REPLACE);

	auto result = stateManager->getMap<std::vector<float>>(key);
	EXPECT_TRUE(result.empty());
}

// Test 79: vector<float> empty REPLACE when no existing data (lines 126-128 false branch)
TEST_F(SystemStateManagerTest, VectorFloatEmptyReplaceNoExisting) {
	const std::string key = "vecfloat.empty.no.existing";

	stateManager->setMap(key, std::unordered_map<std::string, std::vector<float>>{},
						 graph::storage::state::UpdateMode::REPLACE);

	auto result = stateManager->getMap<std::vector<float>>(key);
	EXPECT_TRUE(result.empty());
}

// Test 80: vector<float> scalar set/get round trip
TEST_F(SystemStateManagerTest, VectorFloatScalarSetGet) {
	const std::string key = "vecfloat.scalar";
	const std::string field = "vec";

	std::vector<float> input = {0.1f, 0.2f, 0.3f};
	stateManager->set<std::vector<float>>(key, field, input);

	std::vector<float> defaultVec;
	auto result = stateManager->get<std::vector<float>>(key, field, defaultVec);
	ASSERT_EQ(result.size(), 3UL);
	EXPECT_FLOAT_EQ(result[0], 0.1f);
	EXPECT_FLOAT_EQ(result[1], 0.2f);
	EXPECT_FLOAT_EQ(result[2], 0.3f);
}
