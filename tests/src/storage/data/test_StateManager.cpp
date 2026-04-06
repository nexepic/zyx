/**
 * @file test_StateManager.cpp
 * @author Nexepic
 * @date 2025/8/15
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

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include "boost/uuid/uuid.hpp"
#include "boost/uuid/uuid_generators.hpp"
#include "boost/uuid/uuid_io.hpp"
#include "graph/core/Database.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/StateManager.hpp"

class StateManagerTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::storage::StateManager> stateManager;

	void SetUp() override {
		// Create a unique temporary database file
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_stateManager_" + to_string(uuid) + ".db");

		// Initialize database and get managers from the DataManager
		db = std::make_unique<graph::Database>(testFilePath.string());
		db->open();
		storage = db->getStorage();
		dataManager = storage->getDataManager();
		// Get the manager instance that was created by the DataManager
		stateManager = dataManager->getStateManager();
	}

	void TearDown() override {
		// Release shared_ptrs before closing database
		stateManager.reset();
		dataManager.reset();
		storage.reset();

		if (db) {
			db->close();
		}
		db.reset();

		std::error_code ec;
		std::filesystem::remove(testFilePath, ec);
	}
};

// Test adding and retrieving a state and its properties
TEST_F(StateManagerTest, AddAndGetState) {
	// Create a state
	graph::State state;
	state.setKey("test.state.key");

	// Add the state entity. This creates the initial state with an initial ID.
	stateManager->add(state);
	int64_t originalId = state.getId();
	EXPECT_NE(originalId, 0) << "State should have a non-zero ID after initial add";

	// Add properties to the state via the DataManager.
	// According to the "delete and recreate" design, this operation will:
	// 1. Delete the state with `originalId`.
	// 2. Create a new state chain to store the properties.
	std::unordered_map<std::string, graph::PropertyValue> properties;
	properties["string_prop"] = graph::PropertyValue("value");
	properties["int_prop"] = graph::PropertyValue(42);
	dataManager->addStateProperties(state.getKey(), properties);

	// We can no longer use `originalId` as it now points to a deleted entity.
	// We must retrieve the *new* state entity using its key.
	graph::State retrievedState = stateManager->findByKey("test.state.key");

	// The new state should exist and have a NEW ID.
	EXPECT_NE(retrievedState.getId(), 0) << "State should be findable by key after adding properties";
	EXPECT_EQ(retrievedState.getKey(), "test.state.key") << "Retrieved state should have the same key";

	// Verify properties by getting them from the DataManager using the key.
	auto retrievedProps = dataManager->getStateProperties(state.getKey());
	ASSERT_EQ(retrievedProps.size(), 2UL); // Assuming properties are stored in the state's data.
	EXPECT_EQ(std::get<std::string>(retrievedProps["string_prop"].getVariant()), "value");
	EXPECT_EQ(std::get<int64_t>(retrievedProps["int_prop"].getVariant()), 42);
}

// Test updating a state's properties
TEST_F(StateManagerTest, UpdateState) {
	// Create and add a state
	graph::State state;
	state.setKey("test.state.key");
	stateManager->add(state);

	// First property addition: re-creates the state
	dataManager->addStateProperties(state.getKey(), {{"counter", graph::PropertyValue(1)}});
	graph::State stateAfterFirstUpdate = stateManager->findByKey("test.state.key");
	int64_t idAfterFirstUpdate = stateAfterFirstUpdate.getId();
	EXPECT_NE(idAfterFirstUpdate, 0);

	// Second property addition: re-creates the state AGAIN
	dataManager->addStateProperties(state.getKey(),
									{{"counter", graph::PropertyValue(2)}, {"updated", graph::PropertyValue(true)}});

	// Retrieve the state again by key to get the latest version.
	graph::State stateAfterSecondUpdate = stateManager->findByKey("test.state.key");
	EXPECT_NE(stateAfterSecondUpdate.getId(), 0);

	// Get the updated state's properties
	auto retrievedProps = dataManager->getStateProperties(state.getKey());
	ASSERT_EQ(retrievedProps.size(), 2UL);
	EXPECT_EQ(std::get<int64_t>(retrievedProps.at("counter").getVariant()), 2) << "State property should be updated";
	EXPECT_EQ(std::get<bool>(retrievedProps.at("updated").getVariant()), true) << "New property should be added";
}

// Test removing a state
TEST_F(StateManagerTest, RemoveState) {
	// Create and add a state
	graph::State state;
	state.setKey("test.state.key");
	stateManager->add(state);
	int64_t stateId = state.getId();

	// Verify the state was added
	graph::State retrievedState = stateManager->get(stateId);
	EXPECT_EQ(retrievedState.getId(), stateId) << "State should be retrievable after adding";

	// Remove the state
	stateManager->remove(state);

	// Clear cache to ensure it's removed from disk
	dataManager->clearCache();

	// Verify the state was removed
	graph::State removedState = stateManager->get(stateId);
	EXPECT_EQ(removedState.getId(), 0) << "State should not be retrievable after removal";
}

// Test finding state by key
TEST_F(StateManagerTest, FindStateByKey) {
	// Create and add a state
	graph::State state;
	state.setKey("test.unique.key");
	stateManager->add(state);

	// Find the state by key
	graph::State foundState = stateManager->findByKey("test.unique.key");
	EXPECT_EQ(foundState.getId(), state.getId()) << "Should find state by key";
	EXPECT_EQ(foundState.getKey(), "test.unique.key") << "Found state should have the correct key";

	// Try to find a non-existent state
	graph::State nonExistentState = stateManager->findByKey("non.existent.key");
	EXPECT_EQ(nonExistentState.getId(), 0) << "Non-existent state should return default state with ID 0";
}

// Test batch operations
TEST_F(StateManagerTest, BatchOperations) {
	// Create multiple states
	std::vector<std::string> stateKeys;

	for (int i = 0; i < 5; i++) {
		std::string key = "state.key." + std::to_string(i);
		stateKeys.push_back(key);

		graph::State state;
		state.setKey(key);
		stateManager->add(state); // Initial creation

		// This re-creates the state with a new ID
		dataManager->addStateProperties(key, {{"index", graph::PropertyValue(i)}});
	}

	// We cannot use getBatch with the original IDs. Instead, we should verify
	// that all states are accessible via their keys.
	for (const auto &key: stateKeys) {
		graph::State retrievedState = stateManager->findByKey(key);
		EXPECT_NE(retrievedState.getId(), 0) << "State with key " << key << " should be retrievable.";
	}

	// Now, test after removing one state using the high-level remove function
	// which should correctly handle finding by key.
	stateManager->removeState(stateKeys[2]);

	// Verify the removed state is gone
	graph::State removedState = stateManager->findByKey(stateKeys[2]);
	EXPECT_EQ(removedState.getId(), 0) << "State with key " << stateKeys[2] << " should be removed.";

	// Verify the other states still exist
	for (int i = 0; i < 5; ++i) {
		if (i == 2)
			continue;
		graph::State existingState = stateManager->findByKey(stateKeys[i]);
		EXPECT_NE(existingState.getId(), 0) << "State with key " << stateKeys[i] << " should still exist.";
	}
}

// Test handling non-existent states
TEST_F(StateManagerTest, NonExistentState) {
	// Try to get a non-existent state
	graph::State nonExistentState = stateManager->get(999999);
	EXPECT_EQ(nonExistentState.getId(), 0) << "Non-existent state should return default state with ID 0";

	// Try to update a non-existent state; this should not throw an error
	graph::State invalidState;
	invalidState.setId(999999);
	invalidState.setKey("invalid.key");
	EXPECT_NO_THROW(stateManager->update(invalidState)) << "Updating non-existent state should not throw";
}

// Test state ID allocation
TEST_F(StateManagerTest, StateIdAllocation) {
	// Create multiple states and verify IDs are unique
	graph::State state1;
	graph::State state2;
	state1.setKey("state1");
	state2.setKey("state2");

	stateManager->add(state1);
	stateManager->add(state2);

	EXPECT_NE(state1.getId(), 0);
	EXPECT_NE(state2.getId(), 0);
	EXPECT_NE(state1.getId(), state2.getId()) << "State IDs should be unique";
}

// Test state with empty key
TEST_F(StateManagerTest, StateWithEmptyKey) {
	// Create a state with an empty key
	graph::State state;
	state.setKey(""); // Empty key

	stateManager->add(state);
	int64_t stateId = state.getId();
	EXPECT_NE(stateId, 0) << "State with empty key should still be added";

	// Verify it can be retrieved by ID
	graph::State retrievedState = stateManager->get(stateId);
	EXPECT_EQ(retrievedState.getId(), stateId);
	EXPECT_EQ(retrievedState.getKey(), "");

	// Update state with a key
	state.setKey("new.key");
	stateManager->update(state);

	// Now it should be findable by key
	graph::State foundState = stateManager->findByKey("new.key");
	EXPECT_EQ(foundState.getId(), state.getId());
}

// Test updating state with key change
TEST_F(StateManagerTest, UpdateStateWithKeyChange) {
	// Create a state with an initial key
	graph::State state;
	state.setKey("old.key");
	stateManager->add(state);
	int64_t stateId = state.getId();

	// Verify it's findable by the old key
	graph::State foundByOldKey = stateManager->findByKey("old.key");
	EXPECT_EQ(foundByOldKey.getId(), stateId);

	// Update the state with a new key
	state.setKey("new.key");
	stateManager->update(state);

	// Verify the old key no longer works
	graph::State notFoundByOldKey = stateManager->findByKey("old.key");
	EXPECT_EQ(notFoundByOldKey.getId(), 0) << "Old key should not find the state";

	// Verify the new key works
	graph::State foundByNewKey = stateManager->findByKey("new.key");
	EXPECT_EQ(foundByNewKey.getId(), stateId) << "New key should find the state";
}

// Test state with large properties triggers blob storage
TEST_F(StateManagerTest, LargeStatePropertiesUseBlobStorage) {
	std::string stateKey = "large.state.key";

	// Create large properties (larger than 4KB threshold)
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	std::string largeValue(5000, 'X'); // 5KB of data
	largeProps["large_data"] = graph::PropertyValue(largeValue);

	// Add properties - should trigger blob storage
	dataManager->addStateProperties(stateKey, largeProps);

	// Verify properties were stored
	auto retrievedProps = dataManager->getStateProperties(stateKey);
	EXPECT_EQ(retrievedProps.size(), 1UL);
	EXPECT_EQ(std::get<std::string>(retrievedProps["large_data"].getVariant()).size(), 5000UL);
}

// Test find inactive state returns empty
TEST_F(StateManagerTest, FindInactiveState) {
	// Create and add a state
	graph::State state;
	state.setKey("test.inactive.key");
	stateManager->add(state);

	// Add properties to create a state chain (this will invalidate the original state)
	dataManager->addStateProperties(state.getKey(), {{"prop", graph::PropertyValue(1)}});

	// The original state ID should now point to an inactive state
	// findByKey should return an empty state since the original was deleted
	graph::State foundState = stateManager->findByKey("test.inactive.key");
	// The new state should be found (has a new ID)
	EXPECT_NE(foundState.getId(), 0) << "Should find the new active state by key";
}

// Test get state properties for non-existent key
TEST_F(StateManagerTest, GetPropertiesForNonExistentKey) {
	// Try to get properties for a state that doesn't exist
	auto props = dataManager->getStateProperties("non.existent.key");
	EXPECT_TRUE(props.empty()) << "Should return empty properties for non-existent key";
}

// Test remove state with non-existent key
TEST_F(StateManagerTest, RemoveNonExistentState) {
	// Try to remove a state that doesn't exist - should not throw
	EXPECT_NO_THROW(stateManager->removeState("non.existent.key"));
}

// Test update state when key changes from empty to non-empty
TEST_F(StateManagerTest, UpdateStateKeyFromEmptyToNonEmpty) {
	// Create state with empty key
	graph::State state;
	state.setKey("");
	stateManager->add(state);
	int64_t stateId = state.getId();

	// Update to have a key
	state.setKey("now.has.key");
	stateManager->update(state);

	// Verify it's findable by the new key
	graph::State foundState = stateManager->findByKey("now.has.key");
	EXPECT_EQ(foundState.getId(), stateId);
}

// Test update state when key changes from non-empty to empty
TEST_F(StateManagerTest, UpdateStateKeyFromNonEmptyToEmpty) {
	// Create state with a key
	graph::State state;
	state.setKey("has.key");
	stateManager->add(state);
	int64_t stateId = state.getId();

	// Verify it's findable by key
	graph::State foundBefore = stateManager->findByKey("has.key");
	EXPECT_EQ(foundBefore.getId(), stateId);

	// Update to have empty key
	state.setKey("");
	stateManager->update(state);

	// Verify it's no longer findable by the old key
	graph::State foundAfter = stateManager->findByKey("has.key");
	EXPECT_EQ(foundAfter.getId(), 0) << "State should not be findable by old key after changing to empty";

	// But should still be retrievable by ID
	graph::State byId = stateManager->get(stateId);
	EXPECT_EQ(byId.getId(), stateId);
}

// Test findByKey returns empty for inactive state
TEST_F(StateManagerTest, FindByKeyWithInactiveState) {
	// This test documents the defensive check at line 111 in StateManager.cpp
	// The key-to-id map is carefully maintained to never contain invalid states,
	// so this defensive check is never triggered in normal operation.

	// Create and add a state
	graph::State state;
	state.setKey("test.key");
	stateManager->add(state);
	int64_t stateId = state.getId();

	// Verify it's findable
	graph::State found = stateManager->findByKey("test.key");
	EXPECT_EQ(found.getId(), stateId);

	// Remove the state
	stateManager->remove(state);

	// Clear cache
	dataManager->clearCache();

	// After removal, the key is also removed from the map, so findByKey returns empty
	graph::State notFound = stateManager->findByKey("test.key");
	EXPECT_EQ(notFound.getId(), 0);
}

// ============================================================================
// Branch Coverage Improvement Tests for StateManager.cpp
// ============================================================================

// Test update when the key does NOT change (oldKey == newKey)
// Covers: Branch (58) False path - when oldKey == newKey, the if block is skipped
TEST_F(StateManagerTest, UpdateStateWithSameKey) {
	graph::State state;
	state.setKey("same.key");
	stateManager->add(state);
	int64_t stateId = state.getId();

	// Verify findable by key
	graph::State found = stateManager->findByKey("same.key");
	EXPECT_EQ(found.getId(), stateId);

	// Update the state without changing the key - just modify some other aspect
	// The key stays the same so the key-change branch is skipped
	stateManager->update(state);

	// Key mapping should still work
	graph::State stillFound = stateManager->findByKey("same.key");
	EXPECT_EQ(stillFound.getId(), stateId);
}

// Test addStateProperties with explicit useBlobStorage=true
// Covers: Branch (128) True path - useBlobStorage is already true,
// so the auto-upgrade check is skipped (different path)
TEST_F(StateManagerTest, AddStatePropertiesExplicitBlobStorage) {
	std::string stateKey = "explicit.blob.key";

	// Create properties smaller than 4KB but force blob storage
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["small_data"] = graph::PropertyValue(std::string("small"));

	// Pass useBlobStorage = true explicitly
	stateManager->addStateProperties(stateKey, props, true);

	// Verify properties were stored and are retrievable
	auto retrievedProps = stateManager->getStateProperties(stateKey);
	EXPECT_EQ(retrievedProps.size(), 1UL);
	EXPECT_EQ(std::get<std::string>(retrievedProps["small_data"].getVariant()), "small");
}

// Test addStateProperties where data exceeds 4KB auto-triggers blob storage
// Covers: Branch (128) when !useBlobStorage && serializedData.size() > 4096
// The auto-upgrade to blob storage is triggered
TEST_F(StateManagerTest, AddStatePropertiesAutoUpgradeToBlobStorage) {
	std::string stateKey = "auto.blob.key";

	// Create properties larger than 4KB to trigger auto-upgrade
	std::unordered_map<std::string, graph::PropertyValue> largeProps;
	std::string largeValue(5000, 'Y'); // 5KB string, > 4096 threshold
	largeProps["big_data"] = graph::PropertyValue(largeValue);

	// useBlobStorage = false, but data exceeds threshold so it auto-upgrades
	stateManager->addStateProperties(stateKey, largeProps, false);

	// Verify properties were stored
	auto retrievedProps = stateManager->getStateProperties(stateKey);
	EXPECT_EQ(retrievedProps.size(), 1UL);
	EXPECT_EQ(std::get<std::string>(retrievedProps["big_data"].getVariant()).size(), 5000UL);
}

// Test populateKeyToIdMap after database restart with persisted states
// Covers the iteration in populateKeyToIdMap where isChainHeadState is checked
// and non-head states (prevStateId != 0) are skipped
TEST_F(StateManagerTest, PopulateKeyToIdMapAfterRestart) {
	// Create several states with properties (which create state chains)
	std::string key1 = "persist.key1";
	std::string key2 = "persist.key2";

	stateManager->addStateProperties(key1, {{"prop1", graph::PropertyValue(1)}});
	stateManager->addStateProperties(key2, {{"prop2", graph::PropertyValue(2)}});

	// Flush and restart database to test populateKeyToIdMap
	storage->flush();
	db->close();
	db->open();
	storage = db->getStorage();
	dataManager = storage->getDataManager();
	stateManager = dataManager->getStateManager();

	// After restart, populateKeyToIdMap should have rebuilt the key mapping
	graph::State found1 = stateManager->findByKey(key1);
	EXPECT_NE(found1.getId(), 0) << "Key1 should be found after restart";

	graph::State found2 = stateManager->findByKey(key2);
	EXPECT_NE(found2.getId(), 0) << "Key2 should be found after restart";

	// Verify properties survived the restart
	auto props1 = stateManager->getStateProperties(key1);
	EXPECT_EQ(props1.size(), 1UL);
	auto props2 = stateManager->getStateProperties(key2);
	EXPECT_EQ(props2.size(), 1UL);
}

// Test populateKeyToIdMap skips non-head states (prevStateId != 0)
// Covers: isChainHeadState returning false in populateKeyToIdMap (line 96 False path)
// and isChainHeadState function returning false via prevStateId != 0 (line 192 False path)
TEST_F(StateManagerTest, PopulateKeyToIdMap_SkipsNonHeadStates) {
	// Create a state chain with data large enough to span multiple State entities.
	// Non-head (tail) states have prevStateId != 0, so isChainHeadState returns false.
	std::string stateKey = "multichain.key";

	// State::CHUNK_SIZE is ~150 bytes. Create data > 300 bytes to get 2+ chunks.
	std::string largeData(400, 'A');
	std::unordered_map<std::string, graph::PropertyValue> props;
	props["big_data"] = graph::PropertyValue(largeData);

	// Use internal storage (useBlobStorage=false) to create actual state chain entities
	stateManager->addStateProperties(stateKey, props, false);

	// Verify state is findable
	graph::State found = stateManager->findByKey(stateKey);
	EXPECT_NE(found.getId(), 0) << "State should be findable by key";

	// Flush to disk so populateKeyToIdMap can scan segments
	storage->flush();

	// Restart database to trigger populateKeyToIdMap
	db->close();
	db->open();
	storage = db->getStorage();
	dataManager = storage->getDataManager();
	stateManager = dataManager->getStateManager();

	// After restart, populateKeyToIdMap should have:
	// - Found head state (isChainHeadState returns true) -> added to map
	// - Found tail states (isChainHeadState returns false) -> skipped
	graph::State recovered = stateManager->findByKey(stateKey);
	EXPECT_NE(recovered.getId(), 0) << "State should still be findable after restart";
}

// Test populateKeyToIdMap skips chain head states with empty keys
// Covers: !state.getKey().empty() returning false in populateKeyToIdMap (line 98 False path)
TEST_F(StateManagerTest, PopulateKeyToIdMap_SkipsEmptyKeyHeadStates) {
	// Create a state with empty key - it's a head state but has empty key
	graph::State emptyKeyState;
	emptyKeyState.setKey(""); // Empty key
	stateManager->add(emptyKeyState);
	EXPECT_NE(emptyKeyState.getId(), 0);

	// Also create a state with a non-empty key for comparison
	std::string validKey = "valid.key";
	stateManager->addStateProperties(validKey, {{"prop", graph::PropertyValue(42)}});

	// Flush and restart
	storage->flush();
	db->close();
	db->open();
	storage = db->getStorage();
	dataManager = storage->getDataManager();
	stateManager = dataManager->getStateManager();

	// The empty key state should NOT be in the key-to-id map
	// (isChainHeadState returns true but key is empty, so it's skipped)
	graph::State emptyKeyFound = stateManager->findByKey("");
	// findByKey("") should return empty since empty keys are not mapped
	EXPECT_EQ(emptyKeyFound.getId(), 0) << "Empty key state should not be in the key map";

	// The valid key state should still be findable
	graph::State validFound = stateManager->findByKey(validKey);
	EXPECT_NE(validFound.getId(), 0) << "Valid key state should be in the key map after restart";
}

// Test isChainHeadState returns false for inactive state
// Covers: state.isActive() returning false in isChainHeadState (line 192 second condition False)
TEST_F(StateManagerTest, IsChainHeadState_InactiveStateReturnsFalse) {
	// Create a state, add properties, flush, then remove and flush
	std::string key = "to.remove.head";
	stateManager->addStateProperties(key, {{"val", graph::PropertyValue(1)}});

	graph::State found = stateManager->findByKey(key);
	EXPECT_NE(found.getId(), 0);

	// Remove the state
	stateManager->removeState(key);

	// Flush so the inactive state is persisted to disk
	storage->flush();

	// Create another state that will survive
	stateManager->addStateProperties("survivor.key", {{"v", graph::PropertyValue(2)}});
	storage->flush();

	// Restart - populateKeyToIdMap scans disk, finding both active and inactive states
	// Inactive states should fail isChainHeadState (isActive() returns false)
	db->close();
	db->open();
	storage = db->getStorage();
	dataManager = storage->getDataManager();
	stateManager = dataManager->getStateManager();

	// The removed state should not be found
	graph::State removed = stateManager->findByKey(key);
	EXPECT_EQ(removed.getId(), 0) << "Removed state should not be in key map";

	// Survivor should be found
	graph::State survivor = stateManager->findByKey("survivor.key");
	EXPECT_NE(survivor.getId(), 0) << "Survivor state should be in key map";
}

