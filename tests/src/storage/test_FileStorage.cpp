/**
 * @file test_FileStorage.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/19
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <fstream>
#include <graph/utils/ChecksumUtils.h>
#include <gtest/gtest.h>
#include <zlib.h>
#include "graph/storage/FileStorage.h"

class FileStorageTest : public ::testing::Test {
protected:
	void SetUp() override {
		// Generate a unique temporary file name using UUID
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = std::filesystem::temp_directory_path() / ("test_db_file_" + to_string(uuid) + ".dat");
		fileStorage = std::make_unique<graph::storage::FileStorage>(testFilePath.string());
	}

	void TearDown() override {
		// Clean up code here, e.g., remove the temporary file
		fileStorage->close();
		std::filesystem::remove(testFilePath);
	}

	std::filesystem::path testFilePath;
	std::unique_ptr<graph::storage::FileStorage> fileStorage;
};

TEST(CalculateCrcTest, BasicTest) {
	const char *data = "CRC32 Test";
	size_t length = strlen(data);
	uint32_t expectedCrc = crc32(0L, reinterpret_cast<const Bytef *>(data), length);

	ASSERT_EQ(graph::utils::calculateCrc(data, length), expectedCrc);
}

// Test case for FileStorage::allocateSegment
TEST_F(FileStorageTest, AllocateSegment) {
	// Open the file for writing
	std::fstream file(testFilePath, std::ios::binary | std::ios::out | std::ios::in | std::ios::trunc);
	ASSERT_TRUE(file.is_open());

	// Allocate a segment
	uint64_t segmentOffset = fileStorage->allocateSegment(0, 10);

	// Verify the segment was allocated correctly
	file.seekg(static_cast<std::streamoff>(segmentOffset));
	graph::storage::SegmentHeader header;
	file.read(reinterpret_cast<char *>(&header), sizeof(header));
	ASSERT_EQ(header.data_type, static_cast<unsigned int>(0));
	ASSERT_EQ(header.capacity, static_cast<unsigned int>(10));
	ASSERT_EQ(header.used, static_cast<unsigned int>(0));
	ASSERT_EQ(header.next_segment_offset, static_cast<unsigned long long>(0));
	ASSERT_EQ(header.start_id, static_cast<long long>(0));

	// Verify the segment data area is correctly initialized
	file.seekg(static_cast<std::streamoff>(segmentOffset + sizeof(graph::storage::SegmentHeader)));
	std::vector<char> data(header.capacity * sizeof(graph::Node), 0);
	std::vector<char> fileData(data.size());
	file.read(fileData.data(), static_cast<std::streamsize>(fileData.size()));
	ASSERT_EQ(data, fileData);
}

TEST_F(FileStorageTest, TestOpenClose) {
	fileStorage->open();
	EXPECT_TRUE(fileStorage->isOpen());
	fileStorage->close();
	EXPECT_FALSE(fileStorage->isOpen());
}

TEST_F(FileStorageTest, TestInsertNode) {
	fileStorage->open();
	graph::Node node = fileStorage->insertNode("TestNode");
	graph::Node retrievedNode = fileStorage->getNode(node.getId());
	EXPECT_EQ(retrievedNode.getId(), node.getId());
	EXPECT_EQ(retrievedNode.getLabel(), node.getLabel());
}

TEST_F(FileStorageTest, TestInsertEdge) {
	fileStorage->open();
	graph::Edge edge = fileStorage->insertEdge(1, 2, "TestEdge");
	graph::Edge retrievedEdge = fileStorage->getEdge(edge.getId());
	EXPECT_EQ(retrievedEdge.getId(), edge.getId());
	EXPECT_EQ(retrievedEdge.getLabel(), edge.getLabel());
}

TEST_F(FileStorageTest, TestGetAllNodes) {
	fileStorage->open();
	fileStorage->insertNode("Node1");
	fileStorage->insertNode("Node2");
	auto allNodes = fileStorage->getAllNodes();
	EXPECT_EQ(allNodes.size(), 2u);
	EXPECT_EQ(allNodes[1].getLabel(), "Node1");
	EXPECT_EQ(allNodes[2].getLabel(), "Node2");
}

TEST_F(FileStorageTest, TestGetAllEdges) {
	fileStorage->open();
	fileStorage->insertEdge(1, 2, "Edge1");
	fileStorage->insertEdge(2, 3, "Edge2");
	auto allEdges = fileStorage->getAllEdges();
	EXPECT_EQ(allEdges.size(), 2u);
	EXPECT_EQ(allEdges[1].getLabel(), "Edge1");
	EXPECT_EQ(allEdges[2].getLabel(), "Edge2");
}

// Test for empty data
TEST_F(FileStorageTest, SaveDataEmpty) {
	std::unordered_map<int64_t, graph::Node> data;
	uint64_t segmentHead = 0;
	fileStorage->open();
	std::fstream file(testFilePath, std::ios::binary | std::ios::in | std::ios::out);

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_EQ(segmentHead, 0u);
}

// Test for single data element
TEST_F(FileStorageTest, SaveDataSingleElement) {
	std::unordered_map<int64_t, graph::Node> data = {{1, graph::Node(1, "Node1")}};
	uint64_t segmentHead = 0;
	fileStorage->open();
	std::fstream file(testFilePath, std::ios::binary | std::ios::in | std::ios::out);

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);
}

// Test for data that fits in one segment
TEST_F(FileStorageTest, SaveDataFitsInOneSegment) {
	std::unordered_map<int64_t, graph::Node> data;
	for (int64_t i = 1; i <= 50; ++i) {
		data[i] = graph::Node(i, "Node" + std::to_string(i));
	}
	uint64_t segmentHead = 0;
	fileStorage->open();
	std::fstream file(testFilePath, std::ios::binary | std::ios::in | std::ios::out);

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);
}

// Test for data requiring multiple segments
TEST_F(FileStorageTest, SaveDataMultipleSegments) {
	std::unordered_map<int64_t, graph::Node> data;
	for (int64_t i = 1; i <= 300; ++i) {
		data[i] = graph::Node(i, "Node" + std::to_string(i));
	}
	uint64_t segmentHead = 0;
	fileStorage->open();
	std::fstream file(testFilePath, std::ios::binary | std::ios::in | std::ios::out);

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_NE(segmentHead, 0u);
}

// Test for verifying segment link correctness
TEST_F(FileStorageTest, VerifySegmentLinking) {
	std::unordered_map<int64_t, graph::Node> data;
	for (int64_t i = 1; i <= 300; ++i) {
		data[i] = graph::Node(i, "Node" + std::to_string(i));
	}
	uint64_t segmentHead = 0;
	fileStorage->open();
	std::fstream file(testFilePath, std::ios::binary | std::ios::in | std::ios::out);

	fileStorage->saveData(data, segmentHead, 100);

	// Read segment headers to check linking
	uint64_t currentOffset = segmentHead;
	graph::storage::SegmentHeader header;
	int segmentCount = 0;
	while (currentOffset != 0) {
		file.seekg(static_cast<std::streamoff>(currentOffset));
		file.read(reinterpret_cast<char *>(&header), sizeof(graph::storage::SegmentHeader));
		segmentCount++;
		currentOffset = header.next_segment_offset;
	}
	EXPECT_EQ(segmentCount, 3);
}
