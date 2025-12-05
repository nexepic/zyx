/**
 * @file test_StateManager.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/8/15
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include "graph/core/Database.hpp"
#include "graph/core/State.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/data/StateManager.hpp"
#include "boost/uuid/uuid.hpp"
#include "boost/uuid/uuid_generators.hpp"
#include "boost/uuid/uuid_io.hpp"

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
        // Release resources and clean up the file
        db->close();
        db.reset();

        if (std::filesystem::exists(testFilePath)) {
            std::filesystem::remove(testFilePath);
        }
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
	ASSERT_EQ(retrievedProps.size(), 2); // Assuming properties are stored in the state's data.
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
	dataManager->addStateProperties(state.getKey(), {
		{"counter", graph::PropertyValue(2)},
		{"updated", graph::PropertyValue(true)}
	});

	// Retrieve the state again by key to get the latest version.
	graph::State stateAfterSecondUpdate = stateManager->findByKey("test.state.key");
	EXPECT_NE(stateAfterSecondUpdate.getId(), 0);

	// Get the updated state's properties
	auto retrievedProps = dataManager->getStateProperties(state.getKey());
	ASSERT_EQ(retrievedProps.size(), 2);
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
	for(const auto& key : stateKeys) {
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
		if (i == 2) continue;
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