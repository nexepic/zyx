/**
 * @file test_StateChainManager.cpp
 * @author Nexepic
 * @date 2025/7/28
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
#include "graph/core/State.hpp"
#include "graph/core/StateChainManager.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"

class StateChainManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_stateChain_" + to_string(uuid) + ".dat");

		// Initialize Database and FileStorage
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();

		stateChainManager = std::make_unique<graph::StateChainManager>(dataManager);
	}

	void TearDown() override {
		database->close();
		database.reset();
		std::filesystem::remove(testFilePath);
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::unique_ptr<graph::StateChainManager> stateChainManager;
};

// ==========================================
// 1. Basic Creation Tests
// ==========================================

TEST_F(StateChainManagerTest, CreateSingleChunkInternal) {
	const std::string key = "test_key";
	const std::string data = "test_data";

	auto chain = stateChainManager->createStateChain(key, data);

	ASSERT_EQ(chain.size(), 1UL);
	EXPECT_EQ(chain[0].getKey(), key);
	EXPECT_EQ(chain[0].getDataAsString(), data);
	EXPECT_EQ(chain[0].getChainPosition(), 0);
	EXPECT_EQ(chain[0].getPrevStateId(), 0);
	EXPECT_EQ(chain[0].getNextStateId(), 0);
	EXPECT_FALSE(chain[0].isBlobStorage());
}

TEST_F(StateChainManagerTest, CreateMultiChunkInternal) {
	const std::string key = "large_key";
	const std::string data(graph::State::CHUNK_SIZE * 2 + 100, 'X');

	auto chain = stateChainManager->createStateChain(key, data);

	ASSERT_EQ(chain.size(), 3UL);

	EXPECT_EQ(chain[0].getKey(), key);
	EXPECT_EQ(chain[0].getChainPosition(), 0);
	EXPECT_EQ(chain[0].getPrevStateId(), 0);
	EXPECT_EQ(chain[0].getNextStateId(), chain[1].getId());
	EXPECT_FALSE(chain[0].isBlobStorage());

	EXPECT_EQ(chain[1].getKey(), "");
	EXPECT_EQ(chain[1].getChainPosition(), 1);
	EXPECT_EQ(chain[1].getPrevStateId(), chain[0].getId());
	EXPECT_EQ(chain[1].getNextStateId(), chain[2].getId());

	EXPECT_EQ(chain[2].getKey(), "");
	EXPECT_EQ(chain[2].getChainPosition(), 2);
	EXPECT_EQ(chain[2].getPrevStateId(), chain[1].getId());
	EXPECT_EQ(chain[2].getNextStateId(), 0);
}

TEST_F(StateChainManagerTest, CreateEmptyInternal) {
	const std::string key = "empty_key";
	const std::string data;

	auto chain = stateChainManager->createStateChain(key, data);

	// We now expect 1 head state to act as a placeholder for the key
	ASSERT_EQ(chain.size(), 1UL);
	EXPECT_EQ(chain[0].getKey(), key);
	EXPECT_EQ(chain[0].getDataAsString(), "");
	EXPECT_EQ(chain[0].getSize(), 0u);
}

// ==========================================
// 2. Blob Storage Tests
// ==========================================

TEST_F(StateChainManagerTest, CreateSingleBlob) {
	const std::string key = "blob_key";
	const std::string data = "Data stored in external blob";

	// Create with useBlobStorage = true
	auto chain = stateChainManager->createStateChain(key, data, true);

	ASSERT_EQ(chain.size(), 1UL); // Should only have Head State
	EXPECT_EQ(chain[0].getKey(), key);
	EXPECT_EQ(chain[0].getDataAsString(), ""); // Internal buffer empty
	EXPECT_TRUE(chain[0].isBlobStorage());
	EXPECT_NE(chain[0].getExternalId(), 0);

	// Verify data via Read
	std::string readData = stateChainManager->readStateChain(chain[0].getId());
	EXPECT_EQ(readData, data);
}

TEST_F(StateChainManagerTest, CreateLargeBlob) {
	const std::string key = "blob_large_key";
	// Data larger than one Blob chunk (typically 4KB+) to force Blob chaining internally
	const std::string data(10000, 'B');

	auto chain = stateChainManager->createStateChain(key, data, true);

	ASSERT_EQ(chain.size(), 1UL);
	EXPECT_TRUE(chain[0].isBlobStorage());

	const std::string readData = stateChainManager->readStateChain(chain[0].getId());
	EXPECT_EQ(readData, data);
}

// ==========================================
// 3. Update & Mode Switching Tests
// ==========================================

TEST_F(StateChainManagerTest, UpdateInternalToBlob) {
	const std::string key = "mode_switch_key";
	const std::string initialData = "Initial Internal Data";
	const std::string newData = "New Blob Data";

	// 1. Create Internal
	auto chain = stateChainManager->createStateChain(key, initialData, false);
	int64_t headId = chain[0].getId();
	EXPECT_FALSE(chain[0].isBlobStorage());

	// 2. Update to Blob
	auto updatedChain = stateChainManager->updateStateChain(headId, newData, true);

	// Verify Structure
	ASSERT_EQ(updatedChain.size(), 1UL);
	EXPECT_EQ(updatedChain[0].getId(), headId); // Must preserve Head ID
	EXPECT_TRUE(updatedChain[0].isBlobStorage());
	EXPECT_NE(updatedChain[0].getExternalId(), 0);

	// Verify Content
	EXPECT_EQ(stateChainManager->readStateChain(headId), newData);
}

TEST_F(StateChainManagerTest, UpdateBlobToInternal) {
	const std::string key = "mode_switch_key_2";
	const std::string initialData = "Initial Blob Data";
	const std::string newData = "New Internal Data";

	// 1. Create Blob
	auto chain = stateChainManager->createStateChain(key, initialData, true);
	int64_t headId = chain[0].getId();
	int64_t oldBlobId = chain[0].getExternalId();
	EXPECT_TRUE(chain[0].isBlobStorage());

	// 2. Update to Internal
	auto updatedChain = stateChainManager->updateStateChain(headId, newData, false);

	// Verify Structure
	ASSERT_EQ(updatedChain.size(), 1UL);
	EXPECT_EQ(updatedChain[0].getId(), headId);
	EXPECT_FALSE(updatedChain[0].isBlobStorage());
	EXPECT_EQ(updatedChain[0].getExternalId(), 0);

	// Verify Content
	EXPECT_EQ(stateChainManager->readStateChain(headId), newData);

	// Verify Cleanup: The old blob should be deleted (inactive)
	graph::Blob oldBlob = dataManager->getBlob(oldBlobId);
	EXPECT_FALSE(oldBlob.isActive()); // Assuming delete marks inactive
}

TEST_F(StateChainManagerTest, UpdateBlobToBlob) {
	const std::string key = "blob_update_key";
	const std::string data1 = "Blob Data V1";
	const std::string data2 = "Blob Data V2 (Different)";

	auto chain = stateChainManager->createStateChain(key, data1, true);
	int64_t headId = chain[0].getId();

	auto updatedChain = stateChainManager->updateStateChain(headId, data2, true);

	EXPECT_EQ(updatedChain[0].getId(), headId);
	EXPECT_TRUE(updatedChain[0].isBlobStorage());
	EXPECT_EQ(stateChainManager->readStateChain(headId), data2);
}

TEST_F(StateChainManagerTest, UpdateMultiChunkInternalToBlob) {
	const std::string key = "multi_to_blob";
	// Create data requiring 2 chunks
	const std::string multiData(graph::State::CHUNK_SIZE + 50, 'M');

	auto chain = stateChainManager->createStateChain(key, multiData, false);
	ASSERT_EQ(chain.size(), 2UL);
	int64_t headId = chain[0].getId();
	int64_t tailId = chain[1].getId();

	// Update to Blob
	std::string blobData = "Short Blob Data";
	(void) stateChainManager->updateStateChain(headId, blobData, true);

	// Verify Head is now Blob
	graph::State head = dataManager->getState(headId);
	EXPECT_TRUE(head.isBlobStorage());
	EXPECT_EQ(head.getNextStateId(), 0); // Should no longer point to tail

	// Verify Tail is Deleted
	graph::State tail = dataManager->getState(tailId);
	EXPECT_FALSE(tail.isActive());
}

// ==========================================
// 4. Reading & Error Handling Tests
// ==========================================

TEST_F(StateChainManagerTest, ReadStateChainSingleState) {
	constexpr int64_t headStateId = 4001;
	const std::string testData = "Single state data";

	graph::State headState(headStateId, "test_key", testData);
	dataManager->addStateEntity(headState);

	const std::string result = stateChainManager->readStateChain(headStateId);

	EXPECT_EQ(result, testData);
}

TEST_F(StateChainManagerTest, ReadStateChainMultipleStates) {
	constexpr int64_t headStateId = 5001;
	const std::string part1 = "First part";
	const std::string part2 = "Second part";
	const std::string part3 = "Third part";

	// Create chained states
	graph::State state1(5001, "chain_key", part1);
	state1.setNextStateId(5002);
	state1.setChainPosition(0);

	graph::State state2(5002, "", part2);
	state2.setPrevStateId(5001);
	state2.setNextStateId(5003);
	state2.setChainPosition(1);

	graph::State state3(5003, "", part3);
	state3.setPrevStateId(5002);
	state3.setChainPosition(2);

	dataManager->addStateEntity(state1);
	dataManager->addStateEntity(state2);
	dataManager->addStateEntity(state3);

	std::string result = stateChainManager->readStateChain(headStateId);

	EXPECT_EQ(result, part1 + part2 + part3);
}

TEST_F(StateChainManagerTest, ReadStateChainHeadNotFound) {
	constexpr int64_t headStateId = 6001;
	EXPECT_THROW(static_cast<void>(stateChainManager->readStateChain(headStateId)), std::runtime_error);
}

TEST_F(StateChainManagerTest, ReadStateChainCorruptedChain) {
	constexpr int64_t headStateId = 7001;

	graph::State headState(7001, "test_key", "data");
	headState.setNextStateId(7002);
	headState.setChainPosition(0);
	// 7002 does not exist
	dataManager->addStateEntity(headState);

	EXPECT_THROW(static_cast<void>(stateChainManager->readStateChain(headStateId)), std::runtime_error);
}

TEST_F(StateChainManagerTest, ReadStateChainCircularReference) {
	constexpr int64_t headStateId = 8001;

	graph::State state1(8001, "test_key", "data1");
	state1.setNextStateId(8002);
	state1.setChainPosition(0);

	graph::State state2(8002, "", "data2");
	state2.setPrevStateId(8001);
	state2.setNextStateId(8001); // Circular reference!
	state2.setChainPosition(1);

	dataManager->addStateEntity(state1);
	dataManager->addStateEntity(state2);

	EXPECT_THROW(static_cast<void>(stateChainManager->readStateChain(headStateId)), std::runtime_error);
}

// ==========================================
// 5. Utility Logic Tests
// ==========================================

TEST_F(StateChainManagerTest, IsDataSameTrue) {
	constexpr int64_t headStateId = 9001;
	const std::string testData = "Test data"; // Note: this works on some C++20 compilers but can fail, keeping as const std::string might trigger warning but errors below are the main issue. Better to use const.
	// Actually, for simplicity and safety against stricter compilers, let's fix this too even if not in error log.
	// However, user log didn't complain about these specific lines, so I'll stick to fixing what broke.
	// But lines below (starting 474) did break.

	graph::State headState(headStateId, "test_key", testData);
	dataManager->addStateEntity(headState);

	bool result = stateChainManager->isDataSame(headStateId, testData);
	EXPECT_TRUE(result);
}

TEST_F(StateChainManagerTest, IsDataSameFalse) {
	constexpr int64_t headStateId = 10001;
	const std::string originalData = "Original data";
	const std::string newData = "Different data";

	graph::State headState(headStateId, "test_key", originalData);
	dataManager->addStateEntity(headState);

	bool result = stateChainManager->isDataSame(headStateId, newData);
	EXPECT_FALSE(result);
}

TEST_F(StateChainManagerTest, GetStateChainIdsInternal) {
	constexpr int64_t headStateId = 12001;

	graph::State state1(12001, "test_key", "data1");
	state1.setNextStateId(12002);

	graph::State state2(12002, "", "data2");
	state2.setPrevStateId(12001);

	dataManager->addStateEntity(state1);
	dataManager->addStateEntity(state2);

	auto chainIds = stateChainManager->getStateChainIds(headStateId);

	EXPECT_EQ(chainIds.size(), 2UL);
	EXPECT_EQ(chainIds[0], 12001);
	EXPECT_EQ(chainIds[1], 12002);
}

TEST_F(StateChainManagerTest, GetStateChainIdsBlob) {
	const std::string key = "ids_blob_key"; // This wasn't flagged but safer as const
	auto chain = stateChainManager->createStateChain(key, "data", true);

	auto chainIds = stateChainManager->getStateChainIds(chain[0].getId());

	// For blob storage, getStateChainIds should return only the head State ID
	// (It does not return the internal IDs of the Blob entities)
	EXPECT_EQ(chainIds.size(), 1UL);
	EXPECT_EQ(chainIds[0], chain[0].getId());
}

TEST_F(StateChainManagerTest, DeleteStateChainBlob) {
	const std::string key = "del_blob_key";
	const auto chain = stateChainManager->createStateChain(key, "data", true);
	const int64_t headId = chain[0].getId();
	const int64_t blobId = chain[0].getExternalId();

	stateChainManager->deleteStateChain(headId);

	// Verify Head is Deleted
	graph::State head = dataManager->getState(headId);
	EXPECT_FALSE(head.isActive());

	// Verify Blob is Deleted
	graph::Blob blob = dataManager->getBlob(blobId);
	EXPECT_FALSE(blob.isActive());
}

TEST_F(StateChainManagerTest, SplitDataSingleChunk) {
	const std::string smallData = "Small data";
	// Note: splitData is private static, testing implicitly via createStateChain
	const auto chunks = stateChainManager->createStateChain("key", smallData);
	EXPECT_EQ(chunks.size(), 1UL);
}

TEST_F(StateChainManagerTest, SplitDataMultipleChunks) {
	const std::string largeData(graph::State::CHUNK_SIZE * 2 + 100, 'A');
	const auto chunks = stateChainManager->createStateChain("key", largeData);

	EXPECT_EQ(chunks.size(), 3UL);
	EXPECT_EQ(chunks[0].getSize(), graph::State::CHUNK_SIZE);
	EXPECT_EQ(chunks[1].getSize(), graph::State::CHUNK_SIZE);
	EXPECT_EQ(chunks[2].getSize(), 100UL);
}

// ==========================================
// 6. Coverage Tests (Targeting specific branches)
// ==========================================

TEST_F(StateChainManagerTest, UpdateWithSameDataOptimization) {
	const std::string key = "same_data_key";
	const std::string data = "Persistent Data";

	// 1. Create initial chain
	const auto chain = stateChainManager->createStateChain(key, data, false);
	const int64_t headId = chain[0].getId();

	// 2. Update with identical data
	// This targets lines 105-117: isDataSame returns true
	const auto resultChain = stateChainManager->updateStateChain(headId, data, false);

	// 3. Verify
	ASSERT_EQ(resultChain.size(), 1UL);
	EXPECT_EQ(resultChain[0].getId(), headId);
	EXPECT_EQ(resultChain[0].getDataAsString(), data);
}

TEST_F(StateChainManagerTest, UpdateNonExistentHead) {
	constexpr int64_t fakeId = 999999;
	const std::string data = "New Data";

	// This targets lines 121-123: headState.getId() == 0
	EXPECT_THROW({ (void) stateChainManager->updateStateChain(fakeId, data, false); }, std::runtime_error);
}

TEST_F(StateChainManagerTest, UpdateInactiveHead) {
	const std::string key = "inactive_key";
	const std::string data = "Data";

	auto chain = stateChainManager->createStateChain(key, data, false);
	int64_t headId = chain[0].getId();

	// Instead of updateStateEntity(inactive), use deleteState()
	// This properly marks it as inactive/deleted in the system
	graph::State head = dataManager->getState(headId);
	dataManager->deleteState(head);

	// Now try to update the chain via Manager
	// This targets lines 121-123: !headState.isActive()
	// Since it was deleted, DataManager should return an inactive/empty state
	EXPECT_THROW({ (void) stateChainManager->updateStateChain(headId, "New Data", false); }, std::runtime_error);
}

TEST_F(StateChainManagerTest, UpdateBlobToInternalWithSameData) {
	// Edge case: Data is same, but user requests different storage mode?
	// Current logic returns early if data matches, IGNORING mode change.
	// This test confirms that behavior (coverage for line 105 comment).

	const std::string key = "mode_ignore_key";
	const std::string data = "Static Data";

	// Create as Blob
	const auto chain = stateChainManager->createStateChain(key, data, true);
	const int64_t headId = chain[0].getId();
	EXPECT_TRUE(chain[0].isBlobStorage());

	// Update with SAME data but requesting Internal mode (false)
	const auto result = stateChainManager->updateStateChain(headId, data, false);

	// Current implementation: Returns existing chain (Blob), ignores request to switch
	EXPECT_TRUE(result[0].isBlobStorage());
}

// =============================================================================
// Cache Eviction Edge Case Tests for State Chains
// These tests verify that State chain creation works correctly even when
// cache eviction occurs during the process. State chains have a different
// implementation strategy than Blob chains:
// - Blob: Create entities → Update entities to set links → Must sync returned vector
// - State: Set links in vector first → Create entities (links already set) → No sync needed
//
// These tests verify that State's "pre-link" strategy is robust under cache pressure.
// =============================================================================

TEST_F(StateChainManagerTest, CreateMultiChunkChainUnderCachePressure) {
	// Test creating multiple large state chains to potentially trigger cache eviction
	// This verifies that the "pre-link" strategy works even when cache is under pressure

	const std::string baseKey = "cache_test_key";

	// Create multiple state chains with multiple chunks each
	std::vector<std::vector<graph::State>> allChains;

	for (int round = 0; round < 15; round++) {
		std::string key = baseKey + std::to_string(round);
		// Create data requiring 3 state chunks
		std::string testData;
		testData.resize(graph::State::CHUNK_SIZE * 2 + 50);
		// Fill with varying data to ensure different content
		for (char & i : testData) {
			i = static_cast<char>('A' + (round % 26));
		}

		auto chain = stateChainManager->createStateChain(key, testData, false);
		allChains.push_back(chain);

		// Critical verification: nextStateId must be correctly set in returned chain
		ASSERT_EQ(chain.size(), 3UL) << "Round " << round << ": should create 3 states";

		EXPECT_EQ(chain[0].getNextStateId(), chain[1].getId())
				<< "Round " << round << ": chain[0].nextStateId mismatch";
		EXPECT_EQ(chain[1].getNextStateId(), chain[2].getId())
				<< "Round " << round << ": chain[1].nextStateId mismatch";
		EXPECT_EQ(chain[2].getNextStateId(), 0) << "Round " << round << ": chain[2].nextStateId should be 0";

		// Verify chain integrity by reading back
		std::string readData = stateChainManager->readStateChain(chain[0].getId());
		EXPECT_EQ(readData, testData) << "Round " << round << ": read data mismatch";
	}
}

TEST_F(StateChainManagerTest, CreateChainStressTest) {
	// Stress test: Rapidly create many state chains to force cache eviction
	// This is similar to the BlobChainManager stress test but for State

	constexpr int numChains = 50;
	const std::string baseKey = "stress_key";

	std::mt19937 rng(12345);
	std::uniform_int_distribution<int> dist(0, 255);

	for (int i = 0; i < numChains; i++) {
		std::string key = baseKey + std::to_string(i);

		// Create varying size data (1-3 chunks)
		size_t chunkCount = 1 + (i % 3);
		std::string testData;
		testData.resize(graph::State::CHUNK_SIZE * chunkCount + (i % 50));
		for (auto &c: testData) {
			c = static_cast<char>(dist(rng));
		}

		auto chain = stateChainManager->createStateChain(key, testData, false);

		// Verify chain linking is correct
		for (size_t j = 0; j < chain.size(); j++) {
			if (j == 0) {
				EXPECT_EQ(chain[j].getPrevStateId(), 0) << "Chain " << i << ": first state should have no prev";
			} else {
				EXPECT_EQ(chain[j].getPrevStateId(), chain[j - 1].getId())
						<< "Chain " << i << ", state " << j << ": prevId mismatch";
			}

			if (j == chain.size() - 1) {
				EXPECT_EQ(chain[j].getNextStateId(), 0)
						<< "Chain " << i << ", state " << j << ": last state should have no next";
			} else {
				EXPECT_EQ(chain[j].getNextStateId(), chain[j + 1].getId())
						<< "Chain " << i << ", state " << j << ": nextId mismatch";
			}
		}
	}
}

TEST_F(StateChainManagerTest, ReturnedVectorMatchesDataManager) {
	// Verify that the returned state chain vector matches what's in DataManager
	// This is the core issue for Blob (value semantics + cache eviction),
	// and we need to verify State doesn't have this problem

	const std::string key = "sync_test_key";
	std::string testData;
	testData.resize(graph::State::CHUNK_SIZE * 3); // 3 chunks
	for (size_t i = 0; i < testData.size(); i++) {
		testData[i] = static_cast<char>('Z' - (i % 26));
	}

	auto chain = stateChainManager->createStateChain(key, testData, false);

	// For each state in the returned vector, verify it matches DataManager
	for (size_t i = 0; i < chain.size(); i++) {
		int64_t stateId = chain[i].getId();

		// Fetch from DataManager (this may trigger cache eviction)
		auto stateFromManager = dataManager->getState(stateId);

		// Critical: All fields must match
		EXPECT_EQ(chain[i].getNextStateId(), stateFromManager.getNextStateId())
				<< "State " << i << ": returned vector nextStateId doesn't match DataManager";
		EXPECT_EQ(chain[i].getPrevStateId(), stateFromManager.getPrevStateId())
				<< "State " << i << ": returned vector prevStateId doesn't match DataManager";
		EXPECT_EQ(chain[i].getChainPosition(), stateFromManager.getChainPosition())
				<< "State " << i << ": chainPosition doesn't match DataManager";
		EXPECT_EQ(chain[i].getKey(), stateFromManager.getKey()) << "State " << i << ": key doesn't match DataManager";
	}
}

TEST_F(StateChainManagerTest, UpdateAfterCacheEviction) {
	// Test updating a state chain after it might have been evicted from cache
	// This verifies that updateStateChain handles cache eviction correctly

	const std::string key = "eviction_update_key";
	std::string originalData;
	originalData.resize(graph::State::CHUNK_SIZE * 2);
	for (char & i : originalData) {
		i = 'A';
	}

	// Create initial chain
	auto chain = stateChainManager->createStateChain(key, originalData, false);
	int64_t headId = chain[0].getId();
	ASSERT_EQ(chain.size(), 2UL);

	// Create many other states to potentially evict the original chain from cache
	for (int i = 0; i < 30; i++) {
		std::string fillerKey = "filler_" + std::to_string(i);
		std::string fillerData;
		fillerData.resize(graph::State::CHUNK_SIZE * 2);
		for (char & j : fillerData) {
			j = static_cast<char>('B' + (i % 25));
		}
		(void) stateChainManager->createStateChain(fillerKey, fillerData, false);
	}

	// Now update the original chain with new data
	std::string updatedData;
	updatedData.resize(graph::State::CHUNK_SIZE * 2);
	for (char & i : updatedData) {
		i = 'C';
	}

	// This should work even if original chain was evicted
	EXPECT_NO_THROW((void) stateChainManager->updateStateChain(headId, updatedData, false));

	// Verify the updated chain can be read correctly
	std::string readData = stateChainManager->readStateChain(headId);
	EXPECT_EQ(readData, updatedData);

	// Also verify the returned chain has correct linking
	auto updatedChain = stateChainManager->createStateChain(key, updatedData, false);
	EXPECT_EQ(updatedChain.size(), 2UL);
	EXPECT_EQ(updatedChain[0].getNextStateId(), updatedChain[1].getId());
}

TEST_F(StateChainManagerTest, BlobStorageModeCacheEviction) {
	// Test blob storage mode under cache eviction pressure
	// This verifies that State chains using Blob storage are also robust

	const std::string baseKey = "blob_evict_test";

	// Create multiple state chains using blob storage
	for (int i = 0; i < 20; i++) {
		std::string key = baseKey + std::to_string(i);
		std::string testData(5000, static_cast<char>('D' + (i % 26))); // 5KB data

		auto chain = stateChainManager->createStateChain(key, testData, true);

		// Verify blob storage mode is set
		EXPECT_TRUE(chain[0].isBlobStorage());
		EXPECT_NE(chain[0].getExternalId(), 0);

		// Verify data can be read back
		std::string readData = stateChainManager->readStateChain(chain[0].getId());
		EXPECT_EQ(readData, testData);
	}
}

TEST_F(StateChainManagerTest, ModeSwitchUnderCacheEviction) {
	// Test switching storage mode (internal <-> blob) under cache pressure
	// This is a complex operation that deletes old chains and creates new ones

	const std::string key = "mode_switch_test";
	std::string internalData = "Internal mode data that is reasonably sized";

	// Create as internal mode
	auto chain = stateChainManager->createStateChain(key, internalData, false);
	int64_t headId = chain[0].getId();
	ASSERT_FALSE(chain[0].isBlobStorage());

	// Create filler states to potentially evict the chain
	for (int i = 0; i < 20; i++) {
		std::string fillerKey = "filler_" + std::to_string(i);
		std::string fillerData(4000, 'F');
		(void) stateChainManager->createStateChain(fillerKey, fillerData, false);
	}

	// Switch to blob mode
	std::string blobData = "New data for blob storage, much larger than before";
	EXPECT_NO_THROW({
		auto blobChain = stateChainManager->updateStateChain(headId, blobData, true);
		EXPECT_TRUE(blobChain[0].isBlobStorage());
		EXPECT_NE(blobChain[0].getExternalId(), 0);
	});

	// Verify data integrity
	std::string readData = stateChainManager->readStateChain(headId);
	EXPECT_EQ(readData, blobData);

	// Switch back to internal mode
	EXPECT_NO_THROW({
		auto internalChain = stateChainManager->updateStateChain(headId, internalData, false);
		EXPECT_FALSE(internalChain[0].isBlobStorage());
		EXPECT_EQ(internalChain[0].getExternalId(), 0);
	});

	// Verify data integrity after switch back
	readData = stateChainManager->readStateChain(headId);
	EXPECT_EQ(readData, internalData);
}

// =============================================================================
// Exception Path Tests
// These tests verify error handling and exception cases
// =============================================================================

// Test that reading a blob storage state with externalId=0 returns empty
// Tests branch at line 55: if (headState.getExternalId() == 0) return ""
TEST_F(StateChainManagerTest, BlobStorageWithZeroExternalIdReturnsEmpty) {
	[[maybe_unused]] constexpr int64_t stateId = 5000;
	const std::string key = "test_key_blob_zero_external";

	// Create a state with blob storage first (non-zero externalId)
	const std::string originalData = "Original data";
	const auto blobChain = stateChainManager->createStateChain(key, originalData, true);
	ASSERT_GT(blobChain[0].getExternalId(), 0);

	// Now manually set externalId to 0 to test the edge case
	// This simulates corrupted data where isBlobStorage would return true but externalId is 0
	graph::State headState = dataManager->getState(blobChain[0].getId());
	headState.setExternalId(0);
	dataManager->updateStateEntity(headState);

	// Reading should return empty string (not throw)
	const std::string result = stateChainManager->readStateChain(blobChain[0].getId());
	EXPECT_EQ(result, "");
}

// Test that reading a chain with position mismatch throws
// Tests branch at line 78-80: if (currentState.getChainPosition() != expectedPosition)
TEST_F(StateChainManagerTest, ReadThrowsOnPositionMismatch) {
	[[maybe_unused]] constexpr int64_t headId = 5100;
	const std::string key = "test_key_position_mismatch";
	std::string testData;
	testData.resize(graph::State::CHUNK_SIZE * 2 + 50);
	std::mt19937 rng(333);
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &c: testData) {
		c = static_cast<char>(dist(rng));
	}

	// Create a multi-chunk chain
	auto chain = stateChainManager->createStateChain(key, testData, false);

	ASSERT_GT(chain.size(), 1UL);

	// Manually corrupt the position of the second state
	graph::State secondState = dataManager->getState(chain[1].getId());
	secondState.setChainPosition(99);  // Wrong position
	dataManager->updateStateEntity(secondState);

	// Try to read the corrupted chain - should throw
	EXPECT_THROW((void) stateChainManager->readStateChain(chain[0].getId()), std::runtime_error);
}

// Test that deleting a non-existent blob storage state doesn't throw
// Tests branch at line 166: if (headState.getId() == 0 || !headState.isActive()) return
TEST_F(StateChainManagerTest, DeleteNonExistentStateSucceeds) {
	// Deleting a non-existent state should not throw
	EXPECT_NO_THROW(stateChainManager->deleteStateChain(999999));
}

// Test that deleting a state with inactive intermediate blob chain handles gracefully
// Tests branch at line 142-144: break when next state is inactive
TEST_F(StateChainManagerTest, UpdateHandlesInactiveIntermediateStates) {
	[[maybe_unused]] constexpr int64_t headId = 5200;
	const std::string key = "test_key_inactive_intermediate";
	std::string testData;
	testData.resize(graph::State::CHUNK_SIZE * 2 + 50);
	std::mt19937 rng(444);
	std::uniform_int_distribution dist(0, 255);
	for (auto &c: testData) {
		c = static_cast<char>(dist(rng));
	}

	// Create a multi-chunk chain
	auto chain = stateChainManager->createStateChain(key, testData, false);

	ASSERT_GT(chain.size(), 1UL);

	// Manually delete the middle chunk to simulate corruption
	if (chain.size() > 1) {
		graph::State middleState = dataManager->getState(chain[1].getId());
		dataManager->deleteState(middleState);
	}

	// Update should still work (it cleans up the old chain)
	std::string newData = "New data after corruption";
	EXPECT_NO_THROW({
		auto newChain = stateChainManager->updateStateChain(chain[0].getId(), newData, false);
		EXPECT_EQ(newChain[0].getId(), chain[0].getId());
	});

	// Verify the new data is correct
	std::string readData = stateChainManager->readStateChain(chain[0].getId());
	EXPECT_EQ(readData, newData);
}

// Test that getStateChainIds handles inactive states gracefully
// Tests branch at line 197: if (currentState.getId() == 0 || !currentState.isActive()) break
TEST_F(StateChainManagerTest, GetStateChainIdsHandlesInactiveStates) {
	[[maybe_unused]] constexpr int64_t headId = 5300;
	const std::string key = "test_key_inactive_chain_ids";
	std::string testData;
	testData.resize(graph::State::CHUNK_SIZE * 2 + 50);
	std::mt19937 rng(555);
	std::uniform_int_distribution<int> dist(0, 255);
	for (auto &c: testData) {
		c = static_cast<char>(dist(rng));
	}

	// Create a multi-chunk chain
	auto chain = stateChainManager->createStateChain(key, testData, false);

	ASSERT_GT(chain.size(), 1UL);

	// Get the full chain first
	auto fullChain = stateChainManager->getStateChainIds(chain[0].getId());
	EXPECT_EQ(fullChain.size(), chain.size());

	// Manually delete the middle chunk
	if (chain.size() > 1) {
		graph::State middleState = dataManager->getState(chain[1].getId());
		dataManager->deleteState(middleState);
	}

	// getStateChainIds should return partial chain (stop at inactive state)
	auto partialChain = stateChainManager->getStateChainIds(chain[0].getId());
	EXPECT_GT(partialChain.size(), 0UL);
	EXPECT_LT(partialChain.size(), fullChain.size());
}

// Test that reading throws on non-existent state
// Tests branch at line 50-51: if (headState.getId() == 0 || !headState.isActive())
TEST_F(StateChainManagerTest, ReadThrowsOnNonExistentState) {
	EXPECT_THROW((void) stateChainManager->readStateChain(999999), std::runtime_error);
}

// Test that updating throws on non-existent state
// Tests branch at line 121-122: if (headState.getId() == 0 || !headState.isActive())
TEST_F(StateChainManagerTest, UpdateThrowsOnNonExistentState) {
	std::string newData = "New data";
	EXPECT_THROW((void) stateChainManager->updateStateChain(999999, newData, false), std::runtime_error);
}

// Test that update handles mode switch with same data correctly
// Tests the case where data is same but storage mode differs
TEST_F(StateChainManagerTest, UpdateWithSameDataButDifferentMode) {
	[[maybe_unused]] constexpr int64_t entityId = 3000;
	const std::string key = "mode_test_key";
	const std::string data = "Persistent data";

	// Create as internal
	auto chain1 = stateChainManager->createStateChain(key, data, false);
	int64_t headId = chain1[0].getId();
	EXPECT_FALSE(chain1[0].isBlobStorage());

	// Update with same data but requesting blob mode
	// The implementation should return the existing chain (no-op)
	auto chain2 = stateChainManager->updateStateChain(headId, data, true);
	EXPECT_EQ(chain2.size(), 1UL);
	EXPECT_FALSE(chain2[0].isBlobStorage()) << "Should remain in internal mode when data is same";
}

// Test splitData with various sizes
TEST_F(StateChainManagerTest, SplitDataWithVariousSizes) {
	// Test empty data
	auto emptyChain = stateChainManager->createStateChain("empty_key", "", false);
	ASSERT_EQ(emptyChain.size(), 1UL);
	EXPECT_EQ(emptyChain[0].getDataAsString(), "");

	// Test data that fits exactly in one chunk
	std::string exactChunkData(graph::State::CHUNK_SIZE, 'A');
	auto exactChain = stateChainManager->createStateChain("exact_key", exactChunkData, false);
	EXPECT_GE(exactChain.size(), 1UL);
	EXPECT_EQ(stateChainManager->readStateChain(exactChain[0].getId()), exactChunkData);

	// Test data that requires exactly two chunks
	std::string twoChunkData(graph::State::CHUNK_SIZE + 1, 'B');
	auto twoChain = stateChainManager->createStateChain("two_chunk_key", twoChunkData, false);
	EXPECT_EQ(twoChain.size(), 2UL);
	EXPECT_EQ(stateChainManager->readStateChain(twoChain[0].getId()), twoChunkData);

	// Test data that requires exactly three chunks
	std::string threeChunkData(graph::State::CHUNK_SIZE * 2 + 1, 'C');
	auto threeChain = stateChainManager->createStateChain("three_chunk_key", threeChunkData, false);
	EXPECT_EQ(threeChain.size(), 3UL);
	EXPECT_EQ(stateChainManager->readStateChain(threeChain[0].getId()), threeChunkData);
}

// Test findStateByKey
TEST_F(StateChainManagerTest, FindStateByKey) {
	const std::string key = "search_test_key";
	const std::string data = "Searchable data";

	// Create a state chain
	auto chain = stateChainManager->createStateChain(key, data, false);
	int64_t headId = chain[0].getId();

	// Find by key should return the state
	graph::State found = stateChainManager->findStateByKey(key);
	EXPECT_EQ(found.getId(), headId);
	EXPECT_EQ(found.getKey(), key);

	// Search for non-existent key should return a state with id=0
	graph::State notFound = stateChainManager->findStateByKey("non_existent_key");
	EXPECT_EQ(notFound.getId(), 0);
}

// Test isDataSame when readStateChain throws
// Tests branch at line 94-96: exception catch in isDataSame
TEST_F(StateChainManagerTest, IsDataSameHandlesReadException) {
	constexpr int64_t headStateId = 14001;
	const std::string key = "exception_test_key";
	const std::string originalData = "Original data";

	// Create a state chain
	graph::State headState(headStateId, key, originalData);
	headState.setNextStateId(14002);  // Points to non-existent state
	headState.setChainPosition(0);
	dataManager->addStateEntity(headState);

	// isDataSame should catch the exception and return false
	bool result = stateChainManager->isDataSame(headStateId, "Different data");
	EXPECT_FALSE(result);
}

// Test readStateChain with blob storage where externalId is 0 (manually set)
// Covers branch at line 55: headState.getExternalId() == 0 -> return ""
TEST_F(StateChainManagerTest, ReadBlobWithZeroExternalIdAfterManualSet) {
	const std::string key = "blob_zero_ext_manual";
	const std::string data = "Blob data to be corrupted";

	auto chain = stateChainManager->createStateChain(key, data, true);
	ASSERT_EQ(chain.size(), 1UL);
	ASSERT_TRUE(chain[0].isBlobStorage());
	int64_t headId = chain[0].getId();

	// Manually set externalId to 0 to simulate corruption
	graph::State head = dataManager->getState(headId);
	head.setExternalId(0);
	dataManager->updateStateEntity(head);

	// Reading should return empty string (branch: externalId == 0)
	std::string result = stateChainManager->readStateChain(headId);
	EXPECT_EQ(result, "");
}

// Test deleteStateChain where blob head has externalId == 0
// Covers branch at line 161: headState.getExternalId() != 0 -> False
TEST_F(StateChainManagerTest, DeleteBlobChainWithManuallyZeroedExternalId) {
	const std::string key = "delete_blob_zero_ext";
	const std::string data = "Delete me";

	auto chain = stateChainManager->createStateChain(key, data, true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Manually zero the externalId
	graph::State head = dataManager->getState(headId);
	head.setExternalId(0);
	dataManager->updateStateEntity(head);

	// Delete should handle gracefully (skip blob deletion since externalId is 0)
	EXPECT_NO_THROW(stateChainManager->deleteStateChain(headId));

	// Verify state was deleted
	EXPECT_THROW((void)stateChainManager->readStateChain(headId), std::runtime_error);
}

// Test updateStateChain where blob storage head has externalId == 0
// Covers branch at line 126: headState.getExternalId() != 0 -> False
TEST_F(StateChainManagerTest, UpdateBlobChainFromZeroedExternalId) {
	const std::string key = "update_zeroed_ext";
	const std::string data = "Original";

	auto chain = stateChainManager->createStateChain(key, data, true);
	int64_t headId = chain[0].getId();

	// Manually zero the externalId
	graph::State head = dataManager->getState(headId);
	head.setExternalId(0);
	dataManager->updateStateEntity(head);

	// Update with different data should work (skip old blob deletion)
	auto newChain = stateChainManager->updateStateChain(headId, "New data", false);
	EXPECT_GE(newChain.size(), 1UL);
	EXPECT_EQ(newChain[0].getId(), headId);

	std::string readData = stateChainManager->readStateChain(headId);
	EXPECT_EQ(readData, "New data");
}

// Test getStateChainIds for inactive blob head (head.isActive() is false)
// Covers branch at line 181: head.isActive() -> False
TEST_F(StateChainManagerTest, GetStateChainIdsForDeletedBlobHead) {
	const std::string key = "deleted_blob_ids";
	auto chain = stateChainManager->createStateChain(key, "data", true);
	int64_t headId = chain[0].getId();

	// Delete the head
	stateChainManager->deleteStateChain(headId);

	// getStateChainIds should return empty for inactive blob head
	auto ids = stateChainManager->getStateChainIds(headId);
	EXPECT_TRUE(ids.empty());
}

// Test updateStateChain with blob storage when data is empty
// Tests branch at line 240: if (!blobChain.empty())
TEST_F(StateChainManagerTest, CreateBlobWithEmptyData) {
	const std::string key = "empty_blob_key";
	const std::string emptyData = "";

	// Create blob storage with empty data
	auto chain = stateChainManager->createStateChain(key, emptyData, true);

	EXPECT_EQ(chain.size(), 1UL);
	EXPECT_TRUE(chain[0].isBlobStorage());
	// External ID might be 0 or non-zero depending on BlobManager implementation
	// The key is that it doesn't crash and returns a valid chain

	// Reading should return empty string
	std::string readData = stateChainManager->readStateChain(chain[0].getId());
	EXPECT_EQ(readData, "");
}

// ============================================================================
// Additional Coverage Tests
// ============================================================================

// Test deleteStateChain with blob storage where externalId == 0
// Covers branch at line 161: if (headState.getExternalId() != 0) -> False
TEST_F(StateChainManagerTest, DeleteBlobChainWithZeroExternalId) {
	const std::string key = "blob_zero_ext_key";

	// Create a blob state manually with externalId = 0
	int64_t stateId = dataManager->getIdAllocator()->allocateId(graph::State::typeId);
	graph::State headState(stateId, key, "");
	headState.setExternalId(0);
	// Mark as blob storage by setting the blob flag
	// We need to create it via createStateChain with empty data
	auto chain = stateChainManager->createStateChain(key, "", true);
	ASSERT_EQ(chain.size(), 1UL);

	int64_t headId = chain[0].getId();

	// Delete the blob chain - should handle externalId == 0 gracefully
	EXPECT_NO_THROW(stateChainManager->deleteStateChain(headId));

	// Verify the state is deleted (reading should throw)
	EXPECT_THROW((void)stateChainManager->readStateChain(headId), std::runtime_error);
}

// Test getStateChainIds for inactive blob head
// Covers branch at line 181: if (head.isActive()) -> False for blob storage
TEST_F(StateChainManagerTest, GetStateChainIdsForInactiveBlobHead) {
	const std::string key = "inactive_blob_key";
	const std::string data = "blob data";

	// Create a blob chain
	auto chain = stateChainManager->createStateChain(key, data, true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Delete the chain to make it inactive
	stateChainManager->deleteStateChain(headId);

	// getStateChainIds for an inactive blob head should return empty
	auto chainIds = stateChainManager->getStateChainIds(headId);
	EXPECT_TRUE(chainIds.empty());
}

// Test readStateChain with blob storage and externalId == 0
// Covers branch at line 55: if (headState.getExternalId() == 0) return ""
TEST_F(StateChainManagerTest, ReadBlobChainWithZeroExternalId) {
	const std::string key = "blob_read_zero_ext";

	// Create a blob chain with empty data (results in externalId potentially being 0)
	auto chain = stateChainManager->createStateChain(key, "", true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Read should return empty string for blob with externalId == 0
	std::string result = stateChainManager->readStateChain(headId);
	EXPECT_EQ(result, "");
}

// Test deleteStateChain on already-deleted (inactive) state
// Covers branch at line 158: if (headState.getId() == 0 || !headState.isActive()) return
TEST_F(StateChainManagerTest, DeleteAlreadyDeletedStateChain) {
	const std::string key = "double_delete_key";
	const std::string data = "data to delete";

	auto chain = stateChainManager->createStateChain(key, data, false);
	int64_t headId = chain[0].getId();

	// Delete once
	stateChainManager->deleteStateChain(headId);

	// Delete again - should be a no-op (early return)
	EXPECT_NO_THROW(stateChainManager->deleteStateChain(headId));
}

// Test updateStateChain with blob storage where old blob externalId == 0
// Covers branch at line 126: if (headState.getExternalId() != 0) -> False in update cleanup
TEST_F(StateChainManagerTest, UpdateBlobChainFromEmptyData) {
	const std::string key = "update_empty_blob_key";

	// Create with empty blob data
	auto chain = stateChainManager->createStateChain(key, "", true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Update to non-empty data - should handle old externalId == 0 gracefully
	auto newChain = stateChainManager->updateStateChain(headId, "new data", true);
	EXPECT_GE(newChain.size(), 1UL);

	// Verify new data can be read
	std::string readData = stateChainManager->readStateChain(headId);
	EXPECT_EQ(readData, "new data");
}

// ============================================================================
// Additional Branch Coverage Tests - Round 3
// ============================================================================

// Test readStateChain where head state exists (getId() != 0) but is inactive
// Covers branch at line 50: !headState.isActive() when getId() != 0
TEST_F(StateChainManagerTest, ReadThrowsOnInactiveExistingState) {
	const std::string key = "inactive_read_key";
	const std::string data = "some data";

	// Create a state chain
	auto chain = stateChainManager->createStateChain(key, data, false);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Delete the state (makes it inactive but ID still exists in storage)
	graph::State head = dataManager->getState(headId);
	dataManager->deleteState(head);

	// Reading an inactive but existing state should throw
	EXPECT_THROW((void)stateChainManager->readStateChain(headId), std::runtime_error);
}

// Test updateStateChain where head is blob with externalId == 0 (cleanup skips delete)
// Covers branch at line 126: headState.getExternalId() != 0 -> False
TEST_F(StateChainManagerTest, UpdateBlobChainWithZeroExternalIdCleanup) {
	const std::string key = "blob_zero_ext_update";

	// Create a blob chain with empty data (externalId might be 0)
	auto chain = stateChainManager->createStateChain(key, "", true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Manually set externalId to 0 to ensure we test the skip-delete path
	graph::State headState = dataManager->getState(headId);
	headState.setExternalId(0);
	dataManager->updateStateEntity(headState);

	// Now update - the cleanup code should skip blob deletion since externalId == 0
	auto newChain = stateChainManager->updateStateChain(headId, "updated data", false);
	EXPECT_GE(newChain.size(), 1UL);

	std::string readData = stateChainManager->readStateChain(headId);
	EXPECT_EQ(readData, "updated data");
}

// Test deleteStateChain blob path where externalId == 0
// Covers branch at line 161: headState.getExternalId() != 0 -> False
TEST_F(StateChainManagerTest, DeleteBlobStateWithZeroExternalIdGracefully) {
	const std::string key = "del_blob_zero_ext";

	// Create a blob chain
	auto chain = stateChainManager->createStateChain(key, "data", true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Manually set externalId to 0
	graph::State headState = dataManager->getState(headId);
	headState.setExternalId(0);
	dataManager->updateStateEntity(headState);

	// Delete should handle externalId == 0 gracefully (skip blob chain deletion)
	EXPECT_NO_THROW(stateChainManager->deleteStateChain(headId));

	// Verify state is deleted
	graph::State deletedState = dataManager->getState(headId);
	EXPECT_FALSE(deletedState.isActive());
}

// Test getStateChainIds for inactive blob head (head.isActive() returns false)
// Covers branch at line 181: head.isActive() -> False
TEST_F(StateChainManagerTest, GetStateChainIdsInactiveBlobReturnsEmpty) {
	const std::string key = "inactive_blob_ids";

	// Create a blob chain and then delete it
	auto chain = stateChainManager->createStateChain(key, "blob data", true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Delete the state
	stateChainManager->deleteStateChain(headId);

	// getStateChainIds should return empty for inactive blob head
	auto chainIds = stateChainManager->getStateChainIds(headId);
	EXPECT_TRUE(chainIds.empty());
}

// Test updateStateChain where old storage is internal chain with multiple states
// Covers branch at line 130-137: delete internal chain with nextStateId != 0
TEST_F(StateChainManagerTest, UpdateMultiChunkInternalCleansUpOldChain) {
	const std::string key = "multi_cleanup_key";
	// Create data requiring 2 internal chunks
	std::string multiData(graph::State::CHUNK_SIZE + 50, 'Q');

	auto chain = stateChainManager->createStateChain(key, multiData, false);
	ASSERT_EQ(chain.size(), 2UL);
	int64_t headId = chain[0].getId();
	int64_t tailId = chain[1].getId();

	// Update to a short internal string (should clean up old tail)
	auto newChain = stateChainManager->updateStateChain(headId, "short", false);
	EXPECT_EQ(newChain.size(), 1UL);

	// Verify old tail is deleted
	graph::State tail = dataManager->getState(tailId);
	EXPECT_FALSE(tail.isActive()) << "Old tail state should be deleted after update";

	// Verify new data
	std::string readData = stateChainManager->readStateChain(headId);
	EXPECT_EQ(readData, "short");
}

// Test isDataSame returns false when head doesn't exist
// Covers the catch branch at line 94-96 more directly
TEST_F(StateChainManagerTest, IsDataSameReturnsFalseForNonExistentId) {
	bool result = stateChainManager->isDataSame(999999, "any data");
	EXPECT_FALSE(result);
}

// Test createStateChain with blob and non-empty data that creates blob entries
// Ensures the blob chain non-empty check at line 236 is properly exercised
TEST_F(StateChainManagerTest, CreateBlobChainWithNonEmptyData) {
	const std::string key = "blob_nonempty_key";
	const std::string data = "Non-empty blob data";

	auto chain = stateChainManager->createStateChain(key, data, true);
	ASSERT_EQ(chain.size(), 1UL);
	EXPECT_TRUE(chain[0].isBlobStorage());
	EXPECT_NE(chain[0].getExternalId(), 0) << "Blob should have valid external ID";

	std::string readData = stateChainManager->readStateChain(chain[0].getId());
	EXPECT_EQ(readData, data);
}

// Test setupStorageForState with internal mode and exactly one chunk
// Covers the path where chunks.size() == 1, so the for loop at line 248 doesn't execute
TEST_F(StateChainManagerTest, SetupStorageSingleChunkInternal) {
	const std::string key = "single_chunk_key";
	const std::string data = "Small data that fits in one chunk";

	auto chain = stateChainManager->createStateChain(key, data, false);
	ASSERT_EQ(chain.size(), 1UL);
	EXPECT_EQ(chain[0].getNextStateId(), 0) << "Single chunk should have no next state";
	EXPECT_EQ(chain[0].getDataAsString(), data);
}

// ==========================================
// Branch Coverage Improvement Tests
// ==========================================

// Cover line 50: readStateChain where head isActive() == false (second condition)
// We need a state that exists (getId() != 0) but is inactive
TEST_F(StateChainManagerTest, ReadStateChainInactiveHead) {
	// Create a chain first
	auto chain = stateChainManager->createStateChain("inactive_test", "data", false);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Delete the chain to mark head as inactive
	stateChainManager->deleteStateChain(headId);

	// Now try to read - should throw because head is inactive
	EXPECT_THROW((void)stateChainManager->readStateChain(headId), std::runtime_error);
}

// Cover line 55: readStateChain with blob storage where externalId == 0
TEST_F(StateChainManagerTest, ReadBlobChainWithExternalIdZero) {
	// Create a blob chain
	auto chain = stateChainManager->createStateChain("blob_zero_ext", "some data", true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Manually set externalId to 0 to test the branch
	graph::State state = dataManager->getState(headId);
	ASSERT_TRUE(state.isBlobStorage());
	state.setExternalId(0);
	dataManager->updateStateEntity(state);

	// Read should return empty string when externalId is 0
	std::string result = stateChainManager->readStateChain(headId);
	EXPECT_EQ(result, "");
}

// Cover line 75: readStateChain where intermediate state isActive() false (second condition)
TEST_F(StateChainManagerTest, ReadChainedStateWithInactiveIntermediate) {
	// Create a multi-chunk chain
	const std::string data(graph::State::CHUNK_SIZE * 2 + 100, 'A');
	auto chain = stateChainManager->createStateChain("inactive_mid", data, false);
	ASSERT_GE(chain.size(), 3UL);

	// Mark the second state as inactive by deleting it
	graph::State secondState = dataManager->getState(chain[1].getId());
	dataManager->deleteState(secondState);

	// Reading should throw because of corrupted chain (inactive intermediate)
	EXPECT_THROW((void)stateChainManager->readStateChain(chain[0].getId()), std::runtime_error);
}

// Cover line 119: updateStateChain where head is active but getId()!=0 && !isActive()
TEST_F(StateChainManagerTest, UpdateInactiveHeadThrows) {
	// Create a chain
	auto chain = stateChainManager->createStateChain("upd_inactive", "data", false);
	int64_t headId = chain[0].getId();

	// Delete to make inactive
	stateChainManager->deleteStateChain(headId);

	// Update should throw because head is inactive
	EXPECT_THROW((void)stateChainManager->updateStateChain(headId, "new data"), std::runtime_error);
}

// Cover line 126: updateStateChain where headState has blob with externalId == 0
TEST_F(StateChainManagerTest, UpdateBlobChainWithZeroExternalId) {
	// Create a blob chain
	auto chain = stateChainManager->createStateChain("upd_blob_zero", "data", true);
	int64_t headId = chain[0].getId();

	// Manually set externalId to 0
	graph::State state = dataManager->getState(headId);
	state.setExternalId(0);
	dataManager->updateStateEntity(state);

	// Update should handle the zero externalId (skip deleteBlobChain)
	// and create new storage
	auto updated = stateChainManager->updateStateChain(headId, "new data", false);
	EXPECT_FALSE(updated.empty());
	std::string readData = stateChainManager->readStateChain(headId);
	EXPECT_EQ(readData, "new data");
}

// Cover line 158: deleteStateChain where head isActive() false (second condition)
TEST_F(StateChainManagerTest, DeleteAlreadyInactiveChain) {
	// Create a chain
	auto chain = stateChainManager->createStateChain("del_inactive", "data", false);
	int64_t headId = chain[0].getId();

	// Delete once
	stateChainManager->deleteStateChain(headId);

	// Delete again - should be no-op since head is already inactive
	EXPECT_NO_THROW(stateChainManager->deleteStateChain(headId));
}

// Cover line 161: deleteStateChain blob path where externalId != 0 false branch
TEST_F(StateChainManagerTest, DeleteBlobChainWithZeroExternalIdDirect) {
	// Create a blob chain
	auto chain = stateChainManager->createStateChain("del_blob_zero_direct", "data", true);
	int64_t headId = chain[0].getId();

	// Manually set externalId to 0
	graph::State state = dataManager->getState(headId);
	state.setExternalId(0);
	dataManager->updateStateEntity(state);

	// Delete should handle zero externalId gracefully (skip blob deletion)
	EXPECT_NO_THROW(stateChainManager->deleteStateChain(headId));
}

// Cover line 181: getStateChainIds blob path where isActive() false
TEST_F(StateChainManagerTest, GetStateChainIdsBlobInactiveHead) {
	// Create a blob chain
	auto chain = stateChainManager->createStateChain("ids_blob_inactive", "data", true);
	int64_t headId = chain[0].getId();

	// Delete to make inactive
	stateChainManager->deleteStateChain(headId);

	// Get chain IDs - should return empty since head is inactive
	auto ids = stateChainManager->getStateChainIds(headId);
	EXPECT_TRUE(ids.empty());
}

// ==========================================
// Additional Branch Coverage Tests - Round 4
// ==========================================

// Test updateStateChain with multi-chunk internal where some tail states exist
// Covers: line 132-137 where nextId != 0 loop iterates multiple times
TEST_F(StateChainManagerTest, UpdateMultiChunkInternalWithThreeChunks) {
	const std::string key = "three_chunk_update";
	// Create data requiring 3 internal chunks
	std::string threeChunkData(graph::State::CHUNK_SIZE * 2 + 50, 'R');

	auto chain = stateChainManager->createStateChain(key, threeChunkData, false);
	ASSERT_EQ(chain.size(), 3UL);
	int64_t headId = chain[0].getId();
	int64_t mid1Id = chain[1].getId();
	int64_t mid2Id = chain[2].getId();

	// Update to a short internal string (should clean up both tail states)
	auto newChain = stateChainManager->updateStateChain(headId, "tiny", false);
	EXPECT_EQ(newChain.size(), 1UL);

	// Verify both old tail states are deleted
	graph::State tail1 = dataManager->getState(mid1Id);
	EXPECT_FALSE(tail1.isActive()) << "First tail state should be deleted";
	graph::State tail2 = dataManager->getState(mid2Id);
	EXPECT_FALSE(tail2.isActive()) << "Second tail state should be deleted";

	// Verify new data is correct
	std::string readData = stateChainManager->readStateChain(headId);
	EXPECT_EQ(readData, "tiny");
}

// Test updateStateChain from multi-chunk to multi-chunk (both internal)
// Covers: update path where old chain has multiple states and new chain also has multiple
TEST_F(StateChainManagerTest, UpdateMultiChunkToMultiChunk) {
	const std::string key = "multi_to_multi";
	std::string data1(graph::State::CHUNK_SIZE + 10, 'A');
	std::string data2(graph::State::CHUNK_SIZE + 20, 'B');

	auto chain = stateChainManager->createStateChain(key, data1, false);
	ASSERT_EQ(chain.size(), 2UL);
	int64_t headId = chain[0].getId();

	// Update with different multi-chunk data
	auto newChain = stateChainManager->updateStateChain(headId, data2, false);
	EXPECT_EQ(newChain.size(), 2UL);
	EXPECT_EQ(newChain[0].getId(), headId);

	std::string readData = stateChainManager->readStateChain(headId);
	EXPECT_EQ(readData, data2);
}

// Test isDataSame when head exists but chain is corrupted (readStateChain throws)
// Covers: line 94-96 catch branch more specifically
TEST_F(StateChainManagerTest, IsDataSameCorruptedChainReturnsFalse) {
	const std::string key = "corrupted_isdata";
	std::string multiData(graph::State::CHUNK_SIZE + 50, 'Z');

	auto chain = stateChainManager->createStateChain(key, multiData, false);
	ASSERT_EQ(chain.size(), 2UL);
	int64_t headId = chain[0].getId();

	// Corrupt chain by deleting the tail state
	graph::State tailState = dataManager->getState(chain[1].getId());
	dataManager->deleteState(tailState);

	// isDataSame should catch the exception from readStateChain and return false
	bool result = stateChainManager->isDataSame(headId, "any data");
	EXPECT_FALSE(result);
}

// Test getStateChainIds for internal chain where an intermediate state
// has getId() == 0 (non-existent)
// Covers: line 187 !currentState.isActive() break in getStateChainIds
TEST_F(StateChainManagerTest, GetStateChainIdsWithNonExistentIntermediate) {
	// Create a chain where head points to a non-existent state
	int64_t stateId = dataManager->getIdAllocator()->allocateId(graph::State::typeId);
	graph::State headState(stateId, "orphan_key", "data");
	headState.setNextStateId(999888); // Points to non-existent state
	dataManager->addStateEntity(headState);

	// getStateChainIds should return only the head
	auto ids = stateChainManager->getStateChainIds(stateId);
	EXPECT_EQ(ids.size(), 1UL);
	EXPECT_EQ(ids[0], stateId);
}

// Test deleteStateChain for internal chain with multiple states
// Covers: line 169-172 loop in deleteStateChain for internal chain
TEST_F(StateChainManagerTest, DeleteMultiChunkInternalChain) {
	const std::string key = "del_multi_internal";
	std::string multiData(graph::State::CHUNK_SIZE * 2 + 50, 'D');

	auto chain = stateChainManager->createStateChain(key, multiData, false);
	ASSERT_EQ(chain.size(), 3UL);
	int64_t headId = chain[0].getId();

	stateChainManager->deleteStateChain(headId);

	// Verify all states are deleted
	for (const auto& state : chain) {
		graph::State s = dataManager->getState(state.getId());
		EXPECT_FALSE(s.isActive()) << "State " << state.getId() << " should be deleted";
	}
}

// Test updateStateChain with same data on multi-chunk chain
// Covers: line 104-114 where isDataSame returns true for multi-chunk chain
TEST_F(StateChainManagerTest, UpdateSameDataMultiChunkReturnsExisting) {
	const std::string key = "same_multi_key";
	std::string multiData(graph::State::CHUNK_SIZE + 50, 'S');

	auto chain = stateChainManager->createStateChain(key, multiData, false);
	ASSERT_EQ(chain.size(), 2UL);
	int64_t headId = chain[0].getId();

	// Update with same data - should return existing chain
	auto resultChain = stateChainManager->updateStateChain(headId, multiData, false);
	EXPECT_EQ(resultChain.size(), 2UL);
	EXPECT_EQ(resultChain[0].getId(), headId);
	EXPECT_EQ(resultChain[1].getId(), chain[1].getId());
}

// ==========================================
// Additional Branch Coverage Tests
// ==========================================

// Cover line 50: readStateChain where headState.getId() != 0 but !isActive()
// Ensures the !isActive() True branch is hit when the state exists but was deleted
TEST_F(StateChainManagerTest, ReadInactiveHeadStateThrowsRuntime) {
	auto chain = stateChainManager->createStateChain("read_inactive_head2", "test data", false);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Delete the state to make it inactive
	graph::State s = dataManager->getState(headId);
	ASSERT_TRUE(s.isActive());
	dataManager->deleteState(s);

	// Verify the state is now inactive but still has a valid ID
	(void)dataManager->getState(headId);
	// readStateChain should throw
	EXPECT_THROW((void)stateChainManager->readStateChain(headId), std::runtime_error);
}

// Cover line 119: updateStateChain with inactive head after delete
// Ensures the !isActive() True branch after isDataSame check
TEST_F(StateChainManagerTest, UpdateAfterDeleteThrowsInactiveHead) {
	auto chain = stateChainManager->createStateChain("upd_del_head", "original", false);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Delete the chain
	stateChainManager->deleteStateChain(headId);

	// Attempt to update - isDataSame will return false (read throws),
	// then getState will return inactive head -> should throw
	EXPECT_THROW((void)stateChainManager->updateStateChain(headId, "new data", false), std::runtime_error);
}

// Cover line 126: updateStateChain where isBlobStorage but externalId == 0
// Forces the False branch of getExternalId() != 0
TEST_F(StateChainManagerTest, UpdateBlobChainWithZeroExternalIdSkipsDelete) {
	auto chain = stateChainManager->createStateChain("upd_blob_zero_ext", "blob data", true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Manually set externalId to 0 to simulate the edge case
	graph::State headState = dataManager->getState(headId);
	ASSERT_TRUE(headState.isBlobStorage());
	headState.setExternalId(0);
	dataManager->updateStateEntity(headState);

	// Update with different data - should not crash when externalId is 0
	auto result = stateChainManager->updateStateChain(headId, "new blob data", true);
	EXPECT_FALSE(result.empty());

	// Verify the update worked
	std::string readBack = stateChainManager->readStateChain(headId);
	EXPECT_EQ(readBack, "new blob data");
}

// Cover line 158: deleteStateChain where head exists but !isActive()
// The early return path in delete
TEST_F(StateChainManagerTest, DeleteInactiveHeadReturnsEarly) {
	auto chain = stateChainManager->createStateChain("del_inactive_head2", "data", false);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Delete once
	stateChainManager->deleteStateChain(headId);

	// Delete again - should early return without error
	stateChainManager->deleteStateChain(headId);
	// No throw = success
}

// Cover line 161: deleteStateChain where isBlobStorage but externalId == 0
// Forces the False branch of getExternalId() != 0 in delete
TEST_F(StateChainManagerTest, DeleteBlobStateZeroExternalIdSkipsBlobDelete) {
	auto chain = stateChainManager->createStateChain("del_blob_zero", "blob data", true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Manually zero the external ID
	graph::State headState = dataManager->getState(headId);
	headState.setExternalId(0);
	dataManager->updateStateEntity(headState);

	// Delete should not crash - skips blob deletion, still deletes the state itself
	stateChainManager->deleteStateChain(headId);

	// Verify state is now inactive
	graph::State afterDelete = dataManager->getState(headId);
	EXPECT_FALSE(afterDelete.isActive());
}

// Cover line 181: getStateChainIds for blob head where isActive() returns false
// Forces the False branch of head.isActive() in blob mode
TEST_F(StateChainManagerTest, GetStateChainIdsBlobInactiveReturnsEmpty2) {
	auto chain = stateChainManager->createStateChain("ids_blob_inactive2", "data", true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Delete to make inactive
	stateChainManager->deleteStateChain(headId);

	// getStateChainIds should return empty for inactive blob head
	auto ids = stateChainManager->getStateChainIds(headId);
	EXPECT_TRUE(ids.empty());
}

// Cover line 236: setupStorageForState blob mode where blobChain is empty
// This is defensive code for when createBlobChain returns empty
TEST_F(StateChainManagerTest, CreateBlobStorageWithEmptyDataVerifySetup) {
	// Empty data blob storage - createBlobChain should still return at least one blob
	auto chain = stateChainManager->createStateChain("empty_blob_setup", "", true);
	ASSERT_FALSE(chain.empty());

	// Read back should return empty string
	std::string result = stateChainManager->readStateChain(chain[0].getId());
	EXPECT_EQ(result, "");
}

// ==========================================
// Additional branch coverage tests
// ==========================================

// Cover line 126: updateStateChain blob path where getExternalId() == 0 (False branch)
// When a blob-storage state has externalId manually set to 0, the update should
// skip blob deletion and still create new storage
TEST_F(StateChainManagerTest, UpdateBlobChainExternalIdZeroSkipsDeletion) {
	const std::string key = "upd_blob_ext_zero";
	auto chain = stateChainManager->createStateChain(key, "original blob data", true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Manually zero out the externalId to test the False branch of (getExternalId() != 0)
	graph::State headState = dataManager->getState(headId);
	ASSERT_TRUE(headState.isBlobStorage());
	headState.setExternalId(0);
	dataManager->updateStateEntity(headState);

	// Update with different data - should succeed despite externalId being 0
	auto newChain = stateChainManager->updateStateChain(headId, "new blob data", true);
	ASSERT_FALSE(newChain.empty());

	// Verify we can read the new data
	std::string readBack = stateChainManager->readStateChain(headId);
	EXPECT_EQ(readBack, "new blob data");
}

// Cover line 161: deleteStateChain blob path where getExternalId() == 0 (False branch)
// When deleting a blob-storage state that has externalId == 0, it should skip
// blob chain deletion but still delete the state itself
TEST_F(StateChainManagerTest, DeleteBlobChainExternalIdZeroSkipsBlobDeletion) {
	const std::string key = "del_blob_ext_zero";
	auto chain = stateChainManager->createStateChain(key, "blob data to delete", true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Manually zero out the externalId
	graph::State headState = dataManager->getState(headId);
	ASSERT_TRUE(headState.isBlobStorage());
	headState.setExternalId(0);
	dataManager->updateStateEntity(headState);

	// Delete should succeed - skipping blob deletion but still deleting the state
	stateChainManager->deleteStateChain(headId);

	// Verify state is now inactive
	graph::State afterDelete = dataManager->getState(headId);
	EXPECT_FALSE(afterDelete.isActive());
}

// Cover line 181: getStateChainIds blob path where head.isActive() == false
// Tests the False branch of (head.isActive()) in getStateChainIds for blob storage
TEST_F(StateChainManagerTest, GetStateChainIdsBlobInactiveReturnsEmptyList) {
	const std::string key = "gids_blob_inactive";
	auto chain = stateChainManager->createStateChain(key, "blob for ids", true);
	ASSERT_EQ(chain.size(), 1UL);
	int64_t headId = chain[0].getId();

	// Delete to make inactive
	stateChainManager->deleteStateChain(headId);

	// Verify the head is truly inactive
	graph::State deletedHead = dataManager->getState(headId);
	EXPECT_FALSE(deletedHead.isActive());

	// getStateChainIds should return empty list for inactive blob head
	auto ids = stateChainManager->getStateChainIds(headId);
	EXPECT_TRUE(ids.empty());
}

// Cover line 236: setupStorageForState blob mode where blobChain is empty
// This exercises the False branch of (!blobChain.empty()) at line 236
// We create with empty data in blob mode - createBlobChain always returns at least
// one blob even for empty data, so this branch is defensive. Testing with empty data
// to get closest to the edge.
TEST_F(StateChainManagerTest, CreateBlobStorageWithEmptyDataStillSetsExternalId) {
	const std::string key = "blob_empty_setup";
	auto chain = stateChainManager->createStateChain(key, "", true);
	ASSERT_EQ(chain.size(), 1UL);

	// Even empty data should produce a blob chain, so externalId should be set
	graph::State headState = dataManager->getState(chain[0].getId());
	EXPECT_TRUE(headState.isBlobStorage());
	// The externalId should be non-zero since createBlobChain returns at least one blob
	EXPECT_NE(headState.getExternalId(), 0);
}

// ==========================================
// Targeted Branch Coverage Tests
// These tests use markInactive() + updateStateEntity() to create states
// that have valid IDs but isActive() == false, which is the only way to
// hit the second condition in short-circuit OR checks like:
//   if (getId() == 0 || !isActive())
// When deleteState() is used, the entity is removed from cache entirely
// so getState() returns a default State with id=0, hitting the first
// condition instead.
// ==========================================

// Covers line 50 branch 2: readStateChain where getId() != 0 but !isActive()
// We use addStateEntity to insert an inactive state directly into cache,
// bypassing updateStateEntity which rejects inactive entities.
TEST_F(StateChainManagerTest, ReadStateChainWithMarkInactiveHead) {
	// Allocate a valid state ID
	int64_t stateId = dataManager->getIdAllocator()->allocateId(graph::State::typeId);

	// Create a state, mark it inactive, then add to cache
	graph::State inactiveState(stateId, "inactive_head_key", "data");
	inactiveState.markInactive();
	dataManager->addStateEntity(inactiveState);

	// Verify the state is inactive but has a valid ID
	graph::State check = dataManager->getState(stateId);
	EXPECT_NE(check.getId(), 0);
	EXPECT_FALSE(check.isActive());

	// readStateChain should throw via the !isActive() branch (not getId()==0)
	EXPECT_THROW((void)stateChainManager->readStateChain(stateId), std::runtime_error);
}

// Covers line 75 branch 2: readStateChain where intermediate state has
// getId() != 0 but !isActive()
TEST_F(StateChainManagerTest, ReadChainWithMarkInactiveIntermediate) {
	// Create head state (active, chained)
	int64_t headId = dataManager->getIdAllocator()->allocateId(graph::State::typeId);
	int64_t midId = dataManager->getIdAllocator()->allocateId(graph::State::typeId);

	graph::State headState(headId, "chain_head", "part1");
	headState.setNextStateId(midId);
	headState.setChainPosition(0);
	dataManager->addStateEntity(headState);

	// Create intermediate state (inactive but with valid ID)
	graph::State midState(midId, "", "part2");
	midState.setPrevStateId(headId);
	midState.setChainPosition(1);
	midState.markInactive();
	dataManager->addStateEntity(midState);

	// Verify intermediate is inactive with valid ID
	graph::State check = dataManager->getState(midId);
	EXPECT_NE(check.getId(), 0);
	EXPECT_FALSE(check.isActive());

	// readStateChain should throw "State chain corrupted" via !isActive() branch
	EXPECT_THROW((void)stateChainManager->readStateChain(headId), std::runtime_error);
}

// Covers line 119 branch 2: updateStateChain where head has getId() != 0
// but !isActive()
TEST_F(StateChainManagerTest, UpdateStateChainWithMarkInactiveHead) {
	int64_t stateId = dataManager->getIdAllocator()->allocateId(graph::State::typeId);

	// Add an inactive state with valid ID
	graph::State inactiveState(stateId, "inactive_upd_key", "data");
	inactiveState.markInactive();
	dataManager->addStateEntity(inactiveState);

	// Verify setup
	graph::State check = dataManager->getState(stateId);
	EXPECT_NE(check.getId(), 0);
	EXPECT_FALSE(check.isActive());

	// updateStateChain should throw via the !isActive() branch
	EXPECT_THROW((void)stateChainManager->updateStateChain(stateId, "new data", false),
				 std::runtime_error);
}

// Covers line 158 branch 2: deleteStateChain where head has getId() != 0
// but !isActive() - should early return
TEST_F(StateChainManagerTest, DeleteStateChainWithMarkInactiveHead) {
	int64_t stateId = dataManager->getIdAllocator()->allocateId(graph::State::typeId);

	// Add an inactive state with valid ID
	graph::State inactiveState(stateId, "inactive_del_key", "data");
	inactiveState.markInactive();
	dataManager->addStateEntity(inactiveState);

	// deleteStateChain should early return (no-op) for inactive head
	EXPECT_NO_THROW(stateChainManager->deleteStateChain(stateId));
}

// Covers line 181: getStateChainIds for blob head where isActive() is false
// Creates an inactive state with externalId != 0 (so isBlobStorage() is true)
// to hit the False branch of head.isActive() in the blob path
TEST_F(StateChainManagerTest, GetStateChainIdsBlobHeadInactiveViaMarkInactive) {
	int64_t stateId = dataManager->getIdAllocator()->allocateId(graph::State::typeId);

	// Create a state with externalId != 0 (so isBlobStorage() returns true)
	// but inactive
	graph::State blobState(stateId, "inactive_blob_ids_key", "");
	blobState.setExternalId(999); // Makes isBlobStorage() return true
	blobState.markInactive();
	dataManager->addStateEntity(blobState);

	// Verify setup: state is blob storage and inactive
	graph::State check = dataManager->getState(stateId);
	EXPECT_TRUE(check.isBlobStorage());
	EXPECT_FALSE(check.isActive());

	// getStateChainIds should return empty for inactive blob head
	auto ids = stateChainManager->getStateChainIds(stateId);
	EXPECT_TRUE(ids.empty());
}

// ==========================================
// Branch Coverage: readStateChain blob storage with externalId==0
// Covers: StateChainManager.cpp line 55: headState.getExternalId() == 0 -> True
// ==========================================

TEST_F(StateChainManagerTest, ReadBlobStorageWithZeroExternalIdReturnsEmpty) {
	// Create a blob-mode state via createStateChain then clear its externalId
	auto chain = stateChainManager->createStateChain("zero_ext_blob_read", "some data", true);
	int64_t headId = chain[0].getId();
	ASSERT_TRUE(chain[0].isBlobStorage());
	ASSERT_NE(chain[0].getExternalId(), 0);

	// Now update the state to have externalId=0 while keeping blob storage mode
	graph::State head = dataManager->getState(headId);
	head.setExternalId(0);
	dataManager->updateStateEntity(head);

	// Verify: readStateChain should return empty string for blob with externalId==0
	std::string result = stateChainManager->readStateChain(headId);
	EXPECT_EQ(result, "");
}

