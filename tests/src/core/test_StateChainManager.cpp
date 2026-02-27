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
	constexpr std::string testData = "Test data";

	graph::State headState(headStateId, "test_key", testData);
	dataManager->addStateEntity(headState);

	bool result = stateChainManager->isDataSame(headStateId, testData);
	EXPECT_TRUE(result);
}

TEST_F(StateChainManagerTest, IsDataSameFalse) {
	constexpr int64_t headStateId = 10001;
	constexpr std::string originalData = "Original data";
	constexpr std::string newData = "Different data";

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
	constexpr std::string key = "ids_blob_key";
	auto chain = stateChainManager->createStateChain(key, "data", true);

	auto chainIds = stateChainManager->getStateChainIds(chain[0].getId());

	// For blob storage, getStateChainIds should return only the head State ID
	// (It does not return the internal IDs of the Blob entities)
	EXPECT_EQ(chainIds.size(), 1UL);
	EXPECT_EQ(chainIds[0], chain[0].getId());
}

TEST_F(StateChainManagerTest, DeleteStateChainBlob) {
	constexpr std::string key = "del_blob_key";
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
	constexpr std::string smallData = "Small data";
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
	constexpr std::string key = "same_data_key";
	constexpr std::string data = "Persistent Data";

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
	constexpr std::string data = "New Data";

	// This targets lines 121-123: headState.getId() == 0
	EXPECT_THROW({ (void) stateChainManager->updateStateChain(fakeId, data, false); }, std::runtime_error);
}

TEST_F(StateChainManagerTest, UpdateInactiveHead) {
	constexpr std::string key = "inactive_key";
	constexpr std::string data = "Data";

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

	constexpr std::string key = "mode_ignore_key";
	constexpr std::string data = "Static Data";

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

	constexpr std::string baseKey = "cache_test_key";

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
	constexpr std::string baseKey = "stress_key";

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

	constexpr std::string key = "sync_test_key";
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

	constexpr std::string key = "eviction_update_key";
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

	constexpr std::string baseKey = "blob_evict_test";

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

	constexpr std::string key = "mode_switch_test";
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
	constexpr std::string originalData = "Original data";
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

// Test updateStateChain with blob storage when data is empty
// Tests branch at line 240: if (!blobChain.empty())
TEST_F(StateChainManagerTest, CreateBlobWithEmptyData) {
	constexpr std::string key = "empty_blob_key";
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
