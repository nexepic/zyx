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
	std::filesystem::path testDbPath;

	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::storage::state::SystemStateManager> systemStateManager;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();

		testDbPath = std::filesystem::temp_directory_path() / ("test_label_registry_" + to_string(uuid) + ".db");

		// Ensure clean state
		if (fs::exists(testDbPath)) {
			fs::remove(testDbPath);
		}

		storage = std::make_shared<graph::storage::FileStorage>(testDbPath.string(), 4 * 1024 * 1024,
																graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		storage->open();
		dataManager = storage->getDataManager();
		systemStateManager = storage->getSystemStateManager();
	}

	void TearDown() override {
		// Close and cleanup
		if (storage) {
			storage->close();
		storage.reset();
		}
		std::error_code ec;
		if (fs::exists(testDbPath)) {
			fs::remove(testDbPath, ec);
		}
	}

	// Helper to reopen storage in existing mode to simulate restart
	void reopenStorage() {
		if (storage) {
			storage->close();
		}
		storage = std::make_shared<graph::storage::FileStorage>(testDbPath.string(), 4 * 1024 * 1024,
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
	// Use small cache size to test eviction without creating thousands of labels
	constexpr size_t TEST_CACHE_SIZE = 20;
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager, TEST_CACHE_SIZE);

	// Add more than cache size
	size_t limit = TEST_CACHE_SIZE + 10;
	std::vector<int64_t> ids;

	// 1. Fill cache and trigger eviction
	for (size_t i = 0; i < limit; ++i) {
		std::string label = "Label_" + std::to_string(i);
		ids.push_back(registry.getOrCreateLabelId(label));
	}

	// 2. Retrieve the very first item.
	// Since cache was likely evicted, this forces a look up from Blob/Index storage.
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
	EXPECT_EQ(retrieved.size(), 1024ULL);
}

// ==========================================
// Additional Tests for Branch Coverage
// ==========================================

TEST_F(LabelTokenRegistryTest, MultipleLabelsCacheMiss) {
	// Test cache misses after eviction using small cache
	constexpr size_t TEST_CACHE_SIZE = 20;
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager, TEST_CACHE_SIZE);

	// Create more labels than cache size
	size_t labelCount = TEST_CACHE_SIZE + 5;
	std::vector<std::string> labels;
	for (size_t i = 0; i < labelCount; ++i) {
		labels.push_back("CacheMiss_" + std::to_string(i));
	}

	std::vector<int64_t> ids;
	for (const auto &label: labels) {
		ids.push_back(registry.getOrCreateLabelId(label));
	}

	// Verify all IDs are unique
	std::unordered_set<int64_t> idSet(ids.begin(), ids.end());
	EXPECT_EQ(idSet.size(), labels.size());

	// Verify all labels can be resolved (some from disk after eviction)
	for (size_t i = 0; i < labels.size(); ++i) {
		EXPECT_EQ(registry.getLabelString(ids[i]), labels[i]);
	}
}

TEST_F(LabelTokenRegistryTest, SpecialCharactersInLabel) {
	// Test labels with special characters
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	std::vector<std::string> specialLabels = {
		"Label-With-Dashes",
		"Label_With_Underscores",
		"Label.With.Dots",
		"Label@With@Special#Chars",
		"Label With Spaces",
		"Label\tWith\tTabs",
		"Label\nWith\nNewlines"
	};

	for (const auto &label: specialLabels) {
		int64_t id = registry.getOrCreateLabelId(label);
		EXPECT_GT(id, 0);

		std::string retrieved = registry.getLabelString(id);
		EXPECT_EQ(retrieved, label);
	}
}

TEST_F(LabelTokenRegistryTest, UnicodeLabels) {
	// Test Unicode labels
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	std::vector<std::string> unicodeLabels = {
		"标签",
		"Label",
		"Метки",
		"Etichette",
		"Étiquettes",
		"ラベル"
	};

	for (const auto &label: unicodeLabels) {
		int64_t id = registry.getOrCreateLabelId(label);
		EXPECT_GT(id, 0);

		std::string retrieved = registry.getLabelString(id);
		EXPECT_EQ(retrieved, label);
	}
}

TEST_F(LabelTokenRegistryTest, VeryLongLabel) {
	// Test a very long label (larger than typical blob)
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	// Create a 10KB string
	std::string veryLongLabel(10 * 1024, 'X');

	int64_t id = registry.getOrCreateLabelId(veryLongLabel);
	EXPECT_GT(id, 0);

	std::string retrieved = registry.getLabelString(id);
	EXPECT_EQ(retrieved, veryLongLabel);
	EXPECT_EQ(retrieved.size(), 10 * 1024ULL);
}

TEST_F(LabelTokenRegistryTest, CaseSensitiveLabels) {
	// Test that labels are case-sensitive
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	int64_t id1 = registry.getOrCreateLabelId("Person");
	int64_t id2 = registry.getOrCreateLabelId("PERSON");
	int64_t id3 = registry.getOrCreateLabelId("person");
	int64_t id4 = registry.getOrCreateLabelId("Person"); // Should match id1

	EXPECT_NE(id1, id2);
	EXPECT_NE(id2, id3);
	EXPECT_NE(id1, id3);
	EXPECT_EQ(id1, id4);
}

TEST_F(LabelTokenRegistryTest, CacheConsistencyAfterPersistence) {
	// Test cache consistency across database restart
	int64_t labelId = 0;

	// Scope 1: Create label and cache it
	{
		graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);
		labelId = registry.getOrCreateLabelId("CachedLabel");
		EXPECT_GT(labelId, 0);

		// Verify it's cached
		EXPECT_EQ(registry.getLabelString(labelId), "CachedLabel");

		storage->flush();
	}

	// Scope 2: Reopen - cache should be empty but data available
	reopenStorage();

	{
		graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

		// Should load from disk (cache miss after restart)
		EXPECT_EQ(registry.getLabelString(labelId), "CachedLabel");

		// Should also find in index
		EXPECT_EQ(registry.getOrCreateLabelId("CachedLabel"), labelId);
	}
}

TEST_F(LabelTokenRegistryTest, MultipleRegistriesIndependentCaches) {
	// Test that multiple registry instances maintain independent caches
	graph::storage::LabelTokenRegistry registry1(dataManager, systemStateManager);
	graph::storage::LabelTokenRegistry registry2(dataManager, systemStateManager);

	// Create label in registry1
	int64_t id1 = registry1.getOrCreateLabelId("SharedLabel");
	EXPECT_GT(id1, 0);

	// Resolve in registry2 - should hit index, not registry2's cache
	int64_t id2 = registry2.getOrCreateLabelId("SharedLabel");
	EXPECT_EQ(id2, id1);

	// Both should resolve correctly
	EXPECT_EQ(registry1.getLabelString(id1), "SharedLabel");
	EXPECT_EQ(registry2.getLabelString(id2), "SharedLabel");
}

TEST_F(LabelTokenRegistryTest, EmptyCacheAfterCreation) {
	// Test behavior when cache is empty (initial state)
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	// First access should populate cache
	std::string label = "FirstLabel";
	int64_t id = registry.getOrCreateLabelId(label);

	// Verify it works
	EXPECT_GT(id, 0);
	EXPECT_EQ(registry.getLabelString(id), label);
}

TEST_F(LabelTokenRegistryTest, LabelWithNullBytes) {
	// Test labels containing null bytes
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	std::string labelWithNull = std::string("Before") + '\0' + "After";

	int64_t id = registry.getOrCreateLabelId(labelWithNull);
	EXPECT_GT(id, 0);

	std::string retrieved = registry.getLabelString(id);
	EXPECT_EQ(retrieved, labelWithNull);
}

TEST_F(LabelTokenRegistryTest, ZeroIdHandling) {
	// Test handling of ID 0 (null label)
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	// ID 0 should return empty string
	std::string label = registry.getLabelString(0);
	EXPECT_EQ(label, "");

	// Empty string should return ID 0
	int64_t id = registry.getOrCreateLabelId("");
	EXPECT_EQ(id, graph::storage::LabelTokenRegistry::NULL_LABEL_ID);
}

TEST_F(LabelTokenRegistryTest, NegativeIdHandling) {
	// Test handling of negative IDs
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	// Negative IDs should return empty string
	std::string label = registry.getLabelString(-1);
	EXPECT_EQ(label, "");

	std::string label2 = registry.getLabelString(-999);
	EXPECT_EQ(label2, "");
}

TEST_F(LabelTokenRegistryTest, RapidSequentialCreations) {
	// Test rapid sequential label creations
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	std::vector<int64_t> ids;
	for (int i = 0; i < 100; ++i) {
		std::string label = "RapidLabel_" + std::to_string(i);
		ids.push_back(registry.getOrCreateLabelId(label));
	}

	// Verify all unique
	std::unordered_set<int64_t> idSet(ids.begin(), ids.end());
	EXPECT_EQ(idSet.size(), 100ULL);

	// Verify all resolvable
	for (int i = 0; i < 100; ++i) {
		std::string expected = "RapidLabel_" + std::to_string(i);
		EXPECT_EQ(registry.getLabelString(ids[i]), expected);
	}
}

TEST_F(LabelTokenRegistryTest, LabelWithOnlySpaces) {
	// Test labels with only spaces
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	std::vector<std::string> spaceLabels = {
		" ",
		"  ",
		"   ",
		"\t",
		"\t\t",
		"\n",
		" \t\n "
	};

	for (const auto &label: spaceLabels) {
		int64_t id = registry.getOrCreateLabelId(label);
		EXPECT_GT(id, 0);

		std::string retrieved = registry.getLabelString(id);
		EXPECT_EQ(retrieved, label);
	}
}

TEST_F(LabelTokenRegistryTest, LargeBatchOfUniqueLabels) {
	// Test creating a large batch of unique labels
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	constexpr int LARGE_COUNT = 50;
	std::vector<std::string> labels;
	std::vector<int64_t> ids;

	for (int i = 0; i < LARGE_COUNT; ++i) {
		labels.push_back("BatchLabel_" + std::to_string(i));
		ids.push_back(registry.getOrCreateLabelId(labels[i]));
	}

	// Verify all unique
	std::unordered_set<int64_t> idSet(ids.begin(), ids.end());
	EXPECT_EQ(idSet.size(), static_cast<size_t>(LARGE_COUNT));

	// Verify random subset
	for (int i = 0; i < 10; ++i) {
		int idx = rand() % LARGE_COUNT;
		EXPECT_EQ(registry.getLabelString(ids[idx]), labels[idx]);
	}
}

TEST_F(LabelTokenRegistryTest, RootIndexIdChanges) {
	// Test that rootIndexId changes are detected and persisted
	int64_t initialRootId = 0;

	{
		graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);
		initialRootId = registry.getRootIndexId();
		EXPECT_GT(initialRootId, 0);

		// Force root change by creating many labels
		for (int i = 0; i < 30; ++i) {
			registry.getOrCreateLabelId("RootChangeLabel_" + std::to_string(i));
		}

		storage->flush();
	}

	// Reopen and verify
	reopenStorage();

	{
		graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);
		int64_t newRootId = registry.getRootIndexId();

		// Root ID should be loaded correctly
		EXPECT_GT(newRootId, 0);

		// Labels should still be resolvable
		std::string firstLabel = registry.getLabelString(1);
		EXPECT_FALSE(firstLabel.empty());
	}
}

TEST_F(LabelTokenRegistryTest, CacheHitPath) {
	// Test the cache hit path explicitly
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	std::string label = "CacheHitLabel";
	int64_t id = registry.getOrCreateLabelId(label);

	// First call populates cache, second should hit cache
	int64_t id2 = registry.getOrCreateLabelId(label);
	EXPECT_EQ(id, id2);

	// Resolve should also hit cache
	std::string resolved = registry.getLabelString(id);
	EXPECT_EQ(resolved, label);
}

TEST_F(LabelTokenRegistryTest, NonExistentIdInCache) {
	// Test resolving an ID that doesn't exist
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	// This ID was never created
	int64_t fakeId = 99999;
	std::string label = registry.getLabelString(fakeId);

	// Should return empty string (not found)
	EXPECT_EQ(label, "");
}

TEST_F(LabelTokenRegistryTest, MixedEmptyAndNonEmptyLabels) {
	// Test mixing empty and non-empty labels
	graph::storage::LabelTokenRegistry registry(dataManager, systemStateManager);

	// Empty label
	int64_t emptyId = registry.getOrCreateLabelId("");
	EXPECT_EQ(emptyId, graph::storage::LabelTokenRegistry::NULL_LABEL_ID);

	// Non-empty labels
	int64_t id1 = registry.getOrCreateLabelId("Label1");
	int64_t id2 = registry.getOrCreateLabelId("Label2");

	EXPECT_GT(id1, 0);
	EXPECT_GT(id2, 0);
	EXPECT_NE(id1, id2);

	// Verify all can be resolved
	EXPECT_EQ(registry.getLabelString(emptyId), "");
	EXPECT_EQ(registry.getLabelString(id1), "Label1");
	EXPECT_EQ(registry.getLabelString(id2), "Label2");
}
