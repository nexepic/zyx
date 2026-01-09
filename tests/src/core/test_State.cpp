/**
 * @file test_State.cpp
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

#include <gtest/gtest.h>
#include <sstream>
#include "graph/core/State.hpp"

class StateTest : public ::testing::Test {
protected:
	void SetUp() override {
		// No setup needed for direct State testing
	}

	void TearDown() override {
		// No cleanup needed
	}
};

TEST_F(StateTest, DefaultConstructor) {
	graph::State state;

	EXPECT_EQ(state.getMetadata().id, 0);
	EXPECT_EQ(state.getMetadata().nextStateId, 0);
	EXPECT_EQ(state.getMetadata().prevStateId, 0);
	EXPECT_EQ(state.getMetadata().dataSize, 0u);
	EXPECT_EQ(state.getMetadata().chainPosition, 0);
	EXPECT_TRUE(state.getMetadata().isActive);
	EXPECT_EQ(state.getKey(), "");
	EXPECT_EQ(state.getSize(), 0u);
	EXPECT_EQ(state.getDataAsString(), "");
}

TEST_F(StateTest, ParameterizedConstructor) {
	constexpr int64_t id = 123;
	const std::string key = "test_key";
	const std::string data = "test_data";

	graph::State state(id, key, data);

	EXPECT_EQ(state.getMetadata().id, id);
	EXPECT_EQ(state.getKey(), key);
	EXPECT_EQ(state.getDataAsString(), data);
	EXPECT_EQ(state.getSize(), data.size());
	EXPECT_TRUE(state.getMetadata().isActive);
}

TEST_F(StateTest, SetGetKey) {
	graph::State state;
	const std::string testKey = "configuration_key";

	state.setKey(testKey);

	EXPECT_EQ(state.getKey(), testKey);
}

TEST_F(StateTest, SetKeyTooLong) {
	graph::State state;
	// Create a key longer than MAX_KEY_LENGTH - 1
	const std::string longKey(graph::State::MAX_KEY_LENGTH, 'X');

	EXPECT_THROW(state.setKey(longKey), std::runtime_error);
}

TEST_F(StateTest, SetKeyMaxLength) {
	graph::State state;
	// Create a key at exactly MAX_KEY_LENGTH - 1
	const std::string maxKey(graph::State::MAX_KEY_LENGTH - 1, 'X');

	state.setKey(maxKey);
	EXPECT_EQ(state.getKey(), maxKey);
}

TEST_F(StateTest, SetGetData) {
	graph::State state;
	const std::string testData = "Configuration data for testing";

	state.setData(testData);

	EXPECT_EQ(state.getDataAsString(), testData);
	EXPECT_EQ(state.getSize(), testData.size());
}

TEST_F(StateTest, SetDataTruncation) {
	graph::State state;
	// Create data larger than CHUNK_SIZE
	const std::string oversizedData(graph::State::CHUNK_SIZE + 100, 'Y');

	state.setData(oversizedData);

	EXPECT_EQ(state.getSize(), graph::State::CHUNK_SIZE);
	EXPECT_EQ(state.getDataAsString().size(), graph::State::CHUNK_SIZE);
}

TEST_F(StateTest, SetDataEmpty) {
	graph::State state;
	const std::string emptyData;

	state.setData(emptyData);

	EXPECT_EQ(state.getDataAsString(), "");
	EXPECT_EQ(state.getSize(), 0u);
}

TEST_F(StateTest, CanFitDataMethod) {
	EXPECT_TRUE(graph::State::canFitData(100));
	EXPECT_TRUE(graph::State::canFitData(graph::State::CHUNK_SIZE));
	EXPECT_FALSE(graph::State::canFitData(graph::State::CHUNK_SIZE + 1));
}

TEST_F(StateTest, ChainableMethods) {
	graph::State state;

	state.setNextChainId(123);
	state.setPrevChainId(456);

	EXPECT_EQ(state.getNextChainId(), 123);
	EXPECT_EQ(state.getPrevChainId(), 456);
	EXPECT_EQ(state.getNextStateId(), 123); // Legacy method
	EXPECT_EQ(state.getPrevStateId(), 456); // Legacy method
}

TEST_F(StateTest, LegacyChainMethods) {
	graph::State state;

	state.setNextStateId(789);
	state.setPrevStateId(101112);

	EXPECT_EQ(state.getNextStateId(), 789);
	EXPECT_EQ(state.getPrevStateId(), 101112);
	EXPECT_EQ(state.getNextChainId(), 789); // Should be same as legacy
	EXPECT_EQ(state.getPrevChainId(), 101112); // Should be same as legacy
}

TEST_F(StateTest, SerializeDeserializeEmpty) {
	const graph::State originalState(100, "empty_key", "");
	std::stringstream ss;

	originalState.serialize(ss);
	graph::State deserializedState = graph::State::deserialize(ss);

	EXPECT_EQ(deserializedState.getMetadata().id, originalState.getMetadata().id);
	EXPECT_EQ(deserializedState.getKey(), originalState.getKey());
	EXPECT_EQ(deserializedState.getDataAsString(), originalState.getDataAsString());
	EXPECT_EQ(deserializedState.getSize(), originalState.getSize());
	EXPECT_EQ(deserializedState.getMetadata().isActive, originalState.getMetadata().isActive);
}

TEST_F(StateTest, SerializeDeserializeWithData) {
	const std::string testKey = "config_key";
	const std::string testData = "Serialization test data for state";
	graph::State originalState(200, testKey, testData);
	originalState.setNextChainId(300);
	originalState.setPrevChainId(100);
	originalState.getMutableMetadata().chainPosition = 1;
	originalState.getMutableMetadata().isActive = false;

	std::stringstream ss;
	originalState.serialize(ss);
	graph::State deserializedState = graph::State::deserialize(ss);

	EXPECT_EQ(deserializedState.getMetadata().id, originalState.getMetadata().id);
	EXPECT_EQ(deserializedState.getKey(), originalState.getKey());
	EXPECT_EQ(deserializedState.getDataAsString(), originalState.getDataAsString());
	EXPECT_EQ(deserializedState.getSize(), originalState.getSize());
	EXPECT_EQ(deserializedState.getNextChainId(), originalState.getNextChainId());
	EXPECT_EQ(deserializedState.getPrevChainId(), originalState.getPrevChainId());
	EXPECT_EQ(deserializedState.getMetadata().chainPosition, originalState.getMetadata().chainPosition);
	EXPECT_EQ(deserializedState.getMetadata().isActive, originalState.getMetadata().isActive);
}

TEST_F(StateTest, GetSerializedSize) {
	const std::string testKey = "test_key";
	const std::string testData = "Some test data";
	const graph::State state(1, testKey, testData);

	size_t expectedSize = 0;
	expectedSize += sizeof(int64_t); // id
	expectedSize += sizeof(int64_t); // nextStateId
	expectedSize += sizeof(int64_t); // prevStateId
	expectedSize += sizeof(uint32_t); // dataSize
	expectedSize += sizeof(int32_t); // chainPosition
	expectedSize += graph::State::MAX_KEY_LENGTH; // key buffer
	expectedSize += sizeof(bool); // isActive
	expectedSize += testData.size(); // Data content

	EXPECT_EQ(state.getSerializedSize(), expectedSize);
}

TEST_F(StateTest, GetSerializedSizeEmpty) {
	const graph::State state(1, "", "");

	size_t expectedSize = 0;
	expectedSize += sizeof(int64_t); // id
	expectedSize += sizeof(int64_t); // nextStateId
	expectedSize += sizeof(int64_t); // prevStateId
	expectedSize += sizeof(uint32_t); // dataSize
	expectedSize += sizeof(int32_t); // chainPosition
	expectedSize += graph::State::MAX_KEY_LENGTH; // key buffer
	expectedSize += sizeof(bool); // isActive
	// No string content or data content

	EXPECT_EQ(state.getSerializedSize(), expectedSize);
}

TEST_F(StateTest, Constants) {
	EXPECT_EQ(graph::State::getTotalSize(), 256u);
	EXPECT_EQ(graph::State::TOTAL_STATE_SIZE, 256u);
	EXPECT_EQ(graph::State::MAX_KEY_LENGTH, 64u);

	size_t expectedMetadataSize = 0;
	expectedMetadataSize += sizeof(int64_t) * 3;
	expectedMetadataSize += sizeof(uint32_t);
	expectedMetadataSize += sizeof(int32_t);
	expectedMetadataSize += graph::State::MAX_KEY_LENGTH;
	expectedMetadataSize += sizeof(bool);

	EXPECT_EQ(graph::State::METADATA_SIZE, expectedMetadataSize);
	EXPECT_EQ(graph::State::CHUNK_SIZE, graph::State::TOTAL_STATE_SIZE - graph::State::METADATA_SIZE);
	EXPECT_EQ(graph::State::typeId, graph::toUnderlying(graph::EntityType::State));
}

TEST_F(StateTest, MetadataAccess) {
	graph::State state(42, "test", "data");

	const auto &[id, nextStateId, prevStateId, dataSize, chainPosition, key, isActive] = state.getMetadata();
	EXPECT_EQ(id, 42);
	EXPECT_STREQ(key, "test");
	EXPECT_TRUE(isActive);

	auto &mutableMetadata = state.getMutableMetadata();
	mutableMetadata.chainPosition = 5;
	EXPECT_EQ(state.getMetadata().chainPosition, 5);
}

TEST_F(StateTest, KeyEmptyString) {
	graph::State state;
	state.setKey("");
	EXPECT_EQ(state.getKey(), "");
}

TEST_F(StateTest, DataWithNullCharacters) {
	graph::State state;
	std::string dataWithNull = "test\0embedded";
	dataWithNull.resize(13); // Ensure the null character is included

	state.setData(dataWithNull);

	// The data should preserve null characters since we use size-based storage
	EXPECT_EQ(state.getSize(), 13u);
	EXPECT_EQ(state.getDataAsString().size(), 13u);
}

TEST_F(StateTest, TypeIdConstant) {
	// Verify that typeId matches the expected entity type
	EXPECT_EQ(graph::State::typeId, graph::toUnderlying(graph::EntityType::State));
}

TEST_F(StateTest, ChainPositionHandling) {
	graph::State state;

	auto &metadata = state.getMutableMetadata();
	metadata.chainPosition = -1;
	EXPECT_EQ(state.getMetadata().chainPosition, -1);

	metadata.chainPosition = 1000;
	EXPECT_EQ(state.getMetadata().chainPosition, 1000);
}

TEST_F(StateTest, DataBufferBoundary) {
	graph::State state;
	// Test data exactly at CHUNK_SIZE
	const std::string exactSizeData(graph::State::CHUNK_SIZE, 'Z');

	state.setData(exactSizeData);

	EXPECT_EQ(state.getSize(), graph::State::CHUNK_SIZE);
	EXPECT_EQ(state.getDataAsString(), exactSizeData);
}
