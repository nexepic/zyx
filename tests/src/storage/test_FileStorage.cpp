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
		testFilePath = std::filesystem::temp_directory_path() / ("test_db_file_" + boost::uuids::to_string(uuid) + ".dat");

		// Create and initialize FileStorage
		fileStorage = std::make_unique<graph::storage::FileStorage>(testFilePath.string());
		fileStorage->open();
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
	fileStorage->open();

	// Allocate a segment
	uint64_t segmentOffset = fileStorage->allocateSegment(0, 10);

	// Verify the segment was allocated correctly using SegmentTracker
	auto segmentTracker = fileStorage->getSegmentTracker();
	graph::storage::SegmentHeader header = segmentTracker->getSegmentHeader(segmentOffset);

	ASSERT_EQ(header.data_type, static_cast<unsigned int>(0));
	ASSERT_EQ(header.capacity, static_cast<unsigned int>(10));
	ASSERT_EQ(header.used, static_cast<unsigned int>(0));
	ASSERT_EQ(header.next_segment_offset, static_cast<unsigned long long>(0));
	ASSERT_EQ(header.start_id, static_cast<long long>(0));
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

// Test for empty data
TEST_F(FileStorageTest, SaveDataEmpty) {
	std::unordered_map<int64_t, graph::Node> data;
	uint64_t segmentHead = 0;
	fileStorage->open();

	fileStorage->saveData(data, segmentHead, 100);
	EXPECT_EQ(segmentHead, 0u);
}

// Test for single data element
TEST_F(FileStorageTest, SaveDataSingleElement) {
	std::unordered_map<int64_t, graph::Node> data = {{1, graph::Node(1, "Node1")}};
	uint64_t segmentHead = 0;
	fileStorage->open();

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

    fileStorage->saveData(data, segmentHead, 100);

    // Use SegmentTracker to check linking
    auto segmentTracker = fileStorage->getSegmentTracker();
    uint64_t currentOffset = segmentHead;
    int segmentCount = 0;

    while (currentOffset != 0) {
        graph::storage::SegmentHeader header = segmentTracker->getSegmentHeader(currentOffset);
        segmentCount++;
        currentOffset = header.next_segment_offset;
    }
    EXPECT_EQ(segmentCount, 3);
}
