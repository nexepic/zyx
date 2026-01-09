/**
 * @file test_Blob.cpp
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
#include "graph/core/Blob.hpp"

class BlobTest : public ::testing::Test {
protected:
	void SetUp() override {
		// No setup needed for direct Blob testing
	}

	void TearDown() override {
		// No cleanup needed
	}
};

TEST_F(BlobTest, DefaultConstructor) {
	graph::Blob blob;

	EXPECT_EQ(blob.getMetadata().id, 0);
	EXPECT_EQ(blob.getMetadata().dataSize, 0u);
	EXPECT_EQ(blob.getMetadata().entityId, 0);
	EXPECT_EQ(blob.getMetadata().nextBlobId, 0);
	EXPECT_EQ(blob.getMetadata().prevBlobId, 0);
	EXPECT_TRUE(blob.getMetadata().isActive);
	EXPECT_FALSE(blob.getMetadata().compressed);
}

TEST_F(BlobTest, ParameterizedConstructor) {
	const std::string testData = "Hello, World!";
	graph::Blob blob(42, testData);

	EXPECT_EQ(blob.getMetadata().id, 42);
	EXPECT_EQ(blob.getMetadata().dataSize, testData.size());
	EXPECT_EQ(blob.getDataAsString(), testData);
}

TEST_F(BlobTest, SetDataNormalSize) {
	graph::Blob blob;
	const std::string testData = "Test data for blob";

	blob.setData(testData);

	EXPECT_EQ(blob.getMetadata().dataSize, testData.size());
	EXPECT_EQ(blob.getDataAsString(), testData);
}

TEST_F(BlobTest, SetDataOversizedTruncated) {
	graph::Blob blob;
	const std::string oversizedData(graph::Blob::CHUNK_SIZE + 100, 'X');

	blob.setData(oversizedData);

	EXPECT_EQ(blob.getMetadata().dataSize, graph::Blob::CHUNK_SIZE);
	EXPECT_EQ(blob.getDataAsString().size(), graph::Blob::CHUNK_SIZE);
}

TEST_F(BlobTest, SetDataEmpty) {
	graph::Blob blob;
	const std::string emptyData;

	blob.setData(emptyData);

	EXPECT_EQ(blob.getMetadata().dataSize, 0u);
	EXPECT_EQ(blob.getDataAsString(), "");
}

TEST_F(BlobTest, CanFitDataMethod) {
	EXPECT_TRUE(graph::Blob::canFitData(100));
	EXPECT_TRUE(graph::Blob::canFitData(graph::Blob::CHUNK_SIZE));
	EXPECT_FALSE(graph::Blob::canFitData(graph::Blob::CHUNK_SIZE + 1));
}

TEST_F(BlobTest, ChainableMethods) {
	graph::Blob blob;

	blob.setNextChainId(123);
	blob.setPrevChainId(456);

	EXPECT_EQ(blob.getNextChainId(), 123);
	EXPECT_EQ(blob.getPrevChainId(), 456);
	EXPECT_EQ(blob.getNextBlobId(), 123); // Legacy method
	EXPECT_EQ(blob.getPrevBlobId(), 456); // Legacy method
}

TEST_F(BlobTest, SerializeDeserializeEmpty) {
	graph::Blob originalBlob(100, "");
	std::stringstream ss;

	originalBlob.serialize(ss);
	graph::Blob deserializedBlob = graph::Blob::deserialize(ss);

	EXPECT_EQ(deserializedBlob.getMetadata().id, originalBlob.getMetadata().id);
	EXPECT_EQ(deserializedBlob.getMetadata().dataSize, originalBlob.getMetadata().dataSize);
	EXPECT_EQ(deserializedBlob.getDataAsString(), originalBlob.getDataAsString());
}

TEST_F(BlobTest, SerializeDeserializeWithData) {
	const std::string testData = "Serialization test data";
	graph::Blob originalBlob(200, testData);
	originalBlob.getMutableMetadata().entityId = 999;
	originalBlob.getMutableMetadata().compressed = true;
	originalBlob.setNextChainId(300);
	originalBlob.setPrevChainId(100);

	std::stringstream ss;
	originalBlob.serialize(ss);
	graph::Blob deserializedBlob = graph::Blob::deserialize(ss);

	EXPECT_EQ(deserializedBlob.getMetadata().id, originalBlob.getMetadata().id);
	EXPECT_EQ(deserializedBlob.getMetadata().entityId, originalBlob.getMetadata().entityId);
	EXPECT_EQ(deserializedBlob.getMetadata().compressed, originalBlob.getMetadata().compressed);
	EXPECT_EQ(deserializedBlob.getMetadata().nextBlobId, originalBlob.getMetadata().nextBlobId);
	EXPECT_EQ(deserializedBlob.getMetadata().prevBlobId, originalBlob.getMetadata().prevBlobId);
	EXPECT_EQ(deserializedBlob.getMetadata().dataSize, originalBlob.getMetadata().dataSize);
	EXPECT_EQ(deserializedBlob.getDataAsString(), originalBlob.getDataAsString());
}

TEST_F(BlobTest, GetSerializedSize) {
	const std::string testData = "Size calculation test";
	const graph::Blob blob(1, testData);

	size_t expectedSize = 0;
	expectedSize += sizeof(int64_t) * 4; // id, entityId, nextBlobId, prevBlobId
	expectedSize += sizeof(uint32_t) * 3; // dataSize, entityType, originalSize
	expectedSize += sizeof(int32_t); // chainPosition
	expectedSize += sizeof(bool) * 2; // isActive, compressed
	expectedSize += testData.size(); // actual data
	EXPECT_EQ(blob.getSerializedSize(), expectedSize);
}

TEST_F(BlobTest, GetSerializedSizeEmpty) {
	graph::Blob blob(1, "");

	size_t expectedSize = 0;
	expectedSize += sizeof(int64_t) * 4; // id, entityId, nextBlobId, prevBlobId
	expectedSize += sizeof(uint32_t) * 3; // dataSize, entityType, originalSize
	expectedSize += sizeof(int32_t); // chainPosition
	expectedSize += sizeof(bool) * 2; // isActive, compressed
	EXPECT_EQ(blob.getSerializedSize(), expectedSize);
}

TEST_F(BlobTest, Constants) {
	EXPECT_EQ(graph::Blob::getTotalSize(), 256u);
	EXPECT_EQ(graph::Blob::TOTAL_BLOB_SIZE, 256u);

	size_t expectedMetadataSize = 0;
	expectedMetadataSize += sizeof(int64_t) * 4;
	expectedMetadataSize += sizeof(uint32_t) * 3;
	expectedMetadataSize += sizeof(int32_t);
	expectedMetadataSize += sizeof(bool) * 2;

	EXPECT_EQ(graph::Blob::METADATA_SIZE, expectedMetadataSize);
	EXPECT_EQ(graph::Blob::CHUNK_SIZE, 256u - graph::Blob::METADATA_SIZE);
	EXPECT_EQ(graph::Blob::MAX_COMPRESSED_SIZE, 5u * 1024 * 1024);
}
