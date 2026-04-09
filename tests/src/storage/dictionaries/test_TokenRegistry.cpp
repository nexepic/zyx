/**
 * @file test_TokenRegistry.cpp
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
#include "graph/storage/dictionaries/TokenRegistry.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

namespace fs = std::filesystem;

class TokenRegistryTest : public ::testing::Test {
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
TEST_F(TokenRegistryTest, InitializationNew) {
	// Inject the SystemStateManager. It should detect no existing key and create a new B+ Tree.
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	// Should have allocated a new root index ID (non-zero)
	EXPECT_GT(registry.getRootIndexId(), 0);
}

/**
 * @brief Test basic string to ID creation and retrieval.
 */
TEST_F(TokenRegistryTest, CreateAndResolveLabel) {
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);
	std::string labelName = "Person";

	// 1. Create ID
	int64_t id = registry.getOrCreateTokenId(labelName);
	EXPECT_GT(id, 0);

	// 2. Resolve back to string
	std::string resolved = registry.resolveTokenName(id);
	EXPECT_EQ(resolved, labelName);
}

/**
 * @brief Test that the same string returns the same ID (Deduplication).
 */
TEST_F(TokenRegistryTest, Deduplication) {
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);
	std::string labelName = "City";

	int64_t id1 = registry.getOrCreateTokenId(labelName);

	// Calling it again should return the existing ID
	int64_t id2 = registry.getOrCreateTokenId(labelName);

	EXPECT_EQ(id1, id2);
}

/**
 * @brief Test empty string handling.
 */
TEST_F(TokenRegistryTest, HandleEmptyString) {
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	int64_t id = registry.getOrCreateTokenId("");
	EXPECT_EQ(id, graph::storage::TokenRegistry::NULL_TOKEN_ID);
}

/**
 * @brief Test resolving a null ID.
 */
TEST_F(TokenRegistryTest, HandleNullId) {
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	std::string label = registry.resolveTokenName(graph::storage::TokenRegistry::NULL_TOKEN_ID);
	EXPECT_EQ(label, "");
}

/**
 * @brief Test resolving a non-existent ID (should handle gracefully).
 */
TEST_F(TokenRegistryTest, HandleInvalidId) {
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	// ID 999999 likely doesn't exist in a fresh DB
	std::string label = registry.resolveTokenName(999999);
	EXPECT_EQ(label, "");
}

/**
 * @brief Test that the registry works correctly after cache eviction.
 * The implementation clears the cache entirely when it exceeds CACHE_SIZE.
 */
TEST_F(TokenRegistryTest, CacheEviction) {
	// Use small cache size to test eviction without creating thousands of labels
	constexpr size_t TEST_CACHE_SIZE = 20;
	graph::storage::TokenRegistry registry(dataManager, systemStateManager, TEST_CACHE_SIZE);

	// Add more than cache size
	size_t limit = TEST_CACHE_SIZE + 10;
	std::vector<int64_t> ids;

	// 1. Fill cache and trigger eviction
	for (size_t i = 0; i < limit; ++i) {
		std::string label = "Label_" + std::to_string(i);
		ids.push_back(registry.getOrCreateTokenId(label));
	}

	// 2. Retrieve the very first item.
	// Since cache was likely evicted, this forces a look up from Blob/Index storage.
	std::string firstLabel = "Label_0";
	int64_t firstId = ids[0];

	// Verify resolving ID -> String works from disk
	EXPECT_EQ(registry.resolveTokenName(firstId), firstLabel);

	// Verify resolving String -> ID works from index
	EXPECT_EQ(registry.getOrCreateTokenId(firstLabel), firstId);
}

/**
 * @brief Test persistence simulation.
 * We create labels, flush the database, close it, and reopen it.
 * We verify that the new DataManager (and its internal Registry) resolves the old labels correctly.
 * This confirms that the RootIndexID was correctly saved to SystemStateManager.
 */
TEST_F(TokenRegistryTest, Persistence) {
	int64_t personId = 0;
	int64_t cityId = 0;
	int64_t savedRootId = 0;

	// Scope 1: Initial creation
	{
		// Using the registry managed by DataManager (or manually created via SM)
		// Here we test manually creating one to ensure logic holds
		graph::storage::TokenRegistry registry(dataManager, systemStateManager);

		personId = registry.getOrCreateTokenId("Person");
		cityId = registry.getOrCreateTokenId("City");
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
		graph::storage::TokenRegistry registry(dataManager, systemStateManager);

		// 1. Verify Root ID was reloaded correctly
		EXPECT_EQ(registry.getRootIndexId(), savedRootId);
		EXPECT_GT(registry.getRootIndexId(), 0);

		// 2. Check ID -> String resolution
		EXPECT_EQ(registry.resolveTokenName(personId), "Person");
		EXPECT_EQ(registry.resolveTokenName(cityId), "City");

		// 3. Check String -> ID resolution (Index lookup should find existing IDs)
		EXPECT_EQ(registry.getOrCreateTokenId("Person"), personId);

		// 4. Add new label to ensure the tree is still writable
		int64_t countryId = registry.getOrCreateTokenId("Country");
		EXPECT_GT(countryId, 0);
		EXPECT_NE(countryId, personId);

		// Verify the new label is retrievable
		EXPECT_EQ(registry.resolveTokenName(countryId), "Country");
	}
}

/**
 * @brief Integration Test via DataManager.
 * Ensures the wiring inside DataManager::setSystemStateManager works correctly.
 */
TEST_F(TokenRegistryTest, DataManagerIntegration) {
	// Since SetUp calls DataManager::setSystemStateManager implicitly (via FileStorage init),
	// DataManager should already have a working registry.

	int64_t id = dataManager->getOrCreateTokenId("IntegratedLabel");
	EXPECT_GT(id, 0);

	std::string result = dataManager->resolveTokenName(id);
	EXPECT_EQ(result, "IntegratedLabel");
}

/**
 * @brief Test extremely long labels (Blob support).
 * Since the implementation uses Blobs, it should handle strings larger than typical buffer limits.
 */
TEST_F(TokenRegistryTest, LongLabelSupport) {
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	// Create a 1KB string
	std::string longLabel(1024, 'A');

	int64_t id = registry.getOrCreateTokenId(longLabel);
	EXPECT_GT(id, 0);

	std::string retrieved = registry.resolveTokenName(id);
	EXPECT_EQ(retrieved, longLabel);
	EXPECT_EQ(retrieved.size(), 1024ULL);
}

// ==========================================
// Additional Tests for Branch Coverage
// ==========================================

TEST_F(TokenRegistryTest, MultipleLabelsCacheMiss) {
	// Test cache misses after eviction using small cache
	constexpr size_t TEST_CACHE_SIZE = 20;
	graph::storage::TokenRegistry registry(dataManager, systemStateManager, TEST_CACHE_SIZE);

	// Create more labels than cache size
	size_t labelCount = TEST_CACHE_SIZE + 5;
	std::vector<std::string> labels;
	for (size_t i = 0; i < labelCount; ++i) {
		labels.push_back("CacheMiss_" + std::to_string(i));
	}

	std::vector<int64_t> ids;
	for (const auto &label: labels) {
		ids.push_back(registry.getOrCreateTokenId(label));
	}

	// Verify all IDs are unique
	std::unordered_set<int64_t> idSet(ids.begin(), ids.end());
	EXPECT_EQ(idSet.size(), labels.size());

	// Verify all labels can be resolved (some from disk after eviction)
	for (size_t i = 0; i < labels.size(); ++i) {
		EXPECT_EQ(registry.resolveTokenName(ids[i]), labels[i]);
	}
}

TEST_F(TokenRegistryTest, SpecialCharactersInLabel) {
	// Test labels with special characters
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

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
		int64_t id = registry.getOrCreateTokenId(label);
		EXPECT_GT(id, 0);

		std::string retrieved = registry.resolveTokenName(id);
		EXPECT_EQ(retrieved, label);
	}
}

TEST_F(TokenRegistryTest, UnicodeLabels) {
	// Test Unicode labels
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	std::vector<std::string> unicodeLabels = {
		"标签",
		"Label",
		"Метки",
		"Etichette",
		"Étiquettes",
		"ラベル"
	};

	for (const auto &label: unicodeLabels) {
		int64_t id = registry.getOrCreateTokenId(label);
		EXPECT_GT(id, 0);

		std::string retrieved = registry.resolveTokenName(id);
		EXPECT_EQ(retrieved, label);
	}
}

TEST_F(TokenRegistryTest, VeryLongLabel) {
	// Test a very long label (larger than typical blob)
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	// Create a 10KB string
	std::string veryLongLabel(10 * 1024, 'X');

	int64_t id = registry.getOrCreateTokenId(veryLongLabel);
	EXPECT_GT(id, 0);

	std::string retrieved = registry.resolveTokenName(id);
	EXPECT_EQ(retrieved, veryLongLabel);
	EXPECT_EQ(retrieved.size(), 10 * 1024ULL);
}

TEST_F(TokenRegistryTest, CaseSensitiveLabels) {
	// Test that labels are case-sensitive
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	int64_t id1 = registry.getOrCreateTokenId("Person");
	int64_t id2 = registry.getOrCreateTokenId("PERSON");
	int64_t id3 = registry.getOrCreateTokenId("person");
	int64_t id4 = registry.getOrCreateTokenId("Person"); // Should match id1

	EXPECT_NE(id1, id2);
	EXPECT_NE(id2, id3);
	EXPECT_NE(id1, id3);
	EXPECT_EQ(id1, id4);
}

TEST_F(TokenRegistryTest, CacheConsistencyAfterPersistence) {
	// Test cache consistency across database restart
	int64_t labelId = 0;

	// Scope 1: Create label and cache it
	{
		graph::storage::TokenRegistry registry(dataManager, systemStateManager);
		labelId = registry.getOrCreateTokenId("CachedLabel");
		EXPECT_GT(labelId, 0);

		// Verify it's cached
		EXPECT_EQ(registry.resolveTokenName(labelId), "CachedLabel");

		storage->flush();
	}

	// Scope 2: Reopen - cache should be empty but data available
	reopenStorage();

	{
		graph::storage::TokenRegistry registry(dataManager, systemStateManager);

		// Should load from disk (cache miss after restart)
		EXPECT_EQ(registry.resolveTokenName(labelId), "CachedLabel");

		// Should also find in index
		EXPECT_EQ(registry.getOrCreateTokenId("CachedLabel"), labelId);
	}
}

TEST_F(TokenRegistryTest, MultipleRegistriesIndependentCaches) {
	// Test that multiple registry instances maintain independent caches
	graph::storage::TokenRegistry registry1(dataManager, systemStateManager);
	graph::storage::TokenRegistry registry2(dataManager, systemStateManager);

	// Create label in registry1
	int64_t id1 = registry1.getOrCreateTokenId("SharedLabel");
	EXPECT_GT(id1, 0);

	// Resolve in registry2 - should hit index, not registry2's cache
	int64_t id2 = registry2.getOrCreateTokenId("SharedLabel");
	EXPECT_EQ(id2, id1);

	// Both should resolve correctly
	EXPECT_EQ(registry1.resolveTokenName(id1), "SharedLabel");
	EXPECT_EQ(registry2.resolveTokenName(id2), "SharedLabel");
}

TEST_F(TokenRegistryTest, EmptyCacheAfterCreation) {
	// Test behavior when cache is empty (initial state)
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	// First access should populate cache
	std::string label = "FirstLabel";
	int64_t id = registry.getOrCreateTokenId(label);

	// Verify it works
	EXPECT_GT(id, 0);
	EXPECT_EQ(registry.resolveTokenName(id), label);
}

TEST_F(TokenRegistryTest, LabelWithNullBytes) {
	// Test labels containing null bytes
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	std::string labelWithNull = std::string("Before") + '\0' + "After";

	int64_t id = registry.getOrCreateTokenId(labelWithNull);
	EXPECT_GT(id, 0);

	std::string retrieved = registry.resolveTokenName(id);
	EXPECT_EQ(retrieved, labelWithNull);
}

TEST_F(TokenRegistryTest, ZeroIdHandling) {
	// Test handling of ID 0 (null label)
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	// ID 0 should return empty string
	std::string label = registry.resolveTokenName(0);
	EXPECT_EQ(label, "");

	// Empty string should return ID 0
	int64_t id = registry.getOrCreateTokenId("");
	EXPECT_EQ(id, graph::storage::TokenRegistry::NULL_TOKEN_ID);
}

TEST_F(TokenRegistryTest, NegativeIdHandling) {
	// Test handling of negative IDs
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	// Negative IDs should return empty string
	std::string label = registry.resolveTokenName(-1);
	EXPECT_EQ(label, "");

	std::string label2 = registry.resolveTokenName(-999);
	EXPECT_EQ(label2, "");
}

TEST_F(TokenRegistryTest, RapidSequentialCreations) {
	// Test rapid sequential label creations
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	std::vector<int64_t> ids;
	for (int i = 0; i < 100; ++i) {
		std::string label = "RapidLabel_" + std::to_string(i);
		ids.push_back(registry.getOrCreateTokenId(label));
	}

	// Verify all unique
	std::unordered_set<int64_t> idSet(ids.begin(), ids.end());
	EXPECT_EQ(idSet.size(), 100ULL);

	// Verify all resolvable
	for (int i = 0; i < 100; ++i) {
		std::string expected = "RapidLabel_" + std::to_string(i);
		EXPECT_EQ(registry.resolveTokenName(ids[i]), expected);
	}
}

TEST_F(TokenRegistryTest, LabelWithOnlySpaces) {
	// Test labels with only spaces
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

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
		int64_t id = registry.getOrCreateTokenId(label);
		EXPECT_GT(id, 0);

		std::string retrieved = registry.resolveTokenName(id);
		EXPECT_EQ(retrieved, label);
	}
}

TEST_F(TokenRegistryTest, LargeBatchOfUniqueLabels) {
	// Test creating a large batch of unique labels
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	constexpr int LARGE_COUNT = 50;
	std::vector<std::string> labels;
	std::vector<int64_t> ids;

	for (int i = 0; i < LARGE_COUNT; ++i) {
		labels.push_back("BatchLabel_" + std::to_string(i));
		ids.push_back(registry.getOrCreateTokenId(labels[i]));
	}

	// Verify all unique
	std::unordered_set<int64_t> idSet(ids.begin(), ids.end());
	EXPECT_EQ(idSet.size(), static_cast<size_t>(LARGE_COUNT));

	// Verify random subset
	for (int i = 0; i < 10; ++i) {
		int idx = rand() % LARGE_COUNT;
		EXPECT_EQ(registry.resolveTokenName(ids[idx]), labels[idx]);
	}
}

TEST_F(TokenRegistryTest, RootIndexIdChanges) {
	// Test that rootIndexId changes are detected and persisted
	int64_t initialRootId = 0;

	{
		graph::storage::TokenRegistry registry(dataManager, systemStateManager);
		initialRootId = registry.getRootIndexId();
		EXPECT_GT(initialRootId, 0);

		// Force root change by creating many labels
		for (int i = 0; i < 30; ++i) {
			registry.getOrCreateTokenId("RootChangeLabel_" + std::to_string(i));
		}

		storage->flush();
	}

	// Reopen and verify
	reopenStorage();

	{
		graph::storage::TokenRegistry registry(dataManager, systemStateManager);
		int64_t newRootId = registry.getRootIndexId();

		// Root ID should be loaded correctly
		EXPECT_GT(newRootId, 0);

		// Labels should still be resolvable
		std::string firstLabel = registry.resolveTokenName(1);
		EXPECT_FALSE(firstLabel.empty());
	}
}

TEST_F(TokenRegistryTest, CacheHitPath) {
	// Test the cache hit path explicitly
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	std::string label = "CacheHitLabel";
	int64_t id = registry.getOrCreateTokenId(label);

	// First call populates cache, second should hit cache
	int64_t id2 = registry.getOrCreateTokenId(label);
	EXPECT_EQ(id, id2);

	// Resolve should also hit cache
	std::string resolved = registry.resolveTokenName(id);
	EXPECT_EQ(resolved, label);
}

TEST_F(TokenRegistryTest, NonExistentIdInCache) {
	// Test resolving an ID that doesn't exist
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	// This ID was never created
	int64_t fakeId = 99999;
	std::string label = registry.resolveTokenName(fakeId);

	// Should return empty string (not found)
	EXPECT_EQ(label, "");
}

TEST_F(TokenRegistryTest, MixedEmptyAndNonEmptyLabels) {
	// Test mixing empty and non-empty labels
	graph::storage::TokenRegistry registry(dataManager, systemStateManager);

	// Empty label
	int64_t emptyId = registry.getOrCreateTokenId("");
	EXPECT_EQ(emptyId, graph::storage::TokenRegistry::NULL_TOKEN_ID);

	// Non-empty labels
	int64_t id1 = registry.getOrCreateTokenId("Label1");
	int64_t id2 = registry.getOrCreateTokenId("Label2");

	EXPECT_GT(id1, 0);
	EXPECT_GT(id2, 0);
	EXPECT_NE(id1, id2);

	// Verify all can be resolved
	EXPECT_EQ(registry.resolveTokenName(emptyId), "");
	EXPECT_EQ(registry.resolveTokenName(id1), "Label1");
	EXPECT_EQ(registry.resolveTokenName(id2), "Label2");
}
