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

TEST_F(StateChainManagerTest, CreateStateChainSingleChunk) {
	const std::string key = "test_key";
	const std::string data = "Small data that fits in one chunk";

	auto chain = stateChainManager->createStateChain(key, data);

	ASSERT_EQ(chain.size(), 1UL);
	EXPECT_EQ(chain[0].getKey(), key);
	EXPECT_EQ(chain[0].getDataAsString(), data);
	EXPECT_EQ(chain[0].getChainPosition(), 0);
	EXPECT_EQ(chain[0].getPrevStateId(), 0);
	EXPECT_EQ(chain[0].getNextStateId(), 0);
	EXPECT_FALSE(chain[0].isBlobStorage());
}

TEST_F(StateChainManagerTest, CreateStateChainMultipleChunks) {
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

// Updated expectation for empty data
TEST_F(StateChainManagerTest, CreateStateChainEmptyData) {
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

TEST_F(StateChainManagerTest, CreateStateChainWithBlobStorage) {
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

TEST_F(StateChainManagerTest, CreateStateChainBlobStorageLargeData) {
	const std::string key = "blob_large_key";
	// Data larger than one Blob chunk (typically 4KB+) to force Blob chaining internally
	const std::string data(10000, 'B');

	auto chain = stateChainManager->createStateChain(key, data, true);

	ASSERT_EQ(chain.size(), 1UL);
	EXPECT_TRUE(chain[0].isBlobStorage());

	std::string readData = stateChainManager->readStateChain(chain[0].getId());
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
	const std::string testData = "Test data";

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
	const std::string key = "ids_blob_key";
	auto chain = stateChainManager->createStateChain(key, "data", true);

	auto chainIds = stateChainManager->getStateChainIds(chain[0].getId());

	// For blob storage, getStateChainIds should return only the head State ID
	// (It does not return the internal IDs of the Blob entities)
	EXPECT_EQ(chainIds.size(), 1UL);
	EXPECT_EQ(chainIds[0], chain[0].getId());
}

TEST_F(StateChainManagerTest, DeleteStateChainBlob) {
	const std::string key = "del_blob_key";
	auto chain = stateChainManager->createStateChain(key, "data", true);
	int64_t headId = chain[0].getId();
	int64_t blobId = chain[0].getExternalId();

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
	auto chain = stateChainManager->createStateChain(key, data, false);
	int64_t headId = chain[0].getId();

	// 2. Update with identical data
	// This targets lines 105-117: isDataSame returns true
	auto resultChain = stateChainManager->updateStateChain(headId, data, false);

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
	auto chain = stateChainManager->createStateChain(key, data, true);
	int64_t headId = chain[0].getId();
	EXPECT_TRUE(chain[0].isBlobStorage());

	// Update with SAME data but requesting Internal mode (false)
	auto result = stateChainManager->updateStateChain(headId, data, false);

	// Current implementation: Returns existing chain (Blob), ignores request to switch
	EXPECT_TRUE(result[0].isBlobStorage());
}
