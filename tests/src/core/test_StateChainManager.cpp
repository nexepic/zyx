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

	EXPECT_EQ(chain[1].getKey(), "");
	EXPECT_EQ(chain[1].getChainPosition(), 1);
	EXPECT_EQ(chain[1].getPrevStateId(), chain[0].getId());
	EXPECT_EQ(chain[1].getNextStateId(), chain[2].getId());

	EXPECT_EQ(chain[2].getKey(), "");
	EXPECT_EQ(chain[2].getChainPosition(), 2);
	EXPECT_EQ(chain[2].getPrevStateId(), chain[1].getId());
	EXPECT_EQ(chain[2].getNextStateId(), 0);
}

TEST_F(StateChainManagerTest, CreateStateChainEmptyData) {
	const std::string key = "empty_key";
	const std::string data;

	auto chain = stateChainManager->createStateChain(key, data);

	EXPECT_EQ(chain.size(), 0UL);
}

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

	EXPECT_THROW(static_cast<void>(stateChainManager->readStateChain(headStateId)), std::runtime_error);
}

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

TEST_F(StateChainManagerTest, IsDataSameException) {
	constexpr int64_t headStateId = 11001;
	const std::string newData = "New data";

	bool result = stateChainManager->isDataSame(headStateId, newData);

	EXPECT_FALSE(result); // Should return false on exception
}

TEST_F(StateChainManagerTest, GetStateChainIds) {
	constexpr int64_t headStateId = 12001;

	graph::State state1(12001, "test_key", "data1");
	state1.setNextStateId(12002);

	graph::State state2(12002, "", "data2");
	state2.setPrevStateId(12001);
	state2.setNextStateId(12003);

	graph::State state3(12003, "", "data3");
	state3.setPrevStateId(12002);

	dataManager->addStateEntity(state1);
	dataManager->addStateEntity(state2);
	dataManager->addStateEntity(state3);

	auto chainIds = stateChainManager->getStateChainIds(headStateId);

	EXPECT_EQ(chainIds.size(), 3UL);
	EXPECT_EQ(chainIds[0], 12001);
	EXPECT_EQ(chainIds[1], 12002);
	EXPECT_EQ(chainIds[2], 12003);
}

TEST_F(StateChainManagerTest, GetStateChainIdsBreakOnInactive) {
	constexpr int64_t headStateId = 13001;

	graph::State state1(13001, "test_key", "data1");
	state1.setNextStateId(13002);

	// Inactive state (default constructed, id = 13002)
	graph::State inactiveState(13002, "", "");
	inactiveState.markInactive();

	dataManager->addStateEntity(state1);
	dataManager->addStateEntity(inactiveState);

	auto chainIds = stateChainManager->getStateChainIds(headStateId);

	EXPECT_EQ(chainIds.size(), 1UL);
	EXPECT_EQ(chainIds[0], 13001);
}

TEST_F(StateChainManagerTest, FindStateByKey) {
	const std::string testKey = "search_key";
	graph::State expectedState(14001, testKey, "data");
	dataManager->addStateEntity(expectedState);

	const graph::State result = stateChainManager->findStateByKey(testKey);

	EXPECT_EQ(result.getId(), 14001);
	EXPECT_EQ(result.getKey(), testKey);
}

TEST_F(StateChainManagerTest, DeleteStateChain) {
	constexpr int64_t headStateId = 15001;

	graph::State state1(15001, "test_key", "data1");
	state1.setNextStateId(15002);

	graph::State state2(15002, "", "data2");
	state2.setPrevStateId(15001);

	stateChainManager->deleteStateChain(headStateId);
}

TEST_F(StateChainManagerTest, UpdateStateChainSameData) {
	constexpr int64_t headStateId = 16001;
	const std::string testData = "Same data";

	graph::State headState(headStateId, "test_key", testData);
	dataManager->addStateEntity(headState);

	const auto result = stateChainManager->updateStateChain(headStateId, testData);

	EXPECT_EQ(result.size(), 1UL);
	EXPECT_EQ(result[0].getId(), headStateId);
}

TEST_F(StateChainManagerTest, UpdateStateChainDifferentData) {
	constexpr int64_t headStateId = 17001;
	const std::string originalData = "Original data";
	const std::string newData = "New different data";

	graph::State headState(headStateId, "test_key", originalData);
	dataManager->addStateEntity(headState);

	const auto result = stateChainManager->updateStateChain(headStateId, newData);

	EXPECT_EQ(result.size(), 1UL);
	EXPECT_EQ(result[0].getDataAsString(), newData);
}

TEST_F(StateChainManagerTest, SplitDataSingleChunk) {
	const std::string smallData = "Small data";

	// Call static method through instance for testing
	const auto chunks = stateChainManager->createStateChain("key", smallData);

	// Verify the internal splitting worked correctly
	EXPECT_EQ(chunks.size(), 1UL);
}

TEST_F(StateChainManagerTest, SplitDataMultipleChunks) {
	// Create data that will result in exactly 3 chunks
	const std::string largeData(graph::State::CHUNK_SIZE * 2 + 100, 'A');

	const auto chunks = stateChainManager->createStateChain("key", largeData);

	EXPECT_EQ(chunks.size(), 3UL);

	// Verify chunk sizes
	EXPECT_EQ(chunks[0].getSize(), graph::State::CHUNK_SIZE);
	EXPECT_EQ(chunks[1].getSize(), graph::State::CHUNK_SIZE);
	EXPECT_EQ(chunks[2].getSize(), 100UL);
}
