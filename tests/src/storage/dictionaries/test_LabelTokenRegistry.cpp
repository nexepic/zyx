/**
 * @file test_LabelTokenRegistry.cpp
 * @author Nexepic
 * @date 2026/1/14
 *
 * @copyright Copyright (c) 2026 Nexepic
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
#include <string>
#include <vector>

#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/dictionaries/LabelTokenRegistry.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

namespace fs = std::filesystem;

class LabelTokenRegistryTest : public ::testing::Test {
protected:
	std::string testDbPath;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::storage::state::SystemStateManager> systemStateManager;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		// Create a unique path for the test database
		testDbPath = std::filesystem::temp_directory_path() / ("test_label_registry_" + to_string(uuid) + ".db");

		// Ensure clean state
		if (fs::exists(testDbPath)) {
			fs::remove(testDbPath);
		}

		// Initialize storage stack to get a valid DataManager
		// 4MB cache, Create New mode
		storage = std::make_shared<graph::storage::FileStorage>(testDbPath, 4 * 1024 * 1024,
																graph::storage::OpenMode::CREATE_NEW_FILE);
		storage->open();
		dataManager = storage->getDataManager();
		systemStateManager = storage->getSystemStateManager();
	}

	void TearDown() override {
		// Close and cleanup
		if (storage) {
			storage->close();
		}
		if (fs::exists(testDbPath)) {
			fs::remove(testDbPath);
		}
	}

	// Helper to reopen storage in existing mode to simulate restart
	void reopenStorage() {
		if (storage) {
			storage->close();
		}
		storage = std::make_shared<graph::storage::FileStorage>(testDbPath, 4 * 1024 * 1024,
																graph::storage::OpenMode::OPEN_EXISTING_FILE);
		storage->open();
		dataManager = storage->getDataManager();
		systemStateManager = storage->getSystemStateManager();
	}
};

/**
 * @brief Test initialization of a new registry.
 * It should create a new Index Root via SystemStateManager.
 */
TEST_F(LabelTokenRegistryTest, InitializationNew) {
	// Inject the SystemStateManager. It should detect no existing key and create a new B+ Tree.
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	// Should have allocated a new root index ID (non-zero)
	EXPECT_GT(registry.getRootIndexId(), 0);
}

/**
 * @brief Test basic string to ID creation and retrieval.
 */
TEST_F(LabelTokenRegistryTest, CreateAndResolveLabel) {
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);
	std::string labelName = "Person";

	// 1. Create ID
	int64_t id = registry.getOrCreateLabelId(labelName);
	EXPECT_GT(id, 0);

	// 2. Resolve back to string
	std::string resolved = registry.getLabelString(id);
	EXPECT_EQ(resolved, labelName);
}

/**
 * @brief Test that the same string returns the same ID (Deduplication).
 */
TEST_F(LabelTokenRegistryTest, Deduplication) {
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);
	std::string labelName = "City";

	int64_t id1 = registry.getOrCreateLabelId(labelName);

	// Calling it again should return the existing ID
	int64_t id2 = registry.getOrCreateLabelId(labelName);

	EXPECT_EQ(id1, id2);
}

/**
 * @brief Test empty string handling.
 */
TEST_F(LabelTokenRegistryTest, HandleEmptyString) {
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	int64_t id = registry.getOrCreateLabelId("");
	EXPECT_EQ(id, graph::storage::LabelTokenRegistry::NULL_LABEL_ID);
}

/**
 * @brief Test resolving a null ID.
 */
TEST_F(LabelTokenRegistryTest, HandleNullId) {
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	std::string label = registry.getLabelString(graph::storage::LabelTokenRegistry::NULL_LABEL_ID);
	EXPECT_EQ(label, "");
}

/**
 * @brief Test resolving a non-existent ID (should handle gracefully).
 */
TEST_F(LabelTokenRegistryTest, HandleInvalidId) {
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	// ID 999999 likely doesn't exist in a fresh DB
	std::string label = registry.getLabelString(999999);
	EXPECT_EQ(label, "");
}

/**
 * @brief Test that the registry works correctly after cache eviction.
 * The implementation clears the cache entirely when it exceeds CACHE_SIZE.
 */
TEST_F(LabelTokenRegistryTest, CacheEviction) {
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	// Add one more than cache size
	size_t limit = graph::storage::LabelTokenRegistry::CACHE_SIZE + 10;
	std::vector<int64_t> ids;

	// 1. Fill cache and trigger clear
	for (size_t i = 0; i < limit; ++i) {
		std::string label = "Label_" + std::to_string(i);
		ids.push_back(registry.getOrCreateLabelId(label));
	}

	// 2. Retrieve the very first item.
	// Since cache was likely cleared, this forces a look up from Blob/Index storage.
	std::string firstLabel = "Label_0";
	int64_t firstId = ids[0];

	// Verify resolving ID -> String works from disk
	EXPECT_EQ(registry.getLabelString(firstId), firstLabel);

	// Verify resolving String -> ID works from index
	EXPECT_EQ(registry.getOrCreateLabelId(firstLabel), firstId);
}

/**
 * @brief Test persistence simulation.
 * We create labels, flush the database, close it, and reopen it.
 * We verify that the new DataManager (and its internal Registry) resolves the old labels correctly.
 * This confirms that the RootIndexID was correctly saved to SystemStateManager.
 */
TEST_F(LabelTokenRegistryTest, Persistence) {
	int64_t personId = 0;
	int64_t cityId = 0;
	int64_t savedRootId = 0;

	// Scope 1: Initial creation
	{
		// Using the registry managed by DataManager (or manually created via SM)
		// Here we test manually creating one to ensure logic holds
		graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

		personId = registry.getOrCreateLabelId("Person");
		cityId = registry.getOrCreateLabelId("City");
		savedRootId = registry.getRootIndexId();

		EXPECT_GT(personId, 0);
		EXPECT_GT(cityId, 0);

		// Force flush to disk.
		// This persists the B-Tree Nodes, the Blob Data, AND the SystemState (Root ID).
		storage->flush();
	}

	// Scope 2: Simulate Database Restart
	reopenStorage();

	// Scope 3: Verification
	{
		// Create a new registry instance using the reloaded systemStateManager
		graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

		// 1. Verify Root ID was reloaded correctly
		EXPECT_EQ(registry.getRootIndexId(), savedRootId);
		EXPECT_GT(registry.getRootIndexId(), 0);

		// 2. Check ID -> String resolution
		EXPECT_EQ(registry.getLabelString(personId), "Person");
		EXPECT_EQ(registry.getLabelString(cityId), "City");

		// 3. Check String -> ID resolution (Index lookup should find existing IDs)
		EXPECT_EQ(registry.getOrCreateLabelId("Person"), personId);

		// 4. Add new label to ensure the tree is still writable
		int64_t countryId = registry.getOrCreateLabelId("Country");
		EXPECT_GT(countryId, 0);
		EXPECT_NE(countryId, personId);

		// Verify the new label is retrievable
		EXPECT_EQ(registry.getLabelString(countryId), "Country");
	}
}

/**
 * @brief Integration Test via DataManager.
 * Ensures the wiring inside DataManager::setSystemStateManager works correctly.
 */
TEST_F(LabelTokenRegistryTest, DataManagerIntegration) {
	// Since SetUp calls DataManager::setSystemStateManager implicitly (via FileStorage init),
	// DataManager should already have a working registry.

	int64_t id = dataManager->getOrCreateLabelId("IntegratedLabel");
	EXPECT_GT(id, 0);

	std::string result = dataManager->resolveLabel(id);
	EXPECT_EQ(result, "IntegratedLabel");
}

/**
 * @brief Test extremely long labels (Blob support).
 * Since the implementation uses Blobs, it should handle strings larger than typical buffer limits.
 */
TEST_F(LabelTokenRegistryTest, LongLabelSupport) {
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	// Create a 1KB string
	std::string longLabel(1024, 'A');

	int64_t id = registry.getOrCreateLabelId(longLabel);
	EXPECT_GT(id, 0);

	std::string retrieved = registry.getLabelString(id);
	EXPECT_EQ(retrieved, longLabel);
	EXPECT_EQ(retrieved.size(), 1024);
}
